/* 
Authors: Priya Amrutbhai Patel, ChatGPT
Date: 25 September 2025
Subject: TPJ655
Project Name: BloomX
Project Partner: Dainelle Eleanor Rolle
*/

  #include <Arduino.h>
  #include <WiFi.h>
  
  #include <WebServer.h>
  #include <ESP32Servo.h>
  #include <time.h>
  #include <SPI.h>
  #include <WiFiClientSecure.h>                                                                                                                                                                                                  f
  #include <HTTPClient.h>
  #include <ArduinoJson.h>
  #include "driver/i2s.h"
  #include "driver/gpio.h"
  #include <math.h> 
  #include <string.h>
  #include <LittleFS.h>
  #include <TJpg_Decoder.h>
  #include <FS.h>            
  #include <Adafruit_NeoPixel.h>
  #include <Preferences.h>

/********* PINS *********/
#define LED_PIN     13        // WS2812B DIN
#define CLK_PIN     6         // Rotary CLK
#define DT_PIN      7         // Rotary DT
#define SW_PIN      11        // Rotary Switch (active LOW)  <-- was 38 in your snippet
#define NUM_LEDS    72
#define ACTIVE_LEDS     NUM_LEDS
Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

static const uint8_t BRIGHTNESS_MAX = 160;  // global cap (protects PSU)
static const uint8_t BRIGHTNESS_MIN = 5;    // avoid total black except power-off
static const uint32_t LONG_PRESS_MS = 700;
static const uint32_t ROTARY_DEBOUNCE_MS = 250;

static const uint32_t AUTO_OFF_MS   = 10UL * 60UL * 1000UL; // 10 minutes
static const bool     GAMMA_ON      = true;

/********* STATE & STORAGE *********/
Preferences prefs; // NVS
enum Effect : uint8_t { SOLID=0, BREATHE, RAINBOW, CHASE, SPARKLE, WAVE, COMET, TWINKLE, EFFECT_COUNT };

struct AppState {
  uint8_t  paletteIndex = 0;       // which curated color
  uint8_t  brightness    = 100;    // 0..255 but capped
  Effect   effect        = SOLID;  // current effect
  bool     nightMode     = false;  // warm & dim
  bool     powered       = true;   // master on/off (auto-off toggles this)
} st;

/********* CURATED PALETTE (nice, soft BloomX set) *********/
// {R,G,B}
static const uint8_t PALETTE[][3] = {
  {255, 100, 180}, // BloomX Pink
  {100, 180, 255}, // Sky
  {180, 130, 255}, // Lavender
  {255, 220, 170}, // Warm White (cozy)
  {255, 140,  40}, // Sunset
  {120, 255, 200}, // Mint
  {220, 180, 255}, // Lilac
  {255, 170, 130}, // Peach
  {255, 230, 120}, // Soft Gold
  { 80, 200, 255}  // Ocean
};
static const int PALETTE_SIZE = sizeof(PALETTE) / sizeof(PALETTE[0]);

/********* INPUT / ENCODER *********/
int lastCLK = HIGH;
bool btnPrev = HIGH;
unsigned long btnDownAt = 0;
unsigned long lastInputAt = 0;
bool adjustBrightnessMode = false;     // true while button is held
int  accumTicks = 0;                    // for smoothing & acceleration

/********* RENDER TIMERS *********/
unsigned long nowMs = 0;
unsigned long tRainbow = 0;
unsigned long tBreathe = 0;
unsigned long tChase   = 0;
unsigned long tSparkle = 0;
unsigned long tWave    = 0;
unsigned long tComet   = 0;
unsigned long tTwinkle = 0;

/********* UTILS & STATE PERSISTENCE *********/
uint8_t clamp8(int v, uint8_t lo, uint8_t hi) { return (v < lo) ? lo : (v > hi ? hi : v); }
uint8_t cappedBrightness() { return clamp8(st.nightMode ? 20 : st.brightness, 0, BRIGHTNESS_MAX); }

uint32_t applyGamma(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t c = strip.Color(r, g, b);
  return GAMMA_ON ? strip.gamma32(c) : c;
}

uint32_t paletteColor() {
  const uint8_t* c = PALETTE[st.paletteIndex % PALETTE_SIZE];
  return applyGamma(c[0], c[1], c[2]);
}

void fillAll(uint32_t c) {
  // Only light ACTIVE_LEDS; others off (power-safe)
  for (int i=0; i<NUM_LEDS; i++) {
    if (i < ACTIVE_LEDS) strip.setPixelColor(i, c);
    else                 strip.setPixelColor(i, 0);
  }
  strip.setBrightness(cappedBrightness());
  strip.show();
}

void saveState() {
  prefs.begin("bloomx", false);
  prefs.putUChar("pal", st.paletteIndex);
  prefs.putUChar("bri", st.brightness);
  prefs.putUChar("eff", (uint8_t)st.effect);
  prefs.putBool ("night", st.nightMode);
  prefs.end();
}

void loadState() {
  prefs.begin("bloomx", true);
  st.paletteIndex = prefs.getUChar("pal", st.paletteIndex);
  st.brightness   = prefs.getUChar("bri", st.brightness);
  st.effect       = (Effect)prefs.getUChar("eff", (uint8_t)st.effect);
  st.nightMode    = prefs.getBool ("night", st.nightMode);
  prefs.end();
}

  
  /* ========================= WIFI ========================= */
  const char* WIFI_SSID = ""; //wifi id
  const char* WIFI_PASS = ""; //wifi password

  /* ========================= IR + SERVO (BloomX) ========================= */
  #define IR_PIN     14     // LM393 IR module OUT
  #define SERVO_PIN  40      // servo signa0 (PWM)
  const bool IR_ACTIVE_LOW = true; // LM393 boards usually pull LOW on detect

  static int SERVO_LEDC_TIMER = 3;     // dedicate LEDC timer 3 to the servo

  const int OPEN_ANGLE  = 0;
  const int CLOSED_ANGLE = 30;
  const int SERVO_MIN_US = 500;
  const int SERVO_MAX_US = 2500;
  Servo bloomServo;

  const unsigned long NO_MOTION_TIMEOUT = 180000UL; // 3 min
  const int SAMPLE_MS = 13;   // sampling interval
  const int ON_COUNT  = 5;    // ~50ms confirm detected
  const int OFF_COUNT = 10;   // ~100ms confirm clear

  int  integ = 0;
  bool stableDetected = false, prevStableDetected = false;
  bool isOpen = false;
  unsigned long lastDetectMs = 0;

  /* ========================= WEB SERVER & EVENTS ========================= */
  WebServer server(80);
  static const int MAX_EVENTS = 80;
  String eventsBuf[MAX_EVENTS];
  int evHead = 0, evCount = 0;

  /* ========================= AUDIO / PTT PINS ========================= */
  // INMP441 mic (I2S RX)
  static constexpr int I2S_WS   = 19; // LRCL/WS
  static constexpr int I2S_SD   = 15;  // DOUT
  static constexpr int I2S_SCK  = 18; // BCLK

  // MAX98357A speaker (I2S TX)
  static constexpr int AMP_BCLK = 5;  // BCLK
  static constexpr int AMP_LRCK = 4;  // WS/LRCK
  static constexpr int AMP_DIN  = 2;  // DIN/SDIN
  static constexpr int AMP_SD   = -1; // set to GPIO if MAX98357 SD pin is used
  // ---------------- Pins ----------------
  static constexpr int TFT_CS   = 16;
  static constexpr int TFT_DC   = 17;
  static constexpr int TFT_RST  = 12;
  static constexpr int SCK_PIN  = 9;
  static constexpr int MOSI_PIN = 8;
  static constexpr int TOUCH_IO = 21;
  // Push-to-talk button
  static constexpr int BUTTON_PIN = 38; // active-low to GND
  #define USE_ST7789 0  // 0 = GC9A01, 1 = ST7789
  // --- Inputs / sensors ---

  TaskHandle_t alarmTask = nullptr;
// alarm state (shared by alarm_tone_loop/start/stop)
volatile bool alarmPlaying = false;

  // simple overlay state
  bool alarmFlashOn = false;
  uint32_t lastAlarmBlinkMs = 0;
  const uint32_t ALARM_BLINK_MS = 300;  // blink speed

  #include <Adafruit_GFX.h>
  #if USE_ST7789
    #include <Adafruit_ST7789.h>
    #define COL_BLACK ST77XX_BLACK
    #define COL_WHITE ST77XX_WHITE
    Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);   // <-- move here
  #else
    #include <Adafruit_GC9A01A.h>
    #define COL_BLACK GC9A01A_BLACK
    #define COL_WHITE GC9A01A_WHITE
    Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_RST);  // <-- move here
  #endif
  // TJpg_Decoder calls this to push a block of 16-bit pixels
  // Count blocks just for debugging (optional)
  volatile uint32_t jpgBlocks = 0;




  /* ========================= API KEYS / URLs ========================= */
  // Deepgram (STT + TTS)
  const char* DG_API_KEY = ""; //deepgram api key
  const char* DG_STT_URL = "https://api.deepgram.com/v1/listen?model=general&smart_format=true";
  String convo_topic = "";

  // OpenAI Chat
  const char* OA_API_KEY   = ""; //openai key
  const char* OA_CHAT_URL  = "https://api.openai.com/v1/chat/completions";

  // Keep this simple callback
  bool tft_output(int16_t x,int16_t y,uint16_t w,uint16_t h,uint16_t* bitmap){
    if (x<0 || y<0 || x>=tft.width() || y>=tft.height()) return true;
    tft.drawRGBBitmap(x, y, bitmap, w, h);
    return true;
  }

  // Use this version
  void drawJPG(const char* path, int x=0, int y=0) {
    if (!LittleFS.exists(path)) { Serial.printf("JPG not found: %s\n", path); return; }
    int rc = TJpgDec.drawFsJpg(x, y, path, LittleFS);
    Serial.printf("drawFsJpg(%s) rc=%d\n", path, rc); // expect rc=0
  }

  /* ========================= AUDIO SETTINGS ========================= */
  static constexpr int SAMPLE_RATE = 16000;
  static constexpr int BYTES_PER_SAMPLE = 2;  // 16-bit after downconvert
  static constexpr int CHANNELS = 1;
  static constexpr int MAX_RECORD_SEC = 15;
  static constexpr int MIN_RECORD_SEC = 2;
  static constexpr int BUF_SAMPLES = SAMPLE_RATE * MAX_RECORD_SEC;

  /* ========================= AUDIO BUFFERS ========================= */
  static int16_t  *pcm16          = nullptr;  // mic buffer
  static int32_t   i2sRaw[1024];              // mic DMA scratch
  // --- Conversational UI states (simple bitmoji) ---

  // --- Conversational UI state (place this near the top, after includes) ---
  enum UIState { UI_SLEEP, UI_MAIN, UI_LISTEN, UI_THINK, UI_SPEAK };
  volatile UIState uiState = UI_SLEEP;   // single global definition

  // Helper to return to the pink flower
  static inline void backToPink() { uiState = UI_SLEEP; }

  // ---- Sleep-mode clock ----
  bool showSleepClock = true;         // toggle if you want
  int  lastClockMinute = -1;          // redraw only when minute changes
  uint32_t lastClockTickMs = 0;

// ===== Alarm sound config =====
enum AlarmSound { ALARM_CLASSIC, ALARM_PULSE, ALARM_RISING, ALARM_BELL };

volatile AlarmSound alarmSound = ALARM_CLASSIC;   // default
volatile uint8_t alarmVolumePct = 65;            // 0..100, default loud-ish

// Convert 0..100 to 0.0..~0.9 with a perceptual curve
static inline float alarm_gain() {
  float x = constrain(alarmVolumePct, 0, 100) / 100.0f;
  // cubic-ish for finer control at low volumes; cap a bit under 1.0
  return min(0.9f, 0.95f * (x*x*x + 0.05f));
}

bool save_alarm_cfg() {
  StaticJsonDocument<256> d;
  d["mode"] = (int)alarmSound;
  d["vol"]  = (int)alarmVolumePct;
  File f = LittleFS.open("/alarm_cfg.json", FILE_WRITE);
  if (!f) return false;
  bool ok = (serializeJson(d, f) > 0);
  f.close(); return ok;
}

bool load_alarm_cfg() {
  if (!LittleFS.exists("/alarm_cfg.json")) return true;
  File f = LittleFS.open("/alarm_cfg.json", FILE_READ);
  if (!f) return false;
  StaticJsonDocument<256> d;
  DeserializationError e = deserializeJson(d, f); f.close();
  if (e) return false;
  int m = d["mode"] | (int)ALARM_CLASSIC;
  alarmSound = (AlarmSound) constrain(m, (int)ALARM_CLASSIC, (int)ALARM_BELL);
  alarmVolumePct = (uint8_t) constrain((int)(d["vol"] | 65), 0, 100);
  return true;
}
// === LED helpers for web ===
const char* effectToStr(Effect e){
  switch(e){
    case SOLID: return "SOLID"; case BREATHE: return "BREATHE";
    case RAINBOW: return "RAINBOW"; case CHASE: return "CHASE";
    case SPARKLE: return "SPARKLE"; case WAVE: return "WAVE";
    case COMET: return "COMET"; case TWINKLE: return "TWINKLE";
  }
  return "SOLID";
}
Effect strToEffect(const String& s){
  String t = s; t.toUpperCase();
  if(t=="SOLID") return SOLID; if(t=="BREATHE") return BREATHE;
  if(t=="RAINBOW") return RAINBOW; if(t=="CHASE") return CHASE;
  if(t=="SPARKLE") return SPARKLE; if(t=="WAVE") return WAVE;
  if(t=="COMET") return COMET; if(t=="TWINKLE") return TWINKLE;
  return SOLID;
}

  // Helper: set & draw + keep device awake during interactions
  // TJpg_ Decoder calls this to push a block of 16-bit pixels
  // TJpg_Decoder calls this to push a block of 16-bit pixels

  /* ========================= Helpers ========================= */
  void addEvent(const String& msg) {
    String stamp;
    struct tm t;
    if (getLocalTime(&t, 50)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
      stamp = String(buf);
    } else {
      stamp = String("t+") + String(millis() / 1000) + "s";
    }
    String line = stamp + " — " + msg;
    eventsBuf[evHead] = line;
    evHead = (evHead + 1) % MAX_EVENTS;
    if (evCount < MAX_EVENTS) evCount++;
    Serial.println(line);
  }

  inline bool rawDetected() {
    int r = digitalRead(IR_PIN);
    return IR_ACTIVE_LOW ? (r == LOW) : (r == HIGH);
  }
void slowMove(int startAngle, int endAngle, int stepDelay = 20) {
  if (!bloomServo.attached())
    bloomServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);

  if (startAngle < endAngle) {
    for (int pos = startAngle; pos <= endAngle; pos++) {
      bloomServo.write(pos);
      delay(stepDelay);   // control speed
    }
  } else {
    for (int pos = startAngle; pos >= endAngle; pos--) {
      bloomServo.write(pos);
      delay(stepDelay);
    }
  }
}

void openFlower(const char* reason) {
  // If already open, do nothing (no extra motion from dash or motion sensor)
  if (isOpen) {
    return;
  }

  // Smooth 25° open
  slowMove(CLOSED_ANGLE, OPEN_ANGLE, 30);
  isOpen = true;
  addEvent(String("🌼 Bloomed (") + reason + ")");
}

