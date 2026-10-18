// SPDX-License-Identifier: GPL-3.0-or-later
//
// 6Copains for disting NT - synthesis engine.
//
// A port of nb_monamie v1.0 by @sonoCircuit
// (https://github.com/sonocircuit/nb_monamie), "an approximation of the
// mannequins module 'just friends' by whimsical raps in synthesis mode",
// whose waveshaping is based on the Imaginary Friends engine by @synthetivv
// (https://github.com/monome-community/exquisite-script/blob/master/lib/Engine_ImaginaryFriends.sc).
//
// This header has no disting NT API dependencies, so the native tests drive it
// directly. It follows the SuperCollider SynthDef \monAmie (lib/nb_monamie.sc)
// and the norns LRU voice allocator (lib/mod.lua). Deliberate deviations are
// marked "NT:".

#ifndef COPAINS_ENGINE_H
#define COPAINS_ENGINE_H

#include <stdint.h>

// Unroll the independent per-sample passes so the in-order Cortex-M7 can
// overlap one sample's latency with the next sample's work.
#if defined(__clang__)
#define COPAINS_UNROLL _Pragma("clang loop unroll_count(4)")
#elif defined(__GNUC__)
#define COPAINS_UNROLL _Pragma("GCC unroll 4")
#else
#define COPAINS_UNROLL
#endif

namespace copains {

constexpr int kNumVoices = 6;
constexpr int kNumLayers = 16;  // sounding instances: one per voice plus fading tails
constexpr int kMaxChunk = 32;   // frames per render() call (the control-rate block)

constexpr float kZeroVoltNote = 48.0f;  // 0 V = MIDI note 48 = 130.81 Hz, as the NT's own sequencers
constexpr float kGateRiseVolts = 1.0f;
constexpr float kGateFallVolts = 0.5f;
constexpr float kForcedReleaseSeconds = 0.05f;  // SuperCollider gate -1.05
constexpr float kOutputGain = 0.35481339f;      // -9 dB
constexpr float kMonoGain = 0.70710678f;        // level of a centre-panned voice
constexpr int kTanhTableSize = 1024;            // tanh over [-kTanhRange, kTanhRange]
constexpr float kTanhRange = 8.0f;
constexpr int kFilterTableSize = 256;           // RLPF coefficients over envelope 0..1
constexpr int kOverlapHeadroom = 3;             // free layers kept for new notes in Overlap mode

enum EnvMode : uint8_t { kModeAR = 0, kModeASR = 1, kModeCycle = 2 };

enum Stage : uint8_t {
    kStageIdle = 0,
    kStageAttack,
    kStageFall,     // AR release segment, or cycle falling segment
    kStageRise,     // cycle rising segment
    kStageSustain,  // ASR hold
    kStageRelease,  // gate released (ASR, cycle)
    kStageForced,   // 50 ms fade before reuse: steal, retrigger, panic
};

enum Owner : uint8_t { kOwnerMidi = 0, kOwnerCv = 1 };

// ------------------------------------------------------------------ math

// Branch-free on the Cortex-M7 (FPv5): these become vmaxnm/vminnm, vabs and
// vrint*. A float compare that feeds a branch needs vcmp + vmrs, which waits
// for the FP pipeline to drain; the hot path avoids them.
inline float clampf(float x, float lo, float hi) { return __builtin_fminf(__builtin_fmaxf(x, lo), hi); }
inline float maxf(float a, float b) { return __builtin_fmaxf(a, b); }
inline float absf(float x) { return __builtin_fabsf(x); }
inline float floorFast(float x) { return __builtin_floorf(x); }

// 2^x with relative error around 2e-7 (2^-125 at the bottom of the range).
inline float exp2Fast(float x) {
    x = clampf(x, -125.0f, 125.0f);
    const float r = __builtin_rintf(x);
    const float f = x - r;
    const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.055504109f
                  + f * (0.0096181291f + f * (0.0013333558f + f * 0.00015403530f)))));
    union { float f; int32_t i; } u;
    u.i = ((int32_t)r + 127) << 23;
    return p * u.f;
}

inline float expFast(float x) { return exp2Fast(x * 1.44269504f); }

inline float tanhFast(float x) {
    if (x > 9.0f) return 1.0f;
    if (x < -9.0f) return -1.0f;
    const float x2 = x * x;
    if (x2 < 0.0025f) return x * (1.0f - x2 * (1.0f / 3.0f));
    const float e = exp2Fast(2.88539008f * x);  // e^(2x)
    return (e - 1.0f) / (e + 1.0f);
}

// sin(2*pi*t), t in turns.
inline float sinTurns(float t) {
    t -= __builtin_rintf(t);  // [-0.5, 0.5]
    const float a = __builtin_fabsf(t);
    // fold into [-0.25, 0.25]: sin(2 pi t) = sin(2 pi (0.5 - t))
    const float x = __builtin_copysignf(__builtin_fminf(a, 0.5f - a), t) * 6.28318531f;
    const float x2 = x * x;
    return x * (1.0f + x2 * (-0.16666667f + x2 * (0.0083333333f
             + x2 * (-0.00019841270f + x2 * 2.7557319e-6f))));
}

