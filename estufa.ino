/*
 * ============================================================
 *  ESTUFA DE FILAMENTOS — Firmware v1.1
 *  Board  : ESP32 CYD 2432S028 | Driver: ST7789
 *  Sensor : DHT22        — GPIO 4
 *  Relé 1 : Aquecedor    — GPIO 26  (ativo LOW)
 *  Relé 2 : Cooler/Fan   — GPIO 27  (ativo LOW)
 * ============================================================
 *  MODO ESTUFA  — controle por umidade:
 *    >= 40 % → liga AQUECEDOR + COOLER
 *    <= 32 % → desliga AQUECEDOR, COOLER por 5 min
 *    Temperatura: nunca excede 50 °C (histerese 47/50)
 *
 *  MODO SECAGEM — aquecimento contínuo até temperatura alvo:
 *    Alvo ajustável 45–55 °C (default 45 °C, botões +/− no display)
 *    Histerese de 3 °C: OFF em >= alvo, ON em <= alvo-3
 *    COOLER acompanha o AQUECEDOR
 * ============================================================
 */

#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_FT6206.h>
#include <DHT.h>
#include <Preferences.h>
#include <math.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// ─── Display SPI ─────────────────────────────────────────────
#define TFT_SCK   12
#define TFT_MOSI  11
#define TFT_MISO  13
#define TFT_CS    10
#define TFT_DC    46
#define TFT_RST   -1
#define TFT_BL    45

// ─── Touch I2C ───────────────────────────────────────────────
#define TOUCH_SDA  16
#define TOUCH_SCL  15
#define TOUCH_INT  17
#define TOUCH_RST  18

// ─── Periféricos ─────────────────────────────────────────────
#define DHT22_PIN   2
#define HEATER_PIN  3
#define COOLER_PIN 14
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

// ─── Tela ─────────────────────────────────────────────────────
#define SCREEN_W 320
#define SCREEN_H 240

// ─── Paleta RGB565 ───────────────────────────────────────────
#define CLR_BG        0x0000
#define CLR_WHITE     0xFFFF
#define CLR_GRAY      0x7BEF
#define CLR_DARKGRAY  0x2104
#define CLR_RED       0xF800
#define CLR_GREEN     0x07E0
#define CLR_BLUE      0x001F
#define CLR_YELLOW    0xFFE0
#define CLR_CYAN      0x07FF
#define CLR_ORANGE    0xFB60
#define CLR_GOLD      0xFEA0
#define CLR_PANEL     0x1082

// ─── Limites ESTUFA ──────────────────────────────────────────
#define HUM_TRIGGER       40.0f
#define HUM_RESET         32.0f
#define ESTUFA_TEMP_MAX   50.0f
#define ESTUFA_TEMP_RES   47.0f
#define COOLER_TAIL_MS    (5UL * 60UL * 1000UL)

// ─── Limites SECAGEM ─────────────────────────────────────────
#define SECAR_TEMP_MIN    45.0f
#define SECAR_TEMP_MAX    55.0f
#define SECAR_TEMP_DEF    45.0f
#define SECAR_HISTERESE    3.0f   // OFF em >= alvo, ON em <= alvo-3

// ─── Timing ──────────────────────────────────────────────────
#define SENSOR_INTERVAL        3000UL
#define SENSOR_FAIL_MAX        5
#define SENSOR_RECOVERY_DELAY  10000UL  // espera 10s em emergency antes de tentar
#define SENSOR_RECOVERY_MAX    3        // tentativas soft antes do ESP.restart()
#define NVS_SAVE_INTERVAL      30000UL
#define TEMP_WARN         45.0f   // aviso visual de temperatura

// ─── WiFi ────────────────────────────────────────────────────
#define WIFI_SSID         "Ahri"
#define WIFI_PASS         "Fer988165786*"
#define WIFI_TIMEOUT_MS   12000UL   // tempo máximo aguardando conexão no setup
#define WIFI_RETRY_MS     60000UL   // intervalo de retentativa no loop

// ─── NTP ─────────────────────────────────────────────────────
#define NTP_SERVER        "pool.ntp.org"
#define NTP_GMT_OFFSET    -10800    // UTC-3 (Brasília / Curitiba)
#define NTP_DST_OFFSET    0

// ─── Clima Open-Meteo (sem API key) ──────────────────────────
#define WEATHER_URL       "https://api.open-meteo.com/v1/forecast" \
                          "?latitude=-25.53&longitude=-49.20"       \
                          "&current_weather=true"
#define WEATHER_INTERVAL  (10UL * 60UL * 1000UL)   // a cada 10 min
#define LOCATION_NAME     "Curitiba/PR"

// ─── Layout ──────────────────────────────────────────────────
#define HDR_H      22
#define BTN_Y      24
#define BTN_H     104
#define BTN_W     148
#define BTN_L       6
#define BTN_R     166
// Dentro do botão SECAGEM selecionado:
#define SC_DIV_Y  (BTN_Y + 61)   // y do divisor interno
#define SC_CTR_Y  (BTN_Y + 79)   // y dos botões +/−
#define SC_BTN_H   22
#define SC_BTN_W   38
#define SC_MINUS_X (BTN_R +  5)  // x do botão −
#define SC_PLUS_X  (BTN_R + 105) // x do botão +
// Zonas de toque dos botões +/−
#define MINUS_TX1  (BTN_R +  3)
#define MINUS_TX2  (BTN_R + 45)
#define MINUS_TY1  (BTN_Y + 62)
#define MINUS_TY2  (BTN_Y + BTN_H - 3)
#define PLUS_TX1   (BTN_R + 103)
#define PLUS_TX2   (BTN_R + BTN_W - 3)
#define PLUS_TY1   (BTN_Y + 62)
#define PLUS_TY2   (BTN_Y + BTN_H - 3)

#define SENS_Y    130
#define SENS_H     58
#define STAT_Y    188
#define STAT_H     20
#define FOOT_Y    210
#define FOOT_H     30

// ─── Objetos ─────────────────────────────────────────────────
SPIClass         tftSPI(HSPI);
Adafruit_ILI9341 tft(&tftSPI, TFT_DC, TFT_CS, TFT_RST);
Adafruit_FT6206  ts;
DHT              dht(DHT22_PIN, DHT22);
Preferences      prefs;

// ─── Enums ───────────────────────────────────────────────────
enum CtrlState { ST_IDLE, ST_DRYING, ST_COOLING, ST_EMERGENCY };
enum OpMode    { MODE_NONE, MODE_ESTUFA, MODE_SECAGEM };

