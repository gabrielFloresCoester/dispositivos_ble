/*
 * Painel Remoto - entrada de comando do papel atuador vinda de fora
 * (interface BLE hoje; RS485/MB TCP/MQTT no futuro). Porta de
 * ifFerConfigRunCmd() + ifFerConfigCmdRemPanelCtl (BLE/Atuador/
 * ifFerConfig.c/.h no fwBLE).
 *
 * A escrita chega por acgl GTM_SEND no endereco
 * MSG_EX_ADDRESS_CONFIG_REM_PANEL_CTL (0x00805900) - endereco REAL do
 * fwBLE, mantido pra que um Gateway real comande uma unidade SIM Connect
 * em papel de atuador do mesmo jeito que comanda um Atuador BLE legado
 * (ver memoria sim-connect-command-control-port-strategy). O tratamento
 * do frame acgl fica em actuator_service.c; este modulo so' guarda a
 * palavra de comando e a consome a cada ciclo (actuator_panel_run()).
 *
 * Palavra de 16 bits (little-endian), layout identico ao
 * ifFerConfigCmdRemPanelCtl_t:
 *   bit 0     cmd_open   - pede ABERTO (demanda 1000)
 *   bit 1     cmd_close  - pede FECHADO (demanda 0)
 *   bit 2     cmd_stop   - para
 *   bit 3     ack_alarm  - quita alarmes (escopo Local - ver enum
 *                          actuator_alarm_clear_mode)
 *   bits 4-6  mode       - modo de operacao (consumido pelo arbitro de
 *                          modo, passo 5 - por enquanto ignorado)
 *   bits 7-13 spare
 *   bit 14    alloc      - renova o lease de comando (5 s)
 *   bit 15    free       - libera o lease
 *
 * LEASE (TIME_LEASE = 5000 ms no fwBLE): comando so' vale enquanto o
 * lease esta ativo. A interface escreve alloc=1 periodicamente (< 5 s)
 * pra manter; free=1 (ou 5 s sem alloc) encerra. Isso evita que um
 * comando "preso" (interface que caiu no meio) siga valendo.
 */
#ifndef ACTUATOR_PANEL_H_
#define ACTUATOR_PANEL_H_

#include <stdbool.h>
#include <stdint.h>

/* MSG_EX_ADDRESS_CONFIG_REM_PANEL_CTL no ifFerConfig.h original */
#define ACTUATOR_PANEL_AREA_ADDR 0x00805900UL
#define ACTUATOR_PANEL_AREA_LEN  2U

void actuator_panel_init(void);

/* Guarda a palavra de comando recebida (substitui a anterior por
 * inteiro - mesmo modelo do memcpy do ifFerConfigReceive original).
 * Chamado do contexto de escrita BLE (actuator_service.c).
 */
void actuator_panel_write_word(uint16_t word);

/* Consome a palavra: aplica o lease (alloc/free), e - se o lease estiver
 * ativo - executa quita-alarme / stop / close / open (nessa prioridade),
 * limpando os bits de comando ja atendidos. Chamado a cada ciclo do
 * loop de controle (actuator_control_run()). Retorna true se executou
 * algum comando neste ciclo (equivalente ao retorno de
 * ifFerConfigRunCmd(), pra uso futuro pelo arbitro de modo - passo 5).
 */
bool actuator_panel_run(void);

#endif /* ACTUATOR_PANEL_H_ */
