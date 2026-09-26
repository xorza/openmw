#pragma once

namespace SDLUtil
{
    /// What whoever draws into the window hears from `InputWrapper`: the moments and the keys
    /// that are its own rather than the game's. Every answer is empty by default, for a renderer
    /// that reads the window's size itself and keeps no handlers of its own.
    class GraphicsListener
    {
    public:
        virtual ~GraphicsListener() = default;

        /// The events of one frame are about to be read.
        virtual void beginEvents() {}

        /// F1 to F12, as `index` nought to eleven, held or let go — with no Alt held, which is the
        /// window manager's. The game's own bindings see the key as well.
        virtual void functionKey(int index, bool pressed) {}

        /// The window's drawable size changed. In pixels, and never nought by nought, which is a
        /// window that was minimised.
        virtual void windowResized(int x, int y, int width, int height) {}
    };
}
