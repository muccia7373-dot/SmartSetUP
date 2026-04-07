#include "master_battery.h"
#include "config.h"
#include "io_extension.h"

static float gBattV = 0.0f;
static float gBattSoc = 0.0f;
static bool  gBattValid = false;
static Rf73ChargeState gChg = RF73_CHG_UNKNOWN;
static unsigned long gLastBattMs = 0;

static float clampf_local(float x, float a, float b) {
  if (x < a) return a;
  if (x > b) return b;
  return x;
}

static float rawToVoltage(uint16_t raw) {
  const float vadc = (float(raw) / RF73_MASTER_BATT_ADC_MAX) * RF73_MASTER_BATT_ADC_VREF;
  return vadc * RF73_MASTER_BATT_SCALE;
}

static float voltageToSoc(float v) {
  float soc = (v - RF73_MASTER_BATT_V_EMPTY) / (RF73_MASTER_BATT_V_FULL - RF73_MASTER_BATT_V_EMPTY);
  soc = clampf_local(soc, 0.0f, 1.0f);
  return soc * 100.0f;
}

static Rf73ChargeState detectChargeState() {
#if RF73_MASTER_CHARGE_STATUS_ENABLE

  #if RF73_MASTER_CHARGE_STATUS_SOURCE == 1
    int chargingLevel = digitalRead(RF73_MASTER_CHG_PIN_CHARGING);
    int doneLevel     = digitalRead(RF73_MASTER_CHG_PIN_DONE);

    bool chargingActive = RF73_MASTER_CHG_ACTIVE_LOW ? (chargingLevel == LOW) : (chargingLevel == HIGH);
    bool doneActive     = RF73_MASTER_CHG_ACTIVE_LOW ? (doneLevel == LOW)     : (doneLevel == HIGH);

  #elif RF73_MASTER_CHARGE_STATUS_SOURCE == 2
    uint8_t chargingLevel = IO_EXTENSION_Input(RF73_MASTER_CHG_PIN_CHARGING);
    uint8_t doneLevel     = IO_EXTENSION_Input(RF73_MASTER_CHG_PIN_DONE);

    bool chargingActive = RF73_MASTER_CHG_ACTIVE_LOW ? (chargingLevel == 0) : (chargingLevel != 0);
    bool doneActive     = RF73_MASTER_CHG_ACTIVE_LOW ? (doneLevel == 0)     : (doneLevel != 0);

  #else
    bool chargingActive = false;
    bool doneActive = false;
  #endif

  if (chargingActive && !doneActive) return RF73_CHG_CHARGING;
  if (!chargingActive && doneActive) return RF73_CHG_DONE;
  if (!chargingActive && !doneActive) return RF73_CHG_DISCHARGING;
  return RF73_CHG_UNKNOWN;

#else
  return RF73_CHG_UNKNOWN;
#endif
}

void rf73BatteryBegin() {
  gBattV = 0.0f;
  gBattSoc = 0.0f;
  gBattValid = false;
  gChg = RF73_CHG_UNKNOWN;
  gLastBattMs = 0;

#if RF73_MASTER_CHARGE_STATUS_ENABLE
  #if RF73_MASTER_CHARGE_STATUS_SOURCE == 1
    pinMode(RF73_MASTER_CHG_PIN_CHARGING, INPUT);
    pinMode(RF73_MASTER_CHG_PIN_DONE, INPUT);
  #endif
#endif
}

void rf73BatteryUpdate() {
  if (!RF73_MASTER_BATTERY_ENABLE) return;
  if (millis() - gLastBattMs < RF73_MASTER_BATT_UPDATE_MS) return;
  gLastBattMs = millis();

  uint32_t acc = 0;
  for (int i = 0; i < RF73_MASTER_BATT_SAMPLES; ++i) {
    acc += IO_EXTENSION_Adc_Input();
    delay(2);
  }

  uint16_t raw = acc / RF73_MASTER_BATT_SAMPLES;
  float v = rawToVoltage(raw);

  static unsigned long lastDbg = 0;
  if (millis() - lastDbg > 1000) {
    lastDbg = millis();
    Serial.printf("[BATT] raw=%u  conv=%.3fV\n", raw, v);
  }

  if (raw > 0) {
    if (!gBattValid) {
      gBattV = v;
    } else {
      gBattV = gBattV + RF73_MASTER_BATT_ALPHA * (v - gBattV);
    }

    gBattSoc = voltageToSoc(gBattV);
    gBattValid = true;
  } else {
    gBattValid = false;
  }

  gChg = detectChargeState();
}

bool rf73BatteryValid() {
  return gBattValid;
}

float rf73BatteryVoltage() {
  return gBattV;
}

float rf73BatterySoc() {
  return gBattSoc;
}

Rf73ChargeState rf73BatteryChargeState() {
  return gChg;
}

const char* rf73BatteryChargeText() {
  switch (gChg) {
    case RF73_CHG_CHARGING:      return "CHG";
    case RF73_CHG_DONE:          return "FULL";
    case RF73_CHG_DISCHARGING:   return "BAT";
    case RF73_CHG_EXTERNAL_ONLY: return "EXT";
    default:                     return "CHG ?";
  }
}

extern "C" float read_battery_voltage(void) {
  return rf73BatteryVoltage();
}