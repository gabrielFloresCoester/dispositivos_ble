/*
 * Controle logico do atuador - porta de BLE/Atuador/atControle.c.
 * Ver actuator_control.h pro mapa de funcoes e os seams de passos
 * futuros. O algoritmo abaixo segue o original de perto; onde diverge,
 * ha um comentario "DIVERGENCIA" ou "STUB (passo N)".
 */

#include "actuator_control.h"
#include "actuator_alarm.h"
#include "actuator_motor.h"
#include "actuator_mode.h"
#include "actuator_sensors.h"
#include "actuator_params.h"
#include "actuator_torque.h"
#include "actuator_regevent.h"
#include "position_sensor.h"
#include "watchdog.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(actuator_control, LOG_LEVEL_INF);

/* --- Constantes (atControle.c) --- */
#define RESOLUCAO_ENTRADA   1000
#define LIMITE_SUPERIOR_MIL 1000
#define LIMITE_INFERIOR_MIL 0

/* Parametros do paramDado (actuator_params.h), lidos a cada uso - como
 * paramDado.* no original, uma alteracao pela interface vale no ciclo
 * seguinte:
 *   limiteMargem    (default 20) -> histerese da faixa de "no limite"
 *   faixaParado     (default 20) -> banda morta em torno da demanda
 *   anteciparParada (default 2)  -> quanto antes da posicao alvo comeca a parar
 */
#define LIMITE_MARGEM    actuator_params_limite_margem()
#define FAIXA_PARADA     actuator_params_faixa_parado()
#define ANTECIPAR_PARADA actuator_params_antecipar_parada()

#define TEMPO_MOV_MANUAL_MS    3000
#define TOLERANCIA_MOV_MANUAL  (RESOLUCAO_ENTRADA / 200) /* 0.5% = 5 por mil */
#define TOLER_MOV_INVERTIDO    50                        /* 5% */

/* Periodo do loop de controle. Original roda em trataAplic() (~5ms);
 * 10ms aqui e' suficiente (o cache de posicao de actuator_sensors.c
 * atualiza a cada 15ms de qualquer forma) e mais leve.
 */
#define CTL_RUN_PERIOD_MS 10

/* --- Estados internos (mesmos do atControle.c) --- */
enum ctl_std {
	CTL_STD_PARAR = 0,
	CTL_STD_INCREMENTAR,
	CTL_STD_DECREMENTAR,
};

enum mov_manual_st {
	EMM_MOV_MOTOR,
	EMM_MOV_INERCIA,
	EMM_PARADO,
	EMM_MOV_VOLANTE,
};

enum mov_invertido_st {
	EMI_DESACIONADO,
	EMI_PARADO,
	EMI_PARTIDA,
	EMI_MOVIMENTO,
};

struct posicionar {
	int16_t posicao_mil;
	bool parar;
	bool disparo;
};

/* --- Variaveis de estado (eram __no_init no original) --- */
static struct posicionar at_posicionar;
static enum actuator_mov_status at_status;
static enum ctl_std at_acao_estado;
static int16_t posicao_em_vista;
static int16_t at_posicao_parado;
static enum actuator_ctl_cmd_orig at_origem_cmd;
static enum actuator_ctl_cmd_orig at_origem_parada;

static enum mov_manual_st st_mov_manual;
static int16_t at_posicao_mov_manual;
static int64_t timer_mov_manual; /* deadline em ms (k_uptime_get) */

static enum mov_invertido_st st_mov_invertido;
static int16_t posic_mov_invertido;

/* TEMPO_FALHA_SENSOR (sensPosTor.c) - tolerancia do AL_COM_POS. Comeca em
 * 0 = o boot conta como "ultima resposta no instante 0" (mesmo do torque,
 * actuator_torque.c).
 */
#define TEMPO_FALHA_SENSOR_POS_MS 3000
static int64_t ultimo_pos_online;

static struct k_work_delayable run_work;

/* --- Helpers de timer (equivalente a GetTime/ReloadTimer) --- */
static inline void timer_reload(int64_t *deadline, int64_t ms)
{
	*deadline = k_uptime_get() + ms;
}

/* ms restantes, 0 se expirou - equivale a GetTime() */
static inline int64_t timer_remaining(int64_t deadline)
{
	int64_t left = deadline - k_uptime_get();

	return left > 0 ? left : 0;
}

