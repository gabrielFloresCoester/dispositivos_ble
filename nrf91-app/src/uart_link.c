/*
 * Coester - SIM Connect (nRF9151)
 *
 * Espelho do link UART com o nRF54 - framing identico ao ja validado
 * em exercises/uart-link e nrf54-app/src/uart_link.c
 * (SOF+TYPE+SEQ+LEN+PAYLOAD+CRC16, ACK+retry+fila).
 *
 * ATENCAO ao mexer neste arquivo - ver "Achados de bancada da Fase 3"
 * em docs/PROTOCOLO_54_91.md antes de tudo:
 *   1. Reuso o gate `tx_busy` desde o primeiro commit (achado #3 da
 *      Fase 3: ACK descartado em silencio se colidir com uma
 *      transmissao ja em voo - uart_tx() retorna -EBUSY de forma
 *      limpa, mas sem este gate o retorno era ignorado).
 *   2. A recepcao confiavel da UARTE do nRF91 (uart1) exige
 *      CONFIG_UART_1_NRF_HW_ASYNC=y + CONFIG_UART_1_NRF_HW_ASYNC_TIMER=2
 *      no prj.conf (achado #4 da Fase 3) - NAO REMOVER. Sem isso o
 *      link fica intermitente assim que houver latencia de interrupcao
 *      real no sistema (modem, etc.), mesmo funcionando perfeito
 *      isolado na bancada.
 *
 * MUDANCA NA INTEGRACAO MQTT (docs/PROTOCOLO_91_MQTT.md): ate a Fase 4,
 * o dispatch de frame recebido rodava direto no callback da UART (ISR)
 * porque so chamava printk() (ISR-safe). Agora que os handlers tambem
 * publicam no MQTT (mqtt_client_publish_*, que faz send() de socket
 * via o modem - NAO ISR-safe, mesma categoria de restricao que o
 * Bluetooth host tem no lado do 54, ver o comentario grande em
 * nrf54-app/src/uart_link.c), dispatch_frame() voltou a fazer so
 * ACK/dedup no ISR e desvia o processamento de verdade (as funcoes
 * print_ e mqtt_client_publish_) pra system workqueue via
 * "rx_record_work" - mesmo padrao ja usado do lado do 54.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#include "uart_link.h"
#include "mqtt_client.h"

/* uart1 = arduino_serial nesta board -> pinos D0(RX)/D1(TX) do header Arduino */
static const struct device *link_uart = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* --- Framing: identico a nrf54-app/src/uart_link.c (ver aquele
 * arquivo pros comentarios completos de cada campo) --- */

#define PROTO_SOF         0x7E
#define PROTO_HEADER_LEN  4
#define PROTO_MAX_PAYLOAD 250
#define PROTO_CRC_LEN     2
#define PROTO_MAX_FRAME   (PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN) /* 256 */

#define RECEIVE_BUFF_SIZE PROTO_MAX_FRAME
#define RECEIVE_TIMEOUT   100
#define ACK_TIMEOUT_MS    300
#define MAX_RETRIES       3

/* Tabela de opcodes - docs/PROTOCOLO_54_91.md */
#define PROTO_TYPE_ACK                 0x02
#define PROTO_TYPE_ACTUATOR_CMD        0x10 /* 91->54 [slot][comando cru] */
#define PROTO_TYPE_RAW_DATA            0x11 /* 54->91 [slot][resposta crua] */
#define PROTO_TYPE_ACTUATOR_MANAGE     0x12 /* 91->54 [cmd][addr_type][MAC x6] */
#define PROTO_TYPE_MANAGE_RESULT       0x13 /* 54->91 [seq_original][err] */
#define PROTO_TYPE_GET_STATUS          0x14 /* 91->54 [slot] */
#define PROTO_TYPE_STATUS_RECORD       0x15 /* 54->91 41 bytes */
#define PROTO_TYPE_GET_DISCOVERED      0x16 /* 91->54 [indice|0xFF=todos] */
#define PROTO_TYPE_DISCOVERED_RECORD   0x17 /* 54->91 41 bytes */
#define PROTO_TYPE_GATEWAY_CTRL        0x18 /* 91->54 [cmd][arg...] */
#define PROTO_TYPE_GATEWAY_CTRL_RESULT 0x19 /* 54->91 [seq_original][err] */

