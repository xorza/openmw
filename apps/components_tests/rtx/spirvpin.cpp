#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

// The headers' `HasResultAndType` is behind this switch.
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp>

#include <components/rtxvulkan/spirvpin.hpp>

namespace Rtx
{
    namespace
    {
        void append(std::vector<std::uint32_t>& words, spv::Op op, std::initializer_list<std::uint32_t> operands)
        {
            words.push_back(static_cast<std::uint32_t>(operands.size() + 1) << spv::WordCountShift
                | static_cast<std::uint32_t>(op));
            words.insert(words.end(), operands.begin(), operands.end());
        }

        std::vector<std::uint32_t> spell(std::string_view text)
        {
            std::vector<std::uint32_t> words(text.size() / 4 + 1, 0);
            for (std::size_t at = 0; at < text.size(); ++at)
                words[at / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[at])) << (8 * (at % 4));
            return words;
        }

        /// One compute entry point of straight-line arithmetic on named inputs, laid out in the
        /// sections SPIR-V orders a module by — all the pinning is ever handed that matters here.
        class Writer
        {
            // First, because every id below is counted off it as it is initialised.
            std::uint32_t mNext = 1;
            std::uint32_t mLabel = mNext++;
            std::map<std::uint32_t, std::string> mNames;

        public:
            /// @param glsl whether the module imports `GLSL.std.450`, as every module `glslc` writes does.
            explicit Writer(bool glsl = true)
            {
                append(mCapabilities, spv::OpCapability, { spv::CapabilityShader });
                append(mCapabilities, spv::OpCapability, { spv::CapabilityFloat16 });
                if (glsl)
                    importSet(mGlsl, "GLSL.std.450");

                append(mTypes, spv::OpTypeVoid, { mVoid });
                append(mTypes, spv::OpTypeFunction, { mFunctionType, mVoid });
                append(mTypes, spv::OpTypeFloat, { mFloat, 32 });
                append(mTypes, spv::OpTypeFloat, { mHalf, 16 });
                append(mTypes, spv::OpTypeVector, { mVec2, mFloat, 2 });
                append(mTypes, spv::OpTypeVector, { mVec3, mFloat, 3 });
                append(mTypes, spv::OpTypeMatrix, { mMat2, mVec2, 2 });
                append(mTypes, spv::OpTypeMatrix, { mMat3, mVec3, 3 });
            }

            std::uint32_t mVoid = fresh();
            std::uint32_t mFunctionType = fresh();
            std::uint32_t mFloat = fresh();
            std::uint32_t mHalf = fresh();
            std::uint32_t mVec2 = fresh();
            std::uint32_t mVec3 = fresh();
            std::uint32_t mMat2 = fresh();
            std::uint32_t mMat3 = fresh();
            std::uint32_t mGlsl = fresh();
            std::uint32_t mMain = fresh();

            std::uint32_t fresh() { return mNext++; }

            /// A value of `type` nothing computes, which the description calls `name`.
            std::uint32_t input(std::uint32_t type, const std::string& name)
            {
                const std::uint32_t id = fresh();
                append(mTypes, spv::OpUndef, { type, id });
                mNames[id] = name;
                return id;
            }

            std::uint32_t op(spv::Op op, std::uint32_t type, std::initializer_list<std::uint32_t> operands)
            {
                const std::uint32_t id = fresh();
                std::vector<std::uint32_t> all{ type, id };
                all.insert(all.end(), operands.begin(), operands.end());
                mBody.push_back(
                    static_cast<std::uint32_t>(all.size() + 1) << spv::WordCountShift | static_cast<std::uint32_t>(op));
                mBody.insert(mBody.end(), all.begin(), all.end());
                return id;
            }

            std::uint32_t glsl(std::uint32_t type, GLSLstd450 which, std::initializer_list<std::uint32_t> operands)
            {
                const std::uint32_t id = fresh();
                std::vector<std::uint32_t> all{ type, id, mGlsl, static_cast<std::uint32_t>(which) };
                all.insert(all.end(), operands.begin(), operands.end());
                mBody.push_back(static_cast<std::uint32_t>(all.size() + 1) << spv::WordCountShift
                    | static_cast<std::uint32_t>(spv::OpExtInst));
                mBody.insert(mBody.end(), all.begin(), all.end());
                return id;
            }

            void raw(std::vector<std::uint32_t>& section, spv::Op op, std::initializer_list<std::uint32_t> operands)
            {
                append(section, op, operands);
            }

            void decorate(std::uint32_t id, spv::Decoration decoration)
            {
                append(mAnnotations, spv::OpDecorate, { id, decoration });
            }

