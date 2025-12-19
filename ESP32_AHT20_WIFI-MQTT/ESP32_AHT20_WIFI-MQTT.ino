#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <ArduinoJson.h>

// ========== 配置参数 ==========
#define DEBUG_MODE true  // 调试模式开关

// WiFi配置
const char* ssid = "ch-wifi";
const char* password = "ch123456";

// MQTT配置
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
String deviceId;  // 动态生成设备ID

// 主题定义
#define TOPIC_DATA    "ch_dzbg/data/sensor"    // 传感器数据
#define TOPIC_STATUS  "ch_dzbg/data/status"    // 设备状态
#define TOPIC_COMMAND "ch_dzbg/data/cmd"       // 控制命令
#define TOPIC_LED     "ch_dzbg/data/led"       // LED控制
#define TOPIC_ALL     "ch_dzbg/data/#"         // 订阅所有主题

// 引脚定义
#define LED_PIN 2
#define LED_ON LOW   // ESP32 LED低电平点亮
#define LED_OFF HIGH

// 时间间隔（毫秒）
#define PUBLISH_INTERVAL 10000    // 10秒发布一次
#define SENSOR_READ_INTERVAL 2000 // 2秒读取一次
#define CONNECT_RETRY_INTERVAL 5000  // 5秒重连间隔
#define SENSOR_MAX_RETRIES 5         // 传感器最大重试次数

// ========== 全局对象 ==========
Adafruit_AHTX0 aht;
sensors_event_t humidity, temp;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
WebServer webServer(80);

// ========== 全局变量 ==========
// 传感器数据
struct SensorData {
  float temperature = 0;
  float humidity = 0;
  bool connected = false;
  int readCount = 0;
  int failCount = 0;
} sensorData;

// 设备状态
struct DeviceStatus {
  unsigned long uptime = 0;
  unsigned int publishCount = 0;
  bool wifiConnected = false;
  bool mqttConnected = false;
  bool ledState = false;
  String ipAddress = "";
  int rssi = 0;
} deviceStatus;

// 定时器
struct Timers {
  unsigned long lastPublish = 0;
  unsigned long lastSensorRead = 0;
  unsigned long lastReconnectAttempt = 0;
  unsigned long lastStatusUpdate = 0;
} timers;

// 错误处理
struct ErrorHandler {
  int wifiErrors = 0;
  int mqttErrors = 0;
  int sensorErrors = 0;
  String lastErrorMessage = "";
} errors;

// ========== 函数声明 ==========
// 初始化函数
void setupWiFi();
void setupWebServer();
void setupMQTT();
void generateDeviceId();

// 传感器函数
bool readSensor(bool forceRead = false);
bool initSensor();
void resetSensor();

// MQTT函数
void reconnectMQTT();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishData(const String& dataType = "all");
void publishDeviceStatus();
void publishStatusMessage(const String& message);

// Web服务器函数
void handleRoot();
void handleData();
void handleCommand();
void handleStatus();
void handleNotFound();
String generateWebPage();

// 工具函数
void blinkLED(int times, int delayTime);
void logMessage(const String& message, bool isError = false);
String getUptimeString();

// ========== 设置函数 ==========
void setup() {
  Serial.begin(115200);
  delay(1000);
  
  logMessage("========================================");
  logMessage("🌡️  ESP32 AHT20 MQTT监控系统 (优化版)");
  logMessage("========================================");
  
  // 初始化硬件
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LED_OFF);
  deviceStatus.ledState = false;
  
  // 初始化I2C
  Wire.begin(21, 22);
  logMessage("I2C总线初始化完成");
  
  // 初始化传感器
  if (!initSensor()) {
    logMessage("❌ AHT20传感器初始化失败", true);
  }
  
  // 生成设备ID
  generateDeviceId();
  logMessage("设备ID: " + deviceId);
  
  // 连接WiFi
  setupWiFi();
  
  // 设置Web服务器
  setupWebServer();
  
  // 设置MQTT
  setupMQTT();
  
  // LED闪烁提示初始化完成
  blinkLED(3, 200);
  logMessage("✅ 系统初始化完成");
  logMessage("📡 MQTT服务器: " + String(mqtt_server));
  logMessage("🌐 Web界面: http://" + WiFi.localIP().toString());
  logMessage("========================================");
  
  // 发布初始状态
  publishDeviceStatus();
}

