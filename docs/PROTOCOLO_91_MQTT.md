# Contrato MQTT (nRF9151 <-> nuvem) - espelha `docs/PROTOCOLO_54_91.md`

**Status (2026-08-26): implementado, validado parcialmente em
bancada** - conectou e publicou certinho no broker
`mqtt.nordicsemi.academy` por um periodo (topicos atualizando com o
SIM Onomondo, confirmado em bancada), mas **bloqueado agora por
rejeicao de rede (EMM cause 8) - nao e' bug de firmware**, ver
"Achados de bancada - LTE/SIM" mais abaixo. O codigo (`mqtt_client.c`,
`uart_link.c`) esta pronto e ja foi validado funcionando quando o
registro LTE funcionava. Objetivo confirmado com o usuario: paridade
completa com a interface local via MQTT - leituras periodicas de tudo
que o `nrf54-app` coleta dos atuadores, mais leitura sob demanda de
dados especificos (curvas, eventos, etc.) quando a nuvem pedir.

Arquivos novos: `nrf91-app/src/mqtt_client.{c,h}`,
`nrf91-app/Kconfig`, `nrf91-app/credentials/ca-cert.pem`.
`nrf91-app/src/uart_link.c` ganhou as 3 funcoes de envio que faltavam
(`uart_link_send_actuator_cmd/_manage`, `uart_link_send_gateway_ctrl`)
e o dispatch de frame recebido passou a desviar pra system workqueue
(`rx_record_work`) em vez de rodar direto no ISR da UART - descoberto
durante a implementacao que `mqtt_client_publish_*()` faz `send()` de
socket via o modem, que nao e' ISR-safe (mesma categoria de restricao
documentada pro Bluetooth host no lado do 54).

## Base de codigo reaproveitada

`C:\NordicAcademy\cell-fund\l4\l4_e2` (Nordic Academy, Cellular IoT
Fundamentals, Licao 4 Exercicio 2) - padrao oficial recomendado pela
propria NCS: `mqtt_helper` (`net/mqtt_helper.h`) + `lte_lc_connect_async`
+ TLS com certificado provisionado no modem via `modem_key_mgmt` +
client ID derivado do IMEI (`AT+CGSN`). Reaproveitado quase 1:1:
`modem_configure()`, `client_id_get()`, `certificate_provision()`, o
formato dos callbacks (`on_mqtt_connack`/`on_mqtt_publish`/`on_suback`/
`on_mqtt_disconnect`), e o helper `publish()`.

## Broker (para desenvolvimento agora)

`mqtt.nordicsemi.academy` - mesmo broker do exercicio, TLS, cert em
`credentials/ca-cert.pem` (copiado do exercicio, e' o CA publico do
broker, nao segredo). **E' um broker de sandbox de curso, compartilhado
com outros alunos - nao serve pra dado de campo real.** Trocar por um
broker de producao (AWS IoT / Azure IoT / self-hosted) e' decisao
futura, fora de escopo agora; a troca deve ser so de configuracao
(`Kconfig`/`prj.conf`), sem mudar nada da logica de topicos/payload
abaixo.

## Client ID (identidade da conexao MQTT, separado do prefixo de topico)

`nrf-<IMEI>` (mesmo padrao do exercicio, via `AT+CGSN`) - usado como
`device_id`/client ID da conexao MQTT em si (identidade de sessao,
importa pro broker nao confundir duas conexoes simultaneas). **Nao** e'
mais usado como prefixo de topico (ver abaixo) - os dois papeis foram
separados quando o prefixo virou uma string fixa (`topicGDF`) escolhida
pelo usuario em vez de derivada do IMEI. Se um dia houver mais de um
Gateway publicando no mesmo broker, o prefixo de topico volta a
precisar de algo unico por dispositivo (o client ID ja serve pra isso).

## Topicos - publicacao (Gateway -> nuvem)

Prefixo comum: `topicGDF/` (nome simples e visivel escolhido pelo
usuario pra facilitar achar o proprio trafego num broker compartilhado
de curso, em vez do `gateway_id` derivado de IMEI que a primeira
versao deste documento propunha - configuravel via Kconfig
(`CONFIG_SIMCONNECT_MQTT_TOPIC_PREFIX`), trocar e' so mudar essa
string quando houver um broker de producao com mais de um Gateway
publicando ao mesmo tempo, quando o prefixo por dispositivo volta a
importar).

