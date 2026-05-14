/*
 * ИНСТРУКЦИЯ ПО АКТИВАЦИИ HARDWARE FLASH ENCRYPTION (Arduino IDE / ESP-IDF)
 * 1. Hardware Flash Encryption полагается на eFuse чипа ESP32.
 * 2. Для продакшена необходимо сгенерировать ключ и прошить его в eFuse:
 *    esptool.py --port /dev/ttyUSB0 burn_key flash_encryption my_flash_key.bin
 * 3. Включить фьюз шифрования: 
 *    esptool.py --port /dev/ttyUSB0 burn_efuse FLASH_CRYPT_CNT
 * 4. Убедитесь, что используете Partition Scheme, поддерживающую NVS и LittleFS.
 * ПРИМЕЧАНИЕ: Данная процедура аппаратная и может быть необратима.
 */

#include <Arduino.h>
#include <LittleFS.h>
#include "esp_mac.h"
#include "mbedtls/md.h"

// ==========================================
// ANTS MAKE AM-036 
// LilyGO T-Call 1.3
// ESP-32-WROVER-B and SIM800L
// CP2104
// ==========================================
#define MODEM_RX            26
#define MODEM_TX            27
#define MODEM_PWKEY         4
#define MODEM_RST           5
#define MODEM_POWER_ON      23
#define STATUS_LED          13

// ==========================================
// 1. МОДУЛЬ УПРАВЛЕНИЯ ПИНАМИ (PinConfig)
// ==========================================
struct PinConfig {
  int pin;
  int mode;                 // 0-OFF, 1-ON, 2-Timer/Cycle
  unsigned long workTime;   // ms
  unsigned long pauseTime;  // ms
  unsigned long startTime;  // ms
  bool isWorkPhase;         // Текущая фаза (работа или пауза)
};

// По умолчанию mode = 0, времена = 0
PinConfig pins[] = {
  {12, 0, 0, 0, 0, false},
  {14, 0, 0, 0, 0, false},
  {15, 0, 0, 0, 0, false},
  {2,  0, 0, 0, 0, false},
  {21, 0, 0, 0, 0, false},
  {22, 0, 0, 0, 0, false},
  {25, 0, 0, 0, 0, false}
};
const int numCtrlPins = sizeof(pins) / sizeof(pins[0]);

// ==========================================
// Settings & Constants
// ==========================================
const String DEFAULT_ADMIN_NUMBER = "+70000000000"; 
const int MAX_LOG_ENTRIES = 50;
const char* SMS_LOG_FILE = "/sms_log.txt";
const char* CALL_LOG_FILE = "/calls_log.txt";

// ==========================================
// Global Variables & States
// ==========================================
HardwareSerial SerialModem(1);

enum UIState { MAIN_MENU, SEND_SMS_NUM, SEND_SMS_MSG, GPIO_MENU, ADMIN_AUTH, ADMIN_MENU, SET_ADMIN_NUM, SET_ADMIN_PASS };
UIState uiState = MAIN_MENU;

String outSmsNum = "";
String outSmsMsg = "";
String modemBuffer = "";
unsigned long lastStatusUpdate = 0;
unsigned long lastSmsCleanup = 0; 
bool isBlinking = false;
unsigned long blinkStart = 0;

String currentSignal = "??";
String currentOperator = "Searching...";
String currentTime = "--:--";
String currentAdmin = "";

// Предварительные объявления
void printMenu();
void printGpioTable();
void printAdminMenu();
void sendSMS(String number, String text);
String sendAT(String cmd, uint32_t timeout = 1000);
void wipeModemMemory();
void saveSystemState();

// ==========================================
// 2. МОДУЛЬ КРИПТОГРАФИИ И БЕЗОПАСНОСТИ
// ==========================================
String getChipMAC() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macStr[18];
  sprintf(macStr, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(macStr);
}

String computeSecureHash(String pass, String salt) {
  String payload = pass + salt + getChipMAC();
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char *)payload.c_str(), payload.length());
  unsigned char hash[32];
  mbedtls_md_finish(&ctx, hash);
  mbedtls_md_free(&ctx);
  
  String hashStr = "";
  for(int i = 0; i < 32; i++) {
    char hex[3];
    sprintf(hex, "%02x", hash[i]);
    hashStr += hex;
  }
  return hashStr;
}