// ========== 主循环 ==========
void loop() {
  unsigned long currentMillis = millis();
  deviceStatus.uptime = currentMillis / 1000;
  
  // 处理Web服务器请求
  webServer.handleClient();
  
  // 处理传感器数据
  if (readSensor()) {
    // 定期发布传感器数据
    if (currentMillis - timers.lastPublish >= PUBLISH_INTERVAL) {
      timers.lastPublish = currentMillis;
      publishData("all");
    }
  }
  
  // 定期发布设备状态
  if (currentMillis - timers.lastStatusUpdate >= 30000) { // 30秒一次
    timers.lastStatusUpdate = currentMillis;
    publishDeviceStatus();
  }
  
  // 保持MQTT连接
  if (!mqttClient.connected()) {
    if (currentMillis - timers.lastReconnectAttempt >= CONNECT_RETRY_INTERVAL) {
      timers.lastReconnectAttempt = currentMillis;
      reconnectMQTT();
    }
  } else {
    mqttClient.loop();
  }
  
  delay(10);
}

// ========== 初始化函数实现 ==========
void generateDeviceId() {
  deviceId = "ESP32_AHT20-" + WiFi.macAddress();
  deviceId.replace(":", "");
}

void setupWiFi() {
  logMessage("连接WiFi: " + String(ssid));
  
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    deviceStatus.wifiConnected = true;
    deviceStatus.ipAddress = WiFi.localIP().toString();
    deviceStatus.rssi = WiFi.RSSI();
    
    digitalWrite(LED_PIN, LED_ON);
    deviceStatus.ledState = true;
    
    logMessage("✅ WiFi连接成功");
    logMessage("IP地址: " + deviceStatus.ipAddress);
    logMessage("信号强度: " + String(deviceStatus.rssi) + " dBm");
  } else {
    logMessage("❌ WiFi连接失败", true);
    errors.wifiErrors++;
  }
}

void setupWebServer() {
  // 设置路由
  webServer.on("/", handleRoot);
  webServer.on("/data", handleData);
  webServer.on("/cmd", handleCommand);
  webServer.on("/status", handleStatus);
  webServer.on("/reboot", []() {
    webServer.send(200, "application/json", "{\"message\":\"设备正在重启...\"}");
    delay(1000);
    ESP.restart();
  });
  webServer.onNotFound(handleNotFound);
  
  webServer.begin();
  logMessage("✅ Web服务器已启动");
}

void setupMQTT() {
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);
  mqttClient.setKeepAlive(60);
  
  // 立即尝试连接
  reconnectMQTT();
}

// ========== 传感器函数实现 ==========
bool initSensor() {
  logMessage("初始化AHT20传感器...");
  
  for (int i = 0; i < 3; i++) {  // 最多重试3次
    if (aht.begin()) {
      sensorData.connected = true;
      sensorData.readCount = 0;
      sensorData.failCount = 0;
      
      logMessage("✅ AHT20传感器初始化成功");
      
      // 测试读取一次
      if (readSensor(true)) {
        logMessage("📊 初始读取: " + 
                   String(sensorData.temperature, 1) + "°C, " + 
                   String(sensorData.humidity, 1) + "%");
      }
      
      return true;
    }
    
    logMessage("⚠️ 传感器初始化失败，尝试 " + String(i+1) + "/3", true);
    delay(1000);
  }
  
  sensorData.connected = false;
  return false;
}

