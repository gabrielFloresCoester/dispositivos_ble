/*
 * Protocolo BLE de exposicao do papel de atuador (acgl/ifFerConfig) -
 * ver o comentario grande em actuator_service.h e
 * docs/SENSORES_I2C.md.
 *
 * ESCOPO: GTM_REQUEST e GTM_SEND. Areas REAIS do ifFerConfig:
 * Comando (0x00800000), Painel/paramDado (0x00800800, ver
 * actuator_params.h e docs/PARAMETROS.md), Sensor (0x00804080) e
 * Painel Remoto (0x00805900, so' escrita). Areas SINTETICAS nossas
 * (0xF0000000+): Info e Status/Alarmes. Historico da primeira versao,
 * so' leitura, em duas areas:
 *   - Sensor (endereco 0x00804080 = MSG_EX_ADDRESS_CONFIG_SENSOR no
 *     ifFerConfig.h original), servindo o struct actuator_sensor_data
 *     (ver actuator_sensors.c) - area REAL do fwBLE, fidelidade byte
 *     a byte.
 *   - Info (endereco sintetico 0xF0000000, NAO existe no fwBLE
 *     original) - hoje so' expoe qual fonte de torque esta ativa
 *     (selecao automatica entre ADS1000/PCA9536, ver
 *     actuator_sensors.h e docs/SENSORES_I2C.md).
 * Qualquer outro endereco ou tipo de mensagem (GTM_SEND, GTM_STATUS,
 * GTM_ALLOCTION...) responde GTM_NEG - nao trava nada, so' ainda nao
 * atende. Areas que fazem proxy pra outro I2C (FSA, IO
 * Digital/Analogica, Rede) exigiriam proCoMsg.c/proCoMsgEx.c - fora de
 * escopo aqui, ver docs/SENSORES_I2C.md.
 *
 * DIFERENCA DELIBERADA do offset arbitrario do ifFerConfig original:
 * o cliente real (Gateway) so' pediu, ate' hoje, exatamente 10 bytes a
 * partir da base da area (ver a decodificacao do frame de exemplo em
 * PROTOCOLO_INTERFACE.md). Este modulo aceita qualquer endereco DENTRO
 * do struct (nao so' a base), computando o offset como o original
 * fazia, mas nao valida contra um "tamanho de area" reservado maior
 * que o struct de verdade (o ifFerConfig original reserva 0x800=2048
 * bytes de espaco de endereco por area, bem mais que o struct real -
 * um pedido de tamanho grande la' leria memoria alem do struct.
 * Preferimos ser mais conservadores aqui: o limite e' sizeof(struct
 * actuator_sensor_data), nao um espaco de endereco arbitrario).
 */

#include "actuator_service.h"
#include "actuator_sensors.h"
#include "actuator_alarm.h"
#include "actuator_panel.h"
#include "actuator_control.h"
#include "actuator_mode.h"
#include "actuator_params.h"
#include "actuator_fsa_cfg.h"
#include "actuator_regevent.h"

#include <string.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/toolchain.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(actuator_service, LOG_LEVEL_INF);

/* MSG_EX_ADDRESS_CONFIG_SENSOR no ifFerConfig.h original */
#define SENSOR_AREA_ADDR 0x00804080UL

/* Area SINTETICA NOSSA, fora do espaco real de enderecos proCo
 * (0x00800000-0x008FFFFF) de proposito - nunca colide com nada que um
 * Gateway real peca, e nao existe no ifFerConfig original. Guarda
 * informacao que so' faz sentido pra unidades com selecao automatica
 * de variante de sensor (ver docs/SENSORES_I2C.md) - hoje, so' qual
 * fonte de torque esta ativa.
 *
 * Formato (1 byte): enum actuator_torque_source (actuator_sensors.h) -
 * 0=nenhum, 1=analogico (ADS1000), 2=ON/OFF (PCA9536).
 */
#define INFO_AREA_ADDR 0xF0000000UL
#define INFO_AREA_LEN  1U

