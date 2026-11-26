// must be defined before FreeRTOS headers are pulled in
#define configUSE_TRACE_FACILITY 1
#define configGENERATE_RUN_TIME_STATS 1

#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <Adafruit_BMP280.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ================= CPU LOAD =================
// Computed once per second in loop() only — never from the async HTTP handlers,
// so concurrent /data requests cannot race on the sampling state.
static uint32_t lastIdleTime = 0;
static uint32_t lastTotalTime = 0;
volatile float cpuLoad = 0;

void updateCpuLoad() {
  TaskStatus_t taskStatusArray[20];
  UBaseType_t taskCount;
  uint32_t totalRunTime = 0;

  taskCount = uxTaskGetSystemState(taskStatusArray, 20, &totalRunTime);
  if (taskCount == 0 || totalRunTime == 0) return;

  uint32_t idleTime = 0;
  for (UBaseType_t i = 0; i < taskCount; i++) {
    if (strstr(taskStatusArray[i].pcTaskName, "IDLE") != NULL) {
      idleTime += taskStatusArray[i].ulRunTimeCounter;
    }
  }

  // instantaneous load: difference between two samples (not average since boot)
  uint32_t dTotal = totalRunTime - lastTotalTime;
  uint32_t dIdle = idleTime - lastIdleTime;
  lastTotalTime = totalRunTime;
  lastIdleTime = idleTime;

  if (dTotal == 0) return;

  float cpu = 100.0f - ((float)dIdle / (float)dTotal) * 100.0f;
  if (cpu < 0) cpu = 0;
  if (cpu > 100) cpu = 100;
  cpuLoad = cpu;
}

// ================= WIFI =================
const char *ssid = "YOUR_WIFI_SSID";       // replace with your 2.4 GHz network name
const char *password = "YOUR_WIFI_PASSWORD"; // replace with your Wi-Fi password

AsyncWebServer server(80);

// STATIC IP
IPAddress local_IP(192, 168, 1, 222);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);

unsigned long wifiLastAttempt = 0;
unsigned long wifiStartAttempt = 0;
bool wifiConnecting = false;

// ================= PIN =================
#define SDA_PIN 4
#define SCL_PIN 5
#define SERVO_PIN 10
#define AHT20_ADDR 0x38
#define BUZZER_PIN 6
#define SERVO_CENTER 75
#define SERVO_RANGE 20
#define SERVO_ON  (SERVO_CENTER - SERVO_RANGE + 5)   // 60° — original 55° + 5°
#define SERVO_OFF (SERVO_CENTER + SERVO_RANGE)       // 95°

Servo lampServo;

void beep(int durationMs) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}
// ================= NTP =================
const char *ntpServer = "id.pool.ntp.org";
const long gmtOffset_sec = 7 * 3600;

// ================= SENSOR =================
float humidity = 0;
float tempAHT = 0;
float tempBMP = 0;
float pressure = 0;
float altitude = 0;   // metres, derived from pressure (QNH = 1013.25 hPa)
float tempOffset = 0; // AHT20 calibration offset in °C (persisted in NVS)
float bmpOffset = 0;  // BMP280 calibration offset in °C (it self-heats more than the AHT20)

Adafruit_BMP280 bmp;
bool bmpOK = false;

unsigned long ahtReads = 0;  // count of successful AHT20 reads (for /aht)
int ahtTriggerErr = -1, ahtLastCount = -1;

// ================= TIMER =================
unsigned long lastSensor = 0;
unsigned long lastTimeCheck = 0;
unsigned long lastCpuCheck = 0;
unsigned long lastBmpCheck = 0;

// ================= SERVO =================
bool lampState = false;

void lampON() {
  lampServo.write(SERVO_ON);
  lampState = true;
}

void lampOFF() {
  lampServo.write(SERVO_OFF);
  lampState = false;
}

// ================= JADWAL =================
int onHour = 18, onMinute = 0;
int offHour = 6, offMinute = 0;

void checkSchedule() {
  struct tm t;
  if (!getLocalTime(&t)) return;

  // latch by minute so a drifting loop can never miss (or repeat) a trigger
  static int lastFired = -1;
  int now = t.tm_hour * 60 + t.tm_min;

  if (now == onHour * 60 + onMinute && lastFired != now) {
    lampON();
    lastFired = now;
  }
  if (now == offHour * 60 + offMinute && lastFired != now) {
    lampOFF();
    lastFired = now;
  }
}

