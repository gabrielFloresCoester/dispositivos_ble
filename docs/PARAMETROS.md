# Parâmetros do papel de atuador (port do `paramDado` do fwBLE)

Port do mapa de parâmetros do firmware da linha BLE (`fwBLE` /
`ControleCoesterBLE`, `BLE/ParamZarI.{c,h}`) para o `nrf54-app`. É o
"passo 8" do roteiro de `COMANDO_CONTROLE.md`. Substitui o antigo
`actuator_calib` (característica GATT própria com 5 campos).

**Confiabilidade deste documento**: escrito por uma sessão do Claude a
partir do código-fonte real (fwBLE e deste repositório), sem conferência
campo a campo por um humano. Os offsets foram conferidos contra as âncoras
`OFFSET_BITS_*` do próprio `ParamZarI.h`, mas trate como pista, não como
verdade definitiva, se for base de uma decisão importante.

## Decisão

**Tal e qual o fwBLE** (pedido do Felipe, 2026-10-01). Revisa a decisão de
2026-08-21 registrada no antigo `actuator_calib.h`, que preferia uma
característica própria a replicar o struct.

- **Endereço real**: área Painel do `ifFerConfig`,
  `MSG_EX_ADDRESS_CONFIG_PANEL = 0x00800800`. Um Gateway ou interface que
  leia ou escreva o `paramDado` de um Atuador BLE legado faz o mesmo aqui.
- **Layout byte a byte**: `tParamDado` com `#pragma pack(2)` do IAR,
  compilado **sem** `POS_ABS`/`CTL_NWP` (como o produto: `POS_ABS` só
  existe com `REM_IHM`, que não é definido). **144 bytes.**
- **Sem bitfields C** na imagem: GCC e IAR alocam bitfields de forma
  diferente. Cada campo é offset + máscara/deslocamento, acesso por byte.

## Divergências deliberadas do fwBLE

| fwBLE | SIM Connect | Por quê |
|---|---|---|
| Escrita BLE no `paramDado` é `memcpy` cru, sem checar faixa | Cada campo tocado é validado contra `paramMin`/`paramMax`. Se algum estiver fora da faixa, responde `GTM_NEG` e **nada muda** | Um limite inválido chegaria direto ao controle |
| `IFCC_SAVE`/`IFCC_RESTORE` fora de `accessCmdBLE` (a BLE não grava, só IHM/fieldbus) | Aceitos pela BLE | A interface BLE é o meio de configurar; salvar explícito, um botão por campo |
| `paramCorrigeLimites()` só na IHM (`paramModSentAntHor`) | Em qualquer escrita que mude o bit `antiHorario` | Equivalente funcional, evita recalibrar |
| Persistência em EEPROM I2C com checksum (`save.c`) | ZMS via settings, chave `aprm/v1` (blob de 144 B) | Hardware novo |
| `AL_SEM_PROGRAMA` / `AL_COM_EEPROM` (`paramTrat()`) | Não portados | Alarmes ainda não existem no conjunto reduzido |

O bloco de fábrica (primeiros 34 bytes) é **gravável** pela BLE, como no
`accessWriteMemBLE` do fwBLE (decisão do Felipe, 2026-10-01).

## Mapa (offset → campo)

