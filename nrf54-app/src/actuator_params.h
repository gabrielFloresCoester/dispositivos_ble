/*
 * Parametros do papel de atuador - port do paramDado (tParamDado) de
 * BLE/ParamZarI.{c,h} (ControleCoesterBLE), exposto na area "Painel"
 * REAL do ifFerConfig (MSG_EX_ADDRESS_CONFIG_PANEL = 0x00800800).
 *
 * FIDELIDADE BYTE A BYTE: a imagem em RAM e' exatamente o tParamDado do
 * fwBLE compilado para o produto (sem POS_ABS / CTL_NWP), 144 bytes,
 * #pragma pack(2) do IAR. Os offsets abaixo foram calculados a partir do
 * struct e conferidos contra as ancoras OFFSET_BITS_* que o proprio
 * ParamZarI.h define (34, 50, 72, 78, 92, 98, 99, 105). Um Gateway ou
 * interface que leia/escreva paramDado num Atuador BLE legado faz o
 * mesmo aqui, no mesmo endereco e offset.
 *
 * NAO usamos bitfields C pra imagem: GCC e IAR alocam bitfields de forma
 * diferente (o IAR fecha o container quando o tipo-base muda, o GCC nao).
 * Cada campo e' um offset + (pra bits) mascara/deslocamento, e o acesso
 * e' por byte - o layout fica garantido por construcao.
 *
 * Revisa a decisao de 2026-08-21 (actuator_calib.c, removido): la' o
 * caminho escolhido foi uma characteristic propria com 5 campos. Agora o
 * mapa inteiro vem tal e qual o fwBLE (pedido do Felipe, 2026-10-01).
 *
 * DIVERGENCIAS DELIBERADAS do fwBLE (ver docs/PARAMETROS.md):
 *   - Escrita valida cada campo tocado contra paramMin/paramMax (o fwBLE
 *     faz memcpy cru pela BLE). Fora da faixa -> a escrita inteira e'
 *     recusada, nada muda.
 *   - IFCC_SAVE/IFCC_RESTORE aceitos pela BLE (no fwBLE estao comentados
 *     em accessCmdBLE - la' so' a IHM/fieldbus grava).
 */
#ifndef ACTUATOR_PARAMS_H_
#define ACTUATOR_PARAMS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MSG_EX_ADDRESS_CONFIG_PANEL no ifFerConfig.h original */
#define ACTUATOR_PARAMS_AREA_ADDR 0x00800800UL
/* sizeof(tParamDado) sem POS_ABS */
#define ACTUATOR_PARAMS_SIZE 144U

/* --- Offsets no tParamDado (ParamZarI.h) ---
 * Bloco de fabrica (struct sParamFab, SIZE_PARAM_FAB = 34)
 */
#define PRM_OFF_MODEL             0   /* uint8_t[16] */
#define PRM_OFF_NS                16  /* uint8_t[8] */
#define PRM_OFF_FAB_FAT_TORQ_ABER 24  /* u16 */
#define PRM_OFF_FAB_FAT_TORQ_FECH 26  /* u16 */
#define PRM_OFF_FAB_TORQ_MAX      28  /* u16 */
#define PRM_OFF_FAB_TORQ_MIN      30  /* u16 */
#define PRM_OFF_FAB_GAN_SEN_TORQ  32  /* u8 */
#define PRM_OFF_FAB_LEIT_POS_INV  33  /* u8 */
#define PRM_SIZE_FAB              34

/* OFFSET_BITS_CONFIG_GERAL: fabPront, Ok, antiHorario, inibCmdLoc,
 * deslTrqIncr, trqFechad, alAtQtLoc (bits 0..6)
 */
#define PRM_OFF_BITS_GERAL        34
#define PRM_OFF_IDIOMA            35  /* u8 */
#define PRM_OFF_LIMITE_SUPER      36  /* u16 */
#define PRM_OFF_LIMITE_INFER      38  /* u16 */
#define PRM_OFF_TORQUE_NM_INC     40  /* u16 */
#define PRM_OFF_TORQUE_NM_DEC     42  /* u16 */
#define PRM_OFF_TORQUE_ZERO       44  /* i16 */
#define PRM_OFF_SOBR_TORQ_PART    46  /* u8 */
#define PRM_OFF_LIMITE_MARGEM     47  /* u8 */
#define PRM_OFF_FAIXA_PARADO      48  /* u8 */
#define PRM_OFF_ANTECIPAR_PARADA  49  /* u8 */
/* OFFSET_BITS_CONFIG_LEDS: ledVermAbre, ledLjAlarme */
#define PRM_OFF_BITS_LEDS         50
#define PRM_OFF_SENHA             51  /* uint8_t[4] */
#define PRM_OFF_TAG               55  /* uint8_t[16] */
/* 71: padding (pack(2) alinha o struct sDispoHabi de uint16_t) */
/* OFFSET_BITS_CONFIG_IF: sDispoHabi iOA, iOD, rede (u16) */
#define PRM_OFF_BITS_IF           72
#define PRM_OFF_ESD_AC            74  /* u8 */
/* 75: padding */
#define PRM_OFF_ESD_POSIC         76  /* u16 */
/* OFFSET_BITS_CONFIG_FUNC: ESDDSAq, ESDDFFa, ESDDTrq, PSTI,
 * numPartMaxLig, ampTempLig
 */
