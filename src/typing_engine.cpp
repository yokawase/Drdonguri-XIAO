#include "typing_engine.h"
#include <string.h>

DonguriTypingEngine::DonguriTypingEngine(USBHIDKeyboard& keyboard)
  : _kbd(keyboard), _layout(KBD_LAYOUT_JIS) {
}

void DonguriTypingEngine::begin() {
  _kbd.begin();
  forceReleaseAll();
}

void DonguriTypingEngine::setKeyboardLayout(KeyboardLayoutMode layout) {
  _layout = layout;
}

void DonguriTypingEngine::forceReleaseAll() {
  KeyReport rep;
  memset(&rep, 0, sizeof(KeyReport));
  _kbd.sendReport(&rep);
  delay(2);
  _kbd.sendReport(&rep); // 二重送出による脱落防止
  delay(TYPING_INTER_REPORT_DELAY_MS);
}

void DonguriTypingEngine::clearAltMenuTrap() {
  KeyReport rep;
  memset(&rep, 0, sizeof(KeyReport));
  rep.modifiers = 0x02; // KEYBOARD_MODIFIER_LEFTSHIFT
  _kbd.sendReport(&rep);
  delay(TYPING_INTER_REPORT_DELAY_MS);

  memset(&rep, 0, sizeof(KeyReport));
  _kbd.sendReport(&rep);
  delay(2);
  _kbd.sendReport(&rep);
  delay(TYPING_INTER_REPORT_DELAY_MS);
}

void DonguriTypingEngine::resetImeState() {
  KeyReport rep;
  for (int i = 0; i < 2; i++) {
    memset(&rep, 0, sizeof(KeyReport));
    rep.keys[0] = KEY_JIS_NONCONVERT;
    _kbd.sendReport(&rep);
    delay(TYPING_INTER_REPORT_DELAY_MS);

    memset(&rep, 0, sizeof(KeyReport));
    _kbd.sendReport(&rep);
    delay(2);
    _kbd.sendReport(&rep);
    delay(TYPING_INTER_REPORT_DELAY_MS);
  }
}

void DonguriTypingEngine::sendKatakanaF7(const char* romaji) {
  if (romaji == nullptr || romaji[0] == '\0') return;

  bool sentAny = false;
  for (size_t i = 0; romaji[i] != '\0'; i++) {
    if (romaji[i] != ' ' && (uint8_t)romaji[i] < 0x80) {
      sendSafeChar(romaji[i]);
      sentAny = true;
    }
  }
  if (!sentAny) return;
  delay(20);

  safeWrite(KEY_F7);
  delay(25);
  safeWrite(KEY_RETURN);
  delay(40);
  forceReleaseAll();
}

static void flushAsciiF10Segment(DonguriTypingEngine* engine, bool& hasNonSpace) {
  if (!hasNonSpace) return;
  delay(20);
  engine->safeWrite(KEY_F10);
  delay(25);
  engine->safeWrite(KEY_RETURN);
  delay(35);
  engine->forceReleaseAll();
  hasNonSpace = false;
}

void DonguriTypingEngine::sendAsciiF10(const char* ascii) {
  if (ascii == nullptr || ascii[0] == '\0') return;

  bool hasNonSpace = false;
  char prevAsciiChar = 0;
  size_t i = 0;
  while (ascii[i] != '\0') {
    uint8_t c0 = (uint8_t)ascii[i];
    // UTF-8 ギリシャ文字 (α, β, γ, δ, μ: 0xCE 0xB1..0xBC)
    if (c0 == 0xCE && ascii[i + 1] != '\0') {
      uint8_t c1 = (uint8_t)ascii[i + 1];
      const char* greekRomaji = nullptr;
      if (c1 == 0xB1) greekRomaji = "aruha";
      else if (c1 == 0xB2) greekRomaji = "be-ta";
      else if (c1 == 0xB3) greekRomaji = "ganma";
      else if (c1 == 0xB4) greekRomaji = "deruta";
      else if (c1 == 0xBC) greekRomaji = "myu-";

      if (greekRomaji != nullptr) {
        flushAsciiF10Segment(this, hasNonSpace);
        prevAsciiChar = 0;
        sendGreekG(greekRomaji);
        i += 2;
        continue;
      }
    }

    if (c0 < 0x80) {
      char ch = ascii[i];
      if (ch == ' ' || ch == '\t') {
        flushAsciiF10Segment(this, hasNonSpace);
        prevAsciiChar = 0;
        sendSafeChar(ch);
        i++;
        continue;
      }

      if (ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == ',') {
        flushAsciiF10Segment(this, hasNonSpace);
        sendSafeChar(ch);
        hasNonSpace = true;
        flushAsciiF10Segment(this, hasNonSpace);
        prevAsciiChar = 0;
        i++;
        continue;
      }

      if ((ch >= 'A' && ch <= 'Z') && (prevAsciiChar >= 'a' && prevAsciiChar <= 'z')) {
        flushAsciiF10Segment(this, hasNonSpace);
      }

      sendSafeChar(ch);
      hasNonSpace = true;
      prevAsciiChar = ch;
    }
    i++;
  }

  flushAsciiF10Segment(this, hasNonSpace);
}

