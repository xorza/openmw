-- The keys a watched run answers to. A player script, because only one sees a key; it writes
-- nothing to the world, and sends what it saw to `sky.lua`, which does.
local core = require('openmw.core')
local input = require('openmw.input')
local self = require('openmw.self')
local ui = require('openmw.ui')

-- The one place a key is named. Each is unbound in the game's defaults: F3, F4 and F10 are the
-- rasterizer's overlays, Minus, Equals and the brackets cycle spells and weapons.
local keys = {
    [input.KEY.F6] = { event = 'RtxNextWeather' },
    [input.KEY.F7] = { event = 'RtxPauseClock' },
    [input.KEY.F8] = { event = 'RtxSpeedClock' },
    [input.KEY.PageUp] = { event = 'RtxAddHours', hours = 1 },
    [input.KEY.PageDown] = { event = 'RtxAddHours', hours = -1 },
}

return {
    engineHandlers = {
        onKeyPress = function(key)
            local bound = keys[key.code]
            if bound then
                core.sendGlobalEvent(bound.event, { player = self, hours = bound.hours })
            end
        end,
    },
    eventHandlers = {
        RtxSay = function(text)
            ui.showMessage(text)
        end,
    },
}
