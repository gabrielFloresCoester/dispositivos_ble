# Comando e controle do papel de atuador (port do fwBLE)

Portagem da lógica de **comando e controle** do firmware da linha BLE
(`fwBLE` / `ControleCoesterBLE`, pasta `BLE/Atuador/`) para o
`nrf54-app`, dentro do papel de **atuador** descrito em
`DECISOES_PRODUTO.md`. Cobre abrir/fechar/parar, posicionamento, modo de
operação e o motor de alarmes.

**Confiabilidade deste documento**: como os demais desta pasta, foi
escrito por uma sessão do Claude a partir do código-fonte real (fwBLE e
deste repositório), mas não verificado campo a campo por um humano. Trate
como pista pra reconferir, não como verdade definitiva, se for base de
uma decisão importante. Onde há divergência deliberada do fwBLE, está
marcado com o motivo.

## Estratégia

Portar `atControle.c` / `atModo.c` / `at_alarm.c` (e mais tarde
`atESD.c` / `atPST.c`) **o mais perto possível de 1:1** — mesmas
máquinas de estado, mesmos nomes conceituais, mesmo enum de origem de
comando. Motivo: compatibilidade com Atuadores BLE reais na frota (ver
`PORTABILIDADE_BLE_LEGADO.md` e a decisão de "produto único"). O Driver
DC 2 (`ControleCoesterMSP430`) serviu só de referência pontual pros dois
pontos onde o hardware novo obriga a divergir.

**Duas divergências de fundo:**

1. **Motor por GPIO direto, não FSA.** O `foSeAc.c` do fwBLE monta uma
   mensagem proxiada por I2C pra uma placa FSA separada. O SIM Connect é
   placa única — o motor é acionado direto por 2 pinos do nRF54.
2. **RS485 / Modbus RTU é entrada futura.** Ainda não implementada, mas a
   arquitetura de modo já deixa lugar pra ela entrar como mais uma fonte
   de comando sem redesenho (passo 9).

## Roteiro e o que já foi feito

| Passo | Módulo | Commit | Status |
|---|---|---|---|
| 1 | `actuator_motor.{c,h}` — saída de motor (GPIO) | `32f2214` | ✅ bancada |
| 2 | `actuator_alarm.{c,h}` — motor de alarmes | `d841a4f` | ✅ build |
| 3 | `actuator_control.{c,h}` — controle de posição | `d841a4f` | ✅ bancada |
| 4 | `actuator_panel.{c,h}` + `actuator_service.c` — comando por BLE + interface | `aad3767` | ✅ bancada |
| 5 | `actuator_mode.{c,h}` — árbitro de modo | `5edbde3` | ✅ bancada |
| 6 | ESD / PST (`atESD.c` / `atPST.c`) | — | ⬜ fim da fila |
| 7 | Paridade UART 91↔54 pros comandos | — | ⬜ |
| 8 | `actuator_params` — parâmetros persistidos (`paramZarI.c`), ver `PARAMETROS.md` | `feature-param` | ✅ build |
| 9 | RS485 / Modbus RTU | — | ⬜ só desenho |

## Mapa dos módulos

| SIM Connect | Origem no fwBLE | O que faz |
|---|---|---|
| `actuator_motor.c` | `foSeAc.c` (`fsaIncr`/`fsaDecr`/`fsaParar`/`foSeAcMotorAcionado`) | Aciona os 2 GPIOs do motor. API pública **só booleana**. |
| `actuator_alarm.c` | `at_alarm.c` | Registro de alarmes: tabela `{ação, retenção}` + set/release/check/get_block_mov/clears_all/get_blocked. |
| `actuator_control.c` | `atControle.c` (`at_ctl_run`/`demand`/`stop`, `AtMovManual`, `AtMovInvertido`) | Máquina de posicionamento. Loop de 10 ms. |
| `actuator_panel.c` | `ifFerConfig.c` (`ifFerConfigRunCmd` + `ifFerConfigCmdRemPanelCtl`) | Consome a palavra de comando do Painel Remoto (BLE): lease, modo, abre/fecha/para/quita. |
| `actuator_mode.c` | `atModo.c` (`atModoAcao` e cia.) | Decide qual fonte comanda (LOCAL = BLE / REMOTO = rede) e trata transições de modo. |
| `actuator_service.c` (estendido) | `acgl.c` + áreas do `ifFerConfig` | Trata `GTM_SEND` na área Painel e expõe uma área sintética de status. |

