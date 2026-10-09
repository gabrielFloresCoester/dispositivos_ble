/*
 * Camada de calibracao raw->fisico dos sensores de posicao/torque.
 * Ver o comentario grande em actuator_sensors.c para o escopo exato
 * (o que foi replicado do sensPosTor.c original e o que foi
 * deliberadamente deixado de fora).
 */
#ifndef ACTUATOR_SENSORS_H_
#define ACTUATOR_SENSORS_H_

#include <stdint.h>
#include <stdbool.h>

/* Espelha os 5 primeiros campos de sensPosTor_t (ControleCoesterBLE,
 * BLE/Atuador/sensPosTor.h) - exatamente os 10 bytes que a area
 * "Sensor" do ifFerConfig (MSG_EX_ADDRESS_CONFIG_SENSOR = 0x00804080)
 * ja expoe hoje para leitura direta de posicao/torque num Atuador BLE
 * real (ver docs/SENSORES_I2C.md). Sem padding (todos os campos tem
 * 2 bytes) - pronto pra ser serializado little-endian byte a byte
 * quando o servico BLE for implementado.
 */
struct actuator_sensor_data {
	uint16_t ad_posicao;   /* raw ADC filtrado, sensor de posicao */
	int16_t at_posicao;    /* posicao calibrada, 0-1000 (por mil) - ver ACTUATOR_AT_POSICAO_INDEF */
	int16_t ad_torque;     /* raw ADC filtrado, sensor de torque (sem zero subtraido) */
	int16_t nm_torque;     /* torque calibrado (unidade "Nm" do proCo original) */
	int16_t ad_torque_max; /* maximo de ad_torque no movimento atual (zera ao partir) */
};

/* Sentinela de at_posicao pra "sensor de posicao ainda sem leitura
 * valida" (filtro enchendo o buffer no boot, ou sensor fisicamente
 * ausente/com problema) - DIFERENTE de "posicao = 0" (uma leitura
 * valida no limite inferior). Convencao SINTETICA deste projeto (nao
 * existe no fwBLE/Atuador BLE original - la' o sensor de posicao
 * sempre existe e e' sempre valido em operacao normal; so' faz sentido
 * aqui por causa da selecao automatica/bancada - ver actuator_sensors.c).
 * Fora do range fisico legitimo de at_posicao (aprox. -300 a 1300,
 * mesmo com folga de calibracao) - nunca colide com um valor real.
 * `index.html` reconhece este valor e mostra "INDEF" no lugar do
 * percentual.
 */
#define ACTUATOR_AT_POSICAO_INDEF ((int16_t)0x8000)

/* Qual sensor de torque esta ativo agora (selecao automatica em
 * actuator_sensors.c - ver o comentario grande la'). Valores fixos
 * (serializados por BLE - actuator_service.c, area "info" - nao
 * renumerar). Quando != ANALOG, os campos ad_torque/nm_torque acima
 * NAO sao Nm de verdade - ver ACTUATOR_TORQUE_SOURCE_ONOFF.
 */
enum actuator_torque_source {
	ACTUATOR_TORQUE_SOURCE_NONE = 0,   /* nenhum sensor de torque online ainda */
	ACTUATOR_TORQUE_SOURCE_ANALOG = 1, /* ADS1000 (torque_sensor.h) - ad_torque/nm_torque em Nm de verdade */
	ACTUATOR_TORQUE_SOURCE_ONOFF = 2,  /* PCA9536 (torque_onoff_sensor.h) - ad_torque/nm_torque
					    * carregam os 2 bits crus de sobretorque (0-3), nao Nm */
};

/* Agenda o motor de agregacao periodica (le o cache de
 * position_sensor.c/torque_sensor.c, filtra, calibra). Nao acessa o
 * I2C diretamente.
 */
void actuator_sensors_init(void);

/* Ultimo valor calibrado (cache). Retorna false enquanto os filtros de
 * posicao/torque ainda nao encheram o buffer pela primeira vez.
 */
bool actuator_sensors_get(struct actuator_sensor_data *out);

/* Qual fonte de torque esta ativa neste instante - ver enum acima. */
enum actuator_torque_source actuator_sensors_torque_source(void);

#endif /* ACTUATOR_SENSORS_H_ */
