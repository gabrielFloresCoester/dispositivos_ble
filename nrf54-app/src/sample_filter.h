/*
 * Filtro de media aparada (descarta o maximo e o minimo, tira a media
 * do resto) - mesma logica de sptPegAd() em BLE/Atuador/sensPosTor.c
 * (ControleCoesterBLE), usada la' tanto pro sensor de posicao quanto
 * pro de torque (BUF_ADC = 10 amostras).
 */
#ifndef SAMPLE_FILTER_H_
#define SAMPLE_FILTER_H_

#include <stdint.h>
#include <stdbool.h>

/* BUF_ADC no sensPosTor.c original */
#define SAMPLE_FILTER_LEN 10

struct sample_filter {
	int32_t samples[SAMPLE_FILTER_LEN];
	uint8_t next;
	uint8_t filled; /* quantas posicoes do buffer ja foram escritas, ate' SAMPLE_FILTER_LEN */
};

/* Empilha uma nova amostra (sobrescreve a mais antiga, buffer circular) */
void sample_filter_push(struct sample_filter *filter, int32_t value);

/* Zera o buffer (equivalente a nunca ter enchido) - usar quando a fonte
 * de amostras ficar offline (device_health), pra sample_filter_avg()
 * voltar a reportar "nao pronto" em vez de continuar calculando a
 * media de amostras antigas/paradas indefinidamente. Bug corrigido
 * 2026-08-26 (ver actuator_sensors.c): sem isto, o filtro reportava
 * "pronto" pra sempre depois da primeira vez que enchesse, mesmo com o
 * sensor fisicamente desconectado ha muito tempo - o valor calibrado
 * ficava "preso" no ultimo bom em vez de virar indefinido.
 */
void sample_filter_reset(struct sample_filter *filter);

/* Media aparada (descarta max e min, media dos 8 restantes). Retorna
 * false enquanto o buffer nao encheu pela primeira vez - mesmo espirito
 * do warm-up (adPosContIni/adTrqContIni) do sensPosTor.c original,
 * simplificado (la' o warm-up e' 3x mais longo que o buffer, aqui e'
 * 1x - preciso o bastante pra nao reportar lixo residual dos zeros
 * iniciais, sem exigir a mesma folga extra).
 */
bool sample_filter_avg(struct sample_filter *filter, int32_t *avg_out);

#endif /* SAMPLE_FILTER_H_ */
