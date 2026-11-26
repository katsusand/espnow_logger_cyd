# espnow_logger

## Overview

`components/services/espnow_logger/` は、ESP-NOW の送受信と、受信したセンサーデータの SD 記録を担う service です。

- ESP-NOW 専用の Wi-Fi / ESP-NOW 初期化を 1 か所に集約する
- 送受信 frame の履歴（RAM のリングバッファ）を保持し、画面などから読めるようにする
- 受信したセンサー packet を `sensor.bin`、それ以外の frame を `espnow.csv` へ記録する

SD カードそのもの（mount、状態、抜き差し）は扱いません。それは [SD Card Status](sd_card_status.md) の役割です。この service は、カードが `OK` のときだけ、[SD Card Writer](sd_card_writer.md) の stream を開いて書きます。

English supplement: the service owns the radio lifecycle but never touches the card driver. It reads the card state from `sd_card_status` and writes only through `sd_card_writer` streams, so a missing card is a state, not an error.

## Source Layout

| file | 内容 | ESP-IDF 依存 |
| --- | --- | --- |
| `espnow_logger.c` | 無線、peer、送受信、履歴、NVS、ログ stream の開閉 | あり |
| `espnow_sensor_packet.c` | センサー packet v2 の解釈、`sensor.bin` record の生成、CRC | なし（純 C） |
| `espnow_log_format.c` | `espnow.csv` の行、MAC / hex / UTC の文字列、log file 名 | なし（純 C） |
| `espnow_logger_stub.c` | `APP_ESPNOW=0` のときの同じ API | 最小 |

純 C の 2 つは `test/host/` でそのまま試験します（`bash test/host/run.sh`）。形式は [Sensor Log Format](sensor_log_format.md) が定めます。

## Build Switch

`APP_ESPNOW` で本体と stub を切り替えます。値の扱いは [Build Feature Switches](build_feature_switches.md) のとおりです。

```bash
APP_WIFI_STA=0 ./scripts/idf-env.sh reconfigure   # ESP-NOW を含む（既定）
APP_ESPNOW=0 ./scripts/idf-env.sh reconfigure     # stub
```

この service は Wi-Fi driver を自分で初期化して持つので、いまは STA の Wi-Fi 機能と同時に動かせません。`APP_WIFI_STA` が有効なビルドでは、`espnow_logger_start()` が `ESP_ERR_NOT_SUPPORTED` を返して何も起動しません。ESP-NOW を使うには `APP_WIFI_STA=0` を指定してください。

English supplement: coexistence with STA Wi-Fi is not implemented yet; the guard keeps two owners from fighting over the driver.

## Responsibilities

この service は次を担当します。

- `esp_netif_init()` と default event loop の準備
- `esp_wifi_init()`、`WIFI_MODE_STA`、保存された channel の設定
- `esp_now_init()` と callback の登録
- broadcast peer と、設定された unicast peer の登録
- 起動時送信と periodic 送信（Kconfig で有効にしたとき）
- RX / TX frame のシリアルログと、履歴の保持
- IMU stream 開始 / 停止要求の送信
- sender が運んだ UTC で system clock を合わせる（GPS / IMU batch / attitude の `gps_utc_ms`、UTC time packet）
- SD ログの開始 / 停止と、カードの状態に合わせた stream の開閉

担当しないもの: 通常の AP 接続、DHCP/IP、NTP、Wi-Fi setup UI、画面描画、カードの mount / format / unmount。

## Public API

```c
#include "espnow_logger.h"
```

### Radio

| 関数 | 内容 |
| --- | --- |
| `espnow_logger_start()` | Wi-Fi と ESP-NOW を起動する。保存済みの channel と peer を使う。自動開始の設定が有効なら、記録も求める |
| `espnow_logger_is_started()` | 起動済みか |
| `espnow_logger_get_channel()` / `set_channel()` | 現在の channel。`set_channel()` は反映して NVS に保存する |
| `espnow_logger_get_local_mac()` | 自分の MAC |
| `espnow_logger_has_configured_peer()` / `get_configured_peer_mac()` | 保存された peer。無ければ `ESP_ERR_NOT_FOUND` |
| `espnow_logger_set_configured_peer_mac()` | peer を保存して登録する。`NULL` で消す |
| `espnow_logger_send_broadcast()` / `send_to_configured_peer()` / `send_to_mac()` | 送信 |
| `espnow_logger_send_imu_stream_start_request()` / `stop_request()` / `request_to_mac()` | IMU stream の開始 / 停止要求（`type=5`） |

