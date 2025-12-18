#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_AHTX0.h>
#include <ArduinoJson.h>

// WiFi配置
const char* ssid = "ch-wifi";
const char* password = "ch123456";

// MQTT配置
const char* mqtt_server = "broker.hivemq.com";  // 公共MQTT服务器
// 或者使用本地服务器: "192.168.1.100"
// 或者使用EMQX Cloud: "broker.emqx.io"
const int mqtt_port = 1883;
const char* mqtt_client_id = "ESP32_AHT20_MQTT_Client";

// 主题定义
const char* topic_data = "ch_dzbg/data/sensor";      // 发布传感器数据
const char* topic_status = "ch_dzbg/data/status";  // 发布设备状态
const char* topic_command = "ch_dzbg/data/cmd";    // 订阅控制命令
const char* topic_led = "ch_dzbg/data/led";        // LED控制
const char* topic_all = "ch_dzbg/data/#";          // 订阅所有相关主题

// 定义LED引脚
#define LED_PIN 2

// 创建AHT20对象
Adafruit_AHTX0 aht;
sensors_event_t humidity, temp;

// WiFi和MQTT客户端
WiFiClient espClient;
PubSubClient client(espClient);

// 传感器值
float temperature = 0;
float humidityValue = 0;
bool ahtConnected = false;
bool ledState = false;

// 自动发布间隔
unsigned long lastPublishTime = 0;
const long publishInterval = 5000;  // 5秒发布一次

// 重连计数
unsigned long reconnectTime = 0;
const long reconnectInterval = 5000;

void setup() {
  Serial.begin(115200);
  Serial.println("\nESP32 AHT20 MQTT温湿度传感器");
  Serial.println("=============================");
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // 初始化I2C
  Wire.begin(21, 22);
  Serial.println("I2C初始化完成");
  
  // 初始化AHT20
  Serial.println("初始化AHT20传感器...");
  if (!aht.begin()) {
    Serial.println("AHT20传感器连接失败!");
    ahtConnected = false;
  } else {
    Serial.println("AHT20传感器初始化成功!");
    ahtConnected = true;
  }
  
  // 连接WiFi
  setupWiFi();
  
  // 设置MQTT
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);
  client.setBufferSize(2048);  // 增加缓冲区大小
  
  // 连接MQTT服务器
  reconnectMQTT();
  
  blinkLED(3, 200);
  Serial.println("系统初始化完成");
  Serial.println("====================");
  Serial.println("MQTT服务器: " + String(mqtt_server));
  Serial.println("数据主题: " + String(topic_data));
  Serial.println("状态主题: " + String(topic_status));
  Serial.println("命令主题: " + String(topic_command));
  Serial.println("LED主题: " + String(topic_led));
  Serial.println("订阅主题: " + String(topic_all));
  Serial.println("====================");
}

void loop() {
  unsigned long currentMillis = millis();
  
  // 检查WiFi连接
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi连接断开，尝试重连...");
    setupWiFi();
  }
  
  // 保持MQTT连接
  if (!client.connected()) {
    if (currentMillis - reconnectTime >= reconnectInterval) {
      reconnectTime = currentMillis;
      reconnectMQTT();
    }
  } else {
    client.loop();
    
    // 定期发布传感器数据
    if (currentMillis - lastPublishTime >= publishInterval) {
      lastPublishTime = currentMillis;
      
      if (ahtConnected && readSensorData()) {
        publishSensorData();
        blinkLED(1, 50);  // 发布数据时LED闪烁
      }
    }
  }
  
  delay(10);
}

// 读取传感器数据
bool readSensorData() {
  if (!ahtConnected) {
    // 尝试重新连接传感器
    if (aht.begin()) {
      ahtConnected = true;
      Serial.println("AHT20重新连接成功");
    } else {
      return false;
    }
  }
  
  aht.getEvent(&humidity, &temp);
  
  if (isnan(temp.temperature) || isnan(humidity.relative_humidity)) {
    Serial.println("读取到无效的传感器数据");
    return false;
  }
  
  temperature = temp.temperature;
  humidityValue = humidity.relative_humidity;
  
  return true;
}

// 连接WiFi
void setupWiFi() {
  Serial.println();
  Serial.print("连接WiFi: ");
  Serial.println(ssid);
  
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
    digitalWrite(LED_PIN, HIGH);
    ledState = true;
    Serial.println();
    Serial.println("WiFi连接成功!");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());
    Serial.print("信号强度: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println();
    Serial.println("WiFi连接失败!");
    digitalWrite(LED_PIN, LOW);
    ledState = false;
  }
}

