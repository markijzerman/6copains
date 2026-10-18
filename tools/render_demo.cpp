// SPDX-License-Identifier: GPL-3.0-or-later
//
// Renders nb_monamie's bundled patches through the plug-in (native build) to
// 32-bit float stereo WAV files, for listening before loading the hardware.
// Usage: render_demo <output-dir>

#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "fake_host.h"
#include "copains_plugin.h"

struct Setting { int param, value; };

struct Patch {
    const char* name;
    std::vector<Setting> settings;
    bool morphSweep;
    int stepMs;  // note spacing
    int gateMs;  // note length
};

// Values converted from nb_monamie's data/copains_patches/*.pam (sends dropped).
static const std::vector<Patch> kPatches = {
    { "default", {}, false, 250, 150 },
    { "default_morph_sweep", {}, true, 250, 150 },
    { "soufflet", {
        { kParamSlope, 670 }, { kParamRampMod, -240 }, { kParamPanDrift, 480 },
        { kParamCurve, -480 }, { kParamFmIndex, 0 }, { kParamCurveMod, 590 },
        { kParamFmRatio, 97 }, { kParamLevel, 800 }, { kParamBendRange, 7 },
        { kParamFmMod, 200 }, { kParamRamp, 520 }, { kParamEnvMode, 1 }, { kParamTime, 792 },
      }, true, 1000, 800 },
    { "ciclette", {
        { kParamSlope, 150 }, { kParamRampMod, 720 }, { kParamPanDrift, 480 },
        { kParamCurve, 20 }, { kParamFmIndex, -50 }, { kParamCurveMod, 0 },
        { kParamFmRatio, 150 }, { kParamLevel, 800 }, { kParamBendRange, 12 },
        { kParamFmMod, -400 }, { kParamRamp, 0 }, { kParamEnvMode, 2 }, { kParamTime, 571 },
      }, true, 500, 400 },
};

static void writeWav(const std::string& path, const std::vector<float>& interleaved, int sampleRate) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        perror(path.c_str());
        return;
    }
    const uint32_t dataBytes = (uint32_t)(interleaved.size() * sizeof(float));
    const uint32_t riffBytes = 36 + dataBytes;
    const uint16_t format = 3, channels = 2, bits = 32, align = channels * bits / 8;
    const uint32_t rate = (uint32_t)sampleRate, byteRate = rate * align, fmtBytes = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riffBytes, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtBytes, 4, 1, f);
    fwrite(&format, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byteRate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataBytes, 4, 1, f);
    fwrite(interleaved.data(), sizeof(float), interleaved.size(), f);
    fclose(f);
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : ".";
    const int sampleRate = (int)NT_globals.sampleRate;
    const int notes[] = { 57, 64, 69, 72, 76, 72, 69, 64, 55, 62, 67, 71, 74, 71, 67, 62 };
    const int numNotes = (int)(sizeof(notes) / sizeof(notes[0]));
    const double seconds = 12.0;

    for (const Patch& patch : kPatches) {
        FakeHost h;
        h.create();
        h.set(kParamOutputLMode, 1);
        h.set(kParamOutputRMode, 1);
        for (const Setting& s : patch.settings) h.set(s.param, s.value);

        std::vector<float> out;
        const int steps = (int)(seconds * sampleRate / FakeHost::kFrames);
        const int framesPerStepMs = sampleRate / 1000;
        int nextOn = 0, nextOff = -1, held = -1, index = 0;
        float peak = 0.0f;
        for (int s = 0; s < steps; ++s) {
            const int ms = s * FakeHost::kFrames / framesPerStepMs;
            if (patch.morphSweep) h.set(kParamMorph, (int)(1000.0 * s / steps));
            if (held >= 0 && ms >= nextOff) {
                h.midi(0x80, (uint8_t)held, 0);
                held = -1;
            }
            if (ms >= nextOn && ms < (int)(seconds * 1000) - 1500) {
                held = notes[index++ % numNotes];
                h.midi(0x90, (uint8_t)held, (uint8_t)(70 + (index * 37) % 57));
                nextOn = ms + patch.stepMs;
                nextOff = ms + patch.gateMs;
            }
            h.step();
            for (int i = 0; i < FakeHost::kFrames; ++i) {
                const float l = h.busPtr(13)[i] / 10.0f, r = h.busPtr(14)[i] / 10.0f;  // 0 dBFS = 10 V
                out.push_back(l);
                out.push_back(r);
                peak = l > peak ? l : (-l > peak ? -l : peak);  // in units of 10 V
            }
        }
        const std::string path = dir + "/" + patch.name + ".wav";
        writeWav(path, out, sampleRate);
        printf("%-22s peak %.2f V  -> %s\n", patch.name, (double)(peak * 10.0f), path.c_str());
    }
    return 0;
}