void closeFlower(const char* reason) {
  // If already closed, do nothing (no extra motion from dash or buttons)
  if (!isOpen) {
    return;
  }

  // Smooth 25° close
  slowMove(OPEN_ANGLE, CLOSED_ANGLE, 30);
  isOpen = false;
  addEvent(String("🌙 Closed (") + reason + ")");
}

  bool getTimeString(String& out) {
    struct tm t;
    if (!getLocalTime(&t, 100)) return false;
    char buf[16];
    // 12-hour: "%I:%M %p"  (e.g., 09:34 PM)
    // 24-hour: "%H:%M"     (e.g., 21:34)
    strftime(buf, sizeof(buf), "%I:%M %p", &t);
    out = buf;
    return true;
  }

  void drawClockTextCentered(const String& txt, int y, uint16_t fg, uint16_t shadow) {
    tft.setTextSize(3);
    tft.setTextWrap(false);

    int16_t x1, y1; uint16_t w, h;
    tft.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
    int x = (tft.width() - (int)w) / 2;
    if (x < 0) x = 0;

    // soft shadow (offset 1px)
    tft.setTextColor(shadow);
    tft.setCursor(x + 1, y + 1);
    tft.print(txt);

    // main white text
    tft.setTextColor(fg);
    tft.setCursor(x, y);
    tft.print(txt);
  }
/********* LED EFFECT RENDERERS *********/

// SOLID
void renderSolid() {
  fillAll(paletteColor());
}

// BREATHE (global sine on V)
void renderBreathe() {
  static const uint16_t PERIOD = 2600; // ms
  uint16_t phase = (nowMs - tBreathe) % PERIOD;
  float k = 0.5f + 0.5f * sinf(2.0f * PI * (float)phase / PERIOD);
  uint8_t v = (uint8_t)(k * cappedBrightness());
  uint32_t c = paletteColor();
  for (int i=0; i<ACTIVE_LEDS; i++) {
    uint8_t r = (uint8_t)( (uint8_t)(c >> 16)     * (v / (float)cappedBrightness()) );
    uint8_t g = (uint8_t)( (uint8_t)((c >> 8)&255)* (v / (float)cappedBrightness()) );
    uint8_t b = (uint8_t)( (uint8_t)( c       &255)* (v / (float)cappedBrightness()) );
    strip.setPixelColor(i, applyGamma(r,g,b));
  }
  for (int i=ACTIVE_LEDS; i<NUM_LEDS; i++) strip.setPixelColor(i, 0);
  strip.setBrightness(255); // already scaled
  strip.show();
}

// RAINBOW WIPE
void renderRainbow() {
  static const uint8_t speed = 20; // ms/frame
  if (nowMs - tRainbow < speed) return;
  tRainbow = nowMs;

  static uint16_t baseHue = 0;
  for (int i=0; i<ACTIVE_LEDS; i++) {
    uint16_t hue = baseHue + (i * 65536L / ACTIVE_LEDS);
    uint32_t c = strip.ColorHSV(hue, 255, cappedBrightness());
    strip.setPixelColor(i, GAMMA_ON ? strip.gamma32(c) : c);
  }
  for (int i=ACTIVE_LEDS; i<NUM_LEDS; i++) strip.setPixelColor(i, 0);
  strip.setBrightness(255);
  strip.show();
  baseHue += 256;
}

// CHASE
void renderChase() {
  static const uint8_t speed = 35;
  if (nowMs - tChase < speed) return;
  tChase = nowMs;

  static int pos = 0;
  uint32_t headColor = paletteColor();

  for (int i=0; i<ACTIVE_LEDS; i++) {
    uint32_t c = strip.getPixelColor(i);
    uint8_t r = (uint8_t)((c >> 16) & 0xFF);
    uint8_t g = (uint8_t)((c >> 8)  & 0xFF);
    uint8_t b = (uint8_t)( c        & 0xFF);
    r = (r * 180) / 255; g = (g * 180) / 255; b = (b * 180) / 255;
    strip.setPixelColor(i, applyGamma(r,g,b));
  }

  strip.setPixelColor(pos % ACTIVE_LEDS, headColor);
  for (int i=ACTIVE_LEDS; i<NUM_LEDS; i++) strip.setPixelColor(i, 0);
  strip.setBrightness(cappedBrightness());
  strip.show();
  pos = (pos + 1) % ACTIVE_LEDS;
}

// SPARKLE
void renderSparkle() {
  static const uint8_t speed = 25;
  if (nowMs - tSparkle < speed) return;
  tSparkle = nowMs;

  uint32_t base = applyGamma(
    (uint8_t)(PALETTE[st.paletteIndex][0] * 0.15f),
    (uint8_t)(PALETTE[st.paletteIndex][1] * 0.15f),
    (uint8_t)(PALETTE[st.paletteIndex][2] * 0.15f)
  );
  for (int i=0;i<ACTIVE_LEDS;i++) strip.setPixelColor(i, base);

  for (int k=0;k<3;k++) {
    int i = random(ACTIVE_LEDS);
    strip.setPixelColor(i, paletteColor());
  }
  for (int i=ACTIVE_LEDS;i<NUM_LEDS;i++) strip.setPixelColor(i,0);
  strip.setBrightness(cappedBrightness());
  strip.show();
}

// WAVE
void renderWave() {
  static const uint8_t speed = 22;
  if (nowMs - tWave < speed) return;
  tWave = nowMs;

  for (int i=0;i<ACTIVE_LEDS;i++) {
    float p = (float)i / (ACTIVE_LEDS - 1);
    float s = 0.5f + 0.5f * sinf( (nowMs * 0.004f) + p * 6.2831f );
    uint8_t r = (uint8_t)(PALETTE[st.paletteIndex][0] * s);
    uint8_t g = (uint8_t)(PALETTE[st.paletteIndex][1] * s);
    uint8_t b = (uint8_t)(PALETTE[st.paletteIndex][2] * s);
    strip.setPixelColor(i, applyGamma(r,g,b));
  }
  for (int i=ACTIVE_LEDS;i<NUM_LEDS;i++) strip.setPixelColor(i,0);
  strip.setBrightness(cappedBrightness());
  strip.show();
}

// COMET
void renderComet() {
  static const uint8_t speed = 28;
  if (nowMs - tComet < speed) return;
  tComet = nowMs;

  static int head = 0;
  for (int i=0;i<ACTIVE_LEDS;i++) {
    uint32_t c = strip.getPixelColor(i);
    uint8_t r = (uint8_t)((c >> 16) & 0xFF);
    uint8_t g = (uint8_t)((c >> 8)  & 0xFF);
    uint8_t b = (uint8_t)( c        & 0xFF);
    r = (r * 150) / 255; g = (g * 150) / 255; b = (b * 150) / 255;
    strip.setPixelColor(i, applyGamma(r,g,b));
  }

  strip.setPixelColor(head % ACTIVE_LEDS, paletteColor());
  for (int i=ACTIVE_LEDS;i<NUM_LEDS;i++) strip.setPixelColor(i,0);
  strip.setBrightness(cappedBrightness());
  strip.show();
  head = (head + 1) % ACTIVE_LEDS;
}

// TWINKLE
void renderTwinkle() {
  static const uint8_t speed = 25;
  if (nowMs - tTwinkle < speed) return;
  tTwinkle = nowMs;

  for (int i=0;i<ACTIVE_LEDS;i++) {
    uint32_t c = strip.getPixelColor(i);
    uint8_t r = (uint8_t)((c >> 16) & 0xFF);
    uint8_t g = (uint8_t)((c >> 8)  & 0xFF);
    uint8_t b = (uint8_t)( c        & 0xFF);
    r = (r * 220) / 255; g = (g * 190) / 255; b = (b * 220) / 255;
    strip.setPixelColor(i, applyGamma(r,g,b));
  }

  for (int k=0;k<2;k++) {
    int i = random(ACTIVE_LEDS);
    strip.setPixelColor(i, paletteColor());
  }
  for (int i=ACTIVE_LEDS;i<NUM_LEDS;i++) strip.setPixelColor(i,0);
  strip.setBrightness(cappedBrightness());
  strip.show();
}
void handleLedTest(){
  // fill red, green, blue quickly
  uint32_t cols[3] = {
    strip.Color(255,0,0), strip.Color(0,255,0), strip.Color(0,0,255)
  };
  for(int k=0;k<3;k++){
    for (int i=0;i<ACTIVE_LEDS;i++) strip.setPixelColor(i, cols[k]);
    for (int i=ACTIVE_LEDS;i<NUM_LEDS;i++) strip.setPixelColor(i, 0);
    strip.setBrightness(120);
    strip.show();
    delay(250);
  }
  server.send(200,"application/json; charset=utf-8","{\"ok\":true}");
}


