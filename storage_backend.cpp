#include "storage_backend.h"
#include "io_extension.h"
#include <SD.h>
#include <SPI.h>
#include <SD_MMC.h>

Rf73StorageClass rf73Storage;

bool Rf73StorageClass::begin() {
  _ready = false;
  _usingMmc = false;

  if (RF73_USE_WAVESHARE_7B) {
    // TF onboard 7B: EXIO4 alto prima del mount
    IO_EXTENSION_Output(IO_EXTENSION_IO_4, 1);
    delay(10);

    // Pin SD_MMC dall'esempio Waveshare 07_SD
    if (!SD_MMC.setPins(GPIO_NUM_12, GPIO_NUM_11, GPIO_NUM_13)) {
      return false;
    }

    // 1-bit mode
    if (!SD_MMC.begin("/sdcard", true, false)) {
      return false;
    }

    uint8_t cardType = SD_MMC.cardType();
    if (cardType == CARD_NONE) {
      SD_MMC.end();
      return false;
    }

    _ready = true;
    _usingMmc = true;
    return true;
  }

  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (SD.begin(SD_CS_PIN, SPI, SD_SPI_HZ)) {
    _ready = true;
    _usingMmc = false;
    return true;
  }

  return false;
}

fs::File Rf73StorageClass::open(const String& path, const char* mode) {
  if (_usingMmc) return SD_MMC.open(path, mode);
  return SD.open(path, mode);
}

bool Rf73StorageClass::exists(const String& path) {
  if (_usingMmc) return SD_MMC.exists(path);
  return SD.exists(path);
}

bool Rf73StorageClass::remove(const String& path) {
  if (_usingMmc) return SD_MMC.remove(path);
  return SD.remove(path);
}

const char* Rf73StorageClass::backendName() const {
  return _usingMmc ? "SD_MMC" : "SPI_SD";
}