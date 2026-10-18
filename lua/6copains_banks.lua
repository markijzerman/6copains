-- 6Copains Banks
-- Clock-driven note player with 8 note banks that sends MIDI notes to the
-- 6Copains plug-in (or any MIDI instrument). Port of JF Banks: on every clock
-- tick each of the bank's 6 voices fires with Note Amount probability, at a
-- random velocity. Settings and banks are saved with the preset.
--
-- Author: Mark IJzerman

----------------------------------------------------------------
-- RNG (LCG, no dependency on math.random) - same as JF Banks
----------------------------------------------------------------
local rngState = 12345
local function rng()
    rngState = (rngState * 1103515245 + 12345) & 0x7FFFFFFF
    return ((rngState >> 16) & 0x7FFF) / 32767.0  -- 0.0 .. 1.0
end

----------------------------------------------------------------
-- Note name table: NOTE_NAMES[61] = "C4" (MIDI note 60)
----------------------------------------------------------------
local noteLetters = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"}
local NOTE_NAMES = {}
for i = 0, 127 do
    NOTE_NAMES[i + 1] = noteLetters[(i % 12) + 1] .. tostring(i // 12 - 1)
end

----------------------------------------------------------------
-- Parameters
--   1-9   : main controls
--   10-57 : note banks (bank b, voice v) -> 10 + (b-1)*6 + (v-1)
--   58    : octave (appended later; indices are stored in presets)
--   59    : bank from (script's own Bank, or 6Copains' "Script bank")
----------------------------------------------------------------
local P_CHANNEL   = 1
local P_DEST      = 2
local P_BANK      = 3
local P_RATE      = 4
local P_RATE_RND  = 5
local P_AMOUNT    = 6
local P_VEL_MIN   = 7
local P_VEL_MAX   = 8
local P_GATE      = 9
local P_NOTE_BASE = 10
local P_OCTAVE    = 58
local P_BANK_FROM = 59

local NUM_BANKS  = 8
local NUM_VOICES = 6

local function noteIdx(b, v) return P_NOTE_BASE + (b - 1) * NUM_VOICES + (v - 1) end

local DEST_NAMES = {"Internal", "Internal+USB", "Breakout", "Int+Breakout"}
local DEST_MASKS = {0x8, 0x8 | 0x4, 0x1, 0x8 | 0x1}

-- Bank 1 is JF Banks' default (C major pentatonic); the others are starting points.
local DEFAULT_BANKS = {
    {60, 62, 64, 67, 69, 72},  -- C major pentatonic
    {57, 60, 62, 64, 67, 69},  -- A minor pentatonic
    {50, 57, 62, 64, 65, 69},  -- D dorian
    {53, 60, 64, 67, 71, 76},  -- F lydian
    {52, 55, 59, 64, 65, 71},  -- E phrygian
    {60, 62, 64, 66, 68, 70},  -- whole tone
    {48, 55, 62, 69, 76, 83},  -- stacked fifths
    {72, 74, 76, 79, 81, 84},  -- high pentatonic
}

local MIN_INTERVAL = 0.005  -- seconds, after Clock Rand jitter
local MIN_GATE     = 0.005  -- seconds

local stepOut = {}  -- no outputs, but step must return a table

-- The bank set by 6Copains' "Script bank" parameter, or nil if there is no
-- 6Copains (called monami.e before 1.0) in the preset. Lua parameters are
-- dropped from the Performance Page when a preset loads, so the knob (and its
-- CV) can live on the plug-in instead.
local function pluginBank(self)
    local count = getAlgorithmCount()
    if self.bankAlgCount ~= count then
        self.bankAlgCount = count
        self.bankAlg = findAlgorithm("6Copains") or findAlgorithm("monami.e")
        self.bankParam = self.bankAlg and findParameter(self.bankAlg, "Script bank") or nil
    end
    if not (self.bankAlg and self.bankParam) then return nil end
    local v = getParameter(self.bankAlg, self.bankParam)
    if not v then return nil end
    return math.max(1, math.min(NUM_BANKS, math.floor(v + 0.5)))
end

local function currentBank(self)
    local params = self.parameters
    if params[P_BANK_FROM] == 2 then
        local b = pluginBank(self)
        if b then return b, true end
    end
    return params[P_BANK], false
end

local function noteOn(dest, channel, note, velocity)
    sendMIDI(dest, 0x90 | (channel - 1), note, velocity)
end

local function noteOff(dest, channel, note)
    sendMIDI(dest, 0x80 | (channel - 1), note, 0)
end

-- Send note-offs for every note this script still holds.
local function releaseAll(self, dest, channel)
    for v = 1, NUM_VOICES do
        local held = self.held[v]
        if held.active then
            noteOff(dest, channel, held.note)
            held.active = false
        end
    end
end

----------------------------------------------------------------
-- Script
----------------------------------------------------------------
return {
    name   = "6Copains Banks",
    author = "Mark IJzerman",

    init = function(self)
        self.now       = 0.0
        self.nextTick  = 0.0
        self.held      = {}  -- per bank voice: note waiting for its note-off
        self.fired     = {}  -- per bank voice: time of last note-on (display)
        for v = 1, NUM_VOICES do
            self.held[v]  = {note = 0, off = 0.0, active = false}
            self.fired[v] = -1.0
        end
        self.lastChannel = nil
        self.lastDest    = nil

        local p = {}
        p[P_CHANNEL]  = {"MIDI channel", 1, 16, 1, kNone}
        p[P_DEST]     = {"Destination", DEST_NAMES, 1}
        p[P_BANK]     = {"Bank", 1, NUM_BANKS, 1, kNone}
        p[P_RATE]     = {"Clock Rate", 20, 2000, 150, kMs}
        p[P_RATE_RND] = {"Clock Rand", 0, 100, 0, kPercent}
        p[P_AMOUNT]   = {"Note Amount", 0, 100, 40, kPercent}
        p[P_VEL_MIN]  = {"Vel Min", 1, 127, 40, kNone}
        p[P_VEL_MAX]  = {"Vel Max", 1, 127, 100, kNone}
        p[P_GATE]     = {"Gate Length", 1, 100, 50, kPercent}
        for b = 1, NUM_BANKS do
            for v = 1, NUM_VOICES do
                p[noteIdx(b, v)] = {
                    string.format("Bk%d V%d", b, v),
                    0, 127, DEFAULT_BANKS[b][v],
                    kMIDINote
                }
            end
        end
        p[P_OCTAVE]   = {"Octave", -3, 3, 0, kNone}
        p[P_BANK_FROM] = {"Bank from", {"Script", "6Copains"}, 2}

        return {
            inputs     = 0,
            outputs    = 0,
            parameters = p,
        }
    end,

    ----------------------------------------------------------------
    -- step (called every 1ms)
    ----------------------------------------------------------------
    step = function(self, dt, inputs)
        local params  = self.parameters
        local channel = params[P_CHANNEL]
        local dest    = DEST_MASKS[params[P_DEST]] or 0x8

        self.now = self.now + dt
        local now = self.now

        -- A routing change releases what was started on the old route.
        if channel ~= self.lastChannel or dest ~= self.lastDest then
            if self.lastChannel then releaseAll(self, self.lastDest, self.lastChannel) end
            self.lastChannel = channel
            self.lastDest    = dest
        end

        -- Scheduled note-offs.
        for v = 1, NUM_VOICES do
            local held = self.held[v]
            if held.active and now >= held.off then
                noteOff(dest, channel, held.note)
                held.active = false
            end
        end

        if now < self.nextTick then return stepOut end

        -- Clock tick. Clock Rand jitters each interval by up to +/- the given %.
        local interval = params[P_RATE] / 1000.0
        local rnd = params[P_RATE_RND] / 100.0
        if rnd > 0 then interval = interval * (1.0 + (rng() * 2.0 - 1.0) * rnd) end
        if interval < MIN_INTERVAL then interval = MIN_INTERVAL end
        self.nextTick = now + interval

        local gate = interval * params[P_GATE] / 100.0
        if gate < MIN_GATE then gate = MIN_GATE end

        local bank   = currentBank(self)
        local amount = params[P_AMOUNT]
        local velLo  = math.min(params[P_VEL_MIN], params[P_VEL_MAX])
        local velHi  = math.max(params[P_VEL_MIN], params[P_VEL_MAX])
        local shift  = 12 * params[P_OCTAVE]

        for v = 1, NUM_VOICES do
            if rng() * 100.0 < amount then
                local held = self.held[v]
                if held.active then noteOff(dest, channel, held.note) end
                local note = math.max(0, math.min(127, params[noteIdx(bank, v)] + shift))
                local velocity = velLo + math.floor(rng() * (velHi - velLo) + 0.5)
                noteOn(dest, channel, note, velocity)
                held.note   = note
                held.off    = now + gate
                held.active = true
                self.fired[v] = now
            end
        end

        return stepOut
    end,

    ----------------------------------------------------------------
    -- draw (30fps)
    ----------------------------------------------------------------
    draw = function(self)
        local params = self.parameters
        local bank, fromMonami = currentBank(self)

        drawText(0, 20, string.format("6Copains Banks  ch%d %s",
            params[P_CHANNEL], DEST_NAMES[params[P_DEST]]))

        -- Current bank; a note lights up briefly when it fires.
        drawText(0, 31, string.format(fromMonami and "Bk%d*" or "Bk%d:", bank))
        for v = 1, NUM_VOICES do
            local recent = self.fired[v] >= 0 and (self.now - self.fired[v]) < 0.12
            drawText(30 + (v - 1) * 36, 31, NOTE_NAMES[params[noteIdx(bank, v)] + 1], recent and 15 or 6)
        end

        drawText(0, 42, string.format("Rate:%dms  Rnd:%d%%  Amt:%d%%",
            params[P_RATE], params[P_RATE_RND], params[P_AMOUNT]))
        drawText(0, 53, string.format("Vel:%d-%d  Gate:%d%%  Oct:%+d",
            params[P_VEL_MIN], params[P_VEL_MAX], params[P_GATE], params[P_OCTAVE]))

        return false  -- keep the standard parameter line at the top
    end,
}
