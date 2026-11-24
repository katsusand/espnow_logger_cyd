#!/usr/bin/env python3
"""Plot espnow_logger sensor CSVs (from export_sensor_bin_csv.py) as HTML.

Outputs, per input pair:
  - <stem>_gps_track.html     Leaflet map (OpenStreetMap tiles): track, START / GOAL
  - <stem>_imu_dashboard.html Plotly time-series dashboard

Pure standard library; the generated HTML loads Leaflet / Plotly from a CDN, so
opening it needs network access. The HTML embeds the GPS positions: do not
commit or publish it.
"""

from __future__ import annotations

import argparse
import csv
import json
import sys
from dataclasses import dataclass
from pathlib import Path

LEAFLET_VERSION = "1.9.3"
PLOTLY_VERSION = "3.3.1"

IMU_REQUIRED = [
    "record_ts_ms", "ax_mg", "ay_mg", "az_mg",
    "gx_mdps", "gy_mdps", "gz_mdps", "temp_cdeg",
]
GPS_REQUIRED = [
    "record_ts_ms", "lat_e7", "lon_e7", "alt_mm", "speed_mmps",
    "hdop_centi", "sats", "fix_type",
]


@dataclass
class ImuData:
    t_ms: list      # record_ts_ms
    ax_g: list
    ay_g: list
    az_g: list
    gx_dps: list
    gy_dps: list
    gz_dps: list
    temp_c: list
    total_rows: int = 0
    skipped_rows: int = 0


@dataclass
class GpsData:
    t_ms: list
    lat: list
    lon: list
    alt_m: list
    speed_kmh: list
    hdop: list
    sats: list
    fix_type: list
    total_rows: int = 0
    skipped_rows: int = 0   # rows dropped as "no fix" / invalid / malformed


def _read_rows(path: Path, required: list):
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        missing = [c for c in required if c not in (reader.fieldnames or [])]
        if missing:
            raise ValueError(f"{path}: missing columns: {', '.join(missing)}")
        yield from reader


def load_imu(path: Path) -> ImuData:
    """Raw integer units -> g, deg/s, degC. Malformed rows are skipped."""
    d = ImuData([], [], [], [], [], [], [], [])
    for row in _read_rows(path, IMU_REQUIRED):
        d.total_rows += 1
        try:
            t = int(row["record_ts_ms"])
            v = [int(row[k]) for k in IMU_REQUIRED[1:]]
        except (ValueError, TypeError):
            d.skipped_rows += 1
            continue
        d.t_ms.append(t)
        d.ax_g.append(v[0] / 1000.0)
        d.ay_g.append(v[1] / 1000.0)
        d.az_g.append(v[2] / 1000.0)
        d.gx_dps.append(v[3] / 1000.0)
        d.gy_dps.append(v[4] / 1000.0)
        d.gz_dps.append(v[5] / 1000.0)
        d.temp_c.append(v[6] / 100.0)
    return d


def load_gps(path: Path) -> GpsData:
    """Raw integer units -> degrees, m, km/h.

    Dropped rows: malformed, fix_type == 0 (no fix), lat/lon == (0, 0), or
    lat/lon outside +-90 / +-180. Note: real logs also contain fix_type values
    other than 0..2 (e.g. 1 and 6); only 0 is treated as "no fix".
    """
    d = GpsData([], [], [], [], [], [], [], [])
    for row in _read_rows(path, GPS_REQUIRED):
        d.total_rows += 1
        try:
            t = int(row["record_ts_ms"])
            lat_e7 = int(row["lat_e7"])
            lon_e7 = int(row["lon_e7"])
            alt_mm = int(row["alt_mm"])
            speed = int(row["speed_mmps"])
            hdop = int(row["hdop_centi"])
            sats = int(row["sats"])
            fix = int(row["fix_type"])
        except (ValueError, TypeError):
            d.skipped_rows += 1
            continue
        if (fix == 0 or (lat_e7 == 0 and lon_e7 == 0)
                or abs(lat_e7) > 900_000_000 or abs(lon_e7) > 1_800_000_000):
            d.skipped_rows += 1
            continue
        d.t_ms.append(t)
        d.lat.append(lat_e7 / 1e7)
        d.lon.append(lon_e7 / 1e7)
        d.alt_m.append(alt_mm / 1000.0)
        d.speed_kmh.append(speed / 1000.0 * 3.6)
        d.hdop.append(hdop / 100.0)
        d.sats.append(sats)
        d.fix_type.append(fix)
    return d


