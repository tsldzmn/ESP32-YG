#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// ========== WiFi配置 ==========
const char* WIFI_SSID     = "FFF";
const char* WIFI_PASSWORD = "cs123456";

// ========== 服务器配置 ==========
const char* SERVER_URL = "https://esp32-yg.onrender.com";

// ========== OLED配置 ==========
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);

// ========== 引脚配置 ==========
#define TRIG_PIN     5
#define ECHO_PIN     4
#define TURB_PIN     35
#define TEMP_PIN     23
#define SERVO_PIN    16
#define LIGHT_PIN    13
#define LED_PIN      2

// ========== 传感器对象 ==========
OneWire oneWire(TEMP_PIN);
DallasTemperature sensors(&oneWire);

int servoPos = 0;
bool webLightOverride = false;
bool feederActive = false;
unsigned long lastReport = 0;
unsigned long lastCommand = 0;
const unsigned long REPORT_INTERVAL = 3000;
const unsigned long COMMAND_INTERVAL = 1500;

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
    Serial.println("Report OK");
  } else {
    Serial.println("Report failed");
  }
  http.end();
}

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
      
      if (newFeeder && !feederActive) {
        Serial.println("Feeder triggered from web!");
        feedFish();
      }
      feederActive = newFeeder;
      
      if (newLight) {
        webLightOverride = true;
        digitalWrite(LED_PIN, HIGH);
        Serial.println("Web light ON");
      } else {
        webLightOverride = false;
        digitalWrite(LED_PIN, LOW);
        Serial.println("Web light OFF - auto mode");
      }
    } else {
      Serial.print("JSON error: ");
      Serial.println(error.c_str());
    }
  }
  http.end();
}

void feedFish() {
  Serial.println("Feeding...");
  for (int i = 0; i <= 180; i += 5) {
    int pulse = map(i, 0, 180, 500, 2400);
    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(pulse);
    digitalWrite(SERVO_PIN, LOW);
    delay(20);
  }
  delay(500);
  for (int i = 180; i >= 0; i -= 5) {
    int pulse = map(i, 0, 180, 500, 2400);
    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(pulse);
    digitalWrite(SERVO_PIN, LOW);
    delay(20);
  }
  servoPos = 0;
  Serial.println("Feeding done!");
}

void setup() {
  Serial.begin(115200);
  
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(SERVO_PIN, OUTPUT);
  pinMode(LIGHT_PIN, INPUT);
  pinMode(LED_PIN, OUTPUT);
  
  sensors.begin();
  connectWiFi();
}

void loop() {
  // 超声波
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  float dist = duration * 0.01715;
  
  // 水位计算
  float waterLevel;
  if (dist > 100) {
    waterLevel = -1;
  } else {
    waterLevel = map(dist, 10, 100, 90, 0);
    waterLevel = constrain(waterLevel, 0, 100);
  }
  
  // 浊度
  int turbRaw = analogRead(TURB_PIN);
  int turbPercent = map(turbRaw, 0, 4095, 0, 100);
  
  // 温度
  sensors.requestTemperatures();
  float temp = sensors.getTempCByIndex(0);
  
  // 光敏控制灯
  int lightSensor = digitalRead(LIGHT_PIN);
  if (!webLightOverride) {
    if (lightSensor == 1) {
      digitalWrite(LED_PIN, HIGH);
    } else {
      digitalWrite(LED_PIN, LOW);
    }
  }
  
  // 舵机(本地串口控制)
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '0') servoPos = 0;
    if (c == '1') servoPos = 180;
  }
  
  if (!feederActive) {
    digitalWrite(SERVO_PIN, HIGH);
    delayMicroseconds(servoPos == 0 ? 500 : 2400);
    digitalWrite(SERVO_PIN, LOW);
    delay(20);
  }
  
  // 上报数据
  if (millis() - lastReport >= REPORT_INTERVAL) {
    float reportLevel = (waterLevel == -1) ? 0 : waterLevel;
    reportToServer(reportLevel, temp, turbPercent);
    lastReport = millis();
  }
  
  // 检查服务器指令
  if (millis() - lastCommand >= COMMAND_INTERVAL) {
    checkCommands();
    lastCommand = millis();
  }
  
  // OLED显示
  display.clearDisplay();
  display.setCursor(0, 0);
  if (waterLevel == -1) {
    display.print("Water: Error");
  } else {
    display.print("Water: ");
    display.print(waterLevel, 0);
    display.print(" %");
  }
  
  display.setCursor(0, 16); display.print("Tur: "); display.print(turbPercent); display.print(" %");
  display.setCursor(0, 32); display.print("Temp: "); display.print(temp);
  display.setCursor(0, 48);
  if (lightSensor == 0) {
    display.print("Light: OFF");
  } else {
    display.print("Light: ON");
  }
  display.display();
  delay(100);
}