/* Outra area SINTETICA NOSSA (mesmo espirito da Info acima) - resumo do
 * estado do papel de atuador pra interface: alarmes + estado de
 * movimento. NAO e' a area real de alarmes do fwBLE
 * (MSG_EX_ADDRESS_CONFIG_ALARMS = 0x00804880, que e' o al_stt[] inteiro,
 * 60 x 2 bytes) - essa so' entra quando o conjunto completo de alarmes
 * for portado (ver nota de risco de fidelidade em actuator_alarm.h).
 *
 * Formato (5 bytes):
 *   [0..1] u16 LE  bitmap de actuator_alarm_info_bitmap()
 *   [2]    u8      flags: bit0 = movimento bloqueado por alarme
 *   [3]    u8      actuator_control_get_mov_stt() (enum actuator_mov_status:
 *                  1=L_SUPER 2=L_INFER 3=INCR/abrindo 4=DECR/fechando 5=PARADO)
 *   [4]    u8      actuator_mode_get() (enum actuator_mode: 0=DESLIGA 1=PARA
 *                  2=LOCAL 3=REMOTO 4=PARAM 5=INFO)
 */
#define ALARM_AREA_ADDR 0xF0000010UL
#define ALARM_AREA_LEN  5U

/* Area "Comando" REAL do ifFerConfig (MSG_EX_ADDRESS_FER_CONFIG =
 * 0x00800000) - ifFerConfigCmd_t, 74 bytes com pack(2):
 *   [0..1]   cmdStatus (ifFerConfigCmdStatus_t, u16)
 *   [2..9]   frame (union; os 2 primeiros bytes sao o comando, u16)
 *   [10..73] buffer[NUM_BYTES_BUFFER = 64]
 * Executados hoje: IFCC_SAVE, IFCC_RESTORE e a leitura do registro de
 * eventos (IFCC_START_READ_EVENT/IFCC_SEQ_READ_EVENT) - ver
 * handle_cmd_write().
 */
#define CMD_AREA_ADDR  0x00800000UL
#define CMD_AREA_LEN   74U
#define CMD_OFF_STATUS 0
#define CMD_OFF_CMD    2
#define CMD_OFF_INDEX  4  /* ifFerConfigCmdEvent_t.index (u32, pack(2)) */
#define CMD_OFF_BUFFER 10

/* ifFerConfigEnuCmd_t / ifFerConfigCmdStatus_t (ifFerConfig.h) */
#define IFCC_WAIT             0x0000
#define IFCC_SAVE             0x0002
#define IFCC_RESTORE          0x0004
#define IFCC_START_READ_EVENT 0x0400
#define IFCC_SEQ_READ_EVENT   0x0401
#define IFCCS_READY           0x0000
#define IFCCS_FAIL            0x0002

BUILD_ASSERT(CMD_OFF_BUFFER + ACTUATOR_REGEVENT_SIZE <= CMD_AREA_LEN,
	     "tEvent precisa caber no buffer da area Comando");

static uint8_t cmd_area[CMD_AREA_LEN];

/* Espelha msg_t (BLE/acgl/acgl.h) - 8 bytes, little-endian (nativo no
 * Cortex-M). __packed evita padding antes do address (uint32) que a
 * struct teria por alinhamento natural.
 */
struct __packed msg_hdr {
	uint16_t id_msg_host;
	uint8_t type;
	uint8_t len_data;
	uint32_t address;
};

BUILD_ASSERT(sizeof(struct msg_hdr) == 8, "msg_hdr precisa ter 8 bytes (protocolo acgl)");

static void set_response_header(struct msg_hdr *resp, const struct msg_hdr *req,
				 enum acg_msg_type type, uint8_t len_data)
{
	resp->id_msg_host = req->id_msg_host;
	resp->type = (uint8_t)type;
	resp->len_data = len_data;
	resp->address = req->address;
}