## Fluxo

O loop de controle roda a cada 10 ms (`k_work_delayable`, no fwBLE é
`trataAplic()` a ~5 ms):

```
actuator_control_run()
 ├─ alarme de falha de com. do sensor de posição (position_sensor_is_online)
 ├─ [segurança] sem posição válida → força parada, sai
 ├─ actuator_mode_run()                    ← "de onde vem o comando"
 │   ├─ actuator_panel_run()               ← lease + campo de modo SEMPRE;
 │   │                                        abre/fecha/para/quita SÓ se
 │   │                                        (lease válido && modo == LOCAL)
 │   ├─ transição de modo → alarmes MODO_NAO_REMOTO / LOCAL_INIBIDO / PARADA_LOCAL
 │   └─ DESLIGA/PARA/PARAM/INFO → actuator_control_stop()
 ├─ máquina de estado: INCREMENTAR / DECREMENTAR / PARAR
 │   (banda morta, margens de limite, parada antecipada)
 ├─ atAlarme() → se um alarme bloqueia o sentido pedido, PARAR
 ├─ AtMovManual()    → volante → alarme OPER_MANUAL
 └─ AtMovInvertido() → posição contra o acionamento → alarme FAL_ACION_INV
```

Quando um alarme bloqueia o movimento, o motor de alarmes chama de volta
o controle via callback (`struct actuator_alarm_cb.stop_cb` →
`actuator_control_stop_on_alarm`), registrado em `main.c` —
equivale ao `at_ctl_stop(ACP_ALARME)` que o `al_reg_block_mov()` do fwBLE
faz direto. É callback (não `#include`) pelo mesmo motivo que
`actuator_client.c`/`my_lbs.c` usam callbacks: desacoplar as camadas.

## Detalhe por módulo

### `actuator_motor` — saída de motor

- **P1.30 = abre, P1.31 = fecha** (nó `zephyr,user` no overlay de board).
  Pinos **provisórios de bancada** — trocar os `psels` quando o
  esquemático do circuito de acionamento existir.
- Polaridade `GPIO_ACTIVE_HIGH` é chute — trocar pra `ACTIVE_LOW` no
  overlay se o driver real acionar em nível baixo.
- Interbloqueio lógico: nunca energiza abre e fecha ao mesmo tempo.
- API pública **só booleana** (`actuator_motor_acionado()`), fiel ao
  `foSeAcMotorAcionado()`. O sentido (abrindo/fechando/parado) é
  responsabilidade da camada de controle
  (`actuator_control_get_mov_stt()`); aqui é estado interno só pro banner
  de transição no RTT (`****** ABRINDO ******` etc.).
- **Tempo morto na reversão** (validado em bancada, 2026-10-06, inclusive
  o ajuste e a persistência do valor): depois de desligar num sentido, o oposto
  só é energizado passado esse tempo. **Configurável**, porque é o
  "Tempo de Reversão Acionamento" da placa FSA do fwBLE
  (`dadoFSA.tempoReverContat`): padrão 3000 ms, faixa 100–10000 ms, que
  a IHM ajusta em passos de 100 ms. Fica em `actuator_fsa_cfg.c`,
  exposto no endereço real da área FSA, `0x00801010`. Ver
  `PARAMETROS.md`. `abre()`/`fecha()` chamados dentro do tempo morto
  deixam o motor desligado e retornam; o laço de 10 ms tenta de novo
  sozinho. Religar no mesmo sentido não espera. **Sem** rampa de
  partida ainda: isso depende do circuito de acionamento real (o
  `aplicAcioMot()` do Driver DC 2 tem).

### `actuator_alarm` — motor de alarmes

O **algoritmo** foi portado fiel: tabela `alarm_ctl[]` com `{ação,
retenção}` por alarme, e `set`/`release`/`check`/`get_block_mov`/
`clears_all`/`get_blocked` genéricos.

**Escopo: 14 de ~60 alarmes** (os que `atControle`/`atModo` realmente
consultam, mais os de torque, ver "Proteção por torque" abaixo):

