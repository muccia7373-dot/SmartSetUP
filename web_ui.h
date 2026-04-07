#pragma once

#include "state.h"

String makeHtmlPage();
String makeJsonData();

void handleRoot();
void handleData();
void handleZero();
void handleZeroNode();
void handleSetCfg();
void handleGetCfg();
void handleAssignUnknown();
void handleOtaAssigned();
void handleOtaUnknown();
void handleBatchOtaStart();
void handleSetOptions();

void handleFirmwareInfo();
void handleFwList();
void handleFwFile();
void handleOtaMaster();

void setupWebServer();