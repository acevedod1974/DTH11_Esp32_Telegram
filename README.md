# Termómetro con ESP32

Este proyecto usa un **ESP32** y un sensor **DHT11** para medir la temperatura. Si llega a **27 °C**, intenta mandar un aviso por Telegram y WhatsApp.

## Que necesitas

- Una placa ESP32 compatible con **DOIT ESP32 DEVKIT V1**.
- Un sensor de temperatura DHT11.
- Un cable USB que sirva para conectar la placa al computador.
- VS Code con la extensión PlatformIO.
- Una red Wi-Fi con Internet.
- Ayuda de una persona adulta para conectar los cables y preparar las cuentas de mensajería.

## Conecta el sensor

Con la placa desconectada del USB, conecta los pines así:

| Sensor DHT11 | ESP32   |
| ------------ | ------- |
| DATA         | GPIO 32 |
| VCC          | 3V3     |
| GND          | GND     |

Si tu sensor no viene en un módulo, puede necesitar una resistencia entre DATA y 3V3. Pide ayuda para revisar esto. **No conectes 5V a los pines del ESP32.**

## Prepara las claves

El ESP32 necesita algunos datos para conectarse a tu Wi-Fi y mandar mensajes. No compartas estos datos: son como contraseñas.

1. En VS Code, abre la carpeta `include`.
2. Haz una copia de `secrets.example.h` y llama a la copia `secrets.h`.
3. Abre `secrets.h` y cambia el texto de ejemplo por tus datos, sin borrar las comillas.

Necesitas el nombre y la contraseña del Wi-Fi, los datos de un bot de Telegram (se crean con [BotFather](https://t.me/BotFather)) y el número y la clave de CallMeBot para WhatsApp. Escribe el número con el código de país.

**Importante:** por ahora el programa intenta usar Telegram y WhatsApp siempre. Aunque solo quieras WhatsApp, no borres las líneas `botToken` y `chatId`: sin ellas el proyecto no compila. Si dejas los datos de ejemplo, CallMeBot podría funcionar, pero el programa seguirá intentando Telegram y mostrará errores.

No compartas `secrets.h`, ni publiques tus contraseñas o claves en GitHub, capturas de pantalla o mensajes. El archivo `secrets.h` está excluido de Git, pero revisa siempre que no aparezca en los archivos que vas a compartir.

## Carga el programa

1. Abre esta carpeta del proyecto en VS Code.
2. Conecta el ESP32 al computador por USB.
3. En PlatformIO, busca `esp32doit-devkit-v1` y selecciona **Build** para comprobar que el programa esta listo.
4. Selecciona **Upload** para cargarlo en el ESP32.
5. Selecciona **Monitor** para ver lo que esta haciendo el proyecto. La velocidad del monitor es `115200`.

PlatformIO instala las bibliotecas necesarias durante la compilación. Si VS Code pregunta qué extensión instalar, selecciona PlatformIO IDE.

## Que hara

- El sensor mide la temperatura cada 2 segundos.
- Al llegar a 27 °C, el ESP32 intenta mandar un aviso por cada servicio.
- Si un envio falla, vuelve a intentarlo cada 15 segundos.
- Cuando la temperatura baja a 26 °C o menos, queda listo para una nueva alerta.
- Si no hay Wi-Fi, sigue midiendo y vuelve a intentar conectarse.

Para cambiar la temperatura que activa la alerta, busca `TEMP_LIMIT` en `src/main.cpp`.

## Si algo no funciona

- **No compila:** revisa que el archivo se llame exactamente `include/secrets.h` y que no hayas borrado ninguna línea.
- **No se conecta al Wi-Fi:** revisa el nombre y la contraseña. Distingue entre mayúsculas y minúsculas.
- **No llegan mensajes:** revisa que el ESP32 tenga Internet y que las claves de Telegram y CallMeBot sean correctas.
- **El sensor no muestra un numero:** revisa los cables y que DATA este conectado a GPIO 32.

Pide ayuda a una persona adulta antes de cambiar cables o compartir el proyecto por Internet.
