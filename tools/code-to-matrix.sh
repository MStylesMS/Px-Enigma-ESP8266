#!/bin/sh
# tools/code-to-matrix.sh
#
# Convert a six-digit Enigma code (XX-YY-ZZ form) into a switch-matrix
# pattern. Useful for puzzle authors picking a target code without
# running the firmware.
#
# Numbering convention (matches docs/pin-mapping.md):
#   - Switch 1 is the top-left cell.
#   - Switches are numbered left-to-right, then top-to-bottom.
#   - For the default 5 columns x 4 rows layout (cols=5, rows=4),
#     bit i (0..19) of code_bits maps to switch index (i + 1).
#
# Bit layout:
#   code_int  = parsed integer value of XX-YY-ZZ (0..999999)
#   code_bits = code_int interpreted as 20 bits, where bit 0 is the
#               least-significant bit. Switch index for bit i is i+1.
#
# Usage:
#   ./tools/code-to-matrix.sh
#     -> interactive prompts (defaults to 00-00-00 and 5x4)
#
# Pure POSIX sh; no Node/Python dependency.

set -eu

# ---- helpers ----------------------------------------------------------------

err() { printf '%s\n' "$*" >&2; }

ask() {
    # ask "Prompt text" "default value" -> echoes user input or default
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

# ---- prompt for code --------------------------------------------------------

CODE_INPUT=$(ask "Code (XX-YY-ZZ)" "00-00-00")

# Strip whitespace.
CODE_INPUT=$(printf '%s' "$CODE_INPUT" | tr -d ' \t')

# Accept either XX-YY-ZZ or plain digits.
case "$CODE_INPUT" in
    *-*)
        # Hyphenated form. Concatenate the parts.
        CODE_DIGITS=$(printf '%s' "$CODE_INPUT" | tr -d '-')
        ;;
    *)
        CODE_DIGITS="$CODE_INPUT"
        ;;
esac

if ! is_uint "$CODE_DIGITS"; then
    err "Error: code must be digits (and optional hyphens), got '$CODE_INPUT'"
    exit 1
fi

# Left-pad to 6 digits.
while [ "${#CODE_DIGITS}" -lt 6 ]; do
    CODE_DIGITS="0$CODE_DIGITS"
done

if [ "${#CODE_DIGITS}" -gt 6 ]; then
    err "Error: code must be 6 digits or fewer, got '${#CODE_DIGITS}' digits"
    exit 1
fi

# Parse to integer (drop leading zeros so $((...)) doesn't go octal).
CODE_INT=$(printf '%s' "$CODE_DIGITS" | sed 's/^0*//')
[ -z "$CODE_INT" ] && CODE_INT=0

if [ "$CODE_INT" -gt 999999 ]; then
    err "Error: code value $CODE_INT exceeds 999999"
    exit 1
fi

# Re-format canonical XX-YY-ZZ (always 6 digits).
CODE_PRETTY=$(printf '%06d' "$CODE_INT" | sed 's/\(..\)\(..\)\(..\)/\1-\2-\3/')

# ---- prompt for matrix shape ------------------------------------------------

COLS_INPUT=$(ask "Matrix columns" "5")
ROWS_INPUT=$(ask "Matrix rows"    "4")

if ! is_uint "$COLS_INPUT" || ! is_uint "$ROWS_INPUT"; then
    err "Error: cols and rows must be positive integers"
    exit 1
fi

COLS=$COLS_INPUT
ROWS=$ROWS_INPUT

if [ "$COLS" -lt 1 ] || [ "$COLS" -gt 20 ]; then
    err "Error: cols must be in 1..20"
    exit 1
fi
if [ "$ROWS" -lt 1 ] || [ "$ROWS" -gt 20 ]; then
    err "Error: rows must be in 1..20"
    exit 1
fi

TOTAL=$(( COLS * ROWS ))
if [ "$TOTAL" -lt 1 ] || [ "$TOTAL" -gt 20 ]; then
    err "Error: cols*rows must be in 1..20 (got $TOTAL)"
    exit 1
fi

if [ "$TOTAL" -ne 20 ]; then
    err "Note: cols*rows = $TOTAL (not the default 20). Only the low $TOTAL bits of code_bits will be used."
fi

# ---- compute bit pattern ----------------------------------------------------

# code_bits is just code_int truncated to TOTAL bits.
MASK=$(( (1 << TOTAL) - 1 ))
CODE_BITS=$(( CODE_INT & MASK ))

# Build a 20-element array of 0/1 (only the low TOTAL bits are meaningful).
i=0
BITS=""
while [ "$i" -lt "$TOTAL" ]; do
    bit=$(( (CODE_BITS >> i) & 1 ))
    if [ -z "$BITS" ]; then
        BITS="$bit"
    else
        BITS="$BITS $bit"
    fi
    i=$(( i + 1 ))
done

# ---- output -----------------------------------------------------------------

printf '\n'
printf 'Code:       %s\n' "$CODE_PRETTY"
printf 'code_int:   %d\n' "$CODE_INT"
printf 'code_bits:  %d  (0x%X, %d bits)\n' "$CODE_BITS" "$CODE_BITS" "$TOTAL"
printf 'Matrix:     %d cols x %d rows  (%d switches)\n' "$COLS" "$ROWS" "$TOTAL"
printf '\n'
printf 'Per-switch state (1 = closed, 0 = open):\n'
printf '\n'

# Print as a grid, with switch numbers, row-major (left-to-right, top-to-bottom).
r=0
sw=1
while [ "$r" -lt "$ROWS" ]; do
    c=0
    line_nums=""
    line_vals=""
    while [ "$c" -lt "$COLS" ]; do
        idx=$(( r * COLS + c ))
        bit=$(( (CODE_BITS >> idx) & 1 ))
        # 4-char wide cell, e.g. " 12 " over "  1 ".
        line_nums=$(printf '%s%4d' "$line_nums" "$sw")
        line_vals=$(printf '%s%4d' "$line_vals" "$bit")
        sw=$(( sw + 1 ))
        c=$(( c + 1 ))
    done
    printf '  switch:%s\n' "$line_nums"
    printf '  state: %s\n' "$line_vals"
    printf '\n'
    r=$(( r + 1 ))
done

# Also emit the flat list for scripting consumers.
printf 'Flat list (switch1..switch%d): %s\n' "$TOTAL" "$BITS"
