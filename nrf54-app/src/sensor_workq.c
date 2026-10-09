/*
 * Fila de trabalho dedicada aos pollers I2C dos sensores.
 *
 * BUG CORRIGIDO (2026-08-26): os 3 pollers de sensor (posicao, torque
 * analogico - ads1000.c - e torque ON/OFF - torque_onoff_sensor.c)
 * rodavam, ate' aqui, na fila de trabalho DO SISTEMA (k_sys_work_q, o
 * destino default de k_work_schedule()), cada um fazendo E/S I2C
 * BLOQUEANTE a cada 15ms.
 *
 * Isso e' um antipadrao conhecido do Zephyr: a fila do sistema roda
 * numa prioridade tipicamente COOPERATIVA
 * (CONFIG_SYSTEM_WORKQUEUE_PRIORITY, negativa por padrao) - enquanto
 * ela estiver executando um work item, NENHUMA thread preemptivel
 * (main(), por exemplo) consegue rodar, e o host Bluetooth do Zephyr
 * tambem depende dela pra varias coisas, incluindo a propria
 * advertising deste projeto (adv_work em main.c).
 *
 * Em bancada, com sensores parcialmente conectados/desconectados
 * durante o teste (cenario normal ao validar a deteccao automatica de
 * fonte de torque - ver docs/SENSORES_I2C.md), uma transacao I2C sem
 * ACK pode disparar recuperacao de barramento
 * (i2c_recover_bus()/bit-banging manual de SCL/SDA) DENTRO da fila do
 * sistema - um bloqueio de dezenas de ms sem ceder a CPU. Sintoma
 * reportado pelo Felipe (2026-08-26): comportamento erratico - as
 * vezes trava, as vezes a interface nao conecta (advertising
 * atrasada), as vezes conecta mas nao atualiza (host Bluetooth
 * atrasado), as vezes nem loga (main() e' preemptivel, nao consegue
 * rodar enquanto a fila do sistema estiver presa).
 *
 * Correcao: os pollers I2C passam a rodar numa fila PROPRIA, com sua
 * propria thread, numa prioridade preemptivel normal - isola qualquer
 * lentidao/recuperacao de barramento I2C do resto do sistema (BLE,
 * main()). actuator_sensors.c continua na fila do sistema de
 * proposito: so' le caches (nunca acessa o I2C diretamente), e' rapido
 * o suficiente pra nao incomodar ninguem.
 */

#include "sensor_workq.h"

#define SENSOR_WORKQ_STACK_SIZE 1024
#define SENSOR_WORKQ_PRIORITY   5 /* preemptivel normal - NAO cooperativa como a fila do sistema */

static struct k_work_q sensor_workq;
static K_THREAD_STACK_DEFINE(sensor_workq_stack, SENSOR_WORKQ_STACK_SIZE);

void sensor_workq_init(void)
{
	struct k_work_queue_config cfg = {
		.name = "sensor_i2c",
		.no_yield = false,
	};

	k_work_queue_init(&sensor_workq);
	k_work_queue_start(&sensor_workq, sensor_workq_stack,
			    K_THREAD_STACK_SIZEOF(sensor_workq_stack), SENSOR_WORKQ_PRIORITY, &cfg);
}

struct k_work_q *sensor_workq_get(void)
{
	return &sensor_workq;
}
