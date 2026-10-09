/*
 * Motor de alarmes do papel atuador - porta de BLE/Atuador/at_alarm.c
 * (fwBLE original, ~60 alarmes reais, ver ControleCoesterBLE) pra este
 * projeto. Ver a nota grande no .c pro raciocinio completo (o que foi
 * portado 1:1, o que foi deliberadamente reduzido/adiado, e por que).
 *
 * ESCOPO DESTA PRIMEIRA VERSAO (decidido com o Felipe, 2026-09-04): so'
 * os 8 alarmes que atControle.c/atModo.c/atESD.c/atPST.c realmente
 * consultam pra bloquear movimento ou sinalizar estado - nao os ~60 do
 * original. O MOTOR (o algoritmo: tabela acao/retencao, set/release/
 * check/get_block_mov/clears_all) e' portado por inteiro e fielmente -
 * a reducao e' so' no numero de LINHAS da tabela `alarm_ctl`, que foi
 * desenhada pra crescer por "append" (mesmo em espirito do resto do
 * projeto - ver docs/SENSORES_I2C.md, alarmes ficaram na fila la' desde
 * a portagem dos sensores).
 *
 * ATENCAO - risco de fidelidade em aberto: os valores numericos deste
 * enum (`enum actuator_alarm_id`) NAO preservam a posicao exata do
 * `alarm_t` original (que tem ~60 entradas; aqui so' declaramos as 8
 * portadas, na mesma ORDEM RELATIVA mas sem os "buracos" das que faltam).
 * Isso e' seguro enquanto nada expuser o numero do alarme cru por um
 * canal externo (BLE, RS485...) esperando bater com o valor do fwBLE -
 * ate' hoje nenhuma area do protocolo BLE documentada
 * (docs/PROTOCOLO_INTERFACE.md, docs/SENSORES_I2C.md) faz isso. Se um
 * dia isso for necessario (ex.: replicar uma area "lista de alarmes" do
 * ifFerConfig pra compatibilidade com Gateway real), os 60 valores
 * precisam ser alocados de verdade, na mesma posicao do original -
 * revisitar antes de expor isto por BLE.
 */
#ifndef ACTUATOR_ALARM_H_
#define ACTUATOR_ALARM_H_

#include <stdbool.h>
#include <stdint.h>

/* Os alarmes portados ate agora - mesma ordem relativa do enum
 * `alarm_t` original (BLE/Atuador/at_alarm.h), so sem os ~52 que faltam.
 * Adicionar um alarme novo = 1 linha aqui + 1 linha na tabela
 * `alarm_ctl` em actuator_alarm.c (ver comentario la).
 */
enum actuator_alarm_id {
	ACTUATOR_ALARM_ESD_EXT = 0,    /* Comando de ESD e visto como alarme (AL_ESD_EXT) */
	ACTUATOR_ALARM_PARADA_LOCAL,   /* AL_PARADA_LOCAL */
	ACTUATOR_ALARM_MODO_NAO_REMOTO, /* AL_MODO_NAO_REMOTO */
	ACTUATOR_ALARM_LOCAL_INIBIDO,  /* AL_LOCAL_INIBIDO */
	ACTUATOR_ALARM_FAL_ACION_INV,  /* AL_FAL_ACION_INV - acionamento invertido */
	ACTUATOR_ALARM_OPER_MANUAL,    /* AL_OPER_MANUAL - movimento pelo volante */
	ACTUATOR_ALARM_PST_EXE,        /* AL_PST_EXE - PST em execucao */
	ACTUATOR_ALARM_PST_FAL,        /* AL_PST_FAL - PST falhou */
	ACTUATOR_ALARM_COM_SENS_POS,   /* AL_COM_POS - falha de comunicacao com o sensor de posicao.
				        * Anexado no fim (fora da ordem relativa do fwBLE, onde
				        * AL_COM_POS vem antes) pra nao deslocar os bits ja
				        * existentes - ver a nota de risco de fidelidade acima. */
	/* Alarmes de torque (sptTratTrq() em sensPosTor.c) - anexados no fim
	 * pelo mesmo motivo do COM_SENS_POS (bits 9..12).
	 */
	ACTUATOR_ALARM_TORQUE_AB,       /* AL_TORQUE_AB - sobretorque abrindo; bloqueia so' abrir */
	ACTUATOR_ALARM_TORQUE_FC,       /* AL_TORQUE_FC - sobretorque fechando; bloqueia so' fechar */
	ACTUATOR_ALARM_VALV_TRAVADA,    /* AL_VALV_TRAVADA - torque nos dois sentidos; bloqueia os dois */
	ACTUATOR_ALARM_OPER_INCOMPLETA, /* AL_OPER_INCOMPLETA - comando remoto abortado por torque */
	ACTUATOR_ALARM_COM_SENS_TRQ,    /* AL_COM_TRQ - nenhuma fonte de torque online (bit 13) */
	ACTUATOR_ALARM_NUM,
};

/* Status de movimento - espelha at_ctl_mov_stt_t (atControle.h no
 * original: AS_L_SUPER/AS_L_INFER/AS_INCR/AS_DECR/AS_M_PARA). Vive aqui
 * (nao em actuator_control.c, que ainda nao existe - passo 3) porque
 * actuator_alarm_get_block_mov() precisa do tipo agora; quando
 * actuator_control.c for portado, ele deve REUSAR este mesmo enum (nao
 * duplicar) - ver nota grande no .c sobre a dependencia cruzada entre
 * controle e alarme que ja existe no original.
 */
