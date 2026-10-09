/*
 * Coester - SIM Connect (Gateway)
 *
 * Actuator Manager: gerencia ate MAX_ACTUATORS conexoes Central
 * simultaneas com atuadores REAIS (produto Coester, nRF52832,
 * protocolo proCo/ifFerConfig sobre servico proprietario estilo NUS).
 *
 * Sem NENHUM parsing do protocolo proCo aqui.
 *
 * --- Escala e o teto de 20 conexoes ---
 *
 * O SoftDevice Controller da Nordic suporta no maximo 20 conexoes
 * simultaneas somando TODOS os papeis. A conta do Gateway:
 *
 *     20 total
 *      -1 interface (navegador/celular, papel Peripheral)
 *     ----
 *      19 atuadores reais
 *
 * Os Kits B e C do exercicio original da Nordic foram aposentados
 * justamente por consumirem duas dessas conexoes sem fazer parte do
 * produto. Passar de 20 exigiria trocar para o controlador do Zephyr
 * (CONFIG_BT_LL_SW_SPLIT, qualificado ate 64), mas ai o gargalo deixa
 * de ser software e vira agenda de radio.
 *
 * Com 19 + 1 interface o teto fica exatamente cheio. Se um atuador cair
 * e voltar a anunciar antes de o objeto de conexao antigo ser reciclado,
 * a tentativa de reconexao falha com -ENOMEM e o Gateway tenta de novo na
 * varredura seguinte - atraso de alguns segundos, sem consequencia.
 *
 * --- Intervalo de conexao: lento por padrao, rapido sob demanda ---
 *
 * IMPORTANTE: o enlace NAO morre por falta de trafego da aplicacao. O
 * controlador troca pacotes vazios a cada intervalo de conexao
 * automaticamente; o supervision timeout so expira quando eventos
 * consecutivos sao PERDIDOS (fora de alcance, interferencia). Fazer
 * polling a cada 5 s ou a cada 5 min e indiferente para a sobrevivencia
 * do link - quem o mantem vivo e o intervalo de conexao, nao o
 * setInterval da interface.
 *
 * O que o intervalo realmente controla e o trade-off:
 *
 *   intervalo curto (15 ms)  -> round trip ~30 ms, poucos links cabem
 *   intervalo longo (200 ms) -> round trip ~400 ms, muitos links cabem
 *
 * Polling periodico de 16 atuadores quer intervalo longo. Baixar uma
 * curva de 200 registros quer intervalo curto (a 400 ms por round trip
 * e 2 round trips por registro, seriam ~3 minutos por atuador).
 *
 * A saida e nao escolher: todo link fica LENTO por padrao, e a
 * interface pede modo RAPIDO no slot especifico antes de uma coleta em
 * lote, voltando ao lento no fim.
 */

#ifndef ACTUATOR_CLIENT_H_
#define ACTUATOR_CLIENT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/types.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <bluetooth/gatt_dm.h>

/* UUIDs do protocolo proprietario Coester - identicas as usadas em
 * coleta_ble/ble/protocol.py. Se o produto mudar, atualize aqui E la.
 */
#define BT_UUID_ACTUATOR_SERVICE_VAL \
	BT_UUID_128_ENCODE(0x06290001, 0x0538, 0x408d, 0x8359, 0xe83d95ae1d60)
#define BT_UUID_ACTUATOR_RX_VAL \
	BT_UUID_128_ENCODE(0x06290002, 0x0538, 0x408d, 0x8359, 0xe83d95ae1d60)
#define BT_UUID_ACTUATOR_TX_VAL \
	BT_UUID_128_ENCODE(0x06290003, 0x0538, 0x408d, 0x8359, 0xe83d95ae1d60)

#define BT_UUID_ACTUATOR_SERVICE BT_UUID_DECLARE_128(BT_UUID_ACTUATOR_SERVICE_VAL)
#define BT_UUID_ACTUATOR_RX      BT_UUID_DECLARE_128(BT_UUID_ACTUATOR_RX_VAL)
#define BT_UUID_ACTUATOR_TX      BT_UUID_DECLARE_128(BT_UUID_ACTUATOR_TX_VAL)

