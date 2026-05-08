// switch_matrix.cpp — pure-logic scanner / debouncer.
//
// No Arduino, no GPIO, no millis(). All hardware access goes through the
// ScanIO interface so this compiles on the host for unit testing.
#include "switch_matrix.h"

namespace switch_matrix {

void SwitchMatrix::reset_bit_map_identity() {
    for (uint8_t i = 0; i < NUM_CELLS; ++i) bit_map_[i] = i;
}

bool SwitchMatrix::set_bit_map(const uint8_t bit_map[NUM_CELLS]) {
    bool seen[NUM_CELLS] = {false};
    for (uint8_t i = 0; i < NUM_CELLS; ++i) {
        uint8_t v = bit_map[i];
        if (v >= NUM_CELLS || seen[v]) return false;
        seen[v] = true;
    }
    for (uint8_t i = 0; i < NUM_CELLS; ++i) bit_map_[i] = bit_map[i];
    return true;
}

SwitchMatrix::SwitchMatrix()
    : io_(nullptr), debounce_(4), next_col_(0),
      s_state_(0), s_change_count_(0) {
    for (uint8_t i = 0; i < NUM_CELLS; ++i) {
        cand_value_[i] = 0;
        cand_count_[i] = 0;
    }
    reset_bit_map_identity();
}

void SwitchMatrix::begin(ScanIO& io, uint8_t debounce_samples) {
    io_ = &io;
    if (debounce_samples < 1)  debounce_samples = 1;
    if (debounce_samples > 16) debounce_samples = 16;
    debounce_ = debounce_samples;
    next_col_ = 0;
    s_state_ = 0;
    s_change_count_ = 0;
    for (uint8_t i = 0; i < NUM_CELLS; ++i) {
        cand_value_[i] = 0;
        cand_count_[i] = 0;
    }
    io_->configure();
}

bool SwitchMatrix::tick() {
    if (!io_) return false;

    const uint8_t col = next_col_;
    const uint8_t rows = io_->drive_and_read(col);

    bool flipped_any = false;
    for (uint8_t r = 0; r < NUM_ROWS; ++r) {
        const uint8_t phys_i = bit_index_for(col, r);
        const uint8_t bit_i  = bit_map_[phys_i];
        const uint8_t sample = (rows >> r) & 0x1;

        if (sample == cand_value_[phys_i]) {
            // Same candidate — accumulate, but never overflow.
            if (cand_count_[phys_i] < 0xFF) cand_count_[phys_i]++;
        } else {
            // Candidate changed — restart the debounce window.
            cand_value_[phys_i] = sample;
            cand_count_[phys_i] = 1;
        }

        if (cand_count_[phys_i] >= debounce_) {
            const uint8_t cur = (s_state_ >> bit_i) & 0x1;
            if (cur != cand_value_[phys_i]) {
                if (cand_value_[phys_i]) s_state_ |=  (1u << bit_i);
                else                    s_state_ &= ~(1u << bit_i);
                s_change_count_++;
                flipped_any = true;
            }
            // Cap the counter so a long-held cell doesn't accumulate forever
            // (purely defensive — debounce_ is small, so cap = 2*debounce_).
            if (cand_count_[phys_i] > debounce_) cand_count_[phys_i] = debounce_;
        }
    }

    next_col_++;
    if (next_col_ >= NUM_COLS) {
        next_col_ = 0;
        io_->idle();
    }
    return flipped_any;
}

} // namespace switch_matrix