inline float cosTurns(float t) { return sinTurns(t + 0.25f); }

// SuperCollider RLPF coefficients, rq = 0.78. turns = cutoff / sampleRate.
inline void rlpfCoefficients(float turns, float& a0, float& b1, float& b2) {
    const float half = turns * 0.78f * 0.5f;
    const float d = sinTurns(half) / cosTurns(half);
    const float c = (1.0f - d) / (1.0f + d);
    b1 = (1.0f + c) * cosTurns(turns);
    b2 = -c;
    a0 = (1.0f + c - b1) * 0.25f;
}

struct Rng {
    uint32_t s = 0x9E3779B9u;
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
    float bipolar() { return (float)(int32_t)next() * (1.0f / 2147483648.0f); }
};

// 1 V/oct, 0 V = MIDI note 48. Out-of-range or non-finite CV is clamped.
inline float voltsToNote(float volts) {
    if (!(volts == volts)) volts = 0.0f;
    return kZeroVoltNote + 12.0f * clampf(volts, -10.0f, 10.0f);
}

// Schmitt trigger: rises above 1.0 V, falls below 0.5 V.
struct GateDetector {
    bool high = false;
    int process(float volts) {
        if (!high && volts > kGateRiseVolts) {
            high = true;
            return 1;
        }
        if (high && volts < kGateFallVolts) {
            high = false;
            return -1;
        }
        return 0;
    }
    // Adopt the current level without reporting an edge (fresh-edge rule).
    void resync(float volts) { high = volts > kGateRiseVolts; }
};

// --------------------------------------------------------------- engine

// SuperCollider's VarSaw, sample for sample. The phase wraps only upwards and
// only once per sample, so under heavy FM it runs below 0 or above 1 and the
// output leaves [-1, 1]. That (with the shaper's input clip) is much of the
// original's FM grit, so it is reproduced rather than "fixed".
struct VarSaw {
    // A width with its two slopes, computed once per block.
    struct Width {
        float duty = 0.5f, invDuty = 4.0f, invOneMinusDuty = 4.0f;
        explicit Width(float width = 0.5f) {
            duty = clampf(width, 0.001f, 0.999f);
            invDuty = 2.0f / duty;
            invOneMinusDuty = 2.0f / (1.0f - duty);
        }
    };

    float phase = 0.25f;
    Width w;

    void reset(float width) {
        w = Width(width);
        phase = w.duty * 0.5f;  // iphase = width / 2: starts at zero
    }
    // The new width is latched only when the phase wraps, as VarSaw reads its
    // width input. Written without float branches (see clampf):
    //  - the wrap subtracts 1 once when phase >= 1 and never wraps negatives:
    //    clamp(floor(phase), 0, 1) is exactly that;
    //  - phase < duty ? rising : falling equals min(rising, falling), because
    //    the two lines cross at the duty point - for every phase, including FM
    //    excursions below 0 and above 1.
    float next(float inc, const Width& next) {
        const float wrap = clampf(__builtin_floorf(phase), 0.0f, 1.0f);
        phase -= wrap;
        w.duty += wrap * (next.duty - w.duty);
        w.invDuty += wrap * (next.invDuty - w.invDuty);
        w.invOneMinusDuty += wrap * (next.invOneMinusDuty - w.invOneMinusDuty);
        const float z = __builtin_fminf(phase * w.invDuty, (1.0f - phase) * w.invOneMinusDuty);
        phase = clampf(phase + inc, -64.0f, 64.0f);  // guard; normal FM stays within a few cycles
        return z - 1.0f;
    }
};

// Values mirror nb_monamie's parameter ranges; defaults are its default.pam.
struct Params {
    float level = 0.8f;      // 0..1
    float ramp = 0.08f;      // -1..1
    float curve = 1.0f;      // -1 square, 0 triangle, 1 sine
    float fmRatio = 1.01f;   // 0.5..2
    float fmIndex = 0.0f;    // -1..1, negative = intone
    float panDrift = 0.12f;  // 0..1
    int envMode = kModeAR;
    float slope = 0.03f;     // 0..1 attack share of time
    float time = 0.2108f;    // seconds
    float morph = 0.0f;      // 0..1
    float rampMod = 0.61f;
    float curveMod = 0.0f;
    float fmMod = -0.51f;
    float slopeMod = 0.0f;
    float timeMod = 0.0f;    // seconds at full morph
    // NT: per-voice pan replaces the global pan.
    float pan[kNumVoices] = { -0.2f, 0.2f, -0.5f, 0.5f, -0.8f, 0.8f };
    float bendRange = 7.0f;  // semitones
    // NT: Overlap lets a retriggered voice's old note finish its own envelope
    // instead of fading it out over 50 ms.
    bool overlap = false;
    int octave = 0;  // NT: transposes every note it starts (MIDI and CV), -3..+3
};

