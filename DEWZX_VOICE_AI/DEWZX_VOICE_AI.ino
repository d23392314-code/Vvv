/*******************************************************************************
 *  DEWZX VOICE AI — v1.0
 *  ESP32-S3-CAM | ST7789 240x240 | INMP441 | MAX98357A | OV2640
 *
 *  WIRING (dari schematic):
 *    LCD ST7789 : CLK=19, MOSI=20, RST=21, DC=47, BLK=38, CS=3
 *    INMP441 Mic: SCK=2,  WS=1,   SD=42,  L/R → GND
 *    MAX98357A  : BCK=40, LRC=41, DIN=39
 *    Button     : GPIO0 (tombol BOOT bawaan)
 *
 *  LIBRARIES (install di Arduino Library Manager):
 *    • Adafruit ST7789       v1.11+
 *    • Adafruit GFX Library  v1.11+
 *    • ArduinoJson           v6.21+  (BUKAN v7!)
 *
 *  BOARD SETTINGS (Arduino IDE / GitHub Actions):
 *    Board     : ESP32S3 Dev Module
 *    PSRAM     : OPI PSRAM  ← WAJIB ON
 *    Flash     : 8MB
 *    Partition : Huge App (3MB No OTA)
 *    CPU Freq  : 240MHz
 *    USB Mode  : Hardware CDC and JTAG
 *
 *  CARA PAKAI:
 *    Boot pertama (tanpa config) → LCD tampilkan instruksi setup
 *    Hubungkan HP ke WiFi "DEWZX_Setup" (pass: dewzx1234)
 *    Buka browser → 192.168.4.1 → isi form → Save
 *
 *    Setelah terhubung:
 *    Tekan tombol sebentar  → tanya pakai suara
 *    Tekan tombol lama >1s  → foto + suara (cocok soal sekolah)
 *******************************************************************************/

// ─────────────────────────────────────────────────────────────────────────────
//  INCLUDES
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <esp_camera.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <ArduinoJson.h>
#include <math.h>

// ─────────────────────────────────────────────────────────────────────────────
//  PIN DEFINITIONS
// ─────────────────────────────────────────────────────────────────────────────
// LCD ST7789 240x240 (Hardware SPI)
#define PIN_LCD_CLK   19
#define PIN_LCD_MOSI  20
#define PIN_LCD_RST   21
#define PIN_LCD_DC    47
#define PIN_LCD_BLK   38
#define PIN_LCD_CS     3

// INMP441 Microphone (I2S Port 0)
#define PIN_MIC_SCK    2
#define PIN_MIC_WS     1
#define PIN_MIC_SD    42

// MAX98357A Amplifier (I2S Port 1)
#define PIN_SPK_BCK   40
#define PIN_SPK_LRC   41
#define PIN_SPK_DIN   39

// Camera — AI-Thinker ESP32-S3-CAM pinout
#define CAM_PWDN    -1
#define CAM_RESET   -1
#define CAM_XCLK    15
#define CAM_SIOD     4
#define CAM_SIOC     5
#define CAM_D7      16
#define CAM_D6      17
#define CAM_D5      18
#define CAM_D4      12
#define CAM_D3      10
#define CAM_D2       8
#define CAM_D1       9
#define CAM_D0      11
#define CAM_VSYNC    6
#define CAM_HREF     7
#define CAM_PCLK    13

// Misc
#define PIN_BTN      0   // BOOT button = push-to-talk
#define PIN_LED     48   // onboard LED (opsional)

// ─────────────────────────────────────────────────────────────────────────────
//  AUDIO CONFIG
// ─────────────────────────────────────────────────────────────────────────────
#define MIC_SR        16000   // 16 kHz
#define MIC_BITS         16
#define MIC_CH            1
#define REC_SECS          4
#define AUDIO_BYTES  (MIC_SR * REC_SECS * (MIC_BITS / 8))   // 128 KB

// ─────────────────────────────────────────────────────────────────────────────
//  API CONFIG
// ─────────────────────────────────────────────────────────────────────────────
static const char* GENZAI_BASE   = "https://ai.genzicorp.com/v1";
static const char* CHAT_MODEL    = "qwen/qwen3.8b-cephalon-flash";
static const char* TTS_MODEL     = "cicada";
static const char* TTS_VOICE     = "default";
static const char* SYS_PROMPT    =
  "Kamu adalah DEWZX, asisten AI cerdas milik Dewangga. "
  "Kamu bisa melihat gambar, mendengar suara, dan membantu pelajaran sekolah. "
  "Selalu jawab dalam bahasa Indonesia, singkat, jelas, dan ramah.";

// ─────────────────────────────────────────────────────────────────────────────
//  LCD CONSTANTS
// ─────────────────────────────────────────────────────────────────────────────
#define LCD_W     240
#define LCD_H     240
#define SBAR_H     18    // tinggi status bar bawah

// Colors RGB565
#define C_BG       0x0841
#define C_BG_L     0x000C
#define C_BG_T     0x1001
#define C_BG_S     0x0401
#define C_BG_H     0x0200
#define C_BG_E     0x2000
#define C_WHITE    0xFFFF
#define C_BLACK    0x0000
#define C_CYAN     0x07FF
#define C_PURPLE   0x881F
#define C_YELLOW   0xFFE0
#define C_GREEN    0x07E0
#define C_RED      0xF800
#define C_LGRAY    0x9CF3
#define C_DGRAY    0x4208

// Face geometry
#define EYE_LX     (LCD_W/2 - 42)
#define EYE_RX     (LCD_W/2 + 42)
#define EYE_Y      (LCD_H/2 - 22)
#define EYE_R      22
#define MOUTH_CX   (LCD_W/2)
#define MOUTH_CY   (LCD_H/2 + 44)

// ─────────────────────────────────────────────────────────────────────────────
//  STATE MACHINE
// ─────────────────────────────────────────────────────────────────────────────
enum DevState : uint8_t {
  ST_PROV   = 0,
  ST_CONN   = 1,
  ST_IDLE   = 2,
  ST_LISTEN = 3,
  ST_THINK  = 4,
  ST_SPEAK  = 5,
  ST_HAPPY  = 6,
  ST_ERR    = 7
};