/********* ROTARY INPUT *********/
void onRotate(int dir) {
  lastInputAt = nowMs;

  static unsigned long lastTick = 0;
  unsigned long dt = nowMs - lastTick;
  lastTick = nowMs;
  int step = (dt < 40) ? 4 : (dt < 120 ? 2 : 1);

  if (adjustBrightnessMode) {
    int newB = st.brightness + (dir > 0 ? 1 : -1) * (8 * step);
    st.brightness = clamp8(newB, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    saveState();
  } else {
    if (dir > 0) st.paletteIndex = (st.paletteIndex + step) % PALETTE_SIZE;
    else         st.paletteIndex = (st.paletteIndex + PALETTE_SIZE - step%PALETTE_SIZE) % PALETTE_SIZE;
    saveState();
  }
}

void pollEncoder() {
  int clk = digitalRead(CLK_PIN);
  if (clk != lastCLK && clk == LOW) {
    int dir = (digitalRead(DT_PIN) != clk) ? +1 : -1;
    onRotate(dir);
  }
  lastCLK = clk;

  bool btn = digitalRead(SW_PIN);
  if (btnPrev == HIGH && btn == LOW) {           // pressed
    btnDownAt = nowMs;
    lastInputAt = nowMs;
    adjustBrightnessMode = true;                 // press+turn = brightness
  } else if (btnPrev == LOW && btn == HIGH) {    // released
    unsigned long held = nowMs - btnDownAt;
    lastInputAt = nowMs;
    adjustBrightnessMode = false;

    if (held >= LONG_PRESS_MS) {
      st.nightMode = !st.nightMode;             // long-press: night mode
      st.powered   = true;
      saveState();
   } else if (held > 20 && held < LONG_PRESS_MS && (nowMs - btnDownAt) > ROTARY_DEBOUNCE_MS) {
      st.effect = (Effect)((st.effect + 1) % EFFECT_COUNT); // short press: next effect
      saveState();
    }
  }
  btnPrev = btn;
}

  /* ========================= DASHBOARD HTML ========================= */
static const char HTML_PAGE[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>BloomX</title>
<link rel="icon" type="image/svg+xml" sizes="any"
  href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'%3E%3Ctext x='50%25' y='55%25' text-anchor='middle' dominant-baseline='middle' font-size='86'%3E%F0%9F%8C%B8%3C/text%3E%3C/svg%3E">

<style>
:root{
  --bg:#fff5fb; --ink:#161016; --muted:#6a4f62; --card:#ffffffee; --brand:#b40063; --border:#f2cfe0; --ring:#ffd7e9;
  --btn:#b40063; --btn-ink:#fff; --meshA:#ffe8f3; --meshB:#ffeefc; --meshC:#fff7ef;
}
:root.dark{
  --bg:#1a0f17; --ink:#f8edf3; --muted:#d8b8c9; --card:#24141f; --border:#3c2231; --ring:#5a2a43;
  --btn:#ff4b7d; --btn-ink:#151015; --brand:#ff6aa0; --meshA:#2a1823; --meshB:#2b1a28; --meshC:#2a1a21;
}

*{box-sizing:border-box}
html,body{height:100%}
body{
  margin:0;color:var(--ink);background:var(--bg);font-family:ui-sans-serif,system-ui,Segoe UI,Roboto,Arial;
  background-image:
    radial-gradient(900px 420px at 10% -10%, var(--meshA), transparent 60%),
    radial-gradient(900px 520px at 110% 0%, var(--meshB), transparent 60%),
    radial-gradient(700px 640px at 50% 120%, var(--meshC), transparent 60%);
  background-attachment: fixed;
}

body.noise::before{
  content:""; position:fixed; inset:0; pointer-events:none; z-index:7;
  background-image:url('data:image/svg+xml;utf8,\
  <svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">\
    <filter id="n"><feTurbulence baseFrequency="0.9" numOctaves="2" stitchTiles="stitch"/></filter>\
    <rect width="100%" height="100%" filter="url(%23n)" opacity="0.15"/></svg>');
  mix-blend-mode:multiply;
}

.app{display:flex; min-height:100%}
.sidebar{
  width:260px; flex:0 0 260px; padding:18px; position:sticky; top:0; height:100vh; overflow:auto;
  border-right:1px solid var(--border); background:linear-gradient(180deg, color-mix(in srgb, var(--card) 90%, transparent), transparent 30%);
  z-index:3;
}
.brand{display:flex; align-items:center; gap:10px; margin-bottom:14px}
.brand .logo{width:36px;height:36px; display:grid; place-items:center; border-radius:12px; background:var(--ring)}
.brand h1{margin:0; font-size:20px; letter-spacing:.3px}
.small{color:var(--muted); font-size:12px}

.nav{display:flex; flex-direction:column; gap:8px; margin:14px 0 18px}
.nav button{
  text-align:left; width:100%; padding:12px 12px; border-radius:12px; border:1px solid var(--border);
  background:var(--card); color:var(--ink); cursor:pointer; font-weight:650;
}
.nav button.active{ border-color:var(--ring); box-shadow:0 0 0 3px var(--ring) inset }

.controls{display:flex; gap:8px; flex-wrap:wrap; margin-top:8px}
.btn{border:2px solid var(--btn); background:var(--btn); color:var(--btn-ink); padding:10px 14px; border-radius:12px; font-weight:800; cursor:pointer}
.btn.alt{background:transparent; color:var(--btn)}
.pin{display:inline-block; padding:4px 8px; border-radius:999px; border:1px dashed var(--border); color:var(--muted); font-size:11px}

.main{flex:1; padding:18px; z-index:2; position:relative}
.card{background:var(--card); border:1px solid var(--border); border-radius:16px; padding:16px; box-shadow:0 10px 24px rgba(180,0,99,.12)}
.grid{display:grid; gap:12px}
@media(min-width:960px){ .grid.two{grid-template-columns:1fr 1fr} }
label{display:block; font-weight:650; margin:8px 0 6px}
input[type=text], input[type=time], select, input[type=color]{
  width:100%; padding:10px; border-radius:12px; border:1px solid var(--border); background:transparent; color:var(--ink)
}
input[type=color]{ height:44px; padding:0; }
.hex-note{font-size:12px; color:var(--muted); margin-top:6px}
.list{list-style:none; margin:0; padding:0}
.list li{padding:10px 12px; border-top:1px dashed var(--border); display:flex; align-items:center; justify-content:space-between; gap:10px}
.row{display:flex; gap:10px; flex-wrap:wrap}
footer{margin-top:10px; color:var(--muted); font-size:12px}

/* effects above content */
.fall-layer{ position:fixed; inset:0; pointer-events:none; overflow:hidden; z-index:6 }
.petal{
  position:absolute; left:var(--x,50%); top:-14vh;
  width:var(--w,28px); height:var(--h,44px); opacity:.95;
  filter: drop-shadow(0 6px 16px rgba(255,150,190,.22));
  animation: fallTop var(--dur,22s) linear infinite, swayRotate var(--sway,5.2s) ease-in-out infinite;
  animation-delay: var(--delay,0s), calc(var(--delay,0s)*.35);
  transform: rotate(var(--r,0deg));
}
.petal svg{width:100%; height:100%}
.petal .p1{fill:#ffb7d4}
.petal .p2{fill:#ffc6de; opacity:.7}

.sparkfall{
  position:absolute; left:var(--x,50%); top:-12vh;
  font-size:var(--s,26px);
  color:var(--brand);
  opacity:.9;
  text-shadow:0 4px 12px rgba(180,0,99,.30);
  animation: fallTop var(--dur,24s) linear infinite, swayRotate var(--sway,4.9s) ease-in-out infinite;
  animation-delay: var(--delay,0s), calc(var(--delay,0s)*.40);
}

@keyframes fallTop{ 0%{ top:-14vh } 100%{ top:112vh } }
@keyframes swayRotate{ 0%{ transform:translateX(-8px) rotate(var(--r,0deg)) } 50%{ transform:translateX(10px) rotate(calc(var(--r,0deg)+8deg)) } 100%{ transform:translateX(-8px) rotate(var(--r,0deg)) } }

.toast{position:fixed; right:16px; bottom:16px; background:var(--card); color:var(--ink);
  border:1px solid var(--border); padding:10px 12px; border-radius:12px; box-shadow:0 6px 16px rgba(0,0,0,.12);
  opacity:0; transform:translateY(8px); transition:.2s; pointer-events:none; z-index:8}
.toast.show{opacity:1; transform:translateY(0)}

@media(max-width:880px){
  .sidebar{position:fixed; inset:0 auto 0 0; height:100dvh; transform:translateX(-100%); transition:.2s; background:var(--card)}
  .sidebar.open{transform:none; box-shadow:6px 0 24px rgba(0,0,0,.2)}
  .main{padding-top:56px}
  .topbar{position:fixed; top:0; left:0; right:0; height:56px; display:flex; align-items:center; gap:8px; padding:0 12px;
    background:var(--card); border-bottom:1px solid var(--border); z-index:9}
  .hamb{font-size:22px; background:transparent; border:1px solid var(--border); border-radius:10px; padding:6px 10px}
}

/* LED panel extras */
kbd{font:600 12px/1 ui-monospace,SFMono-Regular,Menlo,monospace; background:var(--border); color:var(--ink); padding:2px 6px; border-radius:6px}
.swatches{display:grid; grid-template-columns:repeat(auto-fill,minmax(28px,1fr)); gap:8px}
.swatch{position:relative; width:100%; padding-top:100%; border-radius:10px; border:1px solid var(--border); cursor:pointer}
.swatch.sel{outline:3px solid var(--ring); outline-offset:1px}
.swatch .tag{position:absolute; right:4px; bottom:4px; font-size:9px; background:#00000033; color:#fff; padding:1px 4px; border-radius:6px}
.range-row{display:flex; align-items:center; gap:10px}
</style>
</head>
<body class="noise">
<div class="app" id="app">

  <!-- Mobile topbar -->
  <div class="topbar" id="topbar" hidden>
    <button class="hamb" id="hamb">☰</button>
    <div style="font-weight:800">BloomX</div>
    <span class="pin" id="ipTop">loading…</span>
    <div style="margin-left:auto" class="row">
      <button class="btn alt" id="themeTop">🌙</button>
    </div>
  </div>

  <!-- SIDEBAR -->
  <aside class="sidebar" id="sidebar">
    <div class="brand">
      <div class="logo">🌸</div>
      <div>
        <h1>BloomX</h1>
        <div class="small">ESP32-S3 • GC9A01/ST7789 • I²S • IR • Touch</div>
      </div>
    </div>

    <div class="nav" role="tablist" aria-label="Sections">
      <button class="tab active" data-panel="home" aria-selected="true">🏠 Home</button>
      <button class="tab" data-panel="ctrl">🎛️ Controls & Events</button>
      <button class="tab" data-panel="oled">🖼️ OLED Settings</button>
      <button class="tab" data-panel="alarm">⏰ Alarms</button>
      <button class="tab" data-panel="led">💡 LED</button>
    </div>

    <div class="controls">
      <button class="btn alt" id="themeBtn" title="Toggle dark mode">🌙 Theme</button>
    </div>

    <div style="margin-top:10px">
      <div class="pin">IP <span id="ip">loading…</span></div>
    </div>

    <div style="margin-top:12px">
      <label style="margin-bottom:6px">Style</label>
      <div class="row">
        <label class="pin"><input id="fxSparkMode" type="checkbox"> Sparkle mode</label>
        <label class="pin"><input id="fxTrail" type="checkbox"> Cursor trail</label>
        <label class="pin"><input id="fxNoise" type="checkbox" checked> Grain</label>
      </div>
    </div>

    <footer style="margin-top:18px"></footer>
  </aside>

  <!-- MAIN -->
  <main class="main">
    <!-- Home -->
    <section class="panel card" data-panel="home">
      <h2>Welcome to BloomX</h2>
      <p>BloomX is a friendly, expressive flower that <b>blooms with motion</b>, reacts to <b>touch</b>, and speaks with a <b>push-to-talk</b> button. Inside is an ESP32-S3 driving a round TFT (GC9A01/ST7789), an IR motion sensor, a capacitive touch pad, an I²S microphone (INMP441) and speaker amp (MAX98357A). When you interact, BloomX listens, thinks, and talks back — and the screen shows messages and moods that you control here.</p>
      <div class="grid two" style="margin-top:8px">
        <div class="card">
          <b>At a glance</b>
          <ul class="list">
            <li>ESP32-S3 Web Dashboard</li>
            <li>Motion-triggered bloom, auto-sleep</li>
            <li>Touch → cute welcome → main message</li>
            <li>Push-to-talk: STT → Chat → TTS</li>
          </ul>
        </div>
        <div class="card">
          <b>Display & Audio</b>
          <ul class="list">
            <li>Round TFT: GC9A01/ST7789</li>
            <li>Custom themes & presets</li>
            <li>I²S mic + stereo out</li>
            <li>Alarms with visual ring</li>
          </ul>
        </div>
      </div>
      <div class="card" style="margin-top:10px">
        <b>Project Managers</b>
        <div class="row" style="margin-top:6px">
          <span class="pin">Project Manager 1: <b>Priya Patel</b></span>
          <span class="pin">Project Manager 2: <b>Danielle Eleanor Rolle</b></span>
        </div>
      </div>
    </section>

    <!-- Controls & Events -->
    <section class="panel card" data-panel="ctrl" hidden>
      <h2>Controls</h2>
      <div class="row" style="margin-bottom:8px">
        <button class="btn" onclick="send('/open')">Open Flower</button>
        <button class="btn alt" onclick="send('/close')">Close Flower</button>
        <button class="btn alt" onclick="send('/clear')">Clear Log</button>
        <span class="pin">Motion auto-opens • Inactivity auto-closes</span>
      </div>
      <h3 style="margin-top:8px">Event Log</h3>
      <ul id="log" class="list" aria-live="polite"></ul>
    </section>

    <!-- OLED Settings -->
    <section class="panel card" data-panel="oled" hidden>
      <h2>OLED Settings</h2>
      <div class="grid two">
        <div>
          <label for="msg">Main message</label>
          <input id="msg" type="text" placeholder="Hello BloomX!">
          <label for="welcome">Welcome (on touch)</label>
          <input id="welcome" type="text" placeholder="Hey Beautiful ;)">
          <label for="preset">Preset</label>
          <select id="preset">
            <option value="">— choose a preset —</option>
            <option>Keep growing.</option>
            <option>You belong here.</option>
            <option>Small steps, big change.</option>
            <option>Bloom where you are.</option>
            <option>Breathe. Try again.</option>
            <option>Progress over perfect.</option>
          </select>
        </div>
        <div>
          <label for="fg">Text color</label>
          <input id="fg" type="color" value="#FFFFFF">
          <div id="fgHex" class="hex-note">#FFFFFF</div>
          <label for="bg">Background color</label>
          <input id="bg" type="color" value="#000000">
          <div id="bgHex" class="hex-note">#000000</div>
        </div>
      </div>
      <div class="row" style="margin-top:10px">
        <button class="btn" onclick="save()">Save</button>
        <button class="btn alt" onclick="showNow()">Show Now</button>
        <button class="btn" onclick="applyPreset()">Apply Preset</button>
        <button class="btn alt" onclick="send('/refresh')">Sleep</button>
        <span id="status" class="pin">Loading…</span>
      </div>
    </section>

    <!-- Alarms -->
    <section class="panel card" data-panel="alarm" hidden>
      <h2>Alarms</h2>
      <div class="row" style="align-items:flex-end">
        <div><label for="alarmTime">Time</label><input id="alarmTime" type="time" value="07:30"></div>
        <div>
          <label>Days</label>
          <div class="row">
            <label class="pin"><input class="dchk" type="checkbox" value="0"> Sun</label>
            <label class="pin"><input class="dchk" type="checkbox" value="1"> Mon</label>
            <label class="pin"><input class="dchk" type="checkbox" value="2"> Tue</label>
            <label class="pin"><input class="dchk" type="checkbox" value="3"> Wed</label>
            <label class="pin"><input class="dchk" type="checkbox" value="4"> Thu</label>
            <label class="pin"><input class="dchk" type="checkbox" value="5"> Fri</label>
            <label class="pin"><input class="dchk" type="checkbox" value="6"> Sat</label>
          </div>
          <span class="small">None checked = every day</span>
        </div>
        <div style="flex:1"><label for="alarmLabel">Label</label><input id="alarmLabel" type="text" placeholder="Wake up"></div>
        <button class="btn" onclick="addAlarm()">Add</button>
        <button class="btn alt" onclick="testAlarm()">Test</button>
        <button class="btn" style="background:#ff4b7d" onclick="stopAlarm()">Stop</button>
      </div>
      <div class="row" style="margin-top:10px; align-items:center">
        <label class="pin">Sound
          <select id="alarmSoundSel">
            <option value="classic">Classic</option>
            <option value="pulse">Pulse</option>
            <option value="rising">Rising</option>
            <option value="bell">Bell</option>
          </select>
        </label>
        <label class="pin">Volume
          <input id="alarmVol" type="range" min="0" max="100" value="65" style="vertical-align:middle">
          <span id="alarmVolVal">65%</span>
        </label>
        <button class="btn alt" onclick="saveAlarmCfg()">Save</button>
      </div>

      <ul id="alarmsList" class="list" style="margin-top:10px"></ul>
    </section>

    <!-- LED -->
    <section class="panel card" data-panel="led" hidden>
      <h2>LED Strip</h2>
      <div class="grid two">
        <div>
          <label class="pin" style="display:inline-flex; align-items:center; gap:8px; padding:8px 10px;">
            <input id="ledPower" type="checkbox"> Power
          </label>

          <label for="ledEffect">Effect</label>
          <select id="ledEffect">
            <option value="SOLID">Solid</option>
            <option value="BREATHE">Breathe</option>
            <option value="RAINBOW">Rainbow</option>
            <option value="CHASE">Chase</option>
            <option value="SPARKLE">Sparkle</option>
            <option value="WAVE">Wave</option>
            <option value="COMET">Comet</option>
            <option value="TWINKLE">Twinkle</option>
          </select>
          <div class="row" style="margin-top:6px">
            <button class="btn" id="ledApplyEffect">Apply Effect</button>
            <button class="btn alt" id="ledOverallDefault">Overall Default</button>
          </div>

          <label style="margin-top:10px">Brightness</label>
          <div class="range-row">
            <input id="ledBri" type="range" min="5" max="160" value="100">
            <span class="pin" id="ledBriVal">100/160</span>
          </div>
          <div class="row" style="margin-top:6px">
            <button class="btn" id="ledSaveBrightness">Save Brightness</button>
            <label class="pin" style="display:inline-flex; align-items:center; gap:8px; padding:8px 10px; margin:0">
              <input id="ledNight" type="checkbox"> Night mode (warm & dim)
            </label>
          </div>

          <div class="row" style="margin-top:10px">
            <button class="btn alt" id="ledRandBtn">Random Color</button>
            <button class="btn" id="ledApplyColor">Apply Color</button>
          </div>
        </div>

        <div>
          <label>Palette</label>
          <div id="ledSwatches" class="swatches" aria-label="LED palette choices"></div>
          <div class="hex-note" id="ledSelHex">—</div>
          <!-- Effect Colors UI removed -->
        </div>
      </div>
    </section>

  </main>
</div>

<!-- Falling layers ABOVE UI -->
<div id="petalLayer" class="fall-layer" aria-hidden="true"></div>
<div id="sparkleLayer" class="fall-layer" aria-hidden="true"></div>

<!-- cursor trail -->
<canvas id="trail" style="position:fixed; inset:0; pointer-events:none; z-index:5;"></canvas>

<div class="toast" id="toast">Saved</div>

<script>
// ----- responsive -----
const sidebar = document.getElementById('sidebar');
const topbar = document.getElementById('topbar');
const hamb = document.getElementById('hamb');
function applyMobile(){
  const isMobile = window.matchMedia('(max-width:880px)').matches;
  topbar.hidden = !isMobile;
  if(!isMobile) sidebar.classList.remove('open');
}
applyMobile(); addEventListener('resize', applyMobile);
if(hamb){ hamb.onclick = ()=> sidebar.classList.toggle('open'); }

// ----- theme -----
const root = document.documentElement;
const themeBtn = document.getElementById('themeBtn');
const themeTop = document.getElementById('themeTop');

function applySavedTheme(){
  const saved = localStorage.getItem('bloomx_theme');
  if(saved === 'dark'){ root.classList.add('dark'); }
  updateThemeIcon();
}
function toggleTheme(){
  root.classList.toggle('dark');
  localStorage.setItem('bloomx_theme', root.classList.contains('dark') ? 'dark' : 'light');
  updateThemeIcon();
}
function updateThemeIcon(){
  const isDark = root.classList.contains('dark');
  if(themeBtn) themeBtn.textContent = isDark ? '☀️ Theme' : '🌙 Theme';
  if(themeTop) themeTop.textContent = isDark ? '☀️' : '🌙';
}
applySavedTheme();
if(themeBtn) themeBtn.onclick = toggleTheme;
if(themeTop) themeTop.onclick = toggleTheme;

// ----- panels -----
const tabs = [...document.querySelectorAll('.tab')];
const panels = [...document.querySelectorAll('.panel')];
tabs.forEach(t=>t.onclick=()=>{
  tabs.forEach(x=>x.classList.remove('active')); t.classList.add('active');
  const name = t.getAttribute('data-panel');
  panels.forEach(p=>p.hidden = (p.getAttribute('data-panel')!==name));
  if (window.matchMedia('(max-width:880px)').matches) sidebar.classList.remove('open');
});

// ----- helpers -----
const $ = id => document.getElementById(id);
const toast = (msg='OK')=>{
  const el = $('toast'); el.textContent = msg; el.classList.add('show'); setTimeout(()=>el.classList.remove('show'), 1200);
};
const send = async (path, body=null)=>{
  try{
    await fetch(path,{method:'POST', headers: body? {'Content-Type':'application/json'}:undefined, body: body? JSON.stringify(body):undefined});
    toast('OK');
  }catch(e){ toast('Failed'); }
};
$('ip').textContent = location.host || 'device'; const ipTop=$('ipTop'); if(ipTop) ipTop.textContent = $('ip').textContent;

// ----- keyboard (kept, no UI tip) -----
addEventListener('keydown', (e)=>{
  if (['INPUT','SELECT','TEXTAREA'].includes((e.target.tagName||'').toUpperCase())) return;
  if (e.key==='o') send('/open');
  if (e.key==='c') send('/close');
});

// ----- Events -----
async function poll(){
  try{
    const data = await (await fetch('/events')).json();
    const ul = $('log'); if(!ul) return;
    ul.innerHTML='';
    (data.events||[]).slice(-200).forEach(line=>{
      const li=document.createElement('li'); li.textContent=line; ul.appendChild(li);
    });
  }catch{}
}
setInterval(poll,1500); poll();

// ----- State (OLED) -----
async function loadState(){
  try{
    const st = await (await fetch('/state')).json();
    $('msg').value = st.main||''; $('welcome').value = st.welcome||'';
    $('fg').value = st.fg||'#FFFFFF'; $('bg').value = st.bg||'#000000';
    $('fgHex').textContent = $('fg').value.toUpperCase();
    $('bgHex').textContent = $('bg').value.toUpperCase();
    if (st.preset) $('preset').value = st.preset;
    $('status').textContent='Loaded';
  }catch{ $('status').textContent='Failed'; }
}
$('fg').addEventListener('input', ()=> $('fgHex').textContent = $('fg').value.toUpperCase());
$('bg').addEventListener('input', ()=> $('bgHex').textContent = $('bg').value.toUpperCase());
async function save(){ await send('/set',{main:$('msg').value, welcome:$('welcome').value, fg:$('fg').value, bg:$('bg').value, preset:$('preset').value}); }
async function showNow(){ await send('/show',{}); }
async function applyPreset(){ await send('/apply',{preset:$('preset').value}); }
loadState();

// ----- Alarms -----
async function loadAlarms(){
  const data = await (await fetch('/alarms')).json().catch(()=>({alarms:[]}));
  const ul = $('alarmsList'); if(!ul) return; ul.innerHTML='';
  (data.alarms||[]).forEach((a,i)=>{
    const hh=String(a.hour).padStart(2,'0'); const mm=String(a.minute).padStart(2,'0');
    let days = a.dowMask===0 ? 'Every day' : [];
    if(a.dowMask!==0){
      const names=['Sun','Mon','Tue','Wed','Thu','Fri','Sat'];
      for(let d=0; d<7; d++) if(a.dowMask & (1<<d)) days.push(names[d]);
      days = days.join(', ');
    }
    const li=document.createElement('li');
    li.innerHTML = `<b>${hh}:${mm}</b> — ${days} — ${a.label||''}`;
    const right=document.createElement('div'); right.className='row';
    const lab=document.createElement('label'); lab.className='pin';
    const chk=document.createElement('input'); chk.type='checkbox'; chk.checked=!!a.enabled;
    chk.onchange=()=>fetch('/alarms/toggle',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({i,enabled:chk.checked})});
    lab.append(chk, document.createTextNode(' Enabled'));
    const del=document.createElement('button'); del.className='btn alt'; del.textContent='Delete';
    del.onclick=()=>fetch('/alarms/delete?i='+i,{method:'POST'}).then(loadAlarms);
    right.append(lab, del); li.appendChild(right); ul.appendChild(li);
  });
}
async function addAlarm(){
  const [hh,mm]=($('alarmTime').value||'07:30').split(':').map(x=>parseInt(x,10));
  let mask=0; document.querySelectorAll('.dchk:checked').forEach(ch=>mask|=(1<<parseInt(ch.value,10)));
  const label=$('alarmLabel').value||'';
  await fetch('/alarms',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({hour:hh,minute:mm,dowMask:mask,enabled:true,label})});
  $('alarmLabel').value=''; document.querySelectorAll('.dchk').forEach(ch=>ch.checked=false); loadAlarms(); toast('Alarm added');
}
async function testAlarm(){ await send('/alarms/test'); }
async function stopAlarm(){ await send('/alarm_stop'); }
async function loadAlarmCfg(){
  try{
    const c = await (await fetch('/alarm/config')).json();
    const sel = document.getElementById('alarmSoundSel');
    const vol = document.getElementById('alarmVol');
    const vv  = document.getElementById('alarmVolVal');
    if (sel) {
      const modes = ['classic','pulse','rising','bell'];
      sel.value = modes[c.mode ?? 0] || 'classic';
    }
    if (vol && vv) {
      vol.value = Math.max(0, Math.min(100, c.volume ?? 65));
      vv.textContent = vol.value + '%';
    }
  }catch(e){}
}
function wireVolLabel(){
  const vol = document.getElementById('alarmVol');
  const vv  = document.getElementById('alarmVolVal');
  if (vol && vv) vol.addEventListener('input', ()=> vv.textContent = vol.value+'%');
}
async function saveAlarmCfg(){
  const sel = document.getElementById('alarmSoundSel');
  const vol = document.getElementById('alarmVol');
  const body = { mode: sel ? sel.value : 'classic', volume: vol ? parseInt(vol.value,10) : 65 };
  await fetch('/alarm/config', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify(body)});
  toast('Alarm sound saved');
}
wireVolLabel(); loadAlarmCfg(); loadAlarms();

/* ======= LED PANEL LOGIC ======= */
const LED_EFFECTS = ['SOLID','BREATHE','RAINBOW','CHASE','SPARKLE','WAVE','COMET','TWINKLE'];

// *** Curated firmware palette (10 colors; index 0..9) ***
const LED_COLORS = [
  '#FF64B4', // 0 BloomX Pink   (255,100,180)
  '#64B4FF', // 1 Sky           (100,180,255)
  '#B482FF', // 2 Lavender      (180,130,255)
  '#FFDCAA', // 3 Warm White    (255,220,170)
  '#FF8C28', // 4 Sunset        (255,140, 40)
  '#78FFC8', // 5 Mint          (120,255,200)
  '#DCB4FF', // 6 Lilac         (220,180,255)
  '#FFAA82', // 7 Peach         (255,170,130)
  '#FFE678', // 8 Soft Gold     (255,230,120)
  '#50C8FF'  // 9 Ocean         ( 80,200,255)
];

// A cozy overall default (sleep-ish)
const OVERALL_DEFAULT = {
  powered: true,
  nightMode: false,
  effect: "SOLID",
  paletteIndex: 3,   // Warm White
  brightness: 80
};

let ledState = { powered:true, effect:'SOLID', brightness:100, nightMode:false, paletteIndex:0 };
let pendingPaletteIndex = null;
let pendingEffect = null;

function renderSwatches(){
  const box = $('ledSwatches'); if(!box) return;
  box.innerHTML = '';
  LED_COLORS.forEach((hex,i)=>{
    const b=document.createElement('button');
    b.className='swatch';
    b.style.background=hex;
    b.title=hex;
    const tag=document.createElement('span'); tag.className='tag'; tag.textContent=i;
    b.appendChild(tag);
    b.onclick=()=>{
      pendingPaletteIndex = i;
      highlightSwatch(i);
      $('ledSelHex').textContent = hex.toUpperCase();
    };
    box.appendChild(b);
  });
  const current = ledState.paletteIndex||0;
  highlightSwatch(current);
  $('ledSelHex').textContent = (LED_COLORS[current]||'').toUpperCase();
}
function highlightSwatch(i){
  [...document.querySelectorAll('#ledSwatches .swatch')].forEach((el,idx)=>el.classList.toggle('sel', idx===i));
}

async function applyColor(){
  const idx = (pendingPaletteIndex!==null)? pendingPaletteIndex : (ledState.paletteIndex||0);
  await fetch('/led/set',{method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({paletteIndex:idx})});
  ledState.paletteIndex = idx;
  toast('Color applied');
}

async function applyEffect(){
  const eff = (pendingEffect!==null)? pendingEffect : ($('ledEffect').value||'SOLID');
  await fetch('/led/set',{method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({effect:eff})});
  ledState.effect = eff;
  toast('Effect applied');
}

async function saveBrightness(){
  const v = parseInt($('ledBri').value, 10);
  await fetch('/led/set', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify({ brightness: v })
  });
  toast('Brightness saved');
}

async function overallDefault(){
  await fetch('/led/set', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body: JSON.stringify(OVERALL_DEFAULT)
  });
  $('ledPower').checked = true;
  $('ledNight').checked = false;
  $('ledEffect').value = 'SOLID';
  $('ledBri').value = OVERALL_DEFAULT.brightness;
  $('ledBriVal').textContent = `${OVERALL_DEFAULT.brightness}/160`;
  highlightSwatch(OVERALL_DEFAULT.paletteIndex);
  $('ledSelHex').textContent = LED_COLORS[OVERALL_DEFAULT.paletteIndex];
  toast('LEDs set to overall default');
}