// ─── Estado do sistema ───────────────────────────────────────
CtrlState ctrlState  = ST_IDLE;
OpMode    activeMode = MODE_NONE;

float curTemp = NAN;
float curHum  = NAN;

// Estufa
bool estufaHeaterWanted = false;
bool estufaTempSafety   = false;

// Secagem
float dryTemp       = SECAR_TEMP_DEF;
bool  dryTempSafety = false;   // true quando temp >= dryTemp, reset em <= dryTemp-3

bool heaterOn        = false;
bool coolerOn        = false;
bool coolerTailActive = false;   // cooler sempre roda 5 min após o aquecedor desligar

unsigned long coolerTailEnd  = 0;
unsigned long lastSensorRead = 0;
unsigned long lastSensDisp   = 0;
unsigned long lastStatDisp   = 0;
unsigned long lastFootDisp   = 0;
unsigned long lastNVSSave    = 0;
int           sensorFails       = 0;
unsigned long emergencyStartMs  = 0;
int           recoveryAttempts  = 0;

// ─── WiFi / NTP / Clima ──────────────────────────────────────
bool          wifiConnected   = false;
bool          ntpSynced       = false;
float         extTemp         = NAN;      // temperatura externa (Open-Meteo)
unsigned long lastWifiCheck   = 0;
unsigned long lastWeatherFetch = 0;

// ─── Cache de tela ───────────────────────────────────────────
float     dspTemp     = -999.0f;
float     dspHum      = -999.0f;
float     dspDryTemp  = -1.0f;
bool      dspHeater    = false;
bool      dspCooler    = false;
bool      dspTailActive = false;
CtrlState dspState     = ST_IDLE;
OpMode    dspMode      = MODE_NONE;

// ─── Cache do rodapé ─────────────────────────────────────────
char     ftrTime[9]  = "";
float    ftrExtTemp  = -9999.0f;
bool     ftrWifi     = false;
uint32_t ftrToday    = 0xFFFFFFFF;
uint32_t ftrTotal    = 0xFFFFFFFF;
bool     footerDirty = true;

// ─── Rastreamento de uso ─────────────────────────────────────
unsigned long modeStartMs = 0;
unsigned long lastSaveMs  = 0;
uint32_t      nvsTotalMin = 0;
uint32_t      nvsDayMin   = 0;
uint32_t      nvsLastDay  = 0;

// ─── Touch ───────────────────────────────────────────────────
unsigned long lastTouchTime = 0;
bool          lastTouchDown = false;

// ═══════════════════════════════════════════════════════════
//  RELÉS — REGRA ÚNICA E IMUTÁVEL:
//    coolerOn = heaterOn || coolerTailActive
//  O cooler NUNCA é controlado diretamente pela lógica de modo.
//  Apenas setHeater() e updateCoolerTail() alteram o estado do cooler.
// ═══════════════════════════════════════════════════════════

// Grava imediatamente no NVS se o aquecedor estava ligado.
// Usado para iniciar o tail automaticamente após um reinício inesperado.
void saveHeaterState(bool on) {
  Preferences p;
  p.begin("estufa", false);
  p.putBool("heaterWas", on);
  p.end();
}

// Deriva e aplica o estado físico do cooler a partir do estado do aquecedor e do tail.
// É a ÚNICA função que toca o GPIO do cooler (exceto emergencyShutdown).
void applyCooler() {
  bool desired = heaterOn || coolerTailActive;
  if (coolerOn == desired) return;
  coolerOn = desired;
  digitalWrite(COOLER_PIN, desired ? RELAY_ON : RELAY_OFF);
}

// Controla o aquecedor. Ao desligar, inicia automaticamente o tail de 5 min no cooler.
void setHeater(bool on) {
  if (heaterOn == on) return;
  bool wasOn = heaterOn;
  heaterOn   = on;
  digitalWrite(HEATER_PIN, on ? RELAY_ON : RELAY_OFF);
  saveHeaterState(on);

  if (wasOn && !on) {
    // Aquecedor desligou — inicia (ou reinicia) o tail do cooler
    coolerTailActive = true;
    coolerTailEnd    = millis() + COOLER_TAIL_MS;
  }
  applyCooler();
}

// Verifica o vencimento do tail. Quando expira, re-avalia o cooler via applyCooler().
void updateCoolerTail() {
  if (!coolerTailActive) return;
  if (millis() < coolerTailEnd) return;
  coolerTailActive = false;
  applyCooler();  // desliga cooler se heater também estiver off
}

// Desativa o sistema de forma segura, respeitando o tail do cooler.
void shutdownAll() {
  estufaHeaterWanted = false;
  estufaTempSafety   = false;
  dryTempSafety      = false;
  setHeater(false);   // dispara o tail se estava ligado; applyCooler() cuida do cooler
}

// Emergência real (sensor falhou): desliga TUDO imediatamente, cancela o tail.
void emergencyShutdown() {
  coolerTailActive = false;
  heaterOn = false; digitalWrite(HEATER_PIN, RELAY_OFF);
  coolerOn = false; digitalWrite(COOLER_PIN, RELAY_OFF);
  saveHeaterState(false);
}

// ═══════════════════════════════════════════════════════════
//  LÓGICA DE CONTROLE
// ═══════════════════════════════════════════════════════════

