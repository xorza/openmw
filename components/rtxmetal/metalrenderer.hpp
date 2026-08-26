#pragma once

#include <memory>
#include <string>

#include <components/rtx/renderer.hpp>

namespace Rtx
{
    /// Builds a Metal renderer, or nothing where this machine cannot trace with one.
    ///
    /// Declared where plain C++ can include it: the implementation is Objective-C++ and every Metal
    /// type stays inside it, so the dispatcher needs no more than a factory to call.
    std::unique_ptr<Renderer> createMetalRenderer(const RendererOptions& options, std::string& reason);
}
