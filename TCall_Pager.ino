#include <Arduino.h>
#include <LittleFS.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include "modem.h"

// LilyGO T-Call v1.3: GPIO assignments must match the connected relay board.
constexpr char VERSION[] = "1.1.0-rc.1";
constexpr int MODEM_RX = 26, MODEM_TX = 27, MODEM_POWER = 23, MODEM_KEY = 4;
constexpr uint32_t MAX_MINUTES = 10080;
constexpr size_t MAX_USB_LINE = 4096;
constexpr char SETTINGS[] = "/settings.txt";
constexpr char SMS_LOG[] = "/sms_log.txt";
constexpr char CALL_LOG[] = "/calls_log.txt";

struct Output {
  uint8_t pin;
  uint8_t mode = 0;  // 0: OFF, 1: constant ON, 2: timer/cycle.
  uint32_t work = 0, pause = 0, started = 0;
  bool working = false;
};
Output outputs[] = {{12}, {14}, {15}, {2}, {21}, {22}, {25}};
HardwareSerial modemSerial(1);
AsyncModem modem(modemSerial);
bool storageReady = false, securityReady = false, modemReady = false;
String adminNumber, passwordSalt, passwordHash;
String signalLevel = "unknown", operatorName = "unknown", networkTime = "unknown";
uint8_t registration = 0, authFailures = 0;
uint32_t authLockStarted = 0;
bool authLocked = false;
String usbLine;
bool discardUsbLine = false;

class UsbWriter {
 public:
  bool available() const { return count_ < 7; }
  void enqueue(const String& frame) {
    if (count_ == 8 || frame.length() > MAX_USB_LINE) return;
    frames_[(head_ + count_) % 8] = frame + "\n";
    ++count_;
  }
  void tick() {
    if (!count_) return;
    int capacity = Serial.availableForWrite();
    if (capacity <= 0) return;
    size_t length = frames_[head_].length() - offset_;
    if (length > size_t(capacity)) length = size_t(capacity);
    if (length > 256) length = 256;
    offset_ += Serial.write(reinterpret_cast<const uint8_t*>(frames_[head_].c_str()) + offset_, length);
    if (offset_ == frames_[head_].length()) {
      frames_[head_] = ""; offset_ = 0; head_ = (head_ + 1) % 8; --count_;
    }
  }
 private:
  String frames_[8];
  uint8_t head_ = 0, count_ = 0;
  size_t offset_ = 0;
};
UsbWriter usbWriter;

String boundedUtf8(const String& text, size_t limit) {
  if (text.length() <= limit) return text;
  while (limit && (uint8_t(text[limit]) & 0xC0) == 0x80) --limit;
  return text.substring(0, limit);
}

String hexEncode(const String& text) {
  constexpr char hex[] = "0123456789ABCDEF";
  String result;
  result.reserve(text.length() * 2);
  for (size_t i = 0; i < text.length(); ++i) {
    uint8_t c = uint8_t(text[i]);
    result += hex[c >> 4];
    result += hex[c & 15];
  }
  return result;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool hexDecode(const String& encoded, String& decoded) {
  if (encoded.length() % 2 || encoded.length() > 1024) return false;
  decoded = "";
  for (size_t i = 0; i < encoded.length(); i += 2) {
    int high = hexDigit(encoded[i]), low = hexDigit(encoded[i + 1]);
    if (high < 0 || low < 0 || (high == 0 && low == 0)) return false;
    decoded += char((high << 4) | low);
  }
  return true;
}

void reply(uint32_t id, bool ok, const String& payload) {
  usbWriter.enqueue("TCALL/1\t" + String(id) + (ok ? "\tOK\t" : "\tERR\t") + hexEncode(boundedUtf8(payload, 1800)));
}

void event(const String& message) {
  usbWriter.enqueue("TCALL/1\t0\tEVENT\t" + hexEncode(boundedUtf8(message, 1800)));
}

bool parseUnsigned(const String& text, uint32_t maximum, uint32_t& value) {
  if (!text.length() || text.length() > 10) return false;
  uint64_t parsed = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') return false;
    parsed = parsed * 10 + text[i] - '0';
    if (parsed > maximum) return false;
  }
  value = uint32_t(parsed);
  return true;
}

