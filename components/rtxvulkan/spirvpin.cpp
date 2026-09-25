#include "spirvpin.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// The headers' `HasResultAndType` and `OpToString` are behind this switch.
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/GLSL.std.450.h>
#include <spirv/unified1/spirv.hpp>

namespace Rtx
{
    namespace
    {
        constexpr std::size_t sHeaderWords = 5;

        struct Instruction
        {
            spv::Op mOp;

            /// Every word after the one holding the opcode and the count.
            std::vector<std::uint32_t> mOperands;
        };

        /// Where an instruction keeps its result and its result's type, which the headers know for
        /// every opcode they name.
        struct Layout
        {
            bool mResult = false;
            bool mType = false;
        };

        Layout layoutOf(spv::Op op)
        {
            Layout layout;
            spv::HasResultAndType(op, &layout.mResult, &layout.mType);
            return layout;
        }

        std::uint32_t operandAt(const Instruction& instruction, std::size_t at)
        {
            if (at >= instruction.mOperands.size())
                throw std::runtime_error(
                    std::format("{} is missing its operand {}", spv::OpToString(instruction.mOp), at));
            return instruction.mOperands[at];
        }

        std::optional<std::uint32_t> resultOf(const Instruction& instruction)
        {
            const Layout layout = layoutOf(instruction.mOp);
            if (!layout.mResult)
                return std::nullopt;
            return operandAt(instruction, layout.mType ? 1 : 0);
        }

        std::optional<std::uint32_t> resultTypeOf(const Instruction& instruction)
        {
            if (!layoutOf(instruction.mOp).mType)
                return std::nullopt;
            return operandAt(instruction, 0);
        }

        std::string_view nameOf(spv::Op op)
        {
            return spv::OpToString(op);
        }

        /// A literal string as SPIR-V spells one: its bytes and a terminating nought, four to a
        /// word with the first in the lowest byte, padded with noughts.
        std::vector<std::uint32_t> spell(std::string_view text)
        {
            std::vector<std::uint32_t> words(text.size() / 4 + 1, 0);
            for (std::size_t at = 0; at < text.size(); ++at)
                words[at / 4] |= static_cast<std::uint32_t>(static_cast<unsigned char>(text[at])) << (8 * (at % 4));
            return words;
        }

        std::string readString(std::span<const std::uint32_t> words)
        {
            std::string text;
            for (const std::uint32_t word : words)
                for (int byte = 0; byte < 4; ++byte)
                {
                    const char letter = static_cast<char>((word >> (8 * byte)) & 0xFFu);
                    if (letter == '\0')
                        return text;
                    text.push_back(letter);
                }
            return text;
        }

        /// What happens to an instruction of `GLSL.std.450`, from the precision the Vulkan
        /// specification's appendix "Precision of GLSL.std.450 Instructions" gives it.
        enum class Treatment : std::uint8_t
        {
            /// Not one this set defines, or one reserved: refused.
            Unknown,

            /// The specification fixes the result: correctly rounded, or the correct result.
            Pinned,

            /// The specification bounds the result by an error, or leaves its precision to the
            /// implementation, and with nothing but its own core free: left to the device.
            Bounded,

            /// Inherited from a formula whose order or fusions the specification leaves free:
            /// rewritten in the order `pinFloatArithmetic` states.
            Lowered,

            /// Inherited from such a formula and not rewritten, because nothing here uses it yet:
            /// refused, so a shader that starts to is pinned first.
            Unpinned,

            /// The fragment stage's interpolation: refused.
            Interpolation,
        };

        constexpr Treatment treatmentOf(std::uint32_t instruction)
        {
            switch (static_cast<GLSLstd450>(instruction))
            {
                case GLSLstd450RoundEven:
                case GLSLstd450Trunc:
                case GLSLstd450FAbs:
                case GLSLstd450SAbs:
                case GLSLstd450FSign:
                case GLSLstd450SSign:
                case GLSLstd450Floor:
                case GLSLstd450Ceil:
                case GLSLstd450Fract:
                case GLSLstd450Radians:
                case GLSLstd450Degrees:
                case GLSLstd450Modf:
                case GLSLstd450ModfStruct:
                case GLSLstd450FMin:
                case GLSLstd450UMin:
                case GLSLstd450SMin:
                case GLSLstd450FMax:
                case GLSLstd450UMax:
                case GLSLstd450SMax:
                case GLSLstd450FClamp:
                case GLSLstd450UClamp:
                case GLSLstd450SClamp:
                case GLSLstd450Step:
                case GLSLstd450Frexp:
                case GLSLstd450FrexpStruct:
                case GLSLstd450Ldexp:
                case GLSLstd450FindILsb:
                case GLSLstd450FindSMsb:
                case GLSLstd450FindUMsb:
                case GLSLstd450NMin:
                case GLSLstd450NMax:
                case GLSLstd450NClamp:
                    return Treatment::Pinned;

                case GLSLstd450Sin:
                case GLSLstd450Cos:
                case GLSLstd450Tan:
                case GLSLstd450Atan:
                case GLSLstd450Atan2:
                case GLSLstd450Sinh:
                case GLSLstd450Cosh:
                case GLSLstd450Tanh:
                case GLSLstd450Atanh:
                case GLSLstd450Pow:
                case GLSLstd450Exp:
                case GLSLstd450Log:
                case GLSLstd450Exp2:
                case GLSLstd450Log2:
                case GLSLstd450Sqrt:
                case GLSLstd450InverseSqrt:
                case GLSLstd450PackSnorm4x8:
                case GLSLstd450PackUnorm4x8:
                case GLSLstd450PackSnorm2x16:
                case GLSLstd450PackUnorm2x16:
                case GLSLstd450PackHalf2x16:
                case GLSLstd450PackDouble2x32:
                case GLSLstd450UnpackSnorm2x16:
                case GLSLstd450UnpackUnorm2x16:
                case GLSLstd450UnpackHalf2x16:
                case GLSLstd450UnpackSnorm4x8:
                case GLSLstd450UnpackUnorm4x8:
                case GLSLstd450UnpackDouble2x32:
                    return Treatment::Bounded;

                // `Round` too: the specification lets a compile round a half either way, and
                // `RoundEven` is one of the two.
                case GLSLstd450Round:
                case GLSLstd450Asin:
                case GLSLstd450Acos:
                case GLSLstd450FMix:
                case GLSLstd450SmoothStep:
                case GLSLstd450Fma:
                case GLSLstd450Length:
                case GLSLstd450Distance:
                case GLSLstd450Cross:
                case GLSLstd450Normalize:
                case GLSLstd450FaceForward:
                case GLSLstd450Reflect:
                case GLSLstd450Refract:
                    return Treatment::Lowered;

                case GLSLstd450Asinh:
                case GLSLstd450Acosh:
                case GLSLstd450Determinant:
                case GLSLstd450MatrixInverse:
                    return Treatment::Unpinned;

                case GLSLstd450InterpolateAtCentroid:
                case GLSLstd450InterpolateAtSample:
                case GLSLstd450InterpolateAtOffset:
                    return Treatment::Interpolation;

                default:
                    return Treatment::Unknown;
            }
        }

