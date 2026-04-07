#pragma once

#include <Arduino.h>

// ================= LEGACY =================
typedef struct {
  char id[4];
  float camber;
  float z;
  bool stable;
} CamberTelemetryPacket;

typedef CamberTelemetryPacket TelemetryPacket;

typedef struct {
  char cmd[10];
} CommandPacket;

// ================= RF V1 =================
#pragma pack(push, 1)

enum PacketTypeV1 : uint8_t {
  PKT_SET_CONFIG     = 1,
  PKT_GET_CONFIG     = 2,
  PKT_CONFIG_ACK     = 3,

  PKT_BATTERY_STATUS = 10,
  PKT_TOE_STATUS     = 11,
  PKT_FW_INFO        = 12,

  PKT_OTA_START      = 20,
  PKT_OTA_STATUS     = 21,
  PKT_OTA_RESULT     = 22
};

typedef struct {
  uint8_t magic[2];
  uint8_t version;
  uint8_t type;
  char id[4];
  float voltage;
  float soc;
} BatteryTelemetryPacketV1;

typedef struct {
  uint8_t magic[2];
  uint8_t version;
  uint8_t type;
  char id[4];
  float toe;
} ToeTelemetryPacketV1;

typedef struct {
  uint8_t magic[2];
  uint8_t version;
  uint8_t type;
  char id[4];
  char fwVersion[16];
} FirmwareInfoPacketV1;

typedef struct {
  uint8_t magic[2];
  uint8_t version;
  uint8_t type;
  char targetId[4];
  char newNodeId[4];
  float alpha;
  uint16_t sampleCount;
  float stabilityThreshold;
  uint16_t stabilityTimeMs;
  uint8_t invertSign;
  uint8_t autoBeepStable;
} ConfigPacketV1;

typedef struct {
  uint8_t magic[2];
  uint8_t version;
  uint8_t type;
  char nodeId[4];
  uint8_t ok;
  float alpha;
  uint16_t sampleCount;
  float stabilityThreshold;
  uint16_t stabilityTimeMs;
  uint8_t invertSign;
  uint8_t autoBeepStable;
  char message[24];
} ConfigAckPacketV1;

typedef struct {
  uint8_t magic[2];
  uint8_t version;
  uint8_t type;
  char targetId[4];
  char ssid[32];
  char password[32];
  char url[96];
  uint32_t fwSize;
  uint32_t fwCrc;
  char fwVersion[12];
} OtaStartPacket;

typedef struct {
  uint8_t magic[2];
  uint8_t version;
  uint8_t type;
  char nodeId[4];
  uint8_t stage;
  uint8_t progress;
} OtaStatusPacket;

typedef struct {
  uint8_t magic[2];
  uint8_t version;
  uint8_t type;
  char nodeId[4];
  uint8_t success;
  char message[24];
} OtaResultPacket;

#pragma pack(pop)