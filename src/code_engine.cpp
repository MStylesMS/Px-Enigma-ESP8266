// code_engine.cpp — pure-logic code engine (no Arduino, no MQTT).
#include "code_engine.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>

namespace code_engine {

static uint8_t s_digit_order[6] = {1, 2, 3, 4, 5, 6};

void reset_digit_order() {
    for (uint8_t i = 0; i < 6; ++i) s_digit_order[i] = (uint8_t)(i + 1);
}

bool set_digit_order(const uint8_t order[6]) {
    bool seen[6] = {false};
    for (uint8_t i = 0; i < 6; ++i) {
        uint8_t v = order[i];
        if (v < 1 || v > 6 || seen[v - 1]) return false;
        seen[v - 1] = true;
    }
    for (uint8_t i = 0; i < 6; ++i) s_digit_order[i] = order[i];
    return true;
}

static uint32_t ordered_code_int(uint32_t code_int) {
    char raw[7];
    snprintf(raw, sizeof(raw), "%06u", (unsigned)(code_int % 1000000u));

    char d[7];
    for (uint8_t i = 0; i < 6; ++i) d[i] = raw[s_digit_order[i] - 1];
    d[6] = '\0';
    return (uint32_t)strtoul(d, nullptr, 10);
}

static void format_plain_code(uint32_t code_int, char* dst) {
    uint32_t v = code_int % 1000000u;
    uint32_t hi = v / 10000u;
    uint32_t mid = (v / 100u) % 100u;
    uint32_t lo  = v % 100u;
    snprintf(dst, 9, "%02u-%02u-%02u",
             (unsigned)hi, (unsigned)mid, (unsigned)lo);
}

// ---------------------------------------------------------------------------
// Wire-format helpers
// ---------------------------------------------------------------------------

void format_code(uint32_t code_int, char* dst) {
    uint32_t v = ordered_code_int(code_int);
    char d[7];
    snprintf(d, sizeof(d), "%06u", (unsigned)v);
    snprintf(dst, 9, "%c%c-%c%c-%c%c", d[0], d[1], d[2], d[3], d[4], d[5]);
}

// Remove all hyphens and spaces from `src`, write digits only to `out`.
// Returns number of digit chars written.
static size_t strip_separators(const char* src, char* out, size_t out_size) {
    size_t n = 0;
    for (; *src && n < out_size - 1; ++src) {
        if (*src >= '0' && *src <= '9') out[n++] = *src;
        else if (*src == '-' || *src == ' ') continue;
        else return 0;   // illegal character
    }
    out[n] = '\0';
    return n;
}

bool parse_target(const char* s, uint32_t* out) {
    if (!s || !out) return false;

    char digits[8];
    size_t n = strip_separators(s, digits, sizeof(digits));
    if (n == 0 || n > 6) return false;

    // Left-pad to 6 with leading zeros — "123" → "000123".
    // Then parse as uint32.
    char padded[7];
    size_t pad = 6 - n;
    for (size_t i = 0; i < pad; ++i) padded[i] = '0';
    for (size_t i = 0; i < n;   ++i) padded[pad + i] = digits[i];
    padded[6] = '\0';

    char* end = nullptr;
    unsigned long v = strtoul(padded, &end, 10);
    if (end != padded + 6) return false;
    if (v > 999999u)       return false;

    *out = (uint32_t)v;
    return true;
}

// ---------------------------------------------------------------------------
// CodeEngine
// ---------------------------------------------------------------------------

CodeEngine::CodeEngine() {
    memset(&cb_, 0, sizeof(cb_));
    memset(&s_,  0, sizeof(s_));
    prev_bits_ = 0xFFFFFFFFu;   // force a tick() fire on first call
}

void CodeEngine::begin(const char* mode_str, bool has_target, uint32_t target_int,
                       const Callbacks& cb) {
    cb_ = cb;
    s_.latched    = false;
    s_.solved     = false;
    s_.code_int   = 0;
    s_.code_bits  = 0;
    s_.code_str[0] = '\0';
    prev_bits_ = 0xFFFFFFFFu;

    s_.mode = (mode_str && strcmp(mode_str, "latching") == 0)
              ? Mode::Latching : Mode::Live;

    s_.has_target    = has_target;
    s_.target_int    = has_target ? (target_int % 1000000u) : 0u;
    if (has_target) format_plain_code(s_.target_int, s_.target_str);
    else             s_.target_str[0] = '\0';
}

// ---------------------------------------------------------------------------

void CodeEngine::fire_code_changed(uint32_t bits) {
    s_.code_bits = bits;
    s_.code_int  = ordered_code_int(derive_code_int(bits));
    format_code(derive_code_int(bits), s_.code_str);

    if (cb_.on_code_changed)
        cb_.on_code_changed(s_.code_int, s_.code_bits, s_.code_str, cb_.user);
}

void CodeEngine::check_target_match(uint32_t old_int, uint32_t new_int) {
    if (!s_.has_target) return;

    bool was_matched = (old_int == s_.target_int);
    bool now_matched = (new_int == s_.target_int);
    if (was_matched == now_matched) return;

    if (s_.mode == Mode::Live) {
        if (now_matched) {
            s_.solved = true;
            if (cb_.on_code_solved)
                cb_.on_code_solved(s_.code_int, s_.code_bits, s_.code_str, cb_.user);
        } else {
            s_.solved = false;
            if (cb_.on_code_unsolved)
                cb_.on_code_unsolved(s_.code_int, s_.code_bits, s_.code_str, cb_.user);
        }
    } else {
        // Latching mode: first solve only.
        if (now_matched && !s_.latched) {
            s_.latched = true;
            if (cb_.on_solve)
                cb_.on_solve(s_.code_int, s_.code_bits, s_.code_str, cb_.user);
        }
    }
}

void CodeEngine::tick(uint32_t matrix_bits) {
    if (matrix_bits == prev_bits_) return;
    uint32_t old_bits = prev_bits_;
    prev_bits_ = matrix_bits;

    // In LATCHED, update code_bits for observability (§5.2) but don't
    // fire code_changed / target-match events.
    if (s_.latched) {
        s_.code_bits = matrix_bits;
        return;
    }

    uint32_t old_int = (old_bits == 0xFFFFFFFFu)
                       ? 0xFFFFFFFFu
                       : ordered_code_int(derive_code_int(old_bits));
    fire_code_changed(matrix_bits);
    check_target_match(old_int, s_.code_int);
}

bool CodeEngine::set_target(bool has_target, uint32_t target_int) {
    uint32_t cur_int = s_.code_int;
    bool was_solved  = s_.solved;

    s_.has_target = has_target;
    s_.target_int = has_target ? (target_int % 1000000u) : 0u;
    if (has_target) format_plain_code(s_.target_int, s_.target_str);
    else             s_.target_str[0] = '\0';

    // Re-evaluate solved/latched state against the new (or cleared) target.
    if (!has_target) {
        s_.solved = false;
        // Clearing target exits LATCHED silently (no on_unlatch — the
        // target is gone, there's nothing to un-latch from).
        s_.latched = false;
        return true;
    }

    // If latching and already latched, a new target clears the latch so
    // play can begin fresh.
    if (s_.mode == Mode::Latching && s_.latched) {
        s_.latched = false;
        if (cb_.on_unlatch) cb_.on_unlatch(cb_.user);
    }

    bool now_matched = (cur_int == s_.target_int);

    // Spec §11.2: setTarget "emits code_solved (live) or solve (latching)
    // immediately if the current code already matches."
    if (s_.mode == Mode::Live) {
        s_.solved = now_matched;
        if (now_matched && !was_solved) {
            if (cb_.on_code_solved)
                cb_.on_code_solved(s_.code_int, s_.code_bits, s_.code_str, cb_.user);
        }
    } else { // Latching
        if (now_matched) {
            s_.latched = true;
            if (cb_.on_solve)
                cb_.on_solve(s_.code_int, s_.code_bits, s_.code_str, cb_.user);
        }
    }
    return true;
}

void CodeEngine::set_mode(Mode m) {
    if (m == s_.mode) return;

    Mode old = s_.mode;
    s_.mode = m;

    if (old == Mode::Latching && s_.latched) {
        // Switching out of latching while latched → clear latch silently
        // per spec: "mode switch clears latch".
        s_.latched = false;
        // No on_unlatch callback for a mode-switch-induced clear.
    }

    // Re-evaluate live solved state for the new mode.
    if (m == Mode::Live) {
        s_.solved = s_.has_target && (s_.code_int == s_.target_int);
    } else {
        // Switching into latching: clear solved flag (latching uses latched).
        s_.solved = false;
    }
}

void CodeEngine::reset() {
    if (!s_.latched) return;
    s_.latched = false;
    if (cb_.on_unlatch) cb_.on_unlatch(cb_.user);
    // After reset, the engine is ACTIVE. Force a re-evaluation on the next
    // tick() by resetting prev_bits_ so tick() sees a "change" and fires
    // code_changed with the current live matrix state.
    prev_bits_ = 0xFFFFFFFFu;
}

} // namespace code_engine
