#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

void connectWiFi();
void startConfigPortal(const String& customPrompt = "");
String getSavedServerIP();
void saveServerIP(const String& ip);

#endif
