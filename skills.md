# Skills & Padrões de Desenvolvimento — Estufa de Filamentos

> Guia de referência para desenvolvimento como engenheiro sênior de firmware ESP32.
> Consulte antes de implementar qualquer funcionalidade nova.

---

## Perfil de Desenvolvedor

**Papel:** Engenheiro sênior de firmware embarcado para ESP32
**Postura:** Código correto antes de código bonito. Sem abstração prematura. Sem features não pedidas.
**Regra de ouro de pinagem:** Nunca alterar GPIOs definidos sem avisar o desenvolvedor e aguardar confirmação explícita. Mesmo que a mudança pareça óbvia.

---

## Hardware Fixo — Não Alterar Sem Confirmação

**Board:** ESP32 CYD 2432S028 ("Cheap Yellow Display")
**Chip:** ESP32-WROOM-32 (Xtensa LX6 dual-core, 240 MHz, 520 KB SRAM, 4 MB Flash)

### GPIOs em uso — TRAVADOS

| Função | GPIO | Direção | Observação |
|--------|------|---------|-----------|
| TFT_MISO | 12 | INPUT | HSPI — não reatribuir |
| TFT_MOSI | 13 | OUTPUT | HSPI — não reatribuir |
| TFT_CLK | 14 | OUTPUT | HSPI — não reatribuir |
| TFT_CS | 15 | OUTPUT | HSPI — não reatribuir |
| TFT_DC | 2 | OUTPUT | HSPI — também é LED onboard |
| TFT_RST | -1 | — | Hardware pull para 3.3V, sem GPIO |
| TFT_BL | 21 | OUTPUT | Backlight (HIGH = ligado) |
| TOUCH_MOSI | 32 | OUTPUT | VSPI — não reatribuir |
| TOUCH_MISO | 39 | INPUT ONLY | VSPI — sem pull-up programável |
| TOUCH_CLK | 25 | OUTPUT | VSPI — não reatribuir |
| TOUCH_CS | 33 | OUTPUT | VSPI — não reatribuir |
| TOUCH_IRQ | 36 | INPUT ONLY | IRQ ativo LOW — sem pull-up programável |

### GPIOs input-only (34, 35, 36, 39) — nunca usar como OUTPUT
> Qualquer tentativa de `pinMode(36, OUTPUT)` ou `pinMode(39, OUTPUT)` é silenciosa mas não funciona — pode causar comportamento indefinido.

### GPIOs disponíveis para expansão (não ocupados pelo hardware base)

| GPIO | Observação |
|------|-----------|
| 4 | Livre |
| 5 | Livre (boot mode — cuidado em reset) |
| 16 | Livre |
| 17 | Livre |
| 18 | Livre |
| 19 | Livre |
| 22 | Livre (I2C SDA padrão) |
| 23 | Livre (I2C SCL padrão) |
| 26 | Livre |
| 27 | Livre |
| 34 | Input only — bom para sensores analógicos |
| 35 | Input only — bom para sensores analógicos |

> **Antes de alocar qualquer GPIO novo para sensor, aquecedor ou ventilador — informar ao desenvolvedor e aguardar aprovação.**

---

## Skill 01 — Inicialização Correta do Hardware

**Contexto:** Todo firmware novo deve inicializar display e touch exatamente nesta ordem e com estes parâmetros.

**Ordem obrigatória:**
1. `Serial.begin(115200)`
2. `pinMode(TFT_BL, OUTPUT)` + `digitalWrite(TFT_BL, HIGH)`
3. `hspi.begin(TFT_CLK, TFT_MISO, TFT_MOSI, TFT_CS)`
4. `tft.init(240, 320)` ← parâmetros são dimensões físicas do painel, não da tela rotacionada
5. `tft.invertDisplay(true)` ← **obrigatório** neste modelo
6. `tft.setRotation(3)` ← landscape 320×240
7. `tft.fillScreen(0x0000)`
8. `vspi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS)`
9. `pinMode(TOUCH_CS, OUTPUT)` + `digitalWrite(TOUCH_CS, HIGH)`
10. `pinMode(TOUCH_IRQ, INPUT)`

**Por que `invertDisplay(true)`?** O painel físico deste modelo tem polaridade de cor invertida no hardware. Omitir faz preto virar branco, vermelho virar ciano, etc.

**Por que `init(240, 320)` e não `(320, 240)`?** A biblioteca recebe dimensões nativas do painel (portrait físico). A rotação 3 é aplicada depois.

---

## Skill 02 — Leitura de Touch Robusta

**Contexto:** O XPT2046/TPM408 é ruidoso. Leitura única é instável. Usar sempre o padrão com 8 amostras + filtro de outlier.

