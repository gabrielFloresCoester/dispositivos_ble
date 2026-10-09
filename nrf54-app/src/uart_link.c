/*
 * Coester - SIM Connect (Gateway)
 *
 * Link UART com o nRF9151 (uart30) - implementa a tabela de opcodes de
 * docs/PROTOCOLO_54_91.md (repositorio principal). O mecanismo de
 * framing (SOF+TYPE+SEQ+LEN+PAYLOAD+CRC16, ACK+retry+fila) e' o mesmo
 * ja provado em exercises/uart-link, so generalizado para a tabela de
 * opcodes 0x10-0x19 em vez do CMD_LED/ECHO_STR de teste.
 *
 * --- Por que o dispatch de opcode roda na workqueue, nao no callback
 *     da UART direto (diferente do exercicio) ---
 *
 * Confirmado em C:\ncs\v3.3.0\zephyr\drivers\serial\uart_nrfx_uarte.c
 * (endrx_isr -> user_callback): o callback assincrono desta UARTE roda
 * em CONTEXTO DE INTERRUPCAO. No exercicio isso era seguro porque
 * dispatch_frame() so tocava GPIO e printk (ambos ISR-safe). Aqui os
 * handlers de opcode chamam actuator_manager_*()/host Bluetooth
 * (bt_conn_le_create, bt_gatt_write, etc.), que NAO sao ISR-safe -
 * esperam contexto de thread (tomam mutex/semaforo, submetem para
 * outras workqueues). Por isso dispatch_frame() (chamado direto do
 * ISR, via uart_cb) so faz a parte generica e ja validada em bancada
 * na Fase 2 (ACK, dedup, retry - tudo GPIO/uart_tx-equivalente) e
 * copia o frame recebido para rx_cmd_work_handler(), que roda na
 * system workqueue (contexto de thread) e so ai chama os handlers de
 * opcode de verdade.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/addr.h>

#include "uart_link.h"
#include "actuator_client.h"
#include "my_lbs.h" /* reusa os defines GATEWAY_CTRL_* (0x01-0x05) */

LOG_MODULE_REGISTER(uart_link, LOG_LEVEL_INF);

/* Definido em main.c - wrapper fino sobre bt_scan_stop()+scan_start()
 * (scan_start() e' static la, uso interno do papel Peripheral/scan).
 * Precisamos dele aqui em dois pontos: GATEWAY_CTRL_RESTART_SCAN
 * (0x02, espelha o que app_gateway_ctrl_cb ja faz pela interface local)
 * e ACTUATOR_MANAGE de adicao (0x12/cmd=0x01), pelo mesmo motivo que
 * app_actuator_manage_cb chama scan_start() apos adicionar - senao o
 * atuador recem-adicionado so seria visto no proximo ciclo de scan que
 * ja estivesse rodando por outro motivo.
 */
extern void app_restart_scan(void);

/* --- Framing: SOF+TYPE+SEQ+LEN+PAYLOAD(0..250)+CRC16 --- */

#define PROTO_SOF         0x7E
#define PROTO_HEADER_LEN  4 /* SOF + TYPE + SEQ + LEN */
#define PROTO_MAX_PAYLOAD 250
#define PROTO_CRC_LEN     2
#define PROTO_MAX_FRAME   (PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN) /* 256 */

#define RECEIVE_BUFF_SIZE PROTO_MAX_FRAME
#define RECEIVE_TIMEOUT   100
#define ACK_TIMEOUT_MS    300
#define MAX_RETRIES       3

/* --- Opcodes: tabela completa de docs/PROTOCOLO_54_91.md --- */

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

/* Codigos de erro do payload de *_RESULT. Erros vindos de
 * actuator_manager_*() usam o proprio -errno (cabe em 1 byte pros
 * codigos que essas funcoes retornam hoje); os que seguem sao
 * especificos deste link, sem -errno correspondente.
 */
