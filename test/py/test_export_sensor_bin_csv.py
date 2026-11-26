"""Tests for scripts/export_sensor_bin_csv.py. Run: python3 -m unittest discover -s test/py"""
import binascii
import csv
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import export_sensor_bin_csv as E  # noqa: E402

GPS_UTC_MS = 1709208000000


def record(rtype, payload, seq, ts_ms):
    head = struct.pack("<HBBHHIQ", 0xA55A, 1, rtype, 20, len(payload), seq, ts_ms)
    body = head + payload
    return body + struct.pack("<I", binascii.crc32(body) & 0xFFFFFFFF)


def raw_blob(n=10):
    return b"".join(struct.pack("<hhhhhh", 100 * i, -i, 1000, 250, 0, -100) for i in range(n))


def batch4(seq_start):
    head = struct.pack("<IBBHhQ", seq_start, 10, 10, 1, 3700, GPS_UTC_MS)
    summary = struct.pack("<18h", *range(18))
    return head + summary + raw_blob()


def batch7(seq_start):
    return struct.pack("<IBBHhQ", seq_start, 10, 10, 1, 3700, GPS_UTC_MS) + raw_blob()


def attitude(seq, flags):
    return struct.pack("<IQhhhhhHH", seq, GPS_UTC_MS, -1234, 567, -89, 321, -654, 1500, flags)


def read_csv(path):
    with open(path, newline="") as fh:
        return list(csv.DictReader(fh))


class ExportTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())

    def write_bin(self, records):
        p = self.tmp / "sensor_t.bin"
        p.write_bytes(b"".join(records))
        return p

    def test_types_4_6_7(self):
        flags = (2 << 8) | (1 << 11) | (2 << 13) | 0x8000
        p = self.write_bin([
            record(4, batch4(100), 0, 1000),
            record(7, batch7(100), 1, 1001),
            record(6, attitude(100, flags), 2, 1002),
            record(99, b"\x01\x02", 3, 1003),  # unknown type is skipped
        ])
        stats = E.convert_sensor_file(p, self.tmp)
        self.assertEqual((stats.imu_batch_records, stats.imu_batch_nosummary_records, stats.attitude_records,
                          stats.skipped_records), (1, 1, 1, 1))

        rows = read_csv(self.tmp / "sensor_t_imu_batch.csv")
        self.assertEqual([r["record_type"] for r in rows], ["4", "7"])
        self.assertEqual(list(rows[0].keys())[-1], "record_type")
        self.assertEqual(rows[0]["summary_accel_avg_x_mg"], "0")
        self.assertEqual(rows[1]["summary_accel_avg_x_mg"], "")
        self.assertEqual(rows[1]["summary_gyro_max_z_centi_dps"], "")
        for r in rows:
            self.assertEqual((r["sequence_start"], r["sample_count"], r["gps_utc_ms"]), ("100", "10", str(GPS_UTC_MS)))
        # both types decode the same raw samples
        self.assertEqual(json.loads(rows[0]["raw_samples_json"]), json.loads(rows[1]["raw_samples_json"]))
        self.assertEqual(json.loads(rows[1]["raw_samples_json"])[9]["ax_mg"], 900)

        att = read_csv(self.tmp / "sensor_t_attitude.csv")
        self.assertEqual(len(att), 1)
        a = att[0]
        self.assertEqual((a["sequence"], a["roll_centi_deg"], a["lat_accel_mg"], a["speed_cmps"]),
                         ("100", "-1234", "-654", "1500"))
        self.assertEqual((a["roll_deg"], a["pitch_deg"], a["yaw_rate_dps"]), ("-12.34", "5.67", "-0.89"))
        self.assertEqual((a["long_accel_g"], a["lat_accel_g"], a["speed_mps"]), ("0.321", "-0.654", "15.00"))
        self.assertEqual((a["vehicle_state"], a["up_confidence"], a["forward_confidence"]), ("2", "1", "2"))
        self.assertTrue(a["gps_utc_iso8601"].startswith("2024-02-29T12:00:00"))

    def test_payload_length_checked_per_type(self):
        # a type 7 record that carries the type 4 length is rejected, and vice versa
        p = self.write_bin([record(7, batch4(1), 0, 1)])
        with self.assertRaises(E.SensorBinError):
            E.convert_sensor_file(p, self.tmp)
        p = self.write_bin([record(4, batch7(1), 0, 1)])
        with self.assertRaises(E.SensorBinError):
            E.convert_sensor_file(p, self.tmp)
        p = self.write_bin([record(6, attitude(1, 0) + b"\x00", 0, 1)])
        with self.assertRaises(E.SensorBinError):
            E.convert_sensor_file(p, self.tmp)


if __name__ == "__main__":
    unittest.main()
