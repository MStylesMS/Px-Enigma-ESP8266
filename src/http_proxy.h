#pragma once

#include <ESP8266WebServer.h>
#include <WString.h>

namespace http_proxy {

struct Ctx {
    String prefix;
    String host;
    String proto;
    bool has_prefix = false;
    bool has_forwarded_host = false;
};

void read(ESP8266WebServer& server, Ctx& ctx);
bool parse_prefix(const char* raw, String& out);
const char* prefix_or_empty(const Ctx& ctx);
void join_path(const Ctx& ctx, const char* path, String& out);
void build_url(const Ctx& ctx, const char* path, String& out);
void build_ws_url(const Ctx& ctx, const char* path, String& out);
bool inject_base_tag(String& html, const Ctx& ctx);

} // namespace http_proxy
