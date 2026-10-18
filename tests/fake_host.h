// SPDX-License-Identifier: GPL-3.0-or-later
//
// Minimal native stand-in for the disting NT host: owns parameter values and
// bus memory, and routes NT_setParameterFromAudio() back into the plug-in the
// way the firmware does. Used by tests and by the render tool.

#ifndef COPAINS_FAKE_HOST_H
#define COPAINS_FAKE_HOST_H

#include <stdint.h>
#include <string.h>
#include <vector>

#include <distingnt/api.h>

struct FakeHost {
    static constexpr int kCommonParams = 2;  // exercises NT_parameterOffset()
    static constexpr int kFrames = 32;       // frames per step()

    const _NT_factory* factory = nullptr;
    _NT_algorithm* alg = nullptr;
    _NT_algorithmRequirements req = {};
    std::vector<uint8_t> sram, dtc;
    std::vector<int16_t> values;  // includes common parameters
    std::vector<float> bus;       // kNT_lastBus * kFrames
    std::vector<int> definitionUpdates;
    std::vector<bool> grayed;
    int setFromAudioCalls = 0;

    static FakeHost*& current() {
        static FakeHost* host = nullptr;
        return host;
    }

    void create() {
        current() = this;
        factory = (const _NT_factory*)pluginEntry(kNT_selector_factoryInfo, 0);
        factory->calculateRequirements(req, nullptr);
        sram.assign(req.sram + 16, 0);
        dtc.assign(req.dtc + 16, 0);
        _NT_algorithmMemoryPtrs ptrs = {};
        ptrs.sram = align(sram.data());
        ptrs.dtc = align(dtc.data());
        alg = factory->construct(ptrs, req, nullptr);
        values.assign(kCommonParams + req.numParameters, 0);
        grayed.assign(kCommonParams + req.numParameters, false);
        for (uint32_t p = 0; p < req.numParameters; ++p) values[kCommonParams + p] = alg->parameters[p].def;
        alg->vIncludingCommon = values.data();
        alg->v = values.data() + kCommonParams;
        bus.assign((size_t)kNT_lastBus * kFrames, 0.0f);
        for (uint32_t p = 0; p < req.numParameters; ++p) factory->parameterChanged(alg, (int)p);
    }

    int16_t get(int p) const { return values[kCommonParams + p]; }

    void set(int p, int value) {
        const _NT_parameter& d = alg->parameters[p];
        if (value < d.min) value = d.min;
        if (value > d.max) value = d.max;
        values[kCommonParams + p] = (int16_t)value;
        factory->parameterChanged(alg, p);
    }

    float* busPtr(int hostBus) { return bus.data() + (size_t)(hostBus - 1) * kFrames; }

    void fill(int hostBus, float volts) {
        float* b = busPtr(hostBus);
        for (int i = 0; i < kFrames; ++i) b[i] = volts;
    }

    void clearOutputs() {
        for (int b = kNT_numInputBusses + 1; b <= kNT_lastBus; ++b) fill(b, 0.0f);
    }

    void step() { factory->step(alg, bus.data(), kFrames / 4); }

    void midi(uint8_t b0, uint8_t b1, uint8_t b2) { factory->midiMessage(alg, b0, b1, b2); }
    void realtime(uint8_t b) { factory->midiRealtime(alg, b); }

    static uint8_t* align(uint8_t* p) { return (uint8_t*)(((uintptr_t)p + 15) & ~(uintptr_t)15); }
};

#endif  // COPAINS_FAKE_HOST_H
