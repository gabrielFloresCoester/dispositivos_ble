#
# Coester - SIM Connect (Gateway)
# Peripheral (interface web/celular) + Central (ate MAX_ACTUATORS
# atuadores reais via servico proprietario Coester).
#
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sim_connect_nrf54)

# NORDIC SDK APP START
target_sources(app PRIVATE
  src/main.c
  src/my_lbs.c
  src/actuator_client.c
  src/actuator_service.c
  src/uart_link.c
  src/device_health.c
  src/sensor_workq.c
  src/ads1000.c
  src/position_sensor.c
  src/torque_sensor.c
  src/torque_onoff_sensor.c
  src/sample_filter.c
  src/actuator_sensors.c
  src/actuator_params.c
  src/actuator_fsa_cfg.c
  src/actuator_torque.c
  src/watchdog.c
  src/actuator_motor.c
  src/actuator_alarm.c
  src/actuator_control.c
  src/actuator_panel.c
  src/actuator_mode.c
  src/actuator_regevent.c
)
# NORDIC SDK APP END

zephyr_library_include_directories(.)
