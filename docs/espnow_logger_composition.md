# ESP-NOW Logger Composition

## Overview

`espnow_logger_composition` は ESP-NOW logger 製品の composition root です。`main/main.c` はこの `espnow_logger_composition_start()` だけを呼びます。起動順と接続関係を決めるだけで、画面の挙動や service の内部は持ちません。時計 (`cyd_clock_*`) はツリーに残っていますが、この製品では登録しません。

English supplement: product composition root, same role as `cyd_clock_composition`. It wires; it does not implement.

## Startup Flow

```text
main
  -> espnow_logger_composition_start
       -> system_boot_start            (NVS / CYD platform / touch calibration; fatal on failure)
       -> sd_card_status_start         (optional; its own task mounts the card)
       -> error_log_store_notify_time_decided(UNAVAILABLE)
       -> seed clock from build time   (only if the clock is before 2024-01-01)
       -> app_shell idle return = 0    (disabled)
       -> espnow_logger_start          (optional; degraded mode on failure)
       -> system_settings_fix(IDLE_RETURN, HIDDEN)
       -> settings_pin_init            (optional)
       -> NVS health check             (invalid NVS: open Initialize NVS confirm)
       -> boot touch shortcut          (opens settings)
       -> espnow_logger_viewer_app_register  (viewer + log settings)
       -> app_shell_start(viewer)
```

- `time_tick_start` と `app_scheduler_init` は呼びません。使うのは時計の表示とアラームだけです。
- この製品には Wi-Fi の時刻同期が無いので、エラーログへ `UNAVAILABLE` を知らせます (時刻の答えを待たせない)。時計は build 時刻で種付けし、受信した UTC パケットで service が上書きします。
- idle return は 0 (無効) です。ログ画面が勝手にホームへ戻る先は無く、ホームは viewer 自身です。home return の条件 callback (`app_shell_set_home_return_allowed_callback`) は、Wi-Fi setup 中の抑止のためのものなので、この製品では要りません。
- `APP_BUILD_UNIX_TIME` はトップレベル `CMakeLists.txt` が `add_compile_definitions` で渡します。

## Registered Apps

| App | 登録 |
|---|---|
| `espnow_logger_viewer_app` (ホーム) | `espnow_logger_viewer_app_register()` |
| `espnow_logger_settings_app` | viewer の `settings_app` として一緒に登録される |

システムの設定 / 情報 / タッチ補正 (`cyd_system_apps`) は基盤が用意します。

## Degraded Mode

任意の service が失敗しても、ブートループにはしません。失敗は `error_log_store` へ記録し、起動を続けます。

- SD カードなし/故障: viewer は動く。ログは `WAITING_CARD`、カードが OK になれば自動で始まる。
- `espnow_logger_start()` 失敗 (Wi-Fi 有効ビルドの `ESP_ERR_NOT_SUPPORTED` を含む): viewer は空の一覧で起動し、設定は開ける。ログ設定の記録ボタンは止まった状態になる。
- `settings_pin_init()` 失敗: PIN が読めない記録は NVS health が拾い、Initialize NVS へ回る。

English contract: only `system_boot_start()` and `app_shell_start()` are fatal.

## Boot Shortcut

画面を押したまま電源を入れると settings が初期 app になります (タッチ補正は settings から)。旧アプリの「押したまま起動でタッチ補正」に相当します。NVS が不正なら Initialize NVS の確認が優先されます。

## Fixed Settings

`system_settings_fix()` は composition からだけ、`app_shell_start()` の前に呼びます。値を決めるのは `app_shell_set_idle_return_timeout_seconds(0)` の行で、`system_settings_fix()` は設定画面で変えられないようにするだけです。

| 項目 | 固定 | 理由 |
|---|---|---|
| `IDLE_RETURN` 無操作で戻る | HIDDEN | 起動のたびに 0 (しない) へ上書きするので、画面で変えても次の起動で戻る。「しない」は現場で見る価値のある値でもないので READONLY にせず隠す。一般ページの他の項目は残るので、隙間は行の空きだけ |

固定しなかった項目と理由:

- ネットワーク1/2 (`STORED_SSIDS`、`WIFI_SETUP`、`SYNC_INTERVAL`、`WIFI_IDLE`、`SYNC_NOW`): `APP_WIFI_STA=0` では基盤がページごと出さないので固定は不要。Wi-Fi 有効ビルドは service が起動しない構成なので対象外。
- `TIMEZONE`: 時刻ページの表示用で、ログは UTC。現場で現地時刻を見る価値があるので残す。
- `BRIGHTNESS` / `THEME`: 製品の都合で縛る理由が無い。
- `TOUCH_CALIBRATE` / `CLEAR_TOUCH_CALIB`: 起動時の長押しショートカットの行き先であり、必要。
- `CLEAR_APP_DATA`: ログの設定 (チャンネル、ピア、自動開始) を消せる、意味のある操作。
- `CLEAR_NVS`: 回復の手段として残す。

注意: 時刻ページの「時計は設定済みです」は 2024 年以降かで判定します。build 時刻の種付けでも「設定済み」と出るため、UTC パケットを受けて合ったことの表示ではありません。

English supplement: only the idle-return item is fixed; the rest are deliberately left open and the reasons are above.

## Feature Switches

- `APP_WIFI_STA=0`: この製品の既定のビルド。`espnow_logger_start()` が使える。
- `APP_ESPNOW=0`: service が stub になり、`espnow_logger_start()` は失敗して degraded モードで起動する。
