#!/bin/sh
# Runs every test: compiled tests/<module>/test_*.c (built by make into
# $BUILD/tests/<module>/) and script tests tests/<module>/test_*.sh|py.
# Prints PASS/FAIL/SKIP per test and a summary; exits 1 on any failure.
# A script exiting 77 counts as SKIP (e.g. a required oracle or image is absent).
# TEST_FILTER (an extended regex) runs only the tests whose name matches it.
BUILD=${BUILD:-build/host}
pass=0; fail=0; skip=0
run() {
    name=$1; shift
    if [ -n "$TEST_FILTER" ] && ! echo "$name" | grep -Eq "$TEST_FILTER"; then return; fi
    out=$(timeout "${TEST_TIMEOUT:-120}" "$@" 2>&1); rc=$?
    if [ $rc -eq 0 ]; then pass=$((pass+1)); echo "PASS $name"
    elif [ $rc -eq 77 ]; then skip=$((skip+1)); echo "SKIP $name: $(echo "$out" | tail -1)"
    else fail=$((fail+1)); echo "FAIL $name (rc=$rc)"; echo "$out" | tail -30 | sed 's/^/    /'; fi
}
for t in $(find "$BUILD/tests" -type f -perm -u+x 2>/dev/null | sort); do
    run "${t#$BUILD/tests/}" "$t"
done
for t in $(find tests -name 'test_*.sh' -o -name 'test_*.py' 2>/dev/null | sort); do
    case $t in *.sh) run "$t" sh "$t";; *.py) run "$t" python3 "$t";; esac
done
echo "tests: $pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
