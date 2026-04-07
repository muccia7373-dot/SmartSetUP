#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <SD.h>

#define MASTER_FW_TYPE_STR "MASTER"
#define MASTER_FW_CHIP_STR "ESP32S3"
#define MASTER_FW_VERSION_STR "V1_2_1"
#define MASTER_FW_CHANNEL_STR "test"
#define MASTER_FW_PROTO_STR "1"

constexpr const char* MASTER_FW_VERSION = MASTER_FW_VERSION_STR;
constexpr const char* MASTER_FW_CHANNEL = MASTER_FW_CHANNEL_STR;
constexpr const char* MASTER_FW_DISPLAY_VERSION = MASTER_FW_VERSION_STR "_" MASTER_FW_CHANNEL_STR;


constexpr const char* DEVICE_NAME = "SmartSetUP Master";

// =====================================================
// LOCAL HMI / WAVESHARE 7B
// =====================================================
constexpr bool RF73_ENABLE_LOCAL_HMI = true;
constexpr bool RF73_USE_WAVESHARE_7B = true;
constexpr int RF73_LOCAL_LCD_WIDTH = 1024;
constexpr int RF73_LOCAL_LCD_HEIGHT = 600;
constexpr int RF73_LOCAL_HEADER_HEIGHT = 56;
constexpr int RF73_LOCAL_FOOTER_HEIGHT = 40;
constexpr unsigned long RF73_LOCAL_UI_REFRESH_MS = 250;
constexpr unsigned long RF73_TIME_PERSIST_MS = 60000;
constexpr const char* RF73_TIMEZONE_TZ = "CET-1CEST,M3.5.0/2,M10.5.0/3";
constexpr const char* RF73_NTP_SERVER_1 = "pool.ntp.org";
constexpr const char* RF73_NTP_SERVER_2 = "time.nist.gov";
constexpr const char* RF73_ASSET_DIR = "/assets";
constexpr const char* RF73_SPLASH_BMP = "/assets/splash.bmp";
constexpr const char* RF73_LOGO_BMP = "/assets/logo.bmp";
constexpr const char* RF73_FW_DIR = "/";

// =====================================================
// MASTER BATTERY
// =====================================================
constexpr bool RF73_MASTER_BATTERY_ENABLE = true;

// ADC letto tramite IO extension Waveshare
constexpr float RF73_MASTER_BATT_ADC_VREF = 3.30f;
constexpr float RF73_MASTER_BATT_ADC_MAX  = 4095.0f;

// Da calibrare con multimetro.
// Parti così, poi correggiamo in base alla misura reale.
constexpr float RF73_MASTER_BATT_SCALE    = 12.17f;

// Filtro e timing
constexpr float RF73_MASTER_BATT_ALPHA    = 0.15f;
constexpr uint16_t RF73_MASTER_BATT_SAMPLES = 8;
constexpr unsigned long RF73_MASTER_BATT_UPDATE_MS = 500;

// Soglie LiPo 1S
constexpr float RF73_MASTER_BATT_V_EMPTY  = 3.20f;
constexpr float RF73_MASTER_BATT_V_FULL   = 4.20f;

// -----------------------------------------------------
// CHARGE STATUS
// 0 = disabilitato
// 1 = GPIO MCU diretto
// 2 = IO expander Waveshare
// -----------------------------------------------------
constexpr bool RF73_MASTER_CHARGE_STATUS_ENABLE = false;
constexpr uint8_t RF73_MASTER_CHARGE_STATUS_SOURCE = 0;

// Se in futuro trovi i pin:
// - per source 1: GPIO ESP32
// - per source 2: pin IO extension 0..7
constexpr int RF73_MASTER_CHG_PIN_CHARGING = -1;
constexpr int RF73_MASTER_CHG_PIN_DONE     = -1;

// true se CHG/DONE sono attivi bassi
constexpr bool RF73_MASTER_CHG_ACTIVE_LOW = true;

__attribute__((used)) static const char RF73_FW_META[] =
  "RF73META|type=" MASTER_FW_TYPE_STR
  "|chip=" MASTER_FW_CHIP_STR
  "|version=" MASTER_FW_VERSION_STR
  "|channel=" MASTER_FW_CHANNEL_STR
  "|proto=" MASTER_FW_PROTO_STR;

// Naming convention for update files:
// RF73_<TARGET>_<CHIP>_<VERSION>_<CHANNEL>.bin
// Examples:
// RF73_MASTER_ESP32S3_V1_2_0_test.bin
// RF73_SLAVE_ESP32C3_V1_2_0_test.bin

// =====================================================
// WIFI AP
// =====================================================
constexpr const char* AP_SSID = "RF73_SETUP";
constexpr const char* AP_PASS = "12345678";
constexpr uint8_t WIFI_CHANNEL = 1;

// =====================================================
// STORAGE / OTA
// =====================================================
// Legacy SPI SD pins kept as fallback for non-7B boards.
// Waveshare 7B uses onboard TF via SD_MMC / board helper path.
constexpr int SD_CS_PIN   = 16;
constexpr int SD_MOSI_PIN = 35;
constexpr int SD_MISO_PIN = 37;
constexpr int SD_SCK_PIN  = 36;
constexpr uint32_t SD_SPI_HZ = 8000000;

// compatibilità con il setup attuale del master
constexpr const char* OTA_FW_PATH = "/slave.bin";
constexpr const char* OTA_FW_URL  = "http://192.168.4.1/slave.bin";

// =====================================================
// DEFAULT CONFIG CACHE
// =====================================================
constexpr float    DEFAULT_ALPHA               = 0.20f;
constexpr uint16_t DEFAULT_SAMPLE_COUNT        = 12;
constexpr float    DEFAULT_STABILITY_THRESHOLD = 0.15f;
constexpr uint16_t DEFAULT_STABILITY_TIME_MS   = 500;
constexpr bool     DEFAULT_INVERT_SIGN         = false;
constexpr bool     DEFAULT_AUTO_BEEP_STABLE    = true;

// =====================================================
// LIMITS / TIMERS
// =====================================================
constexpr int MAX_ASSIGNED_NODES = 5;
constexpr int MAX_UNKNOWN_NODES  = 10;

constexpr unsigned long offlineTimeoutMs   = 3000;
constexpr unsigned long ackTimeoutMs       = 1200;
constexpr unsigned long tablePrintPeriodMs = 3000;