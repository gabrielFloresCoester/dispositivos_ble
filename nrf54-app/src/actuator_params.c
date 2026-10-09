/*
 * Parametros do papel de atuador (paramDado) - ver actuator_params.h pro
 * layout e as divergencias deliberadas do fwBLE.
 */

#include "actuator_params.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(actuator_params, LOG_LEVEL_INF);

/* RESOL_AD_POS em sensPosTor.h - usado por paramCorrigeLimites() */
#define RESOL_AD_POS 0x07FF

/* Chave curta de proposito (<= 8 chars cabe no caminho rapido do ZMS) -
 * mesmo raciocinio do "coe/al" da allow-list e do antigo "acal/v1".
 */
#define PARAMS_KEY "aprm/v1"

/* Chave do antigo actuator_calib (removido) - so' lida pra migrar */
#define LEGACY_CALIB_KEY "acal/v1"

/* paramInic (ParamZarI.c), sem POS_ABS - gerado campo a campo a partir
 * do struct original. Bytes de padding do pack(2) ficam 0.
 */
static const uint8_t param_inic[ACTUATOR_PARAMS_SIZE] = {
	/* 0: model[16] */
	' ', 'F', 'A', 'L', 'H', 'A', ' ', 'T', 'I', 'P', 'O', ' ', ' ', ' ', ' ', 0,
	/* 16: ns[8] */
	'0', '0', '0', '0', '0', ' ', ' ', 0,
	/* 24: fabFatTorqAber=1000, 26: fabFatTorqFech=1000 */
	0xE8, 0x03, 0xE8, 0x03,
	/* 28: fabTorqMax=1400, 30: fabTorqMin=1 */
	0x78, 0x05, 0x01, 0x00,
	/* 32: fabGanSenTorq=0, 33: fabLeitPosiInver=0 */
	0, 0,
	/* 34: bits geral - so' alAtQtLoc=1 (bit 6) */
	BIT(PRM_BIT_AL_AT_QT_LOC),
	/* 35: idioma=0 */
	0,
	/* 36: limiteSuper=1800, 38: limiteInfer=200 */
	0x08, 0x07, 0xC8, 0x00,
	/* 40: torqueNmInc=80, 42: torqueNmDec=80, 44: torqueZero=0 */
	80, 0, 80, 0, 0, 0,
	/* 46: sobrTorqPart=40, limiteMargem=20, faixaParado=20, anteciparParada=2 */
	40, 20, 20, 2,
	/* 50: bits leds = 0 */
	0,
	/* 51: senha[4] */
	0, 0, 0, 0,
	/* 55: tag[16] */
	' ', 'T', 'A', 'G', ' ', 'A', 'T', 'U', 'A', 'D', 'O', 'R', ' ', '?', 0, 0,
	/* 71: padding */
	0,
	/* 72: sDispoHabi (u16) = 0 */
	0, 0,
	/* 74: ESDAc=2, 75: padding */
	2, 0,
	/* 76: ESDPosic=0 */
	0, 0,
	/* 78: bits func - ESDDSAq=1, ESDDFFa=1, ESDDTrq=1 */
	0x07,
	/* 79: PSTM=0, PSTP=10, SinSaid6IODig=9 */
	0, 10, 9,
	/* 82: PSTT=30, 84: tempoOperar=0, 86: numPartMax=300 */
	30, 0, 0, 0, 0x2C, 0x01,
	/* 88: AT1 Par=10 | Mov=4<<9, 90: AT3 idem */
	0x0A, 0x08, 0x0A, 0x08,
	/* 92: abriFechaIODig=0, retentIODig=1 (bit 3) */
	0x08,
	/* 93: SinSaid1..5 IODig = 1, 2, 3, 4, 18 */
	1, 2, 3, 4, 18,
	/* 98: bSinEntrBaiIOD=1 */
	1,
	/* 99: posicLacoAbertIOA=0, pararLacoAbertIOA=1 (bit 7) */
	0x80,
	/* 100: posicRedeFalhaCom=0, tempoEspera=100, acaoRedeFalhaCom=0,
	 * enderRede=247, baudRateRede=5
	 */
	0, 100, 0, 247, 5,
	/* 105: bitFormatRede=0 | mapMem=0, 106: comanPrimar=0 | modoContr=0 */
	0, 0,
	/* 107: ampTempPosX=0, 108: ampTempPosY=0, 109: padding */
	0, 0, 0,
	/* 110: AT2 Par=10 | Mov=4<<9, 112: AT4 idem */
	0x0A, 0x08, 0x0A, 0x08,
	/* 114: spare_nova[15] = 0 (resto do array) */
};