/* --- Leitura de posicao ---
 * sptPosAt()/sptPosComplAt() no original. Aqui os dois usam o mesmo
 * at_posicao de actuator_sensors.c (0-1000 por mil, ja passa um pouco
 * de 0/1000 - ver ACTUATOR_AT_POSICAO_INDEF em actuator_sensors.h).
 * DIVERGENCIA: o original tem um "atPosicaoCompleta" sem clamp pra
 * AtMovInvertido - aqui nao ha essa variante separada, at_posicao serve
 * pros dois (precisao um pouco menor bem nos extremos, aceitavel na v1).
 */
static bool pos_at(int16_t *pos)
{
	struct actuator_sensor_data d;

	if (!actuator_sensors_get(&d) || d.at_posicao == ACTUATOR_AT_POSICAO_INDEF) {
		return false;
	}
	*pos = d.at_posicao;
	return true;
}

/* --- atConRegistraOrigemParada() --- registra so' na transicao pra parada */
static void reg_origem_parada(enum actuator_ctl_cmd_orig origem)
{
	if (at_acao_estado != CTL_STD_PARAR) {
		at_origem_parada = origem;
	}
}

/* --- API publica --- */

void actuator_control_demand(int16_t posic, enum actuator_ctl_cmd_orig orig)
{
	/* STUB (passo 6): atESDAbort(); atPSTAbor(); - qualquer comando
	 * aborta ESD/PST no original.
	 */
	at_posicionar.posicao_mil = posic;
	at_posicionar.parar = false;
	at_posicionar.disparo = true;
	at_origem_cmd = orig;
	LOG_INF("Demanda: posicao=%d origem=%d", posic, orig);
}

void actuator_control_stop(enum actuator_ctl_cmd_orig orig)
{
	/* STUB (passo 6): atESDAbort(); atPSTAbor(); */
	at_posicionar.parar = true;
	reg_origem_parada(orig);
}

void actuator_control_stop_on_alarm(void)
{
	actuator_control_stop(ACTUATOR_CTL_ORIG_ALARME);
}

enum actuator_mov_status actuator_control_get_mov_stt(void)
{
	return at_status;
}

enum actuator_ctl_cmd_orig actuator_control_get_cmd_orig(void)
{
	return at_origem_cmd;
}

enum actuator_ctl_cmd_orig actuator_control_get_stop_orig(void)
{
	return at_origem_parada;
}

/* --- atAbortarAcao() --- */
static enum ctl_std abortar_acao(enum ctl_std estado)
{
	if (at_posicionar.parar) {
		at_posicionar.disparo = false;
		at_posicionar.parar = false;
		return CTL_STD_PARAR;
	}
	return estado;
}

/* --- atPosicionarEm() --- */
static int16_t posicionar_em(int16_t posicao)
{
	if (at_posicionar.disparo) {
		return at_posicionar.posicao_mil;
	}
	return posicao;
}

/* --- atEmPosicaoInc() --- decide se continua abrindo, ou para (na
 * demanda -> ACP_POSICAO, ou no limite superior -> ACP_LIMITE).
 */
static enum ctl_std em_posicao_inc(int16_t posicao)
{
	int16_t pos_at_val;

	if (!pos_at(&pos_at_val)) {
		return CTL_STD_PARAR;
	}
	pos_at_val += ANTECIPAR_PARADA;

	if (pos_at_val < LIMITE_SUPERIOR_MIL) {
		if (pos_at_val < posicao) {
			return CTL_STD_INCREMENTAR;
		}
		reg_origem_parada(ACTUATOR_CTL_ORIG_POSICAO);
		return CTL_STD_PARAR;
	}
	/* Chegou no limite superior (aberto) */
	reg_origem_parada(ACTUATOR_CTL_ORIG_LIMITE);
	return CTL_STD_PARAR;
}

/* --- atEmPosicaoDec() --- simetrico, limite inferior (fechado). */
static enum ctl_std em_posicao_dec(int16_t posicao)
{
	int16_t pos_at_val;

	if (!pos_at(&pos_at_val)) {
		return CTL_STD_PARAR;
	}
	pos_at_val -= ANTECIPAR_PARADA;

	if (pos_at_val > LIMITE_INFERIOR_MIL) {
		if (pos_at_val > posicao) {
			return CTL_STD_DECREMENTAR;
		}
		reg_origem_parada(ACTUATOR_CTL_ORIG_POSICAO);
		return CTL_STD_PARAR;
	}
	/* Chegou no limite inferior (fechado) */
	reg_origem_parada(ACTUATOR_CTL_ORIG_LIMITE);
	return CTL_STD_PARAR;
}

/* --- atSeguiPosicao() --- */
static enum ctl_std segui_posicao(int16_t posicao_alvo)
{
	enum ctl_std estado = em_posicao_dec(posicao_alvo);