// MQTT回调函数
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // 将消息转换为字符串
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("收到MQTT消息 [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);
  
  // 处理命令
  String cmd = message;
  cmd.trim();
  cmd.toUpperCase();
  
  // 处理LED控制
  if (strcmp(topic, topic_led) == 0) {
    if (cmd == "ON" || cmd == "1" || cmd == "HIGH") {
      digitalWrite(LED_PIN, HIGH);
      ledState = true;
      publishStatus("LED已打开");
    } else if (cmd == "OFF" || cmd == "0" || cmd == "LOW") {
      digitalWrite(LED_PIN, LOW);
      ledState = false;
      publishStatus("LED已关闭");
    } else if (cmd == "TOGGLE") {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      publishStatus(ledState ? "LED已打开" : "LED已关闭");
    }
  }
  
  // 处理命令主题
  if (strcmp(topic, topic_command) == 0) {
    if (cmd == "GET_TEMP") {
      if (ahtConnected && readSensorData()) {
        publishSensorData();
      } else {
        publishStatus("传感器读取失败");
      }
    } 
    else if (cmd == "GET_HUMI") {
      if (ahtConnected && readSensorData()) {
        String json = "{\"humidity\":" + String(humidityValue, 1) + "}";
        client.publish(topic_data, json.c_str());
      } else {
        publishStatus("传感器读取失败");
      }
    }
    else if (cmd == "GET_ALL") {
      if (ahtConnected && readSensorData()) {
        publishSensorData();
      } else {
        publishStatus("传感器读取失败");
      }
    }
    else if (cmd == "STATUS") {
      publishDeviceStatus();
    }
    else if (cmd == "RESTART") {
      publishStatus("设备正在重启...");
      delay(1000);
      ESP.restart();
    }
    else if (cmd == "WIFI_INFO") {
      publishWiFiInfo();
    }
    else {
      // 回显未知命令
      publishStatus("未知命令: " + cmd);
    }
  }
}

// 发布传感器数据
void publishSensorData() {
  StaticJsonDocument<256> doc;
  doc["device"] = "ESP32_AHT20";
  doc["temperature"] = temperature;
  doc["humidity"] = humidityValue;
  doc["timestamp"] = millis();
  doc["unit_temp"] = "°C";
  doc["unit_humi"] = "%";
  doc["sensor_connected"] = ahtConnected;
  doc["led_state"] = ledState;
  
  char buffer[256];
  serializeJson(doc, buffer);
  
  if (client.publish(topic_data, buffer)) {
    Serial.print("已发布传感器数据: ");
    Serial.println(buffer);
  } else {
    Serial.println("发布数据失败");
  }
}

// 发布设备状态
void publishDeviceStatus() {
  StaticJsonDocument<512> doc;
  doc["device"] = "ESP32_AHT20";
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;
  doc["temperature"] = temperature;
  doc["humidity"] = humidityValue;
  doc["sensor_connected"] = ahtConnected;
  doc["led_state"] = ledState;
  doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
  doc["mqtt_connected"] = client.connected();
  
  char buffer[512];
  serializeJson(doc, buffer);
  client.publish(topic_status, buffer);
  
  Serial.print("已发布设备状态: ");
  Serial.println(buffer);
}

// 发布状态消息
void publishStatus(String message) {
  StaticJsonDocument<128> doc;
  doc["device"] = "ESP32_AHT20";
  doc["message"] = message;
  doc["timestamp"] = millis();
  
  char buffer[128];
  serializeJson(doc, buffer);
  client.publish(topic_status, buffer);
}

// 发布WiFi信息
void publishWiFiInfo() {
  StaticJsonDocument<256> doc;
  doc["device"] = "ESP32_AHT20";
  doc["wifi_status"] = (WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected";
  if (WiFi.status() == WL_CONNECTED) {
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["rssi"] = WiFi.RSSI();
    doc["ssid"] = WiFi.SSID();
  }
  doc["timestamp"] = millis();
  
  char buffer[256];
  serializeJson(doc, buffer);
  client.publish(topic_status, buffer);
}

// 重新连接MQTT
void reconnectMQTT() {
  if (client.connected()) {
    return;
  }
  
  Serial.print("连接MQTT服务器...");
  Serial.println(mqtt_server);
  
  if (client.connect(mqtt_client_id)) {
    Serial.println("MQTT连接成功!");
    
    // 订阅主题
    client.subscribe(topic_command);
    client.subscribe(topic_led);
    client.subscribe(topic_status);
    
    Serial.println("已订阅主题:");
    Serial.println("  " + String(topic_command));
    Serial.println("  " + String(topic_led));
    Serial.println("  " + String(topic_status));
    
    // 发布连接成功消息
    publishStatus("设备已连接MQTT服务器");
    publishDeviceStatus();
    
  } else {
    Serial.print("MQTT连接失败, rc=");
    Serial.print(client.state());
    Serial.println(" 5秒后重试...");
  }
}

// LED闪烁函数
void blinkLED(int times, int delayTime) {
  bool originalState = digitalRead(LED_PIN);
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH);
    delay(delayTime);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(delayTime);
  }
  digitalWrite(LED_PIN, originalState);
}