/*
 * Registro de eventos - ver actuator_regevent.h e docs/REGISTRO_EVENTOS.md.
 * Porta de BLE/Atuador/atRegEvent.c (fwBLE).
 *
 * CONCORRENCIA: os eventos chegam de varias threads (laco de controle na
 * fila do sistema, host BLE no IFCC_SAVE...) e entram numa k_msgq. Toda
 * operacao na flash (montagem, descarga, leitura) roda sob `flash_lock`:
 *  - a descarga periodica roda numa fila propria, de prioridade baixa -
 *    um apagamento de setor da NOR (ate' ~240 ms na MX25R) nao pode
 *    segurar o laco de controle (fila do sistema, watchdog de 500 ms) nem
 *    o polling I2C (sensor_workq);
 *  - a leitura (IFCC_START/SEQ_READ_EVENT) roda direto no contexto do
 *    host BLE, sincrona: descarrega a fila (liquidaFila do original) e le.
 *    O fwBLE fazia isso em estados (IFCCS_WAIT_*_READ_EVENT) porque o
 *    super-loop nao podia esperar a FAT; aqui o status ja' volta READY.
 */

#include "actuator_regevent.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(actuator_regevent, LOG_LEVEL_INF);

/* Particao "event_log" na flash externa - ver o overlay da placa. */
#define EVLOG_ID   PARTITION_ID(event_log_partition)
#define EVLOG_SIZE PARTITION_SIZE(event_log_partition)

/* NUM_EVENT_REG_FILE no original (tamanho do arquivo / sizeof(tEvent)). */
#define NUM_EVENT_REG_FILE ((uint32_t)(EVLOG_SIZE / ACTUATOR_REGEVENT_SIZE))

/* Setor de apagamento da NOR (4 KB na MX25R e na IS25LP do Painel BLE) -
 * o buffer avanca de setor em setor, apagando o proximo ao entrar nele.
 */
#define EVLOG_SECTOR_SIZE 4096U
#define EV_POR_SETOR      (EVLOG_SECTOR_SIZE / ACTUATOR_REGEVENT_SIZE)

BUILD_ASSERT(EVLOG_SIZE % EVLOG_SECTOR_SIZE == 0, "event_log precisa ser multiplo do setor");

/* TAM_FILA_EVENT / MAX_INTERVALOR_MILI_S do original. */
#define TAM_FILA_EVENT        32
#define MAX_INTERVALO_MS      2000

/* Valor do contador de voltas numa posicao apagada (0xFFFF) - nunca usado
 * como volta valida.
 */
#define VOLTA_APAGADA 0xFFFFU

#define REGEVENT_WORKQ_STACK_SIZE 2048
#define REGEVENT_WORKQ_PRIORITY   10 /* abaixo da sensor_workq (5) */

/* tEvent (atRegEvent.h). No IAR os 14 bytes do ssEvent viram 16 por
 * alinhamento e reserv[] fica com tamanho zero - os bytes 14..15 eram
 * lixo. Aqui levam o contador de voltas (DIVERGENCIA, ver o .h).
 */
struct __packed tevent {
	uint32_t index;
	uint16_t event;
	uint8_t ano;
	uint8_t mes;
	uint8_t dia;
	uint8_t hora;
	uint8_t minu;
	uint8_t segun;
	uint16_t mil_seg;
	uint16_t volta;
};

BUILD_ASSERT(sizeof(struct tevent) == ACTUATOR_REGEVENT_SIZE, "tEvent tem 16 bytes");

K_MSGQ_DEFINE(fila_event, sizeof(struct tevent), TAM_FILA_EVENT, 4);

static K_MUTEX_DEFINE(fila_lock); /* so' threads registram eventos (nenhuma ISR) */
static int64_t ult_evento_ms = -1; /* timerIntervaloMiliS do original */
static atomic_t descartados;
static bool iniciado; /* fila de gravacao ja' subiu (actuator_regevent_init) */

static struct k_work_q regevent_workq;
static K_THREAD_STACK_DEFINE(regevent_workq_stack, REGEVENT_WORKQ_STACK_SIZE);
static struct k_work_delayable descarga_work;

