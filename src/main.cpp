/**
 * ============================================================================
 * PROYECTO: Sistema de Monitoreo Térmico con Notificaciones Multiplataforma
 * PLATAFORMA: ESP32 (Framework Arduino / PlatformIO)
 * SERVICIOS: Telegram Bot API + CallMeBot (WhatsApp API)
 * PERIFÉRICOS:
 *   - Sensor: DHT11 (Temperatura / Humedad) vía GPIO 32
 *   - Pantalla: LCD 1602 con expansor I2C (PCF8574) vía GPIO 21 (SDA) y GPIO 22 (SCL)
 * ============================================================================
 *
 * DESCRIPCIÓN:
 * Firmware para el monitoreo continuo de variables termohigrométricas mediante
 * sensor DHT11 y despliegue local en pantalla LCD 1602 I2C. Incorpora una lógica
 * de disparo de alarmas ante sobretemperatura (TEMP_LIMIT) con histéresis de
 * apagado/rearme (TEMP_RESET). Las notificaciones remotas se transmiten de forma
 * secuencial a través de la API de Bots de Telegram y el gateway CallMeBot (WhatsApp).
 *
 * Para prevenir retardos en la respuesta del bus I2C y saturación de las APIs
 * remotas, el bucle principal opera bajo una arquitectura de temporización
 * concurrente y no bloqueante basada exclusivamente en la función millis().
 * ============================================================================
 */

// Inclusión de librerías esenciales del core de ESP32 y periféricos
#include <Arduino.h>
#include <WiFi.h>              // Gestión del stack TCP/IP y conectividad Wi-Fi STA
#include <WiFiClientSecure.h>  // Cliente TLS/SSL para conexiones cifradas HTTPS
#include <HTTPClient.h>        // Manejador simplificado de peticiones cliente HTTP/HTTPS
#include <UrlEncode.h>         // Codificación de caracteres especiales según RFC 3986
#include <DHT.h>               // Librería Adafruit para adquisición de datos DHT
#include <Wire.h>              // Driver del bus serie síncrono I2C (Two-Wire Interface)
#include <LiquidCrystal_I2C.h> // Controlador para pantallas LCD basadas en HD44780 + PCF8574
#include "secrets.h"           // Credenciales de red (SSID/PASS) y tokens privados

// ============================================================================
// DEFINICIONES DE HARDWARE Y ASIGNACIÓN DE PINES (GPIO)
// ============================================================================
#define DHTPIN 32         // Pin GPIO asignado al bus digital 1-Wire del DHT11
#define DHTTYPE DHT11     // Transductor específico seleccionado
#define I2C_SDA_PIN 21    // Pin GPIO asignado a la línea de datos serie I2C
#define I2C_SCL_PIN 22    // Pin GPIO asignado a la línea de reloj serie I2C
#define LCD_I2C_ADDR 0x27 // Dirección física del módulo expansor PCF8574 (típicamente 0x27 o 0x3F)
#define LCD_COLUMNS 16    // Capacidad horizontal del display (caracteres)
#define LCD_ROWS 2        // Capacidad vertical del display (líneas)

// ============================================================================
// PARÁMETROS OPERATIVOS Y CONTROL DE TIEMPOS (MILLIS)
// ============================================================================
#define TEMP_LIMIT 27               // Umbral superior de temperatura en °C para disparo de alarma
#define TEMP_RESET (TEMP_LIMIT - 1) // Umbral inferior con histéresis de 1 °C para rearme
#define COOLDOWN_MS 15000           // Ventana de enfriamiento entre reintentos de notificación remota
#define WIFI_RETRY_MS 10000         // Intervalo de comprobación e intento de reconexión Wi-Fi
#define SENSOR_INTERVAL_MS 2000     // Período de muestreo del sensor DHT11 (limitación física: >= 1-2 s)
#define LCD_INTERVAL_MS 1000        // Cadencia de refresco del búfer de la pantalla LCD