| Off | Tipo | Campo | Faixa (`paramMin`–`paramMax`) | Default |
|---|---|---|---|---|
| 0 | u8[16] | `model` | — | `" FALHA TIPO    "` |
| 16 | u8[8] | `ns` | — | `"00000  "` |
| 24 | u16 | `fabFatTorqAber` (×0,001) | 1–30000 | 1000 |
| 26 | u16 | `fabFatTorqFech` | 1–30000 | 1000 |
| 28 | u16 | `fabTorqMax` (Nm) | 1–1400 | 1400 |
| 30 | u16 | `fabTorqMin` (Nm) | 1–1400 | 1 |
| 32 | u8 | `fabGanSenTorq` | 0–3 | 0 |
| 33 | u8 | `fabLeitPosiInver` | 0–1 | 0 |
| 34 | bits | b0 `fabPront`, b1 `Ok`, b2 `antiHorario`, b3 `inibCmdLoc`, b4 `deslTrqIncr`, b5 `trqFechad`, b6 `alAtQtLoc` | — | só `alAtQtLoc`=1 |
| 35 | u8 | `idioma` (0 port, 1 esp, 2 ingl) | 0–2 | 0 |
| 36 | u16 | `limiteSuper` (AD) | 50–2001 | 1800 |
| 38 | u16 | `limiteInfer` (AD) | 50–2001 | 200 |
| 40 | u16 | `torqueNmInc` | **`fabTorqMin`–`fabTorqMax`** | 80 |
| 42 | u16 | `torqueNmDec` | **`fabTorqMin`–`fabTorqMax`** | 80 |
| 44 | i16 | `torqueZero` (AD) | — (calibrado em campo) | 0 |
| 46 | u8 | `sobrTorqPart` | 5–200 | 40 |
| 47 | u8 | `limiteMargem` (‰) | 2–100 | 20 |
| 48 | u8 | `faixaParado` (‰) | 1–100 | 20 |
| 49 | u8 | `anteciparParada` (‰) | 0–100 | 2 |
| 50 | bits | b0 `ledVermAbre`, b1 `ledLjAlarme` | — | 0 |
| 51 | u8[4] | `senha` (dígitos) | 0–9 cada | 0000 |
| 55 | u8[16] | `tag` | — | `" TAG ATUADOR ?"` |
| 71 | — | *padding* | | |
| 72 | u16 bits | `sDispoHabi`: b0 `iOA`, b1 `iOD`, b2 `rede` | — | 0 |
| 74 | u8 | `ESDAc` | 0–4 | 2 |
| 75 | — | *padding* | | |
| 76 | u16 | `ESDPosic` (%) | 0–100 | 0 |
| 78 | bits | b0 `ESDDSAq`, b1 `ESDDFFa`, b2 `ESDDTrq`, b3 `PSTI`, b4 `numPartMaxLig`, b5 `ampTempLig` | — | b0..b2 = 1 |
| 79 | u8 | `PSTM` | 0–2 | 0 |
| 80 | u8 | `PSTP` (%) | 3–90 | 10 |
| 81 | u8 | `SinSaid6IODig` | 0–43 | 9 |
| 82 | u16 | `PSTT` (s) | 3–3000 | 30 |
| 84 | i16 | `tempoOperar` (s) | 0–3600 | 0 |
| 86 | i16 | `numPartMax` | 1–1200 | 300 |
| 88 | u16 | `ampTempAT1Par`:9 \| `ampTempAT1Mov`:7 | 0–500 / 1–120 | 10 / 4 |
| 90 | u16 | `ampTempAT3Par`:9 \| `ampTempAT3Mov`:7 | 0–500 / 1–120 | 10 / 4 |
| 92 | bits | b0..2 `abriFechaIODig` (0–3), b3 `retentIODig` | | 0 / 1 |
| 93–97 | u8 | `SinSaid1..5IODig` | 0–44 | 1, 2, 3, 4, 18 |
| 98 | u8 | `bSinEntrBaiIOD` (bitmap) | — | 1 |
| 99 | bits | b0..6 `posicLacoAbertIOA` (0–100), b7 `pararLacoAbertIOA` | | 0 / 1 |
| 100 | u8 | `posicRedeFalhaCom` | 0–100 | 0 |
| 101 | u8 | `tempoEspera` (×100 ms) | 1–250 | 100 |
| 102 | u8 | `acaoRedeFalhaCom` | 0–4 | 0 |
| 103 | u8 | `enderRede` | 1–247 | 247 |
| 104 | u8 | `baudRateRede` (índice 300…115200) | 0–11 | 5 |
| 105 | bits | b0..2 `bitFormatRede` (0–4), b3..7 `mapMem` (0–1) | | 0 |
| 106 | bits | b0..3 `comanPrimar` (0–2), b4..7 `modoContr` (0–1) | | 0 |
| 107 | u8 | `ampTempPosX` (%) | 0–95 | 0 |
| 108 | u8 | `ampTempPosY` (%) | 0–95 | 0 |
| 109 | — | *padding* | | |
| 110 | u16 | `ampTempAT2Par`:9 \| `ampTempAT2Mov`:7 | 0–500 / 1–120 | 10 / 4 |
| 112 | u16 | `ampTempAT4Par`:9 \| `ampTempAT4Mov`:7 | 0–500 / 1–120 | 10 / 4 |
| 114 | u16[15] | `spare_nova` | — | 0 |

## Conferência com `mapa_memoria_struct.xlsx` (2026-10-01)

Planilha da equipe (`X:\P&D\Projetos\Analise e Testes\Standard+ (Novo)\Doc
pro SENAI\mapa_memoria_struct.xlsx`). Os rótulos da interface passaram a
usar a coluna CONFIG dela.

