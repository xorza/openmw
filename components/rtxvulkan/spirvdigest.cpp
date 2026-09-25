#include "spirvdigest.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <smhasher/MurmurHash3.h>

namespace Rtx
{
    namespace
    {
        using Hash = std::array<std::uint64_t, 2>;

        struct Line
        {
            /// Empty for an instruction with no result.
            std::string_view mResult;
            std::string_view mOp;
            std::vector<std::string_view> mOperands;
        };

        bool isId(std::string_view token)
        {
            return token.size() > 1 && token.front() == '%';
        }

        bool isDecoration(std::string_view op)
        {
            return op == "OpDecorate" || op == "OpDecorateId" || op == "OpDecorateString" || op == "OpMemberDecorate"
                || op == "OpMemberDecorateString";
        }

        bool isDebug(std::string_view op)
        {
            return op == "OpSource" || op == "OpSourceContinued" || op == "OpSourceExtension" || op == "OpString"
                || op == "OpName" || op == "OpMemberName" || op == "OpLine" || op == "OpNoLine"
                || op == "OpModuleProcessed";
        }

        /// A line's tokens: split at the spaces outside a quoted string, whose backslash escapes are
        /// kept as the disassembler wrote them.
        std::vector<std::string_view> tokensOf(std::string_view line)
        {
            std::vector<std::string_view> tokens;
            std::size_t at = 0;
            while (at < line.size())
            {
                if (line[at] == ' ')
                {
                    ++at;
                    continue;
                }

                const std::size_t start = at;
                bool quoted = false;
                while (at < line.size() && (quoted || line[at] != ' '))
                {
                    if (quoted && line[at] == '\\')
                        at += 2;
                    else
                    {
                        if (line[at] == '"')
                            quoted = !quoted;
                        ++at;
                    }
                }

                if (quoted || at > line.size())
                    throw std::runtime_error("a string runs past the end of: " + std::string(line));
                tokens.push_back(line.substr(start, at - start));
            }
            return tokens;
        }

        std::vector<Line> linesOf(std::string_view text)
        {
            std::vector<Line> lines;
            while (!text.empty())
            {
                const std::size_t end = text.find('\n');
                std::string_view row = text.substr(0, end);
                text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);

                while (!row.empty() && (row.front() == ' ' || row.front() == '\t'))
                    row.remove_prefix(1);
                while (!row.empty() && (row.back() == ' ' || row.back() == '\t' || row.back() == '\r'))
                    row.remove_suffix(1);
                if (row.empty() || row.front() == ';')
                    continue;

                const std::vector<std::string_view> tokens = tokensOf(row);
                Line line;
                std::size_t first = 0;
                if (tokens.size() >= 3 && tokens[1] == "=")
                {
                    line.mResult = tokens[0];
                    first = 2;
                }

                line.mOp = tokens[first];
                if (!line.mOp.starts_with("Op") || (!line.mResult.empty() && !isId(line.mResult)))
                    throw std::runtime_error("cannot read the line: " + std::string(row));

                line.mOperands.assign(tokens.begin() + static_cast<std::ptrdiff_t>(first) + 1, tokens.end());
                if (!isDebug(line.mOp))
                    lines.push_back(std::move(line));
            }
            return lines;
        }

        /// The bytes a digest is taken of. **Every part says where it ends** — a literal by its
        /// length, a digest by its fixed size, a local by its terminator — so no two sequences of
        /// parts write one byte string.
        class Canon
        {
        public:
            void literal(std::string_view token)
            {
                mBytes += '#';
                mBytes += std::to_string(token.size());
                mBytes += ':';
                mBytes += token;
            }

            void digest(char mark, const Hash& part)
            {
                mBytes += mark;
                mBytes.append(reinterpret_cast<const char*>(part.data()), sizeof(Hash));
            }

            void number(char mark, std::size_t value)
            {
                mBytes += mark;
                mBytes += std::to_string(value);
                mBytes += ';';
            }

            void end() { mBytes += '\n'; }

            Hash hash() const
            {
                constexpr std::array<std::uint64_t, 2> seed{};
                Hash out{};
                MurmurHash3_x64_128(mBytes.data(), static_cast<int>(mBytes.size()), seed.data(), out.data());
                return out;
            }

        private:
            std::string mBytes;
        };