| Topico (sem o prefixo) | Quando publica | Retained | QoS | Payload |
|---|---|---|---|---|
| `actuator/<slot>/status` | Toda vez que chega `STATUS_RECORD` (0x15) - automatico, sem pedir nada (o 54 ja empurra sozinho quando o estado de um slot muda) | **sim** | 1 | `{"slot":N,"state":"READY","addr_type":1,"mac":"AA:BB:CC:DD:EE:FF","name":"..."}` |
| `discovery/<mac_sem_dois_pontos>` | Toda vez que chega `DISCOVERED_RECORD` (0x17) | **sim** | 1 | `{"addr_type":1,"mac":"AA:BB:CC:DD:EE:FF","rssi":-70,"flags":0,"name":"..."}` |
| `actuator/<slot>/raw` | Toda vez que chega `RAW_DATA` (0x11) - resposta crua do proCo (posicao/torque, dados gerais, curva, alarme...) | nao | 1 | `{"slot":N,"data":"<hex>"}` |
| `result` | Toda vez que chega `MANAGE_RESULT` (0x13) ou `GATEWAY_CTRL_RESULT` (0x19) - confirmacao de um comando pedido pela nuvem | nao | 1 | `{"op":"actuator_manage"\|"gateway_ctrl","seq":N,"err":0}` |

**`retained` em `status`/`discovery`**: proposital - um assinante novo
(ou que reconectou) ve o ultimo estado conhecido de cada slot/atuador
descoberto imediatamente, sem precisar esperar o proximo evento ou
pedir `get_status_all` manualmente. `raw`/`result` sao eventos
pontuais, nao faz sentido reter.

**Payload de `raw` fica cru de proposito (hex, sem parsing)** - mesma
filosofia ja documentada em `actuator_client.c`
("Sem NENHUM parsing do protocolo proCo aqui"): quem decodifica
posicao/torque/curva/alarme e' o consumidor na nuvem, usando a mesma
logica que ja existe no `coleta_ble`/interface JS pro dado que chega
por BLE. O firmware so transporta.

## Topico - subscricao (nuvem -> Gateway)

`topicGDF/cmd` - um unico topico, dispatch pelo campo
`"op"` do JSON. QoS 1.

| `op` | Payload adicional | Espelha (UART/BLE) |
|---|---|---|
| `get_status` | `{"slot":N}` | `GET_STATUS` (0x14) de 1 slot |
| `get_status_all` | - | `GET_STATUS` (0x14) de todos os slots em sequencia - mesma funcao ja usada pelo botao 1 (`uart_link_get_status_all()`) |
| `get_discovered` | - | `GET_DISCOVERED` (0x16) indice `0xFF` - mesma funcao ja usada pelo botao 2 (`uart_link_get_discovered_all()`) |
| `actuator_cmd` | `{"slot":N,"data":"<hex>"}` | `ACTUATOR_CMD` (0x10) - **e' o pedido de dado especifico sob demanda** (curva, evento, etc.) - bytes crus do comando proCo, opacos, vindos prontos da nuvem |
| `actuator_manage` | `{"cmd":1\|0,"addr_type":N,"mac":"AA:BB:CC:DD:EE:FF"}` | `ACTUATOR_MANAGE` (0x12) - adicionar/remover da allow-list |
| `gateway_ctrl` | `{"cmd":N,"arg":"<hex>"}` | `GATEWAY_CTRL` (0x18) - `arg` hex opcional dependendo do `cmd` (ver tabela em `PROTOCOLO_54_91.md`) |

**`actuator_cmd` e' o mecanismo dos "dados sob demanda" pedidos** (o
segundo requisito do usuario, alem das leituras periodicas): a nuvem
manda o comando proCo cru (mesmo formato que a interface local ja
manda via characteristic Actuator Command `...1528`), o Gateway
repassa pro atuador via `ACTUATOR_CMD`, a resposta volta como
`RAW_DATA` no topico `actuator/<slot>/raw` de publicacao - a nuvem
correlaciona pelo `slot`, igual o proprio protocolo 54<->91 ja faz.

## Funcoes de envio do `uart_link.c` do 91 (implementadas)

Na Fase 4, `nrf91-app/src/uart_link.c` so enviava `GET_STATUS`/
`GET_DISCOVERED`. Pra "dados sob demanda" funcionar, foram adicionadas
(ja implementadas, chamadas pelo dispatch de `cmd` em `mqtt_client.c`):
- `uart_link_send_actuator_cmd(uint8_t slot, const uint8_t *data, uint8_t len)`
- `uart_link_send_actuator_manage(uint8_t cmd, uint8_t addr_type, const uint8_t mac[6])`
- `uart_link_send_gateway_ctrl(uint8_t cmd, const uint8_t *arg, uint8_t arg_len)`

