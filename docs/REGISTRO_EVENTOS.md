# Registro de eventos (port do `atRegEvent` do fwBLE)

Port do registro de eventos do firmware da linha BLE (`fwBLE` /
`ControleCoesterBLE`, `BLE/Atuador/atRegEvent.{c,h}`) para o `nrf54-app`
(`src/actuator_regevent.{c,h}`), com leitura pela interface como no fwBLE.
Branch `feature-regEven`.

**Confiabilidade deste documento**: escrito por uma sessão do Claude a
partir do código-fonte real (fwBLE e deste repositório). Trate como pista,
não como verdade definitiva, se for base de uma decisão importante.

## Decisões (Felipe, 2026-10-06)

- **Flash externa**: a placa do SIM Connect usa a mesma flash SPI NOR do
  nRF54LM20 DK, **Macronix MX25R6435F (8 MB)**, ou outra NOR SPI
  compatível. O driver `jedec,spi-nor` do Zephyr lê os parâmetros do
  próprio chip (SFDP), então trocar de chip é só devicetree. A IS25LP080D
  do Painel BLE (1 MB) não comportaria FOTA.
- **Uma área fixa por uso** (overlay da placa):

  | Faixa | Tamanho | Uso |
  |---|---|---|
  | `0x000000–0x17FFFF` | 1,5 MB | Reservado para o FOTA (slot secundário do MCUboot; tamanho final quando o MCUboot entrar) |
  | `0x180000–0x27FFFF` | 1 MB | Registro de eventos (65536 eventos) |
  | `0x280000–0x7FFFFF` | 5,5 MB | Livre (curvas de torque, futuro) |

- **Parâmetros continuam na RRAM interna** (settings/ZMS). Motivos:
  segurança (o atuador continua operando se a flash externa falhar),
  disponibilidade desde o boot, ZMS feito para RRAM, e a futura senha
  (`SEGURANCA_SENHA.md`) não deve morar num chip externo legível por
  ponta de prova. **Divergência**: o fwBLE guardava o banco de dados
  (`FILE_DATA`, `contAbsol`) na flash SPI.
- **Sem relógio nesta branch**: eventos com tempo desde o boot (ver
  "Formato"). Data/hora entra depois (área RTC `0x00803000` +
  `IFCC_UPDATE_RTC` e/ou hora da rede pelo nRF91).
- **Corrigir o bug da volta na leitura** (ver "Divergências").

## Como o fwBLE faz (referência)

- `tEvent`: 16 bytes (14 de dados + 2 de alinhamento, `reserv[]` com
  tamanho zero no IAR).
- Fila em RAM de 32 (`TAM_FILA_EVENT`); cheia, descarta. Descarga
  (`regEventSalv`) quando passa 2 s sem evento novo, chega a 16, ou antes
  de uma leitura (`liquidaFila`).
- Arquivo `FILE_LOG` via `sel_mem`: flash SPI externa (IS25LP080D,
  `0x7B000–0xFFFFF`, ~34 mil eventos) ou, com cartão SD, arquivo FAT de
  50 MB (~3,2 milhões). Próxima posição em `contAbsol.absolIndexRegEvent`,
  regravado a cada descarga. Buffer circular; ao dar a volta registra
  `EV_SIS_REG_FIM_FILE`.
- Boot: `EV_SIS_POWER_DOWN` + `EV_SIS_POWER_UP` com as horas que o RTC
  externo com bateria marcou na queda e na volta; senão `EV_SIS_WDT_RST`
  ou `EV_SIS_SW_RST`.
- Leitura pela BLE (comandos liberados em `accessCmdBLE`) na área Comando.

## Formato do registro (`tEvent`, 16 bytes, little-endian)

