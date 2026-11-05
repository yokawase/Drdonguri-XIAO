#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
=============================================================================
Dr.Dongly-Z (Seeed Studio XIAO ESP32S3 Dedicated Edition)
2.1MB 固定長バイナリ (minds.bin, drugs.bin, trie.bin) コンパイラ

1. minds.bin (256バイト固定長 × CQ件数)
   - uint16_t guidelineId (2B)
   - uint16_t cqNumber    (2B)
   - uint8_t  strength    (1B)
   - uint8_t  evidenceLvl (1B)
   - char     icd10[8]    (8B)
   - int8_t   embedding128[128] (128B)
   - char     summary[114] (114B)
   => 計 256 バイト

2. drugs.bin (64バイト固定長 × 品目数)
   - uint32_t hot9Code    (4B)
   - uint16_t drugId      (2B)
   - uint16_t controlFlags(2B)
   - char     genericName[32] (32B)
   - uint8_t  contraBloom[16] (16B)
   - char     safeRomaji[8]   (8B)
   => 計 64 バイト

3. trie.bin (16バイト固定長 × Aho-Corasick ノード数)
   - uint16_t firstChildIdx (2B)
   - uint8_t  childCount    (1B)
   - uint8_t  transByte     (1B)
   - uint16_t failIdx       (2B)
   - uint16_t matchDrugIdx  (2B)
   - uint16_t matchMindsIdx (2B)
   - uint16_t safetyFlags   (2B)
   - uint8_t  patternLen    (1B)
   - uint8_t  reserved[3]   (3B)
   => 計 16 バイト
