/*
 * Motor generico de leitura do ADS1000 (I2C) - ver ads1000.h.
 *
 * Protocolo replicado de BLE/proCo/devices/devs/dev_ads.c
 * (ControleCoesterBLE, driver de producao de verdade - nao o
 * exercicio didatico da Academy, que usava um "ponteiro" que o
 * ADS1000 nem tem):
 *
 *   - Escreve 1 byte de config (ADS1000_CONFIG_DEFAULT=0x80 OR'ado
 *     com o ganho da instancia) so' na primeira vez / quando precisa
 *     reconfigurar - nao a cada leitura.
 *   - Cada leitura pede 3 bytes: [MSB][LSB][echo da config]. Se o
 *     echo nao bater com a config esperada, o sensor "esqueceu" a
 *     config (glitch, power-on) - volta a escrever antes de confiar
 *     em leituras futuras.
 *
 * Isto e' a mesma maquina de 2 estados do dev_ads_t.stt original
 * (WRITE/READ), sem o 3o estado DAS_READY - esse existia so' por
 * causa do agendamento round-robin do ComScan entre varios
 * dispositivos no mesmo barramento fisico; aqui cada instancia tem seu
 * proprio work item periodico e o driver I2C do Zephyr ja serializa o
 * acesso ao barramento entre eles.
 *
 * RASTREIO DE SAUDE (device_health.c, equivalente a comScanOnlineDev()):
 * "sucesso", pro rastreio, significa uma leitura com o echo de config
 * batendo - nao so' o barramento I2C ter respondido. Uma escrita de
 * config bem-sucedida sozinha nao conta ainda (a leitura seguinte que
 * prova, com o echo); um echo que nao bate conta como FALHA (mesmo o
 * barramento tendo respondido fisicamente), porque nao era um ADS1000
 * corretamente configurado no outro lado. Essa distincao importa pra
 * deteccao automatica de variante de sensor: "esta' online" precisa
 * significar "e' genuinamente um ADS1000 respondendo certo", nao so'
 * "algo ACKed no endereco".
 */

#include "ads1000.h"
#include "sensor_workq.h"

#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ads1000, LOG_LEVEL_INF);

/* CONFIG_DEFAULT do dev_ads.c original */
#define ADS1000_CONFIG_DEFAULT 0x80

/* TEMPO_CICLO_MS do proCo original (devices.h) */
#define ADS1000_POLL_MS 15

static uint8_t config_byte(const struct ads1000_dev *dev)
{
	return ADS1000_CONFIG_DEFAULT | dev->gain;
}