String generateSalt() {
  String chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  String salt = "";
  for(int i=0; i<16; i++) {
    salt += chars[random(0, chars.length())];
  }
  return salt;
}

String readFileStr(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  String res = f.readStringUntil('\n');
  res.trim();
  f.close();
  return res;
}

void writeFileStr(const char* path, String data) {
  File f = LittleFS.open(path, "w");
  if (f) { f.println(data); f.close(); }
}

void initSecurityAndState() {
  if (!LittleFS.exists("/admin.txt") || !LittleFS.exists("/salt.txt")) {
    Serial.println("\n[SYSTEM] First run: Default admin and password applied");
    String newSalt = generateSalt();
    writeFileStr("/salt.txt", newSalt);
    writeFileStr("/hash.txt", computeSecureHash("000000", newSalt));
    writeFileStr("/admin.txt", DEFAULT_ADMIN_NUMBER);
    
    saveSystemState();
  }
  
  currentAdmin = readFileStr("/admin.txt");

  File f = LittleFS.open("/pins_state.txt", "r");
  if (f) {
    for (int i = 0; i < numCtrlPins; i++) {
      String line = f.readStringUntil('\n');
      int mode = line.toInt();
      if (mode == 1) { 
        pins[i].mode = 1;
        digitalWrite(pins[i].pin, HIGH);
      } else { 
        pins[i].mode = 0;
        digitalWrite(pins[i].pin, LOW);
      }
    }
    f.close();
  }
}

void saveSystemState() {
  File f = LittleFS.open("/pins_state.txt", "w");
  if (f) {
    for (int i = 0; i < numCtrlPins; i++) {
      f.println(pins[i].mode);
    }
    f.close();
  }
}

// ==========================================
// 3. ОБНОВЛЕНИЕ АППАРАТНЫХ ТАЙМЕРОВ
// ==========================================
void updateHardwareStates() {
  unsigned long currentMillis = millis();
  for (int i = 0; i < numCtrlPins; i++) {
    if (pins[i].mode == 2) {
      unsigned long elapsed = currentMillis - pins[i].startTime;
      if (pins[i].isWorkPhase) {
        if (elapsed >= pins[i].workTime) {
          digitalWrite(pins[i].pin, LOW);
          pins[i].isWorkPhase = false;
          pins[i].startTime = currentMillis;
          if (pins[i].pauseTime == 0) {
            pins[i].mode = 0;
            saveSystemState();
          }
        }
      } else {
        if (elapsed >= pins[i].pauseTime && pins[i].pauseTime > 0) {
          digitalWrite(pins[i].pin, HIGH);
          pins[i].isWorkPhase = true;
          pins[i].startTime = currentMillis;
        }
      }
    }
  }
}

// ==========================================
// 4. ОРИГИНАЛЬНЫЙ ФУНКЦИОНАЛ: Память & UCS2
// ==========================================
int getSimSmsCount() {
  String resp = sendAT("AT+CPMS?", 1000);
  int idx1 = resp.indexOf("\"SM\",");
  if (idx1 != -1) {
    idx1 += 5;
    int idx2 = resp.indexOf(",", idx1);
    if (idx2 != -1) return resp.substring(idx1, idx2).toInt();
  }
  return 0;
}

void deleteOldSms() {
  int simCount = getSimSmsCount();
  if (simCount > 30) {
    Serial.printf("\n[!] На SIM %d SMS (> 30). Удаляем 20 старых...\n", simCount);
    for (int i = 1; i <= 20; i++) {
      SerialModem.print("AT+CMGD="); SerialModem.println(i); delay(50);
    }
    Serial.println("[OK] Очистка SIM завершена.");
  }
}

void wipeModemMemory() {
  Serial.println("\n[!] Полная очистка физической памяти SIM-карты и модема...");
  sendAT("AT+CPMS=\"SM\",\"SM\",\"SM\""); sendAT("AT+CMGD=1,4");
  sendAT("AT+CPMS=\"ME\",\"ME\",\"ME\""); sendAT("AT+CMGD=1,4");
  const char* callLogs[] = {"MC", "RC", "DC"};
  for (int i = 0; i < 3; i++) {
    sendAT("AT+CPBS=\"" + String(callLogs[i]) + "\"");
    for (int j = 1; j <= 20; j++) { SerialModem.print("AT+CPBW="); SerialModem.println(j); delay(40); }
  }
  sendAT("AT+CPBS=\"SM\""); sendAT("AT+CPMS=\"SM\",\"SM\",\"SM\"");
  Serial.println("[OK] Память SIM-карты абсолютно чиста.");
}