=============================================================================
"""

import os
import sys
import struct
import hashlib
from collections import deque

# Flag constants matching xiao_binary_db.h
DRUG_FLAG_NORMAL          = 0x0000
DRUG_FLAG_SAFE_DECOMPOSE  = 0x0001
DRUG_FLAG_F10_BYPASS      = 0x0002
DRUG_FLAG_KATAKANA_F7     = 0x0004
DRUG_FLAG_HIGH_RISK_ALERT = 0x0008
DRUG_FLAG_RENAL_CHECK     = 0x0010


def pad_utf8(text: str, max_bytes: int) -> bytes:
    """UTF-8文字境界を破壊せずにmax_bytes(末尾Null保証)にパディングする"""
    encoded = bytearray()
    for ch in text:
        cb = ch.encode("utf-8")
        if len(encoded) + len(cb) > max_bytes - 1:
            break
        encoded.extend(cb)
    return bytes(encoded) + b"\x00" * (max_bytes - len(encoded))


def deterministic_int8_embedding(seed_text: str) -> bytes:
    """テキストから決定論的かつ正規化された128次元INT8ベクトルを生成"""
    vec = bytearray(128)
    for blk in range(4):
        h = hashlib.sha256(f"{seed_text}:{blk}".encode("utf-8")).digest()
        for i in range(32):
            # Convert 0..255 to signed -127..127
            val = h[i] - 128
            if val == -128:
                val = -127
            vec[blk * 32 + i] = val & 0xFF
    return bytes(vec)


def compute_bloom_16b(icd10_list: list) -> bytes:
    """禁忌ICD-10リストから128bit (16バイト) ブルームフィルタを生成"""
    bits = [0] * 16
    for code in icd10_list:
        for salt in (1, 2, 3):
            digest = hashlib.md5(f"{code}:{salt}".encode("utf-8")).digest()
            bit_pos = int.from_bytes(digest[:2], "little") % 128
            bits[bit_pos // 8] |= (1 << (bit_pos % 8))
    return bytes(bits)


def load_minds_source():
    """既存Mindsデータ資産があれば読み込み、拡張して返す"""
    ref_dir = "/home/medart-code/Drdonguri/firmware/scripts/minds_data"
    entries = []
    if os.path.isdir(ref_dir):
        sys.path.insert(0, ref_dir)
        module_names = [
            "infection", "respiratory", "cardiovascular", "metabolism",
            "gastroenterology", "nephrology_urology", "neurology",
            "psychiatry", "orthopedics_rheumatology", "dermatology",
            "ent_ophthalmology", "pediatrics", "gynecology", "emergency_general",
            "expanded_clinical_cqs", "expanded_clinical_cqs_part2", "expanded_clinical_cqs_part3"
        ]
        for mod_name in module_names:
            try:
                mod = __import__(mod_name)
                entries.extend(getattr(mod, "ENTRIES", []))
            except Exception:
                pass
    return entries


def build_minds_bin(out_dir: str):
    raw_entries = load_minds_source()
    records = []

    for idx, e in enumerate(raw_entries):
        icd10 = str(e.get("icd10", "Z00.0"))[:7]
        cq_num = int(e.get("cq_num", 1))
        strength = int(e.get("strength", 1))
        evidence = int(e.get("evidence_level", 1))
        disease = e.get("disease_name", "")
        rec_text = e.get("recommendation", "")
        summary = f"【{disease}】{rec_text}"
        triggers = e.get("trigger_words", [disease])
        records.append({
            "guidelineId": idx + 1,
            "cqNumber": cq_num,
            "strength": strength,
            "evidenceLevel": evidence,
            "icd10": icd10,
            "summary": summary,
            "triggers": triggers,
            "disease": disease,
        })

    # ICD-10昇順ソート（二分探索 O(log N) 用）
    records.sort(key=lambda r: r["icd10"])
    for new_id, r in enumerate(records):
        r["guidelineId"] = new_id + 1

    STRUCT_FMT = "<HHBB8s128s114s"
    assert struct.calcsize(STRUCT_FMT) == 256

    bin_data = bytearray()
    for r in records:
        emb = deterministic_int8_embedding(f"{r['icd10']}:{r['summary']}")
        packed = struct.pack(
            STRUCT_FMT,
            r["guidelineId"] & 0xFFFF,
            r["cqNumber"] & 0xFFFF,
            r["strength"] & 0xFF,
            r["evidenceLevel"] & 0xFF,
            pad_utf8(r["icd10"], 8),
            emb,
            pad_utf8(r["summary"], 114),
        )
        assert len(packed) == 256
        bin_data.extend(packed)

    out_path = os.path.join(out_dir, "minds.bin")
    with open(out_path, "wb") as f:
        f.write(bin_data)
    print(f"[OK] minds.bin generated: {len(records)} records ({len(bin_data):,} bytes)")
    return records


def build_drugs_bin(out_dir: str):
    # 医療安全重要語（安全分解・ハイリスク語）および代表医薬品マスター
    safety_Critical_seeds = [
        # (hot9, name, romaji_hint, flags, contra_icds)
        (100000001, "休薬", "kyuuyaku", DRUG_FLAG_SAFE_DECOMPOSE | DRUG_FLAG_HIGH_RISK_ALERT, []),
        (100000002, "減量", "genryou", DRUG_FLAG_SAFE_DECOMPOSE | DRUG_FLAG_HIGH_RISK_ALERT, []),
        (100000003, "漸減", "zengen", DRUG_FLAG_SAFE_DECOMPOSE | DRUG_FLAG_HIGH_RISK_ALERT, []),
        (100000004, "中止", "chuusi", DRUG_FLAG_SAFE_DECOMPOSE | DRUG_FLAG_HIGH_RISK_ALERT, []),
        (100000005, "禁忌", "kinki", DRUG_FLAG_SAFE_DECOMPOSE | DRUG_FLAG_HIGH_RISK_ALERT, []),
        (100000006, "増量", "zouryou", DRUG_FLAG_SAFE_DECOMPOSE | DRUG_FLAG_HIGH_RISK_ALERT, []),
        (100000007, "隔日", "kakuzitu", DRUG_FLAG_SAFE_DECOMPOSE | DRUG_FLAG_HIGH_RISK_ALERT, []),
        (100000008, "頓用", "tonyou", DRUG_FLAG_SAFE_DECOMPOSE | DRUG_FLAG_HIGH_RISK_ALERT, []),
        (100000009, "カリウム静注", "kariumu", DRUG_FLAG_SAFE_DECOMPOSE | DRUG_FLAG_HIGH_RISK_ALERT, ["N18.5"]),
        # 代表的な重要医薬品（NSAIDs, 抗凝固薬, 糖尿病薬, 降圧薬, 抗菌薬等）
        (111400101, "ロキソプロフェン", "rokisop", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["K25.0", "N18.5", "J45.9"]),
        (111400102, "ロキソニン", "rokison", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["K25.0", "N18.5"]),
        (111400201, "アセトアミノフェン", "asetoam", DRUG_FLAG_KATAKANA_F7, ["K72.9"]),
        (111400202, "カロナール", "karona-", DRUG_FLAG_KATAKANA_F7, ["K72.9"]),
        (123200101, "ボノプラザン", "bonopur", DRUG_FLAG_KATAKANA_F7, []),
        (123200102, "タケキャブ", "takekya", DRUG_FLAG_KATAKANA_F7, []),
        (121400101, "アムロジピン", "amurozi", DRUG_FLAG_KATAKANA_F7, []),
        (121400201, "カンデサルタン", "kandesa", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_RENAL_CHECK, ["O00.0"]),
        (133300101, "アピキサバン", "apikisa", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["I61.9"]),
        (133300102, "エリキュース", "erikyu-", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["I61.9"]),
        (133300201, "リバーロキサバン", "riba-ro", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["I61.9"]),
        (133300202, "イグザレルト", "iguzare", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["I61.9"]),
        (133300301, "エドキサバン", "edokisa", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["I61.9"]),
        (133300302, "リクシアナ", "rikusia", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["I61.9"]),
        (133300401, "ワルファリン", "warufar", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT, ["I61.9"]),
        (139600101, "メトホルミン", "metohor", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["N18.5", "E10.1"]),
        (139600102, "メトグルコ", "metogur", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["N18.5"]),
        (139600201, "ダパグリフロジン", "dapagur", DRUG_FLAG_KATAKANA_F7, []),
        (139600202, "フォシーガ", "fosi-ga", DRUG_FLAG_KATAKANA_F7, []),
        (139600301, "エンパグリフロジン", "enpagur", DRUG_FLAG_KATAKANA_F7, []),
        (139600302, "ジャディアンス", "zyadia", DRUG_FLAG_KATAKANA_F7, []),
        (161300101, "アモキシシリン", "amokisi", DRUG_FLAG_KATAKANA_F7, []),
        (161300102, "サワシリン", "sawasir", DRUG_FLAG_KATAKANA_F7, []),
        (161300201, "レボフロキサシン", "rebofur", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_RENAL_CHECK, []),
        (161300202, "クラビット", "kurabit", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_RENAL_CHECK, []),
        (161300301, "バンコマイシン", "bankoma", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT | DRUG_FLAG_RENAL_CHECK, ["N18.5"]),
        (161300401, "メロペネム", "meropen", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_RENAL_CHECK, []),
        (161300501, "タゾバクタム・ピペラシリン", "zosinn", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_RENAL_CHECK, []),
        (124500101, "アドレナリン", "adorena", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT, []),
        (124500201, "ノルアドレナリン", "noruado", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_HIGH_RISK_ALERT, []),
        (112100101, "プレガバリン", "puregab", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_RENAL_CHECK, []),
        (112100102, "リリカ", "ririka", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_RENAL_CHECK, []),
        (112100201, "ミロガバリン", "mirogab", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_RENAL_CHECK, []),
        (112100202, "タリージェ", "tari-zy", DRUG_FLAG_KATAKANA_F7 | DRUG_FLAG_RENAL_CHECK, []),
        # F10バイパス必須の医療単位・略語
        (190000001, "SpO2", "SpO2", DRUG_FLAG_F10_BYPASS, []),
        (190000002, "mmHg", "mmHg", DRUG_FLAG_F10_BYPASS, []),
        (190000003, "HbA1c", "HbA1c", DRUG_FLAG_F10_BYPASS, []),
        (190000004, "eGFR", "eGFR", DRUG_FLAG_F10_BYPASS, []),
        (190000005, "NT-proBNP", "NTpro", DRUG_FLAG_F10_BYPASS, []),
    ]

    records = []
    for idx, (hot9, name, romaji, flags, contra) in enumerate(safety_Critical_seeds):
        records.append({
            "hot9Code": hot9,
            "drugId": idx + 1,
            "controlFlags": flags,
            "genericName": name,
            "contraBloom": compute_bloom_16b(contra),
            "safeRomaji": romaji[:7],
        })

    # HOT9昇順ソート（二分探索 O(log N) 用）
    records.sort(key=lambda r: r["hot9Code"])
    for idx, r in enumerate(records):
        r["drugId"] = idx + 1

    STRUCT_FMT = "<IHH32s16s8s"
    assert struct.calcsize(STRUCT_FMT) == 64

    bin_data = bytearray()
    for r in records:
        packed = struct.pack(
            STRUCT_FMT,
            r["hot9Code"] & 0xFFFFFFFF,
            r["drugId"] & 0xFFFF,
            r["controlFlags"] & 0xFFFF,
            pad_utf8(r["genericName"], 32),
            r["contraBloom"],
            pad_utf8(r["safeRomaji"], 8),
        )
        assert len(packed) == 64
        bin_data.extend(packed)

    out_path = os.path.join(out_dir, "drugs.bin")
    with open(out_path, "wb") as f:
        f.write(bin_data)
    print(f"[OK] drugs.bin generated: {len(records)} records ({len(bin_data):,} bytes)")
    return records


def build_aho_corasick_trie_bin(out_dir: str, minds_records: list, drug_records: list):
    """
    薬品名・安全分解キーワード・Minds主要疾患名をすべてUTF-8バイトAho-Corasickオートマトンに構築し、
    16バイト固定長ノード配列 (trie.bin) にシリアライズする。
    """
    class TrieNode:
        def __init__(self, trans_byte=0):
            self.trans_byte = trans_byte
            self.children = {}  # byte -> node_idx
            self.fail = 0
            self.match_drug_idx = 0xFFFF
            self.match_minds_idx = 0xFFFF
            self.safety_flags = 0
            self.pattern_len = 0

    nodes = [TrieNode(0)]  # 0 = Root

    def insert_pattern(utf8_str: str, drug_idx=0xFFFF, minds_idx=0xFFFF, flags=0):
        raw = utf8_str.encode("utf-8")
        if not raw or len(raw) > 255:
            return
        cur = 0
        for b in raw:
            if b not in nodes[cur].children:
                new_idx = len(nodes)
                nodes.append(TrieNode(b))
                nodes[cur].children[b] = new_idx
            cur = nodes[cur].children[b]
        if drug_idx != 0xFFFF:
            nodes[cur].match_drug_idx = drug_idx
        if minds_idx != 0xFFFF:
            nodes[cur].match_minds_idx = minds_idx
        nodes[cur].safety_flags |= flags
        nodes[cur].pattern_len = len(raw)

    # 1. Drug & Safety keywords
    for d_idx, d in enumerate(drug_records):
        insert_pattern(d["genericName"], drug_idx=d_idx, flags=d["controlFlags"])

    # 2. Minds primary diseases & key triggers
    for m_idx, m in enumerate(minds_records):
        if m.get("disease"):
            insert_pattern(m["disease"], minds_idx=m_idx, flags=0)
        for tw in m.get("triggers", [])[:4]:
            if len(tw) >= 2:
                insert_pattern(tw, minds_idx=m_idx, flags=0)

    # BFS順序で子ノードが連続配置されるようにノード番号を再配置（firstChildIdx + childCount 二分探索用）
    old_to_new = {0: 0}
    ordered_old_indices = [0]
    bfs_q = deque([0])

    while bfs_q:
        u_old = bfs_q.popleft()
        sorted_bytes = sorted(nodes[u_old].children.keys())
        for b in sorted_bytes:
            v_old = nodes[u_old].children[b]
            old_to_new[v_old] = len(ordered_old_indices)
            ordered_old_indices.append(v_old)
            bfs_q.append(v_old)

    # 再配置後のツリーでAho-Corasick Failureリンクを計算
    num_nodes = len(ordered_old_indices)
    flat_children = [{} for _ in range(num_nodes)]
    first_child_idx = [0] * num_nodes
    child_count = [0] * num_nodes

    for u_old in ordered_old_indices:
        u_new = old_to_new[u_old]
        sorted_bytes = sorted(nodes[u_old].children.keys())
        child_count[u_new] = len(sorted_bytes)
        if sorted_bytes:
            first_child_idx[u_new] = old_to_new[nodes[u_old].children[sorted_bytes[0]]]
        for b in sorted_bytes:
            v_new = old_to_new[nodes[u_old].children[b]]
            flat_children[u_new][b] = v_new

    fail_idx = [0] * num_nodes
    q = deque()
    for b, v_new in flat_children[0].items():
        fail_idx[v_new] = 0
        q.append(v_new)

    while q:
        u = q.popleft()
        for b, v in flat_children[u].items():
            f = fail_idx[u]
            while f != 0 and b not in flat_children[f]:
                f = fail_idx[f]
            if b in flat_children[f] and flat_children[f][b] != v:
                fail_idx[v] = flat_children[f][b]
            else:
                fail_idx[v] = 0
            q.append(v)

    STRUCT_FMT = "<HBBHHHHB3s"
    assert struct.calcsize(STRUCT_FMT) == 16

    bin_data = bytearray()
    for new_i, old_i in enumerate(ordered_old_indices):
        nd = nodes[old_i]
        packed = struct.pack(
            STRUCT_FMT,
            first_child_idx[new_i] & 0xFFFF,
            child_count[new_i] & 0xFF,
            nd.trans_byte & 0xFF,
            fail_idx[new_i] & 0xFFFF,
            nd.match_drug_idx & 0xFFFF,
            nd.match_minds_idx & 0xFFFF,
            nd.safety_flags & 0xFFFF,
            nd.pattern_len & 0xFF,
            b"\x00\x00\x00",
        )
        assert len(packed) == 16
        bin_data.extend(packed)

    out_path = os.path.join(out_dir, "trie.bin")
    with open(out_path, "wb") as f:
        f.write(bin_data)
    print(f"[OK] trie.bin generated: {num_nodes} nodes ({len(bin_data):,} bytes)")


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    data_dir = os.path.abspath(os.path.join(script_dir, "..", "data"))
    os.makedirs(data_dir, exist_ok=True)

    minds_recs = build_minds_bin(data_dir)
    drug_recs = build_drugs_bin(data_dir)
    build_aho_corasick_trie_bin(data_dir, minds_recs, drug_recs)


if __name__ == "__main__":
    main()
