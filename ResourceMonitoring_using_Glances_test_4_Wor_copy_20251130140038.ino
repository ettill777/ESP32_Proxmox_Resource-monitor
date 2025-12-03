#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>

// ---- OLED Display (modify depending on your module) ----
// Common SSD1306 128x64 I2C:
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ---- WiFi ----
const char* ssid = "FTL-Drive";
const char* password = "7727083064";

// ---- Glances Server ----
const char* glances_ip = "192.168.1.11";
const int glances_port = 61208;


// --- Page timing ---
const unsigned long PAGE_INTERVAL_MS = 3000; // 2s per page
bool pageCPU = true;
unsigned long prevPageMillis = 0;

// --- Network sampling & averaging ---
const int NUM_SAMPLES = 30;
float rx_samples[NUM_SAMPLES] = {0};
float tx_samples[NUM_SAMPLES] = {0};
int sample_index = 0;
bool buffer_filled = false;
unsigned long prevSampleMillis = 0;
const int SAMPLE_INTERVAL_MS = 400;
float max_Bps = 312500000.0; // fallback max speed

// --- Network mid-page updates ---
const int NUM_NETWORK_UPDATES = 4; 
const unsigned long NETWORK_UPDATE_MS = PAGE_INTERVAL_MS / NUM_NETWORK_UPDATES; 
unsigned long prevNetworkUpdate = 0;

// --- Function prototypes ---
void drawBar(int x, int y, int w, int h, float percent);
float getCpuTempFromSensors(JsonVariant sensors);
float fetchFloatFromEndpoint(const String& url, const char* key);
void fetchNetworkSample(float &rx, float &tx);
void drawCPUPage();
void drawNetworkPage(float rx_avg, float tx_avg);
float average(float* arr);

void setup() {
  Serial.begin(115200);
  u8g2.begin();
  u8g2.setFont(u8g2_font_8x13_tf);

  u8g2.clearBuffer();
  u8g2.setCursor(0, 12);
  u8g2.print("Connecting WiFi...");
  u8g2.sendBuffer();

  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    if (millis() - start > 15000) break;
  }

  u8g2.clearBuffer();
  u8g2.setCursor(0, 12);
  u8g2.print(WiFi.status() == WL_CONNECTED ? "WiFi connected" : "WiFi failed");
  u8g2.sendBuffer();
  delay(900);

  prevPageMillis = millis();
  prevSampleMillis = millis();
  prevNetworkUpdate = millis();
}

void loop() {
  unsigned long now = millis();

  // --- PAGE SWITCH ---
  if (now - prevPageMillis >= PAGE_INTERVAL_MS) {
    prevPageMillis = now;
    pageCPU = !pageCPU;

    if (!pageCPU) {
      // Entering network page
      sample_index = 0;
      buffer_filled = false;
      prevSampleMillis = now;
      prevNetworkUpdate = now;
      for (int i = 0; i < NUM_SAMPLES; i++) rx_samples[i] = tx_samples[i] = 0;
    }
  }

  // --- PAGE DISPLAY ---
  if (pageCPU) {
    drawCPUPage();
  } else {
    // Network sampling every SAMPLE_INTERVAL_MS
    if (now - prevSampleMillis >= SAMPLE_INTERVAL_MS) {
      prevSampleMillis = now;
      float rx_cur, tx_cur;
      fetchNetworkSample(rx_cur, tx_cur);
      rx_samples[sample_index] = rx_cur;
      tx_samples[sample_index] = tx_cur;
      sample_index = (sample_index + 1) % NUM_SAMPLES;
      if (sample_index == 0) buffer_filled = true;
    }

    // Network page mid-page update
    if (now - prevNetworkUpdate >= NETWORK_UPDATE_MS) {
      prevNetworkUpdate = now;
      drawNetworkPage(average(rx_samples), average(tx_samples));
    }
  }
}

