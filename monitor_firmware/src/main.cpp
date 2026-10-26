#include <Arduino.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>

// Single-screen system dashboard for the CYD (ESP32-2432S028R).
// The host streams one JSON line per second over USB serial. Each frame is
// rendered into a 4-bit palette sprite (38KB, full screen) and pushed at once,
// so there is no flicker and the palette can use exact RGB565 neon colors.

#define SCREEN_W 320
#define SCREEN_H 240
#define HIST_LEN 150 // sparkline samples: 1 px per sample, 1 sample per update
#define LINK_TIMEOUT_MS 3000
#define LINE_MAX 2048

TFT_eSPI tft;
TFT_eSprite spr(&tft);

// Palette indices used as colors when drawing into the 4-bit sprite.
enum {
  C_BG,
  C_GRID,   // frames, chart guides
  C_OFF,    // unlit bar segments
  C_DIM,    // labels
  C_CYAN,   // primary values
  C_CYANDK, // chart fill
  C_MAG,    // secondary accent
  C_MAGDK,  // chart fill
  C_YEL,    // warning
  C_RED,    // critical
  C_GREEN,  // link ok
  C_WHITE,
};

const uint16_t PALETTE[16] = {
    0x0000, 0x018A, 0x10C5, 0x1498, 0x07FF, 0x020E, 0xF81F, 0x600C,
    0xFFE0, 0xF904, 0x3FE2, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000,
};

struct History {
  float v[HIST_LEN];
  int head = 0;
  int count = 0;

  void push(float x) {
    v[head] = x;
    head = (head + 1) % HIST_LEN;
    if (count < HIST_LEN)
      count++;
  }
  // i = 0 is the oldest stored sample
  float at(int i) const { return v[(head - count + i + HIST_LEN) % HIST_LEN]; }
  float peak() const {
    float m = 0;
    for (int i = 0; i < count; i++)
      m = max(m, at(i));
    return m;
  }
};

struct Stats {
  char host[24] = "";
  char clock[9] = "--:--:--";
  uint32_t uptime = 0;

  char cpuName[24] = "";
  float cpuLoad = 0, cpuTemp = 0, cpuFreq = 0, cpuPwr = -1; // -1 = n/a
  float cores[16] = {0};
  int coreCount = 0;
  int pThreads = 0; // hybrid CPUs: first N threads are P-cores, 0 = uniform
  float loadAvg[3] = {0};
  int procs = 0;

  bool gpuOk = false;
  char gpuName[24] = "";
  float gpuLoad = 0, gpuTemp = 0, gpuPwr = 0, gpuPwrLim = 0;
  int gpuFan = -1; // -1 = not reported
  int gpuClk = 0, memClk = 0;
  int pcieGen = 0, pcieWidth = 0;
  float vramUsed = 0, vramTotal = 0; // MB

  float ramUsed = 0, ramTotal = 0;   // GB
  float swapUsed = 0, swapTotal = 0; // GB
  float diskUsed = 0, diskTotal = 0; // GB, root filesystem
  float nvmeTemp = 0;

  char iface[16] = "";
  float netRx = 0, netTx = 0;   // bytes/s
  float diskRd = 0, diskWr = 0; // bytes/s
};

Stats s;
History hCpu, hGpu, hRx, hTx, hRd, hWr;
StaticJsonDocument<3072> doc;

char lineBuf[LINE_MAX];
size_t lineLen = 0;
bool lineOverflow = false;

bool everConnected = false;
bool isConnected = false;
unsigned long lastDataTime = 0;

// ---------- formatting / color helpers ----------

uint8_t levelColor(float v, float warn, float crit) {
  if (v >= crit)
    return C_RED;
  if (v >= warn)
    return C_YEL;
  return C_CYAN;
}

float pct(float used, float total) {
  return total > 0 ? used / total * 100.0f : 0;
}

