/*
 * Saida de acionamento do motor (papel atuador) - GPIO direto no nRF54,
 * NAO uma mensagem pra placa FSA como no fwBLE original. BLE/Atuador/
 * foSeAc.c fala com um dispositivo FSA separado via I2C/comScan
 * (proCo/devices/devs/dev_fsa.c) - o SIM Connect e' hardware unificado
 * numa placa so', sem FSA, entao o motor e' acionado direto por 2 pinos
 * digitais (decisao com o Felipe, 2026-09-04 - ver memoria de projeto
 * "sim-connect-command-control-port-strategy").
 *
 * Interface pensada pra ser o que a futura porta de atControle.c
 * (actuator_control.c, ainda nao existe) vai chamar no lugar das funcoes
 * do foSeAc.c original:
 *
 *   fsaIncr()              -> actuator_motor_abre()
 *   fsaDecr()              -> actuator_motor_fecha()
 *   fsaParar()              -> actuator_motor_para()
 *   foSeAcMotorAcionado()   -> actuator_motor_acionado()
 *
 * Pinos (P1.30 abre / P1.31 fecha, ver overlay) sao provisorios de
 * bancada - podem mudar quando o esquematico definitivo do circuito de
 * acionamento existir, sem precisar mudar esta interface.
 *
 * Interbloqueios (logicos, neste modulo): nunca energiza abre e fecha ao
 * mesmo tempo, e na reversao espera um tempo morto (configuravel, default
 * 3000 ms como a FSA do fwBLE - ver actuator_fsa_cfg.h)
 * antes de energizar o sentido oposto - abre()/fecha() chamados dentro
 * do tempo morto deixam o motor desligado e retornam. Rampa de partida
 * fica pra quando o circuito de acionamento real existir (o Driver DC 2
 * tem algo assim em aplicAcioMot(), pra placa dele).
 *
 * A API publica e' so' booleana (acionado sim/nao), como o foSeAc.c real -
 * o sentido do movimento (abrindo/fechando/parado) e' responsabilidade da
 * camada de controle (actuator_control_get_mov_stt(), = at_ctl_get_mov_stt()
 * no fwBLE). Este modulo guarda o sentido internamente so' pra logar a
 * transicao no RTT, nao expoe.
 */
#ifndef ACTUATOR_MOTOR_H_
#define ACTUATOR_MOTOR_H_

#include <stdbool.h>

/* Configura os pinos GPIO como saida, ambos inativos. Retorna false se o
 * device GPIO nao estiver pronto - chamador decide se isso e' fatal (hoje,
 * em main.c, nao e').
 */
bool actuator_motor_init(void);

/* Aciona o motor no sentido de abertura (equivalente a fsaIncr() no
 * fwBLE original). Loga uma transicao grande no RTT na borda parado -> abrindo.
 */
void actuator_motor_abre(void);

/* Aciona o motor no sentido de fechamento (equivalente a fsaDecr()).
 * Loga uma transicao grande no RTT na borda parado -> fechando.
 */
void actuator_motor_fecha(void);

/* Desenergiza as duas saidas (equivalente a fsaParar()). Loga uma
 * transicao grande no RTT ao sair de abrindo/fechando.
 */
void actuator_motor_para(void);

/* true se abrindo ou fechando agora (equivalente a foSeAcMotorAcionado()). */
bool actuator_motor_acionado(void);

#endif /* ACTUATOR_MOTOR_H_ */