void DonguriTypingEngine::sendKanjiZ(const char* romaji) {
  if (romaji == nullptr || romaji[0] == '\0') return;

  bool sentAny = false;
  for (size_t i = 0; romaji[i] != '\0'; i++) {
    if (romaji[i] != ' ' && (uint8_t)romaji[i] < 0x80) {
      sendSafeChar(romaji[i]);
      sentAny = true;
    }
  }
  if (!sentAny) return;
  delay(20);

  safeWrite(' ');
  delay(35);
  safeWrite(KEY_RETURN);
  delay(40);
  forceReleaseAll();
}

void DonguriTypingEngine::sendSafeDecomposedKanji(const char* romajiSylSeparated) {
  // 危険熟語（休薬・減量・漸減など）のIME誤変換事故を防ぐため、
  // ドットまたはスペース区切りの1文字単位ごとにSpace変換➔Enter確定を実行
  if (romajiSylSeparated == nullptr || romajiSylSeparated[0] == '\0') return;

  char chunk[64] = {0};
  size_t clen = 0;
  for (size_t i = 0; ; i++) {
    char c = romajiSylSeparated[i];
    if (c == '.' || c == ' ' || c == '\0') {
      if (clen > 0) {
        chunk[clen] = '\0';
        sendKanjiZ(chunk);
        delay(25);
        clen = 0;
      }
      if (c == '\0') break;
    } else if ((uint8_t)c < 0x80 && clen < sizeof(chunk) - 1) {
      chunk[clen++] = c;
    }
  }
}

void DonguriTypingEngine::sendHiraganaH(const char* romaji) {
  if (romaji == nullptr || romaji[0] == '\0') return;

  bool sentAny = false;
  size_t i = 0;
  while (romaji[i] != '\0') {
    uint8_t c0 = (uint8_t)romaji[i];
    if (c0 == 0xE3 && romaji[i + 1] != '\0' && romaji[i + 2] != '\0') {
      uint8_t c1 = (uint8_t)romaji[i + 1];
      uint8_t c2 = (uint8_t)romaji[i + 2];
      if (c1 == 0x80 && c2 == 0x81) { sendSafeChar(','); sentAny = true; i += 3; continue; }
      if (c1 == 0x80 && c2 == 0x82) { sendSafeChar('.'); sentAny = true; i += 3; continue; }
      if (c1 == 0x80 && c2 == 0x8C) { sendSafeChar('['); sentAny = true; i += 3; continue; }
      if (c1 == 0x80 && c2 == 0x8D) { sendSafeChar(']'); sentAny = true; i += 3; continue; }
      if (c1 == 0x83 && c2 == 0xBC) { sendSafeChar('-'); sentAny = true; i += 3; continue; }
      if (c1 == 0x83 && c2 == 0xBB) { sendSafeChar('/'); sentAny = true; i += 3; continue; }
    }

    if (c0 == 0xEF && romaji[i + 1] != '\0' && romaji[i + 2] != '\0') {
      uint8_t c1 = (uint8_t)romaji[i + 1];
      uint8_t c2 = (uint8_t)romaji[i + 2];
      if (c1 == 0xBC && c2 == 0x8C) { sendSafeChar(','); sentAny = true; i += 3; continue; }
      if (c1 == 0xBC && c2 == 0x8E) { sendSafeChar('.'); sentAny = true; i += 3; continue; }
    }

    if (c0 < 0x80 && romaji[i] != ' ') {
      sendSafeChar(romaji[i]);
      sentAny = true;
    }
    i++;
  }

  if (!sentAny) return;
  delay(20);
  safeWrite(KEY_RETURN);
  delay(40);
  forceReleaseAll();
}