bool readSensor(bool forceRead) {
  unsigned long currentMillis = millis();
  
  // 检查读取间隔
  if (!forceRead && (currentMillis - timers.lastSensorRead < SENSOR_READ_INTERVAL)) {
    return sensorData.connected;
  }
  
  timers.lastSensorRead = currentMillis;
  
  // 如果传感器未连接，尝试重连
  if (!sensorData.connected) {
    if (millis() % 10000 < 100) {  // 每10秒尝试一次
      initSensor();
    }
    return false;
  }
  
  // 读取传感器数据
  aht.getEvent(&humidity, &temp);
  
  if (isnan(temp.temperature) || isnan(humidity.relative_humidity)) {
    sensorData.failCount++;
    logMessage("❌ 传感器读取失败 (尝试 " + String(sensorData.failCount) + ")", true);
    errors.sensorErrors++;
    
    // 如果连续失败次数过多，尝试重新初始化
    if (sensorData.failCount >= SENSOR_MAX_RETRIES) {
      logMessage("⚠️ 传感器多次读取失败，尝试重新初始化...");
      resetSensor();
    }
    
    return false;
  }
  
  // 读取成功
  sensorData.temperature = temp.temperature;
  sensorData.humidity = humidity.relative_humidity;
  sensorData.readCount++;
  sensorData.failCount = 0;
  
  if (DEBUG_MODE && (sensorData.readCount % 10 == 0)) {  // 每10次读取打印一次
    logMessage("📊 传感器读数: " + 
               String(sensorData.temperature, 1) + "°C, " + 
               String(sensorData.humidity, 1) + "%");
  }
  
  return true;
}

void resetSensor() {
  logMessage("🔄 重置传感器...");
  sensorData.connected = false;
  sensorData.failCount = 0;
  delay(1000);
  initSensor();
}

// ========== MQTT函数实现 ==========
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  logMessage("📨 收到MQTT消息 [" + String(topic) + "]: " + message);
  
  String cmd = message;
  cmd.trim();
  cmd.toUpperCase();
  
  // 处理LED控制
  if (strcmp(topic, TOPIC_LED) == 0) {
    if (cmd == "ON" || cmd == "1" || cmd == "HIGH") {
      digitalWrite(LED_PIN, LED_ON);
      deviceStatus.ledState = true;
      publishStatusMessage("LED已打开");
    } else if (cmd == "OFF" || cmd == "0" || cmd == "LOW") {
      digitalWrite(LED_PIN, LED_OFF);
      deviceStatus.ledState = false;
      publishStatusMessage("LED已关闭");
    } else if (cmd == "TOGGLE") {
      deviceStatus.ledState = !deviceStatus.ledState;
      digitalWrite(LED_PIN, deviceStatus.ledState ? LED_ON : LED_OFF);
      publishStatusMessage(deviceStatus.ledState ? "LED已打开" : "LED已关闭");
    }
  }
  
  // 处理命令主题
  if (strcmp(topic, TOPIC_COMMAND) == 0) {
    if (cmd == "GET_TEMP") {
      if (readSensor(true)) {
        publishData("temp");
        blinkLED(1, 100);
      } else {
        publishStatusMessage("传感器读取失败");
      }
    } 
    else if (cmd == "GET_HUMI") {
      if (readSensor(true)) {
        publishData("humi");
        blinkLED(1, 100);
      } else {
        publishStatusMessage("传感器读取失败");
      }
    }
    else if (cmd == "GET_ALL") {
      if (readSensor(true)) {
        publishData("all");
        blinkLED(2, 100);
      } else {
        publishStatusMessage("传感器读取失败");
      }
    }
    else if (cmd == "STATUS") {
      publishDeviceStatus();
      blinkLED(1, 200);
    }
    else if (cmd == "RESTART") {
      publishStatusMessage("设备正在重启...");
      delay(1000);
      ESP.restart();
    }
    else if (cmd == "WIFI_INFO") {
      publishData("wifi");
    }
    else if (cmd == "SENSOR_RESET") {
      resetSensor();
      publishStatusMessage("传感器已重置");
    }
    else {
      publishStatusMessage("未知命令: " + cmd);
    }
  }
}

