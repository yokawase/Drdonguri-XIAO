# Dr.Dongly-Z (Seeed Studio XIAO ESP32S3 Dedicated Edition)

**素の Seeed Studio XIAO ESP32S3（秋月電子 `[118078]`：8MB Flash / 8MB OPI PSRAM / 熱収縮チューブ完全密閉仕様）** 専用の医療DXエッジコプロセッサ・ファームウェアリポジトリです。

---

## 1. ハードウェア＆サバイバル仕様
- **マイコン**: Espressif ESP32-S3R8 (Dual Core 240MHz, 8MB Quad Flash + 8MB Octal PSRAM)
- **物理接点ゼロ・完全密閉**: カメラなし・マイクなし・SDスロットなし・物理ボタン操作不要
- **100%ボタンレス全自動書き込み保証**:
  - `USBCDC USBSerial(0)` + `HID Keyboard` + `USB Printer (07h)` の3インターフェース複合構成
  - `pio run -e seeed_xiao_esp32s3 -t upload` 実行時、`scripts/post_upload_watchdog_reset.py` が稼働中の TinyUSB CDC に対して自動で 1200bps タッチを発行し、ROM ブートローダー（`303a:1001`）へ遷移させ、書き込み後に Watchdog ハードリセットで通常ファームウェア（`303a:8025`）へ復帰します。
  - `setup()` 冒頭に1秒のUSBネゴシエーション待機＋2秒のセーフティガードタイマーを配置し、万が一のブートループ・文鎮化を100%防止しています。

---

## 2. メモリマップ & 2.1MB 固定長バイナリ・オンメモリPSRAM展開 (`partitions_8mb.csv`)
- `app0` (`0x10000`, 2.5MB) / `app1` (`0x290000`, 2.5MB): OTA冗長構成
- `binary_db` (`0x510000`, 2.875MB, LittleFS):
  1. **`minds.bin` (256B固定長)**: Minds診療ガイドラインCQ・推奨度・エビデンスレベル・ICD-10・128次元INT8量子化ベクトル・要約文
  2. **`drugs.bin` (64B固定長)**: HOT9コード・薬品ID・安全制御フラグ（安全分解 / F10バイパス / F7カタカナ / ハイリスク警告）・一般名・禁忌ブルームフィルタ
  3. **`trie.bin` (16B固定長)**: UTF-8 Aho-Corasick フラットTrie木（危険熟語「休薬」「減量」「漸減」や重要薬品名を数マイクロ秒で一括検知）
  4. **`kanji_yomi.bin` (12B固定長)**: JIS全漢字 6,527字の音訓代表読みテーブル

起動時に全バイナリを `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` で **8MB PSRAM** 上へ一括ロードし、Flash I/O ゼロ（検索時間 1〜11μs）で動作します。
また、内部SRAM（320KB）消費は **17.1%（55,912 bytes）** に抑えられています。

---

## 3. ビルド・書き込みコマンド

```bash
# 1. 固定長バイナリDBの再生成
python3 scripts/compile_xiao_binary_db.py

# 2. LittleFS (binary_db) パーティションの書き込み（ボタン操作不要）
~/.platformio/penv/bin/pio run -e seeed_xiao_esp32s3 -t uploadfs

# 3. ファームウェア本体のビルド＆書き込み（ボタン操作不要）
~/.platformio/penv/bin/pio run -e seeed_xiao_esp32s3 -t upload
```
