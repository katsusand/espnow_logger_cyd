# Sensor Log Format

## Overview

このドキュメントは、ESP-NOW で受信したセンサーデータを SD カードへ長時間記録するための
`sensor.bin` / `espnow.csv` の保存方針と、`sensor.bin` の binary record format v1 を定義します。
エラーの記録は共通の `error_log_store` が担当します（[Error Log Store](error_log_store.md)）。

想定用途は、M5Stick 系送信機からの IMU 周期送信と GPS イベント送信を、
CYD 側で数時間連続記録することです。

SD カードの mount と状態管理は [SD Card Status](sd_card_status.md)、ファイルへの書き込みは
[SD Card Writer](sd_card_writer.md) の stream が行います。このドキュメントはファイルの中身と名前だけを決めます。

English supplement: This format intentionally separates high-rate sensor recording from low-rate human-readable logs. The binary stream is the primary source for ride analysis. Packet and record layouts are implemented in `espnow_sensor_packet.c`, text and file names in `espnow_log_format.c`; both are pure C and covered by host tests.

## File Roles

記録ファイルは、カードの `logs/` ディレクトリ（無ければ writer が作ります）に次の 2 系統を作ります。

- `logs/sensor_0001.bin`
- `logs/espnow_0001.csv`

役割は次の通りです。

- `sensor_0001.bin`
  - IMU と GPS の primary sensor log
  - binary record の単純連結
  - 時系列順に append する
- `espnow_0001.csv`
  - button を含む IMU / GPS 以外の ESP-NOW packet を残す
  - UTC 時刻ブロードキャストと IMU stream start/stop control packet もここへ含む
  - decode 不能 packet の raw 退避先としても使える
- エラー（decode error を含む）
  - 専用の `error_NNNN.log` は作りません。共通の `error_log_store` が `ERR_<date>_<nn>.LOG`（カードのルート）へ書きます
  - 起動ごとの `boot:` 行、同じエラーの繰り返しの折りたたみ、時刻が確定するまでの保持は、そちらの規則に従います

補足:

- `sensor.bin` には IMU / GPS / IMU batch（type 4 / type 7）/ attitude（type 6）だけを書く
- `espnow.csv` には IMU / GPS / IMU batch / attitude packet を通常は書かない
- 記録中だけ書く。記録していないときの frame は、画面の履歴（RAM）にだけ残る

English supplement: the old per-session `error_NNNN.log` is gone on purpose. `error_log_store` already provides dated files, a boot id line, repeat folding and holding while the clock is unknown, so a second copy would only drift from it. Decode errors are written there with the packet's source MAC.

## Writer Model

SD カードへの書き込みは、`sd_card_writer` の task が一手に行います。この service は stream を 2 本（`sensor`、`espnow`）開き、
ESP-NOW の receive callback から record 1 件ずつを `sd_card_writer_write()` に渡します。待ちません。

- receive callback は stream にコピーして戻る。SD の遅延で Wi-Fi task は止まらない
- バッファが一杯のときは、その record を丸ごと捨てて数える（`sensor.bin` は `seq` の欠けで分かる）
- 1 回の `write()` は 1 record。record が 2 つの file にまたがることはない

stream は「記録を求められている」かつ「`sd_card_status` が `OK`」のあいだだけ開きます。
カードが外れる、満杯になる、書き込みに失敗する、のいずれでも stream は閉じ、カードが戻ると新しい stream（新しい file 番号）で再開します。

English supplement: the performance risk is not the raw data rate but overly frequent synchronized I/O. The writer task and its sector-aligned chunks solve that; the service only keeps its producers non-blocking.

## Sync Policy

両 stream とも、`flush_interval_ms`（`CONFIG_ESPNOW_LOGGER_SD_SYNC_INTERVAL_MS`、既定 2000）ごとに 1 回 sync します。
バッファの大きさは `CONFIG_ESPNOW_LOGGER_SENSOR_BUFFER_KB`（既定 16）と `CONFIG_ESPNOW_LOGGER_CSV_BUFFER_KB`（既定 8）です。

