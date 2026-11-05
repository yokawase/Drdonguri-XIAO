#ifndef XIAO_BINARY_DB_H
#define XIAO_BINARY_DB_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include "esp_heap_caps.h"

// ============================================================================
// Dr.Dongly-Z (Seeed Studio XIAO ESP32S3 Dedicated Edition)
// 8MB Flash (LittleFS: binary_db) ➔ 8MB PSRAM (MALLOC_CAP_SPIRAM)
// 2.1MB 固定長バイナリ・ゼロレイテンシ医療知識＆Aho-Corasick安全照合エンジン
// ============================================================================

// 制御フラグ定義 (DrugSafetyRecord64::controlFlags)
#define DRUG_FLAG_NORMAL          0x0000
#define DRUG_FLAG_SAFE_DECOMPOSE  0x0001  // 安全分解（1文字ずつ確定打鍵）
#define DRUG_FLAG_F10_BYPASS      0x0002  // 半角英数F10変換強制
#define DRUG_FLAG_KATAKANA_F7     0x0004  // 全角カタカナF7変換強制
#define DRUG_FLAG_HIGH_RISK_ALERT 0x0008  // 医療安全ハイリスク語（休薬・減量・漸減・禁忌薬等）
#define DRUG_FLAG_RENAL_CHECK     0x0010  // 腎機能確認推奨

#pragma pack(push, 1)

// ----------------------------------------------------------------------------
// 1. Minds診療ガイドライン CQレコード（厳格256バイト固定長）
//    - ガイドラインID (2B) + CQ番号 (2B) + 推奨度 (1B) + エビデンスレベル (1B)
//      + ICD-10 (8B) + 128次元INT8量子化埋め込みベクトル (128B) + 要約文 (114B UTF-8)
//      ＝ 合計 256 バイト（static_assert検証済）
// ----------------------------------------------------------------------------
struct MindsCqRecord256 {
    uint16_t guidelineId;       // 2B: ガイドラインID (昇順ソート主キーまたはICD10ハッシュ連携)
    uint16_t cqNumber;          // 2B: CQ番号
    uint8_t  strength;          // 1B: 推奨度 (1:強推奨, 2:弱推奨, 3:弱非推奨, 4:強非推奨)
    uint8_t  evidenceLevel;     // 1B: エビデンスレベル (1:A, 2:B, 3:C, 4:D)
    char     icd10[8];          // 8B: ICD-10コード (Null終端/パディング, 例: "I71.0\0\0\0")
    int8_t   embedding128[128]; // 128B: 128次元INT8量子化埋め込みベクトル
    char     summary[114];      // 114B: 推奨要約文 (UTF-8, Null終端保証)
};
static_assert(sizeof(MindsCqRecord256) == 256, "MindsCqRecord256 must be exactly 256 bytes");

// ----------------------------------------------------------------------------
// 2. 医薬品・安全分解マスターレコード（厳格64バイト固定長）
//    - HOT9コード (4B) + 薬品ID (2B) + 制御フラグ (2B: 安全分解/F10バイパス等)
//      + 一般名/打鍵ローマ字 (32B UTF-8) + 禁忌ブルームフィルタ (16B) + パディング (8B)
//      ＝ 合計 64 バイト（static_assert検証済）
// ----------------------------------------------------------------------------
struct DrugSafetyRecord64 {
    uint32_t hot9Code;          // 4B: HOT9コード (昇順ソートキー / FNV-1aハッシュ互換)
    uint16_t drugId;            // 2B: 薬品ID
    uint16_t controlFlags;      // 2B: 制御フラグ (DRUG_FLAG_*)
    char     genericName[32];   // 32B: 一般名または安全打鍵表現 (UTF-8)
    uint8_t  contraBloom[16];   // 16B: 禁忌ICD-10ブルームフィルタ (128bit)
    char     safeRomaji[8];     // 8B: パディング領域兼・短縮ローマ字ヒント
};
static_assert(sizeof(DrugSafetyRecord64) == 64, "DrugSafetyRecord64 must be exactly 64 bytes");