void updateControl() {
  if (activeMode == MODE_NONE) { shutdownAll(); ctrlState = ST_IDLE; return; }
  if (ctrlState == ST_EMERGENCY) { emergencyShutdown(); return; }

  // ──────────────────────────────────────────────────────────
  //  MODO ESTUFA — controle por umidade, cap de 50 °C
  // ──────────────────────────────────────────────────────────
  if (activeMode == MODE_ESTUFA) {

    // Segurança absoluta de temperatura (histerese 47/50)
    if (!isnan(curTemp)) {
      if (!estufaTempSafety && curTemp >= ESTUFA_TEMP_MAX) estufaTempSafety = true;
      if ( estufaTempSafety && curTemp <= ESTUFA_TEMP_RES) estufaTempSafety = false;
    }

    if (!isnan(curHum)) {
      switch (ctrlState) {
        case ST_IDLE:
          if (curHum >= HUM_TRIGGER) {
            estufaHeaterWanted = true;
            ctrlState = ST_DRYING;
          }
          break;

        case ST_DRYING:
          estufaHeaterWanted = true;
          if (curHum <= HUM_RESET) {
            estufaHeaterWanted = false;
            // coolerTailEnd é definido automaticamente por setHeater(false) em applyRelays()
            ctrlState = ST_COOLING;
          }
          break;

        case ST_COOLING:
          estufaHeaterWanted = false;
          if (millis() >= coolerTailEnd) ctrlState = ST_IDLE;
          break;

        default: break;
      }
    }

    bool h = estufaHeaterWanted && !estufaTempSafety && (ctrlState == ST_DRYING);
    setHeater(h);
    // Cooler gerenciado exclusivamente por setHeater()/updateCoolerTail() via applyCooler()
  }

  // ──────────────────────────────────────────────────────────
  //  MODO SECAGEM — aquecimento contínuo até temperatura alvo
  // ──────────────────────────────────────────────────────────
  else if (activeMode == MODE_SECAGEM) {

    if (!isnan(curTemp)) {
      // Atingiu temperatura alvo → desliga
      if (!dryTempSafety && curTemp >= dryTemp) {
        dryTempSafety = true;
      }
      // Resfriou 3 °C abaixo do alvo → pode religar
      if (dryTempSafety && curTemp <= (dryTemp - SECAR_HISTERESE)) {
        dryTempSafety = false;
      }
    }
    // Temp NaN: mantém dryTempSafety atual (conservador)

    bool on = !dryTempSafety;
    setHeater(on);
    // Cooler gerenciado exclusivamente por setHeater()/updateCoolerTail() via applyCooler()
    ctrlState = ST_DRYING;
  }
}

// ═══════════════════════════════════════════════════════════
//  SENSOR DHT22
// ═══════════════════════════════════════════════════════════

void readSensor() {
  if (millis() - lastSensorRead < SENSOR_INTERVAL) return;
  lastSensorRead = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (isnan(t) || isnan(h)) {
    sensorFails++;
    if (sensorFails >= SENSOR_FAIL_MAX && ctrlState != ST_EMERGENCY) {
      ctrlState = ST_EMERGENCY;
      emergencyShutdown();
      emergencyStartMs = millis();
    }
    return;
  }

  // Leitura bem-sucedida — zera contadores e cancela emergency se estava ativo
  sensorFails      = 0;
  recoveryAttempts = 0;
  if (ctrlState == ST_EMERGENCY) {
    ctrlState      = ST_IDLE;
    emergencyStartMs = 0;
    drawScreen();
  }
  curTemp = t;
  curHum  = h;
}

// ═══════════════════════════════════════════════════════════
//  NVS
// ═══════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════
//  RECUPERAÇÃO DE SENSOR
// ═══════════════════════════════════════════════════════════

void checkSensorRecovery() {
  if (ctrlState != ST_EMERGENCY) return;
  if (millis() - emergencyStartMs < SENSOR_RECOVERY_DELAY) return;

  recoveryAttempts++;

  if (recoveryAttempts > SENSOR_RECOVERY_MAX) {
    // Esgotou tentativas soft — reinicia o dispositivo inteiro
    tft.fillRect(0, STAT_Y, SCREEN_W, FOOT_H + STAT_H, CLR_BG);
    tft.setTextColor(CLR_RED); tft.setTextSize(1);
    tft.setCursor(20, STAT_Y + 6);
    tft.print("Sensor irrecuperavel. Reiniciando...");
    delay(3000);
    ESP.restart();
    return;
  }

  // Tentativa soft: reinicializa a biblioteca e aguarda sensor estabilizar
  tft.fillRect(0, STAT_Y, SCREEN_W, STAT_H, CLR_BG);
  tft.setTextColor(CLR_YELLOW); tft.setTextSize(1);
  tft.setCursor(20, STAT_Y + 6);
  char buf[48];
  snprintf(buf, sizeof(buf), "Recuperando sensor... tentativa %d/%d",
           recoveryAttempts, SENSOR_RECOVERY_MAX);
  tft.print(buf);

  dht.begin();
  delay(2000);  // DHT22 precisa de ~2s para estabilizar após reinit

  sensorFails      = 0;
  ctrlState        = ST_IDLE;
  emergencyStartMs = millis();  // reseta timer — se falhar de novo entra no próximo ciclo
  drawScreen();
}

uint32_t currentDayIdx() {
  if (ntpSynced) {
    struct tm t;
    if (getLocalTime(&t)) {
      // Retorna YYYYMMDD — chave de dia estável e real
      return (uint32_t)((t.tm_year + 1900) * 10000 +
                        (t.tm_mon  + 1)    * 100   +
                         t.tm_mday);
    }
  }
  return (uint32_t)(millis() / 86400000UL);  // fallback sem NTP
}

void loadNVS() {
  prefs.begin("estufa", true);
  nvsTotalMin        = prefs.getUInt("totalMin",  0);
  nvsDayMin          = prefs.getUInt("dayMin",    0);
  nvsLastDay         = prefs.getUInt("lastDay",   0);
  dryTemp            = prefs.getFloat("dryTemp",  SECAR_TEMP_DEF);
  bool heaterWasOn   = prefs.getBool("heaterWas", false);
  prefs.end();

  dryTemp = constrain(dryTemp, SECAR_TEMP_MIN, SECAR_TEMP_MAX);
  if (currentDayIdx() != nvsLastDay) { nvsDayMin = 0; nvsLastDay = currentDayIdx(); }

  // Segurança de reinício: se o aquecedor estava ligado quando o dispositivo foi
  // reiniciado (queda de energia, crash), liga o cooler por 5 min para dissipar calor residual.
  if (heaterWasOn) {
    coolerTailActive = true;
    coolerTailEnd    = millis() + COOLER_TAIL_MS;
    coolerOn         = true;
    digitalWrite(COOLER_PIN, RELAY_ON);
    // Limpa o flag — o tail foi iniciado, não precisamos repetir no próximo boot
    saveHeaterState(false);
  }
}

void saveNVS(bool force = false) {
  unsigned long now = millis();
  if (!force && (now - lastNVSSave < NVS_SAVE_INTERVAL)) return;

  uint32_t addMin = 0;
  if (activeMode != MODE_NONE && modeStartMs > 0) {
    unsigned long since = (lastSaveMs > modeStartMs) ? lastSaveMs : modeStartMs;
    addMin = (uint32_t)((now - since) / 60000UL);
  }

  uint32_t dayIdx = currentDayIdx();
  if (dayIdx != nvsLastDay) { nvsDayMin = 0; nvsLastDay = dayIdx; }

  nvsDayMin   += addMin;
  nvsTotalMin += addMin;
  lastSaveMs   = now;
  lastNVSSave  = now;

  prefs.begin("estufa", false);
  prefs.putUInt("totalMin",  nvsTotalMin);
  prefs.putUInt("dayMin",    nvsDayMin);
  prefs.putUInt("lastDay",   nvsLastDay);
  prefs.putFloat("dryTemp",  dryTemp);
  prefs.end();
}

