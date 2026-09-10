// ============================================================
//  NETWATCH -- T-Dongle-S3 Radio Radar
//
//  A passive WiFi + BLE scanner that shows everything discoverable in
//  the air -- far more than a phone's join/pair list -- drawn as a
//  hacker-terminal radar. Transmits nothing; only reads open broadcasts.
//  See scanners.h.
//
//  160x80 screen: the radar is a half-circle anchored bottom-centre,
//  sweeping the upper half. Signal strength sets a blip's distance from
//  the centre (strong = near). Bearing is fixed per device so contacts
//  hold still. Each blip is coloured and labelled by device type.
//
//  Controls (BOOT button):
//    tap  (< 500ms)   -> cycle filter: ALL / WIFI / BLE / TRACKERS
//    hold (>= 500ms)  -> open contact list; in list, tap = next,
//                        hold = exit
// ============================================================

#include <Arduino.h>
#include <math.h>
#include <esp_system.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "display.h"
#include "scanners.h"
#include "bootlogo.h"

#define BTN_PIN   0
#define LED_CLK  39
#define LED_DAT  40

// Radar geometry
#define CX 40
#define CY 64
#define R_MAX 58

#define RSSI_NEAR -35
#define RSSI_FAR  -95

// ---- terminal palette (phosphor green on black) ----
#define COL_BG      0x0000
#define COL_GRID    0x0300   /* dim ring green            */
#define COL_GRID2   0x0180   /* dimmer                    */
#define COL_SWEEP   0x07E0   /* bright sweep edge          */
#define COL_HDR     0x07E0   /* bright green header        */
#define COL_DIM     0x0400   /* dim green body text        */
#define COL_TEXT    0x05E0   /* mid green                  */
#define COL_PANEL   0x0140   /* panel fill                 */
#define COL_FRAME   0x04A0   /* panel border               */
#define COL_WARN    0xFB80   /* amber alert                */
#define COL_TRACK   0xF9E7   /* tracker pink-red           */

// blip colour per device type
static uint16_t typeColour(DevType t) {
  switch (t) {
    case DT_TRACKER:  return COL_TRACK;
    case DT_CAMERA:   return 0xFD20;   // orange
    case DT_HIDDEN:   return 0xF800;   // red
    case DT_PHONE:    return 0x07FF;   // cyan
    case DT_COMPUTER: return 0x5EFF;   // light blue
    case DT_WEARABLE: return 0xFFE0;   // yellow
    case DT_AUDIO:    return 0xC79F;   // violet
    case DT_ROUTER:   return 0x07E0;   // green
    case DT_IOT:      return 0x2E8B;   // teal
    default:          return 0x8410;   // grey
  }
}

enum { F_ALL, F_WIFI, F_BLE, F_TRACKERS, F_COUNT };
static const char* FILTER_NAME[F_COUNT] = { "ALL", "WIFI", "BLE", "TRACKERS" };
static int filter = F_ALL;

static bool listScreen = false;
static int  listSel = 0;

static float sweepAngle = 180;
static uint32_t lastFrame = 0, lastWifi = 0, lastBle = 0;
static bool wifiUp = false, bleUp = false, bleTried = false;
static uint32_t bootMs = 0;

// tracker-follow alert: a tracker seen strong for a while
static uint32_t trackerSince = 0;

// ---------------- LED ----------------
static void ledByte(uint8_t v) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(LED_DAT, (v >> i) & 1);
    digitalWrite(LED_CLK, HIGH);
    digitalWrite(LED_CLK, LOW);
  }
}
static void ledSet(uint8_t r, uint8_t g, uint8_t b, uint8_t bright = 3) {
  for (int i = 0; i < 4; i++) ledByte(0x00);
  ledByte(0xE0 | (bright & 0x1F));
  ledByte(b); ledByte(g); ledByte(r);
  for (int i = 0; i < 4; i++) ledByte(0xFF);
}

