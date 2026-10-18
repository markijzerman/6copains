# 6Copains - development

## Layout

| Path | Contents |
|---|---|
| `src/copains_engine.h` | Synthesis engine and voice allocation. No disting NT API calls. |
| `src/copains.cpp` | Plug-in glue: parameters, pages, routing, MIDI, CV/gate scanning. |
| `src/copains_plugin.h` | Parameter indices (stored in presets: append only) and test hooks. |
| `tests/` | Native behaviour tests, a fake NT host, and API stubs. |
| `tools/render_demo.cpp` | Renders nb_monamie's patches to WAV via the native build. |
| `lua/6copains_banks.lua` | The 6Copains Banks script; `lua/tests/` holds its Lua 5.4 tests. |
| `.github/workflows/release.yml` | Builds, tests and publishes a release for `v*` tags. |

The API is the `distingNT_API` submodule, pinned to tag v1.19.0 (API v14).
Clone with `git clone --recursive`, or run `git submodule update --init`.
The plug-in ID (GUID) is `MjMa`. It was released as 6Copains before 1.0.0;
keep the ID so existing presets load.

## Commands

```bash
make            # ARM object -> plugins/6copains.o
make test       # native tests with ASan/UBSan
make lua-test   # 6Copains Banks tests (Lua 5.4)
make render     # WAV renders -> renders/
make inspect    # ELF checks, pluginEntry export, undefined symbols
make dist       # release zip -> dist/6copains-<VERSION>.zip
make verify     # test + lua-test + hardware + inspect
```

## Releasing

1. Update `VERSION` and commit.
2. Tag `v<VERSION>` and push the tag.
3. The workflow builds with Ubuntu's `gcc-arm-none-eabi`, runs both test
   suites and `inspect`, and attaches `6copains-<VERSION>.zip` to a GitHub
   release.

The zip holds `programs/plug-ins/6copains.o` and
`programs/lua/6copains_banks.lua`. nt_helper (and the NT Plugin Gallery it
installs from) routes `.o` files to `/programs/plug-ins` and `.lua` files to
`/programs/lua`. Keep exactly one plug-in asset per release: the gallery
installs the first non-source `.zip`/`.o`/`.lua` it finds.

The Makefile uses `arm-none-eabi-c++` from PATH. If it's not there, it falls
back to the newest Arm GNU Toolchain in `/Applications` (installed by
`brew install --cask gcc-arm-embedded`).

The hardware build uses `-Wdouble-promotion -Werror`, so an accidental double
in the audio path fails the build.

## Mapping from the SuperCollider source

| SynthDef `\monAmie` | Engine |
|---|---|
| `Lag.kr` (0.1 s) on ramp, curve, fmIndex, atk, rel | `updateControl()`, 60 dB in 100 ms per block |
| `VarSaw.ar(freq, ramp/2, ramp)` | `VarSaw` struct, sample for sample: the phase wraps only upwards, once per sample, so heavy FM can push the output outside [-1, 1] |
| `Slope.ar(tri) > 0` select of log/exp `lincurve` | `rising ? riseHalfC_ : fallHalfC_`, input clipped to [-1, 1], linear below \|curve\| 0.125 as UGen `lincurve` does |
| `LinSelectX(curve.linlin(0.5, 1, 0, 1), [snd, sin])` | `sineMix_` |
| `SinOsc` FM with `ClipNoise` depth | `fmDepth`, `noiseLo` |
| `EnvGen` AR (-6), ASR (-4), cycle (lin, loop) | `startSegment()` / `segmentDone()` |
| `gate -1.05` forced release | `kStageForced`, 50 ms linear |
| `RLPF(env.linexp(0, 1, 120, 20000), 0.78)` | coefficient table over envelope 0..1, interpolated once per block |
| `(snd * level * env).tanh * -9.dbamp`, `Pan2` | tanh table (error < 3e-5), equal-power gains cached per note |
| `voice.lua` LRU (mode 2) | `pickMidiVoice()` |

Engine full scale (1.0) is 5 V before Output gain.

## Evidence so far

- Native tests: `make test` (frequency at 44.1/48/96 kHz, V/oct anchors,
  envelope modes, allocation, sustain, panic, CV partition and routing
  derivation, Schmitt gate, S&H, fresh-edge rules, transition steps, bounded
  output under extreme settings, parameter strings).
- ARM object: `make inspect`. The only undefined symbols are `NT_*` API
  functions, `memcpy`, `memset` and `_GLOBAL_OFFSET_TABLE_`.
