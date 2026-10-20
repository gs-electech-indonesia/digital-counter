
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ===== KONFIGURASI =====
const char* WIFI_SSID = "";
const char* WIFI_PASS = "";
const char* SERVER_URL =
  "https://gseisystem.my.id/OP/api/ingest.php";
const char* API_KEY = "";

constexpr int PIN_PULSE = 27;
constexpr int PIN_OUT = 26;

// NC + INPUT_PULLUP + COM ke GND:
// tuas ditekan = HIGH
// tuas dilepas = LOW
constexpr bool SWITCH_NC = true;

constexpr uint32_t DEBOUNCE_MS = 30;
constexpr uint32_t POST_MS = 7000;
constexpr uint32_t SAVE_MS = 5000;

// ===== DATA BERSAMA =====
volatile uint32_t count = 0;
volatile uint32_t preset = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

Preferences prefs;

uint32_t lastCmdId = 0;
uint32_t ackIds[8];
int ackN = 0;

uint32_t getCount() {
  portENTER_CRITICAL(&mux);
  uint32_t value = count;
  portEXIT_CRITICAL(&mux);
  return value;
}

void setCount(uint32_t value) {
  portENTER_CRITICAL(&mux);
  count = value;
  portEXIT_CRITICAL(&mux);
}

void incrementCount() {
  portENTER_CRITICAL(&mux);
  count++;
  portEXIT_CRITICAL(&mux);
}

uint32_t getPreset() {
  portENTER_CRITICAL(&mux);
  uint32_t value = preset;
  portEXIT_CRITICAL(&mux);
  return value;
}

void setPreset(uint32_t value) {
  portENTER_CRITICAL(&mux);
  preset = value;
  portEXIT_CRITICAL(&mux);
}

// ===== TASK 1: BACA LIMIT SWITCH =====
void taskCounter(void* parameter) {
  const int pressedLevel = SWITCH_NC ? HIGH : LOW;
  const int releasedLevel = SWITCH_NC ? LOW : HIGH;

  int lastRaw = digitalRead(PIN_PULSE);
  int stableState = lastRaw;
  uint32_t lastChange = millis();

  for (;;) {
    uint32_t now = millis();
    int raw = digitalRead(PIN_PULSE);

    if (raw != lastRaw) {
      lastRaw = raw;
      lastChange = now;
    }

    // Terima perubahan hanya setelah stabil
    if (raw != stableState &&
        (uint32_t)(now - lastChange) >= DEBOUNCE_MS) {

      int previousState = stableState;
      stableState = raw;

      // Satu hitungan ketika transisi stabil:
      // ditekan -> dilepas
      if (previousState == pressedLevel &&
          stableState == releasedLevel) {
        incrementCount();

        Serial.printf(
          "COUNT: %lu\n",
          (unsigned long)getCount()
        );
      }
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ===== TASK 2: KONTROL OUTPUT =====
void taskControl(void* parameter) {
  for (;;) {
    uint32_t c = getCount();
    uint32_t p = getPreset();

    bool outputOn = (p > 0 && c >= p);
    digitalWrite(PIN_OUT, outputOn ? HIGH : LOW);

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ===== KONEKSI WIFI =====
void connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t start = millis();

  while (WiFi.status() != WL_CONNECTED &&
         (uint32_t)(millis() - start) < 10000) {
    vTaskDelay(pdMS_TO_TICKS(250));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi tersambung");
  } else {
    Serial.println("WiFi belum tersambung");
  }
}

// ===== KIRIM DATA + TERIMA COMMAND =====
void postData() {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;

  // Praktis untuk tes, tetapi tidak memverifikasi
  // sertifikat server. Gunakan setCACert() untuk produksi.
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, SERVER_URL)) return;

  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");

  JsonDocument doc;
  doc["key"] = API_KEY;
  doc["count"] = getCount();
  doc["preset"] = getPreset();
  doc["out"] = digitalRead(PIN_OUT) == HIGH;
  doc["rssi"] = WiFi.RSSI();

  JsonArray ack = doc["ack"].to<JsonArray>();
  for (int i = 0; i < ackN; i++) {
    ack.add(ackIds[i]);
  }

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);

  Serial.printf("HTTP %d | count=%lu\n",
                code, (unsigned long)getCount());

  if (code == 200) {
    String response = http.getString();

    JsonDocument res;
    DeserializationError err = deserializeJson(res, response);

    if (!err && res["ok"] == true) {
      ackN = 0;

      for (JsonObject cmd : res["commands"].as<JsonArray>()) {
        uint32_t id = cmd["id"] | 0;

        if (id > lastCmdId) {
          String name = cmd["cmd"] | "";

          if (name == "reset") {
            setCount(0);
          } else if (name == "preset") {
            setPreset(cmd["value"] | 0);
          }

          lastCmdId = id;
        }

        if (ackN < 8) {
          ackIds[ackN++] = id;
        }
      }
    }
  } else if (code <= 0) {
    Serial.println(http.errorToString(code));
  }

  http.end();
}

// ===== TASK 3: KOMUNIKASI SERVER =====
void taskNetwork(void* parameter) {
  for (;;) {
    connectWifi();

    if (WiFi.status() == WL_CONNECTED) {
      postData();
    }

    // Jeda setelah proses HTTP selesai.
    // Task counter tetap bisa berjalan.
    vTaskDelay(pdMS_TO_TICKS(POST_MS));
  }
}

// ===== TASK 4: SIMPAN DATA =====
void taskStorage(void* parameter) {
  uint32_t savedCount = getCount();
  uint32_t savedPreset = getPreset();

  for (;;) {
    uint32_t c = getCount();
    uint32_t p = getPreset();

    if (c != savedCount) {
      prefs.putUInt("count", c);
      savedCount = c;
    }

    if (p != savedPreset) {
      prefs.putUInt("preset", p);
      savedPreset = p;
    }

    vTaskDelay(pdMS_TO_TICKS(SAVE_MS));
  }
}

// ===== SETUP =====
void setup() {
  Serial.begin(115200);

  pinMode(PIN_PULSE, INPUT_PULLUP);
  pinMode(PIN_OUT, OUTPUT);
  digitalWrite(PIN_OUT, LOW);

  prefs.begin("counter", false);

  setCount(prefs.getUInt("count", 0));
  setPreset(prefs.getUInt("preset", 0));

  // WiFi dan HTTP dijalankan pada task tersendiri.
  // Task switch diberi prioritas lebih tinggi.
  xTaskCreate(
    taskCounter, "Counter", 2048, nullptr, 3, nullptr
  );

  xTaskCreate(
    taskControl, "Control", 2048, nullptr, 2, nullptr
  );

  xTaskCreate(
    taskNetwork, "Network", 8192, nullptr, 1, nullptr
  );

  xTaskCreate(
    taskStorage, "Storage", 3072, nullptr, 1, nullptr
  );
}

void loop() {
  // Semua pekerjaan ditangani oleh task FreeRTOS.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