#define GET_DISCOVERED_ALL 0xFF

/* Espelha MAX_ACTUATORS de nrf54-app/src/actuator_client.h - o 91 nao
 * inclui aquele header (apps separados), so precisa do numero pra
 * saber ate onde varrer no gatilho de teste do botao 1.
 */
#define GATEWAY_MAX_ACTUATORS 19

/* Registro de 41 bytes, formato identico em STATUS_RECORD e
 * DISCOVERED_RECORD (ver docs/PROTOCOLO_54_91.md /
 * PROTOCOLO_INTERFACE.md pros offsets exatos de cada um).
 */
#define GATEWAY_REC_LEN 41

static uint8_t rx_buf[RECEIVE_BUFF_SIZE];
static uint8_t rx_buf_next[RECEIVE_BUFF_SIZE];
static bool link_ready;

static uint8_t rx_state;
static uint8_t rx_frame_buf[3 + PROTO_MAX_PAYLOAD];
static uint8_t rx_len;
static uint8_t rx_payload_idx;
static uint8_t rx_crc_hi;

static uint8_t tx_sequence;
static uint8_t pending_ack_sequence;
static uint8_t pending_tx_sequence;
static bool waiting_ack;
static uint8_t pending_frame[PROTO_MAX_FRAME];
static uint16_t pending_frame_len;
static uint8_t retry_count;

/* >= GATEWAY_MAX_ACTUATORS: uart_link_get_status_all() enfileira ate
 * 19 GET_STATUS de uma vez (1 por slot) - com fila menor, os ultimos
 * slots seriam descartados em silencio (ver enqueue_frame). */
#define OUTGOING_QUEUE_SIZE 20
struct queued_frame {
	uint8_t type;
	uint8_t payload[PROTO_MAX_PAYLOAD];
	uint8_t payload_len;
};
static struct queued_frame outgoing_queue[OUTGOING_QUEUE_SIZE];
static uint8_t outgoing_queue_head;
static uint8_t outgoing_queue_tail;
static uint8_t outgoing_queue_count;

static bool have_last_command;
static uint8_t last_command_type;
static uint8_t last_command_sequence;

static struct k_work ack_work;
static struct k_work_delayable retry_work;

/* Gate fisico do uart_tx() - ver nota grande no topo do arquivo
 * (achado #3 da Fase 3). Compartilhado entre as tres origens que
 * chamam uart_tx(): envio novo (send_frame), retransmissao
 * (retry_work_handler) e ACK (try_send_ack).
 */
static bool tx_busy;
static bool ack_send_pending;

static uint16_t proto_crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0xFFFF;

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i] << 8;
		for (int b = 0; b < 8; b++) {
			if (crc & 0x8000) {
				crc = (crc << 1) ^ 0x1021;
			} else {
				crc <<= 1;
			}
		}
	}
	return crc;
}

static void send_frame(uint8_t type, const uint8_t *payload, uint8_t payload_len);

static bool outgoing_queue_full(void)
{
	return outgoing_queue_count >= OUTGOING_QUEUE_SIZE;
}

static bool outgoing_queue_empty(void)
{
	return outgoing_queue_count == 0;
}