async function loadLed(){
  $('ledPower').checked = !!ledState.powered;
  $('ledEffect').value = LED_EFFECTS.includes(ledState.effect)? ledState.effect : 'SOLID';
  $('ledBri').value = Math.max(5, Math.min(160, parseInt(ledState.brightness||100,10)));
  $('ledBriVal').textContent = `${$('ledBri').value}/160`;
  $('ledNight').checked = !!ledState.nightMode;
  renderSwatches();
}

// wire inputs
$('ledPower').addEventListener('change', e=> {
  fetch('/led/set',{method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({powered:e.target.checked})});
});
$('ledEffect').addEventListener('change', e=> { pendingEffect = e.target.value; });
$('ledBri').addEventListener('input', e=>{
  const v = parseInt(e.target.value,10);
  $('ledBriVal').textContent = `${v}/160`;
  fetch('/led/set',{method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({brightness:v})});
});
$('ledNight').addEventListener('change', e=>{
  fetch('/led/set',{method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({nightMode:e.target.checked})});
});
$('ledRandBtn').addEventListener('click', ()=>{
  const i = Math.floor(Math.random()*LED_COLORS.length);
  pendingPaletteIndex = i;
  highlightSwatch(i);
  $('ledSelHex').textContent = LED_COLORS[i].toUpperCase();
});
$('ledApplyColor').addEventListener('click', applyColor);
$('ledApplyEffect').addEventListener('click', applyEffect);
$('ledSaveBrightness').addEventListener('click', saveBrightness);
$('ledOverallDefault').addEventListener('click', overallDefault);

// initial load
loadLed();

/* ======= Visual FX (petals/sparkles/trail) ======= */
const Lpetal = $('petalLayer');
const Lspark = $('sparkleLayer');

function petalSVG(){
  return `<svg viewBox="0 0 100 150" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
    <path class="p1" d="M50 4 C74 16 92 46 50 146 C8 46 26 16 50 4 Z"/>
    <path class="p2" d="M58 12 C72 22 80 40 50 120 C46 92 44 70 58 12 Z"/>
  </svg>`;
}
let petalTimer=null;
function spawnPetal(){
  const el=document.createElement('div');
  el.className='petal';
  el.style.setProperty('--x', (4 + Math.random()*92)+'%');
  el.style.setProperty('--w', (22 + Math.random()*12)+'px');
  el.style.setProperty('--h', (34 + Math.random()*18)+'px');
  el.style.setProperty('--dur', (20 + Math.random()*12)+'s');
  el.style.setProperty('--sway', (4.8 + Math.random()*2.0)+'s');
  el.style.setProperty('--delay', (-Math.random()*16)+'s');
  el.style.setProperty('--r', ((Math.random()*20-10)+'deg'));
  el.innerHTML = petalSVG();
  Lpetal.appendChild(el);
  setTimeout(()=>el.remove(), 32000);
}
function startPetals(){
  if(petalTimer) return;
  for(let i=0;i<10;i++) spawnPetal();
  petalTimer = setInterval(spawnPetal, 2600);
}
startPetals();

const glyphs=['✦','❀','✧','❁'];
let sparkTimer=null;
function spawnSpark(){
  const el=document.createElement('span');
  el.className='sparkfall';
  el.textContent = glyphs[Math.floor(Math.random()*glyphs.length)];
  el.style.setProperty('--x', (4 + Math.random()*92)+'%');
  el.style.setProperty('--s', (20 + Math.random()*12)+'px');
  el.style.setProperty('--dur', (22 + Math.random()*12)+'s');
  el.style.setProperty('--sway', (4.6 + Math.random()*1.8)+'s');
  el.style.setProperty('--delay', (-Math.random()*18)+'s');
  el.style.setProperty('--r', ((Math.random()*20-10)+'deg'));
  Lspark.appendChild(el);
  setTimeout(()=>el.remove(), 34000);
}
function startSpark(){
  if(sparkTimer) return;
  for(let i=0;i<6;i++) spawnSpark();
  sparkTimer = setInterval(spawnSpark, 2800);
}
function stopSpark(){
  if(sparkTimer){ clearInterval(sparkTimer); sparkTimer=null; }
  while (Lspark.firstChild) Lspark.removeChild(Lspark.firstChild);
}

const fxSparkMode = $('fxSparkMode'), fxTrail = $('fxTrail'), fxNoise = $('fxNoise');

function loadFXPrefs(){
  const sm = localStorage.getItem('fxSparkMode')==='1';
  const trail = localStorage.getItem('fxTrail')==='1';
  const noise = (localStorage.getItem('fxNoise')!=='0');

  fxSparkMode.checked = sm; fxTrail.checked = trail; fxNoise.checked = noise;
  document.body.classList.toggle('noise', noise);

  if (sm) startSpark(); else stopSpark();
  if (trail) startTrailLoop(); else stopTrailLoop();
}
function saveFXPrefs(){
  localStorage.setItem('fxSparkMode', fxSparkMode.checked?'1':'0');
  localStorage.setItem('fxTrail', fxTrail.checked?'1':'0');
  localStorage.setItem('fxNoise', fxNoise.checked?'1':'0');
  document.body.classList.toggle('noise', fxNoise.checked);
  if (fxSparkMode.checked) startSpark(); else stopSpark();
  if (fxTrail.checked) startTrailLoop(); else stopTrailLoop();
}
fxSparkMode.addEventListener('change', saveFXPrefs);
fxNoise.addEventListener('change', saveFXPrefs);
fxTrail.addEventListener('change', saveFXPrefs);

/* Cursor trail */
const TRAIL = $('trail'); const ctx = TRAIL.getContext('2d');
function resizeTrail(){ TRAIL.width=innerWidth; TRAIL.height=innerHeight; }
addEventListener('resize', resizeTrail); resizeTrail();

const DOT_LIFE = 3500;
const DOT_MIN_R = 2, DOT_MAX_R = 6;
let particles = [];
let raf = null;

function startTrailLoop(){
  if(raf) return;
  const loop = (now)=>{
    ctx.clearRect(0,0,TRAIL.width,TRAIL.height);
    const brand = getComputedStyle(document.documentElement).getPropertyValue('--brand').trim() || '#b40063';
    for (let i = particles.length - 1; i >= 0; i--){
      const p = particles[i];
      const age = now - p.born;
      if (age >= DOT_LIFE){ particles.splice(i,1); continue; }
      const t = 1 - (age / DOT_LIFE);
      ctx.globalAlpha = 0.28 * t;
      ctx.fillStyle = brand;
      ctx.beginPath(); ctx.arc(p.x, p.y, p.r, 0, Math.PI*2); ctx.fill();
    }
    ctx.globalAlpha = 1;
    if (fxTrail.checked || particles.length) { raf = requestAnimationFrame(loop); }
    else { cancelAnimationFrame(raf); raf = null; }
  };
  raf = requestAnimationFrame(loop);
}
function stopTrailLoop(){
  if (raf){ cancelAnimationFrame(raf); raf = null; }
  particles = [];
  ctx.clearRect(0,0,TRAIL.width,TRAIL.height);
}
addEventListener('mousemove', (e)=>{
  if(!fxTrail.checked) return;
  particles.push({ x:e.clientX, y:e.clientY, r: DOT_MIN_R + Math.random()*(DOT_MAX_R - DOT_MIN_R), born: performance.now() });
  if (particles.length > 1200) particles.splice(0, particles.length - 1200);
});

loadFXPrefs();