static size_t handle_sensor_area(const struct msg_hdr *req, uint8_t *resp_buf, size_t resp_buf_len)
{
	struct msg_hdr *resp = (struct msg_hdr *)resp_buf;
	struct actuator_sensor_data data;
	uint32_t addr = req->address;
	uint32_t offset;
	uint8_t avail, want, send_len;

	if (!actuator_sensors_get(&data)) {
		/* Filtro de posicao/torque ainda nao encheu o buffer - recusa
		 * em vez de mandar lixo/zeros como se fosse dado valido.
		 */
		LOG_DBG("GTM_REQUEST na area Sensor, mas ainda sem leitura valida");
		set_response_header(resp, req, ACG_MSG_NEG, 0);
		return sizeof(*resp);
	}

	offset = addr - SENSOR_AREA_ADDR;
	avail = (uint8_t)(sizeof(data) - offset);
	want = req->len_data;
	send_len = want < avail ? want : avail;

	if (resp_buf_len < sizeof(*resp) + send_len) {
		/* Nao deveria acontecer com ACTUATOR_SERVICE_MAX_RESP_LEN
		 * dimensionado certo do lado de quem chama - por seguranca,
		 * responde sem payload em vez de estourar o buffer.
		 */
		LOG_ERR("Buffer de resposta pequeno demais (%zu) pra %u bytes de payload",
			resp_buf_len, send_len);
		set_response_header(resp, req, ACG_MSG_NEG, 0);
		return sizeof(*resp);
	}

	set_response_header(resp, req, ACG_MSG_RESPONSE, send_len);
	memcpy(resp_buf + sizeof(*resp), (const uint8_t *)&data + offset, send_len);

	return sizeof(*resp) + send_len;
}

static size_t handle_info_area(const struct msg_hdr *req, uint8_t *resp_buf, size_t resp_buf_len)
{
	struct msg_hdr *resp = (struct msg_hdr *)resp_buf;
	uint8_t info_byte = (uint8_t)actuator_sensors_torque_source();
	uint8_t send_len = req->len_data < INFO_AREA_LEN ? req->len_data : INFO_AREA_LEN;

	if (resp_buf_len < sizeof(*resp) + send_len) {
		set_response_header(resp, req, ACG_MSG_NEG, 0);
		return sizeof(*resp);
	}

	set_response_header(resp, req, ACG_MSG_RESPONSE, send_len);
	if (send_len > 0) {
		resp_buf[sizeof(*resp)] = info_byte;
	}

	return sizeof(*resp) + send_len;
}

static size_t handle_alarm_area(const struct msg_hdr *req, uint8_t *resp_buf, size_t resp_buf_len)
{
	struct msg_hdr *resp = (struct msg_hdr *)resp_buf;
	uint8_t buf[ALARM_AREA_LEN];
	uint8_t send_len = req->len_data < ALARM_AREA_LEN ? req->len_data : ALARM_AREA_LEN;

	sys_put_le16(actuator_alarm_info_bitmap(), buf);
	buf[2] = actuator_alarm_get_blocked() ? 0x01 : 0x00;
	buf[3] = (uint8_t)actuator_control_get_mov_stt();
	buf[4] = (uint8_t)actuator_mode_get();

	if (resp_buf_len < sizeof(*resp) + send_len) {
		set_response_header(resp, req, ACG_MSG_NEG, 0);
		return sizeof(*resp);
	}

	set_response_header(resp, req, ACG_MSG_RESPONSE, send_len);
	memcpy(resp_buf + sizeof(*resp), buf, send_len);
	return sizeof(*resp) + send_len;
}

/* Resposta GTM_RESPONSE com len bytes de payload (ja' limitados pelo
 * chamador ao fim da area), ou GTM_NEG se nao couber no buffer.
 */
static size_t respond_payload(const struct msg_hdr *req, uint8_t *resp_buf, size_t resp_buf_len,
			      const uint8_t *payload, uint8_t len)
{
	struct msg_hdr *resp = (struct msg_hdr *)resp_buf;

	if (resp_buf_len < sizeof(*resp) + len) {
		LOG_WRN("Resposta de %u bytes nao cabe no buffer (%zu)", len, resp_buf_len);
		set_response_header(resp, req, ACG_MSG_NEG, 0);
		return sizeof(*resp);
	}

	set_response_header(resp, req, ACG_MSG_RESPONSE, len);
	memcpy(resp_buf + sizeof(*resp), payload, len);
	return sizeof(*resp) + len;
}

