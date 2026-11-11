// Desktop simulator: stands in for M5Dial, WiFi, HTTPClient and FreeRTOS on a PC.
// Display, touch (mouse) and speaker come from M5Unified's SDL backend.
#pragma once

#include <M5Unified.h>
#include <SDL.h>
#include <curl/curl.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

// ---------- Arduino core ----------

using std::max;
using std::min;
typedef uint8_t byte;
#define DEG_TO_RAD 0.017453292519943295f
#define constrain(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))
inline uint32_t millis() { return SDL_GetTicks(); }

struct String : std::string {
  String() {}
  String(const char *s) : std::string(s ? s : "") {}
  String(const std::string &s) : std::string(s) {}
  String(int v) : std::string(std::to_string(v)) {}
  String(float v, int decimals) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    assign(buf);
  }
  String operator+(const String &o) const { return String(std::string(*this) + std::string(o)); }
  String operator+(const char *o) const { return *this + String(o); }
};

// settings are not kept between simulator runs
struct Preferences {
  void begin(const char *) {}
  int getInt(const char *, int fallback) { return fallback; }
  void putInt(const char *, int) {}
};

// ---------- WiFi, HTTP ----------

inline struct {
  void begin(const char *, const char *) {}
  bool isConnected() { return true; }
} WiFi;

const int HTTP_CODE_OK = 200;

struct HTTPClient {
  std::string url, body;
  long timeout = 3000;

  void setTimeout(long ms) { timeout = ms; }
  void begin(const String &u) { url = u; }
  void end() {}
  std::string getString() { return body; }
  int GET() { return request(false); }
  int POST(const char *) { return request(true); }

  int request(bool post) {
    body.clear();
    CURL *c = curl_easy_init();
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,
                     +[](char *data, size_t size, size_t n, void *out) -> size_t {
                       ((std::string *)out)->append(data, size * n);
                       return size * n;
                     });
    if (post) curl_easy_setopt(c, CURLOPT_POSTFIELDS, "");
    long code = -1;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    return code;
  }
};

// ---------- FreeRTOS ----------

typedef std::mutex *SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
inline void xSemaphoreTake(SemaphoreHandle_t m, int) { m->lock(); }
inline void xSemaphoreGive(SemaphoreHandle_t m) { m->unlock(); }

struct SimQueue {
  std::mutex m;
  std::condition_variable cv;
  std::deque<std::string> items;
  size_t size;
};
typedef SimQueue *QueueHandle_t;
#define pdMS_TO_TICKS(ms) (ms)
#define portMAX_DELAY 0

inline QueueHandle_t xQueueCreate(int, size_t size) {
  auto q = new SimQueue;
  q->size = size;
  return q;
}
inline bool xQueueSend(QueueHandle_t q, const void *item, int) {
  std::lock_guard<std::mutex> l(q->m);
  q->items.emplace_back((const char *)item, q->size);
  q->cv.notify_one();
  return true;
}
inline bool xQueueReceive(QueueHandle_t q, void *out, int ms) {
  std::unique_lock<std::mutex> l(q->m);
  if (!q->cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return !q->items.empty(); }))
    return false;
  memcpy(out, q->items.front().data(), q->size);
  q->items.pop_front();
  return true;
}
inline void xTaskCreate(void (*fn)(void *), const char *, int, void *arg, int, void *) {
  std::thread(fn, arg).detach();
}

// ---------- M5Dial ----------

// Mouse wheel or left/right = rotate, space or return = press (hold = long press),
// mouse drag = swipe, T = hold an RFID tag to the dial.
struct SimDial {
  M5GFX &Display = M5.Display;
  m5::Touch_Class &Touch = M5.Touch;
  m5::Speaker_Class &Speaker = M5.Speaker;

  struct {
    long count = 0;
    long read() { return count; }
  } Encoder;

  struct {
    bool clicked = false, hold = false, forceHold = false;
    bool wasClicked() { return clicked; }
    bool wasHold() { return hold; }
  } BtnA;

  struct {
    // not in config.example.h, so it shows as unknown until added to TAGS
    struct {
      byte size = 4;
      byte uidByte[4] = {0xde, 0xad, 0xbe, 0xef};
    } uid;
    bool present = false;
    bool PICC_IsNewCardPresent() { return present; }
    bool PICC_ReadCardSerial() { return true; }
    void PICC_HaltA() {}
  } Rfid;

  static inline std::atomic<int> wheel{0};

  void begin(m5::M5Unified::config_t cfg, bool, bool) {
    M5.begin(cfg);
    // the SDL backend owns the event loop, so watch wheel events from the side
    SDL_AddEventWatch(
        [](void *, SDL_Event *e) {
          if (e->type == SDL_MOUSEWHEEL) wheel += e->wheel.y;
          return 0;
        },
        nullptr);
  }

  void update() {
    M5.update();
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    static bool held[4];
    bool now[4] = {keys[SDL_SCANCODE_LEFT] != 0, keys[SDL_SCANCODE_RIGHT] != 0,
                   keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_RETURN],
                   keys[SDL_SCANCODE_T] != 0};
    bool hit[4], released = held[2] && !now[2];
    for (int i = 0; i < 4; i++) {
      hit[i] = now[i] && !held[i];
      held[i] = now[i];
    }
    if (hit[0]) Encoder.count -= ENC_STEP;
    if (hit[1]) Encoder.count += ENC_STEP;
    Encoder.count += wheel.exchange(0) * ENC_STEP;
    // short press = click on release, 500 ms = long press
    static uint32_t downAt;
    static bool holdFired;
    if (hit[2]) {
      downAt = millis();
      holdFired = false;
    }
    BtnA.hold = BtnA.forceHold || (now[2] && !holdFired && millis() - downAt >= 500);
    BtnA.forceHold = false;
    if (BtnA.hold) holdFired = true;
    BtnA.clicked = released && !holdFired;
    Rfid.present = hit[3];
  }
};
inline SimDial M5Dial;

// SIM_SHOT=file.png saves the first frame that shows evcc data, then exits.
// With SIM_ROTATE=n or SIM_HOLD=1 the dial is turned n steps or long-pressed first
// and the next frame is saved.
inline void simShot(M5Canvas &cv, bool ready) {
  const char *path = getenv("SIM_SHOT"), *rotate = getenv("SIM_ROTATE");
  static bool done = false;
  if (!path || !ready) return;
  if ((rotate || getenv("SIM_HOLD")) && !done) {
    done = true;
    if (rotate) M5Dial.Encoder.count += atoi(rotate) * ENC_STEP;
    else M5Dial.BtnA.forceHold = true;
    return;
  }
  size_t len = 0;
  void *png = cv.createPng(&len, 0, 0, cv.width(), cv.height());
  FILE *f = fopen(path, "wb");
  if (png && f) fwrite(png, 1, len, f);
  if (f) fclose(f);
  exit(0);
}

void setup();
void loop();

int main(int, char **) {
  return lgfx::Panel_sdl::main(
      [](bool *running) {
        setup();
        while (*running) {
          loop();
          SDL_Delay(5);
        }
        return 0;
      },
      128);
}
