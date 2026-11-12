#!/bin/bash
# Fuzz the parsers of network and flash input with libFuzzer, under AddressSanitizer and UBSan.
#
# Needs clang (libFuzzer comes with it), so it runs in WSL or any Linux:
#   wsl -d Ubuntu-24.04 -- bash scripts/fuzz.sh [seconds per harness] [harness...]
#
# Default 60 s per harness, all harnesses (test/fuzz/fuzz_<harness>.cpp). Run
# `pio test -e native` once first so ArduinoJson is in .pio/libdeps/native.
#
# Each harness starts from the seeds of test/fuzz/make_seeds.py plus what earlier
# runs found (.pio/fuzz/corpus/<harness>, kept between runs). A crash or a broken
# check stops that harness and leaves the input in .pio/fuzz/crashes/; rerun it with
#   .pio/fuzz/bin/fuzz_<harness> .pio/fuzz/crashes/<file>
# and turn it into a native unit test before fixing (.clinerules: a bug fix starts
# with a failing test).
set -euo pipefail
cd "$(dirname "$0")/.."

SECONDS_EACH=${1:-60}
shift || true

DEPS=.pio/libdeps/native
OUT=.pio/fuzz
mkdir -p "$OUT/bin" "$OUT/obj" "$OUT/corpus" "$OUT/crashes" "$OUT/seeds"

if [ ! -d "$DEPS/ArduinoJson/src" ]; then
    echo "ArduinoJson not found in $DEPS, run 'pio test -e native' first" >&2
    exit 2
fi
command -v clang++ >/dev/null || { echo "clang++ not found (sudo apt-get install clang)" >&2; exit 2; }

# harness -> libraries it needs
declare -A HARNESSES=(
    [snmp_ber]="snmp_ber"
    [snmp_agent]="snmp_agent snmp_ber if_stats lldp netconfig"
    [http_request]="web_ui settings_codec netconfig access_control"
    [web_app]="web_ui settings_codec netconfig access_control"
    [fw_update]="fw_update fw_image monocypher"
    [dhcp]="dhcp_option"
    [dhcp6]="dhcp6_client"
    [lldp]="lldp netconfig"
    [update_client]="update_client netconfig monocypher fw_update fw_image"
    [settings]="settings_codec netconfig update_client monocypher fw_update fw_image syslog clock"
    [text_input]="netconfig syslog clock"
)

# Every harness file must be listed (as scripts/sanitize.sh does for the suites)
failed=0
for f in test/fuzz/fuzz_*.cpp; do
    name=$(basename "$f" .cpp)
    name=${name#fuzz_}
    [ -n "${HARNESSES[$name]+x}" ] || { echo "MISSING  $name is not listed in scripts/fuzz.sh"; failed=1; }
done
[ $failed -eq 0 ] || exit 1

if [ $# -gt 0 ]; then
    selected=("$@")
else
    mapfile -t selected < <(printf '%s\n' "${!HARNESSES[@]}" | sort)
fi

python3 test/fuzz/make_seeds.py "$OUT/seeds" >/dev/null

SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
for name in "${selected[@]}"; do
    [ -n "${HARNESSES[$name]+x}" ] || { echo "unknown harness $name" >&2; exit 2; }
    inc="-I $DEPS/ArduinoJson/src -I test/fuzz"
    src=""
    cobj=""
    for lib in ${HARNESSES[$name]}; do
        inc="$inc -I lib/$lib/include -I lib/$lib/src"
        src="$src $(ls lib/$lib/src/*.cpp 2>/dev/null || true)"
        # C sources (Monocypher) with coverage, for the linker to take as they are
        for c in lib/$lib/src/*.c; do
            [ -e "$c" ] || continue
            o="$OUT/obj/$name.$(basename "$c").o"
            clang -std=c99 -g -O1 -Wall -Wextra -Werror -fsanitize=fuzzer-no-link $SAN -c "$c" -o "$o"
            cobj="$cobj $o"
        done
    done
    bin="$OUT/bin/fuzz_$name"
    # shellcheck disable=SC2086
    clang++ -std=gnu++17 -g -O1 -Wall -Wextra -Werror -fsanitize=fuzzer $SAN \
        $inc "test/fuzz/fuzz_$name.cpp" $src $cobj -o "$bin"

    corpus="$OUT/corpus/$name"
    mkdir -p "$corpus"
    log="$OUT/$name.log"
    # The seeds directory is read but never written; new inputs go to the corpus
    if "$bin" "$corpus" "$OUT/seeds/$name" -max_total_time="$SECONDS_EACH" -timeout=10 -rss_limit_mb=2048 \
            -max_len=4096 -artifact_prefix="$OUT/crashes/$name-" -print_final_stats=1 > "$log" 2>&1; then
        echo "PASS  $name  ($(grep -oE 'stat::number_of_executed_units: *[0-9]+' "$log" | grep -oE '[0-9]+$') runs," \
             "$(grep -oE 'cov: [0-9]+' "$log" | tail -1) edges, corpus $(ls "$corpus" | wc -l))"
    else
        echo "FAIL  $name  (see $log)"
        grep -E "ERROR|SUMMARY|Test unit written|deadly signal" "$log" | head -8 || true
        failed=1
    fi
done

exit $failed
