/**
 * ============================================================================
 * PROYECTO: Sistema de Monitoreo Térmico con Notificaciones Multiplataforma
 * PLATAFORMA: ESP32 (Framework Arduino / PlatformIO)
 * SERVICIOS: Telegram Bot API + CallMeBot (WhatsApp API)
 * PERIFÉRICOS:
 *   - Sensor: DHT11 (Temperatura / Humedad) vía GPIO 32
 *   - Pantalla Activa: OLED 0.96" 128x64 I2C (SSD1306) vía GPIO 21 (SDA) y GPIO 22 (SCL)
 *   - Pantalla Inactiva (Backup): LCD 1602 con PCF8574 (Código preservado en comentarios)
 * ============================================================================
 *
 * DESCRIPCIÓN:
 * Firmware para el monitoreo continuo de magnitudes termohigrométricas mediante
 * sensor DHT11 y despliegue gráfico local en pantalla OLED monocromática de 0.96"
 * (controlador SSD1306, 128x64 px). Incorpora máquina de estados para disparo
 * secuencial de alertas hacia Telegram y WhatsApp con histéresis de seguridad.
 *
 * Toda la arquitectura de temporización y refresco de pantalla opera bajo un
 * esquema no bloqueante con millis() para no interferir con las tareas del ESP32.
 * ============================================================================
 */

// Inclusión de librerías esenciales del core de ESP32 y periféricos
#include <Arduino.h>
#include <WiFi.h>             // Gestión del stack TCP/IP y conectividad Wi-Fi STA
#include <WiFiClientSecure.h> // Cliente TLS/SSL para conexiones cifradas HTTPS
#include <HTTPClient.h>       // Manejador simplificado de peticiones cliente HTTP/HTTPS
#include <UrlEncode.h>        // Codificación de caracteres especiales según RFC 3986
#include <DHT.h>              // Librería Adafruit para adquisición de datos DHT
#include <Wire.h>             // Driver del bus serie síncrono I2C (Two-Wire Interface)

// Librerías de la pantalla OLED activa (SSD1306)
#include <Adafruit_GFX.h>     // Primitivas gráficas del ecosistema Adafruit
#include <Adafruit_SSD1306.h> // Controlador de hardware para paneles OLED SSD1306

/* ============================================================================
   LIBRERÍA DESACTIVADA: PANTALLA LCD 1602 I2C
   ============================================================================ */
// #include <LiquidCrystal_I2C.h> // Controlador para pantallas LCD basadas en HD44780 + PCF8574

#include "secrets.h" // Credenciales de red (SSID/PASS) y tokens privados

// ============================================================================
// DEFINICIONES DE HARDWARE Y ASIGNACIÓN DE PINES (GPIO)
// ============================================================================
#define DHTPIN 32      // Pin GPIO asignado al bus digital 1-Wire del DHT11
#define DHTTYPE DHT11  // Transductor específico seleccionado
#define I2C_SDA_PIN 21 // Pin GPIO asignado a la línea de datos serie I2C
#define I2C_SCL_PIN 22 // Pin GPIO asignado a la línea de reloj serie I2C

// --- Configuración OLED Activa ---
#define SCREEN_WIDTH 128   // Ancho del panel OLED en píxeles
#define SCREEN_HEIGHT 64   // Alto del panel OLED en píxeles
#define OLED_RESET -1      // Reset compartido con el ESP32 (-1 indica que no hay pin dedicado)
#define OLED_I2C_ADDR 0x3C // Dirección I2C de 7 bits (0x78 en escritura de 8 bits equivale a 0x3C)

/* --- Configuración LCD 1602 Desactivada ---
#define LCD_I2C_ADDR 0x27 // Dirección física del módulo expansor PCF8574
#define LCD_COLUMNS 16    // Capacidad horizontal del display (caracteres)
#define LCD_ROWS 2        // Capacidad vertical del display (líneas)
*/

// ============================================================================
// PARÁMETROS OPERATIVOS Y CONTROL DE TIEMPOS (MILLIS)
// ============================================================================
#define TEMP_LIMIT 27               // Umbral superior de temperatura en °C para disparo de alarma
#define TEMP_RESET (TEMP_LIMIT - 1) // Umbral inferior con histéresis de 1 °C para rearme
#define COOLDOWN_MS 15000           // Ventana de enfriamiento entre reintentos de notificación remota
#define WIFI_RETRY_MS 10000         // Intervalo de comprobación e intento de reconexión Wi-Fi
#define SENSOR_INTERVAL_MS 2000     // Período de muestreo del sensor DHT11 (físicamente >= 1-2 s)
#define DISPLAY_INTERVAL_MS 1000    // Cadencia de refresco visual en la pantalla OLED

// ============================================================================
// PARÁMETROS DEL SERVICIO TELEGRAM BOT API
// ============================================================================
const char *telegramHost = "api.telegram.org"; // Host del API REST oficial de Telegram
const int httpsPort = 443;                     // Puerto estándar para transporte seguro HTTPS

// ============================================================================
// INSTANCIACIÓN DE OBJETOS GLOBALES Y VARIABLES DE ESTADO
// ============================================================================
DHT dht(DHTPIN, DHTTYPE);                                                 // Instancia del transductor DHT11
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET); // Instancia del panel OLED SSD1306
WiFiClientSecure secureClient;                                            // Cliente TLS/SSL reutilizable para peticiones seguras

