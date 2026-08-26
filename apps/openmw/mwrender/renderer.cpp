#include "renderer.hpp"

#include <stdexcept>
#include <string>

#include "gl/glrenderer.hpp"

#ifdef OPENMW_RTX
#include "rtx/rtxrenderer.hpp"
#endif

namespace MWRender
{
    std::unique_ptr<Renderer> createRenderer(std::string_view name, const RendererSpec& spec)
    {
        if (name == "opengl")
            return std::make_unique<GlRenderer>(spec);

#ifdef OPENMW_RTX
        if (name == "raytrace")
            return std::make_unique<RtxRenderer>(spec);
#endif

        // **Named rather than fallen back from.** A renderer that quietly became a different one
        // answers "why does it look like that" with silence, and a build without the one asked for
        // is a configuration mistake rather than a runtime condition.
        throw std::runtime_error("this build has no renderer named \"" + std::string(name) + '"');
    }
}
