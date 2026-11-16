// Kitchen blind controller: Hunter Douglas PowerView Gen 2 blind via
// Wemos D1 Mini (ESP8266) + nRF24L01+.
//
// - Sends Open / Close / Stop / Favorite as the Pebble remote would
// - MQTT for Home Assistant (cover + favorite button), with retained state
// - Hears the real remote too, so state stays in sync when it's used
// - Web UI on port 80, mDNS (kitchen-blind.local), ArduinoOTA
//
// Network IDs and protocol notes: docs/protocol-notes.md
// Credentials: include/secrets.h (git-ignored, see secrets.example.h)

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <RFPowerView.h>

#include "secrets.h"

#ifndef HOSTNAME
#define HOSTNAME "kitchen-blind"
#endif
#ifndef MQTT_BASE
#define MQTT_BASE "home/kitchen_blind"
#endif

static const char *TOPIC_SET = MQTT_BASE "/set";
static const char *TOPIC_STATE = MQTT_BASE "/state";
static const char *TOPIC_AVAIL = MQTT_BASE "/availability";
static const char *TOPIC_ATTRS = MQTT_BASE "/attributes";

// ---------------------------------------------------------------------------
// Blind state

enum class Command { OPEN, CLOSE, STOP, FAVORITE };

struct CommandInfo {
  Command cmd;
  const char *name;       // MQTT payload / UI name
  PacketType packetType;  // RFPowerView packet type
  const char *coverState; // Home Assistant cover state after this command
};

static const CommandInfo COMMANDS[] = {
  {Command::OPEN, "OPEN", PacketType::OPEN, "open"},
  {Command::CLOSE, "CLOSE", PacketType::CLOSE, "closed"},
  {Command::STOP, "STOP", PacketType::STOP, "stopped"},
  // The favorite position is part-way open; HA has no state for it.
  {Command::FAVORITE, "FAVORITE", PacketType::MOVE_TO_SAVED_POSITION, "open"},
};

const CommandInfo *findCommand(const String &name) {
  for (const auto &c : COMMANDS) {
    if (name.equalsIgnoreCase(c.name)) return &c;
  }
  return nullptr;
}

const CommandInfo *findCommand(PacketType t) {
  for (const auto &c : COMMANDS) {
    if (c.packetType == t) return &c;
  }
  return nullptr;
}

String lastState = "unknown";     // HA cover state
String lastCommand = "";          // last command name
String lastSource = "";           // "mqtt", "web", "remote"
uint32_t lastChangeMs = 0;

// ---------------------------------------------------------------------------
// Radio

RFPowerView powerView(RF_CE_PIN, RF_CS_PIN, RF_IRQ_PIN, RF_ID);
bool radioOk = false;

// ---------------------------------------------------------------------------
// Network

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
ESP8266WebServer web(80);

uint32_t lastMqttAttempt = 0;

void publishState() {
  if (!mqtt.connected()) return;
  mqtt.publish(TOPIC_STATE, lastState.c_str(), true);
  String attrs = String("{\"last_command\":\"") + lastCommand + "\",\"source\":\"" + lastSource + "\"}";
  mqtt.publish(TOPIC_ATTRS, attrs.c_str(), true);
}

void recordCommand(const CommandInfo &c, const char *source) {
  lastState = c.coverState;
  lastCommand = c.name;
  lastSource = source;
  lastChangeMs = millis();
  publishState();
}

bool sendCommand(const CommandInfo &c, const char *source) {
  if (!radioOk) {
    Serial.println(F("Radio not available, command dropped"));
    return false;
  }
  Packet packet;
  GroupsHeader header;
  header.source = REMOTE_ID;
  header.groups = {BLIND_GROUP};
  packet.header = header;
  packet.type = c.packetType;
  packet.parameters = std::monostate{};
  // The blind accepts any rolling codes; random values make each send
  // look like a fresh button press.
  packet.rollingCode1 = (uint8_t)random(256);
  packet.rollingCode2 = (uint8_t)random(256);

  Serial.printf("TX %s (from %s)\n", c.name, source);
  bool ok = powerView.sendPacket(&packet);  // blocks ~620 ms
  if (ok) recordCommand(c, source);
  return ok;
}

// Packets from the real remote: keep our state in sync.
void onPacket(const Packet *packet) {
  if (!std::holds_alternative<GroupsHeader>(packet->header)) return;
  const auto &h = std::get<GroupsHeader>(packet->header);
  bool forUs = false;
  for (auto g : h.groups) forUs |= (g == BLIND_GROUP);
  if (!forUs) return;

  const CommandInfo *c = findCommand(packet->type);
  if (!c) return;
  // The remote repeats each press hundreds of times; only record changes.
  if (lastCommand == c->name && lastSource == "remote" && millis() - lastChangeMs < 3000) return;
  Serial.printf("RX %s from remote 0x%04X\n", c->name, h.source);
  recordCommand(*c, "remote");
}

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  String msg;
  msg.reserve(length);
  for (unsigned int i = 0; i < length; i++) msg += (char)payload[i];
  msg.trim();

  if (strcmp(topic, TOPIC_SET) == 0) {
    const CommandInfo *c = findCommand(msg);
    if (c) sendCommand(*c, "mqtt");
    else Serial.printf("Unknown MQTT command '%s'\n", msg.c_str());
  } else if (strcmp(topic, TOPIC_STATE) == 0) {
    // Our own retained state, delivered once at (re)connect: restore it.
    if (lastState == "unknown" && msg.length()) {
      lastState = msg;
      Serial.printf("Restored retained state '%s'\n", msg.c_str());
    }
    mqtt.unsubscribe(TOPIC_STATE);
  }
}

