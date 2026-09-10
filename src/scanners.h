// ============================================================
//  ECHO passive radio scanners
//
//  Everything discoverable in the air, labelled. Two sources feed one
//  table:
//
//   * WiFi -- WiFi.scanNetworks() reads the beacons every access point
//     broadcasts, including hidden ones (which leak a nameless beacon).
//     We read name, RSSI, channel, encryption, and the vendor from the
//     BSSID's manufacturer prefix.
//
//   * BLE  -- a passive scan reads the advertising packets that
//     wearables, trackers, sensors and appliances broadcast so they can
//     be found. We read address, RSSI, any name, and classify trackers
//     (AirTag / Tile / SmartTag) and vendors from the address prefix.
//
//  This is strictly what devices broadcast on purpose. It does NOT put
//  the radio in monitor mode and does NOT capture the frames of clients
//  that never chose to be discoverable -- that would be sniffing, a
//  different and invasive thing. ECHO only reads the open air.
// ============================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <cctype>
#include <string>

#define MAX_BLIPS 64
#define BLIP_TTL_MS 15000        // drop a contact unheard this long

enum BlipKind { BK_WIFI, BK_BLE };

// Coarse device class, inferred from the manufacturer prefix or the BLE
// advertisement. Drives the label on the radar.
enum DevType {
  DT_UNKNOWN, DT_ROUTER, DT_PHONE, DT_COMPUTER, DT_WEARABLE,
  DT_TRACKER, DT_CAMERA, DT_AUDIO, DT_IOT, DT_HIDDEN
};

static const char* DT_TAG[] = {
  "?", "NET", "PHON", "PC", "WEAR", "TRAK", "CAM", "AUDIO", "IOT", "HIDN"
};

struct Blip {
  BlipKind kind;
  DevType  type;
  char     name[22];
  char     vendor[10];
  int16_t  rssi;
  uint8_t  channel;
  bool     secure;
  uint16_t angle;
  uint32_t seen;
  uint32_t idHash;
};

static Blip   blips[MAX_BLIPS];
static int    blipCount = 0;
static uint32_t wifiScans = 0, bleScans = 0;

static uint32_t hashStr(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
  return h ? h : 1;
}

// ---------------- manufacturer prefixes ----------------
// A tiny curated OUI table. MAC addresses begin with a 3-byte vendor
// prefix; these are common ones, matched on the first 8 chars
// ("AA:BB:CC"). Not exhaustive -- just enough to label most blips.
struct OuiRule { const char* prefix; DevType type; const char* vendor; };
static const OuiRule OUI[] = {
  { "AC:DE:48", DT_PHONE,    "Apple"  },
  { "A4:83:E7", DT_PHONE,    "Apple"  },
  { "F0:18:98", DT_PHONE,    "Apple"  },
  { "3C:5A:B4", DT_PHONE,    "Google" },
  { "DA:A1:19", DT_PHONE,    "Google" },
  { "44:65:0D", DT_PHONE,    "Amazon" },
  { "FC:65:DE", DT_PHONE,    "Amazon" },
  { "50:DC:E7", DT_COMPUTER, "Dell"   },
  { "B8:27:EB", DT_COMPUTER, "RaspPi" },
  { "DC:A6:32", DT_COMPUTER, "RaspPi" },
  { "24:6F:28", DT_IOT,      "Esp32"  },
  { "7C:9E:BD", DT_IOT,      "Esp32"  },
  { "EC:FA:BC", DT_IOT,      "Esprsf" },
  { "18:FE:34", DT_IOT,      "Esprsf" },
  { "50:02:91", DT_IOT,      "Esprsf" },
  { "2C:AA:8E", DT_CAMERA,   "Wyze"   },
  { "7C:78:B2", DT_CAMERA,   "Wyze"   },
  { "00:62:6E", DT_CAMERA,   "Hangz"  },
  { "A4:DA:22", DT_CAMERA,   "Hangz"  },
  { "10:5A:17", DT_ROUTER,   "TP-Lnk" },
  { "50:C7:BF", DT_ROUTER,   "TP-Lnk" },
  { "C0:56:27", DT_ROUTER,   "Belkin" },
  { "2C:30:33", DT_ROUTER,   "Netgr"  },
  { "9C:3D:CF", DT_ROUTER,   "Netgr"  },
  { "00:0C:43", DT_ROUTER,   "Ralink" },
  { "5C:AA:FD", DT_AUDIO,    "Sonos"  },
  { "B8:8A:60", DT_WEARABLE, "Garmin" },
};

static bool prefixEq(const char* mac, const char* pfx) {
  for (int i = 0; i < 8; i++) {
    char a = mac[i], b = pfx[i];
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    if (a != b) return false;
  }
  return true;
}

static DevType lookupOui(const char* mac, char* vendorOut) {
  for (const OuiRule& r : OUI) {
    if (prefixEq(mac, r.prefix)) {
      strncpy(vendorOut, r.vendor, 9); vendorOut[9] = 0;
      return r.type;
    }
  }
  vendorOut[0] = 0;
  return DT_UNKNOWN;
}

