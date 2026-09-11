#pragma once

namespace Surface
{
    /// A colour as a content file states one: three channels from nought to one, display-encoded.
    ///
    /// **A type, because the space is the whole of what a reader gets wrong.** Morrowind's records
    /// are written in the space the artist saw, and a renderer working in light has to divide the
    /// display curve out of every one of them — `Rtx::decodeColour` is that crossing. An
    /// `osg::Vec3f` says nothing about which side of it a value is on, so a reader that copied one
    /// across was right by inspection and wrong in fact. This is the side a renderer cannot copy
    /// out of.
    ///
    /// **No arithmetic, on purpose.** Nothing weighs, sums or scales a colour on this side of the
    /// crossing: the content states it, a controller replaces it, and a renderer decodes it. A gain
    /// belongs past the decode, where the numbers are light.
    ///
    /// **Its own header for the reason `AlphaMode` has one.** A renderer's material names this and
    /// does not want the eleven texture roles beside it.
    struct Colour
    {
        float mRed = 0.0f;
        float mGreen = 0.0f;
        float mBlue = 0.0f;

        bool operator==(const Colour& other) const = default;
    };
}