// ═══════════════════════════════════════════════════════════
//  TOUCH
// ═══════════════════════════════════════════════════════════

// Touch I2C capacitivo FT6336G
// Chip reporta em portrait (x=0..240, y=0..320).
// setRotation(1) landscape: tx = map(p.y, 0..320, 0..SCREEN_W)
//                           ty = map(p.x, 0..240, SCREEN_H..0)
bool readTouch(int16_t &tx, int16_t &ty) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.x == 0 && p.y == 0) return false;
  tx = constrain(map(p.y, 0, 320, 0, SCREEN_W), 0, SCREEN_W - 1);
  ty = constrain(map(p.x, 0, 240, SCREEN_H, 0), 0, SCREEN_H - 1);
  return true;
}

bool detectTap(int16_t &tx, int16_t &ty) {
  bool pressed = readTouch(tx, ty);
  unsigned long now = millis();
  if (pressed && !lastTouchDown && (now - lastTouchTime > 300)) {
    lastTouchDown = true; lastTouchTime = now; return true;
  }
  if (!pressed) lastTouchDown = false;
  return false;
}

// ═══════════════════════════════════════════════════════════
//  ÍCONES
// ═══════════════════════════════════════════════════════════

void drawIconEstufa(int cx, int cy, uint16_t color) {
  tft.fillRoundRect(cx-4, cy-20, 8, 26, 4, color);
  tft.fillCircle(cx, cy+10, 8, color);
  tft.fillRect(cx-2, cy-4, 4, 18, CLR_BG);
  tft.fillRect(cx-1, cy+2, 2, 8, CLR_RED);
  tft.fillCircle(cx, cy+10, 5, CLR_RED);
  tft.drawFastHLine(cx+4, cy-10, 5, color);
  tft.drawFastHLine(cx+4, cy-2,  5, color);
  tft.drawFastHLine(cx+4, cy+6,  5, color);
}

void drawIconSecagem(int cx, int cy, uint16_t color) {
  tft.fillCircle(cx, cy, 9, color);
  tft.fillCircle(cx, cy, 4, CLR_BG);
  tft.fillCircle(cx, cy, 3, color);
  for (int a = 0; a < 360; a += 45) {
    float r = a * M_PI / 180.0f;
    tft.drawLine(cx+(int)(12.0f*cosf(r)), cy+(int)(12.0f*sinf(r)),
                 cx+(int)(20.0f*cosf(r)), cy+(int)(20.0f*sinf(r)), color);
  }
}

// ═══════════════════════════════════════════════════════════
//  BOTÕES DE MODO
// ═══════════════════════════════════════════════════════════

// Desenha a seção de controle de temperatura dentro do botão SECAGEM
void drawSecagemTempCtrl(bool full) {
  uint16_t clrBrd = CLR_ORANGE;

  if (full) {
    // Divisor interno
    tft.drawFastHLine(BTN_R+4, SC_DIV_Y, BTN_W-8, 0x2945);

    // Texto "Alvo:"
    tft.setTextColor(CLR_GRAY);
    tft.setTextSize(1);
    tft.setCursor(BTN_R + (BTN_W/2) - 22, SC_DIV_Y + 3);
    tft.print("Temp.alvo:");
  }

  // Limpa área dos botões e valor
  tft.fillRect(BTN_R+4, SC_CTR_Y, BTN_W-8, SC_BTN_H, CLR_PANEL);

  // Botão [−]
  bool atMin = (dryTemp <= SECAR_TEMP_MIN);
  uint16_t clrMinus = atMin ? CLR_DARKGRAY : clrBrd;
  tft.fillRoundRect(SC_MINUS_X, SC_CTR_Y, SC_BTN_W, SC_BTN_H, 4, 0x0841);
  tft.drawRoundRect(SC_MINUS_X, SC_CTR_Y, SC_BTN_W, SC_BTN_H, 4, clrMinus);
  tft.setTextColor(clrMinus);
  tft.setTextSize(2);
  tft.setCursor(SC_MINUS_X + 13, SC_CTR_Y + 3);
  tft.print("-");

  // Botão [+]
  bool atMax = (dryTemp >= SECAR_TEMP_MAX);
  uint16_t clrPlus = atMax ? CLR_DARKGRAY : clrBrd;
  tft.fillRoundRect(SC_PLUS_X, SC_CTR_Y, SC_BTN_W, SC_BTN_H, 4, 0x0841);
  tft.drawRoundRect(SC_PLUS_X, SC_CTR_Y, SC_BTN_W, SC_BTN_H, 4, clrPlus);
  tft.setTextColor(clrPlus);
  tft.setTextSize(2);
  tft.setCursor(SC_PLUS_X + 13, SC_CTR_Y + 3);
  tft.print("+");

  // Valor central
  char buf[8];
  snprintf(buf, sizeof(buf), "%.0f", dryTemp);
  tft.setTextColor(CLR_WHITE);
  tft.setTextSize(2);
  // Centro entre os botões: BTN_R + 44 a BTN_R + 104 = 60px
  int vx = BTN_R + 44 + (60 - (int)strlen(buf)*12 + 6) / 2;
  tft.setCursor(vx, SC_CTR_Y + 3);
  tft.print(buf);
  tft.setTextSize(1);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(vx + (int)strlen(buf)*12 + 1, SC_CTR_Y + 7);
  tft.print("C");

  dspDryTemp = dryTemp;
}

