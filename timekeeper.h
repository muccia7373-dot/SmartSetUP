
#pragma once

#include "config.h"

void rf73TimeBegin();
void rf73TimeTick();
bool rf73TimeNow(struct tm& outTm, bool& fromNtp);
String rf73TimeText();
String rf73DateText();
String rf73DateTimeText();
bool rf73TimeIsSynced();
