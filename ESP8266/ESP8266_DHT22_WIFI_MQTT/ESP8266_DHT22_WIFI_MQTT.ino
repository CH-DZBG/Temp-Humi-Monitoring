#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <ArduinoJson.h>

// WiFi配置
const char* ssid = "ch-wifi";
const char* password = "ch123456";
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;

// 主题定义 - 与网页保持一致
const char* topic_data = "ch_dzbg/data/sensor";      // 发布传感器数据
const char* topic_status = "ch_dzbg/data/status";    // 发布设备状态
const char* topic_command = "ch_dzbg/data/cmd";      // 订阅控制命令
const char* topic_all = "ch_dzbg/data/#";           // 订阅所有相关主题

// DHT传感器 - 使用GPIO2
#define DHTTYPE DHT22
#define DHTPIN 2
DHT dht(DHTPIN, DHTTYPE, 11);

WiFiClient espClient;
PubSubClient mqttClient(espClient);
ESP8266WebServer server(80);

// 变量
float temp = 0;
float hum = 0;
bool sensorOK = false;
unsigned long lastRead = 0;
unsigned long lastPublish = 0;
const long readInterval = 2000;    // 2秒读取一次
const long publishInterval = 10000; // 10秒发布一次
String deviceId;
int readAttempts = 0;
int publishCount = 0;

// 连接WiFi
void setupWiFi() {
  Serial.print("连接WiFi: ");
  Serial.println(ssid);
  
  WiFi.begin(ssid, password);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi已连接");
    Serial.print("📡 IP地址: ");
    Serial.println(WiFi.localIP());
    
    // 生成设备ID
    deviceId = "ESP8266_DHT22-" + WiFi.macAddress();
    deviceId.replace(":", "");
    Serial.print("🆔 设备ID: ");
    Serial.println(deviceId);
  } else {
    Serial.println("\n❌ WiFi连接失败!");
  }
}

// MQTT回调函数
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.print("📨 收到MQTT消息 [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);
  
  String cmd = message;
  cmd.trim();
  cmd.toUpperCase();
  
  // 处理命令主题
  if (strcmp(topic, topic_command) == 0) {
    if (cmd == "GET_TEMP") {
      if (sensorOK) {
        String json = "{\"device\":\"" + deviceId + "\",\"temperature\":" + String(temp, 1) + "}";
        mqttClient.publish(topic_data, json.c_str());
        Serial.println("✅ 发送温度数据");
      } else {
        publishStatus("传感器读取失败");
      }
    } 
    else if (cmd == "GET_HUMI") {
      if (sensorOK) {
        String json = "{\"device\":\"" + deviceId + "\",\"humidity\":" + String(hum, 1) + "}";
        mqttClient.publish(topic_data, json.c_str());
        Serial.println("✅ 发送湿度数据");
      } else {
        publishStatus("传感器读取失败");
      }
    }
    else if (cmd == "GET_ALL") {
      if (sensorOK) {
        publishSensorData();
      } else {
        publishStatus("传感器读取失败");
      }
    }
    else if (cmd == "STATUS") {
      publishDeviceStatus();
    }
    else if (cmd == "WIFI_INFO") {
      publishWiFiInfo();
    }
    else if (cmd == "RESTART") {
      publishStatus("设备正在重启...");
      delay(1000);
      ESP.restart();
    }
    else {
      publishStatus("未知命令: " + cmd);
    }
  }
}

// 连接MQTT
void reconnect() {
  if (!mqttClient.connected()) {
    Serial.print("🔌 尝试MQTT连接...");
    
    String clientId = "ESP8266-" + String(ESP.getChipId(), HEX);
    
    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("✅ 已连接");
      
      // 订阅主题
      mqttClient.subscribe(topic_command);
      mqttClient.subscribe(topic_all);
      
      Serial.println("📡 已订阅主题:");
      Serial.println("  " + String(topic_command));
      Serial.println("  " + String(topic_all));
      
      // 发布连接成功消息
      publishStatus("设备已连接MQTT服务器");
      publishDeviceStatus();
      
    } else {
      Serial.print("❌ 失败, rc=");
      Serial.println(mqttClient.state());
    }
  }
}

