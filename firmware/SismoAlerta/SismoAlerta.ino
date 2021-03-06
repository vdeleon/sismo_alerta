/*
  Copyright 2015 Manuel Rodrigo Rábade García <manuel@rabade.net>

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "Wire.h"
#include <TimerOne.h>
#include <Si4707.h>
#include "SismoAlerta.h"

#ifdef YUN_MONITOR
#include <Bridge.h>
#include <Console.h>
#endif

#if YUN_LOGGER
#include <Process.h>
#endif

#if YUN_TWITTER
#include <HttpClient.h>
#include "yun_twitter.h"
#endif

/* variables y objetos globales 
   ============================ */

// weather band radio
Si4707 wbr(SI4707_RESET);
const unsigned long wbr_channels[] = { WBR_CHANNELS };

// banderas y parámetros
boolean scan_error_flag = 0;
byte tune_channel = 0;
boolean asq_prev_status = 0;
byte same_prev_state = 0;
byte same_headers_count = 0;
enum fsm_states { SCAN, LISTEN } state = SCAN;

// timers
unsigned long tune_timer = 0;
unsigned long same_timer = 0;
unsigned long same_test_timer = 0;
volatile unsigned long user_test_timer = 0;
volatile unsigned long same_alert_timer = 0;

// servicio al usuario
volatile unsigned int user_button_integrator = 0;
volatile boolean user_button_prev_state = HIGH;
volatile unsigned long last_user_button_push = 0;
volatile unsigned int last_ext_power_sample = 0;
volatile unsigned long last_update = 0;
volatile boolean update_state = LOW;

// versión monitor
#if YUN_LOGGER
Process proc;
#endif
#if YUN_TWITTER
HttpClient http;
#endif

/* configuración 
   ============= */

void setup() {
  // configuración consola telnet o puerto serial
#ifdef YUN_MONITOR
  Bridge.begin();
  LOG.begin();
#if YUN_LOGGER
  // proceso logger (telnet localhost 6571 | ...)
  proc.runShellCommandAsynchronously("/root/sismo_alerta_logger");
  while (!LOG);
#endif
#else
  LOG.begin(9600);
  LOG.println();
#endif

  // configuramos pines de entrada/salida
  LOG.println(F("SETUP"));
  pinMode(BUZZER, OUTPUT);
  pinMode(POWER_LED_RED, OUTPUT);
  pinMode(POWER_LED_GREEN, OUTPUT);
  pinMode(SIGNAL_LED_RED, OUTPUT);
  pinMode(SIGNAL_LED_GREEN, OUTPUT);
  pinMode(USER_BUTTON, INPUT_PULLUP);
  
  // autoprueba
  LOG.println(F("SELFTEST"));
  digitalWrite(POWER_LED_GREEN, HIGH);
  digitalWrite(SIGNAL_LED_GREEN, HIGH);
  digitalWrite(POWER_LED_RED, LOW);
  digitalWrite(SIGNAL_LED_RED, LOW);
  digitalWrite(BUZZER, HIGH);  
  delay(SELFTEST_DELAY);
  digitalWrite(POWER_LED_GREEN, LOW);
  digitalWrite(SIGNAL_LED_GREEN, LOW);
  digitalWrite(POWER_LED_RED, HIGH);
  digitalWrite(SIGNAL_LED_RED, HIGH);
  digitalWrite(BUZZER, LOW);  
  delay(SELFTEST_DELAY);
  
  // inicializamos si4707
  LOG.println(F("BEGIN"));
  if (wbr.begin()) {
    // iniciamos la interrupción de servicio al usuario
    last_ext_power_sample = analogRead(EXT_POWER);
    Timer1.initialize(SERVICE_USER_MICROSEC);
    Timer1.attachInterrupt(service_user);
    // configuramos si4707
    wbr.setMuteVolume(1);
    wbr.setSNR(TUNE_MIN_SNR);
    wbr.setRSSI(TUNE_MIN_RSSI);
  } else {
    // error al inicializar si4707
    digitalWrite(POWER_LED_RED, HIGH);
    digitalWrite(POWER_LED_GREEN, LOW);
    digitalWrite(SIGNAL_LED_RED, LOW);
    digitalWrite(SIGNAL_LED_GREEN, LOW);
    LOG.println(F("ERROR"));
    while (1);
  }
}

/* maquina de estados 
   ================== */

void loop() {
  switch (state) {
  case SCAN:
    scan();
    break;
  case LISTEN:
    listen();
    break;
  }
}

/* escaneo 
   ======= */

