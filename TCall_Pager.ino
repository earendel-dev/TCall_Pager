#include <Arduino.h>
#include <LittleFS.h>

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

// Массив пинов для управления
const int ctrlPins[] = {12, 14, 15, 2, 21, 22, 25};
const int numCtrlPins = sizeof(ctrlPins) / sizeof(ctrlPins[0]);

// ==========================================
// Settings & Constants
// ==========================================
const String ADMIN_NUMBER = "+70000000000"; 
const int MAX_LOG_ENTRIES = 50;
const char* SMS_LOG_FILE = "/sms_log.txt";
const char* CALL_LOG_FILE = "/calls_log.txt";

// ==========================================
// Global Variables & States
// ==========================================
HardwareSerial SerialModem(1);

enum UIState { MAIN_MENU, SEND_SMS_NUM, SEND_SMS_MSG, GPIO_MENU };
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

// Предварительные объявления
void printMenu();
void printGpioTable();
void sendSMS(String number, String text);
String sendAT(String cmd, uint32_t timeout = 1000);
int getSimSmsCount();
void wipeModemMemory();

// ==========================================
// Очистка памяти SIM
// ==========================================
int getSimSmsCount() {
  String resp = sendAT("AT+CPMS?", 1000);
  int idx1 = resp.indexOf("\"SM\",");
  if (idx1 != -1) {
    idx1 += 5;
    int idx2 = resp.indexOf(",", idx1);
    if (idx2 != -1) {
      return resp.substring(idx1, idx2).toInt();
    }
  }
  return 0;
}

void deleteOldSms() {
  int simCount = getSimSmsCount();
  if (simCount > 30) {
    Serial.printf("\n[!] На SIM %d SMS (> 30). Удаляем 20 старых...\n", simCount);
    for (int i = 1; i <= 20; i++) {
      SerialModem.print("AT+CMGD=");
      SerialModem.println(i);
      delay(50);
    }
    Serial.println("[OK] Очистка SIM завершена.");
  }
}

void wipeModemMemory() {
  Serial.println("\n[!] Полная очистка физической памяти SIM-карты и модема...");
  
  // 1. Очистка SMS (Память SIM)
  sendAT("AT+CPMS=\"SM\",\"SM\",\"SM\""); 
  sendAT("AT+CMGD=1,4");
  
  // 2. Очистка SMS (Внутренняя память ME)
  sendAT("AT+CPMS=\"ME\",\"ME\",\"ME\"");
  sendAT("AT+CMGD=1,4");
  
  // 3. Очистка списков вызовов
  const char* callLogs[] = {"MC", "RC", "DC"};
  for (int i = 0; i < 3; i++) {
    sendAT("AT+CPBS=\"" + String(callLogs[i]) + "\"");
    for (int j = 1; j <= 20; j++) {
      SerialModem.print("AT+CPBW=");
      SerialModem.println(j);
      delay(40);
    }
  }
  
  sendAT("AT+CPBS=\"SM\"");
  sendAT("AT+CPMS=\"SM\",\"SM\",\"SM\"");
  Serial.println("[OK] Память SIM-карты абсолютно чиста.");
}

// ==========================================
// UCS2 <-> UTF8 Cyrillic Converters
// ==========================================
String utf8ToUcs2(String utf8Str) {
  String ucs2 = "";
  for (int i = 0; i < utf8Str.length();) {
    uint32_t codePoint = 0;
    uint8_t c = utf8Str[i];
    if (c <= 0x7F) {
      codePoint = c; i++;
    } else if ((c & 0xE0) == 0xC0) {
      codePoint = ((c & 0x1F) << 6) | (utf8Str[i + 1] & 0x3F); i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      codePoint = ((c & 0x0F) << 12) | ((utf8Str[i + 1] & 0x3F) << 6) | (utf8Str[i + 2] & 0x3F); i += 3;
    } else { i++; continue; }
    char hex[5];
    sprintf(hex, "%04X", codePoint);
    ucs2 += String(hex);
  }
  return ucs2;
}

