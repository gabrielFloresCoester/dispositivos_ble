# SIM Connect — firmware

O SIM Connect é um **atuador elétrico da Coester**. A linha **SIM** é a
sucessora da linha **BLE**, na qual o "cérebro" do atuador é o **Painel
BLE** (nRF52832 + SoftDevice), rodando o **fwBLE**
(`C:\Users\gabriel.flores\Documents\GitHub\ControleCoesterBLE\BLE`, ver
`docs/PORTABILIDADE_BLE_LEGADO.md`). A lógica do atuador é portada do
fwBLE o mais perto possível de 1:1. O resumo para quem chega (inclusive
o Claude Code) está em `CLAUDE.md`.

Cada unidade é um nRF54 (BLE) + nRF9151 (celular) que, dependendo
de como for configurada na comissão, atua sempre como **atuador** (I2C +
BLE Peripheral, controlando motor/sensores localmente) e opcionalmente
também como **gateway** para outras unidades da mesma linha e para a linha
BLE anterior (nRF52832 + SoftDevice, protocolo proCo) — gerenciando uma
frota via BLE Central e retransmitindo pra nuvem via MQTT/celular.

Continuação direta do trabalho anterior (histórico de commit
preservado na migração — ver nomes antigos de cada pasta abaixo).

## Estrutura

```
nrf54-app/           firmware do chip BLE (nRF54) — papel atuador sempre,
                      gateway opcional (fleet management, allow-list,
                      discovery).
nrf91-app/            firmware do chip celular (nRF9151) — link UART com o
                      nrf54-app + uplink MQTT. Era dual_kits_91.
exercises/uart-link/  banco de testes isolado do frame UART entre os dois
                      chips, antes de fundir no nrf54-app/nrf91-app de
                      verdade. Era dual_kits_54.
docs/
  DECISOES_PRODUTO.md      decisões de arquitetura de produto (produto
                            único, atuador sempre + gateway opcional,
                            nomenclatura, FOTA, UART entre 54 e 91).
  PORTABILIDADE_BLE_LEGADO.md  mapa do firmware da linha BLE (fwBLE,
                            nRF52832) - onde procurar e o que aproveitar.
  PROTOCOLO_INTERFACE.md   contrato BLE do nrf54-app com a interface web
                            (características GATT, formatos de registro).
  PROTOCOLO_54_91.md       contrato do link UART entre nrf54-app e
                            nrf91-app, e o que falta decidir antes de
                            implementar (FOTA, rastreio de saúde por
                            canal, etc.).
  PROTOCOLO_91_MQTT.md     contrato MQTT do nrf91-app com a nuvem
                            (tópicos, payloads, orçamento de dados,
                            achados de LTE/SIM).
  SENSORES_I2C.md          portagem dos sensores I2C do fwBLE legado
                            (proCo) pro papel de atuador - protocolo do
                            ADS1000, endereços, o que falta portar.
  COMANDO_CONTROLE.md      portagem da lógica de comando e controle do
                            fwBLE (atControle/atModo/at_alarm) pro papel
                            de atuador - módulos, contrato BLE novo,
                            divergências, o que faltou (ESD/PST, params).
  SEGURANCA_SENHA.md       senha de acesso: versão provisória (feira) e
                            a solução definitiva comum ao SIM Connect e
                            ao fwBLE.
  PARAMETROS.md            port do mapa de parâmetros do fwBLE
                            (paramDado, área 0x00800800, 144 bytes) -
                            layout, faixas, divergências, onde cada
                            parâmetro atua.
  REGISTRO_EVENTOS.md      port do registro de eventos do fwBLE
                            (atRegEvent) - flash SPI externa e suas
                            áreas, formato, leitura pela área Comando,
                            divergências, roteiro de bancada.
```

## Status

`nrf54-app` é o mais maduro: gerencia uma frota de atuadores via BLE
Central, expõe controle via BLE Peripheral pra uma interface web, allow-list
persistente. O link UART 54↔91 (CRC16 + ACK/retry) está validado em
bancada; `exercises/uart-link` continua como banco de testes isolado
dele. O `nrf91-app` já tem MQTT integrado (paridade de comandos com a
interface, validado publicando no broker de sandbox), mas a validação
está parada por rejeição de rede do SIM, não por bug de firmware - ver
`docs/PROTOCOLO_91_MQTT.md`. O papel de atuador (I2C, motor/sensores) começou a ser
portado - sensores de posição e torque (ADS1000) validados em bancada,
ver `docs/SENSORES_I2C.md`; e a lógica de comando e controle
(abrir/fechar/parar, posicionamento, modo de operação, motor de
alarmes) portada do fwBLE e validada em bancada, ver
`docs/COMANDO_CONTROLE.md` (ESD/PST e RS485 ainda pendentes). O mapa de
parâmetros do fwBLE (`paramDado`) foi portado tal e qual, ver
`docs/PARAMETROS.md`. O registro de eventos (`atRegEvent`) foi portado
para a flash SPI externa (MX25R6435F, a mesma do DK, escolhida para a
placa), com leitura pela interface; validação em bancada pendente, ver
`docs/REGISTRO_EVENTOS.md`. Hoje o `nrf54-app` roda os dois papéis (atuador +
gateway) sempre, em toda unidade: o gateway **opcional** por unidade,
decidido em `docs/DECISOES_PRODUTO.md`, ainda não foi implementado (ver
"Pendências registradas" abaixo).

