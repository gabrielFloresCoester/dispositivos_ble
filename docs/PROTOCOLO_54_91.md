# Contrato UART (Kit 54 <-> Kit 91) para espelhar o protocolo BLE da interface

**Status (2026-08-14): Fases 1-4 implementadas e validadas em bancada.**
`nrf54-app/src/uart_link.c` fala a tabela de opcodes abaixo contra o
Gateway de verdade - BLE Central+Peripheral ativo, atuadores reais
conectados - com teste de estresse (180+ frames consecutivos, 100% de
sucesso) e teste de desconexao fisica do fio TX em voo, ambos passando.
`nrf91-app/src/uart_link.c` (Fase 4) pede `GET_STATUS`/`GET_DISCOVERED`
por botao e decodifica os registros que chegam - validado com atuador
real conectado, `RAW_DATA` batendo byte a byte com o log BLE do 54 pro
mesmo comando. Migrado pra NCS 3.4.0 (LTS) em 2026-08-14, com os dois testes de
bancada revalidados sem falha - ver "Migracao para NCS 3.4.0" mais
abaixo pro achado especifico dessa migracao (particao do nRF91).
**Antes de tocar em `uart_link.c` de novo (nrf54 ou nrf91), leia a
secao "Achados de bancada da Fase 3" mais abaixo** - tem
um problema de UARTE do nRF54L/H e do nRF91 que custou uma investigacao
longa pra achar e e' facil de reintroduzir sem querer. A integracao
com MQTT ja foi feita depois (2026-08-26) e tem documento proprio,
`PROTOCOLO_91_MQTT.md` (mesma pasta) - o gatilho de sincronizacao
passou a ser "ao conectar no broker", e os botoes da Fase 4 ficaram
como resync manual. Este documento propoe como
estender o link UART que nasceu em
`exercises/uart-link` (botao -> LED, um exercicio, era
`dual_kits_54`/`dual_kits_91`) para carregar as mesmas 6 operacoes que
a interface HTML ja faz hoje via BLE contra o Gateway (ver
`PROTOCOLO_INTERFACE.md`, mesma pasta), de forma que a nuvem (MQTT no
91) tenha paridade completa com a interface local.

Decisoes ja tomadas (confirmadas com o Gabriel em 2026-08-05):
- Interface local e nuvem sao **praticamente exclusivas** na pratica
  (interface = bancada/debug, nuvem = campo) - nao precisamos de
  arbitragem pesada de concorrencia entre os dois transportes na v0.
- A nuvem precisa de **paridade completa** (1:1) com a interface: as 6
  operacoes, incluindo gerenciar allow-list e trocar modo rapido/lento.

Decisao de produto (2026-08-06): **Caminho B** - produto unico, nao
produtos separados. Isso nao muda o escopo deste documento, mas
delimita ele: o link 54<->91 so precisa estar ativo em unidades
**configuradas como gateway**. Toda unidade e atuador sempre (I2C +
BLE Peripheral); ser gateway (BLE Central + este link UART + MQTT) e
um papel opcional, escolhido por unidade via menu de configuracao, nao
"toda unidade fala com o 91 o tempo todo". Ver `DECISOES_PRODUTO.md`
(mesma pasta) para o racional completo dessa e das outras decisoes de
arquitetura.

## Por que UART, nao SPI nem I2C

SPI e I2C sao barramentos com mestre: so o controller inicia uma
transacao, o peripheral so fala quando e "clocado". Isso e um problema
real aqui porque boa parte do trafego deste protocolo e o 54
empurrando dado **sem ser perguntado** (`RAW_DATA`, `STATUS_RECORD`
disparam quando algo muda no atuador, nao quando o 91 pede). Fazer
isso funcionar em SPI/I2C exigiria um fio extra de "dado pronto" (IRQ)
do 54 pro 91. UART nao tem essa assimetria - os dois lados transmitem
quando querem, exatamente o modelo peer-to-peer que este protocolo ja
pressupoe. Menos fios tambem (2 sinais vs 4-5), e a camada de
confiabilidade (CRC16+ACK+retry) ja esta construida e validada em
`exercises/uart-link`.

Unico cenario que reabriria essa escolha: FOTA cross-chip com volume
grande o suficiente pra UART virar gargalo real de throughput - ver
secao de FOTA abaixo. Nao e motivo pra trocar agora, so pra medir
quando FOTA for implementado.

## Por que isso e mais facil do que parece: o encaixe ja existe

