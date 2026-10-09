/*
 * Protecao por torque - ver actuator_torque.h. Porta de sptTratTrq() +
 * sptTrqParaLimit() (sensPosTor.c), nos dois ramos do original:
 *   - celula de carga (analogico, ADS1000): torque em Nm contra os
 *     limites, margem de partida, assentamento por Nm;
 *   - microchaves (ON/OFF, PCA9536 - ramo #ifdef PAINEL_CQT).
 * A fonte e' a que actuator_sensors.c escolheu automaticamente no ciclo.
 *
 * DIVERGENCIA DELIBERADA (assentamento com microchaves): no fwBLE, com
 * trqFechad e o atuador no limite fechado, sptTrqParaLimit() deixa o motor
 * fechando ate' timerFechanTorq expirar (1,5 s) comparando
 * nmTorque >= torqueNmDec - mas com microchaves nmTorque sao os 2 bits
 * crus (<= 3), entao a comparacao nunca fecha e o motor passa do limite
 * por 1,5 s IGNORANDO a microchave. La' isso era tolerado (a placa FSA /
 * a microchave cortam no hardware); aqui o motor e' GPIO direto, sem nada
 * que corte. Entao: continua fechando ate' 1,5 s OU ate' a microchave de
 * fechamento atuar (com o mesmo filtro de 3 ciclos), o que vier primeiro.
 * Com celula de carga a comparacao em Nm funciona e e' portada como esta'.
 */

#include "actuator_torque.h"
#include "actuator_sensors.h"
#include "actuator_params.h"
#include "actuator_control.h"
#include "actuator_mode.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(actuator_torque, LOG_LEVEL_INF);

/* sensPosTor.c */
#define TEMPO_FECHAN_TRQ_MS     1500 /* janela de assentamento no limite fechado */
#define TEMPO_CH_OFF_MS         3000 /* timerCH1Off/timerCH2Off: "atuou ha' pouco" */
#define CICLOS_FILTRO           3    /* chXCount > 3 */
#define CICLOS_TRQ_FECHAD       150  /* + trqFechad * 150 (~1,5 s a 10 ms) */
#define TEMPO_SOBR_TORQ_PART_MS 3000 /* janela da margem de partida (sobrTorqPart) */
#define TEMPO_IGNOR_TORQ_MS     250  /* inicio da partida em que o torque e' ignorado */
#define RESUL_SOBR_TORQ_PART    100  /* sobrTorqPart em % */
#define FAIXA_POSICAO_TRAVADO   15   /* por mil, em torno da ultima posicao parada */

/* Bits de ad_torque com a fonte ON/OFF (actuator_sensors.c, mesma
 * convencao do proCo: TORQUE_FECHAMENTO_BIT=0x01, TORQUE_ABERTURA_BIT=0x02).
 */
#define TORQUE_FECHAMENTO_BIT 0x01
#define TORQUE_ABERTURA_BIT   0x02
#define TORQUE_AB_FE_BIT      (TORQUE_FECHAMENTO_BIT | TORQUE_ABERTURA_BIT)

/* TEMPO_FALHA_SENSOR (sensPosTor.c) - tempo sem sensor de torque ate'
 * AL_COM_TRQ (ramo analogico do original; o ramo ON/OFF confia no
 * failureRate do comScan - aqui o device_health ja' filtra o "online").
 */
#define TEMPO_FALHA_SENSOR_MS 3000

/* --- Estado das microchaves --- */
static uint16_t ch1_count; /* CH1 = torque de abertura */
static uint16_t ch2_count; /* CH2 = torque de fechamento */
static uint16_t v_trav_count;
static int64_t timer_ch1_off;   /* deadlines (k_uptime_get) */
static int64_t timer_ch2_off;

/* --- Estado da celula de carga --- */
static int64_t timer_sobr_torq_part; /* timeSobrTorqPart: recarregado parado */
static int16_t posicao_ult_parado;
static bool posicao_ult_parado_ok;

/* --- Comum --- */
static int64_t timer_fechan_trq;
static int64_t ultimo_torque_online; /* k_uptime_get() da ultima vez com fonte online */

static inline bool timer_ativo(int64_t deadline)
{
	return k_uptime_get() < deadline;
}

static inline int64_t timer_restante(int64_t deadline)
{
	int64_t r = deadline - k_uptime_get();

	return r > 0 ? r : 0;
}

/* Bits atuais das microchaves. false = fonte de torque nao e' ON/OFF
 * (ou ainda sem leitura).
 */
