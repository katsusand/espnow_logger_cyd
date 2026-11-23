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

`system_settings_fix()` はまだ使っていません。このリポジトリの基盤 (esp32_project の d4d068e 時点) には `system_settings_fix` が無く、esp32_project 側の f865420 に入っています。取り込んだ後で、Wi-Fi 無効ビルドで意味の無い項目を固定するか決めます。ネットワークのページは `APP_WIFI_STA=0` で基盤が自動的に出さないので、残る候補は時刻ページ (時刻同期間隔、タイムゾーン) です。

English supplement: pending the upstream `system_settings_fix` commit; no item is fixed yet.

## Feature Switches

- `APP_WIFI_STA=0`: この製品の既定のビルド。`espnow_logger_start()` が使える。
- `APP_ESPNOW=0`: service が stub になり、`espnow_logger_start()` は失敗して degraded モードで起動する。