void scan() {
  LOG.println(F("SCAN_START"));

  // tomamos muestras del rssi y snr en todos los canales
  unsigned int rssi_sum[WBR_CHANNELS_SIZE] = { 0 };
  unsigned int snr_sum[WBR_CHANNELS_SIZE] = { 0 };
  for (byte t = 0; t < SCAN_TIMES; t++) {
    for (byte c = 0; c < WBR_CHANNELS_SIZE; c++) {
      wbr.setWBFrequency(wbr_channels[c]);
      for (byte s = 0; s < SCAN_SAMPLES; s++) {
        delay(SCAN_SAMPLE_DELAY);
        unsigned int rssi = wbr.getRSSI();
        rssi_sum[c] += rssi;
        unsigned int snr = wbr.getSNR();
        snr_sum[c] += snr;
#if SCAN_SAMPLE_LOG
        LOG.print(F("SCAN_SAMPLE,"));
        LOG.print(t, DEC);
        LOG.print(F(","));
        LOG.print(s, DEC);
        LOG.print(F(","));
        LOG.print(wbr_channels[c], DEC);
        LOG.print(F(","));
        LOG.print(rssi, DEC);
        LOG.print(F(","));
        LOG.println(snr, DEC);
#endif
      }
    }
    delay(SCAN_DELAY);
  }

  // calculamos rssi/snr promedio y encontramos el mejor canal
  byte best_channel = 0;
  unsigned int best_rssi = 0;
  unsigned int best_snr = 0;
  for (byte c = 0; c < WBR_CHANNELS_SIZE; c++) {
    float rssi_avg = rssi_sum[c] / (float) SCAN_AVG_K;
    float snr_avg = snr_sum[c] / (float) SCAN_AVG_K;
    if (rssi_avg >= best_rssi && snr_avg >= best_snr) {
      best_channel = c;
      best_rssi = rssi_avg;
      best_snr = snr_avg;
    }
    LOG.print(F("SCAN,"));
    LOG.print(wbr_channels[c], DEC);
    LOG.print(F(","));
    LOG.print(rssi_avg, 2);
    LOG.print(F(","));
    LOG.println(snr_avg, 2);
  }

  if (best_rssi >= TUNE_MIN_RSSI && best_snr >= TUNE_MIN_SNR) {
    // si encontramos un canal lo sintonizamos y pasamos al estado monitoreo
    state = LISTEN;
    scan_error_flag = 0;
    tune_channel = best_channel;
    wbr.setWBFrequency(wbr_channels[tune_channel]);
    LOG.print(F("SCAN_OK,"));
    LOG.println(wbr_channels[best_channel], DEC);
  } else {
    // repetimos el escaneo
    scan_error_flag = 1;  // bandera para encender el red de señal en rojo
    LOG.println(F("SCAN_ERROR"));
    delay(SCAN_DELAY);
  }
}

/* monitoreo 
   ========= */

void listen() {
  // monitoreamos si el canal aun es valido
  if (wbr.getRSQ()) {
    tune_timer = millis();
  } else {
    if (millis() - tune_timer > TUNE_LOST_DELAY) {
      state = SCAN;      
      same_test_timer = 0;
      LOG.print(F("TUNE_LOST,"));
      LOG.println(wbr_channels[tune_channel], DEC);
      return;
    }
  }

  // monitoreo del tono 1050 khz
  boolean asq_status = wbr.getASQ();
  if (asq_prev_status != asq_status) {
    if (asq_status) {
      same_reset();
      LOG.println(F("ASQ_ON"));
    } else {
      LOG.println(F("ASQ_OFF"));
    }
    asq_prev_status = asq_status;
    return;
  }

  // monitoreo mensajes same
  byte same_state = wbr.getSAMEState();
  if (same_prev_state != same_state) {
    switch (same_state) {
    case SAME_EOM_DET:
      // fin del mensaje
      LOG.println(F("SAME_EOM_DET"));
      if (same_headers_count > 0) {
        // si recibimos solo 1 o 2 cabeceras
        same_message();
      } else {
        same_reset();
      }
      break;
    case SAME_PRE_DET:
      // preámbulo detectado
      same_timer = millis();
      LOG.println(F("SAME_PRE_DET"));
      break;
    case SAME_HDR_DET:
      // cabecera detectada
      same_timer = millis();
      LOG.println(F("SAME_HDR_DET"));
      break;
    case SAME_HDR_RDY:
      // cabecera lista
      same_timer = millis();
      same_headers_count++;
      LOG.print(F("SAME_HDR_RDY,"));
      LOG.println(same_headers_count);
      break;
    }
    // procesamos mensaje después de recibir tres cabeceras
    if (same_headers_count == 3) {
      same_message();
    }
    same_prev_state = same_state;
    return;
  }

  // timeout mensaje same
  if (same_timer && millis() - same_timer > SAME_TIMEOUT) {
    LOG.println(F("SAME_TIMEOUT"));
    if (same_headers_count > 0) {
      // si recibimos solo 1 o 2 cabeceras
      same_message();
    } else {
      same_reset();
    }
  }

  // timeout prueba same
  if (same_test_timer && millis() - same_test_timer > SAME_TEST_TIMEOUT) {
    same_test_timer = 0;
    LOG.println(F("SAME_TEST_TIMEOUT"));
  }
}

/* mensajes same 
   ============= */

