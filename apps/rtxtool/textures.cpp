#include "textures.hpp"

#include <cstddef>
#include <ostream>
#include <span>

#include <components/debug/debugging.hpp>
#include <components/files/conversion.hpp>
#include <components/rtx/scenedesc.hpp>
#include <components/rtx/texturebuilder.hpp>

#include "contactsheet.hpp"
#include "stagedworld.hpp"
#include "world.hpp"

namespace RtxTool
{
    int runTextures(World& world, const ESM::Cell& cell, const StagingRequest& request, const ActorRequest& actors,
        const std::filesystem::path& output, float strength)
    {
        std::ostream& out = Debug::getRawStdout();

        StagedWorld staged(world, cell, request, actors);
        const Rtx::SceneDesc& scene = staged.getScene();

        const Rtx::SceneTextures described(scene, world.getImageManager());
        const ContactSheet sheet = writeContactSheet(described.getDescriptions(), output, strength);
        if (sheet.mCount == 0)
        {
            out << "The cell uses no textures.\n";
            return 1;
        }

        // The sheet carries no lettering, so the order is printed instead: left to right, top to
        // bottom, the way it was drawn.
        const std::span<const VFS::Path::Normalized> paths = scene.getTextures();
        for (std::size_t i = 0; i < paths.size(); ++i)
            out << "  " << i << "  " << paths[i] << '\n';

        out << "wrote " << Files::pathToUnicodeString(output) << ", " << sheet.mCount
            << " textures at --delight=" << strength << '\n';
        return 0;
    }
}