void DonguriTypingEngine::sendGreekG(const char* romaji) {
  // 【同音異義語 Space 2回確定ロジックの完全廃止】
  // 電カルIME候補変動による誤爆を根絶するため、通常の漢字確定 (Space 1回 ➔ Enter) へ統一
  sendKanjiZ(romaji);
}

void DonguriTypingEngine::safeWrite(uint8_t key) {
  KeyReport rep;
  memset(&rep, 0, sizeof(KeyReport));
  if (key == ' ') {
    rep.keys[0] = 0x2C; // HID Usage: Spacebar
  } else if (key == '\n' || key == '\r') {
    rep.keys[0] = 0x28; // HID Usage: Enter
  } else if (key == '\t') {
    rep.keys[0] = 0x2B; // HID Usage: Tab
  } else if (key == '\b') {
    rep.keys[0] = 0x2A; // HID Usage: Backspace
  } else if (key >= 136) {
    rep.keys[0] = (uint8_t)(key - 136);
  } else {
    rep.keys[0] = key;
  }
  _kbd.sendReport(&rep);
  delay(TYPING_INTER_REPORT_DELAY_MS);

  memset(&rep, 0, sizeof(KeyReport));
  _kbd.sendReport(&rep);
  delay(2);
  _kbd.sendReport(&rep);
  delay(TYPING_INTER_REPORT_DELAY_MS);
}