            void importSet(std::uint32_t id, std::string_view name)
            {
                std::vector<std::uint32_t> operands{ id };
                const std::vector<std::uint32_t> spelled = spell(name);
                operands.insert(operands.end(), spelled.begin(), spelled.end());
                mImports.push_back(static_cast<std::uint32_t>(operands.size() + 1) << spv::WordCountShift
                    | static_cast<std::uint32_t>(spv::OpExtInstImport));
                mImports.insert(mImports.end(), operands.begin(), operands.end());
            }

            std::vector<std::uint32_t> finish() const
            {
                std::vector<std::uint32_t> words{ spv::MagicNumber, 0x00010600, 0, mNext, 0 };
                const auto add = [&](const std::vector<std::uint32_t>& section) {
                    words.insert(words.end(), section.begin(), section.end());
                };
                add(mCapabilities);
                add(mExtensions);
                add(mImports);
                append(words, spv::OpMemoryModel, { spv::AddressingModelLogical, spv::MemoryModelGLSL450 });
                std::vector<std::uint32_t> entry{ spv::ExecutionModelGLCompute, mMain };
                const std::vector<std::uint32_t> name = spell("main");
                entry.insert(entry.end(), name.begin(), name.end());
                words.push_back(static_cast<std::uint32_t>(entry.size() + 1) << spv::WordCountShift
                    | static_cast<std::uint32_t>(spv::OpEntryPoint));
                words.insert(words.end(), entry.begin(), entry.end());
                add(mModes);
                add(mDebug);
                add(mAnnotations);
                add(mTypes);
                append(words, spv::OpFunction, { mVoid, mMain, spv::FunctionControlMaskNone, mFunctionType });
                append(words, spv::OpLabel, { mLabel });
                add(mBody);
                append(words, spv::OpReturn, {});
                append(words, spv::OpFunctionEnd, {});
                return words;
            }

            const std::map<std::uint32_t, std::string>& names() const { return mNames; }

            std::vector<std::uint32_t> mCapabilities;
            std::vector<std::uint32_t> mExtensions;
            std::vector<std::uint32_t> mImports;
            std::vector<std::uint32_t> mModes;
            std::vector<std::uint32_t> mDebug;
            std::vector<std::uint32_t> mAnnotations;
            std::vector<std::uint32_t> mTypes;
            std::vector<std::uint32_t> mBody;
        };

        struct Decoded
        {
            struct Made
            {
                spv::Op mOp;
                std::vector<std::uint32_t> mOperands;
            };

            std::uint32_t mBound = 0;
            std::vector<Made> mOrder;
            std::map<std::uint32_t, Made> mDefinitions;
            std::map<std::uint32_t, int> mNoContraction;
            std::map<std::uint32_t, float> mConstants;
        };

        Decoded decode(const std::vector<std::uint32_t>& words)
        {
            Decoded decoded;
            decoded.mBound = words.at(3);
            for (std::size_t at = 5; at < words.size();)
            {
                const std::uint32_t count = words[at] >> spv::WordCountShift;
                Decoded::Made made{ static_cast<spv::Op>(words[at] & spv::OpCodeMask),
                    std::vector<std::uint32_t>(words.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                        words.begin() + static_cast<std::ptrdiff_t>(at + count)) };
                at += count;
                decoded.mOrder.push_back(made);

                bool result = false;
                bool type = false;
                spv::HasResultAndType(made.mOp, &result, &type);
                if (made.mOp == spv::OpDecorate && made.mOperands[1] == spv::DecorationNoContraction)
                    ++decoded.mNoContraction[made.mOperands[0]];
                if (made.mOp == spv::OpConstant)
                    decoded.mConstants[made.mOperands[1]] = std::bit_cast<float>(made.mOperands[2]);
                if (result)
                    decoded.mDefinitions[made.mOperands[type ? 1 : 0]] = made;
            }
            return decoded;
        }

        std::string glslName(std::uint32_t which)
        {
            switch (static_cast<GLSLstd450>(which))
            {
                case GLSLstd450Sqrt:
                    return "sqrt";
                case GLSLstd450InverseSqrt:
                    return "inversesqrt";
                case GLSLstd450FAbs:
                    return "abs";
                case GLSLstd450FSign:
                    return "sign";
                case GLSLstd450FClamp:
                    return "clamp";
                case GLSLstd450FMax:
                    return "max";
                case GLSLstd450Floor:
                    return "floor";
                case GLSLstd450Trunc:
                    return "trunc";
                case GLSLstd450Atan2:
                    return "atan2";
                case GLSLstd450RoundEven:
                    return "roundeven";
                case GLSLstd450Exp2:
                    return "exp2";
                default:
                    return std::format("glsl{}", which);
            }
        }

