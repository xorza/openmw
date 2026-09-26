#include "crashsummary.hpp"

#include <string_view>

namespace Crash
{
    namespace
    {
        std::string_view kindOf(ReportKind kind)
        {
            switch (kind)
            {
                case ReportKind::Crash:
                    return "Crash";
                case ReportKind::Hang:
                    return "Hang";
                case ReportKind::Report:
                    return "Report";
            }

            return "Crash";
        }

        /// What the thread that raised the report did, as its note says: the one that faulted, or
        /// the one that asked. A hang's is whichever thread took the monitor's request, which says
        /// nothing, so it is not marked.
        std::string_view markOf(ReportKind kind)
        {
            switch (kind)
            {
                case ReportKind::Crash:
                    return ", which crashed";
                case ReportKind::Hang:
                    return "";
                case ReportKind::Report:
                    return ", which asked";
            }

            return "";
        }
    }

    void summarise(const CrashFacts& facts, std::vector<std::string>& lines)
    {
        const NotesRead& notes = facts.mNotes;
        const std::string kind = std::string(kindOf(notes.mKind)) + ": ";

        // The reason first where the code that asked gave one, because it is the reason the
        // exception was raised: `std::terminate` raises its own, which names nothing of its cause.
        std::string headline = kind;
        if (notes.mReason[0] != '\0')
            headline += notes.mReason;
        else if (!facts.mException.empty())
            headline += facts.mException;
        else
            headline += "no exception was recorded";
        if (facts.mThread != 0)
            headline += " in thread " + std::to_string(facts.mThread);
        lines.push_back(std::move(headline));

        if (notes.mReason[0] != '\0' && !facts.mException.empty())
            lines.push_back(kind + "raised as " + facts.mException);

        if (!facts.mWhere.empty())
            lines.push_back(kind + "at " + facts.mWhere);

        if (!facts.mStack.empty())
        {
            std::string stack = kind + "return addresses on its stack:";
            for (const std::string& address : facts.mStack)
                stack += " " + address;
            lines.push_back(std::move(stack));
        }

        for (std::size_t i = 0; i < notes.mCount; ++i)
        {
            const NoteCopy& note = notes.mNotes[i];
            lines.push_back(kind + "note of thread " + std::to_string(note.mThread)
                + std::string(note.mThread == facts.mThread ? markOf(notes.mKind) : "") + ": " + note.mText
                + (note.mWhole ? "" : " (half written)"));
        }
        if (notes.mCount == 0)
            lines.push_back(kind + "no thread noted anything");

        for (const auto& [key, value] : facts.mAnnotations)
            lines.push_back(kind + key + ": " + value);

        lines.push_back(kind + (facts.mDump.empty() ? "no dump was written" : "dump " + facts.mDump));
    }
}