	if (estado != CTL_STD_PARAR) {
		return estado;
	}
	return em_posicao_inc(posicao_alvo);
}

/* --- atConAcao() --- consome o "disparo" e decide o sentido inicial. */
static enum ctl_std con_acao(int16_t posicao)
{
	if (at_posicionar.disparo) {
		at_posicionar.disparo = false;
		return segui_posicao(posicao);
	}
	return CTL_STD_PARAR;
}

/* --- atBandaMorta() --- se a demanda esta dentro da faixa de parado em
 * torno da posicao atual, fica parado.
 */
static enum ctl_std banda_morta(enum ctl_std estado, int16_t posicao_em_vista_v)
{
	int16_t pos_at_val;

	if (!pos_at(&pos_at_val)) {
		return CTL_STD_PARAR;
	}
	if (posicao_em_vista_v > (pos_at_val - FAIXA_PARADA) &&
	    posicao_em_vista_v < (pos_at_val + FAIXA_PARADA)) {
		return CTL_STD_PARAR;
	}
	return estado;
}

/* --- atMargensLimite() --- nao deixa "modular" no limite: se ja esta na
 * margem do limite superior, nao volta a incrementar (e vice-versa).
 */
static enum ctl_std margens_limite(enum ctl_std estado_previsto, enum actuator_mov_status ml_status)
{
	switch (estado_previsto) {
	case CTL_STD_INCREMENTAR:
		if (ml_status == ACTUATOR_MOV_L_SUPER) {
			estado_previsto = CTL_STD_PARAR;
		}
		break;
	case CTL_STD_DECREMENTAR:
		if (ml_status == ACTUATOR_MOV_L_INFER) {
			estado_previsto = CTL_STD_PARAR;
		}
		break;
	default:
		break;
	}
	return estado_previsto;
}

/* --- atAlarme() --- se algum alarme bloqueia o sentido pedido, para. */
static enum ctl_std alarme(enum ctl_std estado)
{
	switch (estado) {
	case CTL_STD_INCREMENTAR:
		if (actuator_alarm_get_block_mov(ACTUATOR_MOV_INCR)) {
			return CTL_STD_PARAR;
		}
		break;
	case CTL_STD_DECREMENTAR:
		if (actuator_alarm_get_block_mov(ACTUATOR_MOV_DECR)) {
			return CTL_STD_PARAR;
		}
		break;
	default:
		break;
	}
	return estado;
}

/* --- atStatusPara() --- classifica a posicao parada: no limite superior,
 * no inferior, ou no meio. Histerese: quando ja esta "no meio", usa
 * metade da margem pra sair (evita ficar oscilando na borda).
 */
static enum actuator_mov_status status_para(enum actuator_mov_status status)
{
	int16_t pos_at_val;
	uint16_t limite_margem = LIMITE_MARGEM;

	if (!pos_at(&pos_at_val)) {
		return status;
	}
	if (status == ACTUATOR_MOV_PARADO) {
		limite_margem /= 2;
	}

	if (pos_at_val > (LIMITE_SUPERIOR_MIL - limite_margem)) {
		return ACTUATOR_MOV_L_SUPER;
	}
	if (pos_at_val < (LIMITE_INFERIOR_MIL + limite_margem)) {
		return ACTUATOR_MOV_L_INFER;
	}
	return ACTUATOR_MOV_PARADO;
}

/* --- atIncrementa/atDecrementa/atParar --- */
static void incrementa(void)
{
	actuator_motor_abre();
}

static void decrementa(void)
{
	actuator_motor_fecha();
}

static void parar(void)
{
	/* atParar(): so' desliga se sptTrqParaLimit() deixar - com
	 * "Fechamento com Torque" no limite fechado, o motor segue fechando
	 * por ate' 1,5 s pra assentar (ver actuator_torque.c).
	 */
	if (actuator_torque_para_limit(at_status)) {
		actuator_motor_para();
	}
}

/* --- AtMovManual() --- monitora movimento pelo volante: variacao de
 * posicao maior que a tolerancia com o motor parado dispara
 * OPER_MANUAL; sem atuacao por um periodo, libera.
 */