// initial panel = Home (ARIA)
document.querySelector('.tab[data-panel="home"]').setAttribute('aria-selected','true');
</script>
</body>
</html>
)HTML";

  /* ========================= HTTP handlers ========================= */
  void handleRoot(){ server.send_P(200, "text/html; charset=utf-8", HTML_PAGE); }

  void sendEvents(){
    String json = "{\"events\":[";
    for (int i = 0; i < evCount; ++i) {
      int idx = (evHead - evCount + i + MAX_EVENTS) % MAX_EVENTS;
      String line = eventsBuf[idx];
      line.replace("\\","\\\\"); line.replace("\"","\\\"");
      if (i) json += ",";
      json += "\"" + line + "\"";
    }
    json += "]}";
    server.send(200, "application/json; charset=utf-8", json);
  }

  void handleOpen(){  openFlower("button"); lastDetectMs = millis(); server.send(200,"application/json; charset=utf-8","{}"); }
  void handleClose(){ closeFlower("button"); server.send(200,"application/json; charset=utf-8","{}"); }
  void handleClear(){ evHead = evCount = 0; addEvent("🧹 Log cleared"); server.send(200,"application/json; charset=utf-8","{}"); }

  /* ========================= WIFI shared ========================= */
  void connect_wifi(){
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Wi-Fi connecting");
    int tries=0;
    while (WiFi.status()!=WL_CONNECTED && tries++<120) { Serial.print("."); delay(250); }
    Serial.println();
    if (WiFi.status()==WL_CONNECTED) {
      Serial.print("Wi-Fi OK. Open http://"); Serial.println(WiFi.localIP());
    } else {
      Serial.println("Wi-Fi failed; dashboard will appear once connected.");
    }
  }
  // ---------------- Globals ----------------
  // forward decl so we can call it before its full definition
  void goSleep();


  // Touch ISR (minimal) + debounce in loop
  volatile uint32_t touchISRCount = 0;
  uint32_t lastCountSeen = 0, lastDebounceMs = 0;
  const uint32_t TOUCH_DEBOUNCE_MS = 120;
  bool touchFlag = false;

  // UI state
  String mainMessage    = "Hello BloomX!";
  String welcomeMessage = "Hey Beautiful ;)";     // default welcome if user leaves blank
  String lastPreset     = "";                     // saved when pressing Save
  uint16_t textColor565 = 0xFFFF;                 // white
  uint16_t bgColor565   = 0x0000;                 // black

  // Activity / sleep
  bool activated = false;                         // false = sleep (pink flower)
  const uint32_t WELCOME_DURATION_MS = 3000;
  const uint32_t SLEEP_TIMEOUT_MS    = 3UL*60UL*1000UL; // 3 minutes
  uint32_t welcomeShownAt = 0;
  bool showingWelcome = false;
  uint32_t lastActivityMs = 0;

  uint16_t rgb888_to_565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }
  uint16_t parse_hex_color_to_565(const String& hex, uint16_t fallback) {
    if (hex.length() != 7 || hex[0] != '#') return fallback;
    auto hx = [](char c)->int {
      if (c>='0'&&c<='9') return c-'0';
      if (c>='a'&&c<='f') return 10+c-'a';
      if (c>='A'&&c<='F') return 10+c-'A';
      return 0;
    };
    uint8_t r=(hx(hex[1])<<4)|hx(hex[2]);
    uint8_t g=(hx(hex[3])<<4)|hx(hex[4]);
    uint8_t b=(hx(hex[5])<<4)|hx(hex[6]);
    return rgb888_to_565(r,g,b);
  }

  void drawCentered(const String& msg, uint16_t fg, uint16_t bg) {
    tft.fillScreen(bg);
    tft.setTextColor(fg);
    tft.setTextSize(2);
    tft.setTextWrap(true);
    int16_t x1,y1; uint16_t w,h;
    tft.getTextBounds(msg, 0, 0, &x1, &y1, &w, &h);
    int16_t cx = (tft.width()  - (int)w) / 2;
    int16_t cy = (tft.height() - (int)h) / 2;
    if (cx < 0) cx = 0; if (cy < 0) cy = 0;
    tft.setCursor(cx, cy);
    tft.print(msg);
  }

  // Idle flower screen (sleep mode)
  void drawPinkFlowerIdle() {
    uint16_t bgPink     = rgb888_to_565(0xFF,0xC0,0xCB);
    uint16_t petalPink  = rgb888_to_565(0xFF,0x8F,0xC2);
    uint16_t centerYel  = rgb888_to_565(0xFF,0xE0,0x70);
    uint16_t outline    = rgb888_to_565(0xE0,0x5A,0x9F);

    int cx = tft.width()/2;
    int cy = tft.height()/2;
    int R  = min(cx, cy) - 6;

    tft.fillScreen(bgPink);
    int petalRadius = R * 0.38;
    int ringRadius  = R * 0.58;
    const int petals = 8;
    for (int i=0;i<petals;i++){
      float a = i * (2*PI / petals);
      int px = cx + (int)(ringRadius * cos(a));
      int py = cy + (int)(ringRadius * sin(a));
      tft.fillCircle(px, py, petalRadius, petalPink);
      tft.drawCircle(px, py, petalRadius, outline);
    }
    int coreR = R * 0.32;
    tft.fillCircle(cx, cy, coreR, centerYel);
    tft.drawCircle(cx, cy, coreR, outline);
    tft.fillCircle(cx - coreR/3, cy - coreR/3, coreR/5, COL_WHITE);
  }

  // Cute themed welcome screen
  void drawCuteWelcome() {
    uint16_t bg1 = rgb888_to_565(0xFF,0xD6,0xEB);
    uint16_t bg2 = rgb888_to_565(0xFF,0xB7,0xD5);
    uint16_t petal = rgb888_to_565(0xFF,0x8F,0xC2);
    uint16_t center = rgb888_to_565(0xFF,0xE8,0x9E);

    int cx = tft.width()/2;
    int cy = tft.height()/2;

    for (int y=0; y<tft.height(); y++) {
      uint16_t color = (y < tft.height()/2) ? bg1 : bg2;
      tft.drawFastHLine(0, y, tft.width(), color);
    }

    int R = min(cx, cy) / 2;
    int petalR = R * 0.4;
    int ringR = R * 0.65;
    for (int i=0; i<8; i++) {
      float a = i * (2*PI/8);
      int px = cx + (int)(ringR * cos(a));
      int py = cy - 20 + (int)(ringR * sin(a));
      tft.fillCircle(px, py, petalR, petal);
    }
    tft.fillCircle(cx, cy - 20, R*0.4, center);

    String wm = welcomeMessage.length() ? welcomeMessage : String("Hey Beautiful ✨");
    tft.setTextSize(2);
    tft.setTextColor(COL_WHITE);
    tft.setTextWrap(true);
    int16_t x1,y1; uint16_t w,h;
    tft.getTextBounds(wm, 0, 0, &x1, &y1, &w, &h);
    int16_t tx = (tft.width() - (int)w)/2;
    int16_t ty = max( (tft.height()*2/3) - (int)h/2, 0);
    tft.setCursor(tx, ty);
    tft.print(wm);
  }
  // ==== Bitmoji screens loaded from LittleFS ====
  void showListeningUI() {
    tft.fillScreen(COL_BLACK);
    drawJPG("/listen.jpg");     // put listen.jpg in /data
  }
  void showThinkingUI() {
    tft.fillScreen(COL_BLACK);
    drawJPG("/think.jpg");      // put think.jpg in /data
  }
  void showSpeakingUI() {
    tft.fillScreen(COL_BLACK);
    drawJPG("/speak.jpg");      // put speak.jpg in /data
  }
  void drawAlarmOverlay(bool on) {
    // Draw a blinking ring + tiny bell over the pink flower.
    // We only draw small pieces so it’s fast.

    int cx = tft.width()/2;
    int cy = tft.height()/2;
    int R  = min(cx, cy) - 6;

    uint16_t outline    = rgb888_to_565(0xE0,0x5A,0x9F); // same as flower outline
    uint16_t flash1     = 0xFFFF;  // white
    uint16_t flash2     = rgb888_to_565(0xFF,0x8F,0xC2); // petal pink

    // Blinking ring
    uint16_t col = on ? flash1 : outline;
    for (int i=0; i<3; ++i) tft.drawCircle(cx, cy, R - 4 - i*2, col);

    // Tiny bell under the center
    int bx = cx, by = cy + R/2 - 8;
    uint16_t bellCol = on ? flash1 : flash2;
    // bell dome
    tft.fillCircle(bx, by, 10, bellCol);
    tft.fillRect(bx-10, by, 20, 10, bellCol);     // body
    tft.fillCircle(bx, by+10, 3, COL_WHITE);      // clapper highlight
  }
  // === Classic: short beeps, short gaps (like basic clock) ===
static void alarm_classic_loop() {
  alarmPlaying = true;
  const int fs = SAMPLE_RATE;
  const float f = 1000.0f;      // 1 kHz
  const int onMs = 200, offMs = 150;
  uint8_t fr[4];

  auto silence = [&](int ms){
    int N = (ms*fs)/1000;
    for (int n=0; n<N && alarmPlaying; ++n) {
      fr[0]=fr[1]=fr[2]=fr[3]=0;
      size_t w; i2s_write(I2S_NUM_1, (const char*)fr, 4, &w, portMAX_DELAY);
    }
  };

  while (alarmPlaying) {
    float g = alarm_gain();
    int N = (onMs*fs)/1000;
    for (int n=0; n<N && alarmPlaying; ++n) {
      float s = sinf(2*PI*f * (float)n/fs);
      int16_t v = (int16_t)(s * g * 32767.0f);
      fr[0]=v & 0xFF; fr[1]=(v>>8)&0xFF; fr[2]=fr[0]; fr[3]=fr[1];
      size_t w; i2s_write(I2S_NUM_1, (const char*)fr, 4, &w, portMAX_DELAY);
    }
    silence(offMs);
  }
  alarmPlaying = false;
}

// === Pulse: BEEP-BEEP pattern in pairs ===
static void alarm_pulse_loop() {
  alarmPlaying = true;
  const int fs = SAMPLE_RATE;
  const float f = 880.0f;       // A5
  const int beepMs = 160, gapMs = 120, pairGapMs = 320;
  uint8_t fr[4];

  auto tone = [&](int ms){
    float g = alarm_gain();
    int N = (ms*fs)/1000;
    for (int n=0; n<N && alarmPlaying; ++n) {
      float s = sinf(2*PI*f * (float)n/fs);
      int16_t v = (int16_t)(s * g * 32767.0f);
      fr[0]=v & 0xFF; fr[1]=(v>>8)&0xFF; fr[2]=fr[0]; fr[3]=fr[1];
      size_t w; i2s_write(I2S_NUM_1, (const char*)fr, 4, &w, portMAX_DELAY);
    }
  };
  auto silence = [&](int ms){
    int N = (ms*fs)/1000;
    for (int n=0; n<N && alarmPlaying; ++n) {
      fr[0]=fr[1]=fr[2]=fr[3]=0;
      size_t w; i2s_write(I2S_NUM_1, (const char*)fr, 4, &w, portMAX_DELAY);
    }
  };

  while (alarmPlaying) {
    tone(beepMs); silence(gapMs);
    tone(beepMs); silence(pairGapMs);
  }
  alarmPlaying = false;
}


// === Rising: tri-tone rising motif ===
static void alarm_rising_loop() {
  alarmPlaying = true;
  const int fs = SAMPLE_RATE;
  const float seq[] = { 740.0f, 880.0f, 988.0f }; // F#5, A5, B5
  const int toneMs = 180, stepGap = 80, phraseGap = 260;
  uint8_t fr[4];

  auto tone = [&](float f, int ms){
    float g = alarm_gain();
    int N = (ms*fs)/1000;
    for (int n=0; n<N && alarmPlaying; ++n) {
      float s = sinf(2*PI*f * (float)n/fs);
      int16_t v = (int16_t)(s * g * 32767.0f);
      fr[0]=v & 0xFF; fr[1]=(v>>8)&0xFF; fr[2]=fr[0]; fr[3]=fr[1];
      size_t w; i2s_write(I2S_NUM_1, (const char*)fr, 4, &w, portMAX_DELAY);
    }
  };
  auto silence = [&](int ms){
    int N = (ms*fs)/1000;
    for (int n=0; n<N && alarmPlaying; ++n) {
      fr[0]=fr[1]=fr[2]=fr[3]=0;
      size_t w; i2s_write(I2S_NUM_1, (const char*)fr, 4, &w, portMAX_DELAY);
    }
  };

  while (alarmPlaying) {
    for (int i=0; i<3 && alarmPlaying; ++i) { tone(seq[i], toneMs); silence(stepGap); }
    silence(phraseGap);
  }
  alarmPlaying = false;
}