static bool bits_onoff(uint8_t *bits)
{
	struct actuator_sensor_data d;

	if (actuator_sensors_torque_source() != ACTUATOR_TORQUE_SOURCE_ONOFF ||
	    !actuator_sensors_get(&d)) {
		return false;
	}
	*bits = (uint8_t)d.ad_torque & TORQUE_AB_FE_BIT;
	return true;
}

/* Torque em Nm e posicao atuais com celula de carga. false = fonte nao e'
 * analogica (ou ainda sem leitura).
 */
static bool leitura_analogica(int16_t *nm, int16_t *pos)
{
	struct actuator_sensor_data d;

	if (actuator_sensors_torque_source() != ACTUATOR_TORQUE_SOURCE_ANALOG ||
	    !actuator_sensors_get(&d)) {
		return false;
	}
	*nm = d.nm_torque;
	*pos = d.at_posicao;
	return true;
}

static void oper_incompleta_se_remoto(void)
{
	/* "Caso remoto e comando abortado" */
	if (actuator_mode_get() == ACTUATOR_MODE_REMOTO) {
		actuator_alarm_set(ACTUATOR_ALARM_OPER_INCOMPLETA);
	}
}

/* Parte comum aos dois ramos com o atuador parado (AS_M_PARA/L_SUPER/
 * L_INFER): libera OPER_INCOMPLETA e, com movimento manual (volante),
 * libera os bloqueios de torque e o travamento.
 */
static void parado_comum(void)
{
	actuator_alarm_release(ACTUATOR_ALARM_OPER_INCOMPLETA);

	if (actuator_alarm_get_info(ACTUATOR_ALARM_OPER_MANUAL)) {
		actuator_alarm_action_clear(ACTUATOR_ALARM_TORQUE_AB);
		actuator_alarm_action_clear(ACTUATOR_ALARM_TORQUE_FC);
		if (actuator_alarm_get_action(ACTUATOR_ALARM_VALV_TRAVADA)) {
			actuator_alarm_release(ACTUATOR_ALARM_VALV_TRAVADA);
		}
	}
}

void actuator_torque_check_com(void)
{
	int64_t agora = k_uptime_get();

	/* ultimo_torque_online comeca em 0 = o boot conta como "offline
	 * desde o instante 0": sem sensor nos primeiros 3 s, alarma.
	 */
	if (actuator_sensors_torque_source() != ACTUATOR_TORQUE_SOURCE_NONE) {
		ultimo_torque_online = agora;
		actuator_alarm_release(ACTUATOR_ALARM_COM_SENS_TRQ);
	} else if (agora - ultimo_torque_online >= TEMPO_FALHA_SENSOR_MS) {
		actuator_alarm_set(ACTUATOR_ALARM_COM_SENS_TRQ);
	}
}

/* --- Ramo microchaves (PAINEL_CQT) --- */
static void run_onoff(enum actuator_mov_status status, uint8_t bits)
{
	int64_t agora = k_uptime_get();

	if (bits & TORQUE_ABERTURA_BIT) {
		ch1_count++;
		timer_ch1_off = agora + TEMPO_CH_OFF_MS;
	} else {
		ch1_count = 0;
	}

	if (bits & TORQUE_FECHAMENTO_BIT) {
		ch2_count++;
		timer_ch2_off = agora + TEMPO_CH_OFF_MS;
	} else {
		ch2_count = 0;
	}

	if (bits == TORQUE_AB_FE_BIT) {
		v_trav_count++;
	} else {
		v_trav_count = 0;
	}

	switch (status) {
	case ACTUATOR_MOV_INCR:
		/* Desativa alarme de sobretorque oposto */
		actuator_alarm_release(ACTUATOR_ALARM_TORQUE_FC);

		/* So' monitora a abertura se "Desliga Torque de Abertura" estiver off */
		if (!actuator_params_desl_trq_incr()) {
			if (ch1_count > CICLOS_FILTRO) {
				oper_incompleta_se_remoto();
				actuator_alarm_set(ACTUATOR_ALARM_TORQUE_AB);
				/* CH2 atuou ha' pouco -> valvula travada */
				if (timer_ativo(timer_ch2_off)) {
					v_trav_count = CICLOS_FILTRO + 1;
				}
			} else {
				actuator_alarm_release(ACTUATOR_ALARM_TORQUE_AB);
			}
		}
		break;

	case ACTUATOR_MOV_DECR:
		actuator_alarm_release(ACTUATOR_ALARM_TORQUE_AB);

		/* Com trqFechad, exige ~1,5 s de contato antes de alarmar */
		if (ch2_count > CICLOS_FILTRO + (actuator_params_trq_fechad() ? CICLOS_TRQ_FECHAD : 0)) {
			oper_incompleta_se_remoto();
			actuator_alarm_set(ACTUATOR_ALARM_TORQUE_FC);
			if (timer_ativo(timer_ch1_off)) {
				v_trav_count = CICLOS_FILTRO + 1;
			}
			/* Desarmou porque chegou no torque de fechamento */
			if (actuator_params_trq_fechad()) {
				actuator_control_stop(ACTUATOR_CTL_ORIG_TORQUE);
			}
		} else {
			actuator_alarm_release(ACTUATOR_ALARM_TORQUE_FC);
			timer_fechan_trq = agora + TEMPO_FECHAN_TRQ_MS;
		}
		break;

	case ACTUATOR_MOV_PARADO:
	case ACTUATOR_MOV_L_SUPER:
	case ACTUATOR_MOV_L_INFER:
	default:
		parado_comum();
		actuator_alarm_release(ACTUATOR_ALARM_TORQUE_AB);
		actuator_alarm_release(ACTUATOR_ALARM_TORQUE_FC);
		break;
	}

	/* Torque nos dois sentidos por mais de 3 ciclos (ou um logo apos o
	 * outro, ver acima) -> valvula travada.
	 */
	if (v_trav_count > CICLOS_FILTRO) {
		actuator_alarm_set(ACTUATOR_ALARM_VALV_TRAVADA);
	} else {
		actuator_alarm_release(ACTUATOR_ALARM_VALV_TRAVADA);
	}
}