/* Area Painel (paramDado) - ifFerConfigGetRequest() com offset
 * arbitrario. Diferente do original, nao le alem do fim do struct (o
 * espaco reservado la' e' 0x800, o struct tem 144 bytes).
 */
static size_t handle_params_read(const struct msg_hdr *req, uint8_t *resp_buf,
				 size_t resp_buf_len)
{
	uint8_t buf[ACTUATOR_PARAMS_SIZE];
	uint16_t offset = (uint16_t)(req->address - ACTUATOR_PARAMS_AREA_ADDR);
	uint8_t len = MIN(req->len_data, ACTUATOR_PARAMS_SIZE - offset);

	(void)actuator_params_read(offset, buf, len);
	return respond_payload(req, resp_buf, resp_buf_len, buf, len);
}

static size_t handle_cmd_read(const struct msg_hdr *req, uint8_t *resp_buf, size_t resp_buf_len)
{
	uint16_t offset = (uint16_t)(req->address - CMD_AREA_ADDR);
	uint8_t len = MIN(req->len_data, CMD_AREA_LEN - offset);

	return respond_payload(req, resp_buf, resp_buf_len, &cmd_area[offset], len);
}

/* Area FSA (0x00801000) - so' o campo tempoReverContat (0x00801010, 2
 * bytes) e' emulado, ver actuator_fsa_cfg.h.
 */
static size_t handle_fsa_read(const struct msg_hdr *req, uint8_t *resp_buf, size_t resp_buf_len)
{
	uint8_t buf[ACTUATOR_FSA_TEMPO_REVER_LEN];
	uint16_t offset = (uint16_t)(req->address - ACTUATOR_FSA_TEMPO_REVER_ADDR);
	uint8_t len = MIN(req->len_data, ACTUATOR_FSA_TEMPO_REVER_LEN - offset);

	(void)actuator_fsa_cfg_read(offset, buf, len);
	return respond_payload(req, resp_buf, resp_buf_len, buf, len);
}

static size_t handle_request(const struct msg_hdr *req, uint8_t *resp_buf, size_t resp_buf_len)
{
	struct msg_hdr *resp = (struct msg_hdr *)resp_buf;
	uint32_t addr = req->address;

	if (addr >= SENSOR_AREA_ADDR && addr < SENSOR_AREA_ADDR + sizeof(struct actuator_sensor_data)) {
		return handle_sensor_area(req, resp_buf, resp_buf_len);
	}

	if (addr >= INFO_AREA_ADDR && addr < INFO_AREA_ADDR + INFO_AREA_LEN) {
		return handle_info_area(req, resp_buf, resp_buf_len);
	}

	if (addr >= ALARM_AREA_ADDR && addr < ALARM_AREA_ADDR + ALARM_AREA_LEN) {
		return handle_alarm_area(req, resp_buf, resp_buf_len);
	}

	if (addr >= ACTUATOR_PARAMS_AREA_ADDR &&
	    addr < ACTUATOR_PARAMS_AREA_ADDR + ACTUATOR_PARAMS_SIZE) {
		return handle_params_read(req, resp_buf, resp_buf_len);
	}

	if (addr >= CMD_AREA_ADDR && addr < CMD_AREA_ADDR + CMD_AREA_LEN) {
		return handle_cmd_read(req, resp_buf, resp_buf_len);
	}

	if (addr >= ACTUATOR_FSA_TEMPO_REVER_ADDR &&
	    addr < ACTUATOR_FSA_TEMPO_REVER_ADDR + ACTUATOR_FSA_TEMPO_REVER_LEN) {
		return handle_fsa_read(req, resp_buf, resp_buf_len);
	}

	LOG_WRN("GTM_REQUEST endereco 0x%08x nao suportado ainda", addr);
	set_response_header(resp, req, ACG_MSG_NEG, 0);
	return sizeof(*resp);
}

