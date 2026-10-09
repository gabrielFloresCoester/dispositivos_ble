/*
 * Camada de calibracao raw->fisico pros sensores de posicao/torque -
 * equivalente a BLE/Atuador/sensPosTor.c (ControleCoesterBLE), MAS com
 * escopo deliberadamente estreito: so' a conta de conversao + a
 * filtragem (media aparada de 10 amostras, mesma logica de sptPegAd()
 * - ver sample_filter.c). NAO inclui alarmes (at_alarm_*) nem
 * acoplamento com controle de movimento (at_ctl_get_mov_stt) - ver
 * docs/SENSORES_I2C.md, secao "Escopo explicitamente fora". Aquilo e'
 * a logica de controle do atuador inteira; isto aqui e' so' "qual e' o
 * valor fisico atual".
 *
 * PARAMETROS: limiteSuper/limiteInfer/torqueZero/fabFatTorqAber/
 * fabFatTorqFech/fabLeitPosiInver/antiHorario vem do paramDado
 * (actuator_params.h, area Painel 0x00800800 - configuravel pela
 * interface). Os limites de torque (torqueNmInc/torqueNmDec) nao sao
 * usados aqui - a protecao por sobretorque fica em actuator_torque.c, que
 * le o nm_torque calculado por este modulo.
 *
 * Fator de torque como em sptTratTrq(): fabFatTorqAber enquanto abre,
 * fabFatTorqFech enquanto fecha, e parado mantem o ultimo usado
 * (ultFator no original, comeca em fabFatTorqAber).
 *
 * Inversao de leitura de posicao como em sptTratInvertAd(): o AD
 * filtrado e' complementado (RESOL_AD_POS - ad) se fabLeitPosiInver, e
 * de novo se antiHorario - os dois juntos se anulam.
 *
 * SELECAO AUTOMATICA DE FONTE DE TORQUE (2026-08-26): o sensor de
 * torque tem 2 variantes de hardware mutuamente exclusivas por BOM
 * (analogico ADS1000, torque_sensor.h; digital ON/OFF PCA9536,
 * torque_onoff_sensor.h - ver docs/SENSORES_I2C.md). Em vez de
 * escolher em tempo de compilacao (como o proCo original faz via
 * #ifdef PAINEL_CQT, exigindo 4 firmwares diferentes pras 4
 * combinacoes de posicao x torque), este modulo escolhe sozinho em
 * runtime, a cada ciclo, com base em qual das duas esta' "online"
 * (device_health.h, via torque_sensor_is_online()/
 * torque_onoff_sensor_is_online()) - um unico firmware serve as duas
 * variantes fisicas. Analogico tem prioridade se os dois estiverem
 * online ao mesmo tempo (cenario so' de bancada - no produto real
 * nunca estao os dois presentes juntos).
 *
 * Quando a fonte ativa e' o ON/OFF, ad_torque/nm_torque NAO carregam
 * Nm de verdade - carregam os 2 bits crus de sobretorque (mesmo
 * "reaproveitamento de campo" que o proCo original faz nesta variante,
 * sensPosTor.c ramo PAINEL_CQT: `return posTor.adTorque;` vira
 * posTor.nmTorque). Mantem o mesmo formato de 10 bytes/BLE sem
 * precisar de um campo novo.
 */

#include "actuator_sensors.h"
#include "position_sensor.h"
#include "torque_sensor.h"
#include "torque_onoff_sensor.h"
#include "sample_filter.h"
#include "actuator_params.h"
#include "actuator_control.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(actuator_sensors, LOG_LEVEL_INF);

/* Mesmo TEMPO_CICLO_MS do ComScan legado, ja usado em ads1000.c - aqui
 * so' agrega o que ja foi lido (position_sensor_last_raw()/
 * torque_sensor_last_raw() sao cache, nao acessam o I2C).
 */
#define ACTUATOR_SENSORS_POLL_MS 15

/* RESUL_FAB_FATOR_TORQUE no sensPosTor.c original */
#define TORQUE_FATOR_DIV 1000

/* RESOL_AD_POS (sensPosTor.h) - complemento do AD em sptTratInvertAd() */
#define RESOL_AD_POS 0x07FF

/* Convencao de bit do proCo original (TORQUE_ABERTURA_BIT/
 * TORQUE_FECHAMENTO_BIT em sensPosTor.c, bit=1 = ha sobretorque) -
 * usada so' na hora de serializar os 2 bits em ad_torque/nm_torque.
 * Mapeamento direto a partir da API publica de torque_onoff_sensor.h
 * (tambem true = ha sobretorque, ja' resolvida a polaridade eletrica
 * das microchaves la' dentro - ver nota grande em
 * torque_onoff_sensor.c) - nao ha inversao aqui.
 */
#define TORQUE_ONOFF_WIRE_BIT_FECHAMENTO 0x01
#define TORQUE_ONOFF_WIRE_BIT_ABERTURA   0x02

