#!/usr/bin/env python3
"""Suscriptor de prueba del puente OryzaFlow (verifica TB -> HiveMQ -> backend).

Se conecta al broker HiveMQ Cloud por TLS y muestra lo que llegue al tópico.
Sirve para validar el Rule Chain de ThingsBoard sin necesitar el backend aún.
Uso:  python mqtt_test_subscriber.py
Salir: Ctrl + C
"""
import ssl
import sys
import paho.mqtt.client as mqtt

import broker_config as cfg


def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        print(f"[ok] conectado a {cfg.HIVEMQ_HOST}:{cfg.HIVEMQ_PORT} (TLS)")
        client.subscribe(cfg.TOPIC, qos=1)
        print(f"[ok] suscrito a '{cfg.TOPIC}'  (esperando mensajes...)")
    else:
        print(f"[ERROR] conexion rechazada rc={reason_code} "
              f"(5=usuario/clave malos)")


def on_message(client, userdata, msg):
    payload = msg.payload.decode("utf-8", errors="replace")
    print(f"  <- [{msg.topic}] {payload}")


def main():
    # paho-mqtt 2.x exige declarar la version del API de callbacks.
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.username_pw_set(cfg.HIVEMQ_USER, cfg.HIVEMQ_PASS)
    client.tls_set(tls_version=ssl.PROTOCOL_TLS_CLIENT)  # CA del sistema
    client.on_connect = on_connect
    client.on_message = on_message

    print(f"[..] conectando a {cfg.HIVEMQ_HOST} ...")
    client.connect(cfg.HIVEMQ_HOST, cfg.HIVEMQ_PORT, keepalive=60)
    try:
        client.loop_forever()
    except KeyboardInterrupt:
        print("\n[fin] cerrando.")
        client.disconnect()


if __name__ == "__main__":
    sys.exit(main())
