// code_engine.h — code derivation, target matching, live/latching state machine.
//
// Phase 7: pure-logic module. No Arduino, no MQTT, no GPIO dependencies.
// All outputs are delivered through injected callbacks so the host test
// harness can observe every transition without a broker.
//
// Usage (firmware side):
//   1. code_engine::CodeEngine engine;
//   2. engine.begin(&g_config, callbacks...);
//   3. In the cooperative loop: engine.tick(g_matrix.state());
//   4. MQTT commands call engine.set_target(), engine.reset(), etc.
#pragma once

#include <stdint.h>
#include <stddef.h>

namespace code_engine {

// ---------------------------------------------------------------------------
// Wire-format helpers (host-callable, no Arduino dep)
// ---------------------------------------------------------------------------

// Format a 0..999999 integer as "XX-YY-ZZ" (always 8 chars + NUL).
// dst must be at least 9 bytes.
void format_code(uint32_t code_int, char* dst);

// Set display digit order as a 1-based permutation of [1..6].
// Example [4,2,6,1,5,3] maps raw digits d1..d6 to display d4,d2,d6,d1,d5,d3.
// Returns false if order is invalid.
bool set_digit_order(const uint8_t order[6]);

// Restore identity order [1,2,3,4,5,6].
void reset_digit_order();

// Matrix bit pattern whose ordered display code equals `target_int`.
// Inverse of ordered_code_int(bits % 1_000_000); used for target_grid in state.
uint32_t target_matrix_bits(uint32_t target_int);

// Parse a target string per functional-spec §5.4.
//   Accepts: integer-as-string ("123456"), hyphenated ("12-34-56"), bare int.
//   Returns the parsed 0..999999 value in *out.
//   Returns false for anything that does not match an accepted form.
bool parse_target(const char* s, uint32_t* out);

// Derive the displayed integer from a 20-bit matrix state:
//   displayed = raw_state mod 1,000,000
inline uint32_t derive_code_int(uint32_t bits) {
    return bits % 1000000u;
}

// ---------------------------------------------------------------------------
// Callbacks (injected at begin() time)
// ---------------------------------------------------------------------------

struct Callbacks {
    // Fired every time the debounced matrix state changes (all modes,
    // except in LATCHED — see §5.2).
    // code_int   = bits % 1,000,000
    // code_bits  = raw 20-bit matrix state
    // code_str   = "XX-YY-ZZ" (caller must copy if it needs to persist it)
    void (*on_code_changed)(uint32_t code_int, uint32_t code_bits,
                            const char* code_str, void* user) = nullptr;

    // live mode: fired when code transitions from unmatched → matched.
    // Fires each time (no single-shot guard).
    void (*on_code_solved)(uint32_t code_int, uint32_t code_bits,
                           const char* code_str, void* user) = nullptr;

    // live mode: fired when code transitions from matched → unmatched.
    void (*on_code_unsolved)(uint32_t code_int, uint32_t code_bits,
                             const char* code_str, void* user) = nullptr;

    // latching mode: fired once on the first solve (transitions to LATCHED).
    void (*on_solve)(uint32_t code_int, uint32_t code_bits,
                     const char* code_str, void* user) = nullptr;

    // Fired when the engine leaves LATCHED (via reset() or mode change).
    void (*on_unlatch)(void* user) = nullptr;

    void* user = nullptr;
};

// ---------------------------------------------------------------------------
// Puzzle modes (mirror cfg:: constants without depending on config.h)
// ---------------------------------------------------------------------------

enum class Mode : uint8_t { Live = 0, Latching = 1 };

// ---------------------------------------------------------------------------
// Engine state (visible to main.cpp and state.cpp)
// ---------------------------------------------------------------------------

struct CodeState {
    uint32_t code_int;       // bits % 1,000,000
    uint32_t code_bits;      // raw 20-bit matrix state
    char     code_str[9];    // "XX-YY-ZZ" + NUL

    bool     has_target;
    uint32_t target_int;     // valid iff has_target
    char     target_str[9];  // formatted target, valid iff has_target

    bool     latched;        // true while in LATCHED (latching mode only)
    bool     solved;         // true while code == target (live mode only)
    Mode     mode;
};

// ---------------------------------------------------------------------------
// CodeEngine
// ---------------------------------------------------------------------------

class CodeEngine {
public:
    CodeEngine();

    // Bind config parameters and callbacks. Must be called before tick().
    // `mode_str`: "live" or "latching" (same strings as cfg::PUZZLE_MODE_*).
    // `has_target` / `target_int`: from cfg::Config.
    void begin(const char* mode_str, bool has_target, uint32_t target_int,
               const Callbacks& cb);

    // Process one cooperative iteration.
    // `matrix_bits`: current debounced 20-bit matrix state.
    void tick(uint32_t matrix_bits);

    // ---- MQTT command handlers ----

    // Set or clear the target at runtime.
    // Returns false if the target string is invalid (caller emits
    // command_failed). Pass has_target=false / target_int=0 to clear.
    bool set_target(bool has_target, uint32_t target_int);

    // Change puzzle mode at runtime. If mode changes from latching→live
    // while latched, the latch is cleared silently (no on_unlatch).
    void set_mode(Mode m);

    // Reset latched state → ACTIVE. No-op in live mode or if not latched.
    // Fires on_unlatch callback.
    void reset();

    // Read-only snapshot for state.cpp and MQTT publish helpers.
    const CodeState& state() const { return s_; }

private:
    Callbacks  cb_;
    CodeState  s_;
    uint32_t   prev_bits_;  // last matrix_bits delivered to tick()

    void fire_code_changed(uint32_t bits);
    void check_target_match(uint32_t old_int, uint32_t new_int);
};

} // namespace code_engine
