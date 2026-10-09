/*
 * Arbitro de modo - ver actuator_mode.h. Porta de atModoAcao() e
 * companhia (BLE/Atuador/atModo.c).
 */

#include "actuator_mode.h"
#include "actuator_panel.h"
#include "actuator_control.h"
#include "actuator_alarm.h"
#include "actuator_params.h"
#include "actuator_regevent.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(actuator_mode, LOG_LEVEL_INF);

static enum actuator_mode current_mode;
static enum actuator_mode last_mode;

/* atModoInibLoc() no original: paramDado.inibCmdLoc || ifDigInibLoc().
 * ifDigInibLoc() (entrada digital da placa IO) nao existe aqui - so' o
 * parametro. Inibido, LOCAL_INIBIDO acende ao entrar em LOCAL, o Painel
 * BLE para de despachar comandos (actuator_panel.c) e o LOCAL cai pra
 * REMOTO (mesma logica do original).
 */
static bool mode_inib_local(void)
{
	return actuator_params_inib_cmd_loc();
}

bool actuator_mode_local_inibido(void)
{
	return mode_inib_local();
}

/* atModoRemoto() no original: checa ESD, depois ifFerConfigRunCmd()
 * (Painel BLE - que pra nos e' LOCAL, ja tratado em actuator_panel_run()),
 * depois arbitra discreta/barramento/analogica. Aqui: ESD e' passo 6, e
 * a fonte de comando de rede (RS485/MB TCP/MQTT) e' passo 9 - por
 * enquanto nada comanda em REMOTO.
 */
static void mode_remoto(void)
{
	/* STUB passo 6: if (actuator_esd_cmd()) return; */
	/* STUB passo 9: consumir a fonte de comando de rede. */
}

void actuator_mode_init(void)
{
	current_mode = ACTUATOR_MODE_LOCAL;
	last_mode = ACTUATOR_MODE_LOCAL;
	LOG_INF("Arbitro de modo iniciado - modo LOCAL (interface BLE)");
}

void actuator_mode_set(enum actuator_mode mode)
{
	if (mode > ACTUATOR_MODE_INFO) {
		return; /* fora de 0..5 - ignora (7 = NOCHANGE nunca chega aqui) */
	}

	/* DESLIGA nao tem semantica propria no SIM Connect por enquanto -
	 * colapsado em PARA (decisao com o Felipe, 2026-09-04): a distincao
	 * no fwBLE vinha da forma fisica do seletor com mola (PARA = neutro,
	 * DESLIGA = posicao mantida/cadeavel); aqui todo modo e' escolha
	 * explicita. O valor 0 fica RESERVADO pra um eventual "fora de
	 * servico / bloqueio de manutencao" de verdade (que ai recusaria ate
	 * REMOTO e exigiria destravar explicito).
	 */
	if (mode == ACTUATOR_MODE_DESLIGA) {
		mode = ACTUATOR_MODE_PARA;
	}

	current_mode = mode;
}

enum actuator_mode actuator_mode_get(void)
{
	return current_mode;
}

static const char *mode_name(enum actuator_mode m)
{
	switch (m) {
	case ACTUATOR_MODE_DESLIGA:
		return "DESLIGA";
	case ACTUATOR_MODE_PARA:
		return "PARA";
	case ACTUATOR_MODE_LOCAL:
		return "LOCAL (interface BLE)";
	case ACTUATOR_MODE_REMOTO:
		return "REMOTO (rede)";
	case ACTUATOR_MODE_PARAM:
		return "PARAM";
	case ACTUATOR_MODE_INFO:
		return "INFO";
	default:
		return "?";
	}
}

/* Trata a transicao de modo - equivalente ao bloco
 * `if (ultModo != atModo)` de atModoAcao(). */
