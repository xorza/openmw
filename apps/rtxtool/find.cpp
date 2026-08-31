#include "find.hpp"

#include <cstdint>
#include <ostream>
#include <string>

#include <osg/Vec3f>

#include <components/debug/debugging.hpp>

#include "content.hpp"

namespace RtxTool
{
    int runFind(const Content& content, const ESM::Cell& cell, std::string_view needle)
    {
        std::ostream& out = Debug::getRawStdout();

        std::uint32_t found = 0;
        content.forEachObject(cell, [&](const Content::Object& object) {
            if (object.mModel.value().find(needle) == std::string::npos)
                return;

            const osg::Vec3f at = object.mTransform.getTrans();
            out << "  " << at.x() << ", " << at.y() << ", " << at.z() << "   " << object.mModel << '\n';
            ++found;
        });

        out << found << " objects match \"" << needle << "\"\n";
        return 0;
    }
}