        /// A float instruction outside `GLSL.std.450` that nothing here pins: a derivative — a plain
        /// one fine or coarse at the compile's choice, and none of them taken by a shader here — and
        /// a float sum or product across a subgroup, a float atomic, and a cooperative matrix or
        /// vector, whose order is the hardware's.
        bool isUnpinnable(spv::Op op)
        {
            switch (op)
            {
                case spv::OpDPdx:
                case spv::OpDPdy:
                case spv::OpFwidth:
                case spv::OpDPdxFine:
                case spv::OpDPdyFine:
                case spv::OpFwidthFine:
                case spv::OpDPdxCoarse:
                case spv::OpDPdyCoarse:
                case spv::OpFwidthCoarse:
                case spv::OpGroupFAdd:
                case spv::OpGroupFAddNonUniformAMD:
                case spv::OpGroupFMulKHR:
                case spv::OpGroupNonUniformFAdd:
                case spv::OpGroupNonUniformFMul:
                case spv::OpAtomicFAddEXT:
                case spv::OpCooperativeMatrixMulAddKHR:
                case spv::OpCooperativeMatrixMulAddNV:
                case spv::OpCooperativeMatrixReduceNV:
                case spv::OpCooperativeMatrixPerElementOpNV:
                case spv::OpCooperativeVectorMatrixMulNV:
                case spv::OpCooperativeVectorMatrixMulAddNV:
                case spv::OpCooperativeVectorOuterProductAccumulateNV:
                case spv::OpCooperativeVectorReduceSumAccumulateNV:
                    return true;
                default:
                    return false;
            }
        }

        /// The float arithmetic that rounds and that `NoContraction` keeps from being fused or
        /// reordered — every one that is still there once the module is pinned.
        bool isRounding(spv::Op op)
        {
            switch (op)
            {
                case spv::OpFAdd:
                case spv::OpFSub:
                case spv::OpFMul:
                case spv::OpFDiv:
                case spv::OpVectorTimesScalar:
                case spv::OpMatrixTimesScalar:
                case spv::OpOuterProduct:
                case spv::OpFmaKHR:
                    return true;
                default:
                    return false;
            }
        }

        bool isAnnotation(spv::Op op)
        {
            switch (op)
            {
                case spv::OpDecorate:
                case spv::OpMemberDecorate:
                case spv::OpDecorationGroup:
                case spv::OpGroupDecorate:
                case spv::OpGroupMemberDecorate:
                case spv::OpDecorateId:
                case spv::OpDecorateString:
                case spv::OpMemberDecorateString:
                    return true;
                default:
                    return false;
            }
        }

        /// Whether operand `at` of an instruction is a literal and not an id, for the instructions a
        /// shader's functions hold literals in: a literal that happened to equal a multiply's id would
        /// make the multiply look read twice. Every other operand after the result counts as a read,
        /// which for an instruction this does not name can only cost a fusion.
        bool isLiteral(spv::Op op, std::size_t at)
        {
            switch (op)
            {
                case spv::OpExtInst:
                case spv::OpExtInstWithForwardRefsKHR:
                    return at == 3;
                case spv::OpCompositeExtract:
                    return at >= 3;
                case spv::OpCompositeInsert:
                case spv::OpVectorShuffle:
                    return at >= 4;
                // A selector and a default, then a literal and a label in turn.
                case spv::OpSwitch:
                    return at >= 2 && at % 2 == 0;
                case spv::OpSelectionMerge:
                    return at == 1;
                case spv::OpLoopMerge:
                case spv::OpFunction:
                case spv::OpVariable:
                    return at == 2;
                case spv::OpBranchConditional:
                    return at >= 3;
                // The memory operands: a mask and an alignment, and at most a scope no product is.
                case spv::OpLoad:
                    return at >= 3;
                case spv::OpStore:
                    return at >= 2;
                // The image operands' mask; what follows it is ids, a level of detail among them.
                case spv::OpImageSampleImplicitLod:
                case spv::OpImageSampleExplicitLod:
                case spv::OpImageFetch:
                case spv::OpImageRead:
                    return at == 4;
                case spv::OpImageWrite:
                    return at == 3;
                default:
                    return false;
            }
        }

        /// What says nothing about what a module computes: debug names and source, and lines.
        bool isDebug(spv::Op op)
        {
            switch (op)
            {
                case spv::OpSourceContinued:
                case spv::OpSource:
                case spv::OpSourceExtension:
                case spv::OpName:
                case spv::OpMemberName:
                case spv::OpString:
                case spv::OpLine:
                case spv::OpNoLine:
                case spv::OpModuleProcessed:
                    return true;
                default:
                    return false;
            }
        }

        enum class Kind : std::uint8_t
        {
            Float,
            Vector,
            Matrix,
            Bool,
            Other,
        };

        struct Type
        {
            Kind mKind = Kind::Other;

            /// A float's bits.
            std::uint32_t mWidth = 0;

            /// A vector's component type, or a matrix's column type.
            std::uint32_t mPart = 0;

            /// A vector's components, or a matrix's columns.
            std::uint32_t mCount = 0;
        };

        /// A fused multiply-add the fusion pass makes out of an add or a subtract, and what has to be
        /// made before it — a negation, and a scalar spread across a vector.
        struct Fusion
        {
            /// Where the multiply it takes in stands among the functions' instructions.
            std::size_t mMultiply;
            std::vector<Instruction> mBefore;
            Instruction mFused;
        };

        class Pinner
        {
        public:
            explicit Pinner(std::span<const std::uint32_t> words);

            std::vector<std::uint32_t> assemble();

        private:
            std::array<std::uint32_t, sHeaderWords> mHeader{};

            /// Everything before the first function, and the functions.
            std::vector<Instruction> mGlobal;
            std::vector<Instruction> mFunctions;

            std::optional<std::uint32_t> mGlsl;