| Bytes | Campo | fwBLE | SIM Connect |
|---|---|---|---|
| 0..3 | `index` u32 | Posição no arquivo (volta a 0) | Igual (posição na partição) |
| 4..5 | `event` u16 | ID do `enum EVENT` | Igual, mesmos IDs |
| 6 | `ano` | Ano, 2 dígitos | **Byte alto dos dias desde o boot** |
| 7 | `mes` | Mês 1..12 | **0 = formato "tempo desde o boot"** |
| 8 | `dia` | Dia | **Dias desde o boot (byte baixo)** |
| 9..11 | `hora minu segun` | Hora do RTC | **hh:mm:ss desde o boot** |
| 12..13 | `milSeg` u16 | ms desde o evento anterior, até 2000 | Igual |
| 14..15 | (alinhamento) | Lixo | **Contador de voltas do buffer** |

Nenhuma data válida tem mês 0, então quando a data/hora entrar os eventos
novos (mês 1..12) e os antigos (mês 0) continuam distinguíveis.

## IDs de evento

Os mesmos do `enum EVENT` do fwBLE (`src/actuator_regevent.h`). Alarmes:
`EV_AL_ON` (200) / `EV_AL_OFF` (400) **+ número do alarme no `alarm_t`
do fwBLE** (59 alarmes), não a posição no nosso `enum actuator_alarm_id`.
A tradução fica na coluna `fwble_id` da tabela `alarm_ctl` em
`actuator_alarm.c`; alarme novo precisa preencher essa coluna.

## Pontos de registro

| Evento | Onde | Origem no fwBLE |
|---|---|---|
| `EV_SIS_POWER_UP` / `EV_SIS_WDT_RST` / `EV_SIS_SW_RST` | `actuator_regevent_init()`, causa classificada em `watchdog.c` | `atRegPowerReset()` |
| `EV_SIS_REG_FIM_FILE` | descarga, ao dar a volta | `regEventSalv()` |
| `EV_AL_ON/OFF` + alarme, `EV_AL_BQ` | `reg_set`/`reg_release`/`reg_block_mov` (`actuator_alarm.c`) | `al_reg_set/release/block_mov` |
| `EV_LOC_QT` / `EV_REM_QT` | `actuator_alarm_clears_all()` | `at_alarm_clears_all()` |
| `EV_LOC_PAR/LOCAL/REMOTO/PARAM/INFO/DESLIG` | troca de modo (`actuator_mode.c`) | `atModoAcao()` |
| `EV_LOC_ABR` / `EV_LOC_FEC` | comando abrir/fechar consumido (`actuator_panel.c`) | borda da ação da IHM (`atModo.c`) |
| `EV_OPE_NUL` + estado (1..5) | mudança de `at_status` (`actuator_control.c`) | `atControle.c` |
| `EV_PARAM_SALV` | `IFCC_SAVE` (`actuator_service.c`) | salvar pela IHM (`ifIHM.c`) |

**Sem ponto de registro ainda** (IDs reservados): cartão SD e curvas
(não portados), PST (`EV_LOC_PST`, pendente), barramento de campo e ações
de falha de comunicação (`EV_REM_*`, `EV_ACAO_CONFIG_*`, sem RS485 ainda),
entradas discretas (`EV_REM_DISC_*`, dependem do AD74412R, branch
`integra-ios-digitais`), usuário de fábrica e modo config. de fábrica (não
há IHM).

## Causa do boot

`watchdog.c` classifica no `watchdog_init()`:

- **WDT**: marca `"WDTR"` numa RAM `__noinit`, escrita pelo
  `canal_estourado()` antes do `sys_reboot()`, ou `RESET_WATCHDOG` (WDT31
  estourando sozinho). A marca é necessária porque o `sys_reboot()` aparece
  para o hardware como reset por **software** (`RESETREAS.SREQ`).
- **Energização**: nenhum bit no RESETREAS (o nRF54L não tem bit de
  power-on) ou só `RESET_POR`.
- **SW**: qualquer outro (software, pino de reset, debugger, lockup).

## Armazenamento

- Partição `event_log_partition` (`flash_area`), 65536 posições de 16
  bytes. A NOR precisa ser apagada antes de gravar: ao entrar num setor de
  4 KB (256 eventos), ele é apagado. Os eventos mais antigos daquele setor
  se perdem 256 de cada vez, como no fwBLE.