void drawModeBtn(OpMode mode) {
  bool sel = (activeMode == mode);
  int  bx  = (mode == MODE_ESTUFA) ? BTN_L : BTN_R;
  int  by  = BTN_Y;

  uint16_t clrBrd  = sel ? ((mode == MODE_ESTUFA) ? CLR_CYAN   : CLR_ORANGE) : CLR_DARKGRAY;
  uint16_t clrIcon = sel ? clrBrd  : CLR_GRAY;
  uint16_t clrBg   = sel ? CLR_PANEL : CLR_BG;

  tft.fillRoundRect(bx, by, BTN_W, BTN_H, 10, clrBg);
  tft.drawRoundRect(bx, by, BTN_W, BTN_H, 10, clrBrd);
  if (sel) tft.drawRoundRect(bx+1, by+1, BTN_W-2, BTN_H-2, 9, clrBrd);  // borda dupla

  bool secSelected = (mode == MODE_SECAGEM && sel);

  // Ícone: sobe um pouco quando SECAGEM está selecionado (para abrir espaço para o controle)
  int icy = secSelected ? (by + 22) : (by + 34);
  int icx = bx + BTN_W / 2;

  if (mode == MODE_ESTUFA)
    drawIconEstufa(icx, icy, clrIcon);
  else
    drawIconSecagem(icx, icy, clrIcon);

  // Label
  tft.setTextColor(clrIcon);
  tft.setTextSize(2);
  const char* lbl = (mode == MODE_ESTUFA) ? "ESTUFA" : "SECAGEM";
  int lblW = strlen(lbl) * 12;
  int lblY = secSelected ? (by + 46) : (by + BTN_H - 22);
  tft.setCursor(bx + (BTN_W - lblW) / 2, lblY);
  tft.print(lbl);

  // Ponto de status
  tft.fillCircle(bx + BTN_W - 12, by + 12, 5, sel ? CLR_GREEN : CLR_DARKGRAY);

  // Controle de temperatura (só SECAGEM selecionado)
  if (secSelected) {
    drawSecagemTempCtrl(true);
  }
}

// ═══════════════════════════════════════════════════════════
//  DISPLAY DE SENSORES
// ═══════════════════════════════════════════════════════════

void updateSensors() {
  bool tC = isnan(curTemp) != isnan(dspTemp) || (!isnan(curTemp) && fabsf(curTemp-dspTemp) >= 0.1f);
  bool hC = isnan(curHum)  != isnan(dspHum)  || (!isnan(curHum)  && fabsf(curHum -dspHum)  >= 0.5f);
  if (!tC && !hC) return;

  // Temperatura
  if (tC) {
    dspTemp = curTemp;
    tft.fillRect(BTN_L, SENS_Y+14, BTN_W, 38, CLR_BG);
    uint16_t clr = CLR_WHITE;
    if (!isnan(curTemp)) {
      if      (curTemp >= ESTUFA_TEMP_MAX) clr = CLR_RED;
      else if (curTemp >= TEMP_WARN)       clr = CLR_ORANGE;
    } else { clr = CLR_DARKGRAY; }
    tft.setTextColor(clr);
    tft.setTextSize(3);
    char buf[10];
    if (isnan(curTemp)) snprintf(buf,sizeof(buf),"--.-");
    else                snprintf(buf,sizeof(buf),"%.1f",curTemp);
    tft.setCursor(BTN_L + 4, SENS_Y + 16);
    tft.print(buf);
    tft.setTextSize(2);
    tft.setCursor(BTN_L + 4 + (int)strlen(buf)*18 + 2, SENS_Y + 22);
    tft.print("C");
    // Triângulo de aviso quando próximo do limite
    if (!isnan(curTemp) && curTemp >= TEMP_WARN) {
      tft.fillTriangle(BTN_L+140,SENS_Y+14, BTN_L+130,SENS_Y+30, BTN_L+150,SENS_Y+30, CLR_RED);
      tft.setTextColor(CLR_BG); tft.setTextSize(1);
      tft.setCursor(BTN_L+138, SENS_Y+22); tft.print("!");
    }
  }

  // Umidade
  if (hC) {
    dspHum = curHum;
    tft.fillRect(BTN_R, SENS_Y+14, BTN_W, 38, CLR_BG);
    uint16_t clr = CLR_CYAN;
    if (!isnan(curHum)) {
      if (curHum >= HUM_TRIGGER) clr = CLR_ORANGE;
    } else { clr = CLR_DARKGRAY; }
    tft.setTextColor(clr);
    tft.setTextSize(3);
    char buf[10];
    if (isnan(curHum)) snprintf(buf,sizeof(buf),"--.-");
    else               snprintf(buf,sizeof(buf),"%.1f",curHum);
    tft.setCursor(BTN_R + 4, SENS_Y + 16);
    tft.print(buf);
    tft.setTextSize(2);
    tft.setCursor(BTN_R + 4 + (int)strlen(buf)*18 + 2, SENS_Y + 22);
    tft.print("%");
  }
}

// ═══════════════════════════════════════════════════════════
//  ÍCONES DA STATUS BAR
// ═══════════════════════════════════════════════════════════

// Lâmpada — aquecedor
//   ON : bulbo amarelo + filamento branco + aura + raios
//   OFF: contorno cinza + filamento apagado + soquete escuro
void drawBulbIcon(int cx, int cy, bool on) {
  int gy = cy - 1;   // centro do globo (ligeiramente acima do centro da barra)

  if (on) {
    // Aura de brilho externo (anel dourado)
    tft.drawCircle(cx, gy, 7, CLR_GOLD);
    // Globo aceso
    tft.fillCircle(cx, gy, 5, CLR_YELLOW);
    // Filamento brilhante (ponto branco central)
    tft.fillCircle(cx, gy, 2, CLR_WHITE);
    // Soquete/base
    tft.fillRect(cx-3, gy+6, 6, 2, CLR_GOLD);
    tft.fillRect(cx-2, gy+8, 4, 2, CLR_GOLD);
    // Raios de luz (5 pixels radiais ao redor do globo)
    tft.drawPixel(cx,   gy-8, CLR_GOLD);   // topo
    tft.drawPixel(cx+7, gy-4, CLR_GOLD);   // direita-cima
    tft.drawPixel(cx-7, gy-4, CLR_GOLD);   // esquerda-cima
    tft.drawPixel(cx+7, gy+2, CLR_GOLD);   // direita-baixo
    tft.drawPixel(cx-7, gy+2, CLR_GOLD);   // esquerda-baixo
  } else {
    // Globo apagado — só contorno
    tft.drawCircle(cx, gy, 5, CLR_DARKGRAY);
    // Filamento frio (linha horizontal interna)
    tft.drawFastHLine(cx-2, gy, 4, 0x3186);
    // Soquete apagado
    tft.fillRect(cx-3, gy+6, 6, 2, CLR_DARKGRAY);
    tft.fillRect(cx-2, gy+8, 4, 2, CLR_DARKGRAY);
  }
}