def _js(obj) -> str:
    """JSON safe to embed inside <script>."""
    return json.dumps(obj, separators=(",", ":")).replace("</", "<\\/")


def _r(values, n=3):
    return [round(v, n) for v in values]


def build_map_html(gps: GpsData, title: str, t0_ms: int) -> str:
    if not gps.lat:
        raise ValueError("no usable GPS points")
    pts = [[round(a, 7), round(b, 7)] for a, b in zip(gps.lat, gps.lon)]
    info = [
        f"t={(t - t0_ms) / 1000.0:.1f}s, {s:.1f} km/h, alt {a:.1f} m, sats {n}, hdop {h:.2f}"
        for t, s, a, n, h in zip(gps.t_ms, gps.speed_kmh, gps.alt_m, gps.sats, gps.hdop)
    ]
    return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title}</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/leaflet@{LEAFLET_VERSION}/dist/leaflet.css"/>
<script src="https://cdn.jsdelivr.net/npm/leaflet@{LEAFLET_VERSION}/dist/leaflet.js"></script>
<style>html,body,#map{{height:100%;margin:0}}</style></head>
<body><div id="map"></div>
<script>
var pts={_js(pts)}, info={_js(info)};
var map=L.map('map');
L.tileLayer('https://tile.openstreetmap.org/{{z}}/{{x}}/{{y}}.png',
  {{maxZoom:19,attribution:'&copy; OpenStreetMap contributors'}}).addTo(map);
var line=L.polyline(pts,{{color:'#3388ff',weight:4}}).addTo(map);
pts.forEach(function(p,i){{
  L.circleMarker(p,{{radius:3,color:'#1f5fbf',weight:1,fillOpacity:0.8}}).bindTooltip(info[i]).addTo(map);
}});
L.marker(pts[0]).bindTooltip('START',{{sticky:true}}).addTo(map);
L.marker(pts[pts.length-1]).bindTooltip('GOAL',{{sticky:true}}).addTo(map);
map.fitBounds(line.getBounds());
</script></body></html>
"""


def build_dashboard_html(imu: ImuData | None, gps: GpsData | None, title: str, t0_ms: int) -> str:
    if imu is None or not imu.t_ms:
        raise ValueError("no IMU samples")
    ts = _r([(t - t0_ms) / 1000.0 for t in imu.t_ms], 3)
    acc_norm = _r([(x * x + y * y + z * z) ** 0.5 for x, y, z in zip(imu.ax_g, imu.ay_g, imu.az_g)], 4)
    gyro_norm = _r([(x * x + y * y + z * z) ** 0.5 for x, y, z in zip(imu.gx_dps, imu.gy_dps, imu.gz_dps)], 3)
    traces = [
        # (name, y, yaxis row, unit)
        ("ax_g", _r(imu.ax_g, 3), 1), ("ay_g", _r(imu.ay_g, 3), 1), ("az_g", _r(imu.az_g, 3), 1),
        ("acc_norm_g", acc_norm, 2),
        ("gx_dps", _r(imu.gx_dps, 3), 3), ("gy_dps", _r(imu.gy_dps, 3), 3), ("gz_dps", _r(imu.gz_dps, 3), 3),
        ("gyro_norm_dps", gyro_norm, 4),
        ("temp_c", _r(imu.temp_c, 2), 5),
    ]
    data = [{"type": "scattergl", "mode": "lines", "name": n, "x": ts, "y": y,
             "xaxis": "x", "yaxis": "y" if row == 1 else f"y{row}"} for n, y, row in traces]
    if gps is not None and gps.t_ms:
        data.append({
            "type": "scattergl", "mode": "lines", "name": "GPS speed km/h",
            "x": _r([(t - t0_ms) / 1000.0 for t in gps.t_ms], 3), "y": _r(gps.speed_kmh, 2),
            "xaxis": "x", "yaxis": "y6",
        })
    # 5 stacked rows; temperature (row 5) has GPS speed on a secondary right axis (y6).
    n, gap = 5, 0.04
    h = (1 - gap * (n - 1)) / n
    layout = {"title": {"text": title}, "height": 1100, "hovermode": "x unified",
              "legend": {"orientation": "h"},
              "xaxis": {"title": {"text": "Time from IMU log start [s]"}, "anchor": "y5"}}
    labels = {1: "g", 2: "g", 3: "deg/s", 4: "deg/s", 5: "℃"}
    subtitles = {1: "Acceleration axes", 2: "Acceleration magnitude", 3: "Gyro axes",
                 4: "Gyro magnitude", 5: "Temperature and GPS speed"}
    annotations = []
    for row in range(1, n + 1):
        top = 1 - (row - 1) * (h + gap)
        key = "yaxis" if row == 1 else f"yaxis{row}"
        layout[key] = {"domain": [top - h, top], "title": {"text": labels[row]}, "anchor": "x"}
        annotations.append({"text": subtitles[row], "xref": "paper", "yref": "paper",
                            "x": 0, "y": top, "xanchor": "left", "yanchor": "bottom",
                            "showarrow": False, "font": {"size": 13}})
    layout["yaxis6"] = {"domain": layout["yaxis5"]["domain"], "overlaying": "y5", "side": "right",
                        "title": {"text": "km/h"}, "anchor": "x"}
    layout["annotations"] = annotations
    layout["margin"] = {"t": 80}
    return f"""<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{title}</title>