            /// Whether a rewrite needed `GLSL.std.450` in a module that did not import it.
            bool mImportsGlsl = false;
            std::unordered_set<std::uint32_t> mNonSemantic;
            std::unordered_map<std::uint32_t, Type> mTypes;
            std::unordered_map<std::uint32_t, std::uint32_t> mTypeOf;

            /// The results the module decorates `NoContraction` itself: `precise` in the source.
            std::unordered_set<std::uint32_t> mDecorated;

            /// What the fusion pass leaves as it is: the source's `precise` results and every step a
            /// `precise` operation was rewritten into.
            std::unordered_set<std::uint32_t> mUnfused;

            /// Every float constant, by its type and bits, so a rewrite that needs one reuses it.
            std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> mConstants;
            std::optional<std::uint32_t> mBool;

            /// The declarations the rewrites add before the first function.
            std::vector<Instruction> mDeclared;

            /// The functions as the rewrites leave them.
            std::vector<Instruction> mOut;

            bool mFused = false;

            void read(std::span<const std::uint32_t> words);
            void survey();
            void rewrite();
            void rewriteOne(const Instruction& instruction);
            void rewriteGlsl(const Instruction& instruction);
            void fuse();
            std::optional<Fusion> fusionOf(const Instruction& add,
                const std::unordered_map<std::uint32_t, std::size_t>& definedAt,
                const std::unordered_map<std::uint32_t, std::uint32_t>& uses);

            [[noreturn]] void refuse(const Instruction& instruction, std::string_view why) const;

            std::uint32_t fresh();
            const Type& typeNamed(std::uint32_t type) const;
            std::uint32_t typeOfValue(std::uint32_t value) const;
            std::uint32_t scalarOf(std::uint32_t type) const;
            std::uint32_t floatConstant(std::uint32_t type, float value);
            std::uint32_t boolType();
            std::uint32_t glslSet();

            std::uint32_t emit(
                spv::Op op, std::uint32_t type, std::uint32_t result, std::vector<std::uint32_t> operands);
            std::uint32_t make(spv::Op op, std::uint32_t type, std::vector<std::uint32_t> operands);
            std::uint32_t glsl(std::uint32_t type, GLSLstd450 which, std::vector<std::uint32_t> operands,
                std::optional<std::uint32_t> result = std::nullopt);
            std::vector<std::uint32_t> split(std::uint32_t value);
            std::uint32_t sumOfProducts(std::span<const std::uint32_t> left, std::span<const std::uint32_t> right,
                std::optional<std::uint32_t> result = std::nullopt);
            void perComponent(std::uint32_t type, std::uint32_t result,
                const std::function<std::uint32_t(std::uint32_t component, std::uint32_t into)>& component);
            void verify() const;
        };

        Pinner::Pinner(std::span<const std::uint32_t> words)
        {
            read(words);
            survey();
            rewrite();
            fuse();
        }

        void Pinner::read(std::span<const std::uint32_t> words)
        {
            if (words.size() < sHeaderWords || words[0] != spv::MagicNumber)
                throw std::runtime_error("not a SPIR-V module in this machine's byte order");

            std::copy_n(words.begin(), sHeaderWords, mHeader.begin());

            bool inFunctions = false;
            for (std::size_t at = sHeaderWords; at < words.size();)
            {
                const std::uint32_t count = words[at] >> spv::WordCountShift;
                if (count == 0 || at + count > words.size())
                    throw std::runtime_error(std::format("the instruction at word {} runs past the module", at));

                Instruction instruction{ static_cast<spv::Op>(words[at] & spv::OpCodeMask),
                    std::vector<std::uint32_t>(words.begin() + at + 1, words.begin() + at + count) };

                // **An instruction newer than these headers is one nothing here can classify**, and
                // the float arithmetic it might hold is exactly what a newer header would name.
                if (nameOf(instruction.mOp) == "Unknown")
                    throw std::runtime_error(std::format(
                        "opcode {} is newer than the SPIR-V headers this build has, so what it computes cannot "
                        "be pinned; build against the headers that name it",
                        static_cast<std::uint32_t>(instruction.mOp)));

                inFunctions = inFunctions || instruction.mOp == spv::OpFunction;
                (inFunctions ? mFunctions : mGlobal).push_back(std::move(instruction));
                at += count;
            }
        }

        void Pinner::survey()
        {
            for (const std::vector<Instruction>* section : { &mGlobal, &mFunctions })
                for (const Instruction& instruction : *section)
                    if (const std::optional<std::uint32_t> type = resultTypeOf(instruction))
                        mTypeOf[*resultOf(instruction)] = *type;

            for (const Instruction& instruction : mGlobal)
            {
                const std::vector<std::uint32_t>& operands = instruction.mOperands;
                switch (instruction.mOp)
                {
                    case spv::OpExtInstImport:
                    {
                        const std::uint32_t set = operandAt(instruction, 0);
                        const std::string name = readString(std::span(operands).subspan(1));
                        if (name == "GLSL.std.450")
                            mGlsl = set;
                        else if (name.starts_with("NonSemantic."))
                            mNonSemantic.insert(set);
                        break;
                    }
                    case spv::OpTypeFloat:
                        mTypes[operandAt(instruction, 0)]
                            = Type{ .mKind = Kind::Float, .mWidth = operandAt(instruction, 1) };
                        break;
                    case spv::OpTypeVector:
                        mTypes[operandAt(instruction, 0)] = Type{ .mKind = Kind::Vector,
                            .mPart = operandAt(instruction, 1),
                            .mCount = operandAt(instruction, 2) };
                        break;
                    case spv::OpTypeMatrix:
                        mTypes[operandAt(instruction, 0)] = Type{ .mKind = Kind::Matrix,
                            .mPart = operandAt(instruction, 1),
                            .mCount = operandAt(instruction, 2) };
                        break;
                    case spv::OpTypeBool:
                        mTypes[operandAt(instruction, 0)] = Type{ .mKind = Kind::Bool };
                        mBool = operandAt(instruction, 0);
                        break;
                    case spv::OpConstant:
                        if (operands.size() == 3)
                            mConstants.emplace(std::pair(operands[0], operands[2]), operands[1]);
                        break;
                    case spv::OpDecorate:
                        switch (static_cast<spv::Decoration>(operandAt(instruction, 1)))
                        {
                            case spv::DecorationNoContraction:
                                mDecorated.insert(operands[0]);
                                break;
                            // **Precision the compile may lower is precision a compile may choose.**
                            case spv::DecorationRelaxedPrecision:
                                refuse(
                                    instruction, "a relaxed precision lets each compile pick how precisely to compute");
                            // An explicit environment takes precedence over `NoContraction`.
                            case spv::DecorationFPFastMathMode:
                                refuse(instruction, "a fast-math mode the module sets overrides NoContraction");
                            default:
                                break;
                        }
                        break;
                    case spv::OpExecutionMode:
                    case spv::OpExecutionModeId:
                        if (static_cast<spv::ExecutionMode>(operandAt(instruction, 1))
                            == spv::ExecutionModeFPFastMathDefault)
                            refuse(instruction, "a fast-math default the module sets overrides NoContraction");
                        break;
                    default:
                        break;
                }
            }

            mUnfused = mDecorated;
        }

