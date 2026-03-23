#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

const char* ssid = "4lykagpar";
const char* password = "Z@x@ri@s13";

String botToken = "8727291352:AAH8PJBY4_VD36KkyrizT9U8RYUc2KN0JuY";
String chatID = "8076737651";

const int sensor1Pin = 34;
const int sensor2Pin = 35;
const int ledPin = 2;
const int buzzerPin = 15;
const int valvePin = 4;
const int smallThreshold = 300;
const int bigThreshold   = 800;

int sensor1Value = 0;
int sensor2Value = 0;
int difference = 0;
bool smallLeak = false;
bool bigLeak = false;
bool lastSmallLeak = false;
bool lastBigLeak = false;

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(valvePin, OUTPUT);
  connectToWiFi();
}

void loop() {
  readSensors();
  processLogic();
  controlOutputs();
  handleAlerts();
}


void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("ΣΥΝΔΕΣΗ ΣΕ ΕΞΕΛΙΞΗ");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\n ΕΠΙΤΙΧΗΣ ΣΥΝΔΕΣΗ!");
}


void readSensors() {
  sensor1Value = analogRead(sensor1Pin);
  sensor2Value = analogRead(sensor2Pin);

  difference = abs(sensor1Value - sensor2Value);

  Serial.print("S1: "); Serial.print(sensor1Value);
  Serial.print(" | S2: "); Serial.print(sensor2Value);
  Serial.print(" | Diff: "); Serial.println(difference);
}


void processLogic() {

  if(difference > bigThreshold) {
    bigLeak = true;
    smallLeak = false;
  }
  else if(difference > smallThreshold) {
    smallLeak = true;
    bigLeak = false;
  }
  else {
    smallLeak = false;
    bigLeak = false;
  }
}

void controlOutputs() {

  if(bigLeak) {
    digitalWrite(valvePin, HIGH); 
    digitalWrite(buzzerPin, HIGH);
  } else {
    digitalWrite(valvePin, LOW); 
    digitalWrite(buzzerPin, LOW);
  }

  
  digitalWrite(ledPin, (smallLeak || bigLeak));
}

void handleAlerts() {

  
  if(smallLeak && !lastSmallLeak) {

    String message = "⚠️ Small leak detected\n";
    message += "Diff: " + String(difference) + "\n";
    message += "S1: " + String(sensor1Value) + "\n";
    message += "S2: " + String(sensor2Value);

    sendTelegramMessage(message);
  }

  
  if(bigLeak && !lastBigLeak) {

    String message = "🚨 MAJOR LEAK\nWATER SHUT OFF\n\n";
    message += "Diff: " + String(difference) + "\n";
    message += "S1: " + String(sensor1Value) + "\n";
    message += "S2: " + String(sensor2Value);

    sendTelegramMessage(message);
  }

  
  if(!smallLeak && !bigLeak && (lastSmallLeak || lastBigLeak)) {
    sendTelegramMessage("✅ Leak cleared");
  }

  lastSmallLeak = smallLeak;
  lastBigLeak = bigLeak;
}
void sendTelegramMessage(String text) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient https;

  String url = "https://api.telegram.org/bot" + botToken +
               "/sendMessage?chat_id=" + chatID +
               "&text=" + text;

  https.begin(client, url);

  int httpCode = https.GET();

  if (httpCode > 0) {
    Serial.println("το μηνυμα σταλθηκε");
  } else {
    Serial.println("εμφανιστικε προβλημα στην αποστολη μηνυματος");
  }

  https.end();
}