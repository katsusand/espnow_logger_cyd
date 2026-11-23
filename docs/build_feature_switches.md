# Build Feature Switches

## Overview

このプロジェクトでは、バイナリに含める機能を切り替える場合、component の `CMakeLists.txt` では Kconfig の `CONFIG_*` 値を source/dependency selection の根拠にしません。

Wi-Fi STA のように依存 component とバイナリサイズへ大きく影響する機能は、CMake 再構成時に見える build-time switch で切り替えます。

English supplement: Do not use Kconfig symbols for component source or dependency selection. Use an explicit CMake-visible build feature switch instead.

## Why Not Kconfig In Component CMake

ESP-IDF の component CMake は、Kconfig の値が常に source/dependency selection に使える前提で扱うと壊れやすくなります。

実際に `CONFIG_ESP32_WIFI_STA_ENABLED` や `CONFIG_TIME_SYNC_ENABLED` を component の `CMakeLists.txt` 分岐に使ったところ、次のような不整合が発生しました。

- real source が選ばれているのに、依存は stub 側のままになる
- `esp_event.h`、`esp_timer.h`、`esp_netif_sntp.h`、`cyd_text_input.h` などの include path が欠ける
- `build/` を消さない再構成で、source selection と dependency selection の状態が混ざって見える

これは source の不備ではなく、component 登録時に Kconfig 値を feature composition の根拠として使ったことが原因です。

English supplement: Kconfig belongs to runtime/default configuration after the feature is present. Binary composition must be decided by a value that is reliable at component registration time.

## Current Rule

component の `CMakeLists.txt` で実装ファイルや `REQUIRES` を切り替える場合は、次の方針にします。

- `CONFIG_*` を使わない
- CMake 再構成時に明示される環境変数または CMake option を使う
- switch を変えたら必ず `idf.py reconfigure` を実行する
- 同じ API を維持したい場合は、feature OFF 側に stub 実装を用意する
- `build/` は削除せず、再構成で切り替えられる範囲に留める

English supplement: Feature-off stubs preserve app-level dependencies while removing the heavy service implementation and its transitive ESP-IDF dependencies.

## Wi-Fi STA

Wi-Fi STA 機能は `APP_WIFI_STA` 環境変数で切り替えます。

Wi-Fi 有効ビルド:

```bash
source ~/.espressif/tools/activate_idf_v5.5.6.sh || true
python "$IDF_PATH/tools/idf.py" reconfigure
DEV=1 ninja -C build
```

Wi-Fi 無効ビルド:

```bash
source ~/.espressif/tools/activate_idf_v5.5.6.sh || true
APP_WIFI_STA=0 python "$IDF_PATH/tools/idf.py" reconfigure
APP_WIFI_STA=0 DEV=1 ninja -C build
```

`APP_WIFI_STA=0` では、次の component が stub 実装へ切り替わります。

- `esp32_wifi_sta`
- `wifi_profile_store`
- `wifi_connection`
- `radio_manager`
- `time_sync`
- `cyd_wifi_setup`
- `wifi_rssi_history`

値は大文字小文字を区別せず、`0` / `false` / `off` / `no` のいずれかで無効になります。トップの `CMakeLists.txt` と各 component の `CMakeLists.txt` は同じ判定（`string(TOLOWER ...)` のあと `MATCHES "^(0|false|off|no)$"`）を使います。以前は component 側だけ大文字小文字を区別していたため、`APP_WIFI_STA=OFF` ではコードは Wi-Fi 無効としてコンパイルされる一方、実装と依存は Wi-Fi 有効側が選ばれていました。

English supplement: Every place that reads `APP_WIFI_STA` must lower-case it before matching. Component CMakeLists read the environment variable directly because ESP-IDF evaluates them in a separate script-mode pass where the top-level cache variable is not visible.

`APP_WIFI_STA=0` の時、clock composition は Wi-Fi startup 群を起動しません。アプリ側の API 呼び出しは維持できますが、Wi-Fi 接続、NTP 同期、Wi-Fi setup は動作しません。

English supplement: `APP_WIFI_STA=0` is currently exclusive with STA Wi-Fi. ESPNOW has its own switch, `APP_ESPNOW` (see below), and does not assume that ESPNOW and STA can share the same driver lifecycle.

## ESP-NOW Logger

ESP-NOW logger service（`espnow_logger`）は `APP_ESPNOW` 環境変数で切り替えます。書式は `APP_WIFI_STA` と同じです。

ESP-NOW 有効ビルド（既定）:

```bash
source ~/.espressif/tools/activate_idf_v5.5.6.sh || true
APP_WIFI_STA=0 python "$IDF_PATH/tools/idf.py" reconfigure
APP_WIFI_STA=0 DEV=1 ninja -C build
```

ESP-NOW 無効ビルド:

```bash
source ~/.espressif/tools/activate_idf_v5.5.6.sh || true
APP_ESPNOW=0 python "$IDF_PATH/tools/idf.py" reconfigure
APP_ESPNOW=0 DEV=1 ninja -C build
```

`APP_ESPNOW=0` では、`espnow_logger` が stub 実装へ切り替わります。API は同じで、`esp_wifi` / `esp_now` / `sd_card_writer` などへの依存が無くなります。packet の解釈と文字列整形の純 C（`espnow_sensor_packet.c`、`espnow_log_format.c`）は stub でも含まれます。

値は大文字小文字を区別せず、`0` / `false` / `off` / `no` のいずれかで無効になります。トップの `CMakeLists.txt` と component の `CMakeLists.txt` は、`APP_WIFI_STA` と同じ判定を使います。

English supplement: `APP_ESPNOW` and `APP_WIFI_STA` are independent switches, but the ESP-NOW service owns the Wi-Fi driver, so a build that has both on starts neither: `espnow_logger_start()` returns `ESP_ERR_NOT_SUPPORTED` until coexistence is implemented. Use `APP_WIFI_STA=0` to run ESP-NOW.

## Kconfig Role

Kconfig は、機能がバイナリに含まれている場合の挙動や初期値を設定するために使います。

例:

- `CONFIG_ESP32_WIFI_STA_AUTO_START`
- `CONFIG_ESP32_WIFI_STA_SCAN_LIST_SIZE`
- `CONFIG_ESP32_WIFI_STA_MAX_RETRY`
- `CONFIG_TIME_SYNC_INTERVAL_MINUTES`

これらは Wi-Fi feature が有効な build の中で runtime/default behavior を決める設定です。component の source/dependency selection には使いません。

English supplement: Kconfig may still guard code paths inside a compiled source file. The restriction is specifically about choosing component sources and dependencies in component `CMakeLists.txt`.
