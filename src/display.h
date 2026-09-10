// ============================================================
//  Minimal ST7735 driver for the LilyGO T-Dongle-S3
//
//  Written to replace TFT_eSPI, which hangs in init() on this
//  board with recent ESP32 Arduino cores. This talks to the panel
//  directly over hardware SPI and renders into a RAM framebuffer.
//
//  Framebuffer is stored big-endian so the whole thing can be
//  shipped to the panel with one bulk SPI write per frame.
// ============================================================

#pragma once
#include <Arduino.h>
#include <SPI.h>
#include "font5x7.h"

#define LCD_W 160
#define LCD_H 80

// T-Dongle-S3 LCD pins
#define PIN_SCLK  5
#define PIN_MOSI  3
#define PIN_CS    4
#define PIN_DC    2
#define PIN_RST   1
#define PIN_BL   38

// Backlight is active LOW on this board.
#define BL_ON  LOW
#define BL_OFF HIGH

// Landscape. 0x60 = MV|MX, 0x08 = BGR colour order (this panel needs it;
// without it red and blue come out swapped). Try 0xA8 if the image is
// upside down.
#define LCD_MADCTL 0x68
// The 0.96" 80x160 panel doesn't start at RAM origin.
#define LCD_XOFF 1
#define LCD_YOFF 26

// 16-bit colours
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_RED     0xF800
#define C_GREEN   0x07E0
#define C_BLUE    0x001F
#define C_YELLOW  0xFFE0
#define C_CYAN    0x07FF
#define C_MAGENTA 0xF81F
#define C_GREY    0x8410
#define C_DGREY   0x4208
#define C_NAVY    0x000F
#define C_ORANGE  0xFD20

static SPIClass spiLCD(FSPI);
static uint8_t  FB[LCD_W * LCD_H * 2];

// ---------------- low level ----------------
static inline void lcdCmd(uint8_t c) {
  digitalWrite(PIN_DC, LOW);
  digitalWrite(PIN_CS, LOW);
  spiLCD.transfer(c);
  digitalWrite(PIN_CS, HIGH);
}

static inline void lcdData(const uint8_t* d, size_t n) {
  if (!n) return;
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  for (size_t i = 0; i < n; i++) spiLCD.transfer(d[i]);
  digitalWrite(PIN_CS, HIGH);
}

static inline void lcdCmdData(uint8_t c, const uint8_t* d, size_t n) {
  lcdCmd(c);
  lcdData(d, n);
}

static void lcdSetWindow(int x0, int y0, int x1, int y1) {
  x0 += LCD_XOFF; x1 += LCD_XOFF;
  y0 += LCD_YOFF; y1 += LCD_YOFF;
  uint8_t ca[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
  uint8_t ra[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };
  lcdCmdData(0x2A, ca, 4);
  lcdCmdData(0x2B, ra, 4);
  lcdCmd(0x2C);
}

static void lcdInit() {
  pinMode(PIN_CS,  OUTPUT); digitalWrite(PIN_CS,  HIGH);
  pinMode(PIN_DC,  OUTPUT); digitalWrite(PIN_DC,  HIGH);
  pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, HIGH);
  pinMode(PIN_BL,  OUTPUT); digitalWrite(PIN_BL,  BL_ON);

  spiLCD.begin(PIN_SCLK, -1, PIN_MOSI, -1);
  spiLCD.setFrequency(27000000);
  spiLCD.setDataMode(SPI_MODE0);
  spiLCD.setBitOrder(MSBFIRST);

  digitalWrite(PIN_RST, HIGH); delay(50);
  digitalWrite(PIN_RST, LOW);  delay(50);
  digitalWrite(PIN_RST, HIGH); delay(150);

  lcdCmd(0x01); delay(150);                       // SWRESET
  lcdCmd(0x11); delay(255);                       // SLPOUT

  const uint8_t frm[]  = { 0x01, 0x2C, 0x2D };
  lcdCmdData(0xB1, frm, 3);
  lcdCmdData(0xB2, frm, 3);
  const uint8_t frm3[] = { 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D };
  lcdCmdData(0xB3, frm3, 6);

  const uint8_t inv[] = { 0x07 };               lcdCmdData(0xB4, inv, 1);
  const uint8_t p1[]  = { 0xA2, 0x02, 0x84 };   lcdCmdData(0xC0, p1, 3);
  const uint8_t p2[]  = { 0xC5 };               lcdCmdData(0xC1, p2, 1);
  const uint8_t p3[]  = { 0x0A, 0x00 };         lcdCmdData(0xC2, p3, 2);
  const uint8_t p4[]  = { 0x8A, 0x2A };         lcdCmdData(0xC3, p4, 2);
  const uint8_t p5[]  = { 0x8A, 0xEE };         lcdCmdData(0xC4, p5, 2);
  const uint8_t vm[]  = { 0x0E };               lcdCmdData(0xC5, vm, 1);

  lcdCmd(0x21);                                   // INVON

  const uint8_t mad[] = { LCD_MADCTL };         lcdCmdData(0x36, mad, 1);
  const uint8_t cm[]  = { 0x05 };               lcdCmdData(0x3A, cm, 1);  // 16-bit

  lcdCmd(0x13); delay(10);                        // NORON
  lcdCmd(0x29); delay(100);                       // DISPON
}