// Ventilador — cooler
//   ON : 3 pás ciano + hub branco + anel de rotação
//   OFF: 3 pás cinza escuro + hub cinza
void drawFanIcon(int cx, int cy, bool on) {
  uint16_t clrBlade = on ? CLR_CYAN  : CLR_DARKGRAY;
  uint16_t clrHub   = on ? CLR_WHITE : CLR_GRAY;

  // 3 pás em 0°, 120°, 240° — forma de hélice assimétrica (sugere sentido de rotação)
  for (int i = 0; i < 3; i++) {
    float base = i * 2.094f;           // i × 120° em radianos
    float back = base - 0.61f;         // ponta traseira da pá (-35°)
    float fwd  = base + 0.87f;         // borda dianteira da pá (+50°)

    int x0 = cx + (int)(2.0f * cosf(base));    // raiz (junto ao hub)
    int y0 = cy + (int)(2.0f * sinf(base));
    int x1 = cx + (int)(7.5f * cosf(back));    // ponta exterior traseira
    int y1 = cy + (int)(7.5f * sinf(back));
    int x2 = cx + (int)(6.0f * cosf(fwd));     // borda dianteira
    int y2 = cy + (int)(6.0f * sinf(fwd));

    tft.fillTriangle(x0, y0, x1, y1, x2, y2, clrBlade);
  }

  // Hub central
  tft.fillCircle(cx, cy, 3, clrHub);
  tft.fillCircle(cx, cy, 1, CLR_BG);   // eixo (buraco no centro)

  // Anel externo quando ligado — indica movimento/rotação
  if (on) tft.drawCircle(cx, cy, 9, 0x03EF);  // teal escuro
}

// ═══════════════════════════════════════════════════════════
//  BARRA DE STATUS
// ═══════════════════════════════════════════════════════════

void updateStatusBar() {
  bool changed = (heaterOn        != dspHeater     ||
                  coolerOn        != dspCooler     ||
                  coolerTailActive != dspTailActive ||
                  ctrlState       != dspState      ||
                  activeMode      != dspMode);
  // Força redesenho enquanto tail está ativo (para atualizar o countdown)
  if (coolerTailActive) changed = true;
  if (!changed) return;

  dspHeater    = heaterOn;
  dspCooler    = coolerOn;
  dspTailActive = coolerTailActive;
  dspState     = ctrlState;
  dspMode      = activeMode;

  tft.fillRect(0, STAT_Y, SCREEN_W, STAT_H, CLR_BG);
  tft.drawFastHLine(0, STAT_Y, SCREEN_W, CLR_DARKGRAY);

  // Ícones de estado dos relés (centrados verticalmente na barra)
  int icy = STAT_Y + 9;
  drawBulbIcon(14, icy, heaterOn);   // lâmpada = aquecedor
  drawFanIcon(44,  icy, coolerOn);   // ventilador = cooler
  tft.drawFastVLine(60, STAT_Y+3, STAT_H-6, CLR_DARKGRAY);

  tft.setTextSize(1);
  tft.setCursor(66, STAT_Y + 6);
  if (ctrlState == ST_EMERGENCY) {
    tft.setTextColor(CLR_RED);
    unsigned long elapsed = millis() - emergencyStartMs;
    if (elapsed < SENSOR_RECOVERY_DELAY) {
      char buf[48];
      unsigned long remSec = (SENSOR_RECOVERY_DELAY - elapsed) / 1000UL + 1;
      snprintf(buf, sizeof(buf), "Sensor falhou! Recuperando em %lus (%d/%d)",
               remSec, recoveryAttempts + 1, SENSOR_RECOVERY_MAX);
      tft.print(buf);
    } else {
      tft.print("!! EMERGENCIA: sensor falhou !!");
    }
  } else if (activeMode == MODE_SECAGEM) {
    char buf[40];
    if (heaterOn) {
      tft.setTextColor(CLR_ORANGE);
      snprintf(buf, sizeof(buf), "Secagem %.0fC -> Aquecendo...", dryTemp);
    } else if (coolerTailActive) {
      tft.setTextColor(CLR_CYAN);
      unsigned long rem = (coolerTailEnd > millis()) ? (coolerTailEnd - millis())/1000UL : 0;
      snprintf(buf, sizeof(buf), "Secagem %.0fC -> Resfriando %lus", dryTemp, rem);
    } else {
      tft.setTextColor(CLR_DARKGRAY);
      snprintf(buf, sizeof(buf), "Secagem %.0fC -> Aguardando", dryTemp);
    }
    tft.print(buf);
  } else if (activeMode == MODE_ESTUFA) {
    if (ctrlState == ST_DRYING) {
      tft.setTextColor(estufaTempSafety ? CLR_YELLOW : CLR_ORANGE);
      tft.print(estufaTempSafety ? "Secando (temp.safety ativo)" : "Secando...");
    } else if (ctrlState == ST_COOLING) {
      tft.setTextColor(CLR_CYAN);
      unsigned long rem = (coolerTailEnd > millis()) ? (coolerTailEnd - millis())/1000UL : 0;
      char buf[28]; snprintf(buf,sizeof(buf),"Resfriando... %lus", rem);
      tft.print(buf);
    } else {
      tft.setTextColor(CLR_DARKGRAY);
      tft.print("Aguardando ciclo");
    }
  } else {
    tft.setTextColor(CLR_DARKGRAY);
    tft.print("Nenhum modo ativo");
  }
}

// ═══════════════════════════════════════════════════════════
//  RODAPÉ
// ═══════════════════════════════════════════════════════════