Espelham exatamente as funcoes que ja existem do lado do 54
(`actuator_manager_send_to()` etc.) - so trocam o transporte de saida
(BLE local vira MQTT vindo de fora).

## Gatilho de sincronizacao inicial (substitui o botao da Fase 4)

Ao conectar no broker (`on_mqtt_connack`, `return_code ==
MQTT_CONNECTION_ACCEPTED`): chama `uart_link_get_status_all()` +
`uart_link_get_discovered_all()` uma vez, pra sincronizar o estado
conhecido assim que a nuvem tem alguem ouvindo. Os botoes 1/2
continuam existindo, viram forma manual de forcar resync (decisao ja
tomada em `PROTOCOLO_54_91.md`, "Decisoes tomadas" item 2).

## Achados de bancada - LTE/SIM (2026-08-26)

**Nao e' bug de firmware.** O MQTT chegou a conectar e publicar
certinho no broker por um periodo (topicos atualizando com o SIM
Onomondo, confirmado em bancada) - depois o registro na rede LTE
parou de funcionar, com o modem sendo rejeitado pela rede antes de
chegar a tentar o MQTT:

```
[00:00:08.778,778] <inf> mqtt_client: Network registration status: procurando rede (2)
[00:00:09.069,976] <inf> mqtt_client: RRC mode: Connected
[00:00:09.653,045] <wrn> lte_lc: Registration rejected, EMM cause: 8, Cell ID: ..., Tracking area: ..., LTE mode: 7
[00:00:09.653,076] <inf> mqtt_client: Network registration status: nao registrado (0)
```

### Isolamento (descarta bug no nosso codigo)

Testado o exercicio de referencia `l4_e2` (Nordic Academy Licao 4
Exercicio 2), **sem nenhuma alteracao nossa**, com os 3 SIMs que
vieram com os kits nRF9151-SMA-DK (enviados pelo Joao Dulius,
engenheiro Nordic), em Sao Leopoldo/RS:

| SIM | Resultado | EMM cause |
|---|---|---|
| Onomondo | Rejeitado | 8 - "EPS services and non-EPS services not allowed" |
| Deutsche Telekom | Rejeitado | 8 - mesma torre do Onomondo (TAC 42751) |
| Monogoto | Rejeitado | 15 - "No suitable cells in tracking area" |

Os 3 falharem no exercicio intocado descarta bug no
`nrf91-app`/`mqtt_client.c` - confirmado tambem via
`AT+CEREG=5`/`AT+CEREG?` batendo com os mesmos Cell ID/TAC dos logs.

### Diagnostico (Joao Dulius, Nordic, por e-mail, 2026-08-26)

1. **Os 3 testes nao sao independentes.** EMM cause 8 e' classificada
   pelo 3GPP como rejeicao permanente, o que ativa o Radio Policy
   Manager (RPM, exigido pela GSMA TS.34) - depois da primeira
   rejeicao, o modem passa a permitir poucas tentativas de attach por
   hora (tipicamente 1/hora). Ou seja, o resultado do 2o e 3o SIM ja
   saiu com orcamento de tentativas reduzido - nao sao medicoes
   limpas. Isso e' o mesmo mecanismo do RESET_LOOP ja documentado na
   migracao NCS 3.4.0, so que disparado pela rede em vez de por reset
   local repetido.
2. Cause 8 nao distingue o motivo exato - dois candidatos, ambos
   plausiveis aqui:
   - **SIM nao ativado**: o SIM da Deutsche Telekom vem documentado
     como "Registration required to activate", diferente do
     Monogoto/Onomondo, que vem pre-registrados - falta confirmar se
     o procedimento de ativacao foi feito.
   - **Franquia de dados encerrada**: o Onomondo vem com franquia
     pequena. Funcionar por um tempo (confirmado - MQTT publicou
     certinho por dias) e depois parar bate com esse cenario.
     Registrar o SIM na conta Onomondo (QR code do kit) amplia a
     cota.
3. **Cause 15 (Monogoto) e' outra coisa** - nao e' questao de
   assinatura, e' o modem nao achando celula adequada na tracking
   area. Reavaliar depois, com o modem "limpo" (RPM zerado).

### Procedimento recomendado (evitar queimar tentativas de attach a toa)

Antes de qualquer novo teste:
```
AT+CNEC=24     # reporta causa EMM/ESM exata via +CNEC_EMM
AT+CGEREP=1    # avisa quando o RPM entra em vigor: +CGEV: RESTR 1,2
```
Ao receber cause 8: **nao reiniciar**. `AT+CFUN=4`, esperar 20-30
min, reativar - reiniciar na hora nao adianta (a assinatura nao vai
ter sido corrigida nesse intervalo). Se o RPM avisar que a restricao
esta ativa, esperar ele mesmo liberar (o modem reinicia internamente
quando a restricao acaba). **Nao cortar energia do kit nesse
estado** - sem alimentacao o modem perde a nocao de quanto tempo
ficou fora e nao sabe se a restricao ja expirou.

