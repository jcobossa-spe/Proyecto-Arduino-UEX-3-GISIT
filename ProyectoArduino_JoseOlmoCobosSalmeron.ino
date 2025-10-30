#include <WiFi.h>
#include <WebServer.h>
#include "DHT.h"

// ==================== CONFIGURACIÓN DE RED ====================
const char* ssid = "ToldoTecho_WIFI";       // Nombre del WiFi
const char* password = "12345678";           // Contraseña
WebServer server(80);                        // Puerto del servidor

// ==================== DEFINICIÓN DE PINES ====================
const int pinFinToldo = 26;
const int pinFinTecho = 27;
const int pinLdr = 32;

#define pinDht 25
#define dhtTipo DHT11
DHT dht(pinDht, dhtTipo);

const int motorToldoIn1 = 21;
const int motorToldoIn2 = 19;
const int motorTechoIn1 = 18;
const int motorTechoIn2 = 5;
const int relHumidificador = 23;

//Definiciones para lógica de motores y relés
#define motorOn HIGH
#define motorOff LOW
#define relayOn LOW
#define relayOff HIGH

// ==================== VARIABLES DE CONFIGURACIÓN ====================
float umbralTempAlta = 30.0;
float umbralTempBaja = 10.0;
float umbralHumedad = 20.0;
float umbralLluvia = 95.0;
int ldrThresholdLowLight = 3000;
int ldrThresholdHighLight = 400;

// Intervalos de comprobación
const unsigned long intervaloAmbiental = 15 * 60 * 1000;  // Chequeo cada 15 min
const unsigned long intervaloDht = 3000;                   // Lectura DHT cada 3 seg

// ==================== ESTRUCTURA DEL MOTOR ====================
struct Motor {
  const char* nombre;             // Nombre (toldo o techo)
  int pinFinCarrera;
  int pinPoner;
  int pinQuitar;
  bool activo;
  int pinActivo;
  unsigned long inicioMovimiento; // Tiempo inicio movimiento
  int modo;                       // 1=desplegando, 2=retrayendo, 0=inactivo
  unsigned long tiempoDespliegue; // Tiempo medido
  int estadoFinCarrera;
  unsigned long ultimoCambio;     // Último cambio para debounce
  bool desplegado;                //Ultimo movimiento
};

// Inicialización de estructuras para toldo y techo
Motor toldo = {"TOLDO", pinFinToldo, motorToldoIn1, motorToldoIn2, false, -1, 0, 0, 5000, HIGH, 0, false};
Motor techo = {"TECHO", pinFinTecho, motorTechoIn1, motorTechoIn2, false, -1, 0, 0, 5000, HIGH, 0, false};

// ==================== VARIABLES GLOBALES ====================
unsigned long ultimoChequeoAmbiental = 0;
unsigned long ultimaLecturaDht = 0;
float temperaturaActual = NAN;
float humedadActual = NAN;
bool modoManualHumidificador = false;

// ==================== DECLARACIÓN DE FUNCIONES ====================
void handleRoot();                           // Maneja página principal
void handleGuardar();                        // Guarda nuevos umbrales
void handleDiagnostico();                    // Página de diagnóstico
void handleToggle();                         // Control manual elementos
void comprobarSensoresYActuar();             // Lógica principal
void leerDhtSiEsNecesario();                 // Lectura DHT
void iniciarMotorSeguro(Motor &motor, int modo); // Inicia movimiento motor
void actualizarEstadoMotor(Motor &motor);    // Actualiza estado motor
void actualizarDebounce(Motor &motor);       // Debounce
void configurarMotores();                    // Configuración inicial 

// ==================== SETUP ====================
void setup() {
  Serial.begin(9600);
  dht.begin();

  // Configuración de pines
  pinMode(pinLdr, INPUT);
  configurarMotores();
  pinMode(relHumidificador, OUTPUT);
  digitalWrite(relHumidificador, relayOff);

  // Inicialización WiFi
  WiFi.softAP(ssid, password);
  Serial.println("Punto de acceso WiFi creado.");
  Serial.println(WiFi.softAPIP());
  server.on("/", handleRoot);
  server.on("/guardar", handleGuardar);
  server.on("/diagnostico", handleDiagnostico);
  server.on("/toggle", handleToggle);

  server.begin();
  Serial.println("Servidor web iniciado.");
}