IMU stream 要求は、peer があればその peer へ unicast、無ければ broadcast します (`send_imu_stream_start_request()` / `stop_request()`)。viewer は代わりに、次の「時刻 packet の送信元」へ `request_to_mac()` で unicast します。

| API | 内容 |
| --- | --- |
| `espnow_logger_get_time_source(out)` | 直近 (`ESPNOW_LOGGER_TIME_SOURCE_WINDOW_MS` = 30 秒、sender は 1 分間に数回送る) に UTC time packet (`type=3`) を送ってきた機器の MAC と、その機器が IMU を送信中か (`imu_streaming`: 同じ MAC の IMU / IMU batch (type 4 / 7) / attitude を `ESPNOW_LOGGER_IMU_STREAM_WINDOW_MS` = 2 秒以内に受けた)。該当する機器が無ければ `false` |

English supplement: the time source is the sender of the newest UTC time packet; a different sender replaces it and clears its streaming mark. Updated on the Wi-Fi task under the service lock; readable from any task.

### Event History

| 関数 | 内容 |
| --- | --- |
| `espnow_logger_get_event_sequence()` | 履歴が変わるたびに増える番号。画面が描き直すかを決める |
| `espnow_logger_get_event_count()` | 保持している件数 |
| `espnow_logger_copy_recent_events()` | 新しい順に snapshot を取る |
| `espnow_logger_clear_events()` | 履歴を空にする |

履歴は snapshot で渡すので、画面側は Wi-Fi callback の context を気にしなくてかまいません。

### SD Log

| 関数 | 内容 |
| --- | --- |
| `espnow_logger_start_logging()` | 記録を求める。すぐ戻る |
| `espnow_logger_stop_logging()` | 記録をやめる。非同期で、stream を flush して閉じる |
| `espnow_logger_get_log_status()` | 状態と、stream ごとの書き込み量・取りこぼし |
| `espnow_logger_get_auto_start_log_enabled()` / `set_...()` | 起動時から記録を求めるか。NVS に保存する |

状態（`espnow_logger_log_state_t`）:

| 状態 | 意味 |
| --- | --- |
| `STOPPED` | 記録を求められていない。カードを抜いてよい |
| `WAITING_CARD` | 記録は求められているが、カードが `OK` でない。`OK` になれば自動で始まる |
| `ACTIVE` | stream が開いていて、書いている |
| `FAILED` | カードは `OK` なのに、stream を開けない、または書き込みに失敗した。一定時間後（`CONFIG_ESPNOW_LOGGER_LOG_RETRY_MS`）にもう一度開く |
| `STOPPING` | 停止を求められ、flush している最中 |

カードの状態（未挿入、未フォーマット、満杯）そのものは `sd_card_status_get_state()` で読みます。この service の状態は「記録を求められているか」と「stream がどうか」だけを答えます。

English supplement: `start_logging()` and `stop_logging()` never block on the card; a small control task (it never touches the card either) opens and closes the streams. Poll `espnow_logger_get_log_status()` for `STOPPED` before telling the user the card can be pulled.

## SD Logging

記録の構成は次のとおりです。

```text
Wi-Fi task (RX callback)
   ├─ センサー packet ──► sensor stream ──┐
   └─ それ以外の frame ──► espnow stream ──┤  sd_card_writer の task が card へ書く
                                           ▼
                              logs/sensor_NNNN.bin, logs/espnow_NNNN.csv
```

- RX callback は record を stream へコピーするだけで、カードを待ちません。バッファが一杯なら、その record を捨てて数えます（`get_log_status()` の `records_dropped`）。`sensor.bin` では record の `seq` が欠けるので、後から取りこぼしが分かります
- stream は「記録を求められている」かつ「`sd_card_status` が `OK`」のあいだだけ開きます。カードが外れる（`NO_CARD`）、満杯になる（`FULL`）、書き込みに失敗する（`FAULT`）と閉じ、`OK` に戻ると新しい stream で再開します。カードが戻ると新しい番号の file になります
- ファイル名、番号、大きさによる切り替え、record の形式は [Sensor Log Format](sensor_log_format.md) に従います
- エラー（packet の decode error、送信の失敗など）は、共通の `ERROR_LOG()` で `error_log_store` へ書きます。この service 専用の error log file は持ちません

記録の対象:

- 形式が正しいセンサー packet v2（`type=1` IMU、`type=2` GPS、`type=4` / `type=7` IMU batch、`type=6` attitude）は `sensor.bin` へ
- それ以外のすべての frame（TX も含む。button、UTC time、IMU control、decode 不能なもの）は `espnow.csv` へ
- `gps_utc_ms != 0` の GPS / IMU batch / attitude と、UTC time packet は、system clock を合わせるのにも使う