bool validPhone(const String& text) {
  size_t start = text.startsWith("+") ? 1 : 0;
  if (text.length() - start < 10 || text.length() - start > 15) return false;
  for (size_t i = start; i < text.length(); ++i) if (text[i] < '0' || text[i] > '9') return false;
  return true;
}

bool samePhone(String left, String right) {
  if (!validPhone(left) || !validPhone(right)) return false;
  if (left.startsWith("+")) left.remove(0, 1);
  if (right.startsWith("+")) right.remove(0, 1);
  return left == right;
}

// Support BMP text explicitly instead of silently dropping unsupported emoji.
bool utf8ToUcs2(const String& text, String& encoded, size_t& characters) {
  encoded = "";
  characters = 0;
  for (size_t i = 0; i < text.length();) {
    uint8_t first = text[i++];
    uint32_t code = first;
    unsigned extra = 0;
    if (first < 128) {}
    else if ((first & 0xE0) == 0xC0) { code = first & 31; extra = 1; }
    else if ((first & 0xF0) == 0xE0) { code = first & 15; extra = 2; }
    else return false;
    if (i + extra > text.length()) return false;
    for (unsigned n = 0; n < extra; ++n) {
      uint8_t next = text[i++];
      if ((next & 0xC0) != 0x80) return false;
      code = (code << 6) | (next & 63);
    }
    if ((extra == 1 && code < 128) || (extra == 2 && code < 2048) ||
        (code >= 0xD800 && code <= 0xDFFF) || code == 0) return false;
    char hex[5];
    snprintf(hex, sizeof(hex), "%04X", unsigned(code));
    encoded += hex;
    ++characters;
  }
  return true;
}

bool ucs2ToUtf8(const String& encoded, String& text) {
  if (encoded.length() % 4 || encoded.length() > 1120) return false;
  text = "";
  for (size_t i = 0; i < encoded.length(); i += 4) {
    uint16_t code = 0;
    for (unsigned n = 0; n < 4; ++n) {
      int digit = hexDigit(encoded[i + n]);
      if (digit < 0) return false;
      code = (code << 4) | digit;
    }
    if (code == 0 || (code >= 0xD800 && code <= 0xDFFF)) return false;
    if (code < 128) text += char(code);
    else if (code < 2048) {
      text += char(0xC0 | (code >> 6)); text += char(0x80 | (code & 63));
    } else {
      text += char(0xE0 | (code >> 12)); text += char(0x80 | ((code >> 6) & 63)); text += char(0x80 | (code & 63));
    }
  }
  return true;
}

String readLineFile(const char* path) {
  File file = LittleFS.open(path, "r");
  if (!file) return "";
  String value = file.readStringUntil('\n');
  value.trim();
  return value;
}

bool atomicWrite(const char* path, const String& content) {
  if (!storageReady) return false;
  String temporary = String(path) + ".tmp";
  File file = LittleFS.open(temporary, "w");
  if (!file) return false;
  bool ok = file.print(content) == content.length();
  file.flush();
  ok = ok && !file.getWriteError();
  file.close();
  return ok && LittleFS.rename(temporary, path);
}

String chipMac() {
  uint8_t bytes[6];
  esp_read_mac(bytes, ESP_MAC_WIFI_STA);
  char formatted[18];
  snprintf(formatted, sizeof(formatted), "%02X:%02X:%02X:%02X:%02X:%02X",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  return String(formatted);
}

String newSalt() {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  String result;
  for (uint8_t value : bytes) {
    char hex[3]; snprintf(hex, sizeof(hex), "%02x", value); result += hex;
  }
  return result;
}

String hashPassword(const String& password, const String& salt, bool legacy) {
  unsigned char digest[32];
  String material = salt + chipMac();
  int error;
  if (legacy) {
    String input = password + material;
    error = mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                       reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), digest);
  } else {
    error = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
        reinterpret_cast<const unsigned char*>(password.c_str()), password.length(),
        reinterpret_cast<const unsigned char*>(material.c_str()), material.length(), 20000, sizeof(digest), digest);
  }
  if (error) return "";
  String result = legacy ? "" : "pbkdf2:";
  for (uint8_t value : digest) {
    char hex[3]; snprintf(hex, sizeof(hex), "%02x", value); result += hex;
  }
  return result;
}