/* --- Ramo celula de carga (analogico) ---
 *
 * Limite do sentido + margem de partida, igual ao original:
 *   - nos primeiros TEMPO_IGNOR_TORQ_MS da partida o torque e' ignorado
 *     (GetTime(timeSobrTorqPart) >= TEMPO_SOBR_TORQ_PART - TEMPO_IGNOR_TORQ);
 *   - ate' TEMPO_SOBR_TORQ_PART_MS o limite vale limite * (1 + sobrTorqPart%);
 *   - depois, o limite puro.
 * Retorna o limite efetivo, ou 0 se ainda na janela ignorada.
 */
static uint32_t limite_com_partida(uint16_t limite_nm)
{
	int64_t restante = timer_restante(timer_sobr_torq_part);
	uint32_t lim = limite_nm;

	if (restante >= TEMPO_SOBR_TORQ_PART_MS - TEMPO_IGNOR_TORQ_MS) {
		return 0;
	}
	if (restante > 0) {
		lim += (lim * actuator_params_sobr_torq_part()) / RESUL_SOBR_TORQ_PART;
	}
	return lim;
}

/* Sobretorque num sentido com o alarme do outro sentido ainda sinalizado,
 * aproximadamente na mesma posicao em que parou -> valvula travada.
 */
static void checa_travada(enum actuator_alarm_id oposto, int16_t pos)
{
	if (!actuator_alarm_get_info(oposto) || !posicao_ult_parado_ok ||
	    pos == ACTUATOR_AT_POSICAO_INDEF) {
		return;
	}
	if (pos < posicao_ult_parado + FAIXA_POSICAO_TRAVADO &&
	    pos > posicao_ult_parado - FAIXA_POSICAO_TRAVADO) {
		actuator_alarm_set(ACTUATOR_ALARM_VALV_TRAVADA);
	}
}

/* Log de diagnostico, uma vez por partida: se o torque do sentido esta'
 * sendo monitorado e com qual limite. Sem isto, "nao alarmou" nao diz se
 * foi parametro (deslTrqIncr, limite alto) ou leitura (Nm em 0 porque a
 * carga faz o AD descer abaixo do Torque Zero - ver actuator_sensors.c).
 */
static void loga_partida(enum actuator_mov_status status, int16_t nm)
{
	static enum actuator_mov_status ult_status;

	if (status == ult_status) {
		return;
	}
	ult_status = status;

	if (status == ACTUATOR_MOV_INCR) {
		if (actuator_params_desl_trq_incr()) {
			LOG_INF("Abrindo: torque de abertura NAO monitorado "
				"(\"Desliga Torque de Abertura\" ligado)");
		} else {
			LOG_INF("Abrindo: limite %u Nm (+%u%% nos primeiros 3 s), torque agora %d Nm",
				actuator_params_torque_nm_inc(), actuator_params_sobr_torq_part(),
				nm);
		}
	} else if (status == ACTUATOR_MOV_DECR) {
		LOG_INF("Fechando: limite %u Nm (+%u%% nos primeiros 3 s), torque agora %d Nm",
			actuator_params_torque_nm_dec(), actuator_params_sobr_torq_part(), nm);
	}
}