// ==================== FUNCIONES WEB ====================
void handleRoot() {
  // Página configuración
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>Control Toldo y Techo</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;margin:20px;}input{margin:5px;}</style></head><body>";
  html += "<h2>Configuración de Umbrales</h2>";
  html += "<form action='/guardar' method='get'>";
  html += "Humedad mínima (%): <input type='number' step='0.1' name='humedad' value='" + String(umbralHumedad) + "'><br>";
  html += "Temp máxima (°C): <input type='number' step='0.1' name='tmax' value='" + String(umbralTempAlta) + "'><br>";
  html += "Temp mínima (°C): <input type='number' step='0.1' name='tmin' value='" + String(umbralTempBaja) + "'><br>";
  html += "<button type='submit'>Guardar</button></form><br>";
  html += "<a href='/diagnostico'>Modo Diagnóstico</a>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleGuardar() {
  // Procesa nuevos valores de umbrales desde el formulario
  if (server.hasArg("humedad")) umbralHumedad = server.arg("humedad").toFloat();
  if (server.hasArg("tmax")) umbralTempAlta = server.arg("tmax").toFloat();
  if (server.hasArg("tmin")) umbralTempBaja = server.arg("tmin").toFloat();
  comprobarSensoresYActuar();  // Re-evalúa condiciones con nuevos umbrales
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDiagnostico() {
  // Página de diagnóstico
  leerDhtSiEsNecesario();
  int valorLdr = analogRead(pinLdr);
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta http-equiv='refresh' content='5'>";  // Auto-actualización cada 5s
  html += "<title>Diagnóstico</title><style>body{font-family:Arial;margin:20px;} table{border-collapse:collapse;} th,td{border:1px solid #aaa;padding:6px;}</style></head><body>";
  html += "<h2>Diagnóstico del Sistema</h2>";
  html += "<p>Temperatura: " + String(isnan(temperaturaActual) ? "N/A" : String(temperaturaActual)) + " °C</p>";
  html += "<p>Humedad: " + String(isnan(humedadActual) ? "N/A" : String(humedadActual)) + " %</p>";
  html += "<p>Luz (LDR): " + String(valorLdr) + "</p>";

  // Mostrar estado basado en último movimiento y tiempos de despliegue
  html += "<h3>Estado de Motores</h3>";
  html += "<table><tr><th>Elemento</th><th>Estado</th><th>Tiempo Despliegue (ms)</th></tr>";

  String estadoToldo = toldo.activo ? (toldo.modo == 1 ? "Desplegando" : "Recogiendo") : (toldo.desplegado ? "Desplegado" : "Recogido");
  String estadoTecho = techo.activo ? (techo.modo == 1 ? "Desplegando" : "Recogiendo") : (techo.desplegado ? "Desplegado" : "Recogido");

  html += "<tr><td>Toldo</td><td>" + estadoToldo + "</td><td>" + String(toldo.tiempoDespliegue) + "</td></tr>";
  html += "<tr><td>Techo</td><td>" + estadoTecho + "</td><td>" + String(techo.tiempoDespliegue) + "</td></tr>";
  html += "</table><br>";

  html += "<h3>Control Manual</h3>";
  html += "<form action='/toggle' method='get'>";
  bool humidificadorEncendido = (digitalRead(relHumidificador) == relayOn);
  html += "<button name='rele' value='toldo_put'>Poner Toldo</button>";
  html += "<button name='rele' value='toldo_remove'>Quitar Toldo</button>";
  html += "<button name='rele' value='techo_put'>Poner Techo</button>";
  html += "<button name='rele' value='techo_remove'>Quitar Techo</button>";
  html += "<button name='rele' value='humidificador'>" + String(humidificadorEncendido ? "Apagar Humidificador" : "Encender Humidificador") + "</button>";
  html += "</form><a href='/'>Volver</a></body></html>";
  server.send(200, "text/html", html);
}