補足:

- 電源断では、最後の sync より後に書いた分（最悪で直近約 2 秒）を失う
- 記録を止める操作（`espnow_logger_stop_logging()`）は、stream を flush して閉じる。状態が `STOPPED` になれば、カードを抜いてよい
- 100Hz の IMU に対して毎回 sync する設計は避ける

English supplement: the old `fflush` / `fsync` trigger bytes are gone; the writer decides when to hand bytes to the card, and the service only chooses the sync interval.

## sensor.bin v1

### Basic Rules

`sensor.bin` v1 は次のルールに従います。

- little-endian
- record の単純連結
- 各 record は `header + payload + crc32`
- record 単位で自己完結
- record type は v1 では `IMU` / `GPS` / `IMU batch` / `IMU batch (summary なし)` / `attitude` を使用する

将来別センサーを追加するときは、新しい `type` を追加します。

## ESP-NOW Sensor Packet v2

受信側が `sensor.bin` へ落とす対象 packet は、ESP-NOW payload 上で次の header を持つものとします。

```c
typedef struct __attribute__((packed)) {
    uint16_t magic;    // 0xA55A
    uint8_t version;   // 2
    uint8_t type;      // 1=IMU, 2=GPS, 3=UTC time, 4=IMU batch, 6=attitude, 7=IMU batch (no summary)
} espnow_sensor_packet_header_v1_t;
```

送信側の仕様表:

| Packet | magic | version | type | total len | payload | 送信条件 | receiver での扱い |
| --- | --- | --- | --- | --- | --- | --- | --- |
| IMU | `0xA55A` | `2` | `1` | `24` bytes | `sample_seq:uint32_le`, `ax/ay/az:int16_le`, `gx/gy/gz:int16_le`, `temp_cdeg:int16_le`, `flags:uint16_le` | IMU 1 sample ごと | `len == 24` を確認して IMU として保存 |
| GPS | `0xA55A` | `2` | `2` | `40` bytes | `sample_seq:uint32_le`, `lat_e7:int32_le`, `lon_e7:int32_le`, `alt_mm:int32_le`, `speed_mmps:uint32_le`, `course_cdeg:uint16_le`, `hdop_centi:uint16_le`, `sats:uint8`, `fix_type:uint8`, `flags:uint16_le`, `gps_utc_ms:uint64_le` | 有効な GPS 状態が取れた更新時 | `len == 40` を確認して GPS として保存 |
| UTC time | `0xA55A` | `2` | `3` | `11` bytes | `utc_year:uint16_le`, `utc_month:uint8`, `utc_day:uint8`, `utc_hour:uint8`, `utc_minute:uint8`, `utc_second:uint8` | `RMC` または `ZDA` で有効時刻を得たとき、1 分間に数回 (多くても毎秒 1 回) | `len == 11` を確認して RTC 補正に使う |
| IMU batch | `0xA55A` | `2` | `4` | `178` bytes | `sequence_start:uint32_le`, `sample_count:uint8`, `sample_period_ms:uint8`, `flags:uint16_le`, `temperature_centi_c:int16_le`, `gps_utc_ms:uint64_le`, `summary:36 bytes`, `raw[10]:120 bytes` | sender の IMU stream 有効中 | `len == 178` を確認して `sensor.bin` へ batch record として lossless 保存 |
| IMU batch (summary なし) | `0xA55A` | `2` | `7` | `142` bytes | `sequence_start:uint32_le`, `sample_count:uint8`, `sample_period_ms:uint8`, `flags:uint16_le`, `temperature_centi_c:int16_le`, `gps_utc_ms:uint64_le`, `raw[10]:120 bytes` | sender の IMU stream 有効中（type 4 は sender の `IMU_SEND_LEGACY_BATCH` が有効なときだけ併送） | `len == 142` と `sample_count` が 1..10 を確認して `sensor.bin` へ lossless 保存 |
| Attitude | `0xA55A` | `2` | `6` | `30` bytes | `sequence:uint32_le`, `gps_utc_ms:uint64_le`, `roll_centi_deg:int16_le`, `pitch_centi_deg:int16_le`, `yaw_rate_centi_dps:int16_le`, `long_accel_mg:int16_le`, `lat_accel_mg:int16_le`, `speed_cmps:uint16_le`, `flags:uint16_le` | sender の IMU stream 有効中、100 ms ごと | `len == 30` を確認して `sensor.bin` へ lossless 保存 |
packet 形状:

