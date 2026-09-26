# Crash catcher: a redesign

Status: built, except where section 0 says otherwise. Section 9 records the decisions.

## 0. As built

What was built, and where it departs from the plan below:

- **Crashpad** is `getsentry/crashpad` at the commit sentry-native 0.17.1 pins, the newest release,
  with two of its submodules as pinned archives, `mini_chromium` and `lss`, and the tree's own zlib
  in place of the third (`extern/CMakeLists.txt`). On Linux its libcurl
  upload transport is patched to its socket transport, so nothing links libcurl
  (`extern/crashpadpatch.cmake`). It is on for Windows, Linux and macOS.
- **The legacy catcher is gone**: the Windows monitor and its shared memory, the gdb and lldb path,
  and the in-process summary.
- **The monitor** is this executable with `--crash-monitor`. `crashpadmonitor.cpp` runs
  `crashpad::HandlerMain` with a `UserStreamDataSource` that writes the summary to the log and
  into the dump, stream `0x4F4D5701`.
- **The notes are read from outside.** In place of a registered user stream, which Crashpad does
  not support on macOS, the monitor reads the note table out of the crashed process through
  `ProcessSnapshot::Memory()`, at the address the game gives it on the command line. That works
  the same on all three systems.
- **The CrashpadInfo note on Linux** is forced into every link with
  `--undefined=CRASHPAD_NOTE_REFERENCE`. Crashpad's own reference to it is a dead store that GCC
  removes, and without the note the monitor finds no `CrashpadInfo`: no annotations and no
  settings. The matrix checks the annotations for every mode.
- **The heap near the stacks** is kept on Windows only. Crashpad scans stacks for pointers there
  and nowhere else (`thread_snapshot_win.cc`). On Linux and macOS it keeps what the registers
  point at. The matrix checks a heap marker on Windows.
- **Alternate signal stacks** come from Crashpad's `pthread_create_linux.cc`, linked into every
  executable. Every thread gets one, drivers' and SDL's too, so the API has no `ThreadGuard`.
- **Hangs** use one limit, `[General] crash hang seconds`, 20 by default. The heartbeat is in a
  named shared page (`crashpage.cpp`). The monitor asks for a hang report with `SIGUSR2` on POSIX,
  and with a thread it starts in the game at a function the game names on Windows. Either way,
  Crashpad writes the dump without a crash, and the same data source writes the summary.
- **The start** is in `Debug::setupLogging`, where the log folder is first known. What runs
  before it is only the reading of the configuration.
- **The dialog** shows where upstream's fatal-error box shows: always on Windows, and on POSIX
  when stdin is not a terminal. `OPENMW_CRASH_DIALOG=0` turns it off, and `openmw-rtxtool` sets it.
- **FreeBSD** gets no catcher: `install` says so in the log. It does not keep the legacy path.
- **Symbols:** the package presets add `-g1`, and `-Z7 -DEBUG` on Windows. `rtx package archive`
  writes `openmw-<name>-<system>-symbols.zip` with `dump_syms` 2.3.9. `rtx package crash <dump>`
  reads a dump with `minidump-stackwalk` 0.27.0. Both tools are pinned in `pins.sh`.
- **Tests:** `CrashNoteTest` and `CrashSummaryTest` in `components-tests`, and `crash-tests
  --matrix`, which `rtx <flavour> test` runs. The matrix checks twelve modes on Linux (fourteen on
  Windows), each in its own process with the real catcher.

Not built yet:

- **The WER module** for `/GS` failures, heap corruption and `__fastfail` on Windows. Windows loads
  such a module only if it is listed under `RuntimeExceptionHelperModules` in the registry, so the
  game would have to write to the player's registry. That needs a decision first.
- **macOS has not been built or run.** Upstream's macOS CI is its first build, and no crash has
  been caught on macOS yet.

## 1. The result

The catcher becomes one component with one API. Its behaviour is the same on Windows, Linux and
macOS:

- **One capture path.** A monitor process captures every crash out of process. The monitor is
  this same executable, started with `--crash-monitor`.
- **One report format.** Each crash gives a minidump on every platform. The minidump holds all
  threads, the memory near every stack pointer, the memory map, every module with its build id,
  the per-thread notes and the build's annotations.
