/*
 * Coester - SIM Connect (nRF9151)
 *
 * Integracao MQTT - contrato completo em docs/PROTOCOLO_91_MQTT.md.
 * Base reaproveitada quase 1:1 de C:\NordicAcademy\cell-fund\l4\l4_e2
 * (Nordic Academy, Cellular IoT Fundamentals, Licao 4 Exercicio 2):
 * `modem_configure()`, `client_id_get()`, `certificate_provision()`,
 * formato dos callbacks e o helper `publish()`.
 *
 * Payloads sao JSON simples, montados/lidos por busca textual direta
 * (snprintk pra montar, strstr/strtol pra ler) em vez de uma
 * biblioteca de JSON - as mensagens tem forma fixa e pequena (ver
 * tabelas no protocolo), nao vale a complexidade extra por enquanto.
 *
 * Comandos que chegam do topico de assinatura (`.../cmd`) NAO chamam
 * uart_link_* direto no callback: o callback da mqtt_helper roda numa
 * thread da biblioteca, diferente da system workqueue que os botoes
 * (main.c) ja usam pra chamar as mesmas funcoes - chamar dos dois
 * lugares sem coordenacao seria uma corrida real nas variaveis
 * estaticas do uart_link.c (fila de saida, tx_busy, etc.). Por isso
 * todo comando (vindo de fora ou gerado aqui dentro, tipo a
 * sincronizacao inicial) passa por uma fila (`k_msgq`, thread-safe) +
 * um k_work que roda na system workqueue - a MESMA thread que os
 * botoes usam, entao nunca colide.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/logging/log.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <modem/lte_lc.h>
#include <modem/modem_key_mgmt.h>
#include <net/mqtt_helper.h>

#include "uart_link.h"
#include "mqtt_client.h"

LOG_MODULE_REGISTER(mqtt_client, LOG_LEVEL_INF);

#define IMEI_LEN             15
#define CGSN_RESPONSE_LENGTH (IMEI_LEN + 6 + 1) /* \r\nOK\r\n + \0 */
#define CLIENT_ID_LEN         (sizeof("nrf-") + IMEI_LEN)

#define SUBSCRIBE_TOPIC_ID 1234

static K_SEM_DEFINE(lte_connected, 0, 1);
static uint8_t client_id[CLIENT_ID_LEN];
static bool mqtt_connected;

static char cmd_topic[sizeof(CONFIG_SIMCONNECT_MQTT_TOPIC_PREFIX) + sizeof("/cmd")];

static const unsigned char ca_certificate[] = {
#include "ca-cert.pem"
};

/* --- Fila de comandos (ver nota grande no topo do arquivo) --- */

#define CMD_JSON_MAX_LEN 320

struct pending_cmd {
	char json[CMD_JSON_MAX_LEN];
};
K_MSGQ_DEFINE(cmd_msgq, sizeof(struct pending_cmd), 4, 4);
static struct k_work cmd_work;

/* --- Helpers de JSON "na unha" (ver nota grande no topo) --- */