String ucs2ToUtf8(String ucs2Hex) {
  String utf8 = "";
  for (int i = 0; i < ucs2Hex.length(); i += 4) {
    if (i + 4 > ucs2Hex.length()) break;
    String hexChar = ucs2Hex.substring(i, i + 4);
    uint16_t codePoint = (uint16_t)strtol(hexChar.c_str(), NULL, 16);
    if (codePoint <= 0x7F) {
      utf8 += (char)codePoint;
    } else if (codePoint <= 0x7FF) {
      utf8 += (char)(0xC0 | ((codePoint >> 6) & 0x1F));
      utf8 += (char)(0x80 | (codePoint & 0x3F));
    } else {
      utf8 += (char)(0xE0 | ((codePoint >> 12) & 0x0F));
      utf8 += (char)(0x80 | ((codePoint >> 6) & 0x3F));
      utf8 += (char)(0x80 | (codePoint & 0x3F));
    }
  }
  return utf8;
}

// ==========================================
// LittleFS Buffer Logic (Удаляем 20, если > 45)
// ==========================================
int countLines(const char* filename) {
  File f = LittleFS.open(filename, "r");
  if (!f) return 0;
  int lines = 0;
  while (f.available()) { if (f.read() == '\n') lines++; }
  f.close();
  return lines;
}

void appendToLog(const char* filename, String entry) {
  int currentLines = countLines(filename);
  if (currentLines > 45) {
    File f = LittleFS.open(filename, "r");
    String tempStr = "";
    int skipped = 0;
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (line.length() == 0) continue; 
      if (skipped < 20) { skipped++; continue; }
      tempStr += line + "\n";
    }
    f.close();
    f = LittleFS.open(filename, "w");
    f.print(tempStr);
    f.close();
  }
  File f = LittleFS.open(filename, "a");
  if (f) { 
    entry.trim();
    f.println(entry); 
    f.close(); 
  }
}

void printLog(const char* filename) {
  File f = LittleFS.open(filename, "r");
  if (!f) { Serial.println("Log is empty."); return; }
  String lines[MAX_LOG_ENTRIES + 5];
  int count = 0;
  while (f.available() && count < (MAX_LOG_ENTRIES + 5)) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      lines[count++] = line;
    }
  }
  f.close();
  for (int i = count - 1; i >= 0; i--) { Serial.println(lines[i]); }
}

// ==========================================
// Modem Functions
// ==========================================
String sendAT(String cmd, uint32_t timeout) {
  SerialModem.println(cmd);
  long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (SerialModem.available()) { resp += (char)SerialModem.read(); }
  }
  return resp;
}

void sendSMS(String number, String text) {
  Serial.println("\n[SMS] Отправка на номер: " + number + " ...");
  SerialModem.print("AT+CMGS=\"");
  SerialModem.print(utf8ToUcs2(number));
  SerialModem.println("\"");
  delay(1000); 
  SerialModem.print(utf8ToUcs2(text));
  delay(500);
  SerialModem.write(26); 
  long start = millis();
  String resp = "";
  while (millis() - start < 10000) {
    while (SerialModem.available()) { resp += (char)SerialModem.read(); }
    if (resp.indexOf("OK") != -1 || resp.indexOf("ERROR") != -1) break;
  }
  if (resp.indexOf("OK") != -1) {
    Serial.println("[SMS] Сообщение УСПЕШНО отправлено!");
  } else {
    Serial.println("[SMS] ОШИБКА при отправке: " + resp);
  }
}

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
  isBlinking = true;
  blinkStart = millis();
  digitalWrite(STATUS_LED, HIGH);
  if (uiState != MAIN_MENU) {
    Serial.println("\n\n>>> УВЕДОМЛЕНИЕ: НОВОЕ СОБЫТИЕ <<<\n");
  }
}

