/*
 *  ============================================================================
 *  Control de Caldera por Correo Electronico - IMAP IDLE
 *  ============================================================================
 *
 *  Autor: Angel de la Iglesia Gonzalo
 *  Version/rev: 1/0
 *  Licencia: MIT - Ver LICENSE.txt
 *  Fecha: 20261103
 *
 *  Placa: ESP32 LOLIN32 (WEMOS) / LOLIN32 Lite
 *    - MCU: ESP32-WROOM-32 (dual-core 240 MHz, 32-bit)
 *    - Flash: 4 MB
 *    - WiFi: 802.11 b/g/n (2.4 GHz)
 *    - Nota: en la variante LoLin32 Lite (ASIN B086V1P4BL) el header NO
 *      expone el pin 5V/VIN: solo trae railes 3V (3.3V) y G (GND). El
 *      DC+ del relay debe alimentarse con 5V externo o del VBUS USB.
 *
 *  Relé de potencia: GTIWUNG 1 canal 5V con optoacoplador
 *    - (ASIN B0CSJQZ89V, modelo T722)
 *    - Pines de entrada: DC+ (+5V externo o VBUS USB), DC- (GND), IN (control, HIGH trigger)
 *    - Contactos: NO / COM / NC (cargar maxima AC 250V/10A, DC 30V/10A)
 *    - Jumper High/Low trigger configurado en HIGH (activacion con nivel alto)
 *    - Corriente: standby 5 mA, activacion ~70 mA, trigger 2-4 mA
 *    - Paquete: 10 unidades, pinout DC+/DC-/IN (no usa JD-VCC)
 *
 *  Entornos y versiones usados en la compilacion:
 *    - arduino-cli (1.x)
 *    - Core ESP32 de Espressif (arduino-esp32) v3.3.11  
 *    - BSP: esp32:esp32:lolin32 (WEMOS LOLIN32)
 *    - Bibliotecas:
 *      - ESP Mail Client (Mobizt) v3.4.24
 *        GitHub: https://github.com/mobizt/ESP-Mail-Client
 *        Soporta: IMAP IDLE + SMTP + borrado IMAP
 *      - Preferences (integrada en el core ESP32)
 *        Almacenamiento del estado de la caldera en flash (NVS)
 *      - WiFi (integrada en el core ESP32)
 *
 *  Instalacion de core y libreria con arduino-cli:
 *    arduino-cli core update-index
 *    arduino-cli core install esp32:esp32
 *    arduino-cli lib install "ESP Mail Client"
 *
 *  Compilar (desde este directorio):
 *    arduino-cli compile --fqbn esp32:esp32:lolin32 .
 *
 *  Subir a la placa (ajustar el puerto de tu sistema):
 *    arduino-cli board list                                   # ver puerto
 *    arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:lolin32 .
 *
 *  Compilar + subir en un solo paso:
 *    arduino-cli compile --fqbn esp32:esp32:lolin32 . -u -p /dev/ttyUSB0
 *
 *  Pinout del proyecto:
 *    - GPIO 26  -> IN del modulo relay (sig. control, HIGH enciende la caldera)
 *    - GPIO 14  -> boton de prueba (INPUT_PULLUP, boton a GND, LOW al pulsar)
 *    - Relay DC+ -> +5V externo o VBUS USB (no disponible en el header Lite)
 *    - Relay DC- -> GND comun del ESP32
 *
 *  Estrategia: IMAP IDLE (push en tiempo real)
 *    - El ESP32 mantiene una conexion IMAP permanente con IDLE activo
 *    - Gmail notifica al ESP32 en el instante que llega un correo nuevo
 *    - Se refresca la conexion cada 28 minutos (limite IMAP ~29 min)
 *    - Barrido periodico cada 2 min por si se pierde alguna notificacion
 *    - Lectura con criterio "ALL": obtiene TODOS los correos pendientes
 *    - Tras procesar y borrar un correo, se reactiva la escucha IDLE
 *
 *  Funcionamiento:
 *    - Asunto "encender caldera"  -> enciende la caldera (GPIO 26 = HIGH)
 *    - Asunto "apagar caldera"    -> apaga la caldera   (GPIO 26 = LOW)
 *    - Asunto "estado caldera" -> responde con el estado actual
 *    - Cualquier otro asunto se ignora y se descarta sin respuesta
 *    - Los correos en espera se procesan secuencialmente por orden de llegada
 *    - Tras respuesta SMTP exitosa, el correo se borra del servidor
 *    - Si SMTP falla, el correo queda para reintentar
 *
 *  Prueba del relé por boton (GPIO 14):
 *    - Pulsacion: 3 ciclos de 10 s ON + 10 s OFF (tiempo total ~60 s)
 *    - Durante la prueba NO se envian correos ni se procesa la
 *      operatividad normal: los correos entrantes quedan en INBOX
 *    - Al terminar, el relé vuelve al estado real guardado en NVS
 *
 *  Persistencia y reinicio:
 *    - Cada cambio de estado se guarda en la flash (NVS) via Preferences
 *    - Tras un corte de alimentacion, el arranque recupera el estado previo
 *    - En cada reinicio se envia a EMAIL_REINICIO un aviso con asunto
 *      "Reinicio de caldera" y el estado actual del sistema
 *
 *  Protocolos:
 *    - IMAP IDLE: imap.gmail.com:993 (SSL)
 *    - SMTP:      smtp.gmail.com:465 (SSL)
 *    - NTP:       pool.ntp.org / time.nist.gov
 *    - Zona horaria: cadena POSIX con cambio automatico verano/invierno
 *
 *  ============================================================================
 */

