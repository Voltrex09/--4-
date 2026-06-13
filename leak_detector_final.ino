#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

const char* WIFI_SSID     = "4lykagpar";
const char* WIFI_PASSWORD = "Z@x@ri@s13";

const String BOT_TOKEN = "8727291352:AAH8PJBY4_VD36KkyrizT9U8RYUc2KN0JuY";
const String CHAT_ID   = "8076737651";

const int SENSOR1_PIN = 35;
const int SENSOR2_PIN = 34;
const int LED_PIN     = 2;
const int BUZZER_PIN  = 14;

const uint32_t SMALL_LEAK_THRESHOLD = 5;
const uint32_t BIG_LEAK_THRESHOLD   = 13;

const uint32_t MAX_REALISTIC_DIFF = 200;
const uint32_t MIN_FLOW_TO_CHECK  = 5;
const uint32_t MAX_DIFF_JUMP      = 50;

const uint32_t MEASURE_WINDOW_MS    = 1000;
const uint32_t TELEGRAM_COOLDOWN_MS = 3000;
const uint32_t TELEGRAM_POLL_MS     = 3000;
const uint8_t  CONFIRM_WINDOWS      = 3;
const uint8_t  AVG_SAMPLES          = 7;
const uint8_t  HISTORY_SIZE         = 10;

volatile uint32_t pulseCount1 = 0;
volatile uint32_t pulseCount2 = 0;

uint32_t flow1 = 0;
uint32_t flow2 = 0;

uint32_t diffHistory[AVG_SAMPLES] = {0};
uint8_t  diffIndex                = 0;

struct LogEntry {
  String   message;
  uint32_t timestamp;
};
LogEntry eventLog[HISTORY_SIZE];
uint8_t  logIndex = 0;
uint8_t  logCount = 0;

enum LeakState { NO_LEAK, SMALL_LEAK, BIG_LEAK };
LeakState currentLeak   = NO_LEAK;
LeakState confirmedLeak = NO_LEAK;
LeakState previousLeak  = NO_LEAK;

uint8_t  stableCount      = 0;
uint32_t lastMeasureTime  = 0;
uint32_t lastTelegramTime = 0;
uint32_t lastPollTime     = 0;
uint32_t bootTime         = 0;
long     lastUpdateId     = 0;
bool     muted            = false;
uint32_t noiseIgnoreCount = 0;
uint32_t totalLeakEvents  = 0;

uint32_t smallThreshold = SMALL_LEAK_THRESHOLD;
uint32_t bigThreshold   = BIG_LEAK_THRESHOLD;

void IRAM_ATTR onPulse1() { pulseCount1++; }
void IRAM_ATTR onPulse2() { pulseCount2++; }

void logEvent(const String& msg) {
  eventLog[logIndex].message   = msg;
  eventLog[logIndex].timestamp = millis();
  logIndex = (logIndex + 1) % HISTORY_SIZE;
  if (logCount < HISTORY_SIZE) logCount++;
}

String formatUptime(uint32_t ms) {
  uint32_t seconds = ms / 1000;
  uint32_t minutes = seconds / 60;
  uint32_t hours   = minutes / 60;
  seconds %= 60;
  minutes %= 60;
  char buf[32];
  sprintf(buf, "%uh %um %us", hours, minutes, seconds);
  return String(buf);
}

uint32_t calcMedian() {
  uint32_t sorted[AVG_SAMPLES];
  for (uint8_t i = 0; i < AVG_SAMPLES; i++) sorted[i] = diffHistory[i];
  for (uint8_t i = 1; i < AVG_SAMPLES; i++) {
    uint32_t key = sorted[i];
    int8_t   j   = i - 1;
    while (j >= 0 && sorted[j] > key) {
      sorted[j + 1] = sorted[j];
      j--;
    }
    sorted[j + 1] = key;
  }
  return sorted[AVG_SAMPLES / 2];
}

void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nWiFi failed - will retry in loop");
  }
}

void ensureWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi lost, reconnecting...");
    WiFi.disconnect();
    connectWiFi();
  }
}

