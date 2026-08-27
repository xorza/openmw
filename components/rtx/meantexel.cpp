#include "meantexel.hpp"

#include <cstdint>
#include <vector>

#include <osg/Image>

#include <components/debug/debuglog.hpp>

#include "alphaimage.hpp"
#include "error.hpp"
#include "texelreader.hpp"
#include "texturebuilder.hpp"

namespace Rtx
{
    osg::Vec3f meanTexel(const osg::Image& image)
    {
        std::vector<MipLevel> levels;

        TextureData described;
        try
        {
            described = describeImage(image, levels);
        }
        catch (const Error& what)
        {
            // A format nothing in the game produces, which is a mod's business rather than a broken
            // contract — the caller gets nothing and carries on without whatever this was worth.
            Log(Debug::Warning) << "cannot average \"" << image.getFileName() << "\": " << what.what();
            return osg::Vec3f();
        }

        if (levels.empty())
            return osg::Vec3f();

        // The finest level alone. A mip chain is the same picture at lower rates, so every level
        // holds the same mean to within its own filtering, and the coarse ones cost nothing to skip.
        const MipLevel& level = levels.front();
        if (level.mWidth == 0 || level.mHeight == 0)
            return osg::Vec3f();

        // **Never empty here**, because it is empty only for a description carrying no texels and
        // the level above is one — so the alpha is read rather than defaulted, which is the whole
        // difference between a star sheet worth nearly nothing and one worth the black it is
        // painted on.
        const AlphaImage alpha(described);

        osg::Vec3d total;
        for (std::uint32_t y = 0; y < level.mHeight; ++y)
            for (std::uint32_t x = 0; x < level.mWidth; ++x)
            {
                const osg::Vec3f stored = texelAt(described, level, x, y);
                const double covered = alpha.at(0, x, y) / 255.0;

                total += osg::Vec3d(toLinear(stored.x()), toLinear(stored.y()), toLinear(stored.z())) * covered;
            }

        total /= double(level.mWidth) * level.mHeight;

        return osg::Vec3f(float(total.x()), float(total.y()), float(total.z()));
    }
}
