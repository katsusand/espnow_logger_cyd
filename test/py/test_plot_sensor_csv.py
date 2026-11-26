"""Tests for scripts/plot_sensor_csv.py. Run: python3 -m unittest discover -s test/py"""
import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import plot_sensor_csv as P  # noqa: E402

GPS_COLS = ["record_ts_ms", "lat_e7", "lon_e7", "alt_mm", "speed_mmps", "hdop_centi", "sats", "fix_type"]
IMU_COLS = ["record_ts_ms", "ax_mg", "ay_mg", "az_mg", "gx_mdps", "gy_mdps", "gz_mdps", "temp_cdeg"]


def write_csv(path, cols, rows):
    with open(path, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        w.writerows(rows)


class PlotTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def test_gps_filtering_and_scale(self):
        p = self.tmp / "a_gps.csv"
        write_csv(p, GPS_COLS, [
            [1000, 351490457, 1370011732, 16410, 1000, 660, 4, 1],   # ok
            [2000, 0, 0, 0, 0, 0, 0, 1],                             # 0,0
            [3000, 351490000, 1370010000, 0, 0, 100, 5, 0],          # no fix
            [4000, 910000000, 0, 0, 0, 100, 5, 1],                   # out of range
            [5000, "x", 0, 0, 0, 100, 5, 1],                         # malformed
            [6000, 351489753, 1370010375, 18680, 2500, 120, 6, 6],   # ok, fix_type 6 kept
        ])
        g = P.load_gps(p)
        self.assertEqual((g.total_rows, len(g.lat), g.skipped_rows), (6, 2, 4))
        self.assertAlmostEqual(g.lat[0], 35.1490457, places=7)
        self.assertAlmostEqual(g.speed_kmh[0], 3.6)
        self.assertAlmostEqual(g.alt_m[1], 18.68)
        self.assertAlmostEqual(g.hdop[0], 6.6)

    def test_imu_scale_and_missing_column(self):
        p = self.tmp / "a_imu.csv"
        write_csv(p, IMU_COLS, [[0, 56, -87, 996, -2000, 11, -6, 3730]])
        d = P.load_imu(p)
        self.assertEqual((d.ax_g[0], d.ay_g[0], d.gx_dps[0], d.temp_c[0]), (0.056, -0.087, -2.0, 37.3))
        bad = self.tmp / "b_imu.csv"
        write_csv(bad, ["record_ts_ms"], [[1]])
        with self.assertRaises(ValueError):
            P.load_imu(bad)

    def test_run_counts_match(self):
        g, i = self.tmp / "s_gps.csv", self.tmp / "s_imu.csv"
        write_csv(g, GPS_COLS, [[100 * k, 351490000 + k, 1370000000 + k, 1000, 500, 100, 7, 1] for k in range(5)])
        write_csv(i, IMU_COLS, [[20 * k, 1, 2, 3, 4, 5, 6, 3000] for k in range(50)])
        out = self.tmp / "out"
        files = P.run(g, i, out)
        self.assertEqual(sorted(f.name for f in files), ["s_gps_track.html", "s_imu_dashboard.html"])
        self.assertEqual(files[0].read_text().count(' km/h, alt'), 5)
        self.assertIn('"name":"ax_g"', files[1].read_text())
        # GPS only / IMU only
        self.assertEqual(len(P.run(g, None, self.tmp / "o2")), 1)
        self.assertEqual(len(P.run(None, i, self.tmp / "o3")), 1)

    def test_imu_batch(self):
        import json
        cols = ["record_ts_ms", "temp_cdeg", "raw_samples_json"]
        smp = lambda i: {"offset_ms": i * 10, "ax_mg": 100 * i, "ay_mg": 0, "az_mg": 1000,
                         "gx_centi_dps": 250, "gy_centi_dps": 0, "gz_centi_dps": -100}
        p = self.tmp / "b_imu_batch.csv"
        write_csv(p, cols, [[1000, 3700, json.dumps([smp(i) for i in range(4)])],
                            [1100, 3700, "not json"]])
        d = P.load_imu_batch(p, step=1)
        self.assertEqual((d.total_rows, d.skipped_rows, len(d.t_ms)), (2, 1, 4))
        self.assertEqual((d.t_ms[1], d.ax_g[1], d.gx_dps[0], d.temp_c[0]), (1010, 0.1, 2.5, 37.0))
        d2 = P.load_imu_batch(p, step=2)
        self.assertEqual(len(d2.t_ms), 2)
        self.assertAlmostEqual(d2.ax_g[0], 0.05)
        files = P.run(None, None, self.tmp / "ob", imu_batch_path=p, step=1)
        self.assertEqual([f.name for f in files], ["b_imu_dashboard.html"])

    def test_imu_batch_dedupes_type4_against_type7(self):
        import json
        cols = ["record_ts_ms", "sequence_start", "temp_cdeg", "gps_utc_ms", "raw_samples_json", "record_type"]
        smp = lambda i: {"offset_ms": i * 10, "ax_mg": 100, "ay_mg": 0, "az_mg": 1000,
                         "gx_centi_dps": 0, "gy_centi_dps": 0, "gz_centi_dps": 0}
        raw = json.dumps([smp(i) for i in range(2)])
        p = self.tmp / "d_imu_batch.csv"
        write_csv(p, cols, [
            [1000, 10, 3700, 5000, raw, 4],   # same batch as the type 7 row: dropped
            [1000, 10, 3700, 5000, raw, 7],
            [1100, 20, 3700, 5100, raw, 4],   # no type 7 twin: kept
        ])
        d = P.load_imu_batch(p, step=1)
        self.assertEqual((d.total_rows, d.duplicate_rows, len(d.t_ms)), (2, 1, 4))
        # an older CSV without record_type is never deduplicated
        q = self.tmp / "e_imu_batch.csv"
        write_csv(q, cols[:-1], [[1000, 10, 3700, 5000, raw], [1000, 10, 3700, 5000, raw]])
        d = P.load_imu_batch(q, step=1)
        self.assertEqual((d.total_rows, d.duplicate_rows), (2, 0))

    def test_detect_turns(self):
        # 100 ms samples: stationary 5 s, then 20 km/h straight, 90 deg right turn (gz=-30 dps for 3 s), straight
        gt, gv, it = [], [], []
        for k in range(0, 200):
            gt.append(k * 1000)
            gv.append(0 if k < 5 else 20 / 3.6 * 1000)
        g = P.GpsData(gt, [35.0] * 200, [137.0] * 200, [0] * 200, [(v / 1000 * 3.6) for v in gv], [1] * 200, [8] * 200, [1] * 200)
        imu = P.ImuData([], [], [], [], [], [], [], [])
        for i in range(2000):
            tm = i * 100
            imu.t_ms.append(tm)
            imu.ax_g.append(0); imu.ay_g.append(0); imu.az_g.append(1.0)
            imu.gx_dps.append(0); imu.gy_dps.append(0)
            imu.gz_dps.append(-30.0 if 10000 <= tm < 13000 else 0.0)
            imu.temp_c.append(25)
        turns = P.detect_turns(imu, g)
        self.assertEqual(len(turns), 1)
        self.assertAlmostEqual(turns[0]["deg"], 90.0, delta=3)

    def test_empty_inputs_write_nothing(self):
        i = self.tmp / "e_imu.csv"
        write_csv(i, IMU_COLS, [])
        self.assertEqual(P.run(None, i, self.tmp / "o"), [])


if __name__ == "__main__":
    unittest.main()
