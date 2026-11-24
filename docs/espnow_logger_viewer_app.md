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
rows 0-1    動作中  3分の送信元 2台  計128件
rows 2-5    チャンネル  [−] 13 [＋]      [SD カード ● / ■] (タップで SD ログの開始 / 停止)
rows 6-7    自分 AA:BB:CC:DD:EE:FF
rows 8-9    相手 AA:BB:CC:DD:EE:FF
rows 10-17  イベント一覧 (新しい順、4 件、選んだ行は枠で囲む)
rows 18-21  選んだイベントの 送信元 / 宛先 (MAC 全体)
rows 22-25  payload の 16 進 (10 バイト × 2 行。20 バイトを超える分は「…」)
rows 26-29  [設定]              [IMU 開始 / 停止]
```

一覧の 1 行は `RX 13 UC 38B -61 00:11:22` (方向、channel、BC/UC、長さ、RSSI、送信元の下位 3 バイト) です。TX は方向のあとに送信結果 (`待ち` / `OK` / `NG`) が入り、RSSI はありません。失敗 (`NG`) の行は危険色です。

- 「3分の送信元」は、最新のフレームから 3 分以内に受けたフレームの送信元 MAC の種類数 (broadcast は数えない)。「計」は service が持っている履歴の件数 (上限 `CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE`)
- SD ログのコントロール (チャンネル行の右、12 × 4 マス = 96 × 32px の 1 つのボタン): SD カードの絵と、赤い丸「●」、スラッシュ「/」、四角「■」を常に並べ、記録を求めているか (`requested`) は枠の色だけで示す (記録中は赤い枠と地、停止中は白い枠)。タップで開始 / 停止を切り替える (`espnow_logger_start_logging()` / `stop_logging()`)。文字は添えない (この画面は文字が多いため)
  - 枠の色は `espnow_logger_log_status_t.requested` による。service が動いていないときは押せない
  - カードの絵は `sd_card_status` の状態で決まる。問題があるとき (`NO_CARD` / `NO_FILESYSTEM` / `FULL` / `FAULT`) は、既存の状態アイコン (`sd_card_status_icon_for_state()`: 斜線のカード、黄の `?`、満タンのメーター、赤の `!`) をそのまま出す。正常 (`OK` / `DISABLED`、関数は `NULL`) のときは、view が持つふつうのカードの絵 (形は同じ 12×14 のカード) を出す。service の `FAILED` では絵を変えない
  - アイコンは `cyd_display_bitmap_t` の寿命の決まり (使い終わるまで生きている) どおり、`sd_card_status_icon.c` の static を渡す。view の model には `sd_problem_icon` (問題がなければ `NULL`) として入る。カードの状態が変わると (`sd_card_status_get_revision()`) 描き直す
  - 枠の色は「記録を求めているか」だけを表し、カードの絵とは独立
  - **グレーアウト**: コントロールが押せるのは、service が動いていて `sd_card_status` が `OK` のときだけ。カードの問題 (`NO_CARD` / `NO_FILESYSTEM` / `FULL` / `FAULT`) のあいだと、SD 非対応のビルド (`DISABLED`。絵はふつうのカード) では、枠・地・●と■を無効の配色 (`CYD_UI_THEME_DISABLED`) にして、タップしても何も起きない (ボタンは当たり判定から外れ、確定処理でも `OK` でなければ無視する)。カードの絵だけは色を変えず、何が問題かを読み取れる
  - すでに求めている (●) まま問題の状態になったとき (起動時の自動開始でカードが無い、記録中の抜き取り) は、赤い枠だけグレーにして残す (●と■はもともと両方出ている)。要求はそのままなので、カードが戻れば続きから始まる (`WAITING_CARD`)。この間は画面から取り消せない。取り消したいときは設定の「起動時に自動で始める」で調整する
  - 右上の SD アイコンは、このコントロールが兼ねるので外した
  - 求めた状態は保存しない。再起動すると「起動時に自動で始める」の値に戻る
- IMU のボタンは右に 1 つだけのトグル (12 × 4 マス)。対象は「直近に UTC 時刻 packet を送ってきた機器」(`espnow_logger_get_time_source()`) で、その機器へ unicast で開始 / 停止を要求する (設定した peer や broadcast は使わない)
  - 時刻 packet を送る機器が無い: 「IMU 開始」をグレーアウト (タップしても何も起きない)
  - 機器があり、IMU を送っていない: 「IMU 開始」(押せる。押すと開始要求)
  - 機器があり、IMU を送信中: 「IMU 停止」(押せる。押すと停止要求)
  - 状態が変わる (機器が黙る、IMU が始まる / 止まる) のは frame の到着では起きないので、viewer は 100ms ごとの step で状態を見比べ、変わったときだけ描き直す
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
| `../espnow_logger_settings_app/` | 設定画面 (起動時に自動で始める) |

view は `cyd_sim` と `test/host/test_espnow_viewer_view.c` が使います。

## Settings

設定画面は 16px のスイッチ行が 1 つです (行全体が 1 つのボタン、「オン」「オフ」の字は出さない)。

- 「起動時に自動で始める」: `espnow_logger_set_auto_start_log_enabled()` (NVS に保存)。下に注釈 (カードが無いときは、入れるまで待つ) を置く

SD ログの開始 / 停止は保存されない一時の操作なので、設定には置かず、viewer のコントロールにしました (旧版は設定に「SDにログを記録」のスイッチがあり、再起動で戻るのが設定らしくないという判断で外した)。

peer MAC と既定 channel の編集は作っていません。channel は viewer で変えられます。peer MAC は文字入力画面 (`cyd_text_input`) が要るため、別の作業にしました。

## Kconfig

旧アプリの 2 つの設定は捨てました。

- `ESPNOW_LOGGER_VIEWER_INPUT_POLL_MS`: 100ms は画面の更新の遅れと CPU の釣り合いで決めた定数で、変える理由が無い
- `ESPNOW_LOGGER_VIEWER_TEST_BROADCAST_PAYLOAD`: 試験の broadcast ボタンが無いので不要

## Simulator

`tools/cyd_sim/scenes/espnow_viewer.c` に、viewer (一覧と記録中の赤枠、ログ停止中の白枠、カードの問題 4 種 (`espnow_sd_no_card` / `_no_filesystem` / `_full` / `_fault`)、長い payload、短い payload、まだ届いていない、service 停止中) と設定 (自動開始のオン・オフ) のシーンがあります。問題のアイコンは `sd_card_status_icon_art.c` (描画に依存しない部分) を取り込み、実機と同じ絵を出します。service は実機専用なので、model を手で組んで描きます。

```bash
tools/cyd_sim/build/cyd_sim --export /tmp/cyd_screens --scale 2
```

## Fonts

画面の文言が増えたため、`scripts/ui_fonts/gen_ui_fonts.py` で UI フォントを作り直しました (`components/support/cyd_ui_fonts/generated/`)。文言を変えたら作り直してください ([CYD UI Fonts](cyd_ui_fonts.md#regenerating))。

## Memory

`s_events` (履歴の写し) が `CONFIG_ESPNOW_LOGGER_EVENT_HISTORY_SIZE` × 約 280 バイト (既定 64 件で約 18KB) の static RAM です。3 分間の送信元数を数えるために全履歴が要ります。RAM が足りなければ、service 側に送信元の数を数える API を足して、写しを 4 件にできます。