- **Posição de escrita achada no boot** (sem `contAbsol`): o slot 0 dá a
  volta corrente L; as posições `[0, head)` estão na volta L e as demais
  na anterior ou apagadas. Busca binária (~17 leituras). Se a posição
  achada tiver resto de escrita interrompida, pula até uma apagada.
- Registro meio gravado (falta de energia na escrita) não passa na
  validação (`index` = posição, evento conhecido, volta ≠ 0xFFFF) e sai na
  leitura como `EV_SIS_REG_INV`, como no fwBLE.
- A descarga roda numa fila própria, de prioridade baixa: um apagamento
  de setor (até ~240 ms na MX25R) não segura o laço de controle nem o
  polling I2C.

## Leitura (área Comando, `0x00800000`)

Layout `ifFerConfigCmd_t` (`pack(2)`): `[0..1]` status, `[2..3]` comando,
`[4..7]` índice (u32), `[10..73]` buffer.

- `IFCC_START_READ_EVENT` (`0x0400`) + índice: descarrega a fila e devolve
  o registro do índice (`0xFFFFFFFF` = o mais novo) em `buffer[0..15]`
  (bytes 10..25 da área).
- `IFCC_SEQ_READ_EVENT` (`0x0401`): o próximo mais antigo. Sem sequência
  ativa, recomeça do mais novo (`atRegEventIniAcesSeqDef`).
- **Fim**: `index = 0xFFFFFFFF` e `event = EV_SIS_REG_INV` (10). Chamadas
  seguintes repetem a marca.
- Status `IFCCS_READY` (0) com o registro no buffer já ao responder o
  `GTM_CONFIRM`; `IFCCS_FAIL` (2) se a flash não estiver disponível.

## Divergências deliberadas do fwBLE

| fwBLE | SIM Connect | Motivo |
|---|---|---|
| `FILE_LOG` na flash SPI (1 MB) ou cartão SD | Partição de 1 MB na MX25R6435F | Hardware do SIM Connect; decisão de 2026-10-06 |
| Posição de escrita em `contAbsol`, regravado a cada descarga | Achada no boot por busca binária; bytes 14..15 com o contador de voltas | Sem `contAbsol` portado; evita uma gravação extra a cada descarga |
| Data/hora do RTC | Tempo desde o boot, `mes = 0` | Sem RTC com bateria |
| `EV_SIS_POWER_DOWN` com a hora da queda | Não registrado | Sem RTC com bateria para marcar a queda |
| Ao passar do índice 0, volta para `absolIndexRegEvent - 1` (o mais novo): depois da 1ª volta, a parte mais antiga nunca é lida | Segue para a última posição e continua; para ao completar a volta ou numa posição apagada | Bug do original |
| Sem marca de fim explícita | `index 0xFFFFFFFF` + `EV_SIS_REG_INV` | Mesmo valor que uma posição apagada já dava no original |
| Índice ≥ tamanho limitado a `NUM_EVENT_REG_FILE` (fora do arquivo) | Índice fora da faixa = o mais novo | Evita ler fora da partição |
| Status passa por `IFCCS_WAIT_*_READ_EVENT` | Leitura síncrona, status já `READY` | Flash SPI sem FAT; um cliente que consulta o status continua funcionando |
| `EV_LOC_ABR/FEC` na borda da ação da IHM | Um por comando abrir/fechar | O Painel Remoto é por pulso |

Compatibilidade com um leitor do tipo `baixarSequencia` (`index.html`,
para atuadores remotos), que para quando um índice se repete: com a
correção da volta, ele vê uma linha "Evento inválido" no fim (a primeira
marca de fim) e para na segunda.

## Interface (`nrf54-app/index.html`)

Aba **Eventos** do Atuador conectado (SIMControl). Desde 2026-10-08 ela
mostra este registro no lugar dos antigos "eventos desta conexão", que
só existiam na memória do navegador.