static void enqueue_frame(uint8_t type, const uint8_t *payload, uint8_t payload_len)
{
	struct queued_frame *f;

	if (outgoing_queue_full()) {
		printk("[uart_link] fila de saida cheia, descartando frame tipo=0x%02x\n", type);
		return;
	}

	f = &outgoing_queue[outgoing_queue_tail];
	f->type = type;
	f->payload_len = payload_len;
	memcpy(f->payload, payload, payload_len);
	outgoing_queue_tail = (outgoing_queue_tail + 1) % OUTGOING_QUEUE_SIZE;
	outgoing_queue_count++;
}

static void send_next_queued_frame(void)
{
	struct queued_frame *f;

	if (outgoing_queue_empty()) {
		return;
	}

	f = &outgoing_queue[outgoing_queue_head];
	outgoing_queue_head = (outgoing_queue_head + 1) % OUTGOING_QUEUE_SIZE;
	outgoing_queue_count--;
	send_frame(f->type, f->payload, f->payload_len);
}

static void send_frame(uint8_t type, const uint8_t *payload, uint8_t payload_len)
{
	int ret;
	uint8_t sequence;
	uint16_t crc;

	if (payload_len > PROTO_MAX_PAYLOAD) {
		printk("[uart_link] payload grande demais (%u > %u), descartado\n", payload_len,
		       PROTO_MAX_PAYLOAD);
		return;
	}

	if (waiting_ack || tx_busy) {
		enqueue_frame(type, payload, payload_len);
		return;
	}

	sequence = tx_sequence++;
	pending_frame[0] = PROTO_SOF;
	pending_frame[1] = type;
	pending_frame[2] = sequence;
	pending_frame[3] = payload_len;
	memcpy(&pending_frame[4], payload, payload_len);

	crc = proto_crc16(&pending_frame[1], 3 + payload_len);
	pending_frame[4 + payload_len] = (uint8_t)(crc >> 8);
	pending_frame[4 + payload_len + 1] = (uint8_t)(crc & 0xFF);
	pending_frame_len = 4 + payload_len + 2;

	ret = uart_tx(link_uart, pending_frame, pending_frame_len, SYS_FOREVER_US);
	if (ret == 0) {
		tx_busy = true;
		pending_tx_sequence = sequence;
		retry_count = 0;
		waiting_ack = true;
		k_work_reschedule(&retry_work, K_MSEC(ACK_TIMEOUT_MS));
	} else {
		printk("[uart_link] uart_tx falhou (err %d) para frame tipo=0x%02x seq=%u\n", ret,
		       type, sequence);
	}
}

static void try_send_ack(void)
{
	static uint8_t ack_frame[PROTO_HEADER_LEN + PROTO_CRC_LEN];
	uint16_t crc;
	int ret;

	if (tx_busy) {
		ack_send_pending = true;
		return;
	}

	ack_frame[0] = PROTO_SOF;
	ack_frame[1] = PROTO_TYPE_ACK;
	ack_frame[2] = pending_ack_sequence;
	ack_frame[3] = 0;

	crc = proto_crc16(&ack_frame[1], 3);
	ack_frame[4] = (uint8_t)(crc >> 8);
	ack_frame[5] = (uint8_t)(crc & 0xFF);

	ret = uart_tx(link_uart, ack_frame, sizeof(ack_frame), SYS_FOREVER_US);
	if (ret == 0) {
		tx_busy = true;
		ack_send_pending = false;
	} else {
		ack_send_pending = true;
	}
}

static void send_ack_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	try_send_ack();
}

static void retry_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!waiting_ack) {
		return;
	}

	if (tx_busy) {
		k_work_reschedule(&retry_work, K_MSEC(ACK_TIMEOUT_MS));
		return;
	}

	if (retry_count >= MAX_RETRIES) {
		waiting_ack = false;
		printk("[uart_link] ACK nao recebido seq=%u; desisti apos %d retransmissoes\n",
		       pending_tx_sequence, MAX_RETRIES);
		send_next_queued_frame();
		return;
	}

	retry_count++;
	if (uart_tx(link_uart, pending_frame, pending_frame_len, SYS_FOREVER_US) == 0) {
		tx_busy = true;
	} else {
		printk("[uart_link] erro ao retransmitir seq=%u\n", pending_tx_sequence);
	}
	k_work_reschedule(&retry_work, K_MSEC(ACK_TIMEOUT_MS));
}

