#include <Arduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

#define COLOR_BG 0x0000
#define COLOR_TEXT 0xF800
#define COLOR_DIM 0x8800
#define COLOR_BRIGHT 0xFDA0
#define COLOR_WARN 0xFFE0

#define SCREEN_W 320
#define SCREEN_H 240

#define TOUCH_THRESHOLD 600
#define TOUCH_LEFT_ZONE 80
#define TOUCH_RIGHT_ZONE 240

enum Mode { MODE_STATS, MODE_REACTOR };
Mode currentMode = MODE_REACTOR;
bool modeChanged = true;

struct SystemStats {
  float cpu_load = 0;
  float cpu_temp = 0;
  int cpu_freq = 0;
  float cpu_pwr = 0;
  float cores[16] = {0};
  int core_count = 0;

  float ram_used = 0;
  float ram_total = 0;
  float ram_p = 0;

  float swap_used = 0;
  float swap_p = 0;

  int gpu_load = 0;
  float vram_used = 0;
  float vram_total = 0;
  int gpu_temp = 0;
  float gpu_pwr = 0;

  float disk_p = 0;
  float net_sent = 0;
  float net_recv = 0;
};
SystemStats stats;

unsigned long lastDataTime = 0;
bool isConnected = false;

void setup() {
  Serial.begin(115200);

  pinMode(0, INPUT_PULLUP);

  tft.init();
  tft.setRotation(3);
  tft.invertDisplay(true);
  tft.fillScreen(COLOR_BG);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COLOR_BRIGHT);
  tft.drawString("RBMK-1000", SCREEN_W / 2, SCREEN_H / 2 - 20, 4);
  tft.setTextColor(COLOR_TEXT);
  tft.drawString("REACTOR CORE 4", SCREEN_W / 2, SCREEN_H / 2 + 20, 2);
  delay(1500);
}

void drawLine(int y, String label, String value, uint16_t color) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COLOR_DIM, COLOR_BG);
  tft.drawString(label, 10, y, 2);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(color, COLOR_BG);
  tft.drawString(value, 310, y, 2);
}

void drawStatsScreen() {
  if (modeChanged) {
    tft.fillScreen(COLOR_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COLOR_BRIGHT, COLOR_BG);
    tft.drawString("SYSTEM MONITOR", SCREEN_W / 2, 8, 2);
    modeChanged = false;
  }

  int y = 50;
  int s = 22;

  uint16_t cpuColor = (stats.cpu_load > 80) ? COLOR_WARN : COLOR_TEXT;
  drawLine(y, "CPU",
           String((int)stats.cpu_load) + "% " + String((int)stats.cpu_temp) +
               "C",
           cpuColor);
  y += s;

  uint16_t gpuColor = (stats.gpu_load > 80) ? COLOR_WARN : COLOR_TEXT;
  drawLine(y, "GPU",
           String(stats.gpu_load) + "% " + String(stats.gpu_temp) + "C",
           gpuColor);
  y += s;

  drawLine(y, "PWR", String((int)stats.gpu_pwr) + "W", COLOR_TEXT);
  y += s;

  drawLine(y, "VRAM",
           String(stats.vram_used / 1024.0, 1) + "/" +
               String(stats.vram_total / 1024.0, 1) + "GB",
           COLOR_TEXT);
  y += s;

  uint16_t ramColor = (stats.ram_p > 85) ? COLOR_WARN : COLOR_TEXT;
  drawLine(y, "RAM",
           String(stats.ram_used, 1) + "/" + String(stats.ram_total, 1) + "GB",
           ramColor);
  y += s;

  uint16_t swapColor = (stats.swap_p > 50) ? COLOR_WARN : COLOR_TEXT;
  drawLine(y, "SWAP", String((int)stats.swap_p) + "%", swapColor);
  y += s;

  uint16_t diskColor = (stats.disk_p > 90) ? COLOR_WARN : COLOR_TEXT;
  drawLine(y, "DISK", String((int)stats.disk_p) + "%", diskColor);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(isConnected ? COLOR_TEXT : COLOR_WARN, COLOR_BG);
  tft.drawString(isConnected ? "ONLINE" : "OFFLINE", SCREEN_W / 2, SCREEN_H - 8,
                 2);
}

uint16_t heatColor(float load) {
  if (load < 20)
    return 0x2104;
  if (load < 40)
    return COLOR_DIM;
  if (load < 60)
    return COLOR_TEXT;
  if (load < 80)
    return COLOR_BRIGHT;
  return COLOR_WARN;
}

