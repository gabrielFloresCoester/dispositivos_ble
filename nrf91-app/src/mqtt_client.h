/*
 * Coester - SIM Connect (nRF9151)
 *
 * Integracao MQTT - contrato completo em docs/PROTOCOLO_91_MQTT.md.
 * Base reaproveitada de C:\NordicAcademy\cell-fund\l4\l4_e2 (Nordic
 * Academy, Cellular IoT Fundamentals, Licao 4 Exercicio 2).
 */

#ifndef MQTT_CLIENT_H_
#define MQTT_CLIENT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/types.h>

/** @brief Configura o modem, conecta na rede LTE (bloqueia ate
 *         registrar - pode demorar em campo real), provisiona o
 *         certificado TLS e inicia a conexao com o broker MQTT
 *         (assincrona a partir dai - eventos chegam via callback).
 *
 * @retval 0 sequencia de setup concluida sem erro imediato.
 * @retval <0 falha em algum passo (modem, LTE, certificado ou MQTT).
 */
int mqtt_client_start(void);

/** @brief Publica o registro de status de um slot (retained) -
 *         topico `<prefixo>/actuator/<slot>/status`. Sem efeito se
 *         nao estiver conectado ao broker.
 */
void mqtt_client_publish_status(uint8_t slot, const char *state, uint8_t addr_type,
				 const char *mac, const char *name);

/** @brief Publica um registro de descoberta (retained) - topico
 *         `<prefixo>/discovery/<mac_sem_dois_pontos>`.
 */
void mqtt_client_publish_discovery(uint8_t addr_type, const char *mac, int8_t rssi, uint8_t flags,
				    const char *name);

/** @brief Publica a resposta crua de um atuador (RAW_DATA), hex-
 *         encoded, nao retained - topico `<prefixo>/actuator/<slot>/raw`.
 *         E' o canal por onde volta o dado sob demanda pedido via
 *         `actuator_cmd` (curva, evento, etc.).
 */
void mqtt_client_publish_raw(uint8_t slot, const uint8_t *data, uint8_t len);

/** @brief Publica o resultado de um comando administrativo
 *         (MANAGE_RESULT/GATEWAY_CTRL_RESULT), nao retained - topico
 *         `<prefixo>/result`.
 *
 * @param op "actuator_manage" ou "gateway_ctrl".
 */
void mqtt_client_publish_result(const char *op, uint8_t seq_original, uint8_t err);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H_ */
