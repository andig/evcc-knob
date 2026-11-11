// jq run by evcc (/api/state?jq=): shrinks the state and does all time math
// server-side, so no NTP/timezone here. Prefix with "<index> as $i|".
#pragma once

static const char QUERY[] = R"jq(
def ep: if . == null or .[0:4] == "0001" then null
  else (sub("\\.[0-9]+"; "") | sub("Z$"; "+00:00") | strptime("%Y-%m-%dT%H:%M:%S%z") | mktime) end;
def hm: if . == null then null else strflocaltime("%H:%M") end;
.loadpoints[$i] as $l
| ($l.planProjectedStart | ep) as $ps
| ($l.planProjectedEnd | ep) as $pe
| {
  min: (now | localtime | .[3] * 60 + .[4]),
  lp: $l.title, mode: $l.mode, cp: $l.chargePower, soc: $l.vehicleSoc,
  lim: $l.effectiveLimitSoc, con: $l.connected, veh: $l.vehicleTitle,
  fin: (if ($l.chargeRemainingDuration // 0) > 0 then now + $l.chargeRemainingDuration | hm else null end),
  pt: ($l.effectivePlanTime | ep | hm), psoc: $l.effectivePlanSoc,
  ps: ($ps | hm), pe: ($pe | hm),
  pv: .pvPower, grid: .grid.power, home: .homePower,
  bat: .battery.power, bsoc: .battery.soc,
  price: .tariffGrid,
  fc: ([(.forecast.grid // [])[] | select(.[1] > now)]
    | group_by(.[0] / 3600 | floor) | .[0:24]
    | map((.[0][0] / 3600 | floor * 3600) as $h | [
        ($h | localtime | .[3]),
        (map(.[2]) | add / length),
        (if $ps != null and $pe != null and $h + 3600 > $ps and $h < $pe then 1 else 0 end)
      ]))
}
)jq";