// Binary units, 3 significant digits max so values fit narrow columns.
void fmtRate(char *out, size_t n, float bps) {
  if (bps >= 1073741824.0f)
    snprintf(out, n, "%.2fG/s", bps / 1073741824.0f);
  else if (bps >= 104857600.0f)
    snprintf(out, n, "%.0fM/s", bps / 1048576.0f);
  else if (bps >= 1048576.0f)
    snprintf(out, n, "%.1fM/s", bps / 1048576.0f);
  else if (bps >= 1024.0f)
    snprintf(out, n, "%.0fK/s", bps / 1024.0f);
  else
    snprintf(out, n, "%.0fB/s", bps);
}

void fmtGB(char *out, size_t n, float used, float total) {
  if (total >= 100)
    snprintf(out, n, "%.0f/%.0fG", used, total);
  else
    snprintf(out, n, "%.1f/%.1fG", used, total);
}

void fmtUptime(char *out, size_t n, uint32_t sec) {
  uint32_t d = sec / 86400, h = sec / 3600 % 24, m = sec / 60 % 60;
  if (d > 0)
    snprintf(out, n, "UP %lud%02luh", d, h);
  else
    snprintf(out, n, "UP %luh%02lum", h, m);
}

// ---------- drawing primitives ----------

void text(const char *str, int x, int y, uint8_t color,
          uint8_t datum = TL_DATUM, uint8_t font = 1) {
  spr.setTextDatum(datum);
  spr.setTextColor(color);
  spr.drawString(str, x, y, font);
}

// Label on the left, value right-aligned at x + w.
void row(const char *label, const char *value, int x, int y, int w,
         uint8_t color) {
  text(label, x, y, C_DIM);
  text(value, x + w, y, color, TR_DATUM);
}

// Segmented bar: 3 px segments with 1 px gaps.
void segBar(int x, int y, int w, int h, float p, uint8_t color) {
  int segs = (w + 1) / 4;
  int lit = constrain((int)(p / 100.0f * segs + 0.5f), 0, segs);
  for (int i = 0; i < segs; i++)
    spr.fillRect(x + i * 4, y, 3, h, i < lit ? color : (uint8_t)C_OFF);
}

// Frame with an angled title tab and a chamfered bottom-right corner.
void panel(int x, int y, int w, int h, const char *title, uint8_t accent,
           const char *sub) {
  int tw = strlen(title) * 6 + 7;
  int r = x + w - 1, b = y + h - 1;
  spr.fillRect(x, y, tw, 10, accent);
  spr.fillTriangle(x + tw, y, x + tw, y + 9, x + tw + 9, y + 9, accent);
  text(title, x + 4, y + 1, C_BG);
  if (sub && *sub)
    text(sub, x + tw + 12, y + 1, C_DIM);

  spr.drawFastHLine(x + tw + 9, y + 9, w - tw - 9, C_GRID);
  spr.drawFastVLine(x, y + 10, h - 10, C_GRID);
  spr.drawFastVLine(r, y + 9, h - 15, C_GRID);
  spr.drawFastHLine(x, b, w - 6, C_GRID);
  spr.drawLine(r - 6, b, r, b - 6, C_GRID);
}

// Area chart of history `a` (newest at the right edge), optional line `b`
// overlaid. `scale` maps to full height.
void sparkline(const History &a, const History *b, float scale, int x, int y,
               int h, uint8_t aLine, uint8_t aFill, uint8_t bLine) {
  for (int i = 0; i < HIST_LEN; i += 3)
    spr.drawPixel(x + i, y + h / 2, C_GRID);
  spr.drawFastHLine(x, y + h - 1, HIST_LEN, C_GRID);
  if (scale <= 0)
    return;

  int x0 = x + HIST_LEN - a.count;
  for (int i = 0; i < a.count; i++) {
    int ph = constrain((int)(a.at(i) / scale * (h - 1) + 0.5f), 0, h - 1);
    spr.drawFastVLine(x0 + i, y + h - ph, ph, aFill);
    spr.drawPixel(x0 + i, y + h - 1 - ph, aLine);
  }
  if (!b)
    return;
  x0 = x + HIST_LEN - b->count;
  for (int i = 0; i < b->count; i++) {
    int ph = constrain((int)(b->at(i) / scale * (h - 1) + 0.5f), 0, h - 1);
    spr.drawPixel(x0 + i, y + h - 1 - ph, bLine);
  }
}