/* Escrita na area Comando - ifFerConfigReceive() + ifFerConfigExecCmd().
 * Os bytes sempre caem na imagem (como o memcpy original); se o campo de
 * comando foi tocado, executa:
 *   IFCC_WAIT    -> nada
 *   IFCC_SAVE    -> actuator_params_save()    (paramSalva)
 *   IFCC_RESTORE -> actuator_params_restore() (paramRestau)
 *   IFCC_START_READ_EVENT / IFCC_SEQ_READ_EVENT -> registro de eventos
 *                  (atRegEventIniAcesSeq/atRegEventIniAces), tEvent no
 *                  buffer [10..25]
 *   outro        -> volta pra IFCC_WAIT e recusa (ifFerConfigAccessCmdBLE)
 * DIVERGENCIA: no fwBLE, SAVE/RESTORE estao fora de accessCmdBLE (la' a
 * BLE nao grava parametros, so' IHM/fieldbus). Aqui a interface BLE e' o
 * meio de configurar, entao o salvar explicito entra (decisao do Felipe,
 * 2026-10-01).
 * DIVERGENCIA (leitura de eventos): no fwBLE o status passa por
 * IFCCS_WAIT_*_READ_EVENT enquanto a FAT/flash e' lida no super-loop, e o
 * cliente consulta ate' READY. Aqui a leitura e' sincrona (flash SPI, sem
 * FAT) - o status ja' volta READY com o registro no buffer antes do
 * GTM_CONFIRM. Um cliente que consulta o status continua funcionando.
 */
static bool handle_cmd_write(uint16_t offset, const uint8_t *data, uint8_t len)
{
	uint16_t cmd;
	bool ok;

	memcpy(&cmd_area[offset], data, len);
	if (!(offset < CMD_OFF_CMD + 2 && CMD_OFF_CMD < offset + len)) {
		return true;
	}

	cmd = sys_get_le16(&cmd_area[CMD_OFF_CMD]);
	sys_put_le16(IFCC_WAIT, &cmd_area[CMD_OFF_CMD]);

	switch (cmd) {
	case IFCC_WAIT:
		return true;
	case IFCC_SAVE:
		actuator_params_save();
		/* EV_PARAM_SALV: no fwBLE vem do salvar pela IHM (ifIHM.c). */
		actuator_regevent(EV_PARAM_SALV);
		sys_put_le16(IFCCS_READY, &cmd_area[CMD_OFF_STATUS]);
		return true;
	case IFCC_START_READ_EVENT:
		ok = actuator_regevent_read_start(sys_get_le32(&cmd_area[CMD_OFF_INDEX]),
						  &cmd_area[CMD_OFF_BUFFER]);
		sys_put_le16(ok ? IFCCS_READY : IFCCS_FAIL, &cmd_area[CMD_OFF_STATUS]);
		return true;
	case IFCC_SEQ_READ_EVENT:
		ok = actuator_regevent_read_next(&cmd_area[CMD_OFF_BUFFER]);
		sys_put_le16(ok ? IFCCS_READY : IFCCS_FAIL, &cmd_area[CMD_OFF_STATUS]);
		return true;
	case IFCC_RESTORE:
		actuator_params_restore();
		sys_put_le16(IFCCS_READY, &cmd_area[CMD_OFF_STATUS]);
		return true;
	default:
		LOG_WRN("Comando IFCC 0x%04x nao suportado pela BLE", cmd);
		sys_put_le16(IFCCS_FAIL, &cmd_area[CMD_OFF_STATUS]);
		return false;
	}
}

