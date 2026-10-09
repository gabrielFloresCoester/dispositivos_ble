/*
 * Fila de trabalho dedicada aos pollers I2C dos sensores (ads1000.c -
 * posicao/torque analogico - e torque_onoff_sensor.c) - ver
 * sensor_workq.c para o motivo (isolar E/S I2C bloqueante da fila do
 * sistema, que o host Bluetooth e o main() tambem usam).
 */
#ifndef SENSOR_WORKQ_H_
#define SENSOR_WORKQ_H_

#include <zephyr/kernel.h>

/* Sobe a thread da fila. Chamar uma unica vez, ANTES de
 * position_sensor_init()/torque_sensor_init()/torque_onoff_sensor_init()
 * (main.c) - esses ja agendam trabalho nela.
 */
void sensor_workq_init(void);

/* Fila pronta para k_work_schedule_for_queue()/k_work_reschedule_for_queue(). */
struct k_work_q *sensor_workq_get(void);

#endif /* SENSOR_WORKQ_H_ */
