#include "state.h"

#include <Preferences.h>
#include <cstring>

NodeState nodes[MAX_ASSIGNED_NODES];
UnknownNodeState unknownNodes[MAX_UNKNOWN_NODES];
BatchOtaState batchOta;
SystemOptions systemOptions = {
  true,   // skipAlreadyUpdated
  true,   // allowUnknownBattery
  false,  // blockLowBattery
  false,  // verboseOtaLog
  30.0f   // minBatterySoc
};

WebServer server(80);
uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
unsigned long lastTablePrintMs = 0;
char uiLastEvent[96] = "";

static const char* kNodeIds[MAX_ASSIGNED_NODES] = {"FL", "FR", "ST", "RL", "RR"};
static bool g_stateInited = false;
static bool g_espNowHealthy = false;

static void initNodeDefaults() {
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    memset(&nodes[i], 0, sizeof(nodes[i]));
    safeCopyId(nodes[i].id, kNodeIds[i]);
    nodes[i].cfg.alpha = DEFAULT_ALPHA;
    nodes[i].cfg.sampleCount = DEFAULT_SAMPLE_COUNT;
    nodes[i].cfg.stabilityThreshold = DEFAULT_STABILITY_THRESHOLD;
    nodes[i].cfg.stabilityTimeMs = DEFAULT_STABILITY_TIME_MS;
    nodes[i].cfg.invertSign = DEFAULT_INVERT_SIGN;
    nodes[i].cfg.autoBeepStable = DEFAULT_AUTO_BEEP_STABLE;
    nodes[i].cfg.valid = true;
    strcpy(nodes[i].tx.lastStatus, "IDLE");
    strcpy(nodes[i].ota.message, "");
    strcpy(nodes[i].ota.fwVersion, "");
  }

  memset(unknownNodes, 0, sizeof(unknownNodes));
  resetBatchOtaState();
  g_stateInited = true;
}

static void ensureStateInit() {
  if (!g_stateInited) initNodeDefaults();
}

static bool nodeIsOnlineByTime(unsigned long lastSeen) {
  return lastSeen && ((millis() - lastSeen) <= offlineTimeoutMs);
}

static NodeConfigCache defaultCfg() {
  NodeConfigCache cfg = {};
  cfg.alpha = DEFAULT_ALPHA;
  cfg.sampleCount = DEFAULT_SAMPLE_COUNT;
  cfg.stabilityThreshold = DEFAULT_STABILITY_THRESHOLD;
  cfg.stabilityTimeMs = DEFAULT_STABILITY_TIME_MS;
  cfg.invertSign = DEFAULT_INVERT_SIGN;
  cfg.autoBeepStable = DEFAULT_AUTO_BEEP_STABLE;
  cfg.valid = true;
  return cfg;
}

static void batchLogAppend(const String& line) {
  ensureStateInit();

  String cur(batchOta.log);
  if (cur.length() && !cur.endsWith("\n")) cur += "\n";
  cur += line;

  while (cur.length() >= (int)BATCH_OTA_LOG_SIZE - 1) {
    int nl = cur.indexOf('\n');
    if (nl < 0) {
      cur = cur.substring(cur.length() - ((int)BATCH_OTA_LOG_SIZE / 2));
      break;
    }
    cur.remove(0, nl + 1);
  }

  strlcpy(batchOta.log, cur.c_str(), sizeof(batchOta.log));
}

static BatchOtaItemResult* currentBatchItem() {
  if (!batchOta.active) return nullptr;
  if (batchOta.current < 0 || batchOta.current >= MAX_ASSIGNED_NODES) return nullptr;
  if (!batchOta.items[batchOta.current].used) return nullptr;
  return &batchOta.items[batchOta.current];
}

static void finishBatchItem(BatchOtaItemResult& item, bool success, const char* result) {
  item.finished = true;
  item.success = success;
  item.finishedMs = millis();
  strlcpy(item.result, result ? result : (success ? "SUCCESS" : "FAIL"), sizeof(item.result));
  batchLogAppend(String(item.nodeId) + " -> " + item.result);
}

static void advanceBatchToNextOrFinish() {
  int next = batchOta.current + 1;
  while (next < MAX_ASSIGNED_NODES && !batchOta.items[next].used) next++;

  if (next >= MAX_ASSIGNED_NODES) {
    batchOta.active = false;
    batchOta.finished = true;
    batchOta.phase = BATCH_FINISHED;
    strlcpy(batchOta.status, "Batch OTA completato", sizeof(batchOta.status));
    batchLogAppend("Batch OTA completato");
    return;
  }

  batchOta.current = next;
  batchOta.phase = BATCH_SEND_START;
  batchOta.phaseStartedMs = millis();
  strlcpy(batchOta.status, "Invio OTA al prossimo nodo", sizeof(batchOta.status));
}

static int resolveNodeIndexForPacket(const uint8_t* mac, const char* id) {
  int idx = findNodeIndexByMac(mac);
  if (idx >= 0) return idx;
  if (id && id[0]) {
    idx = findNodeIndexById(id);
    if (idx >= 0) return idx;
  }
  return -1;
}