// ОРИГИНАЛЬНЫЕ КОНВЕРТЕРЫ 1:1
String utf8ToUcs2(String utf8Str) {
  String ucs2 = "";
  for (int i = 0; i < utf8Str.length();) {
    uint32_t codePoint = 0; uint8_t c = utf8Str[i];
    if (c <= 0x7F) { codePoint = c; i++; } 
    else if ((c & 0xE0) == 0xC0) { codePoint = ((c & 0x1F) << 6) | (utf8Str[i + 1] & 0x3F); i += 2; } 
    else if ((c & 0xF0) == 0xE0) { codePoint = ((c & 0x0F) << 12) | ((utf8Str[i + 1] & 0x3F) << 6) | (utf8Str[i + 2] & 0x3F); i += 3; } 
    else { i++; continue; }
    char hex[5]; sprintf(hex, "%04X", codePoint); ucs2 += String(hex);
  }
  return ucs2;
}

String ucs2ToUtf8(String ucs2Hex) {
  String utf8 = "";
  for (int i = 0; i < ucs2Hex.length(); i += 4) {
    if (i + 4 > ucs2Hex.length()) break;
    String hexChar = ucs2Hex.substring(i, i + 4);
    uint16_t codePoint = (uint16_t)strtol(hexChar.c_str(), NULL, 16);
    if (codePoint <= 0x7F) { utf8 += (char)codePoint; } 
    else if (codePoint <= 0x7FF) { utf8 += (char)(0xC0 | ((codePoint >> 6) & 0x1F)); utf8 += (char)(0x80 | (codePoint & 0x3F)); } 
    else { utf8 += (char)(0xE0 | ((codePoint >> 12) & 0x0F)); utf8 += (char)(0x80 | ((codePoint >> 6) & 0x3F)); utf8 += (char)(0x80 | (codePoint & 0x3F)); }
  }
  return utf8;
}

// ==========================================
// LittleFS Buffer Logic
// ==========================================
int countLines(const char* filename) {
  File f = LittleFS.open(filename, "r");
  if (!f) return 0;
  int lines = 0;
  while (f.available()) { if (f.read() == '\n') lines++; }
  f.close(); return lines;
}

void appendToLog(const char* filename, String entry) {
  if (countLines(filename) > 45) {
    File f = LittleFS.open(filename, "r");
    String tempStr = ""; int skipped = 0;
    while (f.available()) {
      String line = f.readStringUntil('\n'); line.trim();
      if (line.length() == 0) continue; 
      if (skipped < 20) { skipped++; continue; }
      tempStr += line + "\n";
    }
    f.close();
    f = LittleFS.open(filename, "w"); f.print(tempStr); f.close();
  }
  File f = LittleFS.open(filename, "a");
  if (f) { entry.trim(); f.println(entry); f.close(); }
}

void printLog(const char* filename) {
  File f = LittleFS.open(filename, "r");
  if (!f) { Serial.println("Log is empty."); return; }
  String lines[MAX_LOG_ENTRIES + 5]; int count = 0;
  while (f.available() && count < (MAX_LOG_ENTRIES + 5)) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.length() > 0) lines[count++] = line;
  }
  f.close();
  for (int i = count - 1; i >= 0; i--) Serial.println(lines[i]);
}

// ==========================================
// Modem Base Functions
// ==========================================
String sendAT(String cmd, uint32_t timeout) {
  SerialModem.println(cmd);
  long start = millis(); String resp = "";
  while (millis() - start < timeout) { while (SerialModem.available()) { resp += (char)SerialModem.read(); } }
  return resp;
}

void sendSMS(String number, String text) {
  Serial.println("\n[SMS] Отправка на номер: " + number + " ...");
  SerialModem.print("AT+CMGS=\""); SerialModem.print(utf8ToUcs2(number)); SerialModem.println("\"");
  delay(1000); SerialModem.print(utf8ToUcs2(text)); delay(500); SerialModem.write(26); 
  
  long start = millis(); String resp = "";
  while (millis() - start < 10000) {
    while (SerialModem.available()) { resp += (char)SerialModem.read(); }
    if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) break;
  }
  if (resp.indexOf("OK") != -1) Serial.println("[SMS] Сообщение УСПЕШНО отправлено!");
  else Serial.println("[SMS] ОШИБКА при отправке: " + resp);
}

