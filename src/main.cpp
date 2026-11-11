#include "config.h"
#ifdef ARDUINO
#include <M5Dial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#else
#include "sim.h"
#endif
#include <ArduinoJson.h>
#include "query.h"
#include "icons.h"
#include "fonts.h"

const uint32_t C_BG = 0x020318, C_TRACK = 0x1C2030, C_SEG = 0x2A2E40, C_GREY = 0x93949E,
               C_DIM = 0x6F7080, C_GREEN = 0x0FDE41, C_DARKGREEN = 0x0B5C26, C_WHITE = 0xFFFFFF;

lgfx::VLWfont fontSmall, fontMed, fontLarge, fontHuge;
const lgfx::IFont *F_SMALL = &fontSmall, *F_MED = &fontMed, *F_LARGE = &fontLarge, *F_HUGE = &fontHuge;

void loadFonts() {
  static lgfx::PointerWrapper data[] = {{FONT_SMALL, sizeof(FONT_SMALL)},
                                        {FONT_MED, sizeof(FONT_MED)},
                                        {FONT_LARGE, sizeof(FONT_LARGE)},
                                        {FONT_HUGE, sizeof(FONT_HUGE)}};
  lgfx::VLWfont *all[] = {&fontSmall, &fontMed, &fontLarge, &fontHuge};
  for (int i = 0; i < 4; i++) all[i]->loadFont(&data[i]);
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
  String lp, veh, fin, planTime, planStart, planEnd;
  int mode = 0, soc = 0, limit = 0, planSoc = 0, batSoc = 0, minute = 0, slots = 0;
  float chargePower = 0, pv = 0, grid = 0, home = 0, bat = 0, price = 0;
  Slot fc[24];
  int loadpoint = 0, lpCount = 0; // loadpoint this state was polled for
  String lps[8];                  // titles of all loadpoints
} st;

enum View { HOME, FLOW, PLAN, MODE, LIMIT, LOADPOINTS, TOAST };
const int PAGES = 3; // HOME, FLOW, PLAN are swipeable pages, the rest overlays

M5Canvas cv(&M5Dial.Display);
Preferences prefs;
View view = HOME, page = HOME;
int edit = 0;             // value being edited in MODE/LIMIT/LOADPOINTS
float scrollPos = 0;      // picker list position, eases towards edit
uint32_t overlayAt = 0;   // last interaction with an overlay
volatile int loadpoint = LOADPOINT; // 1-based, LOADPOINT is only the first-boot default

// handover from the network task: st itself is only touched by the UI loop
SemaphoreHandle_t lock;
QueueHandle_t posts;
State pending;
volatile bool fresh = false, postFailed = false;
long lastEnc = 0;
String toast1, toast2;
bool dirty = true;

// ---------- evcc API ----------

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

State poll() {
  static const String base = String(EVCC_URL) + "/api/state?jq=",
                      query = urlencode(String(" as $i|") + QUERY);
  State next;
  next.loadpoint = loadpoint;
  if (!WiFi.isConnected()) return next;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(base + (next.loadpoint - 1) + query);
  JsonDocument doc;
  bool ok = http.GET() == HTTP_CODE_OK && !deserializeJson(doc, http.getString());
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
  next.soc = doc["soc"] | 0;
  next.limit = doc["lim"] | 0;
  next.planSoc = doc["psoc"] | 0;
  next.batSoc = doc["bsoc"] | 0;
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

  next.slots = 0;
  for (JsonArray s : doc["fc"].as<JsonArray>()) {
    if (next.slots == 24) break;
    next.fc[next.slots++] = {s[0].as<uint8_t>(), s[1].as<float>(), s[2].as<int>() == 1};
  }
  for (JsonVariant title : doc["lps"].as<JsonArray>()) {
    if (next.lpCount == 8) break;
    next.lps[next.lpCount++] = title | "";
  }
  return next;
}