// --- CPU Page ---
void drawCPUPage() {
  float cpu_usage = fetchFloatFromEndpoint(String("http://") + glances_ip + ":" + glances_port + "/api/4/cpu", "total");
  float mem_usage = fetchFloatFromEndpoint(String("http://") + glances_ip + ":" + glances_port + "/api/4/mem", "percent");
  float cpu_temp = NAN;

  HTTPClient http;
  http.begin(String("http://") + glances_ip + ":" + glances_port + "/api/4/sensors");
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    if (payload.length() > 0) {
      DynamicJsonDocument doc(20000);
      if (!deserializeJson(doc, payload)) cpu_temp = getCpuTempFromSensors(doc);
    }
  }
  http.end();

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_8x13_tf);

  u8g2.setCursor(1, 14);
  u8g2.print("CPU ");
  u8g2.print(!isnan(cpu_usage) ? String(cpu_usage, 1) : "N/A");
  u8g2.print(!isnan(cpu_usage) ? "%" : "");
  drawBar(75, 0, 50, 14, isnan(cpu_usage) ? 0 : cpu_usage);

  u8g2.setCursor(1, 34);
  u8g2.print("MEM ");
  u8g2.print(!isnan(mem_usage) ? String(mem_usage, 1) : "N/A");
  u8g2.print(!isnan(mem_usage) ? "%" : "");
  drawBar(75, 22, 50, 14, isnan(mem_usage) ? 0 : mem_usage);

  u8g2.setCursor(1, 54);
  u8g2.print("Temp ");
  if (!isnan(cpu_temp)) u8g2.print(cpu_temp, 1), u8g2.print("C");
  else u8g2.print("N/A");

  u8g2.sendBuffer();
}

// --- Network sampling ---
void fetchNetworkSample(float &rx, float &tx) {
  rx = tx = 0;
  HTTPClient http;
  String net_url = String("http://") + glances_ip + ":" + glances_port + "/api/4/network";
  http.begin(net_url);
  http.setTimeout(2000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(20000);
    if (!deserializeJson(doc, payload)) {
      JsonArray arr = doc.as<JsonArray>();
      for (JsonObject iface : arr) {
        if (strcmp(iface["interface_name"], "nic2") == 0) {
          rx = iface["bytes_recv_rate_per_sec"].as<float>();
          tx = iface["bytes_sent_rate_per_sec"].as<float>();
          if (iface.containsKey("speed")) max_Bps = iface["speed"].as<float>() / 8.0;
          break;
        }
      }
    }
  }
  http.end();
}

void drawNetworkPage(float rx_avg, float tx_avg) {
  auto formatRate = [](float bytesPerSec, String &unit) -> float {
    float value = bytesPerSec;
    unit = "B/s";
    if (value >= 1024.0) { value /= 1024.0; unit = "Ks"; }
    if (value >= 1024.0) { value /= 1024.0; unit = "Ms"; }
    if (value >= 1024.0) { value /= 1024.0; unit = "Gs"; }
    return value;
  };

  String rxUnit, txUnit;
  float rxValue = formatRate(rx_avg, rxUnit);
  float txValue = formatRate(tx_avg, txUnit);

  float rx_percent = (rx_avg / max_Bps) * 100.0;
  float tx_percent = (tx_avg / max_Bps) * 100.0;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_8x13_tf);

  u8g2.setCursor(1, 14);
  u8g2.print(" nic2 interface  ");

  u8g2.setCursor(1, 34);
  u8g2.print("R ");
  u8g2.print(rxValue, 1);
  //u8g2.print(" ");
  u8g2.print(rxUnit);
  drawBar(80, 22, 45, 14, rx_percent);

  u8g2.setCursor(1, 54);
  u8g2.print("T ");
  u8g2.print(txValue, 1);
  //u8g2.print(" ");
  u8g2.print(txUnit);
  drawBar(80, 44, 45, 14, tx_percent);

  u8g2.sendBuffer();
}

// --- Helpers ---
float average(float* arr) {
  float sum = 0;
  int count = buffer_filled ? NUM_SAMPLES : sample_index;
  for (int i = 0; i < count; i++) sum += arr[i];
  return count ? sum / count : 0;
}

void drawBar(int x, int y, int w, int h, float percent) {
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  u8g2.drawFrame(x, y, w, h);
  int fill = (int)((percent / 100.0f) * (w - 2));
  if (fill > 0) u8g2.drawBox(x + 1, y + 1, fill, h - 2);
}

float getCpuTempFromSensors(JsonVariant sensors) {
  if (!sensors.is<JsonArray>()) return NAN;
  for (JsonObject s : sensors.as<JsonArray>()) {
    const char* label = s["label"];
    float val = s["value"];
    if (label && strcmp(label, "Composite") == 0) if (val > 5 && val < 130) return val;
  }
  for (JsonObject s : sensors.as<JsonArray>()) {
    if (s.containsKey("value")) { float v = s["value"]; if (v > 5 && v < 130) return v; }
  }
  return NAN;
}

float fetchFloatFromEndpoint(const String& url, const char* key) {
  float val = NAN;
  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    if (payload.length() > 0) {
      DynamicJsonDocument doc(20000);
      if (!deserializeJson(doc, payload)) if (doc[key]) val = doc[key].as<float>();
    }
  }
  http.end();
  return val;
}