#include <ESP_Mail_Client.h>
#include <Preferences.h>

// ===================== CREDENCIALES - REEMPLAZAR =====================
#define WIFI_SSID         "YOUR_WIFI_SSID"
#define WIFI_PASSWORD     "YOUR_WIFI_PASSWORD"
#define EMAIL_ACCOUNT     "YOUR_EMAIL@gmail.com"
#define EMAIL_PASSWORD    "YOUR_APP_PASSWORD"
#define EMAIL_REINICIO    "EMAIL_REINICIO@gmail.com"
// ======================================================================

#define RELAY_PIN         26

// Boton de prueba: pulsacion => conmutacion temporizada del relé
// (10 s ON + 10 s OFF repetido 3 veces). GPIO 14 es un pin libre del
// Lolin32; se configura con pull-up interno (INPUT_PULLUP), por lo que
// el boton debe conectar el pin a GND al pulsarse (LOW = pulsado).
#define TEST_BUTTON_PIN   14
#define PRUEBA_CICLOS     3
#define PRUEBA_ON_MS      (10UL * 1000UL)
#define PRUEBA_OFF_MS     (10UL * 1000UL)
#define BUTTON_DEBOUNCE_MS 50

#define IDLE_REFRESH_MS   (28UL * 60UL * 1000UL)
#define SWEEP_MS          (2UL * 60UL * 1000UL)

#define IMAP_HOST         "imap.gmail.com"
#define IMAP_PORT         993

#define SMTP_HOST         "smtp.gmail.com"
#define SMTP_PORT         465

// ===================== ZONA HORARIA - SELECCIONAR PAIS =====================
// Descomentar SOLO la linea de tu pais.
// Lista completa de cadenas POSIX:
//   https://github.com/esp8266/Arduino/blob/master/cores/esp8266/TZ.h
//
#define TZ_SPAIN       "CET-1CEST,M3.5.0,M10.5.0/3"     // Espana peninsular
// #define TZ_PORTUGAL  "WET-0WEST,M3.5.0,M10.5.0/1"    // Portugal
// #define TZ_FRANCE    "CET-1CEST,M3.5.0,M10.5.0/3"    // Francia
// #define TZ_GERMANY   "CET-1CEST,M3.5.0,M10.5.0/3"    // Alemania
// #define TZ_UK        "GMT0BST,M3.5.0,M10.5.0"        // Reino Unido
// #define TZ_ITALY     "CET-1CEST,M3.5.0,M10.5.0/3"    // Italia
// #define TZ_ARGENTINA "ART3"                          // Argentina (sin DST)
// #define TZ_MEXICO    "CST6CDT,M4.1.0,M10.5.0"        // Mexico (CDMX)
// #define TZ_US_EAST   "EST5EDT,M3.2.0,M11.1.0"        // EE.UU. este
//
#define MY_TZ  TZ_SPAIN    // <--- seleccionar aqui
// ===========================================================================