// ─────────────────────────────────────────────────────────────────────────────
//  CONVERSATION HISTORY
// ─────────────────────────────────────────────────────────────────────────────
#define MAX_HIST   6
struct HistEntry { String role; String text; };

// ─────────────────────────────────────────────────────────────────────────────
//  GLOBALS
// ─────────────────────────────────────────────────────────────────────────────
Adafruit_ST7789  tft(&SPI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
Preferences      nvs;
WebServer        webSrv(80);

volatile DevState  g_state     = ST_CONN;
volatile bool      g_btnFired  = false;
volatile bool      g_longPress = false;
volatile uint32_t  g_btnDown   = 0;

String  g_ssid, g_wpass, g_chatKey, g_ttsKey;
String  g_statusMsg = "Memulai...";

uint8_t* g_audioBuf   = nullptr;
int      g_histCount  = 0;
HistEntry g_hist[MAX_HIST * 2];

uint32_t g_animFrame    = 0;
uint32_t g_animTick     = 0;
uint32_t g_stateExpire  = 0;

SemaphoreHandle_t g_mtx = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
//  PSRAM-AWARE JSON ALLOCATOR  (ArduinoJson v6)
// ─────────────────────────────────────────────────────────────────────────────
struct PSRamAlloc {
  void* allocate(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
  }
  void deallocate(void* p)             { free(p); }
  void* reallocate(void* p, size_t n) {
    void* r = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return r ? r : realloc(p, n);
  }
};
using BigJson = BasicJsonDocument<PSRamAlloc>;

// ─────────────────────────────────────────────────────────────────────────────
//  UTILITY — safe alloc (PSRAM first, DRAM fallback)
// ─────────────────────────────────────────────────────────────────────────────
static void* safeMalloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}

// ─────────────────────────────────────────────────────────────────────────────
//  UTILITY — Base64 encode  (mbedtls, no dependency)
// ─────────────────────────────────────────────────────────────────────────────
static String b64enc(const uint8_t* data, size_t len) {
  size_t outLen = 0;
  mbedtls_base64_encode(nullptr, 0, &outLen, data, len);
  char* buf = (char*)safeMalloc(outLen + 1);
  if (!buf) return "";
  mbedtls_base64_encode((unsigned char*)buf, outLen + 1, &outLen, data, len);
  buf[outLen] = '\0';
  String s = String(buf);
  free(buf);
  return s;
}

// ─────────────────────────────────────────────────────────────────────────────
//  UTILITY — Build 44-byte WAV header
// ─────────────────────────────────────────────────────────────────────────────
static void wavHeader(uint8_t* h, uint32_t dataBytes, uint32_t sr,
                      uint16_t ch, uint16_t bps) {
  uint32_t brate  = sr * ch * (bps / 8);
  uint16_t balign = ch * (bps / 8);
  uint32_t riffSz = dataBytes + 36;
  uint16_t pcmFmt = 1;
  uint32_t fmtSz  = 16;
  auto w32 = [](uint8_t* p, uint32_t v){ memcpy(p, &v, 4); };
  auto w16 = [](uint8_t* p, uint16_t v){ memcpy(p, &v, 2); };
  memcpy(h,    "RIFF", 4); w32(h+4,  riffSz);
  memcpy(h+8,  "WAVE", 4); memcpy(h+12, "fmt ", 4);
  w32(h+16, fmtSz);  w16(h+20, pcmFmt); w16(h+22, ch);
  w32(h+24, sr);     w32(h+28, brate);  w16(h+32, balign);
  w16(h+34, bps);    memcpy(h+36, "data", 4); w32(h+40, dataBytes);
}

// ─────────────────────────────────────────────────────────────────────────────
//  NVS CONFIG
// ─────────────────────────────────────────────────────────────────────────────
static void loadConfig() {
  nvs.begin("dewzx", true);
  g_ssid    = nvs.getString("ssid",    "");
  g_wpass   = nvs.getString("wpass",   "");
  g_chatKey = nvs.getString("chatkey", "");
  g_ttsKey  = nvs.getString("ttskey",  "");
  nvs.end();
}

static void saveConfig(const String& s, const String& w,
                       const String& ck, const String& tk) {
  nvs.begin("dewzx", false);
  nvs.putString("ssid",    s);
  nvs.putString("wpass",   w);
  nvs.putString("chatkey", ck);
  nvs.putString("ttskey",  tk.isEmpty() ? ck : tk);
  nvs.end();
  g_ssid = s; g_wpass = w; g_chatKey = ck;
  g_ttsKey = tk.isEmpty() ? ck : tk;
}

// ─────────────────────────────────────────────────────────────────────────────
//  PROVISIONING WEB PAGE  (captive portal / AP mode)
// ─────────────────────────────────────────────────────────────────────────────
static const char PROV_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="id">
<head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DEWZX Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font:14px/1.6 'Segoe UI',sans-serif;background:#0d0d1a;color:#e0e0ff;
  min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}
