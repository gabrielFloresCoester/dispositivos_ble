/*
 * Coester - SIM Connect (Gateway)
 *
 * Papeis do Gateway:
 *   - Peripheral: expoe o servico my_lbs a interface (navegador/celular).
 *   - Central: ate MAX_ACTUATORS atuadores REAIS (produto Coester,
 *     nRF52832, protocolo proCo/ifFerConfig sobre servico proprietario
 *     estilo NUS), gerenciados por uma allow-list dinamica.
 *
 * Os Kits B e C (exercicio original da Nordic) foram aposentados: eles
 * ocupavam duas das 20 conexoes do controlador e dois filtros de scan
 * por nome sem fazer parte do produto. Com isso, TODA conexao Central
 * do Gateway e um atuador - o que dispensa a tabela de correlacao
 * endereco->alvo que existia aqui antes.
 *
 * As characteristics de LED/Button/MySensor continuam declaradas em
 * my_lbs.c de proposito: remove-las deslocaria os indices de
 * my_lbs_svc.attrs[] usados nos notifies, um risco gratuito. Os
 * callbacks correspondentes viraram stubs.
 *
 * NOTA DE ARQUITETURA - quem dirige a coleta:
 *   O protocolo proCo nao tem ID de correlacao: a resposta que chega no
 *   TX nao diz a qual comando pertence. Consequencia: leitura periodica
 *   e leitura sob demanda NAO podem ter dois donos. O Gateway e um relay
 *   puro e QUEM DIRIGE E A INTERFACE, serializando por slot.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/settings/settings.h>
#include <dk_buttons_and_leds.h>
#include <bluetooth/scan.h>
#include <bluetooth/gatt_dm.h>

#include "my_lbs.h"
#include "actuator_client.h"
#include "uart_link.h"
#include "position_sensor.h"
#include "torque_sensor.h"
#include "torque_onoff_sensor.h"
#include "actuator_sensors.h"
#include "actuator_params.h"
#include "actuator_fsa_cfg.h"
#include "watchdog.h"
#include "actuator_regevent.h"
#include "sensor_workq.h"
#include "actuator_motor.h"
#include "actuator_alarm.h"
#include "actuator_control.h"
#include "actuator_panel.h"
#include "actuator_mode.h"

LOG_MODULE_REGISTER(Coester_Gateway, LOG_LEVEL_INF);

/* --- Advertising (papel Peripheral) ---
 *
 * Nota sobre BT_LE_ADV_OPT_ONE_TIME: essa flag impediria o host do
 * Zephyr de tentar retomar a advertising sozinho em paralelo com o
 * nosso próprio código (que já faz isso via .recycled, mais abaixo) -
 * mas o compilador deste projeto não a reconhece nesta combinação de
 * Kconfig/versão, e "BT_LE_ADV_OPT_ONE_TIME" é um valor de enum, não
 * uma macro de pré-processador, então não dá para proteger o uso dela
 * com um simples #if defined(). Por isso ela foi removida daqui, e a
 * correção passa a depender só das outras duas mudanças abaixo:
 * eliminar o restart prematuro em on_disconnected() (a causa mais
 * provável do -ENOMEM, confirmada pela própria documentação do Zephyr:
 * "the stack still has one reference to the connection object" nesse
 * callback) e a retentativa com backoff para o caso de recurso
 * genuinamente esgotado.
 */
static const struct bt_le_adv_param *adv_param = BT_LE_ADV_PARAM(
	(BT_LE_ADV_OPT_CONN | BT_LE_ADV_OPT_USE_IDENTITY),
	800, /* 500 ms */
	801, /* 500.625 ms */
	NULL);

#define RUN_STATUS_LED DK_LED1
#define CON_STATUS_LED_PHONE DK_LED2    /* aceso = interface conectada       */
#define CON_STATUS_LED_ACTUATOR DK_LED3 /* aceso = ao menos 1 atuador PRONTO */

#define RUN_LED_BLINK_INTERVAL 1000

/* Retentativa com backoff quando o controlador recusa habilitar a
 * advertising por falta de recurso (status HCI 0x0D, vira -ENOMEM no
 * host) - situação transitória e esperada logo após o boot, quando
 * vários atuadores reconectam quase ao mesmo tempo e o controlador
 * está ocupado com isso. Sem a retentativa, esse -ENOMEM simplesmente
 * desistia e a interface ficava sem conseguir conectar até o próximo
 * evento que disparasse advertising_start() de novo.
 */
#define ADV_RETRY_BASE_MS 300
#define ADV_RETRY_MAX_MS  2000

static struct k_work_delayable adv_work;
static uint8_t adv_retry_count;
static struct bt_conn *phone_conn;

static void scan_start(void);

