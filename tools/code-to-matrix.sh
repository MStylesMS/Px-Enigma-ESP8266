#!/bin/sh
# tools/code-to-matrix.sh
#
# Convert a six-digit Enigma code (XX-YY-ZZ) into required switch states.
#
# This script requires a switch layout file (typically downloaded from the prop)
# so mapping math matches the running firmware/UI.
#
# Usage:
#   ./tools/code-to-matrix.sh <switch_layout.json> [CODE]
#
# If CODE is omitted, interactive prompt is used.

set -eu

README_HINT="See tools/README.md for detailed instructions."

err() { printf '%s\n' "$*" >&2; }

die_cfg() {
    err "Error: $*"
    err "$README_HINT"
    exit 1
}

ask() {
    _prompt="$1"
    _default="$2"
    printf '%s [%s]: ' "$_prompt" "$_default" >&2
    IFS= read -r _ans || _ans=""
    if [ -z "$_ans" ]; then
        printf '%s' "$_default"
    else
        printf '%s' "$_ans"
    fi
}

is_uint() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

json_get_array_csv() {
    _k="$1"
    printf '%s' "$JSON_MIN" | sed -n "s/.*\"$_k\":\[\([^]]*\)\].*/\1/p"
}

json_get_obj_csv() {
    _k="$1"
    printf '%s' "$JSON_MIN" | sed -n "s/.*\"$_k\":{\([^}]*\)}.*/\1/p"
}

json_get_int() {
    _k="$1"
    printf '%s' "$JSON_MIN" | sed -n "s/.*\"$_k\":\([0-9][0-9]*\).*/\1/p"
}

arr_get() {
    _csv="$1"
    _idx="$2"
    printf '%s\n' "$_csv" | awk -F',' -v idx="$_idx" '
    {
      n=split($0, a, ",");
      if (idx < 0 || idx >= n) exit 1;
      gsub(/^[ ]+|[ ]+$/, "", a[idx+1]);
      gsub(/\"/, "", a[idx+1]);
      print a[idx+1];
    }'
}

obj_lookup() {
    _obj_csv="$1"
    _key="$2"
    printf '%s\n' "$_obj_csv" | sed -n "s/.*\"$_key\":\([0-9][0-9]*\).*/\1/p"
}

ensure_perm_0_n_minus_1() {
    _csv="$1"
    _n="$2"
    printf '%s\n' "$_csv" | awk -F',' -v n="$_n" '
    {
      c=split($0, a, ",");
      if (c != n) exit 1;
      for (i=1; i<=c; i++) {
        gsub(/^[ ]+|[ ]+$/, "", a[i]);
        gsub(/\"/, "", a[i]);
        if (a[i] !~ /^[0-9]+$/) exit 1;
        v=a[i]+0;
        if (v < 0 || v >= n) exit 1;
        if (seen[v]) exit 1;
        seen[v]=1;
      }
    }'
}

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    die_cfg "usage: ./tools/code-to-matrix.sh <switch_layout.json> [CODE]"
fi

LAYOUT_FILE="$1"
[ -r "$LAYOUT_FILE" ] || die_cfg "layout file '$LAYOUT_FILE' is missing or unreadable"

JSON_MIN=$(tr -d '\n\r\t ' < "$LAYOUT_FILE")
[ -n "$JSON_MIN" ] || die_cfg "layout file '$LAYOUT_FILE' is empty"

ROWS=$(json_get_int "rows")
COLS=$(json_get_int "cols")
UI_ROWS=$(json_get_int "ui_rows")
UI_COLS=$(json_get_int "ui_cols")
SWITCH_COUNT=$(json_get_int "switch_count")

[ -n "$ROWS" ] || die_cfg "layout missing required integer key: rows"
[ -n "$COLS" ] || die_cfg "layout missing required integer key: cols"
[ -n "$UI_ROWS" ] || UI_ROWS="$ROWS"
[ -n "$UI_COLS" ] || UI_COLS="$COLS"
[ -n "$SWITCH_COUNT" ] || SWITCH_COUNT=$(( UI_ROWS * UI_COLS ))

ROW_GPIOS=$(json_get_array_csv "row_gpios")
COL_GPIOS=$(json_get_array_csv "col_gpios")
PROP_ROW_TO_SCAN_COL=$(json_get_array_csv "prop_row_to_scan_col")
PROP_COL_TO_SCAN_ROW=$(json_get_array_csv "prop_col_to_scan_row")
ROW_GPIO_TO_A=$(json_get_obj_csv "row_gpio_to_a")
COL_GPIO_TO_B=$(json_get_obj_csv "col_gpio_to_b")

[ -n "$ROW_GPIOS" ] || die_cfg "layout missing required array key: row_gpios"
[ -n "$COL_GPIOS" ] || die_cfg "layout missing required array key: col_gpios"
[ -n "$ROW_GPIO_TO_A" ] || die_cfg "layout missing required object key: row_gpio_to_a"
[ -n "$COL_GPIO_TO_B" ] || die_cfg "layout missing required object key: col_gpio_to_b"
[ -n "$PROP_ROW_TO_SCAN_COL" ] || PROP_ROW_TO_SCAN_COL="0,1,2,3"
[ -n "$PROP_COL_TO_SCAN_ROW" ] || PROP_COL_TO_SCAN_ROW="0,1,2,3,4"

if [ "$ROWS" -lt 1 ] || [ "$ROWS" -gt 20 ] || [ "$COLS" -lt 1 ] || [ "$COLS" -gt 20 ]; then
    die_cfg "layout rows/cols must be in 1..20"
fi

TOTAL_BITS=$(( ROWS * COLS ))
if [ "$TOTAL_BITS" -lt 1 ] || [ "$TOTAL_BITS" -gt 20 ]; then
    die_cfg "layout rows*cols must be in 1..20 (got $TOTAL_BITS)"
fi

TOTAL_SWITCHES=$(( UI_ROWS * UI_COLS ))
if [ "$TOTAL_SWITCHES" -lt 1 ] || [ "$TOTAL_SWITCHES" -gt 20 ]; then
    die_cfg "layout ui_rows*ui_cols must be in 1..20 (got $TOTAL_SWITCHES)"
fi

ensure_perm_0_n_minus_1 "$PROP_ROW_TO_SCAN_COL" "$UI_ROWS" || die_cfg "prop_row_to_scan_col must be a permutation of 0..$((UI_ROWS-1))"
ensure_perm_0_n_minus_1 "$PROP_COL_TO_SCAN_ROW" "$UI_COLS" || die_cfg "prop_col_to_scan_row must be a permutation of 0..$((UI_COLS-1))"

if [ "$#" -eq 2 ]; then
    CODE_INPUT="$2"
else
    CODE_INPUT=$(ask "Code (XX-YY-ZZ)" "00-00-00")
fi

CODE_INPUT=$(printf '%s' "$CODE_INPUT" | tr -d ' \t')
case "$CODE_INPUT" in
    *-*) CODE_DIGITS=$(printf '%s' "$CODE_INPUT" | tr -d '-') ;;
    *)   CODE_DIGITS="$CODE_INPUT" ;;
