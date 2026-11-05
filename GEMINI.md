# Dr.Dongly-Z (Seeed Studio XIAO ESP32S3 Dedicated Edition) プロジェクト憲章 & 永続ガイドライン (GEMINI.md)

このドキュメントは、Google Antigravity 2.0 エージェントが本リポジトリ（`Drdonguri-XIAO`）で作業する際に最優先で遵守すべき「絶対ルール」および「システムアーキテクチャ仕様」です。新しいセッションを開始した際も、常にこの指針を最優先として自律的に行動してください。

---

## 1. プロジェクト基本情報

- **プロダクト名**: DrVoice どんぐり-Z 専用ファームウェア (`Dr.Dongly-Z XIAO Edition`)
- **対象ハードウェア**: 秋月電子通商 `[118078]` Seeed Studio XIAO ESP32S3 (ESP32-S3R8)
  - URL: [https://akizukidenshi.com/catalog/g/g118078/](https://akizukidenshi.com/catalog/g/g118078/)
- **マイコン仕様**: Espressif ESP32-S3R8（Xtensa® 32-bit LX7 デュアルコア 240MHz）
  - 内蔵Flash: **8MB**（Quad SPI）
  - 内蔵PSRAM: **8MB**（Octal SPI）
  - ネイティブUSB OTG（GPIO 19: D-, GPIO 20: D+）
  - 外付けアンテナ＋Type-Cケーブル接続状態で**熱収縮チューブにより完全密閉・被覆**
  - カメラなし、オンボードマイクなし、MicroSDスロットなし（Sense版ではない素のXIAO）
- **公開リポジトリ**: [https://github.com/yokawase/Drdonguri-XIAO](https://github.com/yokawase/Drdonguri-XIAO)
- **現在のバージョン**: **`v25.0-XIAO`**

---

## 2. 絶対遵守ルール（忘却厳禁・サバイバル規則）

### ① 既存リポジトリの完全保護（非破壊ルール）
- 過去のAtomS3UおよびT-dongle-S3向け開発リポジトリ（`/home/medart-code/Drdonguri`）には**一切の変更・コミット・プッシュを加えてはならない**。
- 本リポジトリ（`/home/medart-code/Drdonguri-XIAO`）は完全に独立したリポジトリとして保守・開発を行う。

### ② 【最重要】物理ボタン不使用の100%全自動ファームウェア書込（サバイバルルール）
- 本機は熱収縮チューブで完全密閉されているため、基板上の極小タクトスイッチ（Boot/Reset）を物理的に押すことは不可能である。
- **ユーザーに対して「ボタンを押してください」「Bootボタンを押しながらリセットしてください」という指示を出すことを永久に禁止する。**
- **ゼロボタン自動書込アーキテクチャの死守**:
  1. `platformio.ini` に `-D ARDUINO_USB_MODE=1` および `-D ARDUINO_USB_CDC_ON_BOOT=1` を常時設定し、DTR/RTS信号を常時受信可能とする。
  2. 書込時は `scripts/post_upload_watchdog_reset.py` を経由し、稼働中のTinyUSB CDC (303a:8025) に対して1200bpsタッチを実行 ➔ ファームウェアが `usb_persist_restart(RESTART_BOOTLOADER)` を呼び出し、自動でROMブートローダー (303a:1001) へ遷移する。
  3. ホスト側で `303a:1001` の再列挙を確認後、`esptool v5.4.0` を呼び出し `--after watchdog-reset` で書き込み、完了後にハードウェアウォッチドッグ経由で通常ファームウェアへ100%確実に復帰させる。
- **ブートループ文鎮化防止ガード**:
  - `setup()` 冒頭に必ず `delay(1000);` の起動猶予を配置し、PCとのUSBネゴシエーション確立前にパニッククラッシュしてポートを見失う事故を恒久的に防ぐ。

### ③ 32GB MicroSDから「8MB PSRAM / 8MB Flash」への完全凝縮（物理接点ゼロ・高信頼性）
- MicroSDスロット等の物理可動部・接触不良・振動脱落リスクを完全に排除し、医療現場での超高耐久・物理故障ゼロを達成する。
- T-dongle-S3の32GB SDカード資産を、以下の固定長バイナリ（`#pragma pack(1)`）へ凝縮してFlash（`binary_db` パーティション 2.875MB）に格納し、起動時に8MB PSRAM（`MALLOC_CAP_SPIRAM`）へ全展開する：
  1. **Minds診療ガイドライン CQレコード (`minds.bin`)**: 256B固定長（ICD-10, 推奨度, エビデンス, 128d INT8ベクトル, 要約文）。ソート済み二分探索 $O(\log N)$（所要時間 11μs以下）。
  2. **医薬品・安全禁忌マスターレコード (`drugs.bin`)**: 64B固定長（HOT9コード, 制御フラグ, 一般名, 16B禁忌ブルームフィルタ）。所要時間 1〜8μs。
  3. **Aho-Corasick Trie木 (`trie.bin`)**: 16B固定長フラットノード（11,159ノード）。多パターン文字列同時走査 $O(N)$（所要時間 54μs）。
  4. **JIS全漢字 音訓読み辞書 (`kanji_yomi.bin`)**: 12B固定長（JIS第1・第2水準 6,528文字）。

### ④ Windows電子カルテ専用 v20.1 HYBRIDキーストローク最適化（仕様理解の徹底）
- 送出されるキーストロークは、すべて**日本の病院で標準採用されているWindows電子カルテ（MS-IME / ATOK）の変換辞書・順位特性に100%特化**してチューニングされている。
  - `[Z]`: 自立語・漢字熟語（Space 1回 ➔ Enter確定）
  - `[K]`: 全角カタカナ（KEY_F7 ➔ Enter確定）
  - `[H]`: 付属語・助詞・句読点（即時 Enter確定・Space禁止）
  - `[A]`: 半角英数・記号（ASCII直接 / KEY_F10確定）
  - `[G]`: 同音異義語第2候補（Space 2回 ➔ Enter確定、例: 「主訴」「健診」等）
- **Linux開発環境（Mint / Mozc）での注意事項**:
  - Linux Mint（Mozc）では辞書の候補順位がWindows MS-IMEと異なるため、`[G]` により「主訴」が第2候補の「首座」になったり、「分3」が「文3」に変換される現象が発生するが、これは**Windows MS-IME向けに最適化された正常な仕様動作**である。
  - Linux X11デスクトップの中クリック貼り付け誤爆（Primary SelectionによるPDFデータ等の混入）に留意すること（Windows電カル端末では一切発生しない）。

---

## 3. 主要ファイル構成

| ファイル / パス | 役割・技術スタック |
| :--- | :--- |
| [`platformio.ini`](file:///home/medart-code/Drdonguri-XIAO/platformio.ini) | XIAO ESP32S3 専用設定 (`seeed_xiao_esp32s3`, 8MB Flash, 8MB Octal PSRAM, USB CDC on Boot) |
| [`partitions_8mb.csv`](file:///home/medart-code/Drdonguri-XIAO/partitions_8mb.csv) | 8MB Flash 最適化パーティション（`app0`/`app1` 各2.5MB, `binary_db` LittleFS 2.875MB） |
| [`include/xiao_binary_db.h`](file:///home/medart-code/Drdonguri-XIAO/include/xiao_binary_db.h) | 8MB PSRAM常駐型 医療バイナリDBエンジン（オンメモリ二分探索 ＆ Aho-Corasick） |
| [`scripts/compile_xiao_binary_db.py`](file:///home/medart-code/Drdonguri-XIAO/scripts/compile_xiao_binary_db.py) | 医療マスターから固定長バイナリ（`minds.bin`, `drugs.bin`, `trie.bin`）を生成するコンパイラ |
| [`include/typing_engine.h`](file:///home/medart-code/Drdonguri-XIAO/include/typing_engine.h) | JIS(106/109)/US(101/104)動的切替、安全分解打鍵、11ms安全インターキーディレイ制御 |
| [`src/typing_engine.cpp`](file:///home/medart-code/Drdonguri-XIAO/src/typing_engine.cpp) | v20.1 HYBRIDストリーム (`[Z][K][H][A][G][SAFE]`) パーサー ＆ HIDレポート送出エンジン |
| [`include/usb_descriptors.h`](file:///home/medart-code/Drdonguri-XIAO/include/usb_descriptors.h) / [`src/usb_descriptors.c`](file:///home/medart-code/Drdonguri-XIAO/src/usb_descriptors.c) | TinyUSB Composite（CDC + HID Keyboard + 仮想プリンタ Class 07h、MSC完全遮断） |
| [`scripts/post_upload_watchdog_reset.py`](file:///home/medart-code/Drdonguri-XIAO/scripts/post_upload_watchdog_reset.py) | **ゼロボタン自動書込スクリプト**（1200bpsタッチ ➔ ROMブートローダー遷移 ➔ esptool v5.4.0 リセット） |
| [`src/main.cpp`](file:///home/medart-code/Drdonguri-XIAO/src/main.cpp) | 起動猶予セーフティ、TinyUSB CDC/HID、BLE NUS (Nordic UART) / どんぐり互換UUID、オンチップ診断 |
| [`scripts/test_pwa_ble_integration.py`](file:///home/medart-code/Drdonguri-XIAO/scripts/test_pwa_ble_integration.py) | PWA Web Bluetooth プロトコル完全準拠・実機自動テストハーネス |

---

## 4. 臨床カルテ音声入力・医療安全検証プロトコル

本デバイスは、手元のスマホ音声入力（PWA）から送られてくるカルテテキストをリアルタイムに処理する。テスト時は以下の代表的医療テキストおよび音声認識シナリオを用いて検証を行うこと：

### 代表検証テキスト
1. **急病・発熱外来**:
   `主訴: 38.5度の発熱と強い咽頭痛。`
   `処方: ロキソプロフェンNa錠60mg 3錠 分3 毎食後 3日分。`
2. **予防接種・公衆衛生シナリオ**:
   `インフルエンザ文京区体温36度2分過去にワクチンで以上なし完了症状なし可能`
3. **安全スキャン発火語**:
   - 危険語: 「休薬」「減量」「漸減」「禁忌」
   - 重点監視薬品: 「ロキソプロフェン」「ワーファリン」「メトトレキサート」

---

## 5. ファームウェア改修・実機自動書込ワークフロー

ファームウェア（`src/` または `include/`）を変更した際は、以下の手順を自律的に実行して実機検証まで完遂すること：

1. **実機接続確認**: `lsusb | grep 303a` および `/dev/ttyACM*` の存在を確認。
2. **全自動ビルド＆書き込み**:
   ```bash
   cd /home/medart-code/Drdonguri-XIAO
   pio run -e seeed_xiao_esp32s3 -t upload
   ```
   - `scripts/post_upload_watchdog_reset.py` が自動で作動し、ボタンに触れることなく書き込みとWatchdogリセットが完了する。
3. **実機診断テストの実行**:
   ```bash
   ~/.platformio/penv/bin/python scripts/test_pwa_ble_integration.py
   ```
4. **Gitコミット＆リモートプッシュ**:
   - 変更内容をコミットし、`origin main`（`yokawase/Drdonguri-XIAO`）へプッシュする。
