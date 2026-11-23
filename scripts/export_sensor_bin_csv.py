#!/usr/bin/env python3
"""Export espnow_logger sensor.bin records into separate IMU/GPS CSV files.

The current v1 file format stores a stream of records:
  header (20 bytes in v1) + payload + crc32

This tool reads the binary stream, validates CRC32, and writes:
  - <stem>_imu.csv
  - <stem>_gps.csv

It keeps the raw integer fields and also emits a few derived human-readable
columns for convenience.
"""

from __future__ import annotations

import argparse
import binascii
import csv
import json
import struct
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from contextlib import ExitStack
from pathlib import Path
from typing import BinaryIO, Iterable


SENSOR_MAGIC = 0xA55A
SENSOR_VERSION = 1
SENSOR_TYPE_IMU = 1
SENSOR_TYPE_GPS = 2
SENSOR_TYPE_IMU_BATCH = 4

HEADER_PREFIX = struct.Struct("<HBBHHIQ")
CRC32_SIZE = 4
MAX_HEADER_LEN = 1024

IMU_PAYLOAD = struct.Struct("<IhhhhhhhH")
GPS_PAYLOAD = struct.Struct("<IiiiIHHBBHQ")
IMU_BATCH_HEADER = struct.Struct("<IBBHhQ")
IMU_BATCH_SUMMARY = struct.Struct("<hhhhhhhhhhhhhhhhhh")
IMU_BATCH_RAW_SAMPLE = struct.Struct("<hhhhhh")

IMU_CSV_HEADER = [
    "file_offset",
    "record_seq",
    "record_ts_ms",
    "sample_seq",
    "ax_mg",
    "ay_mg",
    "az_mg",
    "gx_mdps",
    "gy_mdps",
    "gz_mdps",
    "temp_cdeg",
    "flags",
]

GPS_CSV_HEADER = [
    "file_offset",
    "record_seq",
    "record_ts_ms",
    "sample_seq",
    "lat_e7",
    "lon_e7",
    "lat_deg",
    "lon_deg",
    "alt_mm",
    "alt_m",
    "speed_mmps",
    "speed_mps",
    "course_cdeg",
    "course_deg",
    "hdop_centi",
    "hdop",
    "sats",
    "fix_type",
    "flags",
    "gps_utc_ms",
    "gps_utc_iso8601",
]

IMU_BATCH_CSV_HEADER = [
    "file_offset",
    "record_seq",
    "record_ts_ms",
    "sequence_start",
    "sample_count",
    "sample_period_ms",
    "flags",
    "temp_cdeg",
    "gps_utc_ms",
    "gps_utc_iso8601",
    "summary_accel_avg_x_mg",
    "summary_accel_avg_y_mg",
    "summary_accel_avg_z_mg",
    "summary_accel_min_x_mg",
    "summary_accel_min_y_mg",
    "summary_accel_min_z_mg",
    "summary_accel_max_x_mg",
    "summary_accel_max_y_mg",
    "summary_accel_max_z_mg",
    "summary_gyro_avg_x_centi_dps",
    "summary_gyro_avg_y_centi_dps",
    "summary_gyro_avg_z_centi_dps",
    "summary_gyro_min_x_centi_dps",
    "summary_gyro_min_y_centi_dps",
    "summary_gyro_min_z_centi_dps",
    "summary_gyro_max_x_centi_dps",
    "summary_gyro_max_y_centi_dps",
    "summary_gyro_max_z_centi_dps",
    "raw_samples_json",
]


@dataclass
class ParseStats:
    imu_records: int = 0
    gps_records: int = 0
    imu_batch_records: int = 0
    skipped_records: int = 0


class SensorBinError(RuntimeError):
    pass


def _iso8601_utc_from_ms(value: int) -> str:
    if value == 0:
        return ""
    dt = datetime.fromtimestamp(value / 1000.0, tz=timezone.utc)
    return dt.isoformat(timespec="milliseconds").replace("+00:00", "Z")


def _read_exact(stream: BinaryIO, size: int) -> bytes | None:
    data = stream.read(size)
    if not data:
        return None
    if len(data) != size:
        raise SensorBinError(f"truncated file while reading {size} bytes")
    return data


def _write_imu_row(writer: csv.writer, file_offset: int, record_seq: int, record_ts_ms: int, payload: bytes) -> None:
    (
        sample_seq,
        ax_mg,
        ay_mg,
        az_mg,
        gx_mdps,
        gy_mdps,
        gz_mdps,
        temp_cdeg,
        flags,
    ) = IMU_PAYLOAD.unpack(payload)
    writer.writerow(
        [
            file_offset,
            record_seq,
            record_ts_ms,
            sample_seq,
            ax_mg,
            ay_mg,
            az_mg,
            gx_mdps,
            gy_mdps,
            gz_mdps,
            temp_cdeg,
            flags,
        ]
    )


