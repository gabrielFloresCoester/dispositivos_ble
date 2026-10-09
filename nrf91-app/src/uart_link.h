/*
 * Coester - SIM Connect (nRF9151)
 *
 * Espelho do link UART com o nRF54 (Gateway) - contrato completo em
 * docs/PROTOCOLO_54_91.md (repositorio principal). Fase 4: so
 * decodifica e loga os registros que chegam do 54 (STATUS_RECORD,
 * DISCOVERED_RECORD, RAW_DATA, MANAGE_RESULT, GATEWAY_CTRL_RESULT) -
 * sem MQTT ainda. O modulo ja nasce separado do main.c mesmo sem
 * estritamente precisar agora, porque a fase de MQTT (proxima depois
 * desta) vai precisar chamar estas mesmas funcoes de fora.
 */

#ifndef UART_LINK_H_
#define UART_LINK_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/types.h>

/** @brief Inicializa o link UART com o 54 (device uart1/arduino_serial).
 *
 * @retval 0 Link pronto.
 * @retval <0 Falha (device nao pronto, callback ou RX).
 */
int uart_link_init(void);

/** @brief Gatilho de teste (TEMPORARIO - ver docs/PROTOCOLO_54_91.md,
 *         secao "Decisoes tomadas"): pede GET_STATUS de cada slot
 *         (0..GATEWAY_MAX_ACTUATORS-1) em sequencia, um de cada vez -
 *         evita ter que fragmentar a leitura em lote (mesma logica que
 *         a interface web ja usa contra o Gateway via BLE).
 *
 *  Quando a integracao MQTT existir, o gatilho principal passa a ser
 *  "ao conectar no broker", com isto sobrando como forma manual de
 *  forcar resync.
 */
void uart_link_get_status_all(void);

/** @brief Gatilho de teste (TEMPORARIO): pede GET_DISCOVERED de todos
 *         os atuadores vistos no scan do Gateway (indice 0xFF).
 */
void uart_link_get_discovered_all(void);

/** @brief Pede GET_STATUS de um unico slot (0x14) - usado pelo
 *         comando MQTT `{"op":"get_status","slot":N}`, ver
 *         docs/PROTOCOLO_91_MQTT.md.
 */
void uart_link_get_status(uint8_t slot);

/** @brief Repassa um comando cru do proCo pro atuador de um slot
 *         (ACTUATOR_CMD, 0x10) - e' o mecanismo de "dado sob demanda"
 *         (curva, evento, etc.) pedido via MQTT
 *         (`{"op":"actuator_cmd","slot":N,"data":"<hex>"}`). A
 *         resposta do atuador chega depois como `RAW_DATA`, publicada
 *         por `mqtt_client_publish_raw()`.
 */
void uart_link_send_actuator_cmd(uint8_t slot, const uint8_t *data, uint8_t len);

/** @brief Adiciona/remove um atuador da allow-list do Gateway
 *         (ACTUATOR_MANAGE, 0x12).
 *
 * @param mac_human Endereco no formato humano (AA:BB:CC:DD:EE:FF,
 *                   indice 0 = primeiro byte) - a inversao pra ordem
 *                   interna do stack que o frame exige e' feita aqui
 *                   dentro, mesma logica de `format_mac()`.
 */
void uart_link_send_actuator_manage(uint8_t cmd, uint8_t addr_type, const uint8_t mac_human[6]);

/** @brief Comando administrativo do Gateway (GATEWAY_CTRL, 0x18) -
 *         limpar descoberta, reiniciar scan, desconectar slot, mudar
 *         velocidade. Ver tabela de `cmd` em docs/PROTOCOLO_54_91.md.
 *         `arg`/`arg_len` podem ser `NULL`/`0` pros comandos que nao
 *         precisam de argumento.
 */
void uart_link_send_gateway_ctrl(uint8_t cmd, const uint8_t *arg, uint8_t arg_len);

#ifdef __cplusplus
}
#endif

#endif /* UART_LINK_H_ */