English supplement: with logging on, one RX frame ends in exactly one place: a sensor record, or a CSV row. A frame that fails to decode is a CSV row plus an error log line.

### Card Pulled Or Full

| カード | この service |
| --- | --- |
| 抜かれた | 次の確認（既定 0.5 秒）までに stream を閉じる。状態は `WAITING_CARD`。戻ると再開 |
| 満杯（`FULL`） | stream を閉じて書くのをやめる。カードはマウントされたままなので PC で読める |
| 書き込みに失敗 | stream を閉じ、`FAILED` にして、一定時間後にもう一度開く。理由は writer がエラーログへ書く |

English supplement: `sd_card_status` closes the card without telling the other stream owners, so the service notices by polling the state and revision every `CONFIG_ESPNOW_LOGGER_LOG_POLL_MS`. A write in that gap fails inside the writer, and the stream is closed on the next poll.

## NVS

namespace は `ftr_espnow`（`ftr_` は、アプリを入れ替えても残る設定。[NVS Storage](nvs_storage.md) 参照）です。

| key | 型 | 内容 | 既定値 |
| --- | --- | --- | --- |
| `channel` | u8 | ESP-NOW の channel（1〜14） | `CONFIG_ESPNOW_LOGGER_WIFI_CHANNEL` |
| `peer_mac` | blob 6 bytes | unicast と IMU control の宛先 | `CONFIG_ESPNOW_LOGGER_UNICAST_PEER_MAC`（空なら無し） |
| `auto_log` | u8 | 起動時から記録を求める | 0 |

不正な値は `nvs_health` に報告して既定値を使います。旧アプリにあった「起動時に SD を mount してエラーログを取る」設定は、`sd_card_status` が常に行うので無くなりました。

## Kconfig

`ESP-NOW Logger` メニューの主な項目です。

- 無線: `ESPNOW_LOGGER_WIFI_CHANNEL`、`ESPNOW_LOGGER_UNICAST_PEER_MAC`（どちらも保存値が無いときの既定）
- 送信: `ESPNOW_LOGGER_SEND_STARTUP_FRAMES`、`ESPNOW_LOGGER_PERIODIC_TX_INTERVAL_MS`（0 で無効）、`ESPNOW_LOGGER_BROADCAST_PAYLOAD`、`ESPNOW_LOGGER_UNICAST_PAYLOAD`
- 履歴: `ESPNOW_LOGGER_EVENT_HISTORY_SIZE`
- SD ログ: `ESPNOW_LOGGER_SENSOR_BUFFER_KB`、`ESPNOW_LOGGER_CSV_BUFFER_KB`、`ESPNOW_LOGGER_SENSOR_MAX_FILE_KB`、`ESPNOW_LOGGER_CSV_MAX_FILE_KB`、`ESPNOW_LOGGER_SD_SYNC_INTERVAL_MS`、`ESPNOW_LOGGER_LOG_POLL_MS`、`ESPNOW_LOGGER_LOG_RETRY_MS`

バッファの大きさは [SD Card Writer](sd_card_writer.md) の Buffer Sizing に従って決めます。`ESPNOW_LOGGER_ENABLED` は、本体を含むビルドの中での実行時の有効 / 無効で、含めるかどうかは `APP_ESPNOW` が決めます。

## Logging Format

シリアルログは 1 frame ごとに `status`、`ts_ms`、`ch`、`src`、`dst`、`rssi`、`len`、`payload`（hex）を出します。`ts_ms` は起動からの経過ミリ秒で、wall clock ではありません。TX 完了の callback は RSSI を持たないので、信号強度は受信側のログを見てください。

`espnow.csv` の `utc_iso8601` は、system clock が合っていれば、その時刻を基にした UTC です。後で GPS UTC か UTC time packet が来ると、その値で合わせ直します。

## Concurrency

- RX / TX の callback は Wi-Fi task で動く。stream への書き込みは待たない
- stream の開閉は、service 専用の小さな task だけが行う。閉じる前に入口を閉じ、書き込み中の callback が出るのを待つ
- `sd_card_writer` の task が、file 番号を決める directory の走査と、書き込みをする。callback の task は走査しない

## Known Limits

- STA Wi-Fi と同時には動かない（`APP_WIFI_STA=0` が必要）
- `sd_card_status` はカードを解放するとき、`error_log_store` 以外の stream 所有者へ知らせない。この service は定期的な確認で補うため、カードが抜かれてから stream が閉じるまで最長で確認 1 回分の遅れがある
- Wi-Fi task の stack に載る buffer が大きい（CSV 1 行ぶん約 0.8 KB、serial log 用の hex 約 0.75 KB）。実機で stack の余裕を確認すること
