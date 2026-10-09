/*
 * Registro de eventos - porta de BLE/Atuador/atRegEvent.{c,h} do fwBLE.
 * Detalhes, divergencias e roteiro de bancada em docs/REGISTRO_EVENTOS.md.
 *
 * O QUE E' IGUAL AO fwBLE:
 *  - Formato do registro (tEvent, 16 bytes) e os IDs de evento (enum
 *    EVENT do atRegEvent.h) - um cliente que decodifica eventos de um
 *    Atuador BLE decodifica os do SIM Connect sem mudanca.
 *  - Fila em RAM de 32 eventos (TAM_FILA_EVENT), descartando quando
 *    cheia; descarga quando a fila passa 2 s sem evento novo ou chega a
 *    metade, e sempre antes de uma leitura (liquidaFila).
 *  - Buffer circular indexado pela posicao (tEvent.index = posicao no
 *    arquivo, volta a 0 ao encher, registrando EV_SIS_REG_FIM_FILE).
 *  - Leitura pela area Comando: IFCC_START_READ_EVENT (indice, ou
 *    0xFFFFFFFF = mais novo) e IFCC_SEQ_READ_EVENT (o proximo mais
 *    antigo) - ver actuator_service.c.
 *
 * DIVERGENCIAS DELIBERADAS (motivo no doc):
 *  - Armazenamento: particao "event_log" (1 MB, 65536 eventos) na flash
 *    SPI externa (MX25R6435F no DK), nao o FILE_LOG do sel_mem
 *    (flash SPI 1 MB / cartao SD).
 *  - Posicao de escrita achada no boot por busca binaria, nao gravada a
 *    cada descarga (contAbsol.absolIndexRegEvent). Pra isso os bytes
 *    14..15 do tEvent (preenchimento de alinhamento no fwBLE, lixo) levam
 *    o contador de voltas do buffer.
 *  - Sem RTC: data/hora = TEMPO DESDE O BOOT, marcado com mes = 0 (nenhuma
 *    data valida tem mes 0): dia = dias (ano = byte alto dos dias), hora/
 *    minu/segun = hh:mm:ss. milSeg mantem a semantica do fwBLE.
 *  - Sem EV_SIS_POWER_DOWN (nao ha RTC com bateria pra marcar a queda).
 *  - Leitura atravessa o buffer inteiro (o fwBLE volta pro mais novo ao
 *    passar do indice 0 e nunca le a parte mais antiga depois da 1a
 *    volta). Fim da leitura: index = 0xFFFFFFFF + EV_SIS_REG_INV.
 */

#ifndef ACTUATOR_REGEVENT_H_
#define ACTUATOR_REGEVENT_H_

#include <stdbool.h>
#include <stdint.h>

/* Tamanho do registro (sizeof(tEvent) no fwBLE, IAR/ARM). */
#define ACTUATOR_REGEVENT_SIZE 16U

/* OVER_INDEX_REG: indice de inicio "o mais novo" / marca de fim. */
#define ACTUATOR_REGEVENT_OVER_INDEX 0xFFFFFFFFUL

/* enum EVENT do atRegEvent.h - MESMOS valores. Os sem ponto de registro
 * no SIM Connect (SD card, curvas, PST, barramento de campo, entradas
 * discretas, usuario de fabrica) ficam declarados pra manter a tabela de
 * validacao igual e reservar os IDs.
 */
