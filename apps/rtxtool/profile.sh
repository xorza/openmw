#!/usr/bin/env bash
# Profiles the renderer's CPU side with perf, over one place or a whole suite.
#
#   profile.sh                             # seyda-neen-ship, on-CPU
#   profile.sh --view=balmora-mages-guild
#   profile.sh --suite=default             # every place the benchmark reports
#   profile.sh --offcpu                    # where it waits instead of works (needs root, for BPF)
#   profile.sh --dwarf                     # unwind the driver too, at about five times the cost
#   profile.sh --tui                       # browse the last recording, run nothing
#
# Anything else goes to `openmw-rtxtool bench`, so `--seconds=30 --upscale=off` work here.
#
# **What `--offcpu` can and cannot see.** BPF collects its stacks by frame pointer and the graphics
# driver has none, so a wait that starts inside `vkWaitForFences` is recorded as an address rather
# than as the frame that asked for it. `--dwarf` does not help there: a BPF-collected stack cannot
# be unwound any other way. What the mode does establish is the shape — which library the process
# waits in, and whether a wait passes through this fork's own code at all. Read the by-library
# table; the total above it is dominated by driver worker threads parked for the length of the run.
#
# **It profiles the build `release.sh` measures, and does not have one of its own.** A profile is
# only as good as its call graph, and a stock Release build has neither line numbers nor frame
# pointers — so `release.sh` carries `-g1 -fno-omit-frame-pointer` instead, measured at 0.2%. A
# second build directory would have cost nothing less and explained a frame nobody timed.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build="$root/build-release"
out="$build/perf"

mode=cpu
unwind=fp
freq=5999
tui=false
place=()
extra=()

for arg in "$@"; do
    case "$arg" in
        --offcpu) mode=offcpu ;;
        --dwarf) unwind=dwarf ;;
        --freq=*) freq="${arg#*=}" ;;
        --tui) tui=true ;;
        --out=*) out="${arg#*=}" ;;
        --view=*) place+=("--views=${arg#*=}") ;;
        --views=*|--suite=*) place+=("$arg") ;;
        *) extra+=("$arg") ;;
    esac
done