/* GTM_SEND (escrita):
 *   - Painel Remoto (0x00805900): palavra de 16 bits pro actuator_panel.c,
 *     consumida no loop de controle.
 *   - Painel / paramDado (0x00800800): offset arbitrario, validado em
 *     actuator_params_write() - fica so' em RAM ate' IFCC_SAVE.
 *   - Comando (0x00800000): ver handle_cmd_write().
 *   - FSA, tempo de reversao (0x00801010, u16): validado e gravado na hora,
 *     ver actuator_fsa_cfg.h.
 * Resposta GTM_CONFIRM em caso de sucesso, GTM_NEG pra endereco/tamanho
 * nao suportado ou valor fora da faixa.
 */
static size_t handle_send(const struct msg_hdr *req, const uint8_t *req_buf, size_t req_len,
			   uint8_t *resp_buf)
{
	struct msg_hdr *resp = (struct msg_hdr *)resp_buf;
	const uint8_t *payload = req_buf + sizeof(*req);
	uint32_t addr = req->address;
	uint8_t len = req->len_data;
	bool ok = false;

	if (req_len < sizeof(*req) + len) {
		LOG_WRN("GTM_SEND com payload menor (%u) que len_data (%u)",
			(unsigned)(req_len - sizeof(*req)), len);
	} else if (addr == ACTUATOR_PANEL_AREA_ADDR && len >= ACTUATOR_PANEL_AREA_LEN) {
		actuator_panel_write_word(sys_get_le16(payload));
		ok = true;
	} else if (addr >= ACTUATOR_PARAMS_AREA_ADDR &&
		   addr < ACTUATOR_PARAMS_AREA_ADDR + ACTUATOR_PARAMS_SIZE) {
		ok = actuator_params_write((uint16_t)(addr - ACTUATOR_PARAMS_AREA_ADDR), payload,
					   len);
	} else if (addr >= CMD_AREA_ADDR && addr < CMD_AREA_ADDR + CMD_AREA_LEN &&
		   len <= CMD_AREA_ADDR + CMD_AREA_LEN - addr) {
		ok = handle_cmd_write((uint16_t)(addr - CMD_AREA_ADDR), payload, len);
	} else if (addr >= ACTUATOR_FSA_TEMPO_REVER_ADDR &&
		   addr < ACTUATOR_FSA_TEMPO_REVER_ADDR + ACTUATOR_FSA_TEMPO_REVER_LEN) {
		ok = actuator_fsa_cfg_write((uint16_t)(addr - ACTUATOR_FSA_TEMPO_REVER_ADDR),
					    payload, len);
	} else {
		LOG_WRN("GTM_SEND endereco 0x%08x / len %u nao suportado", addr, len);
	}

	set_response_header(resp, req, ok ? ACG_MSG_CONFIRM : ACG_MSG_NEG, 0);
	return sizeof(*resp);
}

size_t actuator_service_handle_msg(const uint8_t *req_buf, size_t req_len, uint8_t *resp_buf,
				    size_t resp_buf_len)
{
	const struct msg_hdr *req;
	struct msg_hdr *resp = (struct msg_hdr *)resp_buf;

	if (resp_buf_len < sizeof(struct msg_hdr)) {
		/* Chamador passou um buffer menor que o proprio cabecalho -
		 * nao ha nada seguro a fazer alem de nao escrever.
		 */
		return 0;
	}

	if (req_len < sizeof(struct msg_hdr)) {
		/* Frame curto demais pra ter um cabecalho valido - nao da pra
		 * ecoar idMsgHost/address de verdade, mas ainda respondemos
		 * (GTM_REFUSE, mesmo espirito do acgl_refuse() original).
		 */
		memset(resp, 0, sizeof(*resp));
		resp->type = (uint8_t)ACG_MSG_REFUSE;
		return sizeof(*resp);
	}

	req = (const struct msg_hdr *)req_buf;

	switch ((enum acg_msg_type)req->type) {
	case ACG_MSG_REQUEST:
		return handle_request(req, resp_buf, resp_buf_len);
	case ACG_MSG_SEND:
		return handle_send(req, req_buf, req_len, resp_buf);
	default:
		LOG_DBG("Tipo de mensagem 0x%02x ainda nao implementado", req->type);
		set_response_header(resp, req, ACG_MSG_NEG, 0);
		return sizeof(*resp);
	}
}