#define UART_LINK_ERR_OK          0x00
#define UART_LINK_ERR_BAD_ARGS    0xFC
#define UART_LINK_ERR_UNKNOWN_CMD 0xFD
#define UART_LINK_ERR_UNSUPPORTED 0xFE /* GATEWAY_CTRL_SET_NAME via UART, fora de escopo nesta fase */

static const struct device *link_uart = DEVICE_DT_GET(DT_NODELABEL(uart30));
static bool link_ready;

static uint8_t rx_buf[RECEIVE_BUFF_SIZE];
static uint8_t rx_buf_next[RECEIVE_BUFF_SIZE];

/* --- RX: maquina de estados byte a byte, identica em espirito a
 * exercises/uart-link (ver o comentario completo la) ---
 *   0=aguarda SOF  1=TYPE  2=SEQ  3=LEN  4=PAYLOAD  5=CRC_HI  6=CRC_LO
 */
static uint8_t rx_state;
static uint8_t rx_frame_buf[3 + PROTO_MAX_PAYLOAD]; /* TYPE+SEQ+LEN+PAYLOAD */
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

/* Fila de saida maior que a do exercicio (4): GET_DISCOVERED(0xFF)
 * pode gerar ate MAX_DISCOVERED (12) frames DISCOVERED_RECORD de uma
 * vez, e o mecanismo so transmite 1 por vez (stop-and-wait) - o
 * restante fica enfileirado ate cada ACK chegar. 16 cobre isso com
 * folga para uma push assincrona concorrente (status/discovery/raw
 * data disparados pelos callbacks do actuator_client) sem descartar.
 */
#define OUTGOING_QUEUE_SIZE 16
struct queued_frame {
	uint8_t type;
	uint8_t payload[PROTO_MAX_PAYLOAD];
	uint8_t payload_len;
};
static struct queued_frame outgoing_queue[OUTGOING_QUEUE_SIZE];
static uint8_t outgoing_queue_head;
static uint8_t outgoing_queue_tail;
static uint8_t outgoing_queue_count;

/* Dedup por (tipo, seq) - ver nota identica em exercises/uart-link. */
static bool have_last_command;
static uint8_t last_command_type;
static uint8_t last_command_sequence;

static struct k_work ack_work;
static struct k_work_delayable retry_work;

static uint16_t proto_crc16(const uint8_t *data, size_t len);

/* Gate fisico do uart_tx() - PRECISA valer para as TRES origens que
 * chamam uart_tx() (envio novo em send_frame, retransmissao em
 * retry_work_handler, ACK em send_ack_work_handler), nao so para
 * "esperando ACK do outro lado" (waiting_ack, que e' uma coisa
 * diferente). Achado em bancada (2026-08-12): sem este gate, um ACK
 * podia colidir com uma transmissao de frame de dado ja em voo -
 * uart_tx() retorna -EBUSY de forma limpa nesse caso (confirmado no
 * driver, C:\ncs\v3.3.0\zephyr\drivers\serial\uart_nrfx_uarte.c:1698,
 * nao corrompe nada), mas o ACK era descartado em silencio em vez de
 * ser reenviado - e o volume de pushes de descoberta da Fase 3 (bem
 * maior que o do exercicio) tornava essa colisao comum, nao rara.
 */
static bool tx_busy;
static bool ack_send_pending;

static void try_send_ack(void)
{
	static uint8_t ack_frame[PROTO_HEADER_LEN + PROTO_CRC_LEN]; /* LEN=0, tamanho fixo */
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
		/* -EBUSY (colisao com outra transmissao) ou outro erro -
		 * tenta de novo quando o canal fisico liberar (ver
		 * UART_TX_DONE/UART_TX_ABORTED em uart_cb).
		 */
		ack_send_pending = true;
	}
}

/* Frame de comando recebido, copiado do contexto ISR do uart_cb para
 * ser processado na system workqueue (ver nota grande no topo do
 * arquivo). So existe 1 "em voo" por vez: o remetente (91) so manda o
 * proximo depois de receber nosso ACK, e so mandamos o ACK depois de
 * processar este (ver rx_cmd_work_handler) - dedup acima cobre
 * retransmissoes que cheguem enquanto ainda estamos processando.
 */
