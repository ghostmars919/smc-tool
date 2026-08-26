#!/bin/bash
# test_cli.sh - CLI argument parsing tests for smc-tool
#
# These tests verify that the smc binary handles arguments correctly.
# Tests adapt to root/non-root context: each case is always tested,
# but the expected behavior differs based on the execution context.
#
# SPDX-License-Identifier: GPL-2.0-only

SMC="./smc"
PASS=0
FAIL=0

run_test() {
    local desc="$1"
    local expected_exit="$2"
    shift 2
    local cmd="$@"

    output=$(eval "$cmd" 2>&1)
    actual_exit=$?

    if [ "$actual_exit" -eq "$expected_exit" ]; then
        echo "  PASS  $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $desc (expected exit $expected_exit, got $actual_exit)"
        echo "        output: $output"
        FAIL=$((FAIL + 1))
    fi
}

run_test_stderr() {
    local desc="$1"
    local expected_exit="$2"
    local expected_text="$3"
    shift 3
    local cmd="$@"

    stderr=$(eval "$cmd" 2>&1 >/dev/null)
    actual_exit=$?

    if [ "$actual_exit" -eq "$expected_exit" ] && echo "$stderr" | grep -q "$expected_text"; then
        echo "  PASS  $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $desc (expected exit $expected_exit, got $actual_exit)"
        echo "        expected stderr to contain: $expected_text"
        echo "        actual stderr: $stderr"
        FAIL=$((FAIL + 1))
    fi
}

run_test_stdout() {
    local desc="$1"
    local expected_exit="$2"
    local expected_text="$3"
    shift 3
    local cmd="$@"

    stdout=$(eval "$cmd" 2>/dev/null)
    actual_exit=$?

    if [ "$actual_exit" -eq "$expected_exit" ] && echo "$stdout" | grep -q "$expected_text"; then
        echo "  PASS  $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $desc (expected exit $expected_exit, got $actual_exit)"
        echo "        expected stdout to contain: $expected_text"
        echo "        actual stdout: $stdout"
        FAIL=$((FAIL + 1))
    fi
}

# Build if needed
if [ ! -f "$SMC" ]; then
    echo "Building smc..."
    make -C "$(dirname "$0")/.." smc || { echo "Build failed"; exit 1; }
fi

IS_ROOT=false
[ "$(id -u)" -eq 0 ] && IS_ROOT=true

echo "=== Option flags ==="
run_test_stdout "-h prints usage and exits 0" 0 "Usage:" $SMC -h
run_test_stdout "--help prints usage and exits 0" 0 "Usage:" $SMC --help
run_test_stdout "-V prints version and exits 0" 0 "smc-tool" $SMC -V
run_test_stdout "--version prints version and exits 0" 0 "smc-tool" $SMC --version
run_test_stdout "--help output contains commands" 0 "Commands:" $SMC --help
run_test_stdout "--help output contains get" 0 "get KEY" $SMC --help
run_test_stdout "--help output contains set" 0 "set KEY VALUE" $SMC --help
run_test_stdout "--help output contains list" 0 "list" $SMC --help

echo ""
if [ "$IS_ROOT" = true ]; then
    echo "=== Argument validation (root: expect exit 2) ==="
    run_test "no args: exit 2" 2 $SMC
    run_test "unknown cmd: exit 2" 2 $SMC foo
    run_test "get no key: exit 2" 2 $SMC get
    run_test "get short key: exit 2" 2 $SMC get AB
    run_test "get long key: exit 2" 2 $SMC get ABCDE
    run_test "set no val: exit 2" 2 $SMC set F0Mn
    run_test "set extra arg: exit 2" 2 $SMC set F0Mn 10 20
    run_test "list with arg: exit 2" 2 $SMC list foo
else
    echo "=== Argument validation (non-root: expect exit 1 + Need root) ==="
    run_test_stderr "no args: exit 1 + Need root" 1 "Need root" $SMC
    run_test_stderr "unknown cmd: exit 1 + Need root" 1 "Need root" $SMC foo
    run_test_stderr "get no key: exit 1 + Need root" 1 "Need root" $SMC get
    run_test_stderr "get short key: exit 1 + Need root" 1 "Need root" $SMC get AB
    run_test_stderr "get long key: exit 1 + Need root" 1 "Need root" $SMC get ABCDE
    run_test_stderr "set no val: exit 1 + Need root" 1 "Need root" $SMC set F0Mn
    run_test_stderr "set extra arg: exit 1 + Need root" 1 "Need root" $SMC set F0Mn 10 20
    run_test_stderr "list with arg: exit 1 + Need root" 1 "Need root" $SMC list foo
fi

echo ""
echo "=== Summary ==="
echo "  pass: $PASS"
echo "  fail: $FAIL"

[ "$FAIL" -eq 0 ]