// ==========================================
// SMS Parsing (GPIO УПРАВЛЕНИЕ С ОТВЕТОМ)
// ==========================================
void parseIncomingSMS(int index) {
  String cmd = "AT+CMGR=" + String(index);
  String resp = sendAT(cmd, 3000);
  
  int numStart = resp.indexOf(",\"") + 2;
  if (numStart < 2) return;
  int numEnd = resp.indexOf("\"", numStart);
  String sender = ucs2ToUtf8(resp.substring(numStart, numEnd));
  
  String message = "";
  int headerEnd = resp.indexOf("\n", resp.indexOf("+CMGR:"));
  if (headerEnd != -1) {
    int msgEnd = resp.lastIndexOf("\r\nOK");
    if (msgEnd == -1) msgEnd = resp.lastIndexOf("\n");
    String hexMsg = resp.substring(headerEnd + 1, msgEnd);
    hexMsg.replace("\r", ""); hexMsg.replace("\n", "");
    message = ucs2ToUtf8(hexMsg);
    message.trim();
  }

  // Фильтр мусора: если сообщение пустое или некорректное - игнорируем лог, но удаляем из SIM
  if (message.length() > 0) {
    Serial.println("\n---------------------------------");
    Serial.println("[📩 НОВОЕ ВХОДЯЩЕЕ SMS]");
    Serial.println("От: " + sender);
    Serial.println("Текст: '" + message + "'");
    Serial.println("---------------------------------");

    bool isAdmin = false;
    if (sender.endsWith(ADMIN_NUMBER.substring(ADMIN_NUMBER.length() - 10))) isAdmin = true;

    if (isAdmin) {
      String msgUpper = message;
      msgUpper.toUpperCase();
      if (msgUpper.startsWith("PIN ") || msgUpper.startsWith("#")) {
        String cleanStr = msgUpper;
        cleanStr.replace("PIN ", ""); cleanStr.replace("#", "");
        int spaceIdx = cleanStr.indexOf(" ");
        if (spaceIdx > 0) {
          int pin = cleanStr.substring(0, spaceIdx).toInt();
          String action = cleanStr.substring(spaceIdx + 1); action.trim();
          bool allowed = false;
          for(int p : ctrlPins) if(p == pin) allowed = true;
          if (allowed) {
            if (action == "ON") { digitalWrite(pin, HIGH); sendSMS(sender, "PIN " + String(pin) + " is ON"); }
            else if (action == "OFF") { digitalWrite(pin, LOW); sendSMS(sender, "PIN " + String(pin) + " is OFF"); }
          }
        }
      }
    }
    appendToLog(SMS_LOG_FILE, "[" + currentTime + "] " + sender + " : " + message);
  }

  sendAT("AT+CMGD=" + String(index)); 
  Serial.printf("[SYSTEM] SIM SMS slot %d cleared.\n", index);
  
  if (uiState == MAIN_MENU) printMenu();
}

// ==========================================
// UI & Update Loop Functions
// ==========================================
void printGpioTable() {
  Serial.println("\n----------- GPIO STATUS -----------");
  Serial.println("|  PIN  |  STATUS  |      MODE      |");
  Serial.println("|-------|----------|-------------|");
  for (int i = 0; i < numCtrlPins; i++) {
    int pin = ctrlPins[i];
    String state = (digitalRead(pin) == HIGH) ? "  [ON]  " : "  [OFF] ";
    Serial.printf("|  %2d   | %s |    OUTPUT    |\n", pin, state.c_str());
  }
  Serial.println("-----------------------------------");
  Serial.println("Enter PIN number to toggle or '0' to back:");
}

void updateStatusBar() {
  if (millis() - lastStatusUpdate > 60000) {
    lastStatusUpdate = millis();
    bool updated = false;
    String csq = sendAT("AT+CSQ", 500);
    int i1 = csq.indexOf(":") + 1;
    int i2 = csq.indexOf(",");
    if (i1 > 0 && i2 > 0) {
      String newSig = csq.substring(i1, i2);
      newSig.trim();
      if (currentSignal != newSig) { currentSignal = newSig; updated = true; }
    }
    String cclk = sendAT("AT+CCLK?", 500);
    int t1 = cclk.indexOf(",") + 1;
    if (t1 > 0) {
      String newTime = cclk.substring(t1, t1 + 5);
      if (currentTime != newTime && newTime.indexOf(":") != -1) { currentTime = newTime; updated = true; }
    }
    if (updated && uiState == MAIN_MENU) { Serial.println("\n\n[Авто-обновление статуса]"); printMenu(); }
  }

  if (millis() - lastSmsCleanup > 60000) {
    lastSmsCleanup = millis();
    deleteOldSms();
  }
}