static struct k_work_delayable poll_work;
static struct sample_filter position_filter;
static struct sample_filter torque_filter;
static struct actuator_sensor_data cached;
static bool cached_valid;
static enum actuator_torque_source active_torque_source = ACTUATOR_TORQUE_SOURCE_NONE;

static const char *torque_source_name(enum actuator_torque_source src)
{
	switch (src) {
	case ACTUATOR_TORQUE_SOURCE_ANALOG:
		return "analogico (ADS1000)";
	case ACTUATOR_TORQUE_SOURCE_ONOFF:
		return "ON/OFF (PCA9536)";
	default:
		return "nenhuma (nenhum sensor de torque online)";
	}
}

/* Equivalente a sptPosMil() (sensPosTor.c original) */
static int16_t position_to_milesimos(int32_t ad, uint16_t limite_super, uint16_t limite_infer)
{
	float fator;
	float mil;

	/* Os dois limites tem a mesma faixa [50, 2001] e podem ficar iguais
	 * por configuracao - sem posicao calculavel, nao divide por zero.
	 */
	if (limite_super == limite_infer) {
		return ACTUATOR_AT_POSICAO_INDEF;
	}

	fator = 1000.0f / ((float)limite_super - (float)limite_infer);
	mil = ((float)ad - limite_infer) * fator;

	return (int16_t)mil;
}

/* Equivalente a sptPegAdTrqZer() + o calculo de trqNm dentro de
 * sptTratTrq() (ramo unico - ver nota grande no topo do arquivo sobre
 * fabFatTorqAber == fabFatTorqFech por padrao). So' usado quando a
 * fonte ativa e' o analogico.
 */
static int16_t torque_to_nm(int32_t ad_torque, int16_t torque_zero, uint16_t fator)
{
	int32_t zerado = ad_torque - torque_zero;

	if (zerado < 0) {
		zerado = 0;
	}

	return (int16_t)((zerado * fator) / TORQUE_FATOR_DIV);
}

/* ultFator do sptTratTrq() original - ver nota no topo */
static uint16_t fator_torque_atual(void)
{
	static bool fechando;

	switch (actuator_control_get_mov_stt()) {
	case ACTUATOR_MOV_INCR:
		fechando = false;
		break;
	case ACTUATOR_MOV_DECR:
		fechando = true;
		break;
	default:
		break;
	}

	return fechando ? actuator_params_fab_fat_torq_fech() : actuator_params_fab_fat_torq_aber();
}

/* sptTratInvertAd() original */
static int32_t trata_invert_ad(int32_t ad)
{
	if (actuator_params_fab_leit_pos_inv()) {
		ad = RESOL_AD_POS - ad;
	}
	if (actuator_params_anti_horario()) {
		ad = RESOL_AD_POS - ad;
	}
	return ad;
}