// ВОЗВРАЩЕНА ОРИГИНАЛЬНАЯ ЛОГИКА ЗАПУСКА С ОЖИДАНИЕМ СЕТИ
void initModem() {
  Serial.print("Booting SIM800L...");
  pinMode(MODEM_POWER_ON, OUTPUT); digitalWrite(MODEM_POWER_ON, HIGH);
  pinMode(MODEM_PWKEY, OUTPUT); digitalWrite(MODEM_PWKEY, LOW);
  delay(1200); digitalWrite(MODEM_PWKEY, HIGH); delay(3000);
  
  SerialModem.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  while (sendAT("AT", 500).indexOf("OK") == -1) { Serial.print("."); delay(500); }
  Serial.println(" OK!");
  
  Serial.print("Waiting for Network...");
  while (true) {
    String creg = sendAT("AT+CREG?");
    if (creg.indexOf(",1") != -1 || creg.indexOf(",5") != -1) break;
    Serial.print("."); delay(1000);
  }
  Serial.println(" Registered!");
  
  String cops = "";
  while ((cops = sendAT("AT+COPS?")).indexOf(",\"") == -1) { delay(1000); }
  int opStart = cops.indexOf(",\"") + 2;
  int opEnd = cops.indexOf("\"", opStart);
  currentOperator = cops.substring(opStart, opEnd);

  sendAT("AT+CLTS=1"); 
  
  Serial.print("Waiting for Signal...");
  while (currentSignal == "??" || currentSignal == "0" || currentSignal == "99") {
    String csq = sendAT("AT+CSQ");
    int i1 = csq.indexOf(":") + 1;
    int i2 = csq.indexOf(",");
    if (i1 > 0 && i2 > 0) currentSignal = csq.substring(i1, i2);
    currentSignal.trim();
    delay(1000); Serial.print(".");
  }
  Serial.println(" OK!");

  Serial.print("Syncing Network Time...");
  while (currentTime == "--:--") {
    String cclk = sendAT("AT+CCLK?");
    int t1 = cclk.indexOf(",") + 1;
    if (t1 > 0) {
      String t = cclk.substring(t1, t1 + 5);
      if (t.indexOf(":") != -1 && t.indexOf("00:00") == -1) { currentTime = t; break; }
    }
    delay(2000); Serial.print(".");
  }
  Serial.println(" OK!");
  
  sendAT("AT+CSCS=\"UCS2\"");      
  sendAT("AT+CSMP=17,167,0,8");  
  sendAT("AT+CMGF=1");            
  sendAT("AT+CNMI=2,1,0,0,0");   
  sendAT("AT+CLIP=1");           

  wipeModemMemory();
}

void triggerEventNotification() {
  isBlinking = true; blinkStart = millis(); digitalWrite(STATUS_LED, HIGH);
}

