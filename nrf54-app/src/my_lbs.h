/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef BT_LBS_H_
#define BT_LBS_H_

/**@file
 * @defgroup bt_lbs LED Button Service API
 * @{
 * @brief API for the LED Button Service (LBS).
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <zephyr/types.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/conn.h>

/* NOTA DE ARQUITETURA (2026-08-21): este servico costumava se declarar
 * sob o UUID proprio "00001523-..." (BT_UUID_LBS, removido daqui).
 * Agora se declara sob BT_UUID_ACTUATOR_SERVICE (actuator_client.h,
 * "06290001-...", o mesmo UUID que um Atuador BLE real ja usa em
 * campo) - as characteristics abaixo continuam com os mesmos UUIDs de
 * sempre, so' o servico que as agrupa mudou. Motivo: o pacote de
 * advertising legado (31 bytes) nao comporta anunciar os dois UUIDs de
 * servico ao mesmo tempo (18 bytes cada so' pra scan response) - ver
 * docs/SENSORES_I2C.md e o artifact "Um Servico, Dois Papeis"
 * referenciado la'. Fundindo os dois sob o UUID do Atuador, a
 * interface web e qualquer Gateway (o nosso, ou um futuro) descobrem
 * a mesma unidade pelo mesmo anuncio.
 */

