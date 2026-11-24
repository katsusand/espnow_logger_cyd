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

    def test_empty_inputs_write_nothing(self):
        i = self.tmp / "e_imu.csv"
        write_csv(i, IMU_COLS, [])
        self.assertEqual(P.run(None, i, self.tmp / "o"), [])


if __name__ == "__main__":
    unittest.main()
