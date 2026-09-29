/*
  ============================================================
  AI DESK COMPANION
  ESP32-C3 SuperMini + OLED SSD1306 SPI 128x64 + buzzer pasivo
  ============================================================
  Mismo cableado que cyberpunk_desk_companion / oled_test_spi:

    OLED (modulo de 6 pines, CS atado a GND en R3/R4)
      GND -> GND
      VCC -> 3.3V
      SCL / D0 -> GPIO4   SCK  (SPI por defecto del core)
      SDA / D1 -> GPIO6   MOSI
      RES      -> GPIO3
      DC       -> GPIO2
      CS       -> no cablear. GPIO7 solo existe para la libreria.

    Buzzer pasivo -> GPIO10  (tone(); un buzzer activo no da melodias)
    Boton BOOT    -> GPIO9   (ya esta en la placa, a GND)
      corto  -> muestra la IP y el nombre
      largo  -> limpia la cola y el estado "trabajando"
      5 s    -> portal para WiFi y el nombre del duenio

  Al conectar el WiFi se anuncia en la red como
  "Claudio's Birth de {duenio}". El duenio se escribe en el
  portal cautivo. El daemon de la PC lo reconoce por UDP.

  En reposo muestra reloj NTP + clima + quien esta trabajando.
  POST /notify encola un evento y lo reproduce sin delay():
    sonido -> carita 3s -> mensaje 3s -> otra vez -> reloj

  Librerias (Library Manager):
    Adafruit SSD1306, Adafruit GFX, ArduinoJson v7, WiFiManager (tzapu)
  Placa: ESP32C3 Dev Module
  USB CDC On Boot: Enabled
  Partition Scheme: Huge APP (3MB No OTA / 1MB SPIFFS)
  ============================================================
*/

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <time.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// ---------------------- PINES (no cambiar: ya validados) ----------------------

#define OLED_DC     2
#define OLED_RST    3
#define OLED_CS     7
#define BUZZER_PIN  10
#define BUTTON_PIN  9

#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64

const long UTC_OFFSET_SECONDS = -3 * 3600;  // Argentina, sin horario de verano
const uint16_t BEACON_PORT = 8766;

const uint32_t FACE_MS = 3000;
const uint32_t MESSAGE_MS = 3000;
const uint32_t SLOW_AFTER_MS = 12UL * 60UL * 1000UL;
const uint32_t WORKING_MAX_MS = 45UL * 60UL * 1000UL;
const uint32_t WEATHER_OK_MS = 20UL * 60UL * 1000UL;
const uint32_t WEATHER_RETRY_MS = 45UL * 1000UL;

// ---------------------- HARDWARE / RED ----------------------

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RST, OLED_CS);
WebServer server(80);
WiFiUDP beaconUdp;
Preferences prefs;
SemaphoreHandle_t cfgMutex;
SemaphoreHandle_t weatherMutex;

bool displayOk = false;
bool portalStarted = false;
bool configHold = false;

char cityBuf[40] = "";
char keyBuf[48] = "";
char ownerBuf[32] = "";
char displayName[64] = "Claudio's Birth";
char hostName[32] = "claudios-birth";
char macBuf[13] = "";

const char* PRODUCT_ID = "claudios-birth";
const char* PRODUCT_NAME = "Claudio's Birth";

// ---------------------- CLIMA ----------------------

enum WeatherKind { WX_NONE, WX_SUN, WX_CLOUD, WX_RAIN, WX_STORM, WX_SNOW, WX_MIST };

struct WeatherSnap {
  bool ok;
  int temp;
  int humidity;
  WeatherKind kind;
  char place[20];
};

WeatherSnap weather = {false, 0, 0, WX_NONE, ""};
volatile bool weatherNow = true;
uint32_t nextWeatherAt = 0;

// ---------------------- EVENTOS ----------------------

enum Phase { PH_CLOCK, PH_FACE, PH_MSG };

struct NoteEvent {
  char source[12];
  char type[12];
  char message[96];
  uint32_t mark;
};

const int QUEUE_CAP = 8;
NoteEvent queueBuf[QUEUE_CAP];
int qHead = 0;
int qCount = 0;

Phase phase = PH_CLOCK;
int pass = 0;
uint32_t phaseAt = 0;
uint32_t lastFaceDraw = 0;
int faceFrame = 0;
NoteEvent currentEv;
volatile bool clockDirty = true;
uint32_t infoUntil = 0;

struct Agent {
  bool working;
  uint32_t startedMs;
  bool slowSent;
};

Agent agents[2];  // 0 cursor, 1 claude

// ---------------------- SONIDO (no bloqueante) ----------------------

struct ToneEv {
  uint16_t hz;
  uint16_t ms;
};

const ToneEv MELODY_CAT[] = {
  {680, 45}, {920, 45}, {1180, 50}, {1460, 70}, {1180, 40}, {760, 50}, {520, 90},
  {0, 90},
  {680, 45}, {920, 45}, {1180, 50}, {1460, 70}, {1180, 40}, {760, 50}, {520, 110}
};
const ToneEv MELODY_BIRD[] = {
  {2200, 50}, {0, 40}, {2700, 40}, {0, 30}, {2400, 45},
  {0, 70},
  {2100, 40}, {2600, 55}, {0, 35}, {2900, 70}
};
const ToneEv MELODY_COW[] = {
  {196, 180}, {146, 280}, {174, 220}, {130, 260}
};
const ToneEv MELODY_ALARM[] = {
  {880, 90}, {440, 90}, {988, 80}, {392, 90}, {1046, 70}, {349, 140}
};

