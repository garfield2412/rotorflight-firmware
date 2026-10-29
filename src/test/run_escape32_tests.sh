#!/bin/sh
# Standalone contract/core harness; optional CC and CFLAGS for sanitizers.
set -eu
task_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
test_binary=$(mktemp /tmp/escape32-session-test.XXXXXX)
trap 'rm -f "$test_binary"' EXIT HUP INT TERM
${CC:-cc} -std=c11 -Wall -Wextra -Werror -pedantic ${CFLAGS:-} \
    -I"$task_root/src/main" \
    "$task_root/src/main/io/escape32.c" \
    "$task_root/src/test/unit/escape32_session_unittest.c" \
    -o "$test_binary"
"$test_binary"