void updateFooter() {
  char timeStr[9];
  char buf[48];
  getTimeStr(timeStr);

  uint32_t sessionMin = (activeMode != MODE_NONE && modeStartMs > 0)
                        ? (uint32_t)((millis()-modeStartMs)/60000UL) : 0;
  uint32_t today = nvsDayMin + sessionMin;
  uint32_t total = nvsTotalMin + sessionMin;

  bool timeChanged  = (strcmp(timeStr, ftrTime) != 0);
  bool wifiChanged  = (wifiConnected != ftrWifi);
  bool extChanged   = (isnan(extTemp) != isnan(ftrExtTemp)) ||
                      (!isnan(extTemp) && fabsf(extTemp - ftrExtTemp) >= 0.1f);
  bool usageChanged = (today != ftrToday || total != ftrTotal);
  bool fullRedraw   = footerDirty || wifiChanged || extChanged || usageChanged;

  if (!timeChanged && !fullRedraw) return;

  tft.setTextSize(1);

  if (fullRedraw) {
    footerDirty = false;
    tft.fillRect(0, FOOT_Y, SCREEN_W, FOOT_H, 0x0841);
    tft.drawFastHLine(0, FOOT_Y, SCREEN_W, CLR_DARKGRAY);
    tft.drawFastVLine(74,  FOOT_Y+2, 13, CLR_DARKGRAY);
    tft.drawFastVLine(178, FOOT_Y+2, 13, CLR_DARKGRAY);
    tft.drawFastHLine(0, FOOT_Y+17, SCREEN_W, 0x1082);
    tft.drawFastVLine(168, FOOT_Y+18, 11, CLR_DARKGRAY);

    tft.setTextColor(wifiConnected ? CLR_GRAY : CLR_DARKGRAY);
    tft.setCursor(80, FOOT_Y+5);
    tft.print(wifiConnected ? LOCATION_NAME : "WiFi: OFF");

    tft.setCursor(184, FOOT_Y+5);
    if (!isnan(extTemp)) {
      snprintf(buf, sizeof(buf), "Ext:%.1fC", extTemp);
      tft.setTextColor(CLR_CYAN);
    } else {
      snprintf(buf, sizeof(buf), "Ext: --.-C");
      tft.setTextColor(CLR_DARKGRAY);
    }
    tft.print(buf);

    snprintf(buf, sizeof(buf), "24h: %uh %02um", today/60, today%60);
    tft.setTextColor(CLR_GOLD);
    tft.setCursor(6, FOOT_Y+21); tft.print(buf);
    snprintf(buf, sizeof(buf), "30d: %uh", total/60);
    tft.setCursor(174, FOOT_Y+21); tft.print(buf);
    tft.setTextColor(0x2945);
    tft.setCursor(244, FOOT_Y+21); tft.print("(aprox)");

    ftrWifi    = wifiConnected;
    ftrExtTemp = extTemp;
    ftrToday   = today;
    ftrTotal   = total;
  }

  // Hora: sobrescreve no lugar com cor de fundo — sem fillRect, sem flash
  strcpy(ftrTime, timeStr);
  tft.setTextColor(ntpSynced ? CLR_WHITE : CLR_GRAY, 0x0841);
  tft.setCursor(6, FOOT_Y+5);
  tft.print(timeStr);
}

// ═══════════════════════════════════════════════════════════
//  TELA ESTÁTICA
// ═══════════════════════════════════════════════════════════

void drawScreen() {
  tft.fillScreen(CLR_BG);
  tft.setTextColor(CLR_GRAY); tft.setTextSize(1);
  const char* hdr = "Escolha o modo de operacao";
  tft.setCursor((SCREEN_W - (int)strlen(hdr)*6)/2, 7);
  tft.print(hdr);
  tft.drawFastHLine(0, HDR_H, SCREEN_W, CLR_DARKGRAY);

  drawModeBtn(MODE_ESTUFA);
  drawModeBtn(MODE_SECAGEM);
  tft.drawFastHLine(0, BTN_Y+BTN_H+1, SCREEN_W, CLR_DARKGRAY);

  tft.setTextColor(CLR_DARKGRAY); tft.setTextSize(1);
  tft.setCursor(BTN_L+4, SENS_Y+4); tft.print("TEMPERATURA");
  tft.setCursor(BTN_R+4, SENS_Y+4); tft.print("UMIDADE");
  tft.drawFastVLine(SCREEN_W/2, SENS_Y, SENS_H, CLR_DARKGRAY);
  tft.drawFastHLine(0, SENS_Y+SENS_H, SCREEN_W, CLR_DARKGRAY);

  // Força redesenho dos valores dinâmicos
  dspTemp = -999.0f; dspHum = -999.0f; dspDryTemp = -1.0f;
  dspHeater = !heaterOn; dspCooler = !coolerOn;
  dspState = (CtrlState)99; dspMode = (OpMode)99;
  footerDirty = true;

  updateSensors();
  updateStatusBar();
  updateFooter();
}

// ═══════════════════════════════════════════════════════════
//  TOUCH — seleção de modo e botões +/−
// ═══════════════════════════════════════════════════════════

void handleTouch() {
  int16_t tx, ty;
  if (!detectTap(tx, ty)) return;

  // ── Botões +/− do SECAGEM (prioridade máxima) ────────────
  if (activeMode == MODE_SECAGEM) {
    bool minus = (tx >= MINUS_TX1 && tx <= MINUS_TX2 && ty >= MINUS_TY1 && ty <= MINUS_TY2);
    bool plus  = (tx >= PLUS_TX1  && tx <= PLUS_TX2  && ty >= PLUS_TY1  && ty <= PLUS_TY2);

    if (minus && dryTemp > SECAR_TEMP_MIN) {
      dryTemp -= 1.0f;
      // Se a temperatura atual já está abaixo do novo alvo, reseta safety
      if (!isnan(curTemp) && curTemp < dryTemp) dryTempSafety = false;
      drawSecagemTempCtrl(false);
      saveNVS(true);
      return;
    }
    if (plus && dryTemp < SECAR_TEMP_MAX) {
      dryTemp += 1.0f;
      dryTempSafety = false;   // reseta para permitir aquecimento até novo alvo
      drawSecagemTempCtrl(false);
      saveNVS(true);
      return;
    }
  }

  // ── Seleção de modo ──────────────────────────────────────
  OpMode tapped = MODE_NONE;
  if (tx >= BTN_L && tx <= BTN_L+BTN_W && ty >= BTN_Y && ty <= BTN_Y+BTN_H)
    tapped = MODE_ESTUFA;
  else if (tx >= BTN_R && tx <= BTN_R+BTN_W && ty >= BTN_Y && ty <= BTN_Y+BTN_H)
    tapped = MODE_SECAGEM;

  if (tapped == MODE_NONE) return;

  if (activeMode == tapped) {
    // Toggle: desativa
    saveNVS(true);
    activeMode = MODE_NONE;
    modeStartMs = 0;
    ctrlState = ST_IDLE;
    estufaHeaterWanted = false;
    estufaTempSafety   = false;
    dryTempSafety      = false;
    shutdownAll();
  } else {
    // Ativa novo modo
    if (activeMode != MODE_NONE) saveNVS(true);
    activeMode  = tapped;
    modeStartMs = millis();
    lastSaveMs  = millis();
    ctrlState   = ST_IDLE;
    estufaHeaterWanted = false;
    estufaTempSafety   = false;
    dryTempSafety      = false;
  }

  drawModeBtn(MODE_ESTUFA);
  drawModeBtn(MODE_SECAGEM);
  dspHeater = !heaterOn; dspCooler = !coolerOn;
  dspState  = (CtrlState)99; dspMode = (OpMode)99;
  updateStatusBar();
}