// ---------------- filter ----------------
static bool inFilter(const Blip& b) {
  switch (filter) {
    case F_WIFI:     return b.kind == BK_WIFI;
    case F_BLE:      return b.kind == BK_BLE;
    case F_TRACKERS: return b.type == DT_TRACKER;
    default:         return true;
  }
}

// ---------------- geometry ----------------
static float blipScreenDeg(uint16_t angle) {
  return 180.0f + (angle / 359.0f) * 180.0f;
}
static int rssiToRadius(int rssi) {
  if (rssi > RSSI_NEAR) rssi = RSSI_NEAR;
  if (rssi < RSSI_FAR)  rssi = RSSI_FAR;
  float t = (float)(RSSI_NEAR - rssi) / (RSSI_NEAR - RSSI_FAR);
  return (int)(5 + t * (R_MAX - 7));
}

// ---------------- radar screen ----------------
static void drawRadar() {
  gClear(COL_BG);

  // range rings
  for (int r = R_MAX / 3; r <= R_MAX; r += R_MAX / 3)
    for (int deg = 180; deg <= 360; deg += 4) {
      float a = deg * PI / 180.0f;
      gPixel(CX + (int)(cosf(a) * r), CY + (int)(sinf(a) * r), COL_GRID);
    }
  gLine(CX - R_MAX, CY, CX + R_MAX, CY, COL_GRID);
  gLine(CX, CY, CX, CY - R_MAX, COL_GRID2);
  gLine(CX - 42, CY - 42, CX + 42, CY - 42, COL_GRID2);

  // sweep + trail
  for (int t = 0; t < 26; t++) {
    float a = (sweepAngle - t * 1.4f) * PI / 180.0f;
    if (a < PI) continue;
    uint16_t c = (t == 0) ? COL_SWEEP : (t < 8 ? 0x04E0 : COL_GRID);
    gLine(CX, CY, CX + (int)(cosf(a) * R_MAX),
          CY + (int)(sinf(a) * R_MAX), c);
  }

  // blips
  uint32_t now = millis();
  for (int i = 0; i < blipCount; i++) {
    if (!inFilter(blips[i])) continue;
    float deg = blipScreenDeg(blips[i].angle);
    float a = deg * PI / 180.0f;
    int r = rssiToRadius(blips[i].rssi);
    int bx = CX + (int)(cosf(a) * r);
    int by = CY + (int)(sinf(a) * r);
    float behind = sweepAngle - deg;
    while (behind < 0) behind += 180;
    bool fresh = behind < 45;
    uint16_t col = typeColour(blips[i].type);
    gDisc(bx, by, fresh ? 2 : 1, fresh ? col : COL_GRID);
    if (fresh && blips[i].type == DT_TRACKER) gCircle(bx, by, 4, COL_TRACK);
    else if (fresh && (now - blips[i].seen < 2500)) gCircle(bx, by, 3, col);
  }

  // ---- right-hand readout ----
  char buf[24];
  gText(84, 1, "NETWATCH", COL_HDR);
  gLine(84, 9, 159, 9, COL_FRAME);

  snprintf(buf, sizeof(buf), "FLT %s", FILTER_NAME[filter]);
  gText(84, 12, buf, COL_WARN);

  int total = 0;
  for (int i = 0; i < blipCount; i++) if (inFilter(blips[i])) total++;
  snprintf(buf, sizeof(buf), "CONTACTS %d", total);
  gText(84, 22, buf, COL_TEXT);

  // type tally
  snprintf(buf, sizeof(buf), "NET %d  PC %d",
           blipCountType(DT_ROUTER) + blipCountType(DT_HIDDEN),
           blipCountType(DT_COMPUTER));
  gText(84, 34, buf, COL_DIM);
  snprintf(buf, sizeof(buf), "PHON %d  IOT %d",
           blipCountType(DT_PHONE), blipCountType(DT_IOT));
  gText(84, 43, buf, COL_DIM);
  int trk = blipCountType(DT_TRACKER), cam = blipCountType(DT_CAMERA);
  snprintf(buf, sizeof(buf), "TRK %d  CAM %d", trk, cam);
  gText(84, 52, buf, (trk || cam) ? COL_WARN : COL_DIM);

  // status line
  if (trackerSince && now - trackerSince > 20000)
    gText(84, 62, "TRACKER NEAR!", ((now / 350) % 2) ? COL_TRACK : COL_BG);
  else if (!wifiUp)
    gText(84, 62, "RADIO WARMUP", COL_DIM);
  else
    gText(84, 62, "SCANNING", ((now / 600) % 2) ? COL_HDR : COL_DIM);

  gText(84, 71, "TAP FLT/HOLD LST", COL_GRID);

  lcdFlush();
}

