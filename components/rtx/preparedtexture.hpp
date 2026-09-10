#pragma once

#include <array>

#include <osg/Image>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

#include "mipchain.hpp"
#include "shadingmap.hpp"

namespace Rtx
{
    /// One image, described where the model that names it was read: the levels its file did not
    /// carry, and the light painted into it.
    ///
    /// **The two things describing a texture costs, done off the frame.** `SceneTextures` builds a
    /// mip chain for a file that carried none and estimates the shading of every file, on the frame
    /// a texture arrives; both read every texel. A reader on its own thread does the same reading
    /// once per image, and the frame's describe takes what it finds here instead of reading again.
    ///
    /// Lent by a `Spares` and given back, so a chain's bytes are room the next image refills.
    struct PreparedTexture
    {
        /// The image itself, which is what the frame's describe looks a texture up by: the loader's
        /// cache hands the same object to the template and to whoever asks for the path.
        osg::ref_ptr<const osg::Image> mImage;

        /// The image's file, normalised, which is what the scene names a texture by. Kept here so
        /// that a frame adopting a layer names its texture without building the path again.
        VFS::Path::Normalized mPath;

        /// The levels the file did not carry, or empty where it carried them.
        MipChain mChain;

        /// `ShadingMap::sCells` factors, row by row.
        std::array<float, ShadingMap::sCells> mShading{};

        /// False where the image is in a format this renderer does not upload, which the frame
        /// draws the stand-in for. Nothing above is meaningful then.
        bool mReadable = false;

        /// Makes room for the next image. The chain keeps its bytes.
        void reuse()
        {
            mImage = nullptr;
            mPath.clear();
            mReadable = false;
        }
    };
}
