/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║                                                                  ║
 * ║                      YUMO  BUILDS                                ║
 * ║                     —  WEATHER  —                                ║
 * ║                                                                  ║
 * ║   ESP32-C3 Super Mini  |  v2.0                                   ║
 * ║   Temperature · Humidity · Clock · Web Interface                 ║
 * ║                                                                  ║
 * ╚══════════════════════════════════════════════════════════════════╝
 *
 * ── HARDWARE ────────────────────────────────────────────────────────
 *
 *   MCU      ESP32-C3 Super Mini
 *   SENSOR   SHT3X  — temperature + humidity (I2C)
 *   DISPLAY  0.96" OLED 128×32 SSD1306 (I2C)
 *
 * ── PIN WIRING MAP ──────────────────────────────────────────────────
 *
 *   PIN  8   I2C SDA  →  OLED SDA  +  SHT3X SDA
 *   PIN  9   I2C SCL  →  OLED SCL  +  SHT3X SCL
 *   PIN  3   SECS LED    (330Ω → GND)  yellow — flashes with clock colon
 *   PIN  0   GLOW LED    (330Ω → GND)  purple — pulses with YUMO page frame
 *   PIN 10   FLASH LED   (330Ω → GND)  blue   — full brightness 3× per minute
 *   PIN  1   PROX LED    (330Ω → GND)  red    — glows when hand near wire
 *   PIN  2   TEXT LED    (330Ω → GND)  GREEN  — sine-wave when web text arrives
 *   PIN  7   ANTENNA     bare wire only, no resistor — proximity touch input
 *
 * ── DISPLAY PAGES ───────────────────────────────────────────────────
 *
 *   CLOCK      Auto-detected city + timezone (NTP) — HH:MM with flashing colon + seconds
 *   HUMIDITY   Live humidity reading from SHT3X
 *   TEMPERATURE  Live temperature reading from SHT3X
 *   YUMO       Brand page — animated frame + sine contrast pulse
 *   TEXT       Web message typed character by character on screen
 *
 * ── WEB INTERFACE ───────────────────────────────────────────────────
 *
 *   After WiFi connects, visit  http://[IP shown on OLED]
 *     → Live temperature and humidity readings
 *     → Send any text — appears on OLED one character at a time
 *     → GREEN LED waves while message displays
 *
 * ── NIGHT MODE  (00:00 → 05:00) ─────────────────────────────────────
 *
 *   OLED dims to minimum contrast
 *   PIN 3 (SECS) and PIN 1 (PROX) reduce to very low brightness
 *   PIN 0 (GLOW) dims on brand page
 *   PIN 10 (FLASH) continues its 3× per minute pulse
 *
 * ── WIFI ────────────────────────────────────────────────────────────
 *
 *   First boot  →  connect phone to  YUMO-WEATHER  hotspot
 *                  open browser at  192.168.4.1
 *                  enter your home WiFi password → saved to flash
 *   Next boots  →  connects automatically, no portal needed
 *   Change WiFi →  uncomment  wm.resetSettings()  once, upload,
 *                  reconnect via portal, comment back out, re-upload
 *
 * ── LIBRARIES  (Arduino IDE → Library Manager) ──────────────────────
 *
 *   Adafruit SSD1306
 *   Adafruit GFX Library
 *   Adafruit SHT31 Library
 *   WiFiManager  by tzapu
 *
 * ════════════════════════════════════════════════════════════════════*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_SHT31.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <time.h>

// ────────────── PINS ──────────────
#define I2C_SDA      8
#define I2C_SCL      9
#define LED_SECS     3    // PWM dim — blink every second
#define LED_GLOW     0    // PWM — brand page frame sync (dim in night mode)
#define NIGHT_GLOW_DIM  30  // brightness of glow LED in night mode (0–255)
#define LED_FLASH   10    // PWM — full bright day / dim at night — 3x/60s
#define ANTENNA_PIN  7    // TOUCH WIRE — bare wire only, no resistor
#define LED_PROX     1    // PWM — proximity LED, glows when hand near antenna
#define LED_TEXT     2    // PWM — GREEN, sine-wave on web text

// ────────────── OLED ──────────────
#define SCREEN_W    128
#define SCREEN_H     32
#define OLED_ADDR   0x3C

// ────────────── PWM ───────────────
#define PWM_FREQ    1000
#define PWM_BITS       8   // 0–255

// ────────────── LED LEVELS ────────
#define DIM_SECS      20   // GPIO3 on-brightness (dim, 0–255)
// GPIO1 PROX: off when no hand near antenna, ramps 0→255 as hand approaches

// ────────────── NIGHT MODE (00:00–05:00) ──────────────
// Low power: LEDs off, slow sensor + display refresh, skip brand page
#define NIGHT_HOUR_START    0    // midnight
#define NIGHT_HOUR_END      5    // 5am
#define NIGHT_CONTRAST      6    // OLED very dim (0–255)
#define NIGHT_SECS_DIM      4    // clock colon LED at night
#define NIGHT_REFRESH_MS 2000    // page redraw interval at night (ms)