static int resolveUnknownIndexForPacket(const uint8_t* mac) {
  int idx = findUnknownIndexByMac(mac);
  return idx;
}

static void setNodeTxStatus(NodeState& n, const char* status) {
  strlcpy(n.tx.lastStatus, status ? status : "", sizeof(n.tx.lastStatus));
}

static void setUnknownTxStatus(UnknownNodeState& n, const char* status) {
  strlcpy(n.lastStatus, status ? status : "", sizeof(n.lastStatus));
}

static void applyCfgToNodeCache(NodeConfigCache& cfg, const ConfigAckPacketV1& ack) {
  cfg.alpha = ack.alpha;
  cfg.sampleCount = ack.sampleCount;
  cfg.stabilityThreshold = ack.stabilityThreshold;
  cfg.stabilityTimeMs = ack.stabilityTimeMs;
  cfg.invertSign = ack.invertSign != 0;
  cfg.autoBeepStable = ack.autoBeepStable != 0;
  cfg.valid = ack.ok != 0;
}

void setUiLastEvent(const char* msg) {
  ensureStateInit();
  strlcpy(uiLastEvent, msg ? msg : "", sizeof(uiLastEvent));
}

bool isEspNowLinkHealthy() {
  return g_espNowHealthy;
}

int getAssignedOnlineCount() {
  ensureStateInit();
  int c = 0;
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) if (nodes[i].online) c++;
  return c;
}

int getUnknownOnlineCount() {
  ensureStateInit();
  int c = 0;
  for (int i = 0; i < MAX_UNKNOWN_NODES; i++) if (unknownNodes[i].used && unknownNodes[i].online) c++;
  return c;
}

const char* getCurrentBatchNodeId() {
  ensureStateInit();
  BatchOtaItemResult* item = currentBatchItem();
  return item ? item->nodeId : "--";
}

String getLastBatchLogLine() {
  ensureStateInit();
  String s(batchOta.log);
  s.trim();
  int p = s.lastIndexOf('\n');
  return (p >= 0) ? s.substring(p + 1) : s;
}

bool isValidMagic(const uint8_t* magic) {
  return magic && magic[0] == 'R' && magic[1] == 'F';
}

void safeCopyId(char* dst, const char* src) {
  if (!dst) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, 3);
  dst[3] = '\0';
}

String macToString(const uint8_t* mac) {
  if (!mac) return "--:--:--:--:--:--";
  char b[24];
  snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(b);
}

bool sameMac(const uint8_t* a, const uint8_t* b) {
  return a && b && memcmp(a, b, 6) == 0;
}

bool isOfficialRole(const char* id) {
  if (!id || !id[0]) return false;
  return strcmp(id, "FL") == 0 || strcmp(id, "FR") == 0 || strcmp(id, "ST") == 0 || strcmp(id, "RL") == 0 || strcmp(id, "RR") == 0;
}

int findNodeIndexById(const char* id) {
  ensureStateInit();
  if (!id || !id[0]) return -1;
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) if (strcmp(nodes[i].id, id) == 0) return i;
  return -1;
}

int findNodeIndexByMac(const uint8_t* mac) {
  ensureStateInit();
  if (!mac) return -1;
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    if (nodes[i].macKnown && sameMac(nodes[i].mac, mac)) return i;
  }
  return -1;
}

int findUnknownIndexByMac(const uint8_t* mac) {
  ensureStateInit();
  if (!mac) return -1;
  for (int i = 0; i < MAX_UNKNOWN_NODES; i++) {
    if (unknownNodes[i].used && unknownNodes[i].macKnown && sameMac(unknownNodes[i].mac, mac)) return i;
  }
  return -1;
}

int findFreeUnknownIndex() {
  ensureStateInit();
  for (int i = 0; i < MAX_UNKNOWN_NODES; i++) if (!unknownNodes[i].used) return i;
  return -1;
}

bool ensurePeer(const uint8_t* mac) {
  if (!mac) return false;
  if (esp_now_is_peer_exist(mac)) return true;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = WIFI_CHANNEL;
  peer.encrypt = false;
  esp_err_t err = esp_now_add_peer(&peer);
  return err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST;
}

void markNodeStatus(int idx, const char* status) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES) return;
  setNodeTxStatus(nodes[idx], status);
}

void setUnknownStatus(int idx, const char* status) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_UNKNOWN_NODES || !unknownNodes[idx].used) return;
  setUnknownTxStatus(unknownNodes[idx], status);
}

void updateNodeMac(int idx, const uint8_t* mac) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES || !mac) return;
  memcpy(nodes[idx].mac, mac, 6);
  nodes[idx].macKnown = true;
  ensurePeer(mac);
}

void initUnknownNode(int idx, const uint8_t* mac, const TelemetryPacket& pkt) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_UNKNOWN_NODES || !mac) return;
  UnknownNodeState& u = unknownNodes[idx];
  memset(&u, 0, sizeof(u));
  u.used = true;
  u.online = true;
  u.macKnown = true;
  memcpy(u.mac, mac, 6);
  safeCopyId(u.id, pkt.id);
  u.camber = pkt.camber;
  u.lastSeenMs = millis();
  strcpy(u.lastStatus, "LIVE");
  ensurePeer(mac);
}

