#include "device_id.h"

#include <WiFi.h>

String device_mac() {
  String mac = (WiFi.status() == WL_CONNECTED) ? WiFi.macAddress() : WiFi.softAPmacAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  return mac;
}