BUILD_ASSERT(PRM_OFF_SPARE_NOVA + 15 * 2 == ACTUATOR_PARAMS_SIZE,
	     "layout do tParamDado deve fechar em 144 bytes");

/* --- Tabela de validacao: paramMin/paramMax (ParamZarI.c) ---
 *
 * So' campos com faixa real entram. Ficam de fora, sem checagem (como
 * no fwBLE, onde nao ha paramMod* com clamp pra eles):
 *   - campos de 1 bit (nao tem como sair da faixa 0..1)
 *   - strings (model, ns, tag), bSinEntrBaiIOD (0..255), spare_nova
 *   - torqueZero (calibrado em campo, sem faixa)
 * torqueNmInc/torqueNmDec sao tratados a parte (faixa dinamica).
 */
enum prm_kind {
	PRM_U8,
	PRM_U16,
	PRM_I16,
	PRM_BITS8,  /* campo de 'width' bits a partir de 'shift' num byte */
	PRM_BITS16, /* idem num u16 little-endian */
};

struct prm_field {
	uint8_t off;
	uint8_t kind;
	uint8_t shift;
	uint8_t width;
	int16_t min;
	int16_t max;
	const char *name;
};

#define F_U8(o, mn, mx, n)  {o, PRM_U8, 0, 8, mn, mx, n}
#define F_U16(o, mn, mx, n) {o, PRM_U16, 0, 16, mn, mx, n}
#define F_I16(o, mn, mx, n) {o, PRM_I16, 0, 16, mn, mx, n}
#define F_B8(o, s, w, mn, mx, n)  {o, PRM_BITS8, s, w, mn, mx, n}
#define F_B16(o, s, w, mn, mx, n) {o, PRM_BITS16, s, w, mn, mx, n}