| Bit | Alarme | Ação | Gatilho | Ligado no passo |
|---|---|---|---|---|
| 0 | `ESD_EXT` | nenhuma | — | 6 |
| 1 | `PARADA_LOCAL` | nenhuma | modo REMOTO→PARA movendo | 5 |
| 2 | `MODO_NAO_REMOTO` | nenhuma | sair de REMOTO | 5 |
| 3 | `LOCAL_INIBIDO` | nenhuma | LOCAL + inibição | 5 (stub) |
| 4 | `FAL_ACION_INV` | para os 2 sentidos | posição contra o acionamento (>5%) | 3 |
| 5 | `OPER_MANUAL` | nenhuma | volante (>0.5% com motor parado) | 3 |
| 6 | `PST_EXE` | nenhuma | — | 6 |
| 7 | `PST_FAL` | nenhuma | — | 6 |
| 8 | `COM_SENS_POS` | para os 2 sentidos; some sozinho | `!position_sensor_is_online()` por 3 s (como o `timerSenPosOk` do fwBLE) | pós-4 |
| 9 | `TORQUE_AB` | para só abrir; sai ao fechar ou pelo volante | sobretorque abrindo | torque |
| 10 | `TORQUE_FC` | para só fechar; sai ao abrir ou pelo volante | sobretorque fechando | torque |
| 11 | `VALV_TRAVADA` | para os 2 sentidos | torque nos 2 sentidos | torque |
| 12 | `OPER_INCOMPLETA` | nenhuma | comando REMOTO abortado por torque | torque |
| 13 | `COM_SENS_TRQ` | para os 2 sentidos; some sozinho | nenhuma fonte de torque online por 3 s | torque |

`COM_SENS_POS` foi **anexado no fim** (bit 8) pra não deslocar os bits
0–7 (a interface usa a posição do bit). É `AR_NOT_RETAIN` — some sozinho
quando o sensor volta a responder; fica ativo do boot até a 1ª leitura
boa (esperado, o `comScanOnlineDev` do fwBLE também).

**Divergências:**
- `enum actuator_alarm_id` **NÃO** preserva a posição do `alarm_t` do
  fwBLE (9 valores vs ~60, mesma ordem relativa mas sem os buracos).
  Seguro enquanto nada expuser o número cru do alarme por um canal
  externo esperando bater com o fwBLE. **Reavaliar antes** de implementar
  a área real de alarmes (`0x00804880`) ou expor alarme por RS485.
- `clears_all()` só conta uma quitação **real** (info true→false), não
  "existe algum alarme inativo". Sem isso, o bit `ack_alarm` (que não é
  auto-limpo) dispararia `clears_all` + log a cada ciclo por 5 s. O fwBLE
  tem esse flood latente.
- `al_esd_jump()` (supressão de alarme durante ESD) → stub sempre false
  (ESD é passo 6).
- `atRegEvent()` (histórico de eventos) → portado em
  `actuator_regevent.c`, ver `docs/REGISTRO_EVENTOS.md`. O evento de
  alarme usa o número do `alarm_t` do fwBLE (coluna `fwble_id` da
  tabela), não a posição no nosso enum.
- **Não portado**: API de display cíclico (`at_alarm_get_next_view` etc.
  — não há painel), `get_critic_fail`/`get_pos_fail` (agregam alarmes que
  não existem no conjunto reduzido), `reset_check`/`resetaTudo` (gatilho
  de sensoriamento trifásico via FSA).

### `actuator_control` — controle de posição

Port de `atControle.c` quase 1:1:

- `actuator_control_demand(pos, origem)` — pede posicionamento (`pos` em
  0–1000 por mil; 0 = fechado, 1000 = aberto). `open`/`close` são macros.
- `actuator_control_stop(origem)`.
- `actuator_control_get_mov_stt/cmd_orig/stop_orig()`.
- `enum actuator_ctl_cmd_orig` — enum completo de origem mantido por
  fidelidade (`at_ctl_cmd_orig_t`), vários valores ainda sem uso.
- `enum actuator_mov_status` (mora em `actuator_alarm.h`, reusado):
  `L_SUPER`/`L_INFER`/`INCR`/`DECR`/`PARADO` — espelha `at_ctl_mov_stt_t`.