        void Pinner::rewrite()
        {
            mOut.reserve(mFunctions.size());
            for (const Instruction& instruction : mFunctions)
                rewriteOne(instruction);
        }

        void Pinner::rewriteOne(const Instruction& instruction)
        {
            const auto result = [&] { return *resultOf(instruction); };
            const auto type = [&] { return *resultTypeOf(instruction); };

            if (isUnpinnable(instruction.mOp))
                refuse(instruction,
                    "what it computes is the compile's or the hardware's choice, and nothing here pins it");

            // Steps a `precise` operation is rewritten into keep its promise: none of them is fused.
            const std::size_t first = mOut.size();
            const auto keepPrecise = [&] {
                if (!mUnfused.contains(result()))
                    return;
                for (std::size_t at = first; at < mOut.size(); ++at)
                    if (const std::optional<std::uint32_t> made = resultOf(mOut[at]))
                        mUnfused.insert(*made);
            };

            switch (instruction.mOp)
            {
                case spv::OpDot:
                {
                    const std::vector<std::uint32_t> left = split(operandAt(instruction, 2));
                    const std::vector<std::uint32_t> right = split(operandAt(instruction, 3));
                    sumOfProducts(left, right, result());
                    keepPrecise();
                    return;
                }
                case spv::OpMatrixTimesVector:
                {
                    const std::uint32_t matrix = operandAt(instruction, 2);
                    const Type& shape = typeNamed(typeOfValue(matrix));
                    const std::vector<std::uint32_t> vector = split(operandAt(instruction, 3));
                    perComponent(type(), result(), [&](std::uint32_t row, std::uint32_t into) {
                        std::vector<std::uint32_t> elements;
                        for (std::uint32_t column = 0; column < shape.mCount; ++column)
                            elements.push_back(
                                make(spv::OpCompositeExtract, scalarOf(shape.mPart), { matrix, column, row }));
                        return sumOfProducts(elements, vector, into);
                    });
                    keepPrecise();
                    return;
                }
                case spv::OpVectorTimesMatrix:
                {
                    const std::vector<std::uint32_t> vector = split(operandAt(instruction, 2));
                    const std::uint32_t matrix = operandAt(instruction, 3);
                    const Type& shape = typeNamed(typeOfValue(matrix));
                    perComponent(type(), result(), [&](std::uint32_t column, std::uint32_t into) {
                        const std::vector<std::uint32_t> elements
                            = split(make(spv::OpCompositeExtract, shape.mPart, { matrix, column }));
                        return sumOfProducts(vector, elements, into);
                    });
                    keepPrecise();
                    return;
                }
                case spv::OpMatrixTimesMatrix:
                {
                    const std::uint32_t left = operandAt(instruction, 2);
                    const std::uint32_t right = operandAt(instruction, 3);
                    const Type& leftShape = typeNamed(typeOfValue(left));
                    const Type& rightShape = typeNamed(typeOfValue(right));
                    const Type& product = typeNamed(type());
                    std::vector<std::uint32_t> columns;
                    for (std::uint32_t column = 0; column < product.mCount; ++column)
                    {
                        const std::vector<std::uint32_t> vector
                            = split(make(spv::OpCompositeExtract, rightShape.mPart, { right, column }));
                        std::vector<std::uint32_t> rows;
                        for (std::uint32_t row = 0; row < typeNamed(product.mPart).mCount; ++row)
                        {
                            std::vector<std::uint32_t> elements;
                            for (std::uint32_t inner = 0; inner < leftShape.mCount; ++inner)
                                elements.push_back(
                                    make(spv::OpCompositeExtract, scalarOf(leftShape.mPart), { left, inner, row }));
                            rows.push_back(sumOfProducts(elements, vector));
                        }
                        columns.push_back(make(spv::OpCompositeConstruct, product.mPart, rows));
                    }
                    emit(spv::OpCompositeConstruct, type(), result(), columns);
                    keepPrecise();
                    return;
                }
                case spv::OpFMod:
                case spv::OpFRem:
                {
                    const std::uint32_t x = operandAt(instruction, 2);
                    const std::uint32_t y = operandAt(instruction, 3);
                    const std::uint32_t whole
                        = glsl(type(), instruction.mOp == spv::OpFMod ? GLSLstd450Floor : GLSLstd450Trunc,
                            { make(spv::OpFDiv, type(), { x, y }) });
                    emit(spv::OpFSub, type(), result(), { x, make(spv::OpFMul, type(), { y, whole }) });
                    keepPrecise();
                    return;
                }
                case spv::OpExtInst:
                {
                    const std::uint32_t set = operandAt(instruction, 2);
                    if (mNonSemantic.contains(set))
                        break;
                    if (!mGlsl.has_value() || set != *mGlsl)
                        refuse(instruction, "its extended instruction set is neither GLSL.std.450 nor non-semantic");
                    rewriteGlsl(instruction);
                    keepPrecise();
                    return;
                }
                default:
                    break;
            }

            mOut.push_back(instruction);
        }

