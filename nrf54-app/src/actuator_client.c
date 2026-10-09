/*
 * Coester - SIM Connect (Gateway)
 *
 * Implementacao do Actuator Manager: slots com estado explicito, cache
 * de descoberta passiva, gestao de velocidade do enlace e persistencia
 * da allow-list.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/settings/settings.h>
#include <bluetooth/gatt_dm.h>
#include <string.h>

#include "actuator_client.h"

LOG_MODULE_DECLARE(Coester_Gateway);

/* --- Parametros de conexao ---
 *
 * BT_LE_CONN_PARAM(int_min, int_max, latency, timeout):
 *   intervalos em unidades de 1.25 ms, timeout em unidades de 10 ms.
 *
 * LENTO: 120-200 ms. Com 16 links, cada evento de conexao ocupa uma
 * fatia pequena do intervalo e ainda sobra radio para o scan, para a
 * advertising e para o enlace com a interface.
 *
 * RAPIDO: 15-30 ms. Round trip de ~30 ms, o que torna viavel baixar
 * centenas de registros de curva em tempo aceitavel. Um link por vez.
 *
 * Timeout de 4 s nos dois casos: sao 20 intervalos lentos perdidos
 * antes de declarar o link morto, tolerante o suficiente para ruido
 * industrial sem demorar demais a detectar um atuador que sumiu.
 */
#define ACTUATOR_CONN_PARAM_SLOW BT_LE_CONN_PARAM(96, 160, 0, 400)
#define ACTUATOR_CONN_PARAM_FAST BT_LE_CONN_PARAM(12, 24, 0, 400)

/* O controlador rejeita atualizacao de parametros logo apos o
 * estabelecimento da conexao, e o discovery ainda esta rodando nesse
 * momento (e se beneficia do intervalo padrao, mais rapido). Por isso
 * a troca para LENTO e adiada.
 */
#define PARAM_UPDATE_DELAY_MS 1000

struct actuator_slot {
	bt_addr_le_t addr;
	enum actuator_state state;
	char name[ACTUATOR_NAME_MAX_LEN]; /* campo de tamanho fixo, zero-padded - ver .h */

	struct bt_conn *conn;
	uint16_t rx_handle;
	uint16_t tx_handle;
	uint16_t tx_ccc_handle;
	struct bt_gatt_subscribe_params tx_sub_params;
	bool tx_sub_active;

	enum actuator_speed speed;
	struct k_work_delayable param_work;
	struct k_work_delayable fast_timeout_work;
};

struct discovered_entry {
	bt_addr_le_t addr;
	char name[ACTUATOR_NAME_MAX_LEN];
	int8_t rssi;
	bool valid;
	int64_t last_notified;
};

static struct actuator_slot slots[MAX_ACTUATORS];
static struct discovered_entry discovered[MAX_DISCOVERED];
static struct actuator_manager_cb app_cb;
static struct k_work save_work;

#define DISCOVERY_NOTIFY_MIN_INTERVAL_MS 3000
#define DISCOVERY_RSSI_DELTA_DBM 8

/* --- Helpers de slot --- */

static struct actuator_slot *find_by_addr(const bt_addr_le_t *addr)
{
	for (int i = 0; i < MAX_ACTUATORS; i++) {
		if (slots[i].state != ACTUATOR_STATE_EMPTY &&
		    bt_addr_le_cmp(&slots[i].addr, addr) == 0) {
			return &slots[i];
		}
	}
	return NULL;
}

static struct actuator_slot *find_free_slot(void)
{
	for (int i = 0; i < MAX_ACTUATORS; i++) {
		if (slots[i].state == ACTUATOR_STATE_EMPTY) {
			return &slots[i];
		}
	}
	return NULL;
}

static uint8_t slot_index(const struct actuator_slot *slot)
{
	return (uint8_t)(slot - slots);
}

/* Copia *src* para o campo de nome de tamanho fixo *dest*,
 * preenchendo o restante com zeros.
 *
 * IMPORTANTE - src vazio significa "este pacote nao trouxe nome", NAO
 * "apague o nome". Sem essa regra o nome fica piscando na interface:
 * o SCAN RESPONSE traz o nome e o advertisement principal (que e o
 * unico que passa pelo filtro de UUID) nao traz, entao os dois se
 * alternam varias vezes por segundo, um escrevendo e o outro apagando.
 * O nome so e limpo de verdade quando a entrada e reciclada/removida,
 * via memset explicito.
 *
 * Retorna true se o valor mudou, para o chamador decidir se precisa
 * notificar a interface.
 */