void publishData(const String& dataType) {
  if (!mqttClient.connected()) {
    reconnectMQTT();
    if (!mqttClient.connected()) {
      logMessage("❌ MQTT未连接，无法发布数据", true);
      return;
    }
  }
  
  StaticJsonDocument<512> doc;
  doc["device_id"] = deviceId;
  doc["ip"] = deviceStatus.ipAddress;
  doc["timestamp"] = millis();
  doc["uptime"] = deviceStatus.uptime;
  doc["sensor_connected"] = sensorData.connected;
  doc["error_count"] = errors.sensorErrors;
  doc["read_count"] = sensorData.readCount;
  doc["publish_count"] = deviceStatus.publishCount;
  
  // 根据数据类型添加相应字段
  if (dataType == "temp" || dataType == "all") {
    if (sensorData.connected) {
      doc["temperature"] = sensorData.temperature;
      doc["unit_temp"] = "°C";
    } else {
      doc["temperature"] = nullptr;
    }
  }
  
  if (dataType == "humi" || dataType == "all") {
    if (sensorData.connected) {
      doc["humidity"] = sensorData.humidity;
      doc["unit_humi"] = "%";
    } else {
      doc["humidity"] = nullptr;
    }
  }
  
  if (dataType == "wifi" || dataType == "all") {
    doc["mac"] = WiFi.macAddress();
    doc["rssi"] = deviceStatus.rssi;
    doc["wifi_connected"] = deviceStatus.wifiConnected;
  }
  
  // 添加通用信息
  doc["mqtt_connected"] = mqttClient.connected();
  doc["led_state"] = deviceStatus.ledState;
  doc["free_heap"] = ESP.getFreeHeap();
  
  char buffer[512];
  serializeJson(doc, buffer);
  
  if (mqttClient.publish(TOPIC_DATA, buffer)) {
    deviceStatus.publishCount++;
    logMessage("✅ 数据已发布 [" + dataType + "]: " + String(buffer));
  } else {
    errors.mqttErrors++;
    logMessage("❌ MQTT发布失败", true);
  }
}

void publishDeviceStatus() {
  if (!mqttClient.connected()) return;
  
  StaticJsonDocument<512> doc;
  doc["device_id"] = deviceId;
  doc["type"] = "device_status";
  doc["timestamp"] = millis();
  doc["uptime"] = deviceStatus.uptime;
  doc["ip"] = deviceStatus.ipAddress;
  doc["mac"] = WiFi.macAddress();
  doc["rssi"] = deviceStatus.rssi;
  doc["temperature"] = sensorData.connected ? sensorData.temperature : 0;
  doc["humidity"] = sensorData.connected ? sensorData.humidity : 0;
  doc["sensor_connected"] = sensorData.connected;
  doc["sensor_reads"] = sensorData.readCount;
  doc["sensor_errors"] = errors.sensorErrors;
  doc["wifi_connected"] = deviceStatus.wifiConnected;
  doc["mqtt_connected"] = mqttClient.connected();
  doc["mqtt_errors"] = errors.mqttErrors;
  doc["wifi_errors"] = errors.wifiErrors;
  doc["publish_count"] = deviceStatus.publishCount;
  doc["led_state"] = deviceStatus.ledState;
  doc["free_heap"] = ESP.getFreeHeap();
  doc["last_error"] = errors.lastErrorMessage;
  
  char buffer[512];
  serializeJson(doc, buffer);
  
  if (mqttClient.publish(TOPIC_STATUS, buffer)) {
    logMessage("📊 设备状态已发布");
  }
}