- **One summary.** The monitor, not the crashed process, appends the same summary lines to the
  game's log on every platform.
- **One symbol pipeline.** Each release keeps its symbols. One harness verb then turns a player's
  dump into function names and lines.

The recommended engine is Crashpad, behind our own API. Crashpad is the capture engine of
Chromium, Electron and Sentry. Our code keeps only what is specific to OpenMW: the API, the
notes, the summary, hang detection and the dialog.

## 2. What we have now

| | Windows | Linux, macOS, FreeBSD | Android |
|---|---|---|---|
| Start | `Crash::CrashCatcher` object. It starts a monitor (the same exe) and a shared memory block | `crashCatcherInstall(argc, argv, path)`, a free function | nothing |
| Fault hook | `SetUnhandledExceptionFilter` | `sigaction` for SEGV, ILL, FPE, BUS, ABRT, with one alternate stack on the main thread only | — |
| At the crash | the handler copies the context to shared memory, signals the monitor and waits | the handler forks and then execs the game with `--cc-handle-crash`. It sends the signal data through a pipe | — |
| Capture | the monitor calls `MiniDumpWriteDump` | the child starts **gdb or lldb** on the crashed pid and writes their text | — |
| Output | `<app>-crash.dmp` in the log folder, plus the summary in `<app>.log` (added this session) | `<app>-crash.log` in the **temp folder** | — |
| Hangs | the monitor calls `IsHungAppWindow`, shows a box, and writes a freeze dump on "Abort" | none | — |
| API | a class, `#ifdef _WIN32` in `debugging.cpp` | a free function, `#else` in `debugging.cpp` | an inline no-op |

The component is 1924 lines in nine files.

### Problems

1. **Two designs, two outputs.** Windows writes a dump. POSIX writes debugger text. A crash on
   one platform cannot be read with the tools of the other. `debugging.cpp` holds two `#ifdef`
   branches with different arguments.
2. **POSIX needs a debugger that players do not have.** Without gdb or lldb on the PATH, the log
   says only which signal came. gdb takes seconds to attach, it needs ptrace permission, and it
   fails in sandboxes.
3. **POSIX reports go to the temp folder.** Logs and Windows dumps go to the log folder. Players
   send the log and do not find the crash report.
