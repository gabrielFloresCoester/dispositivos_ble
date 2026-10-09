/*
 * Saida de acionamento do motor (papel atuador) - ver actuator_motor.h
 * pro raciocinio completo (por que GPIO direto e nao FSA) e o mapeamento
 * de nomes com o foSeAc.c original.
 */

#include "actuator_motor.h"
#include "actuator_params.h"
#include "actuator_fsa_cfg.h"

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(actuator_motor, LOG_LEVEL_INF);

/* "zephyr,user" - node especial do Zephyr pra GPIOs de projeto sem um
 * binding proprio (ver overlay de board pro motivo do nome/polaridade
 * dos pinos).
 */
#define MOTOR_GPIO_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec abre_pin = GPIO_DT_SPEC_GET(MOTOR_GPIO_NODE, motor_abre_gpios);
static const struct gpio_dt_spec fecha_pin = GPIO_DT_SPEC_GET(MOTOR_GPIO_NODE, motor_fecha_gpios);

/* Sentido interno - so' pra decidir se loga a transicao no RTT. NAO
 * exposto: quem precisa saber o sentido usa actuator_control_get_mov_stt().
 */
enum motor_dir { MOTOR_PARADO = 0, MOTOR_ABRINDO, MOTOR_FECHANDO };
static enum motor_dir dir = MOTOR_PARADO;

/* Tempo morto na reversao: depois de desligar num sentido, o sentido
 * OPOSTO so' e' energizado apos este intervalo (motor desacelera, rele/
 * ponte H nao comuta com o motor ainda girando ao contrario). Religar no
 * MESMO sentido nao espera. Configuravel - e' o "Tempo de Reversao
 * Acionamento" da FSA do fwBLE (default 3000 ms, 100..10000), ver
 * actuator_fsa_cfg.h. Lido a cada uso: uma alteracao vale na proxima
 * reversao.
 */
#define TEMPO_MORTO_REVERSAO_MS actuator_fsa_cfg_tempo_reversao_ms()

/* Ultimo sentido energizado e quando as saidas foram desligadas - base
 * do tempo morto. Logico (abre/fecha), nao o pino fisico: com antiHorario
 * os pinos trocam, mas a regra continua a mesma.
 */
static enum motor_dir ultimo_sentido = MOTOR_PARADO;
static int64_t desligado_em;
static bool aguardando_log;

/* Nunca energiza as duas saidas ao mesmo tempo - interbloqueio logico
 * minimo (o tempo morto na reversao fica em aguarda_tempo_morto()). Os
 * chamadores deste arquivo (abre/fecha/para abaixo) ja garantem isso por
 * construcao, mas a checagem fica aqui tambem por seguranca - nunca deve
 * disparar.
 */
static void set_pins(bool abre, bool fecha)
{
	if (abre && fecha) {
		LOG_ERR("Pedido de abre+fecha simultaneo - ignorado, parando os dois");
		abre = false;
		fecha = false;
	}

	gpio_pin_set_dt(&abre_pin, abre);
	gpio_pin_set_dt(&fecha_pin, fecha);
}

bool actuator_motor_init(void)
{
	if (!gpio_is_ready_dt(&abre_pin) || !gpio_is_ready_dt(&fecha_pin)) {
		LOG_ERR("GPIO do motor (P1.30/P1.31) nao esta pronto");
		return false;
	}

	int err = gpio_pin_configure_dt(&abre_pin, GPIO_OUTPUT_INACTIVE);

	if (err) {
		LOG_ERR("Falha ao configurar pino de abertura do motor (err %d)", err);
		return false;
	}

	err = gpio_pin_configure_dt(&fecha_pin, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Falha ao configurar pino de fechamento do motor (err %d)", err);
		return false;
	}

	dir = MOTOR_PARADO;
	LOG_INF("Saida de motor iniciada (GPIO direto P1.30/P1.31, sem FSA)");
	return true;
}

/* true = ainda nao pode energizar `sentido` (reversao dentro do tempo
 * morto). Se o motor estiver girando no sentido oposto, desliga agora e
 * comeca a contar. O laco de controle chama abre()/fecha() a cada ciclo,
 * entao o sentido novo entra sozinho quando o tempo vence.
 */
static bool aguarda_tempo_morto(enum motor_dir sentido)
{
	enum motor_dir oposto = (sentido == MOTOR_ABRINDO) ? MOTOR_FECHANDO : MOTOR_ABRINDO;

	if (ultimo_sentido != oposto) {
		return false;
	}

	if (dir == oposto) {
		actuator_motor_para();
	}

	if (k_uptime_get() - desligado_em < TEMPO_MORTO_REVERSAO_MS) {
		if (!aguardando_log) {
			LOG_INF("Reversao: aguardando %d ms de tempo morto", TEMPO_MORTO_REVERSAO_MS);
			aguardando_log = true;
		}
		return true;
	}

	aguardando_log = false;
	return false;
}

void actuator_motor_abre(void)
{
	if (aguarda_tempo_morto(MOTOR_ABRINDO)) {
		return;
	}
	if (dir != MOTOR_ABRINDO) {
		LOG_INF("****************** ABRINDO ******************");
	}
	dir = MOTOR_ABRINDO;
	ultimo_sentido = MOTOR_ABRINDO;
	/* fsaIncr(): com paramDado.antiHorario, abrir aciona o sentido
	 * oposto (a leitura de posicao tambem e' complementada, ver
	 * actuator_sensors.c - os dois juntos mantem o controle coerente).
	 */
	if (actuator_params_anti_horario()) {
		set_pins(false, true);
	} else {
		set_pins(true, false);
	}
}

void actuator_motor_fecha(void)
{
	if (aguarda_tempo_morto(MOTOR_FECHANDO)) {
		return;
	}
	if (dir != MOTOR_FECHANDO) {
		LOG_INF("****************** FECHANDO ******************");
	}
	dir = MOTOR_FECHANDO;
	ultimo_sentido = MOTOR_FECHANDO;
	/* fsaDecr(): espelho do antiHorario em actuator_motor_abre() */
	if (actuator_params_anti_horario()) {
		set_pins(true, false);
	} else {
		set_pins(false, true);
	}
}

void actuator_motor_para(void)
{
	if (dir != MOTOR_PARADO) {
		LOG_INF("****************** PARADO ******************");
		desligado_em = k_uptime_get(); /* base do tempo morto */
		aguardando_log = false;
	}
	dir = MOTOR_PARADO;
	set_pins(false, false);
}

bool actuator_motor_acionado(void)
{
	return dir != MOTOR_PARADO;
}
