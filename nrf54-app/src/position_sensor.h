/*
 * Sensor de posicao (potenciometro sobre ADS1000, I2C @0x49) - fina
 * instancia do motor generico ads1000.c. Modulo agnostico de BLE/UART
 * de proposito, no mesmo espirito de actuator_client.c: quem decide o
 * que fazer com o valor (logar, mandar por BLE/UART, etc.) e o
 * main.c, nao este arquivo.
 */
#ifndef POSITION_SENSOR_H_
#define POSITION_SENSOR_H_

#include "device_health.h"

#include <stdint.h>
#include <stdbool.h>

/* Confere se o barramento I2C esta pronto e agenda o motor de
 * amostragem periodica. Retorna false sem travar o boot do resto do
 * Gateway se o sensor nao estiver plugado/pronto - mesmo padrao de
 * uart_link_init() (nao fatal).
 */
bool position_sensor_init(void);

/* Ultimo valor lido com sucesso (cache, nao acessa o I2C). Ver
 * ads1000_last_raw() para a semantica exata do retorno.
 */
bool position_sensor_last_raw(uint16_t *raw_out);

/* true se o ADS1000 de posicao estiver respondendo (rastreio de saude,
 * device_health.h) - usado por actuator_sensors.c pra saber quando
 * invalidar o filtro (ver sample_filter_reset()) em vez de continuar
 * reportando a media de amostras antigas indefinidamente.
 */
bool position_sensor_is_online(void);

/* Diagnostico (ver ads1000_health()) */
const struct device_health *position_sensor_health(void);

#endif /* POSITION_SENSOR_H_ */
