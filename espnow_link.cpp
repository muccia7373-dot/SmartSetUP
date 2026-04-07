#include "espnow_link.h"
#include "state.h"

static void onDataSent(const esp_now_send_info_t* tx_info, esp_now_send_status_t status) {
  if (!tx_info) return;
  const uint8_t* mac = tx_info->des_addr;

  int idx = findNodeIndexByMac(mac);
  if (idx >= 0) {
    nodes[idx].tx.lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
    if (status != ESP_NOW_SEND_SUCCESS) markNodeStatus(idx, "LOWLVL SEND ERR");
    return;
  }

  int unkIdx = findUnknownIndexByMac(mac);
  if (unkIdx >= 0) {
    unknownNodes[unkIdx].lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
    if (status != ESP_NOW_SEND_SUCCESS) setUnknownStatus(unkIdx, "LOWLVL SEND ERR");
  }
}

static void onDataRecv(const esp_now_recv_info_t* info, const uint8_t* incomingData, int len) {
  const uint8_t* mac = info->src_addr;

  if (len >= 4 && incomingData[0] == 'R' && incomingData[1] == 'F') {
    uint8_t type = incomingData[3];

    if (type == PKT_BATTERY_STATUS && len == sizeof(BatteryTelemetryPacketV1)) {
      BatteryTelemetryPacketV1 pkt;
      memcpy(&pkt, incomingData, sizeof(pkt));
      updateBatteryTelemetryState(mac, pkt);
      return;
    }

    if (type == PKT_TOE_STATUS && len == sizeof(ToeTelemetryPacketV1)) {
      ToeTelemetryPacketV1 pkt;
      memcpy(&pkt, incomingData, sizeof(pkt));
      updateToeTelemetryState(mac, pkt);
      return;
    }

    if (type == PKT_FW_INFO && len == sizeof(FirmwareInfoPacketV1)) {
      FirmwareInfoPacketV1 pkt;
      memcpy(&pkt, incomingData, sizeof(pkt));
      updateFwInfoState(mac, pkt);
      return;
    }

    if (type == PKT_CONFIG_ACK && len == sizeof(ConfigAckPacketV1)) {
      ConfigAckPacketV1 ack;
      memcpy(&ack, incomingData, sizeof(ack));
      applyAckToNode(mac, ack);
      return;
    }

    if (type == PKT_OTA_STATUS && len == sizeof(OtaStatusPacket)) {
      OtaStatusPacket pkt;
      memcpy(&pkt, incomingData, sizeof(pkt));
      applyOtaStatusToNode(mac, pkt);
      return;
    }

    if (type == PKT_OTA_RESULT && len == sizeof(OtaResultPacket)) {
      OtaResultPacket pkt;
      memcpy(&pkt, incomingData, sizeof(pkt));
      applyOtaResultToNode(mac, pkt);
      return;
    }
  }

  if (len == sizeof(TelemetryPacket)) {
    TelemetryPacket pkt;
    memcpy(&pkt, incomingData, sizeof(pkt));
    updateTelemetryState(mac, pkt);
  }
}

void setupEspNow() {
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_send_cb(onDataSent);
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.println("ESP-NOW ready");
}