#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <components/rtxvulkan/spirvdigest.hpp>

namespace Rtx
{
    namespace
    {
        /// A compute kernel that reads one storage buffer's float, takes a half off it and writes it
        /// to another: bindings, a block, two variables of one type told apart by their decorations,
        /// and a subtraction whose operand order is the program.
        constexpr std::string_view sKernel = R"(OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %source %target
OpExecutionMode %main LocalSize 64 1 1
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %source DescriptorSet 0
OpDecorate %source Binding 0
OpDecorate %target DescriptorSet 0
OpDecorate %target Binding 1
OpDecorate %less NoContraction
%void = OpTypeVoid
%action = OpTypeFunction %void
%float = OpTypeFloat 32
%int = OpTypeInt 32 1
%block = OpTypeStruct %float
%blockPointer = OpTypePointer StorageBuffer %block
%floatPointer = OpTypePointer StorageBuffer %float
%source = OpVariable %blockPointer StorageBuffer
%target = OpVariable %blockPointer StorageBuffer
%zero = OpConstant %int 0
%half = OpConstant %float 0.5
%main = OpFunction %void None %action
%entry = OpLabel
%read = OpAccessChain %floatPointer %source %zero
%value = OpLoad %float %read
%less = OpFSub %float %value %half
%written = OpAccessChain %floatPointer %target %zero
OpStore %written %less
OpReturn
OpFunctionEnd
)";

        std::string replaced(std::string_view text, std::string_view from, std::string_view to)
        {
            std::string result(text);
            const std::size_t at = result.find(from);
            EXPECT_NE(at, std::string::npos) << from;
            if (at != std::string::npos)
                result.replace(at, from.size(), to);
            return result;
        }

        /// **One program is one digest, whatever it numbers its ids by and in whatever order it
        /// declares what has no order**: the kernel with every id renamed, its types, constants and
        /// variables declared in another order, its decorations shuffled and its entry point's
        /// interface turned round.
        TEST(RtxSpirvDigestTest, aProgramIsOneDigestWhateverItsIdsAndTheOrderOfWhatHasNone)
        {
            constexpr std::string_view renumbered = R"(OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %9 "main" %41 %40
OpExecutionMode %9 LocalSize 64 1 1
OpDecorate %41 Binding 1
OpDecorate %40 Binding 0
OpDecorate %33 NoContraction
OpDecorate %41 DescriptorSet 0
OpMemberDecorate %22 0 Offset 0
OpDecorate %40 DescriptorSet 0
OpDecorate %22 Block
%21 = OpTypeInt 32 1
%20 = OpTypeFloat 32
%26 = OpConstant %20 0.5
%25 = OpConstant %21 0
%22 = OpTypeStruct %20
%24 = OpTypePointer StorageBuffer %20
%23 = OpTypePointer StorageBuffer %22
%41 = OpVariable %23 StorageBuffer
%40 = OpVariable %23 StorageBuffer
%2 = OpTypeVoid
%3 = OpTypeFunction %2
%9 = OpFunction %2 None %3
%30 = OpLabel
%31 = OpAccessChain %24 %40 %25
%32 = OpLoad %20 %31
%33 = OpFSub %20 %32 %26
%34 = OpAccessChain %24 %41 %25
OpStore %34 %33
OpReturn
OpFunctionEnd
)";

            EXPECT_EQ(digestProgram(sKernel), digestProgram(renumbered));
        }

        /// **A program that does something else is another digest**, each by one change: a literal,
        /// a binding, the operands of the subtraction swapped, the result written where the source
        /// was read, a decoration on an instruction taken off, and one more instruction.
        TEST(RtxSpirvDigestTest, aProgramThatDoesSomethingElseIsAnotherDigest)
        {
            const auto digest = digestProgram(sKernel);
            const struct
            {
                std::string_view mFrom;
                std::string_view mTo;
            } changes[] = {
                { "%half = OpConstant %float 0.5", "%half = OpConstant %float 0.25" },
                { "OpDecorate %target Binding 1", "OpDecorate %target Binding 2" },
                { "%less = OpFSub %float %value %half", "%less = OpFSub %float %half %value" },
                { "%written = OpAccessChain %floatPointer %target %zero",
                    "%written = OpAccessChain %floatPointer %source %zero" },
                { "OpDecorate %less NoContraction\n", "" },
                { "OpStore %written %less\n", "OpStore %written %less\nOpStore %written %less\n" },
            };

            for (const auto& change : changes)
                EXPECT_NE(digestProgram(replaced(sKernel, change.mFrom, change.mTo)), digest) << change.mTo;
        }

        /// **Two globals nothing tells apart are told apart by the order the program first names
        /// them.** Two private floats with no decoration are one colour: writing the one and reading
        /// the other is not the program that reads back what it wrote, and swapping the two
        /// everywhere is.
        TEST(RtxSpirvDigestTest, globalsAlikeAreToldApartByTheOrderTheProgramNamesThem)
        {
            constexpr std::string_view kernel = R"(OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%action = OpTypeFunction %void
%float = OpTypeFloat 32
%pointer = OpTypePointer Private %float
%first = OpVariable %pointer Private
%second = OpVariable %pointer Private
%one = OpConstant %float 1
%main = OpFunction %void None %action
%entry = OpLabel
OpStore %first %one
%back = OpLoad %float %first
OpStore %second %back
OpReturn
OpFunctionEnd
)";

            const std::string crossed
                = replaced(kernel, "%back = OpLoad %float %first", "%back = OpLoad %float %second");
            EXPECT_NE(digestProgram(kernel), digestProgram(crossed)) << "reading the other variable";

            std::string swapped(kernel);
            swapped = replaced(swapped, "OpStore %first %one", "OpStore %second %one");
            swapped = replaced(swapped, "%back = OpLoad %float %first", "%back = OpLoad %float %second");
            swapped = replaced(swapped, "OpStore %second %back", "OpStore %first %back");
            EXPECT_EQ(digestProgram(kernel), digestProgram(swapped)) << "the two swapped everywhere";
        }

        /// **A pointer cycle is named by what it is**: a list node holding a pointer to its own kind,
        /// declared through `OpTypeForwardPointer`, renumbered and reordered, is one digest.
        TEST(RtxSpirvDigestTest, aPointerCycleIsNamedByWhatItIs)
        {
            constexpr std::string_view list = R"(OpCapability Shader
OpCapability PhysicalStorageBufferAddresses
OpMemoryModel PhysicalStorageBuffer64 GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpMemberDecorate %node 0 Offset 0
OpMemberDecorate %node 1 Offset 8
OpTypeForwardPointer %next PhysicalStorageBuffer
%void = OpTypeVoid
%action = OpTypeFunction %void
%float = OpTypeFloat 32
%node = OpTypeStruct %next %float
%next = OpTypePointer PhysicalStorageBuffer %node
%private = OpTypePointer Private %next
%head = OpVariable %private Private
%main = OpFunction %void None %action
%entry = OpLabel
%first = OpLoad %next %head
OpReturn
OpFunctionEnd
)";

            constexpr std::string_view renumbered = R"(OpCapability PhysicalStorageBufferAddresses
