// web_ui.cpp — HTTP server + single-page Web UI for px-enigma-esp8266.
//
// Endpoints (§13.3):
//   GET  /                  — serve index.html from LittleFS
//   GET  /api/config        — current config JSON (?reveal=1 to include secrets)
//   POST /api/config        — overlay and persist config
//   POST /api/config/reset  — factory-reset (wipe + reboot)
//   GET  /api/state         — live device snapshot
//   POST /api/files/upload  — upload allowlisted LittleFS files via multipart
//   GET  /api/log           — last N ring-buffer lines as JSON array
//   POST /api/identify      — placeholder (display not yet initialised)
//   POST /api/reset         — puzzle reset (clears latch, no reboot)
//   POST /api/restart       — reboot device
//   POST /api/sleep         — deep sleep (sleep indicator, wake = power cycle)
//   POST /update            — HTTP OTA (Phase 4); returns 501 until then
//
// Secrets redaction: wifi passwords and mqtt password are replaced with ""
// in GET /api/config unless the query string contains `reveal=1`.
#include "web_ui.h"
#include "http_proxy.h"
#include "log.h"
#include "wifi_mgr.h"
#include "ota_mgr.h"
#include "state.h"
#include "commands.h"

#include <ArduinoJson.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ctype.h>

namespace web_ui {

static const char* TAG = "web";

static ESP8266WebServer s_server(80);

// ESP8266WebServer drops unknown headers unless registered up front.
static const char* k_proxy_headers[] = {
    "X-Forwarded-Prefix",
    "X-Forwarded-Host",
    "X-Forwarded-Proto",
};

static cfg::Config*     s_cfg           = nullptr;
static bool             s_reboot_pending = false;
static uint32_t         s_reboot_at     = 0;
static WiFiClient       s_sse_client;
static bool             s_sse_active = false;
static uint32_t         s_sse_last_code_bits = UINT32_MAX;
static uint32_t         s_sse_last_push_ms = 0;

struct UploadState {
    bool active = false;
    bool ok = false;
    int code = 400;
    String err;
    String target;
    size_t bytes = 0;
    bool reboot_required = false;
    bool validate_json = false;
    File f;
};
static UploadState s_upload;
static const char* kUploadTmpPath = "/.upload.tmp";
static const size_t kUploadMaxBytes = 256 * 1024;
static const char* kToolsReadmeHint = " See tools/README.md for detailed instructions.";

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

static void send_err_str(int code, const String& msg) {
    JsonDocument d;
    d["ok"] = false;
    d["error"] = msg;
    send_json(code, d);
}

static String masked_dots(const String& secret) {
    String out;
    out.reserve(secret.length());
    for (size_t i = 0; i < secret.length(); ++i) out += '.';
    return out;
}

static bool json_string_is_blank(JsonVariantConst v) {
    if (v.isNull()) return false;
    const char* s = v.as<const char*>();
    if (!s) return false;
    while (*s) {
        if (!isspace((unsigned char)*s)) return false;
        ++s;
    }
    return true;
}

static void sse_send_state(const char* event_name = "state") {
    if (!s_sse_active || !s_sse_client.connected()) return;
    JsonDocument doc;
    appstate::build_state(*s_cfg, doc);
    String body;
    serializeJson(doc, body);

    s_sse_client.print("event: ");
    s_sse_client.print(event_name);
    s_sse_client.print("\n");
    s_sse_client.print("data: ");
    s_sse_client.print(body);
    s_sse_client.print("\n\n");
    s_sse_last_push_ms = millis();
}

static bool is_allowed_upload_path(const String& path) {
    return path == "/switch_layout.json" ||
           path == "/config.json" ||
           path == "/logo.png" ||
           path == "/index.html" ||
           path == "/app.js" ||
           path == "/style.css";
}

static bool path_is_json(const String& path) {
    return path.endsWith(".json");
}

static bool path_needs_restart(const String& path) {
    return path == "/switch_layout.json" || path == "/config.json";
}

static void upload_fail(int code, const String& msg) {
    s_upload.ok = false;
    s_upload.code = code;
    s_upload.err = msg + kToolsReadmeHint;
}

static bool validate_uploaded_json() {
    File rf = LittleFS.open(kUploadTmpPath, "r");
    if (!rf) return false;
    JsonDocument doc;
    DeserializationError de = deserializeJson(doc, rf);
    rf.close();
    return !de;
}

static void handle_post_file_upload() {
    if (!heap_ok()) { send_err(503, "low heap"); return; }
    if (!s_upload.active) { send_err(400, "no upload data received"); return; }
    if (!s_upload.ok) {
        send_err_str(s_upload.code, s_upload.err);
        return;
    }

    JsonDocument d;
    d["ok"] = true;
    d["path"] = s_upload.target;
    d["bytes"] = (uint32_t)s_upload.bytes;
    d["reboot_required"] = s_upload.reboot_required;
    send_json(200, d);
}

static void handle_post_file_upload_body() {
    HTTPUpload& up = s_server.upload();

    if (up.status == UPLOAD_FILE_START) {
        s_upload = UploadState{};
        s_upload.active = true;
        s_upload.ok = true;
        s_upload.code = 400;

        if (!s_server.hasArg("path")) {
            upload_fail(400, "missing multipart field: path");
            return;
        }

        String target = s_server.arg("path");
        target.trim();
        if (target.length() == 0 || target[0] != '/' || target.indexOf("..") >= 0) {
            upload_fail(400, "invalid path");
            return;
        }
        if (!is_allowed_upload_path(target)) {
            upload_fail(403, "path is not allowlisted for upload");
            return;
        }

        s_upload.target = target;
        s_upload.validate_json = path_is_json(target);
        s_upload.reboot_required = path_needs_restart(target);

        if (LittleFS.exists(kUploadTmpPath)) {
            LittleFS.remove(kUploadTmpPath);
        }
        s_upload.f = LittleFS.open(kUploadTmpPath, "w");
        if (!s_upload.f) {
            upload_fail(500, "failed to open temporary upload file");
            return;
        }
    }

    if (!s_upload.active || !s_upload.ok) {
        if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
            if (s_upload.f) s_upload.f.close();
            if (LittleFS.exists(kUploadTmpPath)) LittleFS.remove(kUploadTmpPath);
        }
        return;
    }

