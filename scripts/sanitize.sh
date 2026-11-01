#!/bin/bash
# Run the native unit tests under AddressSanitizer and UndefinedBehaviorSanitizer.
#
# MinGW on Windows has no sanitizers, so this runs in WSL (or any Linux with g++):
#   wsl -d Ubuntu-24.04 -- bash scripts/sanitize.sh
#
# Run `pio test -e native` once first so Unity and ArduinoJson are downloaded
# into .pio/libdeps/native. Any out-of-bounds access, use of uninitialized
# memory via UB, signed overflow etc. aborts the suite with a report.
set -euo pipefail
cd "$(dirname "$0")/.."

DEPS=.pio/libdeps/native
OUT=.pio/sanitize
mkdir -p "$OUT"

if [ ! -f "$DEPS/Unity/src/unity.c" ]; then
    echo "Unity not found in $DEPS, run 'pio test -e native' first" >&2
    exit 2
fi

# suite -> libraries it needs
declare -A SUITES=(
    [test_netconfig]="netconfig"
    [test_settings_codec]="settings_codec netconfig"
    [test_power_debouncer]="power_monitor"
    [test_reset_button]="power_monitor"
    [test_mains_alarm]="power_monitor"
    [test_lldp]="lldp netconfig"
    [test_snmp_ber]="snmp_ber"
    [test_snmp_message]="snmp_agent snmp_ber if_stats lldp netconfig"
    [test_snmp_agent]="snmp_agent snmp_ber if_stats lldp netconfig"
    [test_snmp_trap]="snmp_agent snmp_ber if_stats lldp netconfig"
    [test_if_stats]="if_stats"
    [test_dhcp_option]="dhcp_option"
    [test_access_control]="access_control netconfig"
    [test_web_ui]="web_ui settings_codec netconfig access_control"
    [test_tmp1075]="tmp1075"
    [test_watchdog_logic]="watchdog_logic"
    [test_dhcp6_message]="dhcp6_client"
    [test_dhcp6_frame]="dhcp6_client"
    [test_dhcp6_client]="dhcp6_client"
)

failed=0
for suite in "${!SUITES[@]}"; do
    inc="-I $DEPS/Unity/src -I $DEPS/ArduinoJson/src"
    src=""
    for lib in ${SUITES[$suite]}; do
        inc="$inc -I lib/$lib/include"
        src="$src $(ls lib/$lib/src/*.cpp)"
    done
    # shellcheck disable=SC2086
    if g++ -std=gnu++17 -g -O1 -Wall -Wextra -Werror \
           -fsanitize=address,undefined -fno-sanitize-recover=all \
           $inc "test/native/$suite/test_main.cpp" $src "$DEPS/Unity/src/unity.c" \
           -o "$OUT/$suite" && "$OUT/$suite" > "$OUT/$suite.log" 2>&1; then
        echo "PASS  $suite  ($(grep -E '^[0-9]+ Tests' "$OUT/$suite.log"))"
    else
        echo "FAIL  $suite  (see $OUT/$suite.log)"
        tail -20 "$OUT/$suite.log" 2>/dev/null || true
        failed=1
    fi
done

# Suites are the test_* folders (as for PlatformIO); others hold shared fixtures
for suite in test/native/test_*/; do
    name=$(basename "$suite")
    [ -n "${SUITES[$name]+x}" ] || { echo "MISSING  $name is not listed in scripts/sanitize.sh"; failed=1; }
done

exit $failed