bool saveSettings(const String& number, const String& salt, const String& hash) {
  return atomicWrite(SETTINGS, "TCALL2\n" + number + "\n" + salt + "\n" + hash + "\n");
}

bool authenticate(const String& password) {
  if (authLocked && uint32_t(millis() - authLockStarted) >= 30000) authLocked = false;
  if (!securityReady || !password.length() || password.length() > 256 || authLocked) return false;
  String expected = hashPassword(password, passwordSalt, !passwordHash.startsWith("pbkdf2:"));
  if (!expected.length() || expected.length() != passwordHash.length()) return false;
  uint8_t difference = 0;
  for (size_t i = 0; i < expected.length(); ++i) difference |= uint8_t(expected[i] ^ passwordHash[i]);
  if (difference) {
    if (++authFailures >= 5) { authLockStarted = millis(); authLocked = true; authFailures = 0; }
    return false;
  }
  authFailures = 0;
  return true;
}

bool saveOutputs() {
  String content;
  // A reboot cancels timers; only explicit constant-ON settings are restored.
  for (const auto& output : outputs) content += String(output.mode == 1 ? 1 : 0) + "\n";
  return atomicWrite("/pins_state.txt", content);
}

void loadState() {
  if (!storageReady) return;
  if (LittleFS.exists(SETTINGS)) {
    File file = LittleFS.open(SETTINGS, "r");
    if (!file || file.readStringUntil('\n') != "TCALL2") return;
    adminNumber = file.readStringUntil('\n'); adminNumber.trim();
    passwordSalt = file.readStringUntil('\n'); passwordSalt.trim();
    passwordHash = file.readStringUntil('\n'); passwordHash.trim();
  } else if (LittleFS.exists("/admin.txt") || LittleFS.exists("/salt.txt") || LittleFS.exists("/hash.txt")) {
    // Import the original credential files without resetting a partial or damaged installation.
    adminNumber = readLineFile("/admin.txt");
    passwordSalt = readLineFile("/salt.txt");
    passwordHash = readLineFile("/hash.txt");
  } else {
    adminNumber = "+70000000000";
    passwordSalt = newSalt();
    passwordHash = hashPassword("000000", passwordSalt, false);
    if (!passwordHash.length() || !saveSettings(adminNumber, passwordSalt, passwordHash)) return;
  }
  securityReady = validPhone(adminNumber) && passwordSalt.length() >= 16 &&
      (passwordHash.length() == 64 || (passwordHash.startsWith("pbkdf2:") && passwordHash.length() == 71));
  if (!securityReady) return;
  File file = LittleFS.open("/pins_state.txt", "r");
  if (!file) return;
  for (auto& output : outputs) {
    String state = file.readStringUntil('\n'); state.trim();
    output.mode = state == "1" ? 1 : 0;
    digitalWrite(output.pin, output.mode == 1 ? HIGH : LOW);
  }
}

String logSafe(String text) {
  text.replace("\r", " "); text.replace("\n", " "); text.replace("\t", " ");
  if (text.length() > 150) {
    size_t end = 150;
    while (end && (uint8_t(text[end]) & 0xC0) == 0x80) --end;
    text = text.substring(0, end);
  }
  return text;
}