const ToneEv* melody = nullptr;
int melodyLen = 0;
int melodyIdx = -1;
uint32_t noteAt = 0;

void stopMelody() {
  melodyIdx = -1;
  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);
}

void playCurrentNote() {
  if (melodyIdx < 0 || melodyIdx >= melodyLen) return;
  if (melody[melodyIdx].hz == 0) {
    noTone(BUZZER_PIN);
    digitalWrite(BUZZER_PIN, LOW);
  } else {
    tone(BUZZER_PIN, melody[melodyIdx].hz, melody[melodyIdx].ms);
  }
}

void startMelody(const ToneEv* m, int len) {
  melody = m;
  melodyLen = len;
  melodyIdx = 0;
  noteAt = millis();
  playCurrentNote();
}

void soundTick() {
  if (melodyIdx < 0) return;
  if ((uint32_t)(millis() - noteAt) < melody[melodyIdx].ms) return;
  melodyIdx++;
  if (melodyIdx >= melodyLen) {
    stopMelody();
    return;
  }
  noteAt = millis();
  playCurrentNote();
}

void soundFor(const char* source, const char* type) {
  if (strcmp(type, "error") == 0) {
    startMelody(MELODY_ALARM, sizeof(MELODY_ALARM) / sizeof(MELODY_ALARM[0]));
    return;
  }
  if (strcmp(type, "slow") == 0) {
    startMelody(MELODY_COW, sizeof(MELODY_COW) / sizeof(MELODY_COW[0]));
    return;
  }
  if (strcmp(source, "claude") == 0) {
    startMelody(MELODY_BIRD, sizeof(MELODY_BIRD) / sizeof(MELODY_BIRD[0]));
    return;
  }
  startMelody(MELODY_CAT, sizeof(MELODY_CAT) / sizeof(MELODY_CAT[0]));
}

// ---------------------- TEXTO ----------------------

void copyTrunc(char* dst, size_t n, const char* src) {
  if (!src) src = "";
  size_t i = 0;
  for (; src[i] && i + 1 < n; i++) dst[i] = src[i];
  dst[i] = 0;
}

void lowerInPlace(char* s) {
  for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

void sanitizeTo(const char* input, char* out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; input && input[i] && j + 1 < n; i++) {
    uint8_t c = (uint8_t)input[i];
    if (c == 0xC3 && input[i + 1]) {
      uint8_t c2 = (uint8_t)input[i + 1];
      char repl = 0;
      switch (c2) {
        case 0xA1: case 0x81: repl = 'a'; break;
        case 0xA9: case 0x89: repl = 'e'; break;
        case 0xAD: case 0x8D: repl = 'i'; break;
        case 0xB3: case 0x93: repl = 'o'; break;
        case 0xBA: case 0x9A: case 0xBC: case 0x9C: repl = 'u'; break;
        case 0xB1: case 0x91: repl = 'n'; break;
      }
      if (repl) {
        out[j++] = repl;
        i++;
        continue;
      }
    }
    if (c >= 32 && c <= 126) out[j++] = (char)c;
  }
  out[j] = 0;
}

int agentIndex(const char* source) {
  if (strcmp(source, "cursor") == 0) return 0;
  if (strcmp(source, "claude") == 0) return 1;
  return -1;
}

bool isPlayable(const char* type) {
  return strcmp(type, "approval") == 0 || strcmp(type, "finished") == 0 ||
         strcmp(type, "slow") == 0 || strcmp(type, "error") == 0;
}

bool sameEvent(const NoteEvent& a, const NoteEvent& b) {
  return strcmp(a.source, b.source) == 0 && strcmp(a.type, b.type) == 0;
}

void defaultMessage(const NoteEvent& e, char* out, size_t n) {
  const char* who = "Cursor";
  if (strcmp(e.source, "claude") == 0) who = "Claude";
  else if (strcmp(e.source, "cursor") != 0) who = e.source;
  if (strcmp(e.type, "approval") == 0) snprintf(out, n, "%s necesita aprobacion", who);
  else if (strcmp(e.type, "finished") == 0) snprintf(out, n, "%s termino su tarea", who);
  else if (strcmp(e.type, "slow") == 0) snprintf(out, n, "%s esta tardando", who);
  else if (strcmp(e.type, "error") == 0) snprintf(out, n, "%s tuvo un problema", who);
  else snprintf(out, n, "%s", who);
}

// ---------------------- COLA Y AGENTES ----------------------

bool enqueue(const NoteEvent& e) {
  if (qCount >= QUEUE_CAP) return false;
  if (qCount > 0) {
    int last = (qHead + qCount - 1) % QUEUE_CAP;
    if (sameEvent(queueBuf[last], e)) return true;
  }
  if (phase != PH_CLOCK && sameEvent(currentEv, e) && (uint32_t)(millis() - phaseAt) < 4000) {
    return true;
  }
  int tail = (qHead + qCount) % QUEUE_CAP;
  queueBuf[tail] = e;
  qCount++;
  return true;
}

bool dequeue(NoteEvent& e) {
  if (qCount == 0) return false;
  e = queueBuf[qHead];
  qHead = (qHead + 1) % QUEUE_CAP;
  qCount--;
  return true;
}

