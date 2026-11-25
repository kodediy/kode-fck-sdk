#!/usr/bin/env bash
#
# The layers, checked rather than described.
#
#   fck        what a task calls (fck.h, the standard layer: a bool and
#              fck_error()), on the core (core/include/fck_core.h: every call in
#              kode-sdk's kode_status_t), which asks the owner for everything
#   fck_sys    what an Active Object calls (fck_sys.h, the extended layer:
#              requests answered with a signal), on the same core
#   fck_aos    the owner: one task that holds the module, decides when
#   fck_drv    the drivers: a part or a protocol, nothing about who or when
#   fck_bsp    the module described: positions, bits, legs, flash maps
#
# Underneath, kode-sdk, reached through its facade and nothing deeper.
# The rule not here - one owner for the module - is the owner's to keep: J3 is
# lent once, under FCK_BSP_LOAN_TAG, and kode-sdk refuses a second loan.
#
#   usage: tools/check_layers.sh
#   exit:  0 all clear, 1 something crossed a layer
#
set -uo pipefail
cd "$(dirname "$0")/.."

fail=0
report() {   # report <rule> <explanation> <matches>
    if [ -n "$3" ]; then
        printf '\n\033[31mFAIL\033[0m  %s\n      %s\n\n%s\n' "$1" "$2" "$3"
        fail=1
    else
        printf '\033[32mok\033[0m    %s\n' "$1"
    fi
}