void updateUnknownNode(int idx, const TelemetryPacket& pkt) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_UNKNOWN_NODES || !unknownNodes[idx].used) return;
  UnknownNodeState& u = unknownNodes[idx];
  safeCopyId(u.id, pkt.id);
  u.camber = pkt.camber;
  u.online = true;
  u.lastSeenMs = millis();
}

void clearUnknownNode(int idx) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_UNKNOWN_NODES) return;
  memset(&unknownNodes[idx], 0, sizeof(unknownNodes[idx]));
}

bool parseBoolArg(const String& s) {
  String t = s;
  t.trim();
  t.toLowerCase();
  return t == "1" || t == "true" || t == "on" || t == "yes";
}

bool getNodeConfigFromRequest(int idx, ConfigPacketV1& pkt, String& err) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES) {
    err = "idx non valido";
    return false;
  }
  if (!server.hasArg("alpha") || !server.hasArg("sampleCount") || !server.hasArg("stabilityThreshold") ||
      !server.hasArg("stabilityTime") || !server.hasArg("invertSign") || !server.hasArg("autoBeepStable") || !server.hasArg("nodeId")) {
    err = "Parametri config mancanti";
    return false;
  }

  String nodeId = server.arg("nodeId");
  nodeId.trim();
  nodeId.toUpperCase();
  if (!isOfficialRole(nodeId.c_str())) {
    err = "nodeId non valido";
    return false;
  }

  memset(&pkt, 0, sizeof(pkt));
  pkt.magic[0] = 'R';
  pkt.magic[1] = 'F';
  pkt.version = 1;
  pkt.type = PKT_SET_CONFIG;
  safeCopyId(pkt.targetId, nodes[idx].id);
  safeCopyId(pkt.newNodeId, nodeId.c_str());
  pkt.alpha = server.arg("alpha").toFloat();
  pkt.sampleCount = (uint16_t)server.arg("sampleCount").toInt();
  pkt.stabilityThreshold = server.arg("stabilityThreshold").toFloat();
  pkt.stabilityTimeMs = (uint16_t)server.arg("stabilityTime").toInt();
  pkt.invertSign = parseBoolArg(server.arg("invertSign")) ? 1 : 0;
  pkt.autoBeepStable = parseBoolArg(server.arg("autoBeepStable")) ? 1 : 0;

  if (pkt.sampleCount == 0) pkt.sampleCount = 1;
  if (pkt.stabilityTimeMs == 0) pkt.stabilityTimeMs = 10;
  return true;
}

bool getOtaArgsFromRequest(OtaStartPacket& pkt, String& err) {
  memset(&pkt, 0, sizeof(pkt));
  if (!server.hasArg("fwVersion")) {
    err = "fwVersion mancante";
    return false;
  }
  pkt.magic[0] = 'R';
  pkt.magic[1] = 'F';
  pkt.version = 1;
  pkt.type = PKT_OTA_START;
  strlcpy(pkt.ssid, AP_SSID, sizeof(pkt.ssid));
  strlcpy(pkt.password, AP_PASS, sizeof(pkt.password));
  if (server.hasArg("url")) strlcpy(pkt.url, server.arg("url").c_str(), sizeof(pkt.url));
  if (server.hasArg("fwSize")) pkt.fwSize = (uint32_t)server.arg("fwSize").toInt();
  if (server.hasArg("fwCrc")) pkt.fwCrc = (uint32_t)server.arg("fwCrc").toInt();
  strlcpy(pkt.fwVersion, server.arg("fwVersion").c_str(), sizeof(pkt.fwVersion));
  return true;
}

void updateTelemetryState(const uint8_t* mac, const TelemetryPacket& pkt) {
  ensureStateInit();
  g_espNowHealthy = true;
  const unsigned long now = millis();

  int idx = findNodeIndexByMac(mac);
  if (idx < 0 && isOfficialRole(pkt.id)) {
    idx = findNodeIndexById(pkt.id);
    if (idx >= 0 && !nodes[idx].macKnown) updateNodeMac(idx, mac);
  }

  if (idx >= 0) {
    NodeState& n = nodes[idx];
    if (!n.macKnown) updateNodeMac(idx, mac);
    safeCopyId(n.id, pkt.id[0] ? pkt.id : nodes[idx].id);
    n.camber = pkt.camber;
    n.z = pkt.z;
    n.stable = pkt.stable;
    n.online = true;
    n.lastSeenMs = now;
    if (n.tx.pendingAck && (now - n.tx.lastSendMs > ackTimeoutMs)) n.tx.pendingAck = false;
    if (n.tx.lastStatus[0] == '\0' || strcmp(n.tx.lastStatus, "LOWLVL SEND ERR") == 0) strcpy(n.tx.lastStatus, "LIVE");
    return;
  }

  int unkIdx = findUnknownIndexByMac(mac);
  if (unkIdx < 0) {
    unkIdx = findFreeUnknownIndex();
    if (unkIdx >= 0) initUnknownNode(unkIdx, mac, pkt);
  } else {
    updateUnknownNode(unkIdx, pkt);
  }
}

