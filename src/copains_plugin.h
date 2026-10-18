// SPDX-License-Identifier: GPL-3.0-or-later
//
// 6Copains for disting NT - parameter indices and helpers shared with tests.
// Parameter indices are stored in presets: append new ones, never reorder.

#ifndef COPAINS_PLUGIN_H
#define COPAINS_PLUGIN_H

#include <distingnt/api.h>

enum {
    kParamOutputL,
    kParamOutputLMode,
    kParamOutputR,
    kParamOutputRMode,

    kParamLevel,
    kParamRamp,
    kParamCurve,
    kParamFmRatio,
    kParamFmIndex,
    kParamPanDrift,

    kParamEnvMode,
    kParamSlope,
    kParamTime,

    kParamMorph,
    kParamRampMod,
    kParamCurveMod,
    kParamFmMod,
    kParamSlopeMod,
    kParamTimeMod,

    kParamPan1,  // six consecutive voice pans
    kParamMidiChannel = kParamPan1 + 6,
    kParamBendRange,

    kParamGateInput,
    kParamCvCount,
    kParamSampleHold,
    kParamCvVelocity,
    kParamPitchInput1,  // six consecutive read-only pitch inputs

    kParamOutputGain = kParamPitchInput1 + 6,
    kParamRetrigger,
    kParamOctave,
    kParamScriptBank,

    kNumParams,
};

// Envelope Time parameter index 0..1000 -> 0.0026..8 s (exponential).
float copains_timeFromIndex(int index);

// parameterString() implementation; returns the string length or 0.
int copains_formatParameter(int p, int v, char* buff);

const _NT_parameter* copains_parameterTemplate();

namespace copains { class Engine; }

// Test hook: the engine owned by an algorithm instance.
copains::Engine* copains_engineOf(_NT_algorithm* algorithm);

#endif  // COPAINS_PLUGIN_H