**Padrão aprovado:**
- 8 amostras por leitura
- Insertion sort + descarte dos 2 menores e 2 maiores
- Média dos 4 valores centrais
- Verificação de IRQ antes E depois da leitura (detecta dedo levantado durante amostragem)
- Rejeição de valores fora da faixa 200–3900 (ruído elétrico)
- Mapeamento calibrado: raw 300–3750 → pixels 0–319 / 0–239
- `setRotation(3)`: ambos os eixos mapeados normalmente (sem inversão)
- SPISettings: 1 MHz, MSBFIRST, SPI_MODE0
- Comando `0x90` = canal Y do chip = eixo X na tela (landscape)
- Comando `0xD0` = canal X do chip = eixo Y na tela (landscape)

**Debounce:** mínimo 300 ms entre taps reconhecidos (`detectTap()`).

**Nunca usar a função de touch da Adafruit_GFX ou bibliotecas genéricas** — o barramento VSPI é dedicado e a calibração é específica deste modelo.

---

## Skill 03 — State Machine de UI

**Contexto:** Toda navegação de telas usa state machine com enum + funções `drawXXX()` / `loopXXX()`.

**Padrão:**
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
- `loopXXX()` é chamada continuamente no `loop()` — só lida com input e atualizações dinâmicas
- Transição de estado: `currentState = STATE_X; drawX();`
- Nunca chamar `tft.fillScreen()` dentro de `loopXXX()` (causa flickering)

---

## Skill 04 — Rendering sem Flickering

**Contexto:** Atualizar valores numéricos na tela sem redesenhar o fundo completo.

**Técnica — apagar só a área do valor:**
```cpp
// Apaga o retângulo do valor anterior
tft.fillRect(x, y, largura, altura, CLR_BG);
// Desenha o novo valor
tft.setCursor(x, y);
tft.print(novoValor);
```

**Para valores numéricos frequentes (temperatura, timer):**
- Definir posição e tamanho fixos no layout
- Usar `tft.fillRect()` somente na área do número, nunca em toda a tela
- Manter `lastValue` para só redesenhar quando o valor mudar

**Nunca usar** `tft.fillScreen()` no loop de atualização de sensores.

---

## Skill 05 — Controle de Tempo sem `delay()`

**Contexto:** `delay()` bloqueia o loop e congela o touch. Usar sempre comparação de `millis()`.

**Padrão:**
```cpp
unsigned long lastUpdate = 0;
const unsigned long INTERVAL = 1000; // ms

void loop() {
  unsigned long now = millis();
  if (now - lastUpdate >= INTERVAL) {
    lastUpdate = now;
    // executa tarefa periódica
  }
  // touch e UI continuam responsivos
}
```

**Regra:** `delay()` só é aceitável na inicialização (antes do loop principal) ou em animações deliberadas de splash screen.

---

## Skill 06 — Paleta de Cores RGB565

**Contexto:** Todas as cores devem usar constantes nomeadas com prefixo `CLR_`. Nunca usar valores hexadecimais soltos no código.

**Paleta base aprovada:**
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

**Conversão dinâmica:** `tft.color565(R, G, B)` onde R, G, B ∈ [0, 255]

---

## Skill 10 — Controle por Histerese Dupla (umidade + temperatura)

**Contexto:** Quando um sistema precisa de dois limites distintos para ligar e desligar (evitar oscilação rápida), e um segundo critério de segurança que sobrepõe o primeiro.

**Padrão implementado na estufa:**
```
Umidade:
  IDLE     → DRYING   : hum >= HUM_TRIGGER (40%)
  DRYING   → COOLING  : hum <= HUM_RESET   (32%)   ← histerese de 8%
  COOLING  → IDLE     : após COOLER_TAIL_MS (5 min)
  Não reinicia em IDLE até hum >= 40% novamente

Temperatura (sobrepõe umidade):
  tempSafety = true  : temp >= TEMP_CUTOFF (50°C)  — heater OFF forçado
  tempSafety = false : temp <= TEMP_RESUME (47°C)  — heater pode voltar
  3°C de histerese previne ciclos rápidos em 47-50°C
```

**Regra:** `heaterActual = heaterWanted && !tempSafety && (ctrlState == ST_DRYING)`

**Por que dois valores separados e não um único threshold:**
Se usasse apenas 36% para ligar e desligar, uma pequena oscilação de 35-37% faria o relay ciclar dezenas de vezes por hora, desgastando o hardware e causando ruído elétrico.

---

## Skill 11 — Proteção Anti-Falha de Sensor

**Contexto:** Se o DHT22 falha (retorna NaN), não sabemos a temperatura real. Não podemos manter o aquecedor ligado.