void sendTelegram(const String& text) {
  uint32_t now = millis();
  if (now - lastTelegramTime < TELEGRAM_COOLDOWN_MS) {
    Serial.println("Telegram cooldown active, skipping");
    return;
  }
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage";

  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/json");
    String body = "{\"chat_id\":\"" + CHAT_ID + "\",\"text\":\"" + text + "\"}";
    int code = https.POST(body);
    Serial.println("Telegram HTTP " + String(code));
    https.end();
    lastTelegramTime = now;
  } else {
    Serial.println("Telegram: https.begin() failed");
  }
}

void sendTelegramImmediate(const String& text) {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage";

  if (https.begin(client, url)) {
    https.addHeader("Content-Type", "application/json");
    String body = "{\"chat_id\":\"" + CHAT_ID + "\",\"text\":\"" + text + "\"}";
    int code = https.POST(body);
    Serial.println("Telegram immediate HTTP " + String(code));
    https.end();
  } else {
    Serial.println("Telegram immediate: https.begin() failed");
  }
}

void handleStatus() {
  String leakStr = (confirmedLeak == BIG_LEAK)   ? "MAJOR LEAK" :
                   (confirmedLeak == SMALL_LEAK) ? "Minor leak"  : "Normal";

  String msg = "--- SYSTEM STATUS ---"
               "\nLeak state: "      + leakStr +
               "\nSensor 1: "        + String(flow1) + " pulses/s"
               "\nSensor 2: "        + String(flow2) + " pulses/s"
               "\nAlerts muted: "    + String(muted ? "Yes" : "No") +
               "\nNoise rejected: "  + String(noiseIgnoreCount) +
               "\nLeak events: "     + String(totalLeakEvents) +
               "\nUptime: "          + formatUptime(millis() - bootTime);

  sendTelegramImmediate(msg);
}

void handleFlow() {
  uint32_t rawDiff = (flow1 > flow2) ? flow1 - flow2 : flow2 - flow1;
  uint32_t median  = calcMedian();

  String msg = "--- FLOW DATA ---"
               "\nSensor 1: "        + String(flow1)   + " pulses/s"
               "\nSensor 2: "        + String(flow2)   + " pulses/s"
               "\nRaw diff: "        + String(rawDiff)  + " pulses/s"
               "\nFiltered diff: "   + String(median)   + " pulses/s"
               "\nMinor threshold: " + String(smallThreshold) +
               "\nMajor threshold: " + String(bigThreshold);

  sendTelegramImmediate(msg);
}

void handleHistory() {
  if (logCount == 0) {
    sendTelegramImmediate("No events logged yet.");
    return;
  }

  String msg    = "--- LAST " + String(logCount) + " EVENTS ---\n";
  uint8_t start = (logCount < HISTORY_SIZE) ? 0 : logIndex;

  for (uint8_t i = 0; i < logCount; i++) {
    uint8_t idx = (start + i) % HISTORY_SIZE;
    msg += formatUptime(eventLog[idx].timestamp) + ": " + eventLog[idx].message + "\n";
  }

  sendTelegramImmediate(msg);
}

void handleReset() {
  noiseIgnoreCount = 0;
  totalLeakEvents  = 0;
  logCount         = 0;
  logIndex         = 0;
  stableCount      = 0;
  currentLeak      = NO_LEAK;
  confirmedLeak    = NO_LEAK;
  previousLeak     = NO_LEAK;
  diffIndex        = 0;
  for (uint8_t i = 0; i < AVG_SAMPLES; i++) diffHistory[i] = 0;
  digitalWrite(LED_PIN,    LOW);
  digitalWrite(BUZZER_PIN, LOW);
  sendTelegramImmediate("System reset. All stats cleared.");
  logEvent("Manual reset via Telegram");
  Serial.println("System reset via Telegram");
}

void handleMute() {
  muted = true;
  digitalWrite(BUZZER_PIN, LOW);
  sendTelegramImmediate("Alerts muted. Monitoring continues.\nSend /unmute to re-enable.");
  logEvent("Alerts muted");
}

void handleUnmute() {
  muted = false;
  sendTelegramImmediate("Alerts unmuted.");
  logEvent("Alerts unmuted");
}

