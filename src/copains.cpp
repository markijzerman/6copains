// SPDX-License-Identifier: GPL-3.0-or-later
//
// 6Copains for disting NT - plug-in glue: parameters, routing, MIDI, CV/gate.
// Port of nb_monamie by @sonoCircuit, waveshaping after ImaginaryFriends by
// @synthetivv. See copains_engine.h for the synthesis engine.

#include <stddef.h>
#include <new>
#include <distingnt/api.h>

#include "copains_engine.h"
#include "copains_plugin.h"

using copains::Engine;
using copains::kMaxChunk;
using copains::kNumVoices;

namespace {

constexpr float kVoltsPerUnit = 5.0f;  // engine full scale 1.0 = 5 V at 0 dB gain
constexpr float kTimeMinSeconds = 0.0026f;
constexpr float kTimeLogRange = 8.03168474f;  // ln(8 / 0.0026)

const char* const kEnvModeStrings[] = { "AR", "ASR", "Cycle" };
const char* const kOffOnStrings[] = { "Off", "On" };
const char* const kRetriggerStrings[] = { "Fade", "Overlap" };

#define COPAINS_PARAM(name, lo, hi, def, unit, scaling) \
    { name, lo, hi, def, unit, scaling, nullptr },
#define COPAINS_ENUM(name, hi, def, strings) \
    { name, 0, hi, def, kNT_unitEnum, 0, strings },

const _NT_parameter kParameterTemplate[kNumParams] = {
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output L", 1, 13)
    NT_PARAMETER_AUDIO_OUTPUT_WITH_MODE("Output R", 0, 14)

    COPAINS_PARAM("Level", 0, 1000, 800, kNT_unitPercent, kNT_scaling10)
    COPAINS_PARAM("Ramp", -1000, 1000, 80, kNT_unitHasStrings, 0)
    COPAINS_PARAM("Curve", -1000, 1000, 1000, kNT_unitHasStrings, 0)
    COPAINS_PARAM("FM ratio", 50, 200, 101, kNT_unitNone, kNT_scaling100)
    COPAINS_PARAM("FM index", -1000, 1000, 0, kNT_unitHasStrings, 0)
    COPAINS_PARAM("Pan drift", 0, 1000, 120, kNT_unitPercent, kNT_scaling10)

    COPAINS_ENUM("Env mode", 2, 0, kEnvModeStrings)
    COPAINS_PARAM("Slope", 0, 1000, 30, kNT_unitHasStrings, 0)
    COPAINS_PARAM("Time", 0, 1000, 547, kNT_unitHasStrings, 0)

    COPAINS_PARAM("Morph", 0, 1000, 0, kNT_unitPercent, kNT_scaling10)
    COPAINS_PARAM("Ramp mod", -1000, 1000, 610, kNT_unitPercent, kNT_scaling10)
    COPAINS_PARAM("Curve mod", -1000, 1000, 0, kNT_unitPercent, kNT_scaling10)
    COPAINS_PARAM("FM mod", -1000, 1000, -510, kNT_unitPercent, kNT_scaling10)
    COPAINS_PARAM("Slope mod", -1000, 1000, 0, kNT_unitPercent, kNT_scaling10)
    COPAINS_PARAM("Time mod", -1000, 1000, 0, kNT_unitPercent, kNT_scaling10)

    COPAINS_PARAM("Voice 1 pan", -1000, 1000, -200, kNT_unitHasStrings, 0)
    COPAINS_PARAM("Voice 2 pan", -1000, 1000, 200, kNT_unitHasStrings, 0)
    COPAINS_PARAM("Voice 3 pan", -1000, 1000, -500, kNT_unitHasStrings, 0)
    COPAINS_PARAM("Voice 4 pan", -1000, 1000, 500, kNT_unitHasStrings, 0)
    COPAINS_PARAM("Voice 5 pan", -1000, 1000, -800, kNT_unitHasStrings, 0)
    COPAINS_PARAM("Voice 6 pan", -1000, 1000, 800, kNT_unitHasStrings, 0)

    COPAINS_PARAM("MIDI channel", 0, 16, 1, kNT_unitHasStrings, 0)
    COPAINS_PARAM("Bend range", 1, 24, 7, kNT_unitSemitones, 0)

    NT_PARAMETER_CV_INPUT("Gate input", 0, 0)
    COPAINS_PARAM("CV count", 1, kNumVoices, 1, kNT_unitNone, 0)
    COPAINS_ENUM("Sample & hold", 1, 1, kOffOnStrings)
    COPAINS_PARAM("CV velocity", 0, 1000, 1000, kNT_unitPercent, kNT_scaling10)
    // Read-only: derived from Gate input + CV count so the routing view
    // shows every bus the plug-in reads.
    NT_PARAMETER_CV_INPUT("Pitch input 1", 0, 0)
    NT_PARAMETER_CV_INPUT("Pitch input 2", 0, 0)
    NT_PARAMETER_CV_INPUT("Pitch input 3", 0, 0)
    NT_PARAMETER_CV_INPUT("Pitch input 4", 0, 0)
    NT_PARAMETER_CV_INPUT("Pitch input 5", 0, 0)
    NT_PARAMETER_CV_INPUT("Pitch input 6", 0, 0)

    // NT: the original's -9 dB voice level suits a line output. +6 dB puts one
    // full-velocity voice near 2.4 V peak and keeps six-voice chords inside
    // +/-10 V; +12 dB clipped a measured six-voice chord.
    COPAINS_PARAM("Output gain", -40, 24, 6, kNT_unitDb, 0)
    COPAINS_ENUM("Retrigger", 1, 0, kRetriggerStrings)
    COPAINS_PARAM("Octave", -3, 3, 0, kNT_unitNone, 0)
    // Not used by 6Copains: a bank number for scripts such as 6Copains Banks
    // to follow. Lua script parameters are dropped from the Performance Page
    // when a preset loads; this one, and its CV mapping, stay.
    COPAINS_PARAM("Script bank", 1, 8, 1, kNT_unitNone, 0)
};

#undef COPAINS_PARAM
#undef COPAINS_ENUM

const uint8_t kPageSound[] = { kParamLevel, kParamRamp, kParamCurve, kParamFmRatio, kParamFmIndex, kParamPanDrift };
const uint8_t kPageEnvelope[] = { kParamEnvMode, kParamSlope, kParamTime, kParamRetrigger };
const uint8_t kPageMorph[] = { kParamMorph, kParamRampMod, kParamCurveMod, kParamFmMod, kParamSlopeMod, kParamTimeMod };
const uint8_t kPagePan[] = { kParamPan1, kParamPan1 + 1, kParamPan1 + 2, kParamPan1 + 3, kParamPan1 + 4, kParamPan1 + 5 };
const uint8_t kPageMidi[] = { kParamMidiChannel, kParamBendRange };
const uint8_t kPagePlay[] = { kParamOctave, kParamScriptBank };
const uint8_t kPageCv[] = {
    kParamGateInput, kParamCvCount, kParamSampleHold, kParamCvVelocity,
    kParamPitchInput1, kParamPitchInput1 + 1, kParamPitchInput1 + 2,
    kParamPitchInput1 + 3, kParamPitchInput1 + 4, kParamPitchInput1 + 5,
};
const uint8_t kPageRouting[] = { kParamOutputL, kParamOutputLMode, kParamOutputR, kParamOutputRMode, kParamOutputGain };

const _NT_parameterPage kPages[] = {
    { "Sound", ARRAY_SIZE(kPageSound), 0, { 0, 0 }, kPageSound },
    { "Envelope", ARRAY_SIZE(kPageEnvelope), 0, { 0, 0 }, kPageEnvelope },
    { "Morph", ARRAY_SIZE(kPageMorph), 0, { 0, 0 }, kPageMorph },
    { "Pan", ARRAY_SIZE(kPagePan), 0, { 0, 0 }, kPagePan },
    { "MIDI", ARRAY_SIZE(kPageMidi), 0, { 0, 0 }, kPageMidi },
    { "Play", ARRAY_SIZE(kPagePlay), 0, { 0, 0 }, kPagePlay },
    { "CV/gate", ARRAY_SIZE(kPageCv), 0, { 0, 0 }, kPageCv },
    { "Routing", ARRAY_SIZE(kPageRouting), 0, { 0, 0 }, kPageRouting },
};

const _NT_parameterPages kParameterPages = { ARRAY_SIZE(kPages), kPages };

#ifdef COPAINS_PROFILE
// Temporary on-device profiler (make profile): cycle counts on the display.
struct Profile {
    uint32_t stepCycles = 0, frames = 0, layerFrames = 0;
    uint32_t perSample = 0, perLayerSample = 0, layers = 0;
    uint32_t bench[8] = {};
    bool benchDone = false;
    Engine* scratch = nullptr;  // private engine for isolated timing in draw()
    float dtcBuffer[32] = {};
};
#endif

struct CopainsAlgorithm : public _NT_algorithm {
    Engine* engine = nullptr;
#ifdef COPAINS_PROFILE
    Profile prof;
#endif
    _NT_parameter params[kNumParams];  // instance copy: CV count's max is dynamic
    copains::GateDetector gate;
    int syncedGateBus = -1;
    int grayedGateBus = -1;
    bool needsFullRefresh = true;
    bool routingDirty = true;
    bool gateResync = true;
    bool pitchGrayed = false;
    bool sampleHoldLatched = true;
    float outputGain = kVoltsPerUnit;  // volts per engine unit
};

inline int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

inline float* busPointer(float* busFrames, int numFrames, int hostBus) {
    if (hostBus < 1 || hostBus > kNT_lastBus) return nullptr;
    return busFrames + (hostBus - 1) * numFrames;
}

void applyParameter(CopainsAlgorithm* a, int p) {
    copains::Params& e = a->engine->params;
    const int v = a->v[p];
    switch (p) {
    case kParamLevel: e.level = v * 0.001f; break;
    case kParamRamp: e.ramp = v * 0.001f; break;
    case kParamCurve: e.curve = v * 0.001f; break;
    case kParamFmRatio: e.fmRatio = v * 0.01f; break;
    case kParamFmIndex: e.fmIndex = v * 0.001f; break;
    case kParamPanDrift: e.panDrift = v * 0.001f; break;
    case kParamEnvMode: e.envMode = v; break;
    case kParamSlope: e.slope = v * 0.001f; break;
    case kParamTime: e.time = copains_timeFromIndex(v); break;
    case kParamMorph: e.morph = v * 0.001f; break;
    case kParamRampMod: e.rampMod = v * 0.001f; break;
    case kParamCurveMod: e.curveMod = v * 0.001f; break;
    case kParamFmMod: e.fmMod = v * 0.001f; break;
    case kParamSlopeMod: e.slopeMod = v * 0.001f; break;
    case kParamTimeMod: e.timeMod = v * 0.001f; break;
    case kParamBendRange: e.bendRange = (float)v; break;
    case kParamRetrigger: e.overlap = v != 0; break;
    case kParamOctave: e.octave = v; break;
    case kParamOutputGain: a->outputGain = kVoltsPerUnit * copains::exp2Fast((float)v * (0.16609640f)); break;  // 10^(dB/20)
    case kParamGateInput:
    case kParamCvCount:
        a->routingDirty = true;
        break;
    default:
        if (p >= kParamPan1 && p < kParamPan1 + kNumVoices) e.pan[p - kParamPan1] = v * 0.001f;
        else if (p >= kParamPitchInput1 && p < kParamPitchInput1 + kNumVoices) a->routingDirty = true;
        break;
    }
}

// Derive the CV partition and the read-only pitch-input parameters from
// Gate input + CV count. Pitch voice i reads host bus gate + i + 1.
void syncRouting(CopainsAlgorithm* a) {
    a->routingDirty = false;
    const int32_t index = NT_algorithmIndex(a);
    const uint32_t offset = NT_parameterOffset();

    const int gateBus = a->v[kParamGateInput];
    const int room = gateBus > 0 ? kNT_lastBus - gateBus : kNumVoices;
    const int16_t maxCount = (int16_t)clampi(room, 1, kNumVoices);
    if (a->params[kParamCvCount].max != maxCount) {
        a->params[kParamCvCount].max = maxCount;
        if (index >= 0) NT_updateParameterDefinition(index, kParamCvCount);
    }
    int count = a->v[kParamCvCount];
    if (count > maxCount) {
        count = maxCount;
        if (index >= 0) NT_setParameterFromAudio(index, kParamCvCount + offset, maxCount);
    }
    const int effective = gateBus > 0 ? clampi(count < room ? count : room, 0, kNumVoices) : 0;

    if (gateBus != a->syncedGateBus) {
        a->engine->cvRelease();
        a->gateResync = true;
        a->syncedGateBus = gateBus;
    }
    a->engine->setCvVoiceCount(effective);

    if (index < 0) return;
    for (int i = 0; i < kNumVoices; ++i) {
        const int16_t want = (int16_t)(i < effective ? gateBus + i + 1 : 0);
        if (a->v[kParamPitchInput1 + i] != want)
            NT_setParameterFromAudio(index, kParamPitchInput1 + i + offset, want);
    }
    if (!a->pitchGrayed) {
        for (int i = 0; i < kNumVoices; ++i)
            NT_setParameterGrayedOut(index, kParamPitchInput1 + i + offset, true);
        a->pitchGrayed = true;
    }
    if ((gateBus > 0) != (a->grayedGateBus > 0) || a->grayedGateBus < 0) {
        const bool gray = gateBus <= 0;
        NT_setParameterGrayedOut(index, kParamCvCount + offset, gray);
        NT_setParameterGrayedOut(index, kParamSampleHold + offset, gray);
        NT_setParameterGrayedOut(index, kParamCvVelocity + offset, gray);
        a->grayedGateBus = gateBus;
    }
}

void writeOutput(float* out, const float* src, bool replace, float gain, int start, int n) {
    if (!out) return;
    out += start;
    if (replace) {
        for (int i = 0; i < n; ++i) out[i] = src[i] * gain;
    } else {
        for (int i = 0; i < n; ++i) out[i] += src[i] * gain;
    }
}

struct StepContext {
    CopainsAlgorithm* a;
    float* outL;
    float* outR;  // nullptr = mono into outL
    bool replaceL, replaceR;
    const float* pitch[kNumVoices];
    int cvCount;
};

void renderRange(StepContext& c, int start, int end) {
    Engine& e = *c.a->engine;
    while (start < end) {
        const int n = end - start < kMaxChunk ? end - start : kMaxChunk;
        if (!c.a->sampleHoldLatched && e.cvGated()) {
            for (int v = 0; v < c.cvCount; ++v)
                if (c.pitch[v]) e.cvSetNote(v, copains::voltsToNote(c.pitch[v][start]));
        }
        float bufL[kMaxChunk], bufR[kMaxChunk];
        e.render(bufL, c.outR ? bufR : nullptr, n);
        writeOutput(c.outL, bufL, c.replaceL, c.a->outputGain, start, n);
        if (c.outR) writeOutput(c.outR, bufR, c.replaceR, c.a->outputGain, start, n);
        start += n;
    }
}

void calculateRequirements(_NT_algorithmRequirements& req, const int32_t*) {
    req.numParameters = kNumParams;
    req.sram = sizeof(CopainsAlgorithm);
    req.dram = 0;
#ifdef COPAINS_PROFILE
    req.dtc = 2 * sizeof(Engine);
#else
    req.dtc = sizeof(Engine);
#endif
    req.itc = 0;
}

_NT_algorithm* construct(const _NT_algorithmMemoryPtrs& ptrs, const _NT_algorithmRequirements&, const int32_t*) {
    CopainsAlgorithm* a = new (ptrs.sram) CopainsAlgorithm();
    a->engine = new (ptrs.dtc) Engine();
    a->engine->init((float)NT_globals.sampleRate);
#ifdef COPAINS_PROFILE
    a->prof.scratch = new (ptrs.dtc + sizeof(Engine)) Engine();
    a->prof.scratch->init((float)NT_globals.sampleRate);
#endif
    for (int i = 0; i < kNumParams; ++i) a->params[i] = kParameterTemplate[i];
    a->parameters = a->params;
    a->parameterPages = &kParameterPages;
    return a;
}

void parameterChanged(_NT_algorithm* self, int p) {
    CopainsAlgorithm* a = static_cast<CopainsAlgorithm*>(self);
    if (p < 0 || p >= kNumParams) return;
    applyParameter(a, p);
}

void stepBody(CopainsAlgorithm* a, float* busFrames, int numFramesBy4);

void step(_NT_algorithm* self, float* busFrames, int numFramesBy4) {
    CopainsAlgorithm* a = static_cast<CopainsAlgorithm*>(self);
#ifdef COPAINS_PROFILE
    const uint32_t t0 = NT_getCpuCycleCount();
    stepBody(a, busFrames, numFramesBy4);
    const uint32_t dt = NT_getCpuCycleCount() - t0;
    Profile& p = a->prof;
    const int layers = a->engine->activeLayerCount();
    p.stepCycles += dt;
    p.frames += (uint32_t)numFramesBy4 * 4;
    p.layerFrames += (uint32_t)(numFramesBy4 * 4 * layers);
    if (p.frames >= 12000) {
        p.perSample = p.stepCycles / p.frames;
        p.perLayerSample = p.layerFrames ? p.stepCycles / p.layerFrames : 0;
        p.layers = (uint32_t)layers;
        p.stepCycles = p.frames = p.layerFrames = 0;
    }
#else
    stepBody(a, busFrames, numFramesBy4);
#endif
}

void stepBody(CopainsAlgorithm* a, float* busFrames, int numFramesBy4) {
    if (a->needsFullRefresh) {
        for (int p = 0; p < kNumParams; ++p) applyParameter(a, p);
        a->needsFullRefresh = false;
    }
    if (a->routingDirty) syncRouting(a);

    const int numFrames = numFramesBy4 * 4;
    Engine& e = *a->engine;

    StepContext c;
    c.a = a;
    c.outL = busPointer(busFrames, numFrames, a->v[kParamOutputL]);
    c.outR = busPointer(busFrames, numFrames, a->v[kParamOutputR]);
    if (c.outR == c.outL) c.outR = nullptr;
    c.replaceL = a->v[kParamOutputLMode] != 0;
    c.replaceR = a->v[kParamOutputRMode] != 0;
    c.cvCount = e.cvVoiceCount();
    const int gateBus = a->v[kParamGateInput];
    const float* gate = c.cvCount > 0 ? busPointer(busFrames, numFrames, gateBus) : nullptr;
    for (int v = 0; v < kNumVoices; ++v)
        c.pitch[v] = v < c.cvCount ? busPointer(busFrames, numFrames, gateBus + v + 1) : nullptr;

    if (gate && a->gateResync) {
        a->gate.resync(gate[0]);
        a->gateResync = false;
    }

    // Sample-accurate gate edges: render up to the edge, then act on it.
    int start = 0;
    for (int i = 0; i < numFrames; ++i) {
        if (gate) {
            const int edge = a->gate.process(gate[i]);
            if (edge != 0) {
                renderRange(c, start, i);
                start = i;
                if (edge > 0) {
                    float notes[kNumVoices];
                    for (int v = 0; v < c.cvCount; ++v)
                        notes[v] = c.pitch[v] ? copains::voltsToNote(c.pitch[v][i]) : copains::kZeroVoltNote;
                    a->sampleHoldLatched = a->v[kParamSampleHold] != 0;
                    e.cvGateOn(notes, a->v[kParamCvVelocity] * 0.001f);
                } else {
                    e.cvGateOff();
                }
            }
        }
        if (i + 1 - start >= kMaxChunk) {
            renderRange(c, start, i + 1);
            start = i + 1;
        }
    }
    renderRange(c, start, numFrames);
}

void midiMessage(_NT_algorithm* self, uint8_t byte0, uint8_t byte1, uint8_t byte2) {
    CopainsAlgorithm* a = static_cast<CopainsAlgorithm*>(self);
    const int channel = a->v[kParamMidiChannel];
    if (channel != 0 && (byte0 & 0x0F) != channel - 1) return;
    Engine& e = *a->engine;
    const int data1 = byte1 & 0x7F;
    const int data2 = byte2 & 0x7F;
    switch (byte0 & 0xF0) {
    case 0x80:
        e.noteOff(data1);
        break;
    case 0x90:
        if (data2 == 0) e.noteOff(data1);
        else e.noteOn(data1, data2 * (1.0f / 127.0f));
        break;
    case 0xB0:
        if (data1 == 64) e.setSustain(data2 >= 64);
        else if (data1 == 120 || data1 == 123) e.panic();
        else if (data1 == 121) e.resetControllers();
        break;
    case 0xE0:
        e.setPitchBend((float)(((data2 << 7) | data1) - 8192) * (1.0f / 8192.0f));
        break;
    default:
        break;
    }
}

void midiRealtime(_NT_algorithm* self, uint8_t byte) {
    if (byte == 0xFC || byte == 0xFF) static_cast<CopainsAlgorithm*>(self)->engine->panic();
}

int parameterString(_NT_algorithm*, int p, int v, char* buff) {
    return copains_formatParameter(p, v, buff);
}

#ifdef COPAINS_PROFILE
int appendText(char* buff, int len, const char* text);
int appendInt(char* buff, int len, int value);

template <typename F>
uint32_t bestOf(F f) {
    uint32_t best = 0xFFFFFFFFu;
    for (int r = 0; r < 7; ++r) {
        const uint32_t t0 = NT_getCpuCycleCount();
        f();
        const uint32_t dt = NT_getCpuCycleCount() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

void runBenchmarks(CopainsAlgorithm* a) {
    Profile& p = a->prof;
    static volatile float sink = 0.0f;
    // 0: dependent multiply-add chain, cycles per op (FP latency)
    p.bench[0] = bestOf([] { float x = 1.0f; for (int i = 0; i < 1000; ++i) x = x * 0.999f + 0.001f; sink = x; }) / 1000;
    // 1: exp2Fast, cycles each x10
    p.bench[1] = bestOf([] { float s = 0.0f; for (int i = 0; i < 1000; ++i) s += copains::exp2Fast((float)i * 0.001f); sink = s; }) / 100;
    // 2: tanh table, cycles each x10
    Engine* e = p.scratch;
    p.bench[2] = bestOf([e] { float s = 0.0f; for (int i = 0; i < 1000; ++i) s += e->tanhAt((float)i * 0.002f - 1.0f); sink = s; }) / 100;
    // 3: read-modify-write to a stack buffer, cycles each x10
    p.bench[3] = bestOf([] {
        float buf[32];
        for (int i = 0; i < 32; ++i) buf[i] = 0.0f;
        for (int i = 0; i < 1000; ++i) buf[i & 31] += 1.0f;
        sink = buf[5];
    }) / 100;
    // 4: same on a buffer in the instance's DTC block
    float* d = p.dtcBuffer;
    p.bench[4] = bestOf([d] { for (int i = 0; i < 1000; ++i) d[i & 31] += 1.0f; sink = d[5]; }) / 100;
    // 5/6: one isolated note, cycles per sample: plain sine, then square + intone FM
    float L[copains::kMaxChunk], R[copains::kMaxChunk];
    for (int pass = 0; pass < 2; ++pass) {
        e->init((float)NT_globals.sampleRate);
        e->params.curve = pass ? -0.9f : 1.0f;
        e->params.fmIndex = pass ? -1.0f : 0.0f;
        e->params.fmMod = 0.0f;
        e->params.envMode = copains::kModeASR;
        e->params.time = 2.0f;
        e->params.slope = 0.0f;
        e->setCvVoiceCount(0);
        e->noteOn(pass ? 69 : 57, 1.0f);
        for (int w = 0; w < 20; ++w) e->render(L, R, copains::kMaxChunk);
        p.bench[5 + pass] = bestOf([e, &L, &R] { for (int k = 0; k < 10; ++k) e->render(L, R, copains::kMaxChunk); })
                            / (10 * copains::kMaxChunk);
    }
    p.benchDone = true;
    (void)sink;
}

bool profileDraw(_NT_algorithm* self) {
    CopainsAlgorithm* a = static_cast<CopainsAlgorithm*>(self);
    Profile& p = a->prof;
    if (!p.benchDone) runBenchmarks(a);
    char line[64];
    auto row = [&](int y, const char* label, uint32_t v1, const char* mid, uint32_t v2) {
        int n = appendText(line, 0, label);
        n = appendInt(line, n, (int)v1);
        if (mid) {
            n = appendText(line, n, mid);
            n = appendInt(line, n, (int)v2);
        }
        NT_drawText(0, y, line, 15, kNT_textLeft, kNT_textTiny);
    };
    row(6, "STEP CYC/SAMPLE ", p.perSample, "  LAYERS ", p.layers);
    row(13, "CYC PER LAYER-SAMPLE ", p.perLayerSample, nullptr, 0);
    row(20, "FMA LATENCY ", p.bench[0], "  EXP2 X10 ", p.bench[1]);
    row(27, "TANH TABLE X10 ", p.bench[2], nullptr, 0);
    row(34, "STACK RMW X10 ", p.bench[3], "  DTC RMW X10 ", p.bench[4]);
    row(41, "1 NOTE SINE CYC/SAMPLE ", p.bench[5], nullptr, 0);
    row(48, "1 NOTE SQR+FM CYC/SAMPLE ", p.bench[6], nullptr, 0);
    row(56, "SAMPLE RATE ", NT_globals.sampleRate, nullptr, 0);
    return true;
}
#endif

const _NT_factory kFactory = {
    .guid = kCopainsGuid,
    .name = "6Copains",
    .description = "Just Friends-style 6-voice synth, after nb_monamie",
    .numSpecifications = 0,
    .specifications = nullptr,
    .calculateStaticRequirements = nullptr,
    .initialise = nullptr,
    .calculateRequirements = calculateRequirements,
    .construct = construct,
    .parameterChanged = parameterChanged,
    .step = step,
#ifdef COPAINS_PROFILE
    .draw = profileDraw,
#else
    .draw = nullptr,
#endif
    .midiRealtime = midiRealtime,
    .midiMessage = midiMessage,
    .tags = kNT_tagInstrument,
    .hasCustomUi = nullptr,
    .customUi = nullptr,
    .setupUi = nullptr,
    .serialise = nullptr,
    .deserialise = nullptr,
    .midiSysEx = nullptr,
    .parameterUiPrefix = nullptr,
    .parameterString = parameterString,
    .stringParameterBuffer = nullptr,
};

// ---------------------------------------------------------- formatting

int appendText(char* buff, int len, const char* text) {
    while (*text && len < kNT_parameterStringSize - 1) buff[len++] = *text++;
    buff[len] = 0;
    return len;
}

int appendInt(char* buff, int len, int value) {
    char digits[12];
    int n = 0;
    bool negative = value < 0;
    unsigned u = negative ? (unsigned)(-value) : (unsigned)value;
    do {
        digits[n++] = (char)('0' + u % 10);
        u /= 10;
    } while (u && n < 11);
    if (negative && len < kNT_parameterStringSize - 1) buff[len++] = '-';
    while (n && len < kNT_parameterStringSize - 1) buff[len++] = digits[--n];
    buff[len] = 0;
    return len;
}

// Non-negative value with a fixed number of decimals.
int appendFixed(char* buff, int len, float value, int decimals) {
    int scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const int scaled = (int)(value * (float)scale + 0.5f);
    len = appendInt(buff, len, scaled / scale);
    if (decimals > 0) {
        len = appendText(buff, len, ".");
        int frac = scaled % scale;
        for (int d = scale / 10; d > 0; d /= 10) {
            const char digit[2] = { (char)('0' + (frac / d) % 10), 0 };
            len = appendText(buff, len, digit);
        }
    }
    return len;
}

inline int roundedPercent(int v) { return ((v < 0 ? -v : v) + 5) / 10; }  // v in 0.1 %

}  // namespace

float copains_timeFromIndex(int index) {
    const float t = (float)clampi(index, 0, 1000) * 0.001f;
    return kTimeMinSeconds * copains::expFast(t * kTimeLogRange);
}

int copains_formatParameter(int p, int v, char* buff) {
    int len = 0;
    buff[0] = 0;
    switch (p) {
    case kParamRamp: {  // rise/fall share of the cycle
        const int rise = (v + 1000 + 10) / 20;
        len = appendInt(buff, len, rise);
        len = appendText(buff, len, "/");
        return appendInt(buff, len, 100 - rise);
    }
    case kParamCurve: {
        const int pct = roundedPercent(v);
        if (v <= -970) return appendText(buff, 0, "sqr");
        if (v >= 998) return appendText(buff, 0, "sin");
        if (v > -20 && v < 20) return appendText(buff, 0, "tri");
        len = appendText(buff, len, v < 0 ? "sqr" : "tri");
        len = appendInt(buff, len, v < 0 ? pct : 100 - pct);
        len = appendText(buff, len, v < 0 ? " tri" : " sin");
        return appendInt(buff, len, v < 0 ? 100 - pct : pct);
    }
    case kParamFmIndex:
        if (v > -10 && v < 10) return appendText(buff, 0, "0%");
        len = appendText(buff, len, v < 0 ? "intone " : "linear ");
        len = appendInt(buff, len, roundedPercent(v));
        return appendText(buff, len, "%");
    case kParamSlope: {  // attack/release share of Time
        const int attack = roundedPercent(v);
        len = appendInt(buff, len, attack);
        len = appendText(buff, len, "/");
        return appendInt(buff, len, 100 - attack);
    }
    case kParamTime: {
        const float t = copains_timeFromIndex(v);
        len = appendFixed(buff, len, t, t < 0.1f ? 3 : 2);
        return appendText(buff, len, "s");
    }
    case kParamMidiChannel:
        if (v <= 0) return appendText(buff, 0, "Omni");
        return appendInt(buff, 0, v);
    default:
        if (p >= kParamPan1 && p < kParamPan1 + kNumVoices) {
            if (v > -10 && v < 10) return appendText(buff, 0, "> <");
            if (v < 0) {
                len = appendText(buff, len, "L < ");
                return appendInt(buff, len, roundedPercent(v));
            }
            len = appendInt(buff, len, roundedPercent(v));
            return appendText(buff, len, " > R");
        }
        return 0;
    }
}

const _NT_parameter* copains_parameterTemplate() { return kParameterTemplate; }

copains::Engine* copains_engineOf(_NT_algorithm* algorithm) {
    return static_cast<CopainsAlgorithm*>(algorithm)->engine;
}

uintptr_t pluginEntry(_NT_selector selector, uint32_t data) {
    switch (selector) {
    case kNT_selector_version:
        return kNT_apiVersionCurrent;
    case kNT_selector_numFactories:
        return 1;
    case kNT_selector_factoryInfo:
        return (uintptr_t)(data == 0 ? &kFactory : nullptr);
    }
    return 0;
}