void printMenu() {
  Serial.println("\n=================================");
  Serial.printf("[📶 %s] [🌐 %s] [🕒 %s] [📩 SMS: %d/%d] [📞 CALLS: %d/%d]\n", 
    currentSignal.c_str(), currentOperator.c_str(), currentTime.c_str(),
    countLines(SMS_LOG_FILE), MAX_LOG_ENTRIES, countLines(CALL_LOG_FILE), MAX_LOG_ENTRIES);
  Serial.println("=================================");
  Serial.println("1. Read SMS Log");
  Serial.println("2. Read Call History");
  Serial.println("3. Send SMS");
  Serial.println("4. GPIO Control");
  Serial.println("5. Wipe Logs & SIM Memory");
  Serial.println("=================================");
  Serial.print("Select: ");
}

void handleSerialUI() {
  if (!Serial.available()) return;
  String input = Serial.readStringUntil('\n');
  input.trim();
  if (input.length() == 0) return;
  if (input == "0") { uiState = MAIN_MENU; printMenu(); return; }

  switch (uiState) {
    case MAIN_MENU:
      if (input == "1") { Serial.println("\n--- SMS Log ---"); printLog(SMS_LOG_FILE); }
      else if (input == "2") { Serial.println("\n--- Call Log ---"); printLog(CALL_LOG_FILE); }
      else if (input == "3") { uiState = SEND_SMS_NUM; Serial.print("\nEnter Destination Number: "); }
      else if (input == "4") { uiState = GPIO_MENU; printGpioTable(); }
      else if (input == "5") { LittleFS.format(); wipeModemMemory(); Serial.println("\n[DONE] Все логи и SIM очищены."); }
      if (uiState == MAIN_MENU) printMenu();
      break;

    case SEND_SMS_NUM:
      outSmsNum = input; uiState = SEND_SMS_MSG;
      Serial.print("Enter Message: ");
      break;

    case SEND_SMS_MSG:
      outSmsMsg = input; sendSMS(outSmsNum, outSmsMsg); 
      uiState = MAIN_MENU; printMenu();
      break;

    case GPIO_MENU:
      {
        int targetPin = input.toInt();
        bool valid = false;
        for(int p : ctrlPins) if(p == targetPin) valid = true;
        if (valid) { digitalWrite(targetPin, !digitalRead(targetPin)); printGpioTable(); }
        else { Serial.println("Invalid PIN!"); }
      }
      break;
  }
}

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
        int end = modemBuffer.indexOf("\"", start);
        String number = modemBuffer.substring(start, end);
        triggerEventNotification();
        Serial.println("\n[📞 ВХОДЯЩИЙ ЗВОНОК] От: " + number);
        appendToLog(CALL_LOG_FILE, "[" + currentTime + "] Rejected: " + number);
        sendAT("ATH"); 
        
        // Глубокая очистка списков звонков сразу
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
  for (int pin : ctrlPins) { pinMode(pin, OUTPUT); digitalWrite(pin, LOW); }
  if (!LittleFS.begin(true)) { Serial.println("LittleFS Mount Failed"); }
  initModem();
  lastStatusUpdate = millis();
  lastSmsCleanup = millis();
  printMenu();
}

void loop() {
  handleModemAsync();
  handleSerialUI();
  updateStatusBar();

  if (isBlinking) {
    if (millis() - blinkStart > 3000) { isBlinking = false; digitalWrite(STATUS_LED, LOW); }
    else { digitalWrite(STATUS_LED, (millis() / 200) % 2); }
  }
}