static const struct prm_field fields[] = {
	F_U16(PRM_OFF_FAB_FAT_TORQ_ABER, 1, 30000, "fabFatTorqAber"),
	F_U16(PRM_OFF_FAB_FAT_TORQ_FECH, 1, 30000, "fabFatTorqFech"),
	F_U16(PRM_OFF_FAB_TORQ_MAX, 1, 1400, "fabTorqMax"),
	F_U16(PRM_OFF_FAB_TORQ_MIN, 1, 1400, "fabTorqMin"),
	F_U8(PRM_OFF_FAB_GAN_SEN_TORQ, 0, 3, "fabGanSenTorq"),
	F_U8(PRM_OFF_FAB_LEIT_POS_INV, 0, 1, "fabLeitPosiInver"),
	F_U8(PRM_OFF_IDIOMA, 0, 2, "idioma"),
	F_U16(PRM_OFF_LIMITE_SUPER, 50, 2001, "limiteSuper"),
	F_U16(PRM_OFF_LIMITE_INFER, 50, 2001, "limiteInfer"),
	F_U8(PRM_OFF_SOBR_TORQ_PART, 5, 200, "sobrTorqPart"),
	F_U8(PRM_OFF_LIMITE_MARGEM, 2, 100, "limiteMargem"),
	F_U8(PRM_OFF_FAIXA_PARADO, 1, 100, "faixaParado"),
	F_U8(PRM_OFF_ANTECIPAR_PARADA, 0, 100, "anteciparParada"),
	F_U8(PRM_OFF_SENHA + 0, 0, 9, "senha[0]"),
	F_U8(PRM_OFF_SENHA + 1, 0, 9, "senha[1]"),
	F_U8(PRM_OFF_SENHA + 2, 0, 9, "senha[2]"),
	F_U8(PRM_OFF_SENHA + 3, 0, 9, "senha[3]"),
	F_U8(PRM_OFF_ESD_AC, 0, 4, "ESDAc"),
	F_U16(PRM_OFF_ESD_POSIC, 0, 100, "ESDPosic"),
	F_U8(PRM_OFF_PSTM, 0, 2, "PSTM"),
	F_U8(PRM_OFF_PSTP, 3, 90, "PSTP"),
	F_U8(PRM_OFF_SIN_SAID6_IOD, 0, 43, "SinSaid6IODig"),
	F_U16(PRM_OFF_PSTT, 3, 3000, "PSTT"),
	F_I16(PRM_OFF_TEMPO_OPERAR, 0, 3600, "tempoOperar"),
	F_I16(PRM_OFF_NUM_PART_MAX, 1, 1200, "numPartMax"),
	F_B16(PRM_OFF_AMP_TEMP_AT1, 0, 9, 0, 500, "ampTempAT1Par"),
	F_B16(PRM_OFF_AMP_TEMP_AT1, 9, 7, 1, 120, "ampTempAT1Mov"),
	F_B16(PRM_OFF_AMP_TEMP_AT3, 0, 9, 0, 500, "ampTempAT3Par"),
	F_B16(PRM_OFF_AMP_TEMP_AT3, 9, 7, 1, 120, "ampTempAT3Mov"),
	F_B8(PRM_OFF_BITS_ENTR_DIG, 0, 3, 0, 3, "abriFechaIODig"),
	F_U8(PRM_OFF_SIN_SAID1_IOD, 0, 44, "SinSaid1IODig"),
	F_U8(PRM_OFF_SIN_SAID2_IOD, 0, 44, "SinSaid2IODig"),
	F_U8(PRM_OFF_SIN_SAID3_IOD, 0, 44, "SinSaid3IODig"),
	F_U8(PRM_OFF_SIN_SAID4_IOD, 0, 44, "SinSaid4IODig"),
	F_U8(PRM_OFF_SIN_SAID5_IOD, 0, 44, "SinSaid5IODig"),
	F_B8(PRM_OFF_BITS_LAC_ABER, 0, 7, 0, 100, "posicLacoAbertIOA"),
	F_U8(PRM_OFF_POSIC_REDE_FALHA, 0, 100, "posicRedeFalhaCom"),
	F_U8(PRM_OFF_TEMPO_ESPERA, 1, 250, "tempoEspera"),
	F_U8(PRM_OFF_ACAO_REDE_FALHA, 0, 4, "acaoRedeFalhaCom"),
	F_U8(PRM_OFF_ENDER_REDE, 1, 247, "enderRede"),
	F_U8(PRM_OFF_BAUD_RATE_REDE, 0, 11, "baudRateRede"),
	F_B8(PRM_OFF_BITS_REDE, 0, 3, 0, 4, "bitFormatRede"),
	F_B8(PRM_OFF_BITS_REDE, 3, 5, 0, 1, "mapMem"),
	F_B8(PRM_OFF_BITS_CONTR, 0, 4, 0, 2, "comanPrimar"),
	F_B8(PRM_OFF_BITS_CONTR, 4, 4, 0, 1, "modoContr"),
	F_U8(PRM_OFF_AMP_TEMP_POS_X, 0, 95, "ampTempPosX"),
	F_U8(PRM_OFF_AMP_TEMP_POS_Y, 0, 95, "ampTempPosY"),
	F_B16(PRM_OFF_AMP_TEMP_AT2, 0, 9, 0, 500, "ampTempAT2Par"),
	F_B16(PRM_OFF_AMP_TEMP_AT2, 9, 7, 1, 120, "ampTempAT2Mov"),
	F_B16(PRM_OFF_AMP_TEMP_AT4, 0, 9, 0, 500, "ampTempAT4Par"),
	F_B16(PRM_OFF_AMP_TEMP_AT4, 9, 7, 1, 120, "ampTempAT4Mov"),
};

/* Imagem viva do paramDado. Escrita pela thread do BT (acgl) e pela
 * workqueue (restore/load), lida pelo loop de controle e pelo sensor -
 * todo acesso passa pelo spinlock (copias de no maximo 144 bytes).
 */
static uint8_t image[ACTUATOR_PARAMS_SIZE] __aligned(2);
static struct k_spinlock lock;

static struct k_work save_work;
static struct k_work restore_work;

/* Flags do carregamento via settings_load() */
static bool params_loaded;
static bool legacy_calib_loaded;
static uint8_t legacy_calib[10];

static int32_t field_value(const uint8_t *img, const struct prm_field *f)
{
	switch (f->kind) {
	case PRM_U8:
		return img[f->off];
	case PRM_U16:
		return sys_get_le16(&img[f->off]);
	case PRM_I16:
		return (int16_t)sys_get_le16(&img[f->off]);
	case PRM_BITS8:
		return (img[f->off] >> f->shift) & BIT_MASK(f->width);
	case PRM_BITS16:
		return (sys_get_le16(&img[f->off]) >> f->shift) & BIT_MASK(f->width);
	default:
		return 0;
	}
}

static uint8_t field_size(const struct prm_field *f)
{
	return (f->kind == PRM_U8 || f->kind == PRM_BITS8) ? 1 : 2;
}