        /// What `id` computes, spelled out to the inputs and the constants, so an order or a fusion
        /// is a string to compare: `fma(a.2, b.2, mul(a.0, b.0))`.
        std::string describe(const Decoded& module, const std::map<std::uint32_t, std::string>& names, std::uint32_t id)
        {
            if (const auto named = names.find(id); named != names.end())
                return named->second;
            if (const auto constant = module.mConstants.find(id); constant != module.mConstants.end())
                return std::format("{}", constant->second);

            const auto found = module.mDefinitions.find(id);
            if (found == module.mDefinitions.end())
                return std::format("%{}", id);
            const Decoded::Made& made = found->second;
            const auto at = [&](std::size_t index) { return describe(module, names, made.mOperands.at(index)); };
            const auto call = [&](std::string_view name, std::size_t from) {
                std::string text(name);
                text += '(';
                for (std::size_t index = from; index < made.mOperands.size(); ++index)
                    text += (index == from ? "" : ", ") + at(index);
                return text + ')';
            };

            switch (made.mOp)
            {
                case spv::OpFMul:
                    return call("mul", 2);
                case spv::OpFAdd:
                    return call("add", 2);
                case spv::OpFSub:
                    return call("sub", 2);
                case spv::OpFDiv:
                    return call("div", 2);
                case spv::OpFNegate:
                    return call("neg", 2);
                case spv::OpFmaKHR:
                    return call("fma", 2);
                case spv::OpVectorTimesScalar:
                    return call("scale", 2);
                case spv::OpFOrdLessThan:
                    return call("lt", 2);
                case spv::OpSelect:
                    return call("select", 2);
                case spv::OpCompositeConstruct:
                    return call("vec", 2);
                case spv::OpCompositeExtract:
                {
                    std::string text = at(2);
                    for (std::size_t index = 3; index < made.mOperands.size(); ++index)
                        text += std::format(".{}", made.mOperands[index]);
                    return text;
                }
                case spv::OpExtInst:
                    return call(glslName(made.mOperands.at(3)), 4);
                default:
                    return call(std::format("op{}", static_cast<std::uint32_t>(made.mOp)), 2);
            }
        }