void handleUptime() {
  String msg = "--- SYSTEM HEALTH ---"
               "\nUptime: "    + formatUptime(millis() - bootTime) +
               "\nWiFi RSSI: " + String(WiFi.RSSI()) + " dBm"
               "\nFree heap: " + String(ESP.getFreeHeap()) + " bytes"
               "\nIP: "        + WiFi.localIP().toString();
  sendTelegramImmediate(msg);
}

void handleSensitivity(const String& args) {
  int space1 = args.indexOf(' ');
  int space2 = (space1 != -1) ? args.indexOf(' ', space1 + 1) : -1;

  if (space1 == -1 || space2 == -1) {
    sendTelegramImmediate(
      "Usage: /sensitivity [minor] [major]\n"
      "Example: /sensitivity 25 70\n"
      "Current: minor=" + String(smallThreshold) + " major=" + String(bigThreshold)
    );
    return;
  }

  uint32_t newSmall = args.substring(space1 + 1, space2).toInt();
  uint32_t newBig   = args.substring(space2 + 1).toInt();

  if (newSmall < 5 || newBig < 10 || newSmall >= newBig) {
    sendTelegramImmediate("Invalid values.\nMinor must be < major.\nMinimums: minor=5 major=10");
    return;
  }

  smallThreshold = newSmall;
  bigThreshold   = newBig;

  String msg = "Thresholds updated.\nMinor: " + String(smallThreshold) +
               "\nMajor: " + String(bigThreshold);
  sendTelegramImmediate(msg);
  logEvent("Thresholds changed: minor=" + String(smallThreshold) + " major=" + String(bigThreshold));
}

void handleHelp() {
  String msg =
    "--- COMMANDS ---\n"
    "/status              System overview\n"
    "/flow                Live sensor readings\n"
    "/history             Last 10 events\n"
    "/uptime              Health + WiFi + heap\n"
    "/mute                Silence all alerts\n"
    "/unmute              Re-enable alerts\n"
    "/sensitivity X Y     Set minor/major thresholds\n"
    "/reset               Clear all stats\n"
    "/help                This message";
  sendTelegramImmediate(msg);
}

