// ============================================================
// ESP32 智能鱼缸 - 完整版 v2（精确匹配硬件，无继电器）
//
// 你的硬件清单：
// ✅ ESP32S NodeMCU 开发板
// ✅ DS18B20 防水温度传感器（4.7K上拉电阻）
// ✅ HC-SR04 超声波测距模块（测水位）
// ✅ WAVGAT 浑浊度传感器（AO模拟量）
// ✅ 光敏电阻传感器模块（AO模拟量）
// ✅ SG90 舵机 180度（自动喂食）
// ✅ TMB12A 有源蜂鸣器 5V（报警）
// ✅ 0.96寸 OLED 显示屏 I2C
// ✅ 轻触按键 6x6x5 x10（手动控制）
// ✅ MB-102 面包板 + 供电模块
// ✅ 4.7K 欧姆电阻
// ✅ 杜邦线 + USB数据线
//
// 接线表：
// DS18B20:     VCC→3.3V, GND→GND, DATA→GPIO4 (加4.7K上拉到VCC)
// HC-SR04:     VCC→5V, GND→GND, TRIG→GPIO5, ECHO→GPIO18
// WAVGAT浑浊度: VCC→3.3V, GND→GND, AO→GPIO36
// 光敏电阻模块:  VCC→3.3V, GND→GND, DO→GPIO32(数字量)
// SG90舵机:     VCC→5V(面包板), GND→GND, 信号→GPIO16
// TMB12A蜂鸣器: VCC→5V, GND→GND, IN→GPIO15
// OLED I2C:    VCC→3.3V, GND→GND, SDA→GPIO21, SCL→GPIO22
// 按键1(喂食):  一端→GPIO23, 另一端→GND (内部上拉)
// 按键2(灯光):  一端→GPIO19, 另一端→GND (内部上拉)
// 按键3(报警关): 一端→GPIO17, 另一端→GND (内部上拉)
//
// 传感器自动报警：
// - 水温 > 30°C 或 < 18°C → 蜂鸣器报警
// - 水位 < 20% → 蜂鸣器报警
// - 浑浊度 > 80 NTU → 蜂鸣器报警
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

// ==================== 用户配置 ====================
const char* WIFI_SSID     = "FFF";
const char* WIFI_PASSWORD = "cs123456";

// 服务器地址（先填电脑局域网IP，部署云端后改成render域名）
const char* SERVER_URL = "https://esp32-yg.onrender.com";
// const char* SERVER_URL = "https://你的服务名.onrender.com"; // 部署到云端

// ==================== 引脚定义 ====================
#define PIN_TEMP_DATA     4      // DS18B20 数据
#define PIN_TRIG          5      // HC-SR04 触发
#define PIN_ECHO          18     // HC-SR04 回波
#define PIN_TURBIDITY     36     // 浑浊度传感器 AO (WAVGAT)
#define PIN_LIGHT_DO      32     // 光敏电阻传感器 DO（数字量）
#define PIN_SERVO         16     // SG90 舵机信号
#define PIN_BUZZER        15     // TMB12A 蜂鸣器
#define PIN_BTN_FEEDER    23     // 按键：手动喂食
#define PIN_BTN_LIGHT     19     // 按键：手动灯光
#define PIN_BTN_ALARM     17     // 按键：关闭报警
#define PIN_SDA           21     // OLED SDA
#define PIN_SCL           22     // OLED SCL

// ==================== 参数 ====================
#define REPORT_INTERVAL   10000
#define COMMAND_INTERVAL   5000
#define READ_INTERVAL      2000
#define SCREEN_WIDTH      128
#define SCREEN_HEIGHT      64

// ==================== 全局对象 ====================
OneWire oneWire(PIN_TEMP_DATA);
DallasTemperature tempSensor(&oneWire);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Servo feederServo;

// ==================== 数据 ====================
float waterTemp = 0;
int   waterLevel = 0;
int   turbidity = 0;
int   lightLux = 0;
bool  autoLightOn = true;
bool  lightOn = false;
bool  feederOn = false;
bool  alarmActive = false;
bool  alarmMuted = false;    // 用户手动静音
unsigned long feederOffTime = 0;
unsigned long lastRead = 0;
unsigned long lastReport = 0;
unsigned long lastCommand = 0;
int  wifiRetry = 0;

