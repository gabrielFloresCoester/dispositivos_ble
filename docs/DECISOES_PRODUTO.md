# Decisões de arquitetura de produto — linha SIM

Registro das decisões tomadas em 2026-08-06, pra não depender só do
histórico de conversa. O raciocínio visual completo (diagramas
comparando os caminhos) foi publicado como Artifact — link no fim
deste documento — mas não deve ser a única cópia dessa informação.

## Contexto

O Gateway (fala com atuadores legados via BLE, joga pra nuvem) e
o Atuador Coester (motor, sensores, periféricos, quase tudo I2C) vão
nascer sobre o mesmo hardware base: nRF54 (BLE) + nRF9151 (celular).
Não estava decidido se seriam produtos separados ou um produto único.

## Decisão: Caminho B — produto único

Não vai haver "Gateway" e "Atuador" como SKUs separados. Um único
firmware/hardware, papel decidido por unidade.

**Por quê** (resumo do que está no Artifact): migrar de um produto
único pra separados depois (Caminho B → A) é basicamente *subtrair*
código já integrado e testado; migrar de separados pra único (A → B)
é *integrar do zero* uma combinação que nunca foi validada — os dois
pivôs de decisão tardia não custam o mesmo, e essa assimetria pesou a
favor de decidir já pelo único, dado que não havia sinal de mercado
puxando pra produtos separados.

## Modelo de papel: atuador sempre, gateway opcional

Não é "toda unidade roda os dois papéis de BLE o tempo todo". É:

- **Toda unidade é atuador sempre** — I2C (motor/sensores) + BLE
  Peripheral, comportamento parecido com o que a linha BLE atual já
  tem hoje (ver `docs/PORTABILIDADE_BLE_LEGADO.md`).
- **Ser gateway é opcional, por unidade**, escolhido via menu de
  configuração (mesmo mecanismo de persistência que a allow-list já
  usa hoje — ZMS). Só unidade configurada como gateway ativa o papel
  BLE Central (fleet management) e o link UART + MQTT com o 91.

Isso resolve dois problemas de uma vez: quem decide qual unidade é o
gateway do grupo (decisão de configuração, não negociação em tempo
real), e o orçamento de papéis BLE do controlador (só quem é gateway
de verdade disputa Central+Peripheral ao mesmo tempo).

**Status (2026-09-30): decidido, ainda não implementado — ponto
futuro.** Hoje o `nrf54-app` roda os dois papéis em toda unidade (BLE
Central + link UART com o 91 sempre ativos). O menu de configuração
que liga o papel de gateway por unidade ainda não existe. Registrado
em "Pendências registradas" no `README.md`.

## Nomenclatura

- **Nome comercial do equipamento**: "SIM Connect" (encaminhado,
  confirmado pelo Gabriel em 2026-08-06).
- **Nome do repositório/projeto de firmware**: `sim-connect-fw`,
  deliberadamente desacoplado do nome comercial final (se o marketing
  mudar de ideia, não precisa renomear repositório/CMake de novo).
- "Gateway" (não "Hub") é o termo usado pro *papel* que uma unidade
  pode assumir — não é nome de produto à parte, é modo de operação.

## FOTA

Requisito confirmado: firmware do nRF54 atualizável via BLE **ou** via
NB-IoT (relay pelo 91); firmware do 91 com o mesmo princípio
(atualizável por mais de um caminho). Ver `docs/PROTOCOLO_54_91.md`
pro estado do design (aponta pra investigar MCUmgr/SMP do Zephyr antes
de desenhar do zero).

## UART entre 54 e 91 (não SPI, não I2C)

Decisão confirmada, raciocínio completo na conversa original: SPI e
I2C são barramentos com mestre (um lado só fala quando o controller
"clocka" a transação) — mal ajuste pro nosso caso, porque boa parte do
tráfego é o 54 empurrando dado sem ser perguntado (status mudou,
atuador conectou). UART não tem essa assimetria, os dois lados falam
quando querem. Reconsiderar só se FOTA cross-chip precisar de
throughput que a UART não entregue na prática.

## Artifact com os diagramas

https://claude.ai/code/artifact/f6cce395-eafa-4245-8a49-c57a29af024f

Contém: comparação visual dos dois caminhos (módulos, orçamento BLE),
e a análise de custo dos dois sentidos de pivô tardio (A→B vs B→A).
Privado por padrão — só visível a quem o Gabriel compartilhar.
