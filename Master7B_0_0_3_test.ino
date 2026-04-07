#include "config.h"
#include "state.h"
#include "espnow_link.h"
#include "web_ui.h"
#include "storage_backend.h"
#include "timekeeper.h"
#include "local_ui.h"
#include "master_battery.h"
#include "esp_heap_caps.h"

static bool gLocalUiBootOk = false;
static bool gLocalUiRuntimeOk = false;

void setup() {
  Serial.begin(115200);
  delay(1200);

  Serial.println();
  Serial.println("==============================================");
  Serial.println(" RaceFab73 - Master ESP-NOW V4 - BUILD B");
  Serial.println(" Split tabs · Live Measure + Sensors Config - BUILD B");
  Serial.println(" OTA source: onboard TF / storage backend");
  Serial.println("==============================================");

  loadSystemOptions();
  rf73TimeBegin();

  // IMPORTANTE:
  // porta su prima LCD/touch per riservare i grossi blocchi PSRAM
  // ai framebuffer RGB, prima che Wi-Fi / ESP-NOW / WebServer
  // allocchino i propri buffer.
  Serial.printf("PSRAM total: %u | free: %u | largest block: %u\n",
                ESP.getPsramSize(),
                ESP.getFreePsram(),
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

  // 1) Bring-up locale Waveshare 7B
  gLocalUiBootOk = rf73LocalUiBegin();

  // 2) Ora che la board è su, inizializza la lettura batteria master
  //    SENZA richiamare init I2C/IO extension nel setup.
  if (gLocalUiBootOk) {
    rf73BatteryBegin();
  }

  // 3) Monta storage/SD
  Serial.println("Init storage...");
  bool storageOk = rf73Storage.begin();
  if (!storageOk) {
    Serial.println("Storage init failed");
    setUiLastEvent("SD non pronta");
  } else {
    Serial.printf("Storage ready via %s\n", rf73Storage.backendName());
    setUiLastEvent("SD pronta");

    File fw = rf73Storage.open(OTA_FW_PATH, FILE_READ);
    if (fw) {
      Serial.printf("Firmware file found: %s (%u bytes)\n", OTA_FW_PATH, (unsigned)fw.size());
      fw.close();
    } else {
      Serial.printf("Firmware file not found: %s\n", OTA_FW_PATH);
    }
  }

  // 4) Avvia rete solo DOPO il bring-up LCD
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);

  bool apOk = WiFi.softAP(AP_SSID, AP_PASS, WIFI_CHANNEL, 0, 4);
  if (!apOk) {
    Serial.println("SoftAP start failed");
  }
  delay(100);

  printMasterMac();
  setupEspNow();
  setupWebServer();

  // 5) Solo ora avvia il runtime LVGL
  gLocalUiRuntimeOk = gLocalUiBootOk && rf73LocalUiStartRuntime();
  Serial.printf("Local HMI: %s\n", gLocalUiRuntimeOk ? "READY" : "DISABLED/NOT READY");

  Serial.println("MASTER WEB UI V4 READY");
  Serial.println("Open browser on: http://192.168.4.1");
  Serial.printf("OTA URL: %s\n", OTA_FW_URL);
  Serial.printf("FW META: %s\n", RF73_FW_META);
}

void loop() {
  server.handleClient();
  updateOnlineTimeouts();
  updateBatchOta();
  rf73TimeTick();

  if (gLocalUiRuntimeOk) {
    rf73BatteryUpdate();
  }

  rf73LocalUiTick();

  unsigned long now = millis();
  if (now - lastTablePrintMs >= tablePrintPeriodMs) {
    lastTablePrintMs = now;
    printNodeTable();
  }

  yield();
}