- **Offsets: batem todos**, incluindo os 3 paddings (0x47, 0x4B, 0x6D).
- **Ordem dos bits: DIVERGE, em aberto.** A planilha põe o primeiro campo
  declarado nos bits **altos** (ex.: `ampTempAT1Par` nos bits 15..7,
  padrão 1284 = `10<<7 | 4`; `retentIODig` no bit 0). Este port segue o
  padrão documentado do IAR para ARM: o primeiro campo vai nos bits
  **baixos** (`ampTempAT1Par` nos bits 8..0, padrão `0x080A`;
  `retentIODig` no bit 3). O fwBLE não tem `#pragma bitfields=reversed`
  nem opção de compilador para isso. Mas a planilha também não é
  autoritativa: o padrão que ela dá para 0x22 (0) ignora `alAtQtLoc`=1,
  e a máxima de `sDispoHabi` (8) não fecha com 3 bits. **Tira-teima:**
  ler o `paramDado` de um Atuador BLE real em padrão de fábrica. Byte 0x5C
  = `0x08` confirma este port; `0x01` confirma a planilha.
- Faixas em que a planilha usa a tabela `paramMin`/`paramMax` e o port usa
  outra regra: `torqueNmInc`/`Dec` (planilha 10–600; port
  `fabTorqMin`–`fabTorqMax`, regra da IHM `paramModTrqAbrNm`, e
  `paramMax.torqueNm*` não é usado em lugar nenhum do fwBLE);
  `torqueZero` (planilha 0–6000; port sem checagem, campo calibrado).
- Faixas em que a planilha diverge do `ParamZarI.c`: `PSTP` mín. 1 (código
  3) e `SinSaid*` máx. 45 (código 44/43). Vale o código.
- **Senha**: na interface é um campo único de 4 dígitos, no grupo travado
  "Fábrica / Segurança" (só troca quem confirmou a senha atual no
  desbloqueio), mascarado e nunca preenchido com a senha atual. No fio
  continua `senha[4]`, um dígito (0–9) por byte, como no fwBLE.

## Tempo de reversão do motor (campo herdado da FSA) — `0x00801010`

Validado em bancada em 2026-10-06: o padrão de 3000 ms, o ajuste pela
interface e a persistência após reinício.

No fwBLE, o tempo morto na reversão do motor não fica no `paramDado`:
ele é o `tempoReverContat` da placa FSA (`proCo/areaFSA.h`; firmware da
FSA em `ControleCoesterMSP430/FSA/Aplic_FSA.c`). O SIM Connect não tem
FSA, então o valor mora em `actuator_fsa_cfg.c` e quem o aplica é o
`actuator_motor.c`.

| | Valor | Origem |
|---|---|---|
| Endereço | `0x00801010` (u16 LE, ms) | área FSA `0x00801000` + `TAM_INFO` (16) |
| Padrão | 3000 ms | `Aplic_FSA.c` e configurador (`ConfigDefault.xml`) |
| Faixa | 100–10000 ms | `MIN_TEMPO_REVER`/`MAX_TEMPO_REVER` |
| Passo | 100 ms | IHM (`foSeAcModTempRever`); a interface também exige |

- **Só esse campo da área FSA é emulado.** O resto da área (falhas,
  alarmes, status, entradas e saídas da placa) descreve hardware que não
  existe aqui e responde `GTM_NEG`.
- **Divergência:** a escrita valida a faixa (fora dela, `GTM_NEG`) e
  grava na hora, em ZMS com a chave `afsa/v1`, sem `IFCC_SAVE`. No
  fwBLE a escrita BLE é crua, e a FSA só grava com o "salvar" da IHM.
- **Interface:** grupo "Acionamento do motor" no card de parâmetros.
- Vale a partir da próxima reversão: o motor lê o valor a cada uso.

## Área Comando — `0x00800000`

`ifFerConfigCmd_t`, 74 bytes (`cmdStatus` u16 em +0, `frame` em +2..+9 com o
comando u16 em +2, `buffer[64]` em +10). Escrever o comando em `0x00800002`:

| Código | Comando | Efeito aqui |
|---|---|---|
| `0x0000` | `IFCC_WAIT` | nada |
| `0x0002` | `IFCC_SAVE` | `paramSalva()`: marca `Ok`=1 e grava a imagem (assíncrono) |
| `0x0004` | `IFCC_RESTORE` | `paramRestau()`: descarta o que não foi gravado e recarrega da memória (ou defaults) |
| outro | — | volta para `IFCC_WAIT`, `cmdStatus` = `IFCCS_FAIL`, `GTM_NEG` |

## Onde cada parâmetro já atua