## Pendências registradas

Itens já decididos, mas ainda não feitos. Ficam listados aqui pra não
se perderem entre os documentos de cada área. Os quatro primeiros devem
ser resolvidos antes de qualquer unidade ir pra campo.

1. **Gatilhos temporários de bancada.** `nrf54-app`: **removidos** na
   branch `feature-comandos-torque` (os botões do DK não comandam mais
   o motor, e saiu o log periódico de sensores do `main()`). No
   `nrf91-app`, os botões 1/2 ficam como forma manual de forçar resync
   (decisão em `docs/PROTOCOLO_54_91.md`) e não precisam sair.
2. **Watchdog.** `nrf54-app`: **feito** na branch
   `feature-comandos-torque` (`src/watchdog.c`). É o `task_wdt` com
   canais "controle" (500 ms) e "sensores" (2 s), e o WDT31 como
   fallback de hardware. Um canal estourado desliga o motor e reinicia a
   placa. Para validar na bancada, use `WATCHDOG_TESTE` no
   `watchdog.c`. Validado em bancada. Ainda pendente: o watchdog do
   `nrf91-app`, menos crítico porque ele não aciona motor.
3. **Keepalive MQTT.** `CONFIG_MQTT_KEEPALIVE` não está setado no
   `nrf91-app/prj.conf` (fica no default de 60s, ~8,4 MB/mês só de
   ping). Trocar pra 1200s - ver "Orçamento de dados" em
   `docs/PROTOCOLO_91_MQTT.md`.
4. **Senha de acesso de verdade.** A senha pedida ao conectar
   (`askAccessPin()` em `nrf54-app/index.html`) é PROVISÓRIA, feita para
   a feira: só a interface confere, o firmware não exige nada e a senha
   é lida em claro. A solução comum ao SIM Connect e ao fwBLE (pareamento
   LESC + comando `IFCC_AUTH` + trava no firmware) está em
   `docs/SEGURANCA_SENHA.md`.
5. **(Futuro) Gateway opcional por unidade.** Hoje toda unidade roda
   BLE Central (fleet management) + link UART com o 91. Pela decisão de
   `docs/DECISOES_PRODUTO.md`, isso deve ser ligado só nas unidades
   configuradas como gateway (menu de configuração, persistido em ZMS
   como a allow-list); as demais ficam só com o papel de atuador (I2C +
   BLE Peripheral).
6. **(Futuro) Data/hora nos eventos.** O registro de eventos grava o
   tempo desde o boot (`mes = 0`), porque não há RTC com bateria. Falta
   uma fonte de data/hora: área RTC do fwBLE (`0x00803000`) +
   `IFCC_UPDATE_RTC` acertada pela interface e/ou a hora da rede vinda do
   nRF91 pelo link UART. Ver `docs/REGISTRO_EVENTOS.md`.

## Toolchain

nRF Connect SDK (Zephyr) v3.4.0 (LTS), instalada em `C:\ncs\v3.4.0`
(migrado da v3.3.0 em 2026-08-14, ver `docs/PROTOCOLO_54_91.md` pro
achado específico dessa migração - partição do nRF91).

## Como buildar

`west` não fica no PATH do shell por padrão nesta máquina: ele vem
dentro do toolchain instalado pelo nRF Connect for VS Code. A receita
abaixo, em PowerShell, replica o `environment.json` desse toolchain. É
a usada em todos os builds desde 2026-10 (o `nrfutil sdk-manager` não
está instalado nesta máquina):

```
$tc = 'C:\ncs\toolchains\dcbdc366a1'
$env:PATH = "$tc;$tc\mingw64\bin;$tc\bin;$tc\opt\bin;$tc\opt\bin\Scripts;$tc\opt\nanopb\generator-bin;$tc\nrfutil\bin;$tc\opt\zephyr-sdk\gnu\arm-zephyr-eabi\bin;" + $env:PATH
$env:PYTHONPATH = "$tc\opt\bin;$tc\opt\bin\Lib;$tc\opt\bin\Lib\site-packages"
$env:ZEPHYR_TOOLCHAIN_VARIANT = 'zephyr/gnu'
$env:ZEPHYR_SDK_INSTALL_DIR = "$tc\opt\zephyr-sdk"
$env:ZEPHYR_BASE = 'C:\ncs\v3.4.0\zephyr'
$env:NRFUTIL_HOME = "$tc\nrfutil\home"

cd nrf54-app   # ou nrf91-app, ou exercises/uart-link
west build -d build                                      # build ja configurado
west build -b nrf54lm20dk/nrf54lm20b/cpuapp/ns -d build  # primeira vez
```

(troque a board por `nrf9151dk/nrf9151/ns` pro `nrf91-app`, ou
`nrf54lm20dk/nrf54lm20b/cpuapp` pro `exercises/uart-link`.) O hash do
toolchain (`dcbdc366a1`) pode mudar se o nRF Connect for VS Code
atualizar a versão instalada — confira em `C:\ncs\toolchains\` se o
comando falhar com "not found" ou com erro de versão do Zephyr-sdk
incompatível (sinal de que outro hash de toolchain, com SDK mais
antigo, foi pego por engano).
