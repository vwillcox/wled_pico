#pragma once

#include <Arduino.h>

// Colon-free, lowercase MAC address - the same "escapedMac" shape real WLED
// uses for info.mac and its mDNS TXT record. Real WLED tools (Home
// Assistant's WLED integration in particular) use this as the device's
// stable identity, so json_api.cpp and main.cpp's mDNS setup both need the
// same value - hence a shared helper instead of two copies.
String device_mac();
