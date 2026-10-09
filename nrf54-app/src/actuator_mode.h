/*
 * Arbitro de modo de operacao do atuador - porta de BLE/Atuador/atModo.c
 * (fwBLE). Decide, a cada ciclo, QUAL fonte pode comandar o movimento e
 * trata as transicoes de modo (que ligam 3 dos alarmes portados:
 * MODO_NAO_REMOTO, LOCAL_INIBIDO, PARADA_LOCAL).
 *
 * REMAP em relacao ao fwBLE (decisao com o Felipe, 2026-09-04 - ver
 * memoria sim-connect-command-control-port-strategy): o fwBLE pega o
 * modo de um seletor fisico (ifIhmModo()) e trata a interface BLE como
 * fonte REMOTA (ifFerConfigRunCmd() dentro de atModoRemoto()). Aqui nao
 * ha seletor fisico e a interface BLE tem a propriedade de "presenca
 * fisica" que o painel local tinha, entao:
 *   - LOCAL  = interface BLE (area Painel Remoto, actuator_panel.c)
 *   - REMOTO = RS485 / MB TCP / broker MQTT (nenhuma existe ainda - passo 9)
 * O modo e' setado pelo campo `mode` (bits 4-6) da propria palavra do
 * Painel Remoto (= ifFerConfigCmdRemPanelCtl.mode); valor 7 = "nao
 * mudar".
 *
 * STUBS de passos futuros:
 *   - ESD: atModoLocal()/atModoRemoto() no original checam atESDCmd()
 *     antes de tudo. ESD e' passo 6.
 *   - atModoInibLoc(): paramDado.inibCmdLoc || ifDigInibLoc() - passo 8
 *     (parametros). Por ora sempre false.
 *   - Fonte de comando REMOTA: passo 9. Por ora, em REMOTO nada comanda.
 *   - atRegEvent(EV_LOC_*): substituido por LOG.
 */
#ifndef ACTUATOR_MODE_H_
#define ACTUATOR_MODE_H_

#include <stdbool.h>

/* Espelha AT_MODO_t (atModo.h). Valores vao no fio (campo `mode` do
 * Painel Remoto e byte de status em actuator_service.c) - nao renumerar.
 */
enum actuator_mode {
	ACTUATOR_MODE_DESLIGA = 0, /* RESERVADO - hoje colapsado em PARA (ver actuator_mode_set) */
	ACTUATOR_MODE_PARA = 1,
	ACTUATOR_MODE_LOCAL = 2,  /* interface BLE */
	ACTUATOR_MODE_REMOTO = 3, /* RS485 / MB TCP / MQTT (passo 9) */
	ACTUATOR_MODE_PARAM = 4,  /* vestigial (menu de nav do HMI fisico) - sem UI */
	ACTUATOR_MODE_INFO = 5,   /* vestigial - sem UI */
};

/* Valor do campo `mode` do Painel Remoto que significa "manter o modo
 * atual" - comandos normais carregam este valor (so' um write que
 * QUER trocar de modo manda 0..5).
 */
#define ACTUATOR_MODE_NOCHANGE 7

/* Modo default no boot: LOCAL (unica fonte que existe hoje). Quando
 * RS485/MQTT existirem, isto provavelmente vira um parametro persistido
 * (passo 8) - uma unidade de campo controlada por SCADA deve bootar em
 * REMOTO.
 */
void actuator_mode_init(void);

/* Troca o modo (idempotente). Chamado por actuator_panel.c quando o
 * campo `mode` da palavra do Painel != ACTUATOR_MODE_NOCHANGE. Ignora
 * valores fora de 0..5.
 */
void actuator_mode_set(enum actuator_mode mode);

enum actuator_mode actuator_mode_get(void);

/* true se o comando local (interface BLE) esta inibido - atModoInibLoc()
 * (paramDado.inibCmdLoc). actuator_panel.c nao despacha comandos enquanto
 * isso for verdade.
 */
bool actuator_mode_local_inibido(void);

/* Uma passada do arbitro - equivalente a atModoAcao(). Chamado a cada
 * ciclo por actuator_control_run() no lugar onde o fwBLE chama
 * atComandoOrigem()->atModoAcao(). Consome o Painel Remoto (lease +
 * campo de modo sempre; comandos so' em modo LOCAL) e, conforme o modo,
 * libera a fonte de comando ou forca parada.
 */
void actuator_mode_run(void);

#endif /* ACTUATOR_MODE_H_ */