- Hardware testing is recorded separately; native results are not evidence of
  behaviour on the module.

## Hardware results (disting NT, 48 kHz, 24-frame blocks, 2026-10-17)

Installed with nt_helper 2.58; measured through its MCP service and the NT's
USB audio return. Firmware version not recorded.

- Loads and constructs. Parameter pages, enum and string displays look as
  designed. The derived Pitch inputs and the CV controls grey out correctly.
- Routing derivation: Gate input Aux 2 gives Pitch input 1 = Aux 3.
- Clock -> Step Sequencer -> 6Copains (CV/gate) plays. The Step Sequencer
  outputs 0 V for MIDI note 48, which is why the anchor is 48: an earlier
  build anchored at 60 played an octave high.
- Output peaks around 6.5 V when a retriggered note overlaps the previous
  note's tail (Output gain +12 dB). Six voices at +12 dB hit the USB return's
  10 V full scale, so the default gain is now +6 dB.
- MIDI over USB: a held 5-note chord plus the CV voice sounded; pitch bend
  and CC 123 were sent.
- CPU (nt_helper per-slot readout): about 3 % for the single-voice sequencer
  line and 25-31 % with six voices sounding. This is about 3x the native
  benchmark's ratio and is the next thing to profile, starting with
  `NT_getCpuCycleCount()` around `step()`.
- Not yet checked on hardware: sustain pedal, realtime Stop/Reset, mono
  output, CV count above 1, every envelope mode.

## Testing on hardware through nt_helper (2.58.0)

nt_helper's MCP service (`http://localhost:3847/mcp`) can edit presets, read
the screen and CPU, and list the SD card. It cannot upload binary files, so
install `plugins/6copains.o` with nt_helper's plug-in manager. The NT's USB
audio return ("disting NT", 12 channels) records outputs with ffmpeg.

Two nt_helper behaviours, reproduced on 2026-10-17:

- **Rebooting the NT while nt_helper is connected wedges it.** nt_helper only
  reacts to MIDI device changes on its device-selection screen, so it keeps
  dead MIDI handles and every request times out ("No response after 5
  attempts"). Fix: reconnect or restart nt_helper after any NT reboot.
- **Inserting an algorithm above existing slots** (MCP `add` with
  `slot_index`) can exceed the 30 s tool limit when large slots must be moved.
  The new slot is left as an unhydrated placeholder (0 parameters), and
  nt_helper then retries a request for it that times out after about 6 s,
  stalling every other request. Removing that slot clears the stall. Add at
  the end instead, then `move_algorithm` one step at a time.

MCP tool timeouts do not cancel the operation underneath, and device requests
share one serial queue, so don't retry a timed-out call straight away.

## Performance on the Cortex-M7

`make profile` builds a temporary variant with the same GUID. It draws cycle
counts on 6Copains's display from `NT_getCpuCycleCount()`: cycles per sample
in `step()`, cycles per note-sample, and micro-benchmarks run in `draw()`.
Rebuild with `make clean hardware` afterwards.

What it showed (2026-10-17, 600 MHz, 48 kHz):

- One isolated note cost about 290 cycles per sample, for roughly 100
  instructions. Code placement wasn't the cause: a build that merged all
  code into `.text` measured the same. Memory wasn't either: stack and DTC
  access cost the same.
- The cost came from float compares that feed branches. Each one compiles to
  `vcmpe` + `vmrs`, and `vmrs` waits for the FP pipeline. `renderLayer` had
  316 of them. Ternary clamps, `floor` via int casts, range checks and
  selects all produce them.
- The render loop is now written branch-free: `__builtin_fminf`/`fmaxf`
  (`vminnm`/`vmaxnm`), `__builtin_floorf`/`rintf` (`vrint*`), copysign/abs,
  `min()` for VarSaw's segment select, arithmetic blends for latches and
  rising/falling constants. `renderLayer` now has 9 `vmrs` (the whole
  object has 56, mostly setup and control code). The Overlap stress test dropped from 33-40 % to 25-31 %, and Fade
  from 15-26 % to 12-18 %.
- Native benchmarks on an out-of-order desktop CPU don't predict M7 gains.
  The earlier table and caching work was 45 % faster natively but only
  about 5 % on the module. Measure on the module.

Check new hot-path code with:

```bash
arm-none-eabi-objdump -d -C plugins/6copains.o | awk '/<copains::Engine::renderLayer/,/^$/' | grep -c vmrs
```