void publishStatusMessage(const String& message) {
  if (!mqttClient.connected()) return;
  
  StaticJsonDocument<256> doc;
  doc["device_id"] = deviceId;
  doc["type"] = "status_message";
  doc["message"] = message;
  doc["timestamp"] = millis();
  doc["uptime"] = deviceStatus.uptime;
  
  char buffer[256];
  serializeJson(doc, buffer);
  
  mqttClient.publish(TOPIC_STATUS, buffer);
  logMessage("💬 状态消息: " + message);
}

void reconnectMQTT() {
  if (mqttClient.connected()) return;
  
  logMessage("尝试连接MQTT服务器...");
  
  String clientId = "ESP32_AHT20-" + String(random(0xffff), HEX);
  
  if (mqttClient.connect(clientId.c_str())) {
    deviceStatus.mqttConnected = true;
    
    // 订阅主题
    mqttClient.subscribe(TOPIC_COMMAND);
    mqttClient.subscribe(TOPIC_LED);
    mqttClient.subscribe(TOPIC_STATUS);
    
    logMessage("✅ MQTT连接成功");
    logMessage("📡 订阅主题:");
    logMessage("  " + String(TOPIC_COMMAND));
    logMessage("  " + String(TOPIC_LED));
    logMessage("  " + String(TOPIC_STATUS));
    
    // 发布连接消息
    publishStatusMessage("设备已连接MQTT服务器");
    publishDeviceStatus();
    
  } else {
    deviceStatus.mqttConnected = false;
    errors.mqttErrors++;
    
    logMessage("❌ MQTT连接失败, rc=" + String(mqttClient.state()), true);
  }
}

// ========== Web服务器函数实现 ==========
void handleRoot() {
  webServer.send(200, "text/html", generateWebPage());
}

void handleData() {
  StaticJsonDocument<512> doc;
  doc["device_id"] = deviceId;
  doc["temperature"] = sensorData.connected ? sensorData.temperature : 0;
  doc["humidity"] = sensorData.connected ? sensorData.humidity : 0;
  doc["sensor_connected"] = sensorData.connected;
  doc["sensor_reads"] = sensorData.readCount;
  doc["sensor_errors"] = errors.sensorErrors;
  doc["wifi_connected"] = deviceStatus.wifiConnected;
  doc["mqtt_connected"] = mqttClient.connected();
  doc["ip"] = deviceStatus.ipAddress;
  doc["rssi"] = deviceStatus.rssi;
  doc["uptime"] = deviceStatus.uptime;
  doc["publish_count"] = deviceStatus.publishCount;
  doc["led_state"] = deviceStatus.ledState;
  doc["free_heap"] = ESP.getFreeHeap();
  
  String json;
  serializeJson(doc, json);
  webServer.send(200, "application/json", json);
}