def _write_gps_row(writer: csv.writer, file_offset: int, record_seq: int, record_ts_ms: int, payload: bytes) -> None:
    (
        sample_seq,
        lat_e7,
        lon_e7,
        alt_mm,
        speed_mmps,
        course_cdeg,
        hdop_centi,
        sats,
        fix_type,
        flags,
        gps_utc_ms,
    ) = GPS_PAYLOAD.unpack(payload)
    writer.writerow(
        [
            file_offset,
            record_seq,
            record_ts_ms,
            sample_seq,
            lat_e7,
            lon_e7,
            f"{lat_e7 / 1e7:.7f}",
            f"{lon_e7 / 1e7:.7f}",
            alt_mm,
            f"{alt_mm / 1000.0:.3f}",
            speed_mmps,
            f"{speed_mmps / 1000.0:.3f}",
            course_cdeg,
            f"{course_cdeg / 100.0:.2f}",
            hdop_centi,
            f"{hdop_centi / 100.0:.2f}",
            sats,
            fix_type,
            flags,
            gps_utc_ms,
            _iso8601_utc_from_ms(gps_utc_ms),
        ]
    )


def _write_imu_batch_row(writer: csv.writer, file_offset: int, record_seq: int, record_ts_ms: int, payload: bytes) -> None:
    header_size = IMU_BATCH_HEADER.size
    summary_size = IMU_BATCH_SUMMARY.size
    sequence_start, sample_count, sample_period_ms, flags, temp_cdeg, gps_utc_ms = IMU_BATCH_HEADER.unpack(
        payload[:header_size]
    )
    summary_values = IMU_BATCH_SUMMARY.unpack(payload[header_size : header_size + summary_size])
    raw_blob = payload[header_size + summary_size :]
    if sample_count == 0 or sample_count > 10:
        raise SensorBinError(f"invalid IMU batch sample_count: {sample_count}")
    raw_samples = []
    for index in range(sample_count):
        offset = index * IMU_BATCH_RAW_SAMPLE.size
        sample_bytes = raw_blob[offset : offset + IMU_BATCH_RAW_SAMPLE.size]
        ax_mg, ay_mg, az_mg, gx_centi_dps, gy_centi_dps, gz_centi_dps = IMU_BATCH_RAW_SAMPLE.unpack(sample_bytes)
        raw_samples.append(
            {
                "sequence": sequence_start + index,
                "offset_ms": index * sample_period_ms,
                "ax_mg": ax_mg,
                "ay_mg": ay_mg,
                "az_mg": az_mg,
                "gx_centi_dps": gx_centi_dps,
                "gy_centi_dps": gy_centi_dps,
                "gz_centi_dps": gz_centi_dps,
            }
        )

    writer.writerow(
        [
            file_offset,
            record_seq,
            record_ts_ms,
            sequence_start,
            sample_count,
            sample_period_ms,
            flags,
            temp_cdeg,
            gps_utc_ms,
            _iso8601_utc_from_ms(gps_utc_ms),
            *summary_values,
            json.dumps(raw_samples, separators=(",", ":")),
        ]
    )