// 读取传感器（带重试机制）
bool readSensor() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastRead >= readInterval) {
    lastRead = currentTime;
    
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    
    if (isnan(h) || isnan(t)) {
      sensorOK = false;
      readAttempts++;
      
      Serial.print("❌ 读取DHT22失败! 尝试次数: ");
      Serial.println(readAttempts);
      
      // 每5次失败尝试重新初始化传感器
      if (readAttempts >= 5) {
        Serial.println("🔄 重新初始化DHT22...");
        dht.begin();
        readAttempts = 0;
        delay(2000);
      }
      
      return false;
    } else {
      sensorOK = true;
      readAttempts = 0;
      temp = t;
      hum = h;
      
      Serial.print("✅ 读取成功: ");
      Serial.print(temp, 1);
      Serial.print("°C, ");
      Serial.print(hum, 1);
      Serial.println("%");
      
      return true;
    }
  }
  return sensorOK;
}

// 发布传感器数据
void publishSensorData() {
  unsigned long currentTime = millis();
  
  if (currentTime - lastPublish >= publishInterval) {
    lastPublish = currentTime;
    
    if (!mqttClient.connected()) {
      reconnect();
    }
    
    if (mqttClient.connected()) {
      StaticJsonDocument<256> doc;
      doc["device"] = deviceId;
      doc["temperature"] = temp;
      doc["humidity"] = hum;
      doc["timestamp"] = millis();
      doc["unit_temp"] = "°C";
      doc["unit_humi"] = "%";
      doc["sensor_connected"] = sensorOK;
      
      char buffer[256];
      serializeJson(doc, buffer);
      
      if (mqttClient.publish(topic_data, buffer)) {
        publishCount++;
        Serial.println("✅ 数据已发布");
        Serial.print("📤 主题: ");
        Serial.println(topic_data);
        Serial.print("📄 数据: ");
        Serial.println(buffer);
        Serial.print("📊 发布次数: ");
        Serial.println(publishCount);
      } else {
        Serial.println("❌ MQTT发布失败");
      }
    }
  }
  
  mqttClient.loop();
}

// 发布设备状态
void publishDeviceStatus() {
  StaticJsonDocument<512> doc;
  doc["device"] = deviceId;
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  doc["rssi"] = WiFi.RSSI();
  doc["uptime"] = millis() / 1000;
  doc["temperature"] = temp;
  doc["humidity"] = hum;
  doc["sensor_connected"] = sensorOK;
  doc["wifi_connected"] = (WiFi.status() == WL_CONNECTED);
  doc["mqtt_connected"] = mqttClient.connected();
  doc["publish_count"] = publishCount;
  doc["free_heap"] = ESP.getFreeHeap();
  
  char buffer[512];
  serializeJson(doc, buffer);
  mqttClient.publish(topic_status, buffer);
  
  Serial.print("📊 已发布设备状态: ");
  Serial.println(buffer);
}

// 发布状态消息
void publishStatus(String message) {
  StaticJsonDocument<128> doc;
  doc["device"] = deviceId;
  doc["message"] = message;
  doc["timestamp"] = millis();
  
  char buffer[128];
  serializeJson(doc, buffer);
  mqttClient.publish(topic_status, buffer);
  
  Serial.print("💬 状态消息: ");
  Serial.println(message);
}

// 发布WiFi信息
void publishWiFiInfo() {
  StaticJsonDocument<256> doc;
  doc["device"] = deviceId;
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
  mqttClient.publish(topic_status, buffer);
  
  Serial.println("📶 发送WiFi信息");
}

// 手动发布数据（用于Web界面）
void manualPublish() {
  if (sensorOK) {
    publishSensorData();
  } else {
    publishStatus("传感器未就绪，无法发布数据");
  }
}