struct Layer {
    bool active = false;
    bool gate = false;
    bool bendable = false;
    bool segLinear = true;
    uint8_t voice = 0;
    uint8_t mode = kModeAR;
    uint8_t stage = kStageIdle;
    float env = 0.0f;
    float vel = 0.0f;
    float note = 60.0f;
    float segY0 = 0.0f, segDy = 0.0f, segInvN = 1.0f, segC = 0.0f, segInvDen = 1.0f;
    float segE = 1.0f, segG = 1.0f;  // exp(c * progress) and its per-sample factor
    int32_t segK = 0, segN = 1;
    float panRand = 0.0f, pan = 0.0f;
    float panUsed = -10.0f, gainL = 0.0f, gainR = 0.0f;  // cached Pan2 gains
    float semisUsed = -1000.0f, freq = 20.0f;            // cached pitch
    VarSaw osc;
    float modPhase = 0.0f, prevTri = 0.0f;
    float y1 = 0.0f, y2 = 0.0f;
};

struct Voice {
    uint8_t owner = kOwnerMidi;
    int8_t layer = -1;  // current layer, -1 when silent
    int8_t note = -1;   // MIDI note held or sustained on this voice
    bool held = false;
    bool sustained = false;
    uint32_t age = 0;
};

class Engine {
public:
    Params params;

    void init(float sampleRate) {
        sr_ = sampleRate > 1000.0f ? sampleRate : 48000.0f;
        invSr_ = 1.0f / sr_;
        maxFreq_ = sr_ * 0.45f < 20000.0f ? sr_ * 0.45f : 20000.0f;
        for (int i = 0; i < kNumLayers; ++i) layers_[i] = Layer();
        for (int i = 0; i < kNumVoices; ++i) voices_[i] = Voice();
        cvCount_ = 0;
        cvGated_ = false;
        sustainDown_ = false;
        bendNorm_ = 0.0f;
        ageCounter_ = 0;
        smoothInit_ = false;
        for (int i = 0; i <= kTanhTableSize; ++i)
            tanhTable_[i] = tanhFast(((float)i * (2.0f / kTanhTableSize) - 1.0f) * kTanhRange);
        // RLPF(env.linexp(0, 1, 120, 20000), 0.78) depends only on the envelope.
        for (int i = 0; i <= kFilterTableSize; ++i) {
            const float amp = (float)i / kFilterTableSize;
            const float cutoff = clampf(120.0f * expFast(amp * 5.11599581f), 20.0f, maxFreq_);
            rlpfCoefficients(cutoff * invSr_, filterTable_[i][0], filterTable_[i][1], filterTable_[i][2]);
        }
    }

    // ---- MIDI-style control (voices outside the CV partition)

    void noteOn(int note, float velocity) {
        if (note < 0 || note > 127 || cvCount_ >= kNumVoices) return;
        if (velocity <= 0.0f) {
            noteOff(note);
            return;
        }
        int v = findMidiVoice(note);  // same note again reuses its voice
        if (v < 0) v = pickMidiVoice();
        Voice& vc = voices_[v];
        vc.age = ++ageCounter_;
        vc.note = (int8_t)note;
        vc.held = true;
        vc.sustained = false;
        startLayer(v, (float)note, velocity, true);
    }

    void noteOff(int note) {
        for (int v = cvCount_; v < kNumVoices; ++v) {
            Voice& vc = voices_[v];
            if (vc.note != note || !vc.held) continue;
            vc.held = false;
            if (sustainDown_) {
                vc.sustained = true;
            } else {
                vc.note = -1;
                gateOffVoice(v);
            }
            return;
        }
    }

    void setSustain(bool down) {
        sustainDown_ = down;
        if (down) return;
        for (int v = cvCount_; v < kNumVoices; ++v) {
            Voice& vc = voices_[v];
            if (!vc.sustained) continue;
            vc.sustained = false;
            vc.note = -1;
            gateOffVoice(v);
        }
    }

    void setPitchBend(float normalized) { bendNorm_ = clampf(normalized, -1.0f, 1.0f); }

    void resetControllers() {
        bendNorm_ = 0.0f;
        setSustain(false);
    }

    // Fade everything out over 50 ms and clear all note/controller state.
    void panic() {
        for (int i = 0; i < kNumLayers; ++i) forceRelease(i);
        for (int v = 0; v < kNumVoices; ++v) {
            voices_[v].note = -1;
            voices_[v].held = false;
            voices_[v].sustained = false;
        }
        sustainDown_ = false;
        bendNorm_ = 0.0f;
        cvGated_ = false;
    }

    // ---- CV/gate control (voices 0 .. count-1)

