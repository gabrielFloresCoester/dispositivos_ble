/*
 * Controle logico do atuador - porta de BLE/Atuador/atControle.c
 * (fwBLE, ControleCoesterBLE). Maquina de estado de posicionamento:
 * recebe uma demanda de posicao (0-1000 por mil, 0=fechado 1000=aberto)
 * e uma origem de comando, aciona o motor no sentido certo e para
 * quando: chega na demanda (banda morta), chega no limite fisico
 * aberto/fechado, um alarme bloqueia, ou chega um comando de parada.
 *
 * PORTADO 1:1 (mesma logica, nomes adaptados a convencao do projeto):
 *   at_ctl_run()        -> actuator_control_run()   (chamado periodicamente)
 *   at_ctl_demand()     -> actuator_control_demand()
 *   at_ctl_stop()       -> actuator_control_stop()
 *   at_ctl_open/close   -> actuator_control_open/close (macros)
 *   at_ctl_get_mov_stt/cmd_orig/stop_orig -> idem
 *   AtMovManual()       -> deteccao de movimento pelo volante (alarme OPER_MANUAL)
 *   AtMovInvertido()    -> deteccao de acionamento invertido (alarme FAL_ACION_INV)
 *
 * SEAMS PARA PASSOS FUTUROS (ver notas no .c):
 *   - Origem do comando: no original at_ctl_run() chama atModoAcao()
 *     (arbitro de modo Local/Remoto). Aqui isso e' passo 5 - por
 *     enquanto o comando entra so' por chamada direta de
 *     actuator_control_demand()/_stop() (bench trigger em main.c hoje,
 *     entrada BLE no passo 4).
 *   - at_ctl_demand()/_stop() no original abortam ESD e PST ("qualquer
 *     comando aborta ESD/PST"). ESD/PST sao passo 6 - por enquanto nao
 *     ha o que abortar.
 *   - Parada com assentamento por torque no fechamento (sptTrqParaLimit()
 *     no original) - passo futuro. Por enquanto para direto ao chegar no
 *     limite.
 *   - Parametros (limiteMargem/faixaParado/anteciparParada) - hoje sao
 *     #define com o default do ParamZarI.c; passo 8 move pra
 *     actuator_params/persistencia.
 */
#ifndef ACTUATOR_CONTROL_H_
#define ACTUATOR_CONTROL_H_

#include <stdint.h>

#include "actuator_alarm.h" /* enum actuator_mov_status (espelha at_ctl_mov_stt_t) */

/* Origem do comando, do movimento e da parada - espelha
 * at_ctl_cmd_orig_t (atControle.h). Enum completo mantido por fidelidade
 * mesmo com varios valores ainda sem uso (ESD/PST/barramento/analogico
 * chegam nos passos 5/6/9) - servem so' de rotulo em
 * actuator_control_get_cmd_orig()/_get_stop_orig(), pra quem consome
 * saber POR QUE o movimento parou (chegou na demanda, no limite, por
 * alarme...).
 */
enum actuator_ctl_cmd_orig {
	ACTUATOR_CTL_ORIG_NULO = 0,
	ACTUATOR_CTL_ORIG_LIMITE,
	ACTUATOR_CTL_ORIG_POSICAO,
	ACTUATOR_CTL_ORIG_TORQUE,
	ACTUATOR_CTL_ORIG_LOCAL,
	ACTUATOR_CTL_ORIG_REM_BARR_CAMPO,
	ACTUATOR_CTL_ORIG_REM_DISCRETA,
	ACTUATOR_CTL_ORIG_REM_ANALOG,
	ACTUATOR_CTL_ORIG_ALARME,
	ACTUATOR_CTL_ORIG_CONFIG_COMUNIC,
	ACTUATOR_CTL_ORIG_CONFIG_ANALOG,
	ACTUATOR_CTL_ORIG_FUNC_PST,
	ACTUATOR_CTL_ORIG_FUNC_PST_LOCAL,
	ACTUATOR_CTL_ORIG_FUNC_PST_BARR_CAMPO,
	ACTUATOR_CTL_ORIG_FUNC_PST_ENT_DISCRETA,
	ACTUATOR_CTL_ORIG_FUNC_ESD_BARR_CAMPO,
	ACTUATOR_CTL_ORIG_FUNC_ESD_ENT_DISCRETA,
	ACTUATOR_CTL_ORIG_NUM,
};

/* Inicializa o controle e agenda actuator_control_run() periodico. */
void actuator_control_init(void);

/* Uma passada da maquina de estado - equivalente a at_ctl_run().
 * Chamada internamente pelo work item periodico; exposta so' pra teste.
 */
void actuator_control_run(void);

/* Pede posicionamento em `posic` (0-1000 por mil) - equivalente a
 * at_ctl_demand(). `orig` = quem pediu.
 */
void actuator_control_demand(int16_t posic, enum actuator_ctl_cmd_orig orig);

/* Para o movimento ate a proxima demanda - equivalente a at_ctl_stop(). */
void actuator_control_stop(enum actuator_ctl_cmd_orig orig);

#define actuator_control_open(orig)  actuator_control_demand(1000, (orig))
#define actuator_control_close(orig) actuator_control_demand(0, (orig))

/* Status de posicao/movimento - equivalente a at_ctl_get_mov_stt(). */
enum actuator_mov_status actuator_control_get_mov_stt(void);

/* Origem do ultimo comando / da ultima parada - equivalentes a
 * at_ctl_get_cmd_orig() / at_ctl_get_stop_orig().
 */
enum actuator_ctl_cmd_orig actuator_control_get_cmd_orig(void);
enum actuator_ctl_cmd_orig actuator_control_get_stop_orig(void);

/* Adaptador pro callback stop_cb de actuator_alarm (struct
 * actuator_alarm_cb) - equivale a at_ctl_stop(ACP_ALARME) que o
 * al_reg_block_mov() do fwBLE chama. main.c registra isto em
 * actuator_alarm_init().
 */
void actuator_control_stop_on_alarm(void);

#endif /* ACTUATOR_CONTROL_H_ */