// Ship the whole framebuffer in bulk chunks.
static void lcdFlush() {
  lcdSetWindow(0, 0, LCD_W - 1, LCD_H - 1);
  digitalWrite(PIN_DC, HIGH);
  digitalWrite(PIN_CS, LOW);
  const size_t total = sizeof(FB);
  size_t sent = 0;
  while (sent < total) {
    size_t chunk = total - sent;
    if (chunk > 4092) chunk = 4092;
    spiLCD.writeBytes(FB + sent, chunk);
    sent += chunk;
  }
  digitalWrite(PIN_CS, HIGH);
}

// ---------------- drawing into the framebuffer ----------------
static inline void gPixel(int x, int y, uint16_t c) {
  if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return;
  size_t i = ((size_t)y * LCD_W + x) * 2;
  FB[i]     = (uint8_t)(c >> 8);
  FB[i + 1] = (uint8_t)c;
}

static void gClear(uint16_t c) {
  uint8_t hi = c >> 8, lo = c & 0xFF;
  for (size_t i = 0; i < sizeof(FB); i += 2) { FB[i] = hi; FB[i + 1] = lo; }
}

static void gFillRect(int x, int y, int w, int h, uint16_t c) {
  for (int j = 0; j < h; j++)
    for (int i = 0; i < w; i++) gPixel(x + i, y + j, c);
}

static void gRect(int x, int y, int w, int h, uint16_t c) {
  for (int i = 0; i < w; i++) { gPixel(x + i, y, c); gPixel(x + i, y + h - 1, c); }
  for (int j = 0; j < h; j++) { gPixel(x, y + j, c); gPixel(x + w - 1, y + j, c); }
}

// Text is uppercase-only; the font has no lowercase glyphs.
static int gCharW(int scale) { return (FONT_W + 1) * scale; }

static void gChar(int x, int y, char ch, uint16_t c, int scale) {
  if (ch >= 'a' && ch <= 'z') ch -= 32;
  if (ch < 32 || ch > 126) ch = '?';
  const uint8_t* g = FONT5X7[ch - 32];
  for (int col = 0; col < FONT_W; col++) {
    uint8_t bits = g[col];
    for (int row = 0; row < FONT_H; row++) {
      if (!(bits & (1 << row))) continue;
      if (scale == 1) gPixel(x + col, y + row, c);
      else gFillRect(x + col * scale, y + row * scale, scale, scale, c);
    }
  }
}

static void gText(int x, int y, const char* s, uint16_t c, int scale = 1) {
  int cx = x;
  for (const char* p = s; *p; p++) { gChar(cx, y, *p, c, scale); cx += gCharW(scale); }
}

static int gTextW(const char* s, int scale = 1) {
  int n = 0; for (const char* p = s; *p; p++) n++;
  return n * gCharW(scale);
}

static void gTextC(int cx, int y, const char* s, uint16_t c, int scale = 1) {
  gText(cx - gTextW(s, scale) / 2, y, s, c, scale);
}

// ---- extra primitives for the radar ----
static void gLine(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    gPixel(x0, y0, c);
    if (x0 == x1 && y0 == y1) break;
    int e2 = 2 * err;
    if (e2 >= dy) { err += dy; x0 += sx; }
    if (e2 <= dx) { err += dx; y0 += sy; }
  }
}

// Midpoint circle outline.
static void gCircle(int cx, int cy, int r, uint16_t c) {
  int x = r, y = 0, err = 1 - r;
  while (x >= y) {
    gPixel(cx + x, cy + y, c); gPixel(cx - x, cy + y, c);
    gPixel(cx + x, cy - y, c); gPixel(cx - x, cy - y, c);
    gPixel(cx + y, cy + x, c); gPixel(cx - y, cy + x, c);
    gPixel(cx + y, cy - x, c); gPixel(cx - y, cy - x, c);
    y++;
    if (err < 0) err += 2 * y + 1;
    else { x--; err += 2 * (y - x) + 1; }
  }
}

// Filled disc, for blips.
static void gDisc(int cx, int cy, int r, uint16_t c) {
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++)
      if (dx * dx + dy * dy <= r * r) gPixel(cx + dx, cy + dy, c);
}