.box{background:#16162e;border:1px solid #4a0080;border-radius:14px;
  padding:28px 22px;width:100%;max-width:400px;box-shadow:0 0 40px #5500aa33}
h1{text-align:center;font-size:1.7em;color:#bb6aff;margin-bottom:4px}
p.sub{text-align:center;color:#8080aa;font-size:.85em;margin-bottom:20px}
label{display:block;margin:12px 0 3px;font-size:.8em;color:#aa88ee}
input{width:100%;padding:10px 12px;background:#0d0d1a;border:1px solid #4a0080;
  border-radius:7px;color:#e0e0ff;font-size:.95em;outline:none;transition:border .15s}
input:focus{border-color:#bb6aff}
.hint{font-size:.75em;color:#666;margin-top:2px}
button{width:100%;margin-top:20px;padding:12px;
  background:linear-gradient(135deg,#6600cc,#bb6aff);
  border:none;border-radius:8px;color:#fff;font-size:1em;font-weight:600;cursor:pointer}
button:active{opacity:.8}
#msg{text-align:center;margin-top:12px;font-size:.85em;min-height:1.4em}
.ok{color:#66ffaa}.err{color:#ff6060}
</style></head>
<body><div class="box">
<h1>🤖 DEWZX</h1>
<p class="sub">Setup Perangkat AI Suara Dewangga</p>
<form id="f">
  <label>Nama WiFi (SSID)</label>
  <input id="ssid" placeholder="Nama jaringan WiFi" required>
  <label>Password WiFi</label>
  <input id="wpass" type="password" placeholder="Kosongkan jika open network">
  <label>API Key Chat — Qwen Omni</label>
  <input id="chatkey" placeholder="sk-xxxxxxxxxxxxxxxx" required>
  <label>API Key TTS — Cicada</label>
  <input id="ttskey" placeholder="Kosongkan = sama dengan Chat Key">
  <p class="hint">Base URL: https://ai.genzicorp.com/v1 (sudah tertanam)</p>
  <button type="submit">💾 Simpan &amp; Restart</button>
</form>
<div id="msg"></div>
</div>
<script>
document.getElementById('f').onsubmit = async e => {
  e.preventDefault();
  const m = document.getElementById('msg');
  m.textContent = 'Menyimpan...'; m.className = '';
  try {
    const r = await fetch('/save', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        ssid:    document.getElementById('ssid').value,
        wpass:   document.getElementById('wpass').value,
        chatkey: document.getElementById('chatkey').value,
        ttskey:  document.getElementById('ttskey').value
      })
    });
    const j = await r.json();
    if (j.ok) { m.className = 'ok'; m.textContent = '✅ Tersimpan! DEWZX sedang restart...'; }
    else       { m.className = 'err'; m.textContent = '❌ ' + j.msg; }
  } catch(e) { m.className = 'err'; m.textContent = '❌ ' + e.message; }
};
</script></body></html>
)HTML";

static void onRoot()     { webSrv.send_P(200, "text/html", PROV_HTML); }
static void onNotFound() { webSrv.sendHeader("Location", "/"); webSrv.send(302); }

static void onSave() {
  if (!webSrv.hasArg("plain")) { webSrv.send(400); return; }
  DynamicJsonDocument d(512);
  if (deserializeJson(d, webSrv.arg("plain"))) {
    webSrv.send(400, "application/json", "{\"ok\":false,\"msg\":\"JSON tidak valid\"}");
    return;
  }
  String ss = d["ssid"]    | "";
  String wp = d["wpass"]   | "";
  String ck = d["chatkey"] | "";
  String tk = d["ttskey"]  | "";
  if (ss.isEmpty() || ck.isEmpty()) {
    webSrv.send(400, "application/json",
      "{\"ok\":false,\"msg\":\"SSID dan Chat API Key wajib diisi\"}");
    return;
  }
  saveConfig(ss, wp, ck, tk);
  webSrv.send(200, "application/json", "{\"ok\":true}");
  delay(400);
  ESP.restart();
}

static void startProvAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("DEWZX_Setup", "dewzx1234");
  webSrv.on("/",     HTTP_GET,  onRoot);
  webSrv.on("/save", HTTP_POST, onSave);
  webSrv.onNotFound(onNotFound);
  webSrv.begin();
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_state     = ST_PROV;
  g_statusMsg = "AP: DEWZX_Setup | 192.168.4.1";
  xSemaphoreGive(g_mtx);
  Serial.println("[WiFi] AP mode: 192.168.4.1");
}

// ─────────────────────────────────────────────────────────────────────────────
//  CAMERA
// ─────────────────────────────────────────────────────────────────────────────
static bool camInit() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = CAM_D0; c.pin_d1 = CAM_D1; c.pin_d2 = CAM_D2; c.pin_d3 = CAM_D3;
  c.pin_d4 = CAM_D4; c.pin_d5 = CAM_D5; c.pin_d6 = CAM_D6; c.pin_d7 = CAM_D7;
  c.pin_xclk     = CAM_XCLK;
  c.pin_pclk     = CAM_PCLK;
  c.pin_vsync    = CAM_VSYNC;
  c.pin_href     = CAM_HREF;
  c.pin_sccb_sda = CAM_SIOD;
  c.pin_sccb_scl = CAM_SIOC;
  c.pin_pwdn     = CAM_PWDN;
  c.pin_reset    = CAM_RESET;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  if (psramFound()) {
    c.frame_size = FRAMESIZE_VGA; c.jpeg_quality = 15;
    c.fb_count = 2; c.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    c.frame_size = FRAMESIZE_QVGA; c.jpeg_quality = 20;
    c.fb_count = 1; c.fb_location = CAMERA_FB_IN_DRAM;
  }
  return esp_camera_init(&c) == ESP_OK;
}

static String camCapture() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) return "";
  String b64 = b64enc(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return b64;
}

// ─────────────────────────────────────────────────────────────────────────────
//  I2S MIC — INMP441  (I2S Port 0)
// ─────────────────────────────────────────────────────────────────────────────
static bool micInit() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate          = MIC_SR;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;  // INMP441 always 32-bit frame
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 4;
  cfg.dma_buf_len          = 512;
  cfg.use_apll             = false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = PIN_MIC_SCK;
  pins.ws_io_num    = PIN_MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = PIN_MIC_SD;

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_0); return false;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  return true;
}

// Record PCM into g_audioBuf, return bytes captured
static size_t micRecord() {
  if (!g_audioBuf) return 0;
  int32_t tmp[256];   // DRAM stack buf — DMA-safe
  int16_t* pcm = (int16_t*)g_audioBuf;
  size_t   cap = AUDIO_BYTES / sizeof(int16_t);
  size_t   idx = 0;
  uint32_t end = millis() + (REC_SECS * 1000UL);

  while (millis() < end && idx < cap) {
    size_t got = 0;
    if (i2s_read(I2S_NUM_0, tmp, sizeof(tmp), &got, pdMS_TO_TICKS(100)) == ESP_OK) {
      int n = (int)(got / sizeof(int32_t));
      for (int i = 0; i < n && idx < cap; i++) {
        // INMP441: audio in upper 24-bit of 32-bit word
        pcm[idx++] = (int16_t)(tmp[i] >> 11);
      }
    }
  }
  return idx * sizeof(int16_t);
}

// ─────────────────────────────────────────────────────────────────────────────
//  I2S SPEAKER — MAX98357A  (I2S Port 1)
// ─────────────────────────────────────────────────────────────────────────────
static bool spkInit() {
  i2s_config_t cfg = {};
  cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate          = 22050;
  cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count        = 8;
  cfg.dma_buf_len          = 512;
  cfg.use_apll             = false;
  cfg.tx_desc_auto_clear   = true;

  i2s_pin_config_t pins = {};
  pins.mck_io_num   = I2S_PIN_NO_CHANGE;
  pins.bck_io_num   = PIN_SPK_BCK;
  pins.ws_io_num    = PIN_SPK_LRC;
  pins.data_out_num = PIN_SPK_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;

  if (i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr) != ESP_OK) return false;
  if (i2s_set_pin(I2S_NUM_1, &pins) != ESP_OK) {
    i2s_driver_uninstall(I2S_NUM_1); return false;
  }
  return true;
}

