#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_now.h>
#include <esp_wifi.h>

// -----------------------------------------------------------------------------
// ESP32-S3 PHONE LOCALIZER - MASTER / ANCHOR 0
// Arduino-ESP32 3.x, Board: "ESP32S3 Dev Module"
// -----------------------------------------------------------------------------

static const char *AP_SSID = "ESP32_LOCALIZER";
static const char *AP_PASSWORD = "LocateMe2026";  // Change before real use.
static const uint8_t WIFI_CHANNEL = 6;
static const uint8_t MAX_PHONE_CONNECTIONS = 8;

// Locally administered MAC addresses. These must match Node 1 and Node 2.
static const uint8_t MASTER_AP_MAC[6] = {0x02, 0x4C, 0x4F, 0x43, 0x00, 0x01};
static const uint8_t NODE1_STA_MAC[6] = {0x02, 0x4C, 0x4F, 0x43, 0x01, 0x01};
static const uint8_t NODE2_STA_MAC[6] = {0x02, 0x4C, 0x4F, 0x43, 0x02, 0x01};

// Demo keys. Replace both keys in all three sketches with your own 16-byte keys.
static const uint8_t ESPNOW_PMK[16] = {
    'L', 'O', 'C', '_', 'P', 'M', 'K', '_', '2', '0', '2', '6', '_', 'K', 'E', 'Y'};
static const uint8_t ESPNOW_LMK[16] = {
    'L', 'O', 'C', '_', 'L', 'M', 'K', '_', '2', '0', '2', '6', '_', 'K', 'E', 'Y'};

static const uint16_t REPORT_MAGIC = 0x4C52;
static const uint8_t REPORT_VERSION = 1;
static const uint8_t ANCHOR_COUNT = 3;
static const uint8_t MAX_TRACKED_CLIENTS = 15;
static const uint8_t MAX_SAMPLES_PER_WINDOW = 32;
static const uint32_t REPORT_INTERVAL_MS = 500;
static const uint32_t MEASUREMENT_MAX_AGE_MS = 2000;
static const uint32_t CLIENT_REMOVE_MS = 15000;

struct __attribute__((packed)) RssiReport {
  uint16_t magic;
  uint8_t version;
  uint8_t anchorId;
  uint8_t clientMac[6];
  int8_t medianRssi;
  int8_t minRssi;
  int8_t maxRssi;
  uint8_t sampleCount;
  uint32_t windowEndMs;
};

struct SampleBucket {
  bool used;
  uint8_t mac[6];
  int8_t samples[MAX_SAMPLES_PER_WINDOW];
  uint8_t count;
  uint8_t replaceIndex;
  uint32_t lastSeenMs;
};

struct SampleSnapshot {
  bool valid;
  uint8_t mac[6];
  int8_t samples[MAX_SAMPLES_PER_WINDOW];
  uint8_t count;
};

struct ClientMeasurement {
  bool used;
  uint8_t mac[6];
  int8_t rssi[ANCHOR_COUNT];
  uint8_t sampleCount[ANCHOR_COUNT];
  uint32_t updatedMs[ANCHOR_COUNT];
  uint32_t lastAnyMs;
};

SampleBucket localBuckets[MAX_TRACKED_CLIENTS] = {};
ClientMeasurement measurements[MAX_TRACKED_CLIENTS] = {};
portMUX_TYPE sampleMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE measurementMux = portMUX_INITIALIZER_UNLOCKED;

DNSServer dnsServer;
WebServer webServer(80);
uint32_t lastReportMs = 0;