bool appendLog(const char* path, const String& text) {
  if (!storageReady) return false;
  File file = LittleFS.open(path, "r");
  String lines[50];
  size_t count = 0;
  while (file && file.available()) {
    String line = file.readStringUntil('\n'); line.trim();
    if (!line.length()) continue;
    if (count == 50) {
      for (size_t i = 1; i < 50; ++i) lines[i - 1] = lines[i];
      --count;
    }
    lines[count++] = logSafe(line);
  }
  file.close();
  String content;
  size_t first = count == 50 ? 1 : 0;
  for (size_t i = first; i < count; ++i) content += lines[i] + "\n";
  content += logSafe(text) + "\n";
  return atomicWrite(path, content);
}

String recentLog(const char* path) {
  File file = LittleFS.open(path, "r");
  String lines[10]; size_t total = 0;
  while (file && file.available()) {
    String line = file.readStringUntil('\n'); line.trim();
    if (line.length()) lines[total++ % 10] = logSafe(line);
  }
  String content = "Latest 10 entries (maximum 50 retained):\n";
  size_t start = total > 10 ? total - 10 : 0;
  for (size_t i = start; i < total; ++i) content += lines[i % 10] + "\n";
  return content;
}

void updateOutputs() {
  uint32_t now = millis();
  for (auto& output : outputs) {
    if (output.mode != 2) continue;
    uint32_t duration = output.working ? output.work : output.pause;
    if (uint32_t(now - output.started) < duration) continue;
    output.started = now;
    output.working = !output.working;
    if (!output.working && output.pause == 0) output.mode = 0;
    digitalWrite(output.pin, output.working ? HIGH : LOW);
  }
}

bool configureOutput(const String& pinText, const String& mode, const String& workText,
                     const String& pauseText, String& error) {
  uint32_t pin, work, pause;
  if (!storageReady || !securityReady) { error = "Storage or credentials unavailable."; return false; }
  if (!parseUnsigned(pinText, 39, pin) || !parseUnsigned(workText, MAX_MINUTES, work) ||
      !parseUnsigned(pauseText, MAX_MINUTES, pause)) { error = "Invalid GPIO or duration."; return false; }
  if (mode != "OFF" && mode != "ON" && mode != "TIMER" && mode != "CYCLE") {
    error = "Unknown output mode."; return false;
  }
  if (((mode == "TIMER" || mode == "CYCLE") && !work) || (mode == "CYCLE" && !pause) ||
      ((mode == "OFF" || mode == "ON") && (work || pause)) || (mode == "TIMER" && pause)) {
    error = "Durations do not match the selected mode."; return false;
  }
  for (auto& output : outputs) if (output.pin == pin) {
    if (mode == "OFF") {
      output.mode = 0; output.working = false;
      digitalWrite(output.pin, LOW);
      if (!saveOutputs()) { error = "Output is OFF, but its state could not be saved."; return false; }
      return true;
    }
    Output previous = output;
    output.mode = mode == "OFF" ? 0 : mode == "ON" ? 1 : 2;
    output.work = work * 60000UL; output.pause = pause * 60000UL;
    output.started = millis(); output.working = output.mode != 0;
    if (!saveOutputs()) { output = previous; error = "Failed to persist output state."; return false; }
    digitalWrite(output.pin, output.mode ? HIGH : LOW);
    return true;
  }
  error = "GPIO is not an output channel.";
  return false;
}

String statusText() {
  String status = "Firmware: " + String(VERSION) + "\nStorage: " + (storageReady ? "ready" : "unavailable") +
      "\nCredentials: " + (securityReady ? "ready" : "locked") + "\nModem: " + (modemReady ? "ready" : "initializing") +
      "\nNetwork registration: " + String(registration) + "\nSignal: " + signalLevel +
      "\nOperator: " + operatorName + "\nNetwork time: " + networkTime + "\n";
  for (const auto& output : outputs) {
    uint32_t duration = output.working ? output.work : output.pause;
    uint32_t elapsed = millis() - output.started;
    uint32_t remaining = duration > elapsed ? (duration - elapsed) / 1000 : 0;
    status += "GPIO " + String(output.pin) + ": " + (digitalRead(output.pin) ? "ON" : "OFF") +
        ", mode=" + String(output.mode) + ", remaining=" + String(output.mode == 2 ? remaining : 0) + "s\n";
  }
  return status;
}