- IMU packet: `packet_header + sensor_imu_payload_v1_t`
- GPS packet: `packet_header + sensor_gps_payload_v1_t`
- UTC time packet: `packet_header + sensor_utc_time_payload_v1_t`

補足:

- little-endian
- v2 on-air packet でも payload 長は packet type ごとの固定長で判定する
- `magic` 不一致 packet は non-sensor packet とみなす
- `magic` 一致だが version / type / length が不正な packet は decode error として、共通のエラーログ（`ERR_*.LOG`）と `espnow.csv` に残す。記録中だけ書く
- button などの non-sensor packet は `espnow.csv` 側へ残す
- `type = 5` IMU control packet は sender 制御用であり、`sensor.bin` へは保存しない（長さが 5 bytes でなかったり command が 1 / 2 でなければ decode error）
- `type = 2` GPS、`type = 4` / `type = 7` IMU batch、`type = 6` attitude の `gps_utc_ms != 0` は、受信時に RTC 補正にも使う

English supplement: The on-air packet format is intentionally small. ESP-NOW already provides payload length, so v1 does not duplicate a packet-local payload length field before data enters the binary storage pipeline.

## GPS UTC Time Broadcast

`type = 3` は、GPS から有効な UTC 時刻を得たときに送る時刻ブロードキャストです。

```c
typedef struct __attribute__((packed)) {
    uint16_t utc_year;     // little-endian
    uint8_t utc_month;
    uint8_t utc_day;
    uint8_t utc_hour;
    uint8_t utc_minute;
    uint8_t utc_second;
} sensor_utc_time_payload_v1_t;
```

packet 形状:

- full packet: `packet_header + sensor_utc_time_payload_v1_t`
- total length は `11 bytes`

送信条件:

- `RMC` または `ZDA` で有効時刻を得る
- 1 分間に数回送る。多くても毎秒 1 回まで。何秒に送るかは送信側の都合で決まり、receiver は決まった秒を前提にしない

補足:

- receiver は `len == 11` と `magic / version / type` の確認だけで十分
- `espnow_logger` ではこの packet を受信時刻補正に使い、system clock と UTC anchor を更新する
- raw packet は `espnow.csv` に残してよい

English supplement: The time broadcast is a control packet, not a sensor record. Its job is to refresh RTC state without changing the binary sensor stream format.

## IMU Stream Control Packet

IMU stream start/stop 要求は `sensor.bin` record ではなく、ESP-NOW control packet です。

```c
typedef struct __attribute__((packed)) {
    uint16_t magic;   // 0xA55A
    uint8_t version;  // 2
    uint8_t type;     // 5
    uint8_t command;  // 1=start, 2=stop
} sensor_imu_control_packet_v2_t;
```

補足:

- total length は `5 bytes`
- receiver/logger 側ではこの packet を送信し、sender 側は ESP-NOW metadata の `src_addr` を batch unicast 返送先として使う
- configured peer があれば unicast、未設定なら broadcast 送信でよい
- `sensor.bin` には保存しない
- raw packet は通常の TX/RX event として `espnow.csv` の対象になりうる

## Record Header