// ----------------------------------------------------------------------------
// 3. Aho-Corasick ダブル配列/フラットTrieノード（厳格16バイト固定長）
//    - UTF-8バイト遷移をPSRAM上で O(N) 数マイクロ秒で走査するための決定論的オートマトン
// ----------------------------------------------------------------------------
struct AcTrieFlatNode16 {
    uint16_t firstChildIdx;     // 2B: 子ノード配列の開始インデックス (0 = 子なし)
    uint8_t  childCount;        // 1B: 子ノード数
    uint8_t  transByte;         // 1B: 親からこのノードへの遷移UTF-8バイト
    uint16_t failIdx;           // 2B: Aho-Corasick Failureリンク先ノードインデックス
    uint16_t matchDrugIdx;      // 2B: マッチしたDrugSafetyRecord64のインデックス (0xFFFF = なし)
    uint16_t matchMindsIdx;     // 2B: マッチしたMindsCqRecord256のインデックス (0xFFFF = なし)
    uint16_t safetyFlags;       // 2B: マッチ時の即時医療安全フラグ (DRUG_FLAG_*)
    uint8_t  patternLen;        // 1B: マッチしたパターンのUTF-8バイト長
    uint8_t  reserved[3];       // 3B: アライメント予約
};
static_assert(sizeof(AcTrieFlatNode16) == 16, "AcTrieFlatNode16 must be exactly 16 bytes");

#pragma pack(pop)

// Aho-Corasick 照合ヒット結果
struct SafetyMatchResult {
    uint16_t byteOffset;        // 入力テキスト内の終了バイト位置
    uint8_t  patternLen;        // パターンUTF-8バイト長
    uint16_t safetyFlags;       // DRUG_FLAG_*
    uint16_t drugIdx;           // DrugSafetyRecord64 インデックス (0xFFFF=なし)
    uint16_t mindsIdx;          // MindsCqRecord256 インデックス (0xFFFF=なし)
};

class XiaoPsramDatabase {
public:
    XiaoPsramDatabase()
        : _mindsTable(nullptr), _mindsCount(0),
          _drugsTable(nullptr), _drugsCount(0),
          _trieNodes(nullptr), _trieNodeCount(0),
          _psramAllocatedBytes(0), _initialized(false) {}

    bool begin() {
        if (_initialized) return true;

        // LittleFS ("binary_db" パーティション) マウント
        if (!LittleFS.begin(true, "/littlefs", 10, "binary_db")) {
            return false;
        }

        bool okMinds = loadMindsToPsram("/minds.bin");
        bool okDrugs = loadDrugsToPsram("/drugs.bin");
        bool okTrie  = loadTrieToPsram("/trie.bin");

        _initialized = (okMinds && okDrugs && okTrie);
        return _initialized;
    }

    bool isReady() const { return _initialized; }
    size_t getMindsCount() const { return _mindsCount; }
    size_t getDrugsCount() const { return _drugsCount; }
    size_t getTrieNodeCount() const { return _trieNodeCount; }
    size_t getPsramAllocatedBytes() const { return _psramAllocatedBytes; }