void handleCommand() {
  String cmd = webServer.arg("cmd");
  String response = "{\"status\":\"error\",\"message\":\"未知命令\"}";
  
  cmd.toUpperCase();
  
  if (cmd == "GET_ALL") {
    if (readSensor(true)) {
      publishData("all");
      response = "{\"status\":\"success\",\"message\":\"数据已发布\"}";
    } else {
      response = "{\"status\":\"error\",\"message\":\"传感器读取失败\"}";
    }
  } 
  else if (cmd == "GET_TEMP") {
    if (readSensor(true)) {
      publishData("temp");
      response = "{\"status\":\"success\",\"message\":\"温度数据已发布\"}";
    } else {
      response = "{\"status\":\"error\",\"message\":\"传感器读取失败\"}";
    }
  }
  else if (cmd == "GET_HUMI") {
    if (readSensor(true)) {
      publishData("humi");
      response = "{\"status\":\"success\",\"message\":\"湿度数据已发布\"}";
    } else {
      response = "{\"status\":\"error\",\"message\":\"传感器读取失败\"}";
    }
  }
  else if (cmd == "STATUS") {
    publishDeviceStatus();
    response = "{\"status\":\"success\",\"message\":\"设备状态已发布\"}";
  }
  else if (cmd == "SENSOR_RESET") {
    resetSensor();
    response = "{\"status\":\"success\",\"message\":\"传感器已重置\"}";
  }
  else if (cmd == "LED_ON") {
    digitalWrite(LED_PIN, LED_ON);
    deviceStatus.ledState = true;
    response = "{\"status\":\"success\",\"message\":\"LED已打开\"}";
  }
  else if (cmd == "LED_OFF") {
    digitalWrite(LED_PIN, LED_OFF);
    deviceStatus.ledState = false;
    response = "{\"status\":\"success\",\"message\":\"LED已关闭\"}";
  }
  else if (cmd == "LED_TOGGLE") {
    deviceStatus.ledState = !deviceStatus.ledState;
    digitalWrite(LED_PIN, deviceStatus.ledState ? LED_ON : LED_OFF);
    response = "{\"status\":\"success\",\"message\":\"LED已切换\"}";
  }
  else if (cmd == "RESTART") {
    response = "{\"status\":\"success\",\"message\":\"设备正在重启...\"}";
    webServer.send(200, "application/json", response);
    delay(1000);
    ESP.restart();
    return;
  }
  else if (cmd == "WIFI_INFO") {
    publishData("wifi");
    response = "{\"status\":\"success\",\"message\":\"WiFi信息已发布\"}";
  }
  
  webServer.send(200, "application/json", response);
}

void handleStatus() {
  StaticJsonDocument<256> doc;
  doc["status"] = "online";
  doc["device_id"] = deviceId;
  doc["uptime"] = deviceStatus.uptime;
  doc["version"] = "2.0.0";
  doc["author"] = "ESP32_AHT20";
  
  String json;
  serializeJson(doc, json);
  webServer.send(200, "application/json", json);
}

void handleNotFound() {
  String message = "文件未找到\n\n";
  message += "URI: ";
  message += webServer.uri();
  message += "\n方法: ";
  message += (webServer.method() == HTTP_GET) ? "GET" : "POST";
  message += "\n参数: ";
  message += webServer.args();
  message += "\n";
  
  for (uint8_t i = 0; i < webServer.args(); i++) {
    message += " " + webServer.argName(i) + ": " + webServer.arg(i) + "\n";
  }
  
  webServer.send(404, "text/plain", message);
}