        /// **Every order-free operation becomes the steps `spirvpin.hpp` states, fused as it says.**
        /// One module per operation, on inputs named `a`, `b` and `c` for vectors, `s`, `t` and `u` for
        /// scalars and `m` for a matrix, described back from what the pinning wrote.
        TEST(RtxSpirvPinTest, everyOrderFreeOperationBecomesItsStatedSteps)
        {
            struct Case
            {
                std::string_view mName;
                std::uint32_t (*mBuild)(Writer& writer);
                std::string_view mExpected;
            };

            const std::vector<Case> cases{
                { "dot",
                    [](Writer& w) {
                        return w.op(spv::OpDot, w.mFloat, { w.input(w.mVec3, "a"), w.input(w.mVec3, "b") });
                    },
                    "fma(a.2, b.2, fma(a.1, b.1, mul(a.0, b.0)))" },
                { "length", [](Writer& w) { return w.glsl(w.mFloat, GLSLstd450Length, { w.input(w.mVec2, "a") }); },
                    "sqrt(fma(a.1, a.1, mul(a.0, a.0)))" },
                { "length of a scalar",
                    [](Writer& w) { return w.glsl(w.mFloat, GLSLstd450Length, { w.input(w.mFloat, "s") }); },
                    "abs(s)" },
                { "distance",
                    [](Writer& w) {
                        return w.glsl(w.mFloat, GLSLstd450Distance, { w.input(w.mVec2, "a"), w.input(w.mVec2, "b") });
                    },
                    "sqrt(fma(sub(a.1, b.1), sub(a.1, b.1), mul(sub(a.0, b.0), sub(a.0, b.0))))" },
                { "normalize",
                    [](Writer& w) { return w.glsl(w.mVec2, GLSLstd450Normalize, { w.input(w.mVec2, "a") }); },
                    "vec(mul(a.0, inversesqrt(fma(a.1, a.1, mul(a.0, a.0)))), "
                    "mul(a.1, inversesqrt(fma(a.1, a.1, mul(a.0, a.0)))))" },
                { "normalize of a scalar",
                    [](Writer& w) { return w.glsl(w.mFloat, GLSLstd450Normalize, { w.input(w.mFloat, "s") }); },
                    "sign(s)" },
                { "cross",
                    [](Writer& w) {
                        return w.glsl(w.mVec3, GLSLstd450Cross, { w.input(w.mVec3, "a"), w.input(w.mVec3, "b") });
                    },
                    "vec(fma(neg(a.2), b.1, mul(a.1, b.2)), fma(neg(a.0), b.2, mul(a.2, b.0)), "
                    "fma(neg(a.1), b.0, mul(a.0, b.1)))" },
                { "mix",
                    [](Writer& w) {
                        return w.glsl(w.mFloat, GLSLstd450FMix,
                            { w.input(w.mFloat, "s"), w.input(w.mFloat, "t"), w.input(w.mFloat, "u") });
                    },
                    "fma(t, u, mul(s, sub(1, u)))" },
                { "smoothstep",
                    [](Writer& w) {
                        return w.glsl(w.mFloat, GLSLstd450SmoothStep,
                            { w.input(w.mFloat, "s"), w.input(w.mFloat, "t"), w.input(w.mFloat, "u") });
                    },
                    "mul(mul(clamp(div(sub(u, s), sub(t, s)), 0, 1), clamp(div(sub(u, s), sub(t, s)), 0, 1)), "
                    "fma(neg(2), clamp(div(sub(u, s), sub(t, s)), 0, 1), 3))" },
                { "reflect",
                    [](Writer& w) {
                        return w.glsl(w.mVec2, GLSLstd450Reflect, { w.input(w.mVec2, "a"), w.input(w.mVec2, "b") });
                    },
                    "vec(fma(neg(mul(2, fma(b.1, a.1, mul(b.0, a.0)))), b.0, a.0), "
                    "fma(neg(mul(2, fma(b.1, a.1, mul(b.0, a.0)))), b.1, a.1))" },
                { "refract",
                    [](Writer& w) {
                        return w.glsl(w.mFloat, GLSLstd450Refract,
                            { w.input(w.mFloat, "s"), w.input(w.mFloat, "t"), w.input(w.mFloat, "u") });
                    },
                    "select(lt(fma(neg(mul(u, u)), fma(neg(mul(t, s)), mul(t, s), 1), 1), 0), 0, "
                    "fma(neg(fma(u, mul(t, s), sqrt(max(fma(neg(mul(u, u)), fma(neg(mul(t, s)), mul(t, s), 1), 1), "
                    "0)))), "
                    "t, mul(u, s)))" },
                { "faceforward",
                    [](Writer& w) {
                        return w.glsl(w.mFloat, GLSLstd450FaceForward,
                            { w.input(w.mFloat, "s"), w.input(w.mFloat, "t"), w.input(w.mFloat, "u") });
                    },
                    "select(lt(mul(u, t), 0), s, neg(s))" },
                { "asin", [](Writer& w) { return w.glsl(w.mFloat, GLSLstd450Asin, { w.input(w.mFloat, "s") }); },
                    "atan2(s, sqrt(fma(neg(s), s, 1)))" },
                { "acos", [](Writer& w) { return w.glsl(w.mFloat, GLSLstd450Acos, { w.input(w.mFloat, "s") }); },
                    "atan2(sqrt(fma(neg(s), s, 1)), s)" },
                { "round, whose halves a compile may take either way",
                    [](Writer& w) { return w.glsl(w.mFloat, GLSLstd450Round, { w.input(w.mFloat, "s") }); },
                    "roundeven(s)" },
                { "an explicit fma",
                    [](Writer& w) {
                        return w.glsl(w.mFloat, GLSLstd450Fma,
                            { w.input(w.mFloat, "s"), w.input(w.mFloat, "t"), w.input(w.mFloat, "u") });
                    },
                    "fma(s, t, u)" },
                { "mod",
                    [](Writer& w) {
                        return w.op(spv::OpFMod, w.mFloat, { w.input(w.mFloat, "s"), w.input(w.mFloat, "t") });
                    },
                    "fma(neg(t), floor(div(s, t)), s)" },
                { "rem",
                    [](Writer& w) {
                        return w.op(spv::OpFRem, w.mFloat, { w.input(w.mFloat, "s"), w.input(w.mFloat, "t") });
                    },
                    "fma(neg(t), trunc(div(s, t)), s)" },
                { "matrix times vector",
                    [](Writer& w) {
                        return w.op(
                            spv::OpMatrixTimesVector, w.mVec2, { w.input(w.mMat2, "m"), w.input(w.mVec2, "a") });
                    },
                    "vec(fma(m.1.0, a.1, mul(m.0.0, a.0)), fma(m.1.1, a.1, mul(m.0.1, a.0)))" },
                { "vector times matrix",
                    [](Writer& w) {
                        return w.op(
                            spv::OpVectorTimesMatrix, w.mVec2, { w.input(w.mVec2, "a"), w.input(w.mMat2, "m") });
                    },
                    "vec(fma(a.1, m.0.1, mul(a.0, m.0.0)), fma(a.1, m.1.1, mul(a.0, m.1.0)))" },
                { "matrix times matrix",
                    [](Writer& w) {
                        return w.op(
                            spv::OpMatrixTimesMatrix, w.mMat2, { w.input(w.mMat2, "m"), w.input(w.mMat2, "n") });
                    },
                    "vec(vec(fma(m.1.0, n.0.1, mul(m.0.0, n.0.0)), fma(m.1.1, n.0.1, mul(m.0.1, n.0.0))), "
                    "vec(fma(m.1.0, n.1.1, mul(m.0.0, n.1.0)), fma(m.1.1, n.1.1, mul(m.0.1, n.1.0))))" },
            };

            for (const Case& one : cases)
            {
                Writer writer;
                const std::uint32_t result = one.mBuild(writer);
                const Decoded pinned = decode(pinFloatArithmetic(writer.finish()));
                EXPECT_EQ(describe(pinned, writer.names(), result), one.mExpected) << one.mName;
            }
        }

