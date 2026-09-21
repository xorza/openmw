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

-- `steps` weathers on from the one asked for, or back where negative.
local function turnWeather(player, steps)
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

    -- Stepped from the last asked, or from the current where none was — and where neither is one
    -- the region rolls, which a save or the console can leave it as, from before the first going
    -- on and from after the last going back, so the first press lands on an end of the list.
    local current = core.weather.getCurrent(cell)
    local from = asked or (current and current.recordId)
    local at = steps > 0 and 0 or #allowed + 1
    for index, entry in ipairs(allowed) do
        if entry.weather.recordId == from then
            at = index
        end
    end
    local index = (at - 1 + steps) % #allowed + 1
    local chosen = allowed[index]
    asked = chosen.weather.recordId

    -- A change is a transition of 1 / Transition_Delta seconds of the sky's clock — a minute for
    -- most, half that for a storm, and a faster clock speeds it — and a press during one queues
    -- behind the weather arriving.
    local arriving = core.weather.getNext(cell)
    local how = 'arriving'
    if arriving and arriving.recordId ~= chosen.weather.recordId then
        how = 'queued after ' .. arriving.name
    end

    core.weather.changeWeather(regionId, chosen.weather)
    say(player, string.format('%s %s, %d of %d, %d%%', chosen.weather.name, how, index, #allowed, chosen.chance))
end

-- The game's default `timescale`, which the clock keys halve and double: ×1 is the game's own
-- day. Bounded at ×1/8, below which the sky stands still to the eye, and ×1024, at which a day
-- passes in under three seconds and the sun is a streak.
local baseScale = 30
local slowest, fastest = -3, 10

-- What the clock ran at before it was paused, so a second press puts it back.
local heldScale = nil

local function pauseClock(player)
    if heldScale then
        world.setGameTimeScale(heldScale)
        say(player, string.format('clock running, ×%g', heldScale / baseScale))
        heldScale = nil
        return
    end

    heldScale = core.getGameTimeScale()
    if heldScale == 0 then
        heldScale = baseScale
    end
    world.setGameTimeScale(0)
    say(player, 'clock paused')
end

-- `steps` powers of two faster, or slower where negative. A paused clock runs again, stepped from
-- where it was held; one the console set between two powers steps to the power on the side
-- pressed, so ×1.5 goes to ×2 and comes down to ×1.
local function speedClock(player, steps)
    local scale = heldScale or core.getGameTimeScale()
    heldScale = nil
    if scale <= 0 then
        scale = baseScale
    end

    -- The power at or below the scale, then the one at or above it, the same where the scale is
    -- a power. The products are exact, so a scale these keys set is found and not approximated.
    local below = slowest
    while below < fastest and baseScale * 2 ^ (below + 1) <= scale do
        below = below + 1
    end
    local above = below
    if baseScale * 2 ^ below < scale then
        above = below + 1
    end

    local exponent = (steps > 0 and below or above) + steps
    exponent = math.max(slowest, math.min(fastest, exponent))

    world.setGameTimeScale(baseScale * 2 ^ exponent)
    say(player, string.format('clock ×%g', 2 ^ exponent))
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
        RtxTurnWeather = function(data)
            turnWeather(data.player, data.steps)
        end,
        RtxPauseClock = function(data)
            pauseClock(data.player)
        end,
        RtxSpeedClock = function(data)
            speedClock(data.player, data.steps)
        end,
        RtxAddHours = function(data)
            addHours(data.player, data.hours)
        end,
    },
}