static void run_analog(enum actuator_mov_status status, int16_t nm, int16_t pos)
{
	uint32_t lim;

	loga_partida(status, nm);

	switch (status) {
	case ACTUATOR_MOV_INCR:
		actuator_alarm_release(ACTUATOR_ALARM_TORQUE_FC);

		if (!actuator_params_desl_trq_incr()) {
			lim = limite_com_partida(actuator_params_torque_nm_inc());
			if (lim > 0 && nm >= 0 && (uint32_t)nm >= lim) {
				oper_incompleta_se_remoto();
				actuator_alarm_set(ACTUATOR_ALARM_TORQUE_AB);
				checa_travada(ACTUATOR_ALARM_TORQUE_FC, pos);
			}
		}
		break;

	case ACTUATOR_MOV_DECR:
		actuator_alarm_release(ACTUATOR_ALARM_TORQUE_AB);

		/* Janela de assentamento recomeca enquanto fecha sem sobretorque
		 * (no original, ReloadTimer(timerFechanTorq) no ramo AS_DECR).
		 */
		timer_fechan_trq = k_uptime_get() + TEMPO_FECHAN_TRQ_MS;

		lim = limite_com_partida(actuator_params_torque_nm_dec());
		if (lim > 0 && nm >= 0 && (uint32_t)nm >= lim) {
			oper_incompleta_se_remoto();
			actuator_alarm_set(ACTUATOR_ALARM_TORQUE_FC);
			checa_travada(ACTUATOR_ALARM_TORQUE_AB, pos);
		}
		break;

	case ACTUATOR_MOV_PARADO:
	case ACTUATOR_MOV_L_SUPER:
	case ACTUATOR_MOV_L_INFER:
	default:
		/* Parado: a margem de partida recomeca do zero na proxima partida */
		timer_sobr_torq_part = k_uptime_get() + TEMPO_SOBR_TORQ_PART_MS;
		if (pos != ACTUATOR_AT_POSICAO_INDEF) {
			posicao_ult_parado = pos;
			posicao_ult_parado_ok = true;
		}
		parado_comum();
		break;
	}
}

void actuator_torque_run(enum actuator_mov_status status)
{
	uint8_t bits;
	int16_t nm, pos;

	if (bits_onoff(&bits)) {
		run_onoff(status, bits);
		return;
	}

	/* Fora do ramo ON/OFF: zera os filtros das microchaves pra nao
	 * arrastar contagem velha se essa fonte voltar.
	 */
	ch1_count = ch2_count = v_trav_count = 0;

	if (leitura_analogica(&nm, &pos)) {
		run_analog(status, nm, pos);
	}
	/* Sem fonte nenhuma: AL_COM_TRQ, em actuator_torque_check_com(). */
}

bool actuator_torque_para_limit(enum actuator_mov_status status)
{
	uint8_t bits;
	int16_t nm, pos;

	if (!actuator_params_trq_fechad() || status != ACTUATOR_MOV_L_INFER) {
		return true;
	}

	/* Janela de assentamento (timerFechanTorq) ja' acabou -> desliga */
	if (!timer_ativo(timer_fechan_trq)) {
		return true;
	}

	/* O controle ja' esta' em PARAR (chegou no limite) nos dois casos
	 * abaixo - so' libera o desligamento do motor. O original chama
	 * at_ctl_stop(ACP_TORQUE) aqui; um actuator_control_stop() deixaria
	 * uma parada pendente que cancelaria o proximo comando.
	 */
	if (bits_onoff(&bits)) {
		/* DIVERGENCIA (ver topo): a microchave de fechamento atuada (com
		 * filtro) encerra o assentamento antes do tempo.
		 */
		if (ch2_count > CICLOS_FILTRO) {
			LOG_INF("Assentamento por torque: microchave de fechamento atuou");
			return true;
		}
		return false;
	}

	if (leitura_analogica(&nm, &pos)) {
		/* Chegou no torque de fechamento */
		if (nm >= 0 && (uint16_t)nm >= actuator_params_torque_nm_dec()) {
			LOG_INF("Assentamento por torque: %d Nm >= torque de fechamento", nm);
			return true;
		}
		return false;
	}

	/* Sem sensor de torque: sem como medir o assentamento, desliga. */
	return true;
}
