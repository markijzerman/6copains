// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native behaviour tests for 6Copains. Build and run with `make test`.

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <chrono>
#include <vector>

#include "fake_host.h"
#include "copains_engine.h"
#include "copains_plugin.h"

using namespace copains;

static int gChecks = 0, gFailures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++gChecks;                                                               \
        if (!(cond)) {                                                           \
            ++gFailures;                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                    \
    do {                                                                         \
        ++gChecks;                                                               \
        const double va_ = (a), vb_ = (b);                                       \
        if (!(fabs(va_ - vb_) <= (tol))) {                                       \
            ++gFailures;                                                         \
            printf("  FAIL %s:%d: %s = %.7g, expected %.7g +/- %.3g\n", __FILE__, \
                   __LINE__, #a, va_, vb_, (double)(tol));                       \
        }                                                                        \
    } while (0)

// ------------------------------------------------------------- helpers

struct Audio {
    std::vector<float> l, r;
};

static void run(Engine& e, int frames, Audio* out = nullptr, bool mono = false) {
    float L[kMaxChunk], R[kMaxChunk];
    for (int i = 0; i < frames; i += kMaxChunk) {
        const int n = frames - i < kMaxChunk ? frames - i : kMaxChunk;
        e.render(L, mono ? nullptr : R, n);
        if (out) {
            out->l.insert(out->l.end(), L, L + n);
            if (!mono) out->r.insert(out->r.end(), R, R + n);
        }
    }
}

static int seconds(const Engine& e, double s) { return (int)(s * e.sampleRate()); }

// Plain triangle through a fully open gate: easy to measure.
static void cleanPatch(Engine& e) {
    Params& p = e.params;
    p.envMode = kModeASR;
    p.curve = 0.0f;
    p.ramp = 0.0f;
    p.rampMod = 0.0f;
    p.fmIndex = 0.0f;
    p.fmMod = 0.0f;
    p.morph = 0.0f;
    p.panDrift = 0.0f;
    p.slope = 0.1f;
    p.time = 0.05f;
    for (float& pan : p.pan) pan = 0.0f;
}

static double frequency(const std::vector<float>& x, double sr, size_t from) {
    double first = -1.0, last = -1.0;
    int cycles = 0;
    for (size_t i = from + 1; i < x.size(); ++i) {
        if (x[i - 1] <= 0.0f && x[i] > 0.0f) {
            const double t = (double)(i - 1) + -x[i - 1] / (double)(x[i] - x[i - 1]);
            if (first < 0.0) first = t;
            else {
                last = t;
                ++cycles;
            }
        }
    }
    return cycles > 0 ? cycles * sr / (last - first) : 0.0;
}

static float maxStep(const std::vector<float>& x, size_t from, size_t to) {
    float m = 0.0f;
    for (size_t i = from + 1; i < to && i < x.size(); ++i) m = fmaxf(m, fabsf(x[i] - x[i - 1]));
    return m;
}

static bool allFinite(const std::vector<float>& x, float limit) {
    for (float v : x)
        if (!(fabsf(v) <= limit)) return false;
    return true;
}

static float peak(const std::vector<float>& x, size_t from = 0) {
    float m = 0.0f;
    for (size_t i = from; i < x.size(); ++i) m = fmaxf(m, fabsf(x[i]));
    return m;
}

static int firstSilentAfter(const std::vector<float>& x) {
    for (size_t i = x.size(); i-- > 0;)
        if (x[i] != 0.0f) return (int)i + 1;
    return 0;
}

// --------------------------------------------------------- engine tests

static void testFastMath() {
    double e2 = 0, th = 0, sn = 0;
    for (float x = -20.0f; x <= 20.0f; x += 0.01f)
        e2 = fmax(e2, fabs(exp2Fast(x) / exp2(x) - 1.0));
    for (float x = -10.0f; x <= 10.0f; x += 0.001f) th = fmax(th, fabs(tanhFast(x) - tanh(x)));
    for (float t = -3.0f; t <= 3.0f; t += 0.0001f) sn = fmax(sn, fabs(sinTurns(t) - sin(2.0 * M_PI * t)));
    CHECK(e2 < 1e-6);
    CHECK(th < 2e-6);
    CHECK(sn < 1e-5);
    CHECK_NEAR(expFast(80.0f) / exp(80.0), 1.0, 1e-5);
}

static void testEnvelopeShapeAndSineTable() {
    // The multiplicative envelope must track the closed-form EnvGen curve.
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    e.params.envMode = kModeAR;
    e.params.time = 4.0f;
    e.params.slope = 0.5f;  // ~2 s attack, curve -6
    e.noteOn(60, 1.0f);
    const double atk = e.attackSeconds();
    double worst = 0.0;
    for (int b = 0; b < (int)(1.9 * 48000) / kMaxChunk; ++b) {
        run(e, kMaxChunk);
        const double t = (b + 1) * kMaxChunk / 48000.0;
        const double expect = (1.0 - exp(-6.0 * t / atk)) / (1.0 - exp(-6.0));
        worst = fmax(worst, fabs(e.layer(0).env - expect));
    }
    CHECK(worst < 1e-4);

    double table = 0.0;
    for (float x = -1.0f; x <= 1.0f; x += 0.0007f) table = fmax(table, fabs(e.sineShapeAt(x) - tanh(1.86 * x)));
    CHECK(table < 5e-5);
    CHECK_NEAR(e.sineShapeAt(-1.0f), -tanh(1.86), 5e-5);
    CHECK_NEAR(e.sineShapeAt(-2.5f), -tanh(4.65), 5e-5);  // VarSaw can leave [-1, 1] under FM
    double worstTanh = 0.0;
    for (float x = -12.0f; x <= 12.0f; x += 0.0013f) worstTanh = fmax(worstTanh, fabs(e.tanhAt(x) - tanh(x)));
    CHECK(worstTanh < 3e-5);
}

// Literal port of SuperCollider's VarSaw_next (double phase), for comparison.
struct ScVarSaw {
    double phase;
    float duty, invduty, inv1duty;
    explicit ScVarSaw(float width) {
        duty = width < 0.001f ? 0.001f : (width > 0.999f ? 0.999f : width);
        invduty = 2.f / duty;
        inv1duty = 2.f / (1.f - duty);
        phase = duty * 0.5;
    }
    float next(float freqTimesMul, float nextDuty) {
        if (phase >= 1.f) {
            phase -= 1.f;
            duty = nextDuty < 0.001f ? 0.001f : (nextDuty > 0.999f ? 0.999f : nextDuty);
            invduty = 2.f / duty;
            inv1duty = 2.f / (1.f - duty);
        }
        float z = phase < duty ? (float)phase * invduty : (1.f - (float)phase) * inv1duty;
        phase += freqTimesMul;
        return z - 1.f;
    }
};

static void testVarSawMatchesSuperCollider() {
    // Heavy noisy FM, as at FM index 1 with ratio 2 on a 440 Hz note.
    Rng rng;
    VarSaw ours;
    ours.reset(0.3f);
    ScVarSaw ref(0.3f);
    double worst = 0.0;
    float lowest = 0.0f, highest = 0.0f;
    float modPhase = 0.0f;
    const float inc = 440.0f / 48000.0f, modInc = 880.0f / 48000.0f;
    for (int i = 0; i < 48000; ++i) {
        const float noise = (rng.next() & 0x80000000u) ? 2.0f : 0.0f;
        const float dInc = inc + sinTurns(modPhase) * modInc * 4.0f * noise;
        modPhase += modInc;
        if (modPhase >= 1.0f) modPhase -= 1.0f;
        const float width = 0.3f + 0.4f * (float)((i / 4800) % 2);  // width changes mid-run
        const float a = ours.next(dInc, VarSaw::Width(width)), b = ref.next(dInc, width);
        worst = fmax(worst, fabs(a - b));
        lowest = fminf(lowest, b);
        highest = fmaxf(highest, b);
    }
    CHECK(worst < 2e-3);     // float vs double phase only
    CHECK(lowest < -1.5f);   // the gritty excursions really happen
    CHECK(highest <= 1.0f);
}

static void testMidiPitch() {
    const float rates[] = { 44100.0f, 48000.0f, 96000.0f };
    for (float sr : rates) {
        Engine e;
        e.init(sr);
        cleanPatch(e);
        e.noteOn(69, 1.0f);
        Audio a;
        run(e, seconds(e, 0.5), &a);
        CHECK_NEAR(frequency(a.l, sr, (size_t)seconds(e, 0.1)), 440.0, 0.5);
        CHECK(allFinite(a.l, 2.2f));
    }
}

static void testVoltsPerOctave() {
    CHECK_NEAR(voltsToNote(0.0f), 48.0, 1e-6);
    CHECK_NEAR(voltsToNote(1.0f), 60.0, 1e-6);
    CHECK_NEAR(voltsToNote(-1.0f), 36.0, 1e-6);
    CHECK_NEAR(voltsToNote(1000.0f), 168.0, 1e-6);
    CHECK_NEAR(voltsToNote(NAN), 48.0, 1e-6);

    const float volts[] = { -1.0f, 0.0f, 1.0f, 0.75f };
    const double hz[] = { 65.4064, 130.8128, 261.6256, 220.0 };  // 0.75 V = A3 from the NT Step Sequencer
    for (int i = 0; i < 4; ++i) {
        Engine e;
        e.init(48000.0f);
        cleanPatch(e);
        e.setCvVoiceCount(1);
        const float note = voltsToNote(volts[i]);
        e.cvGateOn(&note, 1.0f);
        Audio a;
        run(e, seconds(e, 0.6), &a);
        CHECK_NEAR(frequency(a.l, 48000.0, (size_t)seconds(e, 0.1)), hz[i], hz[i] * 0.001);
    }
}

static void testArMode() {
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    e.params.envMode = kModeAR;
    e.params.time = 0.1f;
    e.params.slope = 0.5f;
    e.noteOn(60, 1.0f);
    CHECK_NEAR(e.attackSeconds(), 0.0505, 1e-4);
    CHECK_NEAR(e.releaseSeconds(), 0.0505, 1e-4);
    run(e, seconds(e, 0.05));
    CHECK(e.activeLayerCount() == 1);
    run(e, seconds(e, 0.06));
    CHECK(e.activeLayerCount() == 0);  // AR finishes with the key still held
    CHECK(e.voice(0).held);
    e.noteOff(60);
    CHECK(!e.voice(0).held);
}

static void testAsrMode() {
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    e.params.time = 0.2f;
    e.params.slope = 0.25f;  // attack 0.05 s, release 0.15 s
    e.noteOn(60, 1.0f);
    run(e, seconds(e, 1.0));
    CHECK(e.layer(0).stage == kStageSustain);
    CHECK_NEAR(e.layer(0).env, 1.0, 1e-6);
    e.noteOff(60);
    CHECK(e.layer(0).stage == kStageRelease);
    run(e, seconds(e, 0.14));
    CHECK(e.activeLayerCount() == 1);
    run(e, seconds(e, 0.02));
    CHECK(e.activeLayerCount() == 0);
}

static void testCycleMode() {
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    e.params.envMode = kModeCycle;
    e.params.time = 0.02f;
    e.params.slope = 0.5f;
    e.noteOn(60, 1.0f);
    int peaks = 0;
    float prev = 0.0f;
    bool rising = true;
    for (int i = 0; i < seconds(e, 0.2) / kMaxChunk; ++i) {
        run(e, kMaxChunk);
        const float env = e.layer(0).env;
        if (rising && env < prev) {
            ++peaks;
            rising = false;
        } else if (!rising && env > prev) {
            rising = true;
        }
        prev = env;
    }
    CHECK(peaks >= 9);  // ~20 ms cycle for 200 ms
    CHECK(e.activeLayerCount() == 1);
    e.noteOff(60);
    run(e, seconds(e, 0.012));
    CHECK(e.activeLayerCount() == 0);
}

static void testLruAllocation() {
    Engine e;
    e.init(48000.0f);
    for (int n = 0; n < 6; ++n) e.noteOn(60 + n, 1.0f);
    for (int v = 0; v < 6; ++v) CHECK(e.voice(v).note == 60 + v);
    e.noteOff(62);
    e.noteOn(70, 1.0f);
    CHECK(e.voice(2).note == 70);  // free voice beats held voices
    e.noteOn(71, 1.0f);
    CHECK(e.voice(0).note == 71);  // then the least recently used held voice
}

static void testSameNoteReusesVoice() {
    Engine e;
    e.init(48000.0f);
    e.noteOn(60, 1.0f);
    run(e, 480);
    e.noteOn(60, 0.5f);
    CHECK(e.voice(0).note == 60);
    CHECK(e.voice(1).note == -1);
    CHECK(e.activeLayerCount() == 2);  // old layer fading, new one starting
    run(e, seconds(e, 0.06));
    CHECK(e.activeLayerCount() == 1);
}

static void testStolenNoteOffIsIgnored() {
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    for (int n = 0; n < 6; ++n) e.noteOn(60 + n, 1.0f);
    e.noteOn(66, 1.0f);  // steals voice 0 from note 60
    CHECK(e.voice(0).note == 66);
    e.noteOff(60);       // the original's mod.lua would release note 66 here
    CHECK(e.voice(0).held);
    CHECK(e.voice(0).layer >= 0 && e.layer(e.voice(0).layer).gate);
}

static void testSustainPedal() {
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    e.noteOn(60, 1.0f);
    run(e, seconds(e, 0.1));
    e.setSustain(true);
    e.noteOff(60);
    CHECK(e.layer(e.voice(0).layer).stage == kStageSustain);
    CHECK(e.voice(0).sustained);
    e.noteOn(60, 1.0f);  // re-press while sustained reuses the voice
    CHECK(e.voice(0).held && !e.voice(0).sustained);
    e.noteOff(60);
    e.setSustain(false);
    CHECK(!e.voice(0).sustained);
    CHECK(e.layer(e.voice(0).layer).stage == kStageRelease);
}

static void testPanic() {
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    e.setCvVoiceCount(2);
    const float notes[2] = { 60.0f, 64.0f };
    e.cvGateOn(notes, 1.0f);
    for (int n = 0; n < 4; ++n) e.noteOn(70 + n, 1.0f);
    e.setSustain(true);
    e.setPitchBend(0.5f);
    run(e, seconds(e, 0.1));
    e.panic();
    for (int i = 0; i < kNumLayers; ++i)
        if (e.layer(i).active) CHECK(e.layer(i).stage == kStageForced);
    for (int v = 0; v < kNumVoices; ++v) CHECK(e.voice(v).layer == -1 && !e.voice(v).held && e.voice(v).note == -1);
    CHECK(!e.sustainDown() && !e.cvGated());
    Audio a;
    run(e, seconds(e, 0.06), &a);
    CHECK(e.activeLayerCount() == 0);
    CHECK(firstSilentAfter(a.l) <= seconds(e, 0.0505));
}

static void testCvPartition() {
    Engine e;
    e.init(48000.0f);
    e.setCvVoiceCount(4);
    const float notes[4] = { 60.0f, 62.0f, 64.0f, 65.0f };
    e.cvGateOn(notes, 1.0f);
    e.noteOn(70, 1.0f);
    e.noteOn(71, 1.0f);
    e.noteOn(72, 1.0f);  // steals a MIDI voice, never a CV one
    CHECK(e.voice(4).note == 72);
    CHECK(e.voice(5).note == 71);
    for (int v = 0; v < 4; ++v) CHECK(e.voice(v).owner == kOwnerCv && e.voice(v).held && e.voice(v).note == -1);

    e.setCvVoiceCount(6);  // MIDI loses its partition: its voices fade out
    CHECK(e.voice(4).layer == -1 && e.voice(5).layer == -1);
    e.noteOn(80, 1.0f);    // no MIDI voices left
    for (int v = 0; v < kNumVoices; ++v) CHECK(e.voice(v).note == -1);
}

static void testCvCountGrowthNeedsEdge() {
    Engine e;
    e.init(48000.0f);
    e.setCvVoiceCount(1);
    const float notes[2] = { 60.0f, 67.0f };
    e.cvGateOn(notes, 1.0f);
    e.setCvVoiceCount(2);  // gate still high
    CHECK(e.voice(1).layer == -1);
    e.cvGateOff();
    e.cvGateOn(notes, 1.0f);
    CHECK(e.voice(1).layer >= 0);
}

static void testTransitionsAreClickFree() {
    // Retrigger, steal and partition shrink must not jump: compare the largest
    // sample-to-sample step against steady state. A hard cut at this level
    // would be ~35x the steady-state step.
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    e.params.curve = 1.0f;   // sine-like, smallest steady-state slope
    e.params.slope = 0.01f;  // 20 ms attack, so only the transition is measured
    e.params.time = 2.0f;
    for (float& pan : e.params.pan) pan = 0.0f;
    Audio a;
    e.noteOn(45, 1.0f);
    run(e, seconds(e, 0.3), &a);
    const float steady = maxStep(a.l, (size_t)seconds(e, 0.1), a.l.size());
    CHECK(steady > 0.001f);

    const size_t retrigger = a.l.size();
    e.noteOn(45, 1.0f);
    run(e, seconds(e, 0.2), &a);
    const float retrig = maxStep(a.l, retrigger - 1, a.l.size());
    printf("  info: retrigger step %.2fx steady\n", (double)(retrig / steady));
    CHECK(retrig < steady * 2.2f);

    for (int n = 0; n < 5; ++n) e.noteOn(46 + n, 1.0f);
    run(e, seconds(e, 0.3), &a);
    const float six = maxStep(a.l, a.l.size() - (size_t)seconds(e, 0.1), a.l.size());
    const size_t steal = a.l.size();
    e.noteOn(57, 1.0f);
    run(e, seconds(e, 0.2), &a);
    const float stolen = maxStep(a.l, steal - 1, a.l.size());
    printf("  info: steal step %.2fx steady\n", (double)(stolen / six));
    CHECK(stolen < six * 2.2f);

    Engine c;
    c.init(48000.0f);
    cleanPatch(c);
    c.params.curve = 1.0f;
    c.params.slope = 0.01f;
    c.params.time = 2.0f;
    c.setCvVoiceCount(3);
    const float notes[3] = { 45.0f, 52.0f, 57.0f };
    c.cvGateOn(notes, 1.0f);
    Audio b;
    run(c, seconds(c, 0.3), &b);
    const float cvSteady = maxStep(b.l, (size_t)seconds(c, 0.1), b.l.size());
    const size_t shrink = b.l.size();
    c.setCvVoiceCount(0);
    run(c, seconds(c, 0.1), &b);
    CHECK(maxStep(b.l, shrink - 1, b.l.size()) <= cvSteady * 1.2f);
    CHECK(peak(b.l, shrink + (size_t)seconds(c, 0.051)) == 0.0f);
}

static void testOverlapRetrigger() {
    for (int overlap = 0; overlap <= 1; ++overlap) {
        Engine e;
        e.init(48000.0f);
        cleanPatch(e);
        e.params.envMode = kModeAR;
        e.params.time = 2.0f;
        e.params.slope = 0.0f;  // instant attack, ~2 s release
        e.params.overlap = overlap != 0;
        e.noteOn(60, 1.0f);
        run(e, seconds(e, 0.2));
        e.noteOn(60, 1.0f);  // key still held: the same voice retriggers
        CHECK(e.activeLayerCount() == 2);
        run(e, seconds(e, 0.3));
        // Fade: the old note is gone after 50 ms. Overlap: it is still decaying.
        CHECK(e.activeLayerCount() == (overlap ? 2 : 1));
        if (overlap) {
            int falling = 0;
            for (int i = 0; i < kNumLayers; ++i)
                if (e.layer(i).active && e.layer(i).stage == kStageFall && e.layer(i).env > 0.1f) ++falling;
            CHECK(falling == 2);
        }
    }

    // ASR in Overlap: a retriggered held note is released, not cut or left hanging.
    Engine a;
    a.init(48000.0f);
    cleanPatch(a);
    a.params.overlap = true;
    a.params.time = 1.0f;
    a.noteOn(60, 1.0f);
    run(a, seconds(a, 0.2));
    const int first = a.voice(0).layer;
    a.noteOn(60, 1.0f);
    CHECK(a.layer(first).active && a.layer(first).stage == kStageRelease);
    CHECK(a.voice(0).layer != first);
    a.panic();
    run(a, seconds(a, 0.06));
    CHECK(a.activeLayerCount() == 0);
}

static void testOverlapPoolBounded() {
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    e.params.overlap = true;
    e.params.envMode = kModeAR;
    e.params.time = 4.0f;
    e.params.slope = 0.0f;
    e.params.curve = 1.0f;
    Audio a;
    int most = 0;
    for (int i = 0; i < 300; ++i) {
        e.noteOn(48 + (i * 7) % 24, 0.8f);
        run(e, seconds(e, 0.01), &a);
        most = e.activeLayerCount() > most ? e.activeLayerCount() : most;
        CHECK(e.activeLayerCount() <= kNumLayers);
    }
    CHECK(most >= kNumLayers - 1);   // genuinely overlapping
    CHECK(allFinite(a.l, 6.0f) && allFinite(a.r, 6.0f));
    CHECK(maxStep(a.l, 0, a.l.size()) < 0.1f);  // tails fade, never cut hard
}

static void testOctave() {
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    e.params.octave = -1;
    e.noteOn(69, 1.0f);
    CHECK_NEAR(e.layer(e.voice(0).layer).note, 57.0, 1e-6);
    Audio a;
    run(e, seconds(e, 0.5), &a);
    CHECK_NEAR(frequency(a.l, 48000.0, (size_t)seconds(e, 0.1)), 220.0, 0.3);
    e.noteOff(69);  // note-off still matches the incoming note number
    CHECK(!e.voice(0).held);
    e.params.octave = 9;  // clamped to +3
    e.setCvVoiceCount(1);
    const float note = 48.0f;
    e.cvGateOn(&note, 1.0f);
    CHECK_NEAR(e.layer(e.voice(0).layer).note, 84.0, 1e-6);
    e.cvSetNote(0, 50.0f);
    CHECK_NEAR(e.layer(e.voice(0).layer).note, 86.0, 1e-6);
}

static void testLayerPoolExhaustion() {
    Engine e;
    e.init(48000.0f);
    e.params.slope = 0.0f;
    for (int i = 0; i < 200; ++i) {
        e.noteOn(60 + (i % 3), 1.0f);
        run(e, 2);
        CHECK(e.activeLayerCount() <= kNumLayers);
    }
    Audio a;
    run(e, seconds(e, 0.1), &a);
    CHECK(allFinite(a.l, 2.2f) && allFinite(a.r, 2.2f));
}

static void testExtremeSettings() {
    Engine e;
    e.init(48000.0f);
    Params& p = e.params;
    p.fmIndex = 1.0f;
    p.fmMod = 1.0f;
    p.fmRatio = 2.0f;
    p.morph = 1.0f;
    p.level = 1.0f;
    p.envMode = kModeCycle;
    p.time = 0.001f;
    p.timeMod = -1.0f;
    const float curves[] = { -1.0f, 0.0f, 0.5f, 1.0f };
    const float ramps[] = { -1.0f, 1.0f };
    for (float curve : curves) {
        for (float ramp : ramps) {
            p.curve = curve;
            p.ramp = ramp;
            p.curveMod = -curve;
            p.rampMod = -ramp;
            e.setCvVoiceCount(2);
            const float notes[2] = { voltsToNote(100.0f), voltsToNote(-100.0f) };
            e.cvGateOn(notes, 1.0f);
            e.noteOn(0, 1.0f);
            e.noteOn(127, 1.0f);
            e.noteOn(64, 1.0f);
            e.noteOn(127, 1.0f);
            e.setPitchBend(1.0f);
            Audio a;
            run(e, seconds(e, 0.25), &a);
            CHECK(allFinite(a.l, 2.2f) && allFinite(a.r, 2.2f));
            e.panic();
        }
    }
}

static void testPitchBend() {
    Engine e;
    e.init(48000.0f);
    cleanPatch(e);
    e.params.bendRange = 7.0f;
    e.setCvVoiceCount(1);
    for (float& pan : e.params.pan) pan = 0.0f;
    e.params.pan[1] = 1.0f;  // MIDI voice hard right
    e.params.pan[0] = -1.0f; // CV voice hard left
    const float note = 69.0f;
    e.cvGateOn(&note, 1.0f);
    e.noteOn(69, 1.0f);
    e.setPitchBend(1.0f);
    Audio a;
    run(e, seconds(e, 0.6), &a);
    CHECK_NEAR(frequency(a.r, 48000.0, (size_t)seconds(e, 0.1)), 440.0 * pow(2.0, 7.0 / 12.0), 0.7);
    CHECK_NEAR(frequency(a.l, 48000.0, (size_t)seconds(e, 0.1)), 440.0, 0.5);
}

static void testMorphTargets() {
    Engine e;
    e.init(48000.0f);
    e.params.time = 0.5f;
    e.params.timeMod = 0.5f;
    e.params.slope = 0.5f;
    e.params.morph = 1.0f;
    run(e, seconds(e, 0.5));  // let the 100 ms lag settle
    CHECK_NEAR(e.attackSeconds(), 0.5005, 1e-3);
    CHECK_NEAR(e.releaseSeconds(), 0.5005, 1e-3);
}

static void testMonoMatchesCentre() {
    Engine s, m;
    s.init(48000.0f);
    m.init(48000.0f);
    cleanPatch(s);
    cleanPatch(m);
    s.noteOn(57, 0.8f);
    m.noteOn(57, 0.8f);
    Audio as, am;
    run(s, 4800, &as);
    run(m, 4800, &am, true);
    float diff = 0.0f;
    for (size_t i = 0; i < as.l.size(); ++i) diff = fmaxf(diff, fabsf(as.l[i] - am.l[i]));
    CHECK(diff < 1e-6f);
}

static void testCpuEstimate() {
    Engine e;
    e.init(48000.0f);
    e.params.fmIndex = 0.5f;
    e.params.curve = 0.3f;
    for (int n = 0; n < 6; ++n) e.noteOn(48 + n * 4, 1.0f);
    const auto t0 = std::chrono::steady_clock::now();
    run(e, 48000 * 4);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 4.0;
    printf("  info: 6 voices, 1 s of audio rendered in %.0f us on this host (unoptimised ASan build)\n", us);
}

// --------------------------------------------------------- plugin tests

static void testFactoryAndParameters() {
    const _NT_factory* f = (const _NT_factory*)pluginEntry(kNT_selector_factoryInfo, 0);
    CHECK(pluginEntry(kNT_selector_version, 0) == kNT_apiVersion14);
    CHECK(pluginEntry(kNT_selector_numFactories, 0) == 1);
    CHECK(pluginEntry(kNT_selector_factoryInfo, 1) == 0);
    CHECK(f->guid == NT_MULTICHAR('M', 'j', 'M', 'a'));
    CHECK(strcmp(f->name, "6Copains") == 0);
    CHECK(f->tags == kNT_tagInstrument);

    FakeHost h;
    h.create();
    CHECK(h.req.numParameters == kNumParams);
    std::vector<int> seen(kNumParams, 0);
    for (uint32_t pg = 0; pg < h.alg->parameterPages->numPages; ++pg) {
        const _NT_parameterPage& page = h.alg->parameterPages->pages[pg];
        for (int i = 0; i < page.numParams; ++i) {
            CHECK(page.params[i] < kNumParams);
            if (page.params[i] < kNumParams) ++seen[page.params[i]];
        }
    }
    for (int p = 0; p < kNumParams; ++p) {
        const _NT_parameter& d = copains_parameterTemplate()[p];
        CHECK(seen[p] == 1);
        CHECK(d.min <= d.def && d.def <= d.max);
        if (d.unit == kNT_unitEnum) {
            for (int i = 0; i <= d.max; ++i) CHECK(d.enumStrings[i] != nullptr && d.enumStrings[i][0] != 0);
        }
    }
    CHECK(strcmp(copains_parameterTemplate()[kParamEnvMode].enumStrings[2], "Cycle") == 0);
}

static void testDefaultsMatchDefaultPatch() {
    FakeHost h;
    h.create();
    h.step();
    const Params& p = copains_engineOf(h.alg)->params;
    CHECK_NEAR(p.level, 0.8, 1e-6);
    CHECK_NEAR(p.ramp, 0.08, 1e-6);
    CHECK_NEAR(p.curve, 1.0, 1e-6);
    CHECK_NEAR(p.fmRatio, 1.01, 1e-6);
    CHECK_NEAR(p.fmIndex, 0.0, 1e-6);
    CHECK_NEAR(p.panDrift, 0.12, 1e-6);
    CHECK(p.envMode == kModeAR);
    CHECK_NEAR(p.slope, 0.03, 1e-6);
    CHECK_NEAR(p.time, 0.21077, 0.21077 * 0.005);
    CHECK_NEAR(p.rampMod, 0.61, 1e-6);
    CHECK_NEAR(p.fmMod, -0.51, 1e-6);
    CHECK_NEAR(p.bendRange, 7.0, 1e-6);
    CHECK(h.get(kParamOutputGain) == 6);
    CHECK_NEAR(copains_timeFromIndex(0), 0.0026, 1e-6);
    CHECK_NEAR(copains_timeFromIndex(1000), 8.0, 1e-4);
}

static void testSilenceAndOutputModes() {
    FakeHost h;
    h.create();
    h.fill(13, 1.0f);
    h.fill(14, 1.0f);
    h.step();  // Add mode, no notes: buses unchanged
    CHECK(peak(std::vector<float>(h.busPtr(13), h.busPtr(13) + FakeHost::kFrames)) == 1.0f);
    h.set(kParamOutputLMode, 1);
    h.set(kParamOutputRMode, 1);
    h.step();  // Replace mode, no notes: exact silence
    for (int i = 0; i < FakeHost::kFrames; ++i) CHECK(h.busPtr(13)[i] == 0.0f && h.busPtr(14)[i] == 0.0f);
}

static float stepPeak(FakeHost& h, int bus, int steps) {
    float m = 0.0f;
    for (int s = 0; s < steps; ++s) {
        h.clearOutputs();
        h.step();
        for (int i = 0; i < FakeHost::kFrames; ++i) m = fmaxf(m, fabsf(h.busPtr(bus)[i]));
    }
    return m;
}

static void testMidiThroughPlugin() {
    FakeHost h;
    h.create();
    h.midi(0x91, 60, 100);  // channel 2: ignored on channel 1
    CHECK(stepPeak(h, 13, 50) == 0.0f);
    h.midi(0x90, 60, 100);
    const float l = stepPeak(h, 13, 150);
    CHECK(l > 0.2f && l < 11.0f);
    CHECK(stepPeak(h, 14, 1) >= 0.0f);
    h.midi(0x80, 60, 0);

    h.set(kParamMidiChannel, 0);  // Omni
    h.midi(0x9F, 64, 127);
    CHECK(copains_engineOf(h.alg)->voice(1).note == 64);
    h.midi(0xBF, 123, 0);
    CHECK(copains_engineOf(h.alg)->voice(1).note == -1);
}

static void testMonoOutput() {
    FakeHost h;
    h.create();
    h.set(kParamOutputR, 0);
    h.midi(0x90, 60, 127);
    float l = 0.0f;
    for (int s = 0; s < 100; ++s) {
        h.fill(13, 0.0f);
        h.fill(14, 3.0f);
        h.step();
        l = fmaxf(l, peak(std::vector<float>(h.busPtr(13), h.busPtr(13) + FakeHost::kFrames)));
        for (int i = 0; i < FakeHost::kFrames; ++i) CHECK(h.busPtr(14)[i] == 3.0f);
    }
    CHECK(l > 0.2f);
}

static void testCvRoutingDerivation() {
    FakeHost h;
    h.create();
    h.step();
    for (int i = 0; i < kNumVoices; ++i) {
        CHECK(h.get(kParamPitchInput1 + i) == 0);
        CHECK(h.grayed[FakeHost::kCommonParams + kParamPitchInput1 + i]);
    }
    CHECK(h.grayed[FakeHost::kCommonParams + kParamCvCount]);  // gate None

    h.set(kParamGateInput, 5);
    h.set(kParamCvCount, 3);
    h.step();
    CHECK(h.get(kParamPitchInput1) == 6 && h.get(kParamPitchInput1 + 1) == 7 && h.get(kParamPitchInput1 + 2) == 8);
    CHECK(h.get(kParamPitchInput1 + 3) == 0);
    CHECK(!h.grayed[FakeHost::kCommonParams + kParamCvCount]);

    h.fill(6, 0.0f);
    h.fill(7, 1.0f);
    h.fill(8, -1.0f);
    h.fill(5, 5.0f);
    h.step();
    Engine* e = copains_engineOf(h.alg);
    CHECK(e->cvVoiceCount() == 3);
    CHECK_NEAR(e->layer(e->voice(0).layer).note, 48.0, 1e-4);
    CHECK_NEAR(e->layer(e->voice(1).layer).note, 60.0, 1e-4);
    CHECK_NEAR(e->layer(e->voice(2).layer).note, 36.0, 1e-4);

    // A user edit of a derived input snaps back to the derivation.
    h.set(kParamPitchInput1, 30);
    h.step();
    CHECK(h.get(kParamPitchInput1) == 6);
}

static void testCvCountBounds() {
    FakeHost h;
    h.create();
    h.set(kParamCvCount, 6);
    h.set(kParamGateInput, 62);
    h.step();
    CHECK(h.alg->parameters[kParamCvCount].max == 2);
    CHECK(h.get(kParamCvCount) == 2);
    CHECK(!h.definitionUpdates.empty() && h.definitionUpdates.back() == kParamCvCount);
    CHECK(h.get(kParamPitchInput1) == 63 && h.get(kParamPitchInput1 + 1) == 64);
    CHECK(copains_engineOf(h.alg)->cvVoiceCount() == 2);

    h.set(kParamGateInput, kNT_lastBus);  // no room for any pitch bus
    h.step();
    CHECK(copains_engineOf(h.alg)->cvVoiceCount() == 0);
    for (int i = 0; i < kNumVoices; ++i) CHECK(h.get(kParamPitchInput1 + i) == 0);

    h.set(kParamGateInput, 0);
    h.step();
    CHECK(h.alg->parameters[kParamCvCount].max == kNumVoices);
    CHECK(copains_engineOf(h.alg)->cvVoiceCount() == 0);
}

static void testSchmittGate() {
    FakeHost h;
    h.create();
    h.set(kParamGateInput, 1);
    h.step();
    const float pattern[] = { 0.0f, 1.2f, 0.8f, 0.6f, 1.2f, 0.4f, 0.9f, 1.1f };
    float* g = h.busPtr(1);
    for (int i = 0; i < FakeHost::kFrames; ++i) g[i] = pattern[(i / 4) % 8];
    const uint32_t before = copains_engineOf(h.alg)->voice(0).age;
    h.step();
    // Rise at 1.2; 0.8 and 0.6 stay inside the band; fall at 0.4; 0.9 does
    // not rise; 1.1 rises again.
    const uint32_t rises = copains_engineOf(h.alg)->voice(0).age - before;
    CHECK(rises == 2);
}

static void testSampleHoldAndTracking() {
    for (int sh = 1; sh >= 0; --sh) {
        FakeHost h;
        h.create();
        h.set(kParamGateInput, 3);
        h.set(kParamSampleHold, sh);
        h.fill(4, 0.0f);
        h.step();  // gate low first: a gate that starts high needs a fresh edge
        h.fill(3, 5.0f);
        h.step();
        h.fill(4, 1.0f);
        h.step();
        Engine* e = copains_engineOf(h.alg);
        CHECK_NEAR(e->layer(e->voice(0).layer).note, sh ? 48.0 : 60.0, 1e-4);
    }
}

static void testFreshEdgeAfterPanic() {
    FakeHost h;
    h.create();
    h.set(kParamGateInput, 1);
    h.fill(2, 0.0f);
    h.step();
    h.fill(1, 5.0f);
    h.step();
    Engine* e = copains_engineOf(h.alg);
    CHECK(e->voice(0).layer >= 0);
    h.midi(0xB0, 120, 0);
    for (int s = 0; s < 100; ++s) h.step();  // gate still high
    CHECK(e->activeLayerCount() == 0);
    h.fill(1, 0.0f);
    h.step();
    h.fill(1, 5.0f);
    h.step();
    CHECK(e->voice(0).layer >= 0);
}

static void testGateBusChangeNeedsFreshEdge() {
    FakeHost h;
    h.create();
    h.set(kParamGateInput, 1);
    h.fill(1, 5.0f);
    h.fill(7, 5.0f);
    h.step();  // first step adopts the high gate without an edge
    Engine* e = copains_engineOf(h.alg);
    CHECK(e->voice(0).layer == -1);
    h.fill(1, 0.0f);
    h.step();
    h.fill(1, 5.0f);
    h.step();
    CHECK(e->voice(0).layer >= 0);
    h.set(kParamGateInput, 7);  // already high
    h.step();
    CHECK(e->voice(0).layer == -1);  // old route faded, waits for an edge
    for (int s = 0; s < 10; ++s) h.step();
    CHECK(e->voice(0).layer == -1);
}

static void testRealtimePanic() {
    const uint8_t kinds[] = { 0xFC, 0xFF };
    for (uint8_t kind : kinds) {
        FakeHost h;
        h.create();
        h.midi(0x90, 60, 100);
        h.midi(0x90, 64, 100);
        h.step();
        h.realtime(kind);
        Engine* e = copains_engineOf(h.alg);
        for (int v = 0; v < kNumVoices; ++v) CHECK(e->voice(v).layer == -1 && e->voice(v).note == -1);
        h.realtime(0xF8);  // clock: ignored
    }
}

static void testParameterStrings() {
    struct Case { int p, v; const char* text; };
    const Case cases[] = {
        { kParamTime, 0, "0.003s" }, { kParamTime, 547, "0.21s" }, { kParamTime, 1000, "8.00s" },
        { kParamPan1, -200, "L < 20" }, { kParamPan1 + 1, 200, "20 > R" }, { kParamPan1 + 5, 0, "> <" },
        { kParamCurve, 1000, "sin" }, { kParamCurve, -1000, "sqr" }, { kParamCurve, 0, "tri" },
        { kParamCurve, -500, "sqr50 tri50" }, { kParamCurve, 300, "tri70 sin30" },
        { kParamFmIndex, -500, "intone 50%" }, { kParamFmIndex, 500, "linear 50%" }, { kParamFmIndex, 0, "0%" },
        { kParamRamp, 0, "50/50" }, { kParamRamp, 1000, "100/0" }, { kParamRamp, -1000, "0/100" },
        { kParamSlope, 30, "3/97" }, { kParamMidiChannel, 0, "Omni" }, { kParamMidiChannel, 5, "5" },
    };
    for (const Case& c : cases) {
        char buff[kNT_parameterStringSize];
        const int len = copains_formatParameter(c.p, c.v, buff);
        const bool ok = strcmp(buff, c.text) == 0 && len == (int)strlen(c.text);
        CHECK(ok);
        if (!ok) printf("    param %d value %d -> \"%s\" (expected \"%s\")\n", c.p, c.v, buff, c.text);
    }
    char buff[kNT_parameterStringSize];
    CHECK(copains_formatParameter(kParamLevel, 500, buff) == 0);
}

int main() {
    struct Test { const char* name; void (*fn)(); };
    const Test tests[] = {
        { "fast math", testFastMath },
        { "envelope curve and sine table", testEnvelopeShapeAndSineTable },
        { "VarSaw matches SuperCollider", testVarSawMatchesSuperCollider },
        { "MIDI pitch at 44.1/48/96 kHz", testMidiPitch },
        { "1 V/oct anchors", testVoltsPerOctave },
        { "AR envelope", testArMode },
        { "ASR envelope", testAsrMode },
        { "cycle envelope", testCycleMode },
        { "LRU allocation", testLruAllocation },
        { "same note reuses voice", testSameNoteReusesVoice },
        { "stolen note-off ignored", testStolenNoteOffIsIgnored },
        { "sustain pedal", testSustainPedal },
        { "panic", testPanic },
        { "CV/MIDI partition", testCvPartition },
        { "CV count growth waits for edge", testCvCountGrowthNeedsEdge },
        { "click-free transitions", testTransitionsAreClickFree },
        { "overlap retrigger", testOverlapRetrigger },
        { "overlap pool stays bounded", testOverlapPoolBounded },
        { "octave", testOctave },
        { "layer pool exhaustion", testLayerPoolExhaustion },
        { "extreme settings stay bounded", testExtremeSettings },
        { "pitch bend (MIDI only)", testPitchBend },
        { "morph targets", testMorphTargets },
        { "mono equals centre pan", testMonoMatchesCentre },
        { "factory and parameter table", testFactoryAndParameters },
        { "defaults match default.pam", testDefaultsMatchDefaultPatch },
        { "silence and output modes", testSilenceAndOutputModes },
        { "MIDI through plug-in", testMidiThroughPlugin },
        { "mono output", testMonoOutput },
        { "CV routing derivation", testCvRoutingDerivation },
        { "CV count bounds", testCvCountBounds },
        { "Schmitt gate", testSchmittGate },
        { "sample & hold vs tracking", testSampleHoldAndTracking },
        { "fresh edge after panic", testFreshEdgeAfterPanic },
        { "gate bus change", testGateBusChangeNeedsFreshEdge },
        { "realtime panic", testRealtimePanic },
        { "parameter strings", testParameterStrings },
        { "CPU estimate", testCpuEstimate },
    };
    for (const Test& t : tests) {
        const int before = gFailures;
        t.fn();
        printf("%s %s\n", gFailures == before ? "ok  " : "FAIL", t.name);
    }
    printf("\n%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