/* Ver a nota sobre o teto de 20 conexoes no cabecalho. Se subir isto,
 * suba CONFIG_BT_MAX_CONN junto - e nao passe de
 * 20 - (1 interface + kits de teste em uso).
 */
#define MAX_ACTUATORS 19

#define MAX_DISCOVERED 12

/* --- Formato de bytes exposto a interface web ---
 *
 * O "nome" aqui é o mesmo campo que o coleta_ble mostra como TAG do
 * atuador (scanner.py usa device.name — o Local Name do advertisement
 * BLE). O Gateway agora captura esse mesmo campo, para a interface web
 * poder identificar o atuador por nome, não só por endereço.
 *
 * Registro de STATUS (41 bytes), identico no read e no notify:
 *   [0] slot  [1] state  [2] addr_type  [3..8] MAC (invertido)
 *   [9..40] nome (32 bytes, UTF-8, preenchido com zeros a direita -
 *           trate como campo de tamanho fixo, corte no primeiro 0x00)
 *
 * Registro de DESCOBERTA (41 bytes):
 *   [0] addr_type  [1..6] MAC  [7] rssi (int8)  [8] flags
 *   [9..40] nome (mesmo formato acima)
 *
 * 32 bytes de nome porque um advertisement BLE legacy tem 31 bytes de
 * payload e cada campo gasta 2 no cabeçalho - ou seja, o maior Local
 * Name possivel tem 29 caracteres. 32 cobre isso com folga e mantem o
 * registro alinhado.
 */
#define ACTUATOR_NAME_MAX_LEN      32
#define ACTUATOR_STATUS_REC_LEN    (9 + ACTUATOR_NAME_MAX_LEN)
#define ACTUATOR_DISCOVERY_REC_LEN (9 + ACTUATOR_NAME_MAX_LEN)

#define ACTUATOR_DISCOVERY_FLAG_KNOWN BIT(0)

/** @brief Estado de um slot, do ponto de vista da interface. */
enum actuator_state {
	ACTUATOR_STATE_EMPTY = 0,     /* slot livre                             */
	ACTUATOR_STATE_WAITING = 1,   /* na allow-list, aguardando o scan achar */
	ACTUATOR_STATE_CONNECTED = 2, /* conectado, discovery em andamento      */
	ACTUATOR_STATE_READY = 3,     /* handles ok + TX assinado - pode operar */
	ACTUATOR_STATE_ERROR = 4,     /* conectou mas discovery falhou          */
};

/** @brief Perfil de velocidade do enlace com um atuador. */
enum actuator_speed {
	ACTUATOR_SPEED_SLOW = 0, /* padrao: 120-200 ms, muitos links cabem */
	ACTUATOR_SPEED_FAST = 1, /* coleta em lote: 15-30 ms, um por vez   */
};

typedef void (*actuator_raw_data_cb_t)(uint8_t slot, const uint8_t *data, uint16_t len);
typedef void (*actuator_status_cb_t)(const uint8_t *rec, uint16_t len);
typedef void (*actuator_discovery_cb_t)(const uint8_t *rec, uint16_t len);

struct actuator_manager_cb {
	actuator_raw_data_cb_t raw_data_cb;
	actuator_status_cb_t status_cb;
	actuator_discovery_cb_t discovery_cb;
};

/** @brief Inicializa o modulo. Chame ANTES de settings_load(). */
int actuator_manager_init(const struct actuator_manager_cb *cb);

int actuator_manager_add(const bt_addr_le_t *addr);
int actuator_manager_remove(const bt_addr_le_t *addr);
bool actuator_manager_is_wanted(const bt_addr_le_t *addr);

int actuator_manager_start(struct bt_conn *conn, const bt_addr_le_t *addr);
bool actuator_manager_owns_conn(struct bt_conn *conn);
void actuator_manager_conn_lost(struct bt_conn *conn);

