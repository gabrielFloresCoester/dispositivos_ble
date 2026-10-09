/*
 * Sensor de torque ON/OFF (PCA9536, I2C @0x41) - variante digital,
 * alternativa ao ADS1000 analogico (torque_sensor.h). Ver
 * torque_onoff_sensor.c pro protocolo e as confirmacoes de hardware
 * (chip, fiacao, polaridade) feitas com o Felipe em 2026-08-26.
 *
 * Selecao automatica entre esta variante e a analogica
 * (torque_sensor.h) acontece em actuator_sensors.c, a cada ciclo - ver
 * docs/SENSORES_I2C.md.
 */
#ifndef TORQUE_ONOFF_SENSOR_H_
#define TORQUE_ONOFF_SENSOR_H_

#include "device_health.h"

#include <stdint.h>
#include <stdbool.h>

/* Confere o barramento I2C e agenda o motor de amostragem periodica
 * (15ms). Retorna false sem travar o boot se o barramento nao estiver
 * pronto - mesmo padrao de position_sensor_init()/torque_sensor_init().
 */
bool torque_onoff_sensor_init(void);

/* abertura/fechamento (saida) = true quando HA sobretorque. Microchaves
 * Normalmente Fechado (confirmado com o Felipe, 2026-08-26): nivel
 * logico ALTO no registrador Input Port do PCA9536 = contato aberto
 * pela ativacao = ha sobretorque; nivel baixo = repouso (contato
 * fechado, via pull-up), sem sobretorque. Sem inversao de polaridade -
 * ver nota grande em torque_onoff_sensor.c. Retorna false se ainda nao
 * houver leitura valida (ou o sensor estiver offline - ver
 * torque_onoff_sensor_is_online()).
 */
bool torque_onoff_sensor_last(bool *abertura, bool *fechamento);

bool torque_onoff_sensor_is_online(void);

/* Diagnostico (ver ads1000_health() em ads1000.h) */
const struct device_health *torque_onoff_sensor_health(void);

#endif /* TORQUE_ONOFF_SENSOR_H_ */