// === Bell: two chimey notes, repeating ===
static void alarm_bell_loop() {
  alarmPlaying = true;
  const int fs = SAMPLE_RATE;
  const float f1 = 659.25f; // E5
  const float f2 = 987.77f; // B5
  const int onMs = 220, betweenMs = 120, phraseGap = 300;
  uint8_t fr[4];

  auto tone = [&](float f, int ms){
    float g = alarm_gain();
    int N = (ms*fs)/1000;
    for (int n=0; n<N && alarmPlaying; ++n) {
      // slight 2nd harmonic to feel like a bell
      float s = 0.82f*sinf(2*PI*f*(float)n/fs) + 0.18f*sinf(2*PI*(2*f)*(float)n/fs);
      int16_t v = (int16_t)(s * g * 32767.0f);
      fr[0]=v & 0xFF; fr[1]=(v>>8)&0xFF; fr[2]=fr[0]; fr[3]=fr[1];
      size_t w; i2s_write(I2S_NUM_1, (const char*)fr, 4, &w, portMAX_DELAY);
    }
  };
  auto silence = [&](int ms){
    int N = (ms*fs)/1000;
    for (int n=0; n<N && alarmPlaying; ++n) {
      fr[0]=fr[1]=fr[2]=fr[3]=0;
      size_t w; i2s_write(I2S_NUM_1, (const char*)fr, 4, &w, portMAX_DELAY);
    }
  };

  while (alarmPlaying) {
    tone(f1, onMs); silence(betweenMs);
    tone(f2, onMs); silence(phraseGap);
  }
  alarmPlaying = false;
}

  void alarmTaskFn(void* pv) {
  switch (alarmSound) {
    case ALARM_CLASSIC: alarm_classic_loop(); break;
    case ALARM_PULSE:   alarm_pulse_loop();   break;
    case ALARM_RISING:  alarm_rising_loop();  break;
    case ALARM_BELL:    alarm_bell_loop();    break;
  }
  alarmTask = nullptr;
  vTaskDelete(NULL);
}


  void startAlarmInfinite() {
    if (alarmPlaying || alarmTask) return;

    // ensure the flower background is on screen
    uiState = UI_SLEEP;
    goSleep();                 // draws the pink flower now
    alarmFlashOn = false;
    lastAlarmBlinkMs = millis();

    xTaskCreatePinnedToCore(alarmTaskFn, "alarmTask", 4096, nullptr, 2, &alarmTask, 0);
    addEvent("⏰ Alarm started (infinite)");
  }

  void stopAlarmNow() {
    if (!alarmPlaying) return;
    alarmPlaying = false;

    // flush DMA with silence
    uint8_t zeros[4*256] = {0}; size_t w;
    i2s_write(I2S_NUM_1, (const char*)zeros, sizeof(zeros), &w, 50 / portTICK_PERIOD_MS);

    addEvent("🛑 Alarm stopped");
    // redraw clean flower (overlay will stop)
    goSleep();
  }


  void goSleep() {
    activated = false;
    showingWelcome = false;
    drawPinkFlowerIdle();

    if (showSleepClock) {
      String ts;
      if (getTimeString(ts)) {
        // pick a readable Y position near the bottom
        int y = tft.height() - 48;  // adjust if you like
        drawClockTextCentered(ts, y, COL_WHITE, rgb888_to_565(0,0,0)); // white with soft black shadow
      }
    }
  }
  // ===== Alarms =====
  struct Alarm {
    uint8_t hour;         // 0..23
    uint8_t minute;       // 0..59
    uint8_t dowMask;      // bit0=Sun .. bit6=Sat (1=active that day) ; 0 = every day
    bool    enabled;
    String  label;
  };

  static const int MAX_ALARMS = 8;
  Alarm alarms[MAX_ALARMS];
  int   alarmCount = 0;

  int lastAlarmMinuteFired = -1;  // prevents retrigger within the same minute

  // Convert tm_wday (0=Sun..6=Sat) to bit
  inline uint8_t dowBit(int w) { return (1 << w); }

  // Save/load alarms to LittleFS as /alarms.json
  bool save_alarms() {
    File f = LittleFS.open("/alarms.json", FILE_WRITE);
    if (!f) return false;
    StaticJsonDocument<2048> d;
    JsonArray arr = d.createNestedArray("alarms");
    for (int i=0;i<alarmCount;i++) {
      JsonObject o = arr.createNestedObject();
      o["hour"] = alarms[i].hour;
      o["minute"] = alarms[i].minute;
      o["dowMask"] = alarms[i].dowMask;
      o["enabled"] = alarms[i].enabled;
      o["label"] = alarms[i].label;
    }
    bool ok = (serializeJson(d, f) > 0);
    f.close();
    return ok;
  }

  bool load_alarms() {
    alarmCount = 0;
    if (!LittleFS.exists("/alarms.json")) return true; // nothing yet
    File f = LittleFS.open("/alarms.json", FILE_READ);
    if (!f) return false;
    StaticJsonDocument<2048> d;
    DeserializationError err = deserializeJson(d, f);
    f.close();
    if (err) return false;
    JsonArray arr = d["alarms"];
    if (!arr.isNull()) {
      for (JsonObject o : arr) {
        if (alarmCount >= MAX_ALARMS) break;
        alarms[alarmCount].hour    = o["hour"] | 0;
        alarms[alarmCount].minute  = o["minute"] | 0;
        alarms[alarmCount].dowMask = o["dowMask"] | 0; // 0 means every day
        alarms[alarmCount].enabled = o["enabled"] | true;
        alarms[alarmCount].label   = (const char*)(o["label"] | "");
        alarmCount++;
      }
    }
    return true;
  }

  void play_chime(int ms = 2500) {
    alarmPlaying = true;

    const int sampleRate = SAMPLE_RATE;
    const float freqs[] = {880.0f, 988.0f, 1318.0f, 988.0f};
    const int tones = 4;
    const int toneMs = ms / tones;
    const float amp = 0.45f;

    uint8_t frame[4];
    for (int t = 0; t < tones && alarmPlaying; ++t) {
      float f = freqs[t];
      int total = (toneMs * sampleRate) / 1000;
      for (int n = 0; n < total && alarmPlaying; ++n) {
        float s = sinf(2.0f * PI * f * (float)n / (float)sampleRate);
        int16_t v = (int16_t)(s * amp * 32767.0f);
        frame[0] = v & 0xFF; frame[1] = (v >> 8) & 0xFF;
        frame[2] = frame[0]; frame[3] = frame[1];
        size_t w;
        i2s_write(I2S_NUM_1, (const char*)frame, sizeof(frame), &w, portMAX_DELAY);
      }
    }

    alarmPlaying = false;
    // optional bloom reset
    backToPink();
  }
  void handleStopAlarm() {
    if (alarmPlaying) {
      alarmPlaying = false;
      addEvent("🛑 Alarm stopped by user");
    }
    server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
  }

 void check_alarms_tick() {
  // Poll fast; we only fire when exact second == 0.
  static uint32_t lastPollMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastPollMs < 100) return;   // ~10x per second
  lastPollMs = nowMs;

  struct tm t;
  if (!getLocalTime(&t, 20)) return;

  // ---- one static, not two ----
  static int lastSeenSec = -1;        // guard so we run once at :00
  if (t.tm_sec != 0) {                // reset guard during the rest of the minute
    lastSeenSec = -1;
    return;
  }
  if (lastSeenSec == 0) return;       // already handled this :00 tick
  lastSeenSec = 0;

  const int curHour = t.tm_hour;
  const int curMin  = t.tm_min;
  const int curDOW  = t.tm_wday;      // 0=Sun..6=Sat

  // Only once per (day, minute)
  static int lastFireYDay = -1;
  static int lastFireMin  = -1;

  for (int i = 0; i < alarmCount; ++i) {
    if (!alarms[i].enabled) continue;

    const bool dayOk = (alarms[i].dowMask == 0) || (alarms[i].dowMask & (1 << curDOW));
    if (!dayOk) continue;

    if (alarms[i].hour == curHour && alarms[i].minute == curMin) {
      if (!(lastFireYDay == t.tm_yday && lastFireMin == curMin)) {
        lastFireYDay = t.tm_yday;
        lastFireMin  = curMin;

        addEvent(String("⏰ Alarm: ") + (alarms[i].label.length() ? alarms[i].label : "Time!"));
        openFlower("alarm");
        startAlarmInfinite();          // or: play_chime();
        uiState = UI_SLEEP;
      }
      break; // only one alarm per minute
    }
  }
}


  void showMain() {
    activated = true;
    drawCentered(mainMessage, textColor565, bgColor565);
    showingWelcome = false;
    lastActivityMs = millis();
  }

  void showPreset(const String& p) {
    activated = true;
    drawCentered(p, textColor565, bgColor565);
    showingWelcome = false;
    lastActivityMs = millis();
  }
  // ---------- Bitmoji helpers ----------
  static inline uint16_t c565(uint8_t r,uint8_t g,uint8_t b){ return rgb888_to_565(r,g,b); }

  struct FaceColors {
    uint16_t bg, skin, line, accent, cloud;
  };

  static void drawFaceBase(const FaceColors& C, int &cx, int &cy, int &R){
    tft.fillScreen(C.bg);
    cx = tft.width()/2;
    cy = tft.height()/2 + 6;           // slightly low
    R  = min(cx, cy) - 18;              // head radius

    // head
    tft.fillCircle(cx, cy, R, C.skin);
    tft.drawCircle(cx, cy, R, C.line);

    // hair band (simple arc)
    tft.drawCircle(cx, cy - R/3, R/2 + 3, C.line);

    // eyes
    int ex = R/3, ey = -R/8;
    int er = max(3, R/12);
    tft.fillCircle(cx - ex, cy + ey, er, C.line);
    tft.fillCircle(cx + ex, cy + ey, er, C.line);
  }

  static void mouthNeutral(int cx,int cy,int R, uint16_t col){
    tft.drawFastHLine(cx - R/6, cy + R/4, R/3, col);
  }
  static void mouthSmile(int cx,int cy,int R, uint16_t col){
    // small smile arc (three lines)
    for(int i=-1;i<=1;i++){
      tft.drawLine(cx - R/5, cy + R/4 + i, cx, cy + R/3 + i, col);
      tft.drawLine(cx, cy + R/3 + i, cx + R/5, cy + R/4 + i, col);
    }
  }
  static void mouthOpen(int cx,int cy,int R, uint16_t col){
    tft.fillCircle(cx, cy + R/3, max(3, R/10), col);
  }

  // three rings of waves
  static void drawWaves(int x0,int y0,int R, uint16_t col){
    for(int i=0;i<3;i++){
      int rr = R/5 + i*R/8;
      tft.drawCircle(x0, y0, rr, col);
    }
  }

  // thought cloud (4 bubbles)
  static void drawThoughtCloud(int cx,int cy,int R, uint16_t cloud, uint16_t line){
    int x = cx + R/2, y = cy - R/3;
    int r1=R/12, r2=R/10, r3=R/8, r4=R/5;
    struct B{int x,y,r;}; B b[4]={{x,y,r1},{x+R/6,y-R/6,r2},{x+R/3,y-2, r3},{x+R/2+6,y-R/6-6,r4}};
    for(auto &bb: b){ tft.fillCircle(bb.x, bb.y, bb.r, cloud); tft.drawCircle(bb.x, bb.y, bb.r, line); }
  }

  // ---------------- ISR ----------------
  void IRAM_ATTR onTouchISR() { touchISRCount++; }
  String hexFrom565(uint16_t c){
    uint8_t r=(c>>11)&0x1F,g=(c>>5)&0x3F,b=c&0x1F;
    r=(r*527+23)>>6; g=(g*259+33)>>6; b=(b*527+23)>>6;
    char buf[8]; snprintf(buf,sizeof(buf),"#%02X%02X%02X", r,g,b); return String(buf);
  }

  void handleState(){
    StaticJsonDocument<320>d;
    d["main"]=mainMessage; d["welcome"]=welcomeMessage;
    d["fg"]=hexFrom565(textColor565); d["bg"]=hexFrom565(bgColor565);
    d["preset"]=lastPreset;
    String out; serializeJson(d,out);
    server.send(200,"application/json; charset=utf-8",out);
  }

  void handleSet(){
    if(!server.hasArg("plain")){ server.send(400,"application/json; charset=utf-8","{\"error\":\"bad json\"}"); return; }
    StaticJsonDocument<640>d;
    if(deserializeJson(d,server.arg("plain"))){ server.send(400,"application/json; charset=utf-8","{\"error\":\"bad json\"}"); return; }
    if(d.containsKey("main"))    mainMessage    = (const char*)d["main"];
    if(d.containsKey("welcome")) welcomeMessage = (const char*)d["welcome"];
    if(d.containsKey("fg"))      textColor565   = parse_hex_color_to_565((const char*)d["fg"], textColor565);
    if(d.containsKey("bg"))      bgColor565     = parse_hex_color_to_565((const char*)d["bg"], bgColor565);
    if(d.containsKey("preset"))  lastPreset     = (const char*)d["preset"];
    server.send(200,"application/json; charset=utf-8","{}");
  }

  void handleShow(){          // show Main message now
    showMain();
    server.send(200,"application/json; charset=utf-8","{}");
  }
  void handleApply(){         // show selected preset now (and remember which)
    if(server.hasArg("plain")){
      StaticJsonDocument<256>d; deserializeJson(d,server.arg("plain"));
      if(d.containsKey("preset")) lastPreset = (const char*)d["preset"];
    }
    if (lastPreset.length()) showPreset(lastPreset);
    else showPreset("🌸 Bloom where you are.");
    server.send(200,"application/json; charset=utf-8","{}");
  }

  void handleRefresh(){       // back to sleep mode
    goSleep();
    server.send(200,"application/json; charset=utf-8","{}");
  }
  void handleGetAlarms() {
    StaticJsonDocument<2048> d;
    JsonArray arr = d.createNestedArray("alarms");
    for (int i=0;i<alarmCount;i++) {
      JsonObject o = arr.createNestedObject();
      o["hour"] = alarms[i].hour;
      o["minute"] = alarms[i].minute;
      o["dowMask"] = alarms[i].dowMask;
      o["enabled"] = alarms[i].enabled;
      o["label"] = alarms[i].label;
    }
    String out; serializeJson(d, out);
    server.send(200, "application/json; charset=utf-8", out);
  }

  void handleAddAlarm() {
    if (!server.hasArg("plain")) { server.send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
    if (alarmCount >= MAX_ALARMS) { server.send(400, "application/json", "{\"error\":\"limit\"}"); return; }

    StaticJsonDocument<512> d;
    if (deserializeJson(d, server.arg("plain"))) { server.send(400, "application/json", "{\"error\":\"bad json\"}"); return; }
    Alarm a;
    a.hour    = d["hour"] | 0;
    a.minute  = d["minute"] | 0;
    a.dowMask = d["dowMask"] | 0;     // 0 = every day
    a.enabled = d["enabled"] | true;
    a.label   = (const char*)(d["label"] | "");

    // Basic bounds
    a.hour = min<uint8_t>(23, a.hour);
    a.minute = min<uint8_t>(59, a.minute);

    alarms[alarmCount++] = a;
    save_alarms();
    server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
  }

  void handleDeleteAlarm() {
    // DELETE /alarms?i=INDEX
    if (!server.hasArg("i")) { server.send(400,"application/json","{\"error\":\"missing index\"}"); return; }
    int idx = server.arg("i").toInt();
    if (idx < 0 || idx >= alarmCount) { server.send(400,"application/json","{\"error\":\"bad index\"}"); return; }
    for (int j=idx+1; j<alarmCount; ++j) alarms[j-1] = alarms[j];
    alarmCount--;
    save_alarms();
    server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
  }

  void handleToggleAlarm() {
    // POST /alarms/toggle  { "i": 0, "enabled": true }
    if (!server.hasArg("plain")) { server.send(400,"application/json","{\"error\":\"bad json\"}"); return; }
    StaticJsonDocument<256> d;
    if (deserializeJson(d, server.arg("plain"))) { server.send(400,"application/json","{\"error\":\"bad json\"}"); return; }
    int i = d["i"] | -1;
    bool en = d["enabled"] | false;
    if (i < 0 || i >= alarmCount) { server.send(400,"application/json","{\"error\":\"bad index\"}"); return; }
    alarms[i].enabled = en;
    save_alarms();
    server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
  }

  void handleTestAlarm() {
  addEvent("🔊 Alarm test (infinite)");
  startAlarmInfinite();                      // was: uiState=UI_SPEAK; play_chime(1500); backToPink();
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}
void handleLedState(){
  StaticJsonDocument<256> d;
  d["powered"]      = st.powered;
  d["effect"]       = effectToStr(st.effect);
  d["brightness"]   = st.brightness;
  d["nightMode"]    = st.nightMode;
  d["paletteIndex"] = st.paletteIndex;
  String out; serializeJson(d,out);
  server.send(200, "application/json; charset=utf-8", out);
}

void handleLedSet(){
  if(!server.hasArg("plain")){
    server.send(400,"application/json; charset=utf-8","{\"error\":\"bad json\"}"); return;
  }
  StaticJsonDocument<256> d;
  if (deserializeJson(d, server.arg("plain"))) {
    server.send(400,"application/json; charset=utf-8","{\"error\":\"bad json\"}"); return;
  }

  bool changed = false;

  if (d.containsKey("powered")) {
    st.powered = d["powered"];
    if (!st.powered) { strip.clear(); strip.show(); }
    changed = true;
  }
  // Optional: effectColors array (5 hex strings, e.g. "#FF69B4")
if (d.containsKey("effectColors")) {
  JsonArray arr = d["effectColors"].as<JsonArray>();
  // TODO: store to NVS / Preferences and let your effects read them.
  // Example: copy into a global array of uint32_t or RGB structs.
  // (safe-guard on size and valid hex)
  changed = true;
}

  if (d.containsKey("effect")) {
    st.effect = strToEffect( (const char*)d["effect"] );
    changed = true;
  }
  if (d.containsKey("brightness")) {
    st.brightness = clamp8((int)d["brightness"], BRIGHTNESS_MIN, BRIGHTNESS_MAX);
    changed = true;
  }
  if (d.containsKey("nightMode")) {
    st.nightMode = d["nightMode"];
    changed = true;
  }
  if (d.containsKey("paletteIndex")) {
    st.paletteIndex = (uint8_t)((int)d["paletteIndex"] % PALETTE_SIZE);
    changed = true;
  }

  if (changed) saveState();
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}
void handleServoTest(){
  if (!bloomServo.attached())
    bloomServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
  bloomServo.write(CLOSED_ANGLE);
  delay(400);
  bloomServo.write(OPEN_ANGLE);
  delay(700);
  bloomServo.write(CLOSED_ANGLE);
  server.send(200,"application/json; charset=utf-8","{\"ok\":true}");
}

  /* ========================= I2S RX: MIC ========================= */
  void i2s_install_rx() {
    const i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = 0,
      .dma_buf_count = 8,
      .dma_buf_len = 128,
      .use_apll = false,
      .tx_desc_auto_clear = false,
      .fixed_mclk = 0
    };
    const i2s_pin_config_t pins = {
      .bck_io_num   = I2S_SCK,
      .ws_io_num    = I2S_WS,
      .data_out_num = I2S_PIN_NO_CHANGE,
      .data_in_num  = I2S_SD
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_zero_dma_buffer(I2S_NUM_0);

  }

  int read_block_and_rms(int16_t* out, int outCapacity, int& outWritten) {
    size_t bytesRead = 0;
    i2s_read(I2S_NUM_0, i2sRaw, sizeof(i2sRaw), &bytesRead, portMAX_DELAY);
    int n32 = bytesRead / sizeof(int32_t);
    if (n32 <= 0) { outWritten = 0; return 0; }

    int64_t sumsq = 0;
    int nCopy = (out && outCapacity > 0) ? min(outCapacity, n32) : 0;

    for (int i = 0; i < n32; ++i) {
      int16_t s16 = (int16_t)(i2sRaw[i] >> 14);
      if (i < nCopy) out[i] = s16;
      sumsq += (int32_t)s16 * (int32_t)s16;
    }
    outWritten = nCopy;
    int rms = (int)sqrt((double)sumsq / n32);
    return rms;
  }

  /* ========================= WAV helpers ========================= */
  static inline void put_le16(uint8_t* p, uint16_t v){ p[0]=v & 0xFF; p[1]=(v>>8)&0xFF; }
  static inline void put_le32(uint8_t* p, uint32_t v){ p[0]=v & 0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24); }
  void write_wav_header(uint8_t* h, uint32_t numSamples) {
    const uint32_t byteRate   = SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE;
    const uint32_t blockAlign = CHANNELS * BYTES_PER_SAMPLE;
    const uint32_t dataSize   = numSamples * BYTES_PER_SAMPLE;
    const uint32_t chunkSize  = 36 + dataSize;

    memcpy(h + 0,  "RIFF", 4);
    put_le32(h + 4, chunkSize);
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);
    put_le16(h + 22, CHANNELS);
    put_le32(h + 24, SAMPLE_RATE);
    put_le32(h + 28, byteRate);
    put_le16(h + 32, (uint16_t)blockAlign);
    put_le16(h + 34, (uint16_t)(8 * BYTES_PER_SAMPLE));
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, dataSize);
  }

  /* ========================= Deepgram STT ========================= */
  String deepgram_transcribe(const int16_t* samples, uint32_t numSamples) {
    WiFiClientSecure client; client.setInsecure();
    HTTPClient https;
    if (!https.begin(client, DG_STT_URL)) return F("begin() failed");

    const uint32_t wavBytes = 44 + numSamples * BYTES_PER_SAMPLE;
    uint8_t* wav = (uint8_t*)malloc(wavBytes);
    if (!wav) { https.end(); return F("OOM building WAV"); }
    write_wav_header(wav, numSamples);
    memcpy(wav + 44, samples, numSamples * BYTES_PER_SAMPLE);

    https.addHeader("Authorization", String("Token ") + DG_API_KEY);
    https.addHeader("Content-Type", "audio/wav");
    https.addHeader("Accept", "application/json");
    https.addHeader("Accept-Encoding", "identity");

    int code = https.POST(wav, wavBytes);
    free(wav);
    if (code <= 0) { String msg = "HTTP error: " + String(code); https.end(); return msg; }

    String payload = https.getString();
    https.end();

    DynamicJsonDocument doc(16384);
    DeserializationError err = deserializeJson(doc, payload);
    if (!err) {
      const char* t = doc["results"]["channels"][0]["alternatives"][0]["transcript"];
      if (t && t[0]) return String(t);
      if (doc["error"].is<const char*>()) return String("Deepgram error: ") + (const char*)doc["error"];
      return F("(no transcript)");
    }
    int idx = payload.indexOf("\"transcript\":\"");
    if (idx >= 0) {
      idx += 14; int end = payload.indexOf('"', idx);
      if (end > idx) {
        String rough = payload.substring(idx, end);
        rough.replace("\\n"," "); rough.replace("\\\"","\""); rough.replace("\\/","/");
        rough.replace("\\t"," "); rough.replace("\\\\","\\");
        return rough;
      }
    }
    int maxLen = (payload.length() > 2000) ? 2000 : payload.length();
    return String("JSON parse error: ") + err.c_str() + " | payload: " + payload.substring(0, maxLen);
  }

  /* ========================= OpenAI Chat ========================= */
  String lastUser = "";
  String lastAssistant = "";

  bool wants_more(String s) {
    s.toLowerCase(); s.trim();
    return (s == "more" || s == "more details" || s.endsWith(" more") || s.endsWith(" details"));
  }

  String openai_chat_reply(const String& userText) {
    bool more = wants_more(userText);

    WiFiClientSecure client; client.setInsecure();
    HTTPClient https;
    if (!https.begin(client, OA_CHAT_URL)) return F("(OpenAI) begin() failed");

    StaticJsonDocument<8192> body;
    body["model"] = "gpt-4.1-mini";

    JsonArray msgs = body.createNestedArray("messages");

    JsonObject sys = msgs.createNestedObject();
    sys["role"] = "system";
    sys["content"] =
      "You are a clear, friendly voice assistant. "
      "When the user asks a new question, explain it completely in 6–10 sentences, "
      "and always end with 'Say more for details.' "
      "If the user says 'more', continue on the SAME topic, add new information, "
      "and again end with 'Say more for details.'";

    if (lastUser.length() && lastAssistant.length()) {
      JsonObject m1 = msgs.createNestedObject(); m1["role"] = "user";      m1["content"] = lastUser;
      JsonObject m2 = msgs.createNestedObject(); m2["role"] = "assistant"; m2["content"] = lastAssistant;
    }

    static String convo_topic = "";
    String prompt = userText;
    if (more && convo_topic.length()) {
      prompt = "Continue explaining in more detail about: " + convo_topic +
              ". Do not repeat earlier points, but add new, deeper info. "
              "End again with 'Say more for details.'";
    } else {
      convo_topic = userText;
    }

    JsonObject m3 = msgs.createNestedObject();
    m3["role"] = "user";
    m3["content"] = prompt;

    body["max_tokens"] = 600;
    body["temperature"] = 0.7;

    String bodyStr; serializeJson(body, bodyStr);
    https.addHeader("Authorization", String("Bearer ") + OA_API_KEY);
    https.addHeader("Content-Type", "application/json");
    https.addHeader("Accept", "application/json");

    int code = https.POST(bodyStr);
    String payload = https.getString();
    https.end();

    if (code <= 0) return String("(OpenAI) HTTP error: ") + code;

    DynamicJsonDocument doc(16384);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) return String("(OpenAI) parse error: ") + err.c_str();

    if (doc["error"].is<JsonObject>()) {
      return String("(OpenAI) API error: ") + (const char*)doc["error"]["message"];
    }

    const char* reply = doc["choices"][0]["message"]["content"];
    if (reply && reply[0]) {
      lastUser = userText;
      lastAssistant = reply;
      return String(reply);
    }

    return "(OpenAI) no content";
  }

  /* ========================= I2S TX (SPEAKER) ========================= */
  void i2s_tx_install_i16() {
    if (AMP_SD >= 0) { pinMode(AMP_SD, OUTPUT); digitalWrite(AMP_SD, HIGH); }
    i2s_config_t cfg = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
      .intr_alloc_flags = 0,
      .dma_buf_count = 16,
      .dma_buf_len   = 512,
      .use_apll = true,
      .tx_desc_auto_clear = true,
      .fixed_mclk = 0
    };
    const i2s_pin_config_t pins = {
      .bck_io_num   = AMP_BCLK,
      .ws_io_num    = AMP_LRCK,
      .data_out_num = AMP_DIN,
      .data_in_num  = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &pins);
    i2s_set_clk(I2S_NUM_1, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    gpio_set_drive_capability((gpio_num_t)AMP_BCLK, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)AMP_LRCK, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability((gpio_num_t)AMP_DIN , GPIO_DRIVE_CAP_3);

    uint8_t z[4 * 256] = {0}; size_t w;
    i2s_write(I2S_NUM_1, (const char*)z, sizeof(z), &w, portMAX_DELAY);
  }

  /* cap outgoing TTS text length so RAM stays sane */
  String cap_for_tts(const String& s, size_t maxChars = 420) {
    if (s.length() <= maxChars) return s;
    int cut = s.lastIndexOf('.', maxChars);
    if (cut < 0) cut = s.lastIndexOf('!', maxChars);
    if (cut < 0) cut = s.lastIndexOf('?', maxChars);
    if (cut < 0) cut = (int)maxChars;
    String out = s.substring(0, cut + 1);
    out.trim();
    out += " … (Say 'more' for details.)";
    return out;
  }

  /* ========================= Deepgram TTS → play ========================= */
  String deepgram_tts_analyze_and_play(const String& text) {
    WiFiClientSecure ssl; ssl.setInsecure();

    String path = String("/v1/speak")
                + "?model=aura-2-thalia-en"
                + "&encoding=linear16"
                + "&sample_rate=16000"
                + "&channels=1"
                + "&container=wav";

    const char* host = "api.deepgram.com";
    if (!ssl.connect(host, 443)) return F("(TTS) TLS connect failed");

    JsonDocument jd; jd["text"] = text;
    String body; serializeJson(jd, body);

    String req;
    req  = "POST " + path + " HTTP/1.1\r\n";
    req += "Host: api.deepgram.com\r\n";
    req += "Authorization: Token " + String(DG_API_KEY) + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Accept: application/octet-stream\r\n";
    req += "Connection: close\r\n";
    req += "Content-Length: " + String(body.length()) + "\r\n\r\n";
    req += body;

    if (ssl.write((const uint8_t*)req.c_str(), req.length()) != (int)req.length()) {
      ssl.stop(); return F("(TTS) write failed");
    }

    auto readLine = [&](String& out)->bool {
      out = ""; int c; bool got=false; unsigned long t0=millis();
      while (millis()-t0 < 10000) {
        c = ssl.read();
        if (c < 0) { delay(1); continue; }
        got = true;
        if (c == '\r') continue;
        if (c == '\n') break;
        out += (char)c;
      }
      return got;
    };
    String line;
    if (!readLine(line)) { ssl.stop(); return F("(TTS) no status"); }
    if (!line.startsWith("HTTP/1.1 200")) {
      while (readLine(line) && line.length()) {}
      ssl.stop(); return String("(TTS) bad status: ") + line;
    }
    bool chunked=false; long clen=-1;
    while (readLine(line) && line.length()) {
      String low=line; low.toLowerCase();
      if (low.startsWith("transfer-encoding:") && low.indexOf("chunked")>=0) chunked = true;
      if (low.startsWith("content-length:")) clen = line.substring(line.indexOf(':')+1).toInt();
    }

    uint8_t* big=nullptr; size_t cap=0, len=0;
    auto reserve=[&](size_t need){
      if (need<=cap) return true;
      size_t newCap = cap? cap*2 : (size_t)65536;
      if (newCap<need) newCap=need;
      uint8_t* nb=(uint8_t*)ps_malloc(newCap); if(!nb) nb=(uint8_t*)malloc(newCap);
      if(!nb) return false;
      if(big && len) memcpy(nb, big, len);
      if(big) free(big);
      big=nb; cap=newCap; return true;
    };
    auto readExactly=[&](uint8_t* dst, size_t n)->bool{
      size_t got=0; unsigned long t0=millis();
      while (got<n && millis()-t0<20000){
        int r=ssl.read(dst+got, n-got);
        if (r>0){ got+=r; t0=millis(); }
        else delay(1);
      }
      return got==n;
    };

    if (!reserve(65536)) { ssl.stop(); return F("(TTS) OOM"); }

    if (chunked) {
      while (true) {
        if (!readLine(line)) { if(big) free(big); ssl.stop(); return F("(TTS) chunk: no size"); }
        unsigned long chunkSize=strtoul(line.c_str(),NULL,16);
        if (chunkSize==0) { readLine(line); break; }
        if (!reserve(len + chunkSize)) { free(big); ssl.stop(); return F("(TTS) OOM2"); }
        if (!readExactly(big+len, chunkSize)) { free(big); ssl.stop(); return F("(TTS) chunk: short read"); }
        len += chunkSize;
        readLine(line);
      }
    } else if (clen>0) {
      if (!reserve(len+clen)) { free(big); ssl.stop(); return F("(TTS) OOM3"); }
      if (!readExactly(big+len, clen)) { free(big); ssl.stop(); return F("(TTS) body short"); }
      len += clen;
    } else {
      uint8_t buf[4096]; unsigned long lastRead = millis();
      while (ssl.connected() || millis()-lastRead < 2000) {
        int r = ssl.read(buf,sizeof(buf));
        if (r>0) {
          if (!reserve(len+r)) { free(big); ssl.stop(); return F("(TTS) OOM4"); }
          memcpy(big+len, buf, r); len += r; lastRead = millis();
        } else { delay(1); }
      }
    }
    ssl.stop();

    auto rd32 = [&](const uint8_t* p){ return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24)); };

    const uint8_t* pcm = big; size_t pcmBytes = len;
    bool isWav = (len>=12 && memcmp(big,"RIFF",4)==0 && memcmp(big+8,"WAVE",4)==0);
    if (isWav) {
      size_t p=12; size_t dataOff=0, dataSize=0;
      while (p+8<=len){
        uint32_t id = rd32(big+p);
        uint32_t sz = rd32(big+p+4);
        size_t next = p+8+sz;
        if (id==0x61746164){ dataOff = p+8; dataSize = (p+8+sz<=len)? sz : (len-(p+8)); }
        if (next<=p || next>len) break;
        p=next;
      }
      if (dataOff>0 && dataSize>0){ pcm = big + dataOff; pcmBytes = min(dataSize, len - dataOff); }
      else { free(big); return F("(TTS) bad WAV"); }
    }

    if (pcmBytes < 4) { free(big); return F("(TTS) empty audio"); }

    i2s_set_clk(I2S_NUM_1, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
    i2s_zero_dma_buffer(I2S_NUM_1);

    const float TTS_GAIN = 2.2f;
    const int FR = 512; static uint8_t out[FR*4];
    size_t idx = 0;
    while (idx + 1 < pcmBytes) {
      int frames = 0; uint8_t* p = out;
      while (frames < FR && idx + 1 < pcmBytes) {
        int16_t m = (int16_t)((uint16_t)pcm[idx] | ((uint16_t)pcm[idx+1] << 8));
        idx += 2;
        int32_t x = (int32_t)(m * TTS_GAIN);
        if (x > 32767)  x = 32767;
        if (x < -32768) x = -32768;
        int16_t s = (int16_t)x;
        *p++ = s & 0xFF; *p++ = (s>>8)&0xFF;  // L
        *p++ = s & 0xFF; *p++ = (s>>8)&0xFF;  // R
        frames++;
      }
      if (frames) {
        size_t written;
        i2s_write(I2S_NUM_1, (const char*)out, frames*4, &written, portMAX_DELAY);
      }
    }

    free(big);
    return "OK";
  }

  bool wait_for_stable_level(int pin, int level, unsigned ms = 25) {
    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
      if (digitalRead(pin) != level) return false;
      delay(1);
    }
    return true;
  }

  /* ========================= PTT TASK (runs in parallel) ========================= */
  void pttTask(void*) {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    for (;;) {
      if (digitalRead(BUTTON_PIN) == HIGH) { vTaskDelay(10/portTICK_PERIOD_MS); continue; }
      if (!wait_for_stable_level(BUTTON_PIN, LOW, 30)) continue;
      

      Serial.println("PTT pressed → recording…");
      uiState = UI_LISTEN;


      const int minSamples = SAMPLE_RATE * MIN_RECORD_SEC;
      const int maxSamples = SAMPLE_RATE * MAX_RECORD_SEC;

      int total = 0;
      while (digitalRead(BUTTON_PIN) == LOW && total < maxSamples) {
        int toWrite = min((int)(sizeof(i2sRaw)/sizeof(i2sRaw[0])), maxSamples - total);
        int written = 0;
        (void)read_block_and_rms(pcm16 + total, toWrite, written);
        total += written;
      }

      (void)wait_for_stable_level(BUTTON_PIN, HIGH, 30);

      if (total < minSamples) {
        Serial.printf("PTT released; too short (%d samples), discarding.\n", total);
        vTaskDelay(80/portTICK_PERIOD_MS);
        backToPink();
        continue;
      }

      Serial.printf("PTT released; captured %d samples (~%0.2fs)\n", total, (double)total / SAMPLE_RATE);
      uiState = UI_THINK;
      if (activated && !alarmPlaying && (millis() - lastActivityMs >= SLEEP_TIMEOUT_MS)) {
    goSleep();
    activated = false;
  }
  if (alarmPlaying) { vTaskDelay(100/portTICK_PERIOD_MS); continue; }


      // 3) STT
      Serial.println("sending to Deepgram…");
      String transcript = deepgram_transcribe(pcm16, (uint32_t)total);
      Serial.println("Deepgram:");
      Serial.println(transcript);

      // 4) Chat → TTS
      if (transcript.length() < 2 ||
          transcript.startsWith("HTTP error") ||
          transcript.startsWith("begin() failed") ||
          transcript.startsWith("OOM") ||
          transcript.startsWith("JSON parse error") ||
          transcript.startsWith("DNS fail")) {
        Serial.println("(Skip OpenAI: STT not valid)");
        backToPink();
      } else {
        Serial.println("asking OpenAI…");
        Serial.print("OpenAI request text: ");
        Serial.println(transcript);
        String oa = openai_chat_reply(transcript);
        Serial.println("OpenAI:");
        Serial.println(oa);
        

        if (oa.length() > 1 && !oa.startsWith("(OpenAI)")) {
          uiState = UI_SPEAK;


          String tts = deepgram_tts_analyze_and_play(cap_for_tts(oa));
          if (tts != "OK") Serial.println(tts);
          backToPink();
        }
      }

      vTaskDelay(80/portTICK_PERIOD_MS);
    }
  }

  /* ========================= SETUP ========================= */
  void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("BloomX OLED + PTT booting…");
