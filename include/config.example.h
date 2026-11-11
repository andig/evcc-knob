// Copy to config.h and adjust.
#pragma once

#define WIFI_SSID "my-wifi"
#define WIFI_PASS "my-password"

#define EVCC_URL "http://evcc.local:7070" // no trailing slash
#define LOADPOINT 1                       // 1-based, as in the evcc API

#define ENC_STEP 4 // encoder counts per detent

// RFID tag UID (lowercase hex) -> evcc vehicle name (not title).
// An unknown tag shows its UID on screen, copy it from there.
static const struct {
  const char *uid;
  const char *vehicle;
} TAGS[] = {
    {"04a1b2c3", "vehicle_1"},
};
