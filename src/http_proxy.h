#pragma once

#include <ESP8266WebServer.h>
#include <WString.h>

namespace http_proxy {

struct Ctx {
    String prefix;
    bool has_prefix = false;
};

void read(ESP8266WebServer& server, Ctx& ctx);
bool inject_base_tag(String& html, const Ctx& ctx);

} // namespace http_proxy
