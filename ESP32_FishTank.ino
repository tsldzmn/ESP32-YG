// ============================================================
// ESP32 智能鱼缸 - 完整版 v4（Adafruit SSD1306 英文版）
//
// 硬件：ESP32S / DS18B20 / HC-SR04 / WAVGAT浑浊度
// 光敏电阻DO / SG90舵机 / TMB12A蜂鸣器 / OLED 0.96 128x64 I2C
// 轻触按键x3
//
// 接线：
// DS18B20:   GPIO4 (4.7K上拉到3V3)
// HC-SR04:   TRIG GPIO5, ECHO GPIO18
// WAVGAT AO: GPIO36
// 光敏DO:    GPIO32
// 舵机:      GPIO16
// 蜂鸣器:    GPIO15
// OLED I2C:  SDA GPIO21, SCL GPIO22
// 按键喂食:  GPIO23
// 按键灯光:  GPIO19
// 按键报警:  GPIO17
// ============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>

const char* WIFI_SSID     = "FFF";
const char* WIFI_PASSWORD = "cs123456";
const char* SERVER_URL = "https://esp32-yg.onrender.com";

#define PIN_TEMP_DATA  4
#define PIN_TRIG       5
#define PIN_ECHO       18
#define PIN_TURBIDITY  36
#define PIN_LIGHT_DO   32
#define PIN_SERVO      16
#define PIN_BUZZER     15
#define PIN_BTN_FEEDER 23
#define PIN_BTN_LIGHT  19
#define PIN_BTN_ALARM  17

#define REPORT_INTERVAL  10000
#define COMMAND_INTERVAL  5000
#define READ_INTERVAL     2000
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT     64

OneWire oneWire(PIN_TEMP_DATA);
DallasTemperature tempSensor(&oneWire);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Servo feederServo;

float waterTemp = 0;
int   waterLevel = 0;
int   turbidity = 0;
bool  autoLightOn = true;
bool  lightOn = false;
bool  feederOn = false;
bool  alarmActive = false;
bool  alarmMuted = false;
unsigned long feederOffTime = 0;
unsigned long lastRead = 0;
unsigned long lastReport = 0;
unsigned long lastCommand = 0;
unsigned long lastBtnFeed = 0;
unsigned long lastBtnLight = 0;
unsigned long lastBtnAlarm = 0;
#define DEBOUNCE_MS 300

// ==================== WiFi ====================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int r = 0;
  while (WiFi.status() != WL_CONNECTED && r < 30) { delay(500); r++; }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK IP:"); Serial.println(WiFi.localIP());
  } else {
    delay(5000);
    connectWiFi();
  }
}

// ==================== Buzzer ====================
void beep(int t, int ms) {
  for (int i = 0; i < t; i++) {
    digitalWrite(PIN_BUZZER, HIGH); delay(ms);
    digitalWrite(PIN_BUZZER, LOW); if (i < t - 1) delay(ms);
  }
}

void checkAlarm() {
  bool a = false;
  if (waterTemp > 30 || (waterTemp > 0 && waterTemp < 18)) a = true;
  if (waterLevel > 10) a = true;
  if (turbidity > 80) a = true;
  if (a && !alarmMuted && !alarmActive) {
    alarmActive = true; beep(3, 150);
    Serial.println("ALARM!");
  } else if (!a && alarmActive) {
    alarmActive = false; alarmMuted = false;
  }
}

// ==================== Sensors ====================
float readTemperature() {
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempCByIndex(0);
  if (t != DEVICE_DISCONNECTED_C && t > -10 && t < 80) return t;
  return waterTemp;
}

float readUltrasonicCM() {
  digitalWrite(PIN_TRIG, LOW); delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long d = pulseIn(PIN_ECHO, HIGH, 30000);
  if (d == 0) return -1;
  return d * 0.034 / 2;
}

void readSensors() {
  float t = readTemperature();
  if (t > 0) waterTemp = t;
  float dist = readUltrasonicCM();
  if (dist >= 0) waterLevel = (int)dist;
  int tr = analogRead(PIN_TURBIDITY);
  turbidity = constrain(map(tr, 500, 3500, 0, 100), 0, 100);
  if (autoLightOn) {
    int lr = digitalRead(PIN_LIGHT_DO);
    if (lr == HIGH && !lightOn) { lightOn = true; Serial.println("Auto Light ON"); }
    else if (lr == LOW && lightOn) { lightOn = false; Serial.println("Auto Light OFF"); }
  }
  checkAlarm();
  Serial.printf("T=%.1fC Dist=%dcm Turb=%d %s\n", waterTemp, waterLevel, turbidity, alarmActive ? "ALARM" : "OK");
}

// ==================== Feeder ====================
void triggerFeeder() {
  if (!feederOn) {
    feederOn = true;
    feederOffTime = millis() + 3000;
    feederServo.write(90);
    Serial.println("Feeding...");
  }
}

// ==================== Report ====================
void reportData() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/esp32/report");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000);
  StaticJsonDocument<128> doc;
  doc["waterTemp"] = round(waterTemp * 10) / 10.0;
  doc["waterLevel"] = waterLevel;
  doc["turbidity"] = turbidity;
  String p; serializeJson(doc, p);
  int code = http.POST(p);
  Serial.printf("Report: %s [%d]\n", p.c_str(), code);
  http.end();
}