    if (up.status == UPLOAD_FILE_WRITE) {
        size_t next = s_upload.bytes + up.currentSize;
        if (next > kUploadMaxBytes) {
            upload_fail(413, "uploaded file exceeds size limit");
            s_upload.f.close();
            LittleFS.remove(kUploadTmpPath);
            return;
        }

        size_t wrote = s_upload.f.write(up.buf, up.currentSize);
        if (wrote != up.currentSize) {
            upload_fail(500, "write failure while receiving upload");
            s_upload.f.close();
            LittleFS.remove(kUploadTmpPath);
            return;
        }
        s_upload.bytes = next;
    } else if (up.status == UPLOAD_FILE_END) {
        s_upload.f.close();

        if (s_upload.validate_json && !validate_uploaded_json()) {
            upload_fail(400, "JSON parse failed; uploaded file appears malformed or corrupted");
            LittleFS.remove(kUploadTmpPath);
            return;
        }

        if (LittleFS.exists(s_upload.target)) {
            LittleFS.remove(s_upload.target);
        }
        if (!LittleFS.rename(kUploadTmpPath, s_upload.target)) {
            upload_fail(500, "failed to move uploaded file into place");
            LittleFS.remove(kUploadTmpPath);
            return;
        }

        pxlog::info(TAG, "uploaded file path=%s bytes=%u", s_upload.target.c_str(), (unsigned)s_upload.bytes);
    } else if (up.status == UPLOAD_FILE_ABORTED) {
        s_upload.f.close();
        LittleFS.remove(kUploadTmpPath);
        upload_fail(400, "upload aborted");
    }
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

static void stream_html(const char* path) {
    http_proxy::Ctx ctx;
    http_proxy::read(s_server, ctx);

    File f = LittleFS.open(path, "r");
    if (!f) {
        char msg[80];
        snprintf(msg, sizeof(msg), "%s not found — run: pio run -t uploadfs", path);
        s_server.send(404, "text/plain", msg);
        return;
    }

    if (!ctx.has_prefix) {
        s_server.streamFile(f, "text/html");
        f.close();
        return;
    }

    String html = f.readString();
    f.close();
    if (!http_proxy::inject_base_tag(html, ctx)) {
        s_server.send(500, "text/plain", "HTML rewrite failed");
        return;
    }
    s_server.send(200, "text/html", html);
}

// ---------------------------------------------------------------------------
// Secrets redaction
// ---------------------------------------------------------------------------

static void redact_config(JsonDocument& doc) {
    // Redact secrets while preserving length as dot placeholders so the
    // UI can indicate that a password is already configured.
    if (!doc["wifi"]["primary"]["password"].isNull()) {
        String s = doc["wifi"]["primary"]["password"].as<String>();
        doc["wifi"]["primary"]["password"] = masked_dots(s);
    }
    if (!doc["wifi"]["backup"]["password"].isNull()) {
        String s = doc["wifi"]["backup"]["password"].as<String>();
        doc["wifi"]["backup"]["password"] = masked_dots(s);
    }
    if (!doc["wifi"]["ap"]["password"].isNull()) {
        String s = doc["wifi"]["ap"]["password"].as<String>();
        doc["wifi"]["ap"]["password"] = masked_dots(s);
    }
    if (!doc["mqtt"]["password"].isNull()) {
        String s = doc["mqtt"]["password"].as<String>();
        doc["mqtt"]["password"] = masked_dots(s);
    }
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

static void handle_root() {
    if (!heap_ok()) { send_err(503, "low heap"); return; }
    stream_html("/index.html");
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

    // Treat blank/whitespace password values as "leave unchanged".
    JsonVariantConst wifi = incoming["wifi"];
    if (!wifi.isNull()) {
        JsonVariantConst pri = wifi["primary"];
        if (!pri.isNull() && json_string_is_blank(pri["password"])) {
            trial.wifi_primary.password = s_cfg->wifi_primary.password;
        }
        JsonVariantConst bak = wifi["backup"];
        if (!bak.isNull() && json_string_is_blank(bak["password"])) {
            trial.wifi_backup.password = s_cfg->wifi_backup.password;
        }
        JsonVariantConst ap = wifi["ap"];
        if (!ap.isNull() && json_string_is_blank(ap["password"])) {
            trial.ap_password = s_cfg->ap_password;
        }
    }
    JsonVariantConst mqtt = incoming["mqtt"];
    if (!mqtt.isNull() && json_string_is_blank(mqtt["password"])) {
        trial.mqtt_password = s_cfg->mqtt_password;
    }

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
    commands::sync_engine_from_config();
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

static void handle_get_events() {
    if (!heap_ok()) { send_err(503, "low heap"); return; }

    if (s_sse_active && s_sse_client.connected()) {
        s_sse_client.stop();
    }

    WiFiClient client = s_server.client();
    client.setNoDelay(true);
    client.print("HTTP/1.1 200 OK\r\n");
    client.print("Content-Type: text/event-stream\r\n");
    client.print("Cache-Control: no-cache\r\n");
    client.print("Connection: keep-alive\r\n");
    client.print("Access-Control-Allow-Origin: *\r\n\r\n");
    client.print("retry: 1000\n\n");

    s_sse_client = client;
    s_sse_active = true;
    s_sse_last_code_bits = UINT32_MAX;
    s_sse_last_push_ms = 0;
    sse_send_state("state");
    pxlog::info(TAG, "SSE client connected");
}

static void handle_post_identify() {
    commands::identify();
    send_ok();
}

static void handle_post_reset() {
    commands::reset_puzzle();
    send_ok();
    pxlog::info(TAG, "puzzle reset via HTTP");
}

static void handle_post_restart() {
    send_ok();
    commands::schedule_restart(500);
    pxlog::info(TAG, "restart requested via HTTP");
}

static void handle_post_sleep() {
    send_ok();
    commands::schedule_sleep(500);
    pxlog::info(TAG, "sleep requested via HTTP");
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
    s_server.on("/api/files/upload",  HTTP_POST, handle_post_file_upload, handle_post_file_upload_body);
    s_server.on("/api/state",         HTTP_GET,  handle_get_state);
    s_server.on("/api/events",        HTTP_GET,  handle_get_events);
    s_server.on("/api/log",           HTTP_GET,  handle_get_log);
    s_server.on("/api/identify",      HTTP_POST, handle_post_identify);
    s_server.on("/api/reset",         HTTP_POST, handle_post_reset);
    s_server.on("/api/restart",       HTTP_POST, handle_post_restart);
    s_server.on("/api/sleep",         HTTP_POST, handle_post_sleep);
    s_server.onNotFound(handle_not_found);

    // Mount HTTP OTA updater on the shared server (must precede begin()).
    ota_mgr::mount_http_update(s_server, *s_cfg);

    s_server.collectHeaders(k_proxy_headers,
                            sizeof(k_proxy_headers) / sizeof(k_proxy_headers[0]));
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

    if (s_sse_active) {
        if (!s_sse_client.connected()) {
            s_sse_client.stop();
            s_sse_active = false;
            pxlog::info(TAG, "SSE client disconnected");
        } else {
            uint32_t now = millis();
            uint32_t bits = 0;
            bool have_code = appstate::get_code_snapshot(&bits, nullptr);
            bool changed = have_code && (bits != s_sse_last_code_bits);
            bool heartbeat = (now - s_sse_last_push_ms >= 2000);
            if (changed || heartbeat) {
                sse_send_state(changed ? "code_changed" : "state");
                if (have_code) s_sse_last_code_bits = bits;
            }
        }
    }

    if (s_reboot_pending && millis() >= s_reboot_at) {
        pxlog::info(TAG, "rebooting");
        pxlog::flush();
        ESP.restart();
    }
}

bool     reboot_pending() { return s_reboot_pending; }
uint32_t reboot_at_ms()   { return s_reboot_at; }

} // namespace web_ui