// ---------------- blip table ----------------
static void blipUpsert(BlipKind kind, DevType type, uint32_t idHash,
                       const char* name, const char* vendor,
                       int rssi, uint8_t channel, bool secure) {
  for (int i = 0; i < blipCount; i++) {
    if (blips[i].idHash == idHash && blips[i].kind == kind) {
      blips[i].rssi = rssi; blips[i].seen = millis();
      if (type != DT_UNKNOWN) blips[i].type = type;
      return;
    }
  }
  if (blipCount >= MAX_BLIPS) {
    int worst = 0;
    for (int i = 1; i < blipCount; i++)
      if (blips[i].rssi < blips[worst].rssi) worst = i;
    if (rssi <= blips[worst].rssi) return;
    blips[worst] = blips[--blipCount];
  }
  Blip& b = blips[blipCount++];
  b.kind = kind; b.type = type; b.idHash = idHash;
  b.rssi = rssi; b.channel = channel; b.secure = secure;
  b.angle = idHash % 360; b.seen = millis();
  strncpy(b.name, name, sizeof(b.name) - 1); b.name[sizeof(b.name)-1] = 0;
  strncpy(b.vendor, vendor, sizeof(b.vendor) - 1); b.vendor[sizeof(b.vendor)-1] = 0;
}

static void blipExpire() {
  uint32_t now = millis();
  int w = 0;
  for (int i = 0; i < blipCount; i++)
    if (now - blips[i].seen < BLIP_TTL_MS) blips[w++] = blips[i];
  blipCount = w;
}

static int blipCountOf(BlipKind k) {
  int n = 0;
  for (int i = 0; i < blipCount; i++) if (blips[i].kind == k) n++;
  return n;
}
static int blipCountType(DevType t) {
  int n = 0;
  for (int i = 0; i < blipCount; i++) if (blips[i].type == t) n++;
  return n;
}

// ---------------- WiFi ----------------
static void wifiScanInit() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setTxPower(WIFI_POWER_8_5dBm);       // shrink the current spike
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

static void wifiScanPump() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true, true); return; }
  if (n >= 0) {
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(i);
      bool hidden = (ssid.length() == 0);
      if (hidden) ssid = "<hidden>";
      String bssid = WiFi.BSSIDstr(i);
      char vendor[10];
      DevType t = lookupOui(bssid.c_str(), vendor);
      if (hidden) t = DT_HIDDEN;
      else if (t == DT_UNKNOWN) t = DT_ROUTER;   // any AP is at least a router
      uint32_t id = hashStr((ssid + bssid).c_str());
      blipUpsert(BK_WIFI, t, id, ssid.c_str(), vendor, WiFi.RSSI(i),
                 (uint8_t)WiFi.channel(i),
                 WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();
    wifiScans++;
    WiFi.scanNetworks(true, true);
  }
}

// ---------------- BLE ----------------
static BLEScan* bleScanner = nullptr;

static void bleScanInit() {
  BLEDevice::init("");
  bleScanner = BLEDevice::getScan();
  bleScanner->setActiveScan(false);         // passive: listen, don't probe
  bleScanner->setInterval(100);
  bleScanner->setWindow(90);
}

// Recognise the common tracker tags from the advertised company ID in
// the manufacturer data. Apple = 0x004C (FindMy / AirTag), Tile = 0x00E0,
// Samsung = 0x0075 (SmartTag).
static DevType classifyBle(BLEAdvertisedDevice& d, char* vendorOut) {
  vendorOut[0] = 0;
  if (d.haveManufacturerData()) {
    std::string md = d.getManufacturerData();
    if (md.length() >= 2) {
      uint16_t cid = (uint8_t)md[0] | ((uint8_t)md[1] << 8);
      switch (cid) {
        case 0x004C: strcpy(vendorOut, "Apple");   return DT_TRACKER;
        case 0x00E0: strcpy(vendorOut, "Tile");    return DT_TRACKER;
        case 0x0075: strcpy(vendorOut, "Samsung"); return DT_TRACKER;
        case 0x0006: strcpy(vendorOut, "Msft");    return DT_COMPUTER;
        case 0x00D8: strcpy(vendorOut, "Garmin");  return DT_WEARABLE;
        default: break;
      }
    }
  }
  if (d.haveName()) {
    std::string nm = d.getName();
    for (auto& ch : nm) ch = tolower((unsigned char)ch);
    if (nm.find("watch") != std::string::npos ||
        nm.find("band")  != std::string::npos) return DT_WEARABLE;
    if (nm.find("buds") != std::string::npos ||
        nm.find("pods") != std::string::npos)  return DT_AUDIO;
    if (nm.find("cam")  != std::string::npos)  return DT_CAMERA;
  }
  return DT_IOT;
}

static void bleScanPump() {
  if (!bleScanner) return;
  BLEScanResults res = bleScanner->start(2, false);
  int n = res.getCount();
  for (int i = 0; i < n; i++) {
    BLEAdvertisedDevice d = res.getDevice(i);
    String addr = d.getAddress().toString().c_str();
    char vendor[10];
    DevType t = classifyBle(d, vendor);
    const char* nm = d.haveName() ? d.getName().c_str()
                   : (t == DT_TRACKER ? "tracker tag" : "BLE device");
    blipUpsert(BK_BLE, t, hashStr(addr.c_str()), nm, vendor,
               d.getRSSI(), 0, false);
  }
  bleScanner->clearResults();
  bleScans++;
}