        void Pinner::rewriteGlsl(const Instruction& instruction)
        {
            const std::uint32_t type = *resultTypeOf(instruction);
            const std::uint32_t result = *resultOf(instruction);
            const std::uint32_t which = operandAt(instruction, 3);
            const auto argument = [&](std::size_t index) { return operandAt(instruction, 4 + index); };

            switch (treatmentOf(which))
            {
                case Treatment::Pinned:
                case Treatment::Bounded:
                    mOut.push_back(instruction);
                    return;
                case Treatment::Unknown:
                    refuse(instruction, std::format("GLSL.std.450 instruction {} is not one the set defines", which));
                case Treatment::Unpinned:
                    refuse(instruction,
                        "it is inherited from a formula whose order the compile chooses and nothing here rewrites "
                        "it yet; write it out in the shader or teach Rtx::pinFloatArithmetic its order");
                case Treatment::Interpolation:
                    refuse(instruction, "interpolation is the rasterizer's and nothing here pins it");
                case Treatment::Lowered:
                    break;
            }

            const std::uint32_t scalar = scalarOf(type);
            switch (static_cast<GLSLstd450>(which))
            {
                case GLSLstd450Round:
                    glsl(type, GLSLstd450RoundEven, { argument(0) }, result);
                    return;
                case GLSLstd450Fma:
                    // The device fuses 32-bit floats and nothing narrower or wider is asked of it.
                    if (typeNamed(scalar).mWidth != 32)
                        refuse(instruction, "only a 32-bit fma is one the device is asked to round once");
                    emit(spv::OpFmaKHR, type, result, { argument(0), argument(1), argument(2) });
                    mFused = true;
                    return;
                case GLSLstd450Length:
                {
                    if (typeNamed(typeOfValue(argument(0))).mKind == Kind::Float)
                    {
                        glsl(type, GLSLstd450FAbs, { argument(0) }, result);
                        return;
                    }
                    const std::vector<std::uint32_t> parts = split(argument(0));
                    glsl(type, GLSLstd450Sqrt, { sumOfProducts(parts, parts) }, result);
                    return;
                }
                case GLSLstd450Distance:
                {
                    const std::vector<std::uint32_t> left = split(argument(0));
                    const std::vector<std::uint32_t> right = split(argument(1));
                    std::vector<std::uint32_t> apart;
                    for (std::size_t at = 0; at < left.size(); ++at)
                        apart.push_back(make(spv::OpFSub, scalar, { left[at], right[at] }));
                    if (apart.size() == 1)
                        glsl(type, GLSLstd450FAbs, { apart[0] }, result);
                    else
                        glsl(type, GLSLstd450Sqrt, { sumOfProducts(apart, apart) }, result);
                    return;
                }
                case GLSLstd450Normalize:
                {
                    if (typeNamed(type).mKind == Kind::Float)
                    {
                        glsl(type, GLSLstd450FSign, { argument(0) }, result);
                        return;
                    }
                    const std::vector<std::uint32_t> parts = split(argument(0));
                    const std::uint32_t scale = glsl(scalar, GLSLstd450InverseSqrt, { sumOfProducts(parts, parts) });
                    perComponent(type, result, [&](std::uint32_t at, std::uint32_t into) {
                        return emit(spv::OpFMul, scalar, into, { parts[at], scale });
                    });
                    return;
                }
                case GLSLstd450Cross:
                {
                    const std::vector<std::uint32_t> a = split(argument(0));
                    const std::vector<std::uint32_t> b = split(argument(1));
                    perComponent(type, result, [&](std::uint32_t at, std::uint32_t into) {
                        const std::uint32_t i = (at + 1) % 3;
                        const std::uint32_t j = (at + 2) % 3;
                        return emit(spv::OpFSub, scalar, into,
                            { make(spv::OpFMul, scalar, { a[i], b[j] }), make(spv::OpFMul, scalar, { a[j], b[i] }) });
                    });
                    return;
                }
                case GLSLstd450FMix:
                {
                    const std::vector<std::uint32_t> x = split(argument(0));
                    const std::vector<std::uint32_t> y = split(argument(1));
                    const std::vector<std::uint32_t> a = split(argument(2));
                    const std::uint32_t one = floatConstant(scalar, 1.0f);
                    perComponent(type, result, [&](std::uint32_t at, std::uint32_t into) {
                        const std::uint32_t kept
                            = make(spv::OpFMul, scalar, { x[at], make(spv::OpFSub, scalar, { one, a[at] }) });
                        return emit(spv::OpFAdd, scalar, into, { kept, make(spv::OpFMul, scalar, { y[at], a[at] }) });
                    });
                    return;
                }
                case GLSLstd450SmoothStep:
                {
                    const std::vector<std::uint32_t> low = split(argument(0));
                    const std::vector<std::uint32_t> high = split(argument(1));
                    const std::vector<std::uint32_t> x = split(argument(2));
                    const std::uint32_t zero = floatConstant(scalar, 0.0f);
                    const std::uint32_t one = floatConstant(scalar, 1.0f);
                    const std::uint32_t two = floatConstant(scalar, 2.0f);
                    const std::uint32_t three = floatConstant(scalar, 3.0f);
                    perComponent(type, result, [&](std::uint32_t at, std::uint32_t into) {
                        const std::uint32_t along = make(spv::OpFDiv, scalar,
                            { make(spv::OpFSub, scalar, { x[at], low[at] }),
                                make(spv::OpFSub, scalar, { high[at], low[at] }) });
                        const std::uint32_t t = glsl(scalar, GLSLstd450FClamp, { along, zero, one });
                        const std::uint32_t shape
                            = make(spv::OpFSub, scalar, { three, make(spv::OpFMul, scalar, { two, t }) });
                        return emit(spv::OpFMul, scalar, into, { make(spv::OpFMul, scalar, { t, t }), shape });
                    });
                    return;
                }
                case GLSLstd450Reflect:
                {
                    const std::vector<std::uint32_t> incident = split(argument(0));
                    const std::vector<std::uint32_t> normal = split(argument(1));
                    const std::uint32_t twice
                        = make(spv::OpFMul, scalar, { floatConstant(scalar, 2.0f), sumOfProducts(normal, incident) });
                    perComponent(type, result, [&](std::uint32_t at, std::uint32_t into) {
                        return emit(spv::OpFSub, scalar, into,
                            { incident[at], make(spv::OpFMul, scalar, { twice, normal[at] }) });
                    });
                    return;
                }
                case GLSLstd450Refract:
                {
                    const std::vector<std::uint32_t> incident = split(argument(0));
                    const std::vector<std::uint32_t> normal = split(argument(1));
                    const std::uint32_t eta = argument(2);
                    const std::uint32_t zero = floatConstant(scalar, 0.0f);
                    const std::uint32_t one = floatConstant(scalar, 1.0f);
                    const std::uint32_t cosine = sumOfProducts(normal, incident);
                    const std::uint32_t across
                        = make(spv::OpFSub, scalar, { one, make(spv::OpFMul, scalar, { cosine, cosine }) });
                    const std::uint32_t k = make(spv::OpFSub, scalar,
                        { one, make(spv::OpFMul, scalar, { make(spv::OpFMul, scalar, { eta, eta }), across }) });
                    const std::uint32_t inside = make(spv::OpFOrdLessThan, boolType(), { k, zero });
                    const std::uint32_t root
                        = glsl(scalar, GLSLstd450Sqrt, { glsl(scalar, GLSLstd450FMax, { k, zero }) });
                    const std::uint32_t bend
                        = make(spv::OpFAdd, scalar, { make(spv::OpFMul, scalar, { eta, cosine }), root });
                    perComponent(type, result, [&](std::uint32_t at, std::uint32_t into) {
                        const std::uint32_t refracted = make(spv::OpFSub, scalar,
                            { make(spv::OpFMul, scalar, { eta, incident[at] }),
                                make(spv::OpFMul, scalar, { bend, normal[at] }) });
                        return emit(spv::OpSelect, scalar, into, { inside, zero, refracted });
                    });
                    return;
                }
                case GLSLstd450FaceForward:
                {
                    const std::vector<std::uint32_t> normal = split(argument(0));
                    const std::vector<std::uint32_t> incident = split(argument(1));
                    const std::vector<std::uint32_t> reference = split(argument(2));
                    // One at a time: the order a call's arguments are evaluated in is the compiler's,
                    // and each of these may number something new.
                    const std::uint32_t truth = boolType();
                    const std::uint32_t cosine = sumOfProducts(reference, incident);
                    const std::uint32_t facing
                        = make(spv::OpFOrdLessThan, truth, { cosine, floatConstant(scalar, 0.0f) });
                    perComponent(type, result, [&](std::uint32_t at, std::uint32_t into) {
                        return emit(spv::OpSelect, scalar, into,
                            { facing, normal[at], make(spv::OpFNegate, scalar, { normal[at] }) });
                    });
                    return;
                }
                case GLSLstd450Asin:
                case GLSLstd450Acos:
                {
                    const std::vector<std::uint32_t> x = split(argument(0));
                    const std::uint32_t one = floatConstant(scalar, 1.0f);
                    const bool sine = which == GLSLstd450Asin;
                    perComponent(type, result, [&](std::uint32_t at, std::uint32_t into) {
                        const std::uint32_t other = glsl(scalar, GLSLstd450Sqrt,
                            { make(spv::OpFSub, scalar, { one, make(spv::OpFMul, scalar, { x[at], x[at] }) }) });
                        return glsl(scalar, GLSLstd450Atan2, { sine ? x[at] : other, sine ? other : x[at] }, into);
                    });
                    return;
                }
                default:
                    break;
            }

            // A treatment table that says "lowered" for an instruction nothing lowers is this file's
            // own mistake.
            throw std::logic_error(std::format("GLSL.std.450 instruction {} is lowered and has no rewrite", which));
        }