/* NAO e mais "static const": o nome anunciado precisa refletir
 * bt_get_name() em tempo real, nao o CONFIG_BT_DEVICE_NAME de
 * compilacao - senao a interface consegue trocar o nome (characteristic
 * GAP padrao, ver prj.conf) mas o pacote de advertising continua
 * mostrando o nome de fabrica pra sempre, porque bt_le_adv_start() so
 * copia os bytes que a gente entrega a ele; nao existe um vinculo
 * automatico com bt_dev.name. Por isso ad[] agora e montado dentro de
 * adv_work_handler(), a cada (re)inicio de advertising, nao uma vez so
 * no boot.
 */
/* UUID do servico Atuador (actuator_client.h), nao mais o BT_UUID_LBS
 * proprio - o my_lbs agora se registra sob esse UUID tambem, ver a
 * nota de arquitetura em my_lbs.h.
 */
static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_ACTUATOR_SERVICE_VAL),
};

static void adv_work_handler(struct k_work *work)
{
	int err;

	/* A interface ja esta conectada - nao ha papel de Peripheral livre
	 * para reabilitar advertising (o controlador reserva
	 * CONFIG_BT_CTLR_SDC_PERIPHERAL_COUNT papeis simultaneos, cujo
	 * default e 1, INDEPENDENTE de CONFIG_BT_MAX_CONN=20 - esse teto e
	 * so a soma de todos os papeis, nao quantos podem ser Peripheral ao
	 * mesmo tempo). Sem esta guarda, toda reciclagem de conexao de
	 * ATUADOR (Central) tambem chamava advertising_start() - ver
	 * recycled_cb() - e cada tentativa batia de frente com o unico slot
	 * de Peripheral ja ocupado pelo celular, falhando com HCI status
	 * 0x0d ("Limited Resources") e repetindo pelo backoff PARA SEMPRE
	 * enquanto o celular seguisse conectado, parando o scan por
	 * atuadores a cada tentativa. Confirmado em campo: atuadores
	 * continuavam conectando e operando normalmente durante o loop, so
	 * a advertising (que nao tinha pra onde ir mesmo) ficava falhando.
	 */
	if (phone_conn) {
		return;
	}

	/* O controlador recusa reconfigurar o endereco aleatorio enquanto
	 * o scanner/iniciador estiver ativo com esse mesmo endereco - e o
	 * Gateway esta sempre escaneando. Sem pausar o scan aqui, um
	 * bt_le_adv_start() no meio de uma tentativa de conexao falha com
	 * "Command Disallowed" e a advertising nunca mais volta.
	 */
	bt_scan_stop();

	/* Montado agora, nao em file scope: pega o nome atual (default de
	 * fabrica ou o que a interface tiver gravado via characteristic GAP
	 * padrao - ver prj.conf), nao o valor fixo de CONFIG_BT_DEVICE_NAME.
	 */
	const char *name = bt_get_name();
	const struct bt_data ad[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
		BT_DATA(BT_DATA_NAME_COMPLETE, name, strlen(name)),
	};

	err = bt_le_adv_start(adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err == -ENOMEM) {
		uint32_t delay = ADV_RETRY_BASE_MS << MIN(adv_retry_count, 3);

		if (delay > ADV_RETRY_MAX_MS) {
			delay = ADV_RETRY_MAX_MS;
		}
		adv_retry_count++;
		LOG_WRN("Advertising sem recurso no controlador agora - %u atuador(es) "
			"conectado(s) neste instante - tentativa %u, de novo em %u ms",
			actuator_manager_count_connected(), adv_retry_count, delay);
		k_work_reschedule(k_work_delayable_from_work(work), K_MSEC(delay));
		scan_start();
		return;
	}

	adv_retry_count = 0;

	if (err == -EALREADY) {
		/* Advertising ja estava ativa. Comum: .recycled nao informa QUAL
		 * conexao foi reciclada (a assinatura do callback nem recebe o
		 * struct bt_conn), entao toda reconexao de ATUADOR (Central)
		 * tambem cai aqui, nao so a da interface - e nesse caso a
		 * advertising para a interface ja estava rodando. Nao e erro.
		 */
		LOG_DBG("Advertising ja estava ativa (recycled provavelmente de "
			"atuador, nao da interface)");
	} else if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
	} else {
		LOG_INF("Advertising (para a interface) iniciado");
	}

	scan_start();
}

static void advertising_start(void)
{
	/* k_work_schedule() (não k_work_reschedule()) de propósito: se já
	 * existe uma retentativa de backoff pendente (aguardando até 2 s
	 * por falta de recurso no controlador), esta chamada não deve
	 * cancelar essa espera e forçar uma tentativa imediata. Era
	 * exatamente isso que estava acontecendo: cada disparo de
	 * .recycled durante uma rajada de conexões (comum com vários
	 * atuadores reconectando de uma vez) cancelava o backoff em
	 * andamento e forçava outra tentativa na hora - todas fadadas a
	 * falhar pelo mesmo motivo, e próximas o suficiente para o próprio
	 * bt_le_adv_start() recusar com -EALREADY (-120), já que a
	 * tentativa anterior ainda nem tinha terminado de processar.
	 * k_work_schedule() só agenda se NÃO houver nada pendente; se já
	 * tem uma retentativa na fila, esta chamada não faz nada, e o
	 * backoff em andamento é respeitado.
	 */
	k_work_schedule(&adv_work, K_NO_WAIT);
}