IMAPSession imap;
SMTPSession smtp;
Session_Config imapConfig;
IMAP_Data imapData;
Preferences prefs;

bool calderaOn = false;
unsigned long lastIdleRefresh = 0;
unsigned long lastSweepMs = 0;

// Estado de la prueba temporal del relé (disparada por boton).
bool pruebaActiva = false;
bool pruebaRelayOn = false;
int  pruebaCiclo = 0;
unsigned long pruebaNextToggle = 0;
unsigned long lastButtonPulseMs = 0;

void reconectarWiFi();
bool processMessage(const IMAP_MSG_Item& msg);
void guardarEstadoCaldera();
void procesarBandeja();
void leerYProcesarCorreos();
void iniciarPrueba();
void actualizarPrueba();
bool comprobarBoton();

// Procesa secuencialmente (por orden de llegada) todos los mensajes
// obtenidos en la ultima lectura IMAP. Cada mensaje se ejecuta y borra
// antes de pasar al siguiente. El borrado se hace aqui, fuera del
// callback de la libreria, para no anidar operaciones sobre la sesion.
void procesarBandeja() {
  IMAP_MSG_List msgList = imap.data();
  size_t total = msgList.msgItems.size();
  if (total == 0)
    return;

  Serial.println("[MAIL] Mensajes en espera: " + String(total));

  for (size_t i = 0; i < total; i++) {
    uint32_t uid = msgList.msgItems[i].UID;

    Serial.println("\n   [MSG " + String(i + 1) + "/" + String(total) + "] UID: " + String(uid));

    if (processMessage(msgList.msgItems[i])) {
      if (imap.deleteMessages(String(uid), true, true)) {
        Serial.println("   >> Correo eliminado de Gmail");
      } else {
        Serial.println("   >> Error al borrar correo: " + imap.errorReason());
      }
    } else {
      Serial.println("   >> Correo mantenido en servidor para reintento");
    }
  }
}

// Lee los correos pendientes del servidor y los procesa.
void leerYProcesarCorreos() {
  if (MailClient.readMail(&imap, false)) {
    procesarBandeja();
  } else {
    Serial.println("[MAIL] Error lectura IMAP: " + imap.errorReason());
  }
  imap.empty();
  imap.selectFolder(F("INBOX"));
  lastIdleRefresh = millis();
  lastSweepMs = millis();
}

String normalizeSubject(const String& raw) {
  String s = raw;
  s.toLowerCase();
  s.trim();
  String out;
  bool prevSpace = false;
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s.charAt(i);
    if (c == ' ' || c == '\t') {
      if (!prevSpace && out.length() > 0) {
        out += ' ';
        prevSpace = true;
      }
    } else {
      out += c;
      prevSpace = false;
    }
  }
  out.trim();
  return out;
}

String extractEmail(const String& raw) {
  int start = raw.indexOf('<');
  int end   = raw.indexOf('>');
  if (start >= 0 && end > start) {
    return raw.substring(start + 1, end);
  }
  return raw;
}

void guardarEstadoCaldera() {
  prefs.begin("caldera", false);
  prefs.putBool("estado", calderaOn);
  prefs.end();
  Serial.println("   [NVS] Estado guardado en flash: " + String(calderaOn ? "ENCENDIDA" : "APAGADA"));
}

bool sendEmailResponse(const String& toEmail, const String& subject, const String& body) {
  Session_Config config;
  config.server.host_name = SMTP_HOST;
  config.server.port = SMTP_PORT;
  config.login.email = EMAIL_ACCOUNT;
  config.login.password = EMAIL_PASSWORD;
  config.login.user_domain = F("127.0.0.1");
  config.time.ntp_server = F("pool.ntp.org,time.nist.gov");
  config.time.gmt_offset = 0;
  config.time.day_light_offset = 0;

  if (!smtp.connect(&config)) {
    Serial.println("   Error SMTP: " + smtp.errorReason());
    return false;
  }
  // Restaurar zona horaria POSIX tras configTime() de la libreria
  configTzTime(MY_TZ, "pool.ntp.org", "time.nist.gov");

  SMTP_Message message;
  message.sender.name = F("ESP32 Caldera");
  message.sender.email = EMAIL_ACCOUNT;
  message.subject = subject;
  message.addRecipient(F("Usuario"), toEmail.c_str());
  message.text.content = body;
  message.text.charSet = F("utf-8");
  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_low;

  bool ok = MailClient.sendMail(&smtp, &message);
  if (!ok)
    Serial.println("   Error envio: " + smtp.errorReason());

  smtp.closeSession();
  return ok;
}