        void Pinner::fuse()
        {
            std::unordered_map<std::uint32_t, std::uint32_t> uses;
            std::unordered_map<std::uint32_t, std::uint32_t> debugUses;
            std::unordered_set<std::uint32_t> mentioned;
            const auto count = [&](const Instruction& instruction) {
                if (isAnnotation(instruction.mOp) || isDebug(instruction.mOp))
                {
                    if (!instruction.mOperands.empty())
                        mentioned.insert(instruction.mOperands[0]);
                    return;
                }
                const bool debug
                    = (instruction.mOp == spv::OpExtInst || instruction.mOp == spv::OpExtInstWithForwardRefsKHR)
                    && mNonSemantic.contains(operandAt(instruction, 2));
                const Layout layout = layoutOf(instruction.mOp);
                const std::size_t from
                    = static_cast<std::size_t>(layout.mType) + static_cast<std::size_t>(layout.mResult);
                for (std::size_t at = from; at < instruction.mOperands.size(); ++at)
                    if (!isLiteral(instruction.mOp, at))
                        ++(debug ? debugUses : uses)[instruction.mOperands[at]];
            };
            for (const Instruction& instruction : mGlobal)
                count(instruction);
            for (const Instruction& instruction : mOut)
                count(instruction);

            std::unordered_map<std::uint32_t, std::size_t> definedAt;
            for (std::size_t at = 0; at < mOut.size(); ++at)
                if (const std::optional<std::uint32_t> result = resultOf(mOut[at]))
                    definedAt[*result] = at;

            std::unordered_map<std::size_t, Fusion> fusions;
            std::unordered_set<std::size_t> dropped;
            for (std::size_t at = 0; at < mOut.size(); ++at)
                if (std::optional<Fusion> fusion = fusionOf(mOut[at], definedAt, uses))
                {
                    // Kept where a name or a debug instruction still mentions it, which the stripped
                    // module then holds unread and the driver takes out.
                    const std::uint32_t multiply = *resultOf(mOut[fusion->mMultiply]);
                    if (debugUses[multiply] == 0 && !mentioned.contains(multiply))
                        dropped.insert(fusion->mMultiply);
                    fusions.emplace(at, std::move(*fusion));
                }

            if (fusions.empty())
                return;
            mFused = true;

            std::vector<Instruction> fused;
            fused.reserve(mOut.size());
            for (std::size_t at = 0; at < mOut.size(); ++at)
            {
                if (dropped.contains(at))
                    continue;
                const auto found = fusions.find(at);
                if (found == fusions.end())
                {
                    fused.push_back(std::move(mOut[at]));
                    continue;
                }
                for (Instruction& before : found->second.mBefore)
                    fused.push_back(std::move(before));
                fused.push_back(std::move(found->second.mFused));
            }
            mOut = std::move(fused);
        }

