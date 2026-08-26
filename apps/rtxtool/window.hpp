#pragma once

#include <cstdint>
#include <string>

#include <osg/Vec3f>

struct SDL_Window;

namespace RtxTool
{
    /// A camera someone steers with a mouse and a keyboard.
    ///
    /// Yaw turns about the world's up axis, which is +Z here, and pitch is clamped just short of
    /// vertical: straight up has no defined roll and the basis would collapse.
    class FlyCamera
    {
    public:
        /// Points the camera from `origin` at `target`, recovering the angles from the direction.
        void look(const osg::Vec3f& origin, const osg::Vec3f& target);

        /// Applies the keyboard state for a frame of `seconds`.
        void advance(float seconds);

        void turn(float deltaYaw, float deltaPitch);

        /// Multiplies the movement speed, for the mouse wheel.
        void scaleSpeed(float factor);

        const osg::Vec3f& getOrigin() const { return mOrigin; }
        osg::Vec3f getForward() const;

        /// How far ahead `getTarget` reports.
        ///
        /// The renderer only wants the direction, but a person reading `pos` and `look` in
        /// `views.cfg` wants to be able to tell where they point. A cell is eight thousand units
        /// across, so this is a landmark's distance rather than a nose's.
        static constexpr float sLookAhead = 1000.0f;

        /// A point the camera is looking at, `sLookAhead` units away.
        osg::Vec3f getTarget() const { return mOrigin + getForward() * sLookAhead; }
        float getSpeed() const { return mSpeed; }

    private:
        osg::Vec3f mOrigin;
        float mYaw = 0.0f;
        float mPitch = 0.0f;

        /// Units a second. Morrowind's player walks at about a hundred, so this is a fast walk and
        /// shift makes it a flight.
        float mSpeed = 900.0f;
    };

    /// An SDL window, and nothing about what will be drawn into it.
    ///
    /// **It names no graphics API.** The renderer is handed the `SDL_Window*` and asks SDL for
    /// whatever its own backend needs — the instance extensions, the surface — so the window stays
    /// the tool's and the surface stays the backend's.
    class Window
    {
    public:
        Window(const std::string& title, std::uint32_t width, std::uint32_t height);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        /// The drawable size in pixels, which is not the window size on a scaled display. Never
        /// zero: a minimised window reports one, which is a size a swapchain can be built at.
        std::uint32_t getWidth() const;
        std::uint32_t getHeight() const;

        /// Replaces the title bar's text. Where this tool puts its instruments: a window someone is
        /// flying has nowhere else to show a number without drawing over the thing being looked at.
        void setTitle(const std::string& title);

        SDL_Window* getHandle() const { return mHandle; }

    private:
        SDL_Window* mHandle = nullptr;
    };

}