**Padrão:**
```cpp
if (isnan(t) || isnan(h)) {
  sensorFails++;
  if (sensorFails >= SENSOR_FAIL_MAX) {
    ctrlState = ST_EMERGENCY;
    emergencyShutdown();   // desliga TUDO
  }
  return;   // não atualiza curTemp/curHum
}
sensorFails = 0;   // reset no sucesso
```

**Regra para temperatura NaN:** mantém `tempSafety` no valor atual (não reseta para false). Conservador por omissão — se não sabe, não deixa o heater voltar.

**Emergência:** uma vez em `ST_EMERGENCY`, só power cycle reseta. Mostra aviso vermelho na tela.

---

## Skill 07 — Leitura de Sensores I2C

**Contexto:** Sensores de temperatura/umidade como SHT31 ou HTU21D usam I2C. Os pinos padrão do ESP32 para I2C são SDA=22, SCL=23 — mas precisam de confirmação antes de usar.

**Padrão para SHT31 (mais preciso, recomendado):**
```cpp
#include <Wire.h>
#include <Adafruit_SHT31.h>

Adafruit_SHT31 sht31;

// No setup():
Wire.begin(SDA_PIN, SCL_PIN);
sht31.begin(0x44);   // endereço padrão

// No loop() (com millis, nunca blocking):
float temp = sht31.readTemperature();
float hum  = sht31.readHumidity();
if (!isnan(temp) && !isnan(hum)) {
  // atualiza display
}
```

**Padrão para DHT22 (mais simples, um pino):**
```cpp
#include <DHT.h>
DHT dht(SENSOR_PIN, DHT22);
dht.begin();
float temp = dht.readTemperature();
float hum  = dht.readHumidity();
```

> **ATENÇÃO:** Pinos SDA/SCL não estão definidos ainda. Informar ao desenvolvedor antes de alocar.

---

## Skill 08 — Controle de Aquecedor (Bang-Bang)

**Contexto:** Controle on/off simples para aquecedor por relay ou MOSFET. Adequado para estufas com inércia térmica alta.

**Padrão com histerese:**
```cpp
const float HYSTERESIS = 1.5; // °C acima/abaixo do alvo

void updateHeater(float currentTemp, float targetTemp) {
  if (currentTemp < targetTemp - HYSTERESIS) {
    digitalWrite(HEATER_PIN, HIGH);  // liga
    heaterOn = true;
  } else if (currentTemp > targetTemp + HYSTERESIS) {
    digitalWrite(HEATER_PIN, LOW);   // desliga
    heaterOn = false;
  }
  // entre os limiares: mantém estado atual (histerese)
}
```

**Histerese obrigatória** — sem ela o relay liga/desliga centenas de vezes por minuto (degrada o relay e causa ruído elétrico).

> **ATENÇÃO:** Pino do aquecedor (`HEATER_PIN`) não está definido. Informar ao desenvolvedor qual GPIO usar antes de implementar.

---

## Skill 09 — Persistência de Configurações (NVS / Preferences)

**Contexto:** Salvar temperatura alvo, perfil selecionado e configurações que devem sobreviver ao reset.

**Padrão com biblioteca Preferences (ESP32 nativa):**
```cpp
#include <Preferences.h>
Preferences prefs;

// Salvar:
prefs.begin("estufa", false);     // namespace "estufa", modo leitura/escrita
prefs.putFloat("targetTemp", targetTemp);
prefs.putInt("profileIdx", profileIdx);
prefs.end();

// Carregar no setup():
prefs.begin("estufa", true);      // modo somente leitura
targetTemp = prefs.getFloat("targetTemp", 65.0);   // 65.0 = default
profileIdx = prefs.getInt("profileIdx", 0);
prefs.end();
```

**Não usar EEPROM.h** — Preferences usa NVS (Non-Volatile Storage) nativa do ESP32, mais robusta e sem risco de wear em escrita frequente.

---

## Regras Gerais de Qualidade

1. **Sem `delay()` no loop principal** — usar `millis()` para tudo
2. **Sem magic numbers** — toda constante tem nome (`#define` ou `const`)
3. **Sem modificar pinagem sem avisar** — sempre perguntar antes
4. **Sem features não pedidas** — implementar exatamente o que foi solicitado
5. **Sem abstração prematura** — 3 linhas repetidas são melhores que uma função desnecessária
6. **Sem comentários que descrevem o que o código faz** — apenas comentários que explicam o *porquê*
7. **Testar no hardware real** — lógica que funciona no simulador pode falhar no ESP32 (timing, IRQ, SPI)
8. **GPIOs input-only (34, 35, 36, 39)** — nunca declarar como OUTPUT
