-- The keys a watched run answers to. A player script, because only one sees a key; it writes
-- nothing to the world, and sends what it saw to `sky.lua`, which does.
local core = require('openmw.core')
local input = require('openmw.input')
local self = require('openmw.self')
local ui = require('openmw.ui')

-- Every key that turns the world, named once. The brackets are the game's weapon cycle, which
-- returns before it does anything to a body carrying no weapon, as the harness's carries none;
-- the rest are unbound in the game's defaults. Home is the one key a window answers that is not
-- here: it prints where the window stands, which is the session's own note, and
-- `RtxTool::Session` reads it off SDL for that reason.
local keys = {
    [input.KEY.LeftBracket] = { event = 'RtxTurnWeather', steps = -1 },
    [input.KEY.RightBracket] = { event = 'RtxTurnWeather', steps = 1 },
    [input.KEY.Slash] = { event = 'RtxPauseClock' },
    [input.KEY.Comma] = { event = 'RtxSpeedClock', steps = -1 },
    [input.KEY.Period] = { event = 'RtxSpeedClock', steps = 1 },
    [input.KEY.PageUp] = { event = 'RtxAddHours', hours = 1 },
    [input.KEY.PageDown] = { event = 'RtxAddHours', hours = -1 },
}

return {
    engineHandlers = {
        onKeyPress = function(key)
            local bound = keys[key.code]
            if bound then
                core.sendGlobalEvent(bound.event, { player = self, hours = bound.hours, steps = bound.steps })
            end
        end,
    },
    eventHandlers = {
        RtxSay = function(text)
            ui.showMessage(text)
        end,
    },
}