def convert_sensor_file(input_path: Path, output_dir: Path) -> ParseStats:
    stats = ParseStats()
    output_dir.mkdir(parents=True, exist_ok=True)

    imu_path = output_dir / f"{input_path.stem}_imu.csv"
    gps_path = output_dir / f"{input_path.stem}_gps.csv"
    imu_batch_path = output_dir / f"{input_path.stem}_imu_batch.csv"

    with ExitStack() as stack:
        stream = stack.enter_context(input_path.open("rb"))
        imu_fh = stack.enter_context(imu_path.open("w", encoding="utf-8", newline=""))
        gps_fh = stack.enter_context(gps_path.open("w", encoding="utf-8", newline=""))
        imu_batch_fh = stack.enter_context(imu_batch_path.open("w", encoding="utf-8", newline=""))
        imu_writer = csv.writer(imu_fh)
        gps_writer = csv.writer(gps_fh)
        imu_batch_writer = csv.writer(imu_batch_fh)
        imu_writer.writerow(IMU_CSV_HEADER)
        gps_writer.writerow(GPS_CSV_HEADER)
        imu_batch_writer.writerow(IMU_BATCH_CSV_HEADER)

        while True:
            record_offset = stream.tell()
            prefix = _read_exact(stream, HEADER_PREFIX.size)
            if prefix is None:
                break

            magic, version, record_type, header_len, payload_len, record_seq, record_ts_ms = HEADER_PREFIX.unpack(prefix)
            if magic != SENSOR_MAGIC:
                raise SensorBinError(f"{input_path}: invalid magic at offset {record_offset}: 0x{magic:04x}")
            if version != SENSOR_VERSION:
                raise SensorBinError(f"{input_path}: unsupported version at offset {record_offset}: {version}")
            if header_len < HEADER_PREFIX.size:
                raise SensorBinError(f"{input_path}: invalid header_len at offset {record_offset}: {header_len}")
            if header_len > MAX_HEADER_LEN:
                raise SensorBinError(f"{input_path}: header_len too large at offset {record_offset}: {header_len}")

            extra_header_len = header_len - HEADER_PREFIX.size
            extra_header = _read_exact(stream, extra_header_len) if extra_header_len else b""
            payload = _read_exact(stream, payload_len)
            crc_bytes = _read_exact(stream, CRC32_SIZE)
            if payload is None or crc_bytes is None:
                raise SensorBinError(f"{input_path}: truncated record at offset {record_offset}")

            record_bytes = prefix + extra_header + payload
            expected_crc = struct.unpack("<I", crc_bytes)[0]
            actual_crc = binascii.crc32(record_bytes) & 0xFFFFFFFF
            if actual_crc != expected_crc:
                raise SensorBinError(
                    f"{input_path}: crc mismatch at offset {record_offset}: "
                    f"expected=0x{expected_crc:08x} actual=0x{actual_crc:08x}"
                )

            if record_type == SENSOR_TYPE_IMU:
                if payload_len != IMU_PAYLOAD.size:
                    raise SensorBinError(
                        f"{input_path}: IMU payload_len mismatch at offset {record_offset}: "
                        f"{payload_len} != {IMU_PAYLOAD.size}"
                    )
                _write_imu_row(imu_writer, record_offset, record_seq, record_ts_ms, payload)
                stats.imu_records += 1
            elif record_type == SENSOR_TYPE_GPS:
                if payload_len != GPS_PAYLOAD.size:
                    raise SensorBinError(
                        f"{input_path}: GPS payload_len mismatch at offset {record_offset}: "
                        f"{payload_len} != {GPS_PAYLOAD.size}"
                    )
                _write_gps_row(gps_writer, record_offset, record_seq, record_ts_ms, payload)
                stats.gps_records += 1
            elif record_type == SENSOR_TYPE_IMU_BATCH:
                if payload_len != IMU_BATCH_HEADER.size + IMU_BATCH_SUMMARY.size + IMU_BATCH_RAW_SAMPLE.size * 10:
                    raise SensorBinError(
                        f"{input_path}: IMU batch payload_len mismatch at offset {record_offset}: "
                        f"{payload_len} != {IMU_BATCH_HEADER.size + IMU_BATCH_SUMMARY.size + IMU_BATCH_RAW_SAMPLE.size * 10}"
                    )
                _write_imu_batch_row(imu_batch_writer, record_offset, record_seq, record_ts_ms, payload)
                stats.imu_batch_records += 1
            else:
                stats.skipped_records += 1

    return stats


def _iter_input_paths(paths: Iterable[Path]) -> Iterable[Path]:
    for path in paths:
        if path.is_dir():
            yield from sorted(path.glob("sensor*.bin"))
        else:
            yield path


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Export espnow_logger sensor.bin records into separate IMU and GPS CSV files."
    )
    parser.add_argument(
        "input",
        nargs="+",
        type=Path,
        help="sensor.bin file, directory containing sensor_*.bin, or multiple input files",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=None,
        help="directory for CSV output. Defaults to the input file directory.",
    )
    args = parser.parse_args(argv)

    exit_code = 0
    inputs = list(_iter_input_paths(args.input))
    if not inputs:
        print("no input files found", file=sys.stderr)
        return 1

    for input_path in inputs:
        if not input_path.exists():
            print(f"missing input: {input_path}", file=sys.stderr)
            exit_code = 1
            continue

        output_dir = args.output_dir if args.output_dir is not None else input_path.parent
        try:
            stats = convert_sensor_file(input_path, output_dir)
        except SensorBinError as exc:
            print(str(exc), file=sys.stderr)
            exit_code = 1
            continue

        print(
            f"{input_path}: imu={stats.imu_records} gps={stats.gps_records} imu_batch={stats.imu_batch_records} "
            f"skipped={stats.skipped_records} -> {output_dir / f'{input_path.stem}_imu.csv'} , "
            f"{output_dir / f'{input_path.stem}_gps.csv'} , {output_dir / f'{input_path.stem}_imu_batch.csv'}"
        )

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