const char PORTAL_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>ESP32 Phone Localizer</title>
  <style>
    body{margin:0;background:#0f172a;color:#e2e8f0;font-family:Arial,sans-serif;display:grid;place-items:center;min-height:100vh}
    main{width:min(88%,480px);background:#1e293b;border:1px solid #334155;border-radius:18px;padding:28px;text-align:center;box-shadow:0 20px 45px #0006}
    h1{font-size:1.55rem;margin:0 0 12px;color:#60a5fa} p{line-height:1.55;color:#cbd5e1}
    .dot{display:inline-block;width:12px;height:12px;border-radius:50%;background:#22c55e;box-shadow:0 0 14px #22c55e;margin-right:8px}
    small{display:block;margin-top:20px;color:#94a3b8}
  </style>
</head>
<body>
<main>
  <h1>ESP32 Phone Localizer</h1>
  <p><span class="dot"></span><strong id="state">Tracking traffic is active</strong></p>
  <p>Keep this page open and the screen awake while the phone is being located.</p>
  <small>This network has no Internet connection.</small>
</main>
<script>
  let ok=0,fail=0;
  async function ping(){
    try{await fetch('/ping?t='+Date.now(),{cache:'no-store'});ok++;document.getElementById('state').textContent='Tracking active • '+ok+' packets';}
    catch(e){fail++;document.getElementById('state').textContent='Reconnecting…';}
  }
  setInterval(ping,250); ping();
</script>
</body>
</html>
)HTML";

bool macEquals(const uint8_t *a, const uint8_t *b) {
  for (uint8_t i = 0; i < 6; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

void copyMac(uint8_t *destination, const uint8_t *source) {
  for (uint8_t i = 0; i < 6; i++) destination[i] = source[i];
}

void formatMac(const uint8_t *mac, char *output) {
  snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int8_t medianOf(int8_t *values, uint8_t count) {
  for (uint8_t i = 1; i < count; i++) {
    int8_t key = values[i];
    int8_t j = i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      j--;
    }
    values[j + 1] = key;
  }
  if (count % 2 == 1) return values[count / 2];
  return (int8_t)(((int16_t)values[count / 2 - 1] + values[count / 2]) / 2);
}

void addLocalSample(const uint8_t *clientMac, int8_t rssi) {
  if ((clientMac[0] & 0x01) != 0) return;  // Ignore multicast/broadcast sources.

  uint32_t now = millis();
  portENTER_CRITICAL(&sampleMux);

  int selected = -1;
  int empty = -1;
  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    if (localBuckets[i].used && macEquals(localBuckets[i].mac, clientMac)) {
      selected = i;
      break;
    }
    if (!localBuckets[i].used && empty < 0) empty = i;
  }

  if (selected < 0) selected = empty;
  if (selected >= 0) {
    SampleBucket &bucket = localBuckets[selected];
    if (!bucket.used) {
      bucket.used = true;
      copyMac(bucket.mac, clientMac);
      bucket.count = 0;
      bucket.replaceIndex = 0;
    }

    if (bucket.count < MAX_SAMPLES_PER_WINDOW) {
      bucket.samples[bucket.count++] = rssi;
    } else {
      bucket.samples[bucket.replaceIndex] = rssi;
      bucket.replaceIndex = (bucket.replaceIndex + 1) % MAX_SAMPLES_PER_WINDOW;
    }
    bucket.lastSeenMs = now;
  }

  portEXIT_CRITICAL(&sampleMux);
}

void promiscuousReceive(void *buffer, wifi_promiscuous_pkt_type_t packetType) {
  if (packetType != WIFI_PKT_DATA) return;

  const wifi_promiscuous_pkt_t *packet =
      reinterpret_cast<const wifi_promiscuous_pkt_t *>(buffer);
  if (packet->rx_ctrl.sig_len < 24) return;

  const uint8_t *frame = packet->payload;
  uint16_t frameControl = frame[0] | (static_cast<uint16_t>(frame[1]) << 8);
  bool toDistributionSystem = (frameControl & (1U << 8)) != 0;
  bool fromDistributionSystem = (frameControl & (1U << 9)) != 0;

  // A phone sending to this AP has ToDS=1, FromDS=0.
  if (!toDistributionSystem || fromDistributionSystem) return;

  const uint8_t *receiverAddress = frame + 4;     // Address 1 / AP BSSID
  const uint8_t *transmitterAddress = frame + 10; // Address 2 / phone MAC
  if (!macEquals(receiverAddress, MASTER_AP_MAC)) return;

  addLocalSample(transmitterAddress, packet->rx_ctrl.rssi);
}

void updateMeasurement(uint8_t anchorId, const uint8_t *clientMac,
                       int8_t rssi, uint8_t sampleCount) {
  if (anchorId >= ANCHOR_COUNT) return;
  uint32_t now = millis();

  portENTER_CRITICAL(&measurementMux);
  int selected = -1;
  int empty = -1;
  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    if (measurements[i].used && macEquals(measurements[i].mac, clientMac)) {
      selected = i;
      break;
    }
    if (!measurements[i].used && empty < 0) empty = i;
  }

  if (selected < 0) selected = empty;
  if (selected >= 0) {
    ClientMeasurement &entry = measurements[selected];
    if (!entry.used) {
      entry.used = true;
      copyMac(entry.mac, clientMac);
      for (uint8_t a = 0; a < ANCHOR_COUNT; a++) {
        entry.updatedMs[a] = 0;
        entry.sampleCount[a] = 0;
        entry.rssi[a] = -127;
      }
    }
    entry.rssi[anchorId] = rssi;
    entry.sampleCount[anchorId] = sampleCount;
    entry.updatedMs[anchorId] = now;
    entry.lastAnyMs = now;
  }
  portEXIT_CRITICAL(&measurementMux);
}

void flushMasterSamples() {
  SampleSnapshot snapshots[MAX_TRACKED_CLIENTS] = {};
  uint32_t now = millis();

  portENTER_CRITICAL(&sampleMux);
  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    SampleBucket &bucket = localBuckets[i];
    if (!bucket.used) continue;

    if (bucket.count > 0) {
      snapshots[i].valid = true;
      copyMac(snapshots[i].mac, bucket.mac);
      snapshots[i].count = bucket.count;
      for (uint8_t s = 0; s < bucket.count; s++) {
        snapshots[i].samples[s] = bucket.samples[s];
      }
      bucket.count = 0;
      bucket.replaceIndex = 0;
    } else if (now - bucket.lastSeenMs > CLIENT_REMOVE_MS) {
      bucket.used = false;
    }
  }
  portEXIT_CRITICAL(&sampleMux);

  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    if (!snapshots[i].valid) continue;
    int8_t median = medianOf(snapshots[i].samples, snapshots[i].count);
    updateMeasurement(0, snapshots[i].mac, median, snapshots[i].count);
  }
}

void outputMeasurementsToPc() {
  ClientMeasurement snapshot[MAX_TRACKED_CLIENTS] = {};
  uint32_t now = millis();

  portENTER_CRITICAL(&measurementMux);
  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    if (measurements[i].used && now - measurements[i].lastAnyMs > CLIENT_REMOVE_MS) {
      measurements[i].used = false;
    }
    snapshot[i] = measurements[i];
  }
  portEXIT_CRITICAL(&measurementMux);

  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    if (!snapshot[i].used) continue;

    uint32_t age[ANCHOR_COUNT];
    bool complete = true;
    for (uint8_t a = 0; a < ANCHOR_COUNT; a++) {
      age[a] = snapshot[i].updatedMs[a] == 0
                   ? 0xFFFFFFFFUL
                   : now - snapshot[i].updatedMs[a];
      if (age[a] > MEASUREMENT_MAX_AGE_MS) complete = false;
    }
    if (!complete) continue;

    char macText[18];
    formatMac(snapshot[i].mac, macText);
    Serial.printf(
        "{\"type\":\"measurement\",\"mac\":\"%s\","
        "\"rssi\":[%d,%d,%d],\"samples\":[%u,%u,%u],"
        "\"age_ms\":[%lu,%lu,%lu]}\n",
        macText,
        snapshot[i].rssi[0], snapshot[i].rssi[1], snapshot[i].rssi[2],
        snapshot[i].sampleCount[0], snapshot[i].sampleCount[1],
        snapshot[i].sampleCount[2],
        static_cast<unsigned long>(age[0]),
        static_cast<unsigned long>(age[1]),
        static_cast<unsigned long>(age[2]));
  }
}