// ================= WIFI =================
void initWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.setAutoReconnect(false);
  Serial.println("WiFi INIT...");
}

void handleWiFi() {

  wl_status_t status = WiFi.status();

  // ================= CONNECTED =================
  if (status == WL_CONNECTED) {
    if (wifiConnecting) {
      Serial.println("✅ WiFi Connected!");
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());

      // 🔊 Bunyi sukses (2x)
      beep(100);
      delay(100);
      beep(100);

      wifiConnecting = false;
    }
    return;
  }

  // ================= TIMEOUT =================
  if (wifiConnecting) {
    if (millis() - wifiStartAttempt >= 10000) {

      Serial.println("⏱ Timeout, reset WiFi...");

      // 🔊 Bunyi gagal (3x)
      beep(50);
      delay(50);
      beep(50);
      delay(50);
      beep(50);

      WiFi.disconnect(true);
      wifiConnecting = false;
    }
    return; // penting supaya tidak langsung reconnect di loop yang sama
  }

  // ================= START CONNECT =================
  if (millis() - wifiLastAttempt > 5000) {

    Serial.println("🔄 Start WiFi connect...");

    WiFi.disconnect(false);
    delay(100);

    WiFi.config(local_IP, gateway, subnet, dns);
    WiFi.begin(ssid, password);

    wifiStartAttempt = millis();
    wifiLastAttempt = millis();
    wifiConnecting = true;
  }
}


// ================= SENSOR =================
void readAHT20() {
  // non-blocking: trigger now, collect on the next call (3 s later, > 80 ms)
  static bool ahtPending = false;
  static unsigned long ahtTriggerAt = 0;

  if (!ahtPending) {
    Wire.beginTransmission(AHT20_ADDR);
    Wire.write(0xAC);
    Wire.write(0x33);
    Wire.write(0x00);
    ahtTriggerErr = Wire.endTransmission();
    ahtTriggerAt = millis();
    ahtPending = true;
    return;
  }
  if (millis() - ahtTriggerAt < 100) return;
  ahtPending = false;

  ahtLastCount = Wire.requestFrom(AHT20_ADDR, 7);
  if (ahtLastCount == 7) {
    uint8_t d[7];
    for (int i = 0; i < 7; i++) d[i] = Wire.read();
    ahtReads++;

    if (d[0] & 0x80) return;  // still busy -> retry on the next cycle

    uint32_t rawH = ((uint32_t)d[1] << 12) | ((uint32_t)d[2] << 4) | (d[3] >> 4);
    uint32_t rawT = ((uint32_t)(d[3] & 0x0F) << 16) | ((uint32_t)d[4] << 8) | d[5];

    humidity = rawH * 100.0 / 1048576.0;
    tempAHT = rawT * 200.0 / 1048576.0 - 50.0 + tempOffset;
  }
}

// ================= TIME =================
String getDateTimeID() {
  struct tm t;
  if (!getLocalTime(&t)) return "Memuat waktu...";

  const char *hari[] = { "Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu" };
  const char *bulan[] = { "Januari", "Februari", "Maret", "April", "Mei", "Juni",
                          "Juli", "Agustus", "September", "Oktober", "November", "Desember" };

  char buf[100];
  sprintf(buf, "%02d:%02d:%02d WIB | %s, %02d %s %d",
          t.tm_hour, t.tm_min, t.tm_sec,
          hari[t.tm_wday], t.tm_mday, bulan[t.tm_mon], t.tm_year + 1900);

  return String(buf);
}