void updateBatteryTelemetryState(const uint8_t* mac, const BatteryTelemetryPacketV1& pkt) {
  ensureStateInit();
  if (!isValidMagic(pkt.magic)) return;
  int idx = resolveNodeIndexForPacket(mac, pkt.id);
  if (idx >= 0) {
    nodes[idx].batteryVoltage = pkt.voltage;
    nodes[idx].batterySoc = pkt.soc;
    nodes[idx].batteryValid = true;
    return;
  }
  int unkIdx = resolveUnknownIndexForPacket(mac);
  if (unkIdx >= 0) {
    unknownNodes[unkIdx].batteryVoltage = pkt.voltage;
    unknownNodes[unkIdx].batterySoc = pkt.soc;
    unknownNodes[unkIdx].batteryValid = true;
  }
}

void updateToeTelemetryState(const uint8_t* mac, const ToeTelemetryPacketV1& pkt) {
  ensureStateInit();
  if (!isValidMagic(pkt.magic)) return;
  int idx = resolveNodeIndexForPacket(mac, pkt.id);
  if (idx >= 0) {
    nodes[idx].toe = pkt.toe;
    nodes[idx].toeValid = true;
    return;
  }
  int unkIdx = resolveUnknownIndexForPacket(mac);
  if (unkIdx >= 0) {
    unknownNodes[unkIdx].toe = pkt.toe;
    unknownNodes[unkIdx].toeValid = true;
  }
}

void updateFwInfoState(const uint8_t* mac, const FirmwareInfoPacketV1& pkt) {
  ensureStateInit();
  if (!isValidMagic(pkt.magic)) return;
  int idx = resolveNodeIndexForPacket(mac, pkt.id);
  if (idx >= 0) {
    strlcpy(nodes[idx].fwVersion, pkt.fwVersion, sizeof(nodes[idx].fwVersion));
    nodes[idx].fwValid = pkt.fwVersion[0] != '\0';
    return;
  }
  int unkIdx = resolveUnknownIndexForPacket(mac);
  if (unkIdx >= 0) {
    strlcpy(unknownNodes[unkIdx].fwVersion, pkt.fwVersion, sizeof(unknownNodes[unkIdx].fwVersion));
    unknownNodes[unkIdx].fwValid = pkt.fwVersion[0] != '\0';
  }
}

bool promoteUnknownToAssigned(int unkIdx, const char* role) {
  ensureStateInit();
  if (unkIdx < 0 || unkIdx >= MAX_UNKNOWN_NODES || !unknownNodes[unkIdx].used || !role || !isOfficialRole(role)) return false;
  int idx = findNodeIndexById(role);
  if (idx < 0) return false;

  if (nodes[idx].macKnown && !sameMac(nodes[idx].mac, unknownNodes[unkIdx].mac)) return false;

  NodeState& n = nodes[idx];
  UnknownNodeState& u = unknownNodes[unkIdx];

  safeCopyId(n.id, role);
  n.camber = u.camber;
  n.online = u.online;
  n.lastSeenMs = u.lastSeenMs;
  n.batteryVoltage = u.batteryVoltage;
  n.batterySoc = u.batterySoc;
  n.batteryValid = u.batteryValid;
  n.toe = u.toe;
  n.toeValid = u.toeValid;
  n.fwValid = u.fwValid;
  strlcpy(n.fwVersion, u.fwVersion, sizeof(n.fwVersion));
  n.ota = u.ota;
  n.cfg = defaultCfg();
  memcpy(n.mac, u.mac, 6);
  n.macKnown = u.macKnown;
  setNodeTxStatus(n, u.lastStatus[0] ? u.lastStatus : "LIVE");

  clearUnknownNode(unkIdx);
  setUiLastEvent((String("Nodo ") + role + " associato").c_str());
  return true;
}

void applyAckToNode(const uint8_t* mac, const ConfigAckPacketV1& ack) {
  ensureStateInit();
  if (!isValidMagic(ack.magic)) return;
  int idx = resolveNodeIndexForPacket(mac, ack.nodeId);
  if (idx >= 0) {
    NodeState& n = nodes[idx];
    n.tx.pendingAck = false;
    n.tx.lastAckOk = ack.ok != 0;
    n.tx.lastAckMs = millis();
    applyCfgToNodeCache(n.cfg, ack);
    setNodeTxStatus(n, ack.message[0] ? ack.message : (ack.ok ? "CFG OK" : "CFG ERR"));
    if (ack.ok && ack.nodeId[0] && strcmp(n.id, ack.nodeId) != 0 && isOfficialRole(ack.nodeId)) safeCopyId(n.id, ack.nodeId);
    return;
  }
  int unkIdx = resolveUnknownIndexForPacket(mac);
  if (unkIdx >= 0) {
    UnknownNodeState& u = unknownNodes[unkIdx];
    u.pendingAck = false;
    u.lastAckOk = ack.ok != 0;
    u.lastAckMs = millis();
    setUnknownTxStatus(u, ack.message[0] ? ack.message : (ack.ok ? "CFG OK" : "CFG ERR"));
    if (ack.ok && u.pendingAssign && isOfficialRole(u.requestedRole)) {
      promoteUnknownToAssigned(unkIdx, u.requestedRole);
    }
  }
}