static void poll_work_handler(struct k_work *work)
{
	struct ads1000_dev *dev =
		CONTAINER_OF(k_work_delayable_from_work(work), struct ads1000_dev, poll_work);
	bool was_online = device_health_is_online(&dev->health);
	int err;

	if (dev->poll_state == ADS1000_ST_WRITE) {
		uint8_t config = config_byte(dev);

		err = i2c_write(dev->i2c_bus, &config, sizeof(config), dev->address);
		if (err) {
			device_health_record_failure(&dev->health);
			/* Sem isto, "nunca conectado" e "conectado mas sem
			 * responder" ficam identicos no log (silencio nos dois
			 * casos) - so' a transicao online->offline loga hoje, e
			 * ela nunca dispara se o dispositivo nunca chegou a ficar
			 * online. Um aviso so' (nao a cada ciclo de 15ms) assim
			 * que a sequencia de falhas bate o teto.
			 */
			if (!dev->health.ever_succeeded &&
			    device_health_count_failure_sequent(&dev->health) >=
				    DEVICE_HEALTH_MAX_FAILURE_SEQUENT &&
			    !dev->warned_never_online) {
				LOG_WRN("[%s] Nenhuma resposta do ADS1000 @0x%02x ainda "
					"(i2c_write retornou %d) - fisicamente ausente, "
					"endereco/fiacao errados, ou alimentacao com "
					"problema?",
					dev->name, dev->address, err);
				dev->warned_never_online = true;
			}
			/* Continua em WRITE - tenta de novo no proximo ciclo */
		} else {
			dev->poll_state = ADS1000_ST_READ;
			if (!dev->logged_first_config) {
				LOG_INF("[%s] Config 0x%02x escrita no ADS1000 @0x%02x",
					dev->name, config, dev->address);
				dev->logged_first_config = true;
			} else {
				LOG_WRN("[%s] Config do ADS1000 reescrita (echo tinha "
					"divergido - sensor pode ter perdido a config, "
					"ex: glitch/power-on)", dev->name);
			}
			/* Escrita sozinha nao prova nada ainda pro rastreio de
			 * saude - so' a leitura seguinte com echo batendo conta
			 * como sucesso de verdade (ver nota grande no topo do
			 * arquivo).
			 */
		}
	} else {
		uint8_t rx[3];

		err = i2c_read(dev->i2c_bus, rx, sizeof(rx), dev->address);
		if (err) {
			device_health_record_failure(&dev->health);
			if (!dev->health.ever_succeeded &&
			    device_health_count_failure_sequent(&dev->health) >=
				    DEVICE_HEALTH_MAX_FAILURE_SEQUENT &&
			    !dev->warned_never_online) {
				LOG_WRN("[%s] Config aceita, mas leitura nunca respondeu "
					"(i2c_read retornou %d) - endereco 0x%02x",
					dev->name, err, dev->address);
				dev->warned_never_online = true;
			}
		} else if (rx[2] != config_byte(dev)) {
			/* Echo nao bate: config foi perdida - nao usa este valor,
			 * volta a escrever antes de confiar na proxima leitura.
			 * Conta como falha pro rastreio de saude: o barramento
			 * respondeu, mas nao era um ADS1000 corretamente
			 * configurado do outro lado.
			 */
			device_health_record_failure(&dev->health);
			dev->poll_state = ADS1000_ST_WRITE;
		} else {
			device_health_record_success(&dev->health);
			dev->cached_raw = (rx[0] << 8) | rx[1];
			dev->cached_valid = true;
			dev->warned_never_online = false; /* pode avisar de novo se cair depois */
		}
	}

	bool now_online = device_health_is_online(&dev->health);

	if (was_online && !now_online) {
		LOG_WRN("[%s] Sensor ficou offline (%d falhas consecutivas)", dev->name,
			DEVICE_HEALTH_MAX_FAILURE_SEQUENT);
	} else if (!was_online && now_online) {
		LOG_INF("[%s] Sensor ficou online", dev->name);
	}

	k_work_reschedule_for_queue(sensor_workq_get(), k_work_delayable_from_work(work),
				     K_MSEC(ADS1000_POLL_MS));
}

bool ads1000_init(struct ads1000_dev *dev, const struct device *i2c_bus, uint8_t address,
		   uint8_t gain, const char *name)
{
	if (!device_is_ready(i2c_bus)) {
		LOG_ERR("[%s] Barramento I2C nao esta pronto - sensor indisponivel "
			"(overlay habilitado? fiacao ok?)", name);
		return false;
	}

	*dev = (struct ads1000_dev){
		.i2c_bus = i2c_bus,
		.address = address,
		.gain = gain,
		.name = name,
		.poll_state = ADS1000_ST_WRITE, /* mesmo default do dev_ads_t.stt original */
	};
	device_health_reset(&dev->health);

	k_work_init_delayable(&dev->poll_work, poll_work_handler);
	k_work_schedule_for_queue(sensor_workq_get(), &dev->poll_work, K_NO_WAIT);

	/* "Driver iniciado", NAO "chip encontrado": aqui so' se confirma que
	 * o barramento I2C deste SIM Connect esta' pronto - o driver ainda
	 * nem tentou falar com o endereco. Se nao houver ADS1000 fisico
	 * neste endereco, esta linha aparece do mesmo jeito, e quem revela
	 * isso e' o rastreio de saude (device_health.h) alguns ciclos
	 * depois - ver ads1000_is_online()/ads1000_failure_rate().
	 */
	LOG_INF("[%s] Driver ADS1000 @0x%02x iniciado (ganho %u) - tentando amostrar a cada %d ms "
		"(presenca do chip so' confirmada apos o 1o ciclo bem-sucedido)",
		name, address, gain, ADS1000_POLL_MS);
	return true;
}

bool ads1000_last_raw(struct ads1000_dev *dev, uint16_t *raw_out)
{
	*raw_out = dev->cached_raw;
	return dev->cached_valid && device_health_is_online(&dev->health);
}

bool ads1000_is_online(struct ads1000_dev *dev)
{
	return device_health_is_online(&dev->health);
}

uint16_t ads1000_failure_rate(struct ads1000_dev *dev)
{
	return device_health_failure_rate(&dev->health);
}

const struct device_health *ads1000_health(const struct ads1000_dev *dev)
{
	return &dev->health;
}

void ads1000_set_gain(struct ads1000_dev *dev, uint8_t gain)
{
	dev->gain = gain;
	/* Forca reescrita: a config antiga (e o echo esperado) mudou */
	dev->poll_state = ADS1000_ST_WRITE;
}
