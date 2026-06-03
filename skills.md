# Skills & Padrões de Desenvolvimento — Estufa de Filamentos

> Guia de referência para desenvolvimento como engenheiro sênior de firmware ESP32-S3.
> Consulte antes de implementar qualquer funcionalidade nova.

---

## Perfil de Desenvolvedor

**Papel:** Engenheiro sênior de firmware embarcado para ESP32-S3
**Postura:** Código correto antes de código bonito. Sem abstração prematura. Sem features não pedidas.
**Regra de ouro de pinagem:** Nunca alterar GPIOs definidos sem avisar o desenvolvedor e aguardar confirmação explícita.

---

## Hardware Fixo — Não Alterar Sem Confirmação

**Board:** LCDWIKI ES3C28P / ES3N28P
**Chip:** ESP32-S3 (Xtensa LX7 dual-core, 240 MHz, 512 KB SRAM, 8 MB Flash)
**IDE settings:** Board=`ESP32S3 Dev Module` | Partition=`Huge APP (3MB No OTA)` | USB=`Hardware CDC and JTAG`

### GPIOs em uso — TRAVADOS

| Função | GPIO | Direção | Observação |
|--------|------|---------|-----------|
| TFT_SCK | 12 | OUTPUT | SPI clock display |
| TFT_MOSI | 11 | OUTPUT | SPI dados display |
| TFT_MISO | 13 | INPUT | SPI leitura |
| TFT_CS | 10 | OUTPUT | Chip select display |
| TFT_DC | 46 | OUTPUT | Data/Command |
| TFT_RST | -1 | — | Sem GPIO |
| TFT_BL | 45 | OUTPUT | Backlight — `analogWrite(TFT_BL, 255)` |
| TOUCH_SDA | 16 | I/O | I2C dados FT6336G |
| TOUCH_SCL | 15 | OUTPUT | I2C clock FT6336G |
| TOUCH_INT | 17 | INPUT | Interrupt (INPUT_PULLUP) |
| TOUCH_RST | 18 | OUTPUT | Reset hardware do touch |
| DHT22_PIN | 2 | I/O | Sensor temperatura/umidade |
| HEATER_PIN | 3 | OUTPUT | Relé aquecedor (ativo LOW) |
| COOLER_PIN | 14 | OUTPUT | Relé cooler/fan (ativo LOW) |
| Spare | 21 | — | Disponível no conector P3/CN1 |

### Nota importante: Sem GPIOs input-only no ESP32-S3

No ESP32-S3 **todos os GPIOs são bidirecionais**. A restrição de GPIOs 34/35/36/39 como input-only existia apenas no ESP32 original (WROOM-32). No S3 não há essa limitação.

Strapping pins que merecem atenção: GPIO 0 (boot mode), GPIO 45 e 46 (já em uso para BL e DC).

### GPIOs livres para expansão futura

| GPIO | Observação |
|------|-----------|
| 4 | Livre |
| 5 | Livre (strapping — cuidado em reset) |
| 6–9 | Livre |
| 21 | Livre — conector físico disponível |
| 38–42 | Livres |
| 47–48 | Livres |

---

## Skill 01 — Inicialização Correta do Hardware

**Contexto:** Todo firmware deve inicializar display e touch exatamente nesta ordem.

**Ordem obrigatória:**
1. `Serial.begin(115200)`
2. Relés OFF antes de qualquer outra coisa: `pinMode(HEATER_PIN, OUTPUT); digitalWrite(HEATER_PIN, RELAY_OFF);`
3. `pinMode(TFT_BL, OUTPUT); analogWrite(TFT_BL, 255)` ← usar `analogWrite`, não `digitalWrite`
4. `tftSPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS)`
5. `tft.begin(40000000)` ← 40 MHz
6. `tft.setRotation(1)` ← landscape 320×240
7. `tft.fillScreen(CLR_BG)`
8. `Wire.begin(TOUCH_SDA, TOUCH_SCL)`
9. Reset hardware do touch: `digitalWrite(TOUCH_RST, LOW); delay(10); digitalWrite(TOUCH_RST, HIGH); delay(100);`
10. `pinMode(TOUCH_INT, INPUT_PULLUP)`
11. `ts.begin(40)` ← threshold FT6206

**Declaração dos objetos:**
```cpp
SPIClass         tftSPI(HSPI);
Adafruit_ILI9341 tft(&tftSPI, TFT_DC, TFT_CS, TFT_RST);
Adafruit_FT6206  ts;
```

