/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/** @file
 *  @brief LED Button Service (LBS) - versao do Gateway (linha SIM)
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include "my_lbs.h"
#include "actuator_client.h"
#include "actuator_service.h"

LOG_MODULE_DECLARE(Coester_Gateway);

static bool notify_mysensor_enabled;
static bool notify_actuator_enabled;
static bool notify_status_enabled;
static bool notify_discovery_enabled;
static bool notify_actuator_tx_enabled;
static bool indicate_enabled;
static bool button_state;
static struct my_lbs_cb lbs_cb;
static struct bt_conn *phone_conn;

static struct bt_gatt_indicate_params ind_params;

/* --- Callbacks de CCC --- */

static void mylbsbc_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	indicate_enabled = (value == BT_GATT_CCC_INDICATE);
}

static void mylbsbc_ccc_mysensor_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_mysensor_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void mylbsbc_ccc_actuator_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_actuator_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Actuator Raw Data notify %s", notify_actuator_enabled ? "ON" : "OFF");
}

static void mylbsbc_ccc_status_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_status_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Actuator Status notify %s", notify_status_enabled ? "ON" : "OFF");
}

static void mylbsbc_ccc_discovery_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_discovery_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Discovery notify %s", notify_discovery_enabled ? "ON" : "OFF");
}

static void mylbsbc_ccc_actuator_tx_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	notify_actuator_tx_enabled = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Actuator TX (acgl/ifFerConfig) notify %s", notify_actuator_tx_enabled ? "ON" : "OFF");
}

static void indicate_cb(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err)
{
	LOG_DBG("Indication %s", err != 0U ? "fail" : "success");
}

/* --- Handlers de escrita --- */

static ssize_t write_led(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			 uint16_t len, uint16_t offset, uint8_t flags)
{
	if (len != 1U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (lbs_cb.led_cb) {
		uint8_t val = *((uint8_t *)buf);

		if (val == 0x00 || val == 0x01) {
			lbs_cb.led_cb(val ? true : false);
		} else {
			return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
		}
	}

	return len;
}

static ssize_t write_actuator_cmd(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	/* [slot(1)] [bytes crus do comando...] */
	if (len < 2) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (lbs_cb.actuator_cmd_cb) {
		const uint8_t *data = (const uint8_t *)buf;

		lbs_cb.actuator_cmd_cb(data[0], &data[1], len - 1);
	}

	return len;
}

/* [cmd(1)] [addr_type(1)] [MAC(6, little-endian)] = 8 bytes */
#define ACTUATOR_MANAGE_WRITE_LEN 8

static ssize_t write_actuator_manage(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len != ACTUATOR_MANAGE_WRITE_LEN) {
		LOG_WRN("Actuator Manage: esperava %d bytes, recebi %u",
			ACTUATOR_MANAGE_WRITE_LEN, len);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	const uint8_t *data = (const uint8_t *)buf;
	uint8_t cmd = data[0];
	bt_addr_le_t addr = { .type = data[1] };

	if (data[1] != BT_ADDR_LE_PUBLIC && data[1] != BT_ADDR_LE_RANDOM) {
		LOG_WRN("Actuator Manage: addr_type invalido (0x%02x)", data[1]);
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	memcpy(addr.a.val, &data[2], sizeof(addr.a.val));

	if (lbs_cb.actuator_manage_cb) {
		lbs_cb.actuator_manage_cb(cmd, &addr);
	}

	return len;
}

static ssize_t write_gateway_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (lbs_cb.gateway_ctrl_cb) {
		const uint8_t *data = (const uint8_t *)buf;

		lbs_cb.gateway_ctrl_cb(data[0], &data[1], len - 1);
	}

	return len;
}

/* Definida mais abaixo (depois de BT_GATT_SERVICE_DEFINE - precisa de
 * my_lbs_svc/ATTR_IDX_ACTUATOR_TX_VAL, que so' existem a partir dali),
 * mas referenciada dentro da declaracao do servico - por isso o
 * prototipo aqui.
 */
static ssize_t write_actuator_rx(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				  const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

/* --- Handlers de leitura --- */

static ssize_t read_button(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	const char *value = attr->user_data;

	if (lbs_cb.button_cb) {
		button_state = lbs_cb.button_cb();
		return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(*value));
	}

	return 0;
}

/* Nota sobre Read Blob: com MTU pequeno o cliente le em pedacos, e
 * este handler e chamado uma vez por pedaco, remontando o buffer a
 * cada chamada. Se o estado mudar no meio de uma leitura longa, o
 * cliente pode ver uma mistura de dois instantes. Na pratica isso e
 * inofensivo aqui (o notify corrige logo em seguida), e some por
 * completo quando o MTU negociado comporta a resposta inteira - que e
 * o caso com as configs de MTU do prj.conf.
 */
static ssize_t read_actuator_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				    void *buf, uint16_t len, uint16_t offset)
{
	static uint8_t status_buf[MY_LBS_STATUS_BUF_LEN];
	uint16_t total = 0;

	if (lbs_cb.actuator_status_read_cb) {
		total = lbs_cb.actuator_status_read_cb(status_buf, sizeof(status_buf));
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, status_buf, total);
}