// --- LED strip + rotary init ---
randomSeed(esp_random());
strip.begin();
delay(5);  // let RMT/strip settle
strip.clear();
strip.show();

// ===== LED BOOT SELF-TEST (remove later) =====
for (int phase = 0; phase < 3; ++phase) {
  uint32_t c = (phase == 0) ? strip.Color(255,0,0)
                : (phase == 1) ? strip.Color(0,255,0)
                                : strip.Color(0,0,255);
  for (int i = 0; i < ACTIVE_LEDS; ++i) strip.setPixelColor(i, c);
  for (int i = ACTIVE_LEDS; i < NUM_LEDS; ++i) strip.setPixelColor(i, 0);
  strip.setBrightness(120);
  strip.show();
  delay(250);
}
strip.clear(); strip.show();


pinMode(CLK_PIN, INPUT_PULLUP);
pinMode(DT_PIN,  INPUT_PULLUP);
pinMode(SW_PIN,  INPUT_PULLUP);

loadState();
st.powered = true;   // force LEDs on during testing
lastInputAt = millis(); // keep auto-off timer fresh

st.brightness = clamp8(st.brightness, BRIGHTNESS_MIN, BRIGHTNESS_MAX);
lastCLK = digitalRead(CLK_PIN);
lastInputAt = millis();

