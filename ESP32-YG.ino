/**
 * @file ESP32-YG.ino
 * @brief ESP32 智能鱼缸控制器
 * @description 监测水位、水温、浊度、光照，控制舵机喂食和LED灯。
 *              每3秒向远程服务器上报数据，支持网页远程控制。
 * @hardware ESP32开发板, HC-SR04超声波, DS18B20温度传感器,
 *           浊度传感器, 光敏电阻, SG90舵机, LED, 0.96寸OLED (SSD1306)
 */

// ============================================================================
// 引入头文件
// ============================================================================
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ============================================================================
// 配置参数
// ============================================================================

/** WiFi 网络名称和密码 */
const char* WIFI_SSID     = "FFF";
const char* WIFI_PASSWORD = "cs123456";

/** 远程服务器地址（用于数据上报和指令接收） */
const char* SERVER_URL = "https://esp32-yg.onrender.com";

/** 定时任务间隔（毫秒） */
const unsigned long REPORT_INTERVAL  = 3000;  // 数据上报间隔
const unsigned long COMMAND_INTERVAL = 1500;  // 服务器指令轮询间隔

// ============================================================================
// 引脚定义
// ============================================================================
#define OLED_SDA     21   // OLED SDA 引脚
#define OLED_SCL     22   // OLED SCL 引脚
#define TRIG_PIN      5   // 超声波 Trig 引脚
#define ECHO_PIN      4   // 超声波 Echo 引脚
#define TURB_PIN     35   // 浊度传感器 模拟引脚
#define TEMP_PIN     23   // DS18B20 温度传感器 引脚
#define SERVO_PIN    16   // SG90 舵机 信号引脚
#define LIGHT_PIN    13   // 光敏电阻 数字引脚
#define LED_PIN       2   // LED 控制引脚

// ============================================================================
// 显示屏配置
// ============================================================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ============================================================================
// 传感器对象
// ============================================================================
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);

// ============================================================================
// 状态变量
// ============================================================================
int servoPos = 0;                // 当前舵机角度 (0 或 180)
bool webLightOverride = false;   // 网页灯光控制覆盖标志
bool feederActive = false;       // 网页喂食触发标志
unsigned long lastReport = 0;    // 上次数据上报时间戳
unsigned long lastCommand = 0;   // 上次指令轮询时间戳

// ============================================================================
// WiFi 连接
// ============================================================================

/**
 * @brief 连接 WiFi 网络并在 OLED 上显示状态
 * @note 阻塞直到连接成功或 15 秒超时
 */
void connectWiFi() {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("Connecting WiFi");
  display.display();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 30) {
    delay(500);
    display.print(".");
    display.display();
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.print("WiFi OK!");
    display.setCursor(0, 16);
    display.print(WiFi.localIP());
    display.display();
    delay(2000);
  }
}

// ============================================================================
// 服务器通信
// ============================================================================

/**
 * @brief 向远程服务器上报传感器数据
 * @param waterLevel 水位百分比 (0-100)
 * @param temp 水温（摄氏度）
 * @param turbPercent 浊度百分比 (0-100)
 */
void reportToServer(float waterLevel, float temp, int turbPercent) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(SERVER_URL) + "/api/esp32/report";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");

  String json = "{\"waterLevel\":" + String(waterLevel, 1) +
                ",\"waterTemp\":" + String(temp, 1) +
                ",\"turbidity\":" + String(turbPercent) + "}";

  int httpCode = http.POST(json);
  if (httpCode > 0) {
    Serial.println("数据上报成功");
  } else {
    Serial.println("数据上报失败");
  }
  http.end();
}

/**
 * @brief 轮询服务器获取远程控制指令（灯光、喂食）
 * @note 根据服务器返回更新本地状态
 */
void checkCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(SERVER_URL) + "/api/esp32/command";
  http.begin(url);

  int httpCode = http.GET();
  if (httpCode > 0) {
    String payload = http.getString();
    Serial.println(payload);

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      bool newFeeder = doc["data"]["feeder"];
      bool newLight = doc["data"]["light"];

      // 如果网页触发了喂食且当前未处于喂食状态，则执行喂食
      if (newFeeder && !feederActive) {
        Serial.println("网页触发喂食！");
        feedFish();
      }
      feederActive = newFeeder;

      // 网页灯光控制：开启则强制亮灯，关闭则恢复光敏自动控制
      if (newLight) {
        webLightOverride = true;
        digitalWrite(LED_PIN, HIGH);
        Serial.println("网页开灯");
      } else {
        webLightOverride = false;
        digitalWrite(LED_PIN, LOW);
        Serial.println("网页关灯 - 恢复光敏控制");
      }
    } else {
      Serial.print("JSON 解析错误: ");
      Serial.println(error.c_str());
    }
  }
  http.end();
}

