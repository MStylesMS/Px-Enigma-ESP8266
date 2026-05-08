// web_ui.cpp — HTTP server + single-page Web UI for px-enigma-esp8266.
//
// Endpoints (§13.3):
//   GET  /                  — serve index.html from LittleFS
//   GET  /api/config        — current config JSON (?reveal=1 to include secrets)
//   POST /api/config        — overlay and persist config
//   POST /api/config/reset  — factory-reset (wipe + reboot)
//   GET  /api/state         — live device snapshot
//   GET  /api/log           — last N ring-buffer lines as JSON array
//   POST /api/identify      — placeholder (display not yet initialised)
//   POST /api/reset         — placeholder (puzzle engine not yet initialised)
//   POST /api/restart       — reboot device
//   POST /update            — HTTP OTA (Phase 4); returns 501 until then
//
// Secrets redaction: wifi passwords and mqtt password are replaced with ""
// in GET /api/config unless the query string contains `reveal=1`.
#include "web_ui.h"
#include "log.h"
#include "wifi_mgr.h"
#include "ota_mgr.h"
#include "state.h"
#include "commands.h"

#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>

namespace web_ui {

static const char* TAG = "web";

static ESP8266WebServer s_server(80);
static cfg::Config*     s_cfg           = nullptr;
static bool             s_reboot_pending = false;
static uint32_t         s_reboot_at     = 0;

// ---------------------------------------------------------------------------
// Heap guard — reject requests if heap is dangerously low.
// This prevents the JSON builders / String allocations from crashing the
// firmware when the WiFi driver has consumed most of the heap.
// ---------------------------------------------------------------------------
static constexpr uint32_t HEAP_MIN_BYTES = 6000;

static bool heap_ok() {
    uint32_t free = ESP.getFreeHeap();
    if (free < HEAP_MIN_BYTES) {
        pxlog::warn(TAG, "low heap=%u — rejecting request", free);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Response helpers
// ---------------------------------------------------------------------------

static void send_json(int code, JsonDocument& doc) {
    String body;
    serializeJson(doc, body);
    s_server.send(code, "application/json", body);
}

static void send_ok() {
    JsonDocument d; d["ok"] = true; send_json(200, d);
}

static void send_err(int code, const char* msg) {
    JsonDocument d; d["ok"] = false; d["error"] = msg;
    send_json(code, d);
}

// ---------------------------------------------------------------------------
// Static file helper
// ---------------------------------------------------------------------------

static void stream_file(const char* path, const char* mime) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        // Use a fixed-size buffer to avoid heap allocation in the 404 path.
        char msg[80];
        snprintf(msg, sizeof(msg), "%s not found — run: pio run -t uploadfs", path);
        s_server.send(404, "text/plain", msg);
        return;
    }
    s_server.streamFile(f, mime);
    f.close();
}

// ---------------------------------------------------------------------------
// Secrets redaction
// ---------------------------------------------------------------------------

static void redact_config(JsonDocument& doc) {
    // Replace password fields with empty string so they are present in
    // the JSON but do not leak credentials to the browser by default.
    if (!doc["wifi"]["primary"]["password"].isNull()) doc["wifi"]["primary"]["password"] = "";
    if (!doc["wifi"]["backup"]["password"].isNull())  doc["wifi"]["backup"]["password"]  = "";
    if (!doc["wifi"]["ap"]["password"].isNull())      doc["wifi"]["ap"]["password"]      = "";
    if (!doc["mqtt"]["password"].isNull())            doc["mqtt"]["password"]            = "";
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void handle_root() {
    if (!heap_ok()) { send_err(503, "low heap"); return; }
    stream_file("/index.html", "text/html");
}

static void handle_static_css() {
    stream_file("/style.css", "text/css");
}

static void handle_static_js() {
    stream_file("/app.js", "application/javascript");
}

static void handle_get_config() {
    if (!heap_ok()) { send_err(503, "low heap"); return; }
    JsonDocument doc;
    cfg::to_json(*s_cfg, doc);
    bool reveal = s_server.hasArg("reveal") && s_server.arg("reveal") == "1";
    if (!reveal) redact_config(doc);
    send_json(200, doc);
}

static void handle_post_config() {
    if (!heap_ok()) { send_err(503, "low heap"); return; }
    if (!s_server.hasArg("plain")) { send_err(400, "empty body"); return; }
    String body = s_server.arg("plain");

    JsonDocument incoming;
    DeserializationError de = deserializeJson(incoming, body);
    if (de) { send_err(400, de.c_str()); return; }

    cfg::Config trial = *s_cfg;
    String err;
    if (!cfg::from_json(trial, incoming, &err)) { send_err(400, err.c_str()); return; }
    if (!cfg::save(trial)) { send_err(500, "save failed"); return; }

    // Detect reboot-required fields BEFORE overwriting *s_cfg.
    bool reboot = (trial.wifi_primary.ssid     != s_cfg->wifi_primary.ssid     ||
                   trial.wifi_primary.password != s_cfg->wifi_primary.password  ||
                   trial.wifi_backup.ssid      != s_cfg->wifi_backup.ssid      ||
                   trial.wifi_backup.password  != s_cfg->wifi_backup.password   ||
                   trial.ap_password           != s_cfg->ap_password            ||
                   trial.mqtt_host             != s_cfg->mqtt_host              ||
                   trial.mqtt_port             != s_cfg->mqtt_port);
    *s_cfg = trial;
    if (reboot) {
        s_reboot_pending = true;
        s_reboot_at = millis() + 1500;
    }

    JsonDocument resp;
    resp["ok"]              = true;
    resp["reboot_required"] = reboot;
    send_json(200, resp);
    pxlog::info(TAG, "config saved (reboot_required=%d)", reboot ? 1 : 0);
}

static void handle_post_config_reset() {
    cfg::wipe();
    JsonDocument resp; resp["ok"] = true; resp["reboot_required"] = true;
    send_json(200, resp);
    s_reboot_pending = true;
    s_reboot_at = millis() + 1000;
    pxlog::warn(TAG, "factory reset via /api/config/reset");
}

static void handle_get_state() {
    if (!heap_ok()) { send_err(503, "low heap"); return; }
    JsonDocument doc;
    appstate::build_state(*s_cfg, doc);
    send_json(200, doc);
}

static void handle_get_log() {
    if (!heap_ok()) { send_err(503, "low heap"); return; }
    // Return log ring buffer as a JSON array of strings (oldest → newest).
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    pxlog::each_line([](const char* line, void* ctx) {
        reinterpret_cast<JsonArray*>(ctx)->add(line);
        return true;
    }, &arr);
    send_json(200, doc);
}

static void handle_post_identify() {
    commands::identify();
    send_ok();
}

static void handle_post_reset() {
    // Placeholder — puzzle engine not yet implemented (Phase 7).
    pxlog::info(TAG, "reset requested (puzzle engine not yet initialised)");
    send_ok();
}

static void handle_post_restart() {
    send_ok();
    commands::schedule_restart(500);
    pxlog::info(TAG, "restart requested via HTTP");
}

// Serve any file that exists in LittleFS but has no explicit route registered
// (e.g. /logo.svg, /switch_layout.json, future static assets).
static const char* mime_for(const String& path) {
    if (path.endsWith(".json")) return "application/json";
    if (path.endsWith(".svg"))  return "image/svg+xml";
    if (path.endsWith(".png"))  return "image/png";
    if (path.endsWith(".ico"))  return "image/x-icon";
    if (path.endsWith(".css"))  return "text/css";
    if (path.endsWith(".js"))   return "application/javascript";
    return "application/octet-stream";
}

static void handle_not_found() {
    const String path = s_server.uri();
    if (LittleFS.exists(path)) {
        stream_file(path.c_str(), mime_for(path));
        return;
    }
    s_server.send(404, "text/plain", "not found");
}

// ---------------------------------------------------------------------------
// begin / loop
// ---------------------------------------------------------------------------

void begin(cfg::Config* c) {
    s_cfg = c;

    s_server.on("/",                  HTTP_GET,  handle_root);
    s_server.on("/index.html",        HTTP_GET,  handle_root);
    s_server.on("/style.css",         HTTP_GET,  handle_static_css);
    s_server.on("/app.js",            HTTP_GET,  handle_static_js);
    s_server.on("/api/config",        HTTP_GET,  handle_get_config);
    s_server.on("/api/config",        HTTP_POST, handle_post_config);
    s_server.on("/api/config/reset",  HTTP_POST, handle_post_config_reset);
    s_server.on("/api/state",         HTTP_GET,  handle_get_state);
    s_server.on("/api/log",           HTTP_GET,  handle_get_log);
    s_server.on("/api/identify",      HTTP_POST, handle_post_identify);
    s_server.on("/api/reset",         HTTP_POST, handle_post_reset);
    s_server.on("/api/restart",       HTTP_POST, handle_post_restart);
    s_server.onNotFound(handle_not_found);

    // Mount HTTP OTA updater on the shared server (must precede begin()).
    ota_mgr::mount_http_update(s_server, *s_cfg);

    s_server.begin();
    pxlog::info(TAG, "HTTP server up on :80");
}

void loop() {
    // Periodically log free heap to aid diagnostics.
    static uint32_t s_last_heap_log = 0;
    uint32_t now = millis();
    if (now - s_last_heap_log >= 30000) {
        s_last_heap_log = now;
        pxlog::info(TAG, "heap=%u", ESP.getFreeHeap());
    }

    s_server.handleClient();

    if (s_reboot_pending && millis() >= s_reboot_at) {
        pxlog::info(TAG, "rebooting");
        pxlog::flush();
        ESP.restart();
    }
}

bool     reboot_pending() { return s_reboot_pending; }
uint32_t reboot_at_ms()   { return s_reboot_at; }

} // namespace web_ui