void noteAgent(const NoteEvent& e) {
  int i = agentIndex(e.source);
  if (i < 0) return;
  if (strcmp(e.type, "working") == 0) {
    agents[i].working = true;
    agents[i].startedMs = millis();
    agents[i].slowSent = false;
    clockDirty = true;
    return;
  }
  if (strcmp(e.type, "finished") == 0 || strcmp(e.type, "error") == 0 || strcmp(e.type, "idle") == 0) {
    if (agents[i].working && (int32_t)(e.mark - agents[i].startedMs) >= 0) {
      agents[i].working = false;
      agents[i].slowSent = false;
      clockDirty = true;
    }
  }
}

void clearAgentsAndQueue() {
  qHead = 0;
  qCount = 0;
  agents[0].working = false;
  agents[1].working = false;
  agents[0].slowSent = false;
  agents[1].slowSent = false;
  phase = PH_CLOCK;
  stopMelody();
  clockDirty = true;
}

void maybeEnqueueSlow() {
  const char* names[2] = {"cursor", "claude"};
  for (int i = 0; i < 2; i++) {
    if (!agents[i].working) continue;
    uint32_t elapsed = millis() - agents[i].startedMs;
    if (elapsed > WORKING_MAX_MS) {
      agents[i].working = false;
      agents[i].slowSent = false;
      clockDirty = true;
      continue;
    }
    if (agents[i].slowSent || elapsed < SLOW_AFTER_MS) continue;
    NoteEvent e = {};
    copyTrunc(e.source, sizeof(e.source), names[i]);
    copyTrunc(e.type, sizeof(e.type), "slow");
    e.mark = millis();
    if (enqueue(e)) agents[i].slowSent = true;
  }
}

// ---------------------- PANTALLA ----------------------

void showLines(const char* a, const char* b, const char* c) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 8);
  display.println(a);
  display.println(b);
  display.println(c);
  display.display();
}

void drawSun(int cx, int cy, int frame) {
  display.fillCircle(cx, cy, 3, SSD1306_WHITE);
  const int8_t dir[8][2] = {
    {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}
  };
  int shift = frame % 2;
  for (int i = shift; i < 8; i += 2) {
    display.drawLine(cx + dir[i][0] * 5, cy + dir[i][1] * 5,
                     cx + dir[i][0] * 8, cy + dir[i][1] * 8, SSD1306_WHITE);
  }
}

void drawCloud(int x, int y) {
  display.fillCircle(x + 4, y + 5, 3, SSD1306_WHITE);
  display.fillCircle(x + 9, y + 4, 4, SSD1306_WHITE);
  display.fillRect(x + 2, y + 5, 12, 4, SSD1306_WHITE);
}

void drawWeatherIcon(WeatherKind kind, int frame) {
  int x = 0;
  int y = 40;
  if (kind == WX_SUN) drawSun(8, y + 6, frame);
  else if (kind == WX_SNOW) {
    drawCloud(0, y);
    display.drawPixel(4, y + 12, SSD1306_WHITE);
    display.drawPixel(10, y + 11 + (frame % 2), SSD1306_WHITE);
  } else if (kind == WX_CLOUD || kind == WX_MIST) {
    drawCloud(0, y);
    if (kind == WX_MIST) {
      display.drawLine(2, y + 12, 14, y + 12, SSD1306_WHITE);
    }
  } else {
    drawCloud(0, y);
    for (int i = 0; i < 3; i++) {
      int dy = (frame + i * 2) % 4;
      display.drawLine(3 + i * 4, y + 10 + dy, 2 + i * 4, y + 13 + dy, SSD1306_WHITE);
    }
  }
}

void drawWifi(bool on) {
  for (int i = 0; i < 3; i++) {
    int h = 2 + i * 2;
    int x = 112 + i * 5;
    int y = 7 - h;
    if (on) display.fillRect(x, y, 3, h, SSD1306_WHITE);
    else display.drawRect(x, y, 3, h, SSD1306_WHITE);
  }
}

void drawAgentChip(int x, const char* label, const Agent& agent) {
  display.setTextSize(1);
  display.setCursor(x, 56);
  display.print(label);
  int cx = x + (int)strlen(label) * 6 + 5;
  if (agent.working) {
    display.fillCircle(cx, 59, 2, SSD1306_WHITE);
    uint32_t sec = (millis() - agent.startedMs) / 1000UL;
    char buf[8];
    if (sec < 60) snprintf(buf, sizeof(buf), "%lus", (unsigned long)sec);
    else if (sec < 3600) snprintf(buf, sizeof(buf), "%lum", (unsigned long)(sec / 60));
    else snprintf(buf, sizeof(buf), "%luh", (unsigned long)(sec / 3600));
    display.setCursor(cx + 6, 56);
    display.print(buf);
  } else {
    display.drawCircle(cx, 59, 2, SSD1306_WHITE);
  }
}

