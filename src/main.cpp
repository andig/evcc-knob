#include "config.h"
#ifdef ARDUINO
#include <M5Dial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <ESP32Encoder.h>
#else
#include "sim.h"
#endif
#include <ArduinoJson.h>
#include "icons.h"
#include "query.h"

// shortcut: websocket off, evcc is polled every 3 s; set 1 to use the websocket again
#define WEBSOCKET 0
#include "fonts.h"

#ifndef TZ
#define TZ "CET-1CEST,M3.5.0,M10.5.0/3" // POSIX time zone, here Central Europe
#endif

const uint32_t C_BG = 0x020318, C_TRACK = 0x1C2030, C_SEG = 0x2A2E40, C_GREY = 0x93949E,
               C_DIM = 0x6F7080, C_GREEN = 0x0FDE41, C_DARKGREEN = 0x0B5C26, C_PILL = 0x0B3D1C, C_BATTERY = 0x0BA631, C_EXPORT = 0xFFCC00, C_WHITE = 0xFFFFFF;

// sizes are the design's, halved; the NUM fonts only hold digits and .:-%
lgfx::VLWfont vlw[8];
const lgfx::IFont *F_SMALL = &vlw[0], *F_MED = &vlw[1], *F_UNIT = &vlw[2],
                  *F_LARGE = &vlw[3], *F_NUM36 = &vlw[4], *F_NUM48 = &vlw[5],
                  *F_NUM52 = &vlw[6], *F_NUM60 = &vlw[7];

void loadFonts() {
  static lgfx::PointerWrapper data[] = {
      {FONT_SMALL, sizeof(FONT_SMALL)}, {FONT_MED, sizeof(FONT_MED)},
      {FONT_UNIT, sizeof(FONT_UNIT)},   {FONT_LARGE, sizeof(FONT_LARGE)},
      {FONT_NUM36, sizeof(FONT_NUM36)}, {FONT_NUM48, sizeof(FONT_NUM48)},
      {FONT_NUM52, sizeof(FONT_NUM52)}, {FONT_NUM60, sizeof(FONT_NUM60)}};
  for (int i = 0; i < 8; i++) vlw[i].loadFont(&data[i]);
}

struct Mode {
  const char *mode, *always, *label; // always: evcc alwaysCharge, null = leave as is
  const uint8_t *icon16, *icon32;
};
const Mode MODES[] = {
    {"off", nullptr, "Off", nullptr, nullptr},
    {"smart", "off", "Smart", nullptr, nullptr},
    {"smart", "once", "Smart", ICON_ONCE_16, ICON_ONCE_32},
    {"smart", "on", "Smart", ICON_ALWAYS_16, ICON_ALWAYS_32},
    {"now", nullptr, "Fast", nullptr, nullptr},
};
const int MODE_COUNT = 5;

struct Slot {
  uint8_t hour;
  float price;
  bool planned;
};

struct State {
  bool ok = false, connected = false;
  bool heating = false, integrated = false; // heating: soc and limit are temperatures
  bool continuous = false, switchDevice = false; // change the mode names and list, as in evcc
  bool socPlan = false; // plan target is a soc, else an energy amount
  float planEnergy = 0, capacity = 0; // capacity of the connected vehicle in kWh
  int vehCount = 0;
  String vehNames[8], vehTitles[8]; // evcc vehicle names and their titles
  String lp, veh, fin, planTime, planStart, planEnd;
  int mode = 0, soc = 0, limit = 0, planSoc = 0, batSoc = -1, minute = 0, slots = 0;
  float chargePower = 0, pv = 0, grid = 0, home = 0, bat = 0, price = 0;
  Slot fc[24];
  int loadpoint = 0, lpCount = 0; // loadpoint this state was polled for
  String lps[8];                  // titles of all loadpoints
} st;

enum View { HOME, FLOW, PLAN, MODE, LIMIT, LOADPOINTS, TOAST };
const int PAGES = 3; // HOME, FLOW, PLAN are swipeable pages, the rest overlays

M5Canvas cv(&M5Dial.Display);

// debug log on USB serial, read with: pio device monitor
#ifdef ARDUINO
#define LOG(...) Serial.printf(__VA_ARGS__)
#define HEAP ESP.getFreeHeap()
#define RSSI WiFi.RSSI()
#else
#define HEAP 0
#define RSSI 0
#define LOG(...) printf(__VA_ARGS__)
#endif
Preferences prefs;
View view = HOME, page = HOME;
int edit = 0;             // value being edited in MODE/LIMIT/LOADPOINTS
uint32_t overlayAt = 0;   // last interaction with an overlay
volatile int loadpoint = LOADPOINT; // 1-based, LOADPOINT is only the first-boot default

// handover from the socket task: st itself is only touched by the UI loop
SemaphoreHandle_t lock;
QueueHandle_t posts;
State pending;
volatile bool fresh = false, postFailed = false;
long lastEnc = 0;
String toast[4];
bool toastCard = false;
bool dirty = true;
bool settled = true; // picker labels are drawn only once the dial rests, the arc at once
bool switching = false; // loadpoint picked, its first data not here yet

// ---------- evcc API ----------

// ISO 8601 time to epoch; 0 if unset (evcc sends year 1 for "no time")
time_t iso(const char *s) {
  int y, mo, d, h, mi, se, oh = 0, om = 0;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6 || y < 1971) return 0;
  const char *z = s + 19;
  while (*z && *z != 'Z' && *z != '+' && *z != '-') z++; // skip fractional seconds
  if (*z == '+' || *z == '-') {
    sscanf(z + 1, "%d:%d", &oh, &om);
    if (*z == '-') oh = -oh, om = -om;
  }
  // days since 1970 from the civil date (Howard Hinnant's algorithm)
  y -= mo <= 2;
  int era = y / 400, yoe = y - era * 400;
  int doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
  return (time_t)days * 86400 + h * 3600 + mi * 60 + se - oh * 3600 - om * 60;
}

bool clockSet() { return time(nullptr) > 1600000000; }

// local "HH:MM"; empty if unset or the clock has no NTP time yet
String hm(time_t t) {
  if (t <= 0 || !clockSet()) return "";
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
  return buf;
}