        /// **A multiply read by nothing but one add or subtract is fused into it, and nothing else
        /// is.** The right-hand product first, so a running sum stays a sum; a negation where a
        /// subtract needs one, which is exact; a scalar spread across the vector it scales.
        TEST(RtxSpirvPinTest, aMultiplyIsFusedIntoTheOneAddThatReadsIt)
        {
            struct Case
            {
                std::string_view mName;
                std::uint32_t (*mBuild)(Writer& writer);
                std::string_view mExpected;
            };

            const std::vector<Case> cases{
                { "a product and a sum",
                    [](Writer& w) {
                        const std::uint32_t s = w.input(w.mFloat, "s");
                        const std::uint32_t t = w.input(w.mFloat, "t");
                        return w.op(
                            spv::OpFAdd, w.mFloat, { w.op(spv::OpFMul, w.mFloat, { s, t }), w.input(w.mFloat, "u") });
                    },
                    "fma(s, t, u)" },
                { "the right-hand product of two",
                    [](Writer& w) {
                        const std::uint32_t left
                            = w.op(spv::OpFMul, w.mFloat, { w.input(w.mFloat, "s"), w.input(w.mFloat, "t") });
                        const std::uint32_t right
                            = w.op(spv::OpFMul, w.mFloat, { w.input(w.mFloat, "u"), w.input(w.mFloat, "v") });
                        return w.op(spv::OpFAdd, w.mFloat, { left, right });
                    },
                    "fma(u, v, mul(s, t))" },
                { "a product less a value",
                    [](Writer& w) {
                        const std::uint32_t product
                            = w.op(spv::OpFMul, w.mFloat, { w.input(w.mFloat, "s"), w.input(w.mFloat, "t") });
                        return w.op(spv::OpFSub, w.mFloat, { product, w.input(w.mFloat, "u") });
                    },
                    "fma(s, t, neg(u))" },
                { "a value less a product",
                    [](Writer& w) {
                        const std::uint32_t u = w.input(w.mFloat, "u");
                        const std::uint32_t product
                            = w.op(spv::OpFMul, w.mFloat, { w.input(w.mFloat, "s"), w.input(w.mFloat, "t") });
                        return w.op(spv::OpFSub, w.mFloat, { u, product });
                    },
                    "fma(neg(s), t, u)" },
                { "a vector scaled and added",
                    [](Writer& w) {
                        const std::uint32_t scaled = w.op(
                            spv::OpVectorTimesScalar, w.mVec2, { w.input(w.mVec2, "a"), w.input(w.mFloat, "s") });
                        return w.op(spv::OpFAdd, w.mVec2, { scaled, w.input(w.mVec2, "b") });
                    },
                    "fma(a, vec(s, s), b)" },
                { "a product read twice",
                    [](Writer& w) {
                        const std::uint32_t product
                            = w.op(spv::OpFMul, w.mFloat, { w.input(w.mFloat, "s"), w.input(w.mFloat, "t") });
                        w.op(spv::OpFAdd, w.mFloat, { product, w.input(w.mFloat, "v") });
                        return w.op(spv::OpFAdd, w.mFloat, { product, w.input(w.mFloat, "u") });
                    },
                    "add(mul(s, t), u)" },
                { "a precise product",
                    [](Writer& w) {
                        const std::uint32_t product
                            = w.op(spv::OpFMul, w.mFloat, { w.input(w.mFloat, "s"), w.input(w.mFloat, "t") });
                        w.decorate(product, spv::DecorationNoContraction);
                        return w.op(spv::OpFAdd, w.mFloat, { product, w.input(w.mFloat, "u") });
                    },
                    "add(mul(s, t), u)" },
                { "a precise sum",
                    [](Writer& w) {
                        const std::uint32_t product
                            = w.op(spv::OpFMul, w.mFloat, { w.input(w.mFloat, "s"), w.input(w.mFloat, "t") });
                        const std::uint32_t sum = w.op(spv::OpFAdd, w.mFloat, { product, w.input(w.mFloat, "u") });
                        w.decorate(sum, spv::DecorationNoContraction);
                        return sum;
                    },
                    "add(mul(s, t), u)" },
                { "a precise dot product, whose steps keep its promise",
                    [](Writer& w) {
                        const std::uint32_t dot
                            = w.op(spv::OpDot, w.mFloat, { w.input(w.mVec3, "a"), w.input(w.mVec3, "b") });
                        w.decorate(dot, spv::DecorationNoContraction);
                        return dot;
                    },
                    "add(add(mul(a.0, b.0), mul(a.1, b.1)), mul(a.2, b.2))" },
                { "half floats, which the device is not asked to fuse",
                    [](Writer& w) {
                        const std::uint32_t product
                            = w.op(spv::OpFMul, w.mHalf, { w.input(w.mHalf, "s"), w.input(w.mHalf, "t") });
                        return w.op(spv::OpFAdd, w.mHalf, { product, w.input(w.mHalf, "u") });
                    },
                    "add(mul(s, t), u)" },
            };

            for (const Case& one : cases)
            {
                Writer writer;
                const std::uint32_t result = one.mBuild(writer);
                const Decoded pinned = decode(pinFloatArithmetic(writer.finish()));
                EXPECT_EQ(describe(pinned, writer.names(), result), one.mExpected) << one.mName;
            }
        }

