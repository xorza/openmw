#include "window.hpp"

#include <algorithm>
#include <cmath>

#include <SDL.h>

#include <osg/Math>

#include <components/rtx/error.hpp>
#include <components/rtx/renderer.hpp>

namespace RtxTool
{
    namespace
    {
        /// Just short of straight up, where yaw stops meaning anything.
        constexpr float sMaxPitch = 1.55f;

        bool isDown(SDL_Scancode key)
        {
            return SDL_GetKeyboardState(nullptr)[key] != 0;
        }
    }

    void FlyCamera::look(const osg::Vec3f& origin, const osg::Vec3f& target)
    {
        mOrigin = origin;

        osg::Vec3f direction = target - origin;
        if (direction.length2() <= 0.0f)
            direction = osg::Vec3f(1.0f, 0.0f, 0.0f);
        direction.normalize();

        mYaw = std::atan2(direction.y(), direction.x());
        mPitch = std::clamp(std::asin(direction.z()), -sMaxPitch, sMaxPitch);
    }

    osg::Vec3f FlyCamera::getForward() const
    {
        return osg::Vec3f(std::cos(mPitch) * std::cos(mYaw), std::cos(mPitch) * std::sin(mYaw), std::sin(mPitch));
    }

    void FlyCamera::turn(float deltaYaw, float deltaPitch)
    {
        mYaw += deltaYaw;
        mPitch = std::clamp(mPitch + deltaPitch, -sMaxPitch, sMaxPitch);
    }

    void FlyCamera::scaleSpeed(float factor)
    {
        mSpeed = std::clamp(mSpeed * factor, 10.0f, 200000.0f);
    }

    void FlyCamera::advance(float seconds)
    {
        const osg::Vec3f forward = getForward();
        const osg::Vec3f up(0.0f, 0.0f, 1.0f);
        osg::Vec3f right = forward ^ up;
        if (right.length2() > 0.0f)
            right.normalize();

        osg::Vec3f movement;
        if (isDown(SDL_SCANCODE_W))
            movement += forward;
        if (isDown(SDL_SCANCODE_S))
            movement -= forward;
        if (isDown(SDL_SCANCODE_D))
            movement += right;
        if (isDown(SDL_SCANCODE_A))
            movement -= right;
        if (isDown(SDL_SCANCODE_E) || isDown(SDL_SCANCODE_SPACE))
            movement += up;
        if (isDown(SDL_SCANCODE_Q) || isDown(SDL_SCANCODE_LCTRL))
            movement -= up;

        if (movement.length2() <= 0.0f)
            return;

        movement.normalize();

        float speed = mSpeed;
        if (isDown(SDL_SCANCODE_LSHIFT))
            speed *= 6.0f;
        if (isDown(SDL_SCANCODE_LALT))
            speed *= 0.15f;

        mOrigin += movement * (speed * seconds);
    }

    Window::Window(const std::string& title, std::uint32_t width, std::uint32_t height)
    {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
            throw Rtx::Error(std::string("cannot start SDL video: ") + SDL_GetError());

        mHandle
            = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, static_cast<int>(width),
                static_cast<int>(height), Rtx::surfaceWindowFlag() | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

        if (mHandle == nullptr)
            throw Rtx::Error(std::string("cannot open a window the backend can draw in: ") + SDL_GetError());
    }

    Window::~Window()
    {
        if (mHandle != nullptr)
            SDL_DestroyWindow(mHandle);

        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    void Window::setTitle(const std::string& title)
    {
        SDL_SetWindowTitle(mHandle, title.c_str());
    }

    std::uint32_t Window::getWidth() const
    {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(mHandle, &width, &height);
        return static_cast<std::uint32_t>(std::max(width, 1));
    }

    std::uint32_t Window::getHeight() const
    {
        int width = 0;
        int height = 0;
        SDL_GetWindowSizeInPixels(mHandle, &width, &height);
        return static_cast<std::uint32_t>(std::max(height, 1));
    }
}
