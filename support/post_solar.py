#!/usr/bin/env python3

# Script to get total generated today from the invertor and upload to a web page
# via a post request to a node.js application running on our hosting platform.

import requests
import logging
import sys
import minimalmodbus
import serial
import time
import json
from datetime import datetime
from datetime import timezone
import paho.mqtt.client as mqtt

# Set logging level
logging.basicConfig(stream=sys.stderr, level=logging.DEBUG)

# api-endpoint
API_ENDPOINT = "https://www.quirkyquipu.co.uk/solar?appid=404d4772-40b2-405e-a6d4-c1b940b5f857&action=update"

# mqtt 
user = 'solar'
passwd = 'sunglorioussun'
host = 'localhost'
port = 1883
usedtoday = ''
hotwater = ''
battery = ''

def modbus_connect():
  instrument = minimalmodbus.Instrument("/dev/ttyAMA0", 1) # Set to inverter"s address
  instrument.serial.baudrate = 9600
  instrument.serial.bytesize = 8
  instrument.serial.parity   = serial.PARITY_NONE
  instrument.serial.stopbits = 1
  instrument.serial.timeout  = 3
  # instrument.debug = TrueÂ§Â§
  return instrument

def modbus_read(instrument):
  # set all values to 0, inverter seems to shut down during the night!
  Today_KW = 0

  #timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")
  timestamp = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S")

  # get data from solis
  Today_KW = instrument.read_register(3014, number_of_decimals=1, functioncode=4, signed=False) # Read Today Energy (KWH Total) as 16-Bit
  logging.info("{:<23s}{:10.2f} kWh".format("Generated (Today)", Today_KW))

  data = {
    "online": timestamp
  }

  # Fix for 0-values during inverter powerup
  if Today_KW > 0: 
      data["today"] = Today_KW
  else:
      data["today"] = 0.0

  return data

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        logging.debug('MQTT connection success...')
    else:
        logging.error('MQTT connection failed')

def on_disconnect(client, userdata, rc):
    if rc != 0:
        logging.error('Unexpected MQTT diconnection')

def on_message(client, userdata, message):
    global usedtoday
    global hotwater
    global battery

    # convert from json to object
    message_obj = json.loads(message.payload)

    usedtoday = message_obj["savedToday"]
    hotwater = message_obj["hotWater"]
    battery = message_obj["battery"]

    if usedtoday and hotwater and battery:
        logging.debug('MQTT data received from ESP32')
        mqttloop = False
        client.disconnect()
        client.loop_stop()


def main():
  try:
    modc = modbus_connect()
    logging.debug(modc)

    data = modbus_read(modc)
    logging.debug(data)

    # Total kWh to send to website
    total = data["today"]
    timenow = data["online"]

    # Get iBoost info via mqtt message queue. Data will arrive in one JSON message 
    # from the ESP/CC1101 program.
    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.username_pw_set(user, passwd)
    client.connect(host, port)
    client.on_message = on_message
    client.subscribe('iboost/iboost')
    client.loop_start()

    '''
    ESP32 program will send iboost message every 10 seconds, they are either 
    there or not.  Give ourselves 15 seconds to get a message.
    '''
    time.sleep(15)  # time to collect any iboost messages
    client.disconnect()
    client.loop_stop()

    app_data = '&total='+str(total)+'&time='+timenow+'&usedtoday='+str(usedtoday)+'&hotwater='+hotwater+'&battery='+battery
    r = requests.post(API_ENDPOINT+app_data)

    logging.info(app_data)
    # extracting response text
    logging.info(r.text)

  except TypeError as err:
    logging.error("TypeError:\n{}".format(err))

  except ValueError as err:
    logging.error("ValueError:\n{}".format(err))

  except minimalmodbus.NoResponseError as err:
    logging.error("Modbus no response:\n{}".format(err))

  except Exception as err:
    logging.error("Exception:\n{}".format(err))

if __name__ == "__main__":
  main()