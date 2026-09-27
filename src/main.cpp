/**
 * ============================================================================
 * PROYECTO: Sistema de Monitoreo Térmico con Notificaciones Multiplataforma
 * PLATAFORMA: ESP32 (Framework Arduino / PlatformIO)
 * SERVICIOS: Telegram Bot API + CallMeBot (WhatsApp API)
 * SENSOR: DHT11 (Temperatura / Humedad)
 * ============================================================================
 *
 * DESCRIPCIÓN:
 * Este firmware lee periódicamente un sensor de temperatura DHT11. Si la
 * temperatura sobrepasa el umbral definido (TEMP_LIMIT), dispara de forma
 * secuencial alertas a un chat de Telegram y a un número de WhatsApp vía
 * el gateway de CallMeBot.
 *
 * Para evitar saturar las APIs de mensajería (rate limits), implementa una
 * ventana de enfriamiento (COOLDOWN_MS) basada en millis().
 * ============================================================================
 */

// Inclusión de librerías esenciales del core de ESP32 y periféricos
#include <Arduino.h>
#include <WiFi.h>             // Gestión de la conexión Wi-Fi (modo estación)
#include <WiFiClientSecure.h> // Cliente TCP/IP con soporte de capa TLS/SSL (HTTPS)
#include <HTTPClient.h>       // Wrapper de alto nivel para peticiones HTTP/HTTPS
#include <UrlEncode.h>        // Codificación de caracteres especiales (espacios, acentos) para URI
#include <DHT.h>              // Librería Adafruit para sensores de la familia DHT
#include "secrets.h"          // Credenciales locales excluidas del repositorio

// ============================================================================
// DEFINICIONES DE HARDWARE Y PARÁMETROS DE OPERACIÓN
// ============================================================================
#define DHTPIN 32                   // Pin GPIO del ESP32 conectado a la línea DATA del DHT11
#define DHTTYPE DHT11               // Modelo específico del sensor (DHT11, DHT22, etc.)
#define TEMP_LIMIT 27               // Umbral de temperatura en °C a partir del cual se dispara la alarma
#define TEMP_RESET (TEMP_LIMIT - 1) // La alarma se rearma al bajar 1 °C bajo el umbral
#define COOLDOWN_MS 15000           // Tiempo mínimo entre reintentos de notificaciones fallidas
#define WIFI_RETRY_MS 10000

// ============================================================================
// PARÁMETROS DEL SERVICIO TELEGRAM BOT API
// ============================================================================
// Dominio del endpoint oficial de Telegram y puerto estándar HTTPS
const char *telegramHost = "api.telegram.org";
const int httpsPort = 443;

// ============================================================================
// INSTANCIACIÓN DE OBJETOS GLOBALES
// ============================================================================
DHT dht(DHTPIN, DHTTYPE);      // Instancia para el control del sensor DHT
WiFiClientSecure secureClient; // Instancia de cliente TLS/SSL reutilizable para Telegram
unsigned long lastWifiAttempt = 0;
unsigned long lastAlertAttempt = 0;
bool wifiWasConnected = false;
bool hasAlertAttempted = false;
bool alarmActive = false;
bool telegramAlertSent = false;
bool whatsAppAlertSent = false;

/**
 * @brief Envía un mensaje con formato Markdown a Telegram y valida el estado HTTP.
 *
 * @param temperature Valor de la temperatura medida que se reportará en el cuerpo del mensaje.
 */
bool sendTelegramAlert(float temperature)
{
  Serial.println("[Telegram] Enviando alerta...");

  String message = "⚠️ *¡Alerta de Temperatura!*\n";
  message += "Dispositivo: Nodo Sala\n";
  message += "Valor Medido: " + String(temperature, 1) + "°C\n";
  message += "Límite Excedido: " + String(TEMP_LIMIT) + "°C\n";
  message += "Ubicación: Sala";

  String url = "https://" + String(telegramHost) + "/bot" + botToken + "/sendMessage";
  String payload = "chat_id=" + String(chatId) + "&text=" + urlEncode(message) + "&parse_mode=Markdown";
  HTTPClient http;
  http.setTimeout(5000);

  if (!http.begin(secureClient, url))
  {
    Serial.println("[Telegram] ERROR: No fue posible inicializar la petición.");
    return false;
  }

  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int httpCode = http.POST(payload);
  bool success = httpCode == HTTP_CODE_OK;

  if (success)
  {
    Serial.println("[Telegram] Telegram aceptó el mensaje (HTTP 200).");
  }
  else
  {
    Serial.printf("[Telegram] Error en la petición. Código HTTP: %d\n", httpCode);
  }

  http.end();
  return success;
}

/**
 * @brief Envía un mensaje a WhatsApp utilizando la API GET de CallMeBot.
 *
 * Genera la URL con los parámetros codificados (número, API Key y texto),
 * y despacha una solicitud GET HTTPS a través de la librería HTTPClient.
 *
 * @param temperature Valor de la temperatura medida a reportar.
 */