/* --- Decodificacao/log dos registros que chegam do 54 ---
 *
 * Formato identico ao ja documentado em PROTOCOLO_INTERFACE.md /
 * actuator_client.h (nrf54-app) - so trocando "veio de uma
 * characteristic BLE" por "veio de um frame UART".
 */

static const char *state_name(uint8_t state)
{
	switch (state) {
	case 0: return "EMPTY";
	case 1: return "WAITING";
	case 2: return "CONNECTED";
	case 3: return "READY";
	case 4: return "ERROR";
	default: return "?";
	}
}

/* MAC armazenado na ordem interna do stack (invertida em relacao ao
 * AA:BB:CC:DD:EE:FF humano - ver nota em actuator_client.h do
 * nrf54-app) - inverte aqui pra imprimir no formato humano. */
static void format_mac(const uint8_t *mac_stack_order, char *out, size_t out_len)
{
	snprintk(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac_stack_order[5],
		  mac_stack_order[4], mac_stack_order[3], mac_stack_order[2], mac_stack_order[1],
		  mac_stack_order[0]);
}

/* Campo de nome de tamanho fixo (32 bytes, zero-padded) - corta no
 * primeiro 0x00, igual a interface JS ja faz. */
static void format_name(const uint8_t *name_field, char *out, size_t out_len)
{
	size_t n = 0;

	while (n < 32 && n < out_len - 1 && name_field[n] != 0) {
		out[n] = (char)name_field[n];
		n++;
	}
	out[n] = '\0';
}

static void print_status_record(const uint8_t *payload, uint8_t len)
{
	char mac[18];
	char name[33];

	if (len < GATEWAY_REC_LEN) {
		printk("[uart_link] STATUS_RECORD curto demais (%u bytes)\n", len);
		return;
	}

	format_mac(&payload[3], mac, sizeof(mac));
	format_name(&payload[9], name, sizeof(name));

	printk("[uart_link] STATUS slot=%u estado=%s addr_type=%u mac=%s nome=\"%s\"\n", payload[0],
	       state_name(payload[1]), payload[2], mac, name);

	mqtt_client_publish_status(payload[0], state_name(payload[1]), payload[2], mac, name);
}

static void print_discovery_record(const uint8_t *payload, uint8_t len)
{
	char mac[18];
	char name[33];

	if (len < GATEWAY_REC_LEN) {
		printk("[uart_link] DISCOVERED_RECORD curto demais (%u bytes)\n", len);
		return;
	}

	format_mac(&payload[1], mac, sizeof(mac));
	format_name(&payload[9], name, sizeof(name));

	printk("[uart_link] DESCOBERTO addr_type=%u mac=%s rssi=%d flags=0x%02x nome=\"%s\"\n",
	       payload[0], mac, (int8_t)payload[7], payload[8], name);

	mqtt_client_publish_discovery(payload[0], mac, (int8_t)payload[7], payload[8], name);
}