// Devuelve true si el correo debe eliminarse del servidor:
//   - comando procesado con respuesta SMTP exitosa
//   - comando redundante (sin cambio de estado): se descarta sin respuesta
//   - asunto no reconocido: se ignora y se descarta sin respuesta
// Devuelve false solo si el SMTP fallo: el correo queda para reintentar.
bool processMessage(const IMAP_MSG_Item& msg) {
  String senderEmail = extractEmail(String(msg.from));
  String subject     = msg.subject;

  Serial.println("   De: " + senderEmail);
  Serial.println("   Asunto original: " + subject);

  String norm = normalizeSubject(subject);
  Serial.println("   Asunto normalizado: [" + norm + "]");

  if (norm == "encender caldera") {
    if (!calderaOn) {
      digitalWrite(RELAY_PIN, HIGH);
      calderaOn = true;
      guardarEstadoCaldera();
      Serial.println("   >> Caldera ENCENDIDA");
      return sendEmailResponse(senderEmail, "caldera encendida", "La caldera esta ENCENDIDA.");
    }
    Serial.println("   >> Caldera ya estaba encendida, sin cambio. Se descarta el correo");
    return true;
  }

  if (norm == "apagar caldera") {
    if (calderaOn) {
      digitalWrite(RELAY_PIN, LOW);
      calderaOn = false;
      guardarEstadoCaldera();
      Serial.println("   >> Caldera APAGADA");
      return sendEmailResponse(senderEmail, "caldera apagada", "La caldera esta APAGADA.");
    }
    Serial.println("   >> Caldera ya estaba apagada, sin cambio. Se descarta el correo");
    return true;
  }

  if (norm == "estado caldera") {
    Serial.println("   >> Consulta de estado");
    String estado = calderaOn ? "caldera encendida" : "caldera apagada";
    return sendEmailResponse(senderEmail, estado, "Estado: " + estado);
  }

  Serial.println("   >> Asunto no reconocido. Ignorado, se descarta sin respuesta");
  return true;
}

// Inicia la prueba temporal del relé: 10 s ON seguido de 10 s OFF,
// repetido PRUEBA_CICLOS veces. Durante la prueba no se envia ningun
// correo ni se procesa la operatividad normal (ver loop()).
void iniciarPrueba() {
  Serial.println("\n========================================");
  Serial.println("  PRUEBA DEL RELE POR BOTON");
  Serial.println("  Secuencia: " + String(PRUEBA_CICLOS) + " x (10 s ON + 10 s OFF)");
  Serial.println("========================================");

  pruebaCiclo = 1;
  pruebaRelayOn = true;                 // Empieza con ON
  digitalWrite(RELAY_PIN, HIGH);
  Serial.println("   [PRUEBA] Ciclo 1/3 -> ON (10 s)");
  pruebaNextToggle = millis() + PRUEBA_ON_MS;
  pruebaActiva = true;
}

// Avanza la maquina de estados de la prueba (no bloqueante).
// Devuelve true mientras la prueba siga activa.
void actualizarPrueba() {
  if (!pruebaActiva)
    return;

  unsigned long now = millis();

  if (now < pruebaNextToggle)
    return;

  if (pruebaRelayOn) {
    // Fin del periodo ON (10 s): cambiar a OFF
    digitalWrite(RELAY_PIN, LOW);
    pruebaRelayOn = false;
    Serial.println("   [PRUEBA] Ciclo " + String(pruebaCiclo) + "/" + String(PRUEBA_CICLOS) + " -> OFF (10 s)");
    pruebaNextToggle = now + PRUEBA_OFF_MS;
  } else {
    // Fin del periodo OFF (10 s): siguiente ciclo ON o fin de la prueba
    if (pruebaCiclo < PRUEBA_CICLOS) {
      pruebaCiclo++;
      digitalWrite(RELAY_PIN, HIGH);
      pruebaRelayOn = true;
      Serial.println("   [PRUEBA] Ciclo " + String(pruebaCiclo) + "/" + String(PRUEBA_CICLOS) + " -> ON (10 s)");
      pruebaNextToggle = now + PRUEBA_ON_MS;
    } else {
      // Prueba terminada: restaurar el estado real de la caldera
      pruebaActiva = false;
      digitalWrite(RELAY_PIN, calderaOn ? HIGH : LOW);
      Serial.println("   [PRUEBA] >>> PRUEBA FINALIZADA <<<");
      Serial.println("   [PRUEBA] Estado real restaurado: " + String(calderaOn ? "ENCENDIDA" : "APAGADA"));
    }
  }
}