// ────────────── TOUCH ─────────────
// Bare wire on GPIO7. INPUT with 10MΩ to GND.
// Touch wire → HIGH → LED fades up smoothly.
// Release → LOW → LED fades down smoothly.
#define TOUCH_RAMP_UP   30   // fast rise
#define TOUCH_RAMP_DOWN  2   // very slow fade
#define TOUCH_TICK_MS    8   // update very frequently
float touchLevel = 0.0;      // 0.0–255.0, drives LED_PROX (pin 1)

// ════════════════════════════════════════════════════════════
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);
Adafruit_SHT31   sht31;
WiFiManager      wm;
WebServer        server(80);

// ────── Sensor data ──────
float gTemp = 0.0, gHumi = 0.0;

// ────── Location / timezone (auto-detected on connect) ──────
String gCity = "LOCAL";   // updated from IP geolocation on WiFi connect

// ────── Timers ──────
unsigned long sensorTimer = 0, flashTimer = 0;
unsigned long secTimer    = 0, touchTimer = 0;
bool          secLedOn    = false;

// ────── Web text ──────
String pendingText   = "";   // incoming from web
String lastText      = "";   // persists — shown in rotation until replaced
bool   hasNewText    = false;

// ────── Night mode ──────
bool nightMode = false;

// ────── Clock page LED_SECS ownership ──────
bool clockPageActive = false;   // suppresses bgTasks LED_SECS pulse

// ────── Hour celebration ──────
int lastCelebHour = -1;         // tracks which hour we last celebrated

// ────── Pages ──────
enum Page { PAGE_CLOCK, PAGE_HUMID, PAGE_TEMP, PAGE_BRAND, PAGE_TEXT };
Page curPage = PAGE_CLOCK;

