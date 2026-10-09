/*
 * Motor de alarmes - ver actuator_alarm.h pro escopo desta primeira
 * versao (8 de ~60 alarmes do fwBLE original). O ALGORITMO abaixo
 * (tabela acao/retencao + set/release/check/get_block_mov/clears_all) e'
 * portado fielmente de BLE/Atuador/at_alarm.c - o que muda e' so' o
 * numero de linhas da tabela e as 3 dependencias externas abaixo, cada
 * uma com uma decisao de escopo explicita:
 *
 * 1. at_ctl_stop()/at_ctl_get_mov_stt() (atControle.c original) - o
 *    original tem uma dependencia CRUZADA real entre alarme e controle
 *    (atControle.c chama at_alarm_get_block_mov(), e at_alarm.c chama de
 *    volta at_ctl_stop() de dentro de al_reg_block_mov()). Como
 *    actuator_control.c ainda nao existe (passo 3), um #include direto
 *    criaria uma dependencia de link que nao compila ainda. Resolvido
 *    com o mesmo padrao de callback que actuator_client.c/my_lbs.c ja
 *    usam neste projeto pra desacoplar camadas (struct actuator_alarm_cb,
 *    ver .h) - actuator_control.c registra `stop_cb` quando existir.
 *    A segunda metade dessa dependencia (at_ctl_get_mov_stt(), usada no
 *    original só pelos modos de retenção AR_ACT_INC_INFO/AR_ACT_DEC_INFO)
 *    nao foi portada ainda porque nenhum dos 8 alarmes de hoje usa esses
 *    modos - ver o enum reduzido `actuator_alarm_retain` abaixo.
 * 2. atESDExec()/paramDado.ESDD* (al_esd_jump() original, supressao de
 *    alarme durante ESD) - ESD e' passo 6, ParamZarI e' passo 8, nenhum
 *    dos dois existe ainda. `esd_jump()` abaixo e' um stub que sempre
 *    retorna false (nenhuma supressao) - correto por enquanto porque
 *    ESD nunca esta ativo, e nenhum dos 8 alarmes de hoje e' candidato a
 *    essa supressao no original de qualquer forma (so
 *    SOBREAQUECIM/FALTA_FASE/TORQUE_AB/TORQUE_FC sao).
 * 3. atRegEvent() - portado em actuator_regevent.c. O evento leva o
 *    numero do alarme no alarm_t do fwBLE (coluna fwble_id da tabela
 *    abaixo), nao a posicao no nosso enum: EV_AL_ON/EV_AL_OFF + alarm_t
 *    e' o que um cliente do fwBLE espera. Os LOG_INF/LOG_WRN continuam
 *    pra visibilidade em RTT.
 *
 * NAO portado (avaliado e descartado, nao "esquecido"):
 * - As funcoes de display ciclico (at_alarm_get_next_view/reset_view/
 *   has_view/get_name, at_alarm_text_index[], ifIhmGetText) - existem no
 *   original pra alimentar um painel local com display. SIM Connect nao
 *   tem display fisico (ver docs/DECISOES_PRODUTO.md). Se um dia for
 *   preciso listar alarmes ativos, o caminho e' uma area/characteristic
 *   BLE nova, no mesmo espirito de PROTOCOLO_INTERFACE.md - nao esta
 *   API de ciclagem pra display.
 * - at_alarm_get_critic_fail()/at_alarm_get_pos_fail() - agregam MUITOS
 *   alarmes especificos (posicao absoluta, IO Analogica/Digital, FSA...)
 *   que nao existem no enum reduzido de hoje. Refazer quando o conjunto
 *   de 60 alarmes estiver completo.
 * - reset_check()/resetaTudo (o gatilho `dadoFSA.senFase` dentro de
 *   at_alarm_clears_all() original) - especifico de sensoriamento
 *   trifasico via FSA, que este hardware nao tem (ver
 *   docs/PORTABILIDADE_BLE_LEGADO.md e a decisao de motor por GPIO
 *   direto, memoria sim-connect-command-control-port-strategy).
 */

#include "actuator_alarm.h"
#include "actuator_regevent.h"

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(actuator_alarm, LOG_LEVEL_INF);

/* Equivalente a al_action_t - os valores que os alarmes portados usam.
 * STOP_INCR/STOP_DECR (AA_STO_INC/AA_STO_DEC) bloqueiam so' um sentido -
 * entraram com os alarmes de torque.
 */
enum alarm_action {
	ALARM_ACTION_NONE = 0,
	ALARM_ACTION_STOP_BOTH,
	ALARM_ACTION_STOP_INCR,
	ALARM_ACTION_STOP_DECR,
};