/* Tudo daqui pra baixo so' sob flash_lock. */
static K_MUTEX_DEFINE(flash_lock);
static const struct flash_area *fa;
static bool montado;
static bool montagem_falhou;
static uint32_t head;  /* contAbsol.absolIndexRegEvent: proxima posicao a gravar */
static uint16_t volta; /* volta corrente do buffer (bytes 14..15) */

/* sAcesSeq do original */
static bool rd_ativo;
static uint32_t rd_next;  /* proxima posicao a ler, ou OVER_INDEX = fim */
static uint32_t rd_lidos;

/* atRegEventValido() */
static bool evento_valido(uint16_t id)
{
	static const uint16_t fixos[] = {
		EV_SIS_, EV_SIS_POWER_UP, EV_SIS_POWER_DOWN, EV_SIS_WDT_RST, EV_SIS_SW_RST,
		EV_SIS_MMC_EJ, EV_SIS_MMC_IN, EV_SIS_MMC_IN_FAT, EV_SIS_REG_FIM_FILE,
		EV_SIS_CURV_FIM_FILE, EV_SIS_REG_INV, EV_SIS_REG_INV_LIDO, EV_SIS_CURV_INV_LIDA,
		EV_SIS_CURV_COLET, EV_AL_BQ, EV_AL_ON, EV_AL_OFF,
		EV_LOC_PST, EV_LOC_PAR, EV_LOC_ABR, EV_LOC_FEC, EV_LOC_QT, EV_LOC_LOCAL,
		EV_LOC_DESLIG, EV_LOC_REMOTO, EV_LOC_INFO, EV_LOC_PARAM, EV_LOC_PARAM_FAB,
		EV_REM_ESD, EV_REM_PST, EV_REM_PAR, EV_REM_ABR, EV_REM_FEC, EV_REM_INIB_LOC,
		EV_REM_POSIC, EV_REM_QT,
		EV_REM_DISC_PAR_A, EV_REM_DISC_FEC_A, EV_REM_DISC_ABR_A, EV_REM_DISC_AUT_A,
		EV_REM_DISC_ESD_A, EV_REM_DISC_PST_A, EV_REM_DISC_7_A, EV_REM_DISC_8_A,
		EV_REM_DISC_PAR_D, EV_REM_DISC_FEC_D, EV_REM_DISC_ABR_D, EV_REM_DISC_AUT_D,
		EV_REM_DISC_ESD_D, EV_REM_DISC_PST_D, EV_REM_DISC_7_D, EV_REM_DISC_8_D,
		EV_OPE_NUL, EV_OPE_L_SUPER, EV_OPE_L_INFER, EV_OPE_INCR, EV_OPE_DECR,
		EV_OPE_M_PARA,
		EV_PARAM_SAI, EV_PARAM_SALV,
		EV_ACAO_CONFIG_FAL_COM_PAR, EV_ACAO_CONFIG_FAL_COM_ABRE,
		EV_ACAO_CONFIG_FAL_COM_FECHA, EV_ACAO_CONFIG_FAL_COM_POSIC,
		EV_FABR_USUAR,
	};

	if (id >= EV_AL_ON && id < EV_AL_ON + ACTUATOR_REGEVENT_NUM_ALARMS_FWBLE) {
		return true;
	}
	if (id >= EV_AL_OFF && id < EV_AL_OFF + ACTUATOR_REGEVENT_NUM_ALARMS_FWBLE) {
		return true;
	}
	/* NUM_FABR_USUAR = 100 */
	if (id >= EV_FABR_USUAR && id < EV_FABR_USUAR + 100) {
		return true;
	}
	for (size_t i = 0; i < ARRAY_SIZE(fixos); i++) {
		if (fixos[i] == id) {
			return true;
		}
	}
	return false;
}

/* DIVERGENCIA (sem RTC): tempo desde o boot no lugar da data/hora, com
 * mes = 0 marcando o formato - ver actuator_regevent.h.
 */
static void carimbo_tempo(struct tevent *e, int64_t agora_ms)
{
	uint64_t s = (uint64_t)agora_ms / 1000U;
	uint32_t dias = MIN(s / 86400U, 0xFFFFU);

	e->ano = (uint8_t)(dias >> 8);
	e->mes = 0;
	e->dia = (uint8_t)dias;
	e->hora = (uint8_t)((s / 3600U) % 24U);
	e->minu = (uint8_t)((s / 60U) % 60U);
	e->segun = (uint8_t)(s % 60U);
}

