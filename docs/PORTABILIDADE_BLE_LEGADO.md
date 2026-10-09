# O que aproveitar do firmware da linha BLE (nRF52832) na linha SIM

**Não faz parte deste repositório e não deve fazer.** Fica registrado
aqui só o mapa de onde procurar e o que já foi escopado, pra não
precisar reabrir essa investigação do zero.

## Onde está

`C:\Users\gabriel.flores\Documents\GitHub\ControleCoesterBLE`
(repositório GitHub próprio, separado deste). ~4987 arquivos no total,
mas a maior parte é volume esperado, não código de produto:

| Pasta | Arquivos | O que é | Entra no port? |
|---|---|---|---|
| `nRF5_SDK_17.0.0_9d13099/` | 1248 | SDK da Nordic vendorizada inteira | Não — só consulta pontual de API/registrador se precisar |
| `BLE/` | 517 | **O produto de verdade** | Sim — ver detalhamento abaixo |
| `BLE_IHM/` | 301 | Variante/ambiente diferente do mesmo hardware | Não (confirmado com o Gabriel, 2026-08-06) |
| `BLE_NV/` | 270 | Variante/ambiente diferente do mesmo hardware | Não (idem) |
| `vault/` | 2 | Desconhecido, possivelmente credencial | Não — nunca abrir sem necessidade clara |

Projeto é IAR Embedded Workbench (`.ewp`/`.ewd`/`.eww`), nRF52832 +
SoftDevice S132 (`iar_s132_nrf52832_xxaa.icf`).

## Dentro de `BLE/`, o que interessa

- **`BLE/proCo/`** — a implementação do protocolo proCo em si:
  `proCoM.c`, `proCoMsg.c`, `proCoMsgEx.c`, `proCoX.c`, mais as
  "areas" de memória que o protocolo endereça (`areaFSA.c` = Fonte/
  Sensoriamento/Atuação, `areaIOA.c`/`areaIOD.c` = IO Analógica/
  Digital, `areaRede.c` = rede). **É o núcleo que precisa bater
  byte-a-byte** com o que `nrf54-app/src/actuator_client.c` já espera
  do lado do Gateway — não é um redesign, é replicar o contrato existente.
- **`BLE/Atuador/`** — a lógica de controle: `atCurvTorq.c` (curva/
  torque), `atRegEvent.c` (registro de eventos), `sensPosTor.c`
  (sensor de posição/torque), `atModo.c`, `at_alarm.c`. Bate com os
  campos que já vimos passar pelo Gateway via `PROTOCOLO_INTERFACE.md`.
- **`BLE/HAL/`** — só como referência de comportamento (que
  registrador, que timing), não pra copiar. Tem nomenclatura de
  periférico da TI/MSP430 (`halI2cMrUSCI.c`, `hal_I2C_Mestre_USART.c`)
  por baixo do nRF52832 atual — sinal de que esse HAL já carrega
  histórico de uma geração anterior. Reforça: reescrever pra API I2C
  do Zephyr, não portar o arquivo.

## O padrão ComScan (vale reaproveitar a ideia, não o barramento)

`BLE/proCo/comScan.c` é um motor genérico de comunicação com
dispositivos I2C: cada dispositivo (FSA, IO Digital, IO Analógica,
Rede, sensores...) se registra com 4 callbacks —
`getTX`/`getRX`/`confirmTX`/`acceptRX` — num motor comum que roda um
ciclo fixo (15ms) com máquina de estado e **contagem de falha/taxa de
falha por dispositivo** (`countFailure`, `failureRate`,
`comScanOnlineDev`).

`pEntradas`/`pSaidas` (mencionados nas conversas) são campos da struct
de dados especificamente da placa IO Digital (`ifDig.c`), um exemplo
de "dispositivo" plugado nesse motor — não um padrão à parte.

Esse padrão (motor genérico + callback plugável + saúde por
dispositivo) é bus-agnóstico e já rima com `struct actuator_manager_cb`
em `actuator_client.c`. Vale usar a mesma ideia no link 54↔91 (ver
`docs/PROTOCOLO_54_91.md`), mas **sem** o ciclo fixo de 15ms — isso é
uma restrição do I2C (mestre precisa varrer todo mundo), e o link
54↔91 é ponto-a-ponto com push assíncrono dos dois lados, não precisa
dessa rigidez.

## Próximo passo (ainda não feito)

Ler `proCoM.c`/`proCoMsg.c`/`proCoX.c` a fundo e comparar campo a
campo com o que `actuator_client.c` já implementa do lado do Gateway, pra
confirmar que o contrato bate e mapear onde precisa da lógica de
`atCurvTorq.c`/`sensPosTor.c`/etc. entrar no port pro nRF54 (Zephyr,
I2C nativo, sem SoftDevice).