static bool json_get_str(const char *buf, const char *key, char *out, size_t out_len)
{
	char pattern[32];
	const char *p;
	const char *start;
	size_t n;

	snprintk(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(buf, pattern);
	if (!p) {
		return false;
	}
	p = strchr(p + strlen(pattern), ':');
	if (!p) {
		return false;
	}
	p++;
	while (*p == ' ') {
		p++;
	}
	if (*p != '"') {
		return false;
	}
	p++;
	start = p;
	while (*p && *p != '"') {
		p++;
	}
	if (*p != '"') {
		return false;
	}
	n = (size_t)(p - start);
	if (n >= out_len) {
		n = out_len - 1;
	}
	memcpy(out, start, n);
	out[n] = '\0';
	return true;
}

static bool json_get_int(const char *buf, const char *key, long *out)
{
	char pattern[32];
	const char *p;
	char *end;

	snprintk(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(buf, pattern);
	if (!p) {
		return false;
	}
	p = strchr(p + strlen(pattern), ':');
	if (!p) {
		return false;
	}
	p++;
	while (*p == ' ') {
		p++;
	}
	*out = strtol(p, &end, 10);
	return end != p;
}

/* "AA:BB:CC:DD:EE:FF" -> 6 bytes, ordem humana (indice 0 = primeiro
 * byte impresso) - a inversao pra ordem interna do stack fica por
 * conta de uart_link_send_actuator_manage(). */
static bool parse_mac(const char *mac_str, uint8_t mac[6])
{
	const char *p = mac_str;

	for (int i = 0; i < 6; i++) {
		char byte_str[3];
		char *end;

		if (!p[0] || !p[1]) {
			return false;
		}
		byte_str[0] = p[0];
		byte_str[1] = p[1];
		byte_str[2] = '\0';
		mac[i] = (uint8_t)strtol(byte_str, &end, 16);
		if (end != &byte_str[2]) {
			return false;
		}
		p += 2;
		if (i < 5) {
			if (*p != ':') {
				return false;
			}
			p++;
		}
	}
	return true;
}

/* "0a1b2c" -> bytes. Retorna a quantidade de bytes decodificados, ou
 * -1 se a string for invalida (tamanho impar, caractere nao-hex) ou
 * nao couber em out_cap. */
static int hex_decode(const char *hex, uint8_t *out, size_t out_cap)
{
	size_t hex_len = strlen(hex);
	size_t n;

	if (hex_len % 2 != 0) {
		return -1;
	}
	n = hex_len / 2;
	if (n > out_cap) {
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		char byte_str[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
		char *end;

		out[i] = (uint8_t)strtol(byte_str, &end, 16);
		if (end != &byte_str[2]) {
			return -1;
		}
	}
	return (int)n;
}

static void hex_encode(const uint8_t *data, uint8_t len, char *out, size_t out_cap)
{
	size_t n = 0;

	for (uint8_t i = 0; i < len && n + 2 < out_cap; i++) {
		n += snprintk(&out[n], out_cap - n, "%02x", data[i]);
	}
	out[n] = '\0';
}

/* --- Dispatch dos comandos (roda na system workqueue, ver nota do topo) --- */

#define MAX_ACTUATOR_CMD_LEN 64
#define MAX_GATEWAY_CTRL_ARG_LEN 16

static void handle_command(const char *json)
{
	char op[24];

	if (!json_get_str(json, "op", op, sizeof(op))) {
		LOG_WRN("Comando MQTT sem \"op\": %s", json);
		return;
	}

	if (strcmp(op, "get_status_all") == 0) {
		uart_link_get_status_all();
	} else if (strcmp(op, "get_status") == 0) {
		long slot;

		if (!json_get_int(json, "slot", &slot)) {
			LOG_WRN("get_status sem \"slot\" valido");
			return;
		}
		uart_link_get_status((uint8_t)slot);
	} else if (strcmp(op, "get_discovered") == 0) {
		uart_link_get_discovered_all();
	} else if (strcmp(op, "actuator_cmd") == 0) {
		long slot;
		char hex[2 * MAX_ACTUATOR_CMD_LEN + 1];
		uint8_t data[MAX_ACTUATOR_CMD_LEN];
		int dlen;

		if (!json_get_int(json, "slot", &slot) ||
		    !json_get_str(json, "data", hex, sizeof(hex))) {
			LOG_WRN("actuator_cmd sem \"slot\"/\"data\" validos");
			return;
		}
		dlen = hex_decode(hex, data, sizeof(data));
		if (dlen < 0) {
			LOG_WRN("actuator_cmd: \"data\" nao e' hex valido");
			return;
		}
		uart_link_send_actuator_cmd((uint8_t)slot, data, (uint8_t)dlen);
	} else if (strcmp(op, "actuator_manage") == 0) {
		long cmd, addr_type;
		char mac_str[18];
		uint8_t mac[6];

		if (!json_get_int(json, "cmd", &cmd) ||
		    !json_get_int(json, "addr_type", &addr_type) ||
		    !json_get_str(json, "mac", mac_str, sizeof(mac_str)) ||
		    !parse_mac(mac_str, mac)) {
			LOG_WRN("actuator_manage: campos invalidos");
			return;
		}
		uart_link_send_actuator_manage((uint8_t)cmd, (uint8_t)addr_type, mac);
	} else if (strcmp(op, "gateway_ctrl") == 0) {
		long cmd;
		char hex[2 * MAX_GATEWAY_CTRL_ARG_LEN + 1];
		uint8_t arg[MAX_GATEWAY_CTRL_ARG_LEN];
		int alen = 0;

		if (!json_get_int(json, "cmd", &cmd)) {
			LOG_WRN("gateway_ctrl sem \"cmd\"");
			return;
		}
		if (json_get_str(json, "arg", hex, sizeof(hex))) {
			alen = hex_decode(hex, arg, sizeof(arg));
			if (alen < 0) {
				LOG_WRN("gateway_ctrl: \"arg\" nao e' hex valido, ignorando arg");
				alen = 0;
			}
		}
		uart_link_send_gateway_ctrl((uint8_t)cmd, arg, (uint8_t)alen);
	} else {
		LOG_WRN("Comando MQTT desconhecido: op=\"%s\"", op);
	}
}

static void cmd_work_handler(struct k_work *work)
{
	struct pending_cmd cmd;

	ARG_UNUSED(work);

	while (k_msgq_get(&cmd_msgq, &cmd, K_NO_WAIT) == 0) {
		handle_command(cmd.json);
	}
}

static void enqueue_command(const char *json, size_t len)
{
	struct pending_cmd cmd;
	size_t n = MIN(len, sizeof(cmd.json) - 1);

	memcpy(cmd.json, json, n);
	cmd.json[n] = '\0';

	if (k_msgq_put(&cmd_msgq, &cmd, K_NO_WAIT) != 0) {
		LOG_WRN("Fila de comandos cheia, descartando: %s", cmd.json);
		return;
	}
	k_work_submit(&cmd_work);
}

/* --- LTE / modem / certificado (quase inalterado do l4_e2) --- */

/* Achado em bancada (2026-08-15): o handler original so logava o
 * status de registro quando JA tinha registrado - qualquer outro
 * status (procurando, negado, sem SIM...) caia no default silencioso,
 * entao uma LTE que nunca registra fica sem log nenhum, indistinguivel
 * de "ainda nao tentou". Log incondicional agora, pra sempre ter pista
 * de onde travou. */
static const char *nw_reg_status_name(enum lte_lc_nw_reg_status status)
{
	switch (status) {
	case LTE_LC_NW_REG_NOT_REGISTERED:
		return "nao registrado";
	case LTE_LC_NW_REG_REGISTERED_HOME:
		return "registrado (rede local)";
	case LTE_LC_NW_REG_SEARCHING:
		return "procurando rede";
	case LTE_LC_NW_REG_REGISTRATION_DENIED:
		return "registro negado";
	case LTE_LC_NW_REG_UNKNOWN:
		return "desconhecido";
	case LTE_LC_NW_REG_REGISTERED_ROAMING:
		return "registrado (roaming)";
	case LTE_LC_NW_REG_UICC_FAIL:
		return "falha no SIM/UICC";
	case LTE_LC_NW_REG_NO_SUITABLE_CELL:
		return "nenhuma celula adequada encontrada";
	default:
		return "?";
	}
}

static const char *modem_evt_name(enum lte_lc_modem_evt_type type)
{
	switch (type) {
	case LTE_LC_MODEM_EVT_LIGHT_SEARCH_DONE:
		return "LIGHT_SEARCH_DONE";
	case LTE_LC_MODEM_EVT_SEARCH_DONE:
		return "SEARCH_DONE";
	case LTE_LC_MODEM_EVT_RESET_LOOP:
		return "RESET_LOOP (modem reiniciando em loop!)";
	case LTE_LC_MODEM_EVT_BATTERY_LOW:
		return "BATTERY_LOW";
	case LTE_LC_MODEM_EVT_OVERHEATED:
		return "OVERHEATED";
	case LTE_LC_MODEM_EVT_NO_IMEI:
		return "NO_IMEI (falha de hardware/provisionamento!)";
	case LTE_LC_MODEM_EVT_CE_LEVEL:
		return "CE_LEVEL";
	case LTE_LC_MODEM_EVT_RF_CAL_NOT_DONE:
		return "RF_CAL_NOT_DONE";
	case LTE_LC_MODEM_EVT_INVALID_BAND_CONF:
		return "INVALID_BAND_CONF";
	case LTE_LC_MODEM_EVT_DETECTED_COUNTRY:
		return "DETECTED_COUNTRY";
	default:
		return "?";
	}
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		LOG_INF("Network registration status: %s (%d)",
			nw_reg_status_name(evt->nw_reg_status), evt->nw_reg_status);
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		    (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			break;
		}
		k_sem_give(&lte_connected);
		break;
	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("RRC mode: %s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "Connected" : "Idle");
		break;
	case LTE_LC_EVT_MODEM_EVENT:
		LOG_INF("Modem event: %s", modem_evt_name(evt->modem_evt.type));
		break;
	default:
		break;
	}
}

static int certificate_provision(void)
{
	int err = 0;
	bool exists;

	err = modem_key_mgmt_exists(CONFIG_MQTT_HELPER_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				     &exists);
	if (err) {
		LOG_ERR("Failed to check for certificates err %d", err);
		return err;
	}

	if (exists) {
		err = modem_key_mgmt_cmp(CONFIG_MQTT_HELPER_SEC_TAG,
					  MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, ca_certificate,
					  sizeof(ca_certificate) - 1);
		LOG_INF("Comparing credentials: %s", err ? "Mismatch" : "Match");
		if (!err) {
			return 0;
		}
	}

	err = modem_key_mgmt_write(CONFIG_MQTT_HELPER_SEC_TAG, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN,
				    ca_certificate, sizeof(ca_certificate) - 1);
	if (err) {
		LOG_ERR("Failed to provision CA certificate: %d", err);
		return err;
	}

	return 0;
}

static int modem_configure(void)
{
	int err;

	LOG_INF("Initializing modem library");
	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Failed to initialize the modem library, error: %d", err);
		return err;
	}

	/* Certificado provisionado com o modem offline, antes de conectar. */
	err = certificate_provision();
	if (err) {
		LOG_ERR("Failed to provision certificates");
		return err;
	}

	LOG_INF("Connecting to LTE network");
	err = lte_lc_connect_async(lte_handler);
	if (err) {
		LOG_ERR("Error in lte_lc_connect_async, error: %d", err);
		return err;
	}

	k_sem_take(&lte_connected, K_FOREVER);
	LOG_INF("Connected to LTE network");

	return 0;
}

static int client_id_get(char *buffer, size_t buffer_len)
{
	int len;
	int err;
	char imei_buf[CGSN_RESPONSE_LENGTH];

	if (!buffer || buffer_len == 0) {
		LOG_ERR("Invalid buffer parameters");
		return -EINVAL;
	}

	if (strlen(CONFIG_SIMCONNECT_MQTT_CLIENT_ID) > 0) {
		len = snprintk(buffer, buffer_len, "%s", CONFIG_SIMCONNECT_MQTT_CLIENT_ID);
		if ((len < 0) || (len >= (int)buffer_len)) {
			LOG_ERR("Failed to format client ID from config, error: %d", len);
			return -EMSGSIZE;
		}
		return 0;
	}

	err = nrf_modem_at_cmd(imei_buf, sizeof(imei_buf), "AT+CGSN");
	if (err) {
		LOG_ERR("Failed to obtain IMEI, error: %d", err);
		return err;
	}

	imei_buf[IMEI_LEN] = '\0';

	len = snprintk(buffer, buffer_len, "nrf-%.*s", IMEI_LEN, imei_buf);
	if ((len < 0) || (len >= (int)buffer_len)) {
		LOG_ERR("Failed to format client ID from IMEI, error: %d", len);
		return -EMSGSIZE;
	}

	return 0;
}

/* --- MQTT: publish/subscribe --- */

/* Nome "publish_message" (nao "mqtt_publish") de proposito - colide
 * com a funcao mqtt_publish() ja declarada em zephyr/net/mqtt.h
 * (incluido via mqtt_helper.h), mesmo sendo static. */
static int publish_message(const char *topic, const void *data, size_t len, bool retain)
{
	struct mqtt_publish_param param = {0};

	param.message.payload.data = (uint8_t *)data;
	param.message.payload.len = len;
	param.message.topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
	param.message.topic.topic.utf8 = (uint8_t *)topic;
	param.message.topic.topic.size = strlen(topic);
	param.message_id = mqtt_helper_msg_id_get();
	param.dup_flag = 0;
	param.retain_flag = retain ? 1 : 0;

	return mqtt_helper_publish(&param);
}

static void subscribe(void)
{
	int err;
	struct mqtt_topic subscribe_topic = {
		.topic = {.utf8 = (uint8_t *)cmd_topic, .size = strlen(cmd_topic)},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE};
	struct mqtt_subscription_list subscription_list = {
		.list = &subscribe_topic, .list_count = 1, .message_id = SUBSCRIBE_TOPIC_ID};

	LOG_INF("Subscribing to %s", cmd_topic);
	err = mqtt_helper_subscribe(&subscription_list);
	if (err) {
		LOG_ERR("Failed to subscribe to topics, error: %d", err);
	}
}

static void on_mqtt_connack(enum mqtt_conn_return_code return_code, bool session_present)
{
	ARG_UNUSED(session_present);

	if (return_code != MQTT_CONNECTION_ACCEPTED) {
		LOG_WRN("Connection to broker not established, return_code: %d", return_code);
		return;
	}

	LOG_INF("Connected to MQTT broker");
	LOG_INF("Hostname: %s", CONFIG_SIMCONNECT_MQTT_BROKER_HOSTNAME);
	LOG_INF("Client ID: %s", (char *)client_id);

	mqtt_connected = true;
	subscribe();

	/* Sincronizacao inicial (docs/PROTOCOLO_91_MQTT.md, "Gatilho de
	 * sincronizacao inicial") - via a mesma fila que comandos vindos
	 * de fora usam, ver nota grande no topo do arquivo. */
	enqueue_command("{\"op\":\"get_status_all\"}", 22);
	enqueue_command("{\"op\":\"get_discovered\"}", 22);
}

static void on_mqtt_suback(uint16_t message_id, int result)
{
	if (result != MQTT_SUBACK_FAILURE) {
		if (message_id == SUBSCRIBE_TOPIC_ID) {
			LOG_INF("Subscribed to %s with QoS %d", cmd_topic, result);
			return;
		}
		LOG_WRN("Subscribed to unknown topic, id: %d with QoS %d", message_id, result);
		return;
	}
	LOG_ERR("Topic subscription failed, error: %d", result);
}

static void on_mqtt_publish(struct mqtt_helper_buf topic, struct mqtt_helper_buf payload)
{
	ARG_UNUSED(topic);

	LOG_INF("Comando MQTT recebido (%u bytes)", payload.size);
	enqueue_command(payload.ptr, payload.size);
}

static void on_mqtt_disconnect(int result)
{
	LOG_INF("MQTT client disconnected: %d", result);
	mqtt_connected = false;
}

/* --- API publica --- */

int mqtt_client_start(void)
{
	int err;

	snprintk(cmd_topic, sizeof(cmd_topic), "%s/cmd", CONFIG_SIMCONNECT_MQTT_TOPIC_PREFIX);

	k_work_init(&cmd_work, cmd_work_handler);

	err = modem_configure();
	if (err) {
		LOG_ERR("Failed to configure the modem, error: %d", err);
		return err;
	}

	struct mqtt_helper_cfg config = {
		.cb =
			{
				.on_connack = on_mqtt_connack,
				.on_disconnect = on_mqtt_disconnect,
				.on_publish = on_mqtt_publish,
				.on_suback = on_mqtt_suback,
			},
	};

	err = mqtt_helper_init(&config);
	if (err) {
		LOG_ERR("Failed to initialize MQTT helper, error: %d", err);
		return err;
	}

	err = client_id_get(client_id, sizeof(client_id));
	if (err) {
		LOG_ERR("Failed to get client ID, error: %d", err);
		return err;
	}

	struct mqtt_helper_conn_params conn_params = {
		.hostname.ptr = CONFIG_SIMCONNECT_MQTT_BROKER_HOSTNAME,
		.hostname.size = strlen(CONFIG_SIMCONNECT_MQTT_BROKER_HOSTNAME),
		.device_id.ptr = (char *)client_id,
		.device_id.size = strlen((char *)client_id),
	};

	err = mqtt_helper_connect(&conn_params);
	if (err) {
		LOG_ERR("Failed to connect to MQTT, error code: %d", err);
		return err;
	}

	return 0;
}

void mqtt_client_publish_status(uint8_t slot, const char *state, uint8_t addr_type,
				 const char *mac, const char *name)
{
	char topic[64];
	char payload[192];
	int len;

	if (!mqtt_connected) {
		return;
	}

	snprintk(topic, sizeof(topic), "%s/actuator/%u/status", CONFIG_SIMCONNECT_MQTT_TOPIC_PREFIX,
		 slot);
	len = snprintk(payload, sizeof(payload),
		       "{\"slot\":%u,\"state\":\"%s\",\"addr_type\":%u,\"mac\":\"%s\","
		       "\"name\":\"%s\"}",
		       slot, state, addr_type, mac, name);
	if (len < 0 || len >= (int)sizeof(payload)) {
		LOG_WRN("Payload de status truncado, nao publicado");
		return;
	}

	if (publish_message(topic, payload, (size_t)len, true)) {
		LOG_WRN("Falha ao publicar status do slot %u", slot);
	}
}

void mqtt_client_publish_discovery(uint8_t addr_type, const char *mac, int8_t rssi, uint8_t flags,
				    const char *name)
{
	char topic_mac[13]; /* MAC sem dois-pontos, 12 hex + \0 */
	char topic[80];
	char payload[160];
	int len;
	size_t j = 0;

	if (!mqtt_connected) {
		return;
	}

	/* Remove os ':' do MAC pra usar como segmento de topico. */
	for (size_t i = 0; mac[i] != '\0' && j < sizeof(topic_mac) - 1; i++) {
		if (mac[i] != ':') {
			topic_mac[j++] = mac[i];
		}
	}
	topic_mac[j] = '\0';

	snprintk(topic, sizeof(topic), "%s/discovery/%s", CONFIG_SIMCONNECT_MQTT_TOPIC_PREFIX,
		 topic_mac);
	len = snprintk(payload, sizeof(payload),
		       "{\"addr_type\":%u,\"mac\":\"%s\",\"rssi\":%d,\"flags\":%u,\"name\":\"%s\"}",
		       addr_type, mac, rssi, flags, name);
	if (len < 0 || len >= (int)sizeof(payload)) {
		LOG_WRN("Payload de descoberta truncado, nao publicado");
		return;
	}

	if (publish_message(topic, payload, (size_t)len, true)) {
		LOG_WRN("Falha ao publicar descoberta de %s", mac);
	}
}

void mqtt_client_publish_raw(uint8_t slot, const uint8_t *data, uint8_t len)
{
	char topic[64];
	char hex[2 * 250 + 1];
	char payload[2 * 250 + 32];
	int plen;

	if (!mqtt_connected) {
		return;
	}

	hex_encode(data, len, hex, sizeof(hex));

	snprintk(topic, sizeof(topic), "%s/actuator/%u/raw", CONFIG_SIMCONNECT_MQTT_TOPIC_PREFIX,
		 slot);
	plen = snprintk(payload, sizeof(payload), "{\"slot\":%u,\"data\":\"%s\"}", slot, hex);
	if (plen < 0 || plen >= (int)sizeof(payload)) {
		LOG_WRN("Payload de raw truncado, nao publicado");
		return;
	}

	if (publish_message(topic, payload, (size_t)plen, false)) {
		LOG_WRN("Falha ao publicar raw do slot %u", slot);
	}
}

void mqtt_client_publish_result(const char *op, uint8_t seq_original, uint8_t err)
{
	char topic[48];
	char payload[64];
	int len;

	if (!mqtt_connected) {
		return;
	}

	snprintk(topic, sizeof(topic), "%s/result", CONFIG_SIMCONNECT_MQTT_TOPIC_PREFIX);
	len = snprintk(payload, sizeof(payload), "{\"op\":\"%s\",\"seq\":%u,\"err\":%u}", op,
		       seq_original, err);
	if (len < 0 || len >= (int)sizeof(payload)) {
		LOG_WRN("Payload de result truncado, nao publicado");
		return;
	}

	if (publish_message(topic, payload, (size_t)len, false)) {
		LOG_WRN("Falha ao publicar result (%s)", op);
	}
}
