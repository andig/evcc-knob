// jq run by evcc (/api/state?jq=): shrinks the state and does all time math
// server-side, so no NTP/timezone here. Prefix with "<index> as $i|".
#pragma once

static const char QUERY[] = R"jq(
def ep: if . == null or .[0:4] == "0001" then null
  else (sub("\\.[0-9]+"; "") | sub("Z$"; "+00:00") | strptime("%Y-%m-%dT%H:%M:%S%z") | mktime) end;
def hm: if . == null then null else strflocaltime("%H:%M") end;
.loadpoints[$i] as $l
| (.vehicles[$l.vehicleName // ""] // {}) as $v
| ($l.planProjectedStart | ep) as $ps
| ($l.planProjectedEnd | ep) as $pe
| {
  min: (now | localtime | .[3] * 60 + .[4]),
  lp: $l.title, mode: $l.mode, cp: $l.chargePower, soc: $l.vehicleSoc,
  cont: $l.chargerFeatureContinuous, sw: $l.chargerFeatureSwitchDevice,
  heat: $l.chargerFeatureHeating, dev: $l.chargerFeatureIntegratedDevice,
  cap: $v.capacity, vt: [.vehicles | to_entries[] | [.key, .value.title]],
  ac: $l.alwaysCharge, lim: $l.effectiveLimitSoc, con: $l.connected, veh: $l.vehicleTitle,
  fin: (if ($l.chargeRemainingDuration // 0) > 0 then now + $l.chargeRemainingDuration | hm else null end),
  pt: ($l.effectivePlanTime | ep | hm), psoc: $l.effectivePlanSoc, pen: $l.planEnergy,
  sbp: ((($l.vehicleName // "") != "" and (($v.features // []) | index("Offline") | not)
    or ($l.vehicleSoc // 0) > 0) and ($v.capacity // 0) > 0),
  ps: ($ps | hm), pe: ($pe | hm), psx: $ps, pex: $pe,
  pv: .pvPower, grid: .grid.power, home: .homePower,
  bat: .battery.power, bsoc: .battery.soc,
  price: .tariffGrid, lps: [.loadpoints[].title]
}
)jq";

// grid price forecast, fetched only every few minutes: the next 24 hours as
// [local hour, average price, hour start in epoch seconds]
static const char FORECAST[] = R"jq(
[(.forecast.grid // [])[] | select(.[1] > now)]
| group_by(.[0] / 3600 | floor) | .[0:24]
| map((.[0][0] / 3600 | floor * 3600) as $h | [($h | localtime | .[3]), (map(.[2]) | add / length), $h])
)jq";