Serial.println("BloomX LED UX ready: turn=palette, press+turn=brightness, short=effect, long=night");

    // --- Inputs / sensors ---
    pinMode(IR_PIN, INPUT);              // If noisy: use INPUT_PULLUP and keep IR_ACTIVE_LOW=true
    pinMode(TOUCH_IO, INPUT);
    attachInterrupt(digitalPinToInterrupt(TOUCH_IO), onTouchISR, RISING);

    // --- Servo (bloom mechanism) ---
    // --- Servo (bloom mechanism) ---
ESP32PWM::allocateTimer(0);              // or 1/2/3, any free timer is fine
bloomServo.setPeriodHertz(50);           // standard servo frequency
bloomServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);

delay(200);                              // let PWM stabilize
bloomServo.write(CLOSED_ANGLE);          // move to closed position slowly
delay(600);                              // give it time to get there

// Optional: detach so we only drive it when we actually move
bloomServo.detach();

    //ESP32PWM::allocateTimer(1);
    //ESP32PWM::allocateTimer(2);
    //ESP32PWM::allocateTimer(3);
    //bloomServo.setPeriodHertz(50);
    //bloomServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
    //bloomServo.write(OPEN_ANGLE);
    //bloomServo.write(CLOSED_ANGLE);

    // --- Network + time ---
    connect_wifi();
    configTzTime("EST5EDT,M3.2.0/2,M11.1.0/2", "pool.ntp.org", "time.nist.gov");

    // --- Display (GC9A01 or ST7789) ---
    Serial.println("Initializing display...");

    // Make sure SPI is clean before reinit
    SPI.end();
    delay(100);
    SPI.begin(SCK_PIN, -1, MOSI_PIN, TFT_CS);   

    // Ensure control pins are configured
    pinMode(TFT_CS, OUTPUT);   digitalWrite(TFT_CS, HIGH);
    pinMode(TFT_DC, OUTPUT);   digitalWrite(TFT_DC, HIGH);
    pinMode(TFT_RST, OUTPUT);  digitalWrite(TFT_RST, HIGH);
    delay(10);

    
  #if USE_ST7789
    tft.init(240, 240);                    // ST7789 square
    tft.setSPISpeed(16000000);
  #else
    tft.begin();                           // GC9A01A round
    tft.setSPISpeed(16000000);
  #endif
    tft.setRotation(0);
    tft.fillScreen(COL_BLACK);
    uiState = UI_SLEEP;
    goSleep();
    // --- Servo attach AFTER TFT is up (prevents boot clashes) ---
//ESP32PWM::allocateTimer(SERVO_LEDC_TIMER);   // reserve only this timer
//bloomServo.setPeriodHertz(50);               // 50 Hz for hobby servo
//bloomServo.attach(SERVO_PIN, SERVO_MIN_US, SERVO_MAX_US);
//delay(20);                                   // let PWM settle
//bloomServo.write(CLOSED_ANGLE);              // start closed (no open->close spike)

  // TJpg_Decoder setuESP32PWM::allocateTimer(0);p must come BEFORE any draw calls
  TJpgDec.setJpgScale(1);        // 1 = full size
  TJpgDec.setSwapBytes(false);    // RGB565 endianness for Adafruit GFX
  TJpgDec.setCallback(tft_output);

    // ---- Filesystem + JPEG decoder ----
  if (!LittleFS.begin(true)) {                 // true = auto-format on first run
    Serial.println("LittleFS mount FAILED (will be empty).");
  } else {
    Serial.println("LittleFS mounted.");
    load_alarm_cfg();   // <-- add this line
  }
  

    // Start in sleep (pink flower)
    goSleep();
    Serial.println("Display ready.");
    // ---- Filesystem + JPEG decoder ----
  load_alarms();
  // After configTzTime(...)
for (int i = 0; i < 60; ++i) {   // wait up to ~3s
  struct tm t;
  if (getLocalTime(&t, 50)) break;
  delay(50);
}


    // --- Web routes (union of both sets) ---
    server.on("/",        HTTP_GET,  handleRoot);
    server.on("/state",   HTTP_GET,  handleState);
    server.on("/set",     HTTP_POST, handleSet);
    server.on("/show",    HTTP_POST, handleShow);
    server.on("/apply",   HTTP_POST, handleApply);
    server.on("/refresh", HTTP_POST, handleRefresh);

    server.on("/events",  HTTP_GET,  sendEvents);
    server.on("/open",    HTTP_POST, handleOpen);
    server.on("/close",   HTTP_POST, handleClose);
    server.on("/clear",   HTTP_POST, handleClear);
    server.on("/led/state", HTTP_GET,  handleLedState);
    server.on("/led/set",   HTTP_POST, handleLedSet);
    server.on("/led/test", HTTP_POST, handleLedTest);
    server.on("/servo/test", HTTP_POST, handleServoTest);


    server.on("/alarms",        HTTP_GET,  handleGetAlarms);
  server.on("/alarms",        HTTP_POST, handleAddAlarm);
  server.on("/alarms/delete", HTTP_POST, handleDeleteAlarm); // expects ?i=...
  server.on("/alarms/toggle", HTTP_POST, handleToggleAlarm);
  server.on("/alarms/test",   HTTP_POST, handleTestAlarm);

  server.on("/alarm_start", HTTP_POST, [](){ startAlarmInfinite(); server.send(200,"application/json","{}"); });
  server.on("/alarm_stop",  HTTP_POST, [](){ stopAlarmNow();       server.send(200,"application/json","{}"); });
// GET current alarm config
server.on("/alarm/config", HTTP_GET, [](){
  StaticJsonDocument<128> d;
  d["mode"] = (int)alarmSound;        // 0=Classic,1=Pulse,2=Rising,3=Bell
  d["volume"] = (int)alarmVolumePct;  // 0..100
  String out; serializeJson(d,out);
  server.send(200,"application/json; charset=utf-8",out);
});

// POST set config: { "mode":"classic|pulse|rising|bell", "volume":0..100 }
server.on("/alarm/config", HTTP_POST, [](){
  if (!server.hasArg("plain")) { server.send(400,"application/json","{\"error\":\"bad json\"}"); return; }
  StaticJsonDocument<256> d;
  if (deserializeJson(d, server.arg("plain"))) { server.send(400,"application/json","{\"error\":\"bad json\"}"); return; }

  if (d.containsKey("mode")) {
    String m = (const char*)d["mode"]; m.toLowerCase();
    if      (m=="classic") alarmSound = ALARM_CLASSIC;
    else if (m=="pulse")   alarmSound = ALARM_PULSE;
    else if (m=="rising")  alarmSound = ALARM_RISING;
    else if (m=="bell")    alarmSound = ALARM_BELL;
  }
  if (d.containsKey("volume")) {
    int v = (int)d["volume"];
    alarmVolumePct = (uint8_t) constrain(v, 0, 100);
  }
  save_alarm_cfg();
  server.send(200,"application/json; charset=utf-8","{\"ok\":true}");
});

    server.begin();

    // --- Audio subsystems (PTT) ---
    i2s_install_rx();          // mic (leave enabled, this worked for you)
    i2s_tx_install_i16();      // speaker

    // Allocate audio buffer
    pcm16 = (int16_t*)ps_malloc(BUF_SAMPLES * sizeof(int16_t));
    if (!pcm16) pcm16 = (int16_t*)malloc(BUF_SAMPLES * sizeof(int16_t));
    if (!pcm16) {
      Serial.println("Failed to alloc audio buffer.");
      while (1) delay(1000);
    }

    // Start the PTT task (core 0, medium stack)
    xTaskCreatePinnedToCore(pttTask, "pttTask", 8192, nullptr, 1, nullptr, 0);

    addEvent("System started");
  }

  /* ========================= LOOP ========================= */
  void loop() {
    server.handleClient();

    // Render UI once per state change (main thread)
  static UIState lastDrawn = UI_SLEEP;
  if (uiState != lastDrawn) {
    lastDrawn = uiState;
    switch (uiState) {
      case UI_LISTEN:
        tft.fillScreen(COL_BLACK);
        drawJPG("/listen.jpg");
        break;
      case UI_THINK:
        tft.fillScreen(COL_BLACK);
        drawJPG("/think.jpg");
        break;
      case UI_SPEAK:
        tft.fillScreen(COL_BLACK);
        drawJPG("/speak.jpg");
        break;
      case UI_MAIN:
        showMain();
        break;
      case UI_SLEEP:
        goSleep();
        break;
    }
  }

    // ========= IR MOTION (integrator debounce) =========
    bool d = rawDetected();
    if (d) { if (integ < ON_COUNT) integ++; }
    else   { if (integ > -OFF_COUNT) integ--; }

    prevStableDetected = stableDetected;
    if (integ >= ON_COUNT)        stableDetected = true;
    else if (integ <= -OFF_COUNT) stableDetected = false;

    unsigned long now = millis();

    // Rising edge = motion detected
    if (!prevStableDetected && stableDetected) {
      addEvent("👋 Motion detected");
        delay(100);                 // let IRQ bursts finish
      openFlower("motion");                 // open on motion
      lastDetectMs   = now;                 // for motion auto-close
      lastActivityMs = now;                 // also counts as user activity (prevents sleep)
    }
    if (stableDetected) {
      lastDetectMs   = now;                 // extend motion window while still detecting
      lastActivityMs = now;                 // keep device awake while motion present
    }

    // Auto-close flower after no motion for a while
    if (isOpen && !stableDetected && (now - lastDetectMs >= NO_MOTION_TIMEOUT)) {
      closeFlower("inactivity");
    }

    // ========= TOUCH (interrupt + debounce) =========
    if (touchISRCount != lastCountSeen) {
      lastCountSeen = touchISRCount;
      if (now - lastDebounceMs >= TOUCH_DEBOUNCE_MS) {
        lastDebounceMs = now;
        touchFlag = true;
      }
    }

    // Touch behavior: show cute welcome, then Main
    if (touchFlag) {
      touchFlag = false;
      activated = true;
      drawCuteWelcome();
      showingWelcome = true;
      welcomeShownAt = now;
      lastActivityMs = now;                 // user interaction -> reset sleep timer
    }

    if (activated && showingWelcome && (millis() - welcomeShownAt >= WELCOME_DURATION_MS)) {
      showMain();                           // switch to saved main message after welcome
      showingWelcome = false;
      lastActivityMs = millis();
    }

    // ========= SYSTEM AUTO-SLEEP (no activity) =========
    if (activated && (millis() - lastActivityMs >= SLEEP_TIMEOUT_MS)) {
      goSleep();                            // puts OLED into sleep (pink flower) and any app-level sleep state
      activated = false;                    // optional: mark deactivated until next touch
    }
    check_alarms_tick();
    // --- Alarm overlay blink on top of the pink flower ---
  if (alarmPlaying) {
    uint32_t now = millis();
    if (now - lastAlarmBlinkMs >= ALARM_BLINK_MS) {
      lastAlarmBlinkMs = now;
      alarmFlashOn = !alarmFlashOn;

      // Redraw the base only occasionally to avoid buildup (every other frame)
      // On toggle ON: redraw base flower, then draw overlay
      if (alarmFlashOn) {
        drawPinkFlowerIdle();
      }
      drawAlarmOverlay(alarmFlashOn);
    }
  }

  // ---- Sleep clock refresh (once per minute while in UI_SLEEP) ----
if (showSleepClock && uiState == UI_SLEEP) {
  static uint32_t lastClockPollMs = 0;
  uint32_t nowMs = millis();
  if (nowMs - lastClockPollMs >= 500) {      // poll twice a second
    lastClockPollMs = nowMs;
    struct tm t;
    if (getLocalTime(&t, 20)) {
      if (t.tm_min != lastClockMinute) {     // minute changed → redraw
        lastClockMinute = t.tm_min;
        drawPinkFlowerIdle();                 // clean background
        String ts;
        if (getTimeString(ts)) {
          int y = tft.height() - 48;
          drawClockTextCentered(ts, y, COL_WHITE, rgb888_to_565(0,0,0));
        }
      }
    }
  }
}
    // ===== LED STRIP + ROTARY (non-blocking) =====
    nowMs = millis();
    pollEncoder();

    // Auto-off after inactivity
    if (st.powered && (nowMs - lastInputAt > AUTO_OFF_MS)) {
      st.powered = false;
      strip.clear(); strip.show();
    }

    if (st.powered) {
      if (st.nightMode) {
        // Very dim warm white
        for (int i=0;i<ACTIVE_LEDS;i++) strip.setPixelColor(i, applyGamma(255, 200, 120));
        for (int i=ACTIVE_LEDS;i<NUM_LEDS;i++) strip.setPixelColor(i,0);
        strip.setBrightness(18);
        strip.show();
      } else {
        switch (st.effect) {
          case SOLID:    renderSolid();    break;
          case BREATHE:  renderBreathe();  break;
          case RAINBOW:  renderRainbow();  break;
          case CHASE:    renderChase();    break;
          case SPARKLE:  renderSparkle();  break;
          case WAVE:     renderWave();     break;
          case COMET:    renderComet();    break;
          case TWINKLE:  renderTwinkle();  break;
          default:       renderSolid();    break;
        }
      }
    }

    // Small sampling delay for IR loop timing (keep responsive to web + touch)
    delay(SAMPLE_MS);
  }