static void print_raw_data(const uint8_t *payload, uint8_t len)
{
	/* Uma linha so (nao 1 printk() por byte) - achado em bancada
	 * (2026-08-15): com CONFIG_LOG=y, CONFIG_LOG_PRINTK vira "y"
	 * automatico (default y if PRINTK - ver zephyr/subsys/logging/
	 * Kconfig.processing), ou seja todo printk() passa a virar
	 * mensagem de log, indo pro buffer/thread de processamento em vez
	 * de sair direto pro console. Um RAW_DATA de ate 250 bytes fazendo
	 * um printk() por byte gerava ate ~250 mensagens de log de uma vez
	 * so, mais rapido do que a thread de log consegue esvaziar -
	 * apareciam como "--- N messages dropped ---" no meio do hexdump. */
	char hex[3 * PROTO_MAX_PAYLOAD + 1]; /* "xx " por byte + terminador */
	size_t n = 0;

	if (len < 1) {
		printk("[uart_link] RAW_DATA sem slot\n");
		return;
	}

	for (uint8_t i = 1; i < len && n + 3 < sizeof(hex); i++) {
		n += snprintk(&hex[n], sizeof(hex) - n, "%02x ", payload[i]);
	}
	hex[n] = '\0';

	printk("[uart_link] RAW_DATA slot=%u (%u bytes): %s\n", payload[0], len - 1, hex);

	mqtt_client_publish_raw(payload[0], &payload[1], len - 1);
}

/* @param mqtt_op Nome do "op" espelhado (docs/PROTOCOLO_91_MQTT.md,
 *                 topico `result`) - "actuator_manage" ou
 *                 "gateway_ctrl", nao o mesmo texto do log.
 */
static void print_result(const char *label, const char *mqtt_op, const uint8_t *payload,
			  uint8_t len)
{
	if (len < 2) {
		printk("[uart_link] %s payload curto (%u bytes)\n", label, len);
		return;
	}
	printk("[uart_link] %s seq_original=%u err=0x%02x%s\n", label, payload[0], payload[1],
	       payload[1] == 0 ? " (ok)" : "");

	mqtt_client_publish_result(mqtt_op, payload[0], payload[1]);
}

/* Frame recebido, copiado do contexto ISR do uart_cb pra ser
 * processado na system workqueue (ver nota grande no topo do arquivo -
 * mqtt_client_publish_*() nao e' ISR-safe). So existe 1 "em voo" por
 * vez: o 54 so manda o proximo depois de receber nosso ACK, e so
 * mandamos o ACK depois de processar este (ver rx_record_work_handler)
 * - dedup abaixo cobre retransmissoes que cheguem enquanto ainda
 * estamos processando.
 */
static struct {
	uint8_t type;
	uint8_t sequence;
	uint8_t len;
	uint8_t payload[PROTO_MAX_PAYLOAD];
} rx_record;
static struct k_work rx_record_work;

static void rx_record_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	switch (rx_record.type) {
	case PROTO_TYPE_STATUS_RECORD:
		print_status_record(rx_record.payload, rx_record.len);
		break;
	case PROTO_TYPE_DISCOVERED_RECORD:
		print_discovery_record(rx_record.payload, rx_record.len);
		break;
	case PROTO_TYPE_RAW_DATA:
		print_raw_data(rx_record.payload, rx_record.len);
		break;
	case PROTO_TYPE_MANAGE_RESULT:
		print_result("MANAGE_RESULT", "actuator_manage", rx_record.payload, rx_record.len);
		break;
	case PROTO_TYPE_GATEWAY_CTRL_RESULT:
		print_result("GATEWAY_CTRL_RESULT", "gateway_ctrl", rx_record.payload,
			     rx_record.len);
		break;
	default:
		printk("[uart_link] opcode desconhecido 0x%02x (%u bytes)\n", rx_record.type,
		       rx_record.len);
		break;
	}

	/* ACK so vai depois do processamento - o remetente so reenvia se
	 * isto estourar 300ms (ACK_TIMEOUT_MS), folga enorme pra qualquer
	 * handler acima (inclusive a publicacao MQTT). */
	pending_ack_sequence = rx_record.sequence;
	k_work_submit(&ack_work);
}

/* Roda em contexto ISR (chamado de uart_cb) - so a parte generica/
 * ja validada em bancada (ACK, dedup) fica aqui. Ver nota grande no
 * topo do arquivo. */