// ---------------- contact list ----------------
static void drawList() {
  gClear(COL_BG);
  gText(2, 1, "CONTACTS", COL_HDR);
  gLine(0, 9, 159, 9, COL_FRAME);

  int idx[MAX_BLIPS], n = 0;
  for (int i = 0; i < blipCount; i++) if (inFilter(blips[i])) idx[n++] = i;

  if (n == 0) {
    gTextC(80, 34, "NOTHING IN RANGE", COL_DIM);
    gText(2, 71, "HOLD:EXIT", COL_GRID);
    lcdFlush();
    return;
  }
  if (listSel >= n) listSel = 0;

  int top = listSel - 2; if (top < 0) top = 0;
  if (top > n - 5) top = (n > 5) ? n - 5 : 0;
  for (int row = 0; row < 5 && top + row < n; row++) {
    Blip& bl = blips[idx[top + row]];
    int y = 12 + row * 10;
    bool sel = (top + row == listSel);
    if (sel) gFillRect(0, y - 1, 116, 9, COL_PANEL);
    uint16_t c = typeColour(bl.type);
    gText(2, y, DT_TAG[bl.type], c);
    char nm[12];
    strncpy(nm, bl.name, 11); nm[11] = 0;
    gText(38, y, nm, sel ? C_WHITE : COL_TEXT);
  }

  // detail panel
  Blip& s = blips[idx[listSel]];
  char buf[24];
  gFillRect(118, 11, 42, 58, COL_PANEL);
  gRect(118, 11, 42, 58, COL_FRAME);
  gText(121, 13, DT_TAG[s.type], typeColour(s.type));
  if (s.vendor[0]) gText(121, 22, s.vendor, COL_TEXT);
  snprintf(buf, sizeof(buf), "%ddBm", s.rssi);
  gText(121, 31, buf, C_WHITE);
  int bars = (s.rssi + 100) / 12; if (bars < 0) bars = 0; if (bars > 5) bars = 5;
  for (int i = 0; i < 5; i++)
    gFillRect(121 + i * 5, 52 - (i + 1) * 3, 4, (i + 1) * 3,
              i < bars ? COL_HDR : COL_GRID);
  if (s.kind == BK_WIFI) {
    snprintf(buf, sizeof(buf), "CH%d", s.channel);
    gText(121, 54, buf, COL_DIM);
    gText(121, 62, s.secure ? "LOCK" : "OPEN", s.secure ? COL_TRACK : COL_HDR);
  } else {
    gText(121, 54, "BLE", COL_DIM);
  }

  snprintf(buf, sizeof(buf), "%d/%d HOLD:EXIT", listSel + 1, n);
  gText(2, 71, buf, COL_GRID);
  lcdFlush();
}

// ---------------- tracker watch ----------------
// If a tracker is currently in range and close, start (or hold) a timer.
// Sustained closeness is what the "TRACKER NEAR" alert keys off.
static void updateTrackerWatch() {
  bool near = false;
  for (int i = 0; i < blipCount; i++)
    if (blips[i].type == DT_TRACKER && blips[i].rssi > -75) { near = true; break; }
  if (near) { if (!trackerSince) trackerSince = millis(); }
  else trackerSince = 0;
}

// ---------------- button ----------------
static bool btnDown = false, btnFired = false;
static uint32_t btnStart = 0;
static const uint32_t HOLD_MS = 500;

static void onHold() { listScreen = !listScreen; listSel = 0; }
static void onTap() {
  if (listScreen) listSel++;
  else filter = (filter + 1) % F_COUNT;
}