```c
typedef struct __attribute__((packed)) {
    uint16_t magic;       // 0xA55A
    uint8_t version;      // 1
    uint8_t type;         // 1=IMU, 2=GPS, 4=IMU batch, 6=attitude, 7=IMU batch (no summary)
    uint16_t header_len;  // sizeof(sensor_record_header_v1_t)
    uint16_t payload_len; // payload bytes
    uint32_t seq;         // sensor.bin record sequence
    uint64_t ts_ms;       // logger monotonic ms
} sensor_record_header_v1_t;
```

各 field の意味:

- `magic`
  - record 再同期用の固定値
- `version`
  - format version
- `type`
  - payload の型
- `header_len`
  - header 拡張時の forward compatibility 用
- `payload_len`
  - payload の byte 数
- `seq`
  - `sensor.bin` 全体で単調増加する record sequence
- `ts_ms`
  - logger 側 monotonic timestamp

補足:

- `ts_ms` は GPS 時刻未取得でも必ず埋める
- `seq` は packet type ごとではなく `sensor.bin` 全体で 1 本にする

English supplement: `ts_ms` gives stable ordering even before GPS time is available. `seq` is file-global so post-processing can detect dropped or duplicated records across mixed sensor types.

## Record Trailer

payload の直後に `crc32` を置きます。

```c
typedef uint32_t sensor_record_crc32_t;
```

`crc32` は `header + payload` に対して計算します。

補足:

- 破損時は次の `magic` を探索して復帰する
- 電源断などで末尾 record が壊れても、前半 record の再生を優先する

## Record Types

v1 で使用する type は次の 5 つです。record type は on-air の `type` と同じ値で、payload は on-air の header 後ろをそのまま保存します。

- `1 = IMU`
- `2 = GPS`
- `4 = IMU batch`
- `6 = attitude`
- `7 = IMU batch (summary なし)`

record version は 1 のままです。type 6 / 7 は古い reader では未知の `type` として `payload_len` で skip されます。

`3` は on-air UTC time packet 用であり、`sensor.bin` record には使いません。

## IMU Payload

IMU payload は固定長です。

```c
typedef struct __attribute__((packed)) {
    uint32_t sample_seq;  // sender-side IMU sequence
    int16_t ax_mg;
    int16_t ay_mg;
    int16_t az_mg;
    int16_t gx_mdps;
    int16_t gy_mdps;
    int16_t gz_mdps;
    int16_t temp_cdeg;    // 0.01 degC
    uint16_t flags;
} sensor_imu_payload_v1_t;
```

単位:

- accel: `mg`
- gyro: `mdps`
- temp: `0.01 degC`

補足:

- `sample_seq` は送信側 IMU stream の連番
- `flags` は v1 では `0` でよい
- 送信側は IMU を基本周期送信する

English supplement: Fixed-size IMU payloads keep the hot path simple and make buffering and offline decoding cheap.

## IMU Batch Payload

IMU batch payload は、sender の `type = 4` packet payload をそのまま lossless に保持します。

```c
typedef struct __attribute__((packed)) {
    uint32_t sequence_start;
    uint8_t sample_count;
    uint8_t sample_period_ms;
    uint16_t flags;
    int16_t temperature_centi_c;
    uint64_t gps_utc_ms;
    imu_batch_summary_v1_t summary;
    imu_raw_sample_v1_t raw[10];
} sensor_imu_batch_payload_v2_t;
```

補足:

- `sensor.bin` では batch を 10 サンプルへ展開せず、1 record として保存する
- 理由は sender packet の `gps_utc_ms` / `summary` / `sample_count` / `sample_period_ms` を欠落なく保持でき、既存 IMU single-sample record も壊さないため
- viewer や offline tool が 10 サンプル展開したい場合は、この batch payload から派生計算する
- `temperature_centi_c` は batch 先頭 sample の温度を表す
- `raw[i]` は accel 3軸 + gyro 3軸 のみを持ち、温度は packet header 側の 1 回だけを使う

