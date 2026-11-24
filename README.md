# ESP-NOW Logger for CYD

ESP32-2432S028R 系の CYD (Cheap Yellow Display) で ESP-NOW のフレームを受信し、一覧表示して SD カードへ記録する ESP-IDF プロジェクトです。M5Stick 系送信機の IMU / GPS センサーデータを、数時間連続で記録する用途を想定しています。

English supplement: ESP-NOW receiver/logger firmware for CYD boards. Derived from [esp32_project](https://github.com/katsusand/esp32_project) (remote `upstream`); the clock components stay in the tree but are not registered.

## Features

- ESP-NOW の送受信フレームを画面に一覧表示し、選択したフレームの送信元・宛先・payload を表示
- 受信したセンサー packet を `sensor.bin` (binary)、それ以外を `espnow.csv` へ SD カードに記録
- 画面から ESP-NOW channel の変更、ログ記録の開始/停止、IMU 送信の開始/停止要求
  - IMU ボタンの対象は、直近に UTC 時刻 packet を送ってきた機器。要求後は相手の状態が変わるまでグレーアウト
- 受信した UTC 時刻 packet で時計を合わせる
- SD カードの状態 (未挿入・異常・容量) をアイコンで表示。カードが無くても動作
- `sensor.bin` を CSV に変換し、GPS 軌跡と IMU グラフの HTML にする補助スクリプト

## Wi-Fi について

ESP-NOW は現状 Wi-Fi STA 機能と共存できないため、`APP_WIFI_STA` 未指定なら Wi-Fi 無効でビルドします。`APP_WIFI_STA=1` の Wi-Fi 有効ビルドでは ESP-NOW は起動しません。変更したら `idf.py reconfigure` が必要です ([Build Feature Switches](docs/build_feature_switches.md))。

## Hardware

- ESP32-2432S028R 系 CYD (ESP32, flash 4 MB)
- 320x240 ST7789 (clone によっては `menuconfig` の `CYD Display` で ILI9341 / 色順 / 反転を調整)
- XPT2046 タッチ、SD カードスロット
- 送信機: ESP-NOW で sensor packet を送る M5Stick 系 (IMU / GPS)

## Build and Flash

ESP-IDF v5.5.6 (EIM 管理) を使います。v5.5.5 より前には戻さないでください。`idf.py` は PATH に無いので、環境を読み込むラッパーを使います。

```bash
./scripts/idf-build.sh
./scripts/idf-flash-monitor.sh /dev/cu.usbserial-XXXX
```

`./scripts/idf-menuconfig.sh` で設定を開き、`./scripts/idf-env.sh <idf.py の引数>` で任意のコマンドを実行できます。monitor の終了は `Ctrl-]` です。初回は LovyanGFX の submodule が必要です。

```bash
git submodule update --init --recursive
```

## Test and Simulator

```bash
bash test/host/run.sh        # ホストテスト (実機不要)
```

`tools/cyd_sim` は SDL で画面を実機なしで確認するシミュレータです ([CYD Simulator](docs/cyd_sim.md))。

## Log Analysis

SD カードの `logs/` にある `sensor.bin` を CSV にして、グラフ化できます。形式は [Sensor Log Format](docs/sensor_log_format.md) を参照してください。

```bash
python3 scripts/export_sensor_bin_csv.py --output-dir out <sensor.bin>
python3 scripts/plot_sensor_csv.py --out out out/*_gps.csv out/*_imu.csv
```

生成する HTML には GPS 位置が入るため、コミットや公開をしないでください。

## Source Layout

| path | 内容 |
| --- | --- |
| `components/products/espnow_logger_composition/` | 製品の組み立て (起動順、app / service の接続) |
| `components/apps/espnow_logger_viewer_app/` | 一覧・操作画面 |
| `components/services/espnow_logger/` | ESP-NOW 送受信、履歴、SD 記録 |
| `components/services/` `platform/` `framework/` | SD、時刻、表示、入力、app_shell などの土台 |
| `docs/` | 各コンポーネントの設計 |
| `third_party/` | LovyanGFX (submodule、編集しない) |

土台 (framework / platform / shared services) の修正は先に esp32_project で行い、`upstream/main` をここへ merge します。

## Documentation

- [ESP-NOW Logger](docs/espnow_logger.md) / [Viewer App](docs/espnow_logger_viewer_app.md) / [Composition](docs/espnow_logger_composition.md)
- [Sensor Log Format](docs/sensor_log_format.md) / [Motorcycle Use Notes](docs/motorcycle_use_notes.md)
- [SD Card Status](docs/sd_card_status.md) / [SD Card Writer](docs/sd_card_writer.md)
- [Build Feature Switches](docs/build_feature_switches.md)
- 土台の各コンポーネントは `docs/` を参照

## License

自作コードとドキュメントは Apache License 2.0 です ([LICENSE](LICENSE))。`third_party/` の LovyanGFX などはそれぞれの upstream ライセンスに従います ([License](docs/license.md))。