// values kept between socket messages that the screens only show in derived form
struct Raw {
  String mode = "off", always = "off", vehicle; // vehicle: evcc name of the connected one
  float remaining = 0;                          // seconds of charging left
  time_t planAt = 0, planFrom = 0, planTo = 0;
  float vehCap[8] = {};
  bool vehOffline[8] = {};
  int prices = 0;
  struct {
    uint32_t start, end;
    float price;
  } grid[400]; // grid price forecast, 15 min slots
};

// loadpoint fields the dial uses; the socket sends each as "loadpoints.<n>.<field>"
const char *const FIELDS[] = {
    "title", "mode", "alwaysCharge", "chargePower", "connected", "vehicleSoc", "vehicleTitle",
    "vehicleName", "effectiveLimitSoc", "chargeRemainingDuration", "effectivePlanTime",
    "effectivePlanSoc", "planEnergy", "planProjectedStart", "planProjectedEnd",
    "chargerFeatureHeating", "chargerFeatureIntegratedDevice", "chargerFeatureContinuous",
    "chargerFeatureSwitchDevice"};

// evcc sends a lot more than the dial needs: only these keys are kept while parsing
JsonDocument makeFilter(int lp) {
  JsonDocument f;
  char key[48];
  for (const char *k : {"pvPower", "homePower", "tariffGrid", "forecast.grid"}) f[k] = true;
  f["grid"]["power"] = true;
  f["battery"]["power"] = true;
  f["battery"]["soc"] = true;
  for (const char *k : {"title", "capacity", "features"}) f["vehicles"]["*"][k] = true;
  for (int n = 0; n < 8; n++) {
    snprintf(key, sizeof(key), "loadpoints.%d.title", n);
    f[key] = true;
  }
  for (const char *k : FIELDS) {
    snprintf(key, sizeof(key), "loadpoints.%d.%s", lp - 1, k);
    f[key] = true;
  }
  return f;
}

// merge one socket message; a message holds only the keys that changed
void apply(JsonObjectConst msg, State &s, Raw &r) {
  JsonVariantConst v;
  char key[48];
  auto has = [&](const char *k) {
    v = msg[k];
    return !v.isUnbound();
  };
  auto lp = [&](const char *field) {
    snprintf(key, sizeof(key), "loadpoints.%d.%s", s.loadpoint - 1, field);
    return has(key);
  };

  if (lp("title")) s.lp = v | "";
  if (lp("mode")) r.mode = v | "off";
  if (lp("alwaysCharge")) r.always = v | "off";
  if (lp("chargePower")) s.chargePower = v | 0.0f;
  if (lp("connected")) s.connected = v | false;
  if (lp("vehicleSoc")) s.soc = lroundf(v | 0.0f);
  if (lp("vehicleTitle")) s.veh = v | "";
  if (lp("vehicleName")) r.vehicle = v | "";
  if (lp("effectiveLimitSoc")) s.limit = lroundf(v | 0.0f);
  if (lp("chargeRemainingDuration")) r.remaining = v | 0.0f;
  if (lp("effectivePlanTime")) r.planAt = iso(v | "");
  if (lp("effectivePlanSoc")) s.planSoc = lroundf(v | 0.0f);
  if (lp("planEnergy")) s.planEnergy = v | 0.0f;
  if (lp("planProjectedStart")) r.planFrom = iso(v | "");
  if (lp("planProjectedEnd")) r.planTo = iso(v | "");
  if (lp("chargerFeatureHeating")) s.heating = v | false;
  if (lp("chargerFeatureIntegratedDevice")) s.integrated = v | false;
  if (lp("chargerFeatureContinuous")) s.continuous = v | false;
  if (lp("chargerFeatureSwitchDevice")) s.switchDevice = v | false;

  for (int n = 0; n < 8; n++) {
    snprintf(key, sizeof(key), "loadpoints.%d.title", n);
    if (!has(key)) continue;
    s.lps[n] = v | "";
    s.lpCount = max(s.lpCount, n + 1);
  }

  if (has("pvPower")) s.pv = v | 0.0f;
  if (has("homePower")) s.home = v | 0.0f;
  if (has("tariffGrid")) s.price = v | 0.0f;
  if (has("grid")) s.grid = v["power"] | 0.0f;
  if (has("battery")) {
    s.bat = v["power"] | 0.0f;
    s.batSoc = lroundf(v["soc"] | -1.0f); // -1: no home battery
  }
  if (has("vehicles")) {
    s.vehCount = 0;
    for (JsonPairConst p : v.as<JsonObjectConst>()) {
      if (s.vehCount == 8) break;
      r.vehCap[s.vehCount] = p.value()["capacity"] | 0.0f;
      r.vehOffline[s.vehCount] = false;
      for (JsonVariantConst f : p.value()["features"].as<JsonArrayConst>())
        if (!strcmp(f | "", "Offline")) r.vehOffline[s.vehCount] = true;
      s.vehNames[s.vehCount] = p.key().c_str();
      s.vehTitles[s.vehCount++] = p.value()["title"] | "";
    }
  }
  if (has("forecast.grid")) {
    r.prices = 0;
    for (JsonArrayConst slot : v.as<JsonArrayConst>()) {
      if (r.prices == 400) break;
      r.grid[r.prices++] = {slot[0] | (uint32_t)0, slot[1] | (uint32_t)0, slot[2] | 0.0f};
    }
  }
}