void same_message() {
  // obtenemos mensaje
  byte size = wbr.getSAMESize();
  if (size < 1) {
    LOG.println(F("SAME_EMPTY"));
    return;
  }
  byte msg[size];
  wbr.getSAMEMessage(size, msg);
  same_reset();
  
  // interpretamos evento
  if (size >= 38) {
    if (msg[5] == 'E' &&
        msg[6] == 'Q' &&
        msg[7] == 'W') {
      // alerta sísmica
      same_alert_timer = millis();
      LOG.println(F("SAME_EQW"));
    } else if (msg[5] == 'R' &&
               msg[6] == 'W' &&
               msg[7] == 'T') {
      // prueba periódica
      same_test_timer = millis();
      LOG.println(F("SAME_RWT"));
    }
  }
  
  // reportamos mensaje
  LOG.print(F("SAME,"));
  for (byte i = 0; i < size; i++) {
    if (msg[i] > 31 && msg[i] < 127) {
      LOG.write(msg[i]);
    } else {
      LOG.print(".");
    }
  }
  LOG.println();

#if YUN_TWITTER
  // publicamos mensaje
  String url = WS_URL;
  url += "?secret=";
  url += WS_SECRET;
  url += "&same=";

  for (byte i = 0; i < size; i++) {
    url += (int) msg[i];
    url += ':';
  }

  http.getAsynchronously(url);
  LOG.print("URL,");
  LOG.println(url);
#endif
}

void same_reset() {
  // limpiamos buffer, timer y contador de cabeceras
  wbr.clearSAMEBuffer();
  same_timer = 0;
  same_headers_count = 0;  
}

/* servicio al usuario 
   =================== */

void service_user() {
  // integrador botón de usuario
  if (digitalRead(USER_BUTTON)) {
    if (user_button_integrator < USER_BUTTON_INT_MAX) {
      user_button_integrator++;
    }
  } else {
    if (user_button_integrator > 0) {
      user_button_integrator--;
    }
  }
  boolean user_button_state = user_button_prev_state;
  if (user_button_integrator == 0) {
    user_button_state = LOW;
  } else if (user_button_integrator >= USER_BUTTON_INT_MAX) {
    user_button_state = HIGH;
    user_button_integrator = USER_BUTTON_INT_MAX;
  }

  // procesamos botón de usuario
  if (user_button_prev_state != user_button_state) {
    if (user_button_state == LOW) {
      user_test_timer = 0;
      last_user_button_push = millis();
    }
    user_button_prev_state = user_button_state;
  } else {
    if (user_button_state == LOW) {
      if (millis() - last_user_button_push > USER_BUTTON_TEST_DELAY) {
        user_test_timer = millis();
        last_user_button_push = 0;
      }
    }
  }

  // filtro voltaje externo
  unsigned int ext_power_sample = (float) EXT_POWER_K_REL * analogRead(EXT_POWER);
  ext_power_sample += (1 - (float) EXT_POWER_K_REL) * last_ext_power_sample;
  last_ext_power_sample = ext_power_sample;
  
  // actualizamos
  if (millis() - last_update > UPDATE_DELAY) {
    update_state = !update_state;
    if (same_alert_timer || user_test_timer) {
      alert_user();
    } else {
      update_user();
    }
    last_update = millis();
  }
}

void alert_user() {
  digitalWrite(SIGNAL_LED_RED, update_state);
  digitalWrite(SIGNAL_LED_GREEN, LOW);
  digitalWrite(POWER_LED_RED, !update_state);
  digitalWrite(POWER_LED_GREEN, LOW);
  digitalWrite(BUZZER, update_state);
  if (millis() - same_alert_timer > ALARM_TIME) {
    same_alert_timer = 0;
  }
  if (millis() - user_test_timer > TEST_TIME) {
    user_test_timer = 0;
  }
}

void update_user() {
  // buzzer apagado
  digitalWrite(BUZZER, LOW);
  
  // led señal
  if (state == SCAN) {
    // escaneando canales
    if (scan_error_flag) {
      // error en escaneo
      digitalWrite(SIGNAL_LED_GREEN, LOW);    
      digitalWrite(SIGNAL_LED_RED, HIGH);
    } else {
      // sintonizando por primera vez
      digitalWrite(SIGNAL_LED_GREEN, LOW);
      digitalWrite(SIGNAL_LED_RED, LOW);          
    }
  } else {
    // con canal sintonizado
    digitalWrite(SIGNAL_LED_RED, LOW);      
    if (same_test_timer) {
      // con prueba same vigente
      digitalWrite(SIGNAL_LED_GREEN, HIGH);
    } else {
      // sin prueba same vigente
      digitalWrite(SIGNAL_LED_GREEN, update_state);
    }
  }
  
  // led energía
  volatile unsigned int milivolts = last_ext_power_sample * (float) EXT_POWER_K_MV;
  if (milivolts >= CHARGE_VOLTAGE) {
    // con energía externa
    digitalWrite(POWER_LED_RED, LOW);
    digitalWrite(POWER_LED_GREEN, HIGH);
  } else {
    // sin energía externa
    digitalWrite(POWER_LED_RED, LOW);
    digitalWrite(POWER_LED_GREEN, !update_state);
  }
}