// ============================================================================
// PARÁMETROS DEL SERVICIO TELEGRAM BOT API
// ============================================================================
const char *telegramHost = "api.telegram.org"; // Host del API REST oficial de Telegram
const int httpsPort = 443;                     // Puerto estándar para transporte seguro HTTPS

// ============================================================================
// INSTANCIACIÓN DE OBJETOS GLOBALES Y VARIABLES DE ESTADO
// ============================================================================
DHT dht(DHTPIN, DHTTYPE);                                   // Instancia del transductor DHT11
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLUMNS, LCD_ROWS); // Instancia del display LCD 1602 vía I2C
WiFiClientSecure secureClient;                              // Cliente TLS/SSL reutilizable para peticiones seguras

// Timers para la gestión no bloqueante de tareas
unsigned long lastWifiAttempt = 0;  // Marca de tiempo del último intento de enlace Wi-Fi
unsigned long lastAlertAttempt = 0; // Marca de tiempo del último despacho de notificación remota
unsigned long lastSensorRead = 0;   // Marca de tiempo de la última lectura analógica/digital del DHT
unsigned long lastLcdUpdate = 0;    // Marca de tiempo de la última actualización del frame LCD

// Banderas de control de estado
bool wifiWasConnected = false;  // Registro de estado para detección de flancos en la conexión
bool hasAlertAttempted = false; // Bandera indicativa de al menos un intento de despacho
bool alarmActive = false;       // Bandera de enclavamiento del estado de alarma térmica
bool telegramAlertSent = false; // Bandera de acuse de recibo de la alerta de Telegram
bool whatsAppAlertSent = false; // Bandera de acuse de recibo de la alerta de WhatsApp

// Variables para almacenamiento en memoria de la última telemetría válida
float currentTemp = NAN;     // Última temperatura adquirida (°C)
float currentHumidity = NAN; // Última humedad relativa adquirida (%RH)

/**
 * @brief Transmite un mensaje con formato Markdown hacia la API de Telegram y procesa el código de respuesta HTTP.
 *
 * @param temperature Valor numérico de temperatura que será interpolado en la cadena de notificación.
 * @return true Si el servidor remoto respondió con código HTTP 200 (OK).
 * @return false En caso de falla de socket, timeout o código HTTP divergente de 200.
 */