enum actuator_event_id {
	EV_SIS_ = 0,
	EV_SIS_POWER_UP,
	EV_SIS_POWER_DOWN,
	EV_SIS_WDT_RST,
	EV_SIS_SW_RST,
	EV_SIS_MMC_EJ,
	EV_SIS_MMC_IN,
	EV_SIS_MMC_IN_FAT,
	EV_SIS_REG_FIM_FILE,
	EV_SIS_CURV_FIM_FILE,
	EV_SIS_REG_INV,
	EV_SIS_REG_INV_LIDO,
	EV_SIS_CURV_INV_LIDA,
	EV_SIS_CURV_COLET,
	/* Alarmes: EV_AL_ON/EV_AL_OFF + numero do alarme no alarm_t do fwBLE
	 * (nao o enum actuator_alarm_id - ver actuator_alarm.c).
	 */
	EV_AL_BQ = 200 - 1,
	EV_AL_ON = 200,
	EV_AL_OFF = 400,
	/* Acoes locais */
	EV_LOC_PST = 600,
	EV_LOC_PAR,
	EV_LOC_ABR,
	EV_LOC_FEC,
	EV_LOC_QT,
	EV_LOC_LOCAL,
	EV_LOC_DESLIG,
	EV_LOC_REMOTO,
	EV_LOC_INFO,
	EV_LOC_PARAM,
	EV_LOC_PARAM_FAB,
	/* Entradas barramento de campo */
	EV_REM_ESD = 700,
	EV_REM_PST,
	EV_REM_PAR,
	EV_REM_ABR,
	EV_REM_FEC,
	EV_REM_INIB_LOC,
	EV_REM_POSIC,
	EV_REM_QT,
	/* Entradas discretas (ativa _A / desativa _D) */
	EV_REM_DISC_PAR_A = 750,
	EV_REM_DISC_FEC_A,
	EV_REM_DISC_ABR_A,
	EV_REM_DISC_AUT_A,
	EV_REM_DISC_ESD_A,
	EV_REM_DISC_PST_A,
	EV_REM_DISC_7_A,
	EV_REM_DISC_8_A,
	EV_REM_DISC_PAR_D,
	EV_REM_DISC_FEC_D,
	EV_REM_DISC_ABR_D,
	EV_REM_DISC_AUT_D,
	EV_REM_DISC_ESD_D,
	EV_REM_DISC_PST_D,
	EV_REM_DISC_7_D,
	EV_REM_DISC_8_D,
	/* Estados de operacao: EV_OPE_NUL + enum actuator_mov_status (1..5) */
	EV_OPE_NUL = 800,
	EV_OPE_L_SUPER,
	EV_OPE_L_INFER,
	EV_OPE_INCR,
	EV_OPE_DECR,
	EV_OPE_M_PARA,
	/* Configuracoes e parametros */
	EV_PARAM_SAI = 900,
	EV_PARAM_SALV,
	/* Acoes pre-configuradas */
	EV_ACAO_CONFIG_FAL_COM_PAR = 1000,
	EV_ACAO_CONFIG_FAL_COM_ABRE,
	EV_ACAO_CONFIG_FAL_COM_FECHA,
	EV_ACAO_CONFIG_FAL_COM_POSIC,
	/* Acesso a parametros de fabrica por usuario, 1100..1199 */
	EV_FABR_USUAR = 1100,
};

/* Quantidade de alarmes do alarm_t do fwBLE (NUM_ALARMS) - faixa valida
 * de EV_AL_ON/EV_AL_OFF.
 */
#define ACTUATOR_REGEVENT_NUM_ALARMS_FWBLE 59

/* Causa do boot, registrada como primeiro evento (atRegPowerReset()). */
enum actuator_regevent_boot {
	ACTUATOR_REGEVENT_BOOT_POWER_UP, /* EV_SIS_POWER_UP */
	ACTUATOR_REGEVENT_BOOT_WDT,      /* EV_SIS_WDT_RST */
	ACTUATOR_REGEVENT_BOOT_SW,       /* EV_SIS_SW_RST */
};

/* Sobe a fila de gravacao e registra o evento de boot. Chamar uma vez,
 * antes de qualquer modulo que registre eventos ja' no 1o ciclo (o laco
 * de controle). A flash e' montada na 1a descarga, fora de main().
 */
void actuator_regevent_init(enum actuator_regevent_boot boot);

/* atRegEvent(): enfileira o evento com o carimbo de tempo de agora. Nao
 * bloqueia; ID fora da tabela vira EV_SIS_REG_INV; fila cheia descarta.
 * Chamavel de qualquer thread (nao de ISR).
 */
void actuator_regevent(uint16_t ev);

/* IFCC_START_READ_EVENT: descarrega a fila e le o registro do indice
 * pedido (ACTUATOR_REGEVENT_OVER_INDEX = o mais novo) em out. A sequencia
 * seguinte (actuator_regevent_read_next) continua dele pro mais antigo.
 * Retorna false se a flash nao estiver disponivel (out recebe a marca de
 * fim). Bloqueia o tempo de uma descarga (ate' um apagamento de setor).
 */
bool actuator_regevent_read_start(uint32_t index, uint8_t out[ACTUATOR_REGEVENT_SIZE]);

/* IFCC_SEQ_READ_EVENT: o proximo registro mais antigo. Sem sequencia
 * ativa, recomeca do mais novo (atRegEventIniAcesSeqDef()). Ao completar
 * o buffer, devolve a marca de fim (index 0xFFFFFFFF, EV_SIS_REG_INV).
 */
bool actuator_regevent_read_next(uint8_t out[ACTUATOR_REGEVENT_SIZE]);

#endif /* ACTUATOR_REGEVENT_H_ */