// 按键防抖
unsigned long lastBtnFeed = 0;
unsigned long lastBtnLight = 0;
unsigned long lastBtnAlarm = 0;
#define DEBOUNCE_MS 300

// ==================== WiFi ====================
void connectWiFi() {
  Serial.print("连接WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiRetry = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetry < 30) {
    delay(500);
    Serial.print(".");
    wifiRetry++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi已连接");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ WiFi连接失败，5秒后重试");
    delay(5000);
    connectWiFi();
  }
}

// ==================== 蜂鸣器报警 ====================
void beep(int times, int durationMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(durationMs);
    digitalWrite(PIN_BUZZER, LOW);
    if (i < times - 1) delay(durationMs);
  }
}

void checkAlarm() {
  bool needAlarm = false;
  if (waterTemp > 30 || (waterTemp > 0 && waterTemp < 18)) needAlarm = true;
  if (waterLevel > 0 && waterLevel < 20) needAlarm = true;
  if (turbidity > 80) needAlarm = true;

  if (needAlarm && !alarmMuted) {
    if (!alarmActive) {
      alarmActive = true;
      beep(3, 150);  // 哔-哔-哔
      Serial.println("🚨 报警触发！");
    }
  } else if (!needAlarm) {
    if (alarmActive) {
      alarmActive = false;
      alarmMuted = false;
    }
  }
}

// ==================== 读取传感器 ====================
float readTemperature() {
  tempSensor.requestTemperatures();
  float t = tempSensor.getTempCByIndex(0);
  if (t != DEVICE_DISCONNECTED_C && t > -10 && t < 80) return t;
  return waterTemp;
}

float readUltrasonicCM() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  long duration = pulseIn(PIN_ECHO, HIGH, 30000);
  if (duration == 0) return -1;
  return duration * 0.034 / 2;
}

int calcWaterLevel() {
  float dist = readUltrasonicCM();
  if (dist < 0) return waterLevel;
  // 传感器装在鱼缸顶部，距离水面越远水位越低
  // dist < 3cm = 满水(100%)，dist = 10cm = 半水(50%)，dist > 10cm = 低水(<50%)
  int level = (int)((1.0 - dist / 12.0) * 100);
  return constrain(level, 0, 100);
}

void readSensors() {
  // 水温
  float t = readTemperature();
  if (t > 0) waterTemp = t;

  // 水位
  int lvl = calcWaterLevel();
  if (lvl > 0 || waterLevel == 0) waterLevel = lvl;

  // 浑浊度
  int turbRaw = analogRead(PIN_TURBIDITY);
  turbidity = map(turbRaw, 500, 3500, 0, 100);
  turbidity = constrain(turbidity, 0, 100);

  // 光照
  int lightRaw = analogRead(PIN_LIGHT_DO);
  lightLux = map(lightRaw, 0, 4095, 0, 100);
  lightLux = constrain(lightLux, 0, 100);

  // 天黑自动开灯（舵机转45度表示开灯效果）
  if (autoLightOn) {
    if (lightLux < 30 && !lightOn) {
      lightOn = true;
      Serial.println("💡 天黑自动开灯");
    } else if (lightLux > 60 && lightOn) {
      lightOn = false;
      Serial.println("💡 天亮自动关灯");
    }
  }

  checkAlarm();
  Serial.printf("📊 T=%.1f°C L=%d%% Tu=%d Lux=%d %s\n", waterTemp, waterLevel, turbidity, lightLux, alarmActive ? "🚨" : "✅");
}

// ==================== 喂食 ====================
void triggerFeeder() {
  if (!feederOn) {
    feederOn = true;
    feederOffTime = millis() + 3000;
    feederServo.write(90);
    Serial.println("🐟 喂食开始");
  }
}

// ==================== 上报 ====================
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

  String payload;
  serializeJson(doc, payload);
  int code = http.POST(payload);
  Serial.printf("📤 上报: %s [%d]\n", payload.c_str(), code);
  http.end();
}

// ==================== 拉取指令 ====================
void pullCommand() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(SERVER_URL) + "/api/esp32/command");
  http.setTimeout(3000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    StaticJsonDocument<256> doc;
    if (!deserializeJson(doc, payload)) {
      bool needLight = doc["data"]["light"] | false;
      bool needFeeder = doc["data"]["feeder"] | false;
      if (needLight != lightOn) {
        lightOn = needLight;
        Serial.printf("💡 远程灯光: %s\n", lightOn ? "开启" : "关闭");
      }
      if (needFeeder && !feederOn) triggerFeeder();
    }
  }
  http.end();
}