#define PRM_OFF_BITS_FUNC         78
#define PRM_OFF_PSTM              79  /* u8 */
#define PRM_OFF_PSTP              80  /* u8 */
#define PRM_OFF_SIN_SAID6_IOD     81  /* u8 */
#define PRM_OFF_PSTT              82  /* u16 */
#define PRM_OFF_TEMPO_OPERAR      84  /* i16 */
#define PRM_OFF_NUM_PART_MAX      86  /* i16 */
#define PRM_OFF_AMP_TEMP_AT1      88  /* u16: Par:9 | Mov:7 */
#define PRM_OFF_AMP_TEMP_AT3      90  /* u16: Par:9 | Mov:7 */
/* OFFSET_BITS_CONFIG_ENTR_DIG: abriFechaIODig:3, retentIODig:1 */
#define PRM_OFF_BITS_ENTR_DIG     92
#define PRM_OFF_SIN_SAID1_IOD     93  /* u8 */
#define PRM_OFF_SIN_SAID2_IOD     94
#define PRM_OFF_SIN_SAID3_IOD     95
#define PRM_OFF_SIN_SAID4_IOD     96
#define PRM_OFF_SIN_SAID5_IOD     97
/* OFFSET_BITS_CONFIG_SIN_ENT_DIG: bSinEntrBaiIOD (8 bits) */
#define PRM_OFF_SIN_ENTR_BAI_IOD  98
/* OFFSET_BITS_CONFIG_LAC_ABER: posicLacoAbertIOA:7, pararLacoAbertIOA:1 */
#define PRM_OFF_BITS_LAC_ABER     99
#define PRM_OFF_POSIC_REDE_FALHA  100 /* u8 */
#define PRM_OFF_TEMPO_ESPERA      101 /* u8, unidade 100 ms */
#define PRM_OFF_ACAO_REDE_FALHA   102 /* u8 */
#define PRM_OFF_ENDER_REDE        103 /* u8 */
#define PRM_OFF_BAUD_RATE_REDE    104 /* u8, indice (0=300 ... 11=115200) */
/* OFFSET_BITS_CONFIG_REDE: bitFormatRede:3, mapMem:5 */
#define PRM_OFF_BITS_REDE         105
/* comanPrimar:4, modoContr:4 */
#define PRM_OFF_BITS_CONTR        106
#define PRM_OFF_AMP_TEMP_POS_X    107 /* u8 */
#define PRM_OFF_AMP_TEMP_POS_Y    108 /* u8 */
/* 109: padding */
#define PRM_OFF_AMP_TEMP_AT2      110 /* u16: Par:9 | Mov:7 */
#define PRM_OFF_AMP_TEMP_AT4      112 /* u16: Par:9 | Mov:7 */
#define PRM_OFF_SPARE_NOVA        114 /* uint16_t[15] */

/* Bits de PRM_OFF_BITS_GERAL */
#define PRM_BIT_FAB_PRONT    0
#define PRM_BIT_OK           1
#define PRM_BIT_ANTI_HORARIO 2
#define PRM_BIT_INIB_CMD_LOC 3
#define PRM_BIT_DESL_TRQ_INC 4
#define PRM_BIT_TRQ_FECHAD   5
#define PRM_BIT_AL_AT_QT_LOC 6

/* Carrega os defaults de fabrica (paramInic) e registra o handler de
 * persistencia (ZMS) - chamar ANTES de settings_load() em main.c.
 */
void actuator_params_init(void);

/* Chamar logo DEPOIS de settings_load(): migra a calibracao gravada
 * pelo antigo actuator_calib ("acal/v1") se ainda nao houver parametros
 * gravados no formato novo.
 */
void actuator_params_post_load(void);

/* Copia len bytes a partir de offset (0..ACTUATOR_PARAMS_SIZE-1) da
 * imagem atual. Retorna false se a faixa estiver fora da area.
 */
bool actuator_params_read(uint16_t offset, uint8_t *buf, size_t len);

/* Escreve len bytes em offset na imagem em RAM, validando todo campo
 * tocado (paramMin/paramMax do ParamZarI.c; torqueNmInc/Dec contra
 * fabTorqMin/fabTorqMax, como paramModTrqAbrNm/FecNm). Se algum campo
 * ficar fora da faixa, nada muda e retorna false. NAO persiste - ver
 * actuator_params_save().
 */
bool actuator_params_write(uint16_t offset, const uint8_t *data, size_t len);

/* IFCC_SAVE: marca paramDado.Ok e grava a imagem atual (assincrono, na
 * workqueue do sistema). Equivalente a paramSalva().
 */
void actuator_params_save(void);

/* IFCC_RESTORE: descarta alteracoes nao salvas, recarregando a imagem
 * gravada (ou os defaults, se nunca houve gravacao). Equivalente a
 * paramRestau().
 */
void actuator_params_restore(void);

/* --- Getters tipados pros consumidores (leitura atomica do campo) --- */
uint16_t actuator_params_limite_super(void);
uint16_t actuator_params_limite_infer(void);
uint16_t actuator_params_torque_nm_inc(void);
uint16_t actuator_params_torque_nm_dec(void);
int16_t actuator_params_torque_zero(void);
uint16_t actuator_params_fab_fat_torq_aber(void);
uint16_t actuator_params_fab_fat_torq_fech(void);
uint8_t actuator_params_limite_margem(void);
uint8_t actuator_params_faixa_parado(void);
uint8_t actuator_params_antecipar_parada(void);
bool actuator_params_fab_leit_pos_inv(void);
bool actuator_params_anti_horario(void);
bool actuator_params_inib_cmd_loc(void);
bool actuator_params_al_at_qt_loc(void);
bool actuator_params_desl_trq_incr(void); /* "Desliga Torque de Abertura" */
bool actuator_params_trq_fechad(void);    /* "Fechamento com Torque" */
uint8_t actuator_params_sobr_torq_part(void); /* "Sobre Torque na Partida" (%) */

#endif /* ACTUATOR_PARAMS_H_ */
