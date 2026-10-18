-- Native test harness for 6copains_banks.lua (Lua 5.4).
-- Stubs the disting NT Lua globals, runs step() at 1 kHz and checks the MIDI.
-- Run: make lua-test (or lua5.4 tests/test_6copains_banks.lua from lua/)

kNone, kMs, kPercent, kMIDINote = 0, 8, 4, 11

-- Algorithm lookups, as the NT provides them; MONAMI = nil means no 6Copains.
MONAMI = nil
function getAlgorithmCount() return MONAMI and 3 or 2 end
function findAlgorithm(name) return (MONAMI and name == "6Copains") and 1 or nil end
function findParameter(alg, name) return (alg == 1 and name == "Script bank") and 41 or nil end
function getParameter(alg, p) return (MONAMI and alg == 1 and p == 41) and MONAMI or nil end

local sent = {}
function sendMIDI(dest, b1, b2, b3) sent[#sent + 1] = {t = NOW, dest = dest, b1 = b1, b2 = b2, b3 = b3} end
local texts = {}
function drawText(x, y, s, colour, align) texts[#texts + 1] = {x = x, y = y, s = s, c = colour or 15} end

local checks, failures = 0, 0
local function check(cond, msg)
    checks = checks + 1
    if not cond then failures = failures + 1; print("  FAIL: " .. msg) end
end

local function load()
    local script = dofile("6copains_banks.lua")
    local config = script.init(script)
    script.parameters = {}
    for i, def in ipairs(config.parameters) do
        script.parameters[i] = (type(def[2]) == "table") and def[3] or def[4]
    end
    check(#config.parameters == 59, "59 parameters, got " .. #config.parameters)
    check(config.inputs == 0 and config.outputs == 0, "no inputs or outputs")
    return script, config
end

local function run(script, seconds)
    for _ = 1, math.floor(seconds * 1000 + 0.5) do
        NOW = (NOW or 0) + 0.001
        local out = script.step(script, 0.001, {})
        assert(type(out) == "table")
    end
end

-- Pair note-ons with note-offs per (channel, note, voice order) and summarise.
local function analyse(from)
    local ons, offs, open = 0, 0, {}
    local stats = {minVel = 128, maxVel = 0, notes = {}, channels = {}, dests = {}, badOrder = 0}
    for i = from or 1, #sent do
        local m = sent[i]
        local status, ch = m.b1 & 0xF0, (m.b1 & 0x0F) + 1
        stats.channels[ch] = true
        stats.dests[m.dest] = true
        local key = ch * 128 + m.b2
        if status == 0x90 then
            ons = ons + 1
            open[key] = (open[key] or 0) + 1
            stats.minVel = math.min(stats.minVel, m.b3)
            stats.maxVel = math.max(stats.maxVel, m.b3)
            stats.notes[m.b2] = true
        elseif status == 0x80 then
            offs = offs + 1
            if not open[key] or open[key] == 0 then stats.badOrder = stats.badOrder + 1
            else open[key] = open[key] - 1 end
        end
    end
    local hanging = 0
    for _, n in pairs(open) do hanging = hanging + n end
    stats.ons, stats.offs, stats.hanging = ons, offs, hanging
    return stats
end

print("defaults: 60 s at 150 ms, 40% amount")
do
    sent, NOW = {}, 0
    local s = load()
    run(s, 60)
    local a = analyse()
    local ticks = 60 / 0.150
    check(math.abs(a.ons - ticks * 6 * 0.40) < ticks * 6 * 0.08, string.format("~%d note-ons expected, got %d", ticks * 2.4, a.ons))
    check(a.badOrder == 0, "every note-off follows its note-on")
    check(a.hanging <= 6, "at most the last tick's notes still held, got " .. a.hanging)
    check(a.minVel >= 40 and a.maxVel <= 100, string.format("velocity in 40-100, got %d-%d", a.minVel, a.maxVel))
    for n in pairs(a.notes) do check(({[60]=1,[62]=1,[64]=1,[67]=1,[69]=1,[72]=1})[n], "note " .. n .. " from bank 1") end
    check(a.channels[1] and not a.channels[2], "channel 1 only")
    check(a.dests[0x8] and not a.dests[0x4], "internal destination only")
    print(string.format("  %d on / %d off, velocity %d-%d", a.ons, a.offs, a.minVel, a.maxVel))
end

print("note amount 100%: every voice every tick; gate length honoured")
do
    sent, NOW = {}, 0
    local s = load()
    s.parameters[6] = 100   -- Note Amount
    s.parameters[4] = 200   -- Clock Rate ms
    s.parameters[9] = 25    -- Gate Length %
    run(s, 2.0)
    local a = analyse()
    check(a.ons == 60, "10 ticks x 6 voices = 60 note-ons, got " .. a.ons)
    -- First note-off for the first tick arrives 50 ms after it.
    local firstOn, firstOff
    for _, m in ipairs(sent) do
        if (m.b1 & 0xF0) == 0x90 and not firstOn then firstOn = m.t end
        if (m.b1 & 0xF0) == 0x80 and not firstOff then firstOff = m.t end
    end
    check(firstOff and math.abs((firstOff - firstOn) - 0.050) < 0.0021, "gate 25% of 200 ms = 50 ms")
end

print("note amount 0%: silence")
do
    sent, NOW = {}, 0
    local s = load()
    s.parameters[6] = 0
    run(s, 5)
    check(#sent == 0, "no MIDI at 0%")
end

print("clock rand keeps intervals within +/- range")
do
    sent, NOW = {}, 0
    local s = load()
    s.parameters[6] = 100
    s.parameters[4] = 100
    s.parameters[5] = 50
    run(s, 30)
    local times, last = {}, nil
    for _, m in ipairs(sent) do
        if (m.b1 & 0xF0) == 0x90 and m.t ~= last then times[#times + 1] = m.t; last = m.t end
    end
    local lo, hi = 1e9, 0
    for i = 2, #times do local d = times[i] - times[i - 1]; lo = math.min(lo, d); hi = math.max(hi, d) end
    check(lo >= 0.049 and hi <= 0.1511, string.format("intervals %.3f-%.3f within 0.05-0.15 s", lo, hi))
    check(hi - lo > 0.05, "intervals actually vary")
end

print("bank switch, channel and destination change")
do
    sent, NOW = {}, 0
    local s = load()
    s.parameters[6] = 100
    s.parameters[9] = 100   -- notes held for the whole interval
    run(s, 1.0)
    s.parameters[3] = 7     -- stacked fifths
    local mark = #sent + 1
    run(s, 1.0)
    local a = analyse(mark)
    for n in pairs(a.notes) do check(({[48]=1,[55]=1,[62]=1,[69]=1,[76]=1,[83]=1})[n], "note " .. n .. " from bank 7") end
    s.parameters[1] = 5     -- channel 5
    s.parameters[2] = 2     -- Internal+USB
    mark = #sent + 1
    run(s, 0.001)
    local releasedOld = 0
    for i = mark, #sent do
        local m = sent[i]
        if (m.b1 & 0xF0) == 0x80 and (m.b1 & 0x0F) == 0 and m.dest == 0x8 then releasedOld = releasedOld + 1 end
    end
    check(releasedOld == 6, "route change releases the 6 held notes on the old channel/destination, got " .. releasedOld)
    run(s, 1.0)
    local b = analyse(mark)
    check(b.channels[5] and b.dests[0xC], "new notes on channel 5 to Internal+USB")
    local whole = analyse()
    check(whole.badOrder == 0, "on/off pairing stays balanced across changes")
end

print("octave shifts notes and their note-offs, clamped to 0-127")
do
    sent, NOW = {}, 0
    local s = load()
    s.parameters[6] = 100
    s.parameters[58] = -2
    run(s, 1.0)
    local a = analyse()
    for n in pairs(a.notes) do check(({[36]=1,[38]=1,[40]=1,[43]=1,[45]=1,[48]=1})[n], "note " .. n .. " is bank 1 two octaves down") end
    check(a.badOrder == 0, "note-offs match the shifted notes")
    s.parameters[3] = 7      -- stacked fifths, up to 83
    s.parameters[58] = 3     -- 83 + 36 = 119, 48 + 36 = 84
    local mark = #sent + 1
    run(s, 1.0)
    local b = analyse(mark)
    check(b.notes[119] and b.notes[84], "shifted up three octaves")
    s.parameters[3] = 8      -- high bank, 84 + 36 = 120; 72 + 36 = 108
    s.parameters[58] = 3
    local all = true
    mark = #sent + 1
    run(s, 0.5)
    for n in pairs(analyse(mark).notes) do if n > 127 then all = false end end
    check(all, "notes clamp at 127")
end

print("bank from 6Copains' Script bank, with fallback")
do
    sent, NOW = {}, 0
    local s = load()
    s.parameters[6] = 100
    MONAMI = 7.0                 -- 6Copains present, Script bank = 7 (stacked fifths)
    run(s, 1.0)
    for n in pairs(analyse().notes) do check(({[48]=1,[55]=1,[62]=1,[69]=1,[76]=1,[83]=1})[n], "note " .. n .. " follows 6Copains bank 7") end
    MONAMI = nil                 -- 6Copains removed: falls back to the script's own Bank (1)
    local mark = #sent + 1
    run(s, 1.0)
    for n in pairs(analyse(mark).notes) do check(({[60]=1,[62]=1,[64]=1,[67]=1,[69]=1,[72]=1})[n], "note " .. n .. " from own bank 1") end
    MONAMI = 3.0
    s.parameters[59] = 1         -- Bank from: Script
    mark = #sent + 1
    run(s, 1.0)
    for n in pairs(analyse(mark).notes) do check(({[60]=1,[62]=1,[64]=1,[67]=1,[69]=1,[72]=1})[n], "note " .. n .. " ignores 6Copains when set to Script") end
    MONAMI = 12.4                -- out of range clamps to 8
    s.parameters[59] = 2
    mark = #sent + 1
    run(s, 1.0)
    for n in pairs(analyse(mark).notes) do check(({[72]=1,[74]=1,[76]=1,[79]=1,[81]=1,[84]=1})[n], "note " .. n .. " from clamped bank 8") end
    MONAMI = nil
end

print("draw")
do
    sent, NOW, texts = {}, 0, {}
    local s = load()
    run(s, 0.5)
    s.draw(s)
    check(#texts == 10, "4 rows + 6 note names, got " .. #texts)
    for _, t in ipairs(texts) do
        check(t.x + #t.s * 6 <= 256, string.format("'%s' fits at x=%d", t.s, t.x))
    end
    local lit = 0
    for _, t in ipairs(texts) do if t.c == 15 and t.s:match("^[A-G]") then lit = lit + 1 end end
    print(string.format("  rows: %s | %s", texts[1].s, texts[#texts - 1].s))
end

print(string.format("\n%d checks, %d failures", checks, failures))
os.exit(failures == 0 and 0 or 1)