static struct {
	uint8_t type;
	uint8_t sequence;
	uint8_t len;
	uint8_t payload[PROTO_MAX_PAYLOAD];
} rx_cmd;
static struct k_work rx_cmd_work;

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
		LOG_WRN("fila de saida cheia, descartando frame tipo=0x%02x", type);
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
		LOG_WRN("payload grande demais (%u > %u), descartado", payload_len,
			PROTO_MAX_PAYLOAD);
		return;
	}

	/* waiting_ack: ja tem um frame nosso esperando ACK do outro lado.
	 * tx_busy: o canal fisico esta ocupado NESTE INSTANTE (colisao com
	 * uma transmissao ja em voo - normalmente um ACK). Os dois motivos
	 * enfileiram do mesmo jeito; quem drena depois e' sempre
	 * send_next_queued_frame() (via ACK recebido, retry esgotado, ou
	 * o canal fisico liberando - ver UART_TX_DONE em uart_cb).
	 */
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
		LOG_WRN("uart_tx falhou (err %d) para frame tipo=0x%02x seq=%u", ret, type,
			sequence);
	}
}

static void send_result(uint8_t result_type, uint8_t seq_original, uint8_t err_code)
{
	uint8_t payload[2] = { seq_original, err_code };

	send_frame(result_type, payload, sizeof(payload));
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
		/* Canal ocupado neste tick (colisao rara com um ACK saindo
		 * ao mesmo tempo) - tenta de novo no proximo, sem gastar uma
		 * das MAX_RETRIES por isso.
		 */
		k_work_reschedule(&retry_work, K_MSEC(ACK_TIMEOUT_MS));
		return;
	}

	if (retry_count >= MAX_RETRIES) {
		waiting_ack = false;
		LOG_WRN("ACK nao recebido seq=%u; desisti apos %d retransmissoes", pending_tx_sequence,
			MAX_RETRIES);
		send_next_queued_frame();
		return;
	}

	retry_count++;
	LOG_DBG("retransmitindo seq=%u tentativa=%u/%u", pending_tx_sequence, retry_count, MAX_RETRIES);
	if (uart_tx(link_uart, pending_frame, pending_frame_len, SYS_FOREVER_US) == 0) {
		tx_busy = true;
	} else {
		LOG_WRN("erro ao retransmitir seq=%u", pending_tx_sequence);
	}
	k_work_reschedule(&retry_work, K_MSEC(ACK_TIMEOUT_MS));
}

/* --- Handlers de opcode (rodam na system workqueue - ver nota do topo) --- */

static void handle_actuator_cmd(const uint8_t *payload, uint8_t len)
{
	uint8_t slot;
	int err;

	if (len < 1) {
		LOG_WRN("ACTUATOR_CMD sem slot informado");
		return;
	}

	slot = payload[0];
	err = actuator_manager_send_to(slot, &payload[1], len - 1);

	if (err == -ENOTCONN) {
		LOG_WRN("ACTUATOR_CMD para o slot %u ignorado: atuador nao esta PRONTO", slot);
	} else if (err) {
		LOG_WRN("Falha ao repassar ACTUATOR_CMD ao slot %u (err %d)", slot, err);
	} else {
		LOG_INF("ACTUATOR_CMD de %u bytes repassado ao slot %u (via UART)", len - 1, slot);
	}
}

