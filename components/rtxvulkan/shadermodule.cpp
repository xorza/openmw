#include "shadermodule.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include <components/files/conversion.hpp>
#include <components/rtx/error.hpp>

#include "device.hpp"
#include "result.hpp"

namespace Rtx
{
    namespace
    {
        constexpr std::uint32_t sSpirvMagic = 0x07230203;

        std::vector<std::uint32_t> readSpirv(const std::filesystem::path& path)
        {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream)
                throw Error("cannot open " + Files::pathToUnicodeString(path));

            const std::streamsize size = stream.tellg();
            if (size <= 0 || size % 4 != 0)
                throw Error(Files::pathToUnicodeString(path) + " is " + std::to_string(size)
                    + " bytes, which is not a whole number of SPIR-V words");

            std::vector<std::uint32_t> words(static_cast<std::size_t>(size) / 4);
            stream.seekg(0);
            stream.read(reinterpret_cast<char*>(words.data()), size);
            if (!stream)
                throw Error("cannot read " + Files::pathToUnicodeString(path));

            if (words.front() != sSpirvMagic)
                throw Error(Files::pathToUnicodeString(path) + " does not begin with the SPIR-V magic number");

            return words;
        }
    }

    ShaderModule::ShaderModule(const Device& device, const std::filesystem::path& path)
    {
        const std::vector<std::uint32_t> words = readSpirv(path);

        const VkShaderModuleCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = words.size() * sizeof(std::uint32_t),
            .pCode = words.data(),
        };

        checkVk(vkCreateShaderModule(device.getHandle(), &createInfo, nullptr, mHandle.put(device.getHandle())),
            "vkCreateShaderModule");

        // The name is built from a path, so it can throw — and a member that has been constructed is
        // destroyed on the way out even where the object it belongs to never was.
        device.setName(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<std::uint64_t>(mHandle.get()),
            Files::pathToUnicodeString(path.filename()).c_str());
    }
}