/* Equivalente a al_retain_t - os modos que os alarmes portados usam.
 * Ainda fora (nenhum alarme portado precisa): AR_ACT_INFO, AR_CONF.
 */
enum alarm_retain {
	ALARM_RETAIN_INFO,        /* libera junto com a acao (AR_INFO) */
	ALARM_RETAIN_INFO_REMOTE, /* como INFO, mas nunca aparece num display local (AR_INFO_REMOTE) */
	ALARM_RETAIN_NOT_RETAIN,  /* libera info assim que a acao cessa, sem quitacao (AR_NOT_RETAIN) */
	ALARM_RETAIN_ACT_INC_INFO, /* release so' libera a acao com o motor FECHANDO (AR_ACT_INC_INFO) */
	ALARM_RETAIN_ACT_DEC_INFO, /* release so' libera a acao com o motor ABRINDO (AR_ACT_DEC_INFO) */
};

struct alarm_ctl_entry {
	enum alarm_action action;
	enum alarm_retain retain;
	uint8_t fwble_id; /* posicao no alarm_t do fwBLE (at_alarm.h) - id do registro de eventos */
	const char *name; /* so' pra log - nao existe no original (la' e' ifIhmGetText) */
};

/* Tabela de controle - equivalente a al_ctl[NUM_ALARMS] no original.
 * Linhas copiadas verbatim de BLE/Atuador/at_alarm.c (mesma acao/
 * retencao), na mesma ordem relativa. Adicionar um alarme = 1 linha
 * aqui (na mesma ordem do enum em actuator_alarm.h) + 1 linha no enum.
 */
static const struct alarm_ctl_entry alarm_ctl[ACTUATOR_ALARM_NUM] = {
	[ACTUATOR_ALARM_ESD_EXT] = { ALARM_ACTION_NONE, ALARM_RETAIN_INFO, 14, "ESD_EXT" },
	[ACTUATOR_ALARM_PARADA_LOCAL] = { ALARM_ACTION_NONE, ALARM_RETAIN_INFO, 15, "PARADA_LOCAL" },
	[ACTUATOR_ALARM_MODO_NAO_REMOTO] = { ALARM_ACTION_NONE, ALARM_RETAIN_INFO_REMOTE, 16,
					      "MODO_NAO_REMOTO" },
	[ACTUATOR_ALARM_LOCAL_INIBIDO] = { ALARM_ACTION_NONE, ALARM_RETAIN_NOT_RETAIN, 26,
					    "LOCAL_INIBIDO" },
	[ACTUATOR_ALARM_FAL_ACION_INV] = { ALARM_ACTION_STOP_BOTH, ALARM_RETAIN_INFO, 28,
					    "FAL_ACION_INV" },
	[ACTUATOR_ALARM_OPER_MANUAL] = { ALARM_ACTION_NONE, ALARM_RETAIN_INFO, 32, "OPER_MANUAL" },
	[ACTUATOR_ALARM_PST_EXE] = { ALARM_ACTION_NONE, ALARM_RETAIN_NOT_RETAIN, 47, "PST_EXE" },
	[ACTUATOR_ALARM_PST_FAL] = { ALARM_ACTION_NONE, ALARM_RETAIN_INFO, 48, "PST_FAL" },
	/* AL_COM_POS no fwBLE: { AA_STO_INC_DEC, AR_NOT_RETAIN } - bloqueia
	 * os dois sentidos e some sozinho quando o sensor volta a responder
	 * (sem exigir quitacao).
	 */
	[ACTUATOR_ALARM_COM_SENS_POS] = { ALARM_ACTION_STOP_BOTH, ALARM_RETAIN_NOT_RETAIN, 2,
					  "COM_SENS_POS" },
	/* Torque - linhas verbatim do at_alarm.c original:
	 *   AL_TORQUE_AB      { AA_STO_INC,     AR_ACT_INC_INFO }
	 *   AL_TORQUE_FC      { AA_STO_DEC,     AR_ACT_DEC_INFO }
	 *   AL_VALV_TRAVADA   { AA_STO_INC_DEC, AR_INFO }
	 *   AL_OPER_INCOMPLETA{ AA_NONE,        AR_INFO }
	 */
	[ACTUATOR_ALARM_TORQUE_AB] = { ALARM_ACTION_STOP_INCR, ALARM_RETAIN_ACT_INC_INFO, 11,
				       "TORQUE_AB" },
	[ACTUATOR_ALARM_TORQUE_FC] = { ALARM_ACTION_STOP_DECR, ALARM_RETAIN_ACT_DEC_INFO, 12,
				       "TORQUE_FC" },
	[ACTUATOR_ALARM_VALV_TRAVADA] = { ALARM_ACTION_STOP_BOTH, ALARM_RETAIN_INFO, 13,
					  "VALV_TRAVADA" },
	[ACTUATOR_ALARM_OPER_INCOMPLETA] = { ALARM_ACTION_NONE, ALARM_RETAIN_INFO, 17,
					     "OPER_INCOMPLETA" },
	/* AL_COM_TRQ no fwBLE: { AA_STO_INC_DEC, AR_NOT_RETAIN } - mesmo
	 * comportamento do COM_SENS_POS: bloqueia os dois sentidos e some
	 * sozinho quando o sensor volta.
	 */
	[ACTUATOR_ALARM_COM_SENS_TRQ] = { ALARM_ACTION_STOP_BOTH, ALARM_RETAIN_NOT_RETAIN, 3,
					  "COM_SENS_TRQ" },
};