void applyOtaStatusToNode(const uint8_t* mac, const OtaStatusPacket& pkt) {
  ensureStateInit();
  if (!isValidMagic(pkt.magic)) return;
  int idx = resolveNodeIndexForPacket(mac, pkt.nodeId);
  if (idx >= 0) {
    NodeState& n = nodes[idx];
    n.ota.active = true;
    n.ota.stage = pkt.stage;
    n.ota.progress = pkt.progress;
    n.ota.lastUpdateMs = millis();
    n.ota.lastOk = false;
    strlcpy(n.ota.message, "OTA attiva", sizeof(n.ota.message));
    setNodeTxStatus(n, "OTA ACTIVE");

    if (batchOta.active) {
      BatchOtaItemResult* item = currentBatchItem();
      if (item && item->nodeIndex == idx) {
        item->sawActive = true;
        item->lastStage = pkt.stage;
        item->lastProgress = pkt.progress;
      }
    }
    return;
  }

  int unkIdx = resolveUnknownIndexForPacket(mac);
  if (unkIdx >= 0) {
    UnknownNodeState& u = unknownNodes[unkIdx];
    u.ota.active = true;
    u.ota.stage = pkt.stage;
    u.ota.progress = pkt.progress;
    u.ota.lastUpdateMs = millis();
    u.ota.lastOk = false;
    strlcpy(u.ota.message, "OTA attiva", sizeof(u.ota.message));
    setUnknownTxStatus(u, "OTA ACTIVE");
  }
}

void applyOtaResultToNode(const uint8_t* mac, const OtaResultPacket& pkt) {
  ensureStateInit();
  if (!isValidMagic(pkt.magic)) return;
  int idx = resolveNodeIndexForPacket(mac, pkt.nodeId);
  if (idx >= 0) {
    NodeState& n = nodes[idx];
    n.ota.active = false;
    n.ota.lastOk = pkt.success != 0;
    n.ota.progress = pkt.success ? 100 : n.ota.progress;
    n.ota.lastUpdateMs = millis();
    strlcpy(n.ota.message, pkt.message, sizeof(n.ota.message));
    setNodeTxStatus(n, pkt.success ? "OTA OK" : "OTA ERR");

    if (batchOta.active) {
      BatchOtaItemResult* item = currentBatchItem();
      if (item && item->nodeIndex == idx) {
        item->lastStage = n.ota.stage;
        item->lastProgress = n.ota.progress;
      }
    }
    return;
  }

  int unkIdx = resolveUnknownIndexForPacket(mac);
  if (unkIdx >= 0) {
    UnknownNodeState& u = unknownNodes[unkIdx];
    u.ota.active = false;
    u.ota.lastOk = pkt.success != 0;
    u.ota.progress = pkt.success ? 100 : u.ota.progress;
    u.ota.lastUpdateMs = millis();
    strlcpy(u.ota.message, pkt.message, sizeof(u.ota.message));
    setUnknownTxStatus(u, pkt.success ? "OTA OK" : "OTA ERR");
  }
}

void updateOnlineTimeouts() {
  ensureStateInit();
  const unsigned long now = millis();

  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    nodes[i].online = nodeIsOnlineByTime(nodes[i].lastSeenMs);
    if (nodes[i].tx.pendingAck && (now - nodes[i].tx.lastSendMs > ackTimeoutMs)) {
      nodes[i].tx.pendingAck = false;
      if (!nodes[i].tx.lastAckOk) setNodeTxStatus(nodes[i], "ACK TIMEOUT");
    }
    if (nodes[i].ota.active && (now - nodes[i].ota.lastUpdateMs > 15000UL)) {
      nodes[i].ota.active = false;
      if (!nodes[i].ota.lastOk) strlcpy(nodes[i].ota.message, "OTA timeout", sizeof(nodes[i].ota.message));
    }
  }

  for (int i = 0; i < MAX_UNKNOWN_NODES; i++) {
    if (!unknownNodes[i].used) continue;
    unknownNodes[i].online = nodeIsOnlineByTime(unknownNodes[i].lastSeenMs);
    if (unknownNodes[i].pendingAck && (now - unknownNodes[i].lastSendMs > ackTimeoutMs)) {
      unknownNodes[i].pendingAck = false;
      if (!unknownNodes[i].lastAckOk) setUnknownTxStatus(unknownNodes[i], "ACK TIMEOUT");
    }
    if (unknownNodes[i].ota.active && (now - unknownNodes[i].ota.lastUpdateMs > 15000UL)) {
      unknownNodes[i].ota.active = false;
      if (!unknownNodes[i].ota.lastOk) strlcpy(unknownNodes[i].ota.message, "OTA timeout", sizeof(unknownNodes[i].ota.message));
    }
  }
}

void printMasterMac() {
  ensureStateInit();
  Serial.print("Master AP MAC: ");
  Serial.println(WiFi.softAPmacAddress());
  Serial.print("Master STA MAC: ");
  Serial.println(WiFi.macAddress());
}