**Por que `analogWrite` no backlight?** O pino BL do ILI9341 nesta placa suporta controle de brilho por PWM. `digitalWrite(HIGH)` também funciona mas limita ao brilho máximo.

**Por que `tft.begin(40000000)` e não `tft.init()`?** O ILI9341 usa `begin(freq)` ao invés de `init(w, h)` da ST7789. Não é necessário `invertDisplay()` neste modelo.

---

## Skill 02 — Leitura de Touch (FT6336G I2C Capacitivo)

**Contexto:** O FT6336G é capacitivo, retorna coordenadas diretamente via I2C. Diferente do XPT2046 (SPI resistivo), não precisa de múltiplas amostras nem filtro manual.

**Mapeamento de coordenadas para `setRotation(1)` landscape:**
O chip reporta em portrait nativo (x: 0–240, y: 0–320). Para landscape:
- `tx = map(p.y, 0, 320, 0, SCREEN_W)`
- `ty = map(p.x, 0, 240, SCREEN_H, 0)`

**Padrão aprovado:**
```cpp
bool readTouch(int16_t &tx, int16_t &ty) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  if (p.x == 0 && p.y == 0) return false;
  tx = constrain(map(p.y, 0, 320, 0, SCREEN_W), 0, SCREEN_W - 1);
  ty = constrain(map(p.x, 0, 240, SCREEN_H, 0), 0, SCREEN_H - 1);
  return true;
}
```

**Debounce de tap:**
```cpp
bool detectTap(int16_t &tx, int16_t &ty) {
  bool pressed = readTouch(tx, ty);
  unsigned long now = millis();
  if (pressed && !lastTouchDown && (now - lastTouchTime > 300)) {
    lastTouchDown = true; lastTouchTime = now; return true;
  }
  if (!pressed) lastTouchDown = false;
  return false;
}
```

**Bibliotecas necessárias:**
```cpp
#include <Wire.h>
#include <Adafruit_FT6206.h>
```

---

## Skill 03 — State Machine de UI

**Contexto:** Toda navegação de telas usa state machine com enum + funções `drawXXX()` / `loopXXX()`.

```cpp
enum AppState { STATE_A, STATE_B, ... };
AppState currentState = STATE_A;

void drawA() { /* renderiza tela A uma vez */ }
void loopA() { /* lida com input enquanto em A */ }

void loop() {
  switch (currentState) {
    case STATE_A: loopA(); break;
    case STATE_B: loopB(); break;
  }
}
```

**Regras:**
- `drawXXX()` é chamada UMA vez na transição de estado (limpa a tela e renderiza)
- `loopXXX()` é chamada continuamente — só lida com input e atualizações dinâmicas
- Nunca chamar `tft.fillScreen()` dentro de `loopXXX()` (causa flickering)

---

## Skill 04 — Rendering sem Flickering

**Técnica base — apagar só a área do valor:**
```cpp
tft.fillRect(x, y, largura, altura, CLR_BG);
tft.setCursor(x, y);
tft.print(novoValor);
```

**Técnica avançada — texto com fundo (sem fillRect):**
```cpp
tft.setTextColor(CLR_WHITE, CLR_BG);   // 2º parâmetro = cor de fundo
tft.setCursor(x, y);
tft.print(valor);   // sobrescreve pixels de fundo junto com o texto
```
Usar quando o texto sempre tem o mesmo número de caracteres (ex: relógio HH:MM:SS). Elimina o flash preto entre apagar e redesenhar.

**Técnica de cache de estado — só redesenha quando muda:**
```cpp
// Variáveis de cache globais (ex: footer)
char  ftrTime[9] = "";
bool  footerDirty = true;   // setado em drawScreen() para forçar redesenho completo

void updateFooter() {
  char timeStr[9]; getTimeStr(timeStr);
  bool timeChanged = (strcmp(timeStr, ftrTime) != 0);
  bool fullRedraw  = footerDirty || wifiChanged || extChanged || usageChanged;
  if (!timeChanged && !fullRedraw) return;
  if (fullRedraw) { /* fillRect + redesenha tudo */ footerDirty = false; }
  // Relógio: sobrescreve no lugar (sem fillRect)
  tft.setTextColor(CLR_WHITE, 0x0841);
  tft.setCursor(6, FOOT_Y+5); tft.print(timeStr);
}
```