void drawClock() {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (infoUntil && (int32_t)(millis() - infoUntil) < 0) {
    display.setTextSize(1);
    display.setCursor(16, 6);
    display.print("Claudio's Birth");
    display.setTextSize(1);
    display.setCursor(10, 24);
    if (WiFi.status() == WL_CONNECTED) display.println(WiFi.localIP());
    else display.println("sin WiFi");
    display.setCursor(10, 40);
    display.print("POST /notify");
    display.setCursor(10, 52);
    display.print("5s: tu nombre");
    display.display();
    return;
  }
  infoUntil = 0;

  struct tm ti;
  bool haveTime = getLocalTime(&ti, 0);
  char head[20];
  char clock[8];
  if (haveTime) {
    const char* dow[] = {"DOM", "LUN", "MAR", "MIE", "JUE", "VIE", "SAB"};
    const char* mon[] = {"ENE", "FEB", "MAR", "ABR", "MAY", "JUN", "JUL", "AGO", "SEP", "OCT", "NOV", "DIC"};
    snprintf(head, sizeof(head), "%s %02d %s", dow[ti.tm_wday], ti.tm_mday, mon[ti.tm_mon]);
    snprintf(clock, sizeof(clock), "%02d:%02d", ti.tm_hour, ti.tm_min);
  } else {
    snprintf(head, sizeof(head), "SIN HORA");
    snprintf(clock, sizeof(clock), "--:--");
  }

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(head);
  drawWifi(WiFi.status() == WL_CONNECTED);

  display.setTextSize(3);
  int16_t x1, y1;
  uint16_t tw, th;
  display.getTextBounds(clock, 0, 0, &x1, &y1, &tw, &th);
  display.setCursor((SCREEN_WIDTH - (int)tw) / 2, 14);
  display.print(clock);

  WeatherSnap w;
  xSemaphoreTake(weatherMutex, portMAX_DELAY);
  w = weather;
  xSemaphoreGive(weatherMutex);

  display.setTextSize(1);
  if (w.ok) {
    static uint8_t frame = 0;
    static uint32_t lastFrame = 0;
    if (millis() - lastFrame > 400) {
      lastFrame = millis();
      frame++;
    }
    drawWeatherIcon(w.kind, frame);
    char line[28];
    char place[12];
    sanitizeTo(w.place, place, sizeof(place));
    if (w.humidity >= 0) {
      snprintf(line, sizeof(line), "%dC %s %d%%", w.temp, place, w.humidity);
    } else {
      snprintf(line, sizeof(line), "%dC %s", w.temp, place);
    }
    display.setCursor(20, 44);
    display.print(line);
  } else if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    char line[28];
    snprintf(line, sizeof(line), "IP %u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    display.setCursor(0, 44);
    display.print(line);
  } else {
    display.setCursor(0, 44);
    display.print("buscando WiFi");
  }

  drawAgentChip(0, "C", agents[0]);
  drawAgentChip(70, "Cl", agents[1]);
  display.display();
}

void faceApproval(int frame) {
  display.clearDisplay();
  display.drawLine(28, 12, 50, 18, SSD1306_WHITE);
  display.drawLine(78, 18, 100, 12, SSD1306_WHITE);
  display.drawCircle(40, 30, 11, SSD1306_WHITE);
  display.drawCircle(88, 30, 11, SSD1306_WHITE);
  int shift = (frame % 4 == 3) ? -3 : 3;
  display.fillCircle(40 + shift, 31, 3, SSD1306_WHITE);
  display.fillCircle(88 + shift, 31, 3, SSD1306_WHITE);
  display.drawLine(46, 50, 82, 50, SSD1306_WHITE);
  display.display();
}

void faceFinished() {
  display.clearDisplay();
  display.drawLine(28, 32, 40, 20, SSD1306_WHITE);
  display.drawLine(40, 20, 52, 32, SSD1306_WHITE);
  display.drawLine(76, 32, 88, 20, SSD1306_WHITE);
  display.drawLine(88, 20, 100, 32, SSD1306_WHITE);
  display.drawLine(46, 44, 64, 54, SSD1306_WHITE);
  display.drawLine(64, 54, 82, 44, SSD1306_WHITE);
  display.display();
}

void faceSlow(int frame) {
  display.clearDisplay();
  display.fillRoundRect(26, 28, 26, 5, 2, SSD1306_WHITE);
  display.fillRoundRect(76, 28, 26, 5, 2, SSD1306_WHITE);
  display.drawLine(48, 50, 80, 52, SSD1306_WHITE);
  int dy = (frame % 3) * 2;
  display.fillCircle(108, 16 + dy, 2, SSD1306_WHITE);
  display.drawLine(108, 18 + dy, 106, 26 + dy, SSD1306_WHITE);
  display.display();
}

void faceError() {
  display.clearDisplay();
  for (int o = 0; o < 2; o++) {
    display.drawLine(28 + o, 16, 52 + o, 40, SSD1306_WHITE);
    display.drawLine(28 + o, 40, 52 + o, 16, SSD1306_WHITE);
    display.drawLine(76 + o, 16, 100 + o, 40, SSD1306_WHITE);
    display.drawLine(76 + o, 40, 100 + o, 16, SSD1306_WHITE);
  }
  display.drawCircle(64, 52, 4, SSD1306_WHITE);
  display.display();
}

void drawFace(const char* type, int frame) {
  if (!displayOk) return;
  if (strcmp(type, "finished") == 0) faceFinished();
  else if (strcmp(type, "slow") == 0) faceSlow(frame);
  else if (strcmp(type, "error") == 0) faceError();
  else faceApproval(frame);
}

int wrapLines(const char* text, char lines[][22], int maxLines) {
  int count = 0;
  const char* p = text;
  while (*p && count < maxLines) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* start = p;
    const char* lastSpace = nullptr;
    int len = 0;
    while (*p && len < 20) {
      if (*p == ' ') lastSpace = p;
      p++;
      len++;
    }
    if (*p && lastSpace && lastSpace > start) {
      len = (int)(lastSpace - start);
      p = lastSpace + 1;
    }
    if (len > 20) len = 20;
    memcpy(lines[count], start, len);
    lines[count][len] = 0;
    count++;
  }
  return count;
}

