/*
 * Configuracao herdada da FSA (tempo de reversao) - ver actuator_fsa_cfg.h.
 */

#include "actuator_fsa_cfg.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(actuator_fsa_cfg, LOG_LEVEL_INF);

/* Chave curta (<= 8 chars, caminho rapido do ZMS) */
#define FSA_CFG_KEY "afsa/v1"

/* u16 lido pelo laco de controle e escrito pela thread do BT - atomic
 * basta (um campo so').
 */
static atomic_t tempo_rever = ATOMIC_INIT(ACTUATOR_FSA_TEMPO_REVER_DEFAULT);
static struct k_work save_work;

static bool faixa_ok(uint16_t v)
{
	return v >= ACTUATOR_FSA_TEMPO_REVER_MIN && v <= ACTUATOR_FSA_TEMPO_REVER_MAX;
}

static void save_work_handler(struct k_work *work)
{
	uint16_t v = (uint16_t)atomic_get(&tempo_rever);
	int err = settings_save_one(FSA_CFG_KEY, &v, sizeof(v));

	if (err) {
		LOG_ERR("Falha ao gravar tempo de reversao (err %d)", err);
	} else {
		LOG_INF("Tempo de reversao gravado: %u ms", v);
	}
}

static int fsa_cfg_settings_set(const char *name, size_t len, settings_read_cb read_cb,
				void *cb_arg)
{
	uint16_t v;
	ssize_t got;

	if (!settings_name_steq(name, "v1", NULL)) {
		return -ENOENT;
	}
	if (len != sizeof(v)) {
		return 0;
	}

	got = read_cb(cb_arg, &v, sizeof(v));
	if (got < 0) {
		return (int)got;
	}
	if (!faixa_ok(v)) {
		LOG_WRN("Tempo de reversao gravado fora da faixa (%u) - mantendo %u ms", v,
			ACTUATOR_FSA_TEMPO_REVER_DEFAULT);
		return 0;
	}

	atomic_set(&tempo_rever, v);
	LOG_INF("Tempo de reversao restaurado: %u ms", v);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(actfsa, "afsa", NULL, fsa_cfg_settings_set, NULL, NULL);

void actuator_fsa_cfg_init(void)
{
	atomic_set(&tempo_rever, ACTUATOR_FSA_TEMPO_REVER_DEFAULT);
	k_work_init(&save_work, save_work_handler);
}

uint16_t actuator_fsa_cfg_tempo_reversao_ms(void)
{
	return (uint16_t)atomic_get(&tempo_rever);
}

bool actuator_fsa_cfg_read(uint16_t offset, uint8_t *buf, size_t len)
{
	uint8_t raw[ACTUATOR_FSA_TEMPO_REVER_LEN];

	if (offset >= sizeof(raw) || len > sizeof(raw) - offset) {
		return false;
	}
	sys_put_le16((uint16_t)atomic_get(&tempo_rever), raw);
	memcpy(buf, &raw[offset], len);
	return true;
}

bool actuator_fsa_cfg_write(uint16_t offset, const uint8_t *data, size_t len)
{
	uint8_t raw[ACTUATOR_FSA_TEMPO_REVER_LEN];
	uint16_t v;

	if (offset >= sizeof(raw) || len == 0 || len > sizeof(raw) - offset) {
		return false;
	}

	sys_put_le16((uint16_t)atomic_get(&tempo_rever), raw);
	memcpy(&raw[offset], data, len);
	v = sys_get_le16(raw);

	if (!faixa_ok(v)) {
		LOG_WRN("Tempo de reversao %u ms fora da faixa [%u, %u]", v,
			ACTUATOR_FSA_TEMPO_REVER_MIN, ACTUATOR_FSA_TEMPO_REVER_MAX);
		return false;
	}

	atomic_set(&tempo_rever, v);
	k_work_submit(&save_work);
	return true;
}