**Intervalos de atualização aprovados no loop:**
```cpp
if (now - lastSensDisp >= 500)  updateSensors();    // 2 Hz
if (now - lastStatDisp >= 1000) updateStatusBar();  // 1 Hz (era 250ms — causava flicker)
if (now - lastFootDisp >= 1000) updateFooter();     // 1 Hz com cache interno
```

---

## Skill 05 — Controle de Tempo sem `delay()`

```cpp
unsigned long lastUpdate = 0;
const unsigned long INTERVAL = 1000;

void loop() {
  unsigned long now = millis();
  if (now - lastUpdate >= INTERVAL) {
    lastUpdate = now;
    // executa tarefa periódica
  }
}
```

`delay()` só é aceitável na inicialização (antes do loop) ou em animações de splash screen.

---

## Skill 06 — Paleta de Cores RGB565

```cpp
#define CLR_BG       0x0000   // preto
#define CLR_WHITE    0xFFFF   // branco
#define CLR_RED      0xF800
#define CLR_GREEN    0x07E0
#define CLR_BLUE     0x001F
#define CLR_YELLOW   0xFFE0
#define CLR_CYAN     0x07FF
#define CLR_ORANGE   0xFB60
#define CLR_GOLD     0xFEA0   // dourado Fikra
#define CLR_GRAY     0x7BEF
#define CLR_DARKGRAY 0x2104
```

Nunca usar valores hex soltos no código — sempre usar constante `CLR_`.
Conversão dinâmica: `tft.color565(R, G, B)` onde R, G, B ∈ [0, 255].

---

## Skill 07 — Leitura de Sensores

**DHT22 (um pino, temperatura + umidade):**
```cpp
#include <DHT.h>
DHT dht(DHT22_PIN, DHT22);   // DHT22_PIN = GPIO 2
dht.begin();
float temp = dht.readTemperature();
float hum  = dht.readHumidity();
if (isnan(temp) || isnan(hum)) { /* tratar falha */ }
```

**SHT31 via I2C (mais preciso, alternativa futura):**
```cpp
#include <Wire.h>
#include <Adafruit_SHT31.h>
Adafruit_SHT31 sht31;
Wire.begin(TOUCH_SDA, TOUCH_SCL);   // mesmos pinos do touch (barramento compartilhado)
sht31.begin(0x44);
float temp = sht31.readTemperature();
float hum  = sht31.readHumidity();
```

---

## Skill 08 — Controle de Relé (Bang-Bang com Histerese)

```cpp
#define RELAY_ON   LOW    // relés ativo LOW
#define RELAY_OFF  HIGH

void setHeater(bool on) {
  if (heaterOn == on) return;   // evita acionamento desnecessário
  heaterOn = on;
  digitalWrite(HEATER_PIN, on ? RELAY_ON : RELAY_OFF);
}
```

**Histerese obrigatória** — sem ela o relay cicla centenas de vezes por hora.

**Sequência de inicialização segura (no setup(), antes de tudo):**
```cpp
pinMode(HEATER_PIN, OUTPUT); digitalWrite(HEATER_PIN, RELAY_OFF);
pinMode(COOLER_PIN, OUTPUT); digitalWrite(COOLER_PIN, RELAY_OFF);
```

---

## Skill 09 — Persistência de Configurações (NVS / Preferences)

```cpp
#include <Preferences.h>
Preferences prefs;

// Salvar:
prefs.begin("estufa", false);
prefs.putFloat("dryTemp", dryTemp);
prefs.putUInt("totalMin", nvsTotalMin);
prefs.end();

// Carregar no setup():
prefs.begin("estufa", true);
dryTemp     = prefs.getFloat("dryTemp", 45.0f);
nvsTotalMin = prefs.getUInt("totalMin", 0);
prefs.end();
```

Não usar `EEPROM.h` — Preferences usa NVS nativa do ESP32/S3, mais robusta.

---

## Skill 10 — Controle por Histerese Dupla (umidade + temperatura)

```
Umidade (MODO ESTUFA):
  IDLE   → DRYING  : hum >= 40%
  DRYING → COOLING : hum <= 32%   ← histerese de 8%
  COOLING → IDLE   : após 5 min (COOLER_TAIL_MS)

Temperatura (segurança — sobrepõe umidade):
  tempSafety = true  : temp >= 50°C → heater OFF forçado
  tempSafety = false : temp <= 47°C → heater pode voltar
```