void mqttConnect() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastMqttAttempt < 5000 && lastMqttAttempt != 0) return;
  lastMqttAttempt = millis();

  String clientId = String(HOSTNAME "-") + String(ESP.getChipId(), HEX);
  Serial.printf("MQTT connecting to %s:%d ... ", MQTT_HOST, MQTT_PORT);
  if (mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS, TOPIC_AVAIL, 1, true, "offline")) {
    Serial.println(F("connected"));
    mqtt.publish(TOPIC_AVAIL, "online", true);
    mqtt.subscribe(TOPIC_SET);
    if (lastState == "unknown") {
      mqtt.subscribe(TOPIC_STATE);  // fetch our retained state once
    } else {
      publishState();
    }
  } else {
    Serial.printf("failed, rc=%d\n", mqtt.state());
  }
}

// ---------------------------------------------------------------------------
// Web UI

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Kitchen Blind</title>
<style>
 body{font-family:system-ui,sans-serif;margin:0;padding:1.5rem;max-width:26rem;margin:auto;
      background:#f4f4f2;color:#222}
 h1{font-size:1.4rem;margin:0 0 .25rem}
 #state{color:#666;margin-bottom:1.25rem;min-height:1.2em}
 button{display:block;width:100%;font-size:1.3rem;padding:1.1rem;margin:.6rem 0;border:0;
        border-radius:.8rem;background:#fff;box-shadow:0 1px 3px #0002;color:#222}
 button:active{transform:scale(.98)}
 .stop{background:#c0392b;color:#fff}
 .fav{background:#2c6e9b;color:#fff}
 .small{font-size:1rem;padding:.7rem;background:#e8e8e5;box-shadow:none}
 button[disabled]{opacity:.5}
 @media (prefers-color-scheme:dark){body{background:#1b1b1b;color:#eee}
   button{background:#2c2c2c;color:#eee}.small{background:#262626}#state{color:#aaa}}
</style></head><body>
<h1>Kitchen Blind</h1>
<div id="state">&nbsp;</div>
<button onclick="send('OPEN')">Open</button>
<button class="fav" onclick="send('FAVORITE')">&#9733; Favorite</button>
<button class="stop" onclick="send('STOP')">Stop</button>
<button class="small" onclick="send('CLOSE')">Close fully</button>
<script>
async function refresh(){
  try{const r=await fetch('/state');const s=await r.json();
    document.getElementById('state').textContent=
      'State: '+s.state+(s.last_command?' (last: '+s.last_command+' from '+s.source+')':'');
  }catch(e){document.getElementById('state').textContent='Not reachable';}
}
async function send(c){
  document.querySelectorAll('button').forEach(b=>b.disabled=true);
  try{await fetch('/cmd?c='+c,{method:'POST'});}finally{
    document.querySelectorAll('button').forEach(b=>b.disabled=false);refresh();}
}
refresh();setInterval(refresh,5000);
</script></body></html>)HTML";

String stateJson() {
  String j = "{\"state\":\"" + lastState + "\",\"last_command\":\"" + lastCommand + "\",\"source\":\"" +
             lastSource + "\",\"radio\":" + (radioOk ? "true" : "false") + ",\"mqtt\":" +
             (mqtt.connected() ? "true" : "false") + ",\"rssi\":" + String(WiFi.RSSI()) +
             ",\"uptime_s\":" + String(millis() / 1000) + "}";
  return j;
}

void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html", INDEX_HTML); });
  web.on("/state", HTTP_GET, []() { web.send(200, "application/json", stateJson()); });
  web.on("/cmd", HTTP_POST, []() {
    const CommandInfo *c = findCommand(web.arg("c"));
    if (!c) {
      web.send(400, "application/json", "{\"error\":\"unknown command\"}");
      return;
    }
    bool ok = sendCommand(*c, "web");
    web.send(ok ? 200 : 503, "application/json", stateJson());
  });
  web.onNotFound([]() { web.send(404, "text/plain", "Not found"); });
  web.begin();
}

// ---------------------------------------------------------------------------

void setupOta() {
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println(F("OTA start"));
    if (mqtt.connected()) {
      mqtt.publish(TOPIC_AVAIL, "offline", true);
      mqtt.disconnect();
    }
  });
  ArduinoOTA.onEnd([]() { Serial.println(F("OTA done, rebooting")); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("OTA error %u\n", e); });
  ArduinoOTA.begin();  // also starts mDNS for HOSTNAME.local
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.printf("=== %s: RF_ID=0x%04X remote=0x%04X group=%u ===\n", HOSTNAME, RF_ID, REMOTE_ID, BLIND_GROUP);
  randomSeed(ESP.getCycleCount() ^ ESP.getChipId());

  powerView.setPacketReceivedCallback(onPacket);
  radioOk = powerView.begin();
  Serial.printf("Radio: %s\n", radioOk ? "OK" : "FAILED - check wiring");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("Wi-Fi connecting to '%s'\n", WIFI_SSID);

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(30);

  setupWeb();
  setupOta();
}

void loop() {
  static bool wasConnected = false;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wasConnected) {
    Serial.printf("Wi-Fi connected, IP %s, http://%s.local/\n", WiFi.localIP().toString().c_str(), HOSTNAME);
  } else if (!connected && wasConnected) {
    Serial.println(F("Wi-Fi lost, reconnecting..."));
  }
  wasConnected = connected;

  powerView.loop();
  ArduinoOTA.handle();
  MDNS.update();
  web.handleClient();
  mqttConnect();
  mqtt.loop();
}
