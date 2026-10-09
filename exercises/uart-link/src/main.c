/*
 * nRF54LM20-DK
 * Botao local -> envia comando por UART para a nRF9151-SMA-DK
 * Comando recebido por UART -> controla o LED local
 *
 * Estrutura identica (de proposito) ao main.c do nRF9151, so troca
 * qual UART e' usada para o link dedicado. Excecao deliberada: so
 * este lado (nRF54) dispara o ciclo periodico de ECHO_STR - o
 * nRF9151 so responde ao que receber (ver handle_echo_str), pra nao
 * ter dois lados injetando trafego novo sem coordenacao.
 *
 * Frame LEN-variavel (migracao de 2026-08-11, ver docs/PROTOCOLO_54_91.md
 * no repositorio principal) - era fixo de 6 bytes com 1 byte de payload:
 *
 *   [0] SOF(0x7E)  [1] TYPE  [2] SEQ  [3] LEN  [4..4+LEN-1] PAYLOAD  [+2] CRC16
 *
 * CRC16 agora cobre TYPE+SEQ+LEN+PAYLOAD inteiro (antes so os 3 bytes
 * fixos TYPE+SEQ+CMD). O mecanismo de ACK+retry+fila continua o
 * mesmo, so generalizado pra guardar/reenviar um frame de tamanho
 * variavel em vez de 1 byte de comando fixo.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>

#define PROTO_SOF         0x7E
#define PROTO_HEADER_LEN  4 /* SOF + TYPE + SEQ + LEN */
#define PROTO_MAX_PAYLOAD 250
#define PROTO_CRC_LEN     2
#define PROTO_MAX_FRAME   (PROTO_HEADER_LEN + PROTO_MAX_PAYLOAD + PROTO_CRC_LEN) /* 256 */

#define RECEIVE_BUFF_SIZE PROTO_MAX_FRAME
#define RECEIVE_TIMEOUT   100
#define ACK_TIMEOUT_MS    300
#define MAX_RETRIES       3

#define PROTO_TYPE_CMD_LED  0x01
#define PROTO_TYPE_ACK      0x02
#define PROTO_TYPE_ECHO_STR 0x03 /* novo - so pra provar payload de tamanho variavel */
#define CMD_LED_ON  '1'
#define CMD_LED_OFF '0'

#define ECHO_REPLY_PREFIX     "echo:"
#define ECHO_PING_INTERVAL_MS 5000

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* uart30: instancia dedicada ao link, definida no overlay (nao e' o console) */
static const struct device *link_uart = DEVICE_DT_GET(DT_NODELABEL(uart30));

static struct gpio_callback button_cb_data;
static uint8_t rx_buf[RECEIVE_BUFF_SIZE];
static uint8_t rx_buf_next[RECEIVE_BUFF_SIZE];
static bool actuator_state;

/* --- RX: maquina de estados byte a byte ---
 *   0 = aguarda SOF
 *   1 = TYPE            2 = SEQ            3 = LEN
 *   4 = PAYLOAD (ate completar LEN bytes, pulado se LEN==0)
 *   5 = CRC_HI          6 = CRC_LO (finaliza, valida, despacha)
 *
 * rx_frame_buf acumula TYPE+SEQ+LEN+PAYLOAD (sem o SOF) pra o CRC ser
 * calculado de uma vez so no estado 6, em vez de incremental byte a
 * byte - mesmo trabalho, bem mais facil de revisar.
 *
 * Nota de robustez: ao contrario da versao anterior, o estado 1 nao
 * rejeita mais TYPE desconhecido na hora - isso agora e' resolvido no
 * dispatch (ver default case), porque a tabela de opcodes real vai
 * crescer bastante (ver docs/PROTOCOLO_54_91.md) e nao faz sentido a
 * camada de framing conhecer todo opcode valido. Efeito colateral
 * aceitavel: lixo na linha demora ate LEN+2 bytes a mais pra ser
 * descartado (pelo CRC) em vez de ser cortado na hora - o CRC16 ainda
 * pega isso com altissima probabilidade, so um pouco mais tarde.
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

#define OUTGOING_QUEUE_SIZE 4
struct queued_frame {
	uint8_t type;
	uint8_t payload[PROTO_MAX_PAYLOAD];
	uint8_t payload_len;
};
static struct queued_frame outgoing_queue[OUTGOING_QUEUE_SIZE];
static uint8_t outgoing_queue_head;
static uint8_t outgoing_queue_tail;
static uint8_t outgoing_queue_count;

/* Dedup por (tipo, seq) - dois opcodes diferentes com o mesmo SEQ
 * (coincidencia de wraparound) nao sao duplicata um do outro, por
 * isso o tipo entra na comparacao agora, nao so o SEQ. */
static bool have_last_command;
static uint8_t last_command_type;
static uint8_t last_command_sequence;

static struct k_work ack_work;
static struct k_work_delayable retry_work;
static struct k_work_delayable echo_ping_work;

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

#define DEBOUNCE_MS 200
static int64_t last_press_uptime;

static struct k_work button_work;

static void actuator_led_set(bool on)
{
	actuator_state = on;
	gpio_pin_set_dt(&led, on ? 1 : 0);
	printk("LED local -> %s\n", on ? "ON" : "OFF");
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
		printk("[proto] fila de frames cheia, descartando tipo=0x%02x\n", type);
		return;
	}

	f = &outgoing_queue[outgoing_queue_tail];
	f->type = type;
	f->payload_len = payload_len;
	memcpy(f->payload, payload, payload_len);
	outgoing_queue_tail = (outgoing_queue_tail + 1) % OUTGOING_QUEUE_SIZE;
	outgoing_queue_count++;
	printk("[proto] frame enfileirado: tipo=0x%02x (%u bytes)\n", type, payload_len);
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
		printk("[proto] payload grande demais (%u > %u), descartado\n", payload_len,
		       PROTO_MAX_PAYLOAD);
		return;
	}

	if (waiting_ack) {
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
		pending_tx_sequence = sequence;
		retry_count = 0;
		waiting_ack = true;
		k_work_reschedule(&retry_work, K_MSEC(ACK_TIMEOUT_MS));
	}
	printk("Frame enviado: tipo=0x%02x seq=%u len=%u crc=0x%04X (ret=%d)\n", type, sequence,
	       payload_len, crc, ret);
}

static void send_command(uint8_t command)
{
	uint8_t payload = command;

	send_frame(PROTO_TYPE_CMD_LED, &payload, 1);
}

static void send_ack_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	static uint8_t ack_frame[PROTO_HEADER_LEN + PROTO_CRC_LEN]; /* LEN=0, tamanho fixo */
	uint16_t crc;
	int ret;

	ack_frame[0] = PROTO_SOF;
	ack_frame[1] = PROTO_TYPE_ACK;
	ack_frame[2] = pending_ack_sequence;
	ack_frame[3] = 0;

	crc = proto_crc16(&ack_frame[1], 3);
	ack_frame[4] = (uint8_t)(crc >> 8);
	ack_frame[5] = (uint8_t)(crc & 0xFF);

	ret = uart_tx(link_uart, ack_frame, sizeof(ack_frame), SYS_FOREVER_US);
	printk("ACK enviado seq=%u crc=0x%04X (ret=%d)\n", pending_ack_sequence, crc, ret);
}

static void retry_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	if (!waiting_ack) {
		return;
	}

	if (retry_count >= MAX_RETRIES) {
		waiting_ack = false;
		printk("[proto] ACK nao recebido seq=%u; desisti apos %d retransmissoes\n",
		       pending_tx_sequence, MAX_RETRIES);
		send_next_queued_frame();
		return;
	}

	retry_count++;
	printk("[proto] retransmitindo seq=%u tentativa=%u/%u\n", pending_tx_sequence, retry_count,
	       MAX_RETRIES);
	if (uart_tx(link_uart, pending_frame, pending_frame_len, SYS_FOREVER_US) != 0) {
		printk("[proto] erro ao retransmitir seq=%u\n", pending_tx_sequence);
	}
	k_work_reschedule(&retry_work, K_MSEC(ACK_TIMEOUT_MS));
}

/* ECHO_STR: prova de payload de tamanho variavel nos dois sentidos.
 * Nao e' um eco recursivo infinito de proposito - quem recebe uma
 * string que ja comeca com "echo:" sabe que e' a resposta de um ciclo
 * que ELE mesmo iniciou (ou que o outro lado iniciou), e so loga; quem
 * recebe uma string SEM esse prefixo entende que e' um pedido novo e
 * responde exatamente uma vez. Isso fecha o ciclo sempre, sem
 * bagunçar o RTT com um ping-pong sem fim.
 */
static void handle_echo_str(const uint8_t *payload, uint8_t len)
{
	char text[40];
	char reply[40];
	uint8_t copy_len = (len < sizeof(text) - 1) ? len : (uint8_t)(sizeof(text) - 1);
	int reply_len;

	memcpy(text, payload, copy_len);
	text[copy_len] = '\0';

	if (strncmp(text, ECHO_REPLY_PREFIX, strlen(ECHO_REPLY_PREFIX)) == 0) {
		printk("[proto] eco de volta recebido: '%s' (%u bytes) - ciclo completo\n", text,
		       len);
		return;
	}

	printk("[proto] echo recebido: '%s' (%u bytes) - respondendo\n", text, len);

	reply_len = snprintk(reply, sizeof(reply), "%s%s", ECHO_REPLY_PREFIX, text);
	if (reply_len < 0) {
		return;
	}
	if (reply_len > (int)sizeof(reply) - 1) {
		reply_len = (int)sizeof(reply) - 1;
	}

	send_frame(PROTO_TYPE_ECHO_STR, (const uint8_t *)reply, (uint8_t)reply_len);
}

static void dispatch_frame(uint8_t type, uint8_t sequence, const uint8_t *payload, uint8_t len)
{
	bool duplicate;

	if (type == PROTO_TYPE_ACK) {
		if (waiting_ack && sequence == pending_tx_sequence) {
			waiting_ack = false;
			k_work_cancel_delayable(&retry_work);
			printk("[proto] ACK recebido seq=%u\n", sequence);
			send_next_queued_frame();
		} else {
			printk("[proto] ACK inesperado seq=%u\n", sequence);
		}
		return;
	}

	duplicate = have_last_command && (type == last_command_type) &&
		    (sequence == last_command_sequence);

	if (duplicate) {
		printk("[proto] comando duplicado tipo=0x%02x seq=%u; reenviando ACK\n", type,
		       sequence);
	} else {
		switch (type) {
		case PROTO_TYPE_CMD_LED:
			if (len >= 1) {
				if (payload[0] == CMD_LED_ON) {
					actuator_led_set(true);
				} else if (payload[0] == CMD_LED_OFF) {
					actuator_led_set(false);
				}
			}
			break;
		case PROTO_TYPE_ECHO_STR:
			handle_echo_str(payload, len);
			break;
		default:
			printk("[proto] opcode desconhecido 0x%02x (%u bytes)\n", type, len);
			break;
		}
		last_command_type = type;
		last_command_sequence = sequence;
		have_last_command = true;
	}

	pending_ack_sequence = sequence;
	k_work_submit(&ack_work);
}

static void echo_ping_work_handler(struct k_work *work)
{
	static uint32_t counter;
	char text[24];
	int len;

	len = snprintk(text, sizeof(text), "ping-%u", counter++);
	if (len > 0) {
		send_frame(PROTO_TYPE_ECHO_STR, (const uint8_t *)text, (uint8_t)len);
	}

	k_work_reschedule(&echo_ping_work, K_MSEC(ECHO_PING_INTERVAL_MS));
}

static void button_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	/* Alterna o comando a ser enviado com base no estado atual armazenado */
	send_command(actuator_state ? CMD_LED_OFF : CMD_LED_ON);
	actuator_state = !actuator_state;
}

static void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	int64_t now = k_uptime_get();

	if ((now - last_press_uptime) < DEBOUNCE_MS) {
		return;
	}
	last_press_uptime = now;

	k_work_submit(&button_work);
}

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
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
					printk("[proto] LEN invalido (%u), descartando frame\n",
					       rx_len);
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
					printk("[proto] CRC16 invalido: recebido=0x%04X "
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
	case UART_RX_BUF_RELEASED:
		break;
	case UART_TX_DONE:
		printk("[debug] UART_TX_DONE: %d byte(s) transmitidos\n", evt->data.tx.len);
		break;
	case UART_TX_ABORTED:
		printk("[debug] UART_TX_ABORTED: transmissao NAO completou\n");
		break;
	case UART_RX_STOPPED:
		printk("[debug] UART_RX_STOPPED, reason=%d\n", evt->data.rx_stop.reason);
		break;
	default:
		printk("[debug] evento UART nao tratado: %d\n", evt->type);
		break;
	}
}

int main(void)
{
	int ret;

	if (!device_is_ready(link_uart)) {
		printk("UART de link nao esta pronta\n");
		return 0;
	}
	if (!gpio_is_ready_dt(&led) || !gpio_is_ready_dt(&button)) {
		printk("GPIO nao esta pronto\n");
		return 0;
	}

	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&button, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	k_work_init(&button_work, button_work_handler);
	k_work_init(&ack_work, send_ack_work_handler);
	k_work_init_delayable(&retry_work, retry_work_handler);
	k_work_init_delayable(&echo_ping_work, echo_ping_work_handler);

	ret = uart_callback_set(link_uart, uart_cb, NULL);
	if (ret) {
		printk("Erro ao registrar callback UART (%d)\n", ret);
		return 0;
	}

	ret = uart_rx_enable(link_uart, rx_buf, sizeof(rx_buf), RECEIVE_TIMEOUT);
	if (ret) {
		printk("Erro ao habilitar RX UART (%d)\n", ret);
		return 0;
	}

	printk("nRF54LM20: pronto. Botao envia comando remoto; UART controla LED local; "
	       "ciclo periodico de ECHO_STR a cada %d ms valida o frame LEN-variavel.\n",
	       ECHO_PING_INTERVAL_MS);

	k_work_reschedule(&echo_ping_work, K_MSEC(ECHO_PING_INTERVAL_MS));

	while (1) {
		k_sleep(K_FOREVER);
	}
}