// ================= WEB =================
String page() {
  return R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">

<style>
body{
  margin:0;
  font-family: 'Segoe UI', sans-serif;
  background: linear-gradient(135deg,#0f2027,#203a43,#2c5364);
  color:white;
  text-align:center;
}

h2{margin-top:15px}
#jam{opacity:0.8}

.grid{
  display:grid;
  grid-template-columns: repeat(auto-fit,minmax(150px,1fr));
  gap:10px;
  padding:10px;
}

.card{
  background: rgba(255,255,255,0.08);
  border-radius:15px;
  padding:15px;
  box-shadow:0 0 10px rgba(0,0,0,0.3);
}

.value{
  font-size:22px;
  font-weight:bold;
  margin-top:5px;
}

button{
  padding:10px 15px;
  border:none;
  border-radius:10px;
  margin:5px;
  font-size:14px;
  cursor:pointer;
}

.on{background:#00c853;color:white}
.off{background:#d50000;color:white}

.indikator{
  width:10px;height:10px;border-radius:50%;
  display:inline-block;margin-left:5px;
}
.active{background:#00e5ff;box-shadow:0 0 8px #00e5ff;}

input{
  padding:8px;
  border-radius:8px;
  border:none;
  margin:5px;
}
</style>
<meta charset="UTF-8">
</head>

<body>

<h2>🌐 ESP32 SMART DASHBOARD</h2>
<h3 id="jam">--</h3>

<div class="grid">
  <div class="card">
    🌡️ AHT20
    <div class="value"><span id="t1">--</span> °C</div>
  </div>

  <div class="card">
    🌡️ BMP280
    <div class="value"><span id="t2">--</span> °C</div>
  </div>

  <div class="card">
    💧 Humidity
    <div class="value"><span id="h">--</span> %</div>
  </div>

  <div class="card">
    🌬️ Pressure
    <div class="value"><span id="p">--</span> hPa</div>
  </div>
</div>

<div class="grid">

  <div class="card">
    📶 WiFi SSID
    <div class="value" id="ssid">--</div>
  </div>

  <div class="card">
    📡 RSSI
    <div class="value"><span id="rssi">--</span> dBm</div>
  </div>

  <div class="card">
    🧠 CPU Load
    <div class="value"><span id="cpu">--</span> %</div>
  </div>

  <div class="card">
    ⛰️ Altitude
    <div class="value"><span id="alt">--</span> m</div>
  </div>

</div>

<div class="card">
<h3>💡 Lampu Teras</h3>
<button class="on" onclick="setLamp('on')">ON <span id="ledOn" class="indikator"></span></button>
<button class="off" onclick="setLamp('off')">OFF <span id="ledOff" class="indikator"></span></button>
</div>

<div class="card">
<h3>⏰ Jadwal Aktif</h3>
ON: <span id="jon">--</span><br>
OFF: <span id="joff">--</span>
</div>

<div class="card">
<h3>⚙️ Set Jadwal</h3>
<input type="time" id="on"><br>
<input type="time" id="off"><br>
<button onclick="save()">Simpan</button>
</div>

<script>

// DATA SENSOR
setInterval(()=>{
fetch('/data').then(r=>r.json()).then(d=>{
ssid.innerText = d.ssid;
rssi.innerText = d.rssi;
cpu.innerText = d.cpu;
alt.innerText = d.alt.toFixed(1);
t1.innerText=d.t1.toFixed(1);
t2.innerText=d.t2.toFixed(1);
h.innerText=d.h.toFixed(1);
p.innerText=d.p.toFixed(1);
});
},3000);

// JAM
setInterval(()=>{
fetch('/time').then(r=>r.text()).then(t=>jam.innerText=t);
},1000);

// JADWAL
function loadJadwal(){
fetch('/jadwal').then(r=>r.json()).then(j=>{
jon.innerText=j.on;
joff.innerText=j.off;
});
}
setInterval(loadJadwal,3000);

// SIMPAN
function save(){
let on=document.getElementById("on").value;
let off=document.getElementById("off").value;
fetch(`/set?on=${on}&off=${off}`);
}

// LAMPU
function setLamp(s){fetch('/'+s);}

function updateLamp(){
fetch('/lamp').then(r=>r.text()).then(s=>{
ledOn.classList.remove("active");
ledOff.classList.remove("active");
if(s=="on") ledOn.classList.add("active");
else ledOff.classList.add("active");
});
}
setInterval(updateLamp,1000);
updateLamp();

</script>

</body>
</html>
)rawliteral";
}

// ================= SETUP =================

void setup() {
  Serial.begin(115200);

  Preferences prefs;
  prefs.begin("calib", true);
  tempOffset = prefs.getFloat("toff", 0.0f);
  bmpOffset = prefs.getFloat("boff", 0.0f);
  prefs.end();

  Wire.begin(SDA_PIN, SCL_PIN);
  // Wire.setClock(100000);  // default is already 100 kHz

  bmpOK = bmp.begin(0x76) || bmp.begin(0x77);

  bmp.setSampling(
    Adafruit_BMP280::MODE_NORMAL,
    Adafruit_BMP280::SAMPLING_X1,
    Adafruit_BMP280::SAMPLING_X4,
    Adafruit_BMP280::FILTER_OFF,
    Adafruit_BMP280::STANDBY_MS_250);
  lampServo.attach(SERVO_PIN);
  lampServo.write(SERVO_CENTER);

  initWiFi();
  configTime(gmtOffset_sec, 0, ntpServer);
  vTaskDelay(1000 / portTICK_PERIOD_MS);
 //================= BUZZER ==============
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);


  // ================= ROUTES =================

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", page());
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    String ssidJson = WiFi.SSID();
    ssidJson.replace("\\", "\\\\");
    ssidJson.replace("\"", "\\\"");

    char json[256];
    snprintf(json, sizeof(json),
             "{\"t1\":%.1f,\"t2\":%.1f,\"h\":%.1f,\"p\":%.1f,"
             "\"rssi\":%d,\"alt\":%.1f,\"cpu\":%.1f,\"off\":%.2f,\"boff\":%.2f,\"ssid\":\"%s\"}",
             tempAHT, tempBMP, humidity, pressure,
             WiFi.RSSI(), altitude,
             cpuLoad, tempOffset, bmpOffset,
             ssidJson.c_str());

    request->send(200, "application/json", json);
  });

  server.on("/time", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", getDateTimeID());
  });

  server.on("/lamp", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", lampState ? "on" : "off");
  });


  server.on("/jadwal", HTTP_GET, [](AsyncWebServerRequest *request) {
    char onBuf[6];
    char offBuf[6];

    sprintf(onBuf, "%02d:%02d", onHour, onMinute);
    sprintf(offBuf, "%02d:%02d", offHour, offMinute);

    String json = "{\"on\":\"" + String(onBuf) + "\",\"off\":\"" + String(offBuf) + "\"}";

    request->send(200, "application/json", json);
  });

  server.on("/aht", HTTP_GET, [](AsyncWebServerRequest *request) {
    Wire.beginTransmission(0x77);
    Wire.write((uint8_t)0x00);   // BMP280 calib reg, read-only -> harmless probe
    int bmpWriteErr = Wire.endTransmission();

    char buf[180];
    snprintf(buf, sizeof(buf),
             "temp=%.1f hum=%.1f reads=%lu (bmp77 writeErr=%d)",
             tempAHT, humidity, ahtReads, bmpWriteErr);
    request->send(200, "text/plain", buf);
  });

  server.on("/offset", HTTP_GET, [](AsyncWebServerRequest *request) {
    bool changed = false;
    if (request->hasParam("v")) {
      float v = request->getParam("v")->value().toFloat();
      if (v >= -20.0f && v <= 20.0f) { tempOffset = v; changed = true; }
    }
    if (request->hasParam("b")) {
      float b = request->getParam("b")->value().toFloat();
      if (b >= -20.0f && b <= 20.0f) { bmpOffset = b; changed = true; }
    }
    if (changed) {
      Preferences p;
      p.begin("calib", false);
      p.putFloat("toff", tempOffset);
      p.putFloat("boff", bmpOffset);
      p.end();
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "aht=%.2f bmp=%.2f", tempOffset, bmpOffset);
    request->send(200, "text/plain", buf);
  });

  server.on("/bmp", HTTP_GET, [](AsyncWebServerRequest *request) {
    Wire.beginTransmission(0x77);
    Wire.write(0xD0);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)0x77, (byte)1);
    uint8_t chipId = Wire.available() ? Wire.read() : 0;

    Wire.beginTransmission(0x77);
    Wire.write(0xFA);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)0x77, (byte)3);
    uint32_t rawT = 0;
    if (Wire.available() >= 3) {
      rawT = ((uint32_t)Wire.read() << 16) | ((uint32_t)Wire.read() << 8) | Wire.read();
    }
    rawT >>= 4;

    Wire.beginTransmission(0x77);
    Wire.write(0xF7);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)0x77, (byte)3);
    uint32_t rawP = 0;
    if (Wire.available() >= 3) {
      rawP = ((uint32_t)Wire.read() << 16) | ((uint32_t)Wire.read() << 8) | Wire.read();
    }
    rawP >>= 4;

    char buf[160];
    Wire.beginTransmission(0x77);
    Wire.write(0xF4);
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)0x77, (byte)1);
    uint8_t ctrl = Wire.available() ? Wire.read() : 0;

    snprintf(buf, sizeof(buf),
             "chipId=0x%02X ctrl=0x%02X rawT=%lu rawP=%lu tempBMP=%.2f press=%.2f",
             chipId, ctrl, rawT, rawP, tempBMP, pressure);
    request->send(200, "text/plain", buf);
  });

  server.on("/i2c", HTTP_GET, [](AsyncWebServerRequest *request) {
    String found = "";
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        char buf[8];
        snprintf(buf, sizeof(buf), "0x%02X ", addr);
        found += buf;
      }
    }
    if (found.length() == 0) found = "none";
    request->send(200, "text/plain", found);
  });

  server.on("/set", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("on") && request->hasParam("off")) {
      int h1, m1, h2, m2;
      String on = request->getParam("on")->value();
      String off = request->getParam("off")->value();

      if (sscanf(on.c_str(), "%d:%d", &h1, &m1) == 2 &&
          sscanf(off.c_str(), "%d:%d", &h2, &m2) == 2 &&
          h1 >= 0 && h1 < 24 && m1 >= 0 && m1 < 60 &&
          h2 >= 0 && h2 < 24 && m2 >= 0 && m2 < 60) {
        onHour = h1; onMinute = m1;
        offHour = h2; offMinute = m2;
        request->send(200, "text/plain", "OK");
        return;
      }
      request->send(400, "text/plain", "Invalid time");
      return;
    }
    request->send(400, "text/plain", "Missing parameter");
  });

  server.on("/on", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("LAMPU ON");
    lampON();
    request->send(200, "text/plain", "OK");
  });

  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("LAMPU OFF");
    lampOFF();
    request->send(200, "text/plain", "OK");
  });
 
  server.on("/sensor_rumah", HTTP_GET, [](AsyncWebServerRequest *request) {

  String json =
  "{"
  "\"nama\":\"sensor rumah\","
  "\"temperature\":" + String(tempAHT,1) + ","
  "\"humidity\":" + String(humidity,1) + ","
  "\"pressure\":" + String(pressure,1) + ","
  "\"lamp\":\"" + String(lampState ? "on" : "off") + "\""
  "}";

  request->send(200, "application/json", json);
  });
  server.begin();
}