void drawReactorScreen() {
  if (modeChanged) {
    tft.fillScreen(COLOR_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(COLOR_BRIGHT, COLOR_BG);
    tft.drawString("REACTOR CORE 4", SCREEN_W / 2, 5, 2);
    modeChanged = false;
  }

  int cellW = 35;
  int cellH = 35;
  int startX = 40;
  int startY = 30;
  int gap = 5;

  for (int row = 0; row < 4; row++) {
    for (int col = 0; col < 4; col++) {
      int idx = row * 4 + col;
      int x = startX + col * (cellW + gap);
      int y = startY + row * (cellH + gap);

      float load = (idx < stats.core_count) ? stats.cores[idx] : 0;
      uint16_t color = heatColor(load);

      tft.fillRect(x, y, cellW, cellH, color);
      tft.drawRect(x, y, cellW, cellH, COLOR_BG);

      tft.setTextDatum(MC_DATUM);
      tft.setTextColor(COLOR_BG, color);
      tft.drawString(String(idx), x + cellW / 2, y + cellH / 2 - 5, 2);
      tft.setTextColor(COLOR_BG, color);
      tft.drawString(String((int)load) + "%", x + cellW / 2, y + cellH / 2 + 7,
                     1);
    }
  }

  int boxX = 220;
  int boxY = 30;
  int boxW = 90;
  int boxH = 35;

  tft.fillRect(boxX, boxY, boxW, boxH, heatColor(stats.gpu_load));
  tft.drawRect(boxX, boxY, boxW, boxH, COLOR_BG);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COLOR_BG);
  tft.drawString("GPU", boxX + boxW / 2, boxY + 10, 2);
  tft.drawString(String(stats.gpu_load) + "%", boxX + boxW / 2, boxY + 24, 1);

  boxY += boxH + gap;

  float vramP =
      (stats.vram_total > 0) ? (stats.vram_used / stats.vram_total * 100) : 0;
  tft.fillRect(boxX, boxY, boxW, boxH, heatColor(vramP));
  tft.drawRect(boxX, boxY, boxW, boxH, COLOR_BG);
  tft.setTextColor(COLOR_BG);
  tft.drawString("VRAM", boxX + boxW / 2, boxY + 10, 2);
  tft.drawString(String((int)vramP) + "%", boxX + boxW / 2, boxY + 24, 1);

  boxY += boxH + gap;

  tft.fillRect(boxX, boxY, boxW, boxH, heatColor(stats.ram_p));
  tft.drawRect(boxX, boxY, boxW, boxH, COLOR_BG);
  tft.setTextColor(COLOR_BG);
  tft.drawString("RAM", boxX + boxW / 2, boxY + 10, 2);
  tft.drawString(String((int)stats.ram_p) + "%", boxX + boxW / 2, boxY + 24, 1);

  boxY += boxH + gap;

  tft.fillRect(boxX, boxY, boxW, boxH, heatColor(stats.swap_p));
  tft.drawRect(boxX, boxY, boxW, boxH, COLOR_BG);
  tft.setTextColor(COLOR_BG);
  tft.drawString("SWAP", boxX + boxW / 2, boxY + 10, 2);
  tft.drawString(String((int)stats.swap_p) + "%", boxX + boxW / 2, boxY + 24,
                 1);

  int powerY = startY + 4 * (cellH + gap) + 6;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COLOR_TEXT, COLOR_BG);
  tft.drawString("CPU:" + String((int)stats.cpu_pwr) + "W", startX, powerY, 1);
  tft.drawString("GPU:" + String((int)stats.gpu_pwr) + "W", startX + 80, powerY,
                 1);

  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(isConnected ? COLOR_TEXT : COLOR_WARN, COLOR_BG);
  tft.drawString(isConnected ? "ONLINE" : "OFFLINE", SCREEN_W / 2, SCREEN_H - 5,
                 1);
}

void loop() {
  static bool lastBtn = HIGH;
  bool btn = digitalRead(0);
  if (btn == LOW && lastBtn == HIGH) {
    currentMode = (currentMode == MODE_STATS) ? MODE_REACTOR : MODE_STATS;
    modeChanged = true;
    delay(300);
  }
  lastBtn = btn;

  uint16_t touchX, touchY;
  if (tft.getTouch(&touchX, &touchY, TOUCH_THRESHOLD)) {
    if (touchX < TOUCH_LEFT_ZONE) {
      if (currentMode != MODE_STATS) {
        currentMode = MODE_STATS;
        modeChanged = true;
        delay(300);
      }
    } else if (touchX > TOUCH_RIGHT_ZONE) {
      if (currentMode != MODE_REACTOR) {
        currentMode = MODE_REACTOR;
        modeChanged = true;
        delay(300);
      }
    }
  }

  if (Serial.available()) {
    String line = Serial.readStringUntil('\n');
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, line);

    if (!error) {
      lastDataTime = millis();
      isConnected = true;

      stats.cpu_load = doc["cpu"]["load"];
      stats.cpu_temp = doc["cpu"]["temp"];
      stats.cpu_freq = doc["cpu"]["freq"];
      stats.cpu_pwr = doc["cpu"]["pwr"];

      JsonArray cores = doc["cpu"]["cores"];
      stats.core_count = min((int)cores.size(), 16);
      for (int i = 0; i < stats.core_count; i++) {
        stats.cores[i] = cores[i];
      }

      stats.ram_used = doc["ram"]["used"];
      stats.ram_total = doc["ram"]["total"];
      stats.ram_p = doc["ram"]["p"];

      stats.swap_used = doc["swap"]["used"];
      stats.swap_p = doc["swap"]["p"];

      stats.gpu_load = doc["gpu"]["gpu_load"];
      stats.vram_used = doc["gpu"]["vram_used"];
      stats.vram_total = doc["gpu"]["vram_total"];
      stats.gpu_temp = doc["gpu"]["gpu_temp"];
      stats.gpu_pwr = doc["gpu"]["gpu_pwr"];

      stats.disk_p = doc["disk"]["p"];
      stats.net_sent = doc["net"]["sent"];
      stats.net_recv = doc["net"]["recv"];
    }
  }

  if (millis() - lastDataTime > 3000) {
    isConnected = false;
  }

  if (currentMode == MODE_STATS) {
    drawStatsScreen();
  } else {
    drawReactorScreen();
  }

  delay(50);
}