bool queueSms(const String& number, const String& text, AsyncModem::Callback done) {
  if (!validPhone(number) || !modemReady || (registration != 1 && registration != 5)) return false;
  String encodedNumber, encodedText; size_t numberLength, textLength;
  if (!utf8ToUcs2(number, encodedNumber, numberLength) || !utf8ToUcs2(text, encodedText, textLength) ||
      !textLength || textLength > 70) return false;
  return modem.enqueue("AT+CMGS=\"" + encodedNumber + "\"", 60000, done, encodedText);
}

// Parse SMS output commands with the same validation as USB requests.
bool applySmsCommand(String text, String& message) {
  text.trim(); text.toUpperCase();
  if (text.startsWith("PIN ")) text.remove(0, 4);
  else if (text.startsWith("#")) text.remove(0, 1);
  else { message = "Expected PIN <number> ON/OFF [work [pause]]."; return false; }
  String fields[4]; size_t count = 0;
  while (text.length() && count < 4) {
    text.trim(); int space = text.indexOf(' ');
    if (space < 0) { fields[count++] = text; text = ""; }
    else { fields[count++] = text.substring(0, space); text.remove(0, space + 1); }
  }
  text.trim();
  if (text.length() || count < 2 || (fields[1] != "ON" && fields[1] != "OFF") ||
      (fields[1] == "OFF" && count != 2)) { message = "Invalid output command."; return false; }
  String mode = fields[1];
  if (count == 3) mode = "TIMER";
  if (count == 4) mode = "CYCLE";
  bool ok = configureOutput(fields[0], mode, count >= 3 ? fields[2] : "0", count == 4 ? fields[3] : "0", message);
  if (ok) message = "GPIO " + fields[0] + " configured: " + mode;
  return ok;
}

uint16_t pendingSms[16];
uint8_t smsHead = 0, smsCount = 0;
bool readingSms = false;
struct SmsFingerprint { uint16_t index = 0; uint32_t hash = 0; };
SmsFingerprint processedSms[64];
uint8_t fingerprintNext = 0;

void deleteProcessedSms(uint16_t index) {
  if (!modem.enqueue("AT+CMGD=" + String(index), 3000, [index](bool deleted, const String&) {
        if (!deleted) event("Processed SMS could not be deleted; it will not be executed again in this run.");
        else for (auto& item : processedSms) if (item.index == index) item.index = 0;
      })) event("SMS delete queue is full.");
}

void scheduleSms(uint16_t index) {
  for (uint8_t i = 0; i < smsCount; ++i) if (pendingSms[(smsHead + i) % 16] == index) return;
  if (index && smsCount < 16) pendingSms[(smsHead + smsCount++) % 16] = index;
}

void handleSmsResponse(uint16_t index, bool ok, const String& response) {
  readingSms = false;
  if (!ok) { event("SMS read failed; the modem copy was retained."); return; }
  int header = response.indexOf("+CMGR:");
  int first = response.indexOf('"', header), second = response.indexOf('"', first + 1);
  int senderStart = response.indexOf('"', second + 1), senderEnd = response.indexOf('"', senderStart + 1);
  int bodyStart = response.indexOf('\n', senderEnd);
  if (header < 0 || first < 0 || second < 0 || senderStart < 0 || senderEnd < 0 || bodyStart < 0) {
    if (appendLog(SMS_LOG, "Malformed SMS at modem index " + String(index))) deleteProcessedSms(index);
    return;
  }
  String sender, text, encodedBody = response.substring(bodyStart + 1);
  encodedBody.trim();
  if (!ucs2ToUtf8(response.substring(senderStart + 1, senderEnd), sender) || !ucs2ToUtf8(encodedBody, text)) {
    if (appendLog(SMS_LOG, "Unsupported SMS encoding at modem index " + String(index))) deleteProcessedSms(index);
    return;
  }
  uint32_t fingerprint = 2166136261UL;
  // Omit the mutable READ/UNREAD status; include sender, date and body.
  String identity = response.substring(senderStart);
  for (size_t i = 0; i < identity.length(); ++i) fingerprint = (fingerprint ^ uint8_t(identity[i])) * 16777619UL;
  for (const auto& item : processedSms) if (item.index == index && item.hash == fingerprint) {
    deleteProcessedSms(index);
    return;
  }
  if (!appendLog(SMS_LOG, "[" + networkTime + "] " + sender + ": " + text)) {
    event("SMS could not be logged; the modem copy was retained."); return;
  }
  processedSms[fingerprintNext] = {index, fingerprint};
  fingerprintNext = (fingerprintNext + 1) % 64;
  event("Incoming SMS recorded.");
  if (samePhone(sender, adminNumber) && securityReady) {
    String result;
    bool applied = applySmsCommand(text, result);
    if (!queueSms(sender, result, [](bool sent, const String&) {
          if (!sent) event("SMS command reply failed.");
        })) event(applied ? "Output updated; SMS reply unavailable." : "SMS command rejected.");
  }
  deleteProcessedSms(index);
}