static bool overlaps(uint16_t a_off, size_t a_len, uint16_t b_off, size_t b_len)
{
	return a_off < b_off + b_len && b_off < a_off + a_len;
}

/* paramCorrigeLimites(): ao trocar o sentido (antiHorario), espelha os
 * limites pra evitar recalibrar. No fwBLE isso so' acontece pela IHM
 * (paramModSentAntHor); aqui vale pra qualquer escrita que mude o bit,
 * que e' o equivalente funcional.
 */
static void corrige_limites(uint8_t *img)
{
	uint16_t super = sys_get_le16(&img[PRM_OFF_LIMITE_SUPER]);
	uint16_t infer = sys_get_le16(&img[PRM_OFF_LIMITE_INFER]);

	sys_put_le16(RESOL_AD_POS - infer, &img[PRM_OFF_LIMITE_SUPER]);
	sys_put_le16(RESOL_AD_POS - super, &img[PRM_OFF_LIMITE_INFER]);
}

/* Valida todo campo da tabela que a escrita [off, off+len) toca. Se
 * force_all, valida a tabela inteira (usado em imagem carregada).
 */
static bool validate(const uint8_t *img, uint16_t off, size_t len, bool force_all)
{
	uint16_t trq_min = sys_get_le16(&img[PRM_OFF_FAB_TORQ_MIN]);
	uint16_t trq_max = sys_get_le16(&img[PRM_OFF_FAB_TORQ_MAX]);

	for (size_t i = 0; i < ARRAY_SIZE(fields); i++) {
		const struct prm_field *f = &fields[i];
		int32_t v;

		if (!force_all && !overlaps(off, len, f->off, field_size(f))) {
			continue;
		}
		v = field_value(img, f);
		if (v < f->min || v > f->max) {
			LOG_WRN("Parametro %s = %d fora da faixa [%d, %d]", f->name, v, f->min,
				f->max);
			return false;
		}
	}

	/* torqueNmInc/Dec: faixa [fabTorqMin, fabTorqMax], como
	 * paramModTrqAbrNm()/paramModTrqFecNm(). Revalida tambem quando o
	 * que mudou foram os proprios limites de fabrica.
	 */
	if (force_all || overlaps(off, len, PRM_OFF_TORQUE_NM_INC, 4) ||
	    overlaps(off, len, PRM_OFF_FAB_TORQ_MAX, 4)) {
		uint16_t inc = sys_get_le16(&img[PRM_OFF_TORQUE_NM_INC]);
		uint16_t dec = sys_get_le16(&img[PRM_OFF_TORQUE_NM_DEC]);

		if (inc < trq_min || inc > trq_max || dec < trq_min || dec > trq_max) {
			LOG_WRN("torqueNmInc=%u/torqueNmDec=%u fora de [fabTorqMin=%u, "
				"fabTorqMax=%u]", inc, dec, trq_min, trq_max);
			return false;
		}
	}

	return true;
}

static void save_work_handler(struct k_work *work)
{
	uint8_t copy[ACTUATOR_PARAMS_SIZE];
	k_spinlock_key_t key = k_spin_lock(&lock);
	int err;

	memcpy(copy, image, sizeof(copy));
	k_spin_unlock(&lock, key);

	err = settings_save_one(PARAMS_KEY, copy, sizeof(copy));
	if (err) {
		LOG_ERR("Falha ao gravar parametros (err %d)", err);
	} else {
		LOG_INF("Parametros gravados (limiteSuper=%u limiteInfer=%u torqueNmInc=%u "
			"torqueNmDec=%u torqueZero=%d)", sys_get_le16(&copy[PRM_OFF_LIMITE_SUPER]),
			sys_get_le16(&copy[PRM_OFF_LIMITE_INFER]),
			sys_get_le16(&copy[PRM_OFF_TORQUE_NM_INC]),
			sys_get_le16(&copy[PRM_OFF_TORQUE_NM_DEC]),
			(int16_t)sys_get_le16(&copy[PRM_OFF_TORQUE_ZERO]));
	}
}

static void load_defaults(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	memcpy(image, param_inic, sizeof(image));
	k_spin_unlock(&lock, key);
}