### Encaminhamento

Por recomendacao do Joao, pra nao travar o desenvolvimento na
franquia limitada do kit, contato feito com dois fornecedores de SIM
de demonstracao (LTE-M + NB-IoT terrestre no Brasil):
- **emnify** - Jefferson Soares (jefferson.soares@emnify.com) - ja
  tem integracao NTN/satelite pronta, relevante se houver visao de
  cobertura satelital no roadmap.
- **Virtueyes** - Pamela Ritter (pamela.ritter@virtueyes.com.br)

Aguardando resposta (2026-08-26).

## Orcamento de dados e arquitetura de telemetria periodica (decisao de design, 2026-09-04)

**Ainda nao implementado** - decisao de arquitetura tomada, discutida
com o usuario, documentada aqui antes de codar. Motivado pela pergunta
"quanto de dado consome um Gateway com N slots ocupados", que expos
dois achados que mudam bastante o desenho original deste documento.

### Achado 1: keepalive MQTT nao configurado (`CONFIG_MQTT_KEEPALIVE`)

`nrf91-app/prj.conf` nunca definiu essa opcao - fica no default do
Zephyr (60s). Isso sozinho gera um PINGREQ/PINGRESP por minuto, o
tempo todo, **independente de quantos slots estao ocupados**:
```
1440 pings/dia x ~200 bytes (TLS+TCP/IP+MQTT) ~= 8,4 MB/mes
```
so' de keepalive, sem nenhum atuador conectado. Trocando pra 1200s (20
min - mesmo teto que a AWS IoT recomenda, comum em exemplos nRF91),
cai pra ~430 KB/mes. **Precisa ser setado explicitamente antes de
qualquer medicao de consumo fazer sentido** - ainda nao aplicado no
`prj.conf`.

### Achado 2: publicar 1 mensagem MQTT por leitura, por slot, e' caro demais pra telemetria de alta frequencia

O padrao atual (`actuator/<slot>/raw`, uma mensagem por evento) faz
sentido pra eventos esporadicos (mudanca de estado, resultado de
comando), mas quebra pra posicao/torque em alta frequencia: o
overhead fixo de protocolo (~80 bytes de MQTT+TLS+TCP/IP) domina um
payload real de ~20-30 bytes. Testado com os requisitos reais do
usuario:

| Coleta | Frequencia definida | Custo "ingenuo" (1 msg/slot/leitura) |
|---|---|---|
| Posicao/torque (por atuador) | 5-10s | ~35-63 MB/mes **por slot** |
| Alarme | 60s | ~5 MB/mes por slot |
| Dados gerais | 300s (5 min) | ~1,6 MB/mes por slot |
| Curvas/eventos | so' sob demanda | custo pontual, ja' coberto por `actuator_cmd`/`raw` |

Pra 20 slots so' de posicao/torque, isso da' ~700 MB a ~1,3 GB/mes -
inviavel. Motivo: mensagem pequena demais pro overhead que carrega.

### Decisao: telemetria de posTor agrupada, self do Gateway separado

1. **Alarme e dados gerais continuam no padrao atual** (`raw`, um por
   evento) - custo baixo, sem necessidade de agrupar.
2. **Curvas/eventos continuam so' sob demanda** (`actuator_cmd` ->
   `raw`) - ja' implementado, sem mudanca.