static void dispatch_frame(uint8_t type, uint8_t sequence, const uint8_t *payload, uint8_t len)
{
	bool duplicate;

	if (type == PROTO_TYPE_ACK) {
		if (waiting_ack && sequence == pending_tx_sequence) {
			waiting_ack = false;
			k_work_cancel_delayable(&retry_work);
			send_next_queued_frame();
		}
		return;
	}

	duplicate = have_last_command && (type == last_command_type) &&
		    (sequence == last_command_sequence);

	if (duplicate) {
		/* Ja processado (ou em processamento) - so reenvia o ACK. */
		pending_ack_sequence = sequence;
		k_work_submit(&ack_work);
		return;
	}

	last_command_type = type;
	last_command_sequence = sequence;
	have_last_command = true;

	rx_record.type = type;
	rx_record.sequence = sequence;
	rx_record.len = len;
	memcpy(rx_record.payload, payload, len);
	k_work_submit(&rx_record_work);
}

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_RX_RDY:
		for (size_t i = 0; i < evt->data.rx.len; i++) {
			uint8_t byte = evt->data.rx.buf[evt->data.rx.offset + i];

			switch (rx_state) {
			case 0:
				rx_state = (byte == PROTO_SOF) ? 1 : 0;
				break;
			case 1:
				rx_frame_buf[0] = byte; /* TYPE */
				rx_state = 2;
				break;
			case 2:
				rx_frame_buf[1] = byte; /* SEQ */
				rx_state = 3;
				break;
			case 3:
				rx_len = byte;
				rx_frame_buf[2] = byte; /* LEN, tambem entra no CRC */
				if (rx_len > PROTO_MAX_PAYLOAD) {
					rx_state = 0;
					break;
				}
				rx_payload_idx = 0;
				rx_state = (rx_len == 0) ? 5 : 4;
				break;
			case 4:
				rx_frame_buf[3 + rx_payload_idx] = byte;
				rx_payload_idx++;
				if (rx_payload_idx == rx_len) {
					rx_state = 5;
				}
				break;
			case 5:
				rx_crc_hi = byte;
				rx_state = 6;
				break;
			case 6: {
				uint8_t rx_crc_lo = byte;
				uint16_t received = ((uint16_t)rx_crc_hi << 8) | rx_crc_lo;
				uint16_t expected = proto_crc16(rx_frame_buf, 3 + rx_len);

				rx_state = 0;
				if (received == expected) {
					dispatch_frame(rx_frame_buf[0], rx_frame_buf[1],
							&rx_frame_buf[3], rx_len);
				} else {
					printk("[uart_link] CRC16 invalido: recebido=0x%04X "
					       "esperado=0x%04X\n",
					       received, expected);
				}
				break;
			}
			default:
				rx_state = 0;
				break;
			}
		}
		break;
	case UART_RX_DISABLED:
		uart_rx_enable(dev, rx_buf, sizeof(rx_buf), RECEIVE_TIMEOUT);
		break;
	case UART_RX_BUF_REQUEST:
		uart_rx_buf_rsp(dev, rx_buf_next, sizeof(rx_buf_next));
		break;
	case UART_RX_STOPPED:
		printk("[uart_link] UART_RX_STOPPED, reason=%d\n", evt->data.rx_stop.reason);
		break;
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		tx_busy = false;
		if (ack_send_pending) {
			try_send_ack();
		} else if (!waiting_ack) {
			send_next_queued_frame();
		}
		break;
	default:
		break;
	}
}

int uart_link_init(void)
{
	int err;

	if (!device_is_ready(link_uart)) {
		printk("[uart_link] uart1 nao esta pronta - link com o 54 desabilitado\n");
		return -ENODEV;
	}

	k_work_init(&ack_work, send_ack_work_handler);
	k_work_init(&rx_record_work, rx_record_work_handler);
	k_work_init_delayable(&retry_work, retry_work_handler);

	err = uart_callback_set(link_uart, uart_cb, NULL);
	if (err) {
		printk("[uart_link] falha ao registrar callback da uart1 (err %d)\n", err);
		return err;
	}

	err = uart_rx_enable(link_uart, rx_buf, sizeof(rx_buf), RECEIVE_TIMEOUT);
	if (err) {
		printk("[uart_link] falha ao habilitar RX da uart1 (err %d)\n", err);
		return err;
	}

	link_ready = true;
	printk("[uart_link] link UART com o 54 pronto (uart1)\n");
	return 0;
}