/* --- Scan: um unico filtro, por UUID do servico do atuador --- */

static void scan_start(void)
{
	int err = bt_scan_start(BT_SCAN_TYPE_SCAN_ACTIVE);

	if (err && err != -EALREADY) {
		LOG_ERR("Falha ao iniciar scan (err %d)", err);
	} else {
		LOG_DBG("Scan (re)iniciado, procurando atuadores...");
	}
}

/* Exportado (nao-static) para uart_link.c: GATEWAY_CTRL_RESTART_SCAN e
 * ACTUATOR_MANAGE de adicao, vindos do 91 via UART, precisam do mesmo
 * reinicio de scan que app_gateway_ctrl_cb/app_actuator_manage_cb ja
 * disparam pela interface local - mas scan_start() acima e' static,
 * uso interno deste arquivo. Wrapper fino, mesmo efeito.
 */
void app_restart_scan(void)
{
	bt_scan_stop();
	scan_start();
}

/* --- Nome do atuador (Local Name do advertisement) ---
 *
 * É o mesmo campo que o coleta_ble mostra como TAG (scanner.py usa
 * device.name, que o bleak preenche a partir daqui). Só está disponível
 * enquanto o atuador ainda não está conectado - ele para de anunciar
 * assim que a conexão GATT sobe - por isso a extração acontece aqui,
 * no momento do scan, e não em algum outro ponto do fluxo.
 */
struct name_extract_ctx {
	char *out;
	size_t out_len;
	bool got_complete;
};

static bool name_extract_cb(struct bt_data *data, void *user_data)
{
	struct name_extract_ctx *ctx = user_data;
	size_t n;

	if (data->type == BT_DATA_NAME_COMPLETE) {
		n = MIN(data->data_len, ctx->out_len - 1);
		memcpy(ctx->out, data->data, n);
		ctx->out[n] = '\0';
		ctx->got_complete = true;
		return false; /* nome completo tem prioridade - encontrou, para */
	}
	if (data->type == BT_DATA_NAME_SHORTENED && !ctx->got_complete) {
		n = MIN(data->data_len, ctx->out_len - 1);
		memcpy(ctx->out, data->data, n);
		ctx->out[n] = '\0';
	}
	return true;
}

static void extract_adv_name(struct net_buf_simple *ad, char *out, size_t out_len)
{
	struct name_extract_ctx ctx = { .out = out, .out_len = out_len, .got_complete = false };

	out[0] = '\0';
	bt_data_parse(ad, name_extract_cb, &ctx);
}

/* --- Callback de scan "cru" - vê TODO pacote recebido, sem filtro ---
 *
 * O bt_scan (acima) só chama scan_filter_match para pacotes que batem
 * com o filtro de UUID - tipicamente só o advertisement principal. Se
 * o nome do atuador vier no SCAN RESPONSE (comum: o pacote principal já
 * está cheio com flags + UUID de 128 bits), esse segundo pacote nunca
 * passa pelo filtro e o nome nunca seria visto.
 *
 * Este callback roda em paralelo ao bt_scan (o Zephyr permite varios
 * bt_le_scan_cb registrados ao mesmo tempo) e ve absolutamente tudo,
 * inclusive SCAN RESPONSE e pacotes de aparelhos que nao sao nossos.
 * actuator_manager_name_seen() e quem faz o filtro de relevancia: so
 * atualiza nome de endereco que ja existe no cache ou numa slot, nunca
 * cria entrada nova a partir daqui - senao qualquer celular ou fone
 * Bluetooth por perto viraria "atuador descoberto".
 */
static void scan_recv_cb(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
	/* +1 para o terminador: extract_adv_name produz uma C-string, e sem
	 * o byte extra perderiamos o ultimo caractere de um nome que use o
	 * campo inteiro.
	 */
	char name[ACTUATOR_NAME_MAX_LEN + 1];

	extract_adv_name(buf, name, sizeof(name));
	if (name[0]) {
		actuator_manager_name_seen(info->addr, name);
	}
}

static struct bt_le_scan_cb scan_recv_callbacks = {
	.recv = scan_recv_cb,
};

static void scan_filter_match(struct bt_scan_device_info *device_info,
			      struct bt_scan_filter_match *filter_match, bool connectable)
{
	struct bt_conn *conn = NULL;
	char adv_name[ACTUATOR_NAME_MAX_LEN + 1];
	int err;