OpCapability Shader
OpMemoryModel PhysicalStorageBuffer64 GLSL450
OpEntryPoint GLCompute %1 "main"
OpExecutionMode %1 LocalSize 1 1 1
OpMemberDecorate %10 1 Offset 8
OpMemberDecorate %10 0 Offset 0
OpTypeForwardPointer %11 PhysicalStorageBuffer
%4 = OpTypeFloat 32
%10 = OpTypeStruct %11 %4
%11 = OpTypePointer PhysicalStorageBuffer %10
%3 = OpTypeVoid
%2 = OpTypeFunction %3
%16 = OpTypePointer Private %11
%18 = OpVariable %16 Private
%1 = OpFunction %3 None %2
%20 = OpLabel
%21 = OpLoad %11 %18
OpReturn
OpFunctionEnd
)";

            EXPECT_EQ(digestProgram(list), digestProgram(renumbered));
        }

        /// **A global is named by what it is to its full depth.** Both modules declare the same
        /// globals: a struct holding a pointer to a struct holding a float, one holding a pointer to
        /// a struct holding an int, and a variable of each. They differ only in which variable the
        /// code loads, and the two variables' types part four levels down — which the refinement's
        /// first rounds cannot see, and a refinement stopped early would name one program.
        TEST(RtxSpirvDigestTest, aGlobalIsNamedByWhatItIsToItsFullDepth)
        {
            constexpr std::string_view deep = R"(OpCapability Shader
OpCapability PhysicalStorageBufferAddresses
OpMemoryModel PhysicalStorageBuffer64 GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
OpMemberDecorate %innerFloat 0 Offset 0
OpMemberDecorate %innerInt 0 Offset 0
OpMemberDecorate %outerFloat 0 Offset 0
OpMemberDecorate %outerInt 0 Offset 0
%void = OpTypeVoid
%action = OpTypeFunction %void
%float = OpTypeFloat 32
%int = OpTypeInt 32 1
%innerFloat = OpTypeStruct %float
%innerInt = OpTypeStruct %int
%toFloat = OpTypePointer PhysicalStorageBuffer %innerFloat
%toInt = OpTypePointer PhysicalStorageBuffer %innerInt
%outerFloat = OpTypeStruct %toFloat
%outerInt = OpTypeStruct %toInt
%privateFloat = OpTypePointer Private %outerFloat
%privateInt = OpTypePointer Private %outerInt
%heldFloat = OpVariable %privateFloat Private
%heldInt = OpVariable %privateInt Private
%main = OpFunction %void None %action
%entry = OpLabel
%kept = OpLoad %outerFloat %heldFloat
OpReturn
OpFunctionEnd
)";

            EXPECT_NE(digestProgram(deep),
                digestProgram(
                    replaced(deep, "%kept = OpLoad %outerFloat %heldFloat", "%kept = OpLoad %outerInt %heldInt")));
        }

        /// **What it cannot read, it refuses, and names**: an operand nothing defines, a decoration of
        /// nothing, a line that is not an instruction, and a module with no function.
        TEST(RtxSpirvDigestTest, whatItCannotReadItRefuses)
        {
            const std::string broken[] = {
                replaced(sKernel, "%value = OpLoad %float %read", "%value = OpLoad %float %nowhere"),
                replaced(sKernel, "OpDecorate %less NoContraction", "OpDecorate %nowhere NoContraction"),
                replaced(sKernel, "OpReturn", "return"),
                "OpCapability Shader\nOpMemoryModel Logical GLSL450\n",
            };

            for (const std::string& text : broken)
                EXPECT_THROW(digestProgram(text), std::runtime_error) << text;
        }
    }
}