// ==========================================
// 5. ГИБРИДНЫЙ ПАРСИНГ SMS (Оригинальная логика UCS2)
// ==========================================
void parseIncomingSMS(int index) {
  String resp = sendAT("AT+CMGR=" + String(index), 2000);
  int nS = resp.indexOf(",\"") + 2; if (nS < 2) return;
  String sender = ucs2ToUtf8(resp.substring(nS, resp.indexOf("\"", nS)));
  
  int hE = resp.indexOf("\n", resp.indexOf("+CMGR:"));
  String msg = "";
  if (hE != -1) {
    msg = ucs2ToUtf8(resp.substring(hE + 1, resp.lastIndexOf("\r\nOK")));
    msg.trim();
  }

  if (msg.length() > 0) {
    // Вывод мгновенного уведомления в Serial-порт
    Serial.println("\n-----------------------------------------");
    Serial.printf("[📩 НОВОЕ СМС] От: %s\n", sender.c_str());
    Serial.printf("Текст: %s\n", msg.c_str());
    Serial.println("-----------------------------------------");

    // 1. ЛОГГИРУЕМ ВСЕ ВХОДЯЩИЕ СМС (от любых номеров)
    appendToLog(SMS_LOG_FILE, "[" + currentTime + "] " + sender + ": " + msg);

    // Подготовка суффикса админского номера (защита от краша, если номер < 10 цифр)
    int adminLen = currentAdmin.length();
    String adminSuffix = (adminLen >= 10) ? currentAdmin.substring(adminLen - 10) : currentAdmin;

    // 2. ПРОВЕРЯЕМ ПРАВА АДМИНА ДЛЯ УПРАВЛЕНИЯ ПИНАМИ
    if (sender.endsWith(adminSuffix)) {
      msg.toUpperCase();
      
      if (msg.startsWith("PIN ") || msg.startsWith("#")) {
        String cmd = msg; 
        cmd.replace("PIN ", ""); 
        cmd.replace("#", "");
        
        int sp = cmd.indexOf(" ");
        if (sp > 0) {
          int pinNum = cmd.substring(0, sp).toInt();
          String act = cmd.substring(sp + 1); 
          act.trim();
          
          int pIdx = -1;
          for(int i=0; i<numCtrlPins; i++) {
            if(pins[i].pin == pinNum) pIdx = i;
          }

          if (pIdx != -1) {
            if (act == "OFF") {
              pins[pIdx].mode = 0; 
              digitalWrite(pinNum, LOW);
              sendSMS(sender, "PIN " + String(pinNum) + " is OFF");
            } 
            else if (act.startsWith("ON")) {
              String params = act.substring(2); 
              params.trim();

              if (params.length() == 0) {
                // Просто ON — включаем навсегда
                pins[pIdx].mode = 1; 
                digitalWrite(pinNum, HIGH);
                sendSMS(sender, "PIN " + String(pinNum) + " is ON CONST");
              } else {
                // ON с цифрами — включаем таймер
                int sp2 = params.indexOf(" ");
                unsigned long wM = 0, pM = 0;
                
                if (sp2 > 0) {
                  wM = params.substring(0, sp2).toInt();
                  pM = params.substring(sp2 + 1).toInt();
                } else {
                  wM = params.toInt();
                }

                if (wM > 0) {
                  pins[pIdx].mode = 2;
                  pins[pIdx].workTime = wM * 60000UL;
                  pins[pIdx].pauseTime = pM * 60000UL;
                  pins[pIdx].isWorkPhase = true;
                  pins[pIdx].startTime = millis();
                  digitalWrite(pinNum, HIGH);

                  // Формируем ответное SMS
                  String rep = "PIN " + String(pinNum) + " ON. Work: " + String(wM) + "min";
                  if (pM > 0) {
                    rep += ", Pause: " + String(pM) + "min (cycle)";
                  } else {
                    rep += ", then OFF";
                  }
                  sendSMS(sender, rep);
                }
              }
            }
            saveSystemState();
          }
        }
      }
    }
  }
  
  sendAT("AT+CMGD=" + String(index)); // Удаляем SMS с симки
  if (uiState == MAIN_MENU) printMenu();
}

// ==========================================
// 6. ЛОКАЛЬНЫЙ ИНТЕРФЕЙС (Serial UI)
// ==========================================
void safeResetState() {
  for (int i = 0; i < numCtrlPins; i++) {
    pins[i].mode = 0;
    digitalWrite(pins[i].pin, LOW);
  }
  saveSystemState();
  Serial.println("\n[!] SAFE RESET: Все пины выключены.");
}

