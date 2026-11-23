# ESP-NOW Logger Viewer App

## Overview

`components/apps/espnow_logger_viewer_app` は、`espnow_logger` service が受けた・送った ESP-NOW のフレームを CYD の画面に並べるアプリです。ログを SD に書くのは service で、この画面は状態を見せて、channel と IMU stream の要求だけを操作します。

あわせて `components/apps/espnow_logger_settings_app` (ログの設定画面) があります。こちらは viewer の `settings_app` として登録するので、viewer を載せなければ入りません。

English supplement: the viewer is a client of the service. It never mounts the card and never opens a log file; it reads `espnow_logger_get_log_status()` and `sd_card_status`.

## Public API

```c
#include "espnow_logger_viewer_app.h"

const app_shell_app_t *espnow_logger_viewer_app_get_app(void);
esp_err_t espnow_logger_viewer_app_register(void);
```

`espnow_logger_viewer_app_register()` は `cyd_clock_app_register()` と同じ作法で、`app_registry` に `espnow_logger` (表示名「ESP-NOW」) を登録します。`settings_app` にはログの設定画面を付けます。`entry` は `static` で、借用されます。呼ぶのは composition の起動中、`app_shell` が走り出す前です。service の `espnow_logger_start()` も composition が呼びます。viewer は呼びません。

旧アプリの `espnow_logger_viewer_app_get()` は `..._get_app()` に改名しました (他のアプリに合わせた)。

## Screen

文字は 16px だけ、色は `CYD_UI_THEME_*` です。

```text
rows 0-1    動作中  3分の送信元 2台  計128件           [SD アイコン]
rows 2-5    チャンネル  [−] 13 [＋]      SDログ / 記録中
rows 6-7    自分 AA:BB:CC:DD:EE:FF
rows 8-9    相手 AA:BB:CC:DD:EE:FF
rows 10-17  イベント一覧 (新しい順、4 件、選んだ行は枠で囲む)
rows 18-21  選んだイベントの 送信元 / 宛先 (MAC 全体)
rows 22-25  payload の 16 進 (10 バイト × 2 行。20 バイトを超える分は「…」)
rows 26-29  [設定] [IMU 開始] [IMU 停止]
```

一覧の 1 行は `RX 13 UC 38B -61 00:11:22` (方向、channel、BC/UC、長さ、RSSI、送信元の下位 3 バイト) です。TX は方向のあとに送信結果 (`待ち` / `OK` / `NG`) が入り、RSSI はありません。失敗 (`NG`) の行は危険色です。

- 「3分の送信元」は、最新のフレームから 3 分以内に受けたフレームの送信元 MAC の種類数 (broadcast は数えない)。「計」は service が持っている履歴の件数 (上限 `CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE`)
- SD ログの表示は service の状態 (`記録中` / `カード待ち` / `失敗` / `停止中…` / `しない`)。カードそのものに問題があれば、時計と同じ SD アイコンが右上に出る (`sd_card_status_icon_for_state()`)。カードの状態が変わると描き直す
- [IMU 開始] / [IMU 停止] は service が動いているときだけ押せる。peer が無いと service が broadcast で送る
- [設定] は `system_settings_app_get_app()` へ移る。設定側は `app_shell_return_to(from_app)` で戻る。PIN の保護は enter guard が掛けるので、viewer は何もしない

## Input

`cyd_input` / `cyd_display` / `cyd_ui` には、押して離して確定する仕組みが共通部品としては無く、時計・時計の設定・システム設定がそれぞれ同じ形の tracker を持っています。viewer も同じ形の tracker を持ちます (旧アプリの処理と同じ: 押したボタンの上で離したときだけ、長押しは無効)。ボタンの当たり判定は `cyd_display_screen_hit_test(cyd_ui_screen_of(&owner), ...)` です。

一覧の選択は、ボタンに当たらない所を離したときに、y 座標から行を求めます (`espnow_logger_viewer_view_event_at()`)。

English contract: all drawing and input happen in the shell task. The screen is rebuilt whole each time (shared screen buffer), when the event sequence or the card state changes or a button changed something. There is no timer redraw.

## Files

| ファイル | 内容 |
|---|---|
| `espnow_logger_viewer_app.c` | service 呼び出し、入力、model の組み立て |
| `espnow_logger_viewer_view.c/.h` | model だけから画面を組む。service のヘッダーを include しない |
| `../espnow_logger_settings_app/` | 設定画面 (SD にログを記録する / 起動時に自動で始める) |

view は `cyd_sim` と `test/host/test_espnow_viewer_view.c` が使います。

## Settings

設定画面は 16px のスイッチ行 (行全体が 1 つのボタン、「オン」「オフ」の字は出さない) が 2 つです。

- 「SDにログを記録」: `espnow_logger_start_logging()` / `stop_logging()` (今の要求の反対を求める)。service が動いていないと押せない
- 「起動時に自動で始める」: `espnow_logger_set_auto_start_log_enabled()` (NVS に保存)

peer MAC と既定 channel の編集は作っていません。channel は viewer で変えられます。peer MAC は文字入力画面 (`cyd_text_input`) が要るため、別の作業にしました。

## Kconfig

旧アプリの 2 つの設定は捨てました。

- `ESPNOW_LOGGER_VIEWER_INPUT_POLL_MS`: 100ms は画面の更新の遅れと CPU の釣り合いで決めた定数で、変える理由が無い
- `ESPNOW_LOGGER_VIEWER_TEST_BROADCAST_PAYLOAD`: 試験の broadcast ボタンが無いので不要

## Simulator

`tools/cyd_sim/scenes/espnow_viewer.c` に、viewer (一覧、長い payload とカード待ち、短い payload と記録失敗、まだ届いていない、停止中) と設定 (オン、サービス停止中) のシーンがあります。service は実機専用なので、model を手で組んで描きます。

```bash
tools/cyd_sim/build/cyd_sim --export /tmp/cyd_screens --scale 2
```

## Fonts

画面の文言が増えたため、`scripts/ui_fonts/gen_ui_fonts.py` で UI フォントを作り直しました (`components/support/cyd_ui_fonts/generated/`)。文言を変えたら作り直してください ([CYD UI Fonts](cyd_ui_fonts.md#regenerating))。

## Memory

`s_events` (履歴の写し) が `CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE` × 約 280 バイト (既定 64 件で約 18KB) の static RAM です。3 分間の送信元数を数えるために全履歴が要ります。RAM が足りなければ、service 側に送信元の数を数える API を足して、写しを 4 件にできます。
