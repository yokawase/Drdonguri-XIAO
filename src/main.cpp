#include <Arduino.h>
#include <USB.h>
#include <USBCDC.h>
#include <USBHIDKeyboard.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "FS.h"
#include "LittleFS.h"
#include "freertos/queue.h"
#include "esp32-hal-tinyusb.h"
#include "device/usbd_pvt.h"
#include "esp_system.h"
#include "soc/rtc_cntl_reg.h"

#include "usb_descriptors.h"
#include "typing_engine.h"
#include "xiao_binary_db.h"

// ============================================================================
// Dr.Dongly-Z (Seeed Studio XIAO ESP32S3 Dedicated Edition)
// Firmware v25.0-XIAO (8MB Flash + 8MB OPI PSRAM / Zero-Button Survival Edition)
// ============================================================================

extern "C" bool tud_mounted(void);
extern "C" bool tud_hid_n_ready(uint8_t instance);

// Seeed Studio XIAO ESP32S3 オンボード黄色ユーザーLED (GPIO 21, Active LOW)
#define XIAO_LED_PIN        21
#define XIAO_BOOT_BTN_PIN   0

// BLE UUID (DrVoice どんぐり君 PWA 互換 + Nordic UART Service 互換)
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// Nordic UART Service (NUS) 互換UUID
#define NUS_SERVICE_UUID    "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_CHAR_UUID    "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_CHAR_UUID    "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// プロトコル定数
#define MAX_PAYLOAD_SIZE    4096
#define MAX_PACKETS         24
#define MAX_PACKET_LEN      256
#define BLE_CHUNK_SIZE      (8 + MAX_PACKET_LEN)
#define BLE_QUEUE_SIZE      48
#define RX_TIMEOUT_MS       3000
#define MAX_RETRY_COUNT     3

enum SystemState {
  STATE_USB_NOT_READY,
  STATE_WAITING_BLE,
  STATE_BLE_CONNECTED,
  STATE_RECEIVING,
  STATE_READY_TO_TYPE,
  STATE_TYPING,
  STATE_ERROR
};

#pragma pack(push, 1)
struct SessionInitHeader {
  uint16_t sessionId;
  uint8_t  totalPackets;
  uint16_t totalBytes;
  uint16_t totalCrc16;
  uint8_t  reserved;
};

struct SlotHeader {
  uint16_t sessionId;
  uint8_t  seqNo;
  uint8_t  mode;
  uint16_t payloadLen;
  uint16_t chunkCrc16;
};

struct KanjiYomiHeader {
  char     magic[4];       // "YOMI"
  uint16_t count;          // レコード件数 (LE)
  uint16_t recordSize;     // 12バイト
  char     reserved[4];
};

struct KanjiYomiRecord {
  char     utf8[4];
  char     romaji[8];
};
#pragma pack(pop)

struct BleQueueItem {
  uint16_t length;
  uint8_t  data[BLE_CHUNK_SIZE];
};

struct PacketSlot {
  bool     received;
  uint16_t len;
  uint8_t  payload[MAX_PACKET_LEN];
};

struct MessageContext {
  uint16_t   sessionId;
  uint8_t    totalPackets;
  uint8_t    receivedCount;
  uint16_t   expectedTotalBytes;
  uint16_t   expectedTotalCrc;
  uint8_t    mode;
  size_t     actualTotalBytes;
  uint32_t   lastActivityTime;
  uint8_t    retryAttempts;
  PacketSlot slots[MAX_PACKETS];
  char       assembledBuffer[MAX_PAYLOAD_SIZE + 1];
};

struct PrinterChunkItem {
  uint16_t length;
  uint8_t  data[64];
};
#define PRINTER_QUEUE_SIZE 128

// ============================================================================
// グローバルインスタンス（大きな構造体はPSRAMへ配置）
// ============================================================================
// ★最重要：TinyUSB CDC (interface 0/1) を有効化し、PCからのDTR/RTSまたは1200bps信号で
// 物理ボタンに一切触れずに100%確実にROMブートローダーへ自動遷移させる
USBCDC USBSerial(0);
USBHIDKeyboard Keyboard;
DonguriTypingEngine g_typingEngine(Keyboard);
XiaoPsramDatabase   g_psramDb;

static BLEServer*         pBleServer = nullptr;
static BLECharacteristic* pTxRxCharacteristic = nullptr;
static BLECharacteristic* pNusTxCharacteristic = nullptr;
static QueueHandle_t      bleQueue = nullptr;
static QueueHandle_t      s_printer_queue = nullptr;

static MessageContext*    g_msgCtx = nullptr; // 8MB PSRAM上に確保
static KanjiYomiRecord*   g_psramKanjiYomi = nullptr;
static uint16_t           g_psramKanjiYomiCount = 0;

static SystemState        currentState = STATE_WAITING_BLE;
static bool               isBleConnected = false;
static bool               lastUsbState = false;
static volatile bool      g_dfuRequested = false;
static volatile bool      s_printer_job_active = false;
static uint32_t           s_last_printer_rx_ms = 0;
static uint32_t           s_printer_total_bytes = 0;

// ============================================================================
// XIAO オンボードLED制御 (GPIO 21: LOW=点灯, HIGH=消灯)
// ============================================================================
void setLedColor(uint8_t r, uint8_t g, uint8_t b, bool forceRefresh = false) {
  (void)forceRefresh;
  // XIAO ESP32S3のオンボードLED (GPIO 21) は単色（Active LOW）
  bool turnOn = (r > 0 || g > 0 || b > 0);
  digitalWrite(XIAO_LED_PIN, turnOn ? LOW : HIGH);
}

// ============================================================================
// ソフトウェアからROMダウンロードモード（ブートローダー）へ突入
// ============================================================================
void rebootToBootloader() {
  digitalWrite(XIAO_LED_PIN, LOW);
  delay(100);
  usb_persist_restart(RESTART_BOOTLOADER);
  REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
  esp_restart();
}

// ============================================================================
// USB仮想プリンタ受信コールバック
// ============================================================================
void onPrinterDataReceived(const uint8_t* data, uint16_t len) {
  if (len == 0 || data == nullptr) return;

  if (len >= 7 && (strncmp((const char*)data, "CMD:DFU", 7) == 0 ||
                   strncmp((const char*)data, "CMD:REBOOT_BOOTLOADER", 21) == 0)) {
    g_dfuRequested = true;
    return;
  }

  s_last_printer_rx_ms = millis();
  s_printer_job_active = true;
  s_printer_total_bytes += len;
  setLedColor(0, 96, 96);

  if (s_printer_queue != nullptr) {
    PrinterChunkItem item;
    item.length = (len > 64) ? 64 : len;
    memcpy(item.data, data, item.length);
    if (xQueueSend(s_printer_queue, &item, 0) != pdTRUE) {
      PrinterChunkItem dummy;
      xQueueReceive(s_printer_queue, &dummy, 0);
      xQueueSend(s_printer_queue, &item, 0);
    }
  }
}