English supplement: Storing the batch as a native batch record preserves the sender contract without inventing receiver-side interpolation rules.

## IMU Batch (no summary) Payload

`type = 7` の payload は、`type = 4` から `summary`（36 bytes）を除いたもので、138 bytes です。header の各項目の意味は type 4 と同じです。

```c
typedef struct __attribute__((packed)) {
    uint32_t sequence_start;
    uint8_t sample_count;       // 1..10
    uint8_t sample_period_ms;
    uint16_t flags;
    int16_t temperature_centi_c;
    uint64_t gps_utc_ms;
    imu_raw_sample_v1_t raw[10];
} sensor_imu_batch_nosummary_payload_v2_t;
```

補足:

- sender が type 4 も併送しているとき（`IMU_SEND_LEGACY_BATCH`）、同じ batch が type 4 と type 7 の 2 record で `sensor.bin` に入る。受信側は重複を除かず、受け取ったまま保存する
- 重複は offline tool で除く（`sequence_start` と `gps_utc_ms` が同じ type 4 と type 7 は同じ batch）

## Attitude Payload

`type = 6` の payload は 26 bytes です。値は車体座標系（X 前、Y 左、Z 上）で、`sequence` は対になる batch の `sequence_start`、値はその batch の最後の sample（`sequence + 9`）の時点のものです。

```c
typedef struct __attribute__((packed)) {
    uint32_t sequence;
    uint64_t gps_utc_ms;
    int16_t roll_centi_deg;      // 0.01 deg, 右傾き正
    int16_t pitch_centi_deg;     // 0.01 deg, 機首上げ正
    int16_t yaw_rate_centi_dps;  // 0.01 deg/s, 左旋回正
    int16_t long_accel_mg;       // 前向き正（重力除去済み）
    int16_t lat_accel_mg;        // 左向き正（重力除去済み）
    uint16_t speed_cmps;         // 補正に使った GPS 速度 [cm/s]（GPS 無効なら 0）
    uint16_t flags;
} sensor_attitude_payload_v2_t;
```

`flags` の bit 8-10 は `vehicle_state`（0 不明 / 1 停止 / 2 直進巡航 / 3 直進加速 / 4 直進減速 / 5 コーナリング / 6 遷移）、bit 11-12 は `up_confidence`、bit 13-14 は `forward_confidence`（0 なし / 1 coarse / 2 good）です。その他の bit は sender の `ESPNOW_PROTOCOL.md` を参照してください。

English supplement: Types 4 and 7 are in the board frame; type 6 is in the vehicle frame.

## GPS Payload

GPS payload は毎回フル状態を持つ固定長 record とします。

```c
typedef struct __attribute__((packed)) {
    uint32_t sample_seq;   // sender-side GPS sequence
    int32_t lat_e7;        // degrees * 1e7
    int32_t lon_e7;        // degrees * 1e7
    int32_t alt_mm;        // millimeters
    uint32_t speed_mmps;   // mm/s
    uint16_t course_cdeg;  // 0.01 degree
    uint16_t hdop_centi;   // hdop * 100
    uint8_t sats;          // satellites used
    uint8_t fix_type;      // 0=none, 1=2D, 2=3D
    uint16_t flags;
    uint64_t gps_utc_ms;   // unix epoch ms, unknown=0
} sensor_gps_payload_v1_t;
```

単位:

- latitude / longitude: `degrees * 1e7`
- altitude: `mm`
- speed: `mm/s`
- course: `0.01 degree`
- hdop: `hdop * 100`

補足:

- `gps_utc_ms=0` は UTC 未解決を意味する
- `gps_utc_ms!=0` を受けた logger は、その時刻で RTC 補正してよい
- GPS は差分表現ではなくフル状態を保持する
- 送信側は `大きく動いた時 + keepalive` を基本にする