	/* Com um unico filtro ativo (UUID do servico proprietario), todo
	 * match aqui JA e um atuador - nao ha necessidade de parsear o
	 * advertisement para descobrir qual alvo casou. Ainda parseamos,
	 * mas so para extrair o nome (ver extract_adv_name acima).
	 */
	extract_adv_name(device_info->adv_data, adv_name, sizeof(adv_name));

	/* Todo atuador visto alimenta o cache de descoberta, inclusive os
	 * ja aprovados, para o RSSI (e o nome) ficarem atualizados na
	 * interface.
	 */
	actuator_manager_seen(device_info->recv_info->addr, device_info->recv_info->rssi, adv_name);

	/* Descoberta passiva: nao conecta em quem nao foi adicionado. A
	 * checagem acontece ANTES de conectar, para evitar o ciclo
	 * conectar/rejeitar/reconectar.
	 */
	if (!actuator_manager_is_wanted(device_info->recv_info->addr)) {
		return;
	}

	LOG_INF("Atuador aprovado encontrado (%s), conectando...",
		adv_name[0] ? adv_name : "sem nome");

	bt_scan_stop();

	err = bt_conn_le_create(device_info->recv_info->addr, BT_CONN_LE_CREATE_CONN,
				BT_LE_CONN_PARAM_DEFAULT, &conn);
	if (err) {
		LOG_ERR("bt_conn_le_create falhou (err %d)", err);
		scan_start();
		return;
	}

	bt_conn_unref(conn);
}

static void scan_connecting_error(struct bt_scan_device_info *device_info)
{
	LOG_ERR("Falha ao conectar durante o scan");
	scan_start();
}

BT_SCAN_CB_INIT(scan_cbs, scan_filter_match, NULL, scan_connecting_error, NULL);

static void scan_init(void)
{
	int err;
	struct bt_scan_init_param scan_init_param = {
		/* Conexao manual: a allow-list decide em scan_filter_match */
		.connect_if_match = false,
	};

	bt_scan_init(&scan_init_param);
	bt_scan_cb_register(&scan_cbs);

	err = bt_scan_filter_add(BT_SCAN_FILTER_TYPE_UUID, BT_UUID_ACTUATOR_SERVICE);
	if (err) {
		LOG_ERR("Falha ao adicionar filtro do servico do atuador (err %d)", err);
	}

	err = bt_scan_filter_enable(BT_SCAN_UUID_FILTER, false);
	if (err) {
		LOG_ERR("Falha ao habilitar filtros (err %d)", err);
	}
}

/* --- Stubs das characteristics herdadas do exercicio original --- */

static void app_led_cb(bool led_state)
{
	ARG_UNUSED(led_state);
}

static bool app_button_cb(void)
{
	return false;
}

/* --- Relay: interface -> atuador (comando cru) --- */

static void app_actuator_cmd_cb(uint8_t slot, const uint8_t *data, uint16_t len)
{
	int err;

	/* Aviso proativo: sem a assinatura do notify, o comando ate sai,
	 * mas a resposta do atuador sera descartada na volta - e o sintoma
	 * ("nao respondeu nada") nao aponta para a causa.
	 */
	if (!my_lbs_actuator_notify_enabled()) {
		LOG_WRN("Comando enviado ao slot %d, mas a Actuator Raw Data "
			"(UUID ...1527) NAO esta assinada - a resposta sera "
			"descartada. Habilite o notify nela antes de comandar.",
			slot);
	}

	err = actuator_manager_send_to(slot, data, len);

	if (err == -ENOTCONN) {
		LOG_WRN("Comando para o slot %d ignorado: atuador nao esta PRONTO "
			"(consulte a Actuator Status antes de enviar)", slot);
		return;
	}
	if (err) {
		LOG_WRN("Falha ao repassar comando ao slot %d (err %d)", slot, err);
		return;
	}

	LOG_INF("Comando de %u bytes repassado ao slot %d", len, slot);
	LOG_HEXDUMP_INF(data, len, "-> RX do atuador");
}

static void app_actuator_manage_cb(uint8_t cmd, const bt_addr_le_t *addr)
{
	char addr_str[BT_ADDR_LE_STR_LEN];
	int err;

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

	if (cmd == 0x01) {
		err = actuator_manager_add(addr);
		if (err) {
			LOG_WRN("Falha ao adicionar atuador %s (err %d)", addr_str, err);
		} else {
			LOG_INF("Atuador %s adicionado pela interface", addr_str);
			/* O scan pode estar parado. Sem isto, o atuador
			 * recem-adicionado so seria visto no proximo evento
			 * que retomasse o scan - o que parece "nao funcionou".
			 */
			scan_start();
		}
	} else {
		err = actuator_manager_remove(addr);
		if (err) {
			LOG_WRN("Falha ao remover atuador %s (err %d)", addr_str, err);
		} else {
			LOG_INF("Atuador %s removido pela interface", addr_str);
		}
	}
}