        std::optional<Fusion> Pinner::fusionOf(const Instruction& add,
            const std::unordered_map<std::uint32_t, std::size_t>& definedAt,
            const std::unordered_map<std::uint32_t, std::uint32_t>& uses)
        {
            if (add.mOp != spv::OpFAdd && add.mOp != spv::OpFSub)
                return std::nullopt;

            const std::uint32_t type = *resultTypeOf(add);
            const std::uint32_t result = *resultOf(add);
            if (mUnfused.contains(result) || typeNamed(scalarOf(type)).mWidth != 32)
                return std::nullopt;

            // A product this add is the only reader of, and that is no `precise` result itself.
            const auto product = [&](std::uint32_t value) -> const Instruction* {
                const auto found = definedAt.find(value);
                const auto read = uses.find(value);
                if (found == definedAt.end() || read == uses.end() || read->second != 1 || mUnfused.contains(value))
                    return nullptr;
                const Instruction& made = mOut[found->second];
                return made.mOp == spv::OpFMul || made.mOp == spv::OpVectorTimesScalar ? &made : nullptr;
            };

            // The right-hand product first, so a running sum stays the sum: `s + a b` is
            // `fma(a, b, s)` whatever `s` was made of.
            const std::uint32_t x = operandAt(add, 2);
            const std::uint32_t y = operandAt(add, 3);
            const Instruction* multiply = product(y);
            const bool right = multiply != nullptr;
            if (!right)
                multiply = product(x);
            if (multiply == nullptr)
                return std::nullopt;

            Fusion fusion{ .mMultiply = definedAt.at(*resultOf(*multiply)), .mBefore = {}, .mFused = {} };
            std::uint32_t a = operandAt(*multiply, 2);
            std::uint32_t b = operandAt(*multiply, 3);
            std::uint32_t c = right ? x : y;

            const auto before = [&](spv::Op op, std::vector<std::uint32_t> operands) {
                const std::uint32_t made = fresh();
                Instruction instruction{ op, { type, made } };
                instruction.mOperands.insert(instruction.mOperands.end(), operands.begin(), operands.end());
                fusion.mBefore.push_back(std::move(instruction));
                mTypeOf[made] = type;
                return made;
            };

            // A vector scaled by a scalar is the scalar spread across the vector, which `OpFmaKHR`
            // needs its three operands to be.
            if (multiply->mOp == spv::OpVectorTimesScalar)
                b = before(spv::OpCompositeConstruct, std::vector<std::uint32_t>(typeNamed(type).mCount, b));

            // Negation is exact, so `c - a b` is `fma(-a, b, c)` and `a b - c` is `fma(a, b, -c)`.
            if (add.mOp == spv::OpFSub)
            {
                if (right)
                    a = before(spv::OpFNegate, { a });
                else
                    c = before(spv::OpFNegate, { c });
            }

            fusion.mFused = Instruction{ spv::OpFmaKHR, { type, result, a, b, c } };
            return fusion;
        }

        std::vector<std::uint32_t> Pinner::assemble()
        {
            // Every rounding step that is left, `NoContraction`, beside whatever the source decorated.
            std::vector<Instruction> decorations;
            for (const Instruction& instruction : mOut)
                if (isRounding(instruction.mOp))
                    if (const std::uint32_t result = *resultOf(instruction); !mDecorated.contains(result))
                        decorations.push_back(Instruction{ spv::OpDecorate, { result, spv::DecorationNoContraction } });

            std::vector<Instruction> global;
            global.reserve(mGlobal.size() + mDeclared.size() + decorations.size() + 3);

            const auto lastOf = [&](const auto& matches) -> std::optional<std::size_t> {
                std::optional<std::size_t> last;
                for (std::size_t at = 0; at < mGlobal.size(); ++at)
                    if (matches(mGlobal[at].mOp))
                        last = at;
                return last;
            };

            const bool hasCapability = std::any_of(mGlobal.begin(), mGlobal.end(), [](const Instruction& instruction) {
                return instruction.mOp == spv::OpCapability && instruction.mOperands.at(0) == spv::CapabilityFMAKHR;
            });
            const bool hasExtension = std::any_of(mGlobal.begin(), mGlobal.end(), [](const Instruction& instruction) {
                return instruction.mOp == spv::OpExtension && readString(instruction.mOperands) == "SPV_KHR_fma";
            });

            // Where each addition goes: a capability among the capabilities, an extension after
            // them and the other extensions, the decorations among the annotations or before the
            // first type where there are none, and the declarations last before the functions.
            const std::optional<std::size_t> lastCapability
                = lastOf([](spv::Op op) { return op == spv::OpCapability; });
            const std::optional<std::size_t> lastExtension = lastOf([](spv::Op op) { return op == spv::OpExtension; });
            const std::optional<std::size_t> lastAnnotation = lastOf(isAnnotation);
            const std::optional<std::size_t> lastImport = lastOf([](spv::Op op) { return op == spv::OpExtInstImport; });
            std::size_t firstType = mGlobal.size();
            for (std::size_t at = 0; at < mGlobal.size(); ++at)
                if (nameOf(mGlobal[at].mOp).starts_with("OpType"))
                {
                    firstType = at;
                    break;
                }

            const std::size_t extensionAfter = lastExtension.value_or(lastCapability.value_or(0));
            const std::size_t importAfter = lastImport.value_or(extensionAfter);
            for (std::size_t at = 0; at < mGlobal.size(); ++at)
            {
                if (at == firstType && !lastAnnotation.has_value())
                    global.insert(global.end(), decorations.begin(), decorations.end());

                global.push_back(std::move(mGlobal[at]));

                if (mFused && !hasCapability && lastCapability == at)
                    global.push_back(Instruction{ spv::OpCapability, { spv::CapabilityFMAKHR } });
                if (mFused && !hasExtension && at == extensionAfter)
                    global.push_back(Instruction{ spv::OpExtension, spell("SPV_KHR_fma") });
                if (mImportsGlsl && at == importAfter)
                {
                    Instruction import{ spv::OpExtInstImport, { *mGlsl } };
                    const std::vector<std::uint32_t> name = spell("GLSL.std.450");
                    import.mOperands.insert(import.mOperands.end(), name.begin(), name.end());
                    global.push_back(std::move(import));
                }
                if (lastAnnotation == at)
                    global.insert(global.end(), decorations.begin(), decorations.end());
            }
            global.insert(global.end(), mDeclared.begin(), mDeclared.end());

            verify();

            std::vector<std::uint32_t> words(mHeader.begin(), mHeader.end());
            const auto write = [&](const Instruction& instruction) {
                words.push_back(static_cast<std::uint32_t>(instruction.mOperands.size() + 1) << spv::WordCountShift
                    | static_cast<std::uint32_t>(instruction.mOp));
                words.insert(words.end(), instruction.mOperands.begin(), instruction.mOperands.end());
            };
            for (const Instruction& instruction : global)
                write(instruction);
            for (const Instruction& instruction : mOut)
                write(instruction);
            return words;
        }

        void Pinner::verify() const
        {
            // What the rewrites exist to remove, found in what they left, is this file's mistake
            // and not the module's.
            for (const Instruction& instruction : mOut)
            {
                bool left = false;
                switch (instruction.mOp)
                {
                    case spv::OpDot:
                    case spv::OpMatrixTimesVector:
                    case spv::OpVectorTimesMatrix:
                    case spv::OpMatrixTimesMatrix:
                    case spv::OpFMod:
                    case spv::OpFRem:
                        left = true;
                        break;
                    case spv::OpExtInst:
                        left = mGlsl.has_value() && operandAt(instruction, 2) == *mGlsl
                            && treatmentOf(operandAt(instruction, 3)) == Treatment::Lowered;
                        break;
                    default:
                        break;
                }
                if (left)
                    throw std::logic_error(
                        std::format("{} is still there once the module is pinned", nameOf(instruction.mOp)));
            }
        }

