
#include "timekeeper.h"
#include <Preferences.h>
#include <time.h>

static time_t g_baseEpoch = 0;
static unsigned long g_baseMillis = 0;
static bool g_ntpSynced = false;
static unsigned long g_lastNtpAttemptMs = 0;
static unsigned long g_lastPersistMs = 0;

static void loadPersistedEpoch() {
  Preferences prefs;
  if (!prefs.begin("rf73time", true)) return;
  uint32_t epoch = prefs.getUInt("epoch", 0);
  prefs.end();
  if (epoch > 1000000000UL) {
    g_baseEpoch = (time_t)epoch;
    g_baseMillis = millis();
  }
}

static void persistEpoch(time_t nowEpoch) {
  Preferences prefs;
  if (!prefs.begin("rf73time", false)) return;
  prefs.putUInt("epoch", (uint32_t)nowEpoch);
  prefs.end();
}

void rf73TimeBegin() {
  loadPersistedEpoch();
  setenv("TZ", RF73_TIMEZONE_TZ, 1);
  tzset();
}

void rf73TimeTick() {
  unsigned long nowMs = millis();

  if (WiFi.status() == WL_CONNECTED && (nowMs - g_lastNtpAttemptMs > 30000UL)) {
    g_lastNtpAttemptMs = nowMs;
    configTzTime(RF73_TIMEZONE_TZ, RF73_NTP_SERVER_1, RF73_NTP_SERVER_2);
  }

  struct tm info;
  if (getLocalTime(&info, 10)) {
    time_t nowEpoch = mktime(&info);
    g_baseEpoch = nowEpoch;
    g_baseMillis = nowMs;
    g_ntpSynced = true;
    if ((nowMs - g_lastPersistMs) > RF73_TIME_PERSIST_MS) {
      g_lastPersistMs = nowMs;
      persistEpoch(nowEpoch);
    }
  }
}

bool rf73TimeNow(struct tm& outTm, bool& fromNtp) {
  if (g_baseEpoch <= 0) return false;
  time_t nowEpoch = g_baseEpoch + (time_t)((millis() - g_baseMillis) / 1000UL);
  localtime_r(&nowEpoch, &outTm);
  fromNtp = g_ntpSynced;
  return true;
}

String rf73TimeText() {
  struct tm tmv; bool fromNtp = false;
  if (!rf73TimeNow(tmv, fromNtp)) return "--:--:--";
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M:%S", &tmv);
  return String(buf);
}

String rf73DateText() {
  struct tm tmv; bool fromNtp = false;
  if (!rf73TimeNow(tmv, fromNtp)) return "--/--/----";
  char buf[24];
  strftime(buf, sizeof(buf), "%d/%m/%Y", &tmv);
  return String(buf);
}

String rf73DateTimeText() {
  return rf73DateText() + " " + rf73TimeText() + (g_ntpSynced ? "" : " *");
}

bool rf73TimeIsSynced() {
  return g_ntpSynced;
}