    void setCvVoiceCount(int count) {
        count = count < 0 ? 0 : (count > kNumVoices ? kNumVoices : count);
        if (count == cvCount_) return;
        for (int v = 0; v < kNumVoices; ++v) {
            const uint8_t owner = v < count ? kOwnerCv : kOwnerMidi;
            if (voices_[v].owner == owner) continue;
            retireVoice(v);
            voices_[v].owner = owner;
        }
        cvCount_ = count;
    }

    // notes[] holds cvVoiceCount() MIDI note numbers (may be fractional).
    void cvGateOn(const float* notes, float velocity) {
        for (int v = 0; v < cvCount_; ++v) {
            Voice& vc = voices_[v];
            vc.held = true;
            vc.age = ++ageCounter_;
            startLayer(v, notes[v], velocity, false);
        }
        cvGated_ = true;
    }

    void cvGateOff() {
        for (int v = 0; v < cvCount_; ++v) {
            voices_[v].held = false;
            gateOffVoice(v);
        }
        cvGated_ = false;
    }

    void cvSetNote(int v, float note) {
        if (v < 0 || v >= cvCount_ || !voices_[v].held || voices_[v].layer < 0) return;
        layers_[voices_[v].layer].note = note + octaveShift();
    }

    // Routing changed: fade the CV voices and forget the gate.
    void cvRelease() {
        for (int v = 0; v < cvCount_; ++v) retireVoice(v);
        cvGated_ = false;
    }

    // ---- audio

    // Overwrites n <= kMaxChunk frames. outR == nullptr renders a mono mix to outL.
    void render(float* outL, float* outR, int n) {
        if (n > kMaxChunk) n = kMaxChunk;
        if (n <= 0) return;
        for (int i = 0; i < n; ++i) {
            outL[i] = 0.0f;
            if (outR) outR[i] = 0.0f;
        }
        updateControl(n);
        for (int li = 0; li < kNumLayers; ++li) {
            Layer& L = layers_[li];
            if (!L.active) continue;
            renderLayer(L, outL, outR, n);
            if (L.stage == kStageIdle) deactivate(li);
        }
    }

    // ---- introspection (tests, display)

    float sampleRate() const { return sr_; }
    int cvVoiceCount() const { return cvCount_; }
    bool cvGated() const { return cvGated_; }
    bool sustainDown() const { return sustainDown_; }
    const Voice& voice(int v) const { return voices_[v]; }
    const Layer& layer(int i) const { return layers_[i]; }
    int activeLayerCount() const {
        int n = 0;
        for (int i = 0; i < kNumLayers; ++i) n += layers_[i].active ? 1 : 0;
        return n;
    }
    float attackSeconds() const { return sm_.atk; }
    float sineShapeAt(float tri) const { return tanhTable(1.86f * tri); }
    float tanhAt(float x) const { return tanhTable(x); }
    float releaseSeconds() const { return sm_.rel; }

private:
    struct Control {
        float ramp = 0.0f, curve = 0.0f, fm = 0.0f, atk = 0.001f, rel = 0.001f;
        float level = 0.0f, bend = 0.0f;
    };

    Control targets() const {
        const Params& p = params;
        const float morph = clampf(p.morph, 0.0f, 1.0f);
        Control t;
        t.ramp = clampf(p.ramp + p.rampMod * morph, -1.0f, 1.0f);
        t.curve = clampf(p.curve + clampf(p.curveMod * morph, -1.0f, 1.0f), -1.0f, 1.0f);
        t.fm = clampf(p.fmIndex + p.fmMod * morph, -1.0f, 1.0f);
        const float dur = clampf(p.time + p.timeMod * morph, 0.001f, 30.0f);
        const float ratio = clampf(p.slope + p.slopeMod * morph, 0.0f, 1.0f);
        t.atk = 0.001f + ratio * (dur - 0.001f);
        t.rel = dur - ratio * (dur - 0.001f);
        t.level = clampf(p.level, 0.0f, 1.0f);
        t.bend = bendNorm_ * clampf(p.bendRange, 0.0f, 48.0f);
        return t;
    }