// ==================== Pull command ====================
void pullCommand() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/esp32/command");
  http.setTimeout(3000);
  if (http.GET() == 200) {
    String p = http.getString();
    StaticJsonDocument<256> d;
    if (!deserializeJson(d, p)) {
      bool nl = d["data"]["light"] | false;
      bool nf = d["data"]["feeder"] | false;
      if (nl != lightOn) { lightOn = nl; Serial.printf("Remote light: %s\n", lightOn ? "ON" : "OFF"); }
      if (nf && !feederOn) triggerFeeder();
    }
  }
  http.end();
}

// ==================== Buttons ====================
void checkButtons() {
  unsigned long n = millis();
  if (digitalRead(PIN_BTN_FEEDER) == LOW && n - lastBtnFeed > DEBOUNCE_MS) {
    lastBtnFeed = n; triggerFeeder(); beep(1, 80);
  }
  if (digitalRead(PIN_BTN_LIGHT) == LOW && n - lastBtnLight > DEBOUNCE_MS) {
    lastBtnLight = n; lightOn = !lightOn; beep(1, 80);
    Serial.printf("Manual light: %s\n", lightOn ? "ON" : "OFF");
  }
  if (digitalRead(PIN_BTN_ALARM) == LOW && n - lastBtnAlarm > DEBOUNCE_MS) {
    lastBtnAlarm = n; alarmMuted = true; digitalWrite(PIN_BUZZER, LOW);
    Serial.println("Alarm muted");
  }
}

// ==================== OLED Display ====================
// Layout (128x64):
// ┌──────────────────────────────┐
// │ ESP32-FishTank        WiFi  │  Row1: title (size1, 6x10)
// │──────────────────────────────│  Line
// │ Temp:  25.5C                 │  Row2: temp (size2, 10x20 big)
// │ Dist:   8cm  LOW!            │  Row3: dist (size2, 10x20 big)
// │──────────────────────────────│  Line
// │ Turb:33  Light:Day  Feeding │  Row4: info (size1, 6x10)
// │──────────────────────────────│  Line
// │  Standby  -  L  -            │  Row5: status (size1, 6x10)
// └──────────────────────────────┘

void updateScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Row 1: Title
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("ESP32-FishTank");
  display.setCursor(90, 0);
  display.print(WiFi.status() == WL_CONNECTED ? "WiFi" : "NO WIFI");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  // Row 2: Temperature (big)
  display.setTextSize(1);
  display.setCursor(0, 14);
  display.print("Temp:");
  display.setTextSize(2);
  display.setCursor(42, 14);
  display.printf("%.1f", waterTemp);
  display.setTextSize(1);
  display.setCursor(96, 14);
  display.print("C");

  // Row 3: Distance (big)
  display.setTextSize(1);
  display.setCursor(0, 34);
  display.print("Dist:");
  display.setTextSize(2);
  display.setCursor(42, 34);
  display.printf("%d", waterLevel);
  display.setTextSize(1);
  display.setCursor(72, 34);
  display.print("cm");
  if (waterLevel > 10) {
    display.setCursor(100, 34);
    display.setTextSize(1);
    display.print("LOW!");
  }

  display.drawLine(0, 52, 127, 52, SSD1306_WHITE);

  // Row 4: Info
  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print("T:");
  display.print(turbidity);
  display.setCursor(40, 56);
  display.print(digitalRead(PIN_LIGHT_DO) == LOW ? "Day" : "Night");
  display.setCursor(80, 56);
  if (alarmActive) display.print("!ALM");
  else if (feederOn) display.print("Feed");
  else if (lightOn) display.print("Lamp");
  else display.print("OK");

  display.display();
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32-FishTank v4 starting...\n");

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BTN_FEEDER, INPUT_PULLUP);
  pinMode(PIN_BTN_LIGHT, INPUT_PULLUP);
  pinMode(PIN_BTN_ALARM, INPUT_PULLUP);
  analogReadResolution(12);
  tempSensor.begin();
  Serial.printf("Temp sensors: %d\n", tempSensor.getDeviceCount());

  Wire.begin(21, 22);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 20);
    display.println("ESP32-FishTank");
    display.setCursor(25, 35);
    display.println("Booting...");
    display.display();
    Serial.println("OLED OK");
  }

  feederServo.attach(PIN_SERVO);
  feederServo.write(0);
  delay(500);
  Serial.println("Servo OK");

  beep(2, 100);
  connectWiFi();
  Serial.println("\nSystem ready!\n");
}

// ==================== Loop ====================
void loop() {
  unsigned long n = millis();

  if (WiFi.status() != WL_CONNECTED) connectWiFi();

  if (feederOn && n >= feederOffTime) {
    feederOn = false;
    feederServo.write(0);
    Serial.println("Feeding done");
  }

  checkButtons();

  if (n - lastRead >= READ_INTERVAL) {
    readSensors();
    updateScreen();
    lastRead = n;
  }

  if (n - lastReport >= REPORT_INTERVAL) {
    reportData();
    lastReport = n;
  }

  if (n - lastCommand >= COMMAND_INTERVAL) {
    pullCommand();
    lastCommand = n;
  }

  delay(50);
}