    // ------------------------------------------------------------------------
    // 1. HOT9コードによるオンメモリ二分探索 (O(log N), 1μs以下)
    // ------------------------------------------------------------------------
    const DrugSafetyRecord64* binarySearchDrugByHot9(uint32_t hot9Code) const {
        if (_drugsTable == nullptr || _drugsCount == 0) return nullptr;
        int32_t low = 0;
        int32_t high = (int32_t)_drugsCount - 1;
        while (low <= high) {
            int32_t mid = low + (high - low) / 2;
            uint32_t midVal = _drugsTable[mid].hot9Code;
            if (midVal == hot9Code) {
                return &_drugsTable[mid];
            } else if (midVal < hot9Code) {
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
        return nullptr;
    }

    // ------------------------------------------------------------------------
    // 2. ICD-10コード前方一致・二分探索 (O(log N), 1μs以下)
    // ------------------------------------------------------------------------
    const MindsCqRecord256* binarySearchMindsByIcd10(const char* icd10) const {
        if (_mindsTable == nullptr || _mindsCount == 0 || icd10 == nullptr) return nullptr;
        char target[8] = {0};
        strncpy(target, icd10, 7);

        int32_t low = 0;
        int32_t high = (int32_t)_mindsCount - 1;
        while (low <= high) {
            int32_t mid = low + (high - low) / 2;
            int cmp = strncmp(_mindsTable[mid].icd10, target, 8);
            if (cmp == 0) {
                return &_mindsTable[mid];
            } else if (cmp < 0) {
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
        return nullptr;
    }

    // ------------------------------------------------------------------------
    // 3. 128次元 INT8 量子化ベクトルによる高速ドット積セマンティック検索
    // ------------------------------------------------------------------------
    const MindsCqRecord256* searchMindsByVectorInt8(const int8_t* queryVec128, int32_t* outBestScore = nullptr) const {
        if (_mindsTable == nullptr || _mindsCount == 0 || queryVec128 == nullptr) return nullptr;
        int32_t bestScore = -2147483647;
        size_t bestIdx = 0;
        for (size_t i = 0; i < _mindsCount; i++) {
            const int8_t* v = _mindsTable[i].embedding128;
            int32_t dot = 0;
            for (size_t d = 0; d < 128; d++) {
                dot += (int32_t)queryVec128[d] * (int32_t)v[d];
            }
            if (dot > bestScore) {
                bestScore = dot;
                bestIdx = i;
            }
        }
        if (outBestScore) *outBestScore = bestScore;
        return &_mindsTable[bestIdx];
    }

    // ------------------------------------------------------------------------
    // 4. Aho-Corasick 多パターン医療安全スキャン (PSRAM Trie走査, 数μs)
    //    「休薬」「減量」「漸減」等の危険熟語および重要薬品名を検知する
    // ------------------------------------------------------------------------
    size_t scanTextAhoCorasick(const char* utf8Text, size_t textLen,
                               SafetyMatchResult* outMatches, size_t maxMatches,
                               uint16_t* outCombinedFlags = nullptr) const {
        if (outCombinedFlags) *outCombinedFlags = 0;
        if (_trieNodes == nullptr || _trieNodeCount == 0 || utf8Text == nullptr || textLen == 0) {
            return 0;
        }

        size_t matchCount = 0;
        uint16_t combinedFlags = 0;
        uint16_t currentNode = 0; // Root = 0

        for (size_t i = 0; i < textLen; i++) {
            uint8_t b = (uint8_t)utf8Text[i];

            // 遷移が見つかるかRootに到達するまでFailureリンクを辿る
            while (currentNode != 0 && findChildNode(currentNode, b) == 0xFFFF) {
                currentNode = _trieNodes[currentNode].failIdx;
            }

            uint16_t nextNode = findChildNode(currentNode, b);
            if (nextNode != 0xFFFF) {
                currentNode = nextNode;
            } else {
                currentNode = 0;
            }

            // 出力リンク（自身およびFailureチェーン上のマッチ）を収集
            uint16_t tempNode = currentNode;
            while (tempNode != 0) {
                const AcTrieFlatNode16& node = _trieNodes[tempNode];
                if (node.patternLen > 0) {
                    combinedFlags |= node.safetyFlags;
                    if (outMatches != nullptr && matchCount < maxMatches) {
                        outMatches[matchCount].byteOffset  = (uint16_t)i;
                        outMatches[matchCount].patternLen  = node.patternLen;
                        outMatches[matchCount].safetyFlags = node.safetyFlags;
                        outMatches[matchCount].drugIdx     = node.matchDrugIdx;
                        outMatches[matchCount].mindsIdx    = node.matchMindsIdx;
                        matchCount++;
                    }
                }
                tempNode = node.failIdx;
            }
        }

        if (outCombinedFlags) *outCombinedFlags = combinedFlags;
        return matchCount;
    }

    const MindsCqRecord256* getMindsByIndex(uint16_t idx) const {
        if (_mindsTable == nullptr || idx >= _mindsCount) return nullptr;
        return &_mindsTable[idx];
    }

    const DrugSafetyRecord64* getDrugByIndex(uint16_t idx) const {
        if (_drugsTable == nullptr || idx >= _drugsCount) return nullptr;
        return &_drugsTable[idx];
    }

private:
    MindsCqRecord256*   _mindsTable;
    size_t              _mindsCount;
    DrugSafetyRecord64* _drugsTable;
    size_t              _drugsCount;
    AcTrieFlatNode16*   _trieNodes;
    size_t              _trieNodeCount;
    size_t              _psramAllocatedBytes;
    bool                _initialized;

    // 子ノードの二分探索（子ノードはtransByte昇順で格納されている）
    inline uint16_t findChildNode(uint16_t parentIdx, uint8_t targetByte) const {
        if (parentIdx >= _trieNodeCount) return 0xFFFF;
        const AcTrieFlatNode16& parent = _trieNodes[parentIdx];
        if (parent.childCount == 0 || parent.firstChildIdx == 0) return 0xFFFF;

        int32_t low = (int32_t)parent.firstChildIdx;
        int32_t high = low + (int32_t)parent.childCount - 1;
        if (high >= (int32_t)_trieNodeCount) high = (int32_t)_trieNodeCount - 1;

        while (low <= high) {
            int32_t mid = low + (high - low) / 2;
            uint8_t midByte = _trieNodes[mid].transByte;
            if (midByte == targetByte) {
                return (uint16_t)mid;
            } else if (midByte < targetByte) {
                low = mid + 1;
            } else {
                high = mid - 1;
            }
        }
        return 0xFFFF;
    }

    bool loadMindsToPsram(const char* path) {
        if (!LittleFS.exists(path)) return false;
        File f = LittleFS.open(path, "r");
        if (!f) return false;
        size_t fileSize = f.size();
        if (fileSize == 0 || (fileSize % sizeof(MindsCqRecord256)) != 0) {
            f.close();
            return false;
        }
        size_t count = fileSize / sizeof(MindsCqRecord256);
        void* buf = heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == nullptr) {
            f.close();
            return false;
        }
        size_t readBytes = f.read((uint8_t*)buf, fileSize);
        f.close();
        if (readBytes != fileSize) {
            heap_caps_free(buf);
            return false;
        }
        _mindsTable = (MindsCqRecord256*)buf;
        _mindsCount = count;
        _psramAllocatedBytes += fileSize;
        return true;
    }

    bool loadDrugsToPsram(const char* path) {
        if (!LittleFS.exists(path)) return false;
        File f = LittleFS.open(path, "r");
        if (!f) return false;
        size_t fileSize = f.size();
        if (fileSize == 0 || (fileSize % sizeof(DrugSafetyRecord64)) != 0) {
            f.close();
            return false;
        }
        size_t count = fileSize / sizeof(DrugSafetyRecord64);
        void* buf = heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == nullptr) {
            f.close();
            return false;
        }
        size_t readBytes = f.read((uint8_t*)buf, fileSize);
        f.close();
        if (readBytes != fileSize) {
            heap_caps_free(buf);
            return false;
        }
        _drugsTable = (DrugSafetyRecord64*)buf;
        _drugsCount = count;
        _psramAllocatedBytes += fileSize;
        return true;
    }

    bool loadTrieToPsram(const char* path) {
        if (!LittleFS.exists(path)) return false;
        File f = LittleFS.open(path, "r");
        if (!f) return false;
        size_t fileSize = f.size();
        if (fileSize == 0 || (fileSize % sizeof(AcTrieFlatNode16)) != 0) {
            f.close();
            return false;
        }
        size_t count = fileSize / sizeof(AcTrieFlatNode16);
        void* buf = heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf == nullptr) {
            f.close();
            return false;
        }
        size_t readBytes = f.read((uint8_t*)buf, fileSize);
        f.close();
        if (readBytes != fileSize) {
            heap_caps_free(buf);
            return false;
        }
        _trieNodes = (AcTrieFlatNode16*)buf;
        _trieNodeCount = count;
        _psramAllocatedBytes += fileSize;
        return true;
    }
};

#endif // XIAO_BINARY_DB_H