void printNodeTable() {
  ensureStateInit();
  Serial.println();
  Serial.println("---- NODES ----");
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    Serial.printf("%s | %s | camber=%0.2f | batt=%s | fw=%s\n",
      nodes[i].id,
      nodes[i].online ? "ONLINE" : "OFFLINE",
      nodes[i].camber,
      nodes[i].batteryValid ? String(nodes[i].batterySoc, 0).c_str() : "--",
      nodes[i].fwValid ? nodes[i].fwVersion : "--");
  }

  int unkOnline = getUnknownOnlineCount();
  Serial.printf("Unknown online: %d\n", unkOnline);
  for (int i = 0; i < MAX_UNKNOWN_NODES; i++) {
    if (!unknownNodes[i].used || !unknownNodes[i].online) continue;
    Serial.printf("UNK[%d] %s | %s | batt=%s | fw=%s\n",
      i,
      unknownNodes[i].id,
      macToString(unknownNodes[i].mac).c_str(),
      unknownNodes[i].batteryValid ? String(unknownNodes[i].batterySoc, 0).c_str() : "--",
      unknownNodes[i].fwValid ? unknownNodes[i].fwVersion : "--");
  }
}

bool sendLegacyCommandToNode(int idx, const char* cmdText) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES || !nodes[idx].macKnown || !cmdText) return false;
  if (!ensurePeer(nodes[idx].mac)) return false;

  CommandPacket pkt = {};
  strlcpy(pkt.cmd, cmdText, sizeof(pkt.cmd));
  esp_err_t err = esp_now_send(nodes[idx].mac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  nodes[idx].tx.pendingAck = false;
  nodes[idx].tx.lastSendMs = millis();
  nodes[idx].tx.lastSendOk = (err == ESP_OK);
  setNodeTxStatus(nodes[idx], err == ESP_OK ? cmdText : "SEND ERR");
  return err == ESP_OK;
}

void sendZeroAll() {
  ensureStateInit();
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    if (nodes[i].macKnown) sendLegacyCommandToNode(i, "zero");
  }
  setUiLastEvent("ZERO ALL inviato");
}

bool sendConfigToNode(int idx, const ConfigPacketV1& pkt) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES || !nodes[idx].macKnown) return false;
  if (!ensurePeer(nodes[idx].mac)) return false;
  esp_err_t err = esp_now_send(nodes[idx].mac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  nodes[idx].tx.pendingAck = (err == ESP_OK);
  nodes[idx].tx.lastSendOk = (err == ESP_OK);
  nodes[idx].tx.lastAckOk = false;
  nodes[idx].tx.lastSendMs = millis();
  setNodeTxStatus(nodes[idx], err == ESP_OK ? "CFG SENT" : "SEND ERR");
  return err == ESP_OK;
}

bool requestConfigFromNode(int idx) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES || !nodes[idx].macKnown) return false;
  if (!ensurePeer(nodes[idx].mac)) return false;

  ConfigPacketV1 pkt = {};
  pkt.magic[0] = 'R';
  pkt.magic[1] = 'F';
  pkt.version = 1;
  pkt.type = PKT_GET_CONFIG;
  safeCopyId(pkt.targetId, nodes[idx].id);
  safeCopyId(pkt.newNodeId, nodes[idx].id);

  esp_err_t err = esp_now_send(nodes[idx].mac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  nodes[idx].tx.pendingAck = (err == ESP_OK);
  nodes[idx].tx.lastAckOk = false;
  nodes[idx].tx.lastSendOk = (err == ESP_OK);
  nodes[idx].tx.lastSendMs = millis();
  setNodeTxStatus(nodes[idx], err == ESP_OK ? "CFG REQ" : "SEND ERR");
  return err == ESP_OK;
}

bool assignUnknownNodeRole(int unkIdx, const char* role) {
  ensureStateInit();
  if (unkIdx < 0 || unkIdx >= MAX_UNKNOWN_NODES || !unknownNodes[unkIdx].used || !role || !isOfficialRole(role) || !unknownNodes[unkIdx].macKnown) return false;
  if (!ensurePeer(unknownNodes[unkIdx].mac)) return false;

  ConfigPacketV1 pkt = {};
  pkt.magic[0] = 'R';
  pkt.magic[1] = 'F';
  pkt.version = 1;
  pkt.type = PKT_SET_CONFIG;
  safeCopyId(pkt.targetId, unknownNodes[unkIdx].id[0] ? unknownNodes[unkIdx].id : "UNK");
  safeCopyId(pkt.newNodeId, role);
  pkt.alpha = DEFAULT_ALPHA;
  pkt.sampleCount = DEFAULT_SAMPLE_COUNT;
  pkt.stabilityThreshold = DEFAULT_STABILITY_THRESHOLD;
  pkt.stabilityTimeMs = DEFAULT_STABILITY_TIME_MS;
  pkt.invertSign = DEFAULT_INVERT_SIGN ? 1 : 0;
  pkt.autoBeepStable = DEFAULT_AUTO_BEEP_STABLE ? 1 : 0;

  esp_err_t err = esp_now_send(unknownNodes[unkIdx].mac, reinterpret_cast<const uint8_t*>(&pkt), sizeof(pkt));
  unknownNodes[unkIdx].pendingAck = (err == ESP_OK);
  unknownNodes[unkIdx].lastAckOk = false;
  unknownNodes[unkIdx].lastSendOk = (err == ESP_OK);
  unknownNodes[unkIdx].lastSendMs = millis();
  strlcpy(unknownNodes[unkIdx].requestedRole, role, sizeof(unknownNodes[unkIdx].requestedRole));
  unknownNodes[unkIdx].pendingAssign = (err == ESP_OK);
  setUnknownTxStatus(unknownNodes[unkIdx], err == ESP_OK ? "ASSIGN SENT" : "SEND ERR");
  return err == ESP_OK;
}