static bool set_name_field(char *dest, const char *src)
{
	char novo[ACTUATOR_NAME_MAX_LEN];

	if (!src || !src[0]) {
		return false;
	}

	memset(novo, 0, sizeof(novo));
	strncpy(novo, src, sizeof(novo));

	if (memcmp(dest, novo, sizeof(novo)) == 0) {
		return false;
	}
	memcpy(dest, novo, sizeof(novo));
	return true;
}

static void fill_status_rec(uint8_t idx, uint8_t *rec)
{
	rec[0] = idx;
	rec[1] = (uint8_t)slots[idx].state;
	rec[2] = slots[idx].addr.type;
	memcpy(&rec[3], slots[idx].addr.a.val, 6);
	memcpy(&rec[9], slots[idx].name, ACTUATOR_NAME_MAX_LEN);
}

/* Ponto unico que dispara o notify de status - usado tanto por mudanca
 * de estado quanto por mudanca de nome, para a interface nunca ficar
 * dessincronizada por alguem esquecer de notificar.
 */
static void notify_slot_status(struct actuator_slot *slot)
{
	uint8_t rec[ACTUATOR_STATUS_REC_LEN];

	if (!app_cb.status_cb) {
		return;
	}
	fill_status_rec(slot_index(slot), rec);
	app_cb.status_cb(rec, sizeof(rec));
}

static void slot_set_state(struct actuator_slot *slot, enum actuator_state state)
{
	if (slot->state == state) {
		return;
	}
	slot->state = state;
	notify_slot_status(slot);
}

/* ERROR sozinho nao basta: sem desconectar, o slot fica com uma conexao
 * viva presa nesse estado ate a interface mandar GATEWAY_CTRL_DISCONNECT_SLOT
 * na mao - e como MAX_ACTUATORS ja usa o teto do controlador (ver nota
 * no .h), um erro transitorio de discovery custaria uma vaga de conexao
 * ate alguem perceber e agir manualmente. Desconectar aqui forca o mesmo
 * caminho de autorrecuperacao que ja existe para queda de conexao:
 * on_disconnected() -> actuator_manager_conn_lost() bota o slot de volta
 * em WAITING (nao EMPTY, continua na allow-list) e o scan tenta de novo
 * sozinho - o mesmo comportamento observado em campo quando a conexao
 * cai por conta propria (ex.: "RF noise?") no meio do discovery.
 */