        /// A fused multiply goes, unless something that does not compute still names it — a debug
        /// instruction or an `OpName` — which then reads a value the driver takes out unread.
        TEST(RtxSpirvPinTest, aFusedMultiplyStaysOnlyWhereSomethingStillNamesIt)
        {
            for (const int mention : { 0, 1, 2 })
            {
                Writer writer;
                const std::uint32_t product = writer.op(
                    spv::OpFMul, writer.mFloat, { writer.input(writer.mFloat, "s"), writer.input(writer.mFloat, "t") });
                const std::uint32_t sum
                    = writer.op(spv::OpFAdd, writer.mFloat, { product, writer.input(writer.mFloat, "u") });
                if (mention == 1)
                {
                    const std::uint32_t debug = writer.fresh();
                    writer.importSet(debug, "NonSemantic.Shader.DebugInfo.100");
                    writer.op(spv::OpExtInst, writer.mVoid, { debug, 29, product });
                }
                if (mention == 2)
                {
                    std::vector<std::uint32_t> operands{ product };
                    const std::vector<std::uint32_t> name = spell("product");
                    operands.insert(operands.end(), name.begin(), name.end());
                    writer.mDebug.push_back(static_cast<std::uint32_t>(operands.size() + 1) << spv::WordCountShift
                        | static_cast<std::uint32_t>(spv::OpName));
                    writer.mDebug.insert(writer.mDebug.end(), operands.begin(), operands.end());
                }

                const Decoded pinned = decode(pinFloatArithmetic(writer.finish()));
                EXPECT_EQ(describe(pinned, writer.names(), sum), "fma(s, t, u)") << "named " << mention;
                EXPECT_EQ(pinned.mDefinitions.contains(product), mention != 0) << "named " << mention;
            }
        }