void DonguriTypingEngine::sendSafeChar(char c) {
  KeyReport rep;
  memset(&rep, 0, sizeof(KeyReport));

  if (c >= 'a' && c <= 'z') {
    rep.keys[0] = (uint8_t)(0x04 + (c - 'a'));
    _kbd.sendReport(&rep);
    delay(TYPING_INTER_REPORT_DELAY_MS);
    forceReleaseAll();
    return;
  }
  if (c >= 'A' && c <= 'Z') {
    rep.modifiers = 0x02; // Left Shift
    rep.keys[0] = (uint8_t)(0x04 + (c - 'A'));
    _kbd.sendReport(&rep);
    delay(TYPING_INTER_REPORT_DELAY_MS);
    forceReleaseAll();
    return;
  }
  if (c >= '1' && c <= '9') {
    rep.keys[0] = (uint8_t)(0x1E + (c - '1'));
    _kbd.sendReport(&rep);
    delay(TYPING_INTER_REPORT_DELAY_MS);
    forceReleaseAll();
    return;
  }
  if (c == '0') {
    rep.keys[0] = 0x27;
    _kbd.sendReport(&rep);
    delay(TYPING_INTER_REPORT_DELAY_MS);
    forceReleaseAll();
    return;
  }

  // 共通制御キー・共通記号
  if (c == ' ')  { rep.keys[0] = 0x2C; _kbd.sendReport(&rep); delay(TYPING_INTER_REPORT_DELAY_MS); forceReleaseAll(); return; }
  if (c == '\t') { rep.keys[0] = 0x2B; _kbd.sendReport(&rep); delay(TYPING_INTER_REPORT_DELAY_MS); forceReleaseAll(); return; }
  if (c == '\n' || c == '\r') { rep.keys[0] = 0x28; _kbd.sendReport(&rep); delay(TYPING_INTER_REPORT_DELAY_MS); forceReleaseAll(); return; }

  // ==========================================================================
  // US (101/104) キーボード配列モード時のマッピング
  // ==========================================================================
  if (_layout == KBD_LAYOUT_US) {
    switch (c) {
      case '-':  rep.keys[0] = 0x2D; break;
      case '=':  rep.keys[0] = 0x2E; break;
      case '[':  rep.keys[0] = 0x2F; break;
      case ']':  rep.keys[0] = 0x30; break;
      case '\\': rep.keys[0] = 0x31; break;
      case ';':  rep.keys[0] = 0x33; break;
      case '\'': rep.keys[0] = 0x34; break;
      case '`':  rep.keys[0] = 0x35; break;
      case ',':  rep.keys[0] = 0x36; break;
      case '.':  rep.keys[0] = 0x37; break;
      case '/':  rep.keys[0] = 0x38; break;
      // US Shift + キー
      case '!':  rep.modifiers = 0x02; rep.keys[0] = 0x1E; break; // Shift + 1
      case '@':  rep.modifiers = 0x02; rep.keys[0] = 0x1F; break; // Shift + 2
      case '#':  rep.modifiers = 0x02; rep.keys[0] = 0x20; break; // Shift + 3
      case '$':  rep.modifiers = 0x02; rep.keys[0] = 0x21; break; // Shift + 4
      case '%':  rep.modifiers = 0x02; rep.keys[0] = 0x22; break; // Shift + 5
      case '^':  rep.modifiers = 0x02; rep.keys[0] = 0x23; break; // Shift + 6
      case '&':  rep.modifiers = 0x02; rep.keys[0] = 0x24; break; // Shift + 7
      case '*':  rep.modifiers = 0x02; rep.keys[0] = 0x25; break; // Shift + 8
      case '(':  rep.modifiers = 0x02; rep.keys[0] = 0x26; break; // Shift + 9
      case ')':  rep.modifiers = 0x02; rep.keys[0] = 0x27; break; // Shift + 0
      case '_':  rep.modifiers = 0x02; rep.keys[0] = 0x2D; break; // Shift + -
      case '+':  rep.modifiers = 0x02; rep.keys[0] = 0x2E; break; // Shift + =
      case '{':  rep.modifiers = 0x02; rep.keys[0] = 0x2F; break; // Shift + [
      case '}':  rep.modifiers = 0x02; rep.keys[0] = 0x30; break; // Shift + ]
      case '|':  rep.modifiers = 0x02; rep.keys[0] = 0x31; break; // Shift + \
      case ':':  rep.modifiers = 0x02; rep.keys[0] = 0x33; break; // Shift + ;
      case '"':  rep.modifiers = 0x02; rep.keys[0] = 0x34; break; // Shift + '
      case '~':  rep.modifiers = 0x02; rep.keys[0] = 0x35; break; // Shift + `
      case '<':  rep.modifiers = 0x02; rep.keys[0] = 0x36; break; // Shift + ,
      case '>':  rep.modifiers = 0x02; rep.keys[0] = 0x37; break; // Shift + .
      case '?':  rep.modifiers = 0x02; rep.keys[0] = 0x38; break; // Shift + /
      default: return;
    }
    _kbd.sendReport(&rep);
    delay(TYPING_INTER_REPORT_DELAY_MS);
    forceReleaseAll();
    return;
  }

  // ==========================================================================
  // JIS (106/109) キーボード配列モード時のマッピング（デフォルト）
  // ==========================================================================
  switch (c) {
    case '-':  rep.keys[0] = 0x2D; break;
    case '^':  rep.keys[0] = 0x2E; break;
    case '@':  rep.keys[0] = 0x2F; break;
    case '[':  rep.keys[0] = 0x30; break;
    case ']':  rep.keys[0] = 0x31; break;
    case ';':  rep.keys[0] = 0x33; break;
    case ':':  rep.keys[0] = 0x34; break;
    case ',':  rep.keys[0] = 0x36; break;
    case '.':  rep.keys[0] = 0x37; break;
    case '/':  rep.keys[0] = 0x38; break;
    case '\\': rep.keys[0] = 0x89; break; // JIS Int3 (￥)
    // JIS Shift + キー
    case '!':  rep.modifiers = 0x02; rep.keys[0] = 0x1E; break;
    case '"':  rep.modifiers = 0x02; rep.keys[0] = 0x1F; break;
    case '#':  rep.modifiers = 0x02; rep.keys[0] = 0x20; break;
    case '$':  rep.modifiers = 0x02; rep.keys[0] = 0x21; break;
    case '%':  rep.modifiers = 0x02; rep.keys[0] = 0x22; break;
    case '&':  rep.modifiers = 0x02; rep.keys[0] = 0x23; break;
    case '\'': rep.modifiers = 0x02; rep.keys[0] = 0x24; break;
    case '(':  rep.modifiers = 0x02; rep.keys[0] = 0x25; break;
    case ')':  rep.modifiers = 0x02; rep.keys[0] = 0x26; break;
    case '=':  rep.modifiers = 0x02; rep.keys[0] = 0x2D; break;
    case '~':  rep.modifiers = 0x02; rep.keys[0] = 0x2E; break;
    case '`':  rep.modifiers = 0x02; rep.keys[0] = 0x2F; break;
    case '{':  rep.modifiers = 0x02; rep.keys[0] = 0x30; break;
    case '}':  rep.modifiers = 0x02; rep.keys[0] = 0x31; break;
    case '+':  rep.modifiers = 0x02; rep.keys[0] = 0x33; break;
    case '*':  rep.modifiers = 0x02; rep.keys[0] = 0x34; break;
    case '<':  rep.modifiers = 0x02; rep.keys[0] = 0x36; break;
    case '>':  rep.modifiers = 0x02; rep.keys[0] = 0x37; break;
    case '?':  rep.modifiers = 0x02; rep.keys[0] = 0x38; break;
    case '_':  rep.modifiers = 0x02; rep.keys[0] = 0x87; break; // JIS Shift + Int1 (ろ)
    case '|':  rep.modifiers = 0x02; rep.keys[0] = 0x89; break; // JIS Shift + Int3 (￥)
    default: return;
  }

  _kbd.sendReport(&rep);
  delay(TYPING_INTER_REPORT_DELAY_MS);
  forceReleaseAll();
}

