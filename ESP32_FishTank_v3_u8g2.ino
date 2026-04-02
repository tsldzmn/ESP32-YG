// ============================================================
// ESP32 智能鱼缸 - 完整版 v3（u8g2 中文显示 WQY字库）
//
// 硬件：ESP32S / DS18B20 / HC-SR04 / WAVGAT浑浊度
// 光敏电阻DO / SG90舵机 / TMB12A蜂鸣器 / OLED 0.96 128x64 I2C
// 轻触按键x3 / 面包板 / 4.7K电阻 / 杜邦线
//
// 接线：
// DS18B20:   GPIO4 (4.7K上拉到3V3) / HC-SR04: TRIG GPIO5 ECHO GPIO18
// WAVGAT AO: GPIO36 / 光敏DO: GPIO32 / 舵机: GPIO16
// 蜂鸣器: GPIO15 / OLED: SDA GPIO21 SCL GPIO22
// 按键喂食: GPIO23 / 按键灯光: GPIO19 / 按键报警: GPIO17
// ============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <U8g2lib.h>
#include <ESP32Servo.h>
#include <Wire.h>

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

OneWire oneWire(PIN_TEMP_DATA);
DallasTemperature tempSensor(&oneWire);

// 0.96" 128x64 I2C OLED, 地址默认0x3C
U8G2_SSD1306_128X64_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

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

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int r = 0;
  while (WiFi.status() != WL_CONNECTED && r < 30) { delay(500); r++; }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK IP:"); Serial.println(WiFi.localIP());
  }
}

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
  } else if (!a && alarmActive) {
    alarmActive = false; alarmMuted = false;
  }
}

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
    if (lr == HIGH && !lightOn) { lightOn = true; }
    else if (lr == LOW && lightOn) { lightOn = false; }
  }
  checkAlarm();
}

void triggerFeeder() {
  if (!feederOn) {
    feederOn = true;
    feederOffTime = millis() + 3000;
    feederServo.write(90);
  }
}

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
  http.POST(p);
  http.end();
}

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
      if (nl != lightOn) lightOn = nl;
      if (nf && !feederOn) triggerFeeder();
    }
  }
  http.end();
}

void checkButtons() {
  unsigned long n = millis();
  if (digitalRead(PIN_BTN_FEEDER) == LOW && n - lastBtnFeed > DEBOUNCE_MS) {
    lastBtnFeed = n; triggerFeeder(); beep(1, 80);
  }
  if (digitalRead(PIN_BTN_LIGHT) == LOW && n - lastBtnLight > DEBOUNCE_MS) {
    lastBtnLight = n; lightOn = !lightOn; beep(1, 80);
  }
  if (digitalRead(PIN_BTN_ALARM) == LOW && n - lastBtnAlarm > DEBOUNCE_MS) {
    lastBtnAlarm = n; alarmMuted = true; digitalWrite(PIN_BUZZER, LOW);
  }
}

// ============================================================
// OLED 显示布局（128x64像素）
// 字体说明：
//   u8g2_font_wqy12_t_gb2312a = 文泉驿12x12中文字（宽12高12，每字占12px）
//   u8g2_font_10x20_tf = 英文大字10x20（数字/英文专用）
//   u8g2_font_6x10_tf = 英文小字6x10
//
// 坐标布局（Y是字符底部坐标）：
//   Y=10  = 第1行底部（6x10小字，占0-10px）
//   Y=12  = 第2行小标签底部（12x12中字，占11-23px）
//   Y=44  = 第3行大数字底部（10x20大字，占24-44px）
//   Y=56  = 第4行小标签底部（12x12中字，占45-56px）
//   Y=64  = 第5行底部（6x10小字，占57-64px）
//
// 布局：
// ┌──────────────────────────┐
// │ESP32-FishTank       WiFi │ y=0-10  6x10小字
// │──────────────────────────│ y=11    分割线
// │ 温度   (12px中文)        │ y=12-23 12x12中字
// │ 25.5C    8cm         LOW │ y=24-44 10x20大字
// │ 浑浊度  (12px中文)        │ y=45-56 12x12中字
// │ Standby  - L -           │ y=57-64 6x10小字
// └──────────────────────────┘

void updateScreen() {
  u8g2.clearBuffer();

  // ===== 第1行：标题（6x10小字） =====
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 10, "ESP32-FishTank");
  u8g2.drawStr(90, 10, (WiFi.status() == WL_CONNECTED) ? "WiFi" : "NO WIFI");

  // ===== 分割线 =====
  u8g2.drawHLine(0, 12, 128, 1);

  // ===== 第2行：中文标签（12x12中字） =====
  u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
  u8g2.drawUTF8(0, 24, "温度");
  u8g2.drawUTF8(70, 24, "距离");

  // ===== 第3行：大数字（10x20英文字） =====
  u8g2.setFont(u8g2_font_10x20_tf);
  char buf[16];
  sprintf(buf, "%.1f", waterTemp);
  u8g2.drawStr(0, 44, buf);
  // 度符号小圆圈
  u8g2.drawCircle(46, 32, 2);

  sprintf(buf, "%dcm", waterLevel);
  u8g2.drawStr(68, 44, buf);

  // LOW警告
  if (waterLevel > 10) {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(108, 44, "LOW");
  }

  // ===== 第4行：中文标签（12x12中字） =====
  u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
  u8g2.drawUTF8(0, 56, "浑浊度");
  u8g2.drawUTF8(70, 56, "光照");

  // ===== 分割线 =====
  u8g2.drawHLine(0, 58, 128, 1);

  // ===== 第5行：状态栏（6x10小字） =====
  u8g2.setFont(u8g2_font_6x10_tf);
  if (alarmActive) {
    u8g2.drawStr(0, 64, "!ALARM");
  } else if (feederOn) {
    u8g2.drawStr(0, 64, "Feeding");
  } else if (lightOn) {
    u8g2.drawStr(0, 64, "Light ON");
  } else {
    u8g2.drawStr(0, 64, "Standby");
  }
  u8g2.drawStr(80, 64, feederOn ? "F" : "-");
  u8g2.drawStr(96, 64, lightOn ? "L" : "-");
  u8g2.drawStr(112, 64, alarmActive ? "!" : " ");

  u8g2.sendBuffer();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BTN_FEEDER, INPUT_PULLUP);
  pinMode(PIN_BTN_LIGHT, INPUT_PULLUP);
  pinMode(PIN_BTN_ALARM, INPUT_PULLUP);
  analogReadResolution(12);
  tempSensor.begin();

  Wire.begin(PIN_SDA, PIN_SCL);
  u8g2.begin();
  // 开机画面
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_wqy12_t_gb2312a);
  u8g2.drawUTF8(24, 28, "ESP32智能鱼缸");
  u8g2.drawUTF8(36, 48, "正在启动...");
  u8g2.sendBuffer();
  delay(2000);

  feederServo.attach(PIN_SERVO);
  feederServo.write(0);
  beep(2, 100);
  connectWiFi();
}

void loop() {
  unsigned long n = millis();
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (feederOn && n >= feederOffTime) {
    feederOn = false; feederServo.write(0);
  }
  checkButtons();
  if (n - lastRead >= READ_INTERVAL) {
    readSensors(); updateScreen(); lastRead = n;
  }
  if (n - lastReport >= REPORT_INTERVAL) {
    reportData(); lastReport = n;
  }
  if (n - lastCommand >= COMMAND_INTERVAL) {
    pullCommand(); lastCommand = n;
  }
  delay(50);
}