`heaterActual = heaterWanted && !tempSafety && (ctrlState == ST_DRYING)`

---

## Skill 11 — Proteção Anti-Falha de Sensor

```cpp
if (isnan(t) || isnan(h)) {
  sensorFails++;
  if (sensorFails >= SENSOR_FAIL_MAX) {   // 5 falhas consecutivas
    ctrlState = ST_EMERGENCY;
    emergencyShutdown();   // desliga TUDO imediatamente, sem cooler tail
  }
  return;
}
sensorFails = 0;
```

Temperatura NaN: mantém `tempSafety` no valor atual (conservador).
Emergência: só reset por power cycle. Mostra aviso vermelho na tela.

---

## Referência — ILI9341 vs ST7789 (diferenças relevantes)

| Aspecto | ST7789 (CYD antigo) | ILI9341 (S3 atual) |
|---------|--------------------|--------------------|
| Init | `tft.init(240, 320)` | `tft.begin(40000000)` |
| invertDisplay | `true` (obrigatório) | Não necessário |
| setRotation landscape | `3` | `1` |
| Biblioteca | `Adafruit_ST7789` | `Adafruit_ILI9341` |
| Backlight | `digitalWrite(HIGH)` | `analogWrite(255)` |

---

## Skill 12 — Cooler Tail: Regra Imutável do Cooler

**Contexto:** O cooler é um dissipador de calor do aquecedor. Desligá-lo junto com o aquecedor deixa calor residual perigoso no elemento. Esta skill descreve o padrão implementado e que **nunca deve ser revertido**.

**Regra:**
```
coolerOn = heaterOn || coolerTailActive
```

**Implementação aprovada (não alterar):**
```cpp
// Única função que toca o GPIO do cooler
void applyCooler() {
  bool desired = heaterOn || coolerTailActive;
  if (coolerOn == desired) return;
  coolerOn = desired;
  digitalWrite(COOLER_PIN, desired ? RELAY_ON : RELAY_OFF);
}

// Ao desligar o aquecedor, inicia o tail de 5 min automaticamente
void setHeater(bool on) {
  if (heaterOn == on) return;
  bool wasOn = heaterOn;
  heaterOn = on;
  digitalWrite(HEATER_PIN, on ? RELAY_ON : RELAY_OFF);
  saveHeaterState(on);          // persiste no NVS para sobreviver a reboot
  if (wasOn && !on) {
    coolerTailActive = true;
    coolerTailEnd    = millis() + COOLER_TAIL_MS;
  }
  applyCooler();
}

// Expira o tail — chamado no loop()
void updateCoolerTail() {
  if (!coolerTailActive || millis() < coolerTailEnd) return;
  coolerTailActive = false;
  applyCooler();
}
```

**O que NÃO fazer:**
- Nunca chamar `setCooler()` ou `digitalWrite(COOLER_PIN, ...)` fora de `applyCooler()` e `emergencyShutdown()`
- Nunca chamar `setCooler(false)` diretamente em `updateControl()` — isso bypassaria o tail

**Segurança de reinício:** `saveHeaterState(true/false)` salva no NVS a cada transição. Em `loadNVS()`, se `heaterWas = true`, o tail é iniciado imediatamente antes da tela aparecer.

**Por que isso importa:** O padrão anterior usava `setCooler(on)` dentro de `updateControl()`. Quando o tail expirava, `coolerTailActive` voltava a `false` e a próxima chamada `setCooler(false)` desligava o cooler imediatamente — exatamente o bug que esta skill resolve.

---

## Regras Gerais de Qualidade

1. **Sem `delay()` no loop principal** — usar `millis()` para tudo
2. **Sem magic numbers** — toda constante tem nome (`#define` ou `const`)
3. **Sem modificar pinagem sem avisar** — sempre perguntar antes
4. **Sem features não pedidas** — implementar exatamente o que foi solicitado
5. **Sem abstração prematura** — 3 linhas repetidas são melhores que uma função desnecessária
6. **Sem comentários que descrevem o que o código faz** — apenas comentários que explicam o *porquê*
7. **Relés sempre OFF na primeira linha do setup()** — segurança antes de qualquer outra inicialização
8. **No ESP32-S3 não há GPIOs input-only** — todos os pinos são bidirecionais