// queue a POST for the current loadpoint; an empty path only triggers a poll
void post(const String &path) {
  char buf[48] = "";
  if (path.length())
    strlcpy(buf, (String("/api/loadpoints/") + loadpoint + path).c_str(), sizeof(buf));
  xQueueSend(posts, buf, 0);
}

// all HTTP runs here, so the UI loop never waits for the network
void netTask(void *) {
  char path[48];
  for (;;) {
    // a queued POST wakes the task early and is followed by a fresh poll
    if (xQueueReceive(posts, path, pdMS_TO_TICKS(3000)) && path[0]) {
      HTTPClient http;
      http.setTimeout(3000);
      http.begin(String(EVCC_URL) + path);
      if (http.POST("") != HTTP_CODE_OK) postFailed = true;
      http.end();
    }
    State next = poll();
    xSemaphoreTake(lock, portMAX_DELAY);
    pending = next;
    fresh = true;
    xSemaphoreGive(lock);
  }
}

// ---------- drawing ----------

void text(const String &s, int y, const lgfx::IFont *font, uint32_t color, int x = 120,
          textdatum_t datum = middle_center) {
  cv.setFont(font);
  cv.setTextColor(color);
  cv.setTextDatum(datum);
  cv.drawString(s.c_str(), x, y);
}

// anti-aliased ring segment, degrees clockwise from 3 o'clock, optionally with round ends
void arc(float from, float to, uint32_t color, bool round = false) {
  const float R0 = 106, R1 = 116, CAP = 5;
  float span = to - from;
  if (span <= 0) return;
  float capX[2], capY[2];
  for (int i = 0; i < 2; i++) {
    float rad = (i ? to : from) * DEG_TO_RAD;
    capX[i] = (R0 + R1) / 2 * cosf(rad);
    capY[i] = (R0 + R1) / 2 * sinf(rad);
  }
  int r = color >> 16 & 0xFF, g = color >> 8 & 0xFF, b = color & 0xFF;

  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 240; x++) {
      float dx = x - 120, dy = y - 120, d2 = dx * dx + dy * dy;
      if (d2 < (R0 - 1) * (R0 - 1) || d2 > (R1 + 1) * (R1 + 1)) continue;
      // coverage: distance in pixels to the nearest edge, 0.5 on the edge itself
      float d = sqrtf(d2), a = min(d - R0, R1 - d) + 0.5f;
      if (span < 360) {
        float t = fmodf(atan2f(dy, dx) / DEG_TO_RAD - from, 360);
        if (t < 0) t += 360;
        float along = t <= span ? min(t, span - t) : -min(t - span, 360 - t);
        a = min(a, along * (float)DEG_TO_RAD * d + 0.5f);
        for (int i = 0; round && i < 2; i++)
          a = max(a, min(CAP + 0.5f - hypotf(dx - capX[i], dy - capY[i]), 1.0f));
      }
      if (a <= 0) continue;
      if (a >= 1) {
        cv.drawPixel(x, y, color);
        continue;
      }
      auto bg = cv.readPixelRGB(x, y);
      cv.drawPixel(x, y, lgfx::color888(bg.r + (r - bg.r) * a, bg.g + (g - bg.g) * a,
                                        bg.b + (b - bg.b) * a));
    }
  }
}

void dot(float angle, int radius, int size, uint32_t color) {
  float rad = angle * DEG_TO_RAD;
  cv.fillSmoothCircle(120 + radius * cosf(rad), 120 + radius * sinf(rad), size, color);
}

// gauge spans 270 degrees, open at the bottom
float gauge(float percent) { return 135 + constrain(percent, 0.0f, 100.0f) * 2.7f; }

String kw(float w) { return String(w / 1000, 1); }

// unit of soc and limit: heating devices report a temperature
const char *unit() { return st.heating ? "\xC2\xB0" "C" : "%"; }

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

