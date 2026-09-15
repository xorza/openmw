-- What the keys turn: the weather over the player, the game's clock, and the hour. A global
-- script, because only one may write the world. Every answer goes back to the player as `RtxSay`.
local core = require('openmw.core')
local world = require('openmw.world')

local function say(player, text)
    player:sendEvent('RtxSay', text)
end

-- The weathers the region rolls at all, in record order: the ones with a chance above nought.
local function weathersOf(regionId)
    local chances = core.regions.records[regionId].weatherProbabilities
    local allowed = {}
    for _, weather in ipairs(core.weather.records) do
        local chance = chances[weather.recordId]
        if chance and chance > 0 then
            table.insert(allowed, { weather = weather, chance = chance })
        end
    end
    return allowed
end

-- The id of the weather the last press asked for, which the walk steps from. The engine keeps
-- only the weather arriving and one queued behind it, and a press during a transition replaces
-- the queued one, so what it reports never says how far the walk got: stepping from its answer
-- stalled on the second weather for as long as the first took to arrive.
local asked = nil

local function nextWeather(player)
    local cell = player.cell
    local regionId = cell.region
    if not regionId then
        say(player, 'no sky here')
        return
    end

    local allowed = weathersOf(regionId)
    if #allowed == 0 then
        say(player, 'the region rolls no weather')
        return
    end

    -- The one after the last asked, or after the current where none was — the first where
    -- neither is one the region rolls, which a save or the console can leave it as.
    local current = core.weather.getCurrent(cell)
    local from = asked or (current and current.recordId)
    local at = 0
    for index, entry in ipairs(allowed) do
        if entry.weather.recordId == from then
            at = index
        end
    end
    local chosen = allowed[at % #allowed + 1]
    asked = chosen.weather.recordId

    -- A change is a transition of 1 / Transition_Delta seconds of the sky's clock — a minute for
    -- most, half that for a storm, and F8 speeds it — and a press during one queues behind the
    -- weather arriving.
    local arriving = core.weather.getNext(cell)
    local how = 'arriving'
    if arriving and arriving.recordId ~= chosen.weather.recordId then
        how = 'queued after ' .. arriving.name
    end

    core.weather.changeWeather(regionId, chosen.weather)
    say(player, string.format('%s %s, %d of %d, %d%%', chosen.weather.name, how, at % #allowed + 1, #allowed, chosen.chance))
end

-- The game's default `timescale`, and ten and a hundred times it.
local speeds = { 30, 300, 3000 }

-- What the clock ran at before it was paused, so a second press puts it back.
local heldScale = nil

local function pauseClock(player)
    if heldScale then
        world.setGameTimeScale(heldScale)
        say(player, string.format('clock running, ×%g', heldScale / speeds[1]))
        heldScale = nil
        return
    end

    heldScale = core.getGameTimeScale()
    if heldScale == 0 then
        heldScale = speeds[1]
    end
    world.setGameTimeScale(0)
    say(player, 'clock paused')
end

local function speedClock(player)
    -- A paused clock is started at the first speed; a running one moves to the speed after the
    -- one it is nearest to, and past the last back to the first.
    local scale = heldScale or core.getGameTimeScale()
    heldScale = nil

    local at = #speeds
    for index, speed in ipairs(speeds) do
        if scale <= speed then
            at = index
            break
        end
    end
    local chosen = speeds[at % #speeds + 1]
    if scale == 0 then
        chosen = speeds[1]
    end

    world.setGameTimeScale(chosen)
    say(player, string.format('clock ×%g', chosen / speeds[1]))
end

local function describeHour(hour)
    local whole = math.floor(hour)
    return string.format('%02d:%02d', whole, math.floor((hour - whole) * 60))
end

-- Written as the hour rather than advanced, so the day and the moons stay where they are. Past
-- midnight the engine rolls the day on by itself; before it the day is taken down here, and on
-- day one it stops at midnight, because the engine clamps the day at one.
local function addHours(player, hours)
    local globals = world.mwscript.getGlobalVariables(player)
    local hour = globals.gamehour + hours

    if hour < 0 then
        if globals.day <= 1 then
            hour = 0
        else
            globals.day = globals.day - 1
            hour = hour + 24
        end
    end

    globals.gamehour = hour
    say(player, describeHour(globals.gamehour))
end

return {
    eventHandlers = {
        RtxNextWeather = function(data)
            nextWeather(data.player)
        end,
        RtxPauseClock = function(data)
            pauseClock(data.player)
        end,
        RtxSpeedClock = function(data)
            speedClock(data.player)
        end,
        RtxAddHours = function(data)
            addHours(data.player, data.hours)
        end,
    },
}