# Build output and fetched components are skipped, and so are lines that look
# like prose: a comment that names esp_timer_get_time() is discussing it.
scan() { grep -rn --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' \
             "$1" components \
             2>/dev/null \
           | grep -v '/build/' \
           | grep -v 'managed_components/' \
           | grep -vE ':[0-9]+: *(\*|//|/\*)' \
           | { [ $# -gt 1 ] && grep -vE "$2" || cat; }; }

# ---- 1. ESP-IDF stops at fck_drv ------------------------------------------
# Above the drivers this is C that speaks kode_status_t. soc/ and hal/ are on
# the list because the register-level headers are not named esp_*.
report "1. ESP-IDF stops at fck_drv" \
  "Something above the drivers is talking to ESP-IDF. Put it behind a driver." \
  "$(scan '#include "\(esp_\|driver/\|soc/\|hal/\)' 'components/fck_drv/')"

# ---- 2. FreeRTOS stops at the owner and the layers' own tasks --------------
# The owner is a task, and its drivers may wait on a UART; the core asks the
# owner and waits for its answer through fck_aos's calls, never its own queue.
# Two places make a task of their own because a layer needs one: fck_events.c
# (the standard layer's listeners) and fck_sys (the extended layer's executors).
report "2. FreeRTOS stops at fck_aos, fck_events.c and fck_sys" \
  "The core is waiting on its own. Ask fck_aos, which owns the module's task." \
  "$(scan '#include "freertos/' 'components/(fck_aos|fck_drv|fck_sys)/|components/fck/src/fck_events\.c:')"

# ---- 3. The module is described in one place ------------------------------
# A position, an address or a flash offset is a fact about KDFCK001. Two copies
# of a hardware fact is how a test app of Kode's came to drive SW0 on P3 alone.
report "3. The module is described in one place" \
  "Define it in fck_bsp.h and use the name everywhere else." \
  "$(scan '^#define FCK_BSP_' 'components/fck_bsp/')"

# ---- 4. The rules are plain C ----------------------------------------------
# fck_plan (the owner's rules: who the chip at 0x21 is, when to look again,
# what a command line may be), fck_drv_link (the firmwares' line protocol, shared
# by the C5's and the nRF's clients), fck_drv_nrf and fck_drv_c5 (the two
# firmwares' vocabularies), fck_drv_nmea (the GNSS receiver's sentences) and
# fck_drv_smp (the SMP the nRF's images go in with) are run as they are by
# tests/host; so are what the CC1101 hears: fck_drv_ook turns GDO0's edges into
# runs and codes, fck_drv_subghz_scan finds where a remote sends,
# fck_drv_subghz_file writes and reads both as a Kode .ksub file,
# fck_drv_subghz_modem gives the packet receiver's registers and
# fck_drv_subghz_duty keeps the hour's air time. An ESP-IDF or FreeRTOS include
# there would take them off the host.
report "4. The rules are plain C" \
  "fck_plan, fck_drv_link, _nrf, _c5, _nmea, _smp, _ook, _subghz_scan, _subghz_file, _subghz_modem and _subghz_duty include neither ESP-IDF nor FreeRTOS: they compute from the bytes, lines, edges and text they are given; the UART, the radio and the waiting belong to their caller." \
  "$(scan '#include "\(esp_\|driver/\|soc/\|hal/\|freertos/\)' \
     | grep -E 'components/fck_(drv|aos)/[a-z0-9_]+/(include|src)/fck_(plan|drv_(link|nrf|c5|nmea|smp|ook|subghz_scan|subghz_file|subghz_modem|subghz_duty))\.[ch]:')"

# ---- 5. kode-sdk through its facade ---------------------------------------
# kode.h, kode_sys.h, kode_defs.h and kode_log.h are
# kode-sdk's public surface.
# A layer header of kode-sdk (kaos, kqp, kdrv, khal, kbsp, kui) here couples
# this SDK to that SDK's insides, which change between releases.
report "5. kode-sdk through its facade" \
  "Use what kode.h exposes; ask for it in kode-sdk if it does not." \
  "$(scan '#include "\(kaos\|kqp\|kdrv_\|khal_\|kbsp_\|kui_\)')"

# ---- 6. The facade does not leak the layers below -------------------------
# What an app includes is fck.h or fck_sys.h, and neither names a driver, an
# owner type or the core: the day a driver changes, no app has to. fck_core.h is
# for the fck component and fck_sys alone (it also refuses any other, in code).
report "6. The facade does not leak the layers below" \
  "Give the facade its own type for what the app needs to see; fck_core.h is for fck and fck_sys." \
  "$({ grep -Hn '#include "\(fck_aos\|fck_core\|fck_drv_link\|fck_drv_c5\|fck_drv_nrf\|fck_drv_cc1101\|fck_drv_tca6408a\|fck_drv_nmea\|fck_mcuboot\|fck_drv_smp\)' \
         components/fck/include/*.h components/fck_sys/include/*.h 2>/dev/null
     scan '#include "fck_core.h"' 'components/fck/|components/fck_sys/'; })"

# ---- 7. QP/C stays in the Active Objects' files -------------------------------
# AO_Fck is fck_ao.c, and what its signals do is fck_aos.c; the extended layer
# (fck_sys) answers the app's Active Objects. QP/C's own headers (qpc.h,
# qp_port.h, kqp.h) are included by nothing else: the drivers, the core and the
# standard layer see no QP/C type, and tests/host_owner plays the AO behind
# fck_ao.h. (kode_sys.h, which brings QP/C in, is rule 14.)
report "7. QP/C stays in the Active Object" \
  "Put what needs QP/C in fck_ao.c, behind a call in fck_ao.h." \
  "$(scan '#include "\(qpc\|qp_port\|qp\|kqp\)\.h"' | grep -v '^components/fck_aos/fck/src/fck_ao\.c:\|^components/fck_sys/')"

# ---- 8. One name, one function ---------------------------------------------
# The components are static libraries: a function of the same name in two of
# them links without a word, and the callers of one get the other: an
# owner's call can reach a facade's function of the same name instead.
report "8. One name, one function" \
  "Give the lower layer's function its file's prefix (fck_drv_link_*, fck_aos_*...)." \
  "$(find components -name '*.c' -not -path '*/build/*' -not -path '*/managed_components/*' -print0 \
       | xargs -0 grep -HoE '^[a-z_][a-z0-9_ *]*[ *]fck_[a-z0-9_]+\(' 2>/dev/null \
       | grep -v ':static' | sed -E 's/^([^:]+):.*[ *](fck_[a-z0-9_]+)\($/\2 \1/' \
       | sort | awk '{n[$1]++; f[$1]=f[$1]" "$2} END {for (k in n) if (n[k] > 1) print k ":" f[k]}')"

# ---- 9. One console name, one row ------------------------------------------
# kode-sdk's registry keeps a firmware's console functions and variables under
# one name each, [a-z0-9_] of 1..15 and never a function's and a variable's at
# once, each described in 48 characters at most. A table with one row against
# that is refused whole, and all that says so is a line on the log: a
# function and a variable of the same name keep every variable of the table
# off the console.
console_rows() {
    for example in examples/*/; do
        find "$example" -name '*.c' -not -path '*/build/*' -not -path '*/managed_components/*' -print0 \
          | xargs -0 awk -v example="${example%/}" '
              /kode_(function|variable)_t[ \t]+[A-Za-z0-9_]+\[\][ \t]*=/ { rows = 1 }
              rows && /^\};/ { rows = 0 }
              rows {
                  line = $0
                  while (match(line, /\.name[ \t]*=[ \t]*"[^"]*"/)) {
                      name = substr(line, RSTART, RLENGTH)
                      sub(/^[^"]*"/, "", name); sub(/"$/, "", name)
                      if (name !~ /^[a-z0-9_]+$/ || length(name) > 15)
                          print FILENAME ":" FNR ": \"" name "\" is not [a-z0-9_] of 1..15"
                      if (name in first)
                          print FILENAME ":" FNR ": \"" name "\" again, first at " first[name]
                      else
                          first[name] = FILENAME ":" FNR
                      line = substr(line, RSTART + RLENGTH)
                  }
                  line = $0
                  while (match(line, /\.desc[ \t]*=[ \t]*"[^"]*"/)) {
                      desc = substr(line, RSTART, RLENGTH)
                      sub(/^[^"]*"/, "", desc); sub(/"$/, "", desc)
                      if (length(desc) > 48)
                          print FILENAME ":" FNR ": a description of " length(desc) " characters, 48 at most"
                      line = substr(line, RSTART + RLENGTH)
                  }
              }'
    done
}
report "9. One console name, one row" \
  "Rename or shorten the row: one bad row and kodeOS refuses its whole table." \
  "$(console_rows)"

# ---- 10. The target build links every call of both layers --------------------
# tests/target names each fck.h and fck_sys.h function in its ALL[] so that
# every one, and everything below it, is compiled and linked for the esp32p4
# before it is first in somebody's app. A call added to a header and not there
# is not checked.
declared_std=$(grep -oE '^[A-Za-z_][A-Za-z0-9_ ]*[ *]fck_[a-z0-9_]+\(' components/fck/include/fck.h \
               | sed -E 's/.*[ *](fck_[a-z0-9_]+)\($/\1/' | sort -u)
declared_sys=$(grep -oE '^[A-Za-z_][A-Za-z0-9_ ]*[ *]fck_(sys_[a-z0-9_]+|why_text)\(' components/fck_sys/include/fck_sys.h \
               | sed -E 's/.*[ *](fck_[a-z0-9_]+)\($/\1/' | sort -u)
declared=$(printf '%s\n%s\n' "$declared_std" "$declared_sys" | sort -u)
listed=$(grep -oE '\(const void \*\)fck_[a-z0-9_]+' tests/target/main/main.c \
         | sed -E 's/.*\)//' | sort -u)
report "10. The target build links every call of both layers (ALL[])" \
  "Add it to ALL[] in tests/target/main/main.c." \
  "$(comm -23 <(printf '%s\n' "$declared") <(printf '%s\n' "$listed"))"

# ---- 11. The any-call cases reach every call of both layers -------------------
# tests/host_owner/cases/test_15_any_call.c makes every call of the core (the
# calls of fck.h but the events), with good, NULL and out-of-range arguments, in
# any order and in any state of the module. The events are the standard layer's
# own: tests/host_standard/cases/test_events.c calls each. A call added to fck.h
# and in neither is not fuzzed.
listed_any=$({ grep -oE '^    CALL\([a-z0-9_]+,' tests/host_owner/cases/test_15_any_call.c \
                 | sed -E 's/.*\(([a-z0-9_]+),/fck_\1/'
               grep -oE 'fck_event_[a-z_]+\(' tests/host_standard/cases/test_events.c | sed -E 's/\($//'; } | sort -u)
report "11. The any-call cases reach every call of the facade" \
  "Add a CALL(...) row for it in tests/host_owner/cases/test_15_any_call.c, or a case in tests/host_standard/cases/test_events.c for an event call." \
  "$(comm -23 <(printf '%s\n' "$declared_std") <(printf '%s\n' "$listed_any"))"

# ---- 12. The standard layer speaks no kode_status_t ---------------------------
# fck.h answers a bool and fck_error() says why; kode-sdk's status codes are
# the core's and the extended layer's. A kode_status_t in fck.h would make an
# app of tasks learn a second way to read an answer.
report "12. fck.h speaks no kode_status_t" \
  "Answer a bool in fck.h; the status is the core's (fck_core.h) and fck_sys.h's." \
  "$(grep -Hn 'kode_status_t\|kode_defs\.h' components/fck/include/*.h 2>/dev/null \
       | grep -vE ':[0-9]+: *(\*|//|/\*)')"

# ---- 13. Every request of fck_sys.h takes the asking Active Object first --------
# A request is answered to the Active Object that asked, so it is the first
# argument of every call in the Requests section of fck_sys.h, and nothing but a
# request is in that section.
requests_bad=$(awk '
    /^ \* Requests *$/ && prev ~ /^\/\* =+/ { inside = 1; prev = $0; next }
    /^ \* Immediate *$/ && prev ~ /^\/\* =+/ { inside = 0 }
    { prev = $0 }
    inside && /^kode_status_t fck_sys_/ {
        getline_line = $0
        while (getline_line !~ /\)/ && (getline nxt) > 0) getline_line = getline_line nxt
        if (getline_line !~ /fck_sys_[a-z0-9_]+\(QActive \*me[,)]/) print FILENAME ":" FNR ": " $0
    }' components/fck_sys/include/fck_sys.h)
report "13. Every request of fck_sys.h takes QActive *me first" \
  "A request names the Active Object its answer is posted to: (QActive *me, ...)." \
  "$requests_bad"
requests=$(awk '
    /^ \* Requests *$/ && prev ~ /^\/\* =+/ { inside = 1; prev = $0; next }
    /^ \* Immediate *$/ && prev ~ /^\/\* =+/ { inside = 0 }
    { prev = $0 }
    inside && /^kode_status_t fck_sys_/ { sub(/\(.*/, ""); sub(/.* /, ""); print }' components/fck_sys/include/fck_sys.h | sort -u)
listed_sys=$(grep -oE 'fck_sys_[a-z0-9_]+\(me[,)]|fck_sys_[a-z0-9_]+\(a[,)]' tests/host_sys/cases/test_sys.c \
             | sed -E 's/\(.*//' | sort -u)
report "13b. The extended layer's suite asks every request (tests/host_sys)" \
  "Add a row for it in the ROW table of tests/host_sys/cases/test_sys.c." \
  "$(comm -23 <(printf '%s\n' "$requests") <(printf '%s\n' "$listed_sys"))"

# ---- 14. Only the extended layer and the owner include kode_sys.h --------------
# kode_sys.h is the Active Objects' side of kode-sdk (QP/C, the signals, the
# doors). The core, the standard layer, the drivers and the module's description
# are plain C and tasks: none of them includes it. The owner does, for the
# supply and the rest signals; fck_sys, because it answers Active Objects.
report "14. Only fck_sys and fck_aos include kode_sys.h" \
  "Keep QP/C types out of the core and the standard layer; ask the owner or fck_sys." \
  "$(scan '#include "kode_sys.h"' 'components/fck_sys/|components/fck_aos/')"

# ---- 15. fck_aos.h is the SDK's own ---------------------------------------------
# fck_aos_command() and fck_aos_command_post() hand a line to a chip without the
# gates fck_command() keeps (the radio limits, the firmware's own words). The
# header compiles only under FCK_AOS_ALLOWED, which fck, fck_sys and fck_aos
# define for their own sources (and the test suites for theirs): nothing else in
# the SDK, the examples or the C5 template includes it or defines the name.
aos_outside=$({
    scan '#include "fck_aos.h"' 'components/(fck|fck_sys|fck_aos)/'
    grep -rn --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' \
         '#include "fck_aos.h"' examples c5 firmware 2>/dev/null
    grep -rn --include='CMakeLists.txt' --include='*.cmake' --include='*.c' --include='*.h' \
         'FCK_AOS_ALLOWED' components examples c5 firmware 2>/dev/null \
      | grep -vE '^components/(fck|fck_sys|fck_aos)/CMakeLists\.txt:|^components/fck_aos/core/include/fck_aos\.h:'
} | grep -v '/build/' | grep -v 'managed_components/')
report "15. fck_aos.h is for fck, fck_sys and fck_aos" \
  "An app includes fck.h or fck_sys.h; fck_aos.h, and FCK_AOS_ALLOWED, are fck's, fck_sys's and fck_aos's." \
  "$aos_outside"

exit "$fail"