bool sendWhatsAppAlert(float temperature)
{
  Serial.println("[WhatsApp] Preparando envío vía CallMeBot...");

  // 1. Construcción del texto plano para WhatsApp
  String message = "⚠️ ¡Alerta de Temperatura!\n";
  message += "Dispositivo: Nodo Sala\n";
  message += "Valor: " + String(temperature, 1) + "°C\n";
  message += "Límite Excedido: " + String(TEMP_LIMIT) + "°C";

  // 2. Construcción del endpoint de la API con los parámetros codificados
  // La función urlEncode transforma espacios en '+', saltos de línea en '%0A', etc.
  String url = "https://api.callmebot.com/whatsapp.php?phone=" + phoneNumber +
               "&text=" + urlEncode(message) +
               "&apikey=" + apiKey;

  // 3. Inicialización del cliente HTTP con soporte SSL
  HTTPClient http;
  WiFiClientSecure client;
  http.setTimeout(5000);

  // Deshabilita la verificación de certificados de la CA para no requerir almacenar el certificado raíz
  client.setInsecure();

  // 4. Apertura del túnel y ejecución del método HTTP GET
  if (http.begin(client, url))
  {
    int httpCode = http.GET();

    // Comprobación del código de estado HTTP recibido (200 = éxito)
    if (httpCode == HTTP_CODE_OK || httpCode == 200)
    {
      Serial.println("[WhatsApp] Mensaje recibido y procesado por CallMeBot.");
      http.end();
      return true;
    }
    else
    {
      // Códigos comunes: 400 (parámetros inválidos), 429 (rate limit / exceso de peticiones)
      Serial.printf("[WhatsApp] Error en la petición. Código HTTP: %d\n", httpCode);
    }

    // Libera recursos internos del objeto HTTPClient
    http.end();
  }
  else
  {
    Serial.println("[WhatsApp] ERROR: No fue posible inicializar la conexión con el servidor.");
  }

  return false;
}

// ============================================================================
// INICIALIZACIÓN DEL SISTEMA (SETUP)
// ============================================================================
void setup()
{
  // Inicialización del puerto serial para depuración
  Serial.begin(115200);

  // Inicialización del sensor DHT11
  dht.begin();

  // Configuración y arranque de la interfaz Wi-Fi en modo Station (cliente)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  lastWifiAttempt = millis();
  Serial.println("Conexión Wi-Fi iniciada; el monitoreo continuará mientras se conecta.");

  // Omitir validación criptográfica de certificados de CA en el cliente de Telegram
  // (Acelera la negociación TLS y evita desincronizaciones de reloj NTP en el microcontrolador)
  secureClient.setInsecure();
}

// ============================================================================
// BUCLE PRINCIPAL (LOOP)
// ============================================================================
void loop()
{
  unsigned long now = millis();

  if (WiFi.status() == WL_CONNECTED)
  {
    if (!wifiWasConnected)
    {
      Serial.println("--- Conexión Wi-Fi establecida ---");
      Serial.print("Dirección IP asignada: ");
      Serial.println(WiFi.localIP());
    }
    wifiWasConnected = true;
  }
  else
  {
    wifiWasConnected = false;
    if (now - lastWifiAttempt >= WIFI_RETRY_MS)
    {
      Serial.println("Wi-Fi desconectado; intentando reconectar...");
      WiFi.reconnect();
      lastWifiAttempt = now;
    }
  }

  // Lectura de la temperatura en grados Celsius desde el DHT11
  float temperature = dht.readTemperature();

  // Impresión de telemetría por el monitor serie
  Serial.print("Temperatura actual: ");
  Serial.print(temperature);
  Serial.println(" °C");

  if (!isnan(temperature) && temperature <= TEMP_RESET && alarmActive)
  {
    Serial.println("Temperatura normalizada; alarma rearmada.");
    alarmActive = false;
    telegramAlertSent = false;
    whatsAppAlertSent = false;
    hasAlertAttempted = false;
  }

  if (!isnan(temperature) && temperature >= TEMP_LIMIT)
  {
    if (!alarmActive)
    {
      Serial.println("\n[ALERTA] ¡Condición de umbral crítico alcanzada!");
      alarmActive = true;
    }

    bool retryReady = !hasAlertAttempted || now - lastAlertAttempt >= COOLDOWN_MS;
    if (WiFi.status() == WL_CONNECTED && retryReady && (!telegramAlertSent || !whatsAppAlertSent))
    {
      if (!telegramAlertSent)
      {
        telegramAlertSent = sendTelegramAlert(temperature);
      }
      if (!whatsAppAlertSent)
      {
        whatsAppAlertSent = sendWhatsAppAlert(temperature);
      }

      lastAlertAttempt = millis();
      hasAlertAttempted = true;
    }
  }

  // Intervalo de muestreo del DHT11 (el sensor DHT11 requiere al menos 1 a 2 segundos entre lecturas)
  delay(2000);
}