void modeLabel(int i, int y, bool large, uint32_t color) {
  const Mode &m = MODES[i];
  const char *label = modeName(i);
  const auto *font = large ? F_LARGE : F_MED;
  int size = large ? 32 : 16, gap = large ? 8 : 5;
  cv.setFont(font);
  int width = cv.textWidth(label) + (m.icon16 ? gap + size : 0);
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

  text(st.lp, 56, F_SMALL, C_GREY);
  modeLabel(st.mode, 80, false, C_GREEN);
  if (!st.connected) {
    text("--", 120, F_HUGE, C_DIM);
    text("Not connected", 172, F_MED, C_WHITE);
    return;
  }
  text(kw(st.chargePower), 120, F_HUGE, C_WHITE);
  text("kW", 150, F_SMALL, C_GREY);
  // heaters and other integrated devices have no vehicle
  if (st.integrated)
    text(st.heating ? String(st.soc) + unit() : String(""), 174, F_MED, C_WHITE);
  else
    text(st.veh.length() ? st.veh + " " + st.soc + "%" : String("Guest vehicle"), 174, F_MED, C_WHITE);
  if (st.fin.length()) text(String(st.limit) + unit() + " at " + st.fin, 196, F_SMALL, C_GREY);
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
    int y = 120 + (i - scrollPos) * 38;
    if (y < 45 || y > 195) continue;
    cv.setClipRect(0, LIST_TOP, 240, LIST_HEIGHT); // keep sliding entries off the arrows
    modeLabel(list[i], y, i == edit, i == edit ? C_GREEN : C_GREY);
    cv.clearClipRect();
  }
  listArrows(count);
}

void drawLoadpoints() {
  listArrows(st.lpCount);
  float step = 270.0f / st.lpCount;
  for (int i = 0; i < st.lpCount; i++) {
    arc(135 + i * step + 5, 135 + (i + 1) * step - 5, i == edit ? C_GREEN : C_SEG, true);
    int y = 120 + (i - scrollPos) * 38;
    if (y < 45 || y > 195) continue;
    // long titles do not fit the large font
    cv.setFont(F_LARGE);
    bool large = i == edit && cv.textWidth(st.lps[i].c_str()) < 190;
    cv.setClipRect(0, LIST_TOP, 240, LIST_HEIGHT);
    text(st.lps[i], y, large ? F_LARGE : F_MED, i == edit ? C_GREEN : C_GREY);
    cv.clearClipRect();
  }
}

void drawLimit() {
  arc(135, 405, C_TRACK, true);
  arc(135, gauge(edit), C_DARKGREEN, true);
  arc(135, gauge(min(st.soc, edit)), C_GREEN, true);
  dot(gauge(edit), 111, 8, C_WHITE);

  text("Limit", 66, F_SMALL, C_GREY);
  text(String(edit), 116, F_HUGE, C_WHITE);
  text(unit(), 148, F_SMALL, C_GREY);
  text(String(st.soc) + unit() + " now", 176, F_MED, C_WHITE);
}

void row(const char *label, const String &value, int y) {
  text(label, y, F_SMALL, C_GREY, 52, middle_left);
  text(value, y, F_MED, C_WHITE, 188, middle_right);
}

void drawFlow() {
  // evcc signs: grid > 0 is import, battery > 0 is discharge
  float in = st.pv + max(st.grid, 0.0f) + max(st.bat, 0.0f);
  float solar = in > 0 ? st.pv / in * 360 : 0;
  arc(-90, 270, C_GREY);
  arc(-90, -90 + solar, C_GREEN);

  text(kw(st.pv), 62, F_LARGE, C_GREEN);
  text("kW solar", 90, F_SMALL, C_GREY);
  row("Grid", kw(st.grid), 118);
  row("Home", kw(st.home), 142);
  row("Car", kw(st.chargePower), 166);
  row("Bat", kw(-st.bat) + " " + st.batSoc + "%", 190);
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
    text("Ready by", 66, F_SMALL, C_GREY);
    text(st.planTime, 112, F_HUGE, C_WHITE);
    text(String(st.planSoc) + "%  " + st.planStart + "-" + st.planEnd, 150, F_MED, C_WHITE);
  } else {
    text("No plan", 112, F_LARGE, C_DIM);
  }
  // shortcut: assumes a currency with 1/100 subunit, add a unit lookup if needed
  text(String(st.price * 100, 1) + " ct/kWh", 176, F_SMALL, C_GREY);
}