static void handle_actuator_manage(uint8_t sequence, const uint8_t *payload, uint8_t len)
{
	bt_addr_le_t addr;
	uint8_t cmd;
	int err;

	if (len < 8) {
		LOG_WRN("ACTUATOR_MANAGE com payload curto (%u bytes, esperado 8)", len);
		send_result(PROTO_TYPE_MANAGE_RESULT, sequence, UART_LINK_ERR_BAD_ARGS);
		return;
	}

	cmd = payload[0];
	addr.type = payload[1];
	memcpy(addr.a.val, &payload[2], sizeof(addr.a.val));

	if (cmd == 0x01) {
		err = actuator_manager_add(&addr);
	} else {
		err = actuator_manager_remove(&addr);
	}

	if (err) {
		LOG_WRN("ACTUATOR_MANAGE (cmd=0x%02x, via UART) falhou (err %d)", cmd, err);
	} else if (cmd == 0x01) {
		/* Mesma logica de app_actuator_manage_cb (main.c) pro caminho
		 * BLE: sem isto, o atuador recem-adicionado so seria visto no
		 * proximo evento que retomasse o scan por outro motivo.
		 */
		app_restart_scan();
	}

	send_result(PROTO_TYPE_MANAGE_RESULT, sequence, err ? (uint8_t)(-err) : UART_LINK_ERR_OK);
}

static void handle_get_status(const uint8_t *payload, uint8_t len)
{
	/* static: 19*41=779 bytes, evita empilhar isso na stack da
	 * workqueue so para ler 1 registro. */
	static uint8_t status_buf[MAX_ACTUATORS * ACTUATOR_STATUS_REC_LEN];
	uint32_t offset;
	uint16_t got;
	uint8_t slot;

	if (len < 1) {
		LOG_WRN("GET_STATUS sem slot informado");
		return;
	}

	slot = payload[0];
	if (slot >= MAX_ACTUATORS) {
		LOG_WRN("GET_STATUS com slot invalido (%u, max %u)", slot, MAX_ACTUATORS - 1);
		return;
	}

	got = actuator_manager_get_status_all(status_buf, sizeof(status_buf));
	offset = (uint32_t)slot * ACTUATOR_STATUS_REC_LEN;

	if (offset + ACTUATOR_STATUS_REC_LEN > got) {
		LOG_WRN("GET_STATUS slot %u fora do buffer devolvido (%u bytes)", slot, got);
		return;
	}

	send_frame(PROTO_TYPE_STATUS_RECORD, &status_buf[offset], ACTUATOR_STATUS_REC_LEN);
}

static void handle_get_discovered(const uint8_t *payload, uint8_t len)
{
	/* static: 12*41=492 bytes, mesmo motivo do status_buf acima. */
	static uint8_t discovery_buf[MAX_DISCOVERED * ACTUATOR_DISCOVERY_REC_LEN];
	uint16_t got;
	uint16_t count;
	uint8_t index;

	if (len < 1) {
		LOG_WRN("GET_DISCOVERED sem indice informado");
		return;
	}

	index = payload[0];
	got = actuator_manager_get_discovery_all(discovery_buf, sizeof(discovery_buf));
	count = got / ACTUATOR_DISCOVERY_REC_LEN;

	if (index == GET_DISCOVERED_ALL) {
		for (uint16_t i = 0; i < count; i++) {
			send_frame(PROTO_TYPE_DISCOVERED_RECORD,
				   &discovery_buf[i * ACTUATOR_DISCOVERY_REC_LEN],
				   ACTUATOR_DISCOVERY_REC_LEN);
		}
		return;
	}

	if (index >= count) {
		LOG_WRN("GET_DISCOVERED indice %u fora do intervalo (%u descoberto(s))", index, count);
		return;
	}

	send_frame(PROTO_TYPE_DISCOVERED_RECORD, &discovery_buf[index * ACTUATOR_DISCOVERY_REC_LEN],
		   ACTUATOR_DISCOVERY_REC_LEN);
}

