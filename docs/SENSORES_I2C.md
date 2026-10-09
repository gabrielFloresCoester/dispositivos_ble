# Sensores I2C do papel atuador — status e decisões

Registro da portagem dos sensores I2C do firmware BLE (`fwBLE`,
repositório `ControleCoesterBLE`, protocolo proCo) para o `nrf54-app`,
dentro do papel de **atuador** descrito em `DECISOES_PRODUTO.md`. Antes
deste trabalho, `nrf54-app` só tinha o papel de Gateway (BLE) - nenhum
código I2C existia no repositório.

**Nota sobre terminologia** (confirmada com o Felipe, 2026-08-21):
não existe "Gateway legado" em campo. O que existe em campo são
unidades **Atuador BLE** (linha BLE, `fwBLE`, nRF52832 + SoftDevice) -
são atuadores, não gateways. O `nrf54-app` deste repositório é o
**primeiro Gateway** que existe. Pela decisão de "produto único"
(`DECISOES_PRODUTO.md`), esta mesma unidade também precisa, no seu
papel de atuador, expor seus dados locais via BLE **do mesmo jeito que
um Atuador BLE real já faz** - não por "compatibilidade com legado",
mas pra que qualquer Gateway (o deste projeto, gerenciando uma frota
que mistura Atuadores BLE antigos com unidades SIM Connect novas em
papel de atuador) consiga tratar os dois tipos de atuador de forma
idêntica, sem variação no `actuator_client.c`.

**Nota sobre confiabilidade deste documento**: como os demais docs
desta pasta, este foi escrito por uma sessão do Claude, não verificado
por um humano campo a campo contra o firmware. Onde possível, as
afirmações abaixo foram checadas direto no código-fonte do
`ControleCoesterBLE` (caminhos e trechos citados) - mas trate como
pista pra verificar de novo, não como verdade definitiva, se for base
de uma decisão importante.

## Sensores implementados

| Sensor | Endereço proCo (8 bits) | Endereço Zephyr (7 bits) | Ganho/protocolo |
|---|---|---|---|
| Posição (potenciômetro) | `ENDER_SENSOR_POSICAO` `0x92` | `0x49` | ADS1000, ganho `0` (fixo) |
| Torque (analógico) | `ENDER_SENSOR_TORQUE` `0x90` | `0x48` | ADS1000, `paramDado.fabGanSenTorq` - default `0`, configurável `0`-`3` |
| Torque (ON/OFF) | `ENDER_SENSOR_TORQUE_ONOFF` `0x82` | `0x41` | PCA9536 (I/O expander), sem ganho |

Os dois sensores de torque são **variantes de BOM mutuamente
exclusivas** - só um dos dois está fisicamente presente por unidade
(ver "Seleção automática de fonte de torque" mais abaixo). Posição e
torque analógico são o mesmo chip (TI ADS1000, ADC I2C). Confirmado
lendo o driver de produção real (`BLE/proCo/devices/devs/dev_ads.c` e
`devices.c` no `ControleCoesterBLE`), não só os exercícios didáticos da
Nordic Academy (`ncs-fund/l6/l6_e1`, `ncs-inter/l7_e1`, que serviram de
ponto de partida mas continham uma simplificação incorreta - ver
abaixo).

## Endereço: 8 bits (proCo) vs 7 bits (Zephyr)

`devices.h` do proCo declara endereços de 8 bits (convenção clássica
8051/PIC: 7 bits do endereço + bit de R/W já embutido em bit 0). A API
I2C do Zephyr usa 7 bits. Conversão: endereço proCo `>> 1`. Ex.:
`0x92 >> 1 = 0x49`, `0x90 >> 1 = 0x48`.

## Achado importante: o protocolo real é mais rico que o exercício didático

A primeira versão deste módulo seguiu o padrão do exercício da Academy
(`l6_e1`/`l7_e1`): escrever 1 byte "ponteiro" `0x00` e ler 2 bytes -
comunicava (o valor variava com o potenciômetro), mas **não é** o
protocolo do firmware de produção. Conferido direto em `dev_ads.c`:

- O ADS1000 **não tem conceito de ponteiro de registrador** (isso é
  coisa de chip tipo BME280). Qualquer escrita de 1 byte É a escrita
  da configuração - não existe escrita "neutra". O exercício da
  Academy, ao escrever `0x00` antes de cada leitura, estava sem querer
  reconfigurando o ADS1000 a cada ciclo.