void drawMessage(const NoteEvent& e) {
  if (!displayOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  char title[12];
  if (strcmp(e.source, "claude") == 0) copyTrunc(title, sizeof(title), "CLAUDE");
  else if (strcmp(e.source, "cursor") == 0) copyTrunc(title, sizeof(title), "CURSOR");
  else {
    copyTrunc(title, sizeof(title), e.source);
    for (char* s = title; *s; s++) *s = (char)toupper((unsigned char)*s);
  }
  int tw = (int)strlen(title) * 6;
  display.setCursor((SCREEN_WIDTH - tw) / 2, 0);
  display.print(title);
  display.drawLine(0, 11, 127, 11, SSD1306_WHITE);

  char raw[96];
  char clean[96];
  if (e.message[0]) copyTrunc(raw, sizeof(raw), e.message);
  else defaultMessage(e, raw, sizeof(raw));
  sanitizeTo(raw, clean, sizeof(clean));

  char lines[4][22];
  int n = wrapLines(clean, lines, 4);
  if (n == 0) {
    copyTrunc(lines[0], 22, "...");
    n = 1;
  }
  int y = 18 + (4 - n) * 5;
  for (int i = 0; i < n; i++) {
    int lw = (int)strlen(lines[i]) * 6;
    display.setCursor((SCREEN_WIDTH - lw) / 2, y + i * 10);
    display.print(lines[i]);
  }
  display.display();
}

// ---------------------- SECUENCIA ----------------------

void enterFace() {
  phase = PH_FACE;
  phaseAt = millis();
  lastFaceDraw = phaseAt;
  faceFrame = 0;
  soundFor(currentEv.source, currentEv.type);
  drawFace(currentEv.type, faceFrame);
}

void enterMessage() {
  phase = PH_MSG;
  phaseAt = millis();
  drawMessage(currentEv);
}

void beginPlayback(const NoteEvent& e) {
  currentEv = e;
  pass = 0;
  noteAgent(e);
  enterFace();
}

void phaseTick() {
  if (configHold) return;
  if (phase == PH_CLOCK) {
    NoteEvent e;
    if (dequeue(e)) {
      beginPlayback(e);
      return;
    }
    static uint32_t lastDraw = 0;
    if (clockDirty || millis() - lastDraw > 250) {
      clockDirty = false;
      lastDraw = millis();
      drawClock();
    }
    return;
  }

  if (phase == PH_FACE && millis() - lastFaceDraw > 650) {
    lastFaceDraw = millis();
    faceFrame++;
    drawFace(currentEv.type, faceFrame);
  }

  uint32_t dur = (phase == PH_FACE) ? FACE_MS : MESSAGE_MS;
  if ((uint32_t)(millis() - phaseAt) < dur) return;

  if (phase == PH_FACE) {
    enterMessage();
    return;
  }
  if (pass == 0) {
    pass = 1;
    enterFace();
    return;
  }
  phase = PH_CLOCK;
  stopMelody();
  clockDirty = true;
}

// ---------------------- CONFIGURACION ----------------------

void trimInPlace(char* s) {
  char* start = s;
  while (*start == ' ') start++;
  if (start != s) memmove(s, start, strlen(start) + 1);
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '"' || s[n - 1] == '\\')) s[--n] = 0;
  char* w = s;
  for (char* r = s; *r; r++) {
    if (*r == '"' || *r == '\\' || *r == '\n' || *r == '\r') continue;
    *w++ = *r;
  }
  *w = 0;
}

void slugAppend(char* dst, size_t n, const char* src, bool separate) {
  size_t j = strlen(dst);
  if (separate && j > 0 && j + 1 < n) dst[j++] = '-';
  bool dash = false;
  for (size_t i = 0; src[i] && j + 1 < n; i++) {
    unsigned char c = (unsigned char)src[i];
    if (isalnum(c)) {
      dst[j++] = (char)tolower(c);
      dash = false;
    } else if (!dash && j > 0 && j + 1 < n) {
      dst[j++] = '-';
      dash = true;
    }
  }
  while (j > 0 && dst[j - 1] == '-') j--;
  dst[j] = 0;
}

void buildIdentity() {
  trimInPlace(ownerBuf);
  if (ownerBuf[0]) snprintf(displayName, sizeof(displayName), "%s de %s", PRODUCT_NAME, ownerBuf);
  else snprintf(displayName, sizeof(displayName), "%s", PRODUCT_NAME);
  hostName[0] = 0;
  slugAppend(hostName, sizeof(hostName), PRODUCT_ID, false);
  if (ownerBuf[0]) slugAppend(hostName, sizeof(hostName), ownerBuf, true);
  if (!hostName[0]) copyTrunc(hostName, sizeof(hostName), PRODUCT_ID);
}

void loadSettings() {
  prefs.begin("deskcomp", true);
  prefs.getString("city", cityBuf, sizeof(cityBuf));
  prefs.getString("owm_key", keyBuf, sizeof(keyBuf));
  prefs.getString("owner", ownerBuf, sizeof(ownerBuf));
  prefs.end();
  if (cityBuf[0] == 0 || keyBuf[0] == 0) {
    prefs.begin("cyberpunk", true);
    if (cityBuf[0] == 0) prefs.getString("city", cityBuf, sizeof(cityBuf));
    if (keyBuf[0] == 0) prefs.getString("owm_key", keyBuf, sizeof(keyBuf));
    prefs.end();
  }
  buildIdentity();
}