static void mov_manual(void)
{
	switch (st_mov_manual) {
	case EMM_MOV_MOTOR:
		if (at_acao_estado == CTL_STD_PARAR) {
			timer_reload(&timer_mov_manual, TEMPO_MOV_MANUAL_MS);
			st_mov_manual = EMM_MOV_INERCIA;
		}
		break;
	case EMM_MOV_INERCIA:
		if (timer_remaining(timer_mov_manual) == 0) {
			at_posicao_mov_manual = at_posicao_parado;
			st_mov_manual = EMM_PARADO;
		} else if (at_acao_estado != CTL_STD_PARAR) {
			st_mov_manual = EMM_MOV_MOTOR;
			actuator_alarm_release(ACTUATOR_ALARM_OPER_MANUAL);
		}
		break;
	case EMM_MOV_VOLANTE:
	case EMM_PARADO:
		if (at_posicao_parado > (at_posicao_mov_manual + TOLERANCIA_MOV_MANUAL) ||
		    at_posicao_parado < (at_posicao_mov_manual - TOLERANCIA_MOV_MANUAL)) {
			timer_reload(&timer_mov_manual, TEMPO_MOV_MANUAL_MS);
			at_posicao_mov_manual = at_posicao_parado;
			st_mov_manual = EMM_MOV_VOLANTE;
			actuator_alarm_set(ACTUATOR_ALARM_OPER_MANUAL);
			/* Tambem libera FAL_ACION_INV depois de mexer no volante. */
			actuator_alarm_release(ACTUATOR_ALARM_FAL_ACION_INV);
		} else if (timer_remaining(timer_mov_manual) == 0) {
			st_mov_manual = EMM_PARADO;
			timer_reload(&timer_mov_manual, TEMPO_MOV_MANUAL_MS);
			actuator_alarm_release(ACTUATOR_ALARM_OPER_MANUAL);
		} else if (at_acao_estado != CTL_STD_PARAR) {
			st_mov_manual = EMM_MOV_MOTOR;
			actuator_alarm_release(ACTUATOR_ALARM_OPER_MANUAL);
		}
		break;
	}
}

/* --- AtMovInvertido() --- monitora se a posicao se desloca ao contrario
 * do sentido acionado (fiacao trocada / mecanismo invertido) -> dispara
 * FAL_ACION_INV.
 */
static void mov_invertido(void)
{
	int16_t at_posic;
	int16_t dif_posi;

	if (!pos_at(&at_posic)) {
		return;
	}

	switch (st_mov_invertido) {
	case EMI_DESACIONADO:
		if (!actuator_motor_acionado()) {
			st_mov_invertido = EMI_PARADO;
		}
		break;
	case EMI_PARADO:
		if (at_acao_estado != CTL_STD_PARAR) {
			posic_mov_invertido = at_posic;
			st_mov_invertido = EMI_PARTIDA;
		}
		break;
	case EMI_PARTIDA:
		/* Aguarda confirmacao do acionamento. */
		if (at_acao_estado == CTL_STD_PARAR) {
			st_mov_invertido = EMI_DESACIONADO;
		} else if (actuator_motor_acionado()) {
			posic_mov_invertido = at_posic;
			st_mov_invertido = EMI_MOVIMENTO;
		}
		break;
	case EMI_MOVIMENTO:
		if (at_acao_estado == CTL_STD_PARAR) {
			st_mov_invertido = EMI_DESACIONADO;
			break;
		}
		if (at_acao_estado == CTL_STD_INCREMENTAR) {
			dif_posi = at_posic - posic_mov_invertido;
		} else { /* CTL_STD_DECREMENTAR */
			dif_posi = posic_mov_invertido - at_posic;
		}

		if (dif_posi >= TOLER_MOV_INVERTIDO) {
			/* Progresso ok, avanca a referencia. */
			posic_mov_invertido = at_posic;
		} else if ((dif_posi + TOLER_MOV_INVERTIDO) <= 0) {
			/* Deslocou ao contrario do acionamento. */
			actuator_alarm_set(ACTUATOR_ALARM_FAL_ACION_INV);
		}
		break;
	}
}