void modemNotification(const String& line) {
  if (line == "RDY") {
    modemReady = false;
    registration = 0;
    event("Modem restarted; configuration will be restored.");
  } else if (line.startsWith("+CMGL:")) {
    int comma = line.indexOf(','); uint32_t index;
    String text = line.substring(6, comma); text.trim();
    if (comma > 6 && parseUnsigned(text, 65535, index)) scheduleSms(index);
  } else if (line.startsWith("+CMTI:")) {
    int comma = line.indexOf(','); uint32_t index;
    String text = line.substring(comma + 1); text.trim();
    if (comma >= 0 && parseUnsigned(text, 65535, index)) scheduleSms(index);
  } else if (line.startsWith("+CLIP:")) {
    int start = line.indexOf('"'), end = line.indexOf('"', start + 1);
    if (start >= 0 && end > start) appendLog(CALL_LOG, "[" + networkTime + "] " + line.substring(start + 1, end));
    if (!modem.enqueue("ATH", 3000, [](bool ok, const String&) {
          event(ok ? "Incoming call rejected." : "Call rejection failed.");
        })) event("Cannot reject call: modem queue is full.");
  }
}

void tickModemStartup() {
  static uint8_t stage = 0, config = 0;
  static uint32_t since = 0;
  static bool pending = false;
  static const char* commands[] = {"ATE0", "AT+CMGF=1", "AT+CSCS=\"UCS2\"", "AT+CSMP=17,167,0,8",
      "AT+CNMI=2,1,0,0,0", "AT+CLIP=1", "AT+CPMS=\"SM\",\"SM\",\"SM\""};
  if (stage == 5 && !modemReady) { stage = 3; config = 0; since = millis(); }
  if (stage == 0) { digitalWrite(MODEM_POWER, HIGH); digitalWrite(MODEM_KEY, LOW); since = millis(); stage = 1; }
  else if (stage == 1 && uint32_t(millis() - since) >= 1200) { digitalWrite(MODEM_KEY, HIGH); since = millis(); stage = 2; }
  else if (stage == 2 && uint32_t(millis() - since) >= 3000) stage = 3;
  else if (stage == 3 && !pending && modem.idle() && uint32_t(millis() - since) >= 1000) {
    pending = modem.enqueue("AT", 1500, [](bool ok, const String&) {
      pending = false; since = millis(); if (ok) stage = 4;
    });
  } else if (stage == 4 && !pending && modem.idle()) {
    if (config == sizeof(commands) / sizeof(commands[0])) {
      modemReady = true; stage = 5; event("Modem initialized; waiting for network registration.");
    } else {
      pending = modem.enqueue(commands[config], 3000, [](bool ok, const String&) {
        pending = false;
        if (ok) ++config;
        else { config = 0; stage = 3; since = millis(); }
      });
    }
  }
}

