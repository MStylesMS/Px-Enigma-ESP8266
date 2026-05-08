// display_mgr.cpp — HT16K33 display driver and renderer.
//
// Phase 9: I2C bus, Adafruit_7segment driver, legacy digit-position layout,
// signal indicator (decimal points), IDENTIFY, OFF, LATCHED blink.
//
// Legacy digit-position layout (replicated verbatim from archive/enigma/enigma.ino):
//
//   Display naming:
//     display_high (0x71) = clockDisplay1 in legacy
//     display_low  (0x70) = clockDisplay2 in legacy
//
//   For code "XX-YY-ZZ"  (XX = code/10000, YY = (code/100)%100, ZZ = code%100):
//     display_low [0]  = tens  of XX
//     display_high[1]  = ones  of XX
//     display_high[3]  = dash  (between XX and YY)
//     display_low [4]  = tens  of YY
//     display_high[0]  = ones  of YY
//     display_low [1]  = dash  (between YY and ZZ)
//     display_low [3]  = tens  of ZZ
//     display_high[4]  = ones  of ZZ
//
// A unit test (test_native_smoke.cpp Phase 9 section) pins this mapping.
#include "display_mgr.h"
#include "commands.h"
#include "log.h"

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_LEDBackpack.h>

namespace display_mgr {

static const char* TAG = "disp";

static Adafruit_7segment s_hi;   // 0x71 — display_high
static Adafruit_7segment s_lo;   // 0x70 — display_low

static bool s_hi_ok = false;
static bool s_lo_ok = false;

static uint8_t s_brightness = 1;

// Blink state for LATCHED
static uint32_t s_blink_last_toggle_ms = 0;
static bool     s_blink_on = true;

// Cache to avoid redundant I2C writes
static char     s_last_code[9]    = {};
static bool     s_last_latched    = false;
static bool     s_last_identify   = false;
static bool     s_last_is_off     = false;
static bool     s_last_mqtt_conn  = false;
static int      s_last_rssi       = 1;     // 1 = "not yet set"
static bool     s_last_low_batt   = false;
static bool     s_last_crit_batt  = false;

// LOW_BATT banner: show "LOW " on display_lo for ~3 s then resume code.
static uint32_t s_low_banner_until_ms = 0;
static bool     s_low_banner_active   = false;

// ---------------------------------------------------------------------------
// I2C probe
// ---------------------------------------------------------------------------

static bool i2c_probe(uint8_t addr) {
    Wire.beginTransmission(addr);
    return Wire.endTransmission() == 0;
}

// ---------------------------------------------------------------------------
// Raw writers
// ---------------------------------------------------------------------------

static const uint8_t SEG_DASH  = 0x40;
static const uint8_t SEG_BLANK = 0x00;
static const uint8_t SEG_8     = 0x7F; // all segments on (includes dp)
// 7-segment bitmasks for C, R, I, T, L, O, W  (for "CRIT", "LOW" banners).
static const uint8_t SEG_C = 0x39;
static const uint8_t SEG_R = 0x50;
static const uint8_t SEG_I = 0x06;
static const uint8_t SEG_T = 0x78;
static const uint8_t SEG_L = 0x38;
static const uint8_t SEG_O = 0x3F;
static const uint8_t SEG_W = 0x3E;

// Write 5 raw bytes to a display (positions 0,1,3,4 are digits; 2 is colon).
static void write_raw(Adafruit_7segment& d,
                      uint8_t p0, uint8_t p1, uint8_t p3, uint8_t p4) {
    d.writeDigitRaw(0, p0);
    d.writeDigitRaw(1, p1);
    d.writeDigitRaw(2, 0);   // colon: always off
    d.writeDigitRaw(3, p3);
    d.writeDigitRaw(4, p4);
    d.writeDisplay();
}

static void blank_display(Adafruit_7segment& d) {
    write_raw(d, SEG_BLANK, SEG_BLANK, SEG_BLANK, SEG_BLANK);
}

// Renders "CRIT" across display_high (right half) — spec §8.2.
// display_high positions: [0]=C [1]=R [3]=I [4]=T
static void render_crit() {
    if (s_lo_ok) blank_display(s_lo);
    if (s_hi_ok) {
        write_raw(s_hi, SEG_C, SEG_R, SEG_I, SEG_T);
    }
}

// Renders "LOW " — L on display_lo pos 0, O on pos 1,
// W on display_hi pos 0, rest blank.
static void render_low() {
    if (s_lo_ok) {
        s_lo.writeDigitRaw(0, SEG_L);
        s_lo.writeDigitRaw(1, SEG_O);
        s_lo.writeDigitRaw(2, 0);
        s_lo.writeDigitRaw(3, SEG_W);
        s_lo.writeDigitRaw(4, SEG_BLANK);
        s_lo.writeDisplay();
    }
    if (s_hi_ok) blank_display(s_hi);
}

// ---------------------------------------------------------------------------
// Signal indicator — decimal point bits
// ---------------------------------------------------------------------------
// Decimal points are encoded in the high bit (bit 7) of each position byte.
// We set them after writing the digit glyph by ORing in 0x80.

static uint8_t dp_mask_for_pos(bool lit) { return lit ? 0x80 : 0x00; }

// Compute how many RSSI bars should be lit (0..7) given rssi_dbm.
// rssi_dbm <= 0 is the normal connected range; > 0 means disconnected.
static int rssi_bars(int rssi_dbm, const int8_t thresholds[cfg::RSSI_THRESHOLDS]) {
    if (rssi_dbm > 0) return 0;   // disconnected / unknown
    for (int i = 0; i < (int)cfg::RSSI_THRESHOLDS; ++i) {
        if (rssi_dbm >= thresholds[i]) return cfg::RSSI_THRESHOLDS - i;
    }
    return 0;
}

// Signal indicator decimal-point bit-to-position mapping (informational):
//   bit 0: display_low  pos 0  (leftmost visible position)
//   bit 1: display_high pos 1
//   bit 2: display_high pos 3
//   bit 3: display_low  pos 4
//   bit 4: display_high pos 0
//   bit 5: display_low  pos 1
//   bit 6: display_low  pos 3
//   bit 7: display_high pos 4  (MQTT connected dot)
// Applied directly in render_code() via (dp_bits >> N) & 1 checks.

// ---------------------------------------------------------------------------
// Core renderers
// ---------------------------------------------------------------------------

// Render the XX-YY-ZZ code in the legacy digit-position layout.
// dp_bits: bitmask 0..7 — which decimal point slots to light.
static void render_code(const char* code_str, uint8_t dp_bits) {
    // Parse the "XX-YY-ZZ" string — exactly 8 chars + NUL.
    // If the string is malformed, fall through to zeros.
    int xx = 0, yy = 0, zz = 0;
    if (code_str && strlen(code_str) == 8) {
        // Fast atoi on fixed positions.
        xx = (code_str[0] - '0') * 10 + (code_str[1] - '0');
        yy = (code_str[3] - '0') * 10 + (code_str[4] - '0');
        zz = (code_str[6] - '0') * 10 + (code_str[7] - '0');
    }

    // Legacy mapping (see file header):
    if (s_lo_ok) {
        s_lo.writeDigitNum(0, xx / 10, (dp_bits & (1 << 0)) != 0);
        s_lo.writeDigitRaw(1, SEG_DASH | dp_mask_for_pos((dp_bits & (1 << 5)) != 0));
        s_lo.writeDigitNum(3, zz / 10, (dp_bits & (1 << 6)) != 0);
        s_lo.writeDigitNum(4, yy / 10, (dp_bits & (1 << 3)) != 0);
        s_lo.writeDigitRaw(2, 0);  // colon off
        s_lo.writeDisplay();
    }
    if (s_hi_ok) {
        s_hi.writeDigitNum(0, yy % 10, (dp_bits & (1 << 4)) != 0);
        s_hi.writeDigitNum(1, xx % 10, (dp_bits & (1 << 1)) != 0);
        s_hi.writeDigitRaw(3, SEG_DASH | dp_mask_for_pos((dp_bits & (1 << 2)) != 0));
        s_hi.writeDigitNum(4, zz % 10, (dp_bits & (1 << 7)) != 0);
        s_hi.writeDigitRaw(2, 0);  // colon off
        s_hi.writeDisplay();
    }
}

static void render_identify() {
    if (s_lo_ok) write_raw(s_lo, SEG_8, SEG_8, SEG_8, SEG_8);
    if (s_hi_ok) write_raw(s_hi, SEG_8, SEG_8, SEG_8, SEG_8);
}

static void render_blank() {
    if (s_lo_ok) blank_display(s_lo);
    if (s_hi_ok) blank_display(s_hi);
}

// ---------------------------------------------------------------------------
// Signal indicator dp_bits calculation
// ---------------------------------------------------------------------------

static uint8_t calc_dp_bits(bool si_enabled, bool mqtt_connected,
                             int rssi_dbm,
                             const int8_t thresholds[cfg::RSSI_THRESHOLDS]) {
    if (!si_enabled) return 0;
    uint8_t bits = 0;
    int bars = rssi_bars(rssi_dbm, thresholds);
    // Dots 0..(bars-1) lit from the left
    // Per spec §8.4: dots 0..6 = RSSI bars, dot 7 = MQTT dot.
    // But the spec says 7 lit dots = ≥ -55 dBm (best signal).
    // dots 0..(bars-1) should be lit, which gives bars dots.
    // The order from spec is: 7 bars = all lit, so we fill from the left.
    for (int i = 0; i < bars && i < 7; ++i) bits |= (1 << i);
    if (mqtt_connected) bits |= (1 << 7);
    return bits;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool begin(const cfg::Config& c) {
    s_brightness = c.display_brightness;

    Wire.begin(pins::I2C_SDA, pins::I2C_SCL);

    // Probe before initialising to detect missing hardware.
    s_hi_ok = i2c_probe(i2c_addr::DISPLAY_HIGH);
    s_lo_ok = i2c_probe(i2c_addr::DISPLAY_LOW);

    if (!s_hi_ok)
        pxlog::warn(TAG, "display_high (0x%02x) not found on I2C", (unsigned)i2c_addr::DISPLAY_HIGH);
    if (!s_lo_ok)
        pxlog::warn(TAG, "display_low (0x%02x) not found on I2C", (unsigned)i2c_addr::DISPLAY_LOW);

    if (s_hi_ok) {
        s_hi.begin(i2c_addr::DISPLAY_HIGH);
        s_hi.setBrightness(s_brightness);
        s_hi.clear();
        s_hi.writeDisplay();
    }
    if (s_lo_ok) {
        s_lo.begin(i2c_addr::DISPLAY_LOW);
        s_lo.setBrightness(s_brightness);
        s_lo.clear();
        s_lo.writeDisplay();
    }

    bool both_ok = s_hi_ok && s_lo_ok;
    pxlog::info(TAG, "init: sda=%u scl=%u display_high=%s display_low=%s",
                (unsigned)pins::I2C_SDA, (unsigned)pins::I2C_SCL,
                s_hi_ok ? "ok" : "MISSING",
                s_lo_ok ? "ok" : "MISSING");
    return both_ok;
}

void show_boot_code(const char* code_str) {
    render_code(code_str, 0);
    if (code_str) strncpy(s_last_code, code_str, 8);
    s_last_code[8] = '\0';
}

void set_brightness(uint8_t b) {
    s_brightness = b;
    if (s_hi_ok) s_hi.setBrightness(b);
    if (s_lo_ok) s_lo.setBrightness(b);
}

void tick(const char* code_str, bool latched, bool identify,
          bool is_off, bool mqtt_connected, int rssi_dbm,
          bool si_enabled,
          const int8_t rssi_thresholds[cfg::RSSI_THRESHOLDS],
          bool low_batt, bool crit_batt) {

    uint32_t now = millis();

    // ---- Identify: override everything for the identify duration ----
    if (identify) {
        if (!s_last_identify) {
            // Transition into identify
            s_last_identify = true;
            render_identify();
        }
        return;
    }
    if (s_last_identify && !identify) {
        // Identify just ended — force a full redraw below.
        s_last_identify = false;
        s_last_code[0]  = '\0';
    }

    // ---- CRIT_BATT: "CRIT" banner, continuous ----
    if (crit_batt) {
        if (!s_last_crit_batt) {
            s_last_crit_batt = true;
            s_last_code[0]   = '\0';   // force redraw on recovery
            render_crit();
        }
        return;
    }
    if (s_last_crit_batt) {
        s_last_crit_batt = false;
        s_last_code[0]   = '\0';
    }

    // ---- LOW_BATT: "LOW " banner for ~3 s on entry, then resume ----
    if (low_batt && !s_last_low_batt) {
        // Transition into LOW_BATT — start the banner timer.
        s_last_low_batt       = true;
        s_low_banner_until_ms = now + LOW_BATT_BANNER_MS;
        s_low_banner_active   = true;
        render_low();
        return;
    }
    if (!low_batt) s_last_low_batt = false;
    if (s_low_banner_active) {
        if ((int32_t)(now - s_low_banner_until_ms) >= 0) {
            // Banner expired — fall through to normal code rendering.
            s_low_banner_active = false;
            s_last_code[0]      = '\0';   // force redraw
        } else {
            return;   // still showing banner
        }
    }

    // ---- OFF: blank both displays ----
    if (is_off) {
        if (!s_last_is_off) {
            s_last_is_off = true;
            render_blank();
        }
        return;
    }
    if (s_last_is_off) {
        s_last_is_off = false;
        s_last_code[0] = '\0';   // force redraw
    }

    // ---- LATCHED blink (1 Hz = 500 ms on/off) ----
    if (latched) {
        uint32_t elapsed = now - s_blink_last_toggle_ms;
        if (elapsed >= DISPLAY_BLINK_HALF_PERIOD_MS) {
            s_blink_on = !s_blink_on;
            s_blink_last_toggle_ms = now;
            if (s_blink_on) {
                render_code(code_str, 0);   // signal indicator suppressed during blink-off
            } else {
                render_blank();
            }
        }
        return;
    }
    // If we just left latched, reset blink state and force redraw.
    if (s_last_latched && !latched) {
        s_last_latched  = false;
        s_blink_on      = true;
        s_last_code[0]  = '\0';
    }
    s_last_latched = latched;

    // ---- ACTIVE: render code + optional signal indicator ----

    // Determine if any relevant display state changed.
    bool code_changed   = !code_str || (strncmp(s_last_code, code_str, 8) != 0);
    bool signal_changed = (mqtt_connected != s_last_mqtt_conn) || (rssi_dbm != s_last_rssi);

    if (!code_changed && !signal_changed) return;

    if (code_str) { strncpy(s_last_code, code_str, 8); s_last_code[8] = '\0'; }
    s_last_mqtt_conn = mqtt_connected;
    s_last_rssi      = rssi_dbm;

    // Signal indicator dots suppressed during IDENTIFY / LOW_BATT / CRIT_BATT
    // (the latter two are Phase 9b, so for now only IDENTIFY suppression applies;
    // that is already handled by the early-return gate above).
    uint8_t dp = calc_dp_bits(si_enabled, mqtt_connected, rssi_dbm, rssi_thresholds);
    render_code(code_str ? code_str : "00-00-00", dp);
}

bool sanity_check_boot_pins() {
    // At the end of setup() we check the three boot-strap pins.
    // GPIO0 and GPIO2 must be HIGH (pulled up; I2C bus idle).
    // GPIO15 must be LOW (on-board pull-down; boot from flash).
    bool gpio0_ok  = (digitalRead(pins::I2C_SDA) == HIGH);  // GPIO0
    bool gpio2_ok  = (digitalRead(pins::I2C_SCL) == HIGH);  // GPIO2
    bool gpio15_ok = (digitalRead(15) == LOW);               // GPIO15
    bool all_ok    = gpio0_ok && gpio2_ok && gpio15_ok;

    if (!gpio0_ok)
        pxlog::warn(TAG, "boot sanity: GPIO0 (SDA) is LOW — unexpected");
    if (!gpio2_ok)
        pxlog::warn(TAG, "boot sanity: GPIO2 (SCL) is LOW — unexpected");
    if (!gpio15_ok)
        pxlog::warn(TAG, "boot sanity: GPIO15 (COL0) is HIGH — unexpected (should be LOW)");
    if (all_ok)
        pxlog::info(TAG, "boot sanity: GPIO0/2/15 all OK");

    return all_ok;
}

} // namespace display_mgr