- **Ao abrir a aba** (ou ao reconectar com ela aberta): `START` com
  `0xFFFFFFFF` e `SEQ` até a marca de fim ou 100 eventos. As linhas vão
  aparecendo enquanto lê. **Reler** recomeça do mais novo; **Carregar
  mais antigos** continua a sequência; no fim aparece "Fim do registro".
- Tabela: tempo (`+hh:mm:ss` ou `+N d hh:mm:ss` desde a energização, ou
  `dd/mm/aaaa hh:mm:ss` quando houver data), evento e nº (o `index`;
  some abaixo de 420 px úteis).
- Nomes por extenso (os do `lang_port.h` são de LCD de 16 caracteres).
  Modos com os nomes do seletor da Operação (`EV_LOC_PAR` e
  `EV_LOC_DESLIG` aparecem como "Modo: Desligado"). Alarmes como
  "Alarme: …" / "Normalizado: …", com o nome da aba Alarmes; a tradução
  `alarm_t` → bit fica em `LOCAL_ALARM_FWBLE_ID`, que acompanha a coluna
  `fwble_id` de `actuator_alarm.c`. Alarme de fora dessa lista sai com o
  nome do `alarm_t` (`ALARM_NAMES`).
- Energização e reinícios em linha cinza e negrito (o tempo recomeça
  ali); alarme ativado e bloqueio de movimento como na aba Alarmes.
- Firmware sem o registro responde `GTM_NEG` ao `START`: a aba avisa
  para atualizar o firmware. `IFCCS_FAIL`: "A memória de eventos do
  Atuador não respondeu".

## Observações

- Até 2026-10-07, `COM_SENS_POS` ficava ativo no boot até a 1ª leitura
  boa do sensor de posição, gerando um par "Com. sens. pos —
  ativo/desativo" a cada energização. Corrigido: agora ele tem a mesma
  tolerância de 3 s do fwBLE (`timerSenPosOk`, `TEMPO_FALHA_SENSOR`) e do
  `COM_SENS_TRQ`.
- O registro só é montado na 1ª descarga (fila própria). Se a flash não
  responder, os eventos são descartados, a leitura devolve `IFCCS_FAIL`
  e o log mostra "Flash do registro de eventos indisponível".

## Validação em bancada

**Pendente.** Roteiro:

1. **Boot limpo**: gravar com a flash externa apagada (ou primeira vez).
   Log: `Registro de eventos: 65536 posicoes, proxima 0, volta 0`. Ler
   eventos: "Energização" e o estado de operação inicial, sem o par
   "Com. sens. pos".
2. **Comandos e modos**: Abrir, Fechar, Parar, trocar Local/Remoto/Parado,
   quitar alarmes, salvar um parâmetro. Após ~2 s, Ler eventos: cada ação
   aparece (Local abrir/fechar, Movim. abertura/fechamento, Limite...,
   Modo ..., Local quitar al., Salvou parâmetros), do mais novo ao mais
   antigo, com o tempo crescente.
3. **Persistência**: desligar e religar a placa. Ler eventos: os
   anteriores continuam e há uma nova "Energização" no topo, com o tempo
   zerado. O log mostra `proxima N` igual à contagem anterior.
4. **Reset por watchdog**: com `WATCHDOG_TESTE 1` em `watchdog.c`, deve
   aparecer "Reset WDT" após o reinício. Voltar para 0.
5. **Reset pelo botão do DK**: deve aparecer "Reset SW".
6. **Alarme**: provocar um sobretorque ou desconectar o sensor de posição:
   aparece o alarme "— ativo" (e "Alarme bloqueio" se bloquear movimento)
   e, ao normalizar, "— desativo".
7. **Volta do buffer** (opcional, longo): reduzir a partição no overlay
   para 2 setores (8 KB, 512 eventos), gerar mais de 512 eventos e
   conferir "Fim arq. eventos", a leitura atravessando o fim do buffer e a
   marca de fim.