bool sendOtaToAssignedNode(int idx, const OtaStartPacket& pkt) {
  ensureStateInit();
  if (idx < 0 || idx >= MAX_ASSIGNED_NODES || !nodes[idx].macKnown) return false;
  if (!ensurePeer(nodes[idx].mac)) return false;

  OtaStartPacket tx = pkt;
  safeCopyId(tx.targetId, nodes[idx].id);
  esp_err_t err = esp_now_send(nodes[idx].mac, reinterpret_cast<const uint8_t*>(&tx), sizeof(tx));
  nodes[idx].tx.lastSendMs = millis();
  nodes[idx].tx.lastSendOk = (err == ESP_OK);
  nodes[idx].ota.lastOk = false;
  nodes[idx].ota.active = false;
  nodes[idx].ota.progress = 0;
  strlcpy(nodes[idx].ota.fwVersion, tx.fwVersion, sizeof(nodes[idx].ota.fwVersion));
  strlcpy(nodes[idx].ota.message, err == ESP_OK ? "OTA start inviato" : "OTA start fallito", sizeof(nodes[idx].ota.message));
  setNodeTxStatus(nodes[idx], err == ESP_OK ? "OTA SENT" : "SEND ERR");
  return err == ESP_OK;
}

bool sendOtaToUnknownNode(int unkIdx, const OtaStartPacket& pkt) {
  ensureStateInit();
  if (unkIdx < 0 || unkIdx >= MAX_UNKNOWN_NODES || !unknownNodes[unkIdx].used || !unknownNodes[unkIdx].macKnown) return false;
  if (!ensurePeer(unknownNodes[unkIdx].mac)) return false;

  OtaStartPacket tx = pkt;
  safeCopyId(tx.targetId, unknownNodes[unkIdx].id[0] ? unknownNodes[unkIdx].id : "UNK");
  esp_err_t err = esp_now_send(unknownNodes[unkIdx].mac, reinterpret_cast<const uint8_t*>(&tx), sizeof(tx));
  unknownNodes[unkIdx].lastSendMs = millis();
  unknownNodes[unkIdx].lastSendOk = (err == ESP_OK);
  unknownNodes[unkIdx].ota.lastOk = false;
  unknownNodes[unkIdx].ota.active = false;
  unknownNodes[unkIdx].ota.progress = 0;
  strlcpy(unknownNodes[unkIdx].ota.fwVersion, tx.fwVersion, sizeof(unknownNodes[unkIdx].ota.fwVersion));
  strlcpy(unknownNodes[unkIdx].ota.message, err == ESP_OK ? "OTA start inviato" : "OTA start fallito", sizeof(unknownNodes[unkIdx].ota.message));
  setUnknownTxStatus(unknownNodes[unkIdx], err == ESP_OK ? "OTA SENT" : "SEND ERR");
  return err == ESP_OK;
}

const char* batchPhaseToText(uint8_t phase) {
  switch (phase) {
    case BATCH_IDLE: return "IDLE";
    case BATCH_PREPARE: return "PREPARE";
    case BATCH_SEND_START: return "SEND_START";
    case BATCH_WAIT_ACTIVITY: return "WAIT_ACTIVITY";
    case BATCH_WAIT_RETURN: return "WAIT_RETURN";
    case BATCH_FINISHED: return "FINISHED";
    default: return "?";
  }
}

void resetBatchOtaState() {
  memset(&batchOta, 0, sizeof(batchOta));
  batchOta.phase = BATCH_IDLE;
  batchOta.current = -1;
  strlcpy(batchOta.status, "Idle", sizeof(batchOta.status));
}

bool startBatchOta(const char* fwFile, const OtaStartPacket& pkt, String& err) {
  ensureStateInit();
  if (isBatchOtaBusy()) {
    err = "Batch OTA gia attivo";
    return false;
  }

  resetBatchOtaState();
  batchOta.active = true;
  batchOta.finished = false;
  batchOta.phase = BATCH_PREPARE;
  batchOta.startedMs = millis();
  batchOta.phaseStartedMs = batchOta.startedMs;
  batchOta.current = -1;
  batchOta.startPkt = pkt;
  strlcpy(batchOta.fwFile, fwFile ? fwFile : "", sizeof(batchOta.fwFile));
  strlcpy(batchOta.status, "Preparazione batch OTA", sizeof(batchOta.status));
  strlcpy(batchOta.log, "", sizeof(batchOta.log));
  batchLogAppend(String("Batch OTA start: ") + batchOta.fwFile);

  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) {
    if (!nodes[i].macKnown) continue;
    BatchOtaItemResult& item = batchOta.items[i];
    memset(&item, 0, sizeof(item));
    item.used = true;
    item.nodeIndex = i;
    safeCopyId(item.nodeId, nodes[i].id);
    batchOta.total++;
  }

  if (batchOta.total == 0) {
    resetBatchOtaState();
    err = "Nessun nodo assegnato aggiornabile";
    return false;
  }

  advanceBatchToNextOrFinish();
  return true;
}