English supplement: Full-state GPS records are preferred over delta-only encoding so each record remains independently meaningful even when packets or log segments are missing.

## espnow.csv

`espnow.csv` は IMU / GPS 以外の ESP-NOW 通信だけを記録します。
button packet, control packet, decode error の raw 退避先として使います。

推奨 header:

```csv
utc_iso8601,ts_ms,src_mac,len,payload_hex
```

列の意味:

- `utc_iso8601`
  - GPS 時刻が解決できていれば `YYYY-MM-DDTHH:MM:SS.mmmZ`
  - GPS 未解決でも、logger system clock が build timestamp で seed 済みならその UTC 推定値を出してよい
  - どちらも無い場合は空欄
- `ts_ms`
  - logger monotonic ms
- `src_mac`
  - 送信元 MAC
- `len`
  - payload 長
- `payload_hex`
  - raw payload の hex dump

補足:

- 必要なら将来 `dir` や `dst_mac` を追加してよい
- IMU / GPS 以外 only という前提を保つ

## Rotation

各 log file は連番で、大きさで切り替えます。

- `logs/sensor_0001.bin`、`logs/sensor_0002.bin`
- `logs/espnow_0001.csv`

規則:

- 記録を始めたあと、最初に作る file は、カード上の全 file（`sensor_*.bin` と `espnow_*.csv`）の最大番号 + 1 を共通の番号にする。同じ回の `sensor_0007.bin` と `espnow_0007.csv` は対になる
- 大きさの上限（`sensor.bin` は `CONFIG_ESPNOW_LOGGER_SENSOR_MAX_FILE_KB`、既定 32768 KB。`espnow.csv` は `CONFIG_ESPNOW_LOGGER_CSV_MAX_FILE_KB`、既定 1024 KB）を超える前に、record の境界で次の file へ切り替える。そのときの番号は、その時点のカード上の最大番号 + 1 で、もう一方の file とは揃わない
- `espnow.csv` は、どの file も先頭が header 行
- 欠番は再利用しない。既存の file には追記も上書きもしない
- カードを抜き差しして記録が再開したときも、新しい番号の file から始める。`sensor.bin` の record の `seq` は、記録を始めるたびに 1 から数え直す（file の切り替えでは続く）
- 上限は、1 record 分だけ超えることがある

English supplement: Monotonic file numbering is intentional. A missing number is diagnostic information and should not be backfilled. Old per-line CSV rotation (5000 lines) was replaced by the writer's size-based rotation.

## Compatibility

v1 reader は次を前提にします。

- `magic == 0xA55A`
- `version == 1`
- `header_len == sizeof(sensor_record_header_v1_t)`
- `type in {1, 2, 4, 6, 7}`

未知の `type` に遭遇した場合でも、
`payload_len` を使って record を skip できる設計を維持します。

English supplement: Unknown future types should be skippable without breaking old tooling, provided the reader trusts `payload_len` only after validating the record length and CRC.

## Offline Export

`sensor.bin` を PC 側で CSV に分けたい場合は、`scripts/export_sensor_bin_csv.py` を使えます。

例:

```bash
python3 scripts/export_sensor_bin_csv.py /path/to/sensor_0001.bin
```

出力:

- `sensor_0001_imu.csv`
- `sensor_0001_gps.csv`
- `sensor_0001_imu_batch.csv`（type 4 と type 7）
- `sensor_0001_attitude.csv`（type 6）

補足:

- CSV には record header の `seq` / `ts_ms` と、各 payload の生データを残します
- GPS 側には `lat_deg` などの派生列も含めます
- `_imu_batch.csv` は最後の列 `record_type`（`4` または `7`）で由来を示します。type 7 は summary を持たないので `summary_*` 列は空です。sender が両方を送っていると、同じ batch が 2 行（type 4 と type 7）入ります
- `_attitude.csv` の列: `file_offset, record_seq, record_ts_ms, sequence, gps_utc_ms, gps_utc_iso8601`、生の値 `roll_centi_deg, pitch_centi_deg, yaw_rate_centi_dps, long_accel_mg, lat_accel_mg, speed_cmps, flags`、派生の `roll_deg, pitch_deg, yaw_rate_dps, long_accel_g, lat_accel_g, speed_mps`、`flags` から取り出した `vehicle_state, up_confidence, forward_confidence`

English supplement: This is an offline convenience tool. The binary file remains the source of truth, and the CSV split is derived data for analysis.

### Plot (GPS map / IMU dashboard)

`scripts/plot_sensor_csv.py` は、上の CSV から HTML を 2 つ作ります。標準ライブラリのみで動き、追加の pip 依存はありません。

```bash
python3 scripts/plot_sensor_csv.py sensor_0001_gps.csv sensor_0001_imu.csv --out /path/to/outdir
```

- GPS の CSV だけ、IMU の CSV だけでも動きます(ファイル名の `_gps.csv` / `_imu.csv` で判別)
- `<name>_gps_track.html`: Leaflet + OpenStreetMap の地図。軌跡、各点(ツールチップに時刻・速度・高度・sats・hdop)、START / GOAL
- `<name>_imu_dashboard.html`: Plotly の 5 段グラフ。加速度 3 軸 [g]、加速度の大きさ、角速度 3 軸 [deg/s]、角速度の大きさ、温度 [℃] と GPS 速度 [km/h](右軸)。横軸は IMU ログ開始からの秒(IMU が無ければ最初の GPS 点から)
- GPS と IMU を一緒に渡すと、地図に曲がり区間のマーカーを出します(橙=右折、紫=左折、ツールチップに角度・時刻・速度)。ジャイロの垂直軸成分を積分して 40° 以上の曲がりを拾う簡易判定で、水平に固定した車載(ダッシュボード等)を想定しています。零点と重力方向は GPS 速度がほぼ 0 の区間から求めます。バイクのようにロールする場合はそのままでは使えません。しきい値は `scripts/plot_sensor_csv.py` の `TURN_*` 定数です
- 単位変換は mg→g、mdps→deg/s、cdeg→℃、`lat_e7`/`lon_e7`→度、mm/s→km/h
- GPS で軌跡から除く行: `fix_type == 0`、緯度経度が (0, 0)、範囲外(±90 / ±180)、数値に変換できない行。除いた数は標準出力に出ます。`fix_type` は 0 以外なら残します(実ログには 1 と 6 があり、上の `0/1/2` の定義とは一致しないため)
- IMU は数値に変換できない行だけ除きます
- `_imu_batch.csv`(実機ログの IMU はこちらに入ります)も IMU 入力として渡せます(`_imu.csv` とは同時に指定できません)。`raw_samples_json` を展開し、時刻は `record_ts_ms + offset_ms`、角速度は centi_dps→deg/s、温度はバッチ値です。`record_type` 列があるときは、同じ `(sequence_start, gps_utc_ms)` の type 7 がある type 4 の行を重複として除きます(`record_type` 列が無い古い CSV はそのまま全行を使います)。サンプルが多い(約 100 万点)ので `--step N`(既定 10)で N サンプルずつ平均して間引きます。`--step 1` で間引きなしですが HTML が非常に大きくなります。間引くと短いピークは鈍ります
- HTML は CDN(Leaflet / Plotly / OSM タイル)を読み込むため、開くにはネットワークが必要です
- 注意: 地図 HTML には実際の位置情報が入ります。`.gitignore` で `*.csv` と出力 HTML を除外していますが、出力先はリポジトリの外にし、公開しないでください

テスト: `python3 -m unittest discover -s test/py`

English supplement: The generated HTML embeds raw GPS positions and loads Leaflet/Plotly from a CDN; keep outputs out of the repository.
