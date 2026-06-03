# Contexto do Projeto — Estufa de Filamentos

> **Como usar este arquivo:** Sempre que iniciar uma nova sessão no Claude relacionada a este projeto, instrua o Claude a ler este arquivo:
> ```
> Leia c:\Users\Fernando\Documents\Arduino\estufa\contexto.md e depois c:\Users\Fernando\Documents\Arduino\estufa\skills.md e esteja pronto para continuar o desenvolvimento do firmware da estufa de filamentos.
> ```

---

## Visão Geral do Projeto

**Projeto:** Firmware para Estufa de Secagem de Filamentos para Impressão 3D
**Hardware:** ESP32-S3 2.8" Capacitive Touch (LCDWIKI ES3C28P / ES3N28P)
**IDE:** Arduino IDE (arquivo `.ino`)
**Board setting:** `ESP32S3 Dev Module` | Partition: `Huge APP (3MB No OTA)` | USB Mode: `Hardware CDC and JTAG`
**Repositório local:** `c:\Users\Fernando\Documents\Arduino\estufa\`
**Repositório GitHub:** `https://github.com/fernandolfla/estufa`
**Autor:** Fikra Creative Studio (`fabio@fikra.com.br`)

---

## Hardware Alvo

| Item | Detalhe |
|------|---------|
| Microcontrolador | ESP32-S3 (Xtensa LX7 dual-core, 240 MHz, 512 KB SRAM, 8 MB Flash) |
| Board name | LCDWIKI ES3C28P / ES3N28P |
| Tela | 2.8" TFT ILI9341, 320×240 px, SPI |
| Touch | FT6336G, I2C capacitivo |
| Sensor | DHT22 (temperatura + umidade) |
| Relé Aquecedor | Ativo LOW — GPIO 3 |
| Relé Cooler/Fan | Ativo LOW — GPIO 14 |

---

## Arquivos do Projeto

| Arquivo | Descrição |
|---------|-----------|
| `estufa.ino` | Firmware principal (Arduino IDE) |
| `contexto.md` | Este arquivo — contexto completo do projeto |
| `skills.md` | Skills, padrões e técnicas aprovadas para este firmware |

---

## GPIOs em uso

| Função | GPIO | Direção | Observação |
|--------|------|---------|-----------|
| TFT_SCK | 12 | OUTPUT | SPI clock display |
| TFT_MOSI | 11 | OUTPUT | SPI dados display |
| TFT_MISO | 13 | INPUT | SPI leitura display |
| TFT_CS | 10 | OUTPUT | Chip select display |
| TFT_DC | 46 | OUTPUT | Data/Command select |
| TFT_RST | -1 | — | Sem GPIO (pull para 3.3V) |
| TFT_BL | 45 | OUTPUT | Backlight (`analogWrite`) |
| TOUCH_SDA | 16 | I/O | I2C dados touch |
| TOUCH_SCL | 15 | OUTPUT | I2C clock touch |
| TOUCH_INT | 17 | INPUT | Interrupt touch (INPUT_PULLUP) |
| TOUCH_RST | 18 | OUTPUT | Reset touch |
| **DHT22** | **2** | **I/O** | Sensor temperatura/umidade |
| **Aquecedor** | **3** | **OUTPUT** | Relé ativo LOW |
| **Cooler/Fan** | **14** | **OUTPUT** | Relé ativo LOW |
| Spare | 21 | — | Conector P3/CN1, livre |

> **No ESP32-S3 não existem GPIOs input-only.** Todos os GPIOs são bidirecionais (diferente do ESP32 original onde 34, 35, 36, 39 eram input-only).

---

## Estado Atual do Firmware (`estufa.ino`)

**Versão:** 1.3 — Cooler tail à prova de falhas + segurança de reinício

### Regra imutável do cooler (implementada v1.3)

```
coolerOn = heaterOn || coolerTailActive
```

O cooler **nunca** é controlado diretamente pela lógica de modo. Apenas `setHeater()` e `updateCoolerTail()` alteram o cooler via `applyCooler()`. Qualquer chamada `setCooler()` foi removida de `updateControl()`.

Isso garante que em **todos os cenários** — troca de modo, desativação, ciclagem de temperatura, reinicialização — o cooler sempre roda 5 minutos após o aquecedor desligar.