// ============================================================================
// CRC-16/CCITT-FALSE (Poly: 0x1021, Init: 0xFFFF)
// ============================================================================
uint16_t calculateCrc16(const uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= ((uint16_t)data[i] << 8);
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc = (crc << 1);
      }
    }
  }
  return crc;
}

static inline uint16_t readUint16LE(const uint8_t* buf) {
  return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static inline size_t getUtf8CharLen(uint8_t c) {
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

// ============================================================================
// PSRAM上の漢字音訓読みテーブル (/kanji_yomi.bin) ロード＆オンメモリ二分探索
// ============================================================================
bool loadKanjiYomiToPsram() {
  if (!LittleFS.exists("/kanji_yomi.bin")) return false;
  File f = LittleFS.open("/kanji_yomi.bin", "r");
  if (!f) return false;

  KanjiYomiHeader hdr;
  if (f.read((uint8_t*)&hdr, sizeof(KanjiYomiHeader)) != sizeof(KanjiYomiHeader)) {
    f.close();
    return false;
  }
  if (memcmp(hdr.magic, "YOMI", 4) != 0 || hdr.recordSize != sizeof(KanjiYomiRecord) || hdr.count == 0) {
    f.close();
    return false;
  }

  size_t tableBytes = (size_t)hdr.count * sizeof(KanjiYomiRecord);
  void* buf = heap_caps_malloc(tableBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (buf == nullptr) {
    f.close();
    return false;
  }

  if (f.read((uint8_t*)buf, tableBytes) != tableBytes) {
    heap_caps_free(buf);
    f.close();
    return false;
  }
  f.close();

  g_psramKanjiYomi = (KanjiYomiRecord*)buf;
  g_psramKanjiYomiCount = hdr.count;
  return true;
}

bool lookupKanjiYomiPsram(const char* utf8Char, char* outRomaji, size_t maxLen) {
  if (utf8Char == nullptr || g_psramKanjiYomi == nullptr || g_psramKanjiYomiCount == 0) return false;

  char target[4] = {0, 0, 0, 0};
  size_t ulen = getUtf8CharLen((uint8_t)utf8Char[0]);
  if (ulen > 4) ulen = 4;
  for (size_t u = 0; u < ulen; u++) target[u] = utf8Char[u];

  int32_t low = 0;
  int32_t high = (int32_t)g_psramKanjiYomiCount - 1;
  while (low <= high) {
    int32_t mid = low + (high - low) / 2;
    int cmp = memcmp(target, g_psramKanjiYomi[mid].utf8, 4);
    if (cmp == 0) {
      if (outRomaji != nullptr && maxLen > 0) {
        strncpy(outRomaji, g_psramKanjiYomi[mid].romaji, maxLen - 1);
        outRomaji[maxLen - 1] = '\0';
      }
      return true;
    } else if (cmp < 0) {
      high = mid - 1;
    } else {
      low = mid + 1;
    }
  }
  return false;
}

// ============================================================================
// メモリセキュア消去 & BLE ACK送信
// ============================================================================
void secureWipeMessageContext() {
  if (g_msgCtx == nullptr) return;
  memset((void*)g_msgCtx, 0, sizeof(MessageContext));
}

void drainBleQueue() {
  if (bleQueue == nullptr) return;
  BleQueueItem item;
  while (xQueueReceive(bleQueue, &item, 0) == pdTRUE) {
    volatile uint8_t* p = (volatile uint8_t*)&item;
    size_t n = sizeof(BleQueueItem);
    while (n--) *p++ = 0;
  }
}

void sendBleAck(const char* ackType, uint16_t sessionId, const char* detail = "") {
  if (!isBleConnected) return;

  char message[96];
  if (strlen(detail) > 0) {
    snprintf(message, sizeof(message), "ACK:%u:%s:%s", sessionId, ackType, detail);
  } else {
    snprintf(message, sizeof(message), "ACK:%u:%s", sessionId, ackType);
  }

  if (pTxRxCharacteristic != nullptr) {
    pTxRxCharacteristic->setValue((uint8_t*)message, strlen(message));
    pTxRxCharacteristic->notify();
  }
  if (pNusTxCharacteristic != nullptr) {
    pNusTxCharacteristic->setValue((uint8_t*)message, strlen(message));
    pNusTxCharacteristic->notify();
  }
  delay(currentState == STATE_TYPING ? 1 : 6);
}

void forceReleaseAllModifiers() {
  g_typingEngine.forceReleaseAll();
}

void executeAutoPullKeySequence() {
  setLedColor(0, 64, 64);
  Keyboard.press(KEY_LEFT_CTRL);
  delay(50);
  Keyboard.press('p');
  delay(80);
  forceReleaseAllModifiers();
  delay(350);
  Keyboard.write(KEY_RETURN);
  delay(100);
  forceReleaseAllModifiers();
  setLedColor(0, 64, 0);
}

// ============================================================================
// かなテーブル（生UTF-8テキスト受信時の安全フォールバック用）
// ============================================================================
struct KanaTableEntry {
  const char* kana;
  const char* romaji;
};

static const KanaTableEntry MONO_KANA_TABLE[] = {
  {"あ", "a"},  {"い", "i"},  {"う", "u"},  {"え", "e"},  {"お", "o"},
  {"か", "ka"}, {"き", "ki"}, {"く", "ku"}, {"け", "ke"}, {"こ", "ko"},
  {"が", "ga"}, {"ぎ", "gi"}, {"ぐ", "gu"}, {"げ", "ge"}, {"ご", "go"},
  {"さ", "sa"}, {"し", "si"}, {"す", "su"}, {"せ", "se"}, {"そ", "so"},
  {"ざ", "za"}, {"じ", "zi"}, {"ず", "zu"}, {"ぜ", "ze"}, {"ぞ", "zo"},
  {"た", "ta"}, {"ち", "ti"}, {"つ", "tu"}, {"て", "te"}, {"と", "to"},
  {"だ", "da"}, {"ぢ", "di"}, {"づ", "du"}, {"で", "de"}, {"ど", "do"},
  {"な", "na"}, {"に", "ni"}, {"ぬ", "nu"}, {"ね", "ne"}, {"の", "no"},
  {"は", "ha"}, {"ひ", "hi"}, {"ふ", "hu"}, {"へ", "he"}, {"ほ", "ho"},
  {"ば", "ba"}, {"び", "bi"}, {"ぶ", "bu"}, {"べ", "be"}, {"ぼ", "bo"},
  {"ぱ", "pa"}, {"ぴ", "pi"}, {"ぷ", "pu"}, {"ぺ", "pe"}, {"ぽ", "po"},
  {"ま", "ma"}, {"み", "mi"}, {"む", "mu"}, {"め", "me"}, {"も", "mo"},
  {"や", "ya"}, {"ゆ", "yu"}, {"よ", "yo"},
  {"ら", "ra"}, {"り", "ri"}, {"る", "ru"}, {"れ", "re"}, {"ろ", "ro"},
  {"わ", "wa"}, {"を", "wo"}, {"ん", "nn"},
  {"っ", "ltu"}, {"ゃ", "xya"}, {"ゅ", "xyu"}, {"ょ", "xyo"},
  {"ぁ", "xa"},  {"ぃ", "xi"},  {"ぅ", "xu"},  {"ぇ", "xe"},  {"ぉ", "xo"},
  {"ア", "a"},  {"イ", "i"},  {"ウ", "u"},  {"エ", "e"},  {"オ", "o"},
  {"カ", "ka"}, {"キ", "ki"}, {"ク", "ku"}, {"ケ", "ke"}, {"コ", "ko"},
  {"ガ", "ga"}, {"ギ", "gi"}, {"グ", "gu"}, {"ゲ", "ge"}, {"ゴ", "go"},
  {"サ", "sa"}, {"シ", "si"}, {"ス", "su"}, {"セ", "se"}, {"ソ", "so"},
  {"ザ", "za"}, {"ジ", "zi"}, {"ズ", "zu"}, {"ゼ", "ze"}, {"ゾ", "zo"},
  {"タ", "ta"}, {"チ", "ti"}, {"ツ", "tu"}, {"テ", "te"}, {"ト", "to"},
  {"ダ", "da"}, {"ヂ", "di"}, {"ヅ", "du"}, {"デ", "de"}, {"ド", "do"},
  {"ナ", "na"}, {"ニ", "ni"}, {"ヌ", "nu"}, {"ネ", "ne"}, {"ノ", "no"},
  {"ハ", "ha"}, {"ヒ", "hi"}, {"フ", "hu"}, {"ヘ", "he"}, {"ホ", "ho"},
  {"バ", "ba"}, {"ビ", "bi"}, {"ブ", "bu"}, {"ベ", "be"}, {"ボ", "bo"},
  {"パ", "pa"}, {"ピ", "pi"}, {"プ", "pu"}, {"ペ", "pe"}, {"ポ", "po"},
  {"マ", "ma"}, {"ミ", "mi"}, {"ム", "mu"}, {"メ", "me"}, {"モ", "mo"},
  {"ヤ", "ya"}, {"ユ", "yu"}, {"ヨ", "yo"},
  {"ラ", "ra"}, {"リ", "ri"}, {"ル", "ru"}, {"レ", "re"}, {"ロ", "ro"},
  {"ワ", "wa"}, {"ヲ", "wo"}, {"ン", "nn"},
  {"ッ", "ltu"}, {"ャ", "xya"}, {"ュ", "xyu"}, {"ョ", "xyo"},
  {"ァ", "xa"},  {"ィ", "xi"},  {"ゥ", "xu"},  {"ェ", "xe"},  {"ォ", "xo"},
  {"ヴ", "vu"},  {"ー", "-"}
};

const char* findMonoKanaRomaji(const char* s) {
  for (size_t i = 0; i < sizeof(MONO_KANA_TABLE) / sizeof(KanaTableEntry); i++) {
    if (memcmp(s, MONO_KANA_TABLE[i].kana, 3) == 0) {
      return MONO_KANA_TABLE[i].romaji;
    }
  }
  return nullptr;
}

// ============================================================================
// 医療安全統合ディスパッチャー:
// 1. 受信テキストをPSRAM上のAho-Corasick Trie木で数μs走査
// 2. 危険熟語（休薬・減量・漸減）や重要薬品名を検知し、BLE通知
// 3. 安全打鍵（安全分解 / F10バイパス / v20.1 HYBRID）を実行
// ============================================================================
void dispatchSafeStreamWithAhoCorasick(const char* buf, size_t total, uint16_t sessionId) {
  if (buf == nullptr || total == 0) return;

  // 1. Aho-Corasick 医療安全スキャン (PSRAM O(N) 数μs)
  uint32_t t0 = micros();
  SafetyMatchResult matches[16];
  uint16_t combinedFlags = 0;
  size_t matchCount = g_psramDb.scanTextAhoCorasick(buf, total, matches, 16, &combinedFlags);
  uint32_t scanUs = micros() - t0;

  if (matchCount > 0) {
    char alertDetail[64];
    snprintf(alertDetail, sizeof(alertDetail), "MATCH=%u,FLAGS=0x%04X,US=%lu",
             (unsigned int)matchCount, (unsigned int)combinedFlags, (unsigned long)scanUs);
    sendBleAck("SAFETY_SCAN", sessionId, alertDetail);
  }

  // 2. メタタグ付きストリーム ([Z], [K], [H], [A], [SAFE] 等) が含まれている場合は
  //    DonguriTypingEngine::dispatchStream で高速打鍵
  if (strstr(buf, "[Z]") != nullptr || strstr(buf, "[K]") != nullptr ||
      strstr(buf, "[H]") != nullptr || strstr(buf, "[A]") != nullptr ||
      strstr(buf, "[SAFE]") != nullptr || strstr(buf, "[IME_RESET]") != nullptr ||
      strstr(buf, "<Z>") != nullptr || strstr(buf, "<K>") != nullptr) {
    static uint32_t s_lastProgressMs = 0;
    static uint16_t s_activeSessionId = 0;
    s_activeSessionId = sessionId;
    s_lastProgressMs = millis();

    g_typingEngine.dispatchStream(buf, total, [](int cur, int tot) {
      // 800ms経過ごと、または完了時にBLEキープアライブ進捗通知を送出し、Supervision Timeout（切断）を未然に防止
      uint32_t now = millis();
      if (now - s_lastProgressMs > 800 || cur == tot) {
        s_lastProgressMs = now;
        char pDetail[32];
        int pct = tot > 0 ? (cur * 100 / tot) : 0;
        snprintf(pDetail, sizeof(pDetail), "%d%%,%d/%d", pct, cur, tot);
        sendBleAck("TYPING_PROGRESS", s_activeSessionId, pDetail);
      }
    });
    return;
  }

  // 3. 生UTF-8文字列が直接送られてきた場合（NUS等からの直接SOAP送信時）:
  //    PSRAM上のAho-Corasick結果およびkanji_yomi.binを用いて安全分解・F10バイパス打鍵を実行
  size_t i = 0;
  while (i < total) {
    uint8_t c0 = (uint8_t)buf[i];

    // ASCII領域: F10バイパスまたは直接安全打鍵
    if (c0 < 0x80) {
      if (c0 == '\r') {
        if (i + 1 < total && buf[i + 1] == '\n') i++;
        g_typingEngine.safeWrite(KEY_RETURN);
        i++;
        continue;
      }
      if (c0 == '\n') {
        g_typingEngine.safeWrite(KEY_RETURN);
        i++;
        continue;
      }
      // 英数字の連続はF10バイパスでIME誤変換を完全遮断
      if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9')) {
        char asciiChunk[64] = {0};
        size_t alen = 0;
        while (i < total && (uint8_t)buf[i] < 0x80 && buf[i] != '\r' && buf[i] != '\n' && alen < sizeof(asciiChunk) - 1) {
          asciiChunk[alen++] = buf[i++];
        }
        asciiChunk[alen] = '\0';
        g_typingEngine.sendAsciiF10(asciiChunk);
        continue;
      }
      g_typingEngine.sendSafeChar((char)c0);
      i++;
      continue;
    }

    // UTF-8 3バイト文字（日本語かな・漢字・句読点）
    size_t ulen = getUtf8CharLen(c0);
    if (ulen == 3 && i + 3 <= total) {
      // 句読点・長音
      if (memcmp(&buf[i], "、", 3) == 0 || memcmp(&buf[i], "。", 3) == 0 || memcmp(&buf[i], "ー", 3) == 0) {
        char punc[4] = {buf[i], buf[i+1], buf[i+2], '\0'};
        g_typingEngine.sendHiraganaH(punc);
        i += 3;
        continue;
      }
      // かな1文字
      const char* kanaRomaji = findMonoKanaRomaji(&buf[i]);
      if (kanaRomaji != nullptr) {
        // カタカナ判定 (0xE3 0x82 A1.. / 0xE3 0x83 ..)
        uint8_t b1 = (uint8_t)buf[i + 1];
        uint8_t b2 = (uint8_t)buf[i + 2];
        bool isKata = (b1 == 0x82 && b2 >= 0xA1) || (b1 == 0x83 && b2 <= 0xBC);
        if (isKata) {
          g_typingEngine.sendKatakanaF7(kanaRomaji);
        } else {
          g_typingEngine.sendHiraganaH(kanaRomaji);
        }
        i += 3;
        continue;
      }
      // 漢字1文字: PSRAM上の kanji_yomi.bin から1文字ずつ確定打鍵（安全分解）
      char yomiRomaji[12] = {0};
      if (lookupKanjiYomiPsram(&buf[i], yomiRomaji, sizeof(yomiRomaji))) {
        g_typingEngine.sendKanjiZ(yomiRomaji);
        i += 3;
        continue;
      }
    }

    i += ulen;
  }
}

void dispatchOutput() {
  if (g_msgCtx == nullptr || g_msgCtx->actualTotalBytes == 0) return;

  bool usbReady = false;
  for (int retry = 0; retry < 5; retry++) {
    if (tud_mounted()) {
      usbReady = true;
      break;
    }
    delay(50);
  }

  if (!usbReady) {
    currentState = STATE_USB_NOT_READY;
    sendBleAck("ERR_USB_NOT_READY", g_msgCtx->sessionId);
    return;
  }

  currentState = STATE_TYPING;
  setLedColor(64, 0, 0);
  sendBleAck("DISPATCH_STARTED", g_msgCtx->sessionId);

  dispatchSafeStreamWithAhoCorasick(g_msgCtx->assembledBuffer, g_msgCtx->actualTotalBytes, g_msgCtx->sessionId);

  forceReleaseAllModifiers();
  sendBleAck("USB_REPORTS_SENT", g_msgCtx->sessionId);
  delay(30);

  secureWipeMessageContext();
  // ★重要：drainBleQueue() は呼ばない！
  // 打鍵中または打鍵直後に届いた次のセッション（SessionInitHeader等）をキューから蒸発させるのを防ぐ

  currentState = isBleConnected ? STATE_BLE_CONNECTED : STATE_WAITING_BLE;
  setLedColor(0, isBleConnected ? 64 : 0, isBleConnected ? 0 : 64);
}

// ============================================================================
// BLE コールバック
// ============================================================================
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    (void)pServer;
    isBleConnected = true;
    currentState = STATE_BLE_CONNECTED;
    setLedColor(0, 64, 0);
  }

  void onDisconnect(BLEServer* pServer) override {
    isBleConnected = false;
    // ★最重要：打鍵中 (STATE_TYPING) は assembledBuffer の読み進めを保護するため
    // secureWipeMessageContext() や Keyboard.releaseAll() は絶対に実行しない！
    // 打鍵が最後まで完了した後に dispatchOutput() の末尾で安全にクリーンアップされる。
    if (currentState != STATE_TYPING) {
      currentState = STATE_WAITING_BLE;
      drainBleQueue();
      secureWipeMessageContext();
      Keyboard.releaseAll();
      setLedColor(0, 0, 64);
    }
    pServer->startAdvertising();
  }
};

class MyCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    uint8_t* pData = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();

    if (len > 0 && bleQueue != nullptr && len <= BLE_CHUNK_SIZE) {
      BleQueueItem item;
      item.length = (uint16_t)len;
      memcpy(item.data, pData, len);
      // キューが満杯の場合は最古のアイテムを破棄して最新アイテムを確実に保持
      if (xQueueSend(bleQueue, &item, 0) != pdTRUE) {
        BleQueueItem dummy;
        xQueueReceive(bleQueue, &dummy, 0);
        xQueueSend(bleQueue, &item, 0);
      }
    }
  }
};

// ============================================================================
// シリアルコマンド処理（PCからのテスト・ステータス確認・DFUリブート対応）
// ============================================================================
void handleUsbSerialCommands() {
  if (!USBSerial.available()) return;
  String line = USBSerial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;

  if (line == "CMD:STATUS" || line == "STATUS") {
    USBSerial.printf(
      "{\"device\":\"Dr.Dongly-Z (Seeed XIAO ESP32S3)\",\"fw\":\"v25.0-XIAO\","
      "\"flashSize\":%u,\"psramSize\":%u,\"freePsram\":%u,\"dbReady\":%s,"
      "\"mindsCount\":%u,\"drugsCount\":%u,\"trieNodes\":%u,\"kanjiYomi\":%u,"
      "\"psramDbBytes\":%u,\"kbdLayout\":\"%s\",\"usbMounted\":%s,\"bleConnected\":%s}\n",
      (unsigned int)ESP.getFlashChipSize(),
      (unsigned int)ESP.getPsramSize(),
      (unsigned int)ESP.getFreePsram(),
      g_psramDb.isReady() ? "true" : "false",
      (unsigned int)g_psramDb.getMindsCount(),
      (unsigned int)g_psramDb.getDrugsCount(),
      (unsigned int)g_psramDb.getTrieNodeCount(),
      (unsigned int)g_psramKanjiYomiCount,
      (unsigned int)g_psramDb.getPsramAllocatedBytes(),
      (g_typingEngine.getKeyboardLayout() == KBD_LAYOUT_US) ? "US" : "JIS",
      tud_mounted() ? "true" : "false",
      isBleConnected ? "true" : "false"
    );
  } else if (line.startsWith("CMD:SCAN:")) {
    String text = line.substring(9);
    SafetyMatchResult matches[16];
    uint16_t flags = 0;
    uint32_t t0 = micros();
    size_t cnt = g_psramDb.scanTextAhoCorasick(text.c_str(), text.length(), matches, 16, &flags);
    uint32_t elapsedUs = micros() - t0;
    USBSerial.printf("{\"scanUs\":%lu,\"matchCount\":%u,\"combinedFlags\":%u,\"matches\":[",
                     (unsigned long)elapsedUs, (unsigned int)cnt, (unsigned int)flags);
    for (size_t m = 0; m < cnt; m++) {
      if (m > 0) USBSerial.print(",");
      const DrugSafetyRecord64* d = g_psramDb.getDrugByIndex(matches[m].drugIdx);
      const MindsCqRecord256* c = g_psramDb.getMindsByIndex(matches[m].mindsIdx);
      USBSerial.printf("{\"offset\":%u,\"len\":%u,\"flags\":%u,\"drug\":\"%s\",\"mindsIcd\":\"%s\"}",
                       matches[m].byteOffset, matches[m].patternLen, matches[m].safetyFlags,
                       d ? d->genericName : "", c ? c->icd10 : "");
    }
    USBSerial.println("]}");
  } else if (line.startsWith("CMD:HOT9:")) {
    uint32_t hot9 = (uint32_t)strtoul(line.substring(9).c_str(), nullptr, 10);
    uint32_t t0 = micros();
    const DrugSafetyRecord64* rec = g_psramDb.binarySearchDrugByHot9(hot9);
    uint32_t elapsedUs = micros() - t0;
    if (rec) {
      USBSerial.printf("{\"found\":true,\"searchUs\":%lu,\"hot9\":%u,\"drugId\":%u,\"flags\":%u,\"name\":\"%s\"}\n",
                       (unsigned long)elapsedUs, rec->hot9Code, rec->drugId, rec->controlFlags, rec->genericName);
    } else {
      USBSerial.printf("{\"found\":false,\"searchUs\":%lu,\"hot9\":%u}\n", (unsigned long)elapsedUs, hot9);
    }
  } else if (line.startsWith("CMD:ICD10:")) {
    String icd = line.substring(10);
    uint32_t t0 = micros();
    const MindsCqRecord256* rec = g_psramDb.binarySearchMindsByIcd10(icd.c_str());
    uint32_t elapsedUs = micros() - t0;
    if (rec) {
      USBSerial.printf("{\"found\":true,\"searchUs\":%lu,\"id\":%u,\"icd10\":\"%s\",\"cq\":%u,\"strength\":%u,\"ev\":%u,\"summary\":\"%s\"}\n",
                       (unsigned long)elapsedUs, rec->guidelineId, rec->icd10, rec->cqNumber, rec->strength, rec->evidenceLevel, rec->summary);
    } else {
      USBSerial.printf("{\"found\":false,\"searchUs\":%lu,\"icd10\":\"%s\"}\n", (unsigned long)elapsedUs, icd.c_str());
    }
  } else if (line.startsWith("CMD:TEST_STREAM:")) {
    String streamStr = line.substring(16);
    USBSerial.printf("{\"test_stream_start\":%u}\n", streamStr.length());
    g_typingEngine.dispatchStream(streamStr.c_str(), streamStr.length(), [](int cur, int tot) {
      if (cur % 50 == 0 || cur == tot) {
        USBSerial.printf("{\"progress\":[%d,%d]}\n", cur, tot);
      }
    });
    USBSerial.println("{\"test_stream_end\":true}");
  } else if (line == "CMD:LAYOUT:US") {
    g_typingEngine.setKeyboardLayout(KBD_LAYOUT_US);
    USBSerial.println("{\"layout\":\"US\"}");
  } else if (line == "CMD:LAYOUT:JIS") {
    g_typingEngine.setKeyboardLayout(KBD_LAYOUT_JIS);
    USBSerial.println("{\"layout\":\"JIS\"}");
  } else if (line == "CMD:DFU" || line == "CMD:REBOOT_BOOTLOADER") {
    USBSerial.println("{\"rebooting\":\"ROM_BOOTLOADER\"}");
    delay(100);
    rebootToBootloader();
  }
}