| Parâmetro | Consumidor | Comportamento (igual ao fwBLE) |
|---|---|---|
| `limiteSuper`/`limiteInfer` | `actuator_sensors.c` | AD → posição por mil (`sptPosMil`) |
| `torqueZero` | `actuator_sensors.c` | AD de torque zerado |
| `fabFatTorqAber`/`Fech` | `actuator_sensors.c` | Fator Nm: abertura ao abrir, fechamento ao fechar, parado mantém o último (`ultFator`) |
| `fabLeitPosiInver`, `antiHorario` | `actuator_sensors.c` | `sptTratInvertAd()`: complementa o AD (`0x7FF - ad`), um anula o outro |
| `antiHorario` | `actuator_motor.c` | `fsaIncr`/`fsaDecr`: troca os pinos abre/fecha |
| `limiteMargem`/`faixaParado`/`anteciparParada` | `actuator_control.c` | Lidos a cada ciclo (antes `#define`) |
| `inibCmdLoc` | `actuator_mode.c` + `actuator_panel.c` | `LOCAL_INIBIDO` ao entrar em LOCAL; o Painel BLE não despacha comandos |
| `alAtQtLoc` | `actuator_mode.c` | Ao **entrar** em PARA, quita alarmes locais |
| `torqueNmInc`/`Dec`, `sobrTorqPart` | `actuator_torque.c` (célula de carga) | Sobretorque em Nm por sentido, margem de partida e assentamento por Nm; a barra de torque da interface também usa |
| `deslTrqIncr`, `trqFechad` | `actuator_torque.c` (as duas variantes) | Ignorar torque ao abrir; fechamento com torque (assentamento no limite fechado) |

**Guardados e expostos, sem consumidor ainda**: ESD/PST (passo 6), amplia
tempo, tempo de operação e partidas/hora, E/S digital e analógica (ver
branch `integra-ios-digitais`), rede/Modbus (passo 9), LEDs, idioma, senha,
tag, `fabGanSenTorq`.

## Desloca zero do torque

No fwBLE só a IHM faz isso: `paramModTrqZero()` grava `torqueZero =
sptAdTrq()`, a leitura A/D atual. Aqui a interface lê `ad_torque` na área
Sensor (`0x00804080`, bytes 4–5, sem o zero subtraído) e escreve esse valor
em `torqueZero` (offset 44), seguido de `IFCC_SAVE`. Mesmo efeito, sem
comando inventado.

## Migração

Na primeira inicialização com este firmware, se existir a chave antiga
`acal/v1` (do `actuator_calib`) e ainda não existir `aprm/v1`, os 5 campos
(`limiteSuper`..`torqueZero`) são copiados para o `paramDado`, gravados, e
a chave antiga é apagada. Os 10 bytes antigos têm a mesma ordem dos
offsets 36–45.

## Interface (`nrf54-app/index.html`)

Card **Parâmetros (paramDado)**, ocupando a largura toda do painel "Este SIM
Connect". Os grupos seguem os menus da IHM do fwBLE (Posição, Torque,
Alarme, Gerais, ESD, PST, Amplia tempo, E/S digital, E/S analógica, Rede,
Fábrica), com os rótulos de `lang_port.h`.

- Lê os 144 bytes em 2 blocos de 72, que cabem num notify com MTU ≥ 83.
- Cada **salvar** faz `GTM_SEND` só dos bytes do campo (bits preservam os
  vizinhos do mesmo byte), depois `IFCC_SAVE`, depois relê tudo.
- **Reler** e **Restaurar gravado** (`IFCC_RESTORE`).
- A característica Calib antiga (`152d`/`152e`) foi removida do firmware e
  da interface.

## Como testar em bancada

1. Gravar o firmware sobre uma unidade que já tinha calibração no formato
   antigo e conectar: os limites/torque antigos aparecem no grupo Posição/
   Torque (migração). No RTT: `Calibracao antiga (acal/v1) migrada`.
2. Mudar *Lim. aberto* para 1700, salvar e reiniciar a placa: o valor
   persiste.
3. Mandar *Lim. aberto* = 3000 pelo console do navegador → toast de recusa
   (`GTM_NEG`); no RTT, `Parametro limiteSuper = 3000 fora da faixa`.
4. *Abertura (Nm)* acima de *Torque máx.* de fábrica → recusado.
5. *Anti-horário* = Sim: os limites são espelhados (`2047 - x`), a posição
   lida inverte e abrir passa a acionar P1.31.
6. *Inibe local* = Sim, depois Local: alarme "Local inibido" acende, e
   Abrir/Fechar/Parar da interface ficam inertes.
7. *Quitar local* = Sim, com um alarme quitável pendente, trocar para
   Parado: o alarme é quitado.
8. *Desloca zero* → "capturar atual": `torqueZero` passa a ser o AD lido, e
   o torque em Nm vai a ~0 sem carga.
9. Mudar um valor sem salvar não é possível pela interface (o salvar sempre
   grava); para testar o restaurar, escrever pelo console só o
   `GTM_SEND` e depois clicar **Restaurar gravado**.