void printGpioTable() {
  Serial.println("\n----------------------------- GPIO STATUS -----------------------------");
  Serial.println("| PIN | STATUS | MODE  | WORK TIME (remain) | PAUSE TIME (remain) |");
  Serial.println("|-----|--------|-------|--------------------|---------------------|");
  for (int i = 0; i < numCtrlPins; i++) {
    String state = (digitalRead(pins[i].pin) == HIGH) ? " ON " : " OFF";
    String modeStr = pins[i].mode == 0 ? "  0  " : (pins[i].mode == 1 ? "CONST" : "TIMER");
    
    char workBuf[32] = "0";
    char pauseBuf[32] = "0";

    if (pins[i].mode == 2) {
       unsigned long wRem = pins[i].isWorkPhase ? (pins[i].workTime - (millis() - pins[i].startTime)) : pins[i].workTime;
       unsigned long pRem = !pins[i].isWorkPhase ? (pins[i].pauseTime - (millis() - pins[i].startTime)) : pins[i].pauseTime;
       
       String wStr = String(wRem / 60000) + "m " + String((wRem % 60000)/1000) + "s";
       String pStr = String(pRem / 60000) + "m " + String((pRem % 60000)/1000) + "s";
       
       sprintf(workBuf, "%-18s", wStr.c_str());
       sprintf(pauseBuf, "%-19s", pStr.c_str());
    } else {
       sprintf(workBuf, "%-18s", "---");
       sprintf(pauseBuf, "%-19s", "---");
    }

    Serial.printf("| %2d  |  %s  | %s | %s | %s |\n", pins[i].pin, state.c_str(), modeStr.c_str(), workBuf, pauseBuf);
  }
  Serial.println("-----------------------------------------------------------------------");
  Serial.println("[ОПИСАНИЕ КОЛОНОК]");
  Serial.println(" - PIN       : Номер контакта (GPIO ESP32)");
  Serial.println(" - STATUS    : Текущее физическое состояние реле (ON / OFF)");
  Serial.println(" - MODE      : 0 (Выключено), CONST (Включено постоянно), TIMER (По таймеру)");
  Serial.println(" - WORK TIME : Время работы (мин:сек) - 0 по умолчанию, если не задано");
  Serial.println(" - PAUSE TIME: Время паузы (мин:сек)  - 0 по умолчанию, если не задано");
  Serial.println("-----------------------------------------------------------------------");
  Serial.println("Enter PIN to toggle CONST_ON/OFF or '0' to back:");
}

void printMenu() {
  Serial.println("\n=================================");
  Serial.printf("[📶 %s] [🕒 %s] [📩 SMS: %d] [📞 CALLS: %d]\n", 
    currentSignal.c_str(), currentTime.c_str(), countLines(SMS_LOG_FILE), countLines(CALL_LOG_FILE));
  Serial.println("=================================");
  Serial.println("1. Read SMS Log");
  Serial.println("2. Read Call History");
  Serial.println("3. Send SMS");
  Serial.println("4. GPIO Control");
  Serial.println("5. Wipe Logs & SIM Memory");
  Serial.println("6. Admin Setup (Auth Required)");
  Serial.println("=================================");
  Serial.print("Select: ");
}

void printAdminMenu() {
  Serial.println("\n------- ADMIN SETUP -------");
  Serial.println("1. Change Admin Number");
  Serial.println("2. Change Password");
  Serial.println("3. SAFE RESET (Pins OFF)");
  Serial.println("0. Back to Main Menu");
  Serial.print("Select: ");
}

void handleSerialUI() {
  if (!Serial.available()) return;
  String input = Serial.readStringUntil('\n'); input.trim();
  if (input.length() == 0) return;

  if (input == "0" && uiState != ADMIN_AUTH) { uiState = MAIN_MENU; printMenu(); return; }

  switch (uiState) {
    case MAIN_MENU:
      if (input == "1") { Serial.println("\n--- SMS Log ---"); printLog(SMS_LOG_FILE); }
      else if (input == "2") { Serial.println("\n--- Call Log ---"); printLog(CALL_LOG_FILE); }
      else if (input == "3") { uiState = SEND_SMS_NUM; Serial.print("\nEnter Destination Number: "); }
      else if (input == "4") { uiState = GPIO_MENU; printGpioTable(); }
      else if (input == "5") { LittleFS.format(); wipeModemMemory(); initSecurityAndState(); Serial.println("\n[DONE] Все логи и SIM очищены."); }
      else if (input == "6") { uiState = ADMIN_AUTH; Serial.print("\nEnter Password: "); return; }
      if (uiState == MAIN_MENU) printMenu();
      break;

    case SEND_SMS_NUM: outSmsNum = input; uiState = SEND_SMS_MSG; Serial.print("Enter Message: "); break;
    case SEND_SMS_MSG: outSmsMsg = input; sendSMS(outSmsNum, outSmsMsg); uiState = MAIN_MENU; printMenu(); break;

    case GPIO_MENU: {
        int targetPin = input.toInt();
        for(int i = 0; i < numCtrlPins; i++) {
          if(pins[i].pin == targetPin) {
            pins[i].mode = (pins[i].mode == 1) ? 0 : 1; 
            digitalWrite(pins[i].pin, pins[i].mode == 1 ? HIGH : LOW);
            saveSystemState();
          }
        }
        printGpioTable();
      } break;

    case ADMIN_AUTH: {
        String savedSalt = readFileStr("/salt.txt");
        String savedHash = readFileStr("/hash.txt");
        if (computeSecureHash(input, savedSalt) == savedHash) {
          Serial.println("\n[AUTH SUCCESS]"); uiState = ADMIN_MENU; printAdminMenu();
        } else {
          Serial.println("\n[AUTH FAILED] Wrong password."); uiState = MAIN_MENU; printMenu();
        }
      } break;

    case ADMIN_MENU:
      if (input == "1") { uiState = SET_ADMIN_NUM; Serial.print("Enter New Admin Number (+...): "); }
      else if (input == "2") { uiState = SET_ADMIN_PASS; Serial.print("Enter New Password: "); }
      else if (input == "3") { safeResetState(); printAdminMenu(); }
      break;

    case SET_ADMIN_NUM:
      currentAdmin = input; writeFileStr("/admin.txt", currentAdmin);
      Serial.println("\n[OK] Admin number updated."); uiState = ADMIN_MENU; printAdminMenu(); break;

    case SET_ADMIN_PASS: {
        String newSalt = generateSalt();
        writeFileStr("/salt.txt", newSalt);
        writeFileStr("/hash.txt", computeSecureHash(input, newSalt));
        Serial.println("\n[OK] Password updated."); uiState = ADMIN_MENU; printAdminMenu();
      } break;
  }
}