        class Program
        {
        public:
            explicit Program(std::string_view disassembly)
                : mLines(linesOf(disassembly))
            {
                std::size_t at = 0;
                for (; at < mLines.size() && mLines[at].mOp != "OpFunction"; ++at)
                {
                    const Line& line = mLines[at];
                    if (isDecoration(line.mOp))
                    {
                        if (line.mOperands.empty() || !isId(line.mOperands[0]))
                            throw std::runtime_error("a decoration names no target");
                        mDecorations[line.mOperands[0]].push_back(&line);
                    }
                    // What it declares is the `OpTypePointer` that defines it.
                    else if (line.mOp == "OpTypeForwardPointer")
                        continue;
                    else if (!line.mResult.empty())
                    {
                        if (!mGlobalAt.emplace(line.mResult, mGlobals.size()).second)
                            throw std::runtime_error(std::string(line.mResult) + " is defined twice");
                        mGlobals.push_back(&line);
                    }
                    else
                        mFixed.push_back(&line);
                }

                for (; at < mLines.size(); ++at)
                {
                    if (mLines[at].mOp == "OpFunction")
                    {
                        if (!mFunctionAt.emplace(mLines[at].mResult, mFunctions.size()).second)
                            throw std::runtime_error(std::string(mLines[at].mResult) + " is defined twice");
                        mFunctions.push_back(Function{ .mBegin = at, .mEnd = at });
                    }
                    if (mFunctions.empty())
                        throw std::runtime_error(std::string(mLines[at].mOp) + " stands outside every function");
                    mFunctions.back().mEnd = at + 1;
                }

                if (mFunctions.empty())
                    throw std::runtime_error("the module defines no function");
                mFunctionDigests.resize(mFunctions.size());

                std::unordered_set<std::string_view> locals;
                for (const Function& function : mFunctions)
                    for (std::size_t line = function.mBegin; line < function.mEnd; ++line)
                        if (!mLines[line].mResult.empty())
                            locals.insert(mLines[line].mResult);
                for (const auto& [target, decorations] : mDecorations)
                    if (!mGlobalAt.contains(target) && !locals.contains(target))
                        throw std::runtime_error(
                            "a decoration names " + std::string(target) + ", which nothing defines");
            }

            Hash digest()
            {
                refine();

                // **From the entry points down, and in the order each names its callees**, because
                // the order a program first names the globals the refinement left alike is part of
                // what it is named by (`nameOutside`), and that order is the program's own only when
                // it is walked from where the program starts. What no entry point reaches follows in
                // the module's order.
                std::vector<const Line*> entries;
                for (const Line* line : mFixed)
                    if (line->mOp == "OpEntryPoint")
                    {
                        if (line->mOperands.size() < 3)
                            throw std::runtime_error("an entry point names no function");
                        entries.push_back(line);
                    }
                std::ranges::sort(
                    entries, {}, [](const Line* line) { return std::pair(line->mOperands[0], line->mOperands[2]); });
                for (const Line* entry : entries)
                    functionDigest(functionAt(entry->mOperands[1]));

                std::vector<Hash> functions;
                functions.reserve(mFunctions.size());
                for (std::size_t function = 0; function < mFunctions.size(); ++function)
                    functions.push_back(functionDigest(function));

                std::vector<Hash> fixed;
                fixed.reserve(mFixed.size());
                for (const Line* line : mFixed)
                    fixed.push_back(fixedDigest(*line));

                std::vector<Hash> globals = mColours;
                std::ranges::sort(fixed);
                std::ranges::sort(globals);
                std::ranges::sort(functions);

                Canon canon;
                for (const Hash& part : fixed)
                    canon.digest('X', part);
                canon.end();
                for (const Hash& part : globals)
                    canon.digest('G', part);
                canon.end();
                for (const Hash& part : functions)
                    canon.digest('F', part);
                return canon.hash();
            }

        private:
            struct Function
            {
                std::size_t mBegin;
                std::size_t mEnd;
            };