static void poll_work_handler(struct k_work *work)
{
	uint16_t pos_raw;
	int32_t pos_filtered;
	bool position_ready;
	bool torque_ready = false;
	enum actuator_torque_source source;

	/* BUG CORRIGIDO (2026-08-26): sample_filter_avg() so' confere se o
	 * buffer ja encheu ALGUMA VEZ - uma vez cheio, continua reportando
	 * "pronto" pra sempre calculando a media das MESMAS amostras
	 * antigas, mesmo que o sensor va' offline e nunca mais empurre uma
	 * amostra nova (sample_filter_push so' acontece quando
	 * position_sensor_last_raw() retorna true, que ja' exige estar
	 * online). Sem o "&& position_sensor_is_online()" abaixo, o
	 * "INDEF" nunca aparecia (o valor calibrado ficava preso no ultimo
	 * bom) e desconectar o sensor um tempo, religar, e ver a media
	 * ainda arrastando amostras de horas atras' parecia "atraso" -
	 * reportado pelo Felipe testando o proprio INDEF. sample_filter_
	 * reset() zera o buffer assim que fica offline, forcando reencher
	 * do zero (150ms) com amostras genuinamente novas quando voltar.
	 */
	bool position_online = position_sensor_is_online();

	if (position_sensor_last_raw(&pos_raw)) {
		sample_filter_push(&position_filter, pos_raw);
	} else if (!position_online) {
		sample_filter_reset(&position_filter);
	}
	position_ready = position_online && sample_filter_avg(&position_filter, &pos_filtered);

	if (torque_sensor_is_online()) {
		source = ACTUATOR_TORQUE_SOURCE_ANALOG;
	} else if (torque_onoff_sensor_is_online()) {
		source = ACTUATOR_TORQUE_SOURCE_ONOFF;
	} else {
		source = ACTUATOR_TORQUE_SOURCE_NONE;
	}

	if (source != active_torque_source) {
		LOG_INF("Fonte de torque: %s", torque_source_name(source));
		active_torque_source = source;
	}

	/* Mesmo motivo do reset de position_filter acima - garante que o
	 * filtro de torque analogico comeca vazio sempre que essa fonte nao
	 * estiver ativa (offline, ou ON/OFF tomou a vez), em vez de
	 * arrastar amostras antigas se o analogico voltar a ser a fonte
	 * mais tarde.
	 */
	if (source != ACTUATOR_TORQUE_SOURCE_ANALOG) {
		sample_filter_reset(&torque_filter);
	}

	switch (source) {
	case ACTUATOR_TORQUE_SOURCE_ANALOG: {
		uint16_t trq_raw;
		int32_t trq_filtered;

		if (torque_sensor_last_raw(&trq_raw)) {
			sample_filter_push(&torque_filter, trq_raw);
		}
		if (sample_filter_avg(&torque_filter, &trq_filtered)) {
			cached.ad_torque = (int16_t)trq_filtered;
			cached.nm_torque = torque_to_nm(trq_filtered,
							actuator_params_torque_zero(),
							fator_torque_atual());
			torque_ready = true;
		}
		break;
	}
	case ACTUATOR_TORQUE_SOURCE_ONOFF: {
		bool abertura, fechamento;

		if (torque_onoff_sensor_last(&abertura, &fechamento)) {
			uint8_t raw2bit = (fechamento ? TORQUE_ONOFF_WIRE_BIT_FECHAMENTO : 0) |
					   (abertura ? TORQUE_ONOFF_WIRE_BIT_ABERTURA : 0);

			cached.ad_torque = raw2bit;
			cached.nm_torque = raw2bit;
			torque_ready = true;
		}
		break;
	}
	default:
		break;
	}

	/* Posicao e torque agora sao servidos de forma INDEPENDENTE (bug
	 * corrigido 2026-08-26, reportado pelo Felipe: a interface so'
	 * atualizava a posicao quando o torque tambem estava valido, porque
	 * a versao anterior deste 'if' exigia os dois juntos - diferente de
	 * um Atuador BLE real, onde os dois sensores sempre existem e essa
	 * dependencia nunca aparece). O frame passa a ser servido assim que
	 * QUALQUER um dos dois tiver uma leitura pra mostrar; o lado que
	 * ainda nao tem leitura usa um sentinela em vez de ficar com o
	 * ultimo valor (ou zero) parecendo dado fresco:
	 *   - posicao: ACTUATOR_AT_POSICAO_INDEF em at_posicao.
	 *   - torque: nao precisa de sentinela - a area "Info" (ver
	 *     actuator_sensors_torque_source()) ja' distingue "sem fonte de
	 *     torque" de "torque = 0" pro lado de fora; ad_torque/nm_torque
	 *     simplesmente mantem o ultimo valor conhecido (ou 0, no boot).
	 */
	if (position_ready) {
		pos_filtered = trata_invert_ad(pos_filtered);
		cached.ad_posicao = (uint16_t)pos_filtered;
		cached.at_posicao = position_to_milesimos(pos_filtered,
							  actuator_params_limite_super(),
							  actuator_params_limite_infer());
	} else {
		cached.at_posicao = ACTUATOR_AT_POSICAO_INDEF;
	}

	if (torque_ready) {
		/* Torque AD maximo do movimento, como sptTratTrq(): zera
		 * (= valor atual) no inicio de cada movimento (abrindo ou
		 * fechando) e acompanha o maior valor depois disso. Pra ON/OFF
		 * sao os 2 bits crus - menos util, mas inofensivo.
		 */
		static enum actuator_mov_status ult_status;
		enum actuator_mov_status status = actuator_control_get_mov_stt();
		bool partiu = status != ult_status &&
			      (status == ACTUATOR_MOV_INCR || status == ACTUATOR_MOV_DECR);

		if (cached.ad_torque > cached.ad_torque_max || partiu) {
			cached.ad_torque_max = cached.ad_torque;
		}
		ult_status = status;
	}

	if (position_ready || torque_ready) {
		cached_valid = true;
	}

	k_work_reschedule(k_work_delayable_from_work(work), K_MSEC(ACTUATOR_SENSORS_POLL_MS));
}

void actuator_sensors_init(void)
{
	memset(&cached, 0, sizeof(cached));
	cached_valid = false;
	active_torque_source = ACTUATOR_TORQUE_SOURCE_NONE;

	k_work_init_delayable(&poll_work, poll_work_handler);
	k_work_schedule(&poll_work, K_NO_WAIT);

	LOG_INF("Calibracao posicao/torque iniciada (limites, zero, fator de torque e "
		"inversao vem do paramDado - actuator_params.c; fonte de torque escolhida "
		"automaticamente entre analogico/ON-OFF)");
}

bool actuator_sensors_get(struct actuator_sensor_data *out)
{
	*out = cached;
	return cached_valid;
}

enum actuator_torque_source actuator_sensors_torque_source(void)
{
	return active_torque_source;
}