// ==================== 按键检测 ====================
void checkButtons() {
  unsigned long now = millis();
  // 喂食按键（GPIO23 → GND）
  if (digitalRead(PIN_BTN_FEEDER) == LOW && now - lastBtnFeed > DEBOUNCE_MS) {
    lastBtnFeed = now;
    triggerFeeder();
    beep(1, 80);
  }
  // 灯光按键（GPIO19 → GND）
  if (digitalRead(PIN_BTN_LIGHT) == LOW && now - lastBtnLight > DEBOUNCE_MS) {
    lastBtnLight = now;
    lightOn = !lightOn;
    beep(1, 80);
    Serial.printf("💡 手动灯光: %s\n", lightOn ? "开启" : "关闭");
  }
  // 关闭报警按键（GPIO17 → GND）
  if (digitalRead(PIN_BTN_ALARM) == LOW && now - lastBtnAlarm > DEBOUNCE_MS) {
    lastBtnAlarm = now;
    alarmMuted = true;
    digitalWrite(PIN_BUZZER, LOW);
    Serial.println("🔇 报警已静音");
  }
}

// ==================== OLED 显示 ====================
void updateScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // 标题
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("AquaSmart");
  display.setCursor(90, 0);
  display.print(WiFi.status() == WL_CONNECTED ? "WiFi" : "NO WIFI");

  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);

  // 温度
  display.setCursor(0, 14);
  display.print("Temp:");
  display.setTextSize(2);
  display.setCursor(40, 12);
  display.printf("%.1f", waterTemp);
  display.setTextSize(1);
  display.print(" C");

  // 水位
  display.setCursor(0, 34);
  display.print("Level:");
  display.setTextSize(2);
  display.setCursor(40, 32);
  display.printf("%d%%", waterLevel);

  // 浑浊度 + 光照
  display.setTextSize(1);
  display.setCursor(0, 54);
  display.print("T:");
  display.print(turbidity);
  display.print(" Lux:");
  display.print(lightLux);
  display.print("%");

  // 状态
  display.setCursor(90, 54);
  if (alarmActive) display.print("!ALM");
  else {
    display.print(lightOn ? "L" : "-");
    display.print(feederOn ? "F" : "-");
  }

  display.display();
}

// ==================== setup ====================
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n🐠 AquaSmart 智能鱼缸 v2 启动...\n");

  // 引脚
  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  // 按键（内部上拉，按下接GND）
  pinMode(PIN_BTN_FEEDER, INPUT_PULLUP);
  pinMode(PIN_BTN_LIGHT, INPUT_PULLUP);
  pinMode(PIN_BTN_ALARM, INPUT_PULLUP);

  analogReadResolution(12);

  // 温度传感器
  tempSensor.begin();
  Serial.printf("温度传感器: %d 个\n", tempSensor.getDeviceCount());

  // OLED
  Wire.begin(PIN_SDA, PIN_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(20, 25);
    display.println("AquaSmart");
    display.setCursor(10, 40);
    display.println("Booting...");
    display.display();
    Serial.println("✅ OLED");
  }

  // 舵机
  feederServo.attach(PIN_SERVO);
  feederServo.write(0);
  delay(500);
  Serial.println("✅ 舵机");

  // 启动音
  beep(2, 100);

  // WiFi
  connectWiFi();

  Serial.println("\n✅ 系统就绪!\n");
}

// ==================== loop ====================
void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi断开，重连中...");
    connectWiFi();
  }

  // 舵机归位
  if (feederOn && now >= feederOffTime) {
    feederOn = false;
    feederServo.write(0);
    Serial.println("🐟 喂食完成");
  }

  // 按键
  checkButtons();

  // 传感器
  if (now - lastRead >= READ_INTERVAL) {
    readSensors();
    updateScreen();
    lastRead = now;
  }

  // 上报
  if (now - lastReport >= REPORT_INTERVAL) {
    reportData();
    lastReport = now;
  }

  // 拉取指令
  if (now - lastCommand >= COMMAND_INTERVAL) {
    pullCommand();
    lastCommand = now;
  }

  delay(50);
}