**Movimento encerra ao chegar no limite físico**: `em_posicao_inc/dec()`
retorna PARAR com origem `LIMITE` quando `posição ± ANTECIPAR_PARADA`
cruza 1000/0; `margens_limite()` impede re-partir depois. Comportamento
do fwBLE, portado como está.

- `AtMovManual()` → detecta movimento pelo volante (variação de posição
  com o motor parado, após ~3 s de inércia) → `OPER_MANUAL`. Também
  libera `FAL_ACION_INV` nesse caminho.
- `AtMovInvertido()` → detecta a posição se deslocando ao contrário do
  sentido acionado → `FAL_ACION_INV`.

**Divergências:**
- **Segurança** (não existe no fwBLE): sem posição válida
  (`ACTUATOR_AT_POSICAO_INDEF`), força parada e não roda a máquina — o
  atuador real assume o sensor sempre presente; na bancada o pot pode
  estar solto.
- `sptPosComplAt()` (posição sem clamp, usada pelo `AtMovInvertido`
  original) → mesmo `at_posicao` com clamp. Precisão um pouco menor bem
  nos extremos 0/1000.
- Assentamento por torque no fechamento (`sptTrqParaLimit()`): portado
  nas duas variantes de sensor, ver `actuator_torque` abaixo.
- Parâmetros `limiteMargem`/`faixaParado`/`anteciparParada`: lidos do
  `paramDado` a cada uso (passo 8, ver `PARAMETROS.md`).
- Timers `AlocTimer`/`GetTime` → deadlines com `k_uptime_get()`.
  `__no_init` → `static`.

### `actuator_torque` — proteção por torque (branch `feature-comandos-torque`)

Porta de `sptTratTrq()` + `sptTrqParaLimit()` (`sensPosTor.c`), nas duas
variantes de sensor do original. Usa a fonte que o `actuator_sensors.c`
escolheu no ciclo. Roda dentro do laço de controle de 10 ms, depois de
decidir o status do ciclo e antes de `atAlarme()`, então um sobretorque
para o motor no mesmo ciclo.

**Comum às duas variantes:**
- "Desliga Torque de Abertura" (`deslTrqIncr`): ignora o torque ao
  abrir.
- Sobretorque em modo REMOTO dá também `OPER_INCOMPLETA`.
- Parado: mexer no volante (`OPER_MANUAL`) libera os bloqueios de torque
  e a válvula travada.
- Falha do sensor (`COM_SENS_TRQ`): nenhuma fonte de torque online por
  3 s bloqueia os dois sentidos e some sozinha quando volta.

**Microchaves** (ramo `PAINEL_CQT`). Validado em bancada em 2026-10-05,
junto com o `COM_SENS_TRQ`:
- Contato ativo por mais de 3 ciclos: abrindo, `TORQUE_AB`; fechando,
  `TORQUE_FC`. Os limiares em ciclos do original (3 de filtro, +150 com
  "Fechamento com Torque") valem 1:1 nos 10 ms.
- "Fechamento com Torque" (`trqFechad`): fechando, só alarma depois de
  ~1,5 s de contato, e então para por torque (origem `TORQUE`).
- Os dois contatos juntos, ou um até 3 s depois do outro, dão
  `VALV_TRAVADA`.

**Célula de carga** (analógico). Portado e validado em bancada em
2026-10-06: sobretorque na abertura e no fechamento, válvula travada e
liberação pelo volante. O assentamento por Nm no limite fechado ainda
não foi exercitado em bancada:
- Torque em Nm (com o fator de abertura ou de fechamento, conforme o
  sentido) ≥ "Torque de Abertura"/"Torque de Fechamento" dá
  `TORQUE_AB`/`TORQUE_FC`.
- Partida: os primeiros 250 ms são ignorados. Até 3 s, o limite ganha a
  margem de "Sobre Torque na Partida" (`sobrTorqPart`, %).
- Sobretorque num sentido, com o alarme do outro sentido ainda
  sinalizado e a posição a ±15‰ de onde parou da última vez, dá
  `VALV_TRAVADA`.
- O torque AD máximo zera a cada início de movimento, como no original.
  Antes era o máximo desde o boot.