// Large percentage readout: font 4 digits + small "%".
void bigPercent(float v, int x, int y, uint8_t color) {
  char buf[8];
  snprintf(buf, sizeof buf, "%d", (int)(v + 0.5f));
  text(buf, x, y, color, TL_DATUM, 4);
  text("%", x + spr.textWidth(buf, 4) + 2, y + 9, color, TL_DATUM, 2);
}

// ---------- screen sections ----------

void drawHeader() {
  char buf[24];
  int i = 0;
  for (; s.host[i] && i < (int)sizeof buf - 1; i++)
    buf[i] = toupper(s.host[i]);
  buf[i] = 0;
  if (!*buf)
    strcpy(buf, "HOST");

  int tw = strlen(buf) * 6 + 8;
  spr.fillRect(0, 0, tw, 11, C_MAG);
  spr.fillTriangle(tw, 0, tw, 10, tw + 10, 10, C_MAG);
  text(buf, 4, 2, C_BG);
  spr.drawFastHLine(tw + 10, 10, SCREEN_W - tw - 10, C_GRID);

  text(s.clock, SCREEN_W / 2, 1, C_WHITE, TC_DATUM);

  bool ok = isConnected;
  uint8_t linkColor = ok ? C_GREEN : C_RED;
  text(ok ? "LINK" : "LOST", SCREEN_W - 1, 1, linkColor, TR_DATUM);
  spr.fillRect(SCREEN_W - 33, 2, 5, 5, linkColor);
  fmtUptime(buf, sizeof buf, s.uptime);
  text(buf, SCREEN_W - 40, 1, C_DIM, TR_DATUM);
}

void drawCpu(int px, int py, int w, int h) {
  char buf[32];
  snprintf(buf, sizeof buf, "%s %dT", s.cpuName, s.coreCount);
  panel(px, py, w, h, "CPU", C_CYAN, buf);

  bigPercent(s.cpuLoad, px + 5, py + 15, levelColor(s.cpuLoad, 70, 90));
  int sx = px + 70, sw = w - 75;
  snprintf(buf, sizeof buf, "%.0fC", s.cpuTemp);
  row("TEMP", buf, sx, py + 15, sw, levelColor(s.cpuTemp, 80, 90));
  snprintf(buf, sizeof buf, "%.2fGHz", s.cpuFreq / 1000.0f);
  row("FREQ", buf, sx, py + 25, sw, C_WHITE);
  if (s.cpuPwr >= 0)
    snprintf(buf, sizeof buf, "%.1fW", s.cpuPwr);
  else
    strcpy(buf, "--");
  row("PKG", buf, sx, py + 35, sw, C_WHITE);

  snprintf(buf, sizeof buf, "%.1f %.1f %.1f", s.loadAvg[0], s.loadAvg[1],
           s.loadAvg[2]);
  text("LA", px + 5, py + 45, C_DIM);
  text(buf, px + 23, py + 45, C_WHITE);
  snprintf(buf, sizeof buf, "%d PROC", s.procs);
  text(buf, px + w - 5, py + 45, C_DIM, TR_DATUM);

  // Per-thread load bars; hybrid CPUs get a gap between P and E groups.
  int n = s.coreCount;
  if (n > 0) {
    bool hybrid = s.pThreads > 0 && s.pThreads < n;
    int gap = hybrid ? 4 : 0;
    int pitch = (w - 10 - gap) / n;
    int top = py + 56, bh = 26;
    for (int i = 0; i < n; i++) {
      int bx = px + 5 + i * pitch + (hybrid && i >= s.pThreads ? gap : 0);
      int fill = constrain((int)(s.cores[i] * bh / 100.0f + 0.5f), 0, bh);
      spr.fillRect(bx, top, pitch - 1, bh, C_OFF);
      spr.fillRect(bx, top + bh - fill, pitch - 1, fill,
                   levelColor(s.cores[i], 70, 90));
    }
    int ly = top + bh + 3;
    if (hybrid) {
      int pEnd = px + 5 + s.pThreads * pitch;
      snprintf(buf, sizeof buf, "P-CORE 0-%d", s.pThreads - 1);
      text(buf, (px + 5 + pEnd) / 2, ly, C_DIM, TC_DATUM);
      text("E-CORE", (pEnd + gap + px + 5 + n * pitch + gap) / 2, ly, C_DIM,
           TC_DATUM);
    } else {
      snprintf(buf, sizeof buf, "THREAD 0-%d", n - 1);
      text(buf, px + w / 2, ly, C_DIM, TC_DATUM);
    }
  }

  sparkline(hCpu, nullptr, 100, px + 4, py + 96, 20, C_CYAN, C_CYANDK, 0);
}