            /// Colour refinement over the globals: each starts from what it is alone, and each
            /// round adds what its operands were named the round before, until a round tells no two
            /// apart that the round before did not. A global's own colour enters every round, so each
            /// round's partition refines the last and the count of colours only grows.
            void refine()
            {
                mColours.resize(mGlobals.size());
                for (std::size_t global = 0; global < mGlobals.size(); ++global)
                {
                    const Line& line = *mGlobals[global];
                    Canon canon;
                    canon.literal(line.mOp);
                    for (const std::string_view operand : line.mOperands)
                        canon.literal(isId(operand) ? std::string_view("%") : operand);

                    std::vector<Hash> decorations;
                    for (const Line* decoration : decorationsOf(line.mResult))
                    {
                        Canon shape;
                        shape.literal(decoration->mOp);
                        for (std::size_t at = 1; at < decoration->mOperands.size(); ++at)
                            shape.literal(
                                isId(decoration->mOperands[at]) ? std::string_view("%") : decoration->mOperands[at]);
                        decorations.push_back(shape.hash());
                    }
                    std::ranges::sort(decorations);
                    for (const Hash& decoration : decorations)
                        canon.digest('D', decoration);

                    mColours[global] = canon.hash();
                }

                std::size_t distinct = countDistinct(mColours);
                std::vector<Hash> next(mGlobals.size());
                while (true)
                {
                    for (std::size_t global = 0; global < mGlobals.size(); ++global)
                    {
                        const Line& line = *mGlobals[global];
                        Canon canon;
                        canon.digest('S', mColours[global]);
                        for (const std::string_view operand : line.mOperands)
                            if (isId(operand))
                                canon.digest('G', mColours[globalAt(operand)]);

                        std::vector<Hash> decorations;
                        for (const Line* decoration : decorationsOf(line.mResult))
                        {
                            Canon named;
                            named.literal(decoration->mOp);
                            for (std::size_t at = 1; at < decoration->mOperands.size(); ++at)
                                if (isId(decoration->mOperands[at]))
                                    named.digest('G', mColours[globalAt(decoration->mOperands[at])]);
                                else
                                    named.literal(decoration->mOperands[at]);
                            decorations.push_back(named.hash());
                        }
                        std::ranges::sort(decorations);
                        for (const Hash& decoration : decorations)
                            canon.digest('D', decoration);

                        next[global] = canon.hash();
                    }

                    mColours.swap(next);
                    const std::size_t refined = countDistinct(mColours);
                    if (refined == distinct)
                        break;
                    distinct = refined;
                }

                std::vector<Hash> sorted = mColours;
                std::ranges::sort(sorted);
                mAlike.assign(mGlobals.size(), false);
                for (std::size_t global = 0; global < mGlobals.size(); ++global)
                {
                    const auto [first, last] = std::ranges::equal_range(sorted, mColours[global]);
                    mAlike[global] = last - first > 1;
                }
            }

            static std::size_t countDistinct(std::vector<Hash> colours)
            {
                std::ranges::sort(colours);
                return static_cast<std::size_t>(std::ranges::unique(colours).begin() - colours.begin());
            }

            const std::vector<const Line*>& decorationsOf(std::string_view id) const
            {
                static const std::vector<const Line*> none;
                const auto found = mDecorations.find(id);
                return found == mDecorations.end() ? none : found->second;
            }

            std::size_t globalAt(std::string_view id) const
            {
                const auto found = mGlobalAt.find(id);
                if (found == mGlobalAt.end())
                    throw std::runtime_error("no global defines " + std::string(id));
                return found->second;
            }

            std::size_t functionAt(std::string_view id) const
            {
                const auto found = mFunctionAt.find(id);
                if (found == mFunctionAt.end())
                    throw std::runtime_error("no function defines " + std::string(id));
                return found->second;
            }

            /// A global's colour or a function's digest, marked apart.
            ///
            /// **Globals the refinement left alike are told apart by the order the program first
            /// names them.** Two variables of one type and one storage class with no decoration are
            /// one colour, and a colour alone would name a program that read the one where it read the
            /// other as the same program. The order of first naming is what swapping them everywhere —
            /// which changes nothing — leaves alone.
            void nameOutside(Canon& canon, std::string_view id)
            {
                if (const auto found = mGlobalAt.find(id); found != mGlobalAt.end())
                {
                    const std::size_t global = found->second;
                    canon.digest('G', mColours[global]);
                    if (mAlike[global])
                    {
                        const auto [named, fresh] = mNamed.emplace(global, 0);
                        if (fresh)
                            named->second = mNamedSoFar[mColours[global]]++;
                        canon.number('N', named->second);
                    }
                }
                else if (const auto function = mFunctionAt.find(id); function != mFunctionAt.end())
                    canon.digest('F', functionDigest(function->second));
                else
                    throw std::runtime_error("nothing the module declares defines " + std::string(id));
            }

