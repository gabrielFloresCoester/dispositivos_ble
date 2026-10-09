/*
 * Watchdog do nrf54-app - task_wdt do Zephyr com o WDT31 (unico WDT
 * acessivel do lado non-secure no nRF54LM20 com TF-M; o WDT30 e' so'
 * secure) como fallback de hardware.
 *
 * Motivo (pendencia "Watchdog" do README, antes de campo/feira): o motor
 * e' acionado direto por GPIO. Se o firmware travar com um pino ligado,
 * nada desliga o motor. Com o watchdog:
 *   - cada tarefa critica tem um canal com prazo (ver watchdog.c);
 *   - canal estourado -> desliga o motor e reinicia a placa;
 *   - se o proprio task_wdt parar (ex.: interrupcoes travadas), o WDT31
 *     reseta sozinho em ~120 ms (CONFIG_TASK_WDT_MIN_TIMEOUT +
 *     CONFIG_TASK_WDT_HW_FALLBACK_DELAY).
 * O WDT pausa com a CPU parada pelo debugger (WDT_OPT_PAUSE_HALTED_BY_DBG,
 * dentro do task_wdt) - depurar com breakpoint nao reseta a placa.
 */
#ifndef WATCHDOG_H_
#define WATCHDOG_H_

#include "actuator_regevent.h"

/* Sobe o task_wdt + WDT31, loga se o ultimo reset foi do watchdog e cria
 * os canais. Chamar cedo no main(), DEPOIS de sensor_workq_init() (o
 * canal de sensores agenda um batimento nessa fila) e ANTES de
 * actuator_control_init() (o laco de controle alimenta o canal dele).
 * Nao fatal: sem watchdog o firmware roda, mas loga erro alto.
 */
void watchdog_init(void);

/* Alimenta o canal do laco de controle - chamado a cada ciclo de
 * actuator_control_run() (10 ms).
 */
void watchdog_feed_controle(void);

/* Causa do boot atual, classificada em watchdog_init() (antes dele,
 * sempre energizacao) - o registro de eventos grava como 1o evento.
 */
enum actuator_regevent_boot watchdog_causa_boot(void);

#endif /* WATCHDOG_H_ */