void drawGpu(int px, int py, int w, int h) {
  char buf[32];
  panel(px, py, w, h, "GPU", C_MAG, s.gpuOk ? s.gpuName : "");
  if (!s.gpuOk) {
    text("NO NVIDIA GPU", px + w / 2, py + h / 2, C_DIM, MC_DATUM);
    return;
  }

  bigPercent(s.gpuLoad, px + 5, py + 15, levelColor(s.gpuLoad, 70, 90));
  int sx = px + 70, sw = w - 75;
  snprintf(buf, sizeof buf, "%.0fC", s.gpuTemp);
  row("TEMP", buf, sx, py + 15, sw, levelColor(s.gpuTemp, 75, 85));
  if (s.gpuFan >= 0)
    snprintf(buf, sizeof buf, "%d%%", s.gpuFan);
  else
    strcpy(buf, "--");
  row("FAN", buf, sx, py + 25, sw, C_WHITE);
  snprintf(buf, sizeof buf, "G%dx%d", s.pcieGen, s.pcieWidth);
  row("PCIE", buf, sx, py + 35, sw, C_WHITE);

  snprintf(buf, sizeof buf, "CORE %dMHz", s.gpuClk);
  text(buf, px + 5, py + 45, C_WHITE);
  snprintf(buf, sizeof buf, "MEM %dMHz", s.memClk);
  text(buf, px + w - 5, py + 45, C_WHITE, TR_DATUM);

  float vp = pct(s.vramUsed, s.vramTotal);
  snprintf(buf, sizeof buf, "%.1f/%.1fG %d%%", s.vramUsed / 1024.0f,
           s.vramTotal / 1024.0f, (int)(vp + 0.5f));
  row("VRAM", buf, px + 5, py + 56, w - 10, C_WHITE);
  segBar(px + 4, py + 66, 150, 4, vp, levelColor(vp, 80, 95));

  float pp = pct(s.gpuPwr, s.gpuPwrLim);
  snprintf(buf, sizeof buf, "%.0f/%.0fW %d%%", s.gpuPwr, s.gpuPwrLim,
           (int)(pp + 0.5f));
  row("POWER", buf, px + 5, py + 74, w - 10, C_WHITE);
  segBar(px + 4, py + 84, 150, 4, pp, levelColor(pp, 85, 98));

  sparkline(hGpu, nullptr, 100, px + 4, py + 96, 20, C_MAG, C_MAGDK, 0);
}

