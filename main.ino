/*
  ESP32 Counter -> Dashboard (PHP/MySQL)
  Library: ArduinoJson v7 (Library Manager). Board: ESP32 Dev Module.

  Pengkabelan (ESP32 hanya toleran 3.3 V!):
    GPIO27  <- sinyal harness, 1 sinyal = 1 harness (sensor/tombol ke GND, pakai INPUT_PULLUP)
    GPIO26  -> modul relay / transistor untuk output
  Sensor industri 12/24 V WAJIB lewat optocoupler atau pembagi tegangan.
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ====== SESUAIKAN ======
// const char* WIFI_SSID  = "";
// const char* WIFI_PASS  = "";
const char* WIFI_SSID  = "";
const char* WIFI_PASS  = "";
const char* SERVER_URL = "";
const char* API_KEY    = "";
const int   PIN_PULSE  = 27;
const int   PIN_OUT    = 26;
const unsigned long DEBOUNCE_US = 280000;  // abaikan sinyal < 150 ms (switch mekanis). Naikkan kalau masih dobel, turunkan kalau ada yang terlewat
const unsigned long POST_MS     = 7000;   // kirim data tiap 10 detik (hemat beban server untuk banyak alat)
const bool  SWITCH_NC  = true;  // false = pakai NO (hitung saat ditekan), true = pakai NC (hitung saat ditekan)
// =======================

volatile uint32_t count = 0;
volatile uint32_t lastPulseUs = 0;
uint32_t preset = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Preferences prefs;

uint32_t lastCmdId = 0;
uint32_t ackIds[8];
int ackN = 0;
unsigned long lastPost = 0, lastSave = 0;
uint32_t savedCount = 0, savedPreset = 0;

volatile bool siapHitung = true;

void IRAM_ATTR onPulse() {
  uint32_t now = micros();
  if (!siapHitung || (now - lastPulseUs <= DEBOUNCE_US)) {
    lastPulseUs = now;
    return;
  }
  bool dipencet = SWITCH_NC ? (digitalRead(PIN_PULSE) == HIGH) : (digitalRead(PIN_PULSE) == LOW);
  if (dipencet) {
    portENTER_CRITICAL_ISR(&mux);
    count++;
    portEXIT_CRITICAL_ISR(&mux);
    siapHitung = false;
  }
  lastPulseUs = now;
}

uint32_t getCount() {
  portENTER_CRITICAL(&mux);
  uint32_t c = count;
  portEXIT_CRITICAL(&mux);
  return c;
}
void setCount(uint32_t v) {
  portENTER_CRITICAL(&mux);
  count = v;
  portEXIT_CRITICAL(&mux);
}

void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 10000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi OK, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi GAGAL (cek SSID/password, harus WiFi 2,4 GHz)");
  }
}

void postData() {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClientSecure client;
  client.setInsecure();               // tanpa verifikasi sertifikat; ganti setCACert() bila perlu
  HTTPClient http;
  if (!http.begin(client, SERVER_URL)) return;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);

  JsonDocument doc;
  doc["key"]    = API_KEY;
  doc["count"]  = getCount();
  doc["preset"] = preset;
  doc["out"]    = digitalRead(PIN_OUT) == HIGH;
  doc["rssi"]   = WiFi.RSSI();
  JsonArray ack = doc["ack"].to<JsonArray>();
  for (int i = 0; i < ackN; i++) ack.add(ackIds[i]);

  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  Serial.printf("Kirim ke server: HTTP %d (hitungan %lu)\n", code, (unsigned long)getCount());
  if (code <= 0) Serial.println(http.errorToString(code));
  if (code == 200) {
    ackN = 0;                          // server sudah menerima konfirmasi
    JsonDocument res;
    if (!deserializeJson(res, http.getString()) && res["ok"] == true) {
      for (JsonObject c : res["commands"].as<JsonArray>()) {
        uint32_t id = c["id"];
        if (id > lastCmdId) {          // jangan jalankan ulang perintah yang sama
          String cmd = c["cmd"].as<String>();
          if (cmd == "reset") setCount(0);
          else if (cmd == "preset") preset = c["value"].as<uint32_t>();
          lastCmdId = id;
        }
        if (ackN < 8) ackIds[ackN++] = id;
      }
    }
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PULSE, INPUT_PULLUP);
  pinMode(PIN_OUT, OUTPUT);
  digitalWrite(PIN_OUT, LOW);

  prefs.begin("counter", false);       // hitungan & preset tetap ada setelah mati listrik
  setCount(prefs.getUInt("count", 0));
  preset = prefs.getUInt("preset", 0);
  savedCount = getCount(); savedPreset = preset;

  attachInterrupt(digitalPinToInterrupt(PIN_PULSE), onPulse, SWITCH_NC ? RISING : FALLING);
  connectWifi();
}

void loop() {
  // Output ON selama hitungan >= preset (preset 0 = output mati)
  bool sudahLepas = SWITCH_NC ? (digitalRead(PIN_PULSE) == LOW) : (digitalRead(PIN_PULSE) == HIGH);
  if (sudahLepas) siapHitung = true;

  bool out = (preset > 0 && getCount() >= preset);
  digitalWrite(PIN_OUT, out ? HIGH : LOW);

  if (millis() - lastPost >= POST_MS) {
    lastPost = millis();
    connectWifi();
    postData();
  }
  if (millis() - lastSave >= 5000) {   // simpan ke flash hanya bila berubah
    lastSave = millis();
    uint32_t c = getCount();
    if (c != savedCount)   { prefs.putUInt("count", c);    savedCount = c; }
    if (preset != savedPreset) { prefs.putUInt("preset", preset); savedPreset = preset; }
  }
  delay(5);
}