void handleToggle() {
  // Acciones manuales
  if (server.hasArg("rele")) {
    String accion = server.arg("rele");
    // Acciones para toldo
    if (accion == "toldo_put" && !toldo.activo) iniciarMotorSeguro(toldo, 1);
    else if (accion == "toldo_remove" && !toldo.activo) iniciarMotorSeguro(toldo, 2);
    // Acciones para techo
    else if (accion == "techo_put" && !techo.activo) iniciarMotorSeguro(techo, 1);
    else if (accion == "techo_remove" && !techo.activo) iniciarMotorSeguro(techo, 2);
    // Control humidificador
    else if (accion == "humidificador") {
      bool encendido = digitalRead(relHumidificador) == relayOn;
      digitalWrite(relHumidificador, encendido ? relayOff : relayOn);
      modoManualHumidificador = !encendido;  // Alterna modo manual
    }
  }
  server.sendHeader("Location", "/diagnostico");
  server.send(303);
}

// ==================== LÓGICA DE CONTROL ====================
void comprobarSensoresYActuar() {
  leerDhtSiEsNecesario();
  int valorLdr = analogRead(pinLdr);

  // ---------------- Control automático del humidificador ----------------
  if (!isnan(humedadActual) && !modoManualHumidificador) {
    if (humedadActual < umbralHumedad) 
      digitalWrite(relHumidificador, relayOn);
    else 
      digitalWrite(relHumidificador, relayOff);
  }

  // ---------------- Detección de lluvia ----------------
  // Si humedad supera el umbral de lluvia, retrae toldo y techo (si están desplegados)
  if (!isnan(humedadActual) && humedadActual > umbralLluvia) {
    if (toldo.desplegado && !toldo.activo) iniciarMotorSeguro(toldo, 2);
    if (techo.desplegado && !techo.activo) iniciarMotorSeguro(techo, 2);
    return;  // Evita evaluar el resto de condiciones
  }

  // ---------------- Lógica de control para TECHO ----------------
  if (!techo.activo) {
    // Despliegue: hace frío y está oscuro
    if (valorLdr > ldrThresholdLowLight && temperaturaActual < umbralTempBaja && !techo.desplegado) {
      iniciarMotorSeguro(techo, 1);
    }
    // Retracción: deja de hacer frío o hay mucha luz
    else if ((temperaturaActual >= umbralTempBaja || valorLdr > ldrThresholdHighLight) && techo.desplegado) {
      iniciarMotorSeguro(techo, 2);
    }
  }

  // ---------------- Lógica de control para TOLDO ----------------
  if (!toldo.activo) {
    // Despliegue: hace calor y hay mucha luz
    if (valorLdr < ldrThresholdHighLight && temperaturaActual > umbralTempAlta && !toldo.desplegado) {
      iniciarMotorSeguro(toldo, 1);
    }
    // Retracción: deja de hacer calor o se oscurece
    else if ((temperaturaActual <= umbralTempAlta || valorLdr < ldrThresholdLowLight) && toldo.desplegado) {
      iniciarMotorSeguro(toldo, 2);
    }
  }
}

// ==================== SENSOR DHT ====================
void leerDhtSiEsNecesario() {
  unsigned long ahora = millis();
  if (ahora - ultimaLecturaDht < intervaloDht) return;
  ultimaLecturaDht = ahora;
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  
  // Actualiza valores sólo si son válidos
  if (!isnan(h)) humedadActual = h;
  if (!isnan(t)) temperaturaActual = t;
}

// ==================== CONTROL DE MOTORES ====================
void iniciarMotorSeguro(Motor &motor, int modo) {
  int pinActivo = (modo == 1) ? motor.pinPoner : motor.pinQuitar;
  int pinOpuesto = (modo == 1) ? motor.pinQuitar : motor.pinPoner;

  // Secuencia segura de activación
  digitalWrite(pinOpuesto, motorOff);  // Asegura pin opuesto apagado
  delay(50);                            // Breve pausa de seguridad
  digitalWrite(pinActivo, motorOn);    // Activa dirección deseada

  // Actualiza estado del motor
  motor.activo = true;
  motor.pinActivo = pinActivo;
  motor.inicioMovimiento = millis();
  motor.modo = modo;

  Serial.print(motor.nombre);
  Serial.print(": movimiento ");
  Serial.println(modo == 1 ? "despliegue" : "retracción");
}