3. **Posicao/torque dos slots (atuadores geridos via BLE/proCo) passa
   a ser agrupada**: o `nrf91-app` acumula as leituras que forem
   chegando do 54 por um ciclo (5-10s) ou um timeout, e publica **uma
   unica mensagem** com todos os slots daquele ciclo, em vez de uma
   por slot. Payload compacto (chaves curtas, nao reaproveita o hex
   passthrough do `raw` generico), QoS 0 (perder uma leitura pontual
   de posicao nao e' grave, e evita o round-trip do PUBACK):
   ```
   topicGDF/telemetry/actuators   (novo topico, proposto)
   {"t": <uptime_ms>, "d": [{"s":1,"p":1234,"tq":56}, {"s":2,...}, ...]}
   ```
   Com isso, 20 slots a 10s ficam em ~155 MB/mes (contra ~700 MB/mes
   no esquema ingenuo) - o overhead de protocolo e' pago uma vez por
   ciclo, nao uma vez por slot.
4. **Achado a parte: o proprio SIM Connect (Gateway) e' tambem um
   atuador (motor GPIO-direto), e nao tinha lugar nesse esquema** - so'
   existiam `slots` pros atuadores geridos via BLE. Posicao/torque do
   proprio Gateway ja e' amostrado localmente no 54 a **15ms**
   (`nrf54-app/src/sensor_workq.c`, `position_sensor.c`, `ads1000.c` -
   fila I2C dedicada, ver o comentario do bug corrigido em 26/08) - ou
   seja, a resolucao/amostragem ja atende de sobra o requisito (o
   usuario pediu "menos de 1s", ja' esta' 66x mais rapido que isso).
   O que faltava era so' a cadencia de **publicacao** ate' a nuvem.
   Definido com o usuario: latencia ponta-a-ponta de 2-3s e' aceitavel
   pra esse dado. Decisao: mensagem propria, dedicada, publicada a
   cada ~2,5s, pegando o valor mais recente do cache de 15ms no
   momento do envio - nao precisa esperar o ciclo dos slots:
   ```
   topicGDF/telemetry/self   (novo topico, proposto)
   {"t": <uptime_ms>, "p": <pos>, "tq": <torque>}
   ```
   Isso fica em ~88 MB/mes sozinho (34.560 msgs/dia a ~85 bytes).

### Orcamento total estimado

```
~155 MB/mes (20 slots, posTor agrupado, ciclo 10s)
+ ~88 MB/mes (posTor do proprio Gateway, ciclo 2,5s)
+ alarme/dados gerais/curvas (baixo, ja' cobertos acima)
------------------------------------------------------
~240 MB/mes (cenario: 20 slots ocupados, uso tipico)
```
Numero de referencia pra dimensionar plano de dados com
emnify/Virtueyes (ver secao anterior) - fora da escala de franquia de
curso, precisa de plano M2M de verdade.

**Importante**: sao estimativas de engenharia (contagem de bytes de
protocolo + frequencias definidas pelo usuario), nao medicao de
bancada. Vale medir consumo real quando os topicos novos estiverem
implementados e rodando.

## Em aberto / nao decidido ainda

1. **Broker de producao** - qual, credenciais, TLS real (nao o cert de
   sandbox do curso). Bloqueado ate existir infra de nuvem definida.
2. **Heartbeat/LWT** - `mqtt_helper_conn_params` nao tem campo de last
   will hoje; pra marcar "Gateway offline" de verdade na nuvem quando a
   conexao cai sem `DISCONNECT` limpo, precisaria descer pra API bruta
   do MQTT (`zephyr/net/mqtt.h`) em vez do helper. Fica junto do gap de
   heartbeat 54<->91 ja documentado (nao implementado ainda).
3. **Reconexao apos queda de LTE/MQTT** - o exercicio de referencia so
   conecta uma vez no `main()`; falta logica de retry/reconexao pra
   operar sem supervisao continua. Necessario antes de considerar isto
   pronto pra campo.
4. **Tamanho de payload vs. MTU do plano de dados celular** - resolvido
   pra `raw`/eventos pontuais (JSON com hex, ~500 bytes no pior caso,
   trivial pra LTE-M/NB-IoT). Pra telemetria periodica, ver secao
   "Orcamento de dados" acima - decisao tomada (agrupar por ciclo),
   falta implementar.
5. **`CONFIG_MQTT_KEEPALIVE` nao setado** - default Zephyr (60s), ~8,4
   MB/mes so' de keepalive. Trocar pra 1200s antes de qualquer medicao
   de consumo de dados fazer sentido (ver "Orcamento de dados" acima).
   **Pendencia registrada** (2026-09-30) na lista do `README.md`.
6. **Telemetria agrupada (`telemetry/actuators`, `telemetry/self`) -
   implementar**: acumulador de leituras por ciclo no `nrf91-app`
   (buffer + timer/timeout de flush), schema compacto novo (nao reusar
   `raw` hex), QoS 0. Depende de decisao de payload binario vs. JSON
   compacto se o orcamento de ~240 MB/mes precisar ser espremido mais.
   **`telemetry/self` depende tambem de um opcode novo no link UART**:
   hoje nenhum opcode leva a posicao/torque do proprio 54 ate o 91 -
   ver "Limitacoes conhecidas" em `PROTOCOLO_54_91.md`.
7. **SIM / plano de dados** - validacao em bancada parada por rejeicao
   de rede (ver "Achados de bancada - LTE/SIM"). Contato com emnify e
   Virtueyes feito em 2026-08-26; sem retorno registrado neste
   documento ate 2026-09-30.
