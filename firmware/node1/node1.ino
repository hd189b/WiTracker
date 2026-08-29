#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// -----------------------------------------------------------------------------
// ESP32-S3 PHONE LOCALIZER - PASSIVE ANCHOR NODE 1
// Arduino-ESP32 3.x, Board: "ESP32S3 Dev Module"
// -----------------------------------------------------------------------------

static const uint8_t ANCHOR_ID = 1;
static const uint8_t WIFI_CHANNEL = 6;
static const uint8_t MASTER_AP_MAC[6] = {0x02, 0x4C, 0x4F, 0x43, 0x00, 0x01};
static const uint8_t THIS_NODE_STA_MAC[6] = {0x02, 0x4C, 0x4F, 0x43, 0x01, 0x01};

// These keys must exactly match the master and Node 2.
static const uint8_t ESPNOW_PMK[16] = {
    'L', 'O', 'C', '_', 'P', 'M', 'K', '_', '2', '0', '2', '6', '_', 'K', 'E', 'Y'};
static const uint8_t ESPNOW_LMK[16] = {
    'L', 'O', 'C', '_', 'L', 'M', 'K', '_', '2', '0', '2', '6', '_', 'K', 'E', 'Y'};

static const uint16_t REPORT_MAGIC = 0x4C52;
static const uint8_t REPORT_VERSION = 1;
static const uint8_t MAX_TRACKED_CLIENTS = 15;
static const uint8_t MAX_SAMPLES_PER_WINDOW = 32;
static const uint32_t REPORT_INTERVAL_MS = 500;
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

SampleBucket buckets[MAX_TRACKED_CLIENTS] = {};
portMUX_TYPE sampleMux = portMUX_INITIALIZER_UNLOCKED;
uint32_t lastReportMs = 0;
uint32_t sentReports = 0;
uint32_t failedReports = 0;

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

void addSample(const uint8_t *clientMac, int8_t rssi) {
  if ((clientMac[0] & 0x01) != 0) return;
  uint32_t now = millis();

  portENTER_CRITICAL(&sampleMux);
  int selected = -1;
  int empty = -1;
  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    if (buckets[i].used && macEquals(buckets[i].mac, clientMac)) {
      selected = i;
      break;
    }
    if (!buckets[i].used && empty < 0) empty = i;
  }

  if (selected < 0) selected = empty;
  if (selected >= 0) {
    SampleBucket &bucket = buckets[selected];
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
  if (!toDistributionSystem || fromDistributionSystem) return;

  const uint8_t *receiverAddress = frame + 4;
  const uint8_t *transmitterAddress = frame + 10;
  if (!macEquals(receiverAddress, MASTER_AP_MAC)) return;

  addSample(transmitterAddress, packet->rx_ctrl.rssi);
}

void flushReports() {
  SampleSnapshot snapshots[MAX_TRACKED_CLIENTS] = {};
  uint32_t now = millis();

  portENTER_CRITICAL(&sampleMux);
  for (uint8_t i = 0; i < MAX_TRACKED_CLIENTS; i++) {
    SampleBucket &bucket = buckets[i];
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

    int8_t minimum = 127;
    int8_t maximum = -127;
    for (uint8_t s = 0; s < snapshots[i].count; s++) {
      minimum = min(minimum, snapshots[i].samples[s]);
      maximum = max(maximum, snapshots[i].samples[s]);
    }

    RssiReport report = {};
    report.magic = REPORT_MAGIC;
    report.version = REPORT_VERSION;
    report.anchorId = ANCHOR_ID;
    copyMac(report.clientMac, snapshots[i].mac);
    report.medianRssi = medianOf(snapshots[i].samples, snapshots[i].count);
    report.minRssi = minimum;
    report.maxRssi = maximum;
    report.sampleCount = snapshots[i].count;
    report.windowEndMs = now;

    esp_err_t result = esp_now_send(MASTER_AP_MAC,
                                    reinterpret_cast<const uint8_t *>(&report),
                                    sizeof(report));
    if (result == ESP_OK) sentReports++;
    else failedReports++;
  }
}

bool addMasterPeer() {
  esp_now_peer_info_t peer = {};
  copyMac(peer.peer_addr, MASTER_AP_MAC);
  peer.channel = WIFI_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = true;
  memcpy(peer.lmk, ESPNOW_LMK, ESP_NOW_KEY_LEN);
  esp_err_t result = esp_now_add_peer(&peer);
  return result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nESP32-S3 Localizer Anchor Node 1 starting...");

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);
  delay(100);

  if (esp_wifi_set_mac(WIFI_IF_STA, THIS_NODE_STA_MAC) != ESP_OK) {
    Serial.println("ERROR: Could not set Node 1 MAC.");
  }
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW initialization failed.");
    while (true) delay(1000);
  }
  esp_now_set_pmk(ESPNOW_PMK);
  if (!addMasterPeer()) {
    Serial.println("ERROR: Could not add master as encrypted peer.");
    while (true) delay(1000);
  }

  wifi_promiscuous_filter_t filter = {};
  filter.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous_rx_cb(promiscuousReceive);
  esp_wifi_set_promiscuous(true);

  char nodeMacText[18];
  formatMac(THIS_NODE_STA_MAC, nodeMacText);
  Serial.printf("Node ID: %u\n", ANCHOR_ID);
  Serial.printf("Node MAC: %s\n", nodeMacText);
  Serial.printf("Fixed channel: %u\n", WIFI_CHANNEL);
  Serial.println("Passive packet capture is active.");
}

void loop() {
  uint32_t now = millis();
  if (now - lastReportMs >= REPORT_INTERVAL_MS) {
    lastReportMs = now;
    flushReports();
  }

  static uint32_t lastStatusMs = 0;
  if (now - lastStatusMs >= 10000) {
    lastStatusMs = now;
    Serial.printf("Reports queued: %lu, immediate send errors: %lu\n",
                  static_cast<unsigned long>(sentReports),
                  static_cast<unsigned long>(failedReports));
  }
  delay(5);
}