void saveSettings() {
  buildIdentity();
  prefs.begin("deskcomp", false);
  prefs.putString("city", cityBuf);
  prefs.putString("owm_key", keyBuf);
  prefs.putString("owner", ownerBuf);
  prefs.end();
}

void copyCfg(char* city, size_t cityN, char* key, size_t keyN) {
  xSemaphoreTake(cfgMutex, portMAX_DELAY);
  copyTrunc(city, cityN, cityBuf);
  copyTrunc(key, keyN, keyBuf);
  xSemaphoreGive(cfgMutex);
}

// ---------------------- CLIMA ----------------------

int kindFromId(int id) {
  if (id >= 200 && id < 300) return WX_STORM;
  if (id >= 300 && id < 600) return WX_RAIN;
  if (id >= 600 && id < 700) return WX_SNOW;
  if (id >= 700 && id < 800) return WX_MIST;
  if (id == 800) return WX_SUN;
  if (id > 800 && id < 900) return WX_CLOUD;
  return WX_NONE;
}

void urlEncode(const char* in, char* out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 4 < n; i++) {
    unsigned char c = (unsigned char)in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[j++] = (char)c;
    else if (c == ' ') {
      out[j++] = '%';
      out[j++] = '2';
      out[j++] = '0';
    } else {
      snprintf(out + j, n - j, "%%%02X", c);
      j += 3;
    }
  }
  out[j] = 0;
}

bool fetchWeather() {
  char city[40];
  char key[48];
  copyCfg(city, sizeof(city), key, sizeof(key));
  if (!city[0] || !key[0] || WiFi.status() != WL_CONNECTED) return false;

  char enc[80];
  urlEncode(city, enc, sizeof(enc));
  char url[240];
  snprintf(url, sizeof(url),
           "https://api.openweathermap.org/data/2.5/weather?q=%s&appid=%s&units=metric&lang=es",
           enc, key);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, url)) return false;
  http.setTimeout(8000);
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err) {
      WeatherSnap snap = {};
      snap.ok = true;
      snap.temp = (int)round((float)(doc["main"]["temp"] | 0.0));
      snap.humidity = doc["main"]["humidity"] | -1;
      snap.kind = (WeatherKind)kindFromId(doc["weather"][0]["id"] | 0);
      const char* name = doc["name"] | city;
      sanitizeTo(name, snap.place, sizeof(snap.place));
      xSemaphoreTake(weatherMutex, portMAX_DELAY);
      weather = snap;
      xSemaphoreGive(weatherMutex);
      clockDirty = true;
      ok = true;
      Serial.printf("Clima %s %dC\n", snap.place, snap.temp);
    }
  } else {
    Serial.printf("Clima HTTP %d\n", code);
  }
  http.end();
  return ok;
}

void weatherTask(void*) {
  for (;;) {
    if (weatherNow || (int32_t)(millis() - nextWeatherAt) >= 0) {
      weatherNow = false;
      bool ok = fetchWeather();
      nextWeatherAt = millis() + (ok ? WEATHER_OK_MS : WEATHER_RETRY_MS);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

size_t jsonEscapeTo(const char* in, char* out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j + 2 < n; i++) {
    if (in[i] == '"' || in[i] == '\\') {
      if (j + 3 >= n) break;
      out[j++] = '\\';
    }
    out[j++] = in[i];
  }
  out[j] = 0;
  return j;
}

// ---------------------- HTTP ----------------------

void fillEventFromFields(NoteEvent& e, const char* source, const char* type, const char* message) {
  memset(&e, 0, sizeof(e));
  copyTrunc(e.source, sizeof(e.source), source && source[0] ? source : "cursor");
  copyTrunc(e.type, sizeof(e.type), type ? type : "");
  copyTrunc(e.message, sizeof(e.message), message ? message : "");
  lowerInPlace(e.source);
  lowerInPlace(e.type);
  e.mark = millis();
}

void handleNotify() {
  NoteEvent e = {};
  String body = server.arg("plain");
  if (body.length() > 0 && body[0] == '{') {
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"json\"}");
      return;
    }
    fillEventFromFields(e, doc["source"] | "", doc["type"] | "", doc["message"] | "");
  } else {
    fillEventFromFields(e, server.arg("source").c_str(), server.arg("type").c_str(), server.arg("message").c_str());
  }

  if (!e.type[0]) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"type\"}");
    return;
  }

  Serial.printf("Evento %s %s\n", e.source, e.type);

  if (strcmp(e.type, "working") == 0 || strcmp(e.type, "idle") == 0) {
    noteAgent(e);
    server.send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (!isPlayable(e.type)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"type\"}");
    return;
  }
  noteAgent(e);
  if (!enqueue(e)) {
    server.send(503, "application/json", "{\"ok\":false,\"error\":\"queue\"}");
    return;
  }
  char resp[48];
  snprintf(resp, sizeof(resp), "{\"ok\":true,\"queued\":%d}", qCount);
  server.send(200, "application/json", resp);
}

