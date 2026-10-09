/*
 * Protecao por torque do papel atuador - porta de sptTratTrq() e
 * sptTrqParaLimit() (BLE/Atuador/sensPosTor.c, fwBLE). Liga o sensor de
 * torque ao controle: alarmes de sobretorque (abrindo/fechando), valvula
 * travada, operacao incompleta e o assentamento por torque no fechamento
 * ("Fechamento com Torque", paramDado.trqFechad).
 *
 * As duas variantes de sensor do original: celula de carga (analogico,
 * ADS1000 - Nm contra os limites) e microchaves (ON/OFF, PCA9536 - o ramo
 * #ifdef PAINEL_CQT). Usa a fonte que actuator_sensors.c escolheu.
 *
 * Roda dentro do laco de controle de 10 ms (actuator_control_run()), que
 * e' o mesmo ritmo do original - os limiares em "ciclos" (3 ciclos de
 * filtro, +150 ciclos ~ 1,5 s com trqFechad) valem 1:1.
 */
#ifndef ACTUATOR_TORQUE_H_
#define ACTUATOR_TORQUE_H_

#include <stdbool.h>

#include "actuator_alarm.h"

/* AL_COM_TRQ - falha do sensor de torque. Chamar a cada ciclo do
 * controle, ANTES do retorno antecipado por falta de posicao (como o
 * COM_SENS_POS), pra que rode sempre. Liga o alarme se NENHUMA fonte de
 * torque (celula de carga ou microchaves) estiver online por
 * TEMPO_FALHA_SENSOR (3 s, inclusive no boot); libera assim que alguma
 * voltar. Uma placa sem nenhum sensor de torque fica bloqueada - como um
 * atuador real com o sensor quebrado (no fwBLE o sensor sempre existe).
 */
void actuator_torque_check_com(void);

/* Uma passada do tratamento de torque - chamar a cada ciclo do controle,
 * DEPOIS de decidir o status de movimento do ciclo e ANTES de
 * consultar os alarmes que bloqueiam movimento (atAlarme()), pra que um
 * sobretorque detectado no ciclo ja' pare o motor no mesmo ciclo.
 */
void actuator_torque_run(enum actuator_mov_status status);

/* sptTrqParaLimit(): true = pode desligar o motor agora. Com "Fechamento
 * com Torque" e o atuador no limite fechado, segura o motor fechando por
 * ate' TEMPO_FECHAN_TRQ pra assentar a valvula - ver o .c pra divergencia
 * deliberada com microchaves.
 */
bool actuator_torque_para_limit(enum actuator_mov_status status);

#endif /* ACTUATOR_TORQUE_H_ */