static void pollButton() {
  bool down = (digitalRead(BTN_PIN) == LOW);
  uint32_t now = millis();
  if (down && !btnDown) { btnDown = true; btnStart = now; btnFired = false; }
  else if (down && btnDown && !btnFired && (now - btnStart >= HOLD_MS)) {
    onHold(); btnFired = true;
  } else if (!down && btnDown) {
    uint32_t held = now - btnStart;
    btnDown = false;
    if (!btnFired && held >= 40) onTap();
  }
}

// ---------------- boot ----------------
static void showBoot() {
  gClear(COL_BG);
  lcdFlush();
  const int STEP = 5;
  for (int cut = STEP; cut <= BOOT_W; cut += STEP) {
    for (int y = 0; y < BOOT_H; y++)
      for (int x = cut - STEP; x < cut && x < BOOT_W; x++)
        gPixel(x, y, BOOT_PX[y * BOOT_W + x]);
    if (cut < BOOT_W)
      for (int y = 0; y < LCD_H; y++) gPixel(cut, y, COL_SWEEP);
    lcdFlush();
    delay(12);
  }
  for (int y = 0; y < BOOT_H; y++)
    for (int x = 0; x < BOOT_W; x++)
      gPixel(x, y, BOOT_PX[y * BOOT_W + x]);

  esp_reset_reason_t rr = esp_reset_reason();
  if (rr == ESP_RST_BROWNOUT) {
    gFillRect(6, 68, 120, 9, C_BLACK); gText(8, 69, "LOW POWER RESET", C_RED);
  } else if (rr == ESP_RST_PANIC) {
    gFillRect(6, 68, 96, 9, C_BLACK); gText(8, 69, "CRASH RESET", C_ORANGE);
  }
  lcdFlush();
  delay(1400);
}

// ---------------- setup / loop ----------------
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   // ride out scan current dips

  pinMode(LED_CLK, OUTPUT);
  pinMode(LED_DAT, OUTPUT);
  ledSet(255, 0, 0);

  Serial.begin(115200);
  pinMode(BTN_PIN, INPUT_PULLUP);

  ledSet(255, 140, 0);
  lcdInit();
  ledSet(0, 255, 0);

  showBoot();
  drawRadar();                                 // a frame before any radio

  bootMs = millis();
  lastFrame = lastWifi = lastBle = bootMs;
}

void loop() {
  uint32_t now = millis();
  pollButton();

  // bring WiFi up shortly after boot so its current draw doesn't coincide
  // with startup and brown out
  if (!wifiUp && now - bootMs > 2500) { wifiScanInit(); wifiUp = true; lastWifi = now; }

  if (wifiUp && filter != F_BLE && now - lastWifi > 300) {
    wifiScanPump(); lastWifi = now;
  }

  // BLE only when its results matter (BLE / ALL / TRACKERS), started lazily
  bool wantBle = (filter == F_BLE || filter == F_ALL || filter == F_TRACKERS);
  if (wantBle) {
    if (!bleTried && now - bootMs > 3500) {
      bleScanInit(); bleUp = (bleScanner != nullptr); bleTried = true;
    }
    if (bleUp && now - lastBle > 3000) { bleScanPump(); lastBle = now; }
  }

  blipExpire();
  updateTrackerWatch();

  if (now - lastFrame >= 33) {
    lastFrame = now;
    if (!listScreen) {
      sweepAngle += 3.5f;
      if (sweepAngle >= 360) sweepAngle -= 180;
      if (sweepAngle < 180) sweepAngle = 180;
      drawRadar();
    } else {
      drawList();
    }

    // LED: pink pulse if a tracker is stalking, else green scan breathing
    if (trackerSince && now - trackerSince > 20000)
      ledSet(255, 40, 90, ((now / 250) % 2) ? 8 : 1);
    else if (blipCount == 0)
      ledSet(0, 40, 0, 1);
    else
      ledSet(0, 180, 40, ((now / 500) % 2) ? 3 : 1);
  }

  delay(2);
}