bool sendTelegramAlert(float temperature)
{
  Serial.println("[Telegram] Iniciando despacho de notificación...");

  // Construcción de la carga útil del mensaje
  String message = "⚠️ *¡Alerta de Temperatura!*\n";
  message += "Dispositivo: Nodo Sala\n";
  message += "Valor Medido: " + String(temperature, 1) + "°C\n";
  message += "Límite Excedido: " + String(TEMP_LIMIT) + "°C\n";
  message += "Ubicación: Sala";

  // Formateo del URI y serialización URL-encoded para los parámetros GET/POST
  String url = "https://" + String(telegramHost) + "/bot" + botToken + "/sendMessage";
  String payload = "chat_id=" + String(chatId) + "&text=" + urlEncode(message) + "&parse_mode=Markdown";

  HTTPClient http;
  http.setTimeout(5000); // Límite de espera de 5 segundos para prevenir bloqueos de pila

  if (!http.begin(secureClient, url))
  {
    Serial.println("[Telegram] ERROR: No fue posible configurar el canal seguro con el host.");
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpCode = http.POST(payload);
  bool success = (httpCode == HTTP_CODE_OK);

  if (success)
  {
    Serial.println("[Telegram] Mensaje procesado exitosamente por la plataforma (HTTP 200).");
  }
  else
  {
    Serial.printf("[Telegram] Falla en la solicitud POST. Código de estado devuelto: %d\n", httpCode);
  }

  http.end(); // Liberación explícita de buffers y sockets asociados
  return success;
}

/**
 * @brief Envía una petición HTTPS GET al endpoint de CallMeBot para su posterior enrutamiento a WhatsApp.
 *
 * @param temperature Lectura térmica actual para su reporte en el cuerpo del mensaje.
 * @return true Si la pasarela remota aceptó la transacción devolviendo HTTP 200.
 * @return false Ante excepciones de conexión o denegación por parte de la pasarela.
 */
bool sendWhatsAppAlert(float temperature)
{
  Serial.println("[WhatsApp] Preparando invocación hacia la pasarela CallMeBot...");

  // Formulación del texto base
  String message = "⚠️ ¡Alerta de Temperatura!\n";
  message += "Dispositivo: Nodo Sala\n";
  message += "Valor: " + String(temperature, 1) + "°C\n";
  message += "Límite Excedido: " + String(TEMP_LIMIT) + "°C";

  // Estructuración de la URI codificada
  String url = "https://api.callmebot.com/whatsapp.php?phone=" + phoneNumber +
               "&text=" + urlEncode(message) +
               "&apikey=" + apiKey;

  HTTPClient http;
  WiFiClientSecure client;
  http.setTimeout(5000);

  // Desactivación de validación CA para optimización de memoria RAM y velocidad de enlace
  client.setInsecure();

  if (http.begin(client, url))
  {
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK || httpCode == 200)
    {
      Serial.println("[WhatsApp] Notificación recibida y aceptada por el gateway.");
      http.end();
      return true;
    }
    else
    {
      Serial.printf("[WhatsApp] Error en la transacción HTTP GET. Código: %d\n", httpCode);
    }

    http.end();
  }
  else
  {
    Serial.println("[WhatsApp] ERROR: No fue posible aperturar el socket hacia CallMeBot.");
  }

  return false;
}

/**
 * @brief Actualiza las líneas del display LCD 1602 con formato estructurado de ancho fijo.
 *
 * Se emplea snprintf con búferes estáticos de 17 bytes (16 caracteres + null terminator)
 * para asegurar que las posiciones previas se sobreescriban sin requerir llamadas costosas
 * a lcd.clear(), evitando el parpadeo del panel óptico.
 *
 * @param temp Valor instantáneo de temperatura (°C).
 * @param hum Valor instantáneo de humedad relativa (%).
 */
void updateLCD(float temp, float hum)
{
  char line0[17];
  char line1[17];

  // Línea 0: Despliegue de variables de proceso
  if (isnan(temp) || isnan(hum))
  {
    snprintf(line0, sizeof(line0), "Sensor: ERROR   ");
  }
  else
  {
    // Formato con punto decimal y longitud fija para estabilizar la posición visual
    snprintf(line0, sizeof(line0), "T:%4.1fC  H:%3.0f%%", temp, hum);
  }

  // Línea 1: Despliegue de estado de conectividad e indicación de alarma
  const char *wifiState = (WiFi.status() == WL_CONNECTED) ? "NET:OK" : "NET:--";
  const char *alarmState = alarmActive ? "ALARM!" : "NORM  ";
  snprintf(line1, sizeof(line1), "%-6s  %-7s", wifiState, alarmState);

  // Escritura sobre la memoria DDRAM del controlador HD44780
  lcd.setCursor(0, 0);
  lcd.print(line0);
  lcd.setCursor(0, 1);
  lcd.print(line1);
}

// ============================================================================
// CONFIGURACIÓN INICIAL DEL SISTEMA (SETUP)
// ============================================================================
void setup()
{
  // Inicialización de la consola serie para trazabilidad y depuración de eventos
  Serial.begin(115200);

  // Configuración del bus serie I2C e inicialización de la pantalla alfanumérica
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando nodo..");

  // Arranque del circuito de muestreo del DHT11
  dht.begin();

  // Inicialización del subsistema Wi-Fi en modo Estación (STA)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiAttempt = millis();
  Serial.println("Conexión Wi-Fi iniciada; el monitoreo local continuará de forma concurrente.");

  // Exención de verificación criptográfica de certificados en el cliente global de Telegram
  secureClient.setInsecure();

  // Breve retardo inicial de establecimiento eléctrico y estabilización del sensor
  delay(1000);
}

// ============================================================================
// CICLO PRINCIPAL DE EJECUCIÓN (LOOP)
// ============================================================================
void loop()
{
  // Captura del tiempo transcurrido desde el reinicio del microcontrolador
  unsigned long now = millis();

  // --------------------------------------------------------------------------
  // TAREA 1: MÁQUINA DE ESTADOS Y CONTROL DE CONECTIVIDAD WI-FI (NO BLOQUEANTE)
  // --------------------------------------------------------------------------
  if (WiFi.status() == WL_CONNECTED)
  {
    if (!wifiWasConnected)
    {
      Serial.println("--- Conexión Wi-Fi establecida ---");
      Serial.print("Dirección IP asignada por DHCP: ");
      Serial.println(WiFi.localIP());
    }
    wifiWasConnected = true;
  }
  else
  {
    wifiWasConnected = false;
    // Si la interfaz pierde enlace, reintenta respetando el intervalo de guarda
    if (now - lastWifiAttempt >= WIFI_RETRY_MS)
    {
      Serial.println("Wi-Fi desconectado; solicitando reconexión a la capa de red...");
      WiFi.reconnect();
      lastWifiAttempt = now;
    }
  }

  // --------------------------------------------------------------------------
  // TAREA 2: ADQUISICIÓN DE DATOS Y GESTIÓN DE ALARMAS TÉRMICAS
  // --------------------------------------------------------------------------
  if (now - lastSensorRead >= SENSOR_INTERVAL_MS)
  {
    lastSensorRead = now;

    // Lectura de magnitudes físicas
    currentTemp = dht.readTemperature();
    currentHumidity = dht.readHumidity();

    if (!isnan(currentTemp))
    {
      // Telemetría hacia consola serie
      Serial.printf("Telemetría -> T: %.1f °C | H: %.1f %%\n", currentTemp, currentHumidity);

      // Comprobación de histéresis: si baja por debajo de TEMP_RESET, se reactiva la capacidad de alerta
      if (currentTemp <= TEMP_RESET && alarmActive)
      {
        Serial.println("Temperatura normalizada por debajo de la histéresis. Alarma rearmada.");
        alarmActive = false;
        telegramAlertSent = false;
        whatsAppAlertSent = false;
        hasAlertAttempted = false;
      }

      // Evaluación del umbral crítico superior
      if (currentTemp >= TEMP_LIMIT)
      {
        if (!alarmActive)
        {
          Serial.println("\n[ALERTA] Umbral crítico alcanzado. Activando secuencia de despacho.");
          alarmActive = true;
        }

        // Validación de condiciones de reintento para el envío de paquetes remotos
        bool retryReady = !hasAlertAttempted || (now - lastAlertAttempt >= COOLDOWN_MS);

        if (WiFi.status() == WL_CONNECTED && retryReady && (!telegramAlertSent || !whatsAppAlertSent))
        {
          // Despacho secuencial con validación de banderas independientes
          if (!telegramAlertSent)
          {
            telegramAlertSent = sendTelegramAlert(currentTemp);
          }
          if (!whatsAppAlertSent)
          {
            whatsAppAlertSent = sendWhatsAppAlert(currentTemp);
          }

          lastAlertAttempt = millis();
          hasAlertAttempted = true;
        }
      }
    }
    else
    {
      Serial.println("[ERROR] Falla de suma de comprobación o desconexión en el bus del DHT11.");
    }
  }

  // --------------------------------------------------------------------------
  // TAREA 3: ACTUALIZACIÓN PERIÓDICA DEL DISPLAY LCD I2C
  // --------------------------------------------------------------------------
  if (now - lastLcdUpdate >= LCD_INTERVAL_MS)
  {
    lastLcdUpdate = now;
    updateLCD(currentTemp, currentHumidity);
  }
}