    // Lag.kr smoothing (60 dB in 100 ms) and per-block derived values.
    void updateControl(int n) {
        const Control t = targets();
        const float k = -6.9077553f * (float)n * invSr_;  // ln(0.001) * seconds
        if (!smoothInit_) {
            sm_ = t;
            smoothInit_ = true;
        } else {
            const float slow = expFast(k * 10.0f);    // 100 ms
            const float fast = expFast(k * 100.0f);   // 10 ms
            const float bend = expFast(k * 200.0f);   // 5 ms
            sm_.ramp = t.ramp + slow * (sm_.ramp - t.ramp);
            sm_.curve = t.curve + slow * (sm_.curve - t.curve);
            sm_.fm = t.fm + slow * (sm_.fm - t.fm);
            sm_.atk = t.atk + slow * (sm_.atk - t.atk);
            sm_.rel = t.rel + slow * (sm_.rel - t.rel);
            sm_.level = t.level + fast * (sm_.level - t.level);
            sm_.bend = t.bend + bend * (sm_.bend - t.bend);
        }
        panCoef_ = expFast(k * 50.0f);  // 20 ms

        duty_ = 0.002f + (sm_.ramp + 1.0f) * 0.5f * 0.996f;
        width_ = VarSaw::Width(duty_);

        // shape = curve.clip(-1, 0.5).abs.lincurve(0, 1, 0, 80, 6) * curve.sign
        const float c = sm_.curve;
        const float s = absf(clampf(c, -1.0f, 0.5f));
        float shape = 80.0f * (expFast(6.0f * s) - 1.0f) * (1.0f / 402.428793f);
        if (c < 0.0f) shape = -shape;
        // UGen lincurve falls back to linlin below |curve| 0.125.
        linearShape_ = shape > -0.125f && shape < 0.125f;
        if (!linearShape_) {
            riseHalfC_ = 0.5f * shape;
            riseInv_ = 2.0f / (expFast(shape) - 1.0f);
            fallHalfC_ = -0.5f * shape;
            fallInv_ = 2.0f / (expFast(-shape) - 1.0f);
        }
        sineMix_ = clampf((c - 0.5f) * 2.0f, 0.0f, 1.0f);

        // fmIndex.abs * Select(fmIndex > 0, [voiceID / 5, 1]), and the noise floor
        // ClipNoise.range(1 - index.lincurve(0, 1, 0, 1, 8), 1) * 2, per voice.
        for (int v = 0; v < kNumVoices; ++v) {
            const float vf = absf(sm_.fm) * (sm_.fm > 0.0f ? 1.0f : (float)v * 0.2f);
            voiceFm_[v] = vf;
            noiseLo_[v] = 2.0f * (1.0f - (expFast(8.0f * vf) - 1.0f) * (1.0f / 2979.95799f));
        }
    }

    float octaveShift() const {
        const int o = params.octave < -3 ? -3 : (params.octave > 3 ? 3 : params.octave);
        return 12.0f * (float)o;
    }

    static float curveFor(uint8_t mode) {
        return mode == kModeAR ? -6.0f : (mode == kModeASR ? -4.0f : 0.0f);
    }

    // EnvGen segment from the current level to target with SuperCollider curvature.
    void startSegment(Layer& L, uint8_t stage, float target, float seconds, float curve) {
        L.stage = stage;
        L.segY0 = L.env;
        L.segDy = target - L.env;
        float n = seconds * sr_;
        if (n < 1.0f) n = 1.0f;
        if (n > 8.0e6f) n = 8.0e6f;
        L.segN = (int32_t)(n + 0.5f);
        L.segK = 0;
        L.segInvN = 1.0f / (float)L.segN;
        L.segC = curve;
        L.segLinear = curve > -0.001f && curve < 0.001f;
        if (!L.segLinear) {
            L.segInvDen = 1.0f / (1.0f - expFast(curve));
            L.segE = 1.0f;
            L.segG = expFast(curve * L.segInvN);
        }
    }

    // Re-derive exp(c * progress) once per block so the per-sample
    // multiplication in tickEnvelope() cannot drift.
    static void resyncEnvelope(Layer& L) {
        if (!L.segLinear && L.stage != kStageIdle && L.stage != kStageSustain)
            L.segE = expFast(L.segC * (float)L.segK * L.segInvN);
    }

    // tanh by table: error below 3e-5, saturating to +/-1 beyond the range.
    float tanhTable(float x) const {
        const float u = clampf((x + kTanhRange) * (kTanhTableSize / (2.0f * kTanhRange)),
                               0.0f, (float)kTanhTableSize - 0.001f);
        const int i = (int)u;
        const float f = u - (float)i;
        return tanhTable_[i] + (tanhTable_[i + 1] - tanhTable_[i]) * f;
    }

    void filterCoefficients(float amp, float& a0, float& b1, float& b2) const {
        const float u = clampf(amp, 0.0f, 1.0f) * kFilterTableSize;
        int i = (int)u;
        if (i >= kFilterTableSize) i = kFilterTableSize - 1;
        const float f = u - (float)i;
        const float* lo = filterTable_[i];
        const float* hi = filterTable_[i + 1];
        a0 = lo[0] + (hi[0] - lo[0]) * f;
        b1 = lo[1] + (hi[1] - lo[1]) * f;
        b2 = lo[2] + (hi[2] - lo[2]) * f;
    }

    void segmentDone(Layer& L) {
        switch (L.stage) {
        case kStageAttack:
            if (L.mode == kModeAR) {
                startSegment(L, kStageFall, 0.0f, sm_.rel, -6.0f);
            } else if (L.mode == kModeASR) {
                L.env = 1.0f;
                L.stage = kStageSustain;
            } else {
                startSegment(L, kStageFall, 0.0f, sm_.rel, 0.0f);
            }
            break;
        case kStageFall:
            if (L.mode == kModeCycle) startSegment(L, kStageRise, 1.0f, sm_.atk, 0.0f);
            else L.stage = kStageIdle;
            break;
        case kStageRise:
            startSegment(L, kStageFall, 0.0f, sm_.rel, 0.0f);
            break;
        default:  // release, forced
            L.env = 0.0f;
            L.stage = kStageIdle;
            break;
        }
    }