// everything that depends on the clock or on more than one message
void derive(State &s, const Raw &r) {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  s.minute = tm.tm_hour * 60 + tm.tm_min;

  s.mode = 0;
  for (int i = 0; i < MODE_COUNT; i++)
    if (r.mode == MODES[i].mode && (!MODES[i].always || r.always == MODES[i].always)) s.mode = i;

  s.fin = r.remaining > 0 ? hm(now + (time_t)r.remaining) : String("");
  s.planTime = hm(r.planAt);
  s.planStart = hm(r.planFrom);
  s.planEnd = hm(r.planTo);

  // as evcc: a plan targets a soc if the vehicle reports one and its capacity is known
  bool known = false, offline = false;
  s.capacity = 0;
  for (int i = 0; i < s.vehCount; i++) {
    if (r.vehicle.length() == 0 || !(s.vehNames[i] == r.vehicle)) continue;
    known = true;
    offline = r.vehOffline[i];
    s.capacity = r.vehCap[i];
  }
  s.socPlan = ((known && !offline) || s.soc > 0) && s.capacity > 0;

  // price forecast as up to 24 hourly averages from now on, for the clock ring
  s.slots = 0;
  uint32_t hour = 0;
  float sum = 0;
  int count = 0;
  for (int i = 0; i <= r.prices && clockSet(); i++) {
    bool end = i == r.prices;
    if (!end && r.grid[i].end <= (uint32_t)now) continue;
    uint32_t h = end ? 0 : r.grid[i].start / 3600 * 3600;
    if (count && (end || h != hour)) {
      if (s.slots == 24) break;
      time_t t = hour;
      localtime_r(&t, &tm);
      bool planned = r.planFrom && r.planTo && t + 3600 > r.planFrom && t < r.planTo;
      s.fc[s.slots++] = {(uint8_t)tm.tm_hour, sum / count, planned};
      sum = count = 0;
    }
    if (end) break;
    hour = h;
    sum += r.grid[i].price;
    count++;
  }
}

// payload of websocket messages, as an ArduinoJson reader. Servers send unmasked frames.
struct WsReader {
  Client &c;
  uint64_t left = 0; // payload bytes left in the current frame
  bool last = true;  // current frame is the last one of its message
  bool failed = false;
  uint8_t buf[1024];
  int pos = 0, len = 0;

  // nothing buffered and nothing on the socket
  bool idle() { return pos == len && c.available() <= 0; }

  // next byte from the socket, -1 once it is closed or stalls for 5 s
  int next() {
    uint32_t start = millis();
    while (pos == len) {
      int n = c.available() > 0 ? c.read(buf, sizeof(buf)) : 0;
      if (n > 0) {
        pos = 0;
        len = n;
      } else if (!c.connected() || millis() - start > 5000) {
        failed = true;
        return -1;
      } else {
        delay(5);
      }
    }
    return buf[pos++];
  }