static void slot_enter_error(struct actuator_slot *slot)
{
	slot_set_state(slot, ACTUATOR_STATE_ERROR);

	if (slot->conn) {
		bt_conn_disconnect(slot->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

/* --- Persistencia da allow-list (Settings/ZMS) ---
 *
 * Chave curta de proposito: no ZMS, nomes de ate 8 bytes cabem no
 * cabecalho da entrada e sao bem mais rapidos de ler/escrever.
 *
 * Serializacao manual (1 byte de tipo + 6 de endereco por atuador) em
 * vez de despejar bt_addr_le_t direto, para o formato gravado nao
 * depender de padding do compilador.
 */
#define ALLOWLIST_KEY "coe/al"
#define ADDR_REC_LEN 7

static void allowlist_save_work(struct k_work *work)
{
	uint8_t buf[MAX_ACTUATORS * ADDR_REC_LEN];
	size_t n = 0;
	int err;

	for (int i = 0; i < MAX_ACTUATORS; i++) {
		if (slots[i].state == ACTUATOR_STATE_EMPTY) {
			continue;
		}
		buf[n++] = slots[i].addr.type;
		memcpy(&buf[n], slots[i].addr.a.val, 6);
		n += 6;
	}

	err = settings_save_one(ALLOWLIST_KEY, buf, n);
	if (err) {
		LOG_ERR("Falha ao gravar allow-list (err %d)", err);
	} else {
		LOG_INF("Allow-list gravada (%u atuadores)", (unsigned)(n / ADDR_REC_LEN));
	}
}

/* Gravar em memoria nao-volatil bloqueia. Como add/remove chegam pelo
 * handler de escrita GATT (thread do BT), a gravacao vai para a
 * workqueue em vez de travar a pilha Bluetooth.
 */
static void allowlist_save(void)
{
	k_work_submit(&save_work);
}

static int allowlist_settings_set(const char *name, size_t len, settings_read_cb read_cb,
				  void *cb_arg)
{
	uint8_t buf[MAX_ACTUATORS * ADDR_REC_LEN];
	ssize_t got;

	if (!settings_name_steq(name, "al", NULL)) {
		return -ENOENT;
	}

	if (len > sizeof(buf)) {
		LOG_WRN("Allow-list gravada e maior que MAX_ACTUATORS - truncando");
		len = sizeof(buf);
	}

	got = read_cb(cb_arg, buf, len);
	if (got < 0) {
		return got;
	}

	for (ssize_t off = 0; off + ADDR_REC_LEN <= got; off += ADDR_REC_LEN) {
		bt_addr_le_t addr = { .type = buf[off] };

		memcpy(addr.a.val, &buf[off + 1], 6);
		actuator_manager_add(&addr);
	}

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(coester, "coe", NULL, allowlist_settings_set, NULL, NULL);

/* --- Velocidade do enlace --- */

static void param_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct actuator_slot *slot = CONTAINER_OF(dwork, struct actuator_slot, param_work);
	const struct bt_le_conn_param *param;
	int err;

	if (!slot->conn) {
		return;
	}

	param = (slot->speed == ACTUATOR_SPEED_FAST) ? ACTUATOR_CONN_PARAM_FAST
						     : ACTUATOR_CONN_PARAM_SLOW;

	err = bt_conn_le_param_update(slot->conn, param);
	if (err == -EALREADY) {
		/* Ja esta nos parametros pedidos - nada a fazer */
		return;
	}
	if (err) {
		LOG_WRN("Falha ao ajustar parametros do slot %d (err %d)", slot_index(slot),
			err);
		return;
	}

	LOG_INF("Slot %d -> modo %s", slot_index(slot),
		slot->speed == ACTUATOR_SPEED_FAST ? "RAPIDO" : "LENTO");
}

static void fast_timeout_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct actuator_slot *slot = CONTAINER_OF(dwork, struct actuator_slot, fast_timeout_work);

	if (slot->speed != ACTUATOR_SPEED_FAST) {
		return;
	}

	LOG_WRN("Slot %d ficou em modo rapido por %d s sem confirmacao - "
		"voltando ao lento (a interface caiu no meio de uma coleta?)",
		slot_index(slot), ACTUATOR_FAST_MODE_TIMEOUT_S);

	slot->speed = ACTUATOR_SPEED_SLOW;
	k_work_reschedule(&slot->param_work, K_NO_WAIT);
}

int actuator_manager_set_speed(uint8_t slot_idx, enum actuator_speed speed)
{
	struct actuator_slot *slot;

	if (slot_idx >= MAX_ACTUATORS) {
		return -EINVAL;
	}

	slot = &slots[slot_idx];

	if (!slot->conn) {
		return -ENOTCONN;
	}

	slot->speed = speed;
	k_work_reschedule(&slot->param_work, K_NO_WAIT);

	if (speed == ACTUATOR_SPEED_FAST) {
		k_work_reschedule(&slot->fast_timeout_work,
				  K_SECONDS(ACTUATOR_FAST_MODE_TIMEOUT_S));
	} else {
		k_work_cancel_delayable(&slot->fast_timeout_work);
	}

	return 0;
}

/* --- Cache de descoberta passiva --- */

static struct discovered_entry *discovery_find(const bt_addr_le_t *addr)
{
	for (int i = 0; i < MAX_DISCOVERED; i++) {
		if (discovered[i].valid && bt_addr_le_cmp(&discovered[i].addr, addr) == 0) {
			return &discovered[i];
		}
	}
	return NULL;
}

/* Sem espaco livre? Recicla a entrada de sinal mais fraco - e a menos
 * provavel de ser o atuador que o operador tem na mao.
 */
static struct discovered_entry *discovery_slot_for_new(void)
{
	struct discovered_entry *weakest = &discovered[0];

	for (int i = 0; i < MAX_DISCOVERED; i++) {
		if (!discovered[i].valid) {
			return &discovered[i];
		}
		if (discovered[i].rssi < weakest->rssi) {
			weakest = &discovered[i];
		}
	}
	return weakest;
}

static void fill_discovery_rec(const struct discovered_entry *entry, uint8_t *rec)
{
	rec[0] = entry->addr.type;
	memcpy(&rec[1], entry->addr.a.val, 6);
	rec[7] = (uint8_t)entry->rssi;
	rec[8] = actuator_manager_is_wanted(&entry->addr) ? ACTUATOR_DISCOVERY_FLAG_KNOWN : 0;
	memcpy(&rec[9], entry->name, ACTUATOR_NAME_MAX_LEN);
}

void actuator_manager_seen(const bt_addr_le_t *addr, int8_t rssi, const char *name)
{
	struct discovered_entry *entry = discovery_find(addr);
	int64_t now = k_uptime_get();
	bool should_notify = false;
	bool name_changed;

	if (!entry) {
		char addr_str[BT_ADDR_LE_STR_LEN];

		entry = discovery_slot_for_new();
		memset(entry, 0, sizeof(*entry));
		bt_addr_le_copy(&entry->addr, addr);
		entry->rssi = rssi;
		entry->valid = true;
		set_name_field(entry->name, name);
		should_notify = true;

		bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));
		LOG_INF("Atuador visto: %s \"%s\" (RSSI %d)", addr_str, name ? name : "", rssi);
	} else {
		int delta = (int)rssi - (int)entry->rssi;

		if (delta < 0) {
			delta = -delta;
		}

		name_changed = set_name_field(entry->name, name);

		if (name_changed || (delta >= DISCOVERY_RSSI_DELTA_DBM &&
				      (now - entry->last_notified) >= DISCOVERY_NOTIFY_MIN_INTERVAL_MS)) {
			should_notify = true;
		}
		entry->rssi = rssi;
	}

	/* Se este endereço já é um slot conhecido (adicionado antes de
	 * conectar, ou reconectando após queda), o nome visto agora no
	 * advertisement também atualiza o cache do slot - é a única janela
	 * em que o nome está disponível, porque o atuador para de anunciar
	 * assim que a conexão GATT é estabelecida.
	 */
	{
		struct actuator_slot *slot = find_by_addr(addr);

		if (slot && name && name[0] && set_name_field(slot->name, name)) {
			notify_slot_status(slot);
		}
	}

	if (should_notify && app_cb.discovery_cb) {
		uint8_t rec[ACTUATOR_DISCOVERY_REC_LEN];

		fill_discovery_rec(entry, rec);
		entry->last_notified = now;
		app_cb.discovery_cb(rec, sizeof(rec));
	}
}