// ==========================================
// Main Loops
// ==========================================
void handleModemAsync() {
  while (SerialModem.available()) {
    char c = SerialModem.read(); modemBuffer += c;
    if (c == '\n') {
      if (modemBuffer.indexOf("+CMTI:") != -1) {
        int index = modemBuffer.substring(modemBuffer.indexOf(",") + 1).toInt();
        triggerEventNotification(); parseIncomingSMS(index);
      } 
      else if (modemBuffer.indexOf("+CLIP:") != -1) {
        int start = modemBuffer.indexOf("\"") + 1;
        String number = modemBuffer.substring(start, modemBuffer.indexOf("\"", start));
        triggerEventNotification();
        Serial.println("\n[📞 ВХОДЯЩИЙ ЗВОНОК] От: " + number);
        appendToLog(CALL_LOG_FILE, "[" + currentTime + "] Rejected: " + number);
        sendAT("ATH"); 
        
        sendAT("AT+CPBS=\"RC\""); sendAT("AT+CMGD=1,4"); 
        sendAT("AT+CPBS=\"MC\""); sendAT("AT+CMGD=1,4"); 
        sendAT("AT+CPBS=\"SM\"");
        
        if (uiState == MAIN_MENU) printMenu();
      }
      modemBuffer = ""; 
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(STATUS_LED, OUTPUT);
  for (int i = 0; i < numCtrlPins; i++) { pinMode(pins[i].pin, OUTPUT); digitalWrite(pins[i].pin, LOW); }
  
  if (!LittleFS.begin(true)) Serial.println("LittleFS Mount Failed");
  
  initSecurityAndState(); // Load secure params & restore pin states
  initModem();            // Setup SIM800L
  
  randomSeed(micros());
  lastStatusUpdate = millis();
  lastSmsCleanup = millis();
  
  // Меню выводится ТОЛЬКО после полной синхронизации initModem()
  printMenu();
}

void loop() {
  handleModemAsync();
  handleSerialUI();
  updateHardwareStates(); // Неблокирующий обработчик таймеров для пинов

  // StatusBar & Cleanup timers
  if (millis() - lastStatusUpdate > 60000) {
    lastStatusUpdate = millis();
    String csq = sendAT("AT+CSQ", 500);
    int i1 = csq.indexOf(":") + 1; int i2 = csq.indexOf(",");
    if (i1 > 0 && i2 > 0) { currentSignal = csq.substring(i1, i2); currentSignal.trim(); }
    
    String cclk = sendAT("AT+CCLK?", 500);
    int t1 = cclk.indexOf(",") + 1;
    if (t1 > 0) {
      String newTime = cclk.substring(t1, t1 + 5);
      if (newTime.indexOf(":") != -1) currentTime = newTime;
    }
  }
  if (millis() - lastSmsCleanup > 60000) { lastSmsCleanup = millis(); deleteOldSms(); }

  // LED blinker
  if (isBlinking) {
    if (millis() - blinkStart > 3000) { isBlinking = false; digitalWrite(STATUS_LED, LOW); }
    else { digitalWrite(STATUS_LED, (millis() / 200) % 2); }
  }
}