**Assentamento no limite fechado** ("Fechamento com Torque" ligado): o
motor segue fechando por até 1,5 s (`timerFechanTorq`, recarregado
enquanto fecha) e para antes disso se:
- com célula de carga: o torque atingir o "Torque de Fechamento" (Nm),
  igual ao original;
- com microchaves: a microchave de fechamento atuar (com filtro).
  **DIVERGÊNCIA:** no fwBLE essa comparação é em Nm, que com microchaves
  nunca fecha, e o motor passa 1,5 s do limite ignorando a microchave,
  porque o FSA e o hardware cortam. Aqui o motor é GPIO direto, sem nada
  que corte.

Nos dois casos, o original chama `at_ctl_stop(ACP_TORQUE)` no fim do
assentamento. Aqui o controle já está em PARAR, então só libera o
desligamento do motor: uma parada pendente cancelaria o próximo comando.

**Destravamento (igual ao fwBLE, decisão de segurança):** quitar
alarmes **não** libera bloqueio de torque nem válvula travada. Quitar só
apaga a sinalização de alarmes cuja ação já terminou. Os bloqueios saem:
- sobretorque num sentido: comandando o sentido oposto;
- os dois tipos, inclusive válvula travada: mexendo no volante com o
  motor parado (alarme de operação manual), e só depois quitando a
  sinalização;
- com microchaves, a válvula travada também some sozinha quando as duas
  microchaves deixam de atuar juntas, porque o fwBLE recalcula a cada
  ciclo nesse ramo;
- reiniciar a placa zera todos os alarmes.

**Diagnóstico:** com célula de carga, a cada partida o RTT mostra se o
torque do sentido está sendo monitorado e com qual limite. Por exemplo,
`Abrindo: limite 80 Nm (+40% nos primeiros 3 s), torque agora 2 Nm`, ou
`Abrindo: torque de abertura NAO monitorado` com "Desliga Torque de
Abertura" ligado. O torque abaixo do Torque Zero é cortado em 0, como em
`sptPegAdTrqZer()`: a proteção assume que a leitura sobe nos dois
sentidos.

**Motor de alarmes estendido** para isso: bloqueio por sentido
(`AA_STO_INC`/`AA_STO_DEC`), as retenções `AR_ACT_INC_INFO`/
`AR_ACT_DEC_INFO` (o bloqueio de um sentido só sai com o motor indo no
outro; usa o callback `mov_stt_cb`) e `actuator_alarm_action_clear()`.

**Ainda não portado:** a supressão de torque durante ESD (`ESDDTrq`,
passo 6).

### `actuator_panel` — comando por BLE (Painel Remoto)

Port de `ifFerConfigRunCmd()` + `ifFerConfigCmdRemPanelCtl`. Ver o
**Contrato BLE** abaixo pro formato da palavra e o lease.

- `actuator_panel_run()` é chamado a cada ciclo por `actuator_mode_run()`.
  Sempre processa o lease e o campo de modo. Comandos (abre/fecha/para/
  quita) só se **lease válido E modo == LOCAL**.
- Sincronização escrita-BLE ↔ loop-de-controle via `atomic_t` (o fwBLE é
  super-loop cooperativo, não precisa).
- **Não portado**: os tipos de mensagem acgl `GTM_ALLOCTION`/
  `GTM_RELEASE` (reserva de sessão por nome de interface, diferente dos
  bits alloc/free de autoridade de comando) → respondem `GTM_NEG`.

### `actuator_mode` — árbitro de modo

Port de `atModoAcao()`.

- `enum actuator_mode`: `DESLIGA`(0) `PARA`(1) `LOCAL`(2) `REMOTO`(3)
  `PARAM`(4) `INFO`(5) — valores no fio, não renumerar. **Default: LOCAL.**
- **Remap deliberado**: no fwBLE o modo vem de um seletor físico e a
  interface BLE é fonte *remota*. Aqui não há seletor, e a interface BLE
  tem a implicação de "presença física" que o painel local tinha:
  - **LOCAL = interface BLE** (área Painel Remoto)
  - **REMOTO = RS485 / MB TCP / broker MQTT** (nada existe ainda — passo 9)
