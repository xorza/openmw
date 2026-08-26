#!/usr/bin/env bash
# Runs the game at each `bench_*` save and prepends what it measured to a results file.
#
#   apps/rtxtool/bench.sh                 # ten seconds at each save
#   apps/rtxtool/bench.sh --seconds=30
#   apps/rtxtool/bench.sh --note="after the appendable texture array"
#
# **The game and not `openmw-rtxtool bench`, because the harness cannot see what costs the game a
# frame.** It stages a world once and re-walks only its actors, so it never pays for the whole-graph
# walk, the sweep, or a cell arriving — and those were every renderer defect worth finding.
#
# **Where to stand is a savegame's job.** A save restores the player, the camera and the world; a
# pair of coordinates restores the first and leaves the rest wherever that run happened to put it.
#
# Newest first, because the question a results file gets asked is "did that help", and the answer is
# the top two lines.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build-release"
saves="$HOME/.local/share/openmw/saves"
results="$root/.notes/bench.txt"

seconds=10
warmup=2
note=""

for arg in "$@"; do
    case "$arg" in
        --seconds=*) seconds="${arg#*=}" ;;
        --warmup=*) warmup="${arg#*=}" ;;
        --note=*) note="${arg#*=}" ;;
        --out=*) results="${arg#*=}" ;;
        *) echo "bench.sh: unknown argument $arg" >&2; exit 1 ;;
    esac
done

mapfile -t places < <(find "$saves" -name 'bench_*.omwsave' | sort)
if [ ${#places[@]} -eq 0 ]; then
    echo "bench.sh: no bench_*.omwsave under $saves — save one anywhere worth measuring" >&2
    exit 1
fi

# `release.sh build` is what owns the configure line and the flags, including the one that compiles
# the benchmark in at all; the target it builds is the harness, and what this needs is the game.
"$root/apps/rtxtool/release.sh" build > /dev/null
cmake --build "$build" -j32 --target openmw

# What the numbers were taken against, so a row that looks wrong can be traced to a tree.
commit="$(git -C "$root" rev-parse --short HEAD)"
dirty=""
git -C "$root" diff --quiet || dirty=" +changes"

printf '  %-22s %8s %8s %8s %8s\n' "place" "median" "mean" "p95" "fps"

rows=""
for save in "${places[@]}"; do
    place="$(basename "$save" .omwsave)"
    place="${place#bench_}"

    log="$(mktemp)"
    OPENMW_RTX_BENCH="${seconds}s:${warmup}s" "$build/openmw" \
        --skip-menu --load-savegame "$save" > "$log" 2>&1 || true

    # The report is a table the game logs whole; each row is `<label> median mean p95 p99 best worst`
    # behind a log timestamp, so the fields are counted from the end and the prefix cannot shift them.
    row="$(awk '/  frame ms /{ print $(NF-5), $(NF-4), $(NF-3) }' "$log" | tail -1)"
    fps="$(awk -F'—' '/frames in .* s —/{ print $2 }' "$log" | tail -1 | awk '{print $1}')"
    if [ -z "$row" ]; then
        cp "$log" "/tmp/bench-$place.log" 2>/dev/null || true
        printf '  %-22s %s\n' "$place" "no report — see /tmp/bench-$place.log"
        rows+="$(printf '  %-22s %-38s' "$place" "no report")"$'\n'
        rm -f "$log"
        continue
    fi

    rm -f "$log"

    line="$(printf '  %-22s %8s %8s %8s %8s' "$place" $row "${fps:-?}")"
    echo "$line"
    rows+="$line"$'\n'
done

mkdir -p "$(dirname "$results")"

# Prepended rather than appended, because the question a results file gets asked is "did that help"
# and the answer is the top two blocks. The heading stays pinned above them.
heading='# Frame times in the game, milliseconds. Newest first.'
existing=""
[ -f "$results" ] && existing="$(grep -v -x -F "$heading" "$results" || true)"

{
    printf '%s\n\n' "$heading"
    printf '%s  %s%s%s\n' "$(date '+%Y-%m-%d %H:%M')" "$commit" "$dirty" "${note:+  — $note}"
    printf '  %-22s %8s %8s %8s %8s\n' "place" "median" "mean" "p95" "fps"
    printf '%s' "$rows"
    [ -n "$existing" ] && printf '%s\n' "$existing"
} > "$results.new"
mv "$results.new" "$results"

echo
echo "  ${results#"$root"/}"