    void tickEnvelope(Layer& L) {
        if (L.stage == kStageIdle || L.stage == kStageSustain) return;
        if (++L.segK >= L.segN) {
            L.env = L.segY0 + L.segDy;
            segmentDone(L);
            return;
        }
        if (L.segLinear) {
            L.env = L.segY0 + L.segDy * ((float)L.segK * L.segInvN);
        } else {
            L.segE *= L.segG;
            L.env = L.segY0 + L.segDy * (1.0f - L.segE) * L.segInvDen;
        }
    }

    void gateOffLayer(Layer& L) {
        L.gate = false;
        if (L.mode == kModeASR) {
            if (L.stage == kStageAttack || L.stage == kStageSustain)
                startSegment(L, kStageRelease, 0.0f, sm_.rel, -4.0f);
        } else if (L.mode == kModeCycle) {
            if (L.stage == kStageAttack || L.stage == kStageFall || L.stage == kStageRise)
                startSegment(L, kStageRelease, 0.0f, sm_.rel, 0.0f);
        }
        // AR ignores the gate, as in the original.
    }

    void gateOffVoice(int v) {
        if (voices_[v].layer >= 0) gateOffLayer(layers_[voices_[v].layer]);
    }

    void forceRelease(int idx) {
        Layer& L = layers_[idx];
        if (!L.active) return;
        if (L.stage != kStageIdle && L.stage != kStageForced)
            startSegment(L, kStageForced, 0.0f, kForcedReleaseSeconds, 0.0f);
        if (voices_[L.voice].layer == idx) voices_[L.voice].layer = -1;
    }

    void deactivate(int idx) {
        Layer& L = layers_[idx];
        L.active = false;
        L.stage = kStageIdle;
        if (voices_[L.voice].layer == idx) voices_[L.voice].layer = -1;
    }

    void retireVoice(int v) {
        Voice& vc = voices_[v];
        if (vc.layer >= 0) forceRelease(vc.layer);
        vc.note = -1;
        vc.held = false;
        vc.sustained = false;
    }

    int findMidiVoice(int note) const {
        for (int v = cvCount_; v < kNumVoices; ++v)
            if (voices_[v].note == note && (voices_[v].held || voices_[v].sustained)) return v;
        return -1;
    }

    // LRU within the MIDI partition: free first, then sustained, then held.
    int pickMidiVoice() const {
        int best = cvCount_;
        int bestRank = 3;
        uint32_t bestAge = 0;
        for (int v = cvCount_; v < kNumVoices; ++v) {
            const Voice& vc = voices_[v];
            const int rank = vc.held ? 2 : (vc.sustained ? 1 : 0);
            if (rank < bestRank || (rank == bestRank && vc.age < bestAge)) {
                best = v;
                bestRank = rank;
                bestAge = vc.age;
            }
        }
        return best;
    }

    // Overlap mode: once the pool is nearly full, fade out the quietest
    // detached tail so a free layer is ready without a hard cut.
    void makeRoomForOverlap() {
        if (activeLayerCount() < kNumLayers - kOverlapHeadroom) return;
        int best = -1;
        float bestLevel = 2.0f;
        for (int i = 0; i < kNumLayers; ++i) {
            const Layer& L = layers_[i];
            if (!L.active || L.stage == kStageForced || voices_[L.voice].layer == i) continue;
            const float level = L.env * L.vel;
            if (level < bestLevel) {
                best = i;
                bestLevel = level;
            }
        }
        if (best >= 0) forceRelease(best);
    }

    // Overlap mode: detach the voice's current note and let it finish.
    void releaseForOverlap(int idx) {
        Layer& L = layers_[idx];
        if (L.gate) gateOffLayer(L);
        if (voices_[L.voice].layer == idx) voices_[L.voice].layer = -1;
    }

    int allocLayer() {
        if (params.overlap) makeRoomForOverlap();
        for (int i = 0; i < kNumLayers; ++i)
            if (!layers_[i].active) return i;
        // Pool exhausted by very rapid retriggers: reuse the quietest fading tail.
        int best = -1;
        float bestLevel = 2.0f;
        for (int pass = 0; pass < 2 && best < 0; ++pass) {
            for (int i = 0; i < kNumLayers; ++i) {
                const Layer& L = layers_[i];
                if (voices_[L.voice].layer == i) continue;
                if (pass == 0 && L.stage != kStageForced) continue;
                const float level = L.env * L.vel;
                if (level < bestLevel) {
                    best = i;
                    bestLevel = level;
                }
            }
        }
        if (best < 0) best = 0;
        deactivate(best);
        return best;
    }

