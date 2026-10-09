/*
 * Sensor de torque ON/OFF (PCA9536, I2C) - ver torque_onoff_sensor.h.
 *
 * CHIP REAL (confirmado com o Felipe, 2026-08-26): PCA9536DR (expansor
 * de I/O de 4 bits, SEM pinos de endereco - endereco fixo 0x41, que
 * bate exatamente com ENDER_SENSOR_TORQUE_ONOFF (0x82) do proCo
 * original >> 1. P0 e P1 ligados com pull-up as microchaves de
 * sobretorque de fechamento e abertura respectivamente - contato
 * fechado (curto pra GND) = nivel baixo; contato aberto = nivel alto
 * (pull-up). CORRECAO (2026-08-26, Felipe): as microchaves sao do tipo
 * Normalmente Fechado (NF/NC) - em repouso (sem sobretorque) o contato
 * esta' FECHADO (nivel baixo); a ativacao por sobretorque ABRE o
 * contato (nivel alto). Ou seja, "ha sobretorque" = nivel ALTO, o
 * mesmo sentido que o proCo original ja usava (`if (adTorque &
 * TORQUE_ABERTURA_BIT)`, bit=1 = ha sobretorque) - sem nenhuma inversao
 * de polaridade na leitura (a versao anterior deste arquivo invertia
 * por engano, assumindo contato Normalmente Aberto).
 *
 * PROTOCOLO - DIFERENTE do ADS1000 de proposito: o PCA9536 E'
 * registrador-enderecado (ao contrario do ADS1000, que nao tem esse
 * conceito - ver a nota grande em ads1000.c). O driver original
 * (dev_ch_accept_rx em dev_ads.c) reaproveita as mesmas
 * dev_ads_get_tx()/get_rx() do ADS1000 por conveniencia de codigo
 * (mesmo motor ComScan) - o que escreve um byte de config (0x80) que
 * NAO corresponde a nenhum registrador real do PCA9536 (registradores
 * validos: 0x00-0x03). Funciona na pratica provavelmente por
 * coincidencia (mascaramento de bits no chip, ou o ponteiro de comando
 * ja estar em 0x00/Input Port desde o power-on e nunca ter sido
 * mudado de fato). Em vez de replicar esse byte sem entender por que
 * funciona, este modulo fala o protocolo real do PCA9536 direto do
 * datasheet:
 *
 *   - Escreve reg CONFIG (0x03) = 0x0F uma vez no boot (garante P0-P3
 *     como entrada, sem depender do default de fabrica do chip).
 *   - Le repetidamente: escreve o ponteiro 0x00 (Input Port) + le 1
 *     byte (i2c_write_read - unico lugar neste projeto onde essa
 *     combinacao e' de fato o protocolo certo; ver a ressalva em
 *     ads1000.c sobre o ADS1000 NAO ser assim).
 *
 * Sem "echo" pra auto-recuperacao (o PCA9536 nao tem esse mecanismo,
 * ao contrario do ADS1000) - se a leitura comecar a falhar
 * persistentemente (rastreio de saude fica offline), reconfigura por
 * garantia antes de confiar na proxima leitura.
 */

#include "torque_onoff_sensor.h"
#include "device_health.h"
#include "sensor_workq.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(torque_onoff_sensor, LOG_LEVEL_INF);

#define I2C_NODE DT_NODELABEL(i2c21)

/* ENDER_SENSOR_TORQUE_ONOFF (0x82) no proCo original >> 1 - e' tambem
 * o endereco fixo de fabrica do PCA9536 (sem pinos de endereco).
 */
#define PCA9536_ADDR 0x41

#define PCA9536_REG_INPUT  0x00
#define PCA9536_REG_CONFIG 0x03

/* Todos os 4 pinos como entrada - so' P0/P1 sao usados (fechamento/
 * abertura), P2/P3 nao usados mas custam nada deixar como entrada tambem.
 */
#define PCA9536_CONFIG_ALL_INPUT 0x0F

#define BIT_FECHAMENTO BIT(0) /* P0 */
#define BIT_ABERTURA   BIT(1) /* P1 */

/* Mesmo TEMPO_CICLO_MS do resto do projeto (ads1000.c) */
#define POLL_MS 15

static const struct device *i2c_bus = DEVICE_DT_GET(I2C_NODE);

static struct k_work_delayable poll_work;
static struct device_health health;
static bool configured;
static bool cached_abertura;
static bool cached_fechamento;
static bool warned_never_online; /* ver ads1000.c - mesma logica de aviso "nunca respondeu" */

static void poll_work_handler(struct k_work *work)
{
	bool was_online = device_health_is_online(&health);
	int err;

	if (!configured) {
		uint8_t cmd[2] = { PCA9536_REG_CONFIG, PCA9536_CONFIG_ALL_INPUT };

		err = i2c_write(i2c_bus, cmd, sizeof(cmd), PCA9536_ADDR);
		if (err) {
			device_health_record_failure(&health);
			/* Mesmo motivo do aviso em ads1000.c: sem isto, "nunca
			 * conectado" e "conectado mas sem responder" ficam
			 * identicos no log (silencio nos dois casos).
			 */
			if (!health.ever_succeeded &&
			    device_health_count_failure_sequent(&health) >=
				    DEVICE_HEALTH_MAX_FAILURE_SEQUENT &&
			    !warned_never_online) {
				LOG_WRN("Nenhuma resposta do PCA9536 @0x%02x ainda (i2c_write "
					"retornou %d) - fisicamente ausente, endereco/fiacao "
					"errados, ou alimentacao com problema?",
					PCA9536_ADDR, err);
				warned_never_online = true;
			}
		} else {
			configured = true;
			LOG_INF("PCA9536 configurado (P0-P3 como entrada)");
		}
	} else {
		uint8_t pointer = PCA9536_REG_INPUT;
		uint8_t rx;

		err = i2c_write_read(i2c_bus, PCA9536_ADDR, &pointer, sizeof(pointer), &rx,
				      sizeof(rx));
		if (err) {
			device_health_record_failure(&health);
			if (!device_health_is_online(&health)) {
				/* Ficou offline - reconfigura por garantia antes
				 * de voltar a confiar em leituras (sem echo pra
				 * detectar "perdeu a config" mais cedo, como no
				 * ADS1000 - so' esse sinal indireto mesmo).
				 */
				configured = false;
			}
			if (!health.ever_succeeded &&
			    device_health_count_failure_sequent(&health) >=
				    DEVICE_HEALTH_MAX_FAILURE_SEQUENT &&
			    !warned_never_online) {
				LOG_WRN("Config aceita, mas leitura do PCA9536 nunca "
					"respondeu (i2c_write_read retornou %d) - endereco "
					"0x%02x", err, PCA9536_ADDR);
				warned_never_online = true;
			}
		} else {
			device_health_record_success(&health);
			warned_never_online = false; /* pode avisar de novo se cair depois */
			/* Microchaves Normalmente Fechado: nivel ALTO = contato
			 * aberto pela ativacao = ha sobretorque (ver nota grande
			 * no topo do arquivo) - sem inversao.
			 */
			cached_fechamento = (rx & BIT_FECHAMENTO) != 0;
			cached_abertura = (rx & BIT_ABERTURA) != 0;
		}
	}

	bool now_online = device_health_is_online(&health);

	if (was_online && !now_online) {
		LOG_WRN("Sensor de torque ON/OFF ficou offline");
	} else if (!was_online && now_online) {
		LOG_INF("Sensor de torque ON/OFF ficou online");
	}


	k_work_reschedule_for_queue(sensor_workq_get(), k_work_delayable_from_work(work),
				     K_MSEC(POLL_MS));
}

bool torque_onoff_sensor_init(void)
{
	if (!device_is_ready(i2c_bus)) {
		LOG_ERR("Barramento I2C (i2c21) nao esta pronto - sensor de "
			"torque ON/OFF indisponivel");
		return false;
	}

	device_health_reset(&health);
	configured = false;
	cached_abertura = false;
	cached_fechamento = false;
	warned_never_online = false;

	k_work_init_delayable(&poll_work, poll_work_handler);
	k_work_schedule_for_queue(sensor_workq_get(), &poll_work, K_NO_WAIT);

	/* "Driver iniciado", NAO "chip encontrado" - mesma ressalva de
	 * ads1000.c: o barramento i2c21 esta' pronto, mas o driver ainda
	 * nem tentou escrever o CONFIG no PCA9536. Se o chip nao estiver
	 * fisicamente presente, esta linha aparece do mesmo jeito - quem
	 * revela isso e' o rastreio de saude (torque_onoff_sensor_is_online())
	 * alguns ciclos depois.
	 */
	LOG_INF("Driver do torque ON/OFF (PCA9536 @0x%02x, i2c21) iniciado - tentando amostrar a "
		"cada %d ms (presenca do chip so' confirmada apos o 1o ciclo bem-sucedido)",
		PCA9536_ADDR, POLL_MS);
	return true;
}

bool torque_onoff_sensor_last(bool *abertura, bool *fechamento)
{
	*abertura = cached_abertura;
	*fechamento = cached_fechamento;
	return device_health_is_online(&health);
}

bool torque_onoff_sensor_is_online(void)
{
	return device_health_is_online(&health);
}

const struct device_health *torque_onoff_sensor_health(void)
{
	return &health;
}