static bool spkPlayWav(const uint8_t* buf, size_t len) {
  if (!buf || len < 44) {
    Serial.printf("[SPK] Buffer terlalu kecil: %u bytes\n", (unsigned)len);
    return false;
  }
  if (memcmp(buf, "RIFF", 4) != 0) {
    Serial.printf("[SPK] Bukan WAV! Byte pertama: %02X %02X %02X %02X\n",
                  buf[0], buf[1], buf[2], buf[3]);
    return false;
  }

  uint32_t sr; uint16_t ch, bps;
  memcpy(&sr,  buf + 24, 4);
  memcpy(&ch,  buf + 22, 2);
  memcpy(&bps, buf + 34, 2);

  // Cari chunk "data" (toleran terhadap WAV non-standar)
  uint32_t off = 12;
  while (off + 8 <= len) {
    if (memcmp(buf + off, "data", 4) == 0) { off += 8; break; }
    uint32_t cs = 0; memcpy(&cs, buf + off + 4, 4);
    off += 8 + cs;
  }
  if (off >= len) return false;

  i2s_set_clk(I2S_NUM_1, sr,
              (i2s_bits_per_sample_t)bps,
              ch == 2 ? I2S_CHANNEL_STEREO : I2S_CHANNEL_MONO);

  size_t written = 0, pcmLen = len - off;
  while (written < pcmLen) {
    size_t chunk = min((size_t)4096, pcmLen - written);
    size_t w = 0;
    i2s_write(I2S_NUM_1, buf + off + written, chunk, &w, pdMS_TO_TICKS(500));
    if (w == 0) break;
    written += w;
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  CONVERSATION HISTORY
// ─────────────────────────────────────────────────────────────────────────────
static void histAdd(const String& role, const String& text) {
  if (g_histCount >= MAX_HIST * 2) {
    for (int i = 0; i < MAX_HIST * 2 - 2; i++) g_hist[i] = g_hist[i + 2];
    g_histCount -= 2;
  }
  g_hist[g_histCount++] = {role, text};
}

// ─────────────────────────────────────────────────────────────────────────────
//  API — CHAT COMPLETIONS (GenZi Qwen Omni)
// ─────────────────────────────────────────────────────────────────────────────
static bool apiChat(const String& audioB64,
                    const String& imageB64,
                    const String& textHint,
                    String&       response) {
  WiFiClientSecure wcs;
  wcs.setInsecure();
  HTTPClient http;
  http.begin(wcs, String(GENZAI_BASE) + "/chat/completions");
  http.addHeader("Authorization", "Bearer " + g_chatKey);
  http.addHeader("Content-Type",  "application/json");
  http.setTimeout(45000);

  BigJson doc(400 * 1024);
  doc["model"]      = CHAT_MODEL;
  doc["max_tokens"] = 512;
  doc["stream"]     = false;

  JsonArray msgs = doc.createNestedArray("messages");

  // System message
  JsonObject sys  = msgs.createNestedObject();
  sys["role"]     = "system";
  sys["content"]  = SYS_PROMPT;

  // History
  for (int i = 0; i < g_histCount; i++) {
    JsonObject m  = msgs.createNestedObject();
    m["role"]     = g_hist[i].role;
    m["content"]  = g_hist[i].text;
  }

  // Current user message
  JsonObject usr = msgs.createNestedObject();
  usr["role"]    = "user";

  bool multimodal = (!audioB64.isEmpty() || !imageB64.isEmpty());
  if (multimodal) {
    JsonArray parts = usr.createNestedArray("content");
    if (!imageB64.isEmpty()) {
      JsonObject ip  = parts.createNestedObject();
      ip["type"]     = "image_url";
      JsonObject iu  = ip.createNestedObject("image_url");
      iu["url"]      = "data:image/jpeg;base64," + imageB64;
    }
    if (!audioB64.isEmpty()) {
      JsonObject ap  = parts.createNestedObject();
      ap["type"]     = "audio_url";
      JsonObject au  = ap.createNestedObject("audio_url");
      au["url"]      = "data:audio/wav;base64," + audioB64;
    }
    JsonObject tp  = parts.createNestedObject();
    tp["type"]     = "text";
    tp["text"]     = textHint.isEmpty() ? "Jawab dalam bahasa Indonesia." : textHint;
  } else {
    usr["content"] = textHint.isEmpty() ? "Halo DEWZX!" : textHint;
  }

  // Serialize ke PSRAM String
  String body;
  body.reserve(doc.memoryUsage() + 256);
  serializeJson(doc, body);
  Serial.printf("[API] Chat POST: %u bytes\n", (unsigned)body.length());

  int code = http.POST(body);
  body = "";  // bebaskan memori segera

  if (code != 200) {
    Serial.printf("[API] Chat error %d: %s\n", code, http.getString().substring(0,200).c_str());
    http.end(); return false;
  }

  String raw = http.getString();
  http.end();

  DynamicJsonDocument res(4096);
  if (deserializeJson(res, raw)) return false;

  response = res["choices"][0]["message"]["content"] | "";
  return !response.isEmpty();
}

// ─────────────────────────────────────────────────────────────────────────────
//  API — TTS (GenZi Cicada)
// ─────────────────────────────────────────────────────────────────────────────
static bool apiTTS(const String& text, uint8_t** outBuf, size_t* outLen) {
  WiFiClientSecure wcs;
  wcs.setInsecure();
  HTTPClient http;
  http.begin(wcs, String(GENZAI_BASE) + "/audio/speech");
  http.addHeader("Authorization", "Bearer " + g_ttsKey);
  http.addHeader("Content-Type",  "application/json");
  http.setTimeout(30000);

  DynamicJsonDocument doc(512);
  doc["model"]           = TTS_MODEL;
  doc["input"]           = text;
  doc["voice"]           = TTS_VOICE;
  doc["response_format"] = "wav";
  doc["speed"]           = 1.0;

  String body;
  serializeJson(doc, body);

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("[TTS] Error %d\n", code);
    http.end(); return false;
  }

  int expLen = http.getSize();
  if (expLen <= 0) expLen = 300 * 1024;

  *outBuf = (uint8_t*)safeMalloc(expLen + 4096);
  if (!*outBuf) { http.end(); return false; }

  WiFiClient* stream   = http.getStreamPtr();
  size_t      got      = 0;
  uint32_t    deadline = millis() + 20000;

  while (http.connected() && millis() < deadline) {
    int avail = stream->available();
    if (avail > 0) {
      size_t canRead = min((size_t)avail, (size_t)(expLen + 4096 - (int)got));
      got += stream->readBytes(*outBuf + got, canRead);
      deadline = millis() + 8000;
    } else { delay(5); }
    if (expLen > 0 && (int)got >= expLen) break;
  }
  http.end();

  *outLen = got;
  if (got < 44) { free(*outBuf); *outBuf = nullptr; return false; }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  LCD HELPERS
// ─────────────────────────────────────────────────────────────────────────────
static void lcdBL(bool on) { digitalWrite(PIN_LCD_BLK, on ? HIGH : LOW); }

// Arc approximation dengan garis
static void drawArc(int cx, int cy, int rx, int ry,
                    float a0, float a1, uint16_t col, int steps = 24) {
  float da = (a1 - a0) / steps;
  int   px = cx + (int)(rx * cosf(a0));
  int   py = cy + (int)(ry * sinf(a0));
  for (int i = 1; i <= steps; i++) {
    float a  = a0 + da * i;
    int   nx = cx + (int)(rx * cosf(a));
    int   ny = cy + (int)(ry * sinf(a));
    tft.drawLine(px, py, nx, ny, col);
    px = nx; py = ny;
  }
}

// ─── Mata ────────────────────────────────────────────────────────────────────
static void drawEye(int cx, int cy, int r, int px, int py,
                    uint16_t iris, bool wink, bool squint) {
  if (wink) {
    tft.fillRoundRect(cx - r, cy - 3, r * 2, 6, 3, iris);
    return;
  }
  if (squint) {
    tft.fillRoundRect(cx - r, cy - r / 4, r * 2, r / 2, r / 4, iris);
    return;
  }
  tft.fillCircle(cx, cy, r, iris);
  tft.fillCircle(cx + px, cy + py, r / 3, C_BLACK);
  tft.drawCircle(cx, cy, r, C_WHITE);
}

// ─── Waveform bar (animasi listening) ────────────────────────────────────────
static void drawBars(int x, int y, int w, int h, uint32_t fr, uint16_t col) {
  const int N = 7, BW = 3;
  int sp = w / N;
  for (int i = 0; i < N; i++) {
    int bh = (int)(h * 0.25f + h * 0.6f *
              (0.5f + 0.5f * sinf(fr * 0.18f + i * 0.9f)));
    int bx = x + i * sp;
    tft.fillRect(bx, y,          BW, h - bh, C_BLACK);
    tft.fillRect(bx, y + h - bh, BW, bh,     col);
  }
}

// ─── Titik animasi (thinking) ─────────────────────────────────────────────────
static void drawDots(int cx, int cy, uint32_t fr, uint16_t col) {
  for (int i = 0; i < 3; i++) {
    int dx = cx - 20 + i * 20;
    int dy = cy + (int)(5.0f * sinf(fr * 0.22f + i * 1.1f));
    tft.fillCircle(dx, dy, (i == (int)(fr / 7) % 3) ? 7 : 5, col);
  }
}

// ─── Status bar ───────────────────────────────────────────────────────────────
static void drawStatusBar(const String& msg, bool wifi) {
  tft.fillRect(0, LCD_H - SBAR_H, LCD_W, SBAR_H, C_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(wifi ? C_GREEN : C_RED);
  tft.setCursor(2, LCD_H - SBAR_H + 5);
  tft.print(wifi ? "W+" : "W-");
  tft.setTextColor(C_LGRAY);
  tft.setCursor(18, LCD_H - SBAR_H + 5);
  String s = msg.substring(0, 29);
  tft.print(s);
}

// ─────────────────────────────────────────────────────────────────────────────
//  FACE ANIMATION
// ─────────────────────────────────────────────────────────────────────────────
static DevState g_lastFaceState = (DevState)255;

static void faceRedrawBg(uint16_t bg) {
  tft.fillRect(0, 0, LCD_W, LCD_H - SBAR_H, bg);
  g_lastFaceState = (DevState)255;
}

static void drawFace(DevState st, uint32_t fr) {
  bool fresh = (st != g_lastFaceState);
  bool wifi  = (WiFi.status() == WL_CONNECTED);

  uint16_t bg = C_BG;
  if      (st == ST_LISTEN) bg = C_BG_L;
  else if (st == ST_THINK)  bg = C_BG_T;
  else if (st == ST_SPEAK)  bg = C_BG_S;
  else if (st == ST_HAPPY)  bg = C_BG_H;
  else if (st == ST_ERR)    bg = C_BG_E;

  if (fresh) { faceRedrawBg(bg); g_lastFaceState = st; }

  int er = EYE_R;
  bool blink = (st == ST_IDLE && (fr % 80) > 76);

  // zona mata (kotak bersih sebelum gambar ulang)
  auto clearEyes = [&](uint16_t c) {
    tft.fillRect(EYE_LX - er - 4, EYE_Y - er - 4, (er + 4) * 2, (er + 4) * 2, c);
    tft.fillRect(EYE_RX - er - 4, EYE_Y - er - 4, (er + 4) * 2, (er + 4) * 2, c);
  };
  auto clearMouth = [&](uint16_t c) {
    tft.fillRect(MOUTH_CX - 36, MOUTH_CY - 20, 72, 40, c);
  };

  switch (st) {
    // ─ IDLE ──────────────────────────────────────────────────────────────────
    case ST_IDLE: {
      if (!fresh && (fr % 4 != 0)) break;
      clearEyes(C_BG);
      drawEye(EYE_LX, EYE_Y, er,  3, 0, C_CYAN, blink, false);
      drawEye(EYE_RX, EYE_Y, er, -3, 0, C_CYAN, blink, false);
      clearMouth(C_BG);
      drawArc(MOUTH_CX, MOUTH_CY - 5, 24, 10, 0.0f, (float)M_PI, C_CYAN);
      if (fresh) {
        tft.setTextColor(C_PURPLE); tft.setTextSize(2);
        tft.setCursor(LCD_W / 2 - 36, 10);
        tft.print("DEWZX");
      }
      break;
    }

    // ─ LISTENING ─────────────────────────────────────────────────────────────
    case ST_LISTEN: {
      clearEyes(C_BG_L);
      drawEye(EYE_LX, EYE_Y, er + 3, 0, 0, C_CYAN, false, false);
      drawEye(EYE_RX, EYE_Y, er + 3, 0, 0, C_CYAN, false, false);
      clearMouth(C_BG_L);
      tft.fillCircle(MOUTH_CX, MOUTH_CY, 11, C_CYAN);
      tft.fillCircle(MOUTH_CX, MOUTH_CY,  5, C_BLACK);
      drawBars( 2,          95, 20, 60, fr,     C_CYAN);
      drawBars(LCD_W - 22,  95, 20, 60, fr + 4, C_CYAN);
      tft.fillRect(0, LCD_H - SBAR_H - 18, LCD_W, 16, C_BG_L);
      tft.setTextColor(C_CYAN); tft.setTextSize(1);
      tft.setCursor(LCD_W / 2 - 32, LCD_H - SBAR_H - 14);
      tft.print("Mendengarkan...");
      break;
    }

    // ─ THINKING ──────────────────────────────────────────────────────────────
    case ST_THINK: {
      if (!fresh && fr % 3 != 0) break;
      clearEyes(C_BG_T);
      drawEye(EYE_LX, EYE_Y, er, 0, 0, C_PURPLE, false, true);
      drawEye(EYE_RX, EYE_Y, er, 0, 0, C_PURPLE, false, true);
      clearMouth(C_BG_T);
      drawDots(MOUTH_CX, MOUTH_CY, fr, C_PURPLE);
      tft.fillRect(0, LCD_H - SBAR_H - 18, LCD_W, 16, C_BG_T);
      tft.setTextColor(C_PURPLE); tft.setTextSize(1);
      tft.setCursor(LCD_W / 2 - 28, LCD_H - SBAR_H - 14);
      tft.print("Memproses...");
      break;
    }

    // ─ SPEAKING ──────────────────────────────────────────────────────────────
    case ST_SPEAK: {
      if (!fresh && fr % 2 != 0) break;
      clearEyes(C_BG_S);
      drawEye(EYE_LX, EYE_Y, er,  3, 0, C_CYAN, false, false);
      drawEye(EYE_RX, EYE_Y, er, -3, 0, C_CYAN, false, false);
      clearMouth(C_BG_S);
      float op = 0.4f + 0.5f * fabsf(sinf(fr * 0.4f));
      int   mh = (int)(8 + 20 * op);
      tft.fillRoundRect(MOUTH_CX - 24, MOUTH_CY - mh / 2, 48, mh, mh / 2, C_CYAN);
      tft.fillRect(0, LCD_H - SBAR_H - 18, LCD_W, 16, C_BG_S);
      tft.setTextColor(C_CYAN); tft.setTextSize(1);
      tft.setCursor(LCD_W / 2 - 22, LCD_H - SBAR_H - 14);
      tft.print("Berbicara...");
      break;
    }

    // ─ HAPPY ─────────────────────────────────────────────────────────────────
    case ST_HAPPY: {
      if (!fresh) break;
      clearEyes(C_BG_H);
      drawEye(EYE_LX, EYE_Y - 4, er, 0, -5, C_YELLOW, false, true);
      drawEye(EYE_RX, EYE_Y - 4, er, 0, -5, C_YELLOW, false, true);
      clearMouth(C_BG_H);
      drawArc(MOUTH_CX, MOUTH_CY - 5, 28, 12, 0.0f, (float)M_PI, C_YELLOW);
      tft.fillRect(0, LCD_H - SBAR_H - 18, LCD_W, 16, C_BG_H);
      tft.setTextColor(C_YELLOW); tft.setTextSize(1);
      tft.setCursor(LCD_W / 2 - 22, LCD_H - SBAR_H - 14);
      tft.print("DEWZX ^_^");
      break;
    }

    // ─ ERROR ─────────────────────────────────────────────────────────────────
    case ST_ERR: {
      if (!fresh) break;
      clearEyes(C_BG_E);
      for (int i = -er; i <= er; i++) {
        tft.drawPixel(EYE_LX + i, EYE_Y + i, C_RED);
        tft.drawPixel(EYE_LX + i, EYE_Y - i, C_RED);
        tft.drawPixel(EYE_RX + i, EYE_Y + i, C_RED);
        tft.drawPixel(EYE_RX + i, EYE_Y - i, C_RED);
      }
      clearMouth(C_BG_E);
      drawArc(MOUTH_CX, MOUTH_CY + 8, 24, 10,
              (float)M_PI, (float)(2.0 * M_PI), C_RED);
      tft.fillRect(0, LCD_H - SBAR_H - 18, LCD_W, 16, C_BG_E);
      tft.setTextColor(C_RED); tft.setTextSize(1);
      tft.setCursor(LCD_W / 2 - 15, LCD_H - SBAR_H - 14);
      tft.print("Error!");
      break;
    }

    // ─ PROVISIONING & CONNECTING ─────────────────────────────────────────────
    case ST_PROV:
    case ST_CONN: {
      if (fresh) {
        tft.setTextColor(C_PURPLE); tft.setTextSize(2);
        tft.setCursor(LCD_W / 2 - 36, 46); tft.print("DEWZX");
        tft.setTextSize(1); tft.setTextColor(C_WHITE);
        if (st == ST_PROV) {
          tft.setCursor(8, 90);  tft.print("Mode Setup WiFi");
          tft.setCursor(8, 106); tft.print("Koneksi ke: DEWZX_Setup");
          tft.setCursor(8, 122); tft.print("Password:   dewzx1234");
          tft.setCursor(8, 138); tft.print("Browser:    192.168.4.1");
        } else {
          tft.setCursor(8, 98);  tft.print("Menghubungkan ke WiFi:");
          tft.setCursor(8, 114);
          String sn = g_ssid.length() > 20 ? g_ssid.substring(0,20) : g_ssid;
          tft.print("\"" + sn + "\"");
        }
      }
      // Spinner
      float ang = (fr % 36) * 10.0f * DEG_TO_RAD;
      tft.fillRect(LCD_W / 2 - 18, 158, 36, 36, bg);
      tft.fillCircle(LCD_W / 2 + (int)(14 * cosf(ang)),
                     176   + (int)(14 * sinf(ang)), 5, C_CYAN);
      break;
    }
    default: break;
  }

  drawStatusBar(g_statusMsg, wifi);
}

// ─────────────────────────────────────────────────────────────────────────────
//  DISPLAY TASK  (Core 1, ~30 fps)
// ─────────────────────────────────────────────────────────────────────────────
static void taskDisplay(void*) {
  for (;;) {
    if (millis() - g_animTick >= 33) {
      g_animFrame++;
      g_animTick = millis();

      DevState s;
      xSemaphoreTake(g_mtx, portMAX_DELAY);
      s = g_state;
      xSemaphoreGive(g_mtx);

      drawFace(s, g_animFrame);

      // Auto-revert dari state timed
      if ((s == ST_HAPPY || s == ST_ERR) &&
           g_stateExpire > 0 && millis() > g_stateExpire) {
        xSemaphoreTake(g_mtx, portMAX_DELAY);
        if (g_state == s) { g_state = ST_IDLE; g_statusMsg = "Siap"; }
        xSemaphoreGive(g_mtx);
        g_stateExpire = 0;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  STATE SETTER (thread-safe)
// ─────────────────────────────────────────────────────────────────────────────
static void setState(DevState s, const char* msg = nullptr, uint32_t expMs = 0) {
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  g_state = s;
  if (msg) g_statusMsg = msg;
  g_stateExpire = expMs > 0 ? millis() + expMs : 0;
  xSemaphoreGive(g_mtx);
}

// ─────────────────────────────────────────────────────────────────────────────
//  AI TASK  (Core 0, spawned per permintaan)
// ─────────────────────────────────────────────────────────────────────────────
struct AIParam { bool withPhoto; };

static void taskAI(void* pv) {
  bool withPhoto = ((AIParam*)pv)->withPhoto;
  delete (AIParam*)pv;

  // ── 1. Rekam suara ─────────────────────────────────────────────────────────
  setState(ST_LISTEN, "Merekam suara...");
  size_t pcmLen = micRecord();
  if (pcmLen == 0) {
    setState(ST_ERR, "Mic gagal!", 3500);
    vTaskDelete(nullptr); return;
  }

  // ── 2. Buat WAV dan encode Base64 ─────────────────────────────────────────
  setState(ST_THINK, "Menyiapkan audio...");
  size_t   wavLen = 44 + pcmLen;
  uint8_t* wav    = (uint8_t*)safeMalloc(wavLen);
  if (!wav) { setState(ST_ERR, "RAM habis!", 3500); vTaskDelete(nullptr); return; }
  wavHeader(wav, (uint32_t)pcmLen, MIC_SR, MIC_CH, MIC_BITS);
  memcpy(wav + 44, g_audioBuf, pcmLen);
  String audioB64 = b64enc(wav, wavLen);
  free(wav);

  // ── 3. Ambil foto (opsional) ───────────────────────────────────────────────
  String imageB64 = "";
  if (withPhoto) {
    setState(ST_THINK, "Mengambil foto...");
    imageB64 = camCapture();
    if (imageB64.isEmpty()) Serial.println("[CAM] Capture gagal, lanjut tanpa foto");
  }

  // ── 4. Panggil AI ──────────────────────────────────────────────────────────
  setState(ST_THINK, "Memanggil AI...");
  String textHint = withPhoto
    ? "Lihat gambar ini dengan teliti. Jika ada soal atau teks, jelaskan dan jawab lengkap."
    : "";

  String aiReply;
  if (!apiChat(audioB64, imageB64, textHint, aiReply)) {
    setState(ST_ERR, "AI gagal!", 3500);
    vTaskDelete(nullptr); return;
  }
  Serial.println("[DEWZX] " + aiReply);
  histAdd("user",      withPhoto ? "[audio+foto]" : "[audio]");
  histAdd("assistant", aiReply);

  // ── 5. TTS ─────────────────────────────────────────────────────────────────
  setState(ST_SPEAK, "Berbicara...");
  String ttsText = aiReply;
  if (ttsText.length() > 500) ttsText = ttsText.substring(0, 500);

  uint8_t* ttsBuf = nullptr;
  size_t   ttsLen = 0;
  if (apiTTS(ttsText, &ttsBuf, &ttsLen)) {
    spkPlayWav(ttsBuf, ttsLen);
    free(ttsBuf);
  } else {
    Serial.println("[TTS] Gagal — speaker tetap diam");
  }

  // ── 6. Selesai ─────────────────────────────────────────────────────────────
  setState(ST_HAPPY, "Siap", 2500);
  vTaskDelete(nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
//  BUTTON ISR
// ─────────────────────────────────────────────────────────────────────────────
void IRAM_ATTR onBtn() {
  if (digitalRead(PIN_BTN) == LOW) {
    g_btnDown = millis();
  } else {
    g_longPress = (millis() - g_btnDown > 1000);
    g_btnFired  = true;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n╔══════════════════════╗");
  Serial.println("║   DEWZX VOICE AI     ║");
  Serial.println("╚══════════════════════╝");

  // GPIO awal
  pinMode(PIN_BTN,     INPUT_PULLUP);
  pinMode(PIN_LED,     OUTPUT);
  pinMode(PIN_LCD_BLK, OUTPUT);
  digitalWrite(PIN_LED,     LOW);
  digitalWrite(PIN_LCD_BLK, LOW);

  // ── LCD ────────────────────────────────────────────────────────────────────
  // Manual RST pulse sebelum SPI aktif
  pinMode(PIN_LCD_RST, OUTPUT);
  digitalWrite(PIN_LCD_RST, HIGH); delay(10);
  digitalWrite(PIN_LCD_RST, LOW);  delay(100);
  digitalWrite(PIN_LCD_RST, HIGH); delay(200);

  // CS LOW dulu agar modul siap terima data
  pinMode(PIN_LCD_CS, OUTPUT);
  digitalWrite(PIN_LCD_CS, LOW);

  // SPI tanpa SS pin — Adafruit manage CS sendiri
  SPI.begin(PIN_LCD_CLK, -1, PIN_LCD_MOSI, -1);
  tft.setSPISpeed(10000000);  // 10MHz — aman untuk semua modul ST7789
  tft.init(LCD_W, LCD_H);
  tft.setRotation(0);
  tft.fillScreen(C_BG);
  lcdBL(true);

  // Splash
  tft.setTextColor(C_PURPLE); tft.setTextSize(3);
  tft.setCursor(LCD_W / 2 - 44, LCD_H / 2 - 32); tft.print("DEWZX");
  tft.setTextColor(C_CYAN);    tft.setTextSize(1);
  tft.setCursor(LCD_W / 2 - 44, LCD_H / 2 + 8);  tft.print("Voice AI v1.0");
  tft.setCursor(LCD_W / 2 - 36, LCD_H / 2 + 22); tft.print("by Dewangga");
  delay(1200);

  // ── PSRAM ─────────────────────────────────────────────────────────────────
  if (psramFound()) {
    Serial.printf("[PSRAM] %u KB tersedia\n", (unsigned)(ESP.getFreePsram() / 1024));
    g_audioBuf = (uint8_t*)heap_caps_malloc(AUDIO_BYTES, MALLOC_CAP_SPIRAM);
  }
  if (!g_audioBuf) {
    Serial.println("[PSRAM] Fallback ke DRAM");
    g_audioBuf = (uint8_t*)malloc(AUDIO_BYTES);
  }
  if (!g_audioBuf) Serial.println("[ERROR] Audio buffer GAGAL dialokasi!");

  // ── Mutex ─────────────────────────────────────────────────────────────────
  g_mtx = xSemaphoreCreateMutex();

  // ── Camera ────────────────────────────────────────────────────────────────
  if (!camInit())  Serial.println("[WARN] Camera init gagal");

  // ── I2S ───────────────────────────────────────────────────────────────────
  if (!micInit())  Serial.println("[WARN] Mic I2S gagal");
  if (!spkInit())  Serial.println("[WARN] Speaker I2S gagal");

  // ── Config NVS ────────────────────────────────────────────────────────────
  loadConfig();

  // ── WiFi ──────────────────────────────────────────────────────────────────
  if (g_ssid.isEmpty() || g_chatKey.isEmpty()) {
    Serial.println("[WiFi] Tidak ada config → mode provisioning");
    startProvAP();
  } else {
    setState(ST_CONN, "Menghubungkan WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(g_ssid.c_str(), g_wpass.c_str());
    Serial.print("[WiFi] Connecting: "); Serial.println(g_ssid);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(300); Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WiFi] OK: " + WiFi.localIP().toString());
      setState(ST_IDLE, ("WiFi: " + WiFi.localIP().toString()).c_str());
    } else {
      Serial.println("[WiFi] Gagal → mode provisioning");
      startProvAP();
    }
  }

  // ── Button ISR ────────────────────────────────────────────────────────────
  attachInterrupt(digitalPinToInterrupt(PIN_BTN), onBtn, CHANGE);

  // ── Display Task (Core 1) ─────────────────────────────────────────────────
  xTaskCreatePinnedToCore(taskDisplay, "disp", 4096, nullptr, 2, nullptr, 1);

  Serial.println("[DEWZX] Siap!");
}

// ─────────────────────────────────────────────────────────────────────────────
//  LOOP  (Core 0)
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  DevState cur;
  xSemaphoreTake(g_mtx, portMAX_DELAY);
  cur = g_state;
  xSemaphoreGive(g_mtx);

  // ── Provisioning: jalankan web server ─────────────────────────────────────
  if (cur == ST_PROV) {
    webSrv.handleClient();
    delay(2);
    return;
  }

  // ── WiFi watchdog ─────────────────────────────────────────────────────────
  static uint32_t lastWdg = 0;
  static bool     prevWifi = false;
  bool nowWifi = (WiFi.status() == WL_CONNECTED);

  if (millis() - lastWdg > 12000) {
    lastWdg = millis();
    if (!nowWifi && !g_ssid.isEmpty()) {
      Serial.println("[WiFi] Reconnecting...");
      xSemaphoreTake(g_mtx, portMAX_DELAY);
      g_statusMsg = "Reconnecting...";
      xSemaphoreGive(g_mtx);
      WiFi.reconnect();
    }
  }
  // Update status saat WiFi kembali terhubung
  if (nowWifi && !prevWifi) {
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    if (g_state == ST_CONN || g_state == ST_IDLE) {
      g_state     = ST_IDLE;
      g_statusMsg = "WiFi OK: " + WiFi.localIP().toString();
    }
    xSemaphoreGive(g_mtx);
  }
  prevWifi = nowWifi;

  // ── Tombol ────────────────────────────────────────────────────────────────
  if (g_btnFired) {
    g_btnFired = false;
    bool lp = g_longPress;

    xSemaphoreTake(g_mtx, portMAX_DELAY);
    cur = g_state;
    xSemaphoreGive(g_mtx);

    if (cur == ST_IDLE) {
      if (!nowWifi) {
        setState(ST_ERR, "Tidak ada WiFi!", 3000);
      } else {
        AIParam* p  = new AIParam();
        p->withPhoto = lp;
        // Stack 28KB cukup untuk TLS + JSON parsing
        xTaskCreatePinnedToCore(taskAI, "ai", 28672, p, 1, nullptr, 0);
      }
    }
  }

  delay(10);
}