    void startLayer(int v, float note, float velocity, bool bendable) {
        if (!smoothInit_) updateControl(1);
        Voice& vc = voices_[v];
        // Fade (the original's gate -1.05): the old sound fades over 50 ms while
        // the new one starts from zero. Overlap: the old sound keeps its envelope.
        if (vc.layer >= 0) {
            if (params.overlap) releaseForOverlap(vc.layer);
            else forceRelease(vc.layer);
        }
        const int idx = allocLayer();
        Layer& L = layers_[idx];
        L = Layer();
        L.active = true;
        L.gate = true;
        L.bendable = bendable;
        L.voice = (uint8_t)v;
        L.mode = (uint8_t)(params.envMode < 0 ? 0 : (params.envMode > 2 ? 2 : params.envMode));
        L.vel = clampf(velocity, 0.0f, 1.0f);
        L.note = note + octaveShift();
        L.panRand = rng_.bipolar() * 0.8f;
        L.pan = clampf(params.pan[v] + clampf(params.panDrift, 0.0f, 1.0f) * L.panRand, -1.0f, 1.0f);
        L.osc.reset(duty_);
        startSegment(L, kStageAttack, 1.0f, sm_.atk, curveFor(L.mode));
        vc.layer = (int8_t)idx;
    }

    // Per-block constants for one note, copied into registers by the loop.
    struct Block {
        float inc, modInc, fmDepth, noiseLo;
        float gainL, gainR, a0, b1, b2;
        float level, sineMix;
        float riseHalfC, riseInv, fallInv;
        VarSaw::Width width;
        bool shaped;
    };

    void renderLayer(Layer& L, float* outL, float* outR, int n) {
        // freq = (freq * bend.midiratio).clip(20, 20000), recomputed only on change
        const float semis = L.note + (L.bendable ? sm_.bend : 0.0f);
        if (semis != L.semisUsed) {
            L.semisUsed = semis;
            L.freq = clampf(440.0f * exp2Fast((semis - 69.0f) * (1.0f / 12.0f)), 20.0f, maxFreq_);
        }
        Block b;
        b.inc = L.freq * invSr_;
        b.modInc = L.freq * clampf(params.fmRatio, 0.5f, 2.0f) * invSr_;
        // SinOsc(freq * ratio, 0, index * freq * ratio * noise * 4), in phase increments
        b.fmDepth = voiceFm_[L.voice] * b.modInc * 4.0f;
        b.noiseLo = noiseLo_[L.voice];

        const float panTarget = clampf(params.pan[L.voice] + clampf(params.panDrift, 0.0f, 1.0f) * L.panRand, -1.0f, 1.0f);
        L.pan = panTarget + panCoef_ * (L.pan - panTarget);
        if (absf(L.pan - L.panUsed) > 1.0e-4f) {  // Pan2 equal power
            L.panUsed = L.pan;
            const float panTurns = (L.pan + 1.0f) * 0.125f;
            L.gainL = cosTurns(panTurns);
            L.gainR = sinTurns(panTurns);
        }
        b.gainL = L.gainL;
        b.gainR = L.gainR;

        // RLPF cutoff from the envelope at control rate, as EnvGen.kr drives it
        filterCoefficients(L.env * L.vel, b.a0, b.b1, b.b2);

        b.level = sm_.level;
        b.width = width_;
        b.sineMix = sineMix_;
        b.shaped = !linearShape_ && sineMix_ < 1.0f;  // fully sine: shaping unused
        b.riseHalfC = riseHalfC_;
        b.riseInv = riseInv_;
        b.fallInv = fallInv_;
        resyncEnvelope(L);

        const bool fm = b.fmDepth > 0.0f;
        if (outR) {
            if (fm) renderLoop<true, true>(L, outL, outR, n, b);
            else renderLoop<true, false>(L, outL, outR, n, b);
        } else {
            if (fm) renderLoop<false, true>(L, outL, nullptr, n, b);
            else renderLoop<false, false>(L, outL, nullptr, n, b);
        }
    }