void handleStatus() {
  WeatherSnap w;
  xSemaphoreTake(weatherMutex, portMAX_DELAY);
  w = weather;
  xSemaphoreGive(weatherMutex);
  const char* phaseName = "clock";
  if (phase == PH_FACE) phaseName = "face";
  else if (phase == PH_MSG) phaseName = "message";
  char nameEsc[96];
  char ownerEsc[64];
  jsonEscapeTo(displayName, nameEsc, sizeof(nameEsc));
  jsonEscapeTo(ownerBuf, ownerEsc, sizeof(ownerEsc));
  char buf[512];
  snprintf(buf, sizeof(buf),
           "{\"ip\":\"%u.%u.%u.%u\",\"name\":\"%s\",\"owner\":\"%s\",\"id\":\"%s\","
           "\"queue\":%d,\"phase\":\"%s\","
           "\"cursor_working\":%s,\"claude_working\":%s,"
           "\"temp\":%d,\"humidity\":%d,\"weather\":%s}",
           WiFi.localIP()[0], WiFi.localIP()[1], WiFi.localIP()[2], WiFi.localIP()[3],
           nameEsc, ownerEsc, PRODUCT_ID,
           qCount, phaseName,
           agents[0].working ? "true" : "false",
           agents[1].working ? "true" : "false",
           w.ok ? w.temp : 0,
           w.ok ? w.humidity : -1,
           w.ok ? "true" : "false");
  server.send(200, "application/json", buf);
}

void handleConfig() {
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"ok\":false}");
    return;
  }
  bool ownerChanged = false;
  xSemaphoreTake(cfgMutex, portMAX_DELAY);
  if (doc["city"].is<const char*>()) copyTrunc(cityBuf, sizeof(cityBuf), doc["city"]);
  if (doc["owm_key"].is<const char*>()) copyTrunc(keyBuf, sizeof(keyBuf), doc["owm_key"]);
  if (doc["owner"].is<const char*>()) {
    char next[32];
    copyTrunc(next, sizeof(next), doc["owner"]);
    trimInPlace(next);
    ownerChanged = strcmp(next, ownerBuf) != 0;
    copyTrunc(ownerBuf, sizeof(ownerBuf), next);
  }
  xSemaphoreGive(cfgMutex);
  saveSettings();
  weatherNow = true;
  server.send(200, "application/json", "{\"ok\":true}");
  if (ownerChanged) {
    delay(200);
    ESP.restart();
  }
}

void setupRoutes() {
  server.on("/notify", HTTP_POST, handleNotify);
  server.on("/notify", HTTP_GET, handleNotify);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/health", HTTP_GET, handleStatus);
  server.on("/config", HTTP_POST, handleConfig);
  server.begin();
}

// ---------------------- WIFI ----------------------

void configModeCallback(WiFiManager* wm) {
  portalStarted = true;
  String ssid = wm->getConfigPortalSSID();
  showLines("Claudio's Birth", "WiFi de config:", ssid.c_str());
  tone(BUZZER_PIN, 880, 80);
}