/* --- Consultas de estado (read das characteristics) --- */

static uint16_t app_status_read_cb(uint8_t *buf, uint16_t buf_len)
{
	uint16_t n = actuator_manager_get_status_all(buf, buf_len);

	LOG_DBG("Leitura de Actuator Status: %u bytes", n);
	return n;
}

static uint16_t app_discovery_read_cb(uint8_t *buf, uint16_t buf_len)
{
	uint16_t n = actuator_manager_get_discovery_all(buf, buf_len);

	LOG_DBG("Leitura de Discovered Actuators: %u bytes", n);
	return n;
}

/* --- Nome do Gateway: persistencia explicita (nao usa bt/name do Zephyr) ---
 *
 * bt_set_name() + CONFIG_BT_DEVICE_NAME_DYNAMIC deveriam persistir
 * sozinhos via settings (bt_settings_store_name(), chave "bt/name"),
 * mas isso NAO sobreviveu a reboot neste projeto - confirmado em campo
 * mesmo com a gravacao reportando sucesso (sem "Unable to store name"
 * no log), enquanto a allow-list, no mesmo backend ZMS so que sob
 * chave nossa ("coe/al"), sobrevive normal. Em vez de depurar a fundo
 * essa integracao embutida do host Zephyr, persistimos do nosso
 * proprio jeito, com o mesmo padrao ja confiavel da allow-list (ver
 * allowlist_save_work/allowlist_settings_set em actuator_client.c).
 *
 * Por isso tambem CONFIG_BT_DEVICE_NAME_GATT_WRITABLE esta desligado
 * no prj.conf: a characteristic GAP padrao (0x2A00) continua de
 * LEITURA (a interface le o nome atual por ela), mas a ESCRITA agora
 * so acontece por aqui, via Gateway Control - a unica forma de
 * garantir que toda troca de nome passa por gateway_name_save() e
 * persiste de verdade. Escrever na characteristic GAP ainda mudaria
 * bt_dev.name em RAM (Zephyr nao bloqueia isso), mas nao persistiria -
 * exatamente a armadilha que causou esse bug.
 */
#define GATEWAY_NAME_KEY "gw/name"

static struct k_work gateway_name_save_work;

static void gateway_name_save_work_handler(struct k_work *work)
{
	const char *name = bt_get_name();
	int err = settings_save_one(GATEWAY_NAME_KEY, name, strlen(name));

	if (err) {
		LOG_ERR("Falha ao gravar nome do Gateway (err %d)", err);
	} else {
		LOG_INF("Nome do Gateway gravado: '%s'", name);
	}
}

static void gateway_name_save(void)
{
	k_work_submit(&gateway_name_save_work);
}