void pollModem() {
  static uint32_t last = 0;
  static uint8_t query = 0;
  if (!modemReady || !modem.idle() || uint32_t(millis() - last) < 5000) return;
  last = millis();
  switch (query++ % 5) {
    case 0: modem.enqueue("AT+CREG?", 2000, [](bool ok, const String& response) {
      if (!ok) { registration = 0; return; }
      int start = response.indexOf("+CREG:"), comma = response.indexOf(',', start);
      registration = comma >= 0 ? response.substring(comma + 1).toInt() : 0;
    }); break;
    case 1: modem.enqueue("AT+CSQ", 2000, [](bool ok, const String& response) {
      int start = response.indexOf("+CSQ:"), comma = response.indexOf(',', start);
      signalLevel = ok && start >= 0 && comma > start ? response.substring(start + 5, comma) : "unknown";
      signalLevel.trim();
    }); break;
    case 2: modem.enqueue("AT+COPS?", 2000, [](bool ok, const String& response) {
      int first = response.indexOf('"'), lastQuote = response.indexOf('"', first + 1);
      operatorName = ok && first >= 0 && lastQuote > first ? boundedUtf8(response.substring(first + 1, lastQuote), 64) : "unknown";
    }); break;
    case 3: modem.enqueue("AT+CCLK?", 2000, [](bool ok, const String& response) {
      int first = response.indexOf('"'), lastQuote = response.indexOf('"', first + 1);
      networkTime = ok && first >= 0 && lastQuote > first ? boundedUtf8(response.substring(first + 1, lastQuote), 64) : "unknown";
    }); break;
    case 4: modem.enqueue("AT+CMGL=\"ALL\"", 5000, [](bool ok, const String& response) {
      if (!ok) return;
      int position = 0;
      while ((position = response.indexOf("+CMGL:", position)) >= 0) {
        int comma = response.indexOf(',', position);
        String value = response.substring(position + 6, comma); value.trim(); uint32_t index;
        if (comma > position && parseUnsigned(value, 65535, index)) scheduleSms(index);
        position += 6;
      }
    }); break;
  }
}