/* O bitmap da area de status (actuator_alarm_info_bitmap) e' u16 */
BUILD_ASSERT(ACTUATOR_ALARM_NUM <= 16, "bitmap de alarmes da area de status tem 16 bits");

struct alarm_status {
	bool action;
	bool info;
};

static struct alarm_status alarm_stt[ACTUATOR_ALARM_NUM];
static bool block_mov;
static struct actuator_alarm_cb callbacks;

static inline bool alarm_valid(enum actuator_alarm_id alarm)
{
	return alarm < ACTUATOR_ALARM_NUM;
}

static inline const char *alarm_name(enum actuator_alarm_id alarm)
{
	return alarm_valid(alarm) ? alarm_ctl[alarm].name : "?";
}

/* Equivalente a al_esd_jump() - ver dependencia 2 no comentario do topo. */
static bool esd_jump(enum actuator_alarm_id alarm)
{
	(void)alarm;
	return false;
}

/* Equivalente a al_reg_set() - registra a transicao pra ativo. */
static void reg_set(enum actuator_alarm_id alarm)
{
	if (!alarm_stt[alarm].action) {
		alarm_stt[alarm].action = true;
		alarm_stt[alarm].info = true;
		LOG_INF("Alarme ATIVADO: %s", alarm_name(alarm));
		actuator_regevent(EV_AL_ON + alarm_ctl[alarm].fwble_id);
	}
}

/* Equivalente a al_reg_release(). */
static void reg_release(enum actuator_alarm_id alarm)
{
	if (alarm_stt[alarm].action) {
		alarm_stt[alarm].action = false;
		LOG_INF("Alarme desativado: %s", alarm_name(alarm));
		actuator_regevent(EV_AL_OFF + alarm_ctl[alarm].fwble_id);
	}
}

/* Equivalente a al_reg_block_mov() - dispara o callback de parada (se
 * registrado, ver dependencia 1 no topo) toda vez que e' chamado, igual
 * ao original (nao so' na borda).
 */
static void reg_block_mov(enum actuator_alarm_id alarm)
{
	if (!block_mov) {
		block_mov = true;
		LOG_WRN("Movimento bloqueado por alarme: %s", alarm_name(alarm));
		actuator_regevent(EV_AL_BQ);
	}
	if (callbacks.stop_cb) {
		callbacks.stop_cb();
	}
}

void actuator_alarm_init(const struct actuator_alarm_cb *cb)
{
	for (int i = 0; i < ACTUATOR_ALARM_NUM; i++) {
		alarm_stt[i].action = false;
		alarm_stt[i].info = false;
	}
	block_mov = false;

	if (cb) {
		callbacks = *cb;
	} else {
		callbacks.stop_cb = NULL;
		callbacks.mov_stt_cb = NULL;
	}

	LOG_INF("Motor de alarmes iniciado (%d de ~60 alarmes do fwBLE portados)", ACTUATOR_ALARM_NUM);
}

void actuator_alarm_set(enum actuator_alarm_id alarm)
{
	if (alarm_valid(alarm)) {
		reg_set(alarm);
	}
}