- Modo trocado pelo campo `mode` (bits 4–6) da própria palavra do Painel
  (= `ifFerConfigCmdRemPanelCtl.mode`). Valor **7 = não mudar** — comandos
  normais carregam 7; só o seletor de modo manda 0–5. Troca de modo
  **não** passa pelo gate de lease (menos perigoso que comandar).
- **Ligou 3 alarmes** que estavam sem gatilho: `MODO_NAO_REMOTO` (set ao
  sair de REMOTO, release ao entrar), `LOCAL_INIBIDO` (LOCAL +
  `mode_inib_local()`, hoje stub false), `PARADA_LOCAL` (estava movendo
  ao ser tirado de REMOTO pra PARA).
- **`DESLIGA` colapsado em `PARA`** (`actuator_mode_set()` coage
  `DESLIGA`→`PARA`). No fwBLE a distinção vinha da forma física do
  seletor com mola (PARA = neutro, DESLIGA = posição mantida/cadeável); as
  diferenças reais de código (alarme `PARADA_LOCAL`, auto-quita de alarme
  local se `alAtQtLoc`, telemetria de fieldbus/HMI) não se justificam
  sem o seletor. O valor `0` fica **reservado** pra um eventual "fora de
  serviço / bloqueio de manutenção" real. `PARAM`/`INFO` também são
  vestigiais (menu de nav do HMI) — sem UI, reservados.
- **Stubs**: verificação de ESD (passo 6), `mode_inib_local()` sempre
  false (`paramDado.inibCmdLoc || ifDigInibLoc()`, passo 8), fonte de
  comando de rede (passo 9), auto-quita de alarme local ao entrar em PARA
  (`paramDado.alAtQtLoc`, passo 8).

## Contrato BLE novo

### Área "Painel Remoto" — `0x00805900` (comando, `GTM_SEND`)

**Endereço REAL do fwBLE** (`MSG_EX_ADDRESS_CONFIG_REM_PANEL_CTL`) — um
Gateway real comanda uma unidade SIM Connect em papel de atuador do mesmo
jeito que comanda um Atuador BLE legado. Palavra de 16 bits, little-endian,
layout idêntico ao `ifFerConfigCmdRemPanelCtl_t`:

| Bit(s) | Campo | Efeito |
|---|---|---|
| 0 | `cmd_open` | demanda ABERTO (1000) |
| 1 | `cmd_close` | demanda FECHADO (0) |
| 2 | `cmd_stop` | para |
| 3 | `ack_alarm` | quita alarmes (escopo Local) — **não é auto-limpo** pelo firmware |
| 4–6 | `mode` | 0–5 = troca de modo; **7 = não mudar** |
| 7–13 | spare | |
| 14 | `alloc` | renova o lease de comando (5 s) |
| 15 | `free` | libera o lease |

**Lease (`TIME_LEASE` = 5000 ms)**: comandos só valem enquanto o lease
está ativo. A interface renova mandando `alloc` junto de cada comando
(sem timer de keepalive separado). `free`, ou 5 s sem `alloc`, encerra —
protege contra comando preso se a interface cair. Resposta:
`GTM_CONFIRM` em sucesso, `GTM_NEG` pra endereço/tamanho não suportado.

Prioridade dos comandos: `stop` > `close` > `open` (igual ao fwBLE).

### Área de status sintética — `0xF0000010` (leitura, `GTM_REQUEST`)

**Nossa**, fora do espaço real proCo. **Não** é a área real de alarmes do
fwBLE (`0x00804880` = `al_stt[]` inteiro, ~60 × 2 bytes) — essa só entra
quando o conjunto completo de alarmes existir. 5 bytes:

| Byte | Conteúdo |
|---|---|
| 0–1 | u16 LE — bitmap de alarmes ativos/não-quitados (`actuator_alarm_info_bitmap()`, bit i = `enum actuator_alarm_id` i) |
| 2 | flags — bit 0 = movimento bloqueado por alarme |
| 3 | `actuator_control_get_mov_stt()` — 1=L_SUPER 2=L_INFER 3=abrindo 4=fechando 5=parado |
| 4 | `actuator_mode_get()` — 0=DESLIGA 1=PARA 2=LOCAL 3=REMOTO 4=PARAM 5=INFO |

