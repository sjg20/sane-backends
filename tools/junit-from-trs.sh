#!/bin/sh -u
# tools/junit-from-trs.sh -- convert Automake test-driver .trs files to JUnit xml
# Copyright (C) 2026  SANE Project
#
# License: GPL-3.0+
#
# Automake's parallel test harness writes one .trs file per test program,
# each containing a ":test-result: RESULT" line (PASS, FAIL, SKIP, XFAIL,
# XPASS or ERROR).  This script walks the build tree for .trs files and
# emits a coarse-grained JUnit report: one <testcase> per test program,
# with failures pulling in the first lines of the sibling .log file.
#
# Usage: tools/junit-from-trs.sh [build-dir ...] > junit_report.xml
# With no build-dir arguments, the current directory is scanned.

# Escape stdin text for safe inclusion as xml character data.
xml_escape() {
    sed \
        -e 's/&/\&amp;/g' \
        -e 's/</\&lt;/g' \
        -e 's/>/\&gt;/g'
}

# Emit one <testcase> element.  $1: name, $2: result, $3: sibling .log (optional).
emit_case() {
    name=$1
    result=$2
    log_file=$3

    case $result in
        PASS)
            printf '\n    <testcase classname="sane-backends" name="%s"/>' "$name"
            ;;
        FAIL)
            printf '\n    <testcase classname="sane-backends" name="%s">' "$name"
            printf '<failure message="test failed" type="%s">' "$result"
            if test -f "$log_file"; then
                head -n 20 "$log_file" | xml_escape
            fi
            printf '</failure></testcase>'
            ;;
        ERROR)
            printf '\n    <testcase classname="sane-backends" name="%s">' "$name"
            printf '<error message="test error" type="%s">' "$result"
            if test -f "$log_file"; then
                head -n 20 "$log_file" | xml_escape
            fi
            printf '</error></testcase>'
            ;;
        SKIP|XFAIL|XPASS)
            printf '\n    <testcase classname="sane-backends" name="%s"><skipped/></testcase>' "$name"
            ;;
        *)
            printf '\n    <testcase classname="sane-backends" name="%s">' "$name"
            printf '<error message="unknown result: %s" type="%s"/></testcase>' "$result" "$result"
            ;;
    esac
}

tests=0
passed=0
failures=0
errors=0
skipped=0
cases=''

if test "$#" -eq 0; then
    set -- .
fi

# Collect .trs files in deterministic order for reproducible output.
trs_files=$(find "$@" -name '*.trs' 2>/dev/null | LC_ALL=C sort)

for trs in $trs_files; do
    result=$(sed -n 's/^[[:space:]]*:test-result:[[:space:]]*//p' "$trs" | head -n 1)
    test -n "$result" || continue

    name=$(basename "$trs" .trs)
    log_file=$(dirname "$trs")/"$name".log

    tests=$((tests + 1))
    case $result in
        PASS)        passed=$((passed + 1)) ;;
        FAIL)        failures=$((failures + 1)) ;;
        ERROR)       errors=$((errors + 1)) ;;
        SKIP|XFAIL|XPASS) skipped=$((skipped + 1)) ;;
        *)           errors=$((errors + 1)) ;;
    esac

    cases="$cases$(emit_case "$name" "$result" "$log_file")"
done

cat << EOF
<?xml version="1.0" encoding="UTF-8"?>
<testsuites>
  <testsuite name="sane-backends" tests="$tests" failures="$failures" errors="$errors" skipped="$skipped">$cases
  </testsuite>
</testsuites>
EOF