static int gateway_name_settings_set(const char *name, size_t len, settings_read_cb read_cb,
				     void *cb_arg)
{
	char buf[CONFIG_BT_DEVICE_NAME_MAX + 1];
	ssize_t got;

	if (!settings_name_steq(name, "name", NULL)) {
		return -ENOENT;
	}

	if (len > CONFIG_BT_DEVICE_NAME_MAX) {
		len = CONFIG_BT_DEVICE_NAME_MAX;
	}

	got = read_cb(cb_arg, buf, len);
	if (got < 0) {
		return got;
	}

	buf[got] = '\0';
	bt_set_name(buf);
	LOG_INF("Nome do Gateway restaurado: '%s'", buf);

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(gw, "gw", NULL, gateway_name_settings_set, NULL, NULL);

/* --- Comandos administrativos da interface --- */

static void app_gateway_ctrl_cb(uint8_t cmd, const uint8_t *arg, uint16_t arg_len)
{
	switch (cmd) {
	case GATEWAY_CTRL_CLEAR_DISCOVERY:
		actuator_manager_clear_discovery();
		break;

	case GATEWAY_CTRL_RESTART_SCAN:
		LOG_INF("Reinicio de scan solicitado pela interface");
		bt_scan_stop();
		scan_start();
		break;

	case GATEWAY_CTRL_DISCONNECT_SLOT:
		if (arg_len < 1) {
			LOG_WRN("Gateway Ctrl: desconexao sem slot informado");
			break;
		}
		actuator_manager_disconnect(arg[0]);
		break;

	case GATEWAY_CTRL_SET_SPEED: {
		int err;

		if (arg_len < 2) {
			LOG_WRN("Gateway Ctrl: modo de enlace sem slot/valor");
			break;
		}

		err = actuator_manager_set_speed(arg[0], arg[1] ? ACTUATOR_SPEED_FAST
							       : ACTUATOR_SPEED_SLOW);
		if (err) {
			LOG_WRN("Gateway Ctrl: falha ao mudar modo do slot %u (err %d)", arg[0], err);
		}
		break;
	}

	case GATEWAY_CTRL_SET_NAME: {
		char name[CONFIG_BT_DEVICE_NAME_MAX + 1];
		int err;

		if (arg_len < 1 || arg_len > CONFIG_BT_DEVICE_NAME_MAX) {
			LOG_WRN("Gateway Ctrl: nome invalido (%u bytes, max %d)", arg_len,
				CONFIG_BT_DEVICE_NAME_MAX);
			break;
		}

		memcpy(name, arg, arg_len);
		name[arg_len] = '\0';

		err = bt_set_name(name);
		if (err) {
			LOG_WRN("Gateway Ctrl: falha ao trocar nome (err %d)", err);
			break;
		}

		LOG_INF("Nome do Gateway trocado pela interface: '%s'", name);
		gateway_name_save();
		break;
	}

	default:
		LOG_WRN("Gateway Ctrl: comando desconhecido (0x%02x)", cmd);
		break;
	}
}

static struct my_lbs_cb phone_facing_callbacks = {
	.led_cb = app_led_cb,
	.button_cb = app_button_cb,
	.actuator_cmd_cb = app_actuator_cmd_cb,
	.actuator_manage_cb = app_actuator_manage_cb,
	.actuator_status_read_cb = app_status_read_cb,
	.discovery_read_cb = app_discovery_read_cb,
	.gateway_ctrl_cb = app_gateway_ctrl_cb,
};

/* --- Relay: atuador -> interface --- */

static void actuator_raw_data_cb(uint8_t slot, const uint8_t *data, uint16_t len)
{
	int err;

	LOG_INF("Resposta de %u bytes do slot %d", len, slot);
	LOG_HEXDUMP_INF(data, len, "<- TX do atuador");

	err = my_lbs_send_actuator_raw_data(slot, data, len);

	if (err == -EACCES) {
		LOG_WRN("Dados do slot %d descartados: a interface nao assinou a "
			"Actuator Raw Data (UUID ...1527)", slot);
	} else if (err) {
		LOG_WRN("Falha ao repassar dados do slot %d (err %d)", slot, err);
	}

	uart_link_send_raw_data(slot, data, len);
}

static void update_actuator_led(void)
{
	/* Recontado do zero a cada mudanca de estado: se so acendessemos
	 * quando ESTE slot vira READY (sem verificar os demais), o LED
	 * nunca apagaria de volta quando o ultimo atuador pronto cair.
	 */
	if (actuator_manager_count_ready() > 0) {
		dk_set_led_on(CON_STATUS_LED_ACTUATOR);
	} else {
		dk_set_led_off(CON_STATUS_LED_ACTUATOR);
	}
}

static void actuator_status_cb(const uint8_t *rec, uint16_t len)
{
	int err;

	update_actuator_led();

	err = my_lbs_send_actuator_status(rec, len);

	/* -EACCES aqui e normal: ninguem com a interface aberta no momento.
	 * O estado continua correto e sera lido inteiro no proximo read.
	 */
	if (err && err != -EACCES) {
		LOG_WRN("Falha ao notificar status (err %d)", err);
	}

	uart_link_send_status(rec, len);
}

static void actuator_discovery_cb(const uint8_t *rec, uint16_t len)
{
	int err = my_lbs_send_discovery(rec, len);

	if (err && err != -EACCES) {
		LOG_WRN("Falha ao notificar descoberta (err %d)", err);
	}

	uart_link_send_discovery(rec, len);
}

static struct actuator_manager_cb actuator_callbacks = {
	.raw_data_cb = actuator_raw_data_cb,
	.status_cb = actuator_status_cb,
	.discovery_cb = actuator_discovery_cb,
};

/* --- MTU: visibilidade explicita --- */

static void att_mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));
	LOG_INF("MTU atualizado (%s): tx=%u rx=%u (payload util = %u bytes)", addr_str, tx, rx,
		(tx < rx ? tx : rx) - 3);
}

static struct bt_gatt_cb gatt_callbacks = {
	.att_mtu_updated = att_mtu_updated,
};