static ssize_t read_discovery(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			      uint16_t len, uint16_t offset)
{
	static uint8_t disc_buf[MY_LBS_DISCOVERY_BUF_LEN];
	uint16_t total = 0;

	if (lbs_cb.discovery_read_cb) {
		total = lbs_cb.discovery_read_cb(disc_buf, sizeof(disc_buf));
	}

	return bt_gatt_attr_read(conn, attr, buf, len, offset, disc_buf, total);
}

/* --- Declaracao do servico ---
 *
 * ATENCAO: os indices de my_lbs_svc.attrs[] abaixo sao usados
 * diretamente nas funcoes de notify. Se voce inserir uma
 * characteristic NO MEIO desta lista, todos os indices seguintes
 * deslocam e as notificacoes passam a apontar para o atributo errado -
 * sem erro de compilacao e sem erro em runtime, so dados no lugar
 * errado. Adicione sempre NO FIM, e atualize a tabela abaixo.
 *
 *   [0]  Primary Service
 *   [1]  decl Button        [2]  valor Button        [3]  CCC
 *   [4]  decl LED           [5]  valor LED
 *   [6]  decl MySensor      [7]  valor MySensor      [8]  CCC
 *   [9]  decl Raw Data      [10] valor Raw Data      [11] CCC
 *   [12] decl Command       [13] valor Command
 *   [14] decl Manage        [15] valor Manage
 *   [16] decl Status        [17] valor Status        [18] CCC
 *   [19] decl Discovery     [20] valor Discovery     [21] CCC
 *   [22] decl Gateway Ctrl  [23] valor Gateway Ctrl
 *   [24] decl Actuator RX   [25] valor Actuator RX
 *   [26] decl Actuator TX   [27] valor Actuator TX   [28] CCC
 */
#define ATTR_IDX_BUTTON_VAL    2
#define ATTR_IDX_MYSENSOR_VAL  7
#define ATTR_IDX_RAWDATA_VAL   10
#define ATTR_IDX_STATUS_VAL    17
#define ATTR_IDX_DISCOVERY_VAL 20
#define ATTR_IDX_ACTUATOR_TX_VAL 27

/* Servico declarado sob BT_UUID_ACTUATOR_SERVICE (nao mais o
 * BT_UUID_LBS proprio) - ver a nota de arquitetura em my_lbs.h.
 */