void drawToast() {
  arc(0, 360, C_GREEN);
  text(toast1, 104, F_LARGE, C_WHITE);
  text(toast2, 142, F_MED, C_GREEN);
}

void draw() {
  cv.fillSprite(C_BG);
  if (view == TOAST) {
    drawToast();
  } else if (!st.ok) {
    text(WiFi.isConnected() ? "evcc offline" : "No WiFi", 120, F_MED, C_GREY);
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

void showToast(const String &line1, const String &line2) {
  toast1 = line1;
  toast2 = line2;
  view = TOAST;
  overlayAt = millis();
}

void onRotate(int steps) {
  if (!st.ok) return;
  int list[MODE_COUNT], count = modeList(list);
  if (view == HOME) {
    view = MODE;
    edit = 0;
    for (int i = 0; i < count; i++)
      if (list[i] == st.mode) edit = i;
    scrollPos = edit;
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
  if (!st.ok) return;
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
      loadpoint = edit + 1;
      prefs.putInt("lp", loadpoint);
      post(""); // poll the new loadpoint right away
      view = page = HOME;
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
  scrollPos = edit = loadpoint - 1;
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
    showToast("Vehicle", "assigning");
    return;
  }
  showToast("Unknown tag", uid);
}

void setup() {
  auto cfg = M5.config();
  M5Dial.begin(cfg, true, true); // encoder, RFID
  cv.createSprite(240, 240);
  loadFonts();
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  prefs.begin("knob");
  loadpoint = prefs.getInt("lp", LOADPOINT);
  lock = xSemaphoreCreateMutex();
  posts = xQueueCreate(4, 48);
  xTaskCreate(netTask, "net", 8192, nullptr, 1, nullptr);
  lastEnc = M5Dial.Encoder.read();
}

void loop() {
  M5Dial.update();
  View before = view;
  int editBefore = edit;

  long enc = M5Dial.Encoder.read();
  if (int steps = (enc - lastEnc) / ENC_STEP) {
    lastEnc += steps * ENC_STEP;
    onRotate(steps);
  }

  if (M5Dial.BtnA.wasClicked()) onPress();
  if (M5Dial.BtnA.wasHold()) onHold();

  auto touch = M5Dial.Touch.getDetail();
  if (touch.wasFlicked() && view < PAGES) {
    int dir = touch.distanceX() < 0 ? 1 : -1;
    view = page = (View)((page + dir + PAGES) % PAGES);
  }

  if (M5Dial.Rfid.PICC_IsNewCardPresent() && M5Dial.Rfid.PICC_ReadCardSerial()) onRfid();

  // overlays close on their own: nothing is changed without a press
  if (view >= PAGES && millis() - overlayAt > (view == TOAST ? 3000 : 5000)) view = page;

  if (fresh) {
    xSemaphoreTake(lock, portMAX_DELAY);
    // drop a poll that was still in flight for the previous loadpoint
    if (pending.loadpoint == loadpoint) st = pending;
    fresh = false;
    xSemaphoreGive(lock);
    dirty = true;
    // the saved loadpoint no longer exists in evcc
    if (st.ok && st.lpCount && loadpoint > st.lpCount) {
      loadpoint = 1;
      post("");
    }
  }
  if (postFailed) {
    postFailed = false;
    showToast("Failed", "not set");
  }

  // picker lists slide to the selected entry instead of jumping
  static uint32_t lastFrame = 0;
  uint32_t dt = min((uint32_t)(millis() - lastFrame), (uint32_t)100);
  lastFrame = millis();
  if ((view == MODE || view == LOADPOINTS) && scrollPos != edit) {
    float rest = edit - scrollPos;
    scrollPos = fabsf(rest) < 0.02f ? edit : scrollPos + rest * (1 - expf(-(float)dt / 60));
    dirty = true;
  }

  if (dirty || view != before || edit != editBefore) {
    draw();
    dirty = false;
  }
}