- O driver real escreve a config
  (`ADS1000_CONFIG_DEFAULT=0x80` OR'ado com o ganho do dispositivo) só
  uma vez / quando precisa reconfigurar - não a cada leitura.
- Cada leitura pede **3 bytes**, não 2: `[MSB][LSB][echo da config]`.
  O 3º byte é conferido contra a config esperada - se não bater, o
  ADS1000 "esqueceu" a configuração (glitch elétrico, power-on) e o
  driver volta a escrevê-la antes de confiar em leituras futuras
  (mecanismo de auto-recuperação).

Isso importa especialmente pro sensor de torque: seu ganho
(`paramDado.fabGanSenTorq`) é um parâmetro de calibração por unidade,
não o default de fábrica do ADS1000. Sem escrever essa config, os
valores brutos lidos não estariam na mesma escala que o fwBLE usa -
exatamente o tipo de incongruência que motivou essa correção antes de
portar o segundo sensor.

## Arquitetura: motor genérico `ads1000.c`

Com 2 sensores reais confirmados (mesmo chip, só endereço/ganho
diferentes), o protocolo foi extraído para um motor genérico
(`nrf54-app/src/ads1000.c`), parametrizado por instância
(`struct ads1000_dev`: barramento I2C, endereço, ganho, nome pra log).
`position_sensor.c` e `torque_sensor.c` são instâncias finas desse
motor.

Cada instância roda seu próprio `k_work_delayable` periódico (15ms -
mesmo `TEMPO_CICLO_MS` do motor ComScan legado, ver
`ComDispositAtuador.h`), mantendo um cache do último valor válido.
`position_sensor_last_raw()`/`torque_sensor_last_raw()` só leem esse
cache, não acessam o I2C na hora.

**Diferença deliberada do ComScan original**: o ComScan legado
(`comScan.c`) faz revezamento manual entre dispositivos
(`execDevList`, `comScanNextDev`) porque é um super-loop single-thread
sem SO - só um dispositivo pode estar "em trânsito" no barramento por
vez, e o código precisa saber disso explicitamente. No Zephyr, o
driver I2C já serializa o acesso físico ao barramento entre chamadas
concorrentes de threads/work items diferentes com segurança - por
isso cada sensor tem seu work item independente, sem nenhuma
coordenação manual entre eles. A parte que **foi** replicada
fielmente é o protocolo por dispositivo (bytes, config, echo) e o
padrão de auto-recuperação por falha - não o agendamento round-robin.

## Bug corrigido: pollers I2C na fila do sistema causavam travamentos erráticos

Reportado pelo Felipe em bancada (2026-08-26), com os 3 sensores
(posição, torque analógico, torque ON/OFF) ligados ao mesmo tempo:
comportamento errático - às vezes "trava", às vezes a interface não
conecta, às vezes conecta mas não atualiza, às vezes nem loga.

**Causa raiz**: os 3 pollers I2C (`ads1000.c` - posição e torque
analógico - e `torque_onoff_sensor.c`) usavam `k_work_schedule()`, que
agenda na fila de trabalho **do sistema** (`k_sys_work_q`), fazendo E/S
I2C bloqueante a cada 15ms **dentro dela**. A fila do sistema roda em
prioridade tipicamente **cooperativa** por padrão no Zephyr
(`CONFIG_SYSTEM_WORKQUEUE_PRIORITY`, negativa) - enquanto ela está
executando um work item, nenhuma thread preemptível comum (`main()`,
por exemplo) roda, e o host Bluetooth do Zephyr também depende dela
para várias coisas, incluindo a própria advertising deste projeto
(`adv_work` em `main.c`).

Em bancada, com sensores parcialmente conectados/desconectados durante
o teste (cenário normal ao validar a seleção automática de torque), uma
transação I2C sem ACK pode disparar recuperação de barramento
(bit-banging manual de SCL/SDA) **dentro da fila do sistema** - um
bloqueio de dezenas de ms sem ceder a CPU. Isso explica os 4 sintomas
de uma vez: advertising atrasada (não conecta), host Bluetooth atrasado
(conecta mas não atualiza), e o próprio `main()` (preemptível) incapaz
de rodar enquanto a fila do sistema estivesse presa (nem loga / parece
travado).

**Correção** (`src/sensor_workq.{c,h}`, novo): os 3 pollers I2C passam
a rodar numa fila de trabalho **dedicada**, com sua própria thread, em
prioridade preemptível normal (`k_work_schedule_for_queue()`/
`k_work_reschedule_for_queue()`, não mais as versões sem `_for_queue`)
- isola qualquer lentidão/recuperação de barramento I2C do resto do
sistema (BLE, `main()`). `main.c` chama `sensor_workq_init()` antes de
`position_sensor_init()`/`torque_sensor_init()`/
`torque_onoff_sensor_init()`. `actuator_sensors.c` continua na fila do
sistema de propósito - só lê caches, nunca acessa o I2C, rápido o
bastante para não incomodar ninguém.

### Diagnóstico: como separar "travamento" de "sensor sem leitura válida"

Durante a investigação acima, o log de bancada ficava em silêncio total
por dezenas de segundos (nenhuma linha de posição/torque/calibrado),
indistinguível à primeira vista de um travamento real do `main()` -
porque TODAS as linhas de sensor no loop de bancada só aparecem quando
há dado válido (`LOG_INF`), e o fallback (`LOG_DBG`, "sem leitura
válida ainda") é invisível no nível de log padrão (`LOG_LEVEL_INF`).

Pra separar as duas hipóteses de vez, foi usado (temporariamente,
removido depois) um log **incondicional** a cada 1s no `main()`,
mostrando `device_health_count_transaction()`/
`_count_failure_sequent()` de cada backend (`position_sensor_health()`/
`torque_sensor_health()`/`torque_onoff_sensor_health()`, novos getters
- ver `device_health.h`). Isso provou, com dado real: o `main()` **nunca
parou** de rodar (o contador por segundo nunca falhou), mas os 3
sensores ficavam com `count_failure_sequent` no teto (10) o tempo
inteiro - ou seja, zero transações bem-sucedidas, não um travamento.

**Causa raiz real, no caso do Felipe**: mau contato no cabo do sensor
de posição (confirmado - `seqfail` foi de 10 pra 0 assim que o contato
foi refeito). Torque analógico/ON-OFF não estavam conectados no
momento, então seu `seqfail=10` constante era o comportamento CORRETO
esperado (ver nota abaixo sobre o log de boot "pronto" ser enganoso).
Nenhum bug de firmware nos backends em si foi encontrado - o log de
boot só parecia contraditório por causa da redação da mensagem, não por
comportamento errado.

**Lição gravada em código**: os getters de saúde
(`ads1000_health()`/`position_sensor_health()`/`torque_sensor_health()`/
`torque_onoff_sensor_health()`, e `device_health_count_transaction()`/
`_count_failure_sequent()`) ficaram como API permanente - úteis pra
depurar de novo no futuro sem precisar reinventar o mesmo diagnóstico
(só a linha de log incondicional em `main()` era temporária e foi
removida).

### Log de boot "pronto" não prova que o chip foi encontrado

Achado durante a mesma investigação: as mensagens de boot
(`ads1000_init()`/`torque_onoff_sensor_init()`, "Driver ADS1000 @0x..
iniciado"/"Driver do torque ON/OFF .. iniciado") só confirmam que o
barramento I2C **deste SIM Connect** está pronto (`device_is_ready()`)
- não que o chip físico respondeu. Elas aparecem do mesmo jeito mesmo
com o sensor fisicamente desconectado (o driver nem tentou a primeira
transação ainda nesse ponto). Quem prova presença real é só o rastreio
de saúde, alguns ciclos depois (`..._is_online()`). Reformulado
(2026-08-26) pra deixar isso explícito no próprio texto do log, depois
do Felipe notar que "não fazia sentido" o torque ON/OFF aparecer
"pronto" sem estar conectado.

## Robustez de I2C: comparação com o fwBLE

Investigação pedida pelo Felipe (2026-08-26), depois de notar que trocar
o sensor de torque ON/OFF pelo analógico "a quente" (fiação viva, DK
ligado) às vezes derrubava a leitura de TODOS os sensores, não só do
que estava sendo trocado - suspeita de barramento I2C travado por um
glitch elétrico do próprio ato de conectar/desconectar. Revisitado o
firmware legado (`ControleCoesterBLE/BLE` - caminho absoluto em
[[fwble-repo-location]] nas memórias desta sessão) para comparar os
mecanismos de robustez de I2C usados lá com o que existe aqui.

### O que o fwBLE faz (`I2cBus/i2c_bus.c`, `HAL/hal_i2c_master.c`, `proCo/comScan.c`)

1. **Timeout de software por transação** (`TIMEOUT_TRANSACTION_DEFAULT
   =15ms`, `TIMEOUT_BUS_FAULT=300ms` pro caso de recuperação) - conferido
   no loop cooperativo do `comScan()`, nunca bloqueia esperando o HAL.
2. **Recuperação ativa de barramento travado**
   (`halI2cMasterReleaseBus()` em `hal_i2c_master.c`): desliga o
   periférico TWI, **pulsa a linha SCL manualmente 8 vezes** com SDA
   solto (a técnica clássica de recuperação I2C - libera um escravo
   preso segurando SDA em nível baixo no meio de uma transação, ex.:
   desconectado/reconectado com o barramento "vivo"), confere se as
   duas linhas voltaram a nível alto, e só então reinicializa o
   periférico. Chamado repetidamente (`comScan.c`, estado
   `CDP_FAULT_CYCLE`) enquanto a transação falhar, até
   `TIMEOUT_BUS_FAULT` esgotar.
3. **Sinalização global de barramento travado**
   (`comScanBusFault()`) - não só "esse dispositivo caiu", mas "o
   barramento inteiro parece travado" (limpo automaticamente no
   próximo sucesso).
4. **Retry com backoff crescente** por dispositivo (`loadTimeRetry`,
   até `MAX_TIME_RETRY=10000ms`) - não martela um dispositivo
   sabidamente offline a cada ciclo; volta a tentar imediatamente
   assim que ele responde de novo.
5. **Rastreio de falha/sucesso por dispositivo**
   (`comScanOnlineDev()`/`comScanDevFailRate()`) - já replicado
   fielmente aqui por `device_health.c`.

### O que existia aqui antes desta investigação

Confirmado lendo o próprio driver do Zephyr
(`zephyr/drivers/i2c/i2c_nrfx_twim.c`): cada transação já tem um
timeout interno via `CONFIG_I2C_NRFX_TRANSFER_TIMEOUT` (**default
500ms** - não trava pra sempre, mas bem mais frouxo que os 15ms do
fwBLE) - então o item 1 já existia, só mais lento. O item 5
(`device_health.c`) já existia. Os itens **2, 3 e 4 não existiam** -
nada neste projeto chamava recuperação de barramento, nada sinalizava
"barramento travado" globalmente, e todos os pollers retentam a cada
15ms pra sempre, sem backoff.

### Item 2 já existia - achado corrigido (2026-08-26, mesmo dia)

Implementação inicial: criado `i2c_bus_health.c`/`.h`, chamando
`i2c_recover_bus()` sempre que um sensor estivesse offline. **Removido
no mesmo dia**, depois do Felipe reportar log cheio de "Barramento I2C
recuperado" e travamentos persistindo - ao investigar, a causa era um
erro de projeto meu, não falta de robustez:

Lendo `zephyr/drivers/i2c/i2c_nrfx_twim.c` com mais cuidado (função
`i2c_nrfx_twim_transfer()`): o **próprio driver do Zephyr já chama
`i2c_recover_bus()` automaticamente** sempre que uma transação
genuinamente trava (timeout do semáforo de conclusão, `-ETIMEDOUT`) -
o item 2 do fwBLE já vinha de fábrica, só faltava apertar o timeout
(item 1) pra ele reagir mais rápido, o que já tinha sido feito.

O bug da minha implementação: `i2c_bus_health_try_recover()` disparava
a partir de `!device_health_is_online()`, sinal que também é verdadeiro
para um NACK comum (sensor simplesmente não populado nessa unidade -
estado PERMANENTE e esperado pra uma das duas variantes de torque, não
uma falha). Isso forçava recuperação de barramento (desliga/religa o
periférico `i2c21` inteiro) a cada ~300ms, para sempre, enquanto
qualquer sensor estivesse ausente - ruído no log e uma fonte nova,
autoinfligida, de instabilidade pro barramento compartilhado pelos
OUTROS sensores que estavam funcionando bem. O próprio fwBLE nunca
comete esse erro: `comScan.c` só entra em `CDP_FAULT_CYCLE` (recuperação)
quando a transação **trava** (estoura o timer), nunca por causa de um
NACK comum tratado por `comScanDevFail()` - a mesma distinção que eu
não tinha replicado corretamente.

**Módulo removido** (`i2c_bus_health.c`/`.h` deletados, chamadas
tiradas de `ads1000.c`/`torque_onoff_sensor.c`). Fica só o item 1
apertado (`CONFIG_I2C_NRFX_TRANSFER_TIMEOUT=100` em `prj.conf`) -
suficiente, já que o driver cuida da recuperação sozinho.

**Lição**: antes de adicionar uma camada de robustez "por precaução",
vale conferir se a plataforma de baixo já faz aquilo - e, ao portar um
padrão de outro projeto (fwBLE), replicar também a condição de
disparo exata (aqui, "trava" ≠ "não responde"), não só a técnica.

### Deixado para depois (pedido explícito do Felipe: documentar, não implementar agora)

- **Item 3 (flag global de barramento travado)**: hoje não há nenhum
  consumidor (BLE característica, alarme, log agregado) para um sinal
  "o barramento i2c21 inteiro parece travado" - só o rastreio
  por-dispositivo (`device_health` de cada sensor) existe, e a
  recuperação de timeout genuíno já é automática (ver acima). Se um dia
  fizer sentido expor isso (ex.: na área "Info" do `acgl`, ver seção
  abaixo, ou um alarme na interface), dá pra contar quantas vezes
  `i2c_recover_bus()` foi chamado pelo driver recentemente - hoje isso
  não é visível de fora dele.
- **Item 4 (retry com backoff)**: hoje os 3 pollers retentam a cada
  15ms pra sempre, mesmo quando uma das duas variantes de torque
  jamais vai existir naquela unidade (decisão de BOM permanente, não
  transitória). Um backoff (crescente até, digamos, 500ms-1s) reduziria
  esse tráfego de barramento inútil em produção sem custo de robustez
  (volta a 15ms imediatamente no primeiro sucesso, mesmo espírito do
  `loadTimeRetry` do fwBLE). Mais uma otimização de limpeza/consumo do
  que uma correção de robustez - o item 2 (recuperação de barramento)
  já cobre o cenário real de barramento travado por hot-plug.

## Sensor de torque ON/OFF (PCA9536) — variante digital

Confirmado com o Felipe (2026-08-25): o chip físico é um **PCA9536DR**
(I/O expander I2C, 4 pinos GPIO), não um ADC - **não há medida
contínua**, só marcação sim/não de sobretorque, uma por sentido:

- `P0` → microchave de **fechamento**; `P1` → microchave de
  **abertura**. Pull-up nos dois pinos, mas o detalhe que importa é o
  **tipo de contato**: são microchaves **Normalmente Fechado** (NF/NC)
  - correção do Felipe em 2026-08-26 (a primeira versão deste documento
  assumia Normalmente Aberto por engano). Em repouso (sem sobretorque)
  o contato está FECHADO (curto pra GND, nível baixo); a ativação por
  sobretorque ABRE o contato (nível alto, via pull-up). Ou seja: **nível
  alto = há sobretorque**, sem nenhuma inversão de polaridade
  necessária na leitura (bate direto com o próprio sentido que o proCo
  original já usa pro bit, `TORQUE_ABERTURA_BIT`/`TORQUE_FECHAMENTO_BIT`
  - ver `actuator_sensors.c`).
- Endereço fixo `0x41` (7 bits) - PCA9536 não tem pinos de seleção de
  endereço.

Protocolo (registrado, ao contrário do ADS1000 - ver
`src/torque_onoff_sensor.c`):

- `CONFIG` (registro `0x03`) escrito uma vez no boot com `0x0F` (todos
  os 4 pinos como entrada) - não precisa reescrever a cada leitura como
  o ADS1000 (não há glitch de configuração conhecido nesse chip; se o
  dispositivo cair offline, `torque_onoff_sensor.c` volta a marcar
  `configured=false` e reconfigura na próxima leitura bem-sucedida).
- `INPUT` (registro `0x00`) lido via `i2c_write_read` a cada 15ms (mesmo
  ciclo do `ads1000.c`).
- A API pública (`torque_onoff_sensor_last()`) já resolve a polaridade
  elétrica das microchaves NF - `true` = "há sobretorque" nos dois
  booleanos (`abertura`/`fechamento`), então nenhum consumidor
  downstream precisa saber sobre nível lógico/pull-up/tipo de contato.

### `device_health.c` — rastreio de saúde por dispositivo (novo, compartilhado)

Item do "Escopo explicitamente fora" original (rastreio de saúde por
dispositivo, equivalente a `comScanOnlineDev()`/`comScanDevFailRate()`
do `comScan.c` legado) - implementado agora porque a seleção automática
de fonte de torque (próxima seção) **depende** dele: precisa saber, a
cada ciclo, se cada backend (`torque_sensor.c` analógico,
`torque_onoff_sensor.c` digital) está "online" pra decidir qual usar.

`struct device_health` (contador de falhas, falhas seguidas, taxa de
falha, "já teve sucesso alguma vez") + `device_health_record_success()`/
`_record_failure()`/`_is_online()`/`_failure_rate()`. Simplificado em
relação ao original: sem a peculiaridade de off-by-one do
`comScanDevFailRate()` legado. Usado hoje por `ads1000.c` (motor de
posição/torque analógico) e `torque_onoff_sensor.c`.

## Seleção automática de fonte de torque (runtime, não compile-time)

**Correção importante do Felipe** (2026-08-25) sobre a recomendação
anterior deste documento: o `fwBLE` original **não** faz seleção em
runtime das variantes de sensor. `PAINEL_CQT` e `POS_ABS` são
`#ifdef`s de **compilação** - existem 4 firmwares diferentes (posição
potenciômetro/absoluta × torque analógico/ON-OFF), escolhidos na hora
de gravar a unidade, não descobertos sozinhos. `TER_ACT_POS_ABS()`
(mencionado antes como possível pista de runtime) é só um getter de
`ter.pos_abs`, um campo de estado - não implica detecção automática.

Este projeto escolhe deliberadamente **não repetir** essa abordagem
pro sensor de torque: em vez de 2 firmwares (analógico/ON-OFF), um
único firmware roda os dois backends (`torque_sensor.c`/
`torque_onoff_sensor.c`) e escolhe em **runtime**, a cada ciclo de
`actuator_sensors.c` (15ms), qual está fisicamente presente - via
`device_health` (`torque_sensor_is_online()`/
`torque_onoff_sensor_is_online()`). Analógico tem prioridade se, por
algum motivo de bancada, os dois estiverem online ao mesmo tempo (no
produto real isso nunca acontece - são variantes de BOM mutuamente
exclusivas). Motivação: menos imagens de firmware pra gerenciar numa
frota com FOTA (um único binário serve as duas variantes de hardware),
à custa de detecção só ficar disponível depois do primeiro ciclo de
poll bem-sucedido (`ACTUATOR_TORQUE_SOURCE_NONE` até lá).

`enum actuator_torque_source` (`actuator_sensors.h`) expõe qual fonte
está ativa agora (`NONE=0`/`ANALOG=1`/`ONOFF=2` - valores fixos,
serializados por BLE, não renumerar). Quando a fonte é `ONOFF`, os
campos `ad_torque`/`nm_torque` de `struct actuator_sensor_data`
**não carregam Nm de verdade** - carregam os 2 bits crus de
sobretorque, reaproveitando a mesma convenção de bit que o proCo
original já usa nessa variante (`TORQUE_FECHAMENTO_BIT=0x01`,
`TORQUE_ABERTURA_BIT=0x02` em `sensPosTor.c`, ramo `PAINEL_CQT`: o
`adTorque` cru é copiado direto pra `nmTorque`). Preserva o formato de
10 bytes sem precisar de um campo novo no wire.

**Posição absoluta continua fora de escopo** (deixado explicitamente
pra depois, 2026-08-25) - só a variante de torque foi resolvida por
enquanto.

## Camada de calibração: `actuator_sensors.c` + `sample_filter.c`

Segunda camada, acima do `ads1000.c`/`position_sensor.c`/
`torque_sensor.c`: converte os valores brutos em valores físicos,
equivalente a `BLE/Atuador/sensPosTor.c` no `ControleCoesterBLE` - mas
com escopo bem mais estreito (ver "Escopo explicitamente fora"
abaixo).

- **`sample_filter.c`**: média aparada de 10 amostras (descarta máximo
  e mínimo, tira média das 8 restantes) - mesma lógica de `sptPegAd()`
  no original. Um filtro por sensor (posição e torque têm buffers
  independentes).
- **`actuator_sensors.c`**: agrega os dois sensores a cada 15ms (lê o
  cache de `position_sensor.c`/`torque_sensor.c`, alimenta os filtros,
  converte):
  - Posição → `at_posicao` (0-1000, "por mil"): equivalente a
    `sptPosMil()`, usando `limiteSuper=1800`/`limiteInfer=200`.
  - Torque → `nm_torque`: equivalente ao cálculo dentro de
    `sptTratTrq()` (`(ad_torque - torqueZero) * fator / 1000`, com
    `fator=1000`) - usa um único fator porque `fabFatTorqAber` e
    `fabFatTorqFech` são iguais por padrão (`1000`/`1000`), então não
    precisamos ainda diferenciar abrindo/fechando (o que exigiria
    `at_ctl_get_mov_stt`, fora de escopo).
  - `ad_torque_max`: máximo de `ad_torque` observado. **Simplificação**:
    o original reseta esse máximo no início de cada movimento
    (precisa de estado de movimento); aqui é só o máximo desde o boot.

Produz `struct actuator_sensor_data` (5 campos, sem padding, 10 bytes)
- **igual byte a byte** aos 10 primeiros bytes de `sensPosTor_t`
  (`adPosicao`, `atPosicao`, `adTorque`, `nmTorque`, `adTorqueMax`),
  os mesmos 10 bytes que a área "Sensor" do `ifFerConfig` já expõe
  hoje num Atuador BLE real (ver seção do protocolo BLE abaixo) -
  pronto pra ser servido pelo serviço BLE quando ele for implementado.

### Parâmetros de calibração: configuráveis via BLE, persistidos (ZMS)

Valores reais confirmados em `ControleCoesterBLE/BLE/ParamZarI.c`
(struct `paramInic`/`paramMin`/`paramMax`):

| Parâmetro | Default | Min | Max | Configurável aqui? |
|---|---|---|---|---|
| `limiteSuper` (posição) | `1800` | `50` | `2001` | Sim |
| `limiteInfer` (posição) | `200` | `50` | `2001` | Sim |
| `torqueNmInc` (limite abrindo) | `80` | `1` | `1400`* | Sim |
| `torqueNmDec` (limite fechando) | `80` | `1` | `1400`* | Sim |
| `torqueZero` | **sem default de fábrica** - calibrado em campo | - | - | Sim (procedimento "desloca zero") |
| `fabFatTorqAber`/`fabFatTorqFech` | `1000`/`1000` | `1` | `30000` | **Não** - decisão explícita (2026-08-21): de fábrica, raramente muda |
| `fabGanSenTorq` | `0` | `0` | `3` | Não (é do `ads1000.c`, ganho do ADC - ver seção anterior) |

\* `1`/`1400` são `fabTorqMin`/`fabTorqMax` no original - também fixos
aqui por enquanto, só usados como o limite de `torqueNmInc`/`torqueNmDec`.

Implementado em **`actuator_calib.c`/`.h`** (novo módulo, separado de
`actuator_sensors.c`):

- `actuator_sensors.c` agora lê `limiteSuper`/`limiteInfer`/`torqueZero`
  deste módulo a cada ciclo de calibração, em vez de `#define` fixos.
  `torqueNmInc`/`torqueNmDec` são armazenados mas **ainda não usados**
  em conta nenhuma - só entram quando a lógica de alarme de sobretorque
  for portada (ver "Escopo explicitamente fora" abaixo).
- Persistência via Settings/ZMS, subárvore própria (`acal/v1`) -
  mesmo mecanismo que a allow-list (`coe/al`) e o nome do Gateway
  (`gw/name`) já usam, cada um em sua própria subárvore independente.
- **`actuator_calib_zero_torque()`** - procedimento oficial **"desloca
  zero"** (nome confirmado com o Felipe - não "tara", como a primeira
  versão chamava). Equivalente a `paramModTrqZero()` no original:
  captura o `ad_torque` atual (cru, filtrado, **sem** subtrair o zero
  anterior) como o novo `torqueZero`.

#### Exposição BLE: characteristics dedicadas, não o endereço real do "Painel"

Ao contrário da área Sensor (onde um Gateway real já manda bytes reais
nesse formato - fidelidade byte a byte compensava), a área "Painel"
(`paramDado`) no fwBLE é um struct de 150+ campos com bitfields, e
**nada hoje escreve nesse endereço via BLE** no produto real (a
configuração de campo é via MB RTU/IHM). Replicar os offsets exatos
seria frágil sem comprar compatibilidade real - decisão confirmada com
o Felipe (2026-08-21): usar characteristics **novas e dedicadas**, com
opcodes simples (mesmo estilo do Gateway Control), não o endereço do
Painel. Se um motivo real de compatibilidade aparecer no futuro (ex.:
reusar uma ferramenta que já fale `ifFerConfig`), vale reconsiderar -
ver nota grande em `actuator_calib.h`.

Duas characteristics novas, no mesmo serviço fundido (`06290001…` -
ver seção seguinte):

```
Actuator Calib Ctrl  (…152d, write)       — [cmd(1)] [arg...]
  0x01 [u16 LE]  limiteSuper
  0x02 [u16 LE]  limiteInfer
  0x03 [u16 LE]  torqueNmInc
  0x04 [u16 LE]  torqueNmDec
  0x05           desloca zero do torque (torqueZero = leitura atual filtrada)

Actuator Calib State (…152e, read+notify) — 10 bytes LE:
  limiteSuper(u16) limiteInfer(u16) torqueNmInc(u16) torqueNmDec(u16) torqueZero(i16)
  Notifica sozinha a cada escrita aceita na Calib Ctrl.
```

Implementado em `my_lbs.c` (handlers `write_calib_ctrl`/
`read_calib_state`, mesma característica de ordem de declaração tardia
que `write_actuator_rx` já precisou - ver nota no código) e
`my_lbs.h` (UUIDs, doc dos opcodes).

### Escopo explicitamente fora (calibração)

`sensPosTor.c` original acopla a conversão física com alarmes
(`at_alarm_*` - sobretorque, válvula travada) e com o estado de
movimento do atuador (`at_ctl_get_mov_stt`). Isso é a lógica de
controle do atuador inteira, não "cálculo de valor físico" - tratado
como fase separada e maior, não misturado aqui.

## Achado grande: o protocolo BLE de exposição (`acgl`/`ifFerConfig`) já está mapeado

Investigação companheira de arquitetura (2026-08-21), pra saber o que
o papel de atuador precisa implementar pra expor posição/torque via
BLE de forma compatível com o que um Atuador BLE real já faz (e que o
Gateway - `actuator_client.c` - já sabe consumir, sem nenhuma mudança).

**Boa notícia**: é bem menor do que pareceu a princípio. O protocolo
proCo tem uma camada de mensagens maior (`proCoMsg.c`/`proCoMsgEx.c`,
~2100 linhas juntas) que só é necessária pras áreas de memória que
fazem *proxy* pra outro dispositivo I2C dentro do Atuador (FSA, IO
Digital, IO Analógica, Rede) - **não é necessária pra área Sensor**,
que é um ponteiro direto pro struct `posTor` (confirmado em
`ifFerConfig.c:802`: `pui8_data = (uint8_t*)&posTor;`, sem indireção).

A ponte de verdade é `BLE/acgl/acgl.c` ("Actuator Configuration Gateway
Local", 360 linhas) - é ele que trata os bytes que chegam pela
characteristic BLE. Decodificado byte a byte contra o exemplo já
existente em `PROTOCOLO_INTERFACE.md` (`01 00 03 0A 80 40 80 00`,
depois de tirar o byte de slot que o Gateway usa):

```
struct msg_t (8 bytes, little-endian, acgl.h):
  [0..1] idMsgHost   (uint16)
  [2]    type        (uint8)  - ACG_TYPE_MSG_t (ver tabela abaixo)
  [3]    lenData     (uint8)
  [4..7] addressData (uint32 LE)
```

Decodificação do exemplo: `type=0x03` (`GTM_REQUEST`, pedido de
leitura), `lenData=0x0A` (10 bytes), `addressData=0x00804080` =
`MSG_EX_ADDRESS_CONFIG_SENSOR` (`ifFerConfig.h`) - bate exatamente com
os 10 bytes de `struct actuator_sensor_data` acima. A resposta vem com
`type=GTM_RESPONSE` (`0x06`) - bate com o que `PROTOCOLO_INTERFACE.md`
já documentava (`byte[3]` deve ser `0x05` ou `0x06`): `GTM_CONFIRM=5`
(confirma escrita), `GTM_RESPONSE=6` (dados de leitura).

Tabela de tipos (`ACG_TYPE_MSG_t`, `acgl.h`):

| Valor | Nome | Sentido |
|---|---|---|
| 0 | `GTM_NULL` | - |
| 1 | `GTM_WAIT` | - |
| 2 | `GTM_STATUS` | pedido de status do gateway |
| 3 | `GTM_REQUEST` | pedido de leitura |
| 4 | `GTM_SEND` | pedido de escrita |
| 5 | `GTM_CONFIRM` | confirma escrita |
| 6 | `GTM_RESPONSE` | resposta de leitura |
| 7 | `GTM_NEG` | recusado (endereço/tamanho inválido) |
| 8 | `GTM_TIMEOUT` | - |
| 9 | `GTM_LACK` | - |
| 10 | `GTM_ALLOCTION` | reserva a "sessão" (nome da interface) |
| 11 | `GTM_RELEASE` | libera a sessão |
| 12 | `GTM_REFUSE` | frame malformado |

Serviço/characteristics BLE que um Atuador BLE real já expõe (já
declarados em `nrf54-app/src/actuator_client.h`, usados hoje só como
relay puro de bytes - `"Sem NENHUM parsing do protocolo proCo aqui"`,
comentário no próprio arquivo):

```
Serviço: 06290001-0538-408d-8359-e83d95ae1d60
RX:      06290002-0538-408d-8359-e83d95ae1d60
TX:      06290003-0538-408d-8359-e83d95ae1d60
```

**Implementado** (`actuator_service.c`/`.h` + `my_lbs.c`/`.h`): um
handler de `msg_t` que reconhece `GTM_REQUEST` em duas áreas e responde
`GTM_RESPONSE` + os bytes correspondentes; qualquer outro endereço/tipo
responde `GTM_NEG` (área não implementada ainda) ou `GTM_REFUSE` (frame
malformado). `proCoMsg.c`/`proCoMsgEx.c` não precisaram ser tocados,
como previsto.

- **Área Sensor** (`0x00804080` = `MSG_EX_ADDRESS_CONFIG_SENSOR`, real,
  existe no `ifFerConfig` original) - `struct actuator_sensor_data`,
  fidelidade byte a byte, descrita acima.
- **Área Info** (`0xF0000000`, **sintética**, criada por este projeto -
  não existe no `ifFerConfig` original, escolhida de propósito fora do
  espaço real de endereços proCo `0x00800000`-`0x008FFFFF` pra nunca
  colidir com nada que um Gateway real peça) - 1 byte, hoje só expõe
  `actuator_sensors_torque_source()` (o `enum actuator_torque_source`
  ativo agora). Existe porque a seleção automática de fonte de torque
  (seção acima) precisa de **algum** jeito de a interface/Gateway saber
  qual variante está ativa antes de decidir como interpretar
  `ad_torque`/`nm_torque` - a área Sensor sozinha não distingue "6 Nm
  reais" de "bits 0b110 crus".

### Fusão de serviços: `my_lbs` passou a se registrar sob `BT_UUID_ACTUATOR_SERVICE`

Descoberto durante a implementação: o pacote de advertising legado (31
bytes) não comporta anunciar dois UUIDs de serviço de 128 bits ao mesmo
tempo (18 bytes cada só pra scan response). Como a interface web
(`my_lbs`, `00001523…`) e o papel de atuador (`06290001…`) precisam
estar **ambos** descobríveis pela mesma unidade, a solução (decidida
com o Felipe, ver artifact "Um Serviço, Dois Papéis" linkado na
conversa original) foi fundir as characteristics do `my_lbs` **dentro**
do serviço `06290001…` (não o contrário - esse UUID é o que unidades
reais do Atuador BLE já usam em campo, não dá pra mudar sem quebrar
compatibilidade; `00001523…` era nosso, livre pra mover).

`my_lbs.c` hoje declara `BT_GATT_SERVICE_DEFINE(my_lbs_svc,
BT_GATT_PRIMARY_SERVICE(BT_UUID_ACTUATOR_SERVICE), ...)` - todas as
characteristics antigas (LED, Raw Data, Command, Manage, Status,
Discovery, Gateway Ctrl) mantiveram os UUIDs de characteristic de
sempre, só o serviço que as agrupa mudou. `nrf54-app/index.html`
acompanhou a mudança (constante `SVC`).

## Escopo explicitamente fora (sensores/dispositivos)

- **`ENDER_POS_ABS`** (`0x4C`→`0x26`): sensor de posição absoluta,
  chip/protocolo completamente diferente (`dev_pos_abs.c`, não
  `dev_ads.c`). Não implementado.
- **Demais dispositivos do `devices.h`** (IO Digital, IO Analógica,
  Rede, RTC, EEPROM, Chave, FerConfig): cada um é um chip/protocolo
  diferente (`dev_ioa.c` etc.) - vão precisar da mesma leitura de
  código-fonte real antes de portar, não dá pra generalizar às cegas
  a partir só do ADS1000. O rastreio de saúde por dispositivo já existe
  como infraestrutura compartilhada (`device_health.c`, ver seção
  acima) e pode ser reaproveitado quando esses dispositivos forem
  portados - hoje só `ads1000.c`/`torque_onoff_sensor.c` o usam.
- **Dispositivos de controle e modo do atuador** (motor, modo de
  operação - o que um Atuador BLE real também tem, além dos sensores):
  fora do escopo *deste* documento. Já portados depois, com documento
  próprio - ver `COMANDO_CONTROLE.md`.

## Interface web (`nrf54-app/index.html`)

Painel "Este SIM Connect (papel de atuador)" - sempre visível quando
conectado, independente de qual slot remoto está selecionado (é o
próprio Gateway respondendo por si mesmo, não um atuador num slot):

- Mostrador de posição (dial circular), **reaproveitando os parsers já
  existentes** (`parsePosicao()`/`parseTorque()`) - o payload de 10
  bytes da área Sensor é byte a byte idêntico ao que `CMD_READ_POS_TOR`
  já devolve de um atuador remoto, então não foi necessário nenhum
  parser novo. `parsePosicao()` também passou a decodificar
  `at_posicao` como `int16` (era `uint16` por engano - um valor
  negativo legítimo, abaixo de `limiteInfer`, decodificava errado) e a
  reconhecer o sentinela `ACTUATOR_AT_POSICAO_INDEF` (2026-08-26):
  quando o sensor de posição ainda não tem leitura válida, mostra
  **"INDEF"** no lugar do percentual, em vez de travar o painel inteiro
  esperando o torque também ficar pronto (bug corrigido na mesma leva -
  ver `actuator_sensors.c`, posição e torque agora são independentes).
- Card "Torque" com **3 vistas mutuamente exclusivas** (2026-08-25),
  alternadas conforme `actuator_sensors_torque_source()` (lido via a
  área Info, `lerTorqueSource()`) - a interface não assume mais que
  torque é sempre uma medida contínua:
  - `#localTorAnalogView` (fonte `ANALOG`): a barra de torque original,
    proporcional ao **limite configurado**
    (`min(torqueNmInc, torqueNmDec)` - o mais conservador dos dois, já
    que a interface ainda não sabe se o atuador está abrindo ou
    fechando), não a um máximo histórico bruto como o painel de um
    slot remoto ainda faz.
  - `#localTorOnoffView` (fonte `ONOFF`): 2 selos ("Abertura"/
    "Fechamento"), acesos (`.tripped`, vermelho) quando o bit
    correspondente está marcado - decodificado de `tor.ad` com a mesma
    convenção de bit do wire (`& 0x02` = abertura, `& 0x01` =
    fechamento, ver seção de seleção automática acima). Sem barra/AD -
    esse sensor não mede.
  - `#localTorNoneView` (fonte `NONE`): mensagem "Nenhum sensor de
    torque detectado" - nenhum dos dois backends respondeu ainda
    (comum logo após o boot, antes do primeiro ciclo de poll).
  - Implementação: `sendAcgRequest(address, lenData)` genérico (Sensor
    e Info reaproveitam o mesmo helper); `lerTorqueSource()` e
    `lerSensorLocal()` são aguardados em sequência a cada tick (só um
    pedido acgl em voo por vez, `pendingLocalSensor` é um slot único).
- **Indicador de dados obsoletos** (2026-08-26): antes, quando o pedido
  acgl falhava (`GTM_NEG` - sensor sem leitura válida, ou fonte de
  torque offline), o `tick()` do polling simplesmente não chamava
  `renderLocalSensor()` - a tela ficava com os ÚLTIMOS valores bons,
  sem nenhum aviso, indistinguível de "atualizado agora mesmo" (bug
  reportado pelo Felipe: "a interface segue fixa nos mesmos valores").
  Agora, depois de `LOCAL_SENSOR_STALE_AFTER` (2) ciclos seguidos sem
  sucesso, `#localLastRead` vira vermelho e mostra "sem leitura válida
  há Xs" - a interface nunca mais finge estar atualizada quando não
  está.
- Card "Calibração": 4 campos (limite superior/inferior de posição,
  limite de torque abrindo/fechando) + botão **"Desloca zero"**.
  Campos validados no navegador antes de escrever (mesma faixa que o
  firmware aceita) e sincronizados automaticamente com o valor real
  (leitura ao conectar + notify a cada mudança) - nunca mostram um
  valor "otimista" não confirmado pelo firmware.

Polling automático a cada 2s enquanto conectado (`lerSensorLocal()` +
`lerTorqueSource()`), silencioso em erro (não tem toast a cada 2s se o
filtro ainda estiver enchendo o buffer logo após o boot).

### Bugs corrigidos: interface lia uma vez e nunca mais atualizava

Depois que o firmware (sensores + fila I2C dedicada) já estava
confirmado funcionando perfeitamente em bancada, o Felipe reportou que
a interface web ainda ficava longos períodos (60s+) sem atualizar,
mesmo com o log do firmware mostrando leituras corretas o tempo todo -
dois bugs distintos, os dois em `index.html`, nenhum no firmware:

1. **`idMsgHost` fixo (deduplicação de notificação pelo Windows)**: o
   cabeçalho de toda mensagem `acgl` tem um campo `idMsgHost` que o
   firmware só ecoa de volta (`actuator_service.c`,
   `set_response_header`) - a interface mandava sempre o mesmo valor
   fixo (`1`). Se os valores físicos não mudassem entre 2 leituras, a
   resposta saía byte a byte idêntica à anterior, e o Windows/WinRT
   (por baixo do Web Bluetooth do Chrome) tem um comportamento
   documentado de **não disparar `characteristicvaluechanged`** para
   um valor notificado idêntico ao último. Corrigido com um contador
   incremental de `idMsgHost` (`nextAcgMsgId()`/`nextProtoMsgId()`) -
   garante que a resposta nunca se repete byte a byte, mesmo com o
   sensor parado. Aplicado tanto na leitura local (`sendAcgRequest`)
   quanto nos comandos de leitura de atuadores remotos
   (`sendCommandAndWaitFrame`) - a aba de "comando cru" foi
   deliberadamente excluída (`patchMsgId=false`), já que ali o usuário
   digita os bytes de propósito, inclusive o `idMsgHost`.
2. **Causa real dos 60s+ de silêncio: `GATT operation already in
   progress`** - o Web Bluetooth só permite **uma operação GATT em voo
   por vez, para o dispositivo BLE inteiro**, não por characteristic.
   A página tinha vários pontos independentes de operação GATT (leitura
   do sensor local, leitura de status/descoberta, comandos de slot,
   notificações) sem nenhuma coordenação global entre eles - só
   coordenação de *negócio* dentro de cada um (`pendingLocalSensor`,
   `withSlotLock`), não do *dispositivo* como um todo. Quando duas
   operações caíam no mesmo instante, a que perdia a corrida falhava
   na hora com esse erro - e cada nova tentativa (a cada 2s) colidia de
   novo, até as duas pararem de se sobrepor por acaso. Corrigido com
   uma fila global (`withGatt()`, no topo da seção "ESCRITA GATT") por
   onde agora passam **todas** as operações GATT da página
   (`writeChar()`, o novo `readChar()`, `startNotifications()`) -
   FIFO, uma de cada vez, para o dispositivo inteiro.

O primeiro bug era real mas só parte da história - o segundo era a
causa dominante dos períodos longos sem atualização. Confirmado
resolvido em bancada pelo Felipe (2026-08-26) depois dos dois.

## Fiação (nRF54LM20-DK)

`i2c21`, pinos `P1.11` (SCL) / `P1.12` (SDA) do header P1 - mesmos
pinos que a Nordic Academy recomenda pra esse board neste exercício.
Pull-up interno habilitado via overlay (`bias-pull-up`), sem resistor
externo. VDD do DK precisa estar em 3.3V (Board Configurator → VDD
nPM VOUT1 → 3.3V → Write config; default de fábrica é 1.8V, abaixo do
mínimo do ADS1000). Posição, torque analógico e torque ON/OFF
compartilham o mesmo barramento (mesmos 4 fios em paralelo, só o
endereço diferencia) - em bancada, é possível ter os 3 conectados ao
mesmo tempo (cenário que o produto real nunca tem, já que torque
analógico/ON-OFF são variantes de BOM mutuamente exclusivas, mas serve
pra testar a seleção automática sem trocar de fiação).

## Validação em bancada

Build completo (sysbuild + TF-M) validado repetidamente ao longo desta
portagem, sempre 100% sem erro. Testado em hardware real:

- Sensores brutos e valor calibrado variam corretamente com o
  potenciômetro/torque aplicado, ex.: `Sensor de posicao: raw=463` /
  `Calibrado: posicao=16.4% torque=6 Nm`. Conferido manualmente:
  `(463 - 200) * 1000 / (1800 - 200) = 164` (16.4%) - bate com
  `sptPosMil()`.
- Leitura da área Sensor via BLE (`06290002…`/`06290003…`) confirmada
  com nRF Connect, batendo com o log de bancada.
- `index.html` reconectando com o serviço fundido, painel local
  atualizando ao vivo, card de Calibração escrevendo/lendo/persistindo
  os 5 parâmetros (sobrevive a reboot).
- **(2026-08-25)** Sensor de torque ON/OFF (PCA9536): leitura de
  abertura/fechamento confirmada isoladamente; depois, com o backend
  analógico fisicamente desconectado, a seleção automática detectou a
  queda (`torque_sensor_is_online()` → false) e trocou pra
  `ACTUATOR_TORQUE_SOURCE_ONOFF` sozinha, voltando a formar o frame de
  10 bytes completo (antes da seleção automática, o frame parava de se
  formar nesse cenário - só o backend analógico era consultado).
- **(2026-08-26)** Fila de trabalho dedicada (`sensor_workq.c`):
  confirmado com dado real (contadores de `device_health`, ver seção de
  diagnóstico acima) que o `main()`/host Bluetooth nunca para de rodar
  mesmo com os 3 sensores I2C falhando 100% ao mesmo tempo - o sintoma
  original ("as vezes trava") não se reproduziu mais depois da
  correção.
- **(2026-08-26)** Causa raiz da instabilidade nesta rodada de teste:
  mau contato no cabo do sensor de posição (não um bug de firmware) -
  confirmado pelo próprio diagnóstico (`seqfail` de 10 para 0 assim que
  o contato foi refeito). Torque analógico/ON-OFF não estavam
  conectados nesse teste - `seqfail=10` constante neles era o
  comportamento correto esperado.
- **(2026-08-26)** Mau contato também no cabo do sensor de torque
  analógico (mesma classe de problema da posição) - só percebido depois
  de melhorar o log de boot (que antes não distinguia "nunca
  conectado" de "conectado mas sem responder", ver seção acima) e
  adicionar o aviso "nenhuma resposta ainda" em `ads1000.c`/
  `torque_onoff_sensor.c`. Corrigido o contato, os 3 sensores (posição,
  torque analógico, torque ON/OFF) comunicam corretamente.
- **(2026-08-26)** As 3 vistas do card de Torque (analógico/ON-OFF/
  nenhum) e o indicador de dados obsoletos: confirmados funcionando em
  bancada pelo Felipe, com sensores de verdade conectados.
- **(2026-08-26)** Interface parando de atualizar por longos períodos
  (60s+) mesmo com o firmware lendo corretamente: os 2 bugs de BLE/JS
  descritos na seção "Bugs corrigidos" acima (`idMsgHost` fixo +
  ausência de fila global de operações GATT) - confirmado corrigido em
  bancada pelo Felipe depois da fila `withGatt()`.
- **(2026-08-26)** Correção de polaridade das microchaves NF/NC do
  torque ON/OFF (nível alto = há sobretorque, sem inversão - ver seção
  do PCA9536 acima): confirmado funcionando em bancada pelo Felipe.
- **(2026-08-26)** `i2c_bus_health.c` implementado e depois **removido
  no mesmo dia** - causava recuperação de barramento redundante (o
  Zephyr já faz isso sozinho em timeout genuíno) e disparava até para
  NACKs normais, gerando ruído no log sem ajudar a robustez de verdade
  - ver seção "Robustez de I2C" acima. Timeout de transferência
  apertado (`CONFIG_I2C_NRFX_TRANSFER_TIMEOUT=100`) continua valendo.
- **(2026-08-26)** Travamento ocasional: melhorou visivelmente depois
  da remoção do `i2c_bus_health.c` redundante - confirmado pelo Felipe
  ("melhorou um pouco"). Não voltou a ser mencionado depois das
  correções seguintes (filtro de amostras); considerado resolvido por
  ora, mas sem uma prova definitiva de causa única - se reaparecer,
  retomar com o mesmo tipo de diagnóstico usado antes (contadores de
  `device_health`).
- **(2026-08-26)** Posição e torque agora são servidos de forma
  independente na área Sensor (`actuator_sensors.c`) - a interface
  mostra "INDEF" na posição em vez de travar o painel inteiro quando só
  a posição está sem leitura válida (bug reportado pelo Felipe:
  diferente de um Atuador BLE real, onde os dois sensores sempre
  existem juntos). Confirmado funcionando em bancada.
- **(2026-08-26)** Bug real por trás do "INDEF" nunca aparecer (e
  possivelmente também da sensação de atraso na posição): achado ao
  testar a mudança acima. `sample_filter_avg()` só confere se o buffer
  encheu **alguma vez** - uma vez cheio, reporta "pronto" pra sempre,
  calculando a média das MESMAS amostras antigas mesmo com o sensor
  offline há muito tempo (`sample_filter_push()` só acontece quando o
  sensor está online, mas o buffer nunca esvazia sozinho). Corrigido
  com `sample_filter_reset()` (novo, em `sample_filter.c`), chamado
  sempre que `position_sensor_is_online()` (nova função, análoga a
  `torque_sensor_is_online()`) ou a fonte de torque analógico ficam
  offline - o filtro zera e exige preenchimento 100% fresco (150ms) da
  próxima vez, em vez de arrastar amostras velhas. **Confirmado
  funcionando em bancada** pelo Felipe ("na interface ficou certinho").
- **(2026-08-26)** Log de bancada (`main.c`) ainda calculava
  posicao/10 e posicao%10 em cima do sentinela `ACTUATOR_AT_POSICAO_
  INDEF` quando a posição estava indefinida, imprimindo algo tipo
  `posicao=-3276.8%` - inofensivo (não afeta o frame BLE real nem a
  interface, só o log de console), mas feio. Corrigido: log mostra
  `posicao=INDEF` nesse caso.

Com isso, toda a leva de trabalho desta sessão (seleção automática de
torque, fila I2C dedicada, 3 vistas da interface, indicador de dados
obsoletos, fila global de GATT, polaridade das microchaves NF,
independência posição/torque com "INDEF") está testada e confirmada em
hardware real pelo Felipe - pronta para commit.

## Arquivos

```
nrf54-app/boards/nrf54lm20dk_nrf54lm20b_cpuapp_ns.overlay   i2c21 habilitado (novo bloco,
                                                             overlay da uart30 intacto)
nrf54-app/prj.conf                                          CONFIG_I2C=y, CONFIG_I2C_NRFX_TRANSFER_TIMEOUT=100
nrf54-app/src/ads1000.{c,h}                                 motor generico ADS1000 (novo)
nrf54-app/src/position_sensor.{c,h}                         instancia - sensor de posicao (novo)
nrf54-app/src/torque_sensor.{c,h}                           instancia - sensor de torque analogico (novo)
nrf54-app/src/torque_onoff_sensor.{c,h}                     driver PCA9536 - torque ON/OFF (novo)
nrf54-app/src/device_health.{c,h}                           rastreio de saude por dispositivo (novo,
                                                             usado por ads1000.c e torque_onoff_sensor.c)
nrf54-app/src/sensor_workq.{c,h}                            fila de trabalho dedicada aos pollers I2C
                                                             (novo - corrige travamentos ver secao acima)
nrf54-app/src/sample_filter.{c,h}                           filtro de media aparada (novo)
nrf54-app/src/actuator_sensors.{c,h}                        calibracao raw->fisico + selecao automatica
                                                             de fonte de torque (novo)
nrf54-app/src/actuator_calib.{c,h}                          parametros configuraveis + persistencia (novo)
nrf54-app/src/actuator_service.{c,h}                        protocolo acgl/ifFerConfig, areas Sensor + Info
                                                             (novo)
nrf54-app/src/my_lbs.{c,h}                                  servico fundido sob BT_UUID_ACTUATOR_SERVICE,
                                                             + characteristics RX/TX e Calib Ctrl/State
nrf54-app/src/main.c                                        init nao-fatal + log periodico de bancada
                                                             (log removido em 2026-10-06)
nrf54-app/index.html                                        SVC atualizado, painel local, card de calibracao,
                                                             3 vistas de torque (analogico/ON-OFF/nenhum),
                                                             indicador de dados obsoletos, idMsgHost variavel,
                                                             fila global de operacoes GATT (withGatt)
```

## Próximos passos

1. **Sensor de posição absoluta** (`ENDER_POS_ABS`, `dev_pos_abs.c`) -
   deixado explicitamente pra depois (2026-08-25). Quando entrar,
   avaliar se o mesmo padrão de seleção automática por
   `device_health` (usado pro torque) também serve pra posição, ou se
   as duas variantes de posição precisam conviver simultaneamente
   (diferente do torque, onde são mutuamente exclusivas).
2. ~~**Dispositivos de controle e modo do atuador** (motor, modo de
   operação)~~ - **feito** (passos 1-5 de `COMANDO_CONTROLE.md`, commits
   `32f2214` a `5edbde3`). O que ficou de fora (ESD/PST, parâmetros
   persistidos, RS485) está registrado lá.
3. **Demais sensores do `devices.h`** (IO Digital, IO Analógica, Rede,
   RTC, EEPROM, Chave, FerConfig) - cada um precisa da mesma leitura de
   código-fonte real antes de portar.
4. **Lógica de controle ligada aos sensores** (`sensPosTor.c`: alarmes
   de sobretorque/válvula travada, integração com movimento) - fase
   maior e separada, ver "Escopo explicitamente fora" acima. O controle
   de posição já existe (`COMANDO_CONTROLE.md`), mas ainda não usa o
   torque: `torqueNmInc`/`torqueNmDec` já estão armazenados e prontos
   pra quando essa lógica existir (passo 8 de `COMANDO_CONTROLE.md`).
   - **Direção confirmada pelo Felipe (2026-08-26, não iniciar ainda)**:
     quando essa fase começar, combinar os mecanismos de validação já
     estudados do fwBLE (`comScan.c`, `device_health.c` - ver seção
     "Robustez de I2C" acima) com a máquina de estados de **cada**
     módulo I2C (`ads1000.c`/`torque_onoff_sensor.c` - hoje só
     WRITE/READ; PCA9536 nem tem máquina de verdade) pra alimentar
     geração de alarmes de verdade (não só log/UI). Provavelmente
     precisa de mais estados por dispositivo do que existe hoje (ex.:
     distinguir "nunca respondeu" de "respondia e parou" de "respondeu
     errado" - hoje tudo isso vira só `device_health` offline/online).
5. **Robustez de I2C - itens deixados para depois** (pedido explícito
   do Felipe, 2026-08-26 - ver seção "Robustez de I2C: comparação com o
   fwBLE" acima para o contexto completo):
   - **Retry com backoff crescente** por sensor (hoje retenta a cada
     15ms pra sempre, mesmo pra uma variante de torque que nunca vai
     existir naquela unidade - decisão de BOM permanente). Reduziria
     tráfego de barramento em produção sem perder robustez.
   - **Flag global de "barramento travado"** (equivalente a
     `comScanBusFault()` do fwBLE) - hoje só existe rastreio
     por-dispositivo (`device_health`); um sinal agregado ficaria
     pronto pra quando houver um consumidor de verdade (BLE, alarme).