// ============================================================================
// Arduino setup() - サバイバルルール準拠
// ============================================================================
void setup() {
  pinMode(XIAO_LED_PIN, OUTPUT);
  digitalWrite(XIAO_LED_PIN, LOW); // 起動表示点灯

  // ==========================================================================
  // 【サバイバルルール ①】USB CDC + HID + 仮想プリンタの即時初期化とネゴシエーション確立
  // ボタンを一切触らずにPC (PlatformIO / esptool) から100%全自動で上書き書き込み可能にする
  // ==========================================================================
  s_printer_queue = xQueueCreate(PRINTER_QUEUE_SIZE, sizeof(PrinterChunkItem));
  donguri_usb_printer_init(onPrinterDataReceived);

  USBSerial.enableReboot(true); // 1200bpsタッチおよびDTR/RTS自動リセット常時許可
  USBSerial.begin(115200);
  Keyboard.begin();

  USB.VID(0x303A);
  USB.PID(0x8027); // Windowsレジストリキャッシュ破棄・完全強制再列挙
  USB.usbClass(TUSB_CLASS_MISC);          // 0xEF (Windows Composite IAD 必須規格)
  USB.usbSubClass(MISC_SUBCLASS_COMMON);  // 0x02
  USB.usbProtocol(MISC_PROTOCOL_IAD);     // 0x01
  USB.firmwareVersion(0x0253);
  USB.productName("Dr.Dongly-Z XIAO Coprocessor");
  USB.manufacturerName("MedArt");
  USB.begin();
  Keyboard.releaseAll();

  // 1. USB CDCの起動猶予（PCとのネゴシエーション確立）
  delay(1000);

  // 2. 万が一のクラッシュ時にもリカバリ可能にするためのセーフティタイマー
  //    （起動直後3秒間はファイルシステムやPSRAMへの重い処理を実行せず、USBからのリセット要求を待機）
  for (int w = 0; w < 20; w++) {
    digitalWrite(XIAO_LED_PIN, (w & 1) ? LOW : HIGH);
    delay(100);
  }
  digitalWrite(XIAO_LED_PIN, LOW);

  // ==========================================================================
  // 3. 8MB PSRAM (MALLOC_CAP_SPIRAM) へのメッセージバッファ＆2.1MB固定長DB全展開
  // ==========================================================================
  g_msgCtx = (MessageContext*)heap_caps_malloc(sizeof(MessageContext), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (g_msgCtx == nullptr) {
    // 万一PSRAM確保失敗時は内部RAMへフォールバック
    g_msgCtx = (MessageContext*)malloc(sizeof(MessageContext));
  }
  secureWipeMessageContext();

  // LittleFS (binary_db) から minds.bin, drugs.bin, trie.bin, kanji_yomi.bin をPSRAMへ一括展開
  g_psramDb.begin();
  loadKanjiYomiToPsram();

  // ==========================================================================
  // 4. BLE起動 (DrVoice どんぐり君互換サービス + Nordic UART Service 両対応)
  // ==========================================================================
  bleQueue = xQueueCreate(BLE_QUEUE_SIZE, sizeof(BleQueueItem));

  BLEDevice::init("EHR-AI-Dongle");
  BLEDevice::setMTU(517);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
  esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);

  pBleServer = BLEDevice::createServer();
  pBleServer->setCallbacks(new MyServerCallbacks());

  // 4.1 既存 DrVoice どんぐり君 互換サービス
  BLEService* pService = pBleServer->createService(SERVICE_UUID);
  pTxRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_WRITE_NR |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxRxCharacteristic->setCallbacks(new MyCallbacks());
  BLE2902* p2902Main = new BLE2902();
  p2902Main->setNotifications(true);
  pTxRxCharacteristic->addDescriptor(p2902Main);
  pService->start();

  // 4.2 Nordic UART Service (NUS) 互換サービス
  BLEService* pNusService = pBleServer->createService(NUS_SERVICE_UUID);
  BLECharacteristic* pNusRx = pNusService->createCharacteristic(
    NUS_RX_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pNusRx->setCallbacks(new MyCallbacks());

  pNusTxCharacteristic = pNusService->createCharacteristic(
    NUS_TX_CHAR_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  BLE2902* p2902Nus = new BLE2902();
  p2902Nus->setNotifications(false);
  pNusTxCharacteristic->addDescriptor(p2902Nus);
  pNusService->start();

  // アドバタイジング開始
  BLEAdvertisementData oAdvData;
  oAdvData.setFlags(0x06);
  oAdvData.setName("EHR-AI-Dongle");

  BLEAdvertisementData oScanResp;
  oScanResp.setCompleteServices(BLEUUID(SERVICE_UUID));

  BLEAdvertising* pAdv = BLEDevice::getAdvertising();
  pAdv->setAdvertisementData(oAdvData);
  pAdv->setScanResponseData(oScanResp);
  pAdv->setScanResponse(true);
  pAdv->setMinInterval(0x20);
  pAdv->setMaxInterval(0x30);
  pAdv->setMinPreferred(0x0C);
  pAdv->setMaxPreferred(0x18);
  pAdv->start();

  lastUsbState = tud_mounted();
  setLedColor(0, 0, 64);
}

// ============================================================================
// Arduino loop()
// ============================================================================
void loop() {
  if (g_dfuRequested) {
    g_dfuRequested = false;
    delay(80);
    rebootToBootloader();
    return;
  }

  // PCからのUSB CDCシリアルコマンド監視
  handleUsbSerialCommands();

  // BLE切断時の自動再アドバタイズ
  static bool oldBleConnected = false;
  if (!isBleConnected && oldBleConnected) {
    delay(150);
    if (pBleServer != nullptr) {
      BLEDevice::startAdvertising();
    }
    oldBleConnected = isBleConnected;
  }
  if (isBleConnected && !oldBleConnected) {
    oldBleConnected = isBleConnected;
  }

  // USBマウント状態の監視
  bool currentUsbState = tud_mounted();
  if (currentUsbState != lastUsbState) {
    lastUsbState = currentUsbState;
    sendBleAck(currentUsbState ? "USB_MOUNTED" : "USB_UNMOUNTED", 0);
  }

  // BLE受信キュー処理
  BleQueueItem item;
  while (xQueueReceive(bleQueue, &item, 0) == pdTRUE) {

    // 0. ダイレクト v20.1 HYBRID ストリームの即時打鍵
    if (item.length >= 3 && (item.data[0] == '[' || item.data[0] == '<') &&
        (strncmp((const char*)item.data, "[IME_RESET]", 11) == 0 ||
         strncmp((const char*)item.data, "[SAFE]", 6) == 0 ||
         strncmp((const char*)item.data, "[Z]", 3) == 0 ||
         strncmp((const char*)item.data, "[K]", 3) == 0 ||
         strncmp((const char*)item.data, "[H]", 3) == 0 ||
         strncmp((const char*)item.data, "[A]", 3) == 0 ||
         strncmp((const char*)item.data, "[G]", 3) == 0 ||
         strncmp((const char*)item.data, "[F7]", 4) == 0 ||
         strncmp((const char*)item.data, "[F10]", 5) == 0 ||
         strncmp((const char*)item.data, "[RAW]", 5) == 0)) {
      dispatchSafeStreamWithAhoCorasick((const char*)item.data, item.length, 0);
      sendBleAck("HYBRID_STREAM_SENT", 0);
      continue;
    }

    // A. 遠隔制御コマンド (CMD:...)
    if (item.length >= 4 && memcmp(item.data, "CMD:", 4) == 0) {
      char cmdBuf[96];
      size_t copyLen = item.length < sizeof(cmdBuf) - 1 ? item.length : sizeof(cmdBuf) - 1;
      memcpy(cmdBuf, item.data, copyLen);
      cmdBuf[copyLen] = '\0';

      if (strncmp(cmdBuf, "CMD:ALT:", 8) == 0) {
        sendBleAck("ERR_ALT_DISABLED", 0);
        continue;
      }

      // キーボード配列動的切り替えコマンド (JIS / US)
      if (strcmp(cmdBuf, "CMD:LAYOUT:US") == 0 || strcmp(cmdBuf, "CMD:KBD_US") == 0) {
        g_typingEngine.setKeyboardLayout(KBD_LAYOUT_US);
        sendBleAck("LAYOUT_OK", 0, "US");
        continue;
      }
      if (strcmp(cmdBuf, "CMD:LAYOUT:JIS") == 0 || strcmp(cmdBuf, "CMD:KBD_JIS") == 0) {
        g_typingEngine.setKeyboardLayout(KBD_LAYOUT_JIS);
        sendBleAck("LAYOUT_OK", 0, "JIS");
        continue;
      }

      // HOT9 二分探索クエリ (CMD:HOT9:111400101)
      if (strncmp(cmdBuf, "CMD:HOT9:", 9) == 0) {
        uint32_t hot9 = (uint32_t)strtoul(cmdBuf + 9, nullptr, 10);
        const DrugSafetyRecord64* rec = g_psramDb.binarySearchDrugByHot9(hot9);
        if (rec) {
          char detail[64];
          snprintf(detail, sizeof(detail), "%u:0x%04X:%s", rec->drugId, rec->controlFlags, rec->genericName);
          sendBleAck("HOT9_FOUND", 0, detail);
        } else {
          sendBleAck("HOT9_NOT_FOUND", 0);
        }
        continue;
      }

      // ICD-10 Minds CQ 二分探索クエリ (CMD:ICD10:I71.0)
      if (strncmp(cmdBuf, "CMD:ICD10:", 10) == 0) {
        const MindsCqRecord256* rec = g_psramDb.binarySearchMindsByIcd10(cmdBuf + 10);
        if (rec) {
          char detail[64];
          snprintf(detail, sizeof(detail), "CQ%u:S%u:E%u", rec->cqNumber, rec->strength, rec->evidenceLevel);
          sendBleAck("ICD10_FOUND", rec->guidelineId, detail);
        } else {
          sendBleAck("ICD10_NOT_FOUND", 0);
        }
        continue;
      }

      if (strcmp(cmdBuf, "CMD:TRIGGER") == 0) {
        if (g_msgCtx != nullptr && (currentState == STATE_READY_TO_TYPE ||
            (currentState == STATE_RECEIVING && g_msgCtx->receivedCount == g_msgCtx->totalPackets))) {
          sendBleAck("REMOTE_TRIGGER_ACCEPTED", g_msgCtx->sessionId);
          dispatchOutput();
        } else if (currentState == STATE_TYPING) {
          sendBleAck("ALREADY_TYPING", g_msgCtx ? g_msgCtx->sessionId : 0);
        } else {
          // すでに打鍵完了済みの場合は安全に通知（ERR_NOT_READY_TO_TYPE を返すと PWA がエラー中断するため絶対に ERR_ は返さない）
          sendBleAck("ALREADY_COMPLETED", 0);
        }
        continue;
      }

      if (strncmp(cmdBuf, "CMD:PING:", 9) == 0) {
        sendBleAck("PONG", 0, cmdBuf + 9);
        continue;
      }

      if (strcmp(cmdBuf, "CMD:WIPE") == 0) {
        secureWipeMessageContext();
        drainBleQueue();
        Keyboard.releaseAll();
        currentState = isBleConnected ? STATE_BLE_CONNECTED : STATE_WAITING_BLE;
        sendBleAck("WIPED", 0);
        continue;
      }

      if (strcmp(cmdBuf, "CMD:STATUS") == 0) {
        char statusDetail[64];
        snprintf(statusDetail, sizeof(statusDetail), "%d,%s,PSRAM:%uKB,%s",
                 currentState, tud_mounted() ? "USB_OK" : "USB_NO",
                 (unsigned int)(ESP.getFreePsram() / 1024),
                 (g_typingEngine.getKeyboardLayout() == KBD_LAYOUT_US) ? "US" : "JIS");
        sendBleAck("STATUS_RESP", g_msgCtx ? g_msgCtx->sessionId : 0, statusDetail);
        continue;
      }

      if (strcmp(cmdBuf, "CMD:AUTO_PULL") == 0 || strcmp(cmdBuf, "CMD:PULL") == 0) {
        sendBleAck("AUTO_PULL_STARTING", 0);
        executeAutoPullKeySequence();
        sendBleAck("AUTO_PULL_TRIGGERED", 0);
        continue;
      }

      if (strcmp(cmdBuf, "CMD:REBOOT_BOOTLOADER") == 0 || strcmp(cmdBuf, "CMD:DFU") == 0) {
        sendBleAck("REBOOTING_BOOTLOADER", 0);
        delay(150);
        rebootToBootloader();
        continue;
      }
    }

    if (g_msgCtx == nullptr) continue;

    // B. セッション初期化フレーム (8バイト)
    if (item.length == sizeof(SessionInitHeader)) {
      uint16_t sid = readUint16LE(item.data);
      uint8_t totPkts = item.data[2];
      uint16_t totBytes = readUint16LE(item.data + 3);
      uint16_t totCrc = readUint16LE(item.data + 5);

      if (totPkts == 0 || totPkts > MAX_PACKETS || totBytes == 0 || totBytes > MAX_PAYLOAD_SIZE) {
        sendBleAck("ERR_INIT_PARAMS", sid);
        currentState = STATE_ERROR;
        continue;
      }

      secureWipeMessageContext();
      Keyboard.releaseAll();
      g_msgCtx->sessionId = sid;
      g_msgCtx->totalPackets = totPkts;
      g_msgCtx->expectedTotalBytes = totBytes;
      g_msgCtx->expectedTotalCrc = totCrc;
      g_msgCtx->lastActivityTime = millis();
      g_msgCtx->retryAttempts = 0;
      currentState = STATE_RECEIVING;
      sendBleAck("SESSION_READY", sid);
      continue;
    }

    // C. データスロットフレーム (ヘッダ8B + ペイロード)
    if (item.length > sizeof(SlotHeader)) {
      uint16_t sid = readUint16LE(item.data);
      uint8_t seqNo = item.data[2];
      uint8_t mode = item.data[3];
      uint16_t payloadLen = readUint16LE(item.data + 4);
      uint16_t chunkCrc = readUint16LE(item.data + 6);
      uint8_t* payload = item.data + sizeof(SlotHeader);
      size_t actualLen = item.length - sizeof(SlotHeader);

      if (currentState != STATE_RECEIVING || g_msgCtx->sessionId != sid) {
        continue;
      }
      if (seqNo >= g_msgCtx->totalPackets || payloadLen != actualLen || payloadLen > MAX_PACKET_LEN) {
        sendBleAck("ERR_SLOT_HEADER", sid);
        continue;
      }
      if (calculateCrc16(payload, payloadLen) != chunkCrc) {
        sendBleAck("ERR_CHUNK_CRC", sid);
        continue;
      }

      if (!g_msgCtx->slots[seqNo].received) {
        g_msgCtx->slots[seqNo].len = payloadLen;
        memcpy(g_msgCtx->slots[seqNo].payload, payload, payloadLen);
        g_msgCtx->slots[seqNo].received = true;
        g_msgCtx->receivedCount++;
        g_msgCtx->mode = mode;
        g_msgCtx->lastActivityTime = millis();
      }

      if (g_msgCtx->receivedCount == g_msgCtx->totalPackets) {
        size_t offset = 0;
        bool bufferOverflow = false;
        for (uint8_t slotIdx = 0; slotIdx < g_msgCtx->totalPackets; slotIdx++) {
          if (offset + g_msgCtx->slots[slotIdx].len > MAX_PAYLOAD_SIZE) {
            bufferOverflow = true;
            break;
          }
          memcpy(g_msgCtx->assembledBuffer + offset, g_msgCtx->slots[slotIdx].payload, g_msgCtx->slots[slotIdx].len);
          offset += g_msgCtx->slots[slotIdx].len;
        }

        if (bufferOverflow || offset != g_msgCtx->expectedTotalBytes) {
          sendBleAck("ERR_LEN_MISMATCH", g_msgCtx->sessionId);
          secureWipeMessageContext();
          currentState = STATE_ERROR;
          break;
        }

        g_msgCtx->assembledBuffer[offset] = '\0';
        g_msgCtx->actualTotalBytes = offset;

        if (calculateCrc16((uint8_t*)g_msgCtx->assembledBuffer, offset) != g_msgCtx->expectedTotalCrc) {
          sendBleAck("ERR_TOTAL_CRC", g_msgCtx->sessionId);
          secureWipeMessageContext();
          currentState = STATE_ERROR;
          break;
        }

        currentState = STATE_READY_TO_TYPE;
        sendBleAck("ALL_PACKETS_READY", g_msgCtx->sessionId);
        delay(60);
        dispatchOutput();
      }
    }
  }

  // 受信タイムアウトと再送要求 (RETRY)
  if (g_msgCtx != nullptr && currentState == STATE_RECEIVING && g_msgCtx->receivedCount < g_msgCtx->totalPackets) {
    if (millis() - g_msgCtx->lastActivityTime > RX_TIMEOUT_MS) {
      if (g_msgCtx->retryAttempts < MAX_RETRY_COUNT) {
        g_msgCtx->retryAttempts++;
        uint32_t missingBitmap = 0;
        for (uint8_t pIdx = 0; pIdx < g_msgCtx->totalPackets; pIdx++) {
          if (!g_msgCtx->slots[pIdx].received) {
            missingBitmap |= (1UL << pIdx);
          }
        }
        char bitmapHex[14];
        if (g_msgCtx->totalPackets <= 16) {
          snprintf(bitmapHex, sizeof(bitmapHex), "0x%04X", (unsigned int)missingBitmap);
        } else {
          snprintf(bitmapHex, sizeof(bitmapHex), "0x%08lX", (unsigned long)missingBitmap);
        }
        sendBleAck("RETRY", g_msgCtx->sessionId, bitmapHex);
        g_msgCtx->lastActivityTime = millis();
      } else {
        sendBleAck("ERR_TIMEOUT_ABORT", g_msgCtx->sessionId);
        secureWipeMessageContext();
        drainBleQueue();
        Keyboard.releaseAll();
        currentState = isBleConnected ? STATE_BLE_CONNECTED : STATE_WAITING_BLE;
      }
    }
  }

  // 仮想プリンタ受信データのBLE転送
  // ★カルテ転送中 (STATE_RECEIVING / STATE_TYPING) は ACK 衝突を防ぐためアイドル時のみ転送
  if (s_printer_queue != nullptr && currentState == STATE_BLE_CONNECTED) {
    PrinterChunkItem prnItem;
    while (xQueueReceive(s_printer_queue, &prnItem, 0) == pdTRUE) {
      if (isBleConnected && pTxRxCharacteristic != nullptr) {
        uint8_t notifyBuf[68];
        notifyBuf[0] = 'P'; notifyBuf[1] = 'R'; notifyBuf[2] = 'N'; notifyBuf[3] = ':';
        memcpy(&notifyBuf[4], prnItem.data, prnItem.length);
        pTxRxCharacteristic->setValue(notifyBuf, prnItem.length + 4);
        pTxRxCharacteristic->notify();
        delay(4);
      }
    }
  }

  if (s_printer_job_active && currentState == STATE_BLE_CONNECTED && (millis() - s_last_printer_rx_ms > 400)) {
    s_printer_job_active = false;
    if (isBleConnected && pTxRxCharacteristic != nullptr) {
      char endMsg[32];
      snprintf(endMsg, sizeof(endMsg), "PRN_END:%u", s_printer_total_bytes);
      pTxRxCharacteristic->setValue((uint8_t*)endMsg, strlen(endMsg));
      pTxRxCharacteristic->notify();
    }
    s_printer_total_bytes = 0;
  }

  delay(10);
}
