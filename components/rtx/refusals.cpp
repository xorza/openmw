#include "refusals.hpp"

#include <string>

#include <components/debug/debuglog.hpp>

namespace Rtx
{
    namespace
    {
        /// How the log says what became of one kind: what the renderer does, the article an
        /// unnamed one takes, and the noun.
        struct Wording
        {
            std::string_view mDoes;
            std::string_view mArticle;
            std::string_view mNoun;
        };

        constexpr std::array<Wording, sRefusedKinds> sWording{
            Wording{ "leaves out", "a", "mesh" },
            Wording{ "leaves out", "a", "model" },
            Wording{ "draws a stand-in for", "a", "texture" },
            Wording{ "leaves out", "a", "sky layer" },
            Wording{ "leaves out", "a", "moon" },
            Wording{ "leaves out", "a", "lamp" },
            Wording{ "leaves out", "an", "emitter" },
            Wording{ "leaves out sprites of", "an", "emitter" },
        };

        /// A kind added to `Refused` and not here would log with no words at all.
        constexpr bool everyKindWorded()
        {
            for (const Wording& wording : sWording)
                if (wording.mNoun.empty())
                    return false;

            return true;
        }

        static_assert(everyKindWorded(), "a kind of refusal the log has no words for");

    }

    void Refusals::refuse(Refused kind, std::string_view name, std::string_view why)
    {
        if (mNamed.find(Key(kind, name, why)) != mNamed.end())
            return;

        mNamed.insert(Refusal{ .mKind = kind, .mName = std::string(name), .mWhy = std::string(why) });
        ++mCounts[static_cast<std::size_t>(kind)];

        const Wording& wording = sWording[static_cast<std::size_t>(kind)];
        if (name.empty())
            Log(Debug::Warning) << "Ray tracing " << wording.mDoes << ' ' << wording.mArticle << ' ' << wording.mNoun
                                << ": " << why;
        else
            Log(Debug::Warning) << "Ray tracing " << wording.mDoes << " the " << wording.mNoun << " \"" << name
                                << "\": " << why;
    }

    void Refusals::refuse(std::span<const Refusal> held)
    {
        for (const Refusal& refusal : held)
            refuse(refusal.mKind, refusal.mName, refusal.mWhy);
    }
}
