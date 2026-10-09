/*
 * Protocolo BLE de exposicao do papel de atuador (acgl/ifFerConfig).
 *
 * Espelha BLE/acgl/acgl.c + BLE/Atuador/ifFerConfig.c
 * (ControleCoesterBLE) - o mesmo mecanismo que um Atuador BLE real ja
 * usa pra responder pedidos de leitura/escrita em "areas de memoria".
 * Ver docs/SENSORES_I2C.md, secao "Achado grande: o protocolo BLE de
 * exposicao (acgl/ifFerConfig) ja esta mapeado", pro raciocinio
 * completo e a decodificacao byte a byte que validou este formato.
 *
 * Modulo puramente logico - nao mexe em BLE/GATT diretamente. Quem
 * chama e o handler de escrita da characteristic RX (my_lbs.c), que
 * repassa a resposta pra characteristic TX via notify.
 */
#ifndef ACTUATOR_SERVICE_H_
#define ACTUATOR_SERVICE_H_

#include <stddef.h>
#include <stdint.h>

/* Espelha ACG_TYPE_MSG_t (BLE/acgl/acgl.h no ControleCoesterBLE). */
enum acg_msg_type {
	ACG_MSG_NULL = 0,
	ACG_MSG_WAIT = 1,
	ACG_MSG_STATUS = 2,
	ACG_MSG_REQUEST = 3,
	ACG_MSG_SEND = 4,
	ACG_MSG_CONFIRM = 5,
	ACG_MSG_RESPONSE = 6,
	ACG_MSG_NEG = 7,
	ACG_MSG_TIMEOUT = 8,
	ACG_MSG_LACK = 9,
	ACG_MSG_ALLOCTION = 10,
	ACG_MSG_RELEASE = 11,
	ACG_MSG_REFUSE = 12,
};

/* Tamanho maximo de resposta que este modulo escreve hoje (cabecalho
 * de 8 bytes + o maior payload suportado - a area Painel/paramDado
 * inteira, 144 bytes). Quem chama (my_lbs.c) deve passar um buffer de
 * pelo menos este tamanho. A resposta sai por notify, entao um pedido
 * maior que (MTU - 3 - 8) e' truncado pelo stack - a interface le o
 * paramDado em pedacos de 72 bytes, que cabem em qualquer MTU >= 83.
 */
#define ACTUATOR_SERVICE_MAX_RESP_LEN (8 + 144)

/* Processa um frame recebido pela characteristic RX (cabecalho de 8
 * bytes + payload cru - ver struct msg_hdr em actuator_service.c) e
 * produz a resposta a notificar na characteristic TX. Retorna o
 * numero de bytes escritos em resp_buf (sempre >= 8, o tamanho do
 * cabecalho, mesmo em caso de recusa/erro).
 */
size_t actuator_service_handle_msg(const uint8_t *req_buf, size_t req_len, uint8_t *resp_buf,
				    size_t resp_buf_len);

#endif /* ACTUATOR_SERVICE_H_ */