<script src="https://cdn.plot.ly/plotly-{PLOTLY_VERSION}.min.js" charset="utf-8"></script>
<style>body{{margin:0;font-family:sans-serif}}</style></head>
<body><div id="chart"></div>
<script>
Plotly.newPlot('chart',{_js(data)},{_js(layout)},{{responsive:true}});
</script></body></html>
"""


def _stem(path: Path, suffix: str) -> str:
    s = path.stem
    return s[: -len(suffix)] if s.endswith(suffix) else s


def run(gps_path: Path | None, imu_path: Path | None, out_dir: Path) -> list[Path]:
    gps = load_gps(gps_path) if gps_path else None
    imu = load_imu(imu_path) if imu_path else None
    # Common time origin: first IMU sample if any, else first GPS point.
    if imu and imu.t_ms:
        t0 = imu.t_ms[0]
    elif gps and gps.t_ms:
        t0 = gps.t_ms[0]
    else:
        t0 = 0
    out_dir.mkdir(parents=True, exist_ok=True)
    written = []
    if gps_path:
        stem = _stem(gps_path, "_gps")
        print(f"{gps_path}: rows={gps.total_rows} points={len(gps.t_ms)} dropped={gps.skipped_rows}")
        if gps.t_ms:
            p = out_dir / f"{stem}_gps_track.html"
            p.write_text(build_map_html(gps, f"{stem} GPS track", t0), encoding="utf-8")
            written.append(p)
        else:
            print(f"{gps_path}: no usable GPS points; map not written", file=sys.stderr)
    if imu_path:
        stem = _stem(imu_path, "_imu")
        print(f"{imu_path}: rows={imu.total_rows} samples={len(imu.t_ms)} dropped={imu.skipped_rows}")
        if imu.t_ms:
            p = out_dir / f"{stem}_imu_dashboard.html"
            p.write_text(build_dashboard_html(imu, gps, f"{stem} IMU dashboard", t0), encoding="utf-8")
            written.append(p)
        else:
            print(f"{imu_path}: no IMU samples; dashboard not written", file=sys.stderr)
    return written


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Plot espnow_logger CSVs (from export_sensor_bin_csv.py) as a GPS map and an IMU dashboard. "
                    "Pass *_gps.csv and/or *_imu.csv (detected by file name suffix). "
                    "Generated HTML contains GPS positions and loads Leaflet/Plotly from a CDN.")
    parser.add_argument("csv", nargs="+", type=Path, help="<name>_gps.csv and/or <name>_imu.csv (at most one of each)")
    parser.add_argument("--out", type=Path, required=True, help="output directory (do not point inside the repo tracked tree)")
    args = parser.parse_args(argv)

    gps_path = imu_path = None
    for p in args.csv:
        if p.stem.endswith("_gps") and gps_path is None:
            gps_path = p
        elif p.stem.endswith("_imu") and imu_path is None:
            imu_path = p
        else:
            parser.error(f"cannot classify (need one *_gps.csv and/or one *_imu.csv): {p}")
    for p in (gps_path, imu_path):
        if p is not None and not p.exists():
            print(f"missing input: {p}", file=sys.stderr)
            return 1
    try:
        written = run(gps_path, imu_path, args.out)
    except (ValueError, OSError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    for p in written:
        print(f"wrote {p}")
    return 0 if written else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
