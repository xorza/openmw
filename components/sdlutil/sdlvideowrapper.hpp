#ifndef OPENMW_COMPONENTS_SDLUTIL_SDLVIDEOWRAPPER_H
#define OPENMW_COMPONENTS_SDLUTIL_SDLVIDEOWRAPPER_H

#include <SDL_types.h>

struct SDL_Window;

namespace Settings
{
    enum class WindowMode;
}

namespace SDLUtil
{

    class VideoWrapper
    {
    public:
        explicit VideoWrapper(SDL_Window* window);
        ~VideoWrapper();

        void setGammaContrast(float gamma, float contrast);

        void setVideoMode(int width, int height, Settings::WindowMode windowMode, bool windowBorder);

        void centerWindow();

    private:
        SDL_Window* mWindow;

        float mGamma;
        float mContrast;
        bool mHasSetGammaContrast;

        // Store system gamma ramp on window creation. Restore system gamma ramp on exit
        Uint16 mOldSystemGammaRamp[256 * 3];
    };

}

#endif