static void handle_gateway_ctrl(uint8_t sequence, const uint8_t *payload, uint8_t len)
{
	uint8_t cmd;
	const uint8_t *arg;
	uint16_t arg_len;
	uint8_t err_code = UART_LINK_ERR_OK;
	int err;

	if (len < 1) {
		send_result(PROTO_TYPE_GATEWAY_CTRL_RESULT, sequence, UART_LINK_ERR_BAD_ARGS);
		return;
	}

	cmd = payload[0];
	arg = &payload[1];
	arg_len = len - 1;

	switch (cmd) {
	case GATEWAY_CTRL_CLEAR_DISCOVERY:
		actuator_manager_clear_discovery();
		break;

	case GATEWAY_CTRL_RESTART_SCAN:
		LOG_INF("Reinicio de scan solicitado pelo 91 (via UART)");
		app_restart_scan();
		break;

	case GATEWAY_CTRL_DISCONNECT_SLOT:
		if (arg_len < 1) {
			err_code = UART_LINK_ERR_BAD_ARGS;
			break;
		}
		err = actuator_manager_disconnect(arg[0]);
		if (err) {
			err_code = (uint8_t)(-err);
		}
		break;

	case GATEWAY_CTRL_SET_SPEED:
		if (arg_len < 2) {
			err_code = UART_LINK_ERR_BAD_ARGS;
			break;
		}
		err = actuator_manager_set_speed(arg[0], arg[1] ? ACTUATOR_SPEED_FAST
								  : ACTUATOR_SPEED_SLOW);
		if (err) {
			err_code = (uint8_t)(-err);
		}
		break;

	case GATEWAY_CTRL_SET_NAME:
		/* Fora de escopo nesta fase - bt_set_name()/gateway_name_save()
		 * sao static em main.c, e trocar nome nao e prioridade pro
		 * link UART agora (ver docs/PROTOCOLO_54_91.md).
		 */
		LOG_WRN("GATEWAY_CTRL_SET_NAME via UART nao suportado nesta fase");
		err_code = UART_LINK_ERR_UNSUPPORTED;
		break;

	default:
		LOG_WRN("GATEWAY_CTRL via UART: comando desconhecido (0x%02x)", cmd);
		err_code = UART_LINK_ERR_UNKNOWN_CMD;
		break;
	}

	send_result(PROTO_TYPE_GATEWAY_CTRL_RESULT, sequence, err_code);
}

static void rx_cmd_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	switch (rx_cmd.type) {
	case PROTO_TYPE_ACTUATOR_CMD:
		handle_actuator_cmd(rx_cmd.payload, rx_cmd.len);
		break;
	case PROTO_TYPE_ACTUATOR_MANAGE:
		handle_actuator_manage(rx_cmd.sequence, rx_cmd.payload, rx_cmd.len);
		break;
	case PROTO_TYPE_GET_STATUS:
		handle_get_status(rx_cmd.payload, rx_cmd.len);
		break;
	case PROTO_TYPE_GET_DISCOVERED:
		handle_get_discovered(rx_cmd.payload, rx_cmd.len);
		break;
	case PROTO_TYPE_GATEWAY_CTRL:
		handle_gateway_ctrl(rx_cmd.sequence, rx_cmd.payload, rx_cmd.len);
		break;
	default:
		LOG_WRN("opcode desconhecido 0x%02x (%u bytes)", rx_cmd.type, rx_cmd.len);
		break;
	}

	/* ACK so vai depois do processamento (ver nota do topo do
	 * arquivo): o remetente so reenvia se isto estourar 300ms
	 * (ACK_TIMEOUT_MS), folga enorme pra qualquer handler acima.
	 */
	pending_ack_sequence = rx_cmd.sequence;
	k_work_submit(&ack_work);
}