void actuator_regevent(uint16_t ev)
{
	struct tevent e = { 0 };
	uint32_t na_fila;
	int err;

	if (!evento_valido(ev)) {
		ev = EV_SIS_REG_INV;
	}

	/* Carimbo e put juntos sob o lock: a ordem na fila e' a ordem dos
	 * carimbos, mesmo com eventos de threads diferentes.
	 */
	k_mutex_lock(&fila_lock, K_FOREVER);
	int64_t agora = k_uptime_get();

	e.event = ev;
	carimbo_tempo(&e, agora);
	/* milSeg: ms desde o evento anterior, ate' 2000 (o original faz
	 * MAX_INTERVALOR_MILI_S - tempo restante do timer recarregado a cada
	 * evento; o timer comeca expirado, entao o 1o evento leva 2000).
	 */
	e.mil_seg = (ult_evento_ms < 0) ? MAX_INTERVALO_MS
					 : (uint16_t)MIN(agora - ult_evento_ms, MAX_INTERVALO_MS);
	err = k_msgq_put(&fila_event, &e, K_NO_WAIT);
	if (err == 0) {
		ult_evento_ms = agora;
	}
	k_mutex_unlock(&fila_lock);

	if (err) {
		/* "Se a fila encher para de registrar!" - igual ao original. */
		atomic_inc(&descartados);
		return;
	}

	if (!iniciado) {
		return; /* fica na fila; init agenda a descarga */
	}

	/* Descarga quando a fila fica velha (2 s sem evento novo) ou grande
	 * (metade) - regEventSalv().
	 */
	na_fila = k_msgq_num_used_get(&fila_event);
	k_work_reschedule_for_queue(&regevent_workq, &descarga_work,
				    na_fila >= TAM_FILA_EVENT / 2 ? K_NO_WAIT
								  : K_MSEC(MAX_INTERVALO_MS));
}

static bool posicao_apagada(const struct tevent *e)
{
	const uint8_t *p = (const uint8_t *)e;

	for (size_t i = 0; i < sizeof(*e); i++) {
		if (p[i] != 0xFF) {
			return false;
		}
	}
	return true;
}

/* Registro gravado por nos naquela posicao: indice bate, evento conhecido,
 * volta valida. Cobre registro meio gravado (falta de energia no meio da
 * escrita).
 */
static bool registro_valido(const struct tevent *e, uint32_t slot)
{
	return e->index == slot && e->volta != VOLTA_APAGADA && evento_valido(e->event);
}

static int ler_slot(uint32_t slot, struct tevent *e)
{
	return flash_area_read(fa, (off_t)slot * ACTUATOR_REGEVENT_SIZE, e, sizeof(*e));
}

static uint16_t proxima_volta(uint16_t v)
{
	v++;
	return (v == VOLTA_APAGADA) ? 0 : v;
}

/* Posicao gravada na volta `v`? (predicado da busca binaria) */
static bool slot_na_volta(uint32_t slot, uint16_t v)
{
	struct tevent e;

	return ler_slot(slot, &e) == 0 && registro_valido(&e, slot) && e.volta == v;
}

/* DIVERGENCIA: o original le a posicao de escrita do contAbsol (gravado a
 * cada descarga). Aqui ela e' achada pela propria flash: as posicoes
 * [0, head) estao na volta corrente L (a do slot 0) e [head, N) na volta
 * anterior ou apagadas - busca binaria pela 1a posicao fora da volta L
 * (~17 leituras de 16 bytes pra 65536 posicoes).
 */
static void achar_head(void)
{
	struct tevent e;

	if (ler_slot(0, &e) != 0 || !registro_valido(&e, 0)) {
		/* Slot 0 vazio: buffer novo, OU caiu a energia entre apagar o
		 * setor 0 e gravar a volta seguinte. O ultimo slot diz qual.
		 */
		if (ler_slot(NUM_EVENT_REG_FILE - 1, &e) == 0 &&
		    registro_valido(&e, NUM_EVENT_REG_FILE - 1)) {
			volta = proxima_volta(e.volta);
		} else {
			volta = 0;
		}
		head = 0;
		return;
	}

	uint32_t lo = 1;
	uint32_t hi = NUM_EVENT_REG_FILE;

	volta = e.volta;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (slot_na_volta(mid, volta)) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}
	head = lo;

	/* NOR so' grava sobre posicao apagada: se a posicao achada tiver
	 * resto de uma escrita interrompida, pula ate' uma apagada (ou ate' o
	 * inicio do proximo setor, que e' apagado ao entrar nele).
	 */
	while (head < NUM_EVENT_REG_FILE && (head % EV_POR_SETOR) != 0) {
		if (ler_slot(head, &e) == 0 && posicao_apagada(&e)) {
			break;
		}
		head++;
	}

	if (head >= NUM_EVENT_REG_FILE) {
		head = 0;
		volta = proxima_volta(volta);
	}
}