// Web界面
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP8266 DHT22 传感器监控</title>
<style>
body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }
.container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
.header { text-align: center; margin-bottom: 20px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; padding: 20px; border-radius: 8px; }
.status { padding: 10px; border-radius: 5px; margin: 10px 0; text-align: center; }
.status-ok { background: #d4edda; color: #155724; }
.status-error { background: #f8d7da; color: #721c24; }
.data-box { display: flex; justify-content: space-around; margin: 20px 0; flex-wrap: wrap; }
.data-card { padding: 20px; border-radius: 10px; text-align: center; width: 45%; margin-bottom: 10px; }
.temp { background: linear-gradient(135deg, #ff6b6b, #ee5a52); color: white; }
.hum { background: linear-gradient(135deg, #4ecdc4, #44a08d); color: white; }
.data-value { font-size: 2.5em; font-weight: bold; margin: 10px 0; }
.info { background: #e7f3ff; padding: 15px; border-radius: 8px; margin: 20px 0; }
.control-panel { background: #f8f9fa; padding: 20px; border-radius: 8px; margin: 20px 0; }
.control-buttons { display: flex; flex-wrap: wrap; gap: 10px; margin-top: 10px; }
.btn { padding: 10px 20px; border: none; border-radius: 5px; cursor: pointer; font-weight: bold; transition: all 0.3s; }
.btn-primary { background: #667eea; color: white; }
.btn-success { background: #28a745; color: white; }
.btn-warning { background: #ffc107; color: black; }
.btn-danger { background: #dc3545; color: white; }
.btn:hover { transform: translateY(-2px); box-shadow: 0 5px 15px rgba(0,0,0,0.1); }
</style>
</head><body>
<div class="container">
<div class="header">
<h1>🌡️ ESP8266 DHT22 传感器监控</h1>
<p>支持MQTT远程控制和Web界面监控</p>
</div>

<div class="status )rawliteral" + String(sensorOK ? "status-ok" : "status-error") + R"rawliteral(">
传感器状态: <strong>)rawliteral" + String(sensorOK ? "正常" : "异常") + R"rawliteral(</strong> | 
MQTT: <strong>)rawliteral" + String(mqttClient.connected() ? "已连接" : "未连接") + R"rawliteral(</strong>
</div>

<div class="data-box">
<div class="data-card temp">
<div>🌡️ 温度</div>
<div class="data-value">)rawliteral" + (sensorOK ? String(temp, 1) + "°C" : "N/A") + R"rawliteral(</div>
</div>
<div class="data-card hum">
<div>💧 湿度</div>
<div class="data-value">)rawliteral" + (sensorOK ? String(hum, 1) + "%" : "N/A") + R"rawliteral(</div>
</div>
</div>

<div class="info">
<h3>📊 设备信息</h3>
<p><strong>设备ID:</strong> )rawliteral" + deviceId + R"rawliteral(</p>
<p><strong>IP地址:</strong> )rawliteral" + WiFi.localIP().toString() + R"rawliteral(</p>
<p><strong>WiFi信号:</strong> )rawliteral" + String(WiFi.RSSI()) + R"rawliteral( dBm</p>
<p><strong>运行时间:</strong> )rawliteral" + String(millis() / 1000) + R"rawliteral( 秒</p>
<p><strong>发布次数:</strong> )rawliteral" + String(publishCount) + R"rawliteral(</p>
<p><strong>空闲内存:</strong> )rawliteral" + String(ESP.getFreeHeap()) + R"rawliteral( 字节</p>
</div>

<div class="control-panel">
<h3>🔄 手动控制</h3>
<div class="control-buttons">
<button class="btn btn-primary" onclick="sendCommand('GET_ALL')">获取传感器数据</button>
<button class="btn btn-success" onclick="sendCommand('STATUS')">获取设备状态</button>
<button class="btn btn-warning" onclick="sendCommand('WIFI_INFO')">获取WiFi信息</button>
<button class="btn btn-danger" onclick="sendCommand('RESTART')">重启设备</button>
</div>
</div>

<div class="info">
<h3>📡 MQTT配置</h3>
<p><strong>数据主题:</strong> ch_dzbg/data/sensor</p>
<p><strong>状态主题:</strong> ch_dzbg/data/status</p>
<p><strong>命令主题:</strong> ch_dzbg/data/cmd</p>
<p><strong>订阅主题:</strong> ch_dzbg/data/#</p>
</div>

<div style="text-align: center; color: #666; font-size: 0.9em; margin-top: 20px;">
<p>📶 数据每10秒自动发布到MQTT | 🔄 最后更新: )rawliteral" + String(millis() / 1000) + R"rawliteral( 秒前</p>
<p>📱 使用浏览器访问 <a href="http://)rawliteral" + WiFi.localIP().toString() + R"rawliteral(" target="_blank">网页监控界面</a> 获得更佳体验</p>
</div>
</div>

<script>
function sendCommand(cmd) {
  fetch('/cmd?command=' + cmd)
    .then(response => response.text())
    .then(data => {
      alert('命令已发送: ' + cmd + '\n响应: ' + data);
      location.reload();
    })
    .catch(error => alert('发送失败: ' + error));
}
</script>
</body></html>
)rawliteral";

  server.send(200, "text/html", html);
}

// Web界面命令处理
void handleCommand() {
  String cmd = server.arg("command");
  cmd.toUpperCase();
  
  if (cmd == "GET_ALL") {
    manualPublish();
    server.send(200, "text/plain", "命令已执行: 发布传感器数据");
  } 
  else if (cmd == "STATUS") {
    publishDeviceStatus();
    server.send(200, "text/plain", "命令已执行: 发布设备状态");
  }
  else if (cmd == "WIFI_INFO") {
    publishWiFiInfo();
    server.send(200, "text/plain", "命令已执行: 发布WiFi信息");
  }
  else if (cmd == "RESTART") {
    server.send(200, "text/plain", "设备将在3秒后重启...");
    delay(3000);
    ESP.restart();
  }
  else {
    server.send(400, "text/plain", "未知命令: " + cmd);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n========================================");
  Serial.println("🌡️  ESP8266 DHT22 MQTT监控系统");
  Serial.println("========================================");
  
  // 初始化DHT传感器
  Serial.println("初始化DHT22传感器...");
  dht.begin();
  delay(2000);  // 给传感器稳定时间
  
  // 连接WiFi
  setupWiFi();
  
  // 设置MQTT
  mqttClient.setServer(mqtt_server, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);
  mqttClient.setKeepAlive(60);
  
  // 设置Web服务器
  server.on("/", handleRoot);
  server.on("/cmd", handleCommand);
  server.on("/data", []() {
    String json = "{";
    json += "\"device\":\"" + deviceId + "\",";
    json += "\"sensor_connected\":" + String(sensorOK ? "true" : "false") + ",";
    json += "\"temperature\":" + (sensorOK ? String(temp, 1) : "null") + ",";
    json += "\"humidity\":" + (sensorOK ? String(hum, 1) : "null") + ",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"uptime\":" + String(millis() / 1000);
    json += "}";
    server.send(200, "application/json", json);
  });
  
  server.begin();
  Serial.println("✅ HTTP服务器已启动");
  Serial.print("🌐 访问地址: http://");
  Serial.println(WiFi.localIP());
  
  // 连接MQTT
  reconnect();
  
  Serial.println("\n✅ 系统初始化完成");
  Serial.println("========================================");
  Serial.println("📤 数据发布主题: " + String(topic_data));
  Serial.println("📥 命令订阅主题: " + String(topic_command));
  Serial.println("📡 状态发布主题: " + String(topic_status));
  Serial.println("========================================");
  
  // 发布初始状态
  publishDeviceStatus();
}

void loop() {
  server.handleClient();
  
  // 检查WiFi连接
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi连接断开，尝试重连...");
    setupWiFi();
  }
  
  // 保持MQTT连接
  if (!mqttClient.connected()) {
    reconnect();
  } else {
    mqttClient.loop();
    
    // 读取传感器
    readSensor();
    
    // 发布数据
    publishSensorData();
  }
  
  delay(100);
}