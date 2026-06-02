# Contexto do Projeto — Estufa de Filamentos

> **Como usar este arquivo:** Sempre que iniciar uma nova sessão no Claude relacionada a este projeto, cole o conteúdo deste arquivo no início da conversa (ou instrua o Claude a lê-lo com: _"leia o arquivo contexto.md em c:\Users\luizavelar\Desktop\luiz\estufa\ antes de qualquer coisa"_). Isso garante continuidade total do projeto entre sessões.
>
> **Comando rápido para nova sessão:**
> ```
> Leia c:\Users\luizavelar\Desktop\luiz\estufa\contexto.md e depois c:\Users\luizavelar\Desktop\luiz\estufa\ST7789.md e esteja pronto para continuar o desenvolvimento do firmware da estufa de filamentos.
> ```

---

## Visão Geral do Projeto

**Projeto:** Firmware para Estufa de Secagem de Filamentos para Impressão 3D
**Hardware:** ESP32 CYD 2432S028 (Cheap Yellow Display) com tela ST7789 2.8" e touch XPT2046/TPM408
**IDE:** Arduino IDE (arquivo `.ino`)
**Repositório local:** `c:\Users\luizavelar\Desktop\luiz\estufa\`
**Autor:** Fikra Creative Studio (`fabio@fikra.com.br`)

---

## Hardware Alvo

| Item | Detalhe |
|------|---------|
| Microcontrolador | ESP32 (Espressif ESP32-WROOM-32) |
| Board name | ESP32 CYD 2432S028 ("Cheap Yellow Display") |
| Tela | 2.8" TFT ST7789, 320×240 px, SPI/HSPI |
| Touch | XPT2046 / TPM408, SPI/VSPI, IRQ ativo LOW |
| Referência de pinagem | Ver `ST7789.md` |

---

## Arquivos do Projeto

| Arquivo | Descrição |
|---------|-----------|
| `estufa.ino` | Firmware principal (Arduino IDE) |
| `contexto.md` | Este arquivo — contexto completo do projeto |
| `skills.md` | Skills, padrões e técnicas aprovadas para este firmware |
| `ST7789.md` | Referência técnica de pinagem, inicialização e calibração do hardware |
| `exemplo.txt` | Firmware diagnóstico de referência da Fikra (não modificar) |

---

## Estado Atual do Firmware (`estufa.ino`)

**Versão:** 1.1 — Dois modos distintos de operação + controle de temperatura alvo

### GPIOs em uso (hardware completo)
| Função | GPIO |
|--------|------|
| Display MISO | 12 |
| Display MOSI | 13 |
| Display CLK | 14 |
| Display CS | 15 |
| Display DC | 2 |
| Display BL | 21 |
| Touch MOSI | 32 |
| Touch MISO | 39 |
| Touch CLK | 25 |
| Touch CS | 33 |
| Touch IRQ | 36 |
| **DHT22 sensor** | **4** |
| **Relé Aquecedor** | **26** |
| **Relé Cooler/Fan** | **27** |

### Lógica de controle implementada

**MODO ESTUFA (controle por umidade):**
- State machine: `ST_IDLE` → `ST_DRYING` (umid ≥ 40%) → `ST_COOLING` (umid ≤ 32%) → `ST_IDLE`
- Histerese: ciclo só inicia em ≥ 40%, só para em ≤ 32% (sem oscilação)
- Cooler tail: cooler fica ON por 5 min após o aquecedor desligar
- Segurança de temperatura: aquecedor OFF em ≥ 50°C, re-habilita em ≤ 47°C (histerese 3°C)

**MODO SECAGEM (aquecimento contínuo até temperatura alvo):**
- Aquecedor ON continuamente, sem controle de umidade
- Temperatura alvo ajustável 45–55°C (padrão 45°C, salvo em NVS)
- Histerese: OFF em ≥ alvo, re-habilita em ≤ alvo-3°C (3°C histerese)
- Cooler acompanha o aquecedor (ON/OFF juntos)
- Botões +/− aparecem dentro do botão SECAGEM ao selecionar o modo
- Ao aumentar alvo: reseta dryTempSafety para permitir re-aquecimento

**Ambos os modos:**
- Emergência: 5 falhas consecutivas do sensor → `ST_EMERGENCY`, tudo OFF
- Relés inicializam OFF antes de qualquer lógica (primeira instrução do setup)
- `dryTemp` persiste em NVS entre sessões

### UI implementada
- Header: "Escolha o modo de operacao"
- 2 botões grandes com ícones: **ESTUFA** (termômetro, cor cyan) e **SECAGEM** (sol, cor laranja)
- Botão selecionado fica destacado com cor + borda dupla + ponto verde
- Tap no botão ativo desativa o modo (toggle)
- **SECAGEM selecionado**: exibe controle de temperatura alvo dentro do botão
  - Ícone e label sobem para abrir espaço
  - Divisor interno + "Temp.alvo:"
  - Botões [−] e [+] ajustam 1°C por tap (range 45–55°C)
  - Valor atual exibido entre os botões
  - Ao mudar dryTemp: salva NVS imediatamente
- Display grande de temperatura (textSize=3) e umidade (textSize=3)
  - Branco: normal | Laranja: ≥ 45°C | Vermelho: ≥ 50°C
  - Cyan: umidade normal | Laranja: umidade alta (≥ 40%)
  - Triângulo de aviso quando temp ≥ 45°C
- Barra de status: indicadores ● Aquecedor e ● Cooler + estado do ciclo + tempo de resfriamento
- Rodapé linha 1: uptime (HH:MM:SS) | local placeholder | temp externa placeholder
- Rodapé linha 2: uso 24h e 30d (acumulado via NVS, refinamento aguarda API de tempo)

### Pendente / próximas features
- [ ] Integração com API de tempo (endpoint a receber) → hora real, local, temp externa
- [ ] Com NTP/API: rastreamento preciso de 24h e 30d (hoje usa uptime millis-based)
- [ ] Perfis de secagem por tipo de filamento (diferentes parâmetros por modo)
- [ ] Gráfico histórico de temperatura/umidade
- [ ] Notificações/alertas (WiFi)

---

## Funcionalidade Pretendida da Estufa

A estufa controla:
1. **Temperatura:** mantém faixa definida por perfil de filamento
2. **Umidade:** monitora e exibe umidade em tempo real
3. **Tempo:** ciclos com duração configurável por tipo de filamento
4. **UI touch:** toda interação via tela sensível ao toque

### Perfis de Secagem (referência, valores típicos)
| Filamento | Temperatura | Tempo |
|-----------|------------|-------|
| PLA       | 45–50 °C   | 4–6 h |
| PETG      | 55–65 °C   | 4–6 h |
| ABS       | 60–70 °C   | 4–6 h |
| Nylon     | 70–80 °C   | 8–12 h |
| TPU       | 50–60 °C   | 4–8 h |
| ASA       | 60–70 °C   | 4–6 h |

---

## Convenções de Código Adotadas

- Linguagem: C++ Arduino (`.ino`)
- Biblioteca de display: `Adafruit_ST7789` + `Adafruit_GFX`
- Cores: RGB565 (16-bit), prefixo `CLR_`
- Pinos: prefixo `TFT_` para display, `TOUCH_` para touch
- State machine com enum `AppState` e funções `drawXXX()` / `loopXXX()`
- Touch com 8 amostras + filtro outlier (mesmo padrão do `exemplo.txt`)
- `invertDisplay(true)` obrigatório neste modelo
- `setRotation(3)` para landscape

---

## Referências Técnicas

- Pinagem completa: ver `ST7789.md`
- Skills e padrões aprovados: ver `skills.md`
- Firmware diagnóstico de referência: `exemplo.txt`