/* Ver o porquê desta função existir no comentário do .h - em resumo,
 * mescla um nome visto num pacote que NAO tinha a UUID do servico
 * (tipicamente um SCAN RESPONSE), sem criar entrada nova.
 */
void actuator_manager_name_seen(const bt_addr_le_t *addr, const char *name)
{
	struct discovered_entry *entry;
	struct actuator_slot *slot;

	if (!name || !name[0]) {
		return;
	}

	entry = discovery_find(addr);
	if (entry && set_name_field(entry->name, name)) {
		if (app_cb.discovery_cb) {
			uint8_t rec[ACTUATOR_DISCOVERY_REC_LEN];

			fill_discovery_rec(entry, rec);
			app_cb.discovery_cb(rec, sizeof(rec));
		}
	}

	slot = find_by_addr(addr);
	if (slot && set_name_field(slot->name, name)) {
		notify_slot_status(slot);
	}
}

void actuator_manager_clear_discovery(void)
{
	memset(discovered, 0, sizeof(discovered));
	LOG_INF("Cache de descoberta limpo");
}

uint16_t actuator_manager_get_discovery_all(uint8_t *buf, uint16_t buf_len)
{
	uint16_t written = 0;

	for (int i = 0; i < MAX_DISCOVERED; i++) {
		if (!discovered[i].valid) {
			continue;
		}
		if (written + ACTUATOR_DISCOVERY_REC_LEN > buf_len) {
			break;
		}
		fill_discovery_rec(&discovered[i], &buf[written]);
		written += ACTUATOR_DISCOVERY_REC_LEN;
	}

	return written;
}

