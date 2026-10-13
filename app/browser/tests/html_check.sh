#!/bin/sh
# html_check.sh HTML_TEST [FONT.bin JIS.bin] - render tests/pages/*.html
# and compare with the expected .txt files, feeding the pages whole, byte by
# byte and in random pieces.  The *.jis.html pages (Shift_JIS) need the
# font and the table of tools/mkjis84.py.
set -e
bin=${1:?html_test binary}
dir=$(dirname "$0")/pages
fail=0
for f in "$dir"/*.html; do
    exp=${f%.html}.txt
    opt=
    case $f in
    *.jis.html)
        [ -n "$3" ] || continue
        opt="-f $2 -j $3" ;;
    esac
    for c in 32768 1 0; do
        if ! "$bin" -a -i -c $c $opt "$f" 2>/dev/null | cmp -s - "$exp"; then
            echo "FAIL $f (chunk $c)"
            "$bin" -a -i -c $c $opt "$f" 2>/dev/null | diff "$exp" - | head -20
            fail=1
        fi
    done
done
[ $fail = 0 ] && echo "html: all passed"
exit $fail