void loop() {

  handleWiFi();
  

  // ================= SENSOR =================
  if (millis() - lastSensor > 3000) {

    readAHT20();

    if (bmpOK) {
      float newTemp = bmp.readTemperature() + bmpOffset;
      float newPress = bmp.readPressure() / 100.0;

      // VALIDASI biar gak stuck
      if (newTemp > -40 && newTemp < 85 &&
          newPress > 300 && newPress < 1100) {
        if (tempBMP == 0 && pressure == 0) {
          tempBMP = newTemp;
          pressure = newPress;
        } else {
          tempBMP = (tempBMP * 0.7) + (newTemp * 0.3);
          pressure = (pressure * 0.7) + (newPress * 0.3);
        }
      }
    }

    // altitude from pressure (ISA, sea-level reference QNH = 1013.25 hPa)
    if (pressure > 0) {
      altitude = 44330.0f * (1.0f - powf(pressure / 1013.25f, 1.0f / 5.255f));
    }

    lastSensor = millis();
  }

  // ================= JADWAL =================
  if (millis() - lastTimeCheck > 1000) {
    checkSchedule();
    lastTimeCheck = millis();
  }

  // ================= CPU LOAD (1 Hz, loop task only) =================
  if (millis() - lastCpuCheck > 1000) {
    updateCpuLoad();
    lastCpuCheck = millis();
  }

  // ================= BMP280 MODE WATCHDOG =================
  // A glitch on the I2C line can reset the sensor into sleep mode, leaving
  // the ADC registers frozen at 0x80000. Re-apply NORMAL mode if needed.
  if (bmpOK && millis() - lastBmpCheck > 10000) {
    Wire.beginTransmission(0x77);
    Wire.write(0xF4);                 // CTRL_MEAS
    Wire.endTransmission();
    Wire.requestFrom((uint8_t)0x77, (byte)1);
    uint8_t ctrl = Wire.available() ? Wire.read() : 0;
    if ((ctrl & 0x03) != 0x03) {       // not NORMAL mode -> reconfigure
      bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,
                      Adafruit_BMP280::SAMPLING_X1,
                      Adafruit_BMP280::SAMPLING_X4,
                      Adafruit_BMP280::FILTER_OFF,
                      Adafruit_BMP280::STANDBY_MS_250);
    }
    lastBmpCheck = millis();
  }

  delay(5);
}