void rememberMac() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(macBuf, sizeof(macBuf), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void sendBeacon(const IPAddress& dest) {
  char ownerEsc[64];
  char nameEsc[96];
  jsonEscapeTo(ownerBuf, ownerEsc, sizeof(ownerEsc));
  jsonEscapeTo(displayName, nameEsc, sizeof(nameEsc));
  IPAddress ip = WiFi.localIP();
  char payload[240];
  snprintf(payload, sizeof(payload),
           "{\"id\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"owner\":\"%s\",\"name\":\"%s\",\"mac\":\"%s\"}",
           PRODUCT_ID, ip[0], ip[1], ip[2], ip[3], ownerEsc, nameEsc, macBuf);
  if (!beaconUdp.beginPacket(dest, BEACON_PORT)) return;
  beaconUdp.print(payload);
  beaconUdp.endPacket();
}

void beaconTick() {
  if (WiFi.status() != WL_CONNECTED) return;
  static uint32_t last = 0;
  static bool sentFirst = false;
  if (sentFirst && millis() - last < 10000) return;
  sentFirst = true;
  last = millis();
  if (!macBuf[0]) rememberMac();
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress bcast(
    (uint8_t)(ip[0] | (uint8_t)~mask[0]),
    (uint8_t)(ip[1] | (uint8_t)~mask[1]),
    (uint8_t)(ip[2] | (uint8_t)~mask[2]),
    (uint8_t)(ip[3] | (uint8_t)~mask[3])
  );
  sendBeacon(bcast);
  if (bcast != IPAddress(255, 255, 255, 255)) sendBeacon(IPAddress(255, 255, 255, 255));
}

void wifiMaintain() {
  if (WiFi.status() == WL_CONNECTED) return;
  static uint32_t last = 0;
  if (millis() - last < 15000) return;
  last = millis();
  WiFi.reconnect();
}

void enterConfigPortal() {
  configHold = false;
  stopMelody();
  showLines("Claudio's Birth", "WiFi de config:", "ClaudioBirth-Setup");
  tone(BUZZER_PIN, 523, 120);
  while (digitalRead(BUTTON_PIN) == LOW) delay(20);
  delay(200);
  server.stop();

  WiFiManager wm;
  wm.setTitle("Claudio's Birth");
  wm.setConfigPortalTimeout(180);
  WiFiManagerParameter pOwner("owner", "Tu nombre", ownerBuf, sizeof(ownerBuf) - 1);
  WiFiManagerParameter pCity("city", "Ciudad (OpenWeather)", cityBuf, sizeof(cityBuf) - 1);
  WiFiManagerParameter pKey("owm", "OpenWeather API key", keyBuf, sizeof(keyBuf) - 1);
  wm.addParameter(&pOwner);
  wm.addParameter(&pCity);
  wm.addParameter(&pKey);

  bool saved = wm.startConfigPortal("ClaudioBirth-Setup");
  if (saved) {
    xSemaphoreTake(cfgMutex, portMAX_DELAY);
    copyTrunc(ownerBuf, sizeof(ownerBuf), pOwner.getValue());
    copyTrunc(cityBuf, sizeof(cityBuf), pCity.getValue());
    copyTrunc(keyBuf, sizeof(keyBuf), pKey.getValue());
    xSemaphoreGive(cfgMutex);
    saveSettings();
  }
  ESP.restart();
}

void buttonTick() {
  if (millis() < 1200) return;
  static bool prev = false;
  static uint32_t downAt = 0;
  static bool hintFired = false;
  bool down = digitalRead(BUTTON_PIN) == LOW;
  if (down && !prev) {
    downAt = millis();
    hintFired = false;
  }
  uint32_t held = millis() - downAt;
  configHold = down && held > 2500;
  if (down && !hintFired && held > 2500) {
    hintFired = true;
    tone(BUZZER_PIN, 440, 40);
    showLines("Claudio's Birth", "segui apretado", "para configurar");
  }
  if (down && held > 5000) {
    enterConfigPortal();
    return;
  }
  if (!down && prev) {
    if (held > 1200 && held <= 5000) {
      clearAgentsAndQueue();
      tone(BUZZER_PIN, 660, 40);
    } else if (held > 40 && held <= 1200 && phase == PH_CLOCK) {
      infoUntil = millis() + 6000;
      clockDirty = true;
    }
    configHold = false;
  }
  prev = down;
}

// ---------------------- SETUP / LOOP ----------------------

void setup() {
  Serial.begin(115200);
  delay(200);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  cfgMutex = xSemaphoreCreateMutex();
  weatherMutex = xSemaphoreCreateMutex();

  displayOk = display.begin(SSD1306_SWITCHCAPVCC);
  if (displayOk) {
    display.setTextWrap(false);
    display.setTextColor(SSD1306_WHITE);
    showLines("Claudio's Birth", "Conectando WiFi...", "");
  } else {
    Serial.println("OLED no inicializo");
  }

  loadSettings();
  if (displayOk && ownerBuf[0]) {
    char who[40];
    snprintf(who, sizeof(who), "de %s", ownerBuf);
    showLines("Claudio's Birth", who, "Conectando WiFi...");
  }
  Serial.printf("Identidad: %s\n", displayName);
  Serial.printf("Ciudad: %s  API clima: %s\n", cityBuf[0] ? cityBuf : "(vacia)", keyBuf[0] ? "si" : "no");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(hostName);
  WiFi.setSleep(false);

  WiFiManager wm;
  wm.setTitle("Claudio's Birth");
  wm.setAPCallback(configModeCallback);
  wm.setConnectTimeout(25);
  wm.setConfigPortalTimeout(180);
  WiFiManagerParameter pOwner("owner", "Tu nombre", ownerBuf, sizeof(ownerBuf) - 1);
  WiFiManagerParameter pCity("city", "Ciudad (OpenWeather)", cityBuf, sizeof(cityBuf) - 1);
  WiFiManagerParameter pKey("owm", "OpenWeather API key", keyBuf, sizeof(keyBuf) - 1);
  wm.addParameter(&pOwner);
  wm.addParameter(&pCity);
  wm.addParameter(&pKey);

  bool connected = wm.autoConnect("ClaudioBirth-Setup");
  if (connected && portalStarted) {
    xSemaphoreTake(cfgMutex, portMAX_DELAY);
    copyTrunc(ownerBuf, sizeof(ownerBuf), pOwner.getValue());
    copyTrunc(cityBuf, sizeof(cityBuf), pCity.getValue());
    copyTrunc(keyBuf, sizeof(keyBuf), pKey.getValue());
    xSemaphoreGive(cfgMutex);
    saveSettings();
    ESP.restart();
  }

  if (connected) {
    WiFi.setHostname(hostName);
    rememberMac();
    Serial.print("IP ");
    Serial.println(WiFi.localIP());
    Serial.printf("En la red: %s (%s)\n", displayName, hostName);
    configTime(UTC_OFFSET_SECONDS, 0, "pool.ntp.org", "time.nist.gov");
    if (MDNS.begin(hostName)) {
      MDNS.setInstanceName(displayName);
      MDNS.addService("http", "tcp", 80);
      MDNS.addServiceTxt("http", "tcp", "id", PRODUCT_ID);
      MDNS.addServiceTxt("http", "tcp", "owner", ownerBuf);
    }
    beaconUdp.begin(8767);
    char who[40];
    if (ownerBuf[0]) snprintf(who, sizeof(who), "de %s", ownerBuf);
    else copyTrunc(who, sizeof(who), "sin nombre");
    showLines("Claudio's Birth", who, WiFi.localIP().toString().c_str());
  } else {
    showLines("Sin WiFi", "Reinicio el intento", "en el loop");
    Serial.println("Sin WiFi, sigo en modo reloj local");
  }

  setupRoutes();
  nextWeatherAt = millis() + 4000;
  xTaskCreate(weatherTask, "weather", 24576, nullptr, 1, nullptr);
  clockDirty = true;
}

void loop() {
  server.handleClient();
  wifiMaintain();
  beaconTick();
  buttonTick();
  soundTick();
  maybeEnqueueSlow();
  phaseTick();
}