    // Block-wise rendering in three passes, keeping per-sample float compares
    // out of the hot loops (measured on the module: they, not the arithmetic,
    // dominated the cost). A) serial recurrences: envelope, FM, VarSaw.
    // B) the waveshaper, independent per sample. C) the filter recurrence with
    // the output stage folded in.
    template <bool kStereo, bool kFm>
    void renderLoop(Layer& L, float* __restrict outL, float* __restrict outR, int n, const Block b) {
        float amp[kMaxChunk], tri[kMaxChunk], snd[kMaxChunk];
        const float prevTri = L.prevTri;

        // A) envelope (EnvGen segments, as tickEnvelope() on locals), FM, VarSaw
        {
            uint8_t stage = L.stage;
            float env = L.env;
            const float vel = L.vel;
            int32_t segK = L.segK, segN = L.segN;
            float segY0 = L.segY0, segDy = L.segDy, segInvN = L.segInvN;
            float segE = L.segE, segG = L.segG, segInvDen = L.segInvDen;
            bool segLinear = L.segLinear;
            VarSaw osc = L.osc;
            float modPhase = L.modPhase;
            uint32_t rs = rng_.s;
            float last = prevTri;
            for (int i = 0; i < n; ++i) {
                if (stage != kStageIdle && stage != kStageSustain) {
                    if (++segK >= segN) {
                        L.stage = stage;
                        L.env = segY0 + segDy;
                        segmentDone(L);
                        stage = L.stage;
                        env = L.env;
                        segK = L.segK;
                        segN = L.segN;
                        segY0 = L.segY0;
                        segDy = L.segDy;
                        segInvN = L.segInvN;
                        segE = L.segE;
                        segG = L.segG;
                        segInvDen = L.segInvDen;
                        segLinear = L.segLinear;
                    } else if (segLinear) {
                        env = segY0 + segDy * ((float)segK * segInvN);
                    } else {
                        segE *= segG;
                        env = segY0 + segDy * (1.0f - segE) * segInvDen;
                    }
                }
                amp[i] = env * vel;

                float inc = b.inc;
                if (kFm) {
                    rs ^= rs << 13;
                    rs ^= rs >> 17;
                    rs ^= rs << 5;
                    const float noise = (rs & 0x80000000u) ? 2.0f : b.noiseLo;
                    inc += sinTurns(modPhase) * b.fmDepth * noise;
                }
                modPhase += b.modInc;
                modPhase -= __builtin_floorf(modPhase);
                last = osc.next(inc, b.width);
                tri[i] = last;
            }
            L.prevTri = last;
            L.stage = stage;
            L.env = env;
            L.segK = segK;
            L.segE = segE;
            L.osc = osc;
            L.modPhase = modPhase;
            if (kFm) rng_.s = rs;
        }

        // B) lincurve clips its input; the rising half (Slope > 0) bends with
        // +shape, the falling half with -shape. sign = +1 rising, -1 falling
        // (a tie counts as falling, like Slope > 0); fallHalfC is -riseHalfC.
        // The sine branch, (tri * 1.86).tanh, sees the unclipped tri.
        {
            if (b.shaped) {
                const float invMid = 0.5f * (b.riseInv + b.fallInv);
                const float invHalfDiff = 0.5f * (b.riseInv - b.fallInv);
                COPAINS_UNROLL
                for (int i = 0; i < n; ++i) {
                    const float t = tri[i];
                    const float sign = __builtin_copysignf(1.0f, t - (i ? tri[i - 1] : prevTri) - 1.0e-30f);
                    const float c = clampf(t, -1.0f, 1.0f) + 1.0f;
                    snd[i] = -1.0f + (expFast(sign * b.riseHalfC * c) - 1.0f) * (invMid + sign * invHalfDiff);
                }
            } else {
                COPAINS_UNROLL
                for (int i = 0; i < n; ++i) snd[i] = clampf(tri[i], -1.0f, 1.0f);
            }
            if (b.sineMix > 0.0f) {
                COPAINS_UNROLL
                for (int i = 0; i < n; ++i) snd[i] += (tanhTable(tri[i] * 1.86f) - snd[i]) * b.sineMix;
            }
        }

        // C) RLPF, then (snd * level * env).tanh * -9 dB, panned into the outputs.
        {
            const float gL = (kStereo ? b.gainL : kMonoGain) * kOutputGain;
            const float gR = b.gainR * kOutputGain;
            float y1 = L.y1, y2 = L.y2;
            for (int i = 0; i < n; ++i) {
                const float y0 = b.a0 * snd[i] + b.b1 * y1 + b.b2 * y2;
                const float out = tanhTable((y0 + 2.0f * y1 + y2) * b.level * amp[i]);
                y2 = y1;
                y1 = y0;
                outL[i] += out * gL;
                if (kStereo) outR[i] += out * gR;
            }
            L.y1 = y1;
            L.y2 = y2;
        }
    }

    float sr_ = 48000.0f, invSr_ = 1.0f / 48000.0f, maxFreq_ = 20000.0f;
    Layer layers_[kNumLayers];
    Voice voices_[kNumVoices];
    Rng rng_;
    Control sm_;
    float panCoef_ = 0.0f;
    float duty_ = 0.5f;
    VarSaw::Width width_;
    bool linearShape_ = true;
    float riseHalfC_ = 0.0f, riseInv_ = 0.0f, fallHalfC_ = 0.0f, fallInv_ = 0.0f;
    float sineMix_ = 0.0f;
    float voiceFm_[kNumVoices] = {};
    float noiseLo_[kNumVoices] = {};
    float tanhTable_[kTanhTableSize + 1];
    float filterTable_[kFilterTableSize + 1][3];
    int cvCount_ = 0;
    bool cvGated_ = false;
    bool sustainDown_ = false;
    bool smoothInit_ = false;
    float bendNorm_ = 0.0f;
    uint32_t ageCounter_ = 0;
};

}  // namespace copains

#endif  // COPAINS_ENGINE_H