# One place unless told otherwise. A profile that averaged an exterior and an interior would
# describe neither: the two do not spend their frame on the same thing, which is the whole reason
# the benchmark visits both.
if [ ${#place[@]} -eq 0 ]; then
    place=(--views=seyda-neen-ship)
fi

mkdir -p "$out"

# One file per question. On-CPU and off-CPU are two recordings rather than two events in one,
# because `perf report` writes every event in a file to the same page and cannot be asked for one.
if [ "$mode" = offcpu ]; then
    data="$out/blocked.data"
    slug=blocked
else
    data="$out/cpu.data"
    slug=cpu
fi

if [ "$tui" = true ]; then
    [ -f "$data" ] || { echo "no recording at $data — run profile.sh without --tui first" >&2; exit 1; }
    exec perf report -i "$data" --no-inline
fi

# Configured and built by `release.sh`, which owns the flags this needs, so the binary perf reads is
# the one `release.sh bench` reported on.
"$here/release.sh" build

# perf's control fifo. `--delay=-1` starts the counters off and `bench` turns them on around the
# frames it measures, so the recording is those frames: not the engine starting, not two seconds of
# a cell coming off the disk, not the renderer being taken down. Trimming a whole-run recording
# afterwards is the alternative, and it needs a boundary nobody can name to better than a second.
control="$out/control.fifo"
rm -f "$control"
mkfifo "$control"
trap 'rm -f "$control"' EXIT

# **The ring buffer is perf's own default**, because `perf_event_mlock_kb` is 2048 on this box and
# `-m` above that is refused outright rather than clamped. Whether it was enough is not left to
# guesswork: a dropped sample is a hole in the profile and perf counts them.
record=(perf record --delay=-1 --control="fifo:$control" -o "$data")

if [ "$mode" = offcpu ]; then
    # `dummy` never fires. What it is there for is to give perf an event to attach to, so the only
    # samples in the file are the BPF profiler's — a `task-clock` alongside would put two events in
    # one file, and `perf report` has no way to be asked for one of them.
    #
    # A millisecond, against a default of five hundred: half a second is thirty frames, so the
    # default would not see a single one of the waits this renderer is made of.
    record+=(-e dummy --off-cpu --off-cpu-thresh 1 --call-graph fp)
elif [ "$unwind" = dwarf ]; then
    # The default 8 KiB of stack per sample truncates OpenMW's deeper traversals, and a truncated
    # DWARF unwind is worse than a frame-pointer one: it looks complete and stops in the middle.
    record+=(-e task-clock -F "$freq" --call-graph dwarf,32768)
else
    # **`task-clock` rather than `cycles`, because this machine's CPU is hybrid.** `cycles` resolves
    # to `cpu_core/cycles/` and `cpu_atom/cycles/` — two events, two reports, and a thread that
    # migrated between a P core and an E core split across both. `task-clock` is one software event
    # on every core, and it counts nanoseconds, which is what a frame budget is denominated in.
    record+=(-e task-clock -F "$freq" --call-graph fp)
fi

bench=("$build/openmw-rtxtool" bench --validation=false "${place[@]}"
       "--perf-control=$control" "${extra[@]}")

cd "$build"

if [ "$mode" = offcpu ]; then
    # Off-CPU sampling is BPF, and BPF here is root's — `unprivileged_bpf_disabled` is 2. So the
    # harness runs as the user, where it has a home directory, a Wayland socket and a GPU, and perf
    # attaches to it from root. `perf record -p` exits by itself when its target does.
    #
    #   sudo setcap cap_perfmon,cap_bpf,cap_sys_ptrace+ep "$(command -v perf)"
    #
    # removes the sudo, at the price of a system change that the next perf upgrade undoes.
    echo "profile: off-CPU sampling needs root for BPF — perf runs under sudo, the harness does not"
    sudo -v

    "${bench[@]}" 2>&1 | tee "$out/bench.txt" &
    harness=$!

    # The harness itself, not the pipeline it is the head of. It has a cell to read before it
    # reaches the first frame it measures, and that is the second perf has to attach in.
    sleep 0.5
    target="$(pgrep -n -x openmw-rtxtool || true)"
    [ -n "$target" ] || { echo "profile: the harness did not start" >&2; exit 1; }

    sudo "${record[@]}" -p "$target" &
    recorder=$!

    wait "$harness"
    wait "$recorder" || true
    sudo chown "$(id -u):$(id -g)" "$data"
else
    "${record[@]}" -- "${bench[@]}" 2>&1 | tee "$out/bench.txt"
fi

# What the run itself said it measured, which is exactly what the recording is bounded to. Every
# figure below is per second of that window, so it means the same thing whether the profile covers
# one place or six.
wall="$(awk '/frames in .* s / { total += $4 } END { printf "%.4f", total }' "$out/bench.txt")"
frames="$(awk '/frames in .* s / { total += $1 } END { print total + 0 }' "$out/bench.txt")"

# **`--no-inline` throughout.** perf resolves the inline stack at a return address, and at this
# optimisation level that address usually lands in whatever was inlined *after* the call — so a
# chain through `placeScene` comes back as `~basic_string`, `_M_dispose`, `_M_is_local`. Real
# symbols and a chain that can be read beat inline names that name the wrong thing.
common=(-i "$data" --stdio --no-inline)

summary="$(perf report "${common[@]}" -g none --sort dso 2>/dev/null || true)"
lost="$(printf '%s' "$summary" | awk '/Total Lost Samples/ { print $NF; exit }')"
nanoseconds="$(printf '%s' "$summary" | awk '/Event count/ { print $NF; exit }')"

# Last run's reports go before this one's are written, so a file left behind by a mode or a perf
# version that no longer writes it cannot be read as part of this profile.
rm -f "$out/$slug"-*.txt "$out/$slug.folded" "$out/$slug.svg"

printf '%s\n' "$summary" > "$out/$slug-libraries.txt"
perf report "${common[@]}" --no-children -g none --percent-limit 0.3 > "$out/$slug-self.txt" 2>/dev/null
perf report "${common[@]}" --children -g none --sort symbol --percent-limit 0.5 \
    > "$out/$slug-total.txt" 2>/dev/null
perf report "${common[@]}" --no-children -g graph,2,caller --percent-limit 1 \
    > "$out/$slug-callers.txt" 2>/dev/null

# What `-g1` is in the release build for. A line resolves what a symbol cannot: the hottest entry
# after `Group::traverse` is an address in libc until the line tables say `memmove-vec-unaligned`.
perf report "${common[@]}" --no-children -g none --sort srcline --percent-limit 0.5 \
    > "$out/$slug-lines.txt" 2>/dev/null

# A flame graph if something on this box can fold a stack, and a line saying what to install if not.
# Nothing here depends on one: the four reports above are the same data, and the callers file is the
# same shape read the other way up.
if command -v inferno-collapse-perf >/dev/null; then
    perf script -i "$data" --no-inline 2>/dev/null | inferno-collapse-perf > "$out/$slug.folded"
    inferno-flamegraph --title "$slug" < "$out/$slug.folded" > "$out/$slug.svg"
elif command -v stackcollapse-perf.pl >/dev/null; then
    perf script -i "$data" --no-inline 2>/dev/null | stackcollapse-perf.pl > "$out/$slug.folded"
    flamegraph.pl --title "$slug" < "$out/$slug.folded" > "$out/$slug.svg"
fi

# The symbol column, narrowed to something a terminal can hold. Templates make a C++ symbol as long
# as it likes and the part that identifies it is at the front.
narrow() {
    awk -v width=86 '{ print (length($0) > width ? substr($0, 1, width - 1) "…" : $0) }'
}

echo
echo "profile: $frames frames over ${wall} s"

if [ -n "$lost" ] && [ "$lost" != 0 ]; then
    echo "  $lost samples lost — the ring buffer overflowed, lower --freq"
fi

if [ -n "$nanoseconds" ]; then
    label="on-CPU  "
    unit="cores busy, all the time"
    [ "$mode" = offcpu ] && { label="blocked "; unit="thread-seconds of waiting per second"; }
    awk -v ns="$nanoseconds" -v wall="$wall" -v label="$label" -v unit="$unit" \
        'BEGIN { printf "  %s %6.2f %s\n", label, ns / 1e9 / wall, unit }'
fi

# **Pass-through frames come out.** A frame that spent nothing itself and cost what the frame above
# it cost is a link in a chain, not a place the time went — `_start` through `wrapApplication` to
# `run` is six rows of the same number. Dropping them starts the list where the cost first divides,
# and keeps every frame that either did work or is where a chain forked.
echo
echo "  by total time — self and everything it called:"
awk '/^ +[0-9]/ {
        children = $1; sub(/%/, "", children)
        self = $2; sub(/%/, "", self)
        if (seen++ == 0) above = children
        link = (self + 0 == 0) && (above - children < 0.5)
        above = children
        if (link) next
        if (shown++ >= 12) exit
        symbol = substr($0, index($0, "] ") + 2); sub(/ {2,}.*$/, "", symbol)
        printf "    %7s %7s  %s\n", $1, $2, symbol
     }' "$out/$slug-total.txt" | narrow

echo
echo "  by self time:"
awk '/^ +[0-9]/ && shown++ < 12 {
        symbol = substr($0, index($0, "] ") + 2); sub(/ {2,}.*$/, "", symbol)
        printf "    %7s  %-30s %s\n", $1, $3, symbol
     }' "$out/$slug-self.txt" | narrow

echo
echo "  by source line:"
awk '/^ +[0-9]/ && shown++ < 8 { printf "    %7s  %s\n", $1, $2 }' "$out/$slug-lines.txt"

echo
echo "  by library — of the whole stack, and of the leaf:"
awk '/^ +[0-9]/ && shown++ < 10 { printf "    %7s %7s  %s\n", $1, $2, $3 }' "$out/$slug-libraries.txt"

echo
for file in "$out/$slug"*.txt "$out/$slug.svg" "$out/bench.txt"; do
    [ -e "$file" ] && echo "  ${file#"$root"/}" || true
done
[ -e "$out/$slug.svg" ] || echo "  (no flame graph: pacman -S inferno)"
echo "  apps/rtxtool/profile.sh --tui   to walk the call graph"