O `nrf54-app` de hoje ja **nao** fala BLE GATT direto de dentro da
logica de negocio. `actuator_client.c` so conhece um conjunto de
callbacks (`struct actuator_manager_cb` - `status_cb`, `discovery_cb`,
`raw_data_cb`) que o `main.c` registra em `actuator_manager_init()`.
Quem de fato chama `my_lbs_send_actuator_status()` etc. sao as funcoes
do `main.c` (`actuator_status_cb()`, `actuator_discovery_cb()`,
`actuator_raw_data_cb()`) - **nao** o `actuator_client.c` diretamente.

Isso quer dizer que adicionar um segundo destino (a UART pro 91) e
literalmente uma linha a mais dentro de cada uma dessas 3 funcoes do
`main.c`, ao lado da chamada que ja existe pro `my_lbs`:

```c
static void actuator_status_cb(const uint8_t *rec, size_t len)
{
	update_actuator_led();
	my_lbs_send_actuator_status(rec, len);
	uart91_send_status(rec, len);   /* <- novo, mesmo registro de 41 bytes */
}
```

O caminho inverso (requisicoes vindas do 91) e simetrico: um novo
`uart_link.c` no `nrf54-app` so precisa chamar as mesmas funcoes que
`my_lbs.c` ja chama hoje (`actuator_manager_send_to()`,
`actuator_manager_add()`, `actuator_manager_remove()`,
`actuator_manager_set_speed()`, etc.) - o `actuator_client.c` nao muda
nada, ele ja e agnostico de transporte.

Na pratica, o trabalho de firmware no 54 e: (1) um novo modulo
`uart_link.c` que fala o frame descrito abaixo e chama as mesmas
funcoes do `actuator_client.c`, e (2) 3 linhas novas nos callbacks do
`main.c` acima. Isso e' bem menor do que "reescrever o Gateway para
suportar dois transportes".

## Formato do frame (implementado - LEN variavel, CRC16)

```
[0]        SOF        0x7E
[1]        TYPE       opcode - ver tabela abaixo
[2]        SEQ        1 byte, wrap 0..255 (chave do ACK/dedup, junto com TYPE)
[3]        LEN        tamanho do payload, 0..250
[4..4+LEN-1] PAYLOAD  especifico por opcode (ver tabela)
[4+LEN]    CRC16      CCITT (poly 0x1021, init 0xFFFF), 2 bytes,
                       cobre TYPE+SEQ+LEN+PAYLOAD inteiro (nao so o SOF pra fora)
```

`PROTO_MAX_FRAME = 256` (4 de cabecalho + ate 250 de payload + 2 de
CRC) em todas as implementacoes (`exercises/uart-link`, `nrf91-app`,
`nrf54-app/src/uart_link.c`). Mecanismo de ACK + retry (ate 3
tentativas, 300ms cada) + fila de saida continua o mesmo em espirito
nas tres - so a fila de saida do `nrf54-app` foi alargada pra 16
posicoes (nao 4, como no exercicio) porque `GET_DISCOVERED(0xFF)` pode
gerar ate `MAX_DISCOVERED` (12) frames de resposta de uma vez, e o
mecanismo e' stop-and-wait (1 frame fisico em voo por vez) - com fila
de 4 esses frames extras seriam descartados em silencio.

**Historico** (pra quem encontrar referencias antigas a "5 bytes" ou
CRC8 em commits/conversas): era fixo de 5 bytes
(`SOF+TIPO+SEQ+CMD+CRC8`, 1 byte de payload) no exercicio original;
virou 6 bytes/CRC16 em 2026-08-06; virou LEN-variavel (formato acima)
em 2026-08-11, migrado primeiro em `exercises/uart-link` + `nrf91-app`
(checkpoint de bancada: ciclo ECHO_STR ok, retry/timeout ok,
desconexao fisica ok) antes de entrar em `nrf54-app/src/uart_link.c`
na Fase 3.

## Por que nao precisamos de fragmentacao (por enquanto)

O maior payload do protocolo de hoje e a leitura **em lote** da
Actuator Status: 19 x 41 = 779 bytes - nao cabe em `LEN <= 250`. Mas
todo o resto (um registro de Status, um de Discovered, uma resposta de
Raw Data, um Actuator Command, um Actuator Manage de 8 bytes) fica bem
abaixo de 250 bytes por registro.

Proposta v0: **o 91 nao pede o lote inteiro de uma vez.** Para o sync
inicial (equivalente ao que a interface faz no `read()` da `152a` ao
conectar), o firmware do 91 pede status **slot por slot**, em loop -
19 requisicoes pequenas em vez de 1 grande. Mais lento no boot (mas
sao poucos segundos), evita reprojetar fragmentacao/reassemblagem numa
UART com buffers pequenos. Se isso incomodar na pratica, um comando
`GET_STATUS_ALL` fragmentado fica pra v1.