// ═══════════════════════════════════════════════════════════
//  WIFI / NTP / CLIMA
// ═══════════════════════════════════════════════════════════

// Mostra mensagem de status WiFi no rodapé durante o setup
void showWifiStatus(const char* msg) {
  tft.fillRect(0, FOOT_Y, SCREEN_W, FOOT_H, 0x0841);
  tft.drawFastHLine(0, FOOT_Y, SCREEN_W, CLR_DARKGRAY);
  tft.setTextSize(1);
  tft.setTextColor(CLR_GRAY);
  tft.setCursor(6, FOOT_Y + 11);
  tft.print(msg);
}

// Conecta ao WiFi. Bloqueia até WIFI_TIMEOUT_MS ou conexão bem-sucedida.
// Chamado uma vez no setup(); reconexões silenciosas são feitas no loop.
void connectWiFi() {
  showWifiStatus("Conectando WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(300);
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    // Sincroniza o relógio interno com o servidor NTP
    configTime(NTP_GMT_OFFSET, NTP_DST_OFFSET, NTP_SERVER);
    showWifiStatus("WiFi OK — sincronizando horario...");

    // Aguarda até 5 s para o NTP responder
    struct tm t;
    unsigned long ntpStart = millis();
    while (!getLocalTime(&t) && millis() - ntpStart < 5000) delay(200);
    ntpSynced = getLocalTime(&t);
  } else {
    showWifiStatus("WiFi: falha — operando sem rede");
    delay(1500);
  }
}

// Verifica conexão e tenta reconectar se caiu (não bloqueante)
void checkWiFi() {
  if (millis() - lastWifiCheck < WIFI_RETRY_MS) return;
  lastWifiCheck = millis();

  bool connected = (WiFi.status() == WL_CONNECTED);
  if (connected && !wifiConnected) {
    // Reconectou — sincroniza NTP
    configTime(NTP_GMT_OFFSET, NTP_DST_OFFSET, NTP_SERVER);
  }
  wifiConnected = connected;
  if (wifiConnected) {
    struct tm t;
    if (!ntpSynced) ntpSynced = getLocalTime(&t);
  }
}

// Faz GET na Open-Meteo e extrai temperature de current_weather.
// Usa HTTPS sem validação de certificado (aceitável para IoT hobbyista).
// Bloqueia por ~1-3 s — chamado no máximo a cada WEATHER_INTERVAL.
void fetchWeather() {
  if (!wifiConnected) return;
  if (millis() - lastWeatherFetch < WEATHER_INTERVAL && lastWeatherFetch > 0) return;
  lastWeatherFetch = millis();

  WiFiClientSecure cli;
  cli.setInsecure();   // sem validação de certificado
  HTTPClient http;
  http.begin(cli, WEATHER_URL);
  http.setTimeout(8000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();

    // Localiza o objeto "current_weather": {...} e extrai "temperature"
    int cwPos = payload.indexOf("\"current_weather\":{");
    if (cwPos < 0) cwPos = payload.indexOf("\"current_weather\": {");
    if (cwPos >= 0) {
      int tPos = payload.indexOf("\"temperature\":", cwPos);
      if (tPos >= 0) {
        int valStart = tPos + 14;
        while (valStart < payload.length() && payload[valStart] == ' ') valStart++;
        char c = payload[valStart];
        if (c == '-' || (c >= '0' && c <= '9')) {
          float val = payload.substring(valStart).toFloat();
          if (val > -60.0f && val < 70.0f) extTemp = val;
        }
      }
    }
  }
  http.end();
}

// Preenche buf (tamanho >= 9) com horário local "HH:MM:SS"
// ou com uptime se NTP não estiver sincronizado.
void getTimeStr(char* buf) {
  struct tm t;
  if (ntpSynced && getLocalTime(&t)) {
    snprintf(buf, 9, "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  } else {
    unsigned long up = millis() / 1000UL;
    snprintf(buf, 9, "%02lu:%02lu:%02lu", (up/3600)%24, (up/60)%60, up%60);
  }
}

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  // Relés OFF — segurança antes de qualquer outra inicialização
  pinMode(HEATER_PIN, OUTPUT); digitalWrite(HEATER_PIN, RELAY_OFF);
  pinMode(COOLER_PIN, OUTPUT); digitalWrite(COOLER_PIN, RELAY_OFF);

  // Display
  pinMode(TFT_BL, OUTPUT); analogWrite(TFT_BL, 255);
  tftSPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin(40000000);
  tft.setRotation(1);
  tft.fillScreen(CLR_BG);

  // Touch I2C
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, LOW);  delay(10);
  digitalWrite(TOUCH_RST, HIGH); delay(100);
  pinMode(TOUCH_INT, INPUT_PULLUP);
  ts.begin(40);

  // Sensor
  dht.begin();

  // NVS (carrega dryTemp salvo e contadores de uso)
  loadNVS();

  // Tela inicial (antes do WiFi para resposta imediata)
  drawScreen();

  // WiFi + NTP + primeira leitura de clima
  connectWiFi();      // bloqueia até 12 s com feedback no footer
  fetchWeather();     // busca temperatura externa imediatamente após conectar
  drawScreen();       // redesenha com dados reais (hora, local, temp ext)
}

// ═══════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════

void loop() {
  unsigned long now = millis();

  readSensor();
  checkSensorRecovery();
  updateControl();
  updateCoolerTail();   // gerencia o tail de 5 min do cooler após cada desligamento do aquecedor

  if (now - lastSensDisp >= 500)  { lastSensDisp = now; updateSensors(); }
  if (now - lastStatDisp >= 1000) { lastStatDisp = now; updateStatusBar(); }
  if (now - lastFootDisp >= 1000) { lastFootDisp = now; updateFooter(); }

  // WiFi: verifica reconexão a cada 60 s (não bloqueante)
  checkWiFi();

  // Clima: busca Open-Meteo a cada 10 min (bloqueia ~1-3 s quando executa)
  if (wifiConnected) fetchWeather();

  // Atualiza o valor de dryTemp no botão se mudou (ex.: após NVS load)
  if (activeMode == MODE_SECAGEM && dryTemp != dspDryTemp) {
    drawSecagemTempCtrl(false);
  }

  saveNVS();
  handleTouch();
}
