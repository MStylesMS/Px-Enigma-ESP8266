#include "http_proxy.h"

#include <ctype.h>

namespace http_proxy {

static bool prefix_char_ok(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '/';
}

static bool normalize_prefix(const char* raw, String& out) {
    if (!raw) return false;

    while (*raw == ' ' || *raw == '\t') raw++;
    if (raw[0] != '/') return false;

    out = raw;
    out.trim();
    while (out.endsWith("/")) {
        out.remove(out.length() - 1);
    }
    if (out.length() <= 1) return false;
    if (out.indexOf("..") >= 0) return false;

    for (unsigned i = 0; i < out.length(); ++i) {
        if (!prefix_char_ok(out[i])) return false;
    }
    return true;
}

void read(ESP8266WebServer& server, Ctx& ctx) {
    ctx = Ctx{};
    if (!server.hasHeader("X-Forwarded-Prefix")) return;

    String raw = server.header("X-Forwarded-Prefix");
    String normalized;
    if (normalize_prefix(raw.c_str(), normalized)) {
        ctx.prefix = normalized;
        ctx.has_prefix = true;
    }
}

bool inject_base_tag(String& html, const Ctx& ctx) {
    if (!ctx.has_prefix) return true;

    int idx = html.indexOf("<head>");
    if (idx < 0) idx = html.indexOf("<HEAD>");
    if (idx < 0) return false;

    int insert = html.indexOf('>', idx);
    if (insert < 0) return false;
    insert++;

    String tag = String("<base href=\"") + ctx.prefix + "/\">";
    html = html.substring(0, insert) + tag + html.substring(insert);
    return true;
}

} // namespace http_proxy