            /// **An entry point's interface is a set**, so its variables are sorted by their colours.
            Hash fixedDigest(const Line& line)
            {
                Canon canon;
                canon.literal(line.mOp);
                if (line.mOp == "OpEntryPoint")
                {
                    if (line.mOperands.size() < 3)
                        throw std::runtime_error("an entry point names no function");
                    canon.literal(line.mOperands[0]);
                    nameOutside(canon, line.mOperands[1]);
                    canon.literal(line.mOperands[2]);

                    std::vector<Hash> interface;
                    for (std::size_t at = 3; at < line.mOperands.size(); ++at)
                    {
                        Canon variable;
                        nameOutside(variable, line.mOperands[at]);
                        interface.push_back(variable.hash());
                    }
                    std::ranges::sort(interface);
                    for (const Hash& variable : interface)
                        canon.digest('G', variable);
                    return canon.hash();
                }

                for (const std::string_view operand : line.mOperands)
                    if (isId(operand))
                        nameOutside(canon, operand);
                    else
                        canon.literal(operand);
                return canon.hash();
            }

            /// A function's instructions in their order, each id it defines named by where it is
            /// defined and each decoration on one written after it. **A function a call names is
            /// digested first**, which SPIR-V's ban on recursion keeps finite.
            Hash functionDigest(std::size_t function)
            {
                if (mFunctionDigests[function].has_value())
                    return *mFunctionDigests[function];
                if (!mDigesting.insert(function).second)
                    throw std::runtime_error("a function calls itself");

                const Function& range = mFunctions[function];
                std::unordered_map<std::string_view, std::size_t> numbered;
                for (std::size_t at = range.mBegin; at < range.mEnd; ++at)
                    if (!mLines[at].mResult.empty() && !numbered.emplace(mLines[at].mResult, numbered.size()).second)
                        throw std::runtime_error(std::string(mLines[at].mResult) + " is defined twice");

                const auto name = [&](Canon& canon, std::string_view operand) {
                    if (!isId(operand))
                        canon.literal(operand);
                    else if (const auto local = numbered.find(operand); local != numbered.end())
                        canon.number('L', local->second);
                    else
                        nameOutside(canon, operand);
                };

                Canon canon;
                for (std::size_t at = range.mBegin; at < range.mEnd; ++at)
                {
                    const Line& line = mLines[at];
                    if (!line.mResult.empty())
                        canon.number('L', numbered.at(line.mResult));
                    canon.literal(line.mOp);
                    for (const std::string_view operand : line.mOperands)
                        name(canon, operand);

                    if (!line.mResult.empty())
                    {
                        std::vector<Hash> decorations;
                        for (const Line* decoration : decorationsOf(line.mResult))
                        {
                            Canon named;
                            named.literal(decoration->mOp);
                            for (std::size_t operand = 1; operand < decoration->mOperands.size(); ++operand)
                                name(named, decoration->mOperands[operand]);
                            decorations.push_back(named.hash());
                        }
                        std::ranges::sort(decorations);
                        for (const Hash& decoration : decorations)
                            canon.digest('D', decoration);
                    }
                    canon.end();
                }

                mDigesting.erase(function);
                mFunctionDigests[function] = canon.hash();
                return *mFunctionDigests[function];
            }

            std::vector<Line> mLines;
            std::unordered_map<std::string_view, std::vector<const Line*>> mDecorations;
            std::vector<const Line*> mGlobals;
            std::unordered_map<std::string_view, std::size_t> mGlobalAt;
            std::vector<Hash> mColours;

            /// Whether another global shares a global's colour, and the order the program first
            /// named each such global in, counted within its colour.
            std::vector<bool> mAlike;
            std::unordered_map<std::size_t, std::size_t> mNamed;
            std::map<Hash, std::size_t> mNamedSoFar;

            std::vector<const Line*> mFixed;
            std::vector<Function> mFunctions;
            std::unordered_map<std::string_view, std::size_t> mFunctionAt;
            std::vector<std::optional<Hash>> mFunctionDigests;
            std::unordered_set<std::size_t> mDigesting;
        };
    }

    std::array<std::uint64_t, 2> digestProgram(std::string_view disassembly)
    {
        return Program(disassembly).digest();
    }
}