String generateWebPage() {
  String page = "<!DOCTYPE html><html><head>";
  page += "<meta charset='UTF-8'>";
  page += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  page += "<title>ESP32 AHT20 传感器监控</title>";
  page += "<style>";
  page += "body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }";
  page += ".container { max-width: 800px; margin: 0 auto; }";
  page += ".header { background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 20px; border-radius: 10px; margin-bottom: 20px; }";
  page += ".card { background: white; padding: 20px; border-radius: 10px; margin-bottom: 20px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  page += ".data-row { display: flex; justify-content: space-between; margin: 10px 0; }";
  page += ".sensor-data { display: flex; justify-content: space-around; }";
  page += ".sensor-box { text-align: center; padding: 20px; border-radius: 10px; color: white; flex: 1; margin: 0 10px; }";
  page += ".temp-box { background: linear-gradient(135deg, #ff6b6b, #ee5a52); }";
  page += ".humi-box { background: linear-gradient(135deg, #4ecdc4, #44a08d); }";
  page += ".value { font-size: 3em; font-weight: bold; margin: 10px 0; }";
  page += ".unit { font-size: 1.2em; margin-left: 5px; }";
  page += ".btn { display: inline-block; padding: 10px 20px; margin: 5px; border: none; border-radius: 5px; cursor: pointer; font-weight: bold; text-decoration: none; }";
  page += ".btn-primary { background: #007bff; color: white; }";
  page += ".btn-success { background: #28a745; color: white; }";
  page += ".btn-warning { background: #ffc107; color: black; }";
  page += ".btn-danger { background: #dc3545; color: white; }";
  page += ".btn:hover { opacity: 0.8; }";
  page += ".status { padding: 10px; border-radius: 5px; margin: 10px 0; }";
  page += ".status-online { background: #d4edda; color: #155724; }";
  page += ".status-offline { background: #f8d7da; color: #721c24; }";
  page += ".controls { display: flex; flex-wrap: wrap; justify-content: center; }";
  page += "</style>";
  page += "</head><body>";
  page += "<div class='container'>";
  
  // 头部
  page += "<div class='header'>";
  page += "<h1>🌡️ ESP32 AHT20 传感器监控系统</h1>";
  page += "<p>设备ID: " + deviceId + " | 版本: 2.0.0</p>";
  page += "</div>";
  
  // 状态信息
  page += "<div class='card'>";
  page += "<h2>📊 设备状态</h2>";
  page += "<div class='status " + String(mqttClient.connected() ? "status-online" : "status-offline") + "'>";
  page += "MQTT: " + String(mqttClient.connected() ? "已连接" : "未连接");
  page += " | 传感器: " + String(sensorData.connected ? "正常" : "异常");
  page += " | WiFi: " + String(deviceStatus.wifiConnected ? "已连接" : "未连接");
  page += "</div>";
  page += "<div class='data-row'><span>📶 WiFi信号:</span><span>" + String(deviceStatus.rssi) + " dBm</span></div>";
  page += "<div class='data-row'><span>🏠 IP地址:</span><span>" + deviceStatus.ipAddress + "</span></div>";
  page += "<div class='data-row'><span>⏱️ 运行时间:</span><span>" + getUptimeString() + "</span></div>";
  page += "<div class='data-row'><span>📤 发布次数:</span><span>" + String(deviceStatus.publishCount) + "</span></div>";
  page += "<div class='data-row'><span>📊 读取次数:</span><span>" + String(sensorData.readCount) + "</span></div>";
  page += "<div class='data-row'><span>❌ 错误次数:</span><span>" + String(errors.sensorErrors) + "</span></div>";
  page += "<div class='data-row'><span>💡 LED状态:</span><span>" + String(deviceStatus.ledState ? "开" : "关") + "</span></div>";
  page += "</div>";
  
  // 传感器数据
  page += "<div class='card'>";
  page += "<h2>📈 传感器数据</h2>";
  page += "<div class='sensor-data'>";
  page += "<div class='sensor-box temp-box'>";
  page += "<div>🌡️ 温度</div>";
  page += "<div class='value'>" + String(sensorData.connected ? sensorData.temperature : 0, 1) + "<span class='unit'>°C</span></div>";
  page += "<div>最后更新: " + String((millis() - timers.lastSensorRead) / 1000) + "秒前</div>";
  page += "</div>";
  page += "<div class='sensor-box humi-box'>";
  page += "<div>💧 湿度</div>";
  page += "<div class='value'>" + String(sensorData.connected ? sensorData.humidity : 0, 1) + "<span class='unit'>%</span></div>";
  page += "<div>最后更新: " + String((millis() - timers.lastSensorRead) / 1000) + "秒前</div>";
  page += "</div>";
  page += "</div>";
  page += "</div>";
  
  // 控制面板
  page += "<div class='card'>";
  page += "<h2>🎮 控制面板</h2>";
  page += "<div class='controls'>";
  page += "<button class='btn btn-primary' onclick=\"sendCommand('GET_ALL')\">🔄 获取数据</button>";
  page += "<button class='btn btn-success' onclick=\"sendCommand('GET_TEMP')\">🌡️ 获取温度</button>";
  page += "<button class='btn btn-success' onclick=\"sendCommand('GET_HUMI')\">💧 获取湿度</button>";
  page += "<button class='btn btn-warning' onclick=\"sendCommand('STATUS')\">📊 设备状态</button>";
  page += "<button class='btn btn-warning' onclick=\"sendCommand('SENSOR_RESET')\">🔧 重置传感器</button>";
  page += "<button class='btn " + String(deviceStatus.ledState ? "btn-warning" : "btn-success") + "' onclick=\"sendCommand('LED_TOGGLE')\">💡 LED切换</button>";
  page += "<button class='btn btn-danger' onclick=\"if(confirm('确定要重启设备吗？')) sendCommand('RESTART')\">🔄 重启设备</button>";
  page += "</div>";
  page += "</div>";
  
  // MQTT信息
  page += "<div class='card'>";
  page += "<h2>📡 MQTT配置</h2>";
  page += "<div class='data-row'><span>服务器:</span><span>" + String(mqtt_server) + "</span></div>";
  page += "<div class='data-row'><span>端口:</span><span>" + String(mqtt_port) + "</span></div>";
  page += "<div class='data-row'><span>数据主题:</span><span>" + String(TOPIC_DATA) + "</span></div>";
  page += "<div class='data-row'><span>状态主题:</span><span>" + String(TOPIC_STATUS) + "</span></div>";
  page += "<div class='data-row'><span>命令主题:</span><span>" + String(TOPIC_COMMAND) + "</span></div>";
  page += "<div class='data-row'><span>订阅主题:</span><span>" + String(TOPIC_ALL) + "</span></div>";
  page += "</div>";
  
  page += "<div style='text-align: center; color: #666; font-size: 0.9em; margin-top: 20px;'>";
  page += "<p>📡 自动发布间隔: " + String(PUBLISH_INTERVAL / 1000) + "秒 | 🔄 最后更新: " + String((millis() - timers.lastPublish) / 1000) + "秒前</p>";
  page += "<p>📱 访问 <a href='/data' target='_blank'>/data</a> 获取JSON数据 | 访问 <a href='/status' target='_blank'>/status</a> 获取状态</p>";
  page += "</div>";
  page += "</div>";
  
  // JavaScript
  page += "<script>";
  page += "function sendCommand(cmd) {";
  page += "  fetch('/cmd?cmd=' + cmd)";
  page += "    .then(response => response.json())";
  page += "    .then(data => {";
  page += "      alert('命令执行结果: ' + data.message);";
  page += "      if(data.status === 'success') {";
  page += "        setTimeout(() => location.reload(), 1000);";
  page += "      }";
  page += "    })";
  page += "    .catch(error => alert('发送失败: ' + error));";
  page += "}";
  page += "// 自动刷新页面";
  page += "setTimeout(() => location.reload(), 30000);";
  page += "</script>";
  page += "</body></html>";
  
  return page;
}

// ========== 工具函数实现 ==========
void blinkLED(int times, int delayTime) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LED_ON);
    delay(delayTime);
    digitalWrite(LED_PIN, LED_OFF);
    if (i < times - 1) delay(delayTime);
  }
  digitalWrite(LED_PIN, deviceStatus.ledState ? LED_ON : LED_OFF);
}

void logMessage(const String& message, bool isError) {
  if (DEBUG_MODE || isError) {
    String timestamp = "[" + String(millis() / 1000) + "s] ";
    if (isError) {
      errors.lastErrorMessage = message;
      Serial.println("❌ " + timestamp + message);
    } else {
      Serial.println("✅ " + timestamp + message);
    }
  }
}

String getUptimeString() {
  unsigned long seconds = deviceStatus.uptime;
  unsigned long days = seconds / 86400;
  seconds %= 86400;
  unsigned long hours = seconds / 3600;
  seconds %= 3600;
  unsigned long minutes = seconds / 60;
  seconds %= 60;
  
  String result = "";
  if (days > 0) result += String(days) + "天 ";
  if (hours > 0) result += String(hours) + "时 ";
  if (minutes > 0) result += String(minutes) + "分 ";
  result += String(seconds) + "秒";
  return result;
}