// ============================================================================
// 执行器控制
// ============================================================================

/**
 * @brief 控制舵机旋转完成喂食动作 (0度 -> 180度 -> 0度)
 * @note 以 5 度为步进平滑转动
 */
void feedFish() {
  Serial.println("开始喂食...");
  feederActive = true;
  // 正向旋转至 180 度
  for (int i = 0; i <= 180; i += 5) {
    int pulse = map(i, 0, 180, 500, 2400);
    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(pulse);
    digitalWrite(SERVO_PIN, LOW);
    delay(20);
  }
  delay(500);
  // 反向旋转回 0 度
  for (int i = 180; i >= 0; i -= 5) {
    int pulse = map(i, 0, 180, 500, 2400);
    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(pulse);
    digitalWrite(SERVO_PIN, LOW);
    delay(20);
  }
  servoPos = 0;
  feederActive = false;  // 喂食完成后立即重置标志，允许下次喂食
  Serial.println("喂食完成！");
}

// ============================================================================
// 初始化
// ============================================================================

void setup() {
  // 初始化串口通信
  Serial.begin(115200);

  // 初始化 OLED 显示屏
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // 配置引脚模式
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(SERVO_PIN, OUTPUT);
  pinMode(LIGHT_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);

  // 初始化温度传感器
  sensors.begin();

  // 连接 WiFi
  connectWiFi();
}

// ============================================================================
// 主循环
// ============================================================================

void loop() {
  // --- 读取传感器数据 ---

  // 超声波测距 (HC-SR04)
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  float dist = duration * 0.01715;

  // 将距离转换为水位百分比
  // 10cm = 90% 满, 100cm = 0% 满, >100cm = 错误
  float waterLevel;
  if (dist > 100) {
    waterLevel = -1;
  } else {
    waterLevel = map(dist, 10, 100, 90, 0);
    waterLevel = constrain(waterLevel, 0, 100);
  }

  // 浊度传感器 (模拟输入)
  int turbRaw = analogRead(TURB_PIN);
  int turbPercent = map(turbRaw, 0, 4095, 0, 100);

  // 温度传感器 (DS18B20)
  sensors.requestTemperatures();
  float temp = sensors.getTempCByIndex(0);

  // 光敏电阻 (数字输入)
  int lightSensor = digitalRead(LIGHT_PIN);

  // --- 本地控制逻辑 ---

  // LED 控制：光敏自动控制，除非被网页覆盖
  if (!webLightOverride) {
    digitalWrite(LED_PIN, lightSensor == 1 ? HIGH : LOW);
  }

  // 舵机控制：串口本地控制 ('0' = 0度, '1' = 180度)
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '0') servoPos = 0;
    if (c == '1') servoPos = 180;
  }

  // 非喂食状态下保持舵机当前位置
  if (!feederActive) {
    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(servoPos == 0 ? 500 : 2400);
    digitalWrite(SERVO_PIN, LOW);
    delay(20);
  }

  // --- 服务器通信 ---

  // 定时上报传感器数据
  if (millis() - lastReport >= REPORT_INTERVAL) {
    float reportLevel = (waterLevel == -1) ? 0 : waterLevel;
    reportToServer(reportLevel, temp, turbPercent);
    lastReport = millis();
  }

  // 定时轮询服务器远程控制指令
  if (millis() - lastCommand >= COMMAND_INTERVAL) {
    checkCommands();
    lastCommand = millis();
  }

  // --- OLED 显示更新 ---

  display.clearDisplay();
  display.setCursor(0, 0);
  if (waterLevel == -1) {
    display.print("Water: Error");
  } else {
    display.print("Water: ");
    display.print(waterLevel, 0);
    display.print(" %");
  }

  display.setCursor(0, 16);
  display.print("Tur: ");
  display.print(turbPercent);
  display.print(" %");

  display.setCursor(0, 32);
  display.print("Temp: ");
  display.print(temp);

  display.setCursor(0, 48);
  display.print("Light: ");
  display.print(lightSensor == 0 ? "OFF" : "ON");

  display.display();
  delay(100);
}