static void on_mode_change(enum actuator_mode mode)
{
	/* Saiu de REMOTO -> alarme de "modo nao remoto" (some ao voltar). */
	if (last_mode == ACTUATOR_MODE_REMOTO) {
		actuator_alarm_set(ACTUATOR_ALARM_MODO_NAO_REMOTO);

		/* ...e se estava movendo ao ser tirado de REMOTO pra PARA,
		 * marca PARADA_LOCAL (comando remoto derrubado localmente).
		 */
		if (mode == ACTUATOR_MODE_PARA) {
			enum actuator_mov_status s = actuator_control_get_mov_stt();

			if (s == ACTUATOR_MOV_INCR || s == ACTUATOR_MOV_DECR) {
				actuator_alarm_set(ACTUATOR_ALARM_PARADA_LOCAL);
			}
		}
	}

	LOG_INF("Modo -> %s", mode_name(mode));

	/* Registro dos modos - mesmo switch de atModoAcao(). */
	switch (mode) {
	case ACTUATOR_MODE_DESLIGA:
		actuator_regevent(EV_LOC_DESLIG);
		break;
	case ACTUATOR_MODE_PARA:
		actuator_regevent(EV_LOC_PAR);
		break;
	case ACTUATOR_MODE_LOCAL:
		actuator_regevent(EV_LOC_LOCAL);
		break;
	case ACTUATOR_MODE_REMOTO:
		actuator_regevent(EV_LOC_REMOTO);
		break;
	case ACTUATOR_MODE_PARAM:
		actuator_regevent(EV_LOC_PARAM);
		break;
	case ACTUATOR_MODE_INFO:
		actuator_regevent(EV_LOC_INFO);
		break;
	default:
		break;
	}

	switch (mode) {
	case ACTUATOR_MODE_LOCAL:
		actuator_alarm_check(mode_inib_local(), ACTUATOR_ALARM_LOCAL_INIBIDO);
		break;
	case ACTUATOR_MODE_REMOTO:
		actuator_alarm_release(ACTUATOR_ALARM_MODO_NAO_REMOTO);
		actuator_alarm_release(ACTUATOR_ALARM_PARADA_LOCAL);
		break;
	case ACTUATOR_MODE_PARA:
		/* atModoPara(): ao ENTRAR em PARA (so' uma vez, ultModo !=
		 * AM_PARA), quita os alarmes locais se paramDado.alAtQtLoc. A
		 * condicao ifIhmAcao() == AT_AC_N do original (nenhuma acao de
		 * IHM em curso) e' sempre verdadeira aqui - nao ha IHM.
		 */
		if (actuator_params_al_at_qt_loc()) {
			actuator_alarm_clears_all(ACTUATOR_ALARM_CLEAR_LOCAL);
		}
		break;
	default:
		break;
	}
}

void actuator_mode_run(void)
{
	/* Sempre: processa o Painel Remoto. Ele cuida do lease e do campo
	 * de modo (que pode chamar actuator_mode_set() aqui, mudando
	 * current_mode ANTES do dispatch abaixo - de proposito: um write
	 * que troca modo + manda comando no mesmo frame ja e' avaliado no
	 * modo novo). Comandos de movimento so' sao despachados por ele se
	 * o modo for LOCAL (ver actuator_panel_run()).
	 */
	actuator_panel_run();

	enum actuator_mode mode = current_mode;

	if (mode != last_mode) {
		on_mode_change(mode);
	}

	/* Dispatch por modo - equivalente ao 2o switch de atModoAcao(). */
	switch (mode) {
	case ACTUATOR_MODE_LOCAL:
		/* Comandos ja foram consumidos por actuator_panel_run() acima.
		 * Se o local estiver inibido, o original cai pra remoto - aqui
		 * so' significa "nada comanda" (sem fonte remota ainda).
		 */
		if (mode_inib_local()) {
			mode_remoto();
		}
		break;

	case ACTUATOR_MODE_REMOTO:
		mode_remoto();
		break;

	case ACTUATOR_MODE_DESLIGA: /* nunca chega aqui - colapsado em PARA */
	case ACTUATOR_MODE_PARA:
	case ACTUATOR_MODE_PARAM:
	case ACTUATOR_MODE_INFO:
		/* atModoDelisgado()/atModoPara()/atModoParam()/atModoInfo() no
		 * original: todos param o movimento. (A auto-quitacao do
		 * atModoPara() por alAtQtLoc fica em on_mode_change().)
		 */
		actuator_control_stop(ACTUATOR_CTL_ORIG_LOCAL);
		break;
	}

	last_mode = mode;
}