/* --- Instancia LCD 1602 Desactivada ---
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLUMNS, LCD_ROWS);
*/

// Timers para la gestión no bloqueante de tareas
unsigned long lastWifiAttempt = 0;   // Marca de tiempo del último intento de enlace Wi-Fi
unsigned long lastAlertAttempt = 0;  // Marca de tiempo del último despacho de notificación remota
unsigned long lastSensorRead = 0;    // Marca de tiempo de la última adquisición del sensor
unsigned long lastDisplayUpdate = 0; // Marca de tiempo de la última actualización gráfica

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
 * @param temperature Valor numérico de temperatura reportado.
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

  // Formateo del URI y serialización URL-encoded para los parámetros
  String url = "https://" + String(telegramHost) + "/bot" + botToken + "/sendMessage";
  String payload = "chat_id=" + String(chatId) + "&text=" + urlEncode(message) + "&parse_mode=Markdown";

  HTTPClient http;
  http.setTimeout(5000); // Límite de espera de 5 segundos

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
 * @brief Renderiza el cuadro de telemetría y estado en la pantalla OLED de 128x64 píxeles.
 *
 * Distribución del lienzo:
 *   - Encabezado: Barra superior con estado Wi-Fi y Alarma.
 *   - Zona central: Lectura destacada de Temperatura y Humedad.
 *   - Pie de pantalla: IP asignada o indicador de desconexión.
 *
 * @param temp Valor instantáneo de temperatura (°C).
 * @param hum Valor instantáneo de humedad relativa (%).
 */
void updateOLED(float temp, float hum)
{
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // --- 1. Encabezado de Estado (Y: 0 a 10) ---
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (WiFi.status() == WL_CONNECTED)
  {
    display.print("WiFi: OK");
  }
  else
  {
    display.print("WiFi: --");
  }

  // Indicador de Alarma alineado a la derecha
  if (alarmActive)
  {
    display.setCursor(76, 0);
    display.print("[ALERTA]");
  }
  else
  {
    display.setCursor(82, 0);
    display.print("[NORM]");
  }

  // Línea divisoria horizontal decorativa
  display.drawFastHLine(0, 11, SCREEN_WIDTH, SSD1306_WHITE);

  // --- 2. Despliegue de Variables (Y: 16 a 44) ---
  if (isnan(temp) || isnan(hum))
  {
    display.setTextSize(1);
    display.setCursor(18, 25);
    display.print("ERROR EN SENSOR");
  }
  else
  {
    // Temperatura en fuente aumentada
    display.setTextSize(2);
    display.setCursor(4, 18);
    display.printf("%4.1f C", temp);

    // Humedad relativa
    display.setTextSize(1);
    display.setCursor(6, 38);
    display.printf("Humedad: %.0f %%", hum);
  }

  // Línea divisoria inferior
  display.drawFastHLine(0, 50, SCREEN_WIDTH, SSD1306_WHITE);

  // --- 3. Barra de Información Inferior (Y: 54) ---
  display.setTextSize(1);
  display.setCursor(0, 54);
  if (WiFi.status() == WL_CONNECTED)
  {
    display.print(WiFi.localIP().toString());
  }
  else
  {
    display.print("Sin IP asignada");
  }

  // Transferencia de memoria de video (framebuffer) al controlador SSD1306 vía I2C
  display.display();
}

/* ============================================================================
   FUNCIÓN DESACTIVADA: ACTUALIZACIÓN DE PANTALLA LCD 1602 I2C
   ============================================================================
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
    snprintf(line0, sizeof(line0), "T:%4.1fC  H:%3.0f%%", temp, hum);
  }

  // Línea 1: Despliegue de estado de conectividad e indicación de alarma
  const char *wifiState = (WiFi.status() == WL_CONNECTED) ? "NET:OK" : "NET:--";
  const char *alarmState = alarmActive ? "ALARM!" : "NORM  ";
  snprintf(line1, sizeof(line1), "%-6s  %-7s", wifiState, alarmState);

  lcd.setCursor(0, 0);
  lcd.print(line0);
  lcd.setCursor(0, 1);
  lcd.print(line1);
}
============================================================================ */

// ============================================================================
// CONFIGURACIÓN INICIAL DEL SISTEMA (SETUP)
// ============================================================================
void setup()
{
  // Inicialización de la consola serie para trazabilidad y depuración de eventos
  Serial.begin(115200);

  // Configuración del bus serie I2C
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Inicialización del panel OLED SSD1306 con bomba de carga interna habilitada (SSD1306_SWITCHCAPVCC)
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR))
  {
    Serial.println("[ERROR] No se detectó la pantalla OLED SSD1306 en la dirección I2C configurada.");
  }
  else
  {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(10, 20);
    display.print("Iniciando nodo...");
    display.setCursor(10, 36);
    display.print("Sensor DHT11 / WiFi");
    display.display();
  }

  /* --- Inicialización de pantalla LCD 1602 desactivada ---
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Iniciando nodo..");
  */

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
  // TAREA 3: ACTUALIZACIÓN PERIÓDICA DEL DISPLAY OLED (128x64)
  // --------------------------------------------------------------------------
  if (now - lastDisplayUpdate >= DISPLAY_INTERVAL_MS)
  {
    lastDisplayUpdate = now;
    updateOLED(currentTemp, currentHumidity);

    /* --- Llamado LCD 1602 Desactivado ---
    updateLCD(currentTemp, currentHumidity);
    */
  }
}