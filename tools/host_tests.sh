#!/usr/bin/env bash
#
# The host suites: every test that does not need the module.
#
# Eight projects, in this order:
#   tests/host          the rules in plain C, one function at a time
#   tests/host_drv      the drivers that talk to ESP-IDF, against a fake bus
#                       and a fake CC1101
#   tests/host_owner    the owner and its worker on FreeRTOS (linux), against a
#                       model of the module, with a fake Active Object
#   tests/host_ao       the owner's Active Object on kode-sdk's QP/C
#   tests/host_facade   the core (fck_core.h) against a fake owner
#   tests/host_standard the standard layer (fck.h: fck_standard.c, fck_events.c)
#                       over a fake core and a fake owner
#   tests/host_sys      the extended layer (fck_sys.h) on kode-sdk's QP/C, over a
#                       fake core and a fake owner; it reads the kode-sdk clone
#                       that tests/host_ao keeps, so it runs after it
#   tests/host_console  examples/console_actions's rows over an fck_sys.h that
#                       writes down what it is asked
# A list rather than a single path because kode-sdk learned it needs several -
# C allows one definition per binary, so a seam that one suite fakes and
# another tests for real gets a project of its own.
#
#   usage: tools/host_tests.sh [--sanitize]
#
# --sanitize builds every project in its own build_san directory with
# AddressSanitizer and UndefinedBehaviorSanitizer on the code under test (see
# tests/sanitize.cmake) and runs it; a memory error or undefined
# behaviour aborts the case. The plain build is left alone.
#   exit:  0 every case passed, 1 something failed or a suite shrank
#
set -uo pipefail
cd "$(dirname "$0")/.."

# "<project directory>:<elf name>", in the order they run.
PROJECTS=(
    "tests/host:fck_host_tests"
    "tests/host_drv:fck_drv_tests"
    "tests/host_owner:fck_owner_tests"
    "tests/host_ao:fck_ao_tests"
    "tests/host_facade:fck_facade_tests"
    "tests/host_standard:fck_standard_tests"
    "tests/host_sys:fck_sys_tests"
    "tests/host_console:fck_console_tests"
)

if [ -z "${IDF_PATH:-}" ]; then
    printf '\033[31mFAIL\033[0m  ESP-IDF is not activated.\n'
    printf '      . <path-to-esp-idf>/export.sh\n'
    exit 1
fi

# idf.py refuses a target that contradicts IDF_TARGET in the environment, and
# esp-idf-ci-action exports one. The target is not a choice this script offers.
unset IDF_TARGET

total_ran=0
out_dir=build
if [ "${1:-}" = "--sanitize" ]; then
    out_dir=build_san
    export FCK_SANITIZE=1
    # Leaks off: the cases keep their fixtures for the life of the process.
    export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:strict_string_checks=1"
    export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
fi

for entry in "${PROJECTS[@]}"; do
    proj="${entry%%:*}"
    elf="$proj/$out_dir/${entry##*:}.elf"

    # How many cases the sources declare, compared with how many ran: a case
    # file the link drops still leaves Unity printing OK over the rest.
    declared=$(grep -rh --include='*.c' -c 'TEST_CASE(' "$proj/cases" \
               | awk '{ n += $1 } END { print n + 0 }')

    # A build directory configured by another ESP-IDF refuses this one.
    cache="$proj/$out_dir/CMakeCache.txt"
    if [ -f "$cache" ] && ! grep -q "^PYTHON:.*=$(command -v python)$" "$cache"; then
        rm -rf "${proj:?}/$out_dir"
    fi

    if ! grep -q '^CONFIG_IDF_TARGET="linux"' "$proj/sdkconfig" 2>/dev/null; then
        if ! idf.py -C "$proj" -B "$proj/$out_dir" --preview set-target linux > "$proj/build.log" 2>&1; then
            printf '\033[31mFAIL\033[0m  %s: could not set the linux target\n\n' "$proj"
            tail -30 "$proj/build.log"
            exit 1
        fi
    fi

    # The case list is globbed and CONFIGURE_DEPENDS is not available to an
    # IDF component, so an added case would be left out of an incremental
    # build. Watch the list and reconfigure when it moves.
    stamp="$proj/$out_dir/.case-list"
    current=$(ls "$proj"/cases/test_*.c 2>/dev/null | sort)
    if [ ! -f "$stamp" ] || [ "$current" != "$(cat "$stamp" 2>/dev/null)" ]; then
        if ! idf.py -C "$proj" -B "$proj/$out_dir" reconfigure > "$proj/build.log" 2>&1; then
            printf '\033[31mFAIL\033[0m  %s: did not configure\n\n' "$proj"
            tail -30 "$proj/build.log"
            exit 1
        fi
        printf '%s' "$current" > "$stamp"
    fi

    if ! idf.py -C "$proj" -B "$proj/$out_dir" build > "$proj/build.log" 2>&1; then
        printf '\033[31mFAIL\033[0m  %s: did not build\n\n' "$proj"
        tail -30 "$proj/build.log"
        exit 1
    fi

    printf -- '-- %s --\n' "$proj"
    out=$("$elf" < /dev/null 2>&1)
    status=$?
    echo "$out"

    ran=$(echo "$out" | awk '/[0-9]+ Tests [0-9]+ Failures/ { print $1; exit }')

    if [ "$status" -ne 0 ]; then
        printf '\n\033[31mFAIL\033[0m  %s: %s case(s) failed\n' "$proj" "$status"
        exit 1
    fi
    if [ "${ran:-0}" != "$declared" ]; then
        printf '\n\033[31mFAIL\033[0m  %s: %s cases declared, %s ran - the link dropped some\n' \
               "$proj" "$declared" "${ran:-0}"
        exit 1
    fi
    total_ran=$((total_ran + ran))
done

printf '\n\033[32mok\033[0m    %s cases across %s suites, all passing\n' \
       "$total_ran" "${#PROJECTS[@]}"
exit 0