// ════════════════════════════════════════════════════════════
//  WEB SERVER — HTML page
// ════════════════════════════════════════════════════════════
const char WEB_PAGE[] PROGMEM = R"rawhtml(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta charset="UTF-8">
<title>YUMO BUILDS</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0;}
  body{background:#000;color:#fff;font-family:monospace;min-height:100vh;
       display:flex;flex-direction:column;align-items:center;padding:30px 20px;}
  .title{font-size:18px;letter-spacing:6px;border:1px solid #fff;
         padding:12px 24px;margin-bottom:24px;}
  .sensors{display:flex;gap:24px;margin-bottom:28px;font-size:14px;
           letter-spacing:2px;}
  .temp{color:#ff6b35;} .humi{color:#4ecdc4;}
  .section{font-size:11px;letter-spacing:3px;color:#888;margin-bottom:12px;}
  input[type=text]{width:100%;max-width:320px;background:#111;color:#fff;
    border:1px solid #444;padding:12px;font-family:monospace;font-size:14px;
    outline:none;letter-spacing:1px;}
  input[type=text]:focus{border-color:#fff;}
  button{background:#fff;color:#000;border:none;padding:12px 28px;
    font-family:monospace;font-size:13px;letter-spacing:3px;cursor:pointer;
    margin-top:10px;width:100%;max-width:320px;}
  button:active{background:#ccc;}
  .status{font-size:11px;color:#666;margin-top:12px;letter-spacing:2px;
          min-height:16px;}
  .divider{width:100%;max-width:320px;border-top:1px solid #222;
           margin:24px 0;}
  .hint{font-size:10px;color:#444;letter-spacing:1px;text-align:center;
        max-width:300px;}
</style></head><body>
<div class="title">YUMO BUILDS</div>
<div class="sensors">
  <span class="temp" id="t">TEMP: --.-°C</span>
  <span class="humi" id="h">HUM: --.-%</span>
</div>
<div class="section">SEND TEXT TO DISPLAY</div>
<input type="text" id="msg" maxlength="48" placeholder="type message..." autocomplete="off">
<button onclick="sendText()">SEND →</button>
<div class="status" id="status"></div>
<div class="divider"></div>
<div class="hint">Text will appear on the OLED one character at a time</div>
<script>
function sendText(){
  var m=document.getElementById('msg').value.trim();
  if(!m){return;}
  document.getElementById('status').innerText='SENDING...';
  fetch('/text?msg='+encodeURIComponent(m))
    .then(r=>r.text())
    .then(()=>{
      document.getElementById('status').innerText='SENT TO DISPLAY';
      document.getElementById('msg').value='';
      setTimeout(()=>{document.getElementById('status').innerText='';},3000);
    }).catch(()=>{document.getElementById('status').innerText='ERROR';});
}
function refresh(){
  fetch('/sensors').then(r=>r.json()).then(d=>{
    document.getElementById('t').innerText='TEMP: '+d.temp+'°C';
    document.getElementById('h').innerText='HUM: '+d.humi+'%';
  }).catch(()=>{});
}
document.getElementById('msg').addEventListener('keydown',function(e){
  if(e.key==='Enter'){sendText();}
});
refresh();
setInterval(refresh,5000);
</script></body></html>
)rawhtml";

// ════════════════════════════════════════════════════════════
//  WEB SERVER HANDLERS
// ════════════════════════════════════════════════════════════
void handleRoot() {
  server.send_P(200, "text/html", WEB_PAGE);
}

void handleText() {
  if (server.hasArg("msg")) {
    String msg = server.arg("msg");
    msg.trim();
    if (msg.length() > 0) {
      pendingText = msg;
      hasNewText  = true;
      Serial.printf("WEB: new text → \"%s\"\n", msg.c_str());
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleSensors() {
  char buf[64];
  snprintf(buf, sizeof(buf),
           "{\"temp\":\"%.1f\",\"humi\":\"%.1f\"}", gTemp, gHumi);
  server.send(200, "application/json", buf);
}

void startWebServer() {
  server.on("/",        handleRoot);
  server.on("/text",    handleText);
  server.on("/sensors", handleSensors);
  server.begin();
  Serial.println("Web server started.");
  Serial.print("Visit http://");
  Serial.println(WiFi.localIP());
}

// ════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════
void setup() {
  // ── Kill green LED immediately — prevent dim glow on reset ──
  pinMode(LED_TEXT, OUTPUT);
  digitalWrite(LED_TEXT, LOW);

  Serial.begin(115200);
  delay(1000);

  // ── PWM LEDs ──
  ledcAttach(LED_SECS,  PWM_FREQ, PWM_BITS);  ledcWrite(LED_SECS,  0);
  ledcAttach(LED_PROX,  PWM_FREQ, PWM_BITS);  ledcWrite(LED_PROX,  0);
  ledcAttach(LED_FLASH, PWM_FREQ, PWM_BITS);  ledcWrite(LED_FLASH, 0);
  ledcAttach(LED_TEXT,  PWM_FREQ, PWM_BITS);  ledcWrite(LED_TEXT,  0);

  // ── Digital LEDs ──
  ledcAttach(LED_GLOW, PWM_FREQ, PWM_BITS);  ledcWrite(LED_GLOW, 0);

  // ── Touch wire ──────────────────────────────────────────
  // ── Touch wire — bare wire on GPIO7, no resistor needed ──
  pinMode(ANTENNA_PIN, INPUT);

  // ── I2C + OLED ──
  Wire.begin(I2C_SDA, I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED FAIL"); for (;;);
  }

  // ── SHT3X ──
  if (!sht31.begin(0x44)) {
    Serial.println("SHT31 FAIL"); for (;;);
  }

  // ── LED boot test ──
  testLEDs();

  // ── Boot page only on true power-on, not after software restart ──
  if (esp_reset_reason() == ESP_RST_POWERON || esp_reset_reason() == ESP_RST_UNKNOWN) {
    runBootPage();
  }

  runWifiPage();
  readSensor();

  // ── Timers ──
  sensorTimer = millis();
  flashTimer  = millis() - 55000;
  secTimer    = millis();
  touchTimer  = millis();

  Serial.println("--- YUMO BUILDS READY ---");
}

// ════════════════════════════════════════════════════════════
//  LED BOOT TEST  — wave sequence
// ════════════════════════════════════════════════════════════
void testLEDs() {
  Serial.println("=== LED BOOT TEST ===");

  // All off to start
  ledcWrite(LED_SECS,  0); ledcWrite(LED_GLOW,  0);
  ledcWrite(LED_FLASH, 0); ledcWrite(LED_PROX,  0);
  ledcWrite(LED_TEXT,  0);

  // ── Wave: each LED fades in then out, one by one ──
  const uint8_t leds[] = { LED_SECS, LED_GLOW, LED_FLASH, LED_PROX, LED_TEXT };
  const uint8_t count  = sizeof(leds);

  for (uint8_t i = 0; i < count; i++) {
    // Fade in
    for (int b = 0; b <= 255; b += 12) {
      ledcWrite(leds[i], (uint8_t)b);
      delay(4);
    }
    ledcWrite(leds[i], 255);
    delay(60);
    // Fade out
    for (int b = 255; b >= 0; b -= 12) {
      ledcWrite(leds[i], (uint8_t)b);
      delay(4);
    }
    ledcWrite(leds[i], 0);
    delay(20);
  }

  // ── All on together — full blast ──
  delay(80);
  for (int b = 0; b <= 255; b += 15) {
    ledcWrite(LED_SECS,  (uint8_t)b);
    ledcWrite(LED_GLOW,  (uint8_t)b);
    ledcWrite(LED_FLASH, (uint8_t)b);
    ledcWrite(LED_PROX,  (uint8_t)b);
    ledcWrite(LED_TEXT,  (uint8_t)b);
    delay(4);
  }
  delay(400);

  // ── All fade out together ──
  for (int b = 255; b >= 0; b -= 8) {
    ledcWrite(LED_SECS,  (uint8_t)b);
    ledcWrite(LED_GLOW,  (uint8_t)b);
    ledcWrite(LED_FLASH, (uint8_t)b);
    ledcWrite(LED_PROX,  (uint8_t)b);
    ledcWrite(LED_TEXT,  (uint8_t)b);
    delay(6);
  }
  ledcWrite(LED_SECS,  0); ledcWrite(LED_GLOW,  0);
  ledcWrite(LED_FLASH, 0); ledcWrite(LED_PROX,  0);
  ledcWrite(LED_TEXT,  0);

  Serial.println("=== BOOT TEST DONE ===\n");
}

// ════════════════════════════════════════════════════════════
//  MAIN LOOP
// ════════════════════════════════════════════════════════════
void loop() {
  // New text from web — save it, will show in rotation
  if (hasNewText) {
    hasNewText = false;
    lastText   = pendingText;
    pendingText = "";
  }

  switch (curPage) {
    case PAGE_CLOCK: runClockPage(6000); if(curPage==PAGE_CLOCK) fadeToPage(PAGE_HUMID); break;
    case PAGE_HUMID: runHumidPage(4000); if(curPage==PAGE_HUMID) fadeToPage(PAGE_TEMP);  break;
    case PAGE_TEMP:  runTempPage(4000);  if(curPage==PAGE_TEMP)  fadeToPage(PAGE_BRAND); break;
    case PAGE_BRAND:
      if (nightMode) { fadeToPage(PAGE_CLOCK); break; }  // skip at night
      runBrandPage(9000);
      if (curPage == PAGE_BRAND) {
        // After brand page — show text if we have one, else go to clock
        if (lastText.length() > 0) fadeToPage(PAGE_TEXT);
        else                        fadeToPage(PAGE_CLOCK);
      }
      break;
    case PAGE_TEXT:
      runTextPage();
      if (curPage == PAGE_TEXT) fadeToPage(PAGE_CLOCK);
      break;
  }
}

// ════════════════════════════════════════════════════════════
//  TOUCH LED UPDATE
//  INPUT_PULLUP: idle=HIGH, touched=LOW
//  Touch → ramps up to 255. Release → ramps down to 0.
// ════════════════════════════════════════════════════════════
void updateTouchLED() {
  bool touched = (digitalRead(ANTENNA_PIN) == HIGH);
  if (touched) touchLevel = min(255.0f, touchLevel + TOUCH_RAMP_UP);
  else         touchLevel = max(0.0f,   touchLevel - TOUCH_RAMP_DOWN);
  ledcWrite(LED_PROX, (uint8_t)touchLevel);
  ledcWrite(LED_PROX, (uint8_t)touchLevel);
}

// ════════════════════════════════════════════════════════════
//  NIGHT MODE CHECK
// ════════════════════════════════════════════════════════════
bool checkNightMode() {
  struct tm t;
  if (!getLocalTime(&t)) return false;
  return (t.tm_hour >= NIGHT_HOUR_START && t.tm_hour < NIGHT_HOUR_END);
}

// ════════════════════════════════════════════════════════════
//  HOUR CELEBRATION  — fires once per hour, day mode only
//  Wave ripple → all flash → fade out
// ════════════════════════════════════════════════════════════
void hourCelebration() {
  const uint8_t all[] = { LED_SECS, LED_GLOW, LED_FLASH, LED_PROX, LED_TEXT };
  const uint8_t N = sizeof(all);

  // ── Round 1: ripple left to right, each LED fades in/out ──
  for (int r = 0; r < 2; r++) {
    for (uint8_t i = 0; i < N; i++) {
      for (int b = 0; b <= 255; b += 25) { ledcWrite(all[i], b); delay(3); }
      for (int b = 255; b >= 0; b -= 25) { ledcWrite(all[i], b); delay(3); }
      ledcWrite(all[i], 0);
    }
  }

  // ── Round 2: all flash together 4 times ──
  for (int f = 0; f < 4; f++) {
    for (uint8_t i = 0; i < N; i++) ledcWrite(all[i], 255);
    delay(80);
    for (uint8_t i = 0; i < N; i++) ledcWrite(all[i], 0);
    delay(80);
  }

  // ── Round 3: slow fade up all together then fade out ──
  for (int b = 0; b <= 255; b += 5) {
    for (uint8_t i = 0; i < N; i++) ledcWrite(all[i], b);
    delay(6);
  }
  delay(300);
  for (int b = 255; b >= 0; b -= 5) {
    for (uint8_t i = 0; i < N; i++) ledcWrite(all[i], b);
    delay(6);
  }

  // ── All off ──
  for (uint8_t i = 0; i < N; i++) ledcWrite(all[i], 0);
}

// ════════════════════════════════════════════════════════════
//  BACKGROUND TASKS
// ════════════════════════════════════════════════════════════
void bgTasks() {
  unsigned long now = millis();

  server.handleClient();
  nightMode = checkNightMode();

  if (nightMode) {
    setContrast(NIGHT_CONTRAST);
    // ── All LEDs off at night to save battery ──
    if (!clockPageActive) ledcWrite(LED_SECS, 0);
    ledcWrite(LED_FLASH, 0);
    ledcWrite(LED_GLOW,  0);
    // Touch still works but capped very dim
    if (now - touchTimer >= TOUCH_TICK_MS) {
      touchTimer = now;
      bool touched = (digitalRead(ANTENNA_PIN) == HIGH);
      if (touched) touchLevel = min(255.0f, touchLevel + TOUCH_RAMP_UP);
      else         touchLevel = max(0.0f,   touchLevel - TOUCH_RAMP_DOWN);
      ledcWrite(LED_PROX,  (uint8_t)(touchLevel / 8));
      ledcWrite(LED_PROX, (uint8_t)(touchLevel / 8));
    }
    // Sensor: read every 60s at night instead of every 2s
    if (now - sensorTimer >= 60000) {
      sensorTimer = now;
      readSensor();
    }
  } else {
    if (!clockPageActive && now - secTimer >= 1000) {
      secTimer = now;
      ledcWrite(LED_SECS, DIM_SECS); delay(30); ledcWrite(LED_SECS, 0);
    }
    if (now - flashTimer >= 60000) {
      flashTimer = now;
      for (int i = 0; i < 3; i++) {
        ledcWrite(LED_FLASH, 255); delay(35);
        ledcWrite(LED_FLASH, 0);   delay(55);
      }
    }
    // ── Hour celebration ──
    struct tm tc;
    if (getLocalTime(&tc) && tc.tm_min == 0 && tc.tm_sec == 0
        && tc.tm_hour != lastCelebHour) {
      lastCelebHour = tc.tm_hour;
      hourCelebration();
    }
    if (now - sensorTimer >= 2000) {
      sensorTimer = now;
      readSensor();
    }
    // Touch updated in page loops for fast response
  }
}

// ════════════════════════════════════════════════════════════
//  HELPERS
// ════════════════════════════════════════════════════════════
void drawFrame() {
  display.drawRect(0, 0, SCREEN_W, SCREEN_H, SSD1306_WHITE);
}

void setContrast(uint8_t val) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write(0x00); Wire.write(0x81); Wire.write(val);
  Wire.endTransmission();
}

void fadeToPage(Page next) {
  ledcWrite(LED_GLOW, 0);
  for (int c = 180; c >= 0; c -= 20) { setContrast((uint8_t)c); delay(15); }
  curPage = next;
  setContrast(180);
}

// ════════════════════════════════════════════════════════════
//  TEXT LED WAVE  (GPIO2 — green)
//  Smooth slow sine-wave. Call continuously with page start time.
// ════════════════════════════════════════════════════════════
void waveTextLED(unsigned long startMs) {
  float t    = (millis() - startMs) / 1000.0f;
  float sine = (sinf(t * 1.6f) + 1.0f) / 2.0f;   // slow, 0.0–1.0
  ledcWrite(LED_TEXT, (uint8_t)(sine * 220));       // max 220 — not harsh white
}

// ════════════════════════════════════════════════════════════
//  ★  TEXT PAGE
//  Types lastText slowly on OLED, holds 5s, returns to rotation.
//  lastText persists — shown every cycle until new text arrives.
// ════════════════════════════════════════════════════════════
void runTextPage() {
  Serial.printf("TEXT PAGE: \"%s\"\n", lastText.c_str());

  String msg     = lastText;
  int    lineMax = (SCREEN_W - 8) / 6;
  unsigned long waveStart = millis();

  // ── Type text one character at a time ──
  String shown = "";
  for (int i = 0; i <= (int)msg.length(); i++) {
    bgTasks();

    if (hasNewText) {
      lastText    = pendingText;
      pendingText = "";
      hasNewText  = false;
      msg         = lastText;
      shown       = "";
      i           = 0;
    }

    display.clearDisplay(); drawFrame();
    display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
    display.setCursor(4, 2); display.print("MSG:");

    String visible = shown;
    if ((int)visible.length() > lineMax)
      visible = visible.substring(visible.length() - lineMax);
    display.setCursor(4, 14); display.print(visible);
    if ((millis() / 300) % 2 == 0) display.print("_");
    display.display();

    if (i < (int)msg.length()) shown += msg[i];

    // ── 75ms typing delay broken into 5ms steps — LED stays smooth ──
    unsigned long charStart = millis();
    while (millis() - charStart < 75) {
      waveTextLED(waveStart);
      delay(5);
    }
  }

  // ── Hold full message for 5 seconds ──
  unsigned long hold = millis();
  while (millis() - hold < 5000) {
    bgTasks();
    waveTextLED(waveStart);
    if (hasNewText) break;

    display.clearDisplay(); drawFrame();
    display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
    display.setCursor(4, 2); display.print("MSG:");
    String visible = msg;
    if ((int)visible.length() > lineMax)
      visible = visible.substring(visible.length() - lineMax);
    display.setCursor(4, 14); display.print(visible);
    display.display();
    delay(5);
  }

  // ── Smooth fade out ──
  for (int b = ledcRead(LED_TEXT); b >= 0; b -= 5) {
    ledcWrite(LED_TEXT, (uint8_t)b);
    delay(8);
  }
  ledcWrite(LED_TEXT, 0);

  // lastText NOT cleared — stays for next rotation cycle
}

// ════════════════════════════════════════════════════════════
//  ★  BOOT PAGE
// ════════════════════════════════════════════════════════════
void runBootPage() {
  for (int f = 0; f < 4; f++) {
    display.clearDisplay(); drawFrame();
    display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
    display.setCursor((SCREEN_W - 11*6) / 2, 6);
    display.print("YUMO BUILDS"); display.display(); delay(180);
    display.clearDisplay(); display.display(); delay(120);
  }

  display.clearDisplay(); drawFrame();
  display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
  display.setCursor((SCREEN_W - 11*6) / 2, 4);
  display.print("YUMO BUILDS");

  const int bx=8, by=20, bw=112, bh=7;
  display.drawRect(bx, by, bw, bh, SSD1306_WHITE);
  display.display();

  for (int i = 1; i <= bw-2; i++) {
    display.fillRect(bx+1, by+1, i, bh-2, SSD1306_WHITE);
    display.display();
    delay(1500 / (bw-2));
  }
}

// ════════════════════════════════════════════════════════════
//  AUTO TIMEZONE  — detects location from IP, sets clock offset
//  API: worldtimeapi.org/api/ip  (free, no key needed)
//  Sets gCity (e.g. "PARIS", "NEW YORK") and configures NTP offset.
//  Falls back to UTC if unreachable.
// ════════════════════════════════════════════════════════════
void fetchTimezone() {
  display.clearDisplay(); drawFrame();
  display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
  display.setCursor(16, 5);  display.print("DETECTING");
  display.setCursor(16, 17); display.print("LOCATION...");
  display.display();

  HTTPClient http;
  http.begin("http://worldtimeapi.org/api/ip");
  http.setTimeout(6000);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();

    // ── Parse utc_offset → e.g. "+01:00" or "-05:30" ──
    int idx = body.indexOf("\"utc_offset\":\"");
    if (idx >= 0) {
      String raw = body.substring(idx + 14, idx + 21); // "+HH:MM"
      int sign = (raw[0] == '-') ? -1 : 1;
      int h    = raw.substring(1, 3).toInt();
      int m    = raw.substring(4, 6).toInt();
      long offsetSec = sign * (h * 3600L + m * 60L);
      configTime(offsetSec, 0, "pool.ntp.org", "time.google.com");
      Serial.printf("TZ offset: %s (%ld s)\n", raw.c_str(), offsetSec);
    }

    // ── Parse timezone city → "Europe/Paris" → "PARIS" ──
    //    handles multi-part like "America/Indiana/Indianapolis"
    idx = body.indexOf("\"timezone\":\"");
    if (idx >= 0) {
      int end   = body.indexOf("\"", idx + 12);
      String tz = body.substring(idx + 12, end);          // "Europe/Paris"
      int slash = tz.lastIndexOf('/');
      String city = (slash >= 0) ? tz.substring(slash + 1) : tz;
      city.replace("_", " ");
      city.toUpperCase();
      gCity = city;
      Serial.printf("City: %s\n", gCity.c_str());
    }

  } else {
    // Fallback — UTC, label stays "LOCAL"
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    gCity = "LOCAL";
    Serial.printf("TZ fetch failed (%d) — using UTC\n", code);
  }

  http.end();
}

// ════════════════════════════════════════════════════════════
//  ★  WIFI PAGE  — YUMO-WEATHER portal + web server start
// ════════════════════════════════════════════════════════════
void runWifiPage() {
  // Custom dark portal styling
  const char* portalCSS =
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0;}"
    "body{background:#000!important;color:#fff!important;"
    "font-family:monospace!important;min-height:100vh;"
    "display:flex;flex-direction:column;align-items:center;"
    "padding:30px 20px;}"
    "/* Header banner */"
    "body::before{content:'YUMO-WEATHER';"
    "display:block;font-size:15px;letter-spacing:8px;"
    "border:1px solid #fff;padding:10px 22px;"
    "margin-bottom:6px;}"
    "body::after{content:'WEATHER STATION';"
    "display:block;font-size:9px;letter-spacing:5px;"
    "color:#555;margin-bottom:28px;}"
    ".wrap{background:#000!important;border:1px solid #222!important;"
    "padding:20px!important;width:100%!important;max-width:340px!important;}"
    "h1{display:none!important;}"
    "h3{letter-spacing:3px!important;font-size:11px!important;"
    "color:#888!important;margin-bottom:14px!important;"
    "border-bottom:1px solid #222!important;padding-bottom:8px!important;}"
    "input[type=text],input[type=password]{"
    "background:#111!important;color:#fff!important;"
    "border:1px solid #333!important;border-radius:0!important;"
    "font-family:monospace!important;font-size:13px!important;"
    "padding:10px!important;width:100%!important;outline:none!important;"
    "letter-spacing:1px!important;margin-bottom:10px!important;}"
    "input:focus{border-color:#fff!important;}"
    "button,input[type=submit]{"
    "background:#fff!important;color:#000!important;border:none!important;"
    "font-family:monospace!important;font-size:12px!important;"
    "letter-spacing:4px!important;padding:12px!important;"
    "width:100%!important;cursor:pointer!important;margin-top:4px!important;}"
    "button:active,input[type=submit]:active{background:#ccc!important;}"
    "a{color:#444!important;font-size:10px!important;letter-spacing:1px!important;}"
    "a:hover{color:#888!important;}"
    "br{display:none!important;}"
    ".msg{font-size:10px!important;letter-spacing:2px!important;"
    "color:#888!important;margin:10px 0!important;}"
    "</style>";

  wm.setCustomHeadElement(portalCSS);
  wm.setTitle("YUMO-WEATHER");

  // To reset saved WiFi: uncomment once, upload, then comment back and re-upload.
  // wm.resetSettings();

  // AP callback — show on OLED when portal opens
  wm.setAPCallback([](WiFiManager*) {
    display.clearDisplay(); drawFrame();
    display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
    display.setCursor(4,  2); display.print("CONNECT PHONE TO:");
    display.setCursor(4, 12); display.print("> YUMO-WEATHER <");
    display.setCursor(4, 22); display.print("192.168.4.1");
    display.display();
    Serial.println("Portal open — connect to YUMO-WEATHER");
  });

  wm.setConnectTimeout(30);       // how long to try saved network
  wm.setConfigPortalTimeout(300); // portal stays open 5 min

  // Blocking autoConnect — reliable on ESP32-C3
  WiFi.mode(WIFI_AP_STA);
  bool res = wm.autoConnect("YUMO-WEATHER");

  if (!res) {
    // Portal timed out without connecting — show message and restart
    display.clearDisplay(); drawFrame();
    display.setTextColor(SSD1306_WHITE); display.setTextSize(1);
    display.setCursor(10, 5);  display.print("WIFI TIMEOUT");
    display.setCursor(10, 17); display.print("RESTARTING...");
    display.display();
    Serial.println("WiFi: portal timed out, restarting.");
    delay(3000);
    ESP.restart();
  }

  Serial.println("WiFi: connected!");
  Serial.print("IP: "); Serial.println(WiFi.localIP());

  // Auto-detect timezone + city from IP location
  fetchTimezone();

  // Start web server
  startWebServer();

  // Connected splash — show IP
  display.clearDisplay(); drawFrame();
  drawWifiIcon(8, 18, 3);
  display.setTextSize(1);
  display.setCursor(30, 3);  display.print("CONNECTED");
  display.setCursor(30, 16);
  display.print(WiFi.localIP());
  display.display();
  delay(3000);
}

void drawWifiIcon(int cx, int cy, int numArcs) {
  display.fillCircle(cx, cy, 2, SSD1306_WHITE);
  int radii[] = {5, 9, 13};
  for (int a = 0; a < numArcs && a < 3; a++) {
    for (float ang = 210.0f; ang <= 330.0f; ang += 6.0f) {
      float r = radii[a], rad = ang * PI / 180.0f;
      int px = cx + (int)(r * cos(rad));
      int py = cy + (int)(r * sin(rad));
      if (px > 0 && px < SCREEN_W-1 && py > 0 && py < SCREEN_H-1) {
        display.drawPixel(px, py,     SSD1306_WHITE);
        display.drawPixel(px, py - 1, SSD1306_WHITE);
      }
    }
  }
}

// ════════════════════════════════════════════════════════════
//  ★  CLOCK PAGE
// ════════════════════════════════════════════════════════════
void runClockPage(unsigned long dur) {
  clockPageActive = true;
  unsigned long start = millis();

  while (millis() - start < dur) {
    bgTasks();
    if (hasNewText) { clockPageActive = false; ledcWrite(LED_SECS, 0); return; }

    display.clearDisplay(); drawFrame();
    display.setTextColor(SSD1306_WHITE);

    struct tm t;
    if (!getLocalTime(&t)) {
      display.setTextSize(1);
      display.setCursor(22, 12); display.print("SYNCING TIME...");
    } else {
      char hh[3], mm[3], sec[3], day[12];
      strftime(hh,  sizeof(hh),  "%H",       &t);
      strftime(mm,  sizeof(mm),  "%M",       &t);
      strftime(sec, sizeof(sec), "%S",       &t);
      strftime(day, sizeof(day), "%a %d %b", &t);

      // Colon flashes every 500ms — LED_SECS stays in exact sync
      bool colonOn = (millis() / 500) % 2 == 0;
      uint8_t secsLevel = nightMode ? NIGHT_SECS_DIM : DIM_SECS;
      ledcWrite(LED_SECS, colonOn ? secsLevel : 0);

      // Header row
      display.setTextSize(1);
      display.setCursor(3, 2);  display.print(gCity);
      display.drawLine(47, 1, 47, 10, SSD1306_WHITE);
      display.setCursor(51, 2); display.print(day);
      display.drawLine(1, 10, 126, 10, SSD1306_WHITE);

      // Big time — HH : MM
      // textSize 2: each char = 12px wide, 16px tall. Start x=10
      display.setTextSize(2);
      display.setCursor(10, 14); display.print(hh);           // x=10..34
      if (colonOn) {
        display.setCursor(34, 14); display.print(":");         // x=34..46  flashing
      }
      display.setCursor(46, 14); display.print(mm);           // x=46..70

      // Seconds — right next to minutes, small, baseline-aligned
      display.setTextSize(1);
      display.setCursor(72, 22); display.print(":"); display.print(sec);
    }
    display.display();
    if (nightMode) {
      // slow refresh at night — update every 2s, still handle touch
      unsigned long nStart = millis();
      while (millis() - nStart < NIGHT_REFRESH_MS) {
        bgTasks();
        updateTouchLED();
        delay(50);
      }
    } else {
      for (int ti = 0; ti < 20; ti++) { updateTouchLED(); delay(10); }
    }
  }

  clockPageActive = false;
  ledcWrite(LED_SECS, 0);
}

// ════════════════════════════════════════════════════════════
//  ★  HUMIDITY PAGE
// ════════════════════════════════════════════════════════════
void runHumidPage(unsigned long dur) {
  unsigned long start = millis();
  while (millis() - start < dur) {
    bgTasks();
    if (hasNewText) return;

    display.clearDisplay(); drawFrame();
    display.setTextColor(SSD1306_WHITE);
    drawDropIcon(4, 3);
    display.setTextSize(1); display.setCursor(32, 2); display.print("HUMIDITY");
    display.setTextSize(2); display.setCursor(32, 14);
    char buf[8]; dtostrf(gHumi, 4, 1, buf);
    display.print(buf);
    display.setTextSize(1); display.print(" %");
    display.display();
    if (nightMode) {
      unsigned long nStart = millis();
      while (millis() - nStart < NIGHT_REFRESH_MS) { bgTasks(); delay(50); }
    } else {
      for(int ti=0;ti<20;ti++){updateTouchLED();delay(10);}
    }
  }
}

void drawDropIcon(int x, int y) {
  display.fillTriangle(x+7, y, x+1, y+12, x+13, y+12, SSD1306_WHITE);
  display.fillCircle(x+7, y+14, 6, SSD1306_WHITE);
  display.drawPixel(x+5, y+9,  SSD1306_BLACK);
  display.drawPixel(x+4, y+11, SSD1306_BLACK);
}

// ════════════════════════════════════════════════════════════
//  ★  TEMPERATURE PAGE
// ════════════════════════════════════════════════════════════
void runTempPage(unsigned long dur) {
  unsigned long start = millis();
  while (millis() - start < dur) {
    bgTasks();
    if (hasNewText) return;

    display.clearDisplay(); drawFrame();
    display.setTextColor(SSD1306_WHITE);
    drawThermIcon(4, 2);
    display.setTextSize(1); display.setCursor(32, 2); display.print("TEMPERATURE");
    display.setTextSize(2); display.setCursor(32, 14);
    char buf[8]; dtostrf(gTemp, 4, 1, buf);
    display.print(buf);
    display.setTextSize(1); display.print(" \xF7""C");
    display.display();
    if (nightMode) {
      unsigned long nStart = millis();
      while (millis() - nStart < NIGHT_REFRESH_MS) { bgTasks(); delay(50); }
    } else {
      for(int ti=0;ti<20;ti++){updateTouchLED();delay(10);}
    }
  }
}

void drawThermIcon(int x, int y) {
  display.fillRect(x+4, y,    5, 16, SSD1306_WHITE);
  display.fillRect(x+5, y+1,  3, 10, SSD1306_BLACK);
  display.fillRect(x+5, y+9,  3,  5, SSD1306_WHITE);
  display.fillCircle(x+6, y+21, 6, SSD1306_WHITE);
  display.fillCircle(x+6, y+21, 3, SSD1306_BLACK);
  display.fillCircle(x+6, y+21, 2, SSD1306_WHITE);
}

// ════════════════════════════════════════════════════════════
//  ★  BRAND PAGE
//  LED_GLOW (GPIO0) flashes in exact sync with outer frame.
//  OLED contrast follows sine wave (30–250).
// ════════════════════════════════════════════════════════════
void runBrandPage(unsigned long dur) {
  unsigned long start        = millis();
  bool          frameVisible = true;
  unsigned long frameToggle  = millis();

  if (!nightMode) ledcWrite(LED_GLOW, 60);
  else            ledcWrite(LED_GLOW, NIGHT_GLOW_DIM);

  while (millis() - start < dur) {
    bgTasks();
    if (hasNewText) { ledcWrite(LED_GLOW, 0); return; }

    // Brand page gets its own dramatic sine contrast
    float   elapsed  = (millis() - start) / 1000.0f;
    float   sine     = (sin(elapsed * 1.4f) + 1.0f) / 2.0f;
    uint8_t contrast = (uint8_t)(30 + sine * 220);
    setContrast(contrast);

    if (millis() - frameToggle >= 600) {
      frameToggle  = millis();
      frameVisible = !frameVisible;
      ledcWrite(LED_GLOW, frameVisible ? (nightMode ? NIGHT_GLOW_DIM : 60) : 0);
    }

    display.clearDisplay();
    if (frameVisible)
      display.drawRect(0, 0, SCREEN_W, SCREEN_H, SSD1306_WHITE);
    display.drawRect(3, 3, SCREEN_W-6, SCREEN_H-6, SSD1306_WHITE);
    for (int cx : {1, (int)SCREEN_W-3})
      for (int cy : {1, (int)SCREEN_H-3})
        display.fillRect(cx, cy, 2, 2, SSD1306_WHITE);
    const char* txt = "YUMO BUILDS";
    display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
    display.setCursor((SCREEN_W - (int)strlen(txt)*6) / 2, 12);
    display.print(txt);
    display.display();
    updateTouchLED(); delay(30);
  }

  ledcWrite(LED_GLOW, 0);
  setContrast(180);
}

// ════════════════════════════════════════════════════════════
//  SENSOR READ
// ════════════════════════════════════════════════════════════
void readSensor() {
  float t = sht31.readTemperature();
  float h = sht31.readHumidity();
  if (!isnan(t)) gTemp = t;
  if (!isnan(h)) gHumi = h;
  Serial.printf("TEMP: %.1f C  HUM: %.1f%%\n", gTemp, gHumi);
}