void onEspNowReceive(const esp_now_recv_info_t *receiveInfo,
                     const uint8_t *data, int length) {
  if (length != sizeof(RssiReport)) return;

  RssiReport report;
  memcpy(&report, data, sizeof(report));
  if (report.magic != REPORT_MAGIC || report.version != REPORT_VERSION) return;
  if (report.anchorId != 1 && report.anchorId != 2) return;

  const uint8_t *expectedMac = report.anchorId == 1 ? NODE1_STA_MAC : NODE2_STA_MAC;
  if (!macEquals(receiveInfo->src_addr, expectedMac)) return;

  updateMeasurement(report.anchorId, report.clientMac,
                    report.medianRssi, report.sampleCount);
}

bool addEncryptedPeer(const uint8_t *peerMac) {
  esp_now_peer_info_t peer = {};
  copyMac(peer.peer_addr, peerMac);
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_AP;
  peer.encrypt = true;
  memcpy(peer.lmk, ESPNOW_LMK, ESP_NOW_KEY_LEN);
  esp_err_t result = esp_now_add_peer(&peer);
  return result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST;
}

void servePortal() {
  webServer.send_P(200, "text/html", PORTAL_HTML);
}

void startPortal() {
  dnsServer.start(53, "*", WiFi.softAPIP());
  webServer.on("/", HTTP_GET, servePortal);
  webServer.on("/ping", HTTP_GET, []() {
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "text/plain", "ok");
  });
  webServer.on("/generate_204", HTTP_GET, servePortal);
  webServer.on("/hotspot-detect.html", HTTP_GET, servePortal);
  webServer.on("/ncsi.txt", HTTP_GET, servePortal);
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "http://192.168.4.1/", true);
    webServer.send(302, "text/plain", "");
  });
  webServer.begin();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32-S3 Localizer Master starting...");

  WiFi.mode(WIFI_AP);
  delay(100);
  if (esp_wifi_set_mac(WIFI_IF_AP, MASTER_AP_MAC) != ESP_OK) {
    Serial.println("ERROR: Could not set the fixed master AP MAC.");
  }

  if (!WiFi.softAP(AP_SSID, AP_PASSWORD, WIFI_CHANNEL, false,
                   MAX_PHONE_CONNECTIONS)) {
    Serial.println("ERROR: SoftAP failed to start.");
    while (true) delay(1000);
  }

  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW initialization failed.");
    while (true) delay(1000);
  }
  esp_now_set_pmk(ESPNOW_PMK);
  esp_now_register_recv_cb(onEspNowReceive);

  if (!addEncryptedPeer(NODE1_STA_MAC) || !addEncryptedPeer(NODE2_STA_MAC)) {
    Serial.println("ERROR: Could not add encrypted ESP-NOW peers.");
    while (true) delay(1000);
  }

  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(promiscuousReceive);
  esp_wifi_set_promiscuous(true);

  startPortal();

  char masterMacText[18];
  formatMac(MASTER_AP_MAC, masterMacText);
  Serial.printf("AP: %s\n", AP_SSID);
  Serial.printf("Password: %s\n", AP_PASSWORD);
  Serial.printf("Channel: %u\n", WIFI_CHANNEL);
  Serial.printf("Master AP MAC: %s\n", masterMacText);
  Serial.printf("Phone page: http://%s/\n", WiFi.softAPIP().toString().c_str());
  Serial.println("Waiting for phones and anchor reports...");
}

void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();

  uint32_t now = millis();
  if (now - lastReportMs >= REPORT_INTERVAL_MS) {
    lastReportMs = now;
    flushMasterSamples();
    outputMeasurementsToPc();
  }

  delay(2);
}