static bool montar(void)
{
	int err;

	if (montado) {
		return true;
	}
	if (montagem_falhou) {
		return false;
	}

	err = flash_area_open(EVLOG_ID, &fa);
	if (err || !flash_area_device_is_ready(fa)) {
		LOG_ERR("Flash do registro de eventos indisponivel (err %d) - eventos descartados",
			err);
		montagem_falhou = true;
		return false;
	}

	achar_head();
	montado = true;
	LOG_INF("Registro de eventos: %u posicoes, proxima %u, volta %u", NUM_EVENT_REG_FILE,
		head, volta);
	return true;
}

/* regEventSalv(): grava a fila na flash. */
static void descarregar(void)
{
	struct tevent e;
	bool fim_arquivo = false;
	atomic_val_t perdidos = atomic_clear(&descartados);
	int err;

	if (perdidos) {
		LOG_WRN("Fila de eventos cheia: %ld evento(s) descartado(s)", (long)perdidos);
	}

	while (k_msgq_peek(&fila_event, &e) == 0) {
		off_t off = (off_t)head * ACTUATOR_REGEVENT_SIZE;

		if ((head % EV_POR_SETOR) == 0) {
			err = flash_area_erase(fa, off, EVLOG_SECTOR_SIZE);
			if (err) {
				/* Sem setor apagado nao da pra gravar - o resto fica
				 * na fila pra proxima descarga.
				 */
				LOG_ERR("Falha ao apagar setor do registro em 0x%lx (err %d)",
					(long)off, err);
				k_work_reschedule_for_queue(&regevent_workq, &descarga_work,
							    K_MSEC(MAX_INTERVALO_MS));
				break;
			}
		}

		(void)k_msgq_get(&fila_event, &e, K_NO_WAIT);
		e.index = head;
		e.volta = volta;
		err = flash_area_write(fa, off, &e, sizeof(e));
		if (err) {
			LOG_ERR("Falha ao gravar evento %u na posicao %u (err %d)", e.event, head, err);
		}

		head++;
		if (head >= NUM_EVENT_REG_FILE) {
			head = 0;
			volta = proxima_volta(volta);
			fim_arquivo = true;
		}
	}

	if (fim_arquivo) {
		/* Mesmo do original: vai pra fila e sai na proxima descarga. */
		actuator_regevent(EV_SIS_REG_FIM_FILE);
	}
}

static void descarga_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&flash_lock, K_FOREVER);
	if (montar()) {
		descarregar();
	} else {
		k_msgq_purge(&fila_event);
	}
	k_mutex_unlock(&flash_lock);
}

static void marca_fim(uint8_t out[ACTUATOR_REGEVENT_SIZE])
{
	struct tevent e = { 0 };

	e.index = ACTUATOR_REGEVENT_OVER_INDEX;
	e.event = EV_SIS_REG_INV;
	memcpy(out, &e, sizeof(e));
}

/* Posicao do registro mais novo, ou OVER_INDEX se o buffer estiver vazio
 * (atRegEventGetIndexDefault(), sem o caso "head == 0 devolve 0").
 */
static uint32_t slot_mais_novo(void)
{
	struct tevent e;
	uint32_t s = (head == 0) ? NUM_EVENT_REG_FILE - 1 : head - 1;

	if (ler_slot(s, &e) == 0 && registro_valido(&e, s)) {
		return s;
	}
	return ACTUATOR_REGEVENT_OVER_INDEX;
}

/* atRegEventAcesTrat(): le rd_next e anda pro mais antigo.
 * DIVERGENCIA: ao passar do 0 vai pro fim do buffer (o original voltava
 * pra posicao de escrita - 1, repetindo o mais novo); para ao completar
 * a volta ou numa posicao apagada.
 */