void updateBatchOta() {
  ensureStateInit();
  if (!batchOta.active) return;

  unsigned long now = millis();
  BatchOtaItemResult* item = currentBatchItem();
  if (!item) {
    advanceBatchToNextOrFinish();
    return;
  }

  NodeState& n = nodes[item->nodeIndex];

  switch ((BatchOtaPhase)batchOta.phase) {
    case BATCH_SEND_START: {
      if (!n.macKnown) {
        finishBatchItem(*item, false, "MAC MISSING");
        advanceBatchToNextOrFinish();
        return;
      }
      item->started = true;
      item->startedMs = now;
      bool ok = sendOtaToAssignedNode(item->nodeIndex, batchOta.startPkt);
      if (!ok) {
        finishBatchItem(*item, false, "SEND ERR");
        advanceBatchToNextOrFinish();
        return;
      }
      strlcpy(batchOta.status, "Attesa attivita OTA", sizeof(batchOta.status));
      batchLogAppend(String(item->nodeId) + " -> START SENT");
      batchOta.phase = BATCH_WAIT_ACTIVITY;
      batchOta.phaseStartedMs = now;
      return;
    }

    case BATCH_WAIT_ACTIVITY: {
      if (n.ota.active) {
        item->sawActive = true;
        strlcpy(batchOta.status, "OTA attiva sul nodo", sizeof(batchOta.status));
        batchOta.phase = BATCH_WAIT_RETURN;
        batchOta.phaseStartedMs = now;
        return;
      }
      if ((now - batchOta.phaseStartedMs) > 10000UL) {
        finishBatchItem(*item, false, "NO ACTIVITY");
        advanceBatchToNextOrFinish();
      }
      return;
    }

    case BATCH_WAIT_RETURN: {
      item->lastStage = n.ota.stage;
      item->lastProgress = n.ota.progress;

      if (item->sawActive && !n.online) item->sawOfflineAfterActive = true;

      if (!n.ota.active) {
        bool success = n.ota.lastOk || (n.fwValid && strcmp(n.fwVersion, batchOta.startPkt.fwVersion) == 0);
        if (success) {
          finishBatchItem(*item, true, "SUCCESS");
          advanceBatchToNextOrFinish();
          return;
        }
        if (n.ota.message[0]) {
          finishBatchItem(*item, false, n.ota.message);
          advanceBatchToNextOrFinish();
          return;
        }
      }

      if ((now - batchOta.phaseStartedMs) > 120000UL) {
        finishBatchItem(*item, false, "TIMEOUT");
        advanceBatchToNextOrFinish();
      }
      return;
    }

    default:
      return;
  }
}

bool isBatchOtaBusy() {
  return batchOta.active;
}

bool isAnySlaveOtaActive() {
  ensureStateInit();
  for (int i = 0; i < MAX_ASSIGNED_NODES; i++) if (nodes[i].ota.active) return true;
  for (int i = 0; i < MAX_UNKNOWN_NODES; i++) if (unknownNodes[i].used && unknownNodes[i].ota.active) return true;
  return false;
}

bool isUiActionLockedByOta() {
  return isBatchOtaBusy() || isAnySlaveOtaActive();
}

void loadSystemOptions() {
  ensureStateInit();
  Preferences prefs;
  if (!prefs.begin("rf73opts", true)) return;
  systemOptions.skipAlreadyUpdated = prefs.getBool("skipSame", systemOptions.skipAlreadyUpdated);
  systemOptions.allowUnknownBattery = prefs.getBool("allowUnk", systemOptions.allowUnknownBattery);
  systemOptions.blockLowBattery = prefs.getBool("blockLow", systemOptions.blockLowBattery);
  systemOptions.verboseOtaLog = prefs.getBool("verbOta", systemOptions.verboseOtaLog);
  systemOptions.minBatterySoc = prefs.getFloat("minSoc", systemOptions.minBatterySoc);
  prefs.end();
}

bool saveSystemOptions() {
  Preferences prefs;
  if (!prefs.begin("rf73opts", false)) return false;
  prefs.putBool("skipSame", systemOptions.skipAlreadyUpdated);
  prefs.putBool("allowUnk", systemOptions.allowUnknownBattery);
  prefs.putBool("blockLow", systemOptions.blockLowBattery);
  prefs.putBool("verbOta", systemOptions.verboseOtaLog);
  prefs.putFloat("minSoc", systemOptions.minBatterySoc);
  prefs.end();
  return true;
}