// Comprueba el boton con debounce (boton conectado a GND, pull-up interno).
// Devuelve true en el flanco de bajada (pulsacion) de modo no repetitivo.
bool comprobarBoton() {
  bool pulsado = (digitalRead(TEST_BUTTON_PIN) == LOW);
  static bool estadoAnterior = false;
  static unsigned long lastDebounce = 0;

  if (pulsado != estadoAnterior) {
    lastDebounce = millis();
    estadoAnterior = pulsado;
  }

  // Solo dispara en el flanco de bajada estable (ignora el flanco de subida)
  if (pulsado && (millis() - lastDebounce >= BUTTON_DEBOUNCE_MS) && (millis() - lastButtonPulseMs > 300)) {
    lastButtonPulseMs = millis();
    return true;
  }

  return false;
}

bool iniciarEscuchaIMAP() {
  Serial.println("\n[IDLE] Conectando al servidor IMAP...");

  imapConfig.server.host_name = IMAP_HOST;
  imapConfig.server.port = IMAP_PORT;
  imapConfig.login.email = EMAIL_ACCOUNT;
  imapConfig.login.password = EMAIL_PASSWORD;
  imapConfig.time.ntp_server = F("pool.ntp.org,time.nist.gov");
  imapConfig.time.gmt_offset = 0;
  imapConfig.time.day_light_offset = 0;
  imapConfig.network_connection_handler = reconectarWiFi;

  imapData.fetch.headerOnly = true;
  // CRITICO: con criterio vacio la libreria SOLO obtiene el ultimo mensaje.
  // Con "ALL" busca todos los correos pendientes en INBOX y los devuelve
  // todos, permitiendo el procesamiento secuencial y el descarte de los
  // no reconocidos. Al borrarse los procesados, normalmente devuelve cero.
  imapData.search.criteria = "ALL";

  if (!imap.connect(&imapConfig, &imapData)) {
    Serial.println("   Error conexion IMAP: " + imap.errorReason());
    imap.closeSession();
    return false;
  }
  // Restaurar zona horaria POSIX tras configTime() de la libreria
  configTzTime(MY_TZ, "pool.ntp.org", "time.nist.gov");

  imap.keepAlive(5, 5, 1);

  if (!imap.selectFolder(F("INBOX"))) {
    Serial.println("   Error al abrir INBOX");
    imap.closeSession();
    return false;
  }

  lastIdleRefresh = millis();
  Serial.println("   >>> IDLE LISTO - Enviando listen() en loop() <<<");
  return true;
}

void reconectarWiFi() {
  Serial.print("[WIFI] Reconectando");
  WiFi.reconnect();
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    Serial.print(".");
    delay(500);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Conectado - IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] ERROR: No se pudo conectar");
  }
}

