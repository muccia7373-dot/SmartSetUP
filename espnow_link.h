#pragma once

#include "state.h"

void onSend(const esp_now_send_info_t *tx_info, esp_now_send_status_t status);
void onReceive(const esp_now_recv_info_t *info, const uint8_t *incomingData, int len);
void setupEspNow();