/** @brief Button Characteristic UUID. */
#define BT_UUID_LBS_BUTTON_VAL                                                                     \
	BT_UUID_128_ENCODE(0x00001524, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

/** @brief LED Characteristic UUID. */
#define BT_UUID_LBS_LED_VAL BT_UUID_128_ENCODE(0x00001525, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

/** @brief MYSENSOR characteristic UUID. */
#define BT_UUID_LBS_MYSENSOR_VAL \
	BT_UUID_128_ENCODE(0x00001526, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

/** @brief Actuator Raw Data Characteristic UUID (notify).
 *
 *  Formato: [slot(1)] [bytes crus da resposta do atuador...].
 *  Nenhum parsing no firmware - mesma filosofia do coleta_ble.
 */
#define BT_UUID_LBS_ACTUATOR_VAL \
	BT_UUID_128_ENCODE(0x00001527, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

/** @brief Actuator Command Characteristic UUID (write).
 *
 *  Formato: [slot(1)] [bytes crus do comando...]. O Gateway so repassa ao
 *  RX do atuador daquele slot (Write Without Response).
 */
#define BT_UUID_LBS_ACTUATOR_CMD_VAL \
	BT_UUID_128_ENCODE(0x00001528, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

/** @brief Actuator Manage Characteristic UUID (write, 8 bytes).
 *
 *  Formato: [cmd(1)] [addr_type(1)] [MAC(6, ordem interna do stack -
 *  INVERTIDA em relacao ao AA:BB:CC:DD:EE:FF humano)].
 *  cmd=0x01 adiciona; cmd=0x00 remove. addr_type: 0x00 public, 0x01 random.
 */
#define BT_UUID_LBS_ACTUATOR_MANAGE_VAL \
	BT_UUID_128_ENCODE(0x00001529, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

/** @brief Actuator Status Characteristic UUID (read + notify).
 *
 *  Esta e a "fonte da verdade" que a interface consulta ao conectar e
 *  acompanha ao vivo depois - sem ela, a UI teria que adivinhar o
 *  estado a partir do log RTT.
 *
 *  READ  -> MAX_ACTUATORS registros, em ordem de slot (ver
 *           ACTUATOR_STATUS_REC_LEN em actuator_client.h - hoje 41
 *           bytes cada, 19*41=779 bytes com MAX_ACTUATORS=19). Passa
 *           do MTU minimo, mas o GATT resolve via Read Blob
 *           automaticamente e o readValue() do Web Bluetooth remonta
 *           transparente.
 *  NOTIFY -> UM registro, so o slot que mudou. Notify nao tem
 *           fragmentacao, entao mandar todos de uma vez seria
 *           truncado silenciosamente em MTU pequeno.
 *
 *  Registro (formato completo em actuator_client.h - NAO duplique os
 *  offsets aqui de novo, e assim que essa doc ficou desatualizada da
 *  primeira vez: o campo de nome foi adicionado la e esqueceram de
 *  atualizar aqui):
 *    [0] slot  [1] state  [2] addr_type  [3..8] MAC (invertido)
 *    [9..40] nome (ACTUATOR_NAME_MAX_LEN bytes, UTF-8, zero-padded -
 *            corte no primeiro 0x00)
 */
#define BT_UUID_LBS_ACTUATOR_STATUS_VAL \
	BT_UUID_128_ENCODE(0x0000152a, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

/** @brief Discovered Actuators Characteristic UUID (read + notify).
 *
 *  Atuadores vistos no scan mas ainda nao adicionados. E o que permite
 *  a interface oferecer "encontrei estes por perto, clique para
 *  adicionar" em vez de exigir que o operador digite um MAC na mao.
 *
 *  Registro (formato completo em actuator_client.h):
 *    [0] addr_type  [1..6] MAC (invertido)  [7] rssi (int8)
 *    [8] flags (bit0 = ja esta na allow-list)
 *    [9..40] nome (ACTUATOR_NAME_MAX_LEN bytes, mesmo formato acima)
 */
#define BT_UUID_LBS_DISCOVERY_VAL \
	BT_UUID_128_ENCODE(0x0000152b, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

/** @brief Gateway Control Characteristic UUID (write).
 *
 *  Formato: [cmd(1)] [arg(0..n)]
 *    0x01                  -> limpa o cache de descoberta
 *    0x02                  -> forca reinicio do scan
 *    0x03 [slot(1)]        -> desconecta aquele slot (segue na allow-list)
 *    0x04 [slot(1)] [0|1]  -> modo do enlace: 0=lento, 1=rapido
 *
 *  O 0x04 e o que torna viavel baixar curvas/eventos com muitos
 *  atuadores conectados: peca RAPIDO antes da coleta, LENTO ao
 *  terminar. Reverte sozinho apos alguns minutos se a interface
 *  esquecer (ou cair no meio).
 */
#define BT_UUID_LBS_GATEWAY_CTRL_VAL \
	BT_UUID_128_ENCODE(0x0000152c, 0x1212, 0xefde, 0x1523, 0x785feabcd123)

#define BT_UUID_LBS_BUTTON BT_UUID_DECLARE_128(BT_UUID_LBS_BUTTON_VAL)
#define BT_UUID_LBS_LED BT_UUID_DECLARE_128(BT_UUID_LBS_LED_VAL)
#define BT_UUID_LBS_MYSENSOR        BT_UUID_DECLARE_128(BT_UUID_LBS_MYSENSOR_VAL)
#define BT_UUID_LBS_ACTUATOR        BT_UUID_DECLARE_128(BT_UUID_LBS_ACTUATOR_VAL)
#define BT_UUID_LBS_ACTUATOR_CMD    BT_UUID_DECLARE_128(BT_UUID_LBS_ACTUATOR_CMD_VAL)
#define BT_UUID_LBS_ACTUATOR_MANAGE BT_UUID_DECLARE_128(BT_UUID_LBS_ACTUATOR_MANAGE_VAL)
#define BT_UUID_LBS_ACTUATOR_STATUS BT_UUID_DECLARE_128(BT_UUID_LBS_ACTUATOR_STATUS_VAL)
#define BT_UUID_LBS_DISCOVERY       BT_UUID_DECLARE_128(BT_UUID_LBS_DISCOVERY_VAL)
#define BT_UUID_LBS_GATEWAY_CTRL    BT_UUID_DECLARE_128(BT_UUID_LBS_GATEWAY_CTRL_VAL)

/* Buffers internos dos handlers de read. my_lbs.c nao inclui
 * actuator_client.h (para nao acoplar as camadas), entao nao conhece
 * MAX_ACTUATORS/MAX_DISCOVERED - por isso os valores sao dimensionados
 * com folga aqui.
 *
 * ATENCAO: se MAX_ACTUATORS ou o tamanho do registro (ACTUATOR_STATUS_REC_LEN,
 * hoje 41 bytes = 9 + nome de 32) crescerem, ESTES numeros tem que
 * crescer junto. Hoje: 19 slots x 41 bytes = 779 (status),
 * 12 descobertos x 41 bytes = 492 (discovery). Um buffer pequeno demais
 * devolveria os primeiros registros e cortaria o resto em silencio.
 */
#define MY_LBS_STATUS_BUF_LEN    832
#define MY_LBS_DISCOVERY_BUF_LEN 512

/* Comandos aceitos pela Gateway Control */
#define GATEWAY_CTRL_CLEAR_DISCOVERY 0x01
#define GATEWAY_CTRL_RESTART_SCAN    0x02
#define GATEWAY_CTRL_DISCONNECT_SLOT 0x03
#define GATEWAY_CTRL_SET_SPEED       0x04
/* [name cru, 1..CONFIG_BT_DEVICE_NAME_MAX bytes, sem terminador] - ver
 * nota em main.c sobre por que isso NAO usa mais a characteristic GAP
 * padrao (0x2A00): alem de escrita bloqueada em alguns navegadores/SO
 * (Web Bluetooth no Windows recusa write nela), bt_set_name() sozinho
 * NAO persiste de forma confiavel neste projeto (bt/name via settings
 * embutido do Zephyr nao sobrevive a reboot aqui - allow-list, no
 * mesmo backend ZMS, sobrevive normalmente). Este opcode persiste
 * explicitamente, do nosso jeito, igual a allow-list. */
#define GATEWAY_CTRL_SET_NAME        0x05

/** @brief Callback type for when an LED state change is received. */
typedef void (*led_cb_t)(const bool led_state);

/** @brief Callback type for when the button state is pulled. */
typedef bool (*button_cb_t)(void);

/** @brief Raw actuator command written by the phone/browser. */
typedef void (*actuator_cmd_cb_t)(uint8_t slot, const uint8_t *data, uint16_t len);

/** @brief Add/remove an actuator from the Gateway's allow-list.
 *         cmd: 1=add, 0=remove.
 */
typedef void (*actuator_manage_cb_t)(uint8_t cmd, const bt_addr_le_t *addr);

/** @brief Preenche *buf* com o estado atual (read da characteristic).
 *         Retorna a quantidade de bytes escritos.
 */
typedef uint16_t (*state_read_cb_t)(uint8_t *buf, uint16_t buf_len);

/** @brief Comando de controle do Gateway. */
typedef void (*gateway_ctrl_cb_t)(uint8_t cmd, const uint8_t *arg, uint16_t arg_len);

/** @brief Callback struct used by the LBS Service. */
struct my_lbs_cb {
	led_cb_t led_cb;
	button_cb_t button_cb;
	actuator_cmd_cb_t actuator_cmd_cb;
	actuator_manage_cb_t actuator_manage_cb;
	state_read_cb_t actuator_status_read_cb;
	state_read_cb_t discovery_read_cb;
	gateway_ctrl_cb_t gateway_ctrl_cb;
};

/** @brief Initialize the LBS Service. */
int my_lbs_init(struct my_lbs_cb *callbacks);

/** @brief Informa ao servico qual conexao e a do celular/navegador.
 *
 *  Usado apenas para consultar o MTU negociado e avisar em log quando
 *  um payload nao cabe - sem isso, o truncamento do bt_gatt_notify e
 *  silencioso, que e exatamente o tipo de bug dificil de achar quando
 *  uma curva de 72 bytes chega pela metade na tela.
 */
void my_lbs_set_phone_conn(struct bt_conn *conn);

/** @brief true se a interface assinou a Actuator Raw Data.
 *
 *  Serve para avisar cedo: mandar um comando sem ter assinado o notify
 *  faz a resposta do atuador ser descartada, e o sintoma ("nao
 *  respondeu nada") nao aponta para a causa.
 */
bool my_lbs_actuator_notify_enabled(void);

/** @brief Send the button state as indication. */
int my_lbs_send_button_state_indicate(bool button_state);

/** @brief Send the button state as notification. */
int my_lbs_send_button_state_notify(bool button_state);

/** @brief Send the sensor value as notification. */
int my_lbs_send_sensor_notify(uint32_t sensor_value);

/** @brief Notifica bytes crus de um atuador, prefixados pelo slot.
 *
 * @retval -EACCES  O cliente nao assinou a characteristic.
 * @retval -EMSGSIZE O payload nao cabe no MTU negociado (nada e enviado).
 */
int my_lbs_send_actuator_raw_data(uint8_t slot, const uint8_t *data, uint16_t len);

/** @brief Notifica um registro de status de slot (9 bytes). */
int my_lbs_send_actuator_status(const uint8_t *rec, uint16_t len);

/** @brief Notifica um registro de descoberta (9 bytes). */
int my_lbs_send_discovery(const uint8_t *rec, uint16_t len);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* BT_LBS_H_ */