void drawMemory(int px, int py, int w, int h) {
  char buf[24];
  panel(px, py, w, h, "MEM", C_CYAN, nullptr);

  struct {
    const char *label;
    float used, total, warn, crit;
  } rows[] = {
      {"RAM", s.ramUsed, s.ramTotal, 75, 90},
      {"SWAP", s.swapUsed, s.swapTotal, 25, 60},
      {"ROOT", s.diskUsed, s.diskTotal, 85, 95},
  };

  for (int i = 0; i < 3; i++) {
    int y = py + 13 + i * 10;
    float p = pct(rows[i].used, rows[i].total);
    uint8_t c = levelColor(p, rows[i].warn, rows[i].crit);
    text(rows[i].label, px + 5, y, C_DIM);
    segBar(px + 36, y, 180, 7, p, c);
    if (rows[i].total <= 0) {
      text("OFF", px + w - 5, y, C_DIM, TR_DATUM);
      continue;
    }
    snprintf(buf, sizeof buf, "%d%%", (int)(p + 0.5f));
    text(buf, px + w - 5, y, c, TR_DATUM);
    fmtGB(buf, sizeof buf, rows[i].used, rows[i].total);
    text(buf, px + w - 33, y, C_WHITE, TR_DATUM);
  }
}

// Shared layout for NET and DISK I/O: two rates on one line, dual chart below.
void drawRates(int px, int py, int w, int h, const char *title,
               const char *sub, const char *aLabel, float a, const History &ha,
               const char *bLabel, float b, const History &hb,
               float minScale) {
  char buf[16], line[24];
  panel(px, py, w, h, title, C_CYAN, sub);

  fmtRate(buf, sizeof buf, a);
  snprintf(line, sizeof line, "%s %s", aLabel, buf);
  text(line, px + 5, py + 13, C_CYAN);
  fmtRate(buf, sizeof buf, b);
  snprintf(line, sizeof line, "%s %s", bLabel, buf);
  text(line, px + w - 5, py + 13, C_MAG, TR_DATUM);

  float scale = max(max(ha.peak(), hb.peak()), minScale);
  sparkline(ha, &hb, scale, px + 4, py + 24, 25, C_CYAN, C_CYANDK, C_MAG);
}

void drawLinkBanner() {
  char buf[32];
  if (everConnected)
    snprintf(buf, sizeof buf, "LINK LOST %lus",
             (millis() - lastDataTime) / 1000);
  else
    strcpy(buf, "AWAITING HOST");

  int bw = 200, bh = 36, bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2;
  spr.fillRect(bx, by, bw, bh, C_BG);
  spr.drawRect(bx, by, bw, bh, C_RED);
  spr.drawRect(bx + 2, by + 2, bw - 4, bh - 4, C_RED);
  text(buf, SCREEN_W / 2, SCREEN_H / 2, C_RED, MC_DATUM, 2);
}

void render() {
  spr.fillSprite(C_BG);
  drawHeader();
  drawCpu(0, 16, 158, 120);
  drawGpu(162, 16, 158, 120);
  drawMemory(0, 139, 320, 44);

  char sub[24];
  drawRates(0, 186, 158, 54, "NET", s.iface, "RX", s.netRx, hRx, "TX",
            s.netTx, hTx, 64 * 1024.0f);
  snprintf(sub, sizeof sub, "NVME %.0fC", s.nvmeTemp);
  drawRates(162, 186, 158, 54, "DISK", sub, "RD", s.diskRd, hRd, "WR",
            s.diskWr, hWr, 1048576.0f);

  if (!isConnected)
    drawLinkBanner();
  spr.pushSprite(0, 0);
}

// ---------- serial protocol ----------

void copyStr(char *dst, size_t n, JsonVariantConst v) {
  strlcpy(dst, v | "", n);
}

