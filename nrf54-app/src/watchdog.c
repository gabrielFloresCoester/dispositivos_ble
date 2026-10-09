/*
 * Watchdog - ver watchdog.h.
 */

#include "watchdog.h"
#include "actuator_motor.h"
#include "sensor_workq.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_INF);

/* Prazos dos canais (ms):
 *  - controle: fila do sistema (laco de 10 ms, sensores calibrados,
 *    host BLE, gravacao de parametros no ZMS). 500 ms = 50 ciclos de
 *    folga - acima de qualquer bloqueio legitimo da fila (gravacao no
 *    RRAM e' rapida), abaixo do que deixaria o motor andar "cego".
 *  - sensores: sensor_workq (polling I2C de posicao/torque). Se travar,
 *    a posicao congela no ultimo valor e o controle seguiria acionando o
 *    motor com posicao velha. 2 s cobre timeouts de I2C de sensor ausente.
 */
#define WDT_CONTROLE_MS    500
#define WDT_SENSORES_MS    2000
#define BATIMENTO_SENS_MS  100

/* TESTE DE BANCADA - deixar 0 no firmware normal. Com 1, 15 s apos o boot
 * a fila do sistema e' travada de proposito (laco infinito): o canal
 * "controle" deve estourar em ~500 ms, o motor desligar e a placa
 * reiniciar, logando "Ultimo reset foi do WATCHDOG" no boot seguinte.
 * Teste com o motor andando (mande Abrir antes dos 15 s).
 */
#define WATCHDOG_TESTE 0

static int canal_controle = -1;
static int canal_sensores = -1;
static struct k_work_delayable batimento_sensores;

/* Marca de "reiniciado pelo watchdog", numa RAM que o boot nao zera
 * (sobrevive a reset por software, nao a falta de energia). Necessaria
 * porque canal_estourado() reinicia com sys_reboot(): pro hardware isso e'
 * reset por SOFTWARE (RESETREAS.SREQ), nao por watchdog - so' o WDT31
 * estourando sozinho aparece como RESET_WATCHDOG. Usada pelo registro de
 * eventos (EV_SIS_WDT_RST).
 */
#define MARCA_RESET_WDT 0x57445452UL /* "WDTR" */
static __noinit uint32_t marca_reset;

static enum actuator_regevent_boot causa_boot = ACTUATOR_REGEVENT_BOOT_POWER_UP;

/* Canal estourado (roda no contexto do timer do task_wdt): desliga o
 * motor ANTES de reiniciar - o reset ja' solta os pinos, mas assim o
 * motor para no mesmo instante, sem depender do circuito externo ter
 * pull-down. Com callback, o task_wdt NAO reinicia sozinho - o
 * sys_reboot() abaixo e' obrigatorio.
 */
static void canal_estourado(int canal, void *user_data)
{
	actuator_motor_para();
	marca_reset = MARCA_RESET_WDT;
	LOG_ERR("WATCHDOG: canal '%s' (%d) sem alimentacao - motor desligado, reiniciando",
		(const char *)user_data, canal);
	LOG_PANIC();
	sys_reboot(SYS_REBOOT_COLD);
}

static void batimento_sensores_handler(struct k_work *work)
{
	task_wdt_feed(canal_sensores);
	k_work_reschedule_for_queue(sensor_workq_get(), k_work_delayable_from_work(work),
				    K_MSEC(BATIMENTO_SENS_MS));
}

#if WATCHDOG_TESTE
static void trava_fila_sistema(struct k_work *work)
{
	LOG_WRN("WATCHDOG_TESTE: travando a fila do sistema de proposito");
	for (;;) {
		k_busy_wait(1000);
	}
}
static K_WORK_DELAYABLE_DEFINE(trava_work, trava_fila_sistema);
#endif

/* Classifica o boot pro registro de eventos (atRegPowerReset() do fwBLE):
 *  - marca do canal_estourado() ou RESET_WATCHDOG (WDT31) -> WDT;
 *  - nenhum bit (o RESETREAS do nRF54L nao tem bit de power-on) ou so'
 *    RESET_POR -> energizacao;
 *  - qualquer outro (software, pino, debugger, lockup) -> reset por SW.
 */
static void loga_causa_reset(void)
{
	uint32_t causa = 0;
	bool marca_wdt = (marca_reset == MARCA_RESET_WDT);
	bool causa_ok = (hwinfo_get_reset_cause(&causa) == 0);

	marca_reset = 0;

	if (marca_wdt || (causa_ok && (causa & RESET_WATCHDOG))) {
		causa_boot = ACTUATOR_REGEVENT_BOOT_WDT;
		LOG_WRN("Ultimo reset foi do WATCHDOG (causa 0x%08x%s)", causa,
			marca_wdt ? ", canal do task_wdt" : "");
	} else if (!causa_ok || (causa & ~RESET_POR) == 0) {
		causa_boot = ACTUATOR_REGEVENT_BOOT_POWER_UP;
		LOG_INF("Causa do ultimo reset: 0x%08x (energizacao)", causa);
	} else {
		causa_boot = ACTUATOR_REGEVENT_BOOT_SW;
		LOG_INF("Causa do ultimo reset: 0x%08x", causa);
	}

	if (causa_ok) {
		(void)hwinfo_clear_reset_cause();
	}
}

enum actuator_regevent_boot watchdog_causa_boot(void)
{
	return causa_boot;
}

void watchdog_init(void)
{
	const struct device *hw_wdt = DEVICE_DT_GET_OR_NULL(DT_ALIAS(watchdog0));
	int err;

	loga_causa_reset();

	if (hw_wdt && !device_is_ready(hw_wdt)) {
		LOG_ERR("WDT31 nao esta pronto - task_wdt sem fallback de hardware");
		hw_wdt = NULL;
	}

	err = task_wdt_init(hw_wdt);
	if (err) {
		LOG_ERR("task_wdt_init falhou (err %d) - SEM WATCHDOG", err);
		return;
	}

	canal_controle = task_wdt_add(WDT_CONTROLE_MS, canal_estourado, "controle");
	canal_sensores = task_wdt_add(WDT_SENSORES_MS, canal_estourado, "sensores");
	if (canal_controle < 0 || canal_sensores < 0) {
		LOG_ERR("task_wdt_add falhou (controle=%d sensores=%d)", canal_controle,
			canal_sensores);
		return;
	}

	k_work_init_delayable(&batimento_sensores, batimento_sensores_handler);
	k_work_schedule_for_queue(sensor_workq_get(), &batimento_sensores, K_NO_WAIT);

	LOG_INF("Watchdog ativo: controle %d ms, sensores %d ms, fallback de hardware %s",
		WDT_CONTROLE_MS, WDT_SENSORES_MS, hw_wdt ? "WDT31" : "NENHUM");

#if WATCHDOG_TESTE
	LOG_WRN("WATCHDOG_TESTE ligado - a fila do sistema trava em 15 s");
	k_work_schedule(&trava_work, K_SECONDS(15));
#endif
}

void watchdog_feed_controle(void)
{
	if (canal_controle >= 0) {
		task_wdt_feed(canal_controle);
	}
}
