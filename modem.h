#pragma once

#include <Arduino.h>
#include <functional>

// One AT transaction owns the modem at a time. Unsolicited notifications
// are routed separately, including while a command is waiting for a reply.
class AsyncModem {
 public:
  using Callback = std::function<void(bool, const String&)>;
  using Notification = std::function<void(const String&)>;

  explicit AsyncModem(HardwareSerial& serial) : serial_(serial) {}
  Notification notification;

  bool enqueue(const String& command, uint32_t timeout, Callback callback,
               const String& smsBody = "") {
    if (count_ == capacity || command.length() > 512 || smsBody.length() > 280) return false;
    jobs_[(head_ + count_) % capacity] = {command, smsBody, timeout, callback};
    ++count_;
    return true;
  }

  bool idle() const { return !active_ && count_ == 0; }

  void tick() {
    // Bound each iteration so USB and output timers always receive CPU time.
    unsigned budget = 512;
    while (budget-- && serial_.available()) {
      char c = serial_.read();
      if (active_ && waitingPrompt_ && c == '>') {
        serial_.print(current_.body);
        serial_.write(26);
        waitingPrompt_ = false;
        line_ = "";
        started_ = millis();
        continue;
      }
      if (c == '\n') {
        line_.trim();
        consumeLine(line_);
        line_ = "";
      } else if (c != '\r') {
        if (line_.length() < 2048) line_ += c;
        else if (active_) finish(false, "Modem response is too long.");
      }
    }
    if (active_ && uint32_t(millis() - started_) >= current_.timeout) {
      // Never retry an outgoing SMS automatically: the network may have
      // accepted it even if its final acknowledgement was lost.
      if (current_.body.length()) serial_.write(27);
      finish(false, "Modem response timeout.");
      quietStarted_ = millis();
      cooling_ = true;
    }
    if (cooling_ && uint32_t(millis() - quietStarted_) >= 2000) cooling_ = false;
    if (!active_ && count_ && !cooling_) {
      current_ = jobs_[head_];
      jobs_[head_] = Job();
      head_ = (head_ + 1) % capacity;
      --count_;
      response_ = "";
      line_ = "";
      active_ = true;
      waitingPrompt_ = current_.body.length() != 0;
      started_ = millis();
      serial_.println(current_.command);
    }
  }

 private:
  struct Job {
    String command;
    String body;
    uint32_t timeout = 1000;
    Callback callback;
  };
  static constexpr uint8_t capacity = 8;
  HardwareSerial& serial_;
  Job jobs_[capacity], current_;
  uint8_t head_ = 0, count_ = 0;
  bool active_ = false, waitingPrompt_ = false, cooling_ = false;
  uint32_t started_ = 0, quietStarted_ = 0;
  String line_, response_;

  void finish(bool ok, const String& response) {
    Callback callback = current_.callback;
    active_ = false;
    waitingPrompt_ = false;
    current_ = Job();
    if (callback) callback(ok, response);
  }

  void consumeLine(const String& line) {
    if (!line.length()) return;
    if (line.startsWith("+CMTI:") || line.startsWith("+CLIP:") || line == "RDY") {
      if (notification) notification(line);
      return;
    }
    if (!active_) return;
    if (current_.command.startsWith("AT+CMGL=") && line.startsWith("+CMGL:")) {
      if (notification) notification(line);
      return;
    }
    if (current_.command.startsWith("AT+CMGL=") && line != "OK" && line != "ERROR" &&
        !line.startsWith("+CMS ERROR:") && !line.startsWith("+CME ERROR:")) return;
    if (line == "OK") {
      bool validSms = !current_.body.length() || response_.indexOf("+CMGS:") >= 0;
      finish(validSms, validSms ? response_ : "Missing SMS submission acknowledgement.");
    } else if (line == "ERROR" || line.startsWith("+CME ERROR:") || line.startsWith("+CMS ERROR:")) {
      finish(false, line);
    } else if (response_.length() + line.length() + 1 <= 4096) {
      response_ += line + "\n";
    } else {
      finish(false, "Modem response is too long.");
    }
  }
};