### Áreas já existentes (não mudaram)

- `0x00804080` — Sensor (posição/torque, 10 bytes) — ver `SENSORES_I2C.md`.
- `0xF0000000` — Info sintética (fonte de torque ativa, 1 byte).

## Interface (`nrf54-app/index.html`)

No card "Posição" do painel "Este SIM Connect (papel de atuador)":

- **Botões Abrir / Parar / Fechar** → `GTM_SEND` na área Painel. Cada um
  carrega `alloc` (re-arma o lease) e `mode = 7` (não mexe no modo).
- **Seletor de modo** (Local / Remoto / Parado) → `GTM_SEND` com
  `(modo << 4) | alloc`. Sincroniza com o modo que o firmware reporta
  (byte 4 da área de status). Fora de LOCAL, mostra aviso de que os
  comandos daqui são ignorados.
- **Botão do sentido ativo pisca** (verde abrindo, vermelho fechando)
  enquanto o movimento roda — lido da área de status a cada 1,5 s.

Card "Alarmes" novo: lista dos alarmes locais (14 desde a proteção por torque) (ativos em vermelho),
selo "Movimento bloqueado", botão "Quitar alarmes" (`ack_alarm`, seguido
de uma escrita que limpa o bit já que o firmware não auto-limpa).

Infra: `withAcg()` serializa comandos contra o polling de sensor (um
pedido acgl em voo por vez). A lista de 118 nomes de alarme do fwBLE já
existia em `index.html` (`ALARM_NAMES`, de `ressources/bytes_alarmes.json`,
usada pra atuador remoto num slot) — a lista local nova é
`LOCAL_ALARM_NAMES` (14 entradas). Essa tabela de 118 é referência pronta
pro port completo dos alarmes.

## Gatilhos de bancada (removidos em 2026-10-06, conferido em bancada)

Os botões do DK, que injetavam demanda direto no controle sem passar pelo
árbitro de modo nem pelo lease, e o log periódico de sensores no laço do
`main()` foram removidos (branch `feature-comandos-torque`). A única
entrada de comando é o Painel BLE, via árbitro de modo.

## O que ficou de fora / próximos passos

### Passo 6 — ESD e PST
`atESD.c` (Emergency Shutdown) e `atPST.c` (Partial Stroke Test). Os
seams já existem: `actuator_control_demand/stop()` têm comentário
`STUB passo 6: atESDAbort()/atPSTAbor()`; `actuator_mode` tem
`STUB passo 6` na verificação de ESD; os 4 alarmes `ESD_EXT`/`PST_EXE`/
`PST_FAL` (e o `al_esd_jump`) já estão no enum sem gatilho.

### Passo 7 — paridade UART 91↔54
Estender a entrada de comando (hoje só BLE) pro transporte da nuvem, via
os opcodes do `PROTOCOLO_54_91.md`.

### Passo 8 — parâmetros persistidos (`paramZarI.c`)
**Feito na branch `feature-param`** (2026-10-01). Ver `PARAMETROS.md`. O
mapa inteiro do `paramDado` (144 bytes) foi portado tal e qual, na área
Painel real (`0x00800800`), com salvar explícito (`IFCC_SAVE`).

Ligados ao comportamento: `limiteMargem`/`faixaParado`/`anteciparParada`
(antes `#define`), `inibCmdLoc`, `alAtQtLoc`, `fabLeitPosiInver`/
`antiHorario` (sensor + motor), `fabFatTorqAber`/`Fech` e, com a proteção
por torque, `torqueNmInc`/`torqueNmDec`, `sobrTorqPart`, `deslTrqIncr` e
`trqFechad`. Ainda sem consumidor:
- `ifDigInibLoc()`: aguarda a E/S digital.

### Passo 9 — RS485 / Modbus RTU
Só desenho por enquanto. Entra como mais uma fonte de comando (modo
REMOTO). Ao pegar: reservar UART + pinos DE/RE no orçamento do nRF54;
mapa de registros pode espelhar os blocos do Driver DC 2
(`ENUM_BLOCKS_t`: COMANDO/STATUS/ENTRADAS/CONFIG/INFO/...).