  // upgrade request; true if the server switched to the websocket protocol
  bool handshake(const char *host) {
    c.print(String("GET /ws HTTP/1.1\r\nHost: ") + host +
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: c21hcnQta25vYi1ldmNjIQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
    char status[13] = "";
    int n = 0, newlines = 0;
    while (newlines < 2) { // headers end with an empty line
      int b = next();
      if (b < 0) return false;
      if (n < 12) status[n++] = b;
      if (b == '\n') newlines++;
      else if (b != '\r') newlines = 0;
    }
    return !strcmp(status, "HTTP/1.1 101");
  }

  // frame header; false on a close frame or a broken connection
  bool header() {
    for (;;) {
      int h = next(), l = next();
      if (h < 0 || l < 0) return false;
      left = l & 0x7F;
      int more = left == 126 ? 2 : left == 127 ? 8 : 0;
      if (more) left = 0;
      while (more--) left = left << 8 | (uint8_t)next();
      int op = h & 0x0F;
      if (failed || op == 8) return false;
      if (op < 8) {
        last = h & 0x80;
        return true;
      }
      skip(); // ping or pong
    }
  }

  void skip() {
    while (left && next() >= 0) left--;
    left = 0;
  }

  // drop what the JSON parser left of the message
  void finish() {
    skip();
    while (!last && header()) skip();
  }

  int read() {
    while (!left)
      if (last || !header()) return -1;
    left--;
    return next();
  }

  size_t readBytes(char *out, size_t n) {
    size_t i = 0;
    for (int b; i < n && (b = read()) >= 0; i++) out[i] = b;
    return i;
  }
};

void handover(const State &s) {
  xSemaphoreTake(lock, portMAX_DELAY);
  pending = s;
  fresh = true;
  xSemaphoreGive(lock);
}

// evcc pushes its state on /ws: everything once on connect, then each value as it changes
void socketTask(void *) {
  char scheme[8] = "", host[64] = "";
  int port = 0;
  if (sscanf(EVCC_URL, "%7[a-z]://%63[^:/]:%d", scheme, host, &port) < 3)
    port = strcmp(scheme, "https") ? 80 : 443;
  WiFiClient plain;
  WiFiClientSecure secure;
  secure.setInsecure(); // shortcut: no certificate check, add a CA if evcc is not on the LAN
  Client &c = strcmp(scheme, "https") ? (Client &)plain : (Client &)secure;
  static Raw raw;
  static WsReader ws{c};

  for (;;) {
    State cur;
    cur.loadpoint = loadpoint;
    raw = Raw();
    ws.pos = ws.len = ws.left = 0;
    ws.last = true;
    ws.failed = false;
    if (WiFi.isConnected() && c.connect(host, port) && ws.handshake(host)) {
      JsonDocument filter = makeFilter(cur.loadpoint);
      bool changed = false;
      uint32_t seen = millis(), shown = 0;
      // a changed loadpoint needs the full state again, so it reconnects
      while (!ws.failed && cur.loadpoint == loadpoint && millis() - seen < 120000) {
        // show a burst of messages at once; refresh every 20 s for the clock
        if (cur.ok && (changed ? ws.idle() || millis() - shown > 500 : millis() - shown > 20000)) {
          derive(cur, raw);
          handover(cur);
          changed = false;
          shown = millis();
        }
        if (ws.idle()) {
          delay(20);
          continue;
        }
        if (!ws.header()) break;
        JsonDocument doc;
        if (!deserializeJson(doc, ws, DeserializationOption::Filter(filter)) &&
            doc.as<JsonObjectConst>().size()) {
          apply(doc.as<JsonObjectConst>(), cur, raw);
          cur.ok = changed = true;
        }
        ws.finish();
        seen = millis();
      }
    }
    LOG("%lu ws closed wifi=%d failed=%d ok=%d\n", (unsigned long)millis(), WiFi.isConnected(),
        ws.failed, cur.ok);
    c.stop();
    if (cur.loadpoint != loadpoint) continue;
    // connection lost: say so, then retry
    State offline;
    offline.loadpoint = cur.loadpoint;
    handover(offline);
    delay(3000);
  }
}

String urlencode(const String &s) {
  String out;
  char buf[4];
  for (char c : s) {
    if (isalnum(c)) {
      out += c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
      out += buf;
    }
  }
  return out;
}

String lastBody; // last poll's response: an identical one changes nothing on screen

// whole state of the current loadpoint in one request, shrunk by evcc's jq;
// changed is false when evcc returned exactly the previous response
State poll(bool &changed) {
  changed = true;
  static const String base = String(EVCC_URL) + "/api/state?jq=",
                      query = urlencode(String(" as $i|") + QUERY);
  State next;
  next.loadpoint = loadpoint;
  if (!WiFi.isConnected()) return next;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(base + (next.loadpoint - 1) + query);
  JsonDocument doc;
  int code = http.GET();
  String body = code == HTTP_CODE_OK ? String(http.getString().c_str()) : String();
  bool ok = code == HTTP_CODE_OK && !deserializeJson(doc, body.c_str());
  changed = !ok || body != lastBody;
  lastBody = ok ? body : String();
  LOG("%lu http %d rssi=%d\n", (unsigned long)millis(), code, (int)RSSI);
  http.end();
  next.ok = ok;
  if (!ok) return next;

  next.lp = doc["lp"] | "";
  next.veh = doc["veh"] | "";
  next.fin = doc["fin"] | "";
  next.planTime = doc["pt"] | "";
  next.planStart = doc["ps"] | "";
  next.planEnd = doc["pe"] | "";
  next.connected = doc["con"] | false;
  next.heating = doc["heat"] | false;
  next.integrated = doc["dev"] | false;
  next.continuous = doc["cont"] | false;
  next.switchDevice = doc["sw"] | false;
  // evcc sends these as floats: read as float, or a fraction would fall back to the default
  next.soc = lroundf(doc["soc"] | 0.0f);
  next.limit = lroundf(doc["lim"] | 0.0f);
  next.planSoc = lroundf(doc["psoc"] | 0.0f);
  next.socPlan = doc["sbp"] | false;
  next.planEnergy = doc["pen"] | 0.0f;
  next.capacity = doc["cap"] | 0.0f;
  next.batSoc = lroundf(doc["bsoc"] | -1.0f); // -1: no home battery
  next.minute = doc["min"] | 0;
  next.chargePower = doc["cp"] | 0.0f;
  next.pv = doc["pv"] | 0.0f;
  next.grid = doc["grid"] | 0.0f;
  next.home = doc["home"] | 0.0f;
  next.bat = doc["bat"] | 0.0f;
  next.price = doc["price"] | 0.0f;

  const char *mode = doc["mode"] | "off";
  const char *always = doc["ac"] | "off";
  for (int i = 0; i < MODE_COUNT; i++)
    if (!strcmp(mode, MODES[i].mode) && (!MODES[i].always || !strcmp(always, MODES[i].always)))
      next.mode = i;

  for (JsonArray s : doc["fc"].as<JsonArray>()) {
    if (next.slots == 24) break;
    next.fc[next.slots++] = {s[0].as<uint8_t>(), s[1].as<float>(), s[2].as<int>() == 1};
  }
  for (JsonVariant title : doc["lps"].as<JsonArray>()) {
    if (next.lpCount == 8) break;
    next.lps[next.lpCount++] = title | "";
  }
  for (JsonArray v : doc["vt"].as<JsonArray>()) {
    if (next.vehCount == 8) break;
    next.vehNames[next.vehCount] = v[0] | "";
    next.vehTitles[next.vehCount++] = v[1] | "";
  }
  return next;
}

// queue a POST for the current loadpoint
void post(const String &path) {
  char buf[48];
  strlcpy(buf, (String("/api/loadpoints/") + loadpoint + path).c_str(), sizeof(buf));
  xQueueSend(posts, buf, 0);
}

// POSTs (and without websocket the polls) run here, so the UI loop never waits for the network
void postTask(void *) {
  char path[48];
  for (;;) {
    // a queued POST wakes the task early and is followed by a fresh poll
    if (xQueueReceive(posts, path, pdMS_TO_TICKS(WEBSOCKET ? 1000 : 3000)) && path[0]) {
      HTTPClient http;
      http.setTimeout(3000);
      http.begin(String(EVCC_URL) + path);
      if (http.POST("") != HTTP_CODE_OK) postFailed = true;
      http.end();
      lastBody = String(); // the next poll confirms or reverts what the UI already shows
    }
#if !WEBSOCKET
    uint32_t t = millis();
    bool changed;
    State next = poll(changed);
    LOG("%lu poll ok=%d changed=%d %lums heap=%lu\n", (unsigned long)t, next.ok, changed,
        (unsigned long)(millis() - t), (unsigned long)HEAP);
    if (changed) handover(next);
#endif
  }
}

// ---------- drawing ----------

// y is the text baseline, as in the design
void text(const String &s, int y, const lgfx::IFont *font, uint32_t color, int x = 120,
          textdatum_t datum = baseline_center) {
  cv.setFont(font);
  cv.setTextColor(color);
  cv.setTextDatum(datum);
  cv.drawString(s.c_str(), x, y);
}

struct Run {
  String s;
  const lgfx::IFont *font;
  uint32_t color;
};

// runs of text in different fonts on one baseline, centred as a whole
void line(int y, std::initializer_list<Run> runs, int gap = 4) {
  int width = -gap;
  for (auto &r : runs) {
    cv.setFont(r.font);
    width += cv.textWidth(r.s.c_str()) + gap;
  }
  int x = 120 - width / 2;
  for (auto &r : runs) {
    text(r.s, y, r.font, r.color, x, baseline_left);
    x += cv.textWidth(r.s.c_str()) + gap;
  }
}

// anti-aliased ring segment, degrees clockwise from 3 o'clock, optionally with round ends
const float R0 = 106, R1 = 116, RI = R0 - 1, RO = R1 + 1; // ring edges, and with AA margin

// angle and centre distance of the ring's pixels, built once so arc() needs no sqrtf or
// atan2f per pixel. Only one quadrant is stored, the others are its mirror images: the full
// ring took 25 kB, and without that RAM the TLS handshake to evcc failed.
// Fixed arrays, not vectors: growing a vector needs one large free block (bad_alloc, abort).
const int RING_N = 118;          // |dx|, |dy| <= RO
const int RING_PIXELS = 2106;    // quadrant pixels with RI <= distance <= RO
struct Ring {
  uint16_t first[RING_N];        // index of row |dy|'s first pixel
  uint8_t start[RING_N], len[RING_N]; // one run of |dx| per row
  uint16_t ang[RING_PIXELS];     // quadrant angle 0..90 degrees scaled to 0..65535
  uint8_t dist[RING_PIXELS];     // (distance - 104) * 16
  Ring() {
    int n = 0;
    for (int y = 0; y < RING_N; y++) {
      first[y] = n;
      start[y] = len[y] = 0;
      for (int x = 0; x < RING_N; x++) {
        float d2 = x * x + y * y;
        if (d2 < RI * RI || d2 > RO * RO || n == RING_PIXELS) continue;
        if (!len[y]) start[y] = x;
        len[y]++;
        ang[n] = atan2f(y, x) / DEG_TO_RAD * 65535 / 90;
        dist[n++] = lroundf((sqrtf(d2) - 104) * 16);
      }
    }
  }
};

void arc(float from, float to, uint32_t color, bool round = false) {
  static Ring ring;
  const float CAP = 5;
  float span = to - from;
  if (span <= 0) return;
  float capX[2], capY[2];
  for (int i = 0; i < 2; i++) {
    float rad = (i ? to : from) * DEG_TO_RAD;
    capX[i] = (R0 + R1) / 2 * cosf(rad);
    capY[i] = (R0 + R1) / 2 * sinf(rad);
  }
  int r = color >> 16 & 0xFF, g = color >> 8 & 0xFF, b = color & 0xFF;

  // scan only the segment's bounding box: its ends and the axis crossings between them
  float bx0 = 240, bx1 = 0, by0 = 240, by1 = 0;
  auto reach = [&](float deg) {
    float c = cosf(deg * DEG_TO_RAD), s = sinf(deg * DEG_TO_RAD);
    bx0 = min(bx0, 120 + min(c * R0, c * R1));
    bx1 = max(bx1, 120 + max(c * R0, c * R1));
    by0 = min(by0, 120 + min(s * R0, s * R1));
    by1 = max(by1, 120 + max(s * R0, s * R1));
  };
  reach(from);
  reach(to);
  for (float q = ceilf(from / 90) * 90; q < to; q += 90) reach(q);
  const int PAD = CAP + 2; // round ends and the anti-aliased edge reach past the segment
  int xa = max(0, (int)bx0 - PAD), xb = min(239, (int)bx1 + PAD);
  int ya = max(0, (int)by0 - PAD), yb = min(239, (int)by1 + PAD);

  cv.startWrite();
  for (int y = ya; y <= yb; y++) {
    int dy = y - 120, ady = abs(dy);
    if (ady >= RING_N || !ring.len[ady]) continue;
    int s = ring.start[ady], e = s + ring.len[ady] - 1;
    // right half (dx >= 0), then left half (dx < 0, so dx = 0 is drawn once)
    for (int side = 1; side >= -1; side -= 2) {
      int lo = side > 0 ? max(s, xa - 120) : max(max(s, 1), 120 - xb);
      int hi = side > 0 ? min(e, xb - 120) : min(e, 120 - xa);
      for (int adx = lo; adx <= hi; adx++) {
        int i = ring.first[ady] + adx - s, x = 120 + side * adx;
        float dx = side * adx, d = ring.dist[i] / 16.0f + 104;
        // coverage: distance in pixels to the nearest edge, 0.5 on the edge itself
        float a = min(d - R0, R1 - d) + 0.5f;
        if (span < 360) {
          // mirror the quadrant angle into the pixel's quadrant, screen y points down
          float q = ring.ang[i] * (90.0f / 65535);
          float deg = side > 0 ? (dy >= 0 ? q : 360 - q) : (dy >= 0 ? 180 - q : 180 + q);
          float t = deg - from;
          t -= 360 * floorf(t / 360);
          float along = t <= span ? min(t, span - t) : -min(t - span, 360 - t);
          a = min(a, along * (float)DEG_TO_RAD * d + 0.5f);
          for (int c = 0; round && c < 2; c++)
            a = max(a, min(CAP + 0.5f - hypotf(dx - capX[c], dy - capY[c]), 1.0f));
        }
        if (a <= 0) continue;
        if (a >= 1) {
          cv.writePixel(x, y, color);
          continue;
        }
        auto bg = cv.readPixelRGB(x, y);
        cv.writePixel(x, y, lgfx::color888(bg.r + (r - bg.r) * a, bg.g + (g - bg.g) * a,
                                           bg.b + (b - bg.b) * a));
      }
    }
  }
  cv.endWrite();
}

void dot(float angle, int radius, int size, uint32_t color) {
  float rad = angle * DEG_TO_RAD;
  cv.fillSmoothCircle(120 + radius * cosf(rad), 120 + radius * sinf(rad), size, color);
}

// gauge spans 270 degrees, open at the bottom
float gauge(float percent) { return 135 + constrain(percent, 0.0f, 100.0f) * 2.7f; }

#define DOT " \xC2\xB7 " // separator used in the design

// below 50 W is zero, so no "-0.0"
String kw(float w) { return String(fabsf(w) < 50 ? 0 : w / 1000, 1); }

// unit of soc and limit: heating devices report a temperature
const char *unit() { return st.heating ? "\xC2\xB0" "C" : "%"; }

// soc or limit with its unit: "62 %" or "65°C"
String value(int v) { return String(v) + (st.heating ? "" : " ") + unit(); }

// mode name, followed by the evcc always-charge symbol (1x or endless)
// evcc names: continuous devices (heat pumps) run Normal/Boost, switch devices turn On
const char *modeName(int i) {
  bool now = !strcmp(MODES[i].mode, "now");
  if (st.continuous && i == 0) return "Normal";
  if (st.continuous && now) return "Boost";
  if (st.switchDevice && now) return "On";
  return MODES[i].label;
}

// modes offered in the picker: switch devices have no always-charge variants
int modeList(int *list) {
  int n = 0;
  for (int i = 0; i < MODE_COUNT; i++)
    if (!(st.switchDevice && MODES[i].icon16)) list[n++] = i;
  return n;
}

void modeLabel(int i, int y, bool large, uint32_t color, bool pill = false) {
  const Mode &m = MODES[i];
  const char *label = modeName(i);
  const auto *font = large ? F_LARGE : pill ? F_MED : F_UNIT;
  int size = large ? 32 : 16, gap = large ? 8 : 5;
  cv.setFont(font);
  int width = cv.textWidth(label) + (m.icon16 ? gap + size : 0);
  if (pill) cv.fillSmoothRoundRect(110 - width / 2, y - 9, width + 20, 18, 9, C_PILL);
  text(label, y, font, color, 120 - width / 2, middle_left);
  if (m.icon16)
    cv.drawXBitmap(120 + width / 2 - size, y - size / 2, large ? m.icon32 : m.icon16, size, size,
                   color);
}

void drawHome() {
  arc(135, 405, C_TRACK, true);
  arc(135, gauge(st.soc), C_GREEN, true);
  dot(gauge(st.limit), 111, 5, C_WHITE);
  dot(gauge(st.limit), 111, 3, C_BG);

  text(st.lp, 66, F_SMALL, C_GREY);
  modeLabel(st.mode, 82, false, C_GREEN, true);
  if (!st.connected) {
    text("--", 141, F_NUM52, C_DIM);
    text("Not connected", 164, F_MED, C_WHITE);
    return;
  }
  line(141, {{kw(st.chargePower), F_NUM52, C_WHITE}, {"kW", F_UNIT, C_GREY}});
  // heaters and other integrated devices have no vehicle
  if (st.integrated)
    text(st.heating ? value(st.soc) : String(""), 164, F_MED, C_WHITE);
  else
    text(st.veh.length() ? st.veh + DOT + value(st.soc) : String("Guest vehicle"), 164, F_MED,
         C_WHITE);
  if (st.fin.length()) text(value(st.limit) + " at " + st.fin, 181, F_SMALL, C_GREY);
}

const int LIST_TOP = 62, LIST_HEIGHT = 116; // picker entries are drawn only between the arrows

// arrows above and below a picker list when entries are hidden there
void listArrows(int count) {
  if (edit > 1) cv.fillTriangle(120, 46, 112, 56, 128, 56, C_DIM);
  if (edit < count - 2) cv.fillTriangle(120, 194, 112, 184, 128, 184, C_DIM);
}

void drawMode() {
  int list[MODE_COUNT], count = modeList(list);
  float step = 270.0f / count;
  for (int i = 0; i < count; i++) {
    arc(135 + i * step + 5, 135 + (i + 1) * step - 5, i == edit ? C_GREEN : C_SEG, true);
    int y = 120 + (i - edit) * 38;
    if (!settled || y < 45 || y > 195) continue;
    cv.setClipRect(0, LIST_TOP, 240, LIST_HEIGHT); // keep entries off the arrows
    modeLabel(list[i], y, i == edit, i == edit ? C_GREEN : C_GREY);
    cv.clearClipRect();
  }
  if (settled) listArrows(count);
}

void drawLoadpoints() {
  if (settled && !switching) listArrows(st.lpCount);
  float step = 270.0f / st.lpCount;
  for (int i = 0; i < st.lpCount; i++) {
    arc(135 + i * step + 5, 135 + (i + 1) * step - 5, i == edit ? C_GREEN : C_SEG, true);
    int y = 120 + (i - edit) * 38;
    if (!settled || y < 45 || y > 195 || (switching && i != edit)) continue;
    // long titles do not fit the large font
    cv.setFont(F_LARGE);
    bool large = i == edit && cv.textWidth(st.lps[i].c_str()) < 190;
    cv.setClipRect(0, LIST_TOP, 240, LIST_HEIGHT);
    text(st.lps[i], y, large ? F_LARGE : F_UNIT, i == edit ? C_GREEN : C_GREY, 120,
         middle_center);
    cv.clearClipRect();
  }
}

void drawLimit() {
  arc(135, 405, C_TRACK, true);
  arc(135, gauge(edit), C_DARKGREEN, true);
  arc(135, gauge(min(st.soc, edit)), C_GREEN, true);
  dot(gauge(edit), 111, 8, C_WHITE);

  text("Limit", 72, F_SMALL, C_GREY);
  line(132, {{String(edit), F_NUM60, C_WHITE}, {unit(), F_UNIT, C_GREY}});
  String now = value(st.soc) + " now";
  // energy still to charge up to the edited limit
  float add = (edit - st.soc) / 100.0f * st.capacity;
  if (!st.heating && add >= 1) now += String(DOT "+") + String(add, 0) + " kWh";
  text(now, 155, F_MED, C_WHITE);
  text("Press to set", 175, F_SMALL, C_GREY);
}

void row(const char *label, const String &value, int y) {
  text(label, y, F_MED, C_GREY, 45, baseline_left);
  text(value + " kW", y, F_MED, C_WHITE, 195, baseline_right);
}

void drawFlow() {
  // evcc signs: grid > 0 is import, battery > 0 is discharge
  // evcc energy bar: self-used solar, battery discharge, grid import, solar export
  float out = max(-st.grid, 0.0f);
  float parts[] = {max(st.pv - out, 0.0f), max(st.bat, 0.0f), max(st.grid, 0.0f), out};
  const uint32_t colors[] = {C_GREEN, C_BATTERY, C_GREY, C_EXPORT};
  float in = parts[0] + parts[1] + parts[2] + parts[3], from = -90;
  if (in <= 0) arc(-88, 268, C_GREY);
  for (int i = 0; in > 0 && i < 4; i++) {
    float span = parts[i] / in * 360;
    if (span > 4) arc(from + 2, from + span - 2, colors[i]);
    from += span;
  }

  line(85, {{kw(st.pv), F_NUM36, C_GREEN}, {"kW solar", F_MED, C_GREY}});
  row("Grid", kw(st.grid), 106);
  row("Home", kw(st.home), 126);
  row("Car", kw(st.chargePower), 145);
  if (st.batSoc < 0) return;
  // shown from the battery's side: + is charging
  row("Battery", (st.bat <= -50 ? "+" : "") + kw(-st.bat), 165);
  text(String("Battery ") + st.batSoc + " %", 185, F_SMALL, C_GREY);
}

void drawPlan() {
  float lo = 1e9, hi = -1e9;
  for (int i = 0; i < st.slots; i++) {
    lo = min(lo, st.fc[i].price);
    hi = max(hi, st.fc[i].price);
  }
  // 24 h clock face, midnight on top; cheapest third of the price range is dark green
  for (int i = 0; i < st.slots; i++) {
    const Slot &s = st.fc[i];
    float a = s.hour * 15 - 90;
    arc(a + 1, a + 14, s.planned ? C_GREEN : s.price <= lo + (hi - lo) / 3 ? C_DARKGREEN : C_SEG);
  }
  dot(st.minute / 4.0f - 90, 98, 3, C_WHITE);

  if (st.planTime.length()) {
    // as in evcc: a plan is dimmed in off and fast mode, target is soc or energy
    bool off = strcmp(MODES[st.mode].mode, "smart");
    String target = st.socPlan && st.planSoc ? String(st.planSoc) + " %"
                    : st.planEnergy > 0 ? String(st.planEnergy, 0) + " kWh" : String("");
    String window = st.planStart.length() ? st.planStart + " to " + st.planEnd : String("");
    text("Ready by", 79, F_SMALL, off ? C_DIM : C_GREY);
    text(st.planTime, 125, F_NUM48, off ? C_DIM : C_WHITE);
    text(target + (target.length() && window.length() ? DOT : "") + window, 150, F_MED,
         off ? C_DIM : C_WHITE);
  } else {
    text("No plan", 130, F_LARGE, C_DIM);
  }
  // average price of the planned hours, then the current price
  float sum = 0;
  int planned = 0;
  for (int i = 0; i < st.slots; i++)
    if (st.fc[i].planned) sum += st.fc[i].price, planned++;
  // shortcut: assumes a currency with 1/100 subunit, add a unit lookup if needed
  String now = String(st.price * 100, 0) + " ct";
  text(planned ? String(sum / planned * 100, 0) + " ct/kWh" DOT "now " + now : now + "/kWh", 169,
       F_SMALL, C_GREY);
}

// RFID card with radio waves, as in the design
void drawCard() {
  cv.drawRoundRect(105, 61, 31, 22, 3, C_GREEN);
  cv.drawRoundRect(106, 62, 29, 20, 2, C_GREEN);
  cv.fillArc(111, 72, 5, 7, -49, 49, C_GREEN);
  cv.fillArc(113, 72, 10, 12, -46, 46, C_GREEN);
  cv.fillArc(115, 72, 14, 16, -40, 40, C_GREEN);
}

void drawToast() {
  arc(0, 360, C_GREEN);
  if (toastCard) drawCard();
  text(toast[0], 106, F_SMALL, C_GREY);
  text(toast[1], 140, F_LARGE, C_WHITE);
  text(toast[2], 164, F_MED, C_GREEN);
  text(toast[3], 184, F_SMALL, C_GREY);
}

void draw() {
  cv.fillSprite(C_BG);
  if (view == TOAST) {
    drawToast();
  } else if (!st.ok) {
    text(WiFi.isConnected() ? "evcc offline" : "No WiFi", 125, F_MED, C_GREY);
  } else {
    switch (view) {
      case HOME: drawHome(); break;
      case FLOW: drawFlow(); break;
      case PLAN: drawPlan(); break;
      case MODE: drawMode(); break;
      case LIMIT: drawLimit(); break;
      case LOADPOINTS: drawLoadpoints(); break;
      default: break;
    }
  }
  cv.pushSprite(0, 0);
#ifndef ARDUINO
  simShot(cv, st.ok);
#endif
}

// ---------- input ----------

// label, title, green line, grey line; card shows the RFID symbol
void showToast(const String &label, const String &title, const String &green,
               const String &grey = "", bool card = false) {
  toast[0] = label;
  toast[1] = title;
  toast[2] = green;
  toast[3] = grey;
  toastCard = card;
  view = TOAST;
  overlayAt = millis();
}

void onRotate(int steps) {
  if (!st.ok || switching) {
    LOG("%lu rotate ignored ok=%d switching=%d\n", (unsigned long)millis(), st.ok, switching);
    return;
  }
  int list[MODE_COUNT], count = modeList(list);
  if (view == HOME) {
    view = MODE;
    edit = 0;
    for (int i = 0; i < count; i++)
      if (list[i] == st.mode) edit = i;
  }
  if (view == MODE)
    edit = constrain(edit + steps, 0, count - 1);
  else if (view == LIMIT)
    edit = constrain(edit + steps * 5, 20, 100);
  else if (view == LOADPOINTS)
    edit = constrain(edit + steps, 0, st.lpCount - 1);
  else
    return;
  overlayAt = millis();
  M5Dial.Speaker.tone(6000, 10);
}

void onPress() {
  if (!st.ok || switching) return;
  switch (view) {
    case MODE: {
      int list[MODE_COUNT];
      modeList(list);
      const Mode &m = MODES[list[edit]];
      post(String("/mode/") + m.mode);
      if (m.always) post(String("/alwayscharge/") + m.always);
      st.mode = list[edit]; // shown at once, the next poll confirms or reverts it
      view = page;
      break;
    }
    case LIMIT:
      post(String("/limitsoc/") + edit);
      st.limit = edit;
      view = page;
      break;
    case LOADPOINTS:
      if (edit + 1 == loadpoint) {
        view = page = HOME;
        break;
      }
      loadpoint = edit + 1;
      prefs.putInt("lp", loadpoint);
      switching = true; // the picker shows only the new loadpoint until its data is in
      dirty = true;     // neither view nor edit change, so redraw explicitly
      {
        char wake[48] = ""; // empty entry: no POST, only an early poll
        xQueueSend(posts, wake, 0);
      }
      break;
    case HOME:
      view = LIMIT;
      edit = st.limit;
      overlayAt = millis();
      break;
    default:
      view = page = HOME;
  }
  M5Dial.Speaker.tone(8000, 20);
}

// long press on home opens the loadpoint picker
void onHold() {
  if (!st.ok || view != HOME) return;
  view = LOADPOINTS;
  edit = loadpoint - 1;
  overlayAt = millis();
  M5Dial.Speaker.tone(4000, 40);
}

void onRfid() {
  String uid;
  char buf[3];
  for (byte i = 0; i < M5Dial.Rfid.uid.size; i++) {
    snprintf(buf, sizeof(buf), "%02x", M5Dial.Rfid.uid.uidByte[i]);
    uid += buf;
  }
  M5Dial.Rfid.PICC_HaltA();
  M5Dial.Speaker.tone(8000, 20);

  for (auto &tag : TAGS) {
    if (uid != tag.uid) continue;
    post(String("/vehicle/") + tag.vehicle);
    String title = tag.vehicle;
    for (int i = 0; i < st.vehCount; i++)
      if (st.vehNames[i] == tag.vehicle) title = st.vehTitles[i];
    showToast("Vehicle", title, "Assigned to " + st.lp,
              String(modeName(st.mode)) + DOT "limit " + value(st.limit), true);
    return;
  }
  showToast("Unknown tag", uid, "Not assigned", "", true);
}

#ifdef ARDUINO
ESP32Encoder enc;
long encRead() { return enc.getCount(); }
#else
long encRead() { return M5Dial.Encoder.read(); }
#endif

void setup() {
  auto cfg = M5.config();
#ifdef ARDUINO
  cfg.serial_baudrate = 115200; // M5Unified skips Serial.begin by default, the debug log needs it
#endif
#ifdef ARDUINO
  // the M5Dial encoder library counts in interrupts and misses edges when the contacts
  // bounce; the PCNT hardware counter with its glitch filter does not
  M5Dial.begin(cfg, false, true); // RFID
  Serial.setTxTimeoutMs(0); // never wait for a host that does not read the log
  ESP32Encoder::useInternalWeakPullResistors = puType::up; // contacts switch to GND
  enc.attachFullQuad(DIAL_ENCODER_PIN_A, DIAL_ENCODER_PIN_B);
  enc.setFilter(1023);
#else
  M5Dial.begin(cfg, true, true); // encoder, RFID
#endif
  LOG("evcc-knob boot\n");
  cv.createSprite(240, 240);
  loadFonts();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  prefs.begin("knob");
  loadpoint = prefs.getInt("lp", LOADPOINT);
  lock = xSemaphoreCreateMutex();
  posts = xQueueCreate(4, 48);
#ifdef ARDUINO
  configTzTime(TZ, "pool.ntp.org"); // plan and finish times are shown in local time
#endif
#if WEBSOCKET
  xTaskCreate(socketTask, "socket", 12288, nullptr, 1, nullptr);
#endif
  xTaskCreate(postTask, "post", 8192, nullptr, 1, nullptr);
  lastEnc = encRead();
#ifndef ARDUINO
  if (const char *p = getenv("SIM_PAGE")) view = page = (View)atoi(p); // screenshots of other pages
#endif
}

void loop() {
  M5Dial.update();
  View before = view;
  int editBefore = edit;

  long enc = encRead();
  // step past half a detent: truncating drops a click in one direction once a lost count
  // shifts the rest position; exactly half would flip back and forth at rest
  int moved = enc - lastEnc, slack = (ENC_STEP - 1) / 2;
  int steps = (moved + (moved > 0 ? slack : -slack)) / ENC_STEP;
  if (steps) {
    lastEnc += steps * ENC_STEP;
    onRotate(steps);
  }
  static long encLogged = 0;
  if (enc != encLogged) {
    // rest is the count offset from the detent: not 0 at rest means a lost count
    LOG("%lu enc=%ld moved=%d steps=%d rest=%ld view=%d edit=%d\n", (unsigned long)millis(), enc,
        moved, steps, enc - lastEnc, view, edit);
    encLogged = enc;
  }

  if (M5Dial.BtnA.wasClicked()) onPress();
  if (M5Dial.BtnA.wasHold()) onHold();

  auto touch = M5Dial.Touch.getDetail();
  if (touch.wasFlicked() && view < PAGES) {
    int dir = touch.distanceX() < 0 ? 1 : -1;
    int pages = st.heating ? PLAN : PAGES; // evcc has no plan for heating devices
    view = page = (View)((page + dir + pages) % pages);
  }

  if (M5Dial.Rfid.PICC_IsNewCardPresent() && M5Dial.Rfid.PICC_ReadCardSerial()) onRfid();

  if (switching) overlayAt = millis();
  // overlays close on their own: nothing is changed without a press
  if (view >= PAGES && millis() - overlayAt > (view == TOAST ? 3000 : 5000)) view = page;

  if (fresh) {
    xSemaphoreTake(lock, portMAX_DELAY);
    // drop a poll that was still in flight for the previous loadpoint
    if (pending.loadpoint == loadpoint) {
      st = pending;
      if (switching) view = page = HOME;
      switching = false;
    }
    fresh = false;
    xSemaphoreGive(lock);
    dirty = true;
    if (st.heating && page == PLAN) {
      if (view == PLAN) view = HOME;
      page = HOME;
    }
    // the saved loadpoint no longer exists in evcc
    if (st.ok && st.lpCount && loadpoint > st.lpCount) {
      loadpoint = 1;
    }
  }
  if (postFailed) {
    postFailed = false;
    showToast("", "Failed", "Not set");
  }

  // switching keeps overlayAt fresh to hold the picker open, but the dial rests
  bool rest = switching || (view != MODE && view != LOADPOINTS) || millis() - overlayAt > 150;
  if (rest != settled) {
    settled = rest;
    dirty = true;
  }

  if (dirty || view != before || edit != editBefore) {
    uint32_t t = millis();
    draw();
    LOG("%lu draw view=%d settled=%d %lums\n", (unsigned long)t, view, settled,
        (unsigned long)(millis() - t));
    dirty = false;
  }
}