### Lógica de controle

**MODO ESTUFA (controle por umidade):**
- State machine: `ST_IDLE` → `ST_DRYING` (umid ≥ 40%) → `ST_COOLING` (umid ≤ 32%) → `ST_IDLE`
- Histerese de umidade: 40% para ligar, 32% para desligar (8% de margem)
- Segurança de temperatura: aquecedor OFF em ≥ 50°C, re-habilita em ≤ 47°C

**MODO SECAGEM (aquecimento contínuo até temperatura alvo):**
- Temperatura alvo ajustável 45–55°C (padrão 45°C, salvo em NVS)
- Histerese: OFF em ≥ alvo, re-habilita em ≤ alvo-3°C
- Botões +/− dentro do botão SECAGEM para ajuste fino

**Ambos os modos — cooler tail:**
- Aquecedor liga → cooler liga junto (`applyCooler()`)
- Aquecedor desliga → `coolerTailActive = true`, cooler fica ON por 5 min
- Tail expira → `applyCooler()` desliga cooler (somente se aquecedor também off)
- Troca de modo (ex.: SECAGEM→ESTUFA): tail preservado automaticamente
- `heaterWas` salvo no NVS a cada transição do aquecedor
- **Reinício com aquecedor ligado:** `loadNVS()` detecta `heaterWas=true` e inicia tail de 5 min imediatamente no boot, antes de qualquer outra coisa
- Emergência (sensor falhou): shutdown imediato SEM tail, `heaterWas=false` gravado

**Ambos os modos — outros:**
- Emergência: 5 falhas consecutivas do sensor → `ST_EMERGENCY`, tudo OFF
- Relés inicializam OFF antes de qualquer outra lógica
- `dryTemp` persiste em NVS entre sessões

### UI

- Header fixo: "Escolha o modo de operacao"
- 2 botões grandes: **ESTUFA** (ciano) e **SECAGEM** (laranja)
- Display de temperatura e umidade com textSize=3
- Barra de status: ícones aquecedor/cooler + estado do ciclo + countdown tail
- Rodapé linha 1: hora (NTP) | local | temperatura externa (Open-Meteo)
- Rodapé linha 2: uso acumulado 24h e 30d (NVS)
- WiFi + NTP + Open-Meteo integrados

### Otimizações de rendering implementadas

- Footer com cache de estado: só faz `fillRect` completo quando WiFi/extTemp/uso mudam
- Relógio usa `setTextColor(fg, bg)` para sobrescrever no lugar, sem flash preto
- Status bar: intervalo de 1000ms (era 250ms), elimina flicker de 4Hz
- Cache de valores (`dspTemp`, `dspHum`, etc.) evita redesenho desnecessário

### Pendente / próximas features

- [ ] Calibração de toque: mapear coordenadas reais do FT6336G para o layout
- [ ] Perfis de secagem por tipo de filamento
- [ ] Gráfico histórico de temperatura/umidade
- [ ] Notificações/alertas via WiFi

---

## Funcionalidade Pretendida da Estufa

| Filamento | Temperatura | Tempo |
|-----------|------------|-------|
| PLA       | 45–50 °C   | 4–6 h |
| PETG      | 55–65 °C   | 4–6 h |
| ABS       | 60–70 °C   | 4–6 h |
| Nylon     | 70–80 °C   | 8–12 h |
| TPU       | 50–60 °C   | 4–8 h |
| ASA       | 60–70 °C   | 4–6 h |

---

## Convenções de Código

- Linguagem: C++ Arduino (`.ino`)
- Biblioteca de display: `Adafruit_ILI9341` + `Adafruit_GFX`
- Touch: `Adafruit_FT6206` (I2C capacitivo)
- Cores: RGB565 (16-bit), prefixo `CLR_`
- Pinos display: prefixo `TFT_`, touch: prefixo `TOUCH_`
- State machine com enum + funções `drawXXX()` / `loopXXX()`
- `setRotation(1)` para landscape 320×240 no ILI9341
- Sem `delay()` no loop principal — usar `millis()` para tudo
- Relés ativo LOW: `RELAY_ON = LOW`, `RELAY_OFF = HIGH`