void uart_link_get_status_all(void)
{
	if (!link_ready) {
		printk("[uart_link] link nao esta pronto, ignorando GET_STATUS\n");
		return;
	}

	for (uint8_t slot = 0; slot < GATEWAY_MAX_ACTUATORS; slot++) {
		send_frame(PROTO_TYPE_GET_STATUS, &slot, 1);
	}
}

void uart_link_get_discovered_all(void)
{
	uint8_t all = GET_DISCOVERED_ALL;

	if (!link_ready) {
		printk("[uart_link] link nao esta pronto, ignorando GET_DISCOVERED\n");
		return;
	}

	send_frame(PROTO_TYPE_GET_DISCOVERED, &all, 1);
}

void uart_link_get_status(uint8_t slot)
{
	if (!link_ready) {
		printk("[uart_link] link nao esta pronto, ignorando GET_STATUS\n");
		return;
	}

	send_frame(PROTO_TYPE_GET_STATUS, &slot, 1);
}

void uart_link_send_actuator_cmd(uint8_t slot, const uint8_t *data, uint8_t len)
{
	uint8_t payload[PROTO_MAX_PAYLOAD];

	if (!link_ready) {
		printk("[uart_link] link nao esta pronto, ignorando ACTUATOR_CMD\n");
		return;
	}
	if (len > PROTO_MAX_PAYLOAD - 1) {
		printk("[uart_link] ACTUATOR_CMD de %u bytes grande demais (max %u), descartado\n",
		       len, PROTO_MAX_PAYLOAD - 1);
		return;
	}

	payload[0] = slot;
	memcpy(&payload[1], data, len);
	send_frame(PROTO_TYPE_ACTUATOR_CMD, payload, (uint8_t)(len + 1));
}

void uart_link_send_actuator_manage(uint8_t cmd, uint8_t addr_type, const uint8_t mac_human[6])
{
	uint8_t payload[8];

	if (!link_ready) {
		printk("[uart_link] link nao esta pronto, ignorando ACTUATOR_MANAGE\n");
		return;
	}

	payload[0] = cmd;
	payload[1] = addr_type;
	/* Frame exige a ordem interna do stack (invertida em relacao ao
	 * AA:BB:CC:DD:EE:FF humano) - ver nota em format_mac() acima e em
	 * actuator_client.h do nrf54-app. */
	payload[2] = mac_human[5];
	payload[3] = mac_human[4];
	payload[4] = mac_human[3];
	payload[5] = mac_human[2];
	payload[6] = mac_human[1];
	payload[7] = mac_human[0];

	send_frame(PROTO_TYPE_ACTUATOR_MANAGE, payload, sizeof(payload));
}

void uart_link_send_gateway_ctrl(uint8_t cmd, const uint8_t *arg, uint8_t arg_len)
{
	uint8_t payload[PROTO_MAX_PAYLOAD];

	if (!link_ready) {
		printk("[uart_link] link nao esta pronto, ignorando GATEWAY_CTRL\n");
		return;
	}
	if (arg_len > PROTO_MAX_PAYLOAD - 1) {
		printk("[uart_link] GATEWAY_CTRL arg de %u bytes grande demais, descartado\n",
		       arg_len);
		return;
	}

	payload[0] = cmd;
	if (arg_len > 0) {
		memcpy(&payload[1], arg, arg_len);
	}
	send_frame(PROTO_TYPE_GATEWAY_CTRL, payload, (uint8_t)(arg_len + 1));
}