void actuator_alarm_release(enum actuator_alarm_id alarm)
{
	if (!alarm_valid(alarm)) {
		return;
	}

	switch (alarm_ctl[alarm].retain) {
	case ALARM_RETAIN_NOT_RETAIN:
		reg_release(alarm);
		alarm_stt[alarm].info = false;
		break;
	case ALARM_RETAIN_INFO:
	case ALARM_RETAIN_INFO_REMOTE:
		reg_release(alarm);
		break;
	case ALARM_RETAIN_ACT_INC_INFO:
		/* Sobretorque abrindo: o bloqueio de abrir so' sai quando o motor
		 * estiver fechando (ou pelo volante, action_clear).
		 */
		if (callbacks.mov_stt_cb && callbacks.mov_stt_cb() == ACTUATOR_MOV_DECR) {
			reg_release(alarm);
		}
		break;
	case ALARM_RETAIN_ACT_DEC_INFO:
		if (callbacks.mov_stt_cb && callbacks.mov_stt_cb() == ACTUATOR_MOV_INCR) {
			reg_release(alarm);
		}
		break;
	}

	/* Confere consistencia - se por algum motivo a acao ainda estiver
	 * ativa (ex.: retencao nao liberou), o info tem que refletir isso.
	 */
	if (alarm_stt[alarm].action) {
		alarm_stt[alarm].info = true;
	}
}

void actuator_alarm_action_clear(enum actuator_alarm_id alarm)
{
	if (alarm_valid(alarm)) {
		reg_release(alarm);
	}
}

void actuator_alarm_check(bool active, enum actuator_alarm_id alarm)
{
	if (active) {
		actuator_alarm_set(alarm);
	} else {
		actuator_alarm_release(alarm);
	}
}

bool actuator_alarm_get_action(enum actuator_alarm_id alarm)
{
	return alarm_valid(alarm) ? alarm_stt[alarm].action : false;
}

bool actuator_alarm_get_info(enum actuator_alarm_id alarm)
{
	if (!alarm_valid(alarm)) {
		return false;
	}
	/* A acao sempre e' informada, mesmo se info==false por algum motivo. */
	return alarm_stt[alarm].action || alarm_stt[alarm].info;
}

bool actuator_alarm_get_block_mov(enum actuator_mov_status at_status)
{
	for (int i = 0; i < ACTUATOR_ALARM_NUM; i++) {
		if (!alarm_stt[i].action) {
			continue;
		}

		/* Info sempre acompanha uma acao ativa. */
		alarm_stt[i].info = true;

		if (esd_jump((enum actuator_alarm_id)i)) {
			continue;
		}

		switch (alarm_ctl[i].action) {
		case ALARM_ACTION_STOP_BOTH:
			if (at_status == ACTUATOR_MOV_INCR || at_status == ACTUATOR_MOV_DECR) {
				reg_block_mov((enum actuator_alarm_id)i);
				return true;
			}
			break;
		case ALARM_ACTION_STOP_INCR:
			if (at_status == ACTUATOR_MOV_INCR) {
				reg_block_mov((enum actuator_alarm_id)i);
				return true;
			}
			break;
		case ALARM_ACTION_STOP_DECR:
			if (at_status == ACTUATOR_MOV_DECR) {
				reg_block_mov((enum actuator_alarm_id)i);
				return true;
			}
			break;
		case ALARM_ACTION_NONE:
			break;
		}
	}

	block_mov = false;
	return false;
}

void actuator_alarm_clears_all(enum actuator_alarm_clear_mode mode)
{
	bool cleaned = false;

	/* DIVERGENCIA do fwBLE: o original marca `cleaned` pra QUALQUER alarme
	 * inativo (logava EV_LOC_QT a cada chamada). Aqui so' conta quando de
	 * fato limpa uma sinalizacao (info true -> false) - importante porque
	 * o bit ack_alarm do Painel Remoto nao e' auto-limpo, entao esta
	 * funcao e' chamada a cada ciclo enquanto ele estiver setado.
	 */
	for (int i = 0; i < ACTUATOR_ALARM_NUM; i++) {
		if (!alarm_stt[i].action && alarm_stt[i].info) {
			alarm_stt[i].info = false;
			cleaned = true;
		}
	}

	if (cleaned) {
		LOG_INF("Alarmes quitados (%s)",
			mode == ACTUATOR_ALARM_CLEAR_LOCAL ? "local/BLE" : "remoto");
		actuator_regevent(mode == ACTUATOR_ALARM_CLEAR_LOCAL ? EV_LOC_QT : EV_REM_QT);
	}
}

bool actuator_alarm_get_blocked(void)
{
	return block_mov;
}

uint16_t actuator_alarm_info_bitmap(void)
{
	uint16_t bm = 0;

	for (int i = 0; i < ACTUATOR_ALARM_NUM; i++) {
		if (alarm_stt[i].action || alarm_stt[i].info) {
			bm |= BIT(i);
		}
	}
	return bm;
}