void DonguriTypingEngine::dispatchStream(const char* stream, size_t length, TypingProgressCallback progressCb) {
  if (stream == nullptr || length == 0) return;

  size_t i = 0;
  while (i < length) {
    if (progressCb != nullptr) {
      progressCb((int)i, (int)length);
    }

    if (strncmp(&stream[i], "[IME_RESET]", 11) == 0 || strncmp(&stream[i], "<IME_RESET>", 11) == 0) {
      resetImeState();
      i += 11;
      continue;
    }

    // [SAFE]...[/SAFE] 危険熟語・安全分解打鍵タグ
    if (strncmp(&stream[i], "[SAFE]", 6) == 0 || strncmp(&stream[i], "<SAFE>", 6) == 0) {
      i += 6;
      char romaji[128] = {0};
      size_t rlen = 0;
      while (i < length) {
        if (strncmp(&stream[i], "[/SAFE]", 7) == 0 || strncmp(&stream[i], "</SAFE>", 7) == 0) {
          i += 7;
          break;
        }
        if (stream[i] == '[' || stream[i] == '<' || stream[i] == '\n' || stream[i] == '\r') {
          break;
        }
        if (rlen < sizeof(romaji) - 1) {
          romaji[rlen++] = stream[i];
        }
        i++;
      }
      romaji[rlen] = '\0';
      if (rlen > 0) {
        sendSafeDecomposedKanji(romaji);
      }
      continue;
    }

    if (strncmp(&stream[i], "[Z]", 3) == 0 || strncmp(&stream[i], "<Z>", 3) == 0) {
      i += 3;
      char romaji[128] = {0};
      size_t rlen = 0;
      while (i < length) {
        if (strncmp(&stream[i], "[/Z]", 4) == 0 || strncmp(&stream[i], "</Z>", 4) == 0) {
          i += 4;
          break;
        }
        if (stream[i] == '[' || stream[i] == '<' || stream[i] == '\n' || stream[i] == '\r') {
          break;
        }
        if (rlen < sizeof(romaji) - 1) {
          romaji[rlen++] = stream[i];
        }
        i++;
      }
      romaji[rlen] = '\0';
      if (rlen > 0) {
        sendKanjiZ(romaji);
      }
      continue;
    }

    if (strncmp(&stream[i], "[K]", 3) == 0 || strncmp(&stream[i], "<K>", 3) == 0) {
      i += 3;
      char romaji[128] = {0};
      size_t rlen = 0;
      while (i < length) {
        if (strncmp(&stream[i], "[/K]", 4) == 0 || strncmp(&stream[i], "</K>", 4) == 0) {
          i += 4;
          break;
        }
        if (stream[i] == '[' || stream[i] == '<' || stream[i] == '\n' || stream[i] == '\r') {
          break;
        }
        if (rlen < sizeof(romaji) - 1) {
          romaji[rlen++] = stream[i];
        }
        i++;
      }
      romaji[rlen] = '\0';
      if (rlen > 0) {
        sendKatakanaF7(romaji);
      }
      continue;
    }

    if (strncmp(&stream[i], "[H]", 3) == 0 || strncmp(&stream[i], "<H>", 3) == 0) {
      i += 3;
      char romaji[128] = {0};
      size_t rlen = 0;
      while (i < length) {
        if (strncmp(&stream[i], "[/H]", 4) == 0 || strncmp(&stream[i], "</H>", 4) == 0) {
          i += 4;
          break;
        }
        if (stream[i] == '[' || stream[i] == '<' || stream[i] == '\n' || stream[i] == '\r') {
          break;
        }
        if (rlen < sizeof(romaji) - 1) {
          romaji[rlen++] = stream[i];
        }
        i++;
      }
      romaji[rlen] = '\0';
      if (rlen > 0) {
        sendHiraganaH(romaji);
        if ((strcmp(romaji, "、") == 0 || strcmp(romaji, "。") == 0) && i < length && stream[i] == ' ') {
          i++;
        }
      }
      continue;
    }

    if (strncmp(&stream[i], "[G]", 3) == 0 || strncmp(&stream[i], "<G>", 3) == 0) {
      i += 3;
      char romaji[64] = {0};
      size_t rlen = 0;
      while (i < length) {
        if (strncmp(&stream[i], "[/G]", 4) == 0 || strncmp(&stream[i], "</G>", 4) == 0) {
          i += 4;
          break;
        }
        if (stream[i] == '[' || stream[i] == '<' || stream[i] == '\n' || stream[i] == '\r') {
          break;
        }
        if (rlen < sizeof(romaji) - 1) {
          romaji[rlen++] = stream[i];
        }
        i++;
      }
      romaji[rlen] = '\0';
      if (rlen > 0) {
        sendKanjiZ(romaji);
      }
      continue;
    }

    if (strncmp(&stream[i], "[ALT]", 5) == 0 || strncmp(&stream[i], "<ALT>", 5) == 0) {
      i += 5;
      while (i < length) {
        if (strncmp(&stream[i], "[/ALT]", 6) == 0 || strncmp(&stream[i], "</ALT>", 6) == 0) {
          i += 6;
          break;
        }
        if (stream[i] == '[' || stream[i] == '<' || stream[i] == '\n' || stream[i] == '\r') {
          break;
        }
        i++;
      }
      continue;
    }

    if (strncmp(&stream[i], "[F7]", 4) == 0 || strncmp(&stream[i], "<F7>", 4) == 0) {
      i += 4;
      char romaji[64] = {0};
      size_t rlen = 0;
      while (i < length) {
        if (strncmp(&stream[i], "[/F7]", 5) == 0 || strncmp(&stream[i], "</F7>", 5) == 0) {
          i += 5;
          break;
        }
        if (stream[i] == '[' || stream[i] == '<' || stream[i] == '\n' || stream[i] == '\r') {
          break;
        }
        if (rlen < sizeof(romaji) - 1) {
          romaji[rlen++] = stream[i];
        }
        i++;
      }
      romaji[rlen] = '\0';
      if (rlen > 0) {
        sendKatakanaF7(romaji);
      }
      continue;
    }

    if (strncmp(&stream[i], "[A]", 3) == 0 || strncmp(&stream[i], "<A>", 3) == 0 ||
        strncmp(&stream[i], "[F10]", 5) == 0 || strncmp(&stream[i], "<F10>", 5) == 0) {
      size_t openLen = (stream[i+1] == 'F' || stream[i+1] == 'f') ? 5 : 3;
      i += openLen;
      char ascii[128] = {0};
      size_t alen = 0;
      while (i < length) {
        if (strncmp(&stream[i], "[/A]", 4) == 0 || strncmp(&stream[i], "</A>", 4) == 0) {
          i += 4;
          break;
        }
        if (strncmp(&stream[i], "[/F10]", 6) == 0 || strncmp(&stream[i], "</F10>", 6) == 0) {
          i += 6;
          break;
        }
        if (stream[i] == '[' || stream[i] == '<' || stream[i] == '\n' || stream[i] == '\r') {
          break;
        }
        if (alen < sizeof(ascii) - 1) {
          ascii[alen++] = stream[i];
        }
        i++;
      }
      ascii[alen] = '\0';
      if (alen > 0) {
        sendAsciiF10(ascii);
      }
      continue;
    }

    if (strncmp(&stream[i], "[RAW]", 5) == 0 || strncmp(&stream[i], "<RAW>", 5) == 0) {
      i += 5;
      while (i < length) {
        if (strncmp(&stream[i], "[/RAW]", 6) == 0 || strncmp(&stream[i], "</RAW>", 6) == 0) {
          i += 6; break;
        }
        if (stream[i] == '[' || stream[i] == '<' || stream[i] == '\n' || stream[i] == '\r') {
          break;
        }
        sendSafeChar(stream[i]);
        i++;
      }
      continue;
    }

    if (stream[i] == '<' || stream[i] == '[') {
      if (strncmp(&stream[i], "<ENTER>", 7) == 0 || strncmp(&stream[i], "[ENTER]", 7) == 0) {
        safeWrite(KEY_RETURN); i += 7; continue;
      }
      if (strncmp(&stream[i], "<ALT_ENTER>", 11) == 0 || strncmp(&stream[i], "[ALT_ENTER]", 11) == 0) {
        KeyReport rep;
        memset(&rep, 0, sizeof(KeyReport));
        rep.modifiers = 0x04;
        rep.keys[0] = 0x28;
        _kbd.sendReport(&rep);
        delay(TYPING_INTER_REPORT_DELAY_MS);
        forceReleaseAll();
        clearAltMenuTrap();
        i += 11;
        continue;
      }
      if (strncmp(&stream[i], "<CTRL_ENTER>", 12) == 0 || strncmp(&stream[i], "[CTRL_ENTER]", 12) == 0) {
        KeyReport rep;
        memset(&rep, 0, sizeof(KeyReport));
        rep.modifiers = 0x01;
        rep.keys[0] = 0x28;
        _kbd.sendReport(&rep);
        delay(TYPING_INTER_REPORT_DELAY_MS);
        forceReleaseAll();
        i += 12;
        continue;
      }
      if (strncmp(&stream[i], "<TAB>", 5) == 0 || strncmp(&stream[i], "[TAB]", 5) == 0) {
        safeWrite(KEY_TAB); i += 5; continue;
      }
      if (strncmp(&stream[i], "<BS>", 4) == 0 || strncmp(&stream[i], "[BS]", 4) == 0) {
        safeWrite(KEY_BACKSPACE); i += 4; continue;
      }
      if (strncmp(&stream[i], "<DEL>", 5) == 0 || strncmp(&stream[i], "[DEL]", 5) == 0) {
        safeWrite(KEY_DELETE); i += 5; continue;
      }
      if (strncmp(&stream[i], "<LEFT>", 6) == 0 || strncmp(&stream[i], "[LEFT]", 6) == 0) {
        safeWrite(KEY_LEFT_ARROW); i += 6; continue;
      }
      if (strncmp(&stream[i], "<RIGHT>", 7) == 0 || strncmp(&stream[i], "[RIGHT]", 7) == 0) {
        safeWrite(KEY_RIGHT_ARROW); i += 7; continue;
      }
      if (strncmp(&stream[i], "<ESC>", 5) == 0 || strncmp(&stream[i], "[ESC]", 5) == 0) {
        safeWrite(KEY_ESC); i += 5; continue;
      }
    }

    if ((uint8_t)stream[i] == 0xEF && i + 2 < length) {
      uint8_t b1 = (uint8_t)stream[i + 1];
      uint8_t b2 = (uint8_t)stream[i + 2];
      if (b1 == 0xBC && b2 == 0x8C) { sendSafeChar(','); i += 3; continue; }
      if (b1 == 0xBC && b2 == 0x8E) { sendSafeChar('.'); i += 3; continue; }
      if (b1 == 0xBC && b2 == 0x9C) { sendSafeChar('<'); i += 3; continue; }
      if (b1 == 0xBC && b2 == 0x9E) { sendSafeChar('>'); i += 3; continue; }
      if (b1 == 0xBD && b2 == 0x9E) { sendSafeChar('~'); i += 3; continue; }
    }

    if ((uint8_t)stream[i] >= 0x20 && (uint8_t)stream[i] <= 0x7E) {
      sendSafeChar(stream[i]);
    } else if (stream[i] == '\n' || stream[i] == '\r') {
      safeWrite(KEY_RETURN);
    }
    i++;
  }

  forceReleaseAll();
  clearAltMenuTrap();
  if (progressCb != nullptr) {
    progressCb((int)length, (int)length);
  }
}