static void ler_sequencia(uint8_t out[ACTUATOR_REGEVENT_SIZE])
{
	struct tevent e;
	uint32_t slot = rd_next;
	uint32_t anterior;
	uint32_t mais_novo = (head == 0) ? NUM_EVENT_REG_FILE - 1 : head - 1;

	if (slot == ACTUATOR_REGEVENT_OVER_INDEX || rd_lidos >= NUM_EVENT_REG_FILE) {
		marca_fim(out);
		return;
	}

	if (ler_slot(slot, &e) != 0 || posicao_apagada(&e)) {
		rd_next = ACTUATOR_REGEVENT_OVER_INDEX;
		marca_fim(out);
		return;
	}

	/* atRegEventRegistroValido(): registro estragado sai como
	 * EV_SIS_REG_INV, com o resto como foi lido.
	 */
	if (!registro_valido(&e, slot)) {
		e.event = EV_SIS_REG_INV;
	}
	memcpy(out, &e, sizeof(e));
	rd_lidos++;

	anterior = (slot == 0) ? NUM_EVENT_REG_FILE - 1 : slot - 1;
	rd_next = (anterior == mais_novo) ? ACTUATOR_REGEVENT_OVER_INDEX : anterior;
}

/* atRegEventIniAcesSeq() + 1a leitura. */
static void iniciar_sequencia(uint32_t index, uint8_t out[ACTUATOR_REGEVENT_SIZE])
{
	/* O original limita a NUM_EVENT_REG_FILE (posicao fora do arquivo);
	 * aqui indice fora da faixa vale como "o mais novo".
	 */
	if (index == ACTUATOR_REGEVENT_OVER_INDEX || index >= NUM_EVENT_REG_FILE) {
		index = slot_mais_novo();
	}

	rd_ativo = true;
	rd_lidos = 0;
	rd_next = index;
	ler_sequencia(out);
}

bool actuator_regevent_read_start(uint32_t index, uint8_t out[ACTUATOR_REGEVENT_SIZE])
{
	bool ok;

	k_mutex_lock(&flash_lock, K_FOREVER);
	ok = montar();
	if (ok) {
		descarregar(); /* liquidaFila */
		iniciar_sequencia(index, out);
	} else {
		marca_fim(out);
	}
	k_mutex_unlock(&flash_lock);
	return ok;
}

bool actuator_regevent_read_next(uint8_t out[ACTUATOR_REGEVENT_SIZE])
{
	bool ok;

	k_mutex_lock(&flash_lock, K_FOREVER);
	ok = montar();
	if (ok) {
		descarregar();
		if (rd_ativo) {
			ler_sequencia(out);
		} else {
			/* IFCC_SEQ_READ_EVENT sem sequencia: atRegEventIniAcesSeqDef() */
			iniciar_sequencia(ACTUATOR_REGEVENT_OVER_INDEX, out);
		}
	} else {
		marca_fim(out);
	}
	k_mutex_unlock(&flash_lock);
	return ok;
}

void actuator_regevent_init(enum actuator_regevent_boot boot)
{
	struct k_work_queue_config cfg = {
		.name = "regevent",
		.no_yield = false,
	};

	k_work_queue_init(&regevent_workq);
	k_work_queue_start(&regevent_workq, regevent_workq_stack,
			   K_THREAD_STACK_SIZEOF(regevent_workq_stack), REGEVENT_WORKQ_PRIORITY, &cfg);
	k_work_init_delayable(&descarga_work, descarga_handler);
	iniciado = true;

	/* atRegPowerReset() - DIVERGENCIA: sem EV_SIS_POWER_DOWN (o original
	 * tira a hora da queda do RTC com bateria, que nao temos).
	 */
	switch (boot) {
	case ACTUATOR_REGEVENT_BOOT_WDT:
		actuator_regevent(EV_SIS_WDT_RST);
		break;
	case ACTUATOR_REGEVENT_BOOT_SW:
		actuator_regevent(EV_SIS_SW_RST);
		break;
	case ACTUATOR_REGEVENT_BOOT_POWER_UP:
	default:
		actuator_regevent(EV_SIS_POWER_UP);
		break;
	}
}
