#include "ui/Text.h"

namespace {

// ASCII stand-ins for U+00C0..U+00FF.
const char *const LATIN1[64] = {
    "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
    "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
    "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y",
};

const char *asciiFor(uint32_t cp) {
  if (cp >= 0xC0 && cp <= 0xFF) return LATIN1[cp - 0xC0];
  switch (cp) {
    case 0x00A0: return " ";
    case 0x2018: case 0x2019: case 0x00B4: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2010: case 0x2013: case 0x2014: return "-";
    case 0x2026: return "...";
    case 0x0152: return "OE";
    case 0x0153: return "oe";
    case 0x0160: return "S";
    case 0x0161: return "s";
    case 0x017D: return "Z";
    case 0x017E: return "z";
    case 0x0141: return "L";
    case 0x0142: return "l";
    default: return "?";
  }
}

}  // namespace

String toDisplayText(const String &utf8) {
  String out;
  out.reserve(utf8.length());
  const uint8_t *s = reinterpret_cast<const uint8_t *>(utf8.c_str());
  size_t n = utf8.length();

  for (size_t i = 0; i < n;) {
    uint8_t c = s[i];
    if (c < 0x80) {
      out += char(c);
      i++;
      continue;
    }
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    bool valid = extra > 0 && i + extra < n;
    for (int k = 1; valid && k <= extra; k++) valid = (s[i + k] & 0xC0) == 0x80;
    if (!valid) {  // stray or truncated byte
      out += '?';
      i++;
      continue;
    }
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 1; k <= extra; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
    out += asciiFor(cp);
    i += extra + 1;
  }
  return out;
}

String fitText(TFT_eSPI &tft, const String &text, uint8_t font, int16_t maxWidth) {
  if (tft.textWidth(text, font) <= maxWidth) return text;
  String s = text;
  while (s.length() > 0 && tft.textWidth(s + "...", font) > maxWidth) {
    s.remove(s.length() - 1);
  }
  s.trim();
  return s + "...";
}

int wrapText(TFT_eSPI &tft, const String &text, uint8_t font, int16_t maxWidth, String lines[],
             int maxLines) {
  String rest = text;
  rest.trim();
  int count = 0;
  while (rest.length() && count < maxLines) {
    if (count == maxLines - 1 || tft.textWidth(rest, font) <= maxWidth) {
      lines[count++] = fitText(tft, rest, font, maxWidth);
      break;
    }
    // Longest prefix that fits, then back up to the last space in it.
    unsigned int fit = 1;
    while (fit < rest.length() && tft.textWidth(rest.substring(0, fit + 1), font) <= maxWidth) fit++;
    int space = rest.substring(0, fit + 1).lastIndexOf(' ');
    unsigned int cut = space > 0 ? space : fit;
    lines[count] = rest.substring(0, cut);
    lines[count++].trim();
    rest = rest.substring(cut);
    rest.trim();
  }
  return count;
}

String formatTime(uint32_t ms) {
  uint32_t total = ms / 1000;
  char buf[12];
  snprintf(buf, sizeof(buf), "%lu:%02lu", (unsigned long)(total / 60), (unsigned long)(total % 60));
  return buf;
}
