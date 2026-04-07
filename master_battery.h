#pragma once

#include <Arduino.h>

enum Rf73ChargeState : uint8_t {
  RF73_CHG_UNKNOWN = 0,
  RF73_CHG_CHARGING,
  RF73_CHG_DONE,
  RF73_CHG_DISCHARGING,
  RF73_CHG_EXTERNAL_ONLY
};

void rf73BatteryBegin();
void rf73BatteryUpdate();

bool rf73BatteryValid();
float rf73BatteryVoltage();
float rf73BatterySoc();
Rf73ChargeState rf73BatteryChargeState();
const char* rf73BatteryChargeText();

extern "C" float read_battery_voltage(void);