void actualizarEstadoMotor(Motor &motor) {
  if (!motor.activo) return;  // Solo procesa motores activos
  
  unsigned long ahora = millis();
  unsigned long tiempo = ahora - motor.inicioMovimiento;

  if (motor.modo == 1) {  // Modo despliegue
    if (motor.estadoFinCarrera == LOW) {
      motor.tiempoDespliegue = tiempo;  // Guarda tiempo de referencia
      motor.desplegado = true;          // Actualiza estado lógico al completar despliegue
      digitalWrite(motor.pinActivo, motorOff);
      motor.activo = false;
      Serial.print(motor.nombre); Serial.println(": despliegue completado");
    }
    // Timeout por seguridad
    else if (tiempo >= (motor.tiempoDespliegue > 0 ? motor.tiempoDespliegue * 2 : 15000)) {
      motor.desplegado = true;          // Si timeout, asumimos desplegado
      digitalWrite(motor.pinActivo, motorOff);
      motor.activo = false;
      Serial.print(motor.nombre); Serial.println(": TIMEOUT en despliegue");
    }
  }
  else if (motor.modo == 2) {  // Modo retracción
    unsigned long tiempoEsperado = motor.tiempoDespliegue > 0 ? motor.tiempoDespliegue : 5000;
    if (tiempo >= tiempoEsperado) {
      motor.desplegado = false;         // Actualiza estado lógico al completar retracción
      digitalWrite(motor.pinActivo, motorOff);
      motor.activo = false;
      Serial.print(motor.nombre); Serial.println(": retracción completada");
    }
  }
}

// ==================== DEBOUNCE ====================
void actualizarDebounce(Motor &motor) {
  unsigned long ahora = millis();
  int lecturaActual = digitalRead(motor.pinFinCarrera);

  if (lecturaActual != motor.estadoFinCarrera) {
    // Confirma cambio después de 50ms (estable)
    if (ahora - motor.ultimoCambio >= 50) {
      motor.estadoFinCarrera = lecturaActual;
      Serial.print(motor.nombre);
      Serial.print(": fin de carrera = ");
      Serial.println(lecturaActual == LOW ? "LOW (presionado)" : "HIGH (libre)");
    }
  } else {
    motor.ultimoCambio = ahora;
  }
}

// ==================== CONFIGURACIÓN DE MOTORES ====================
void configurarMotores() {
  // Configuración inicial de pines para toldo
  pinMode(toldo.pinFinCarrera, INPUT_PULLUP);
  pinMode(toldo.pinPoner, OUTPUT);
  pinMode(toldo.pinQuitar, OUTPUT);
  digitalWrite(toldo.pinPoner, motorOff);
  digitalWrite(toldo.pinQuitar, motorOff);
  toldo.estadoFinCarrera = digitalRead(toldo.pinFinCarrera);
  // Inicializa estado lógico según fin de carrera actual (si está presionado = desplegado)
  toldo.desplegado = (toldo.estadoFinCarrera == LOW);

  // Configuración inicial de pines para techo
  pinMode(techo.pinFinCarrera, INPUT_PULLUP);
  pinMode(techo.pinPoner, OUTPUT);
  pinMode(techo.pinQuitar, OUTPUT);
  digitalWrite(techo.pinPoner, motorOff);
  digitalWrite(techo.pinQuitar, motorOff);
  techo.estadoFinCarrera = digitalRead(techo.pinFinCarrera);
  // Inicializa estado lógico según fin de carrera actual
  techo.desplegado = (techo.estadoFinCarrera == LOW);
}

// ==================== LOOP PRINCIPAL ====================
void loop() {
  server.handleClient();
  actualizarDebounce(toldo);
  actualizarDebounce(techo);
  actualizarEstadoMotor(toldo);
  actualizarEstadoMotor(techo);

  // Chequeo ambiental periódico
  unsigned long ahora = millis();
  if (ahora - ultimoChequeoAmbiental >= intervaloAmbiental) {
    ultimoChequeoAmbiental = ahora;
    comprobarSensoresYActuar();  // Ejecuta lógica de control automático
  }
}