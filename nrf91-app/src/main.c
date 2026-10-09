/*
 * nRF9151-SMA-DK
 *
 * Modulo espelho do link UART com o nRF54 (Gateway, Fase 4) +
 * integracao MQTT (docs/PROTOCOLO_91_MQTT.md). Logica de framing/
 * dispatch UART mora em uart_link.c, logica de LTE/MQTT em
 * mqtt_client.c; este main.c so inicializa os dois e cuida dos botoes
 * fisicos, que continuam existindo como gatilho MANUAL (o gatilho
 * principal agora e' "ao conectar no broker MQTT", dentro de
 * mqtt_client.c):
 *
 *   Botao 1 -> GET_STATUS de cada slot do Gateway, em sequencia
 *   Botao 2 -> GET_DISCOVERED (todos os atuadores vistos no scan)
 *
 * Os registros que chegam do 54 sao decodificados em uart_link.c, que
 * publica no MQTT (mqtt_client_publish_*) alem de logar no terminal.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>

#include "uart_link.h"
#include "mqtt_client.h"

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec button2 = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);

static struct gpio_callback button_cb_data;
static struct gpio_callback button2_cb_data;

/* Debounce por tempo, compartilhado entre os dois botoes - mesmo
 * padrao ja usado no exercicio original. */
#define DEBOUNCE_MS 200
static int64_t last_press_uptime;

static struct k_work button_work;
static struct k_work button2_work;

static void button_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	printk("Botao 1: pedindo GET_STATUS de todos os slots...\n");
	uart_link_get_status_all();
}

static void button2_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	printk("Botao 2: pedindo GET_DISCOVERED (todos)...\n");
	uart_link_get_discovered_all();
}

/* ISR: so faz o minimo (debounce + agendar work) - o pedido de
 * verdade (uart_tx etc.) roda na system workqueue. */
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

static void button2_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	int64_t now = k_uptime_get();

	if ((now - last_press_uptime) < DEBOUNCE_MS) {
		return;
	}
	last_press_uptime = now;

	k_work_submit(&button2_work);
}

int main(void)
{
	int ret;

	if (!gpio_is_ready_dt(&button) || !gpio_is_ready_dt(&button2)) {
		printk("GPIO nao esta pronto\n");
		return 0;
	}

	gpio_pin_configure_dt(&button, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);

	gpio_pin_configure_dt(&button2, GPIO_INPUT);
	gpio_pin_interrupt_configure_dt(&button2, GPIO_INT_EDGE_TO_ACTIVE);

	gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb_data);

	gpio_init_callback(&button2_cb_data, button2_pressed, BIT(button2.pin));
	gpio_add_callback(button2.port, &button2_cb_data);

	k_work_init(&button_work, button_work_handler);
	k_work_init(&button2_work, button2_work_handler);

	/* UART primeiro (rapido, nao bloqueia) - assim o link com o 54 ja
	 * esta pronto pra receber antes do proximo passo, que bloqueia
	 * esperando a rede LTE registrar. */
	ret = uart_link_init();
	if (ret) {
		printk("uart_link_init falhou (err %d) - sem link com o 54 por enquanto\n", ret);
	}

	printk("nRF9151: pronto. Botao 1 = GET_STATUS (todos os slots), "
	       "botao 2 = GET_DISCOVERED (todos) - gatilho manual, o principal e' "
	       "conectar no broker MQTT. Conectando...\n");

	ret = mqtt_client_start();
	if (ret) {
		printk("mqtt_client_start falhou (err %d) - sem MQTT por enquanto, "
		       "so UART/terminal\n", ret);
	}

	while (1) {
		k_sleep(K_FOREVER);
	}
}
