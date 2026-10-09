/*
 * Rastreio de saude por dispositivo - equivalente a
 * comScanDevFail()/comScanSuccess()/comScanOnlineDev()/
 * comScanDevFailRate() em BLE/proCo/comScan.c (ControleCoesterBLE).
 *
 * Modulo pequeno e reutilizavel: qualquer coisa que fale com um
 * dispositivo I2C (ads1000.c hoje; futuros IO Digital/Analogica/Rede/
 * RTC/etc., e a deteccao automatica de variante de sensor) mantem um
 * struct device_health e chama device_health_record_success()/
 * _record_failure() a cada tentativa de comunicacao, e pergunta
 * device_health_is_online()/_failure_rate() quando precisar saber o
 * estado.
 *
 * Diferenca deliberada do original: aqui, toda falha incrementa o
 * contador de falhas consecutivas (reset a zero em qualquer sucesso).
 * O comScan.c original tem uma peculiaridade onde a PRIMEIRA falha
 * apos um sucesso reseta esse contador em vez de incrementar (so'
 * conta como "consecutiva" a partir da segunda falha seguida) - nao
 * replicado aqui de proposito: e' um detalhe de contabilidade interna,
 * nao faz parte de nenhum protocolo de fio que precise bater byte a
 * byte, e o comportamento direto (toda falha conta) e' mais facil de
 * raciocinar sobre.
 */
#ifndef DEVICE_HEALTH_H_
#define DEVICE_HEALTH_H_

#include <stdint.h>
#include <stdbool.h>

/* MAX_COUNT_FAILURE_SEQ no comScan.c original - depois de tantas
 * falhas seguidas, o dispositivo e' considerado offline.
 */
#define DEVICE_HEALTH_MAX_FAILURE_SEQUENT 10

/* MAX_COUNT_FAILURE / MAX_COUNT_SUCCESS no comScan.c original (mesma
 * constante nos dois) - tamanho da janela de transacoes usada pra
 * calcular a taxa de falha.
 */
#define DEVICE_HEALTH_RATE_WINDOW 9999

struct device_health {
	uint16_t count_failure;         /* falhas na janela atual */
	uint16_t count_failure_sequent; /* falhas consecutivas - zera em qualquer sucesso */
	uint16_t count_transaction;     /* transacoes (sucesso+falha) na janela atual */
	uint16_t failure_rate;          /* congelado no fim de cada janela - comScanDevFailRate() */
	bool ever_succeeded;            /* false = equivalente a CDSC_WAITING, nunca teve 1 sucesso */
};

/* Zera o struct pro estado inicial (equivalente a CDSC_WAITING) */
void device_health_reset(struct device_health *h);

void device_health_record_success(struct device_health *h);
void device_health_record_failure(struct device_health *h);

/* false se: nunca teve sucesso ainda, OU esta na sequencia maxima de
 * falhas consecutivas (DEVICE_HEALTH_MAX_FAILURE_SEQUENT) - mesma
 * logica de comScanOnlineDev().
 */
bool device_health_is_online(const struct device_health *h);

/* Taxa de falha da ultima janela COMPLETA (0..DEVICE_HEALTH_RATE_WINDOW
 * falhas por DEVICE_HEALTH_RATE_WINDOW transacoes) - comScanDevFailRate().
 * So' atualiza no fim de cada janela; entre janelas, reflete a ultima
 * janela fechada, nao a parcial em andamento (mesmo comportamento do
 * original).
 */
uint16_t device_health_failure_rate(const struct device_health *h);

/* Contadores crus da janela ATUAL (nao esperam o fim da janela como
 * device_health_failure_rate() acima) - pensados para diagnostico
 * (confirmar que o poller de um dispositivo esta' de fato rodando e
 * tentando transacoes, distinguindo "nunca tentou" de "tenta e sempre
 * falha").
 */
uint16_t device_health_count_transaction(const struct device_health *h);
uint16_t device_health_count_failure_sequent(const struct device_health *h);

#endif /* DEVICE_HEALTH_H_ */
