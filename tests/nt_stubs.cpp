// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native implementations of the disting NT API functions 6Copains calls.

#include "fake_host.h"

static float sWorkBuffer[4096];

const _NT_globals NT_globals = { 48000, FakeHost::kFrames, sWorkBuffer, sizeof(sWorkBuffer), 0, 0 };

int32_t NT_algorithmIndex(const _NT_algorithm*) { return 0; }

uint32_t NT_parameterOffset(void) { return FakeHost::kCommonParams; }

void NT_setParameterFromAudio(uint32_t, uint32_t parameter, int16_t value) {
    FakeHost* host = FakeHost::current();
    if (!host) return;
    ++host->setFromAudioCalls;
    host->set((int)parameter - FakeHost::kCommonParams, value);
}

void NT_setParameterFromUi(uint32_t index, uint32_t parameter, int16_t value) {
    NT_setParameterFromAudio(index, parameter, value);
}

void NT_setParameterGrayedOut(uint32_t, uint32_t parameter, bool gray) {
    FakeHost* host = FakeHost::current();
    if (host && parameter < host->grayed.size()) host->grayed[parameter] = gray;
}

void NT_updateParameterDefinition(uint32_t, uint32_t parameterIndex) {
    FakeHost* host = FakeHost::current();
    if (host) host->definitionUpdates.push_back((int)parameterIndex);
}