esac

if ! is_uint "$CODE_DIGITS"; then
    err "Error: code must be digits (and optional hyphens), got '$CODE_INPUT'"
    exit 1
fi

while [ "${#CODE_DIGITS}" -lt 6 ]; do
    CODE_DIGITS="0$CODE_DIGITS"
done

if [ "${#CODE_DIGITS}" -gt 6 ]; then
    err "Error: code must be 6 digits or fewer, got '${#CODE_DIGITS}' digits"
    exit 1
fi

CODE_INT=$(printf '%s' "$CODE_DIGITS" | sed 's/^0*//')
[ -z "$CODE_INT" ] && CODE_INT=0
if [ "$CODE_INT" -gt 999999 ]; then
    err "Error: code value $CODE_INT exceeds 999999"
    exit 1
fi

CODE_PRETTY=$(printf '%06d' "$CODE_INT" | sed 's/\(..\)\(..\)\(..\)/\1-\2-\3/')
MASK=$(( (1 << TOTAL_BITS) - 1 ))
CODE_BITS=$(( CODE_INT & MASK ))

printf '\n'
printf 'Layout file: %s\n' "$LAYOUT_FILE"
printf 'Code:        %s\n' "$CODE_PRETTY"
printf 'code_int:    %d\n' "$CODE_INT"
printf 'code_bits:   %d  (0x%X, %d bits)\n' "$CODE_BITS" "$CODE_BITS" "$TOTAL_BITS"
printf 'UI matrix:   %d cols x %d rows  (%d switches, showing first %d)\n' "$UI_COLS" "$UI_ROWS" "$TOTAL_SWITCHES" "$SWITCH_COUNT"
printf '\n'
printf 'Per-switch state (1 = closed, 0 = open):\n\n'

r=0
sw=1
flat=""
needed=""
while [ "$r" -lt "$UI_ROWS" ]; do
    c=0
    line_nums=""
    line_vals=""
    while [ "$c" -lt "$UI_COLS" ]; do
        scan_col=$(arr_get "$PROP_ROW_TO_SCAN_COL" "$r") || die_cfg "layout array index error in prop_row_to_scan_col"
        scan_row=$(arr_get "$PROP_COL_TO_SCAN_ROW" "$c") || die_cfg "layout array index error in prop_col_to_scan_row"

        row_gpio=$(arr_get "$ROW_GPIOS" "$scan_row") || die_cfg "layout array index error in row_gpios"
        col_gpio=$(arr_get "$COL_GPIOS" "$scan_col") || die_cfg "layout array index error in col_gpios"

        a=$(obj_lookup "$ROW_GPIO_TO_A" "$row_gpio")
        b=$(obj_lookup "$COL_GPIO_TO_B" "$col_gpio")
        [ -n "$a" ] || a="$scan_row"
        [ -n "$b" ] || b="$scan_col"

        bit_index=$(( a + ROWS * b ))
        bit=0
        if [ "$sw" -le "$SWITCH_COUNT" ]; then
            bit=$(( (CODE_BITS >> bit_index) & 1 ))
        fi

        line_nums=$(printf '%s%4d' "$line_nums" "$sw")
        line_vals=$(printf '%s%4d' "$line_vals" "$bit")

        if [ "$sw" -le "$SWITCH_COUNT" ]; then
            if [ -z "$flat" ]; then
                flat="$bit"
            else
                flat="$flat $bit"
            fi
            if [ "$bit" -eq 1 ]; then
                if [ -z "$needed" ]; then
                    needed="$sw"
                else
                    needed="$needed, $sw"
                fi
            fi
        fi

        sw=$(( sw + 1 ))
        c=$(( c + 1 ))
    done
    printf '  switch:%s\n' "$line_nums"
    printf '  state: %s\n' "$line_vals"
    printf '\n'
    r=$(( r + 1 ))
done

printf 'Flat list (switch1..switch%d): %s\n' "$SWITCH_COUNT" "$flat"
if [ -n "$needed" ]; then
    printf 'Switches that must be ON: %s\n' "$needed"
else
    printf 'Switches that must be ON: none\n'
fi
