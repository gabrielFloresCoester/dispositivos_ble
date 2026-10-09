/*
 * Sensor de torque (ADS1000, I2C).
 *
 * Mesmo chip e protocolo do sensor de posicao (ver position_sensor.c
 * e o comentario grande em ads1000.c) - so' o endereco e o ganho
 * mudam. Endereco: ENDER_SENSOR_TORQUE 0x90 no devices.h original
 * (8 bits, proCo) = 0x90 >> 1 = 0x48 em 7 bits (Zephyr).
 *
 * GANHO: devices.c (dev_ads_get_config_cb) usa
 * paramDado.fabGanSenTorq para este endereco - um parametro de
 * calibracao por unidade (confirmado: default 0, configuravel 0-3).
 * Este projeto ainda nao tem um subsistema de calibracao/parametros
 * (equivalente a paramDado) - por enquanto usa o default 0 fixo;
 * torque_sensor_set_gain() fica pronta para quando esse subsistema
 * existir.
 */

#include "torque_sensor.h"
#include "ads1000.h"

#include <zephyr/devicetree.h>
#include <zephyr/device.h>

#define I2C_NODE DT_NODELABEL(i2c21)

/* Endereco 7 bits, ver nota acima (era 0x90/8 bits no proCo original) */
#define TORQUE_SENSOR_ADDR 0x48

/* paramDado.fabGanSenTorq: default confirmado 0, configuravel 0-3 */
#define TORQUE_SENSOR_GAIN_DEFAULT 0

static struct ads1000_dev dev;

bool torque_sensor_init(void)
{
	const struct device *i2c_bus = DEVICE_DT_GET(I2C_NODE);

	return ads1000_init(&dev, i2c_bus, TORQUE_SENSOR_ADDR, TORQUE_SENSOR_GAIN_DEFAULT,
			     "torque");
}

bool torque_sensor_last_raw(uint16_t *raw_out)
{
	return ads1000_last_raw(&dev, raw_out);
}

bool torque_sensor_is_online(void)
{
	return ads1000_is_online(&dev);
}

void torque_sensor_set_gain(uint8_t gain)
{
	ads1000_set_gain(&dev, gain);
}

const struct device_health *torque_sensor_health(void)
{
	return ads1000_health(&dev);
}
