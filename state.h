#pragma once

#include "config.h"
#include "protocol.h"

struct NodeConfigCache {
  float alpha;
  uint16_t sampleCount;
  float stabilityThreshold;
  uint16_t stabilityTimeMs;
  bool invertSign;
  bool autoBeepStable;
  bool valid;
};

struct NodeTxState {
  bool pendingAck;
  bool lastSendOk;
  bool lastAckOk;
  unsigned long lastSendMs;
  unsigned long lastAckMs;
  char lastStatus[24];
};

struct NodeOtaState {
  bool active;
  bool lastOk;
  uint8_t stage;
  uint8_t progress;
  unsigned long lastUpdateMs;
  char fwVersion[12];
  char message[24];
};

struct NodeState {
  char id[4];
  float camber;
  float z;
  bool stable;
  bool online;
  unsigned long lastSeenMs;

  bool macKnown;
  uint8_t mac[6];

  float batteryVoltage;
  float batterySoc;
  bool batteryValid;

  float toe;
  bool toeValid;

  char fwVersion[16];
  bool fwValid;

  NodeConfigCache cfg;
  NodeTxState tx;
  NodeOtaState ota;
};

struct UnknownNodeState {
  bool used;
  bool online;
  bool macKnown;
  uint8_t mac[6];
  char id[4];
  float camber;
  unsigned long lastSeenMs;

  float batteryVoltage;
  float batterySoc;
  bool batteryValid;

  float toe;
  bool toeValid;

  char fwVersion[16];
  bool fwValid;

  bool pendingAssign;
  char requestedRole[4];

  bool lastSendOk;
  bool pendingAck;
  bool lastAckOk;
  unsigned long lastSendMs;
  unsigned long lastAckMs;
  char lastStatus[24];

  NodeOtaState ota;
};

enum BatchOtaPhase : uint8_t {
  BATCH_IDLE = 0,
  BATCH_PREPARE,
  BATCH_SEND_START,
  BATCH_WAIT_ACTIVITY,
  BATCH_WAIT_RETURN,
  BATCH_FINISHED
};

struct BatchOtaItemResult {
  bool used;
  int8_t nodeIndex;
  char nodeId[4];
  bool started;
  bool finished;
  bool success;
  bool sawActive;
  bool sawOfflineAfterActive;
  uint8_t lastStage;
  uint8_t lastProgress;
  unsigned long startedMs;
  unsigned long finishedMs;
  char result[24];
};

constexpr size_t BATCH_OTA_LOG_SIZE = 2048;

struct BatchOtaState {
  bool active;
  bool finished;
  uint8_t phase;
  uint8_t total;
  int8_t current;
  unsigned long startedMs;
  unsigned long phaseStartedMs;
  char fwFile[48];
  OtaStartPacket startPkt;
  BatchOtaItemResult items[MAX_ASSIGNED_NODES];
  char status[64];
  char log[BATCH_OTA_LOG_SIZE];
};

struct SystemOptions {
  bool skipAlreadyUpdated;
  bool allowUnknownBattery;
  bool blockLowBattery;
  bool verboseOtaLog;
  float minBatterySoc;
};

extern NodeState nodes[MAX_ASSIGNED_NODES];
extern UnknownNodeState unknownNodes[MAX_UNKNOWN_NODES];
extern BatchOtaState batchOta;
extern SystemOptions systemOptions;

extern WebServer server;
extern uint8_t broadcastAddress[6];

extern portMUX_TYPE mux;
extern unsigned long lastTablePrintMs;

extern char uiLastEvent[96];

void setUiLastEvent(const char* msg);
bool isEspNowLinkHealthy();
int getAssignedOnlineCount();
int getUnknownOnlineCount();
const char* getCurrentBatchNodeId();
String getLastBatchLogLine();

bool isValidMagic(const uint8_t* magic);
void safeCopyId(char* dst, const char* src);
String macToString(const uint8_t* mac);
bool sameMac(const uint8_t* a, const uint8_t* b);
bool isOfficialRole(const char* id);

int findNodeIndexById(const char* id);
int findNodeIndexByMac(const uint8_t* mac);
int findUnknownIndexByMac(const uint8_t* mac);
int findFreeUnknownIndex();

bool ensurePeer(const uint8_t* mac);

void markNodeStatus(int idx, const char* status);
void setUnknownStatus(int idx, const char* status);
void updateNodeMac(int idx, const uint8_t* mac);

void initUnknownNode(int idx, const uint8_t* mac, const TelemetryPacket& pkt);
void updateUnknownNode(int idx, const TelemetryPacket& pkt);
void clearUnknownNode(int idx);

bool parseBoolArg(const String& s);
bool getNodeConfigFromRequest(int idx, ConfigPacketV1& pkt, String& err);
bool getOtaArgsFromRequest(OtaStartPacket& pkt, String& err);

void updateTelemetryState(const uint8_t* mac, const TelemetryPacket& pkt);
void updateBatteryTelemetryState(const uint8_t* mac, const BatteryTelemetryPacketV1& pkt);
void updateToeTelemetryState(const uint8_t* mac, const ToeTelemetryPacketV1& pkt);
void updateFwInfoState(const uint8_t* mac, const FirmwareInfoPacketV1& pkt);

bool promoteUnknownToAssigned(int unkIdx, const char* role);
void applyAckToNode(const uint8_t* mac, const ConfigAckPacketV1& ack);
void applyOtaStatusToNode(const uint8_t* mac, const OtaStatusPacket& pkt);
void applyOtaResultToNode(const uint8_t* mac, const OtaResultPacket& pkt);

void updateOnlineTimeouts();
void printMasterMac();
void printNodeTable();

bool sendLegacyCommandToNode(int idx, const char* cmdText);
void sendZeroAll();
bool sendConfigToNode(int idx, const ConfigPacketV1& pkt);
bool requestConfigFromNode(int idx);
bool assignUnknownNodeRole(int unkIdx, const char* role);

bool sendOtaToAssignedNode(int idx, const OtaStartPacket& pkt);
bool sendOtaToUnknownNode(int unkIdx, const OtaStartPacket& pkt);

const char* batchPhaseToText(uint8_t phase);
void resetBatchOtaState();
bool startBatchOta(const char* fwFile, const OtaStartPacket& pkt, String& err);
void updateBatchOta();
bool isBatchOtaBusy();
bool isAnySlaveOtaActive();
bool isUiActionLockedByOta();

void loadSystemOptions();
bool saveSystemOptions();
