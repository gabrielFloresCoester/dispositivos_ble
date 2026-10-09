/*
 * Configuracao do acionamento herdada da placa FSA do fwBLE - hoje so' o
 * "Tempo de Reversao Acionamento" (dadoFSA.tempoReverContat).
 *
 * No fwBLE esse tempo morto vive na placa FSA (firmware MSP430,
 * ControleCoesterMSP430/FSA/Aplic_FSA.c: default 3000 ms; faixa
 * MIN/MAX_TEMPO_REVER = 100..10000 ms em proCo/areaFSA.h; a IHM ajusta
 * em passos de 100 ms). O SIM Connect nao tem FSA (motor por GPIO direto),
 * entao o valor mora aqui e quem o aplica e' actuator_motor.c.
 *
 * Exposto no endereco REAL da area FSA do ifFerConfig
 * (MSG_EX_ADDRESS_CONFIG_FSA = 0x00801000), no offset do campo dentro de
 * tDadoFSA: info[TAM_INFO = 16] e depois configSisFSA, cujo primeiro
 * campo e' tempoReverContat (u16 LE) -> 0x00801010. SO' esse campo e'
 * emulado: o resto da area FSA (falhas, alarmes, status, entradas e
 * saidas da placa) descreve hardware que nao existe aqui e responde
 * GTM_NEG. Um configurador/Gateway que ajuste o tempo de reversao de um
 * Atuador BLE legado faz o mesmo aqui.
 *
 * DIVERGENCIA: no fwBLE a escrita BLE e' crua e a FSA so' grava com o
 * "salvar" da IHM; aqui a escrita valida a faixa (fora -> GTM_NEG) e
 * grava na hora, como o salvar por campo da interface.
 */
#ifndef ACTUATOR_FSA_CFG_H_
#define ACTUATOR_FSA_CFG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ACTUATOR_FSA_TEMPO_REVER_ADDR 0x00801010UL
#define ACTUATOR_FSA_TEMPO_REVER_LEN  2U

#define ACTUATOR_FSA_TEMPO_REVER_MIN     100   /* MIN_TEMPO_REVER */
#define ACTUATOR_FSA_TEMPO_REVER_MAX     10000 /* MAX_TEMPO_REVER */
#define ACTUATOR_FSA_TEMPO_REVER_DEFAULT 3000  /* Aplic_FSA.c */

/* Default + handler de persistencia - chamar ANTES de settings_load(). */
void actuator_fsa_cfg_init(void);

/* Tempo morto na reversao do motor, em ms. */
uint16_t actuator_fsa_cfg_tempo_reversao_ms(void);

/* Leitura/escrita por offset dentro do campo (0..1), como as demais areas
 * acgl. A escrita precisa deixar o u16 inteiro dentro da faixa
 * [MIN, MAX]; se nao, retorna false e nada muda. Valido -> grava.
 */
bool actuator_fsa_cfg_read(uint16_t offset, uint8_t *buf, size_t len);
bool actuator_fsa_cfg_write(uint16_t offset, const uint8_t *data, size_t len);

#endif /* ACTUATOR_FSA_CFG_H_ */