/* --- dispatch_frame: roda em contexto ISR (chamado de uart_cb) - so a
 * parte generica/ja validada em bancada (ACK, dedup) fica aqui. Ver
 * nota grande no topo do arquivo. ---
 */
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
		/* Ja processado (ou em processamento) - so reenvia o ACK,
		 * sem rechamar os handlers de opcode.
		 */
		pending_ack_sequence = sequence;
		k_work_submit(&ack_work);
		return;
	}

	last_command_type = type;
	last_command_sequence = sequence;
	have_last_command = true;

	rx_cmd.type = type;
	rx_cmd.sequence = sequence;
	rx_cmd.len = len;
	memcpy(rx_cmd.payload, payload, len);
	k_work_submit(&rx_cmd_work);
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
					LOG_WRN("CRC16 invalido: recebido=0x%04X esperado=0x%04X",
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
		/* Removido por engano ao simplificar em cima do exercicio -
		 * sem isto ficamos cegos pro mesmo tipo de erro que o 91 esta
		 * reportando (reason=4/UART_ERROR_FRAMING) se ele tambem
		 * estiver acontecendo do lado do 54, que roda BLE Central+
		 * Peripheral simultaneamente (o exercicio, sem Bluetooth
		 * nenhum, nao tem essa fonte de interferencia/contencao).
		 */
		LOG_WRN("UART_RX_STOPPED, reason=%d", evt->data.rx_stop.reason);
		break;
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		/* Libera o gate fisico (ver nota grande no topo do arquivo) e
		 * destrava quem ficou esperando: um ACK que colidiu com esta
		 * transmissao tem prioridade; se nao havia ACK pendente e
		 * tambem nao estamos esperando ACK do outro lado para um
		 * frame nosso, tenta drenar a fila de saida (cobre o caso
		 * raro de um frame de dado ter sido reenfileirado por causa
		 * desta mesma colisao).
		 */
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
		LOG_WRN("uart30 nao esta pronta - link com o 91 desabilitado (overlay ausente/errado? "
			"ver nrf54lm20dk_nrf54lm20b_cpuapp_ns.overlay - repare o \"_ns\")");
		return -ENODEV;
	}

	k_work_init(&ack_work, send_ack_work_handler);
	k_work_init(&rx_cmd_work, rx_cmd_work_handler);
	k_work_init_delayable(&retry_work, retry_work_handler);

	err = uart_callback_set(link_uart, uart_cb, NULL);
	if (err) {
		LOG_WRN("Falha ao registrar callback da uart30 (err %d) - link com o 91 desabilitado",
			err);
		return err;
	}

	err = uart_rx_enable(link_uart, rx_buf, sizeof(rx_buf), RECEIVE_TIMEOUT);
	if (err) {
		LOG_WRN("Falha ao habilitar RX da uart30 (err %d) - link com o 91 desabilitado", err);
		return err;
	}

	link_ready = true;
	LOG_INF("Link UART com o 91 pronto (uart30)");
	return 0;
}

void uart_link_send_status(const uint8_t *rec, uint16_t len)
{
	if (!link_ready) {
		return;
	}
	if (len > PROTO_MAX_PAYLOAD) {
		LOG_WRN("registro de status de %u bytes nao cabe no link UART (max %u)", len,
			PROTO_MAX_PAYLOAD);
		return;
	}
	send_frame(PROTO_TYPE_STATUS_RECORD, rec, (uint8_t)len);
}

void uart_link_send_discovery(const uint8_t *rec, uint16_t len)
{
	if (!link_ready) {
		return;
	}
	if (len > PROTO_MAX_PAYLOAD) {
		LOG_WRN("registro de descoberta de %u bytes nao cabe no link UART (max %u)", len,
			PROTO_MAX_PAYLOAD);
		return;
	}
	send_frame(PROTO_TYPE_DISCOVERED_RECORD, rec, (uint8_t)len);
}

void uart_link_send_raw_data(uint8_t slot, const uint8_t *data, uint16_t len)
{
	uint8_t payload[PROTO_MAX_PAYLOAD];

	if (!link_ready) {
		return;
	}
	if (len > PROTO_MAX_PAYLOAD - 1) {
		LOG_WRN("resposta de %u bytes do slot %u grande demais pro link UART (max %u), "
			"descartada", len, slot, PROTO_MAX_PAYLOAD - 1);
		return;
	}

	payload[0] = slot;
	memcpy(&payload[1], data, len);
	send_frame(PROTO_TYPE_RAW_DATA, payload, (uint8_t)(len + 1));
}