## Correlacao: reusa o que ja existe, nao inventa um ID novo

O protocolo proCo original (atuador <-> Gateway) nao tem ID de correlacao
e funciona porque so ha 1 comando em voo por slot. A BLE de hoje usa o
mesmo truque: todo payload comeca com `[slot]`, e e assim que a
interface sabe de qual atuador e a resposta.

Proposta: manter exatamente essa regra no link 54<->91.
- **Operacoes com slot** (Actuator Command, Status, Raw Data): o
  `[slot]` no payload e a chave de correlacao - nao precisa de nada
  a mais, desde que so haja 1 pedido em voo por slot (ver secao de
  concorrencia abaixo). Slots diferentes podem estar em voo em
  paralelo sem problema, igual hoje na BLE.
- **Operacoes administrativas sem slot** (Gateway Ctrl 0x01/0x02, Actuator
  Manage por MAC): usam o `SEQ` do proprio frame como correlacao,
  igual ao ACK que ja existe hoje - nao precisa de campo novo.

## Concorrencia (v0 - leve, dado que os transportes sao praticamente exclusivos)

Como decidido, nao vamos construir uma fila/lock por slot compartilhada
entre BLE e UART agora. Proposta minima para v0 (na epoca deste
rascunho):

- `actuator_manager_send_to()` ganharia uma checagem simples: se o slot
  ja tem uma resposta pendente (rastreado por um novo campo leve,
  tipo `bool cmd_in_flight` por slot, com timeout), loga um aviso
  ("comando via UART colidiu com comando via BLE no slot N - resposta
  pode ser da requisicao errada") em vez de travar ou enfileirar.
- Se isso aparecer nos logs com frequencia depois de operar de
  verdade, aí sim vale portar a mesma ideia da fila de discovery
  (`discovery_queue*` em `actuator_client.c`) para comandos.

**IMPORTANTE - isto NAO foi implementado na Fase 3.** `handle_actuator_cmd()`
em `uart_link.c` chama `actuator_manager_send_to()` direto, sem
nenhuma checagem de colisao nem log de aviso - `actuator_client.c` nao
tem o campo `cmd_in_flight` proposto acima. Ou seja: hoje, um comando
via BLE (Actuator Command `...1528`) e um `ACTUATOR_CMD` via UART pro
MESMO slot, quase ao mesmo tempo, podem genuinamente colidir - a
resposta do atuador (`raw_data_cb`) vai pra ambos os transportes de
qualquer forma (ver `actuator_raw_data_cb` no `main.c`), entao nenhum
dos dois lados fica sem resposta, mas quem pediu por UART pode receber
a resposta de um comando que a interface local mandou (e vice-versa),
sem nenhum aviso no log. Ver "Limitacoes conhecidas" mais abaixo.

## Tabela de opcodes (mapeamento 1:1 com a BLE)

| Opcode | Nome | Direcao | Payload | Espelha (BLE) |
|---|---|---|---|---|
| `0x01` | `CMD_LED` | 91->54 / 54->91 | 1 byte | (exercicio original - pode virar heartbeat/debug, ver abaixo) |
| `0x02` | `ACK` | ambos | `[seq_original]` | - |
| `0x10` | `ACTUATOR_CMD` | 91->54 | `[slot][comando cru]` | write Actuator Command `...1528` |
| `0x11` | `RAW_DATA` | 54->91 | `[slot][resposta crua]` | notify Actuator Raw Data `...1527` |
| `0x12` | `ACTUATOR_MANAGE` | 91->54 | `[cmd][addr_type][MAC x6]` | write Actuator Manage `...1529` |
| `0x13` | `MANAGE_RESULT` | 54->91 | `[seq_original][0=ok / err code]` | (BLE nao tem equivalente explicito - GATT status implicito) |
| `0x14` | `GET_STATUS` | 91->54 | `[slot]` | read Actuator Status `...152a` (por slot, ver secao fragmentacao) |
| `0x15` | `STATUS_RECORD` | 54->91 | 41 bytes (identico ao registro BLE) | read/notify Actuator Status `...152a` |
| `0x16` | `GET_DISCOVERED` | 91->54 | `[indice]` ou `0xFF`=todos (ate 12) | read Discovered Actuators `...152b` |
| `0x17` | `DISCOVERED_RECORD` | 54->91 | 41 bytes (identico ao registro BLE) | read/notify Discovered Actuators `...152b` |
| `0x18` | `GATEWAY_CTRL` | 91->54 | `[cmd][arg...]` | write Gateway Control `...152c` |
| `0x19` | `GATEWAY_CTRL_RESULT` | 54->91 | `[seq_original][0=ok / err code]` | - |

Os registros de 41 bytes (`STATUS_RECORD`, `DISCOVERED_RECORD`) sao
**byte-a-byte identicos** ao formato ja documentado em
`PROTOCOLO_INTERFACE.md` (mesma pasta, mesmo layout de slot/state/MAC/nome).
Isso significa que o 91 pode reusar a mesma logica de parsing que a
interface JS ja tem, so trocando "veio de uma characteristic BLE" por
"veio de um frame UART" - o dado em si nao muda.

**Status de implementacao:**
- `nrf54-app/src/uart_link.c` (Fase 3): `0x02` (ACK, parte do framing)
  e `0x10`-`0x19` inteiros implementados e validados em bancada.
  `GATEWAY_CTRL_SET_NAME` (0x05, dentro do payload de `0x18`) e'
  aceito mas retorna erro "nao suportado" de proposito - ver
  "Limitacoes conhecidas" abaixo.
- `nrf91-app/src/uart_link.c`: na Fase 4 so enviava `0x14`/`0x16`
  (`GET_STATUS` em loop por todos os slots, `GET_DISCOVERED` com
  indice `0xFF`), disparados pelos botoes 1 e 2. Com a integracao MQTT
  (2026-08-26) passou a enviar tambem `0x10`/`0x12`/`0x18`
  (`uart_link_send_actuator_cmd/_manage`, `uart_link_send_gateway_ctrl`
  em `nrf91-app/src/uart_link.h`), disparados por comandos vindos do
  topico `cmd` - ver `PROTOCOLO_91_MQTT.md`. Recebe e decodifica
  `0x11`/`0x13`/`0x15`/`0x17`/`0x19` e publica cada um no MQTT.
- `CMD_LED` (0x01) **decidido que NAO vira heartbeat/debug** (a duvida
  que a proxima secao registrava) e **removido do `nrf91-app` na Fase
  4** (o botao->LED de brinquedo do exercicio deu lugar aos gatilhos
  reais `GET_STATUS`/`GET_DISCOVERED`). Do lado `nrf54-app`, um frame
  `0x01` recebido ainda cai no `default` do dispatch (logado como
  "opcode desconhecido", recebe ACK do mesmo jeito, nenhuma acao) -
  mas na pratica isso so aconteceria hoje se alguem religasse o
  firmware antigo do exercicio no 91 por engano.

## O link UART nao tem "conectado/desconectado" como a BLE - precisa de heartbeat

A BLE avisa o Gateway quando a interface conecta/desconecta
(`on_connected`/`on_disconnected`). A UART nao tem esse conceito: se o
91 travar ou reiniciar, o 54 nao fica sabendo sozinho, e vice-versa.
Proposta original: `CMD_LED`/opcode `0x01` (ou um novo `0x00 PING`)
vira um heartbeat periodico (ex.: a cada 5-10 s) nos dois sentidos; se
um lado parar de ver o heartbeat do outro por N ciclos, loga e (do
lado do 91) marca no MQTT que o Gateway esta inacessivel, em vez de a
nuvem achar que o silencio significa "nada mudou".

**Decidido na Fase 3:** reusar `CMD_LED` (0x01) pra isso, nao - ver a
nota de status de implementacao na secao anterior. **O mecanismo de
heartbeat em si continua em aberto/nao implementado** - fica pra
quando a Fase 4 (ou a integracao MQTT) precisar de verdade saber se o
outro lado esta vivo. Ate la, o unico jeito de perceber "o outro lado
sumiu" e' indireto: `ACK nao recebido...desisti apos 3 retransmissoes`
nos logs de quem tentou mandar algo.

## Inspiracao do firmware BLE (ControleCoesterBLE): ComScan

O painel atual (nRF52832) tem um motor generico pra falar com varios
dispositivos I2C (FSA, IO Digital, IO Analogica, Rede, sensores...):
`proCo/comScan.c`. Cada dispositivo se registra com 4 callbacks
(`getTX`/`getRX`/`confirmTX`/`acceptRX`) num motor comum, que roda um
ciclo fixo (15ms) com maquina de estado e **contagem de falha/taxa de
falha por dispositivo** (`countFailure`, `failureRate`,
`comScanOnlineDev`).

Vale reusar o *padrao* (motor generico + callback plugavel por
"dispositivo" + saude por dispositivo) no link 54<->91 - e ele ja rima
com o que `actuator_client.c` faz hoje (`struct actuator_manager_cb`).
Nao vale reusar o *barramento* (I2C força um ciclo fixo porque o
mestre precisa varrer todo mundo; UART ponto-a-ponto nao tem essa
restricao, e queremos manter a capacidade de push assincrono dos dois
lados - ver secao de framing acima).

O que falta hoje neste documento e existe no ComScan: rastreio de
saude por canal/opcode (taxa de falha, "esta offline?"), mais robusto
que so ACK+retry+desiste. Ainda nao desenhado - avaliar quando
implementar o `uart_link.c` de verdade.

## FOTA bidirecional (52->91 e 91->54, pelos dois canais)

Requisito confirmado (2026-08-06): FW do 54 atualizavel via BLE **ou**
via NB-IoT (relay pelo 91); FW do 91 com o mesmo principio. Isso muda
o link 54<->91: alem dos registros pequenos (<=250 bytes) que o
formato de frame acima cobre bem, precisa transportar imagem de
firmware inteira (centenas de KB) em algum momento.

Nao desenhar do zero: **investigar MCUmgr/SMP** (subsistema padrao do
Zephyr/NCS pra DFU) antes de inventar chunking+resume proprios - ja
tem transporte por UART e por BLE prontos, e integra com MCUboot pra
swap/rollback. Se servir, o "FOTA relay" pode virar so um transporte
SMP a mais, nao um novo protocolo do zero.

Em aberto: opcodes de FOTA provavelmente merecem faixa propria
(ex.: `0x20`+) separada da tabela principal, ja que o modelo de
transferencia (grande, em blocos, possivelmente com throughput maior
que o resto) e' bem diferente do resto do protocolo.

## Decisoes tomadas (eram "Em aberto" ate a Fase 3)

1. **Baud rate e tamanho de buffer real da UART**: 115200 nos dois
   lados (`current-speed` nos overlays, igual em `nrf54-app`,
   `nrf91-app` e `exercises/uart-link`); `PROTO_MAX_FRAME = 256` cabe
   o maior frame sem fragmentar em varios eventos `UART_RX_RDY`.
2. **Quem inicia o GET_STATUS/GET_DISCOVERED em loop no boot do 91**:
   implementado na Fase 4 - o gatilho e' o **botao fisico** do
   `nrf91-app` (botao 1 = `GET_STATUS` de cada slot 0..18 em
   sequencia via `uart_link_get_status_all()`, botao 2 =
   `GET_DISCOVERED` todos via `uart_link_get_discovered_all()`),
   marcado explicitamente como **temporario** no codigo/comentarios.
   Com o MQTT integrado, o gatilho principal passou a ser "ao conectar
   no broker" (`on_mqtt_connack()` em `nrf91-app/src/mqtt_client.c`),
   com o botao sobrando como forma manual de forcar resync.
3. **Topicos MQTT** (formato, 1 topico por atuador vs. agregado, QoS,
   retained ou nao) - decidido e implementado no documento irmao
   `PROTOCOLO_91_MQTT.md`, que espelha esta tabela de opcodes para os
   topicos.
4. **Onde mora o `uart_link.c` no `nrf54-app`**: `nrf54-app/src/uart_link.{c,h}`,
   registrado em `main()` logo apos `actuator_manager_init()` -
   diferente de `my_lbs_init()`/`actuator_manager_init()`, erro na
   inicializacao **nao e' fatal**: se o 91 nao estiver plugado na
   bancada, a interface BLE local continua funcionando sozinha.

## Limitacoes conhecidas (Fase 3)

- **`GATEWAY_CTRL_SET_NAME` (0x05) nao suportado via UART** - retorna
  `err=0xFE` de proposito. `bt_set_name()`/`gateway_name_save()` sao
  `static` em `main.c`; trocar nome do Gateway pela nuvem nao e'
  prioridade agora. Fica pra quando alguem precisar de verdade.
- **Concorrencia BLE x UART no mesmo slot**: nao ha nenhuma protecao
  nem log de aviso (ver secao "Concorrencia" acima - a mitigacao leve
  que estava proposta ali nunca foi implementada).
- **Heartbeat 54<->91**: nao implementado (ver secao anterior). O
  unico sinal indireto de "o outro lado sumiu" hoje e' o proprio log
  de retry esgotado.
- **`GET_DISCOVERED(0xFF)` nao tem terminador de lista**: quando o 91
  pede "todos", o 54 manda um `DISCOVERED_RECORD` por entrada
  encontrada (0 a `MAX_DISCOVERED`=12), mas nao existe nenhum campo ou
  frame que diga "acabou, eram N registros" - o 91 tem que descobrir
  sozinho quando parou de chegar coisa nova (ex.: por timeout apos o
  ultimo recebido). Gap real de protocolo, nao decidido ainda; nao
  bloqueou a Fase 3 porque o unico consumidor ate agora (RTT/log) nao
  precisa saber "quando parou".
- **`ACTUATOR_CMD` (0x10), `GET_STATUS` (0x14) e `GET_DISCOVERED`
  (0x16) nao tem frame de erro dedicado** - ao contrario de
  `ACTUATOR_MANAGE`/`GATEWAY_CTRL` (que sempre respondem com
  `MANAGE_RESULT`/`GATEWAY_CTRL_RESULT`, sucesso ou erro), estes tres
  simplesmente nao respondem nada se o slot/indice for invalido ou o
  atuador nao estiver pronto - so um log do lado do 54. O 91 precisa
  timeoutar sozinho pra perceber que um pedido nao vai ser atendido.
- **Nao existe opcode pra telemetria do proprio Gateway** (registrado
  em 2026-09-30). `PROTOCOLO_91_MQTT.md` decidiu publicar posicao/
  torque da propria unidade em `telemetry/self` a cada ~2,5s, mas a
  tabela acima so carrega dado de **slots** (atuadores geridos via
  BLE) - nada leva o dado local do 54 (`actuator_sensors_get()`) ate o
  91. Precisa de um opcode novo (54->91, push periodico ou sob pedido)
  antes de implementar `telemetry/self`.

## Achados de bancada da Fase 3 (2026-08-11 a 2026-08-13)

Sequencia real da depuracao, na ordem em que os problemas apareceram -
util pra quem for escrever `nrf91-app/src/uart_link.c` (Fase 4) ou
portar este link pra outro board, porque **o mesmo padrao de bug tende
a se repetir** se o codigo novo nao carregar estas licoes.

**1. Nome do overlay de board tem que bater com o target inteiro.**
`nrf54-app` builda pra `nrf54lm20dk/nrf54lm20b/cpuapp/ns` (TF-M);
`exercises/uart-link` builda pro mesmo target hoje (foi migrado de
`.../cpuapp` secure durante esta mesma investigacao). Um overlay
nomeado pro target errado (ex.: sem o `_ns`) nao da erro de build - o
Zephyr so ignora o arquivo, e o periferico fica desabilitado em
silencio. Ver o comentario grande no topo de
`nrf54-app/boards/nrf54lm20dk_nrf54lm20b_cpuapp_ns.overlay`.

**2. Pino errado no overlay: RX em P0.08, quando o correto e' P0.07.**
A primeira versao do overlay (copiada do exercicio) redefinia o
pinctrl da `uart30` do zero e errava: usava `UART_RX` em P0.08, mas o
pinctrl nativo do board (`nrf54lm20dk_nrf54lm20_a_b-pinctrl.dtsi`) diz
`UART_TX=P0.06, UART_RTS=P0.08` num grupo e `UART_RX=P0.07,
UART_CTS=P0.09` no outro - P0.08 e' RTS, nao RX, neste board. Corrigido
trocando a estrategia: **nao redefinir pinctrl nenhum**, so
`&uart30 { status = "okay"; };` - o board nativo ja entrega tudo
correto (`nrf54lm20dk_common.dtsi`), so faltava habilitar.

**3. ACK descartado em silencio ao colidir com uma transmissao em voo
(`-EBUSY`).** `uart_link.c` tem tres origens que chamam `uart_tx()`
(envio novo, retransmissao, envio de ACK) sem nenhuma coordenacao
entre elas. Confirmado no driver
(`zephyr/drivers/serial/uart_nrfx_uarte.c`, funcao `uarte_nrfx_tx`):
uma segunda chamada de `uart_tx()` enquanto outra transmissao esta em
voo retorna `-EBUSY` de forma limpa (nao corrompe nada), mas o
handler do ACK ignorava esse retorno - o ACK simplesmente sumia.
Corrigido com um gate (`tx_busy`) compartilhado entre as tres origens:
se colidir, o ACK fica marcado como pendente e e' reenviado assim que
`UART_TX_DONE`/`UART_TX_ABORTED` libera o canal fisico.

**4. O achado grande: UARTE do nRF54L/H (e do nRF91) perde/corrompe
byte sob latencia de interrupcao maior - BLE ativo e' o gatilho.**
Depois dos itens 1-3 corrigidos, o link continuava falhando quase 100%
das vezes com `nrf54-app` (BLE Central+Peripheral ativo: scan continuo
+ atuadores conectando), mas funcionava perfeito com **o mesmo fio,
mesmo pino**, rodando `exercises/uart-link` (sem Bluetooth nenhum) -
prova de bancada de que a fiacao nunca foi o problema. Diagnostico
confirmou a causa: desligar so o advertising/scan do `nrf54-app`
(radio ocioso) tambem resolveu 100% - apontando pra contencao de
interrupcao do controlador BLE, nao pra ruido de RF na fiacao.

A causa raiz exata (via base de conhecimento da Nordic): a UARTE do
nRF54L/H (EasyDMA) so troca de buffer de recepcao quando expira um
timeout de inatividade, e reiniciar o receptor nesse instante pode
perder/corromper o byte que estava chegando bem naquela hora -
documentado pela propria Nordic ("Reliable reception on nRF54L and
nRF54H Series"), e piora com mais latencia de interrupcao (exatamente
o efeito do radio BLE competindo por CPU). Confirmado tambem numa
thread da devzone da Nordic: outro usuario com o mesmo sintoma
(`UART_RX_STOPPED reason=4`, mesma correlacao com gaps de inatividade
entre mensagens, linha eletricamente limpa no osciloscopio dele).

**Fix - os dois lados do link precisam, cada um do seu jeito:**
- **`nrf54-app` (nRF54L/H)**: propriedade `timer` no node `uart30` do
  devicetree + `CONFIG_UARTE_NRFX_UARTE_COUNT_BYTES_WITH_TIMER=y` no
  `prj.conf`. `timer20` escolhido por estar livre (nada mais no
  projeto usa TIMER20-24). Ver
  `nrf54-app/boards/nrf54lm20dk_nrf54lm20b_cpuapp_ns.overlay`.
- **`nrf91-app` (nRF91, mecanismo diferente - gerenciado por Kconfig,
  sem propriedade de devicetree)**: `CONFIG_UART_1_NRF_HW_ASYNC=y` +
  `CONFIG_UART_1_NRF_HW_ASYNC_TIMER=2` no `prj.conf`. `TIMER2`
  escolhido por estar livre (TIMER0/1/2 sao os unicos no core de
  aplicacao do nRF9151; o tick do sistema usa RTC, nao essas TIMER).

**Isto nao e' opcional nem cosmetico.** Qualquer `uart_link.c` novo
(Fase 4 no `nrf91-app`, ou porte pra outro board da familia nRF54/nRF91)
precisa entrar com esta configuracao desde o primeiro commit - sem
ela, o link funciona perfeito em bancada isolada (sem BLE/modem
competindo por interrupcao) e falha quase 100% assim que o resto do
sistema real estiver rodando junto, um padrao de bug particularmente
enganoso porque "funcionava no meu teste" e some.

**TF-M (secure vs non-secure) NAO e' a causa.** Foi cogitado como
hipotese no meio da investigacao (o `nrf54-app` builda `.../cpuapp/ns`
com TF-M; o exercicio buildava `.../cpuapp` secure na epoca) e chegou
a ser mencionado como suspeita. Descartado: `exercises/uart-link` foi
migrado pra `.../cpuapp/ns` (TF-M) durante esta mesma investigacao e
continua funcionando sem o fix do timer - porque nunca teve BLE
competindo por interrupcao pra disparar o problema. A causa real e'
exclusivamente a combinacao "radio BLE ativo" + "UARTE sem o modo de
recepcao confiavel", independente de secure/non-secure.

**Validacao final em bancada (2026-08-13, com os dois fixes acima):**
teste de estresse com interface web + multiplos atuadores reais
conectados (180+ frames `DISCOVERED_RECORD` consecutivos, 100% de
sucesso, zero CRC invalido, zero `UART_RX_STOPPED`) e teste de
desconexao fisica do fio TX durante um pedido em voo (retry + dedup
funcionaram, sistema se recuperou sozinho sem travar nem perder
comando, so atrasar). Fase 3 considerada validada.

## Validacao da Fase 4 (2026-08-14)

`nrf91-app/src/uart_link.c` escrito do zero ja com o gate `tx_busy` do
achado #3 (evita reintroduzir o bug de ACK perdido) - `prj.conf` ja
tinha o `CONFIG_UART_1_NRF_HW_ASYNC`/`TIMER2` do achado #4 desde a
Fase 3, entao nao precisou de nenhuma mudanca de configuracao, so
codigo novo.

Testado em bancada com interface web + 1 atuador real conectado e
operando (comandos reais do proCo, respostas de 18/42/126 bytes):

- Botao 1 (`GET_STATUS`) e botao 2 (`GET_DISCOVERED`) decodificaram e
  imprimiram os registros certinho (MAC, RSSI, nome, estado do slot).
- `RAW_DATA` relayado via UART bateu **byte a byte** com o mesmo dado
  visto no log BLE nativo do `nrf54-app` (`<- TX do atuador`) pro
  mesmo comando - prova cruzada de que o dado chega identico pelos
  dois transportes.
- Sessao continua de mais de 6 minutos (scan + interface + atuador
  conectado) com apenas **1** `ACK nao recebido...desisti` isolado -
  taxa de falha residual compativel com um link fisico real, sem
  repetir o padrao de falha quase total que motivou a investigacao da
  Fase 3.

Fase 4 considerada validada. Na epoca faltava a integracao MQTT, feita
depois - ver `PROTOCOLO_91_MQTT.md`.

## Migracao para NCS 3.4.0 (2026-08-14)

Motivo: 3.4.0 e' LTS (5 anos de suporte), 3.3.0 nao e'. Migracao feita
so depois de Fases 1-4 completas e validadas em 3.3.0, deliberadamente
isolada da integracao MQTT - ver a nota sobre isso na secao anterior a
"Decisoes tomadas".

**O link UART em si (`timer20`/`CONFIG_UARTE_NRFX_UARTE_COUNT_BYTES_WITH_TIMER`
no `nrf54-app`; `CONFIG_UART_1_NRF_HW_ASYNC`/`TIMER2` no `nrf91-app`) nao
precisou de nenhuma mudanca** - as duas configs sobreviveram a migracao
identicas e continuam validadas (ver "Validacao final" abaixo). O
problema real da migracao foi outro, especifico do nRF91.

**Achado: `nrf91-app` ficava mudo (nem o banner de boot aparecia) apos
migrar.** Build e flash sem erro, mas nenhum log - sintoma identico ao
que outro usuario reportou na devzone da Nordic pra mesma migracao,
confirmado pela propria Nordic como comportamento esperado (nao bug,
so documentado tarde): a partir do 3.4.0, o layout de particao padrao
do nRF91 (`nrf91xx_partition.dtsi`) reserva um `boot_partition` no
endereco 0 pra um bootloader (MCUboot). Este projeto nunca usou
MCUboot - sem ele pra pular do endereco 0 pro slot real da imagem, o
chip tenta executar a `boot_partition` vazia e trava, sem log nenhum.
`erase`/`recover` nao ajuda (nao e' dado velho, e' layout de memoria
incompativel). Só afeta o nRF91 - o `nrf54-app` sempre bootou normal,
porque o layout do nRF54 nao tem essa reserva.

**Fix**: incluir no overlay do `nrf91-app`
(`nrf91-app/boards/nrf9151dk_nrf9151_ns.overlay`) o layout de particao
pronto da Nordic pra TF-M sem bootloader:

```
#include <samples/cellular/nrf91_crypto_partitions.dtsi>
#include <samples/cellular/nrf91_sram_crypto_partitions.dtsi>
```

**Atencao ao escolher a variante certa** - a Nordic oferece 4 layouts
de flash em `nrf/dts/samples/cellular/`, e a variante "minima"
(`nrf91_no_bootloader_partitions.dtsi`) parece a escolha obvia mas
**nao serve aqui**: ela faz `/delete-node/` nas particoes de Protected
Storage/ITS/OTP do TF-M, e o Kconfig padrao deste board target ja
habilita `CONFIG_TFM_PARTITION_PROTECTED_STORAGE` - sem essas
particoes o build do TF-M falha (`TFM_HAL_PS_FLASH_AREA_ADDR must be
defined`). A variante `nrf91_crypto_partitions.dtsi` mantem essas
particoes intactas, so redefine o layout do app - foi a que funcionou.

**Falso alarme no meio do caminho**: depois do fix acima, o 91 voltou
a bootar, mas o link UART continuou falhando 100% dos dois lados -
por um instante pareceu que a migracao tinha quebrado a recepcao
confiavel tambem. Causa real: o fio TX/RX entre as placas tinha sido
trocado de porta durante o manuseio fisico (varios ciclos de
erase/recover/reflash) - nada a ver com o SDK. Fica registrado aqui
como lembrete: depois de qualquer sessao de manuseio fisico intenso
das placas, reconferir a fiacao antes de suspeitar do software.

**Validacao final em bancada (2026-08-14, fiacao corrigida)**: teste
de estresse e teste de desconexao fisica do fio TX, os dois passando
sem nenhuma falha - migracao pro NCS 3.4.0 considerada concluida e
validada.
