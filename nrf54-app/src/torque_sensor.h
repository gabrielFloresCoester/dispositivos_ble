/*
 * Sensor de torque (mesmo chip ADS1000 do sensor de posicao, I2C
 * @0x48) - fina instancia do motor generico ads1000.c. Variante
 * analogica (ENDER_SENSOR_TORQUE) - NAO a variante digital ON/OFF
 * (ENDER_SENSOR_TORQUE_ONOFF), que e' protocolo diferente (1 byte,
 * sem echo) e nao esta implementada ainda.
 */
#ifndef TORQUE_SENSOR_H_
#define TORQUE_SENSOR_H_

#include "device_health.h"

#include <stdint.h>
#include <stdbool.h>

/* Confere se o barramento I2C esta pronto e agenda o motor de
 * amostragem periodica. Retorna false sem travar o boot do resto do
 * Gateway se o sensor nao estiver plugado/pronto - mesmo padrao de
 * uart_link_init() (nao fatal).
 */
bool torque_sensor_init(void);

/* Ultimo valor lido com sucesso (cache, nao acessa o I2C). Ver
 * ads1000_last_raw() para a semantica exata do retorno.
 */
bool torque_sensor_last_raw(uint16_t *raw_out);

/* true se o ADS1000 analogico estiver respondendo (rastreio de saude,
 * device_health.h) - usado pela selecao automatica de variante em
 * actuator_sensors.c.
 */
bool torque_sensor_is_online(void);

/* Troca o ganho em runtime (0-3, equivalente a paramDado.fabGanSenTorq
 * no proCo original - default 0, configuravel por unidade). Ainda nao
 * existe subsistema de calibracao/parametros neste projeto (algo
 * equivalente a paramDado) - por enquanto so' o default e' usado; esta
 * funcao fica pronta para quando esse subsistema existir.
 */
void torque_sensor_set_gain(uint8_t gain);

/* Diagnostico (ver ads1000_health()) */
const struct device_health *torque_sensor_health(void);

#endif /* TORQUE_SENSOR_H_ */