/* --- Conexao --- */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	struct bt_conn_info info;

	if (err) {
		LOG_ERR("Connection failed (err %u)", err);
		scan_start();
		return;
	}

	bt_conn_get_info(conn, &info);

	if (info.role == BT_CONN_ROLE_PERIPHERAL) {
		LOG_INF("Interface conectada");
		phone_conn = conn;
		my_lbs_set_phone_conn(conn);
		dk_set_led_on(CON_STATUS_LED_PHONE);
		return;
	}

	/* Central: com um unico filtro de scan, so ha um tipo de alvo. */
	if (!actuator_manager_is_wanted(info.le.dst)) {
		LOG_INF("Atuador nao aprovado - desconectando");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}

	LOG_INF("Conectado a atuador aprovado, iniciando discovery...");
	actuator_manager_start(conn, info.le.dst);

	scan_start();
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	struct bt_conn_info info;

	bt_conn_get_info(conn, &info);

	if (info.role == BT_CONN_ROLE_PERIPHERAL) {
		LOG_INF("Interface desconectada (reason %u)", reason);
		phone_conn = NULL;
		my_lbs_set_phone_conn(NULL);
		dk_set_led_off(CON_STATUS_LED_PHONE);
		/* NÃO chamar advertising_start() aqui: o objeto de conexão
		 * ainda não foi liberado neste ponto (só quando .recycled
		 * disparar, logo abaixo), e tentar reiniciar a advertising
		 * antes disso falha com -ENOMEM - o controlador ainda conta
		 * essa conexão como ocupada. É exatamente esse padrão
		 * "restart no callback de disconnect" que a documentação da
		 * NCS 3.0+ substituiu pelo callback .recycled.
		 */
		return;
	}

	if (actuator_manager_owns_conn(conn)) {
		LOG_INF("Atuador desconectado (reason %u)", reason);
		actuator_manager_conn_lost(conn);
	}

	scan_start();
}

static void recycled_cb(void)
{
	advertising_start();
}

static struct bt_conn_cb connection_callbacks = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.recycled = recycled_cb,
};