int actuator_manager_send_to(uint8_t slot, const uint8_t *data, uint16_t len);
int actuator_manager_disconnect(uint8_t slot);

/** @brief Quantos slots têm uma conexão BLE ativa agora (qualquer
 *         estado - conectando, discovery em andamento, ou pronto),
 *         não só os PRONTOS.
 *
 *  Existe para diagnóstico: quando a advertising falha por falta de
 *  recurso no controlador (status HCI 0x0D / -ENOMEM), logar esse
 *  número junto ajuda a distinguir "genuinamente muitas conexões
 *  disputando recurso" de "algo mais específico" (ver a discussão em
 *  PROTOCOLO_INTERFACE.md sobre a trava transitória de ~34s observada
 *  em campo, cuja causa exata no controlador ainda não foi confirmada).
 */
uint8_t actuator_manager_count_connected(void);

/** @brief Quantos slots estao PRONTOS (READY) agora.
 *
 *  Existe para o LED CON_STATUS_LED_ACTUATOR do main.c: ele deve ficar
 *  aceso enquanto houver ao menos um atuador pronto, e apagar assim que
 *  o ultimo cair - o que exige recontar do zero a cada mudanca de
 *  estado, nao so ligar quando ALGUM fica pronto (senao o LED nunca
 *  apaga de volta).
 */
uint8_t actuator_manager_count_ready(void);

/* Modo rapido reverte sozinho apos este tempo. Sem isso, uma aba de
 * navegador fechada no meio de um download deixaria aquele link
 * consumindo radio indefinidamente, degradando todos os outros.
 */
#define ACTUATOR_FAST_MODE_TIMEOUT_S 180

/** @brief Coloca o enlace de um slot em modo rapido ou lento.
 *
 * @retval 0 Pedido agendado.
 * @retval -EINVAL Slot fora do intervalo.
 * @retval -ENOTCONN Slot nao conectado.
 */
int actuator_manager_set_speed(uint8_t slot, enum actuator_speed speed);

uint16_t actuator_manager_get_status_all(uint8_t *buf, uint16_t buf_len);
uint16_t actuator_manager_get_discovery_all(uint8_t *buf, uint16_t buf_len);

/** @brief Registra que um atuador foi visto no scan (endereço, RSSI e
 *         o Local Name do advertisement, se houver). Chamado para TODO
 *         atuador visto, esteja ele na allow-list ou não — alimenta o
 *         cache de descoberta e, se o endereço já for de um slot
 *         conhecido, também atualiza o nome cacheado nele.
 *
 * @param name Nome extraído do advertisement, ou string vazia/NULL se
 *             o anúncio não trouxer nome.
 */
void actuator_manager_seen(const bt_addr_le_t *addr, int8_t rssi, const char *name);

/** @brief Atualiza SÓ o nome de um endereço já conhecido (cache de
 *         descoberta e/ou slot), sem RSSI e sem criar entrada nova.
 *
 *  Existe por causa de uma particularidade do BLE: em muitos produtos,
 *  o Local Name vem no pacote de SCAN RESPONSE, não no advertisement
 *  principal - e o advertisement principal é o único que passa pelo
 *  filtro de UUID usado em actuator_manager_seen(). O bleak (Python)
 *  resolve isso escondido, porque o SO já mescla os dois pacotes antes
 *  da aplicação ver; aqui no Gateway isso tem que ser feito à mão, com um
 *  segundo callback de scan que vê TODO pacote recebido (ver
 *  bt_le_scan_cb_register no main.c). Por isso, deliberadamente, esta
 *  função só atualiza endereços que JÁ existem no cache ou numa slot -
 *  nunca cria uma entrada nova a partir de um pacote sem UUID, senão
 *  qualquer aparelho Bluetooth por perto viraria "descoberta".
 */
void actuator_manager_name_seen(const bt_addr_le_t *addr, const char *name);
void actuator_manager_clear_discovery(void);

#ifdef __cplusplus
}
#endif

#endif /* ACTUATOR_CLIENT_H_ */
