#include <exception>
#include <format>
#include <iostream>
#include <iterator>
#include <string>

#include "spirvdigest.hpp"

// `openmw-rtx-spirv-digest`: `Rtx::digestProgram` of the disassembly on standard input, as 32 hex
// digits — what `rtx <flavour> kernels` hashes each specialized kernel by.

int main(int argc, char* /*argv*/[])
{
    if (argc != 1)
    {
        std::cerr << "usage: spirv-dis --raw-id --no-header <module.spv> | openmw-rtx-spirv-digest\n";
        return 2;
    }

    try
    {
        const std::string disassembly{ std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>() };
        const auto digest = Rtx::digestProgram(disassembly);
        std::cout << std::format("{:016x}{:016x}\n", digest[0], digest[1]);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "openmw-rtx-spirv-digest: " << error.what() << '\n';
        return 1;
    }
}
