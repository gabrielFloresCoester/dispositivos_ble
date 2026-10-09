/*
 * Sensor de posicao (potenciometro sobre ADS1000, I2C).
 *
 * Mesmo sensor do firmware BLE legado (ControleCoesterBLE/proCo,
 * ENDER_SENSOR_POSICAO 0x92 em devices.h). 0x92 e endereco de 8 bits
 * (convencao proCo, byte ja deslocado 1 posicao com o bit de R/W em
 * 0) - em 7 bits, convencao da API I2C do Zephyr, isso e 0x92 >> 1 =
 * 0x49.
 *
 * O protocolo em si (escrita de config, leitura de 3 bytes + echo,
 * auto-recuperacao) mora em ads1000.c, compartilhado com
 * torque_sensor.c - ver o comentario grande la' para o porque desse
 * protocolo (nao e' o "ponteiro de registrador" que o exercicio da
 * Academy usava).
 *
 * GANHO: 0 - devices.c (dev_ads_get_config_cb) so' retorna ganho != 0
 * para ENDER_SENSOR_TORQUE; o sensor de posicao sempre usa 0.
 */

#include "position_sensor.h"
#include "ads1000.h"

#include <zephyr/devicetree.h>
#include <zephyr/device.h>

#define I2C_NODE DT_NODELABEL(i2c21)

/* Endereco 7 bits, ver nota acima (era 0x92/8 bits no proCo original) */
#define POSITION_SENSOR_ADDR 0x49
#define POSITION_SENSOR_GAIN 0

static struct ads1000_dev dev;

bool position_sensor_init(void)
{
	const struct device *i2c_bus = DEVICE_DT_GET(I2C_NODE);

	return ads1000_init(&dev, i2c_bus, POSITION_SENSOR_ADDR, POSITION_SENSOR_GAIN, "posicao");
}

bool position_sensor_last_raw(uint16_t *raw_out)
{
	return ads1000_last_raw(&dev, raw_out);
}

bool position_sensor_is_online(void)
{
	return ads1000_is_online(&dev);
}

const struct device_health *position_sensor_health(void)
{
	return ads1000_health(&dev);
}