void pollTelegram() {
  ensureWiFi();
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  String url = "https://api.telegram.org/bot" + BOT_TOKEN
             + "/getUpdates?timeout=0&offset=" + String(lastUpdateId + 1);

  if (!https.begin(client, url)) return;

  int code = https.GET();
  if (code != 200) { https.end(); return; }

  String payload = https.getString();
  https.end();

  int pos = 0;
  while (true) {
    int uidIdx = payload.indexOf("\"update_id\":", pos);
    if (uidIdx == -1) break;

    int  uidStart = uidIdx + 12;
    int  uidEnd   = payload.indexOf(",", uidStart);
    long uid      = payload.substring(uidStart, uidEnd).toInt();

    int textIdx = payload.indexOf("\"text\":\"", uidStart);
    int nextUid = payload.indexOf("\"update_id\":", uidStart + 1);

    if (textIdx != -1 && (nextUid == -1 || textIdx < nextUid)) {
      int    textStart = textIdx + 8;
      int    textEnd   = payload.indexOf("\"", textStart);
      String text      = payload.substring(textStart, textEnd);

      Serial.println("Command: " + text);

      if      (text == "/status")               handleStatus();
      else if (text == "/flow")                 handleFlow();
      else if (text == "/history")              handleHistory();
      else if (text == "/reset")                handleReset();
      else if (text == "/mute")                 handleMute();
      else if (text == "/unmute")               handleUnmute();
      else if (text == "/uptime")               handleUptime();
      else if (text == "/help")                 handleHelp();
      else if (text.startsWith("/sensitivity")) handleSensitivity(text);
      else sendTelegramImmediate("Unknown command. Send /help for the list.");
    }

    lastUpdateId = uid;
    pos          = uidEnd;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(SENSOR1_PIN, INPUT_PULLDOWN);
  pinMode(SENSOR2_PIN, INPUT_PULLDOWN);
  pinMode(LED_PIN,     OUTPUT);
  pinMode(BUZZER_PIN,  OUTPUT);

  attachInterrupt(digitalPinToInterrupt(SENSOR1_PIN), onPulse1, RISING);
  attachInterrupt(digitalPinToInterrupt(SENSOR2_PIN), onPulse2, RISING);

  connectWiFi();

  bootTime        = millis();
  lastMeasureTime = millis();
  lastPollTime    = millis();

  sendTelegramImmediate("System online. Send /help for commands.");
  logEvent("System boot");

  Serial.println("System ready. Monitoring...");
}

void loop() {
  uint32_t now = millis();

  if (now - lastPollTime >= TELEGRAM_POLL_MS) {
    lastPollTime = now;
    pollTelegram();
  }

  if (now - lastMeasureTime >= MEASURE_WINDOW_MS) {
    lastMeasureTime = now;

    noInterrupts();
    flow1 = pulseCount1;
    flow2 = pulseCount2;
    pulseCount1 = 0;
    pulseCount2 = 0;
    interrupts();

    uint32_t rawDiff = (flow1 > flow2) ? (flow1 - flow2) : (flow2 - flow1);

    if (rawDiff > MAX_REALISTIC_DIFF) {
      Serial.printf("NOISE: rawDiff %u > max %u  [ignored]\n", rawDiff, MAX_REALISTIC_DIFF);
      noiseIgnoreCount++;
      stableCount = 0;
      return;
    }

    if (flow1 < MIN_FLOW_TO_CHECK && flow2 < MIN_FLOW_TO_CHECK) {
      Serial.printf("IDLE: flow1=%u flow2=%u both near zero  [ignored]\n", flow1, flow2);
      stableCount = 0;
      return;
    }

    uint32_t lastDiff = diffHistory[(diffIndex + AVG_SAMPLES - 1) % AVG_SAMPLES];
    if (lastDiff > 0 && rawDiff > lastDiff + MAX_DIFF_JUMP) {
      Serial.printf("JUMP: %u -> %u exceeds max jump %u  [ignored]\n", lastDiff, rawDiff, MAX_DIFF_JUMP);
      noiseIgnoreCount++;
      stableCount = 0;
      return;
    }

    diffHistory[diffIndex] = rawDiff;
    diffIndex = (diffIndex + 1) % AVG_SAMPLES;

    uint32_t diff = calcMedian();

    Serial.printf("Flow1: %u  Flow2: %u  Raw: %u  Median: %u  Stable: %u  Candidate: %d  Confirmed: %d\n",
                  flow1, flow2, rawDiff, diff, stableCount, currentLeak, confirmedLeak);

    LeakState newLeak;
    if      (diff >= bigThreshold)   newLeak = BIG_LEAK;
    else if (diff >= smallThreshold) newLeak = SMALL_LEAK;
    else                             newLeak = NO_LEAK;

    if (newLeak == currentLeak) {
      stableCount++;
    } else {
      currentLeak = newLeak;
      stableCount = 0;
    }

    if (stableCount >= CONFIRM_WINDOWS) {
      previousLeak  = confirmedLeak;
      confirmedLeak = currentLeak;

      digitalWrite(LED_PIN, confirmedLeak != NO_LEAK ? HIGH : LOW);
      if (!muted) digitalWrite(BUZZER_PIN, confirmedLeak == BIG_LEAK ? HIGH : LOW);

      if (confirmedLeak != previousLeak) {
        totalLeakEvents++;

        if (confirmedLeak == BIG_LEAK) {
          String msg = "MAJOR LEAK detected!"
                       "\nSensor 1: " + String(flow1) +
                       "\nSensor 2: " + String(flow2) +
                       "\nDiff: "     + String(diff);
          if (!muted) sendTelegram(msg);
          logEvent("MAJOR LEAK - diff=" + String(diff));

        } else if (confirmedLeak == SMALL_LEAK && previousLeak != BIG_LEAK) {
          String msg = "Minor leak detected."
                       "\nSensor 1: " + String(flow1) +
                       "\nSensor 2: " + String(flow2) +
                       "\nDiff: "     + String(diff);
          if (!muted) sendTelegram(msg);
          logEvent("Minor leak - diff=" + String(diff));

        } else if (confirmedLeak == NO_LEAK) {
          if (!muted) sendTelegram("Leak cleared. System normal.");
          logEvent("Leak cleared");
          digitalWrite(BUZZER_PIN, LOW);
        }
      }
    }
  }
}