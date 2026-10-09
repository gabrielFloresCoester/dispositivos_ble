/*
 * Motor generico de leitura do ADS1000 (I2C), parametrizado por
 * endereco e ganho - usado por position_sensor.c e torque_sensor.c
 * (mesmo chip do proCo original, so' endereco e ganho mudam entre os
 * dois - ver dev_ads.c/devices.c no ControleCoesterBLE).
 *
 * Cada "instancia" e' um struct ads1000_dev, dono do seu proprio work
 * item periodico e cache - nao ha estado global compartilhado entre
 * instancias (posicao e torque rodam cada um o seu, de forma
 * independente; o driver I2C do Zephyr ja serializa o acesso fisico
 * ao barramento entre eles com seguranca, sem precisar de revezamento
 * manual tipo o execDevList do ComScan original).
 */
#ifndef ADS1000_H_
#define ADS1000_H_

#include "device_health.h"

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <stdint.h>
#include <stdbool.h>

enum ads1000_poll_state {
	ADS1000_ST_WRITE, /* precisa (re)escrever a config antes de confiar numa leitura */
	ADS1000_ST_READ,  /* config assumida valida - so' ler e conferir o echo */
};

struct ads1000_dev {
	const struct device *i2c_bus;
	uint8_t address; /* 7 bits */
	uint8_t gain;    /* 0-3, ver paramDado.fabGanSenTorq no proCo original */
	const char *name; /* so' para log, ex. "posicao", "torque" */

	/* Estado interno - nao mexer de fora, so' pelas funcoes abaixo */
	struct k_work_delayable poll_work;
	uint16_t cached_raw;
	bool cached_valid;
	enum ads1000_poll_state poll_state;
	struct device_health health;
	bool logged_first_config;
	bool warned_never_online; /* ver nota em ads1000.c sobre "nunca respondeu" */
};

/* Inicia uma instancia: confere o barramento I2C e agenda o motor de
 * amostragem periodica (15ms, mesmo TEMPO_CICLO_MS do ComScan
 * legado). Retorna false sem travar o boot se o barramento nao
 * estiver pronto - mesmo padrao de uart_link_init().
 */
bool ads1000_init(struct ads1000_dev *dev, const struct device *i2c_bus, uint8_t address,
		   uint8_t gain, const char *name);

/* Ultimo valor lido com sucesso E' com echo de config validado (cache,
 * nao acessa o I2C). Ver position_sensor_last_raw() para a semantica
 * exata do retorno.
 */
bool ads1000_last_raw(struct ads1000_dev *dev, uint16_t *raw_out);

/* Rastreio de saude (device_health.h) - "online" aqui significa "ja
 * teve pelo menos uma leitura com echo de config valido, e nao esta
 * numa sequencia de falhas consecutivas" - ver ads1000_last_raw()
 * acima, que ja usa isso internamente; exposto tambem separado pra
 * quem precisar da razao (offline vs "so' nao leu ainda") ou da taxa
 * de falha, ex. deteccao automatica de variante de sensor.
 */
bool ads1000_is_online(struct ads1000_dev *dev);
uint16_t ads1000_failure_rate(struct ads1000_dev *dev);

/* Acesso direto ao rastreio de saude (device_health.h) - pra diagnostico
 * (ex.: device_health_count_transaction(), pra confirmar que o poller
 * esta' de fato rodando).
 */
const struct device_health *ads1000_health(const struct ads1000_dev *dev);

/* Troca o ganho em runtime (0-3) - forca reescrita de config no
 * proximo ciclo. Pensado para quando existir um subsistema de
 * calibracao/parametros de verdade (equivalente a paramDado); por
 * enquanto quem chama e' so' o valor default fixo de cada sensor.
 */
void ads1000_set_gain(struct ads1000_dev *dev, uint8_t gain);

#endif /* ADS1000_H_ */