        /// **Every rounding step that is left is `NoContraction`, once, and a module that fuses asks
        /// for the instruction it fuses with.** One of every kind of rounding step the pinning keeps,
        /// one of them decorated already, and a square root beside them that is the device's.
        TEST(RtxSpirvPinTest, everyRoundingStepIsNoContractionOnceAndAFusedModuleAsksForTheFusion)
        {
            Writer writer;
            const std::uint32_t s = writer.input(writer.mFloat, "s");
            const std::uint32_t t = writer.input(writer.mFloat, "t");
            const std::uint32_t a = writer.input(writer.mVec2, "a");
            const std::uint32_t m = writer.input(writer.mMat2, "m");
            const std::uint32_t added = writer.op(spv::OpFAdd, writer.mFloat, { s, t });
            const std::uint32_t taken = writer.op(spv::OpFSub, writer.mFloat, { s, t });
            const std::uint32_t product = writer.op(spv::OpFMul, writer.mFloat, { s, t });
            const std::uint32_t quotient = writer.op(spv::OpFDiv, writer.mFloat, { s, t });
            const std::uint32_t scaled = writer.op(spv::OpVectorTimesScalar, writer.mVec2, { a, s });
            const std::uint32_t matrix = writer.op(spv::OpMatrixTimesScalar, writer.mMat2, { m, s });
            const std::uint32_t outer = writer.op(spv::OpOuterProduct, writer.mMat2, { a, a });
            const std::uint32_t root = writer.glsl(writer.mFloat, GLSLstd450Sqrt, { s });
            const std::uint32_t fused = writer.glsl(writer.mFloat, GLSLstd450Fma, { s, t, root });
            writer.decorate(added, spv::DecorationNoContraction);

            const std::vector<std::uint32_t> words = pinFloatArithmetic(writer.finish());
            const Decoded pinned = decode(words);

            const auto decorations = [&](std::uint32_t id) {
                const auto found = pinned.mNoContraction.find(id);
                return found == pinned.mNoContraction.end() ? 0 : found->second;
            };
            for (const std::uint32_t step : { added, taken, product, quotient, scaled, matrix, outer, fused })
                EXPECT_EQ(decorations(step), 1) << describe(pinned, writer.names(), step);
            EXPECT_EQ(decorations(root), 0) << "a square root is the device's and takes no decoration";
            EXPECT_EQ(pinned.mDefinitions.at(root).mOp, spv::OpExtInst);

            std::vector<std::uint32_t> capabilities;
            std::vector<std::string> extensions;
            std::size_t firstType = 0;
            std::size_t lastDecoration = 0;
            for (std::size_t at = 0; at < pinned.mOrder.size(); ++at)
            {
                const Decoded::Made& made = pinned.mOrder[at];
                if (made.mOp == spv::OpCapability)
                    capabilities.push_back(made.mOperands[0]);
                if (made.mOp == spv::OpExtension)
                {
                    std::string text;
                    for (const std::uint32_t word : made.mOperands)
                        for (int byte = 0; byte < 4 && ((word >> (8 * byte)) & 0xFFu) != 0; ++byte)
                            text.push_back(static_cast<char>((word >> (8 * byte)) & 0xFFu));
                    extensions.push_back(text);
                }
                if (made.mOp == spv::OpDecorate)
                    lastDecoration = at;
                if (firstType == 0 && made.mOp == spv::OpTypeVoid)
                    firstType = at;
            }
            EXPECT_EQ(capabilities,
                (std::vector<std::uint32_t>{ spv::CapabilityShader, spv::CapabilityFloat16, spv::CapabilityFMAKHR }));
            EXPECT_EQ(extensions, std::vector<std::string>{ "SPV_KHR_fma" });
            EXPECT_LT(lastDecoration, firstType) << "a decoration after the first type is out of SPIR-V's layout";

            std::uint32_t highest = 0;
            for (const auto& [id, made] : pinned.mDefinitions)
                highest = std::max(highest, id);
            EXPECT_GT(pinned.mBound, highest);
        }

        /// A rewrite that needs `GLSL.std.450` in a module that does not import it imports it: `mod`
        /// takes a floor, and the set is declared once, among the imports.
        TEST(RtxSpirvPinTest, aRewriteThatNeedsTheGlslSetImportsIt)
        {
            Writer writer(false);
            const std::uint32_t result = writer.op(
                spv::OpFMod, writer.mFloat, { writer.input(writer.mFloat, "s"), writer.input(writer.mFloat, "t") });

            const Decoded pinned = decode(pinFloatArithmetic(writer.finish()));
            EXPECT_EQ(describe(pinned, writer.names(), result), "fma(neg(t), floor(div(s, t)), s)");

            std::size_t imports = 0;
            std::size_t memoryModel = 0;
            std::size_t lastImport = 0;
            for (std::size_t at = 0; at < pinned.mOrder.size(); ++at)
            {
                if (pinned.mOrder[at].mOp == spv::OpExtInstImport)
                {
                    ++imports;
                    lastImport = at;
                }
                if (pinned.mOrder[at].mOp == spv::OpMemoryModel)
                    memoryModel = at;
            }
            EXPECT_EQ(imports, 1u);
            EXPECT_LT(lastImport, memoryModel) << "an import after the memory model is out of SPIR-V's layout";
        }

