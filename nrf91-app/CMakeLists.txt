cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sim_connect_nrf91)

target_sources(app PRIVATE
  src/main.c
  src/uart_link.c
  src/mqtt_client.c
)

# Certificado da CA do broker MQTT embutido no binario (nao e' segredo -
# e' o CA publico do broker de desenvolvimento, ver
# docs/PROTOCOLO_91_MQTT.md) - mesmo mecanismo do exercicio de
# referencia (cell-fund/l4/l4_e2).
if(CONFIG_MODEM_KEY_MGMT)
  set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/certs)
  zephyr_include_directories(${gen_dir})
  generate_inc_file_for_target(app credentials/ca-cert.pem ${gen_dir}/ca-cert.pem)
endif()
