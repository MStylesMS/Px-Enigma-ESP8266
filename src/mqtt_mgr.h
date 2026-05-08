// mqtt_mgr.h — MQTT plumbing for px-enigma-esp8266.
//
// Wraps PubSubClient with reconnect-with-backoff, per-connect announce,
// periodic state heartbeat, retained config override, and helpers for
// publishing JSON-shaped state/events/warnings.
//
// Topic resolution (spec §10):
//   commands : <base_topic>/commands         (in,  retain=off)
//   state    : <base_topic>/state            (out, retain=off)
//   events   : <base_topic>/events           (out, retain=off)
//   warnings : <base_topic>/warnings         (out, retain=off)
//   config   : <base_topic>/config           (in,  retain=ON)
//   announce : <announce_topic>              (out, retain=off)
//
// All publishes are QoS 1.
#pragma once

#include "config.h"
#include <Arduino.h>
#include <ArduinoJson.h>

namespace mqtt_mgr {

// Callback invoked from the MQTT client when a message arrives on the
// commands topic. The config-override topic is consumed internally and
// never bubbles up to this callback.
typedef void (*CommandCb)(const uint8_t* payload, size_t len, void* user);

// Call once in setup() after wifi_mgr::begin().
void begin(cfg::Config* c, CommandCb cb, void* user);

// Cooperative tick — non-blocking. Drives reconnect, heartbeat, etc.
void loop();

// True iff the underlying PubSubClient reports connected.
bool connected();

// Publish a fresh state snapshot. No-op if not connected.
bool publish_state();

// Publish announce envelope (called automatically once per (re)connect).
bool publish_announce();

// Publish an event to <base_topic>/events.
//   type:    "command" | "code" | "battery" | "state" | "system" | "config"
//   event:   short event name (e.g. "command_success")
//   message: optional human-readable message (may be nullptr)
//   data:    optional structured payload (may be JsonVariantConst())
bool publish_event(const char* type, const char* event, const char* message,
                   JsonVariantConst data);

// Publish a warning to <base_topic>/warnings.
bool publish_warning(const char* warning, const char* message,
                     JsonVariantConst data);

// Mark that the saved config was invalid; a warning will be emitted on
// the next successful connect (so it is observable from the broker).
void note_config_invalid_pending();

} // namespace mqtt_mgr