        void Pinner::refuse(const Instruction& instruction, std::string_view why) const
        {
            const std::optional<std::uint32_t> result = resultOf(instruction);
            std::string what(nameOf(instruction.mOp));
            if (instruction.mOp == spv::OpExtInst && instruction.mOperands.size() > 3)
                what += std::format(" {}", instruction.mOperands[3]);
            if (result.has_value())
                what += std::format(" %{}", *result);
            throw std::runtime_error(std::format("{} cannot be pinned: {}", what, why));
        }

        std::uint32_t Pinner::fresh()
        {
            return mHeader[3]++;
        }

        const Type& Pinner::typeNamed(std::uint32_t type) const
        {
            const auto found = mTypes.find(type);
            if (found == mTypes.end())
                throw std::runtime_error(
                    std::format("%{} is used as a float, vector or matrix type and is none", type));
            return found->second;
        }

        std::uint32_t Pinner::typeOfValue(std::uint32_t value) const
        {
            const auto found = mTypeOf.find(value);
            if (found == mTypeOf.end())
                throw std::runtime_error(std::format("%{} is read and never made", value));
            return found->second;
        }

        std::uint32_t Pinner::scalarOf(std::uint32_t type) const
        {
            const Type& shape = typeNamed(type);
            switch (shape.mKind)
            {
                case Kind::Float:
                    return type;
                case Kind::Vector:
                    return scalarOf(shape.mPart);
                case Kind::Matrix:
                    return scalarOf(shape.mPart);
                default:
                    throw std::runtime_error(std::format("%{} is no float, vector or matrix type", type));
            }
        }

        std::uint32_t Pinner::floatConstant(std::uint32_t type, float value)
        {
            if (typeNamed(type).mWidth != 32)
                throw std::runtime_error(
                    std::format("a rewrite needs a constant of the {}-bit float %{}, and only 32 bits are pinned",
                        typeNamed(type).mWidth, type));

            const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
            const auto found = mConstants.find(std::pair(type, bits));
            if (found != mConstants.end())
                return found->second;

            const std::uint32_t made = fresh();
            mDeclared.push_back(Instruction{ spv::OpConstant, { type, made, bits } });
            mConstants.emplace(std::pair(type, bits), made);
            mTypeOf[made] = type;
            return made;
        }

        std::uint32_t Pinner::boolType()
        {
            if (!mBool.has_value())
            {
                mBool = fresh();
                mDeclared.push_back(Instruction{ spv::OpTypeBool, { *mBool } });
                mTypes[*mBool] = Type{ .mKind = Kind::Bool };
            }
            return *mBool;
        }

        std::uint32_t Pinner::glslSet()
        {
            if (!mGlsl.has_value())
            {
                mGlsl = fresh();
                mImportsGlsl = true;
            }
            return *mGlsl;
        }

        std::uint32_t Pinner::emit(
            spv::Op op, std::uint32_t type, std::uint32_t result, std::vector<std::uint32_t> operands)
        {
            Instruction instruction{ op, { type, result } };
            instruction.mOperands.insert(instruction.mOperands.end(), operands.begin(), operands.end());
            mOut.push_back(std::move(instruction));
            mTypeOf[result] = type;
            return result;
        }

        std::uint32_t Pinner::make(spv::Op op, std::uint32_t type, std::vector<std::uint32_t> operands)
        {
            return emit(op, type, fresh(), std::move(operands));
        }

        std::uint32_t Pinner::glsl(std::uint32_t type, GLSLstd450 which, std::vector<std::uint32_t> operands,
            std::optional<std::uint32_t> result)
        {
            operands.insert(operands.begin(), { glslSet(), static_cast<std::uint32_t>(which) });
            return emit(spv::OpExtInst, type, result.has_value() ? *result : fresh(), std::move(operands));
        }

        std::vector<std::uint32_t> Pinner::split(std::uint32_t value)
        {
            const std::uint32_t type = typeOfValue(value);
            const Type& shape = typeNamed(type);
            if (shape.mKind == Kind::Float)
                return { value };
            if (shape.mKind != Kind::Vector)
                throw std::runtime_error(std::format("%{} is read as a float vector and is not one", value));

            std::vector<std::uint32_t> parts;
            for (std::uint32_t at = 0; at < shape.mCount; ++at)
                parts.push_back(make(spv::OpCompositeExtract, shape.mPart, { value, at }));
            return parts;
        }

        std::uint32_t Pinner::sumOfProducts(std::span<const std::uint32_t> left, std::span<const std::uint32_t> right,
            std::optional<std::uint32_t> result)
        {
            const std::uint32_t scalar = typeOfValue(left[0]);
            if (typeNamed(scalar).mWidth != 32)
                throw std::runtime_error("a rewrite of a sum of products over a float that is not 32 bits wide");

            if (left.size() == 1)
                return emit(spv::OpFMul, scalar, result.has_value() ? *result : fresh(), { left[0], right[0] });

            std::uint32_t sum = make(spv::OpFMul, scalar, { left[0], right[0] });
            for (std::size_t at = 1; at < left.size(); ++at)
            {
                const std::uint32_t product = make(spv::OpFMul, scalar, { left[at], right[at] });
                const bool last = at + 1 == left.size();
                sum = emit(spv::OpFAdd, scalar, last && result.has_value() ? *result : fresh(), { sum, product });
            }
            return sum;
        }

        void Pinner::perComponent(std::uint32_t type, std::uint32_t result,
            const std::function<std::uint32_t(std::uint32_t component, std::uint32_t into)>& component)
        {
            const Type& shape = typeNamed(type);
            if (shape.mKind == Kind::Float)
            {
                component(0, result);
                return;
            }

            std::vector<std::uint32_t> parts;
            for (std::uint32_t at = 0; at < shape.mCount; ++at)
                parts.push_back(component(at, fresh()));
            emit(spv::OpCompositeConstruct, type, result, parts);
        }
    }

    std::vector<std::uint32_t> pinFloatArithmetic(std::span<const std::uint32_t> module)
    {
        return Pinner(module).assemble();
    }
}