bool parseLine(char *line) {
  if (deserializeJson(doc, line) != DeserializationError::Ok)
    return false;

  copyStr(s.host, sizeof s.host, doc["h"]);
  copyStr(s.clock, sizeof s.clock, doc["t"]);
  s.uptime = doc["up"] | 0;

  JsonObjectConst cpu = doc["cpu"];
  copyStr(s.cpuName, sizeof s.cpuName, cpu["n"]);
  s.cpuLoad = cpu["l"] | 0.0f;
  s.cpuTemp = cpu["t"] | 0.0f;
  s.cpuFreq = cpu["f"] | 0.0f;
  s.cpuPwr = cpu["w"] | -1.0f;
  s.pThreads = cpu["p"] | 0;
  s.procs = cpu["pr"] | 0;
  JsonArrayConst cores = cpu["c"];
  s.coreCount = min((int)cores.size(), 16);
  for (int i = 0; i < s.coreCount; i++)
    s.cores[i] = cores[i] | 0.0f;
  JsonArrayConst la = cpu["la"];
  for (int i = 0; i < 3; i++)
    s.loadAvg[i] = la[i] | 0.0f;

  JsonObjectConst gpu = doc["gpu"];
  s.gpuOk = !gpu.isNull();
  if (s.gpuOk) {
    copyStr(s.gpuName, sizeof s.gpuName, gpu["n"]);
    s.gpuLoad = gpu["l"] | 0.0f;
    s.gpuTemp = gpu["t"] | 0.0f;
    s.gpuPwr = gpu["w"] | 0.0f;
    s.gpuPwrLim = gpu["wl"] | 0.0f;
    s.gpuFan = gpu["fan"] | -1;
    s.gpuClk = gpu["cc"] | 0;
    s.memClk = gpu["mc"] | 0;
    s.pcieGen = gpu["pg"] | 0;
    s.pcieWidth = gpu["pw"] | 0;
    s.vramUsed = gpu["vu"] | 0.0f;
    s.vramTotal = gpu["vt"] | 0.0f;
  }

  s.ramUsed = doc["ram"][0] | 0.0f;
  s.ramTotal = doc["ram"][1] | 0.0f;
  s.swapUsed = doc["swp"][0] | 0.0f;
  s.swapTotal = doc["swp"][1] | 0.0f;

  JsonObjectConst dsk = doc["dsk"];
  s.diskUsed = dsk["u"] | 0.0f;
  s.diskTotal = dsk["t"] | 0.0f;
  s.diskRd = dsk["r"] | 0.0f;
  s.diskWr = dsk["w"] | 0.0f;
  s.nvmeTemp = dsk["tmp"] | 0.0f;

  JsonObjectConst net = doc["net"];
  copyStr(s.iface, sizeof s.iface, net["if"]);
  s.netRx = net["rx"] | 0.0f;
  s.netTx = net["tx"] | 0.0f;

  hCpu.push(s.cpuLoad);
  hGpu.push(s.gpuLoad);
  hRx.push(s.netRx);
  hTx.push(s.netTx);
  hRd.push(s.diskRd);
  hWr.push(s.diskWr);
  return true;
}

// Non-blocking line reader; overlong lines are dropped whole.
bool pollSerial() {
  bool updated = false;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      if (!lineOverflow && lineLen > 0) {
        lineBuf[lineLen] = 0;
        updated |= parseLine(lineBuf);
      }
      lineLen = 0;
      lineOverflow = false;
    } else if (lineLen < LINE_MAX - 1) {
      lineBuf[lineLen++] = c;
    } else {
      lineOverflow = true;
    }
  }
  return updated;
}

void setup() {
  // A frame takes tens of ms to render; the default 256 B RX buffer would
  // overflow mid-line while drawing.
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);

  tft.init();
  tft.setRotation(3);
  tft.invertDisplay(true);
  tft.fillScreen(TFT_BLACK);

  spr.setColorDepth(4);
  if (spr.createSprite(SCREEN_W, SCREEN_H) == nullptr) {
    tft.setTextColor(TFT_RED);
    tft.drawString("SPRITE ALLOC FAILED", 10, 10, 2);
    while (true)
      delay(1000);
  }
  spr.createPalette(PALETTE);
  render();
}

void loop() {
  bool dirty = false;

  if (pollSerial()) {
    lastDataTime = millis();
    isConnected = everConnected = true;
    dirty = true;
  }

  if (isConnected && millis() - lastDataTime > LINK_TIMEOUT_MS) {
    isConnected = false;
    dirty = true;
  }

  // While offline, refresh once a second so the lost-time counter ticks.
  static unsigned long lastDraw = 0;
  if (!isConnected && millis() - lastDraw > 1000)
    dirty = true;

  if (dirty) {
    render();
    lastDraw = millis();
  }
  delay(5);
}
