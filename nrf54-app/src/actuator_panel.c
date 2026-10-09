/*
 * Painel Remoto - ver actuator_panel.h. Porta de ifFerConfigRunCmd()
 * (BLE/Atuador/ifFerConfig.c). O que diverge do original:
 *  - Sincronizacao: a palavra e' escrita no contexto BLE e consumida no
 *    contexto do loop de controle - `atomic_t` cobre isso (o fwBLE e'
 *    super-loop cooperativo, nao precisa). Uma corrida na janela de
 *    ~10ms entre escrever e consumir e' inofensiva (a interface
 *    re-envia).
 *  - `mode` (bits 4-6): guardado mas nao consumido - e' o arbitro de
 *    modo (passo 5) que usa.
 *  - ESD/PST: no fwBLE ifFerConfigRunCmd() tambem cuidava de um alarme
 *    de descarga; aqui so' quita-alarme comum (ack_alarm). O resto vem
 *    nos passos 5/6.
 */

#include "actuator_panel.h"
#include "actuator_control.h"
#include "actuator_alarm.h"
#include "actuator_mode.h"
#include "actuator_regevent.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(actuator_panel, LOG_LEVEL_INF);

#define PANEL_CMD_OPEN   BIT(0)
#define PANEL_CMD_CLOSE  BIT(1)
#define PANEL_CMD_STOP   BIT(2)
#define PANEL_ACK_ALARM  BIT(3)
#define PANEL_MODE_MASK  (BIT(4) | BIT(5) | BIT(6))
#define PANEL_MODE_SHIFT 4
#define PANEL_ALLOC      BIT(14)
#define PANEL_FREE       BIT(15)

/* TIME_LEASE no ifFerConfig.c */
#define PANEL_LEASE_MS 5000

static atomic_t panel_word;
/* Deadline do lease - so' tocado no contexto de actuator_panel_run(). */
static int64_t lease_deadline;

void actuator_panel_init(void)
{
	atomic_set(&panel_word, 0);
	lease_deadline = 0;
	LOG_INF("Painel remoto iniciado (area 0x%08lx, lease %d ms)", ACTUATOR_PANEL_AREA_ADDR,
		PANEL_LEASE_MS);
}

void actuator_panel_write_word(uint16_t word)
{
	atomic_set(&panel_word, word);
}

bool actuator_panel_run(void)
{
	uint16_t w = (uint16_t)atomic_get(&panel_word);
	uint16_t consumed = 0;
	bool ran = false;

	/* Lease: alloc renova, free libera (mesmos bits do fwBLE). */
	if (w & PANEL_ALLOC) {
		consumed |= PANEL_ALLOC;
		lease_deadline = k_uptime_get() + PANEL_LEASE_MS;
	} else if (w & PANEL_FREE) {
		consumed |= PANEL_FREE;
		lease_deadline = 0;
	}

	/* Campo `mode` (bits 4-6): 7 = nao mudar. NAO e' consumido (limpar os
	 * bits daria 0 = DESLIGA) - e' idempotente (actuator_mode_set pro
	 * mesmo valor e' no-op). Nao passa pelo gate de lease de proposito:
	 * trocar de modo e' menos perigoso que comandar, e um seletor de modo
	 * na interface pode nao ter mandado alloc recente.
	 */
	uint8_t mode_field = (w & PANEL_MODE_MASK) >> PANEL_MODE_SHIFT;

	if (mode_field != ACTUATOR_MODE_NOCHANGE) {
		actuator_mode_set((enum actuator_mode)mode_field);
	}

	/* Comando so' vale com lease ativo E em modo LOCAL (a interface BLE
	 * e' a fonte "local" pra nos - ver actuator_mode.h) E sem inibicao do
	 * comando local (paramDado.inibCmdLoc). ack_alarm segue a mesma regra
	 * (quitacao pela interface = acao local).
	 */
	if (k_uptime_get() < lease_deadline && actuator_mode_get() == ACTUATOR_MODE_LOCAL &&
	    !actuator_mode_local_inibido()) {
		/* O fwBLE NAO auto-limpa ack_alarm (so' os cmd_*) - a interface
		 * escreve 0 quando quiser parar de quitar. Mantido igual.
		 */
		if (w & PANEL_ACK_ALARM) {
			actuator_alarm_clears_all(ACTUATOR_ALARM_CLEAR_LOCAL);
			ran = true;
		}

		if (w & PANEL_CMD_STOP) {
			consumed |= PANEL_CMD_STOP;
			actuator_control_stop(ACTUATOR_CTL_ORIG_LOCAL);
			ran = true;
		} else if (w & PANEL_CMD_CLOSE) {
			consumed |= PANEL_CMD_CLOSE;
			/* EV_LOC_FEC/EV_LOC_ABR: no fwBLE sao registrados na borda
			 * da acao da IHM (atModo.c, ultAcao). Aqui cada comando e'
			 * um pulso consumido - um registro por comando.
			 */
			actuator_regevent(EV_LOC_FEC);
			actuator_control_close(ACTUATOR_CTL_ORIG_LOCAL);
			ran = true;
		} else if (w & PANEL_CMD_OPEN) {
			consumed |= PANEL_CMD_OPEN;
			actuator_regevent(EV_LOC_ABR);
			actuator_control_open(ACTUATOR_CTL_ORIG_LOCAL);
			ran = true;
		}
	}

	if (consumed) {
		atomic_and(&panel_word, ~(atomic_val_t)consumed);
	}

	return ran;
}
