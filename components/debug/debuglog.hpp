#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <filesystem>
#include <iostream>

namespace Debug
{
    enum Level : unsigned
    {
        Error = 1,
        Warning = 2,
        Info = 3,
        Verbose = 4,
        Debug = 5,
        All = 6,
    };

    /// Whether a fatal error may put a window in front of somebody and wait for a click.
    ///
    /// **On for the game and off for the tools.** A player who double-clicked an icon has nowhere to
    /// read a crash log, and a box is the only thing that tells them anything at all. A developer
    /// running a harness already has the log in front of them, and the box is a click standing
    /// between them and the next run — which in a loop is the loop stopping dead.
    ///
    /// Set once, before anything can fail. Both a caught exception and a signal ask it.
    void setFatalDialogs(bool allowed);

    /// Whatever the application said above, and nothing else. A terminal on the standard streams
    /// looks like a second reason to stay quiet and is not one to act on: a player who launched
    /// from a shell is still a player.
    bool wantsFatalDialog();
}

class Log
{
public:
    static Debug::Level sMinDebugLevel;
    static bool sWriteLevel;

    explicit Log(Debug::Level level);
    ~Log();

    template <typename T>
    Log& operator<<(const T& rhs)
    {
        if (mShouldLog)
            std::cout << rhs;

        return *this;
    }

    Log& operator<<(const std::filesystem::path& rhs);

    Log& operator<<(const std::u8string& rhs);

    Log& operator<<(std::u8string_view rhs);

    Log& operator<<(const char8_t* rhs);

private:
    const bool mShouldLog;
};

#endif
