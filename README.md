# 6Copains

A six-voice, Just Friends-style synthesizer for the Expert Sleepers disting NT,
plus **6Copains Banks**, a Lua script that plays it from banks of notes.

6Copains is a C++ port of [nb_monamie](https://github.com/sonocircuit/nb_monamie)
(monami.e) by [sonoCircuit](https://github.com/sonocircuit), which its author
describes as "an approximation of the mannequins module 'just friends' by
whimsical raps in synthesis mode". Its waveshaping comes from
[Imaginary Friends](https://github.com/monome-community/exquisite-script/blob/master/lib/Engine_ImaginaryFriends.sc),
a SuperCollider engine by [@synthetivv](https://llllllll.co/u/synthetivv) (see
[Credits](#credits-and-licence)).

Each voice is a variable-slope triangle that bends towards a square or a sine
(**Curve**, **Ramp**), with noisy linear FM (**FM ratio**, **FM index**). It
runs through a velocity-sensitive low-pass gate driven by an AR, ASR or
cycling envelope. **Morph** fades in a set of offsets, so one mappable control
can move the whole patch.

Play it from MIDI, from a CV/gate group, or both at once, or let 6Copains Banks
play it.

## Installation

**From the NT Plugin Gallery:** install *6Copains* with
[nt_helper](https://github.com/No-Such-Device/nt_helper)'s gallery browser.
It puts both files in place.

**By hand:** download `6copains-<version>.zip` from the
[Releases](../../releases) page and copy its `programs` folder to the root of
the disting NT's MicroSD card:

- `programs/plug-ins/6copains.o`: the 6Copains algorithm (category: instrument)
- `programs/lua/6copains_banks.lua`: the 6Copains Banks script (for the Lua Script algorithm)

Then reboot the NT. It reads plug-ins and Lua scripts only at startup.

Built against disting NT C++ API v14. Expert Sleepers' firmware notes list
API v14 as part of firmware 1.19.0, so use 1.19.0 or later.

### Upgrading from monami.e

6Copains is the same algorithm renamed, with the same ID, so presets made
with monami.e load as 6Copains.

- Delete the old `programs/plug-ins/monamie.o`. Two plug-ins with the same ID
  shouldn't be on the card at once.
- If you used `monamie_banks.lua`, delete it too. In your presets, choose
  `6copains_banks.lua` again as the Lua Script's **Program**: the script list
  is sorted by name, so its position changed.

## Parameters

Percent parameters have 0.1 % resolution, so they map smoothly from CV.

### Sound

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Level | 0–100 % | 80 % | Drive into each voice's soft clipper (tanh). |
| Ramp | rise/fall 0/100 – 100/0 | 54/46 | Triangle symmetry: rising share of the cycle. 50/50 is symmetric. Changes take effect at the start of the next cycle. |
| Curve | sqr – tri – sin | sin | Fully left is square, centre is triangle, fully right is sine-like. Left of centre bends the triangle towards a square. Right of centre bends it the other way up to half-way, then crossfades to a sine. |
| FM ratio | 0.50–2.00 | 1.01 | Modulator frequency as a multiple of the note. |
| FM index | intone 100 % – 0 % – linear 100 % | 0 % | Linear FM depth with added noise; the noise rises sharply near full depth. **linear**: every voice gets the same depth. **intone**: depth scales with voice number, from none on voice 1 to full on voice 6. At high depth the oscillator's phase runs backwards, like SuperCollider's VarSaw in the original, and the shaper clips the result. That's where the FM grit comes from. |
| Pan drift | 0–100 % | 12 % | Random pan offset per note, up to ±80 % of the stereo field at 100 %. |

### Envelope

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Env mode | AR, ASR, Cycle | AR | **AR**: attack then release, ignoring note length. **ASR**: holds while the note or gate is held. **Cycle**: rises and falls repeatedly while held, then releases. |
| Slope | atk/rel 0/100 – 100/0 | 3/97 | How Time is split between attack and release. The shortest segment is 1 ms. |
| Time | 0.0026–8 s (exponential) | 0.21 s | Total attack + release time. |
| Retrigger | Fade, Overlap | Fade | What happens to a note when its voice is reused. **Fade**: as in nb_monamie, it fades out over 50 ms. **Overlap**: it keeps its own envelope (an ASR note is released) and rings on. |

The envelope also opens the low-pass gate, from 120 Hz when closed to 20 kHz
fully open, scaled by velocity. Soft notes are darker as well as quieter.

**Env mode** is read when a note starts; changing it does not affect notes
already sounding. Attack and release follow parameter changes smoothly
(100 ms lag) and apply from the next envelope segment.

### Morph

| Parameter | Range | Default |
|---|---|---|
| Morph | 0–100 % | 0 % |
| Ramp mod | ±100 % | +61 % |
| Curve mod | ±100 % | 0 % |
| FM mod | ±100 % | −51 % |
| Slope mod | ±100 % | 0 % |
| Time mod | ±100 % | 0 % |

Each *mod* parameter is an offset added to its base parameter, scaled by
**Morph**. At Morph 0 % they have no effect; at 100 % they apply in full. The
result is clamped to the base parameter's range. **Time mod** adds or removes up
to 1 second of Time.

Morph is meant to be mapped. Use the disting NT's mapping to drive it from a
CV, a MIDI CC or i2c. On norns this was the "morph amt [map me]" control,
driven by `nb:modulate`.

### Pan

| Parameter | Range | Default |
|---|---|---|
| Voice 1 pan | L < 100 – > < – 100 > R | L < 20 |
| Voice 2 pan | | 20 > R |
| Voice 3 pan | | L < 50 |
| Voice 4 pan | | 50 > R |
| Voice 5 pan | | L < 80 |
| Voice 6 pan | | 80 > R |

Equal-power pan per voice; Pan drift is added per note. The defaults alternate
sides and widen with voice number, so voices 1–2, 1–4 and 1–6 are each
balanced. Successive MIDI notes fan out across the field.

### MIDI

| Parameter | Range | Default |
|---|---|---|
| MIDI channel | Omni, 1–16 | 1 |
| Bend range | 1–24 semitones | 7 |

- **Note on/off** with velocity; note-on velocity 0 counts as note-off.
- **Pitch bend** affects MIDI voices only, not CV voices.
- **Sustain** (CC 64, on at ≥ 64) holds released notes until the pedal lifts.
- **Panic**: CC 120 (All Sound Off), CC 123 (All Notes Off), realtime Stop
  (0xFC) and System Reset (0xFF) fade every voice out over 50 ms and clear
  sustain and pitch bend. CC messages must be on the selected channel;
  Stop and Reset have no channel.
- **CC 121** (Reset All Controllers) centres pitch bend and lifts sustain.

### CV/gate

| Parameter | Range | Default |
|---|---|---|
| Gate input | None, any bus | None |
| CV count | 1–6 | 1 |
| Sample & hold | Off, On | On |
| CV velocity | 0–100 % | 100 % |
| Pitch input 1–6 | read-only | — |

One gate drives up to six voices at once, like a chord. Pitch CVs are taken
from the buses directly after the gate bus. With **Gate input** on Input 1 and
**CV count** 3, Input 1 is the gate and Inputs 2, 3 and 4 are the pitches of
voices 1, 2 and 3. The greyed-out **Pitch input** parameters show which buses
are in use, so the routing view shows them too. You cannot edit them. CV
count is limited so that no pitch bus would fall past the last bus.

- **Gate**: rises above 1.0 V, falls below 0.5 V. Noise between the two
  thresholds does not retrigger.
- **Pitch**: 1 V/octave, 0 V = MIDI note 48 = 130.81 Hz. That's the same as
  the disting NT's Step Sequencer, so its note numbers play at the pitch shown.
  CV is clamped to ±10 V and the oscillator to 20 Hz – 20 kHz.
- **Sample & hold On**: pitch is captured on the rising edge.
  **Off**: pitch follows the CV while the gate is high and holds through the
  release. The setting is read on each rising edge.
- **CV velocity** sets the velocity of CV notes, which sets brightness as well
  as level.
- A gate that is already high when you route it, or after a panic, does
  nothing until it goes low and rises again.

### Play

| Parameter | Range | Default | What it does |
|---|---|---|---|
| Octave | −3 to +3 | 0 | Transposes every note 6Copains starts (MIDI and CV) by octaves. Notes already sounding keep their pitch. |
| Script bank | 1–8 | 1 | Not used by 6Copains. A bank number for scripts such as 6Copains Banks to follow, so the bank knob and its CV can sit on the Performance Page. |

The firmware drops Performance Page items that point at Lua script parameters
when a preset loads. Parameters of a C++ plug-in like 6Copains, and their CV
mappings, stay put.

### Routing

| Parameter | Range | Default |
|---|---|---|
| Output L | bus | Output 1 |
| Output L mode | Add, Replace | Add |
| Output R | None, bus | Output 2 |
| Output R mode | Add, Replace | Add |
| Output gain | −40 to +24 dB | +6 dB |

Set **Output R** to None, or to the same bus as Output L, for a mono mix on
Output L. In mono, every voice is at the level of a centre-panned voice.

**Output gain** sets level only. At the default +6 dB, one full-velocity voice
peaks at about 2–2.5 V, depending on its pan, and a six-voice chord stays
roughly within ±6 V. +12 dB suits single lines (about 4–5 V per voice), but a
six-voice chord at +12 dB reached ±10 V in testing. The plug-in does not limit
its own output.

## CPU

Measured on a disting NT at 48 kHz (2026-10-17), with a fast clock and 3 s notes:

- 12–18 % with Retrigger on Fade (six voices retriggering);
- 25–31 % with Retrigger on Overlap and the 16-note pool full.

CPU grows with the number of sounding notes.

## Voices

There are six voices. CV/gate takes voices 1 to CV count; MIDI uses the rest.
With no Gate input, MIDI has all six. MIDI never takes a CV voice.

MIDI notes choose a voice in this order:

1. the voice already playing the same note;
2. the free voice used least recently;
3. the sustained voice used least recently;
4. the held voice used least recently, which is stolen.

A new note on a busy voice never cuts the old one hard. With **Retrigger** on
Fade (nb_monamie's behaviour), the old sound fades out over 50 ms while the new
note starts from silence, and the oscillator and filter restart. Steals and
partition changes always work this way, so they stay click-free.

With **Retrigger** on Overlap, the old note keeps its envelope, so long notes
ring past fast retriggers. Up to 16 notes can sound at once. When the pool
fills, the quietest tail fades out over 50 ms to make room.

## 6Copains Banks (Lua)

A clock-driven note player, ported from Mark IJzerman's JF Banks script for
Just Friends. A free-running clock fires the 6 notes of the selected bank. Each
note plays with Note Amount probability at a random velocity, sent as MIDI.
The default destination is the NT's internal MIDI, so a 6Copains in the same
preset (MIDI channel 1) plays it.

Add a **Lua Script** algorithm, set its **Program** to `6copains_banks.lua`,
and put 6Copains after it.

| Parameter | Range | Default | |
|---|---|---|---|
| MIDI channel | 1–16 | 1 | Match 6Copains' MIDI channel. |
| Destination | Internal, Internal+USB, Breakout, Int+Breakout | Internal | Where the notes go. |
| Bank | 1–8 | 1 | Which 6-note bank plays. |
| Clock Rate | 20–2000 ms | 150 ms | Tick interval. |
| Clock Rand | 0–100 % | 0 % | Each interval varies randomly by up to ± this much. |
| Note Amount | 0–100 % | 40 % | Chance of each bank note playing on a tick. |
| Vel Min / Vel Max | 1–127 | 40 / 100 | Random velocity range. On 6Copains velocity sets both level and brightness. |
| Gate Length | 1–100 % of the interval | 50 % | Time until note-off. Matters for 6Copains' ASR and Cycle modes; AR ignores it. |
| Bk1 V1 … Bk8 V6 | MIDI notes | see script | 8 banks × 6 notes. |
| Octave | −3 to +3 | 0 | Shifts every bank note by octaves, clamped to MIDI 0–127. |
| Bank from | Script, 6Copains | 6Copains | Where the bank comes from. **6Copains**: its *Script bank* parameter, including CV modulation, read on every clock tick; falls back to this script's Bank if no 6Copains is in the preset. |

**Bank from 6Copains:** Lua script parameters are dropped from the
Performance Page when a preset loads, but 6Copains' parameters aren't. Put
6Copains' **Script bank** on the Performance Page, and give it any CV mapping
you like. The script follows it, including the CV. The script's own display
marks this as `Bk5*`.

### Compared with JF Banks

- MIDI notes instead of Just Friends i2c commands, so JF Mode, Run and
  Retune are gone. Velocity 1–127 replaces Vol Min/Max in volts.
- New: MIDI channel, Destination and Gate Length. Notes still held when
  the channel or destination changes get a note-off.
- Clock Rand now picks one random interval per tick. JF Banks re-rolled the
  threshold every millisecond, which skewed ticks towards early.
- Clock Rate's minimum is 20 ms, not 1 ms. Six notes per millisecond would
  saturate 6Copains' voices.
- The banks start with different scales. Bank 1 is JF Banks' default,
  C major pentatonic.

## Changes from nb_monamie

- **Added**: CV/gate input, sustain pedal, panic messages, per-voice pan,
  Output gain, mono output, Retrigger (Overlap), Octave, Script bank.
- **Removed**: Send A/B and their mod amounts (norns delay/reverb sends). Put a
  delay or reverb algorithm after 6Copains instead. The global pan control is
  replaced by the six voice pans.
- **Fixed**: in nb_monamie, releasing a note whose voice had been stolen would
  release the new note on that voice. Here the stale note-off is ignored.
- **Env mode** applies to new notes. In the original, all three envelopes run
  in parallel and switching mode affects sounding notes immediately.

## nb_monamie's patches

Set these values to recall the patches that ship with nb_monamie. Parameters
not listed stay at their defaults.

| Parameter | default | soufflet | ciclette |
|---|---|---|---|
| Level | 80 % | 80 % | 80 % |
| Ramp | 54/46 | 76/24 | 50/50 |
| Curve | sin | sqr48 tri52 | tri98 sin2 |
| FM ratio | 1.01 | 0.97 | 1.50 |
| FM index | 0 % | 0 % | intone 5 % |
| Pan drift | 12 % | 48 % | 48 % |
| Env mode | AR | ASR | Cycle |
| Slope | 3/97 | 67/33 | 15/85 |
| Time | 0.21 s | 1.51 s | 0.26 s |
| Ramp mod | +61 % | −24 % | +72 % |
| Curve mod | 0 % | +59 % | 0 % |
| FM mod | −51 % | +20 % | −40 % |
| Bend range | 7 | 7 | 12 |

## Credits and licence

- **[nb_monamie](https://github.com/sonocircuit/nb_monamie)** (monami.e),
  the norns nb voice and SuperCollider engine this is ported from, by
  [sonoCircuit](https://github.com/sonocircuit). Discussion:
  [monami.e on lines](https://llllllll.co/t/monami-e/75626). GPL-3.0.
- **[Imaginary Friends](https://github.com/monome-community/exquisite-script/blob/master/lib/Engine_ImaginaryFriends.sc)**,
  the SuperCollider engine whose waveshaping nb_monamie uses, with
  permission, by [@synthetivv](https://llllllll.co/u/synthetivv), written for
  [exquisite-script](https://github.com/monome-community/exquisite-script)
  ([demo](https://llllllll.co/t/exquisite-script-a-collective-scripting-game-for-norns/34989/26)).
- **Just Friends** is a module by Mannequins (Whimsical Raps). 6Copains is an
  unofficial approximation, by way of nb_monamie, and isn't affiliated with
  them.
- **disting NT** and its [C++ API](https://github.com/expertsleepersltd/distingNT_API)
  (MIT) are by Expert Sleepers.
- disting NT port and 6Copains Banks: Mark IJzerman.

Licensed under the GNU General Public License v3.0 (see `LICENSE`), as a
derivative of nb_monamie.

Building and testing: see [DEVELOPMENT.md](DEVELOPMENT.md).