4. **Windows misses whole classes of crash.** These paths do not reach the unhandled exception
   filter:
   - `abort()`, which ends in `__fastfail` when `_CALL_REPORTFAULT` is set;
   - `/GS` stack-cookie failures and heap corruption (`0xC0000374`), which Windows sends to WER
     directly;
   - pure virtual calls and CRT invalid-parameter calls, which go to CRT handlers;
   - `std::terminate`.

   Sentry reports the same gap for every Windows backend except Crashpad
   ([sentry-native #591](https://github.com/getsentry/sentry-native/issues/591)).
5. **Only the main thread has an alternate signal stack.** A stack overflow on a worker thread
   (`Rtx::Worker`, `parallel`, `SceneUtil::WorkQueue`) faults again inside the handler, and it
   reports nothing.
6. **The POSIX handler does work that is not signal-safe.** It calls `printf` and copies a
   `std::optional` inside the handler. Its child process calls `std::filesystem` and SDL.
7. **macOS catches crashes with signals.** Mach exceptions come first, some crashes (stack
   overflow among them) never become a signal, and `EXC_CRASH` cannot be handled in process
   ([Lacking Rhoticity](http://lackingrhoticity.blogspot.com/2013/08/handling-crashes-on-mac-os-x.html),
   [Mike Ash](https://www.mikeash.com/pyblog/friday-qa-2013-01-11-mach-exception-handlers.html)).
8. **The hang detection asks the wrong question.** `IsHungAppWindow` asks whether the window pumps
   messages. A game can pump messages while its frame loop is stuck. It is Windows only.
9. **The shared-memory struct is fragile.** A comment in it says that "when we've made this struct
   bigger in the past, things exploded".
10. **No symbols are kept, and no tool reads a dump.** For the RTX 2060 crash, each frame had to
    be named by hand from the strings the function referenced.
11. **No test crashes anything.** Nothing proves that a crash on any platform gives a report.

## 3. What established reporters do

- **Capture out of process.** Crashpad
  ([overview design](https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/doc/overview_design.md)),
  Sentry's `crashpad` and `native` backends
  ([backend trade-offs](https://docs.sentry.io/platforms/native/advanced-usage/backend-tradeoffs)),
  and Embark's `minidumper`
  ([crash-handling](https://github.com/embarkstudios/crash-handling)) all read the crashed process
  from a healthy one. A corrupt heap or a held lock in the crashed process then cannot stop the
  report. In-process backends (Breakpad, Sentry `inproc`) come after these, for platforms where a
  second process is not possible.
- **Do almost nothing in the crashed process.** Its handler records where it faulted, tells the
  monitor, and waits. The rules: async-signal-safe calls only; no allocation, lock or stdio; an
  alternate stack on each thread; one crash handled at a time; any other crashing thread waits and
  does not exit.
- **One format everywhere: the minidump.** Crashpad and Breakpad write minidumps on Linux and
  macOS too. `minidump-stackwalk`
  ([rust-minidump](https://github.com/rust-minidump/rust-minidump)), WinDbg and lldb read them.
- **Symbolize at the desk, not at the crash.** `dump_syms` makes Breakpad symbol files from PDB,
  DWARF and Mach-O at release time. `minidump-stackwalk` uses them to name every frame.
- **Annotations and custom streams.** Crashpad reads process-wide key/value annotations and
  registered memory ranges out of process, through `CrashpadInfo`
  (`set_simple_annotations`, `AddUserDataMinidumpStream`, `set_extra_memory_ranges`).
- **Platform hooks, from Crashpad's `CrashpadClient`:**
  - **Windows:** an unhandled exception filter, plus a WER runtime module (`RegisterWerModule`)
    for the fail-fast exceptions that bypass the filter.
  - **Linux:** a signal handler and a socket to the handler, with `PR_SET_PTRACER` for Yama
    ptrace scope 1 ([Yama](https://docs.kernel.org/admin-guide/LSM/Yama.html)).
    `InitializeSignalStackForThread` gives each thread its alternate stack.
  - **macOS:** a Mach exception port that the handler owns, so no code runs in the crashed
    process.
- **The handler can run inside the application's own executable.** Chromium runs it as
  `chrome --type=crashpad-handler` and calls `crashpad::HandlerMain` itself. The CMake build of
  `getsentry/crashpad` exposes the handler as a library for exactly this
  ([getsentry/crashpad #14](https://github.com/getsentry/crashpad/pull/14)).
- **Detect hangs with a heartbeat.** The loop that must progress writes a counter. A watchdog
  outside the loop dumps the process when the counter stops.

## 4. Goals and non-goals

Goals:

1. One API with no platform in it. `debugging.cpp` has no `#ifdef` for the catcher.
2. The same artifacts on Windows, Linux and macOS: a minidump and a log summary, both in the log
   folder.
3. Every crash class in the matrix of section 8 is caught on every platform that has it.
4. No debugger on the player's machine.
5. Nothing in the crashed process but async-signal-safe work.
6. No cost in a frame. A note stays a copy of at most 256 bytes. The heartbeat is one relaxed
   atomic store for each frame.
7. Every release keeps its symbols, and one harness verb turns a dump into a named report.
8. A crash test for every row of the matrix, on every CI platform.

Non-goals: uploading reports to a server, Android, and a report viewer in the game.

## 5. Design

### 5.1 Public API (`components/crashcatcher/crash.hpp`)

```cpp
namespace Crash
{
    struct Settings
    {
        std::string_view mApplication;       // "OpenMW", names the files
        std::filesystem::path mReportFolder; // the log folder
        std::filesystem::path mLogFile;      // where the monitor appends the summary
        std::chrono::seconds mHangAfter{ 20 }; // no heartbeat for this long is a hang; nought: off
        bool mDialog = true;                 // a message box after a report
    };

    /// Runs the monitor and never returns, where this process was started as one. Called first
    /// thing in `main`, before anything else runs.
    [[noreturn]] void runMonitorIfAsked(int argc, char** argv);

    /// Starts the monitor and hooks every fault path this platform has. An error, with the
    /// reason, where it cannot (a debugger attached, no second process possible, the switch off).
    /// Only one exists, and destroying it ends the monitor cleanly.
    class Catcher
    {
    public:
        static Result<Catcher, std::string> install(int argc, char** argv, const Settings& settings);

        /// From the loop that must progress, once for each iteration: one relaxed store.
        void heartbeat();
    };

    /// Unchanged from today: a per-thread note of what the thread is doing.
    void note(std::string_view what, std::string_view subject = {});

    /// A process-wide key and value in every report: the version, the renderer, the device, the
    /// driver. Copied into a fixed table, so later calls may pass temporaries.
    void annotate(std::string_view key, std::string_view value);

    /// For a thread this code creates: its alternate signal stack and its note slot. A no-op where
    /// the platform needs neither.
    class ThreadGuard;

    /// A report without a crash, for a contract broken where the game can continue.
    void report(std::string_view reason);
}
```

`wrapApplication` calls `runMonitorIfAsked` and then `Catcher::install`, with no platform
branches. `OPENMW_DISABLE_CRASH_CATCHER` stays. `openmw-rtxtool` keeps setting it by default, and
now also sets `mDialog = false`.

### 5.2 Processes

```
 game process                                   monitor process (same exe, --crash-monitor)
 ─────────────                                  ─────────────────────────────────────────
 Catcher::install ── starts ──────────────────▶ crashpad::HandlerMain + our data sources
   Crashpad client: filter / signals / WER        waits on the registration channel
   Mach port on macOS
   note table, annotations,  ── registered ──▶  read from the game's memory at crash time
   heartbeat counter             ranges
                                                watchdog thread: reads the heartbeat
 fault ─▶ Crashpad's in-process step          ─▶ snapshot of all threads ─▶ <app>-<time>.dmp
          (async-signal-safe, then waits)        our data source: summary ─▶ <app>.log
                                                 dialog, then the game ends
```

- **Crashpad** owns the fault hooks, the IPC, the snapshot and the minidump writer on Windows,
  Linux and macOS.
- **Our monitor code** runs inside the handler process as a `crashpad::UserStreamDataSource`. It
  sees the same `ProcessSnapshot` that the minidump is written from. It adds the OpenMW stream
  (the notes) and appends the summary to the log. The summary therefore comes from one place on
  every platform, and no crashed process is asked to format text.
- **Crashpad's database** lives in `<log folder>/crashes/`. Our code copies each new dump to
  `<log folder>/<app>-crash-<time>.dmp` for players to find, and keeps the newest five.

### 5.3 The summary

The monitor writes it from the snapshot. The format is the same on every platform:

```
[01:33:13.012 E] Crash: EXCEPTION_ACCESS_VIOLATION reading 0x12204cfe000 in thread 12632 (main)
[01:33:13.012 E] Crash: at VCRUNTIME140.dll+0x1dd06
[01:33:13.012 E] Crash: return addresses on its stack: openmw.exe+0x112a9a7 openmw.exe+0x112b1f7 …
[01:33:13.012 E] Crash: note of thread 12632, which crashed: staging the texture "textures/…"
[01:33:13.012 E] Crash: openmw 0.52.0 rtx-v0.1.1 (7e09ca1174), Vulkan on NVIDIA GeForce RTX 2060 SUPER 616.92
[01:33:13.012 E] Crash: dump written to C:\Users\…\openmw-crash-20260926-013313.dmp
```

A snapshot has registers and stack memory, but no unwinder. So the monitor prints the faulting
address, and then the stack values that point into module code, labelled as candidates. This is
the same scan that named the RTX 2060 crash's frames. Exact frames come from
`minidump-stackwalk` at the desk (5.7). The in-process unwinder added this session
(`windowscrashsummary.cpp`) goes away. It was Windows-only work inside a crashed process.

### 5.4 Notes and annotations

- The per-thread note table (`crashnote.cpp`, 32 slots with sequence counters) stays as it is.
  At install, its address range is registered with `AddUserDataMinidumpStream`, as stream type
  `0x4F4D5701` ("OMW" 1). Every dump carries it, and the summary reads it.
- `annotate` writes into a fixed `crashpad::SimpleStringDictionary`, registered through
  `set_simple_annotations`. The game sets the version, the git commit, the renderer, the
  device, the driver and the settings that change what runs, such as DLSS and the upscaler mode.

### 5.5 Fault paths for each platform

| Event | Windows | Linux | macOS | FreeBSD (fallback) |
|---|---|---|---|---|
| Access violation, illegal instruction, divide by zero | Crashpad filter | Crashpad signal handler | Mach exception port | signal handler |
| Stack overflow, main thread | filter (guard page) | alternate stack | Mach port (no in-process code) | alternate stack |
| Stack overflow, worker thread | filter | `ThreadGuard` alternate stack | Mach port | `ThreadGuard` |
| `abort()` | `SIGABRT` handler plus `_set_abort_behavior(0, _CALL_REPORTFAULT)` → `report` | `SIGABRT` | `SIGABRT` (no Mach exception) | `SIGABRT` |
| `std::terminate` | `std::set_terminate` → `report`, then abort | same | same | same |
| Pure virtual call, CRT invalid parameter | `_set_purecall_handler`, `_set_invalid_parameter_handler` → `report` | — | — | — |
| `/GS`, heap corruption, `__fastfail` | WER module (`RegisterWerModule`) | — | — | — |
| Bus error | — | `SIGBUS` | Mach port | `SIGBUS` |
| Hang (heartbeat stops) | watchdog → `DumpAndCrashTargetProcess` or a dump without a crash | watchdog → a signal to the game, whose handler asks for a dump | watchdog → a dump through the Mach port | watchdog → the summary only |
| A second crash during handling | waits | waits | kernel serialises | waits |

FreeBSD is the one exception to symmetry, because Crashpad does not support it. It keeps an
in-process fallback: the same signal hooks, a summary with the fault address, the notes and the
candidate return addresses, written with `write(2)`, and no dump.

### 5.6 Hangs

`Catcher::heartbeat()` stores the frame counter in a page the monitor maps. A monitor thread reads
it every second. When it stays unchanged for **20 s**, the monitor:

1. writes a hang dump of all threads (the process continues);
2. appends a "Hang:" summary to the log, with every thread's note;
3. shows a dialog: "OpenMW has not responded for 20 seconds. A report is saved in … Wait / End
   OpenMW". The dialog closes itself if the heartbeat comes back, and the log then says for how
   long the game stopped. "End OpenMW" ends the process after the dump, as the Windows freeze box
   does today.

The limit is a setting, `[General] crash hang seconds`, and nought turns the check off.

**One limit for everything, and what it costs.** The player's log for the RTX 2060 crash shows the
pipeline compilation with a warm driver cache: 19 pipelines, 8.8 s of compile time in all, the
slowest (`visibility sun sea`) 1.0 s, and about 1.4 s of wall time in parallel. That is well under
the limit. A cold cache after a driver update, a very large cell load or a save can pass 20 s, and
then the player sees the dialog and a report is written for work that was only slow. "Wait"
continues the game, and the dialog closes itself when the frames come back. If such reports turn
up, a second, longer limit for known slow work is the next step: a scope around each slow
operation that names it.

This replaces `IsHungAppWindow` on Windows and adds hangs on Linux and macOS.

### 5.7 Symbols and the desk tool

- The CI release job runs Mozilla's `dump_syms` on each shipped binary: `openmw.exe` with its
  PDB, and the Linux binaries before they are stripped into the AppImage. It uploads
  `symbols-<tag>.zip` as a release asset beside the archives. Players never download it.
- `rtx crash <dump> [--symbols=<zip or dir>]` runs `minidump-stackwalk --human` and prints every
  thread with function names and lines, the OpenMW stream decoded as notes, and the annotations.
- Both tools are Rust binaries, used in CI and at the desk only. The game links neither.

### 5.8 Privacy

A dump holds game memory near each stack pointer, the memory map and module paths. It must not
hold the environment. Phase 3 checks what Crashpad's snapshot records, and turns off what
carries the environment, as `MiniDumpWithProcessThreadData` would on Windows. The dialog says what the files contain, and that sending them is the player's choice.

## 6. Alternatives considered

| | A. Crashpad (recommended) | B. Breakpad | C. Our own, on all platforms | D. Embark's Rust crates |
|---|---|---|---|---|
| Out of process | yes, on Windows, Linux and macOS | Windows only. Linux and macOS are in process | yes, if we write it | yes |
| Minidump on Linux and macOS | yes | yes | only with our own writer (Breakpad's Linux writer is about 5,000 lines) | yes |
| Windows fail-fast and WER | yes | no | our own WER module | no |
| macOS Mach port | yes | in process | ours to write | yes |
| Maintained | yes: Chromium, Sentry | maintenance mode | by us | yes |
| Build | CMake through `getsentry/crashpad`, which pins `mini_chromium` | CMake ports exist | nothing new | adds a Rust toolchain to every build |
| Size (estimate, to measure in phase 3) | a few MB linked into the exe | smaller | — | a few MB, plus the toolchain |
| FreeBSD | no | no | yes | no |

C is the only option with no dependency. Its cost is a minidump writer for Linux and macOS, a Mach
exception server, a WER module, and an IPC protocol, all maintained by us. That is the work that
Crashpad already does and tests at Chromium's scale. D brings Rust into a C++ build. B is in
process where it matters most, on Linux.

## 7. Implementation plan

Each phase ends in a build that CI checks on both legs, and none of them leaves a platform
without a catcher.

**Phase 0: decisions.** See section 9. Nothing is built before them.

**Phase 1: the crash test harness, against today's catcher.**
- `crash-tests`: a small executable. Each mode is one row of section 8: `--crash=null-read`,
  `--crash=stack-overflow-worker`, `--crash=abort`, `--crash=terminate`, `--crash=purecall`,
  `--crash=fastfail`, `--crash=hang`, … It installs the catcher with the real `Settings`, notes
  something, and then crashes.
- `rtx debug crash-matrix`: runs every mode in a temporary folder and checks the artifacts:
  a dump exists and parses, the crashed thread and the exception code are correct, the note is in
  the OpenMW stream, and the summary lines are in the log.
- CI runs the matrix on both legs. On this phase it fails where section 2 says that today's
  catcher fails. Each failure is recorded as an expected failure that names the problem. A later
  phase removes that entry, which proves the fix.

**Phase 2: the API, over today's backends.**
- `crash.hpp` as in 5.1. `debugging.cpp` loses its `#ifdef`s.
- Both old backends sit behind `Catcher`. The behaviour does not change yet.

**Phase 3: Crashpad on Linux and Windows.**
- `getsentry/crashpad` at a pinned commit, through `FetchContent`, beside the Windows deps and the
  AppImage build. The handler library is linked into `openmw`, and `runMonitorIfAsked` calls
  `crashpad::HandlerMain` for `--crash-monitor`.
- The database goes in `<log folder>/crashes`, with the copy-out and retention of 5.2.
- The notes stream and the annotations (5.4).
- The monitor's data source writes the summary (5.3).
- `ThreadGuard` in `Rtx::Worker`, `Rtx::parallel` and `SceneUtil::WorkQueue`.
- The old Windows monitor, SHM, summary and dump code are deleted. The POSIX gdb path stays for
  FreeBSD only.
- The matrix rows for these platforms move from expected failures to passes.

**Phase 4: the Windows paths around the filter.**
- The WER module (`crashpad_wer.dll`), registered at install and shipped beside the exe.
- `SIGABRT`, `_set_abort_behavior`, `_set_purecall_handler`, `_set_invalid_parameter_handler`,
  and `std::set_terminate` on every platform, each ending in `report` and then a crash.

**Phase 5: hangs.** The heartbeat, the limit, its setting and the dialog of 5.6 on all three
platforms. `IsHungAppWindow` goes. The harness shortens the limit, so a hang row of the matrix
takes seconds.

**Phase 6: symbols and the desk tool.** `dump_syms` in the release job, the symbols asset, and
`rtx crash`. The first real player dump is then read with it, as the acceptance test.

**Phase 7: macOS and FreeBSD.** Crashpad with the Mach port on macOS, in upstream's macOS CI
(which runs the whole `components-tests` and the matrix). FreeBSD gets the in-process fallback of
5.5, with the matrix limited to what it promises.

**Phase 8: the dialog and the documentation.** One dialog text on every platform, naming both
files. `docs/rtx/architecture.md` gains a section on the catcher, and the player-facing README gets
"what to send after a crash".

## 8. Test matrix

Each cell is what `rtx debug crash-matrix` asserts: **D** = a dump that parses, with the right
exception, the right crashed thread and the OpenMW stream. **S** = the summary in the log. `—` =
the platform does not have this event.

| Mode | Windows | Linux | macOS | FreeBSD |
|---|---|---|---|---|
| null read, main thread | D S | D S | D S | S |
| write to read-only memory | D S | D S | D S | S |
| stack overflow, main thread | D S | D S | D S | S |
| stack overflow, worker thread | D S | D S | D S | S |
| `abort()` | D S | D S | D S | S |
| uncaught exception (`std::terminate`) | D S | D S | D S | S |
| pure virtual call | D S | D S | D S | S |
| CRT invalid parameter | D S | — | — | — |
| `__fastfail` / `/GS` | D S | — | — | — |
| illegal instruction | D S | D S | D S | S |
| two threads crash at once | one D S | one D S | one D S | one S |
| a crash inside the note code | D S | D S | D S | S |
| a crash before logging is set up | D, summary to stderr | D, summary to stderr | D, summary to stderr | stderr |
| hang for longer than `mHangAfter` | hang D S, game continues | same | same | S |
| a stop shorter than `mHangAfter` | nothing reported | same | same | same |
| `report()` without a crash | D S, game continues | same | same | S |

The unit tests of today stay: the note table on all platforms, and the summary formatting as a
pure function of a snapshot. A small snapshot fake makes that testable without a crash.

## 9. Decisions

1. **The Crashpad dependency: accepted.** `getsentry/crashpad` (Apache 2.0) at a pinned commit,
   with `mini_chromium` and `zlib`, and uploading built out if the build allows it. Its handler
   is linked into our executable. The size it adds and the WER DLL are measured in phase 3.
2. **`dump_syms` and `minidump-stackwalk`: accepted** as CI and desk tools, downloaded in CI at a
   pinned version with a checksum. At the desk, the install is yours to run.
3. **Symbols: a public release asset**, `symbols-<tag>.zip` beside each release's archives.
4. **The upstream diff: accepted.** The component is replaced in the fork.
5. **Hangs:** the dialog appears. One limit, 20 s, for now (section 5.6). It is a setting.

## 10. Risks

- **Crashpad on the AppImage.** The monitor is the same binary, so the AppImage needs nothing new.
  But `PR_SET_PTRACER` must name the monitor, and some sandboxes forbid ptrace. The fallback there
  is the in-process summary of 5.5, which the matrix also runs with ptrace denied.
- **The Windows deps build.** Upstream's prebuilt deps are MSVC 2022. Crashpad builds with the
  same toolset, but it enlarges the first CI build. The deps cache holds it after that.
- **Anti-cheat and overlays** (the Steam overlay, NVIDIA's overlay) install their own exception
  filters. Crashpad's filter can be replaced after install. Phase 3 re-asserts it after the
  renderer starts, and the matrix tests with a second filter installed on top of ours.
- **The dump size.** It is about 80 MB with the driver's data segments, as the RTX 2060 dump was.
  Phase 3 measures Crashpad's dump against it. The notes and the annotations are registered
  explicitly, so they do not depend on data segments being captured.

## Sources

- [Crashpad overview design](https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/doc/overview_design.md)
- [`CrashpadClient`](https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/client/crashpad_client.h) and [`CrashpadInfo`](https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/client/crashpad_info.h)
- [Sentry native: backend trade-offs](https://docs.sentry.io/platforms/native/advanced-usage/backend-tradeoffs), [backends](https://docs.sentry.io/platforms/native/configuration/backends)
- [sentry-native #591: alternative backends on Windows miss `abort()`](https://github.com/getsentry/sentry-native/issues/591)
- [getsentry/crashpad #14: the handler as a static library](https://github.com/getsentry/crashpad/pull/14)
- [EmbarkStudios crash-handling](https://github.com/embarkstudios/crash-handling)
- [rust-minidump and minidump-stackwalk](https://github.com/rust-minidump/rust-minidump)
- [Yama ptrace scope](https://docs.kernel.org/admin-guide/LSM/Yama.html)
- [Mach exceptions and POSIX signals on macOS](http://lackingrhoticity.blogspot.com/2013/08/handling-crashes-on-mac-os-x.html), [Mike Ash: Mach exception handlers](https://www.mikeash.com/pyblog/friday-qa-2013-01-11-mach-exception-handlers.html)
- [Effective minidumps](https://debuginfo.com/articles/effminidumps.html)
- [RtlVirtualUnwind](https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlvirtualunwind)