static void restore_work_handler(struct k_work *work)
{
	int err;

	/* paramRestau(): volta pra imagem gravada. Sem gravacao, ficam os
	 * defaults (paramPadraoCompleto()). Os defaults so' entram DEPOIS de
	 * confirmar que nao ha nada gravado - o handler troca a imagem
	 * inteira de uma vez, entao o loop de controle nunca ve um estado
	 * intermediario (limites pulando pros defaults e voltando).
	 */
	params_loaded = false;
	err = settings_load_subtree("aprm");
	if (err) {
		LOG_ERR("Falha ao recarregar parametros (err %d)", err);
	}
	if (!params_loaded) {
		load_defaults();
	}
	LOG_INF("Parametros restaurados (%s)", params_loaded ? "gravados" : "defaults");
}

static int params_settings_set(const char *name, size_t len, settings_read_cb read_cb,
			       void *cb_arg)
{
	uint8_t loaded[ACTUATOR_PARAMS_SIZE];
	ssize_t got;

	if (!settings_name_steq(name, "v1", NULL)) {
		return -ENOENT;
	}
	if (len != sizeof(loaded)) {
		LOG_WRN("Parametros gravados com tamanho inesperado (%u, esperava %u) - "
			"mantendo defaults", (unsigned)len, (unsigned)sizeof(loaded));
		return 0;
	}

	got = read_cb(cb_arg, loaded, len);
	if (got < 0) {
		return (int)got;
	}

	if (!validate(loaded, 0, sizeof(loaded), true)) {
		LOG_WRN("Parametros gravados com campo fora da faixa - mantendo defaults");
		return 0;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	memcpy(image, loaded, sizeof(image));
	k_spin_unlock(&lock, key);
	params_loaded = true;
	LOG_INF("Parametros restaurados da memoria nao volatil");
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(actprm, "aprm", NULL, params_settings_set, NULL, NULL);

/* So' pra migrar a calibracao do antigo actuator_calib (5 campos,
 * struct de 10 bytes: limite_super, limite_infer, torque_nm_inc,
 * torque_nm_dec, torque_zero).
 */
static int legacy_calib_settings_set(const char *name, size_t len, settings_read_cb read_cb,
				     void *cb_arg)
{
	ssize_t got;

	if (!settings_name_steq(name, "v1", NULL)) {
		return -ENOENT;
	}
	if (len != sizeof(legacy_calib)) {
		return 0;
	}

	got = read_cb(cb_arg, legacy_calib, len);
	if (got < 0) {
		return (int)got;
	}
	legacy_calib_loaded = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(actcal_legacy, "acal", NULL, legacy_calib_settings_set, NULL, NULL);

void actuator_params_init(void)
{
	load_defaults();
	k_work_init(&save_work, save_work_handler);
	k_work_init(&restore_work, restore_work_handler);
}

void actuator_params_post_load(void)
{
	if (!legacy_calib_loaded) {
		return;
	}

	if (!params_loaded) {
		/* Os 10 bytes do struct antigo sao exatamente os offsets 36..45
		 * do paramDado (limiteSuper..torqueZero, mesma ordem, LE).
		 */
		uint8_t candidate[ACTUATOR_PARAMS_SIZE];
		k_spinlock_key_t key = k_spin_lock(&lock);

		memcpy(candidate, image, sizeof(candidate));
		k_spin_unlock(&lock, key);
		memcpy(&candidate[PRM_OFF_LIMITE_SUPER], legacy_calib, sizeof(legacy_calib));

		if (validate(candidate, PRM_OFF_LIMITE_SUPER, sizeof(legacy_calib), false)) {
			key = k_spin_lock(&lock);
			memcpy(image, candidate, sizeof(image));
			k_spin_unlock(&lock, key);
			LOG_INF("Calibracao antiga (acal/v1) migrada pro paramDado");
			k_work_submit(&save_work);
		}
	}

	(void)settings_delete(LEGACY_CALIB_KEY);
	legacy_calib_loaded = false;
}

bool actuator_params_read(uint16_t offset, uint8_t *buf, size_t len)
{
	if (offset >= ACTUATOR_PARAMS_SIZE || len > ACTUATOR_PARAMS_SIZE - offset) {
		return false;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	memcpy(buf, &image[offset], len);
	k_spin_unlock(&lock, key);
	return true;
}

bool actuator_params_write(uint16_t offset, const uint8_t *data, size_t len)
{
	uint8_t candidate[ACTUATOR_PARAMS_SIZE];
	bool anti_antes, anti_depois;
	k_spinlock_key_t key;

	if (offset >= ACTUATOR_PARAMS_SIZE || len == 0 || len > ACTUATOR_PARAMS_SIZE - offset) {
		return false;
	}

	key = k_spin_lock(&lock);
	memcpy(candidate, image, sizeof(candidate));
	k_spin_unlock(&lock, key);

	anti_antes = candidate[PRM_OFF_BITS_GERAL] & BIT(PRM_BIT_ANTI_HORARIO);
	memcpy(&candidate[offset], data, len);
	anti_depois = candidate[PRM_OFF_BITS_GERAL] & BIT(PRM_BIT_ANTI_HORARIO);

	if (anti_antes != anti_depois) {
		corrige_limites(candidate);
		if (!validate(candidate, offset, len, false) ||
		    !validate(candidate, PRM_OFF_LIMITE_SUPER, 4, false)) {
			return false;
		}
	} else if (!validate(candidate, offset, len, false)) {
		return false;
	}

	/* Quem escreve e' a thread do BT; a imagem pode ter mudado entre a
	 * copia e aqui so' por outra escrita da mesma thread ou por um
	 * restore - nos dois casos, a ultima escrita vence, como no fwBLE.
	 */
	key = k_spin_lock(&lock);
	memcpy(image, candidate, sizeof(image));
	k_spin_unlock(&lock, key);

	LOG_INF("Parametros: %u byte(s) escritos no offset %u (RAM - IFCC_SAVE grava)",
		(unsigned)len, offset);
	return true;
}

void actuator_params_save(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	/* paramSalva(): paramDado.Ok = 1 */
	image[PRM_OFF_BITS_GERAL] |= BIT(PRM_BIT_OK);
	k_spin_unlock(&lock, key);

	k_work_submit(&save_work);
}

void actuator_params_restore(void)
{
	k_work_submit(&restore_work);
}

/* --- Getters --- */

static uint8_t get_u8(uint16_t off)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint8_t v = image[off];

	k_spin_unlock(&lock, key);
	return v;
}

static uint16_t get_u16(uint16_t off)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint16_t v = sys_get_le16(&image[off]);

	k_spin_unlock(&lock, key);
	return v;
}

static bool get_bit_geral(uint8_t bit)
{
	return (get_u8(PRM_OFF_BITS_GERAL) & BIT(bit)) != 0;
}

uint16_t actuator_params_limite_super(void)
{
	return get_u16(PRM_OFF_LIMITE_SUPER);
}

uint16_t actuator_params_limite_infer(void)
{
	return get_u16(PRM_OFF_LIMITE_INFER);
}

uint16_t actuator_params_torque_nm_inc(void)
{
	return get_u16(PRM_OFF_TORQUE_NM_INC);
}

uint16_t actuator_params_torque_nm_dec(void)
{
	return get_u16(PRM_OFF_TORQUE_NM_DEC);
}

int16_t actuator_params_torque_zero(void)
{
	return (int16_t)get_u16(PRM_OFF_TORQUE_ZERO);
}

uint16_t actuator_params_fab_fat_torq_aber(void)
{
	return get_u16(PRM_OFF_FAB_FAT_TORQ_ABER);
}

uint16_t actuator_params_fab_fat_torq_fech(void)
{
	return get_u16(PRM_OFF_FAB_FAT_TORQ_FECH);
}

uint8_t actuator_params_limite_margem(void)
{
	return get_u8(PRM_OFF_LIMITE_MARGEM);
}

uint8_t actuator_params_faixa_parado(void)
{
	return get_u8(PRM_OFF_FAIXA_PARADO);
}

uint8_t actuator_params_antecipar_parada(void)
{
	return get_u8(PRM_OFF_ANTECIPAR_PARADA);
}

bool actuator_params_fab_leit_pos_inv(void)
{
	return get_u8(PRM_OFF_FAB_LEIT_POS_INV) != 0;
}

bool actuator_params_anti_horario(void)
{
	return get_bit_geral(PRM_BIT_ANTI_HORARIO);
}

bool actuator_params_inib_cmd_loc(void)
{
	return get_bit_geral(PRM_BIT_INIB_CMD_LOC);
}

bool actuator_params_al_at_qt_loc(void)
{
	return get_bit_geral(PRM_BIT_AL_AT_QT_LOC);
}

bool actuator_params_desl_trq_incr(void)
{
	return get_bit_geral(PRM_BIT_DESL_TRQ_INC);
}

bool actuator_params_trq_fechad(void)
{
	return get_bit_geral(PRM_BIT_TRQ_FECHAD);
}

uint8_t actuator_params_sobr_torq_part(void)
{
	return get_u8(PRM_OFF_SOBR_TORQ_PART);
}