enum actuator_mov_status {
	ACTUATOR_MOV_L_SUPER = 1,
	ACTUATOR_MOV_L_INFER,
	ACTUATOR_MOV_INCR,
	ACTUATOR_MOV_DECR,
	ACTUATOR_MOV_PARADO,
};

/* Equivalente a al_clear_mode_t - ver a decisao "BLE = Local" na memoria
 * de projeto sim-connect-command-control-port-strategy: quando
 * actuator_mode.c (passo 5) existir, uma quitacao vinda da interface BLE
 * usa ACTUATOR_ALARM_CLEAR_LOCAL, RS485/MB TCP/MQTT usam _REMOTE.
 */
enum actuator_alarm_clear_mode {
	ACTUATOR_ALARM_CLEAR_LOCAL,
	ACTUATOR_ALARM_CLEAR_REMOTE,
};

/* Hook pro passo 3 (actuator_control.c, ainda nao existe) preencher -
 * ver nota grande no .c sobre por que isto e' um callback opcional em
 * vez de um #include direto de actuator_control.h. Passar NULL em
 * actuator_alarm_init() pra "sem callback ainda" (comportamento de hoje).
 */
struct actuator_alarm_cb {
	/* Forca parada do movimento - equivalente a at_ctl_stop(ACP_ALARME)
	 * dentro de al_reg_block_mov() no original. Chamado quando
	 * actuator_alarm_get_block_mov() encontra um alarme bloqueando.
	 */
	void (*stop_cb)(void);

	/* Status de movimento atual - equivalente a at_ctl_get_mov_stt()
	 * chamado dentro de at_alarm_release() no original, so' pelos modos
	 * de retencao AR_ACT_INC_INFO/AR_ACT_DEC_INFO (alarmes de torque:
	 * o bloqueio de um sentido so' sai quando o motor vai no outro).
	 * NULL = esses alarmes nunca liberam por release (so' por
	 * actuator_alarm_action_clear()).
	 */
	enum actuator_mov_status (*mov_stt_cb)(void);
};

/* Zera o estado de todos os alarmes (equivalente a at_alarm_ini_pu()) e
 * registra os callbacks pro passo 3 (cb pode ser NULL - "sem callback
 * ainda", que e' o caso hoje).
 */
void actuator_alarm_init(const struct actuator_alarm_cb *cb);

/* Ativa o alarme indicado (equivalente a at_alarm_set()). */
void actuator_alarm_set(enum actuator_alarm_id alarm);

/* Libera o alarme indicado, respeitando o modo de retencao da tabela
 * (equivalente a at_alarm_release()).
 */
void actuator_alarm_release(enum actuator_alarm_id alarm);

/* Libera a ACAO do alarme incondicionalmente, ignorando o modo de
 * retencao (equivalente a at_alarm_action_clear()). Usado quando o
 * volante e' mexido: libera os bloqueios de torque mesmo sem inverter o
 * sentido. A sinalizacao (info) continua ate' ser quitada.
 */
void actuator_alarm_action_clear(enum actuator_alarm_id alarm);

/* Ativa ou libera conforme `active` (equivalente a at_alarm_check()). */
void actuator_alarm_check(bool active, enum actuator_alarm_id alarm);

/* true se o alarme esta ativo agora (equivalente a at_alarm_get_action()). */
bool actuator_alarm_get_action(enum actuator_alarm_id alarm);

/* true se deve ser exibido/sinalizado (ativo, ou ainda nao quitado -
 * equivalente a at_alarm_get_info()).
 */
bool actuator_alarm_get_info(enum actuator_alarm_id alarm);

/* Percorre os alarmes ativos e retorna true se algum bloqueia o
 * movimento pedido em `at_status` (so' ACTUATOR_MOV_INCR/_DECR importam
 * aqui) - equivalente a at_alarm_get_block_mov(). Chama o stop_cb
 * (se registrado) no primeiro alarme bloqueante encontrado, igual ao
 * original.
 */
bool actuator_alarm_get_block_mov(enum actuator_mov_status at_status);

/* Quita (limpa a sinalizacao de) todo alarme cuja acao ja tenha cessado -
 * equivalente a at_alarm_clears_all(). NAO afeta alarmes ainda ativos.
 */
void actuator_alarm_clears_all(enum actuator_alarm_clear_mode mode);

/* true se o motor esta bloqueado por algum alarme agora - equivalente a
 * at_alarm_get_blocked().
 */
bool actuator_alarm_get_blocked(void);

/* Bitmap dos alarmes a sinalizar (bit i = actuator_alarm_get_info(i)).
 * Nao existe no fwBLE (la' a area de alarmes e' o al_stt[] inteiro, 60
 * entradas x 2 bytes) - aqui e' um resumo compacto pra area sintetica
 * que actuator_service.c expoe pra interface. Ver a nota de risco de
 * fidelidade no topo deste header.
 */
uint16_t actuator_alarm_info_bitmap(void);

#endif /* ACTUATOR_ALARM_H_ */