void handleRequest(const String& line) {
  String fields[8]; size_t count = 0, start = 0;
  for (size_t i = 0; i <= line.length(); ++i) if (i == line.length() || line[i] == '\t') {
    if (count == 8) { reply(0, false, "Too many fields."); return; }
    fields[count++] = line.substring(start, i); start = i + 1;
  }
  uint32_t id = 0;
  if (count < 3 || fields[0] != "TCALL/1" || !parseUnsigned(fields[1], 2147483647UL, id) || !id) {
    reply(0, false, "Expected protocol TCALL/1. Use the matching desktop client."); return;
  }
  for (size_t i = 3; i < count; ++i) {
    String decoded;
    if (!hexDecode(fields[i], decoded)) { reply(id, false, "Invalid argument encoding."); return; }
    fields[i] = decoded;
  }
  const String& command = fields[2];
  if (command == "HELLO" && count == 3) reply(id, true, "TCall Pager " + String(VERSION));
  else if (command == "INIT_STORAGE" && count == 3) {
    // First-time provisioning requires an explicit command and physical BOOT
    // button access. A mounted filesystem can never be formatted by USB.
    if (storageReady || digitalRead(0) != LOW) {
      reply(id, false, "Storage initialization requires an unmounted filesystem and the BOOT button held down."); return;
    }
    if (!LittleFS.format() || !LittleFS.begin(false)) { reply(id, false, "Storage initialization failed."); return; }
    storageReady = true;
    loadState();
    reply(id, securityReady, securityReady ? "Storage initialized. Change the default credentials." : "Credential initialization failed.");
  }
  else if (command == "STATUS" && count == 3) reply(id, true, statusText());
  else if ((command == "SMS_LOG" || command == "CALL_LOG") && count == 3) {
    reply(id, storageReady, storageReady ? recentLog(command == "SMS_LOG" ? SMS_LOG : CALL_LOG) : "Storage unavailable.");
  } else if (command == "GPIO" && count == 7) {
    String error;
    bool ok = configureOutput(fields[3], fields[4], fields[5], fields[6], error);
    reply(id, ok, ok ? "Output setting applied." : error);
  } else if (command == "SEND_SMS" && count == 5) {
    if (!queueSms(fields[3], fields[4], [id](bool ok, const String& response) {
          reply(id, ok, ok ? "SMS submitted to the modem; recipient delivery is not confirmed." : response);
        })) reply(id, false, "Invalid SMS, modem not registered, or command queue full.");
  } else if ((command == "SET_ADMIN" || command == "SET_PASSWORD" || command == "SAFE_RESET" || command == "CLEAR_LOGS") &&
             count == (command == "SET_ADMIN" || command == "SET_PASSWORD" ? 5 : 4)) {
    if (!authenticate(fields[3])) { reply(id, false, "Authentication failed or temporarily locked."); return; }
    if (command == "SET_ADMIN") {
      if (!validPhone(fields[4]) || !saveSettings(fields[4], passwordSalt, passwordHash)) {
        reply(id, false, "Invalid number or settings write failed."); return;
      }
      adminNumber = fields[4]; reply(id, true, "Administrator number updated.");
    } else if (command == "SET_PASSWORD") {
      String encoded; size_t characters;
      if (!utf8ToUcs2(fields[4], encoded, characters) || characters < 6 || characters > 64) {
        reply(id, false, "Password must contain 6-64 BMP characters."); return;
      }
      String salt = newSalt(), hash = hashPassword(fields[4], salt, false);
      if (!hash.length() || !saveSettings(adminNumber, salt, hash)) { reply(id, false, "Settings write failed."); return; }
      passwordSalt = salt; passwordHash = hash; reply(id, true, "Password updated.");
    } else if (command == "SAFE_RESET") {
      for (auto& output : outputs) { output.mode = 0; output.working = false; digitalWrite(output.pin, LOW); }
      reply(id, saveOutputs(), "All outputs are OFF. Check status if persistence failed.");
    } else {
      bool sms = !LittleFS.exists(SMS_LOG) || LittleFS.remove(SMS_LOG);
      bool calls = !LittleFS.exists(CALL_LOG) || LittleFS.remove(CALL_LOG);
      reply(id, sms && calls, sms && calls ? "Logs cleared; settings and SIM messages preserved." : "Some logs could not be removed.");
    }
  } else reply(id, false, "Unknown command or incorrect argument count.");
}

void handleUsb() {
  unsigned budget = 256;
  while (budget-- && usbWriter.available() && Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (!discardUsbLine && usbLine.length()) handleRequest(usbLine);
      usbLine = ""; discardUsbLine = false;
    } else if (c != '\r' && !discardUsbLine) {
      usbLine += c;
      if (usbLine.length() > MAX_USB_LINE) { usbLine = ""; discardUsbLine = true; reply(0, false, "USB line too long."); }
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP);
  for (auto& output : outputs) { pinMode(output.pin, OUTPUT); digitalWrite(output.pin, LOW); }
  pinMode(MODEM_POWER, OUTPUT); pinMode(MODEM_KEY, OUTPUT);
  // Never format automatically after a mount failure: settings must survive.
  storageReady = LittleFS.begin(false);
  loadState();
  modemSerial.setRxBufferSize(4096);
  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  modem.notification = modemNotification;
  event("TCall Pager " + String(VERSION) + "; USB ready. Modem initialization is asynchronous.");
  if (!storageReady || !securityReady) event("Storage unavailable or credentials invalid; output changes are locked.");
}

void loop() {
  updateOutputs();
  usbWriter.tick();
  handleUsb();
  tickModemStartup();
  modem.tick();
  if (modemReady && !readingSms && smsCount && modem.idle()) {
    uint16_t index = pendingSms[smsHead]; smsHead = (smsHead + 1) % 16; --smsCount;
    readingSms = modem.enqueue("AT+CMGR=" + String(index), 5000,
        [index](bool ok, const String& response) { handleSmsResponse(index, ok, response); });
  }
  pollModem();
  updateOutputs();
  delay(1);  // Yield to the ESP32 scheduler without delaying modem transactions.
}