int main(void)
{
	int blink_status = 0;
	int err;

	LOG_INF("Starting Coester SIM Connect - Gateway (ate %d atuadores reais)", MAX_ACTUATORS);

	err = dk_leds_init();
	if (err) {
		LOG_ERR("LEDs init failed (err %d)", err);
		return -1;
	}

	/* Saida de motor (papel atuador) - GPIO direto, ver actuator_motor.h.
	 * Nao fatal: se os pinos nao estiverem prontos por algum motivo, o
	 * resto do Gateway continua funcionando, so' fica sem acionamento de
	 * motor. Comando so' pelo Painel BLE (ou rede), via arbitro de modo -
	 * os botoes do DK nao comandam o motor.
	 */
	(void)actuator_motor_init();

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("Bluetooth init failed (err %d)", err);
		return -1;
	}

	bt_conn_cb_register(&connection_callbacks);
	bt_gatt_cb_register(&gatt_callbacks);

	err = my_lbs_init(&phone_facing_callbacks);
	if (err) {
		LOG_ERR("Failed to init LBS (err:%d)", err);
		return -1;
	}

	err = actuator_manager_init(&actuator_callbacks);
	if (err) {
		LOG_ERR("Failed to init actuator manager (err:%d)", err);
		return -1;
	}

	/* Diferente de my_lbs_init/actuator_manager_init acima, falha aqui
	 * NAO e fatal: se o 91 nao estiver plugado na bancada (ou a uart30
	 * nao tiver sido habilitada corretamente), a interface BLE local
	 * continua funcionando sozinha - so fica registrado em log (ver
	 * uart_link_init()).
	 */
	(void)uart_link_init();

	/* ANTES de qualquer sensor: os pollers I2C (ads1000.c,
	 * torque_onoff_sensor.c) agendam trabalho nesta fila assim que
	 * inicializados, logo abaixo. Fila dedicada de proposito - ver
	 * sensor_workq.c pro bug que isso corrige (E/S I2C bloqueante
	 * competindo com o host Bluetooth/main() pela fila do sistema).
	 */
	sensor_workq_init();

	/* Mesmo padrao de nao-fatal do uart_link_init() acima: se algum
	 * sensor nao estiver plugado na bancada, o resto do Gateway
	 * continua funcionando normalmente. Posicao e torque sao sempre
	 * pareados no produto (mesmo chip ADS1000, enderecos diferentes -
	 * ver ads1000.c).
	 */
	(void)position_sensor_init();
	(void)torque_sensor_init();

	/* Torque ON/OFF (PCA9536) - variante de BOM mutuamente exclusiva
	 * com a analogica (torque_sensor.h). A selecao automatica de fonte de
	 * torque (actuator_sensors.c) consulta torque_onoff_sensor_is_online()
	 * pra decidir qual variante usar. Nao fatal, como os demais sensores -
	 * ver docs/SENSORES_I2C.md.
	 */
	(void)torque_onoff_sensor_init();

	/* Parametros (paramDado do fwBLE, area Painel 0x00800800) - ANTES de
	 * actuator_sensors_init() (que ja le limites/torque) e ANTES de
	 * settings_load() mais abaixo (que restaura o que estiver gravado,
	 * se houver) - ver actuator_params.h.
	 */
	actuator_params_init();

	/* Tempo de reversao do motor (campo herdado da FSA do fwBLE, area
	 * 0x00801010) - antes de settings_load(), mesmo motivo do de cima.
	 */
	actuator_fsa_cfg_init();

	/* Camada de calibracao (raw -> posicao em per mil / torque em Nm) -
	 * ver actuator_sensors.c pro escopo exato.
	 */
	actuator_sensors_init();

	/* Motor de alarmes + controle logico (papel atuador). O controle
	 * registra seu at_ctl_stop(ACP_ALARME) no motor de alarmes via
	 * callback (actuator_control_stop_on_alarm). Depois de
	 * actuator_sensors_init() porque o loop de controle le a posicao.
	 * Comando chega pelo Painel BLE (actuator_panel, logo abaixo).
	 */
	{
		const struct actuator_alarm_cb alarm_cb = {
			.stop_cb = actuator_control_stop_on_alarm,
			.mov_stt_cb = actuator_control_get_mov_stt,
		};

		actuator_alarm_init(&alarm_cb);
	}

	/* Watchdog - logo ANTES do laco de controle (que alimenta o canal
	 * "controle" a cada 10 ms) pra nao sobrar janela em que o canal ja'
	 * existe mas ninguem o alimenta. Depois de sensor_workq_init() (o
	 * canal "sensores" agenda um batimento la'). Ver watchdog.h.
	 */
	watchdog_init();

	/* Registro de eventos (atRegEvent do fwBLE) - depois do watchdog (que
	 * classifica a causa do boot, 1o evento gravado) e antes do laco de
	 * controle (que ja' registra alarmes/estado no 1o ciclo). A flash
	 * externa e' montada na fila propria do modulo, fora daqui.
	 */
	actuator_regevent_init(watchdog_causa_boot());
	actuator_control_init();

	/* Painel Remoto - entrada de comando por BLE (acgl GTM_SEND na area
	 * 0x00805900, tratada em actuator_service.c). Consumido a cada ciclo
	 * pelo arbitro de modo, dentro de actuator_control_run().
	 */
	actuator_panel_init();

	/* Arbitro de modo (Local = interface BLE / Remoto = rede). Default
	 * LOCAL. Consome o Painel Remoto e decide quem comanda.
	 */
	actuator_mode_init();

	/* Depois do actuator_manager_init (que zera os slots) e depois do
	 * bt_enable: settings_load() repovoa a allow-list gravada. Sem
	 * isso, um reboot no meio da demonstracao custaria varios minutos
	 * redigitando enderecos MAC. De quebra, esta e' tambem a chamada
	 * que restaura bt/name (o handler generico "bt" do proprio Zephyr,
	 * settings.c:set_setting()) - por isso a ordem bt_enable() ANTES de
	 * settings_load() importa: o handler ignora tudo sob "bt/" se
	 * BT_DEV_ENABLE ainda nao estiver setado.
	 */
	err = settings_subsys_init();
	if (err) {
		LOG_ERR("settings_subsys_init falhou (err %d) - a allow-list "
			"nao sobrevivera a um reboot", err);
	} else {
		err = settings_load();
		if (err) {
			LOG_ERR("settings_load falhou (err %d)", err);
		}
	}

	/* Migra a calibracao do antigo actuator_calib ("acal/v1") pro
	 * paramDado, se ainda nao houver parametros gravados no formato novo.
	 */
	actuator_params_post_load();

	/* settings_load() acima ja restaurou o nome, se algum foi gravado
	 * (ver gateway_name_settings_set - chave "gw/name", nao a "bt/name"
	 * embutida do Zephyr, que nao persistia de forma confiavel aqui). */
	LOG_INF("Nome do Gateway: '%s'", bt_get_name());

	scan_init();
	bt_le_scan_cb_register(&scan_recv_callbacks);

	LOG_INF("Bluetooth initialized");

	k_work_init(&gateway_name_save_work, gateway_name_save_work_handler);
	k_work_init_delayable(&adv_work, adv_work_handler);
	/* Só advertising_start() aqui - ela já termina chamando scan_start()
	 * dentro de adv_work_handler (sucesso, falha ou retentativa, os três
	 * casos chamam). Chamar scan_start() de novo aqui, direto, criava
	 * uma corrida: duas chamadas a bt_scan_start() quase simultâneas,
	 * uma pelo workqueue (via adv_work_handler) e outra por esta thread
	 * - a segunda chegava com a primeira ainda em transição e o
	 * controlador recusava com -EBUSY ("Falha ao iniciar scan (err -16)").
	 */
	advertising_start();

	/* So' o LED de "vivo". Sensores e movimento sao expostos pela area
	 * Sensor/status do servico BLE (actuator_service.c) e pela interface; o
	 * controle loga as transicoes de status sozinho.
	 */
	for (;;) {
		dk_set_led(RUN_STATUS_LED, (++blink_status) % 2);
		k_sleep(K_MSEC(RUN_LED_BLINK_INTERVAL));
	}
}