/* --- at_ctl_run() --- */
void actuator_control_run(void)
{
	enum actuator_mov_status ult_at_status = at_status;
	int16_t pos_now;

	/* Primeira coisa do ciclo, antes de qualquer retorno antecipado (ex.:
	 * sem posicao) - senao um pot desconectado reiniciaria a placa em
	 * loop. O que o watchdog vigia e' o laco rodar, nao o sensor.
	 */
	watchdog_feed_controle();

	/* Falha de comunicacao com o sensor de posicao (equivalente a
	 * AL_COM_POS no fwBLE, set/release em sensPosTor.c). Gatilho: o
	 * rastreio de saude do ADS1000 @0x49 (device_health, via
	 * position_sensor_is_online()). retain NOT_RETAIN -> some sozinho
	 * quando o sensor volta. Como no fwBLE (timerSenPosOk, recarregado a
	 * cada resposta do sensor, TEMPO_FALHA_SENSOR = 3 s desde o boot), so'
	 * alarma depois de 3 s seguidos sem o sensor - antes ligava no 1o
	 * ciclo do boot e soltava na 1a leitura boa, deixando um par
	 * ativo/desativo no registro de eventos a cada energizacao.
	 */
	{
		int64_t agora = k_uptime_get();

		if (position_sensor_is_online()) {
			ultimo_pos_online = agora;
			actuator_alarm_release(ACTUATOR_ALARM_COM_SENS_POS);
		} else if (agora - ultimo_pos_online >= TEMPO_FALHA_SENSOR_POS_MS) {
			actuator_alarm_set(ACTUATOR_ALARM_COM_SENS_POS);
		}
	}

	/* Mesma ideia pro sensor de torque (AL_COM_TRQ) - ver
	 * actuator_torque_check_com() pro tempo de tolerancia de 3 s.
	 */
	actuator_torque_check_com();

	/* DIVERGENCIA/seguranca (nao existe no original): sem posicao
	 * valida nao da pra decidir limite/sentido - forca parada.
	 * O atuador BLE real assume o sensor sempre presente; aqui o pot
	 * pode estar solto na bancada.
	 */
	if (!pos_at(&pos_now)) {
		actuator_motor_para();
		at_acao_estado = CTL_STD_PARAR;
		at_status = ACTUATOR_MOV_PARADO;
		return;
	}

	/* Origem do comando - equivalente a atComandoOrigem()->atModoAcao().
	 * O arbitro de modo (actuator_mode.c) decide qual fonte comanda:
	 * consome o Painel Remoto (BLE = LOCAL), e em DESLIGA/PARA/PARAM/INFO
	 * forca parada. E' a unica entrada de comando.
	 */
	actuator_mode_run();

	posicao_em_vista = posicionar_em(posicao_em_vista);
	at_acao_estado = abortar_acao(at_acao_estado);

	switch (at_acao_estado) {
	case CTL_STD_INCREMENTAR:
		incrementa();
		at_acao_estado = em_posicao_inc(posicao_em_vista);
		at_status = ACTUATOR_MOV_INCR;
		break;

	case CTL_STD_DECREMENTAR:
		decrementa();
		at_acao_estado = em_posicao_dec(posicao_em_vista);
		at_status = ACTUATOR_MOV_DECR;
		break;

	case CTL_STD_PARAR:
	default:
		at_status = status_para(at_status);
		parar();
		at_posicao_parado = pos_now;
		at_acao_estado = banda_morta(con_acao(posicao_em_vista), posicao_em_vista);
		at_acao_estado = margens_limite(at_acao_estado, at_status);
		break;
	}

	/* Torque (sptTratTrq(), que no original roda no mesmo laco de
	 * aplicacao): alarmes de sobretorque/valvula travada do ciclo, antes
	 * de consultar os bloqueios - um sobretorque para o motor ja' aqui.
	 */
	actuator_torque_run(at_status);

	/* Alarme que cancela movimento. */
	at_acao_estado = alarme(at_acao_estado);

	if (ult_at_status != at_status) {
		/* atRegEvent(EV_OPE_NUL + atStatus) - enum actuator_mov_status
		 * tem os mesmos valores do at_ctl_mov_stt_t (1..5).
		 */
		actuator_regevent(EV_OPE_NUL + (uint16_t)at_status);
		LOG_INF("Controle: status %d -> %d (origem cmd=%d parada=%d)", ult_at_status,
			at_status, at_origem_cmd, at_origem_parada);
	}

	mov_manual();
	mov_invertido();
}

static void run_work_handler(struct k_work *work)
{
	actuator_control_run();
	k_work_reschedule(&run_work, K_MSEC(CTL_RUN_PERIOD_MS));
}

void actuator_control_init(void)
{
	at_posicionar.parar = true;
	at_posicionar.disparo = false;
	at_posicionar.posicao_mil = 0;
	at_acao_estado = CTL_STD_PARAR;
	at_status = ACTUATOR_MOV_PARADO;
	posicao_em_vista = 0;
	at_origem_cmd = ACTUATOR_CTL_ORIG_NULO;
	at_origem_parada = ACTUATOR_CTL_ORIG_NULO;

	timer_mov_manual = k_uptime_get();
	at_posicao_mov_manual = 0;
	st_mov_manual = EMM_MOV_MOTOR;
	st_mov_invertido = EMI_DESACIONADO;

	k_work_init_delayable(&run_work, run_work_handler);
	k_work_reschedule(&run_work, K_MSEC(CTL_RUN_PERIOD_MS));

	LOG_INF("Controle do atuador iniciado (loop %d ms)", CTL_RUN_PERIOD_MS);
}