        /// **A module with nothing to pin comes back as it was, word for word**: integer arithmetic,
        /// a square root and an exponential are left to the device, and nothing asks for a fusion.
        TEST(RtxSpirvPinTest, aModuleWithNothingToPinComesBackAsItWas)
        {
            Writer writer;
            const std::uint32_t s = writer.input(writer.mFloat, "s");
            writer.glsl(writer.mFloat, GLSLstd450Exp2, { writer.glsl(writer.mFloat, GLSLstd450Sqrt, { s }) });
            writer.op(spv::OpFNegate, writer.mFloat, { s });

            const std::vector<std::uint32_t> module = writer.finish();
            EXPECT_EQ(pinFloatArithmetic(module), module);
        }

        /// **What cannot be pinned stops the build, and says what it is.** One module per refusal,
        /// and a module that is not one.
        TEST(RtxSpirvPinTest, whatCannotBePinnedIsRefusedByName)
        {
            struct Case
            {
                std::string_view mName;
                void (*mBuild)(Writer& writer);
                std::string_view mSaid;
            };

            const std::vector<Case> cases{
                { "a derivative", [](Writer& w) { w.op(spv::OpDPdx, w.mFloat, { w.input(w.mFloat, "s") }); },
                    "OpDPdx" },
                { "a subgroup sum",
                    [](Writer& w) {
                        w.op(spv::OpGroupNonUniformFAdd, w.mFloat,
                            { 3, spv::GroupOperationReduce, w.input(w.mFloat, "s") });
                    },
                    "OpGroupNonUniformFAdd" },
                { "a relaxed precision",
                    [](Writer& w) {
                        w.decorate(w.op(spv::OpFAdd, w.mFloat, { w.input(w.mFloat, "s"), w.input(w.mFloat, "t") }),
                            spv::DecorationRelaxedPrecision);
                    },
                    "relaxed precision" },
                { "a fast-math mode",
                    [](Writer& w) {
                        const std::uint32_t sum
                            = w.op(spv::OpFAdd, w.mFloat, { w.input(w.mFloat, "s"), w.input(w.mFloat, "t") });
                        w.raw(w.mAnnotations, spv::OpDecorate, { sum, spv::DecorationFPFastMathMode, 0x10000 });
                    },
                    "fast-math mode" },
                { "a fast-math default",
                    [](Writer& w) {
                        w.raw(w.mModes, spv::OpExecutionModeId,
                            { w.mMain, spv::ExecutionModeFPFastMathDefault, w.mFloat, 1 });
                    },
                    "fast-math default" },
                { "another extended set",
                    [](Writer& w) {
                        const std::uint32_t set = w.fresh();
                        w.importSet(set, "SPV_AMD_shader_trinary_minmax");
                        w.op(spv::OpExtInst, w.mFloat, { set, 1, w.input(w.mFloat, "s") });
                    },
                    "neither GLSL.std.450 nor non-semantic" },
                { "a determinant",
                    [](Writer& w) { w.glsl(w.mFloat, GLSLstd450Determinant, { w.input(w.mMat2, "m") }); },
                    "OpExtInst 33" },
                { "the reserved instruction 47",
                    [](Writer& w) {
                        w.glsl(w.mFloat, GLSLstd450IMix,
                            { w.input(w.mFloat, "s"), w.input(w.mFloat, "t"), w.input(w.mFloat, "u") });
                    },
                    "not one the set defines" },
                { "an opcode newer than the headers",
                    [](Writer& w) { w.op(static_cast<spv::Op>(0xFFFF), w.mFloat, {}); },
                    "newer than the SPIR-V headers" },
            };

            for (const Case& one : cases)
            {
                Writer writer;
                one.mBuild(writer);
                try
                {
                    pinFloatArithmetic(writer.finish());
                    ADD_FAILURE() << one.mName << " was pinned";
                }
                catch (const std::runtime_error& refused)
                {
                    EXPECT_NE(std::string(refused.what()).find(one.mSaid), std::string::npos)
                        << one.mName << ": " << refused.what();
                }
            }

            // The last instruction, `OpFunctionEnd`, claiming a word the module does not have.
            std::vector<std::uint32_t> cut = Writer().finish();
            cut.back() = 2u << spv::WordCountShift | static_cast<std::uint32_t>(spv::OpFunctionEnd);
            EXPECT_THROW(pinFloatArithmetic(cut), std::runtime_error) << "an instruction that runs past the module";

            std::vector<std::uint32_t> swapped = Writer().finish();
            swapped[0] = 0x03022307;
            EXPECT_THROW(pinFloatArithmetic(swapped), std::runtime_error) << "a module in the other byte order";
        }
    }
}