uint16_t actuator_manager_get_status_all(uint8_t *buf, uint16_t buf_len)
{
	uint16_t written = 0;

	for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
		if (written + ACTUATOR_STATUS_REC_LEN > buf_len) {
			break;
		}
		fill_status_rec(i, &buf[written]);
		written += ACTUATOR_STATUS_REC_LEN;
	}

	return written;
}

/* --- API de allow-list --- */

int actuator_manager_init(const struct actuator_manager_cb *cb)
{
	memset(slots, 0, sizeof(slots));
	memset(discovered, 0, sizeof(discovered));
	memset(&app_cb, 0, sizeof(app_cb));

	if (cb) {
		app_cb = *cb;
	}

	k_work_init(&save_work, allowlist_save_work);

	for (int i = 0; i < MAX_ACTUATORS; i++) {
		k_work_init_delayable(&slots[i].param_work, param_work_handler);
		k_work_init_delayable(&slots[i].fast_timeout_work, fast_timeout_handler);
	}

	return 0;
}

int actuator_manager_add(const bt_addr_le_t *addr)
{
	struct actuator_slot *slot = find_by_addr(addr);
	char addr_str[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(addr, addr_str, sizeof(addr_str));

	if (slot) {
		LOG_INF("Atuador ja conhecido (slot %d): %s", slot_index(slot), addr_str);
		return 0;
	}

	slot = find_free_slot();
	if (!slot) {
		LOG_WRN("Nenhum slot livre (MAX_ACTUATORS=%d atingido)", MAX_ACTUATORS);
		return -ENOMEM;
	}

	bt_addr_le_copy(&slot->addr, addr);
	slot->speed = ACTUATOR_SPEED_SLOW;
	memset(slot->name, 0, sizeof(slot->name));

	/* Se este endereço já apareceu num scan antes de ser adicionado
	 * (o caso comum: usuário clicou num item da lista de descobertos),
	 * o nome já está no cache - copia na hora, sem esperar outro scan.
	 */
	{
		struct discovered_entry *disc = discovery_find(addr);

		if (disc) {
			memcpy(slot->name, disc->name, sizeof(slot->name));
		}
	}

	LOG_INF("Atuador adicionado (slot %d): %s", slot_index(slot), addr_str);

	slot_set_state(slot, ACTUATOR_STATE_WAITING);
	allowlist_save();

	return 0;
}

int actuator_manager_remove(const bt_addr_le_t *addr)
{
	struct actuator_slot *slot = find_by_addr(addr);

	if (!slot) {
		return -ENOENT;
	}

	k_work_cancel_delayable(&slot->fast_timeout_work);

	if (slot->conn) {
		LOG_INF("Atuador removido (slot %d) - desconectando agora", slot_index(slot));
		/* EMPTY antes de desconectar: e assim que conn_lost() sabe
		 * que foi remocao explicita e nao deve reconectar.
		 */
		slot_set_state(slot, ACTUATOR_STATE_EMPTY);
		bt_conn_disconnect(slot->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	} else {
		LOG_INF("Atuador removido (slot %d) - nao estava conectado", slot_index(slot));
		slot_set_state(slot, ACTUATOR_STATE_EMPTY);
		memset(&slot->addr, 0, sizeof(slot->addr));
	}

	allowlist_save();

	return 0;
}

bool actuator_manager_is_wanted(const bt_addr_le_t *addr)
{
	return find_by_addr(addr) != NULL;
}

/* --- Notificacoes vindas do TX do atuador --- */

static uint8_t tx_notify_func(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
			      const void *data, uint16_t length)
{
	struct actuator_slot *slot = CONTAINER_OF(params, struct actuator_slot, tx_sub_params);

	if (!data) {
		LOG_INF("Atuador (slot %d): TX unsubscribed", slot_index(slot));
		params->value_handle = 0U;
		slot->tx_sub_active = false;
		return BT_GATT_ITER_STOP;
	}

	LOG_DBG("Atuador (slot %d): %u bytes recebidos via TX", slot_index(slot), length);
	LOG_HEXDUMP_DBG(data, length, "Actuator TX raw");

	if (app_cb.raw_data_cb) {
		app_cb.raw_data_cb(slot_index(slot), (const uint8_t *)data, length);
	}

	return BT_GATT_ITER_CONTINUE;
}

static int subscribe_tx(struct actuator_slot *slot)
{
	int err;

	if (slot->tx_sub_active) {
		return -EALREADY;
	}

	slot->tx_sub_params.notify = tx_notify_func;
	slot->tx_sub_params.value = BT_GATT_CCC_NOTIFY;
	slot->tx_sub_params.value_handle = slot->tx_handle;
	slot->tx_sub_params.ccc_handle = slot->tx_ccc_handle;
	atomic_set_bit(slot->tx_sub_params.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

	err = bt_gatt_subscribe(slot->conn, &slot->tx_sub_params);
	if (err) {
		LOG_ERR("Falha ao assinar TX (slot %d, err %d)", slot_index(slot), err);
		slot_enter_error(slot);
		return err;
	}

	LOG_INF("Atuador (slot %d) PRONTO - TX assinado", slot_index(slot));
	slot->tx_sub_active = true;
	slot_set_state(slot, ACTUATOR_STATE_READY);

	/* PHY 2M dobra a taxa no ar, o que reduz pela metade o tempo de
	 * radio de cada evento de conexao. Com 16 links disputando um unico
	 * radio isso nao e luxo - e o que faz caber. Se o atuador nao
	 * suportar, o controlador simplesmente mantem 1M.
	 */
	err = bt_conn_le_phy_update(slot->conn, BT_CONN_LE_PHY_PARAM_2M);
	if (err) {
		LOG_WRN("Slot %d: pedido de PHY 2M falhou (err %d) - seguindo em 1M",
			slot_index(slot), err);
	}

	/* Troca para o intervalo lento, adiada: o controlador rejeita
	 * atualizacao logo apos a conexao, e o PHY update acabou de ser
	 * disparado (uma procedure de link layer por vez).
	 */
	slot->speed = ACTUATOR_SPEED_SLOW;
	k_work_reschedule(&slot->param_work, K_MSEC(PARAM_UPDATE_DELAY_MS));

	return 0;
}

/* --- Discovery (bt_gatt_dm) do servico proprietario ---
 *
 * RESTRIÇÃO DOCUMENTADA DO MÓDULO: "Only one discovery procedure can be
 * started simultaneously" (nRF Connect SDK, GATT Discovery Manager) -
 * bt_gatt_dm é uma instância ÚNICA e GLOBAL, não uma por conexão. Com
 * vários atuadores conectando perto um do outro no tempo (comum logo
 * após o boot, quando todos reconectam de uma vez), é praticamente
 * garantido que duas descobertas se sobreponham - e a segunda a
 * chamar bt_gatt_dm_start() recebe -EALREADY (-120) na hora, sem
 * nenhuma tentativa nova automática depois.
 *
 * A fila abaixo serializa isso: se uma descoberta já está em
 * andamento quando um novo atuador conecta, ele entra na fila em vez
 * de chamar bt_gatt_dm_start() na hora. Assim que a descoberta atual
 * termina (sucesso, serviço não encontrado ou erro), o próximo da fila
 * é iniciado.
 */
static bool discovery_busy;
static uint8_t discovery_queue[MAX_ACTUATORS];
static uint8_t discovery_queue_head;
static uint8_t discovery_queue_tail;
static uint8_t discovery_queue_count;

/* Declaradas aqui porque discovery_complete/service_not_found/error e
 * discovery_start_for_slot se chamam mutuamente (a callback termina
 * chamando "avança a fila", que pode iniciar a próxima descoberta) -
 * sem isso, alguma das duas teria que ser definida antes de existir.
 */
static void discovery_advance_queue(void);
static void discovery_start_for_slot(struct actuator_slot *slot);

static void discovery_queue_push(uint8_t slot_idx)
{
	if (discovery_queue_count >= MAX_ACTUATORS) {
		LOG_ERR("Fila de discovery cheia - nao deveria acontecer (slot %d perdido)",
			slot_idx);
		return;
	}
	discovery_queue[discovery_queue_tail] = slot_idx;
	discovery_queue_tail = (discovery_queue_tail + 1) % MAX_ACTUATORS;
	discovery_queue_count++;
}

static bool discovery_queue_pop(uint8_t *slot_idx_out)
{
	if (discovery_queue_count == 0) {
		return false;
	}
	*slot_idx_out = discovery_queue[discovery_queue_head];
	discovery_queue_head = (discovery_queue_head + 1) % MAX_ACTUATORS;
	discovery_queue_count--;
	return true;
}

static void discovery_complete(struct bt_gatt_dm *dm, void *ctx)
{
	struct actuator_slot *slot = (struct actuator_slot *)ctx;
	const struct bt_gatt_dm_attr *gatt_service_attr = bt_gatt_dm_service_get(dm);
	const struct bt_gatt_service_val *gatt_service_val =
		bt_gatt_dm_attr_service_val(gatt_service_attr);
	const struct bt_gatt_dm_attr *chrc;
	const struct bt_gatt_dm_attr *desc;

	LOG_INF("Discovery completo no atuador (slot %d)", slot_index(slot));

	if (bt_uuid_cmp(gatt_service_val->uuid, BT_UUID_ACTUATOR_SERVICE)) {
		LOG_ERR("Servico descoberto nao e o esperado (slot %d)", slot_index(slot));
		bt_gatt_dm_data_release(dm);
		slot_enter_error(slot);
		discovery_advance_queue();
		return;
	}

	chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_ACTUATOR_RX);
	if (chrc) {
		desc = bt_gatt_dm_desc_by_uuid(dm, chrc, BT_UUID_ACTUATOR_RX);
		if (desc) {
			slot->rx_handle = desc->handle;
		}
	}

	chrc = bt_gatt_dm_char_by_uuid(dm, BT_UUID_ACTUATOR_TX);
	if (chrc) {
		desc = bt_gatt_dm_desc_by_uuid(dm, chrc, BT_UUID_ACTUATOR_TX);
		if (desc) {
			slot->tx_handle = desc->handle;
		}
		desc = bt_gatt_dm_desc_by_uuid(dm, chrc, BT_UUID_GATT_CCC);
		if (desc) {
			slot->tx_ccc_handle = desc->handle;
		}
	}

	LOG_INF("Atuador (slot %d) - handles: RX=%u TX=%u TX_CCC=%u", slot_index(slot),
		slot->rx_handle, slot->tx_handle, slot->tx_ccc_handle);

	bt_gatt_dm_data_release(dm);

	if (!slot->rx_handle || !slot->tx_handle || !slot->tx_ccc_handle) {
		LOG_ERR("Nao encontrei RX/TX/CCC esperados (slot %d) - UUIDs batem "
			"com o protocol.py atual do coleta_ble?",
			slot_index(slot));
		slot_enter_error(slot);
		discovery_advance_queue();
		return;
	}

	subscribe_tx(slot);
	discovery_advance_queue();
}

static void discovery_service_not_found(struct bt_conn *conn, void *ctx)
{
	struct actuator_slot *slot = (struct actuator_slot *)ctx;

	LOG_WRN("Servico do atuador nao encontrado (slot %d)", slot_index(slot));
	slot_enter_error(slot);
	discovery_advance_queue();
}

static void discovery_error(struct bt_conn *conn, int err, void *ctx)
{
	struct actuator_slot *slot = (struct actuator_slot *)ctx;

	LOG_ERR("Erro de discovery no atuador (slot %d, err %d)", slot_index(slot), err);
	slot_enter_error(slot);
	discovery_advance_queue();
}

static const struct bt_gatt_dm_cb discovery_cb = {
	.completed = discovery_complete,
	.service_not_found = discovery_service_not_found,
	.error_found = discovery_error,
};

static void discovery_start_for_slot(struct actuator_slot *slot)
{
	int err;

	discovery_busy = true;

	err = bt_gatt_dm_start(slot->conn, BT_UUID_ACTUATOR_SERVICE, &discovery_cb, slot);
	if (err) {
		LOG_ERR("bt_gatt_dm_start (slot %d) falhou (err %d)", slot_index(slot), err);
		slot_enter_error(slot);
		discovery_busy = false;
		discovery_advance_queue();
	}
}

/* Chamado sempre que a descoberta atual termina (por qualquer motivo) -
 * libera o "turno" e, se houver alguém na fila, inicia o próximo.
 */
static void discovery_advance_queue(void)
{
	uint8_t slot_idx;

	discovery_busy = false;

	while (discovery_queue_pop(&slot_idx)) {
		struct actuator_slot *slot = &slots[slot_idx];

		/* O atuador pode ter desconectado enquanto esperava na fila -
		 * nesse caso so pula para o proximo, sem tentar discovery
		 * numa conexao que ja nao existe mais.
		 */
		if (!slot->conn) {
			continue;
		}
		discovery_start_for_slot(slot);
		return;
	}
}

/* --- Ciclo de vida da conexao --- */

int actuator_manager_start(struct bt_conn *conn, const bt_addr_le_t *addr)
{
	struct actuator_slot *slot = find_by_addr(addr);

	if (!slot) {
		LOG_ERR("actuator_manager_start para endereco desconhecido");
		return -ENOENT;
	}

	slot->conn = conn;
	slot_set_state(slot, ACTUATOR_STATE_CONNECTED);

	if (discovery_busy) {
		LOG_INF("Discovery de outro atuador em andamento - slot %d entra na fila",
			slot_index(slot));
		discovery_queue_push(slot_index(slot));
		return 0;
	}

	discovery_start_for_slot(slot);
	return 0;
}

bool actuator_manager_owns_conn(struct bt_conn *conn)
{
	for (int i = 0; i < MAX_ACTUATORS; i++) {
		if (slots[i].conn == conn) {
			return true;
		}
	}
	return false;
}

void actuator_manager_conn_lost(struct bt_conn *conn)
{
	for (int i = 0; i < MAX_ACTUATORS; i++) {
		if (slots[i].conn != conn) {
			continue;
		}

		k_work_cancel_delayable(&slots[i].param_work);
		k_work_cancel_delayable(&slots[i].fast_timeout_work);

		slots[i].conn = NULL;
		slots[i].rx_handle = 0;
		slots[i].tx_handle = 0;
		slots[i].tx_ccc_handle = 0;
		slots[i].tx_sub_active = false;
		slots[i].speed = ACTUATOR_SPEED_SLOW;
		memset(&slots[i].tx_sub_params, 0, sizeof(slots[i].tx_sub_params));

		if (slots[i].state == ACTUATOR_STATE_EMPTY) {
			LOG_INF("Atuador (slot %d) desconectado apos remocao - liberado", i);
			memset(&slots[i].addr, 0, sizeof(slots[i].addr));
		} else {
			LOG_INF("Atuador (slot %d) desconectado - continua na lista, "
				"tentara reconectar", i);
			slot_set_state(&slots[i], ACTUATOR_STATE_WAITING);
		}
		return;
	}
}

/* --- Envio de comandos --- */

int actuator_manager_send_to(uint8_t slot, const uint8_t *data, uint16_t len)
{
	if (slot >= MAX_ACTUATORS) {
		return -EINVAL;
	}

	/* READY, nao apenas "tem conn": entre CONNECTED e READY o TX ainda
	 * nao foi assinado, entao a resposta do atuador se perderia.
	 */
	if (slots[slot].state != ACTUATOR_STATE_READY || !slots[slot].conn ||
	    !slots[slot].rx_handle) {
		return -ENOTCONN;
	}

	return bt_gatt_write_without_response(slots[slot].conn, slots[slot].rx_handle, data, len,
					      false);
}

int actuator_manager_disconnect(uint8_t slot)
{
	if (slot >= MAX_ACTUATORS) {
		return -EINVAL;
	}

	if (!slots[slot].conn) {
		return -ENOTCONN;
	}

	LOG_INF("Desconexao do slot %d solicitada pela interface", slot);
	return bt_conn_disconnect(slots[slot].conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

uint8_t actuator_manager_count_connected(void)
{
	uint8_t n = 0;

	for (int i = 0; i < MAX_ACTUATORS; i++) {
		if (slots[i].conn) {
			n++;
		}
	}
	return n;
}

uint8_t actuator_manager_count_ready(void)
{
	uint8_t n = 0;

	for (int i = 0; i < MAX_ACTUATORS; i++) {
		if (slots[i].state == ACTUATOR_STATE_READY) {
			n++;
		}
	}
	return n;
}
