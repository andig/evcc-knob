#include <M5Dial.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "query.h"

const uint32_t C_BG = 0x020318, C_TRACK = 0x1C2030, C_SEG = 0x2A2E40, C_GREY = 0x93949E,
               C_DIM = 0x6F7080, C_GREEN = 0x0FDE41, C_DARKGREEN = 0x0B5C26, C_WHITE = 0xFFFFFF;

const auto *F_SMALL = &fonts::FreeSans9pt7b;
const auto *F_MED = &fonts::FreeSansBold9pt7b;
const auto *F_LARGE = &fonts::FreeSansBold18pt7b;
const auto *F_HUGE = &fonts::FreeSansBold24pt7b;

const char *MODES[] = {"off", "pv", "minpv", "now"};
const char *MODE_LABELS[] = {"Off", "Solar", "Min+Solar", "Fast"};

struct Slot {
  uint8_t hour;
  float price;
  bool planned;
};

struct State {
  bool ok = false, connected = false;
  String lp, veh, fin, planTime, planStart, planEnd;
  int mode = 0, soc = 0, limit = 0, planSoc = 0, batSoc = 0, minute = 0, slots = 0;
  float chargePower = 0, pv = 0, grid = 0, home = 0, bat = 0, price = 0;
  Slot fc[24];
} st;

enum View { HOME, FLOW, PLAN, MODE, LIMIT, TOAST };
const int PAGES = 3; // HOME, FLOW, PLAN are swipeable pages, the rest overlays

M5Canvas cv(&M5Dial.Display);
View view = HOME, page = HOME;
int edit = 0;             // value being edited in MODE/LIMIT
uint32_t overlayAt = 0;   // last interaction with an overlay

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
  static const String url = String(EVCC_URL) + "/api/state?jq=" +
                            urlencode(String(LOADPOINT - 1) + " as $i|" + QUERY);
  State next;
  if (!WiFi.isConnected()) return next;
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(url);
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
  for (int i = 0; i < 4; i++)
    if (!strcmp(mode, MODES[i])) next.mode = i;

  next.slots = 0;
  for (JsonArray s : doc["fc"].as<JsonArray>()) {
    if (next.slots == 24) break;
    next.fc[next.slots++] = {s[0].as<uint8_t>(), s[1].as<float>(), s[2].as<int>() == 1};
  }
  return next;
}

void post(const String &path) {
  char buf[48];
  strlcpy(buf, path.c_str(), sizeof(buf));
  xQueueSend(posts, buf, 0);
}

// all HTTP runs here, so the UI loop never waits for the network
void netTask(void *) {
  char path[48];
  for (;;) {
    // a queued POST wakes the task early and is followed by a fresh poll
    if (xQueueReceive(posts, path, pdMS_TO_TICKS(3000))) {
      HTTPClient http;
      http.setTimeout(3000);
      http.begin(String(EVCC_URL) + "/api/loadpoints/" + LOADPOINT + path);
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
  cv.drawString(s, x, y);
}

void arc(float from, float to, uint32_t color) {
  if (to > from) cv.fillArc(120, 120, 106, 116, from, to, color);
}

void dot(float angle, int radius, int size, uint32_t color) {
  float rad = angle * DEG_TO_RAD;
  cv.fillCircle(120 + radius * cosf(rad), 120 + radius * sinf(rad), size, color);
}

// gauge spans 270 degrees, open at the bottom
float gauge(float percent) { return 135 + constrain(percent, 0.0f, 100.0f) * 2.7f; }

String kw(float w) { return String(w / 1000, 1); }

void drawHome() {
  arc(135, 405, C_TRACK);
  arc(135, gauge(st.soc), C_GREEN);
  dot(gauge(st.limit), 111, 5, C_WHITE);
  dot(gauge(st.limit), 111, 3, C_BG);

  text(st.lp, 56, F_SMALL, C_GREY);
  text(MODE_LABELS[st.mode], 80, F_MED, C_GREEN);
  if (!st.connected) {
    text("--", 120, F_HUGE, C_DIM);
    text("Not connected", 172, F_MED, C_WHITE);
    return;
  }
  text(kw(st.chargePower), 120, F_HUGE, C_WHITE);
  text("kW", 150, F_SMALL, C_GREY);
  text(st.veh.length() ? st.veh + " " + st.soc + "%" : String("Guest vehicle"), 174, F_MED, C_WHITE);
  if (st.fin.length()) text(String(st.limit) + "% at " + st.fin, 196, F_SMALL, C_GREY);
}

void drawMode() {
  for (int i = 0; i < 4; i++) {
    arc(135 + i * 70, 195 + i * 70, i == edit ? C_GREEN : C_SEG);
    int y = 120 + (i - edit) * 38;
    if (y < 45 || y > 195) continue;
    if (i == edit)
      text(MODE_LABELS[i], y, F_LARGE, C_GREEN);
    else
      text(MODE_LABELS[i], y, F_MED, C_GREY);
  }
}

void drawLimit() {
  arc(135, 405, C_TRACK);
  arc(135, gauge(edit), C_DARKGREEN);
  arc(135, gauge(min(st.soc, edit)), C_GREEN);
  dot(gauge(edit), 111, 8, C_WHITE);

  text("Limit", 66, F_SMALL, C_GREY);
  text(String(edit), 116, F_HUGE, C_WHITE);
  text("%", 148, F_SMALL, C_GREY);
  text(String(st.soc) + "% now", 176, F_MED, C_WHITE);
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
      default: break;
    }
  }
  cv.pushSprite(0, 0);
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
  if (view == HOME) {
    view = MODE;
    edit = st.mode;
  }
  if (view == MODE)
    edit = constrain(edit + steps, 0, 3);
  else if (view == LIMIT)
    edit = constrain(edit + steps * 5, 20, 100);
  else
    return;
  overlayAt = millis();
  M5Dial.Speaker.tone(6000, 10);
}

void onPress() {
  if (!st.ok) return;
  switch (view) {
    case MODE:
      post(String("/mode/") + MODES[edit]);
      st.mode = edit; // shown at once, the next poll confirms or reverts it
      view = page;
      break;
    case LIMIT:
      post(String("/limitsoc/") + edit);
      st.limit = edit;
      view = page;
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
  WiFi.begin(WIFI_SSID, WIFI_PASS);
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

  if (M5Dial.BtnA.wasPressed()) onPress();

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
    st = pending;
    fresh = false;
    xSemaphoreGive(lock);
    dirty = true;
  }
  if (postFailed) {
    postFailed = false;
    showToast("Failed", "not set");
  }

  if (dirty || view != before || edit != editBefore) {
    draw();
    dirty = false;
  }
}