void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("\n========================================");
  Serial.println("  Control de Caldera - Versión/rev: 1/0");
  Serial.println("========================================\n");

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(TEST_BUTTON_PIN, INPUT_PULLUP);
  Serial.println("[INIT] Btn prueba en GPIO " + String(TEST_BUTTON_PIN) +
                 " (pulsar a GND) -> prueba del rele 3 x (10 s ON + 10 s OFF)");

  prefs.begin("caldera", true);
  calderaOn = prefs.getBool("estado", false);
  prefs.end();

  digitalWrite(RELAY_PIN, calderaOn ? HIGH : LOW);
  Serial.println("[INIT] Relay en GPIO " + String(RELAY_PIN) + " -> " +
                 (calderaOn ? "ENCENDIDO (estado recuperado de flash)" : "APAGADO (estado recuperado de flash)"));

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[WIFI] Conectando");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    Serial.print(".");
    delay(500);
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Conectado - IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n[WIFI] ERROR: No se pudo conectar");
  }

  MailClient.networkReconnect(true);
  imap.debug(1);

  // Configurar zona horaria POSIX (cambio automatico verano/invierno)
  configTzTime(MY_TZ, "pool.ntp.org", "time.nist.gov");

  if (WiFi.status() == WL_CONNECTED) {
    String estado = calderaOn ? "caldera encendida" : "caldera apagada";
    String cuerpo = "El sistema se ha reiniciado tras un corte de alimentación\nEstado actual " + estado;
    Serial.println("[REINICIO] Enviando aviso a " + String(EMAIL_REINICIO));
    if (sendEmailResponse(EMAIL_REINICIO, "Reinicio de caldera", cuerpo)) {
      Serial.println("[REINICIO] Aviso enviado correctamente");
    } else {
      Serial.println("[REINICIO] ERROR: no se pudo enviar el aviso");
    }

    if (iniciarEscuchaIMAP()) {
      Serial.println("[SYSTEM] Comprobando correos acumulados...");
      leerYProcesarCorreos();
    }
  }

  Serial.println("\n[SYSTEM] Enviar asunto 'encender caldera' / 'apagar caldera' / 'estado caldera'");
  Serial.println("[SYSTEM] Latencia: ~0 segundos (push en tiempo real)\n");
}

void loop() {
  // ==================== BOTON DE PRUEBA DEL RELE ====================
  // Si se pulsa el boton se inicia la prueba temporal. Mientras la
  // prueba este activa, NO se envia ningun correo ni se procesa la
  // operatividad normal; los correos que lleguen se quedan en INBOX y
  // se procesan al terminar la prueba (barrido/IDLE posterior).
  if (!pruebaActiva && comprobarBoton()) {
    imap.stopListen();
    iniciarPrueba();
  }

  if (pruebaActiva) {
    actualizarPrueba();
    return;
  }
  // ==================================================================

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WIFI] Desconectado, reconectando...");
    reconectarWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      if (iniciarEscuchaIMAP()) {
        leerYProcesarCorreos();
      }
    }
    return;
  }

  if (!imap.connected()) {
    Serial.println("[IDLE] Conexion IMAP perdida, reconectando...");
    if (millis() - lastIdleRefresh > 10000) {
      if (iniciarEscuchaIMAP()) {
        leerYProcesarCorreos();
      }
    }
    return;
  }

  // Mantener IDLE activo. listen() puede fallar si la sesion esta
  // degradada pero connected() sigue devolviendo true. En ese caso,
  // los sweeps y refreshes que estan mas abajo actuan como mecanismo
  // de recuperacion, asi que NO hacemos return aqui.
  bool listening = imap.listen();

  if (listening && imap.folderChanged()) {
    SelectedFolderInfo sFolder = imap.selectedFolder();
    IMAP_Polling_Status ps = sFolder.pollingStatus();

    Serial.println("[IDLE] Cambio detectado - tipo: " + String(ps.type));

    imap.stopListen();
    leerYProcesarCorreos();
  }

  // Barrido periodico de seguridad: recupera correos cuya notificacion
  // IDLE se haya perdido (p.ej. llegaron mientras se enviaba un SMTP).
  // Se ejecuta SIEMPRE, aunque listen() haya fallado, para garantizar
  // que el sistema se recupera ante una sesion IDLE degradada.
  if (millis() - lastSweepMs >= SWEEP_MS) {
    Serial.println("[SWEEP] Barrido periodico de bandeja...");
    imap.stopListen();
    leerYProcesarCorreos();
  }

  // Refresh de la conexion IDLE cada 28 minutos (limite IMAP ~29 min).
  // Tambien se ejecuta siempre para cubrir el caso de listen() fallido.
  if (millis() - lastIdleRefresh >= IDLE_REFRESH_MS) {
    Serial.println("[IDLE] Refrescando conexion (28 min)...");
    imap.stopListen();
    imap.closeSession();
    delay(1000);
    if (iniciarEscuchaIMAP()) {
      lastSweepMs = millis();
      Serial.println("[IDLE] Refresh completado, leyendo correos pendientes...");
      leerYProcesarCorreos();
    } else {
      Serial.println("[IDLE] ERROR: fallo al refrescar conexion, reintentando en el proximo ciclo");
    }
  }
}
