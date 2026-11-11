# smart-knob

[evcc](https://evcc.io) remote for the [M5Stack Dial](https://shop.m5stack.com/products/m5stack-dial-v1-1): one loadpoint per dial.

- Home: charge power, mode, vehicle SoC with limit marker
- Rotate: pick mode, press to set
- Press: edit SoC limit, press to set
- Swipe: energy flow, plan with 24 h price ring
- RFID tag: assign a vehicle to the loadpoint

## Build

```sh
cp include/config.example.h include/config.h   # WiFi, evcc URL, loadpoint, RFID tags
pio run -t upload
```

The dial polls `GET /api/state?jq=…` (query in `include/query.h`) and writes via
`POST /api/loadpoints/<n>/{mode,limitsoc,vehicle}/…`. Plain HTTP, no evcc authentication.
