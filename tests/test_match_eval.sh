#!/bin/sh

set -eu

evaluator=$1
build_dir=$2
tmpdir=$(mktemp -d "$build_dir/fpc-match-eval.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

make_set() {
	label=$1
	identity=$2
	for sample in 1 2 3 4; do
		awk -v identity="$identity" '
			BEGIN {
				printf "P5\n# synthetic evaluator smoke test\n112 88\n255\n"
				for (y = 0; y < 88; y++) {
					for (x = 0; x < 112; x++) {
						phase = x * 0.34 + 5 * sin(y * 0.045 + identity * 0.7)
							+ 1.3 * sin(x * 0.035 + y * 0.027 + identity)
						noise = (x * 37 + y * 53 + identity * 71) % 17 - 8
						value = int(128 + cos(phase) * 42 + noise)
						printf "%c", value
					}
				}
			}' > "$tmpdir/$label-$sample.pgm"
	done
}

make_set a 1
make_set b 9
output=$("$evaluator" --frames 4 \
	--finger "synthetic-a:$tmpdir/a-*.pgm" \
	--finger "synthetic-b:$tmpdir/b-*.pgm")
printf '%s\n' "$output"
case "$output" in
	*"genuine comparisons:"*"EER "*) ;;
	*) printf '%s\n' "evaluator output is incomplete" >&2; exit 1 ;;
esac

sweep_output=$("$evaluator" --sweep \
	--finger "synthetic-a:$tmpdir/a-*.pgm" \
	--finger "synthetic-b:$tmpdir/b-*.pgm")
printf '%s\n' "$sweep_output"
case "$sweep_output" in
	*"frames=4 EER="*"frames=16 EER="*) ;;
	*) printf '%s\n' "evaluator sweep output is incomplete" >&2; exit 1 ;;
esac