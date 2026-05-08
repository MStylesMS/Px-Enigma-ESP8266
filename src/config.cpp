// config.cpp — LittleFS-backed persistent configuration for px-enigma-esp8266.
//
// Implements: mac_suffix(), load(), save(), wipe(), factory_reset_requested().
// The JSON serialization layer lives in config_json.cpp (no platform deps).
#include "config.h"
#include "log.h"

#include <ESP8266WiFi.h>
#include <LittleFS.h>

namespace cfg {

static const char* TAG      = "config";
static const char* PATH     = "/config.json";
static const char* PATH_BAD = "/config.bad.json";

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

String mac_suffix() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char buf[5];
    snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
    return String(buf);
}

static bool read_file(const char* path, String& out) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    out.reserve(f.size() + 1);
    while (f.available()) out += static_cast<char>(f.read());
    f.close();
    return true;
}

static bool write_file(const char* path, const String& s) {
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    size_t n = f.write(reinterpret_cast<const uint8_t*>(s.c_str()), s.length());
    f.close();
    return n == s.length();
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------

bool load(Config& c, bool& was_invalid) {
    was_invalid = false;

    if (!LittleFS.begin()) {
        pxlog::err(TAG, "LittleFS mount failed; using built-in defaults");
        load_defaults(c);
        was_invalid = true;
        return true;
    }

    load_defaults(c);

    if (!LittleFS.exists(PATH)) {
        pxlog::info(TAG, "no /config.json; using built-in defaults");
        return true;
    }

    String body;
    if (!read_file(PATH, body)) {
        pxlog::err(TAG, "failed to read %s; using defaults", PATH);
        was_invalid = true;
        return true;
    }

    JsonDocument doc;
    DeserializationError de = deserializeJson(doc, body);
    if (de) {
        pxlog::err(TAG, "json parse error: %s — renaming to %s and resetting to defaults",
                   de.c_str(), PATH_BAD);
        LittleFS.remove(PATH_BAD);
        LittleFS.rename(PATH, PATH_BAD);
        was_invalid = true;
        load_defaults(c);
        save(c);
        return true;
    }

    String err;
    if (!from_json(c, doc, &err)) {
        pxlog::err(TAG, "schema error: %s — renaming to %s and resetting to defaults",
                   err.c_str(), PATH_BAD);
        LittleFS.remove(PATH_BAD);
        LittleFS.rename(PATH, PATH_BAD);
        was_invalid = true;
        load_defaults(c);
        save(c);
        return true;
    }

    pxlog::info(TAG, "loaded /config.json (%u bytes)", static_cast<unsigned>(body.length()));
    return true;
}

// ---------------------------------------------------------------------------
// save
// ---------------------------------------------------------------------------

bool save(const Config& c) {
    JsonDocument doc;
    to_json(c, doc);
    String body;
    serializeJsonPretty(doc, body);
    if (!write_file(PATH, body)) {
        pxlog::err(TAG, "failed to write %s", PATH);
        return false;
    }
    pxlog::info(TAG, "saved /config.json (%u bytes)", static_cast<unsigned>(body.length()));
    return true;
}

// ---------------------------------------------------------------------------
// wipe
// ---------------------------------------------------------------------------

bool wipe() {
    bool ok = true;
    if (LittleFS.exists(PATH))     ok = LittleFS.remove(PATH)     && ok;
    if (LittleFS.exists(PATH_BAD)) ok = LittleFS.remove(PATH_BAD) && ok;
    pxlog::warn(TAG, "config wiped");
    return ok;
}

// ---------------------------------------------------------------------------
// factory_reset_requested — call once at boot, before I2C init
// ---------------------------------------------------------------------------

bool factory_reset_requested(uint32_t hold_ms) {
    pinMode(0, INPUT_PULLUP);  // GPIO0 = FLASH button
    if (digitalRead(0) != LOW) return false;
    pxlog::info(TAG, "FLASH held; watching for %u ms factory-reset window", hold_ms);
    uint32_t t0 = millis();
    while (millis() - t0 < hold_ms) {
        if (digitalRead(0) != LOW) return false;
        delay(20);
        yield();
    }
    pxlog::warn(TAG, "factory reset triggered");
    return true;
}

} // namespace cfg