BT_GATT_SERVICE_DEFINE(
	my_lbs_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_ACTUATOR_SERVICE),

	BT_GATT_CHARACTERISTIC(BT_UUID_LBS_BUTTON, BT_GATT_CHRC_READ | BT_GATT_CHRC_INDICATE,
			       BT_GATT_PERM_READ, read_button, NULL, &button_state),
	BT_GATT_CCC(mylbsbc_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_LBS_LED, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL,
			       write_led, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_LBS_MYSENSOR, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL,
			       NULL, NULL),
	BT_GATT_CCC(mylbsbc_ccc_mysensor_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* Actuator Raw Data - [slot][bytes crus da resposta] */
	BT_GATT_CHARACTERISTIC(BT_UUID_LBS_ACTUATOR, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL,
			       NULL, NULL),
	BT_GATT_CCC(mylbsbc_ccc_actuator_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* Actuator Command - [slot][bytes crus do comando] */
	BT_GATT_CHARACTERISTIC(BT_UUID_LBS_ACTUATOR_CMD, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
			       NULL, write_actuator_cmd, NULL),

	/* Actuator Manage - [cmd][addr_type][MAC] */
	BT_GATT_CHARACTERISTIC(BT_UUID_LBS_ACTUATOR_MANAGE, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE,
			       NULL, write_actuator_manage, NULL),

	/* Actuator Status - read devolve todos os slots, notify manda um */
	BT_GATT_CHARACTERISTIC(BT_UUID_LBS_ACTUATOR_STATUS,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ,
			       read_actuator_status, NULL, NULL),
	BT_GATT_CCC(mylbsbc_ccc_status_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* Discovered Actuators - atuadores vistos e ainda nao adicionados */
	BT_GATT_CHARACTERISTIC(BT_UUID_LBS_DISCOVERY, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_discovery, NULL, NULL),
	BT_GATT_CCC(mylbsbc_ccc_discovery_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* Gateway Control - comandos administrativos da interface */
	BT_GATT_CHARACTERISTIC(BT_UUID_LBS_GATEWAY_CTRL, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL,
			       write_gateway_ctrl, NULL),

	/* Atuador (protocolo acgl/ifFerConfig) - RX: pedidos do Gateway;
	 * TX: respostas. Areas implementadas (Comando, Painel/paramDado,
	 * Sensor, Painel Remoto + sinteticas) em actuator_service.c; o
	 * resto responde GTM_NEG. Os parametros (antes na characteristic
	 * Calib, removida) vivem na area Painel - ver docs/PARAMETROS.md.
	 */
	BT_GATT_CHARACTERISTIC(BT_UUID_ACTUATOR_RX, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL,
			       write_actuator_rx, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_ACTUATOR_TX, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL,
			       NULL, NULL),
	BT_GATT_CCC(mylbsbc_ccc_actuator_tx_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

static ssize_t write_actuator_rx(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				  const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	/* static: 152 bytes nao vao pra pilha da thread do BT */
	static uint8_t resp[ACTUATOR_SERVICE_MAX_RESP_LEN];
	size_t resp_len;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	resp_len = actuator_service_handle_msg((const uint8_t *)buf, len, resp, sizeof(resp));

	if (notify_actuator_tx_enabled && resp_len > 0) {
		bt_gatt_notify(NULL, &my_lbs_svc.attrs[ATTR_IDX_ACTUATOR_TX_VAL], resp, resp_len);
	}

	return len;
}

int my_lbs_init(struct my_lbs_cb *callbacks)
{
	if (callbacks) {
		lbs_cb = *callbacks;
	}

	return 0;
}

void my_lbs_set_phone_conn(struct bt_conn *conn)
{
	phone_conn = conn;
}

bool my_lbs_actuator_notify_enabled(void)
{
	return notify_actuator_enabled;
}

int my_lbs_send_button_state_indicate(bool button_state)
{
	if (!indicate_enabled) {
		return -EACCES;
	}

	ind_params.attr = &my_lbs_svc.attrs[ATTR_IDX_BUTTON_VAL];
	ind_params.func = indicate_cb;
	ind_params.destroy = NULL;
	ind_params.data = &button_state;
	ind_params.len = sizeof(button_state);

	return bt_gatt_indicate(NULL, &ind_params);
}

int my_lbs_send_sensor_notify(uint32_t sensor_value)
{
	if (!notify_mysensor_enabled) {
		return -EACCES;
	}

	return bt_gatt_notify(NULL, &my_lbs_svc.attrs[ATTR_IDX_MYSENSOR_VAL], &sensor_value,
			      sizeof(sensor_value));
}

/* Maior payload que o Gateway aceita repassar do atuador ao navegador.
 * O maior frame real do protocolo proCo hoje e o de alarmes
 * (8 de header + 118 de payload = 126 bytes), entao 244 da folga.
 */
#define ACTUATOR_NOTIFY_MAX_PAYLOAD 244

int my_lbs_send_actuator_raw_data(uint8_t slot, const uint8_t *data, uint16_t len)
{
	/* static: evita alocar no stack a cada notify. Assume chamadas
	 * serializadas (vem do callback de notify do actuator_client, que
	 * roda no contexto unico da pilha BT).
	 */
	static uint8_t notify_buf[1 + ACTUATOR_NOTIFY_MAX_PAYLOAD];

	if (!notify_actuator_enabled) {
		return -EACCES;
	}

	if (len > ACTUATOR_NOTIFY_MAX_PAYLOAD) {
		LOG_WRN("Actuator raw data: %u bytes excede o limite de %d, truncando", len,
			ACTUATOR_NOTIFY_MAX_PAYLOAD);
		len = ACTUATOR_NOTIFY_MAX_PAYLOAD;
	}

	/* Checagem explicita de MTU. Sem isso o bt_gatt_notify trunca em
	 * silencio: uma curva de 72 bytes chegaria com 20 no navegador e
	 * o parser do outro lado acusaria "payload invalido" sem nenhuma
	 * pista de onde veio o problema. Melhor recusar e dizer o porque.
	 */
	if (phone_conn) {
		uint16_t max_payload = bt_gatt_get_mtu(phone_conn) - 3;

		if (len + 1 > max_payload) {
			LOG_ERR("Payload de %u bytes (slot %d) nao cabe no MTU "
				"negociado (max %u). Confirme as configs de MTU "
				"do prj.conf e se o navegador negociou MTU maior.",
				len + 1, slot, max_payload);
			return -EMSGSIZE;
		}
	}

	notify_buf[0] = slot;
	memcpy(&notify_buf[1], data, len);

	return bt_gatt_notify(NULL, &my_lbs_svc.attrs[ATTR_IDX_RAWDATA_VAL], notify_buf, len + 1);
}

/* Cada registro agora tem 41 bytes (9 + nome de 32) - precisa de MTU
 * negociado >= 44 (payload = MTU - 3) para caber num unico notify sem
 * truncar. Na pratica isso nunca e problema: o MTU exchange com a
 * interface acontece logo apos a conexao (ver att_mtu_updated no
 * main.c), bem antes de qualquer characteristic ser assinada.
 */
int my_lbs_send_actuator_status(const uint8_t *rec, uint16_t len)
{
	if (!notify_status_enabled) {
		return -EACCES;
	}

	return bt_gatt_notify(NULL, &my_lbs_svc.attrs[ATTR_IDX_STATUS_VAL], rec, len);
}

int my_lbs_send_discovery(const uint8_t *rec, uint16_t len)
{
	if (!notify_discovery_enabled) {
		return -EACCES;
	}

	return bt_gatt_notify(NULL, &my_lbs_svc.attrs[ATTR_IDX_DISCOVERY_VAL], rec, len);
}
