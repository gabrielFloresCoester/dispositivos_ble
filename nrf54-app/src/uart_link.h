/*
 * Coester - SIM Connect (Gateway)
 *
 * Link UART dedicado com o nRF9151 (uart30) - contrato completo em
 * docs/PROTOCOLO_54_91.md (repositorio principal). Mesmo mecanismo de
 * framing ja provado em exercises/uart-link (SOF+TYPE+SEQ+LEN+PAYLOAD+CRC16,
 * ACK+retry+fila), agora falando a tabela de opcodes 0x10-0x19 que
 * espelha 1:1 as operacoes que a interface web ja faz via BLE contra
 * my_lbs.c/actuator_client.c.
 *
 * Este modulo e' o unico ponto de main.c que precisa saber que existe
 * um segundo transporte (UART) alem da BLE - main.c so chama as 3
 * funcoes uart_link_send_*() abaixo, ao lado das chamadas my_lbs_send_*()
 * que ja existem, exatamente como my_lbs.c e actuator_client.c ja
 * fazem para o transporte BLE.
 */

#ifndef UART_LINK_H_
#define UART_LINK_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/types.h>

/** @brief Inicializa o link UART com o 91 (device uart30, ver overlay
 *         nrf54lm20dk_nrf54lm20b_cpuapp_ns.overlay).
 *
 *  Chame DEPOIS de actuator_manager_init(). Ao contrario de
 *  my_lbs_init()/actuator_manager_init(), falha AQUI NAO e fatal: se o
 *  91 nao estiver plugado na bancada (ou a uart30 nao tiver sido
 *  habilitada corretamente), a interface BLE local continua
 *  funcionando sozinha - so fica registrado em log.
 *
 * @retval 0 Link pronto.
 * @retval -ENODEV Device uart30 nao esta pronto (overlay ausente/errado).
 * @retval <0 Falha ao registrar callback ou habilitar RX.
 */
int uart_link_init(void);

/** @brief Encaminha um registro de status de slot (41 bytes) ao 91 como
 *         frame 0x15 STATUS_RECORD - mesmo registro, byte a byte, que
 *         my_lbs_send_actuator_status() manda por BLE.
 *
 *  Sem efeito se uart_link_init() falhou ou ainda nao rodou.
 */
void uart_link_send_status(const uint8_t *rec, uint16_t len);

/** @brief Encaminha um registro de descoberta (41 bytes) ao 91 como
 *         frame 0x17 DISCOVERED_RECORD - espelha
 *         my_lbs_send_discovery().
 */
void uart_link_send_discovery(const uint8_t *rec, uint16_t len);

/** @brief Encaminha a resposta crua de um atuador ao 91 como frame
 *         0x11 RAW_DATA, payload [slot][bytes crus] - mesmo formato
 *         que my_lbs_send_actuator_raw_data() manda por BLE.
 */
void uart_link_send_raw_data(uint8_t slot, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* UART_LINK_H_ */