### Outros itens registrados
- **Watchdog: feito e validado em bancada** (`src/watchdog.c`). É o
  `task_wdt` com o WDT31 (o único acessível do lado non-secure) como
  fallback de hardware. Canais:
  - "controle" (500 ms): alimentado na primeira linha de
    `actuator_control_run()`, cobre a fila do sistema;
  - "sensores" (2 s): batimento de 100 ms na `sensor_workq`. Se essa fila
    travar, a posição congela.

  Um canal estourado desliga o motor e reinicia a placa. Se o próprio
  `task_wdt` parar, o WDT31 reseta em ~120 ms. No boot, o log mostra se
  o último reset foi do watchdog. Teste de bancada: `WATCHDOG_TESTE` em
  `watchdog.c`.
- **A confirmar contra o fwBLE: LOCAL → REMOTO com o motor andando.**
  Hoje a troca não para o movimento (`mode_remoto()` não faz nada, e em
  REMOTO o Parar da interface é ignorado pelo Painel). O movimento
  segue até a demanda/limite, ou até alguém trocar pra Parado. Conferir
  se o `atModoAcao()` original faz o mesmo ou se é divergência de port.
- **Os ~51 alarmes restantes** — expansão futura. A tabela de 118 nomes
  em `index.html` é a referência.
- **Área real de alarmes `0x00804880`** — quando o conjunto completo
  existir. Aí o `enum actuator_alarm_id` precisa ser realinhado com o
  `alarm_t` do fwBLE (risco de fidelidade aberto).
- **Alarmes de driver de motor** (sobrecorrente / temperatura / falta de
  fase) — dependem de sensoriamento no circuito de acionamento, que
  ainda não existe. O `foSeAc.h` do fwBLE tem os getters
  (`falhaAcionar`/`falhaMotorSobreAquecido`/...); o Driver DC 2 tem
  equivalentes por ADC.
- **Rampa de partida do motor**: depende do circuito de acionamento
  real (`aplicAcioMot()` do Driver DC 2). O tempo morto na reversão já
  existe e é configurável (padrão 3000 ms, como a FSA).
- **`FAL_ACION_INV` trava o comando** até o volante ser mexido — **fiel
  ao fwBLE, decisão de segurança** (fiação invertida não deve ser
  dispensável de tela remota). Recuperação na bancada: esperar ~3 s
  (inércia), girar o pot > 0.5% com o motor parado.
- **`GTM_ALLOCTION` / `GTM_RELEASE`** (sessão acgl) — respondem `GTM_NEG`.

## Como testar em bancada

Potenciômetro no `i2c21` como feedback de posição (ver
`SENSORES_I2C.md`). Interface: reabrir a página, conectar.

1. **Modo Local (default)**: Abrir → o controle aciona P1.30, o motor
   "abre", o botão Abrir pisca verde, e para ao chegar em ~1000
   (`origem parada = LIMITE`). Fechar simétrico em P1.31. Apertar de novo
   enquanto move → para.
2. **Banda morta**: se a posição já está a < 20 por mil da demanda, não
   sai do lugar.
3. **Acionamento invertido**: com o motor comandando abrir, girar o pot
   no sentido de fechar > 5% → `FAL_ACION_INV`, selo "Movimento
   bloqueado", motor para. Só sai girando o pot (volante) após os ~3 s
   de inércia.
4. **Volante**: motor parado, girar o pot > 0.5% → `OPER_MANUAL` (some
   ~3 s depois de parar de girar).
5. **Falha de sensor**: desconectar o pot → `COM_SENS_POS` acende em
   ~3 s, bloqueia comando; reconectar → some sozinho.
6. **Modo Remoto**: o seletor mostra o aviso; Abrir/Fechar/Parar da
   interface ficam inertes (sem fonte de rede ainda).
7. **Remoto → Parado movendo**: em Local, mande Abrir; enquanto pisca,
   troque pra Remoto e depois Parado. Ao sair de Remoto acende
   `MODO_NAO_REMOTO`; se movia, acende também `PARADA_LOCAL`. Voltar pra
   Remoto limpa os dois.

## Build

Ver `README.md`. `west build -b nrf54lm20dk/nrf54lm20b/cpuapp/ns`.
Todos os passos acima foram validados com build completo (sysbuild +
TF-M) sem erro nem warning novo.
