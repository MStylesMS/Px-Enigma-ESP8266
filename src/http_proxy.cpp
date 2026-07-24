#include "http_proxy.h"

#include <ctype.h>

namespace http_proxy {

static bool prefix_char_ok(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '/';
}

static String trim_copy(const String& value) {
    String out = value;
    out.trim();
    return out;
}

bool parse_prefix(const char* raw, String& out) {
    if (!raw) return false;

    String value = trim_copy(String(raw));
    if (!value.startsWith("/")) return false;

    while (value.endsWith("/")) {
        value.remove(value.length() - 1);
    }
    if (value.length() <= 1) return false;
    if (value.indexOf("..") >= 0) return false;

    for (unsigned i = 0; i < value.length(); ++i) {
        if (!prefix_char_ok(value[i])) return false;
    }

    out = value;
    return true;
}

static bool normalize_host(const String& raw, String& out) {
    String value = trim_copy(raw);
    if (value.length() == 0 || value.length() >= 128) return false;
    out = value;
    return true;
}

static bool normalize_proto(const String& raw, String& out) {
    String value = trim_copy(raw);
    if (value.startsWith("https")) {
        out = "https";
        return true;
    }
    if (value.startsWith("http")) {
        out = "http";
        return true;
    }
    return false;
}

static int find_head_insert_pos(const String& html) {
    const int len = html.length();
    for (int i = 0; i + 5 < len; ++i) {
        if (html[i] != '<') continue;
        if (tolower(html[i + 1]) != 'h') continue;
        if (tolower(html[i + 2]) != 'e') continue;
        if (tolower(html[i + 3]) != 'a') continue;
        if (tolower(html[i + 4]) != 'd') continue;
        if (html[i + 5] != '>' && !isspace((unsigned char)html[i + 5])) continue;

        int j = i + 5;
        while (j < len && html[j] != '>') {
            j++;
        }
        if (j < len) return j + 1;
    }
    return -1;
}

static bool has_external_base(const Ctx& ctx) {
    return ctx.has_forwarded_host && ctx.host.length() > 0 && ctx.proto.length() > 0;
}

void read(ESP8266WebServer& server, Ctx& ctx) {
    ctx = Ctx{};

    if (server.hasHeader("X-Forwarded-Prefix")) {
        String normalized;
        if (parse_prefix(server.header("X-Forwarded-Prefix").c_str(), normalized)) {
            ctx.prefix = normalized;
            ctx.has_prefix = true;
        }
    }

    if (server.hasHeader("X-Forwarded-Host")) {
        String normalized;
        if (normalize_host(server.header("X-Forwarded-Host"), normalized)) {
            ctx.host = normalized;
            ctx.has_forwarded_host = true;
        }
    }

    if (server.hasHeader("X-Forwarded-Proto")) {
        String normalized;
        if (normalize_proto(server.header("X-Forwarded-Proto"), normalized)) {
            ctx.proto = normalized;
        }
    }
}

const char* prefix_or_empty(const Ctx& ctx) {
    return ctx.has_prefix ? ctx.prefix.c_str() : "";
}

void join_path(const Ctx& ctx, const char* path, String& out) {
    if (!path || path[0] != '/') {
        out = path ? String(path) : String("/");
        return;
    }

    const char* prefix = prefix_or_empty(ctx);
    if (prefix[0] == '\0') {
        out = String(path);
        return;
    }

    out = String(prefix) + String(path);
}

void build_url(const Ctx& ctx, const char* path, String& out) {
    String joined;
    join_path(ctx, path, joined);

    if (has_external_base(ctx)) {
        out = ctx.proto + "://" + ctx.host + joined;
        return;
    }

    out = joined;
}

void build_ws_url(const Ctx& ctx, const char* path, String& out) {
    String joined;
    join_path(ctx, path, joined);

    if (has_external_base(ctx)) {
        const char* ws_scheme = (ctx.proto == "https") ? "wss" : "ws";
        out = String(ws_scheme) + "://" + ctx.host + joined;
        return;
    }

    out = joined;
}

bool inject_base_tag(String& html, const Ctx& ctx) {
    if (!ctx.has_prefix) return true;

    int insert = find_head_insert_pos(html);
    if (insert < 0) return false;

    String tag = String("<base href=\"") + ctx.prefix + "/\">";
    html = html.substring(0, insert) + tag + html.substring(insert);
    return true;
}

} // namespace http_proxy
