# Monitor de temperatura ESP32 con alertas

Firmware para ESP32 que lee un sensor DHT11 y envía alertas de temperatura por Telegram y WhatsApp mediante CallMeBot.

## Características

- Lectura de temperatura con un DHT11 cada 2 segundos.
- Activación de alarma al alcanzar `27 °C` (configurable en `src/main.cpp`).
- Un aviso por canal en cada episodio de alarma; la alarma se rearma al bajar a `26 °C` o menos.
- Reintentos cada 15 segundos únicamente para los canales cuyo envío falló.
- El monitoreo continúa mientras el ESP32 intenta conectarse o reconectarse al Wi-Fi.

## Hardware

- ESP32 compatible con la placa DOIT ESP32 DEVKIT V1.
- Sensor DHT11 o módulo DHT11.
- Conecta `DATA` a GPIO `32`, `VCC` a `3V3` y `GND` a `GND`.
- Si usas el sensor sin módulo, instala una resistencia pull-up de 4.7 kΩ a 10 kΩ entre `DATA` y `3V3`. Algunos módulos ya la incluyen.

Comprueba el pinout de tu placa y sensor antes de alimentarlos. No apliques 5 V a un GPIO del ESP32.

## Requisitos

- Visual Studio Code con PlatformIO IDE, o PlatformIO Core.
- Una red Wi-Fi con acceso a Internet.
- Un bot creado con [BotFather](https://t.me/BotFather) y el ID del chat destinatario.
- Una cuenta y clave de API de CallMeBot para WhatsApp.

Las bibliotecas necesarias están declaradas en `platformio.ini`; PlatformIO las instala al compilar.

## Configuración de credenciales

El firmware se compila para el microcontrolador: no carga automáticamente variables desde un archivo `.env`. Para mantener las claves fuera de Git, usa un header local:

1. Copia `include/secrets.example.h` como `include/secrets.h`.
2. Edita `include/secrets.h` con el SSID y contraseña Wi-Fi, el token de Telegram, el ID del chat, el número internacional y la clave de CallMeBot.
3. No cambies los nombres de las variables: el firmware los utiliza directamente.

`include/secrets.h` está excluido en `.gitignore`; la plantilla `include/secrets.example.h` sí debe incluirse en GitHub. No pongas claves reales en la plantilla, en `README.md` ni en capturas de pantalla.

Si una clave ya se publicó o compartió, revócala y genera una nueva. Agregar un archivo a `.gitignore` no borra secretos que ya se hayan enviado a un repositorio remoto ni de su historial.

## Compilar y cargar

1. Abre la carpeta del proyecto en VS Code con PlatformIO.
2. Crea `include/secrets.h` y configura las credenciales.
3. Compila el entorno `esp32doit-devkit-v1`.
4. Conecta el ESP32 por USB y carga el firmware.
5. Abre el monitor serie a `115200` baudios.

También puedes ejecutar desde una terminal en la carpeta del proyecto:

```sh
pio run
pio run --target upload
pio device monitor --baud 115200
```

## Comportamiento de las alertas

Cuando la temperatura alcanza el umbral, se intenta enviar un mensaje a cada canal. Un envío HTTP aceptado se marca como completado para que las lecturas altas siguientes no repitan ese mensaje. Si falla un canal, solo ese canal se reintenta después del intervalo configurado. Al descender a `TEMP_RESET`, se rearma la alarma para un nuevo episodio.

Si el ESP32 no tiene conexión, sigue leyendo el sensor y no intenta enviar avisos hasta conectarse. El estado de conexión y los códigos HTTP se informan en el monitor serie.

## Seguridad

En esta versión los clientes HTTPS usan `setInsecure()`, que cifra la conexión pero no valida el certificado del servidor. Esto reduce la protección ante intermediarios; para un despliegue real configura la validación de certificados de las autoridades correspondientes.

Las credenciales compiladas dentro del firmware también pueden extraerse del dispositivo. No reutilices contraseñas ni tokens y regenera cualquier secreto que se haya expuesto.

## Publicar en GitHub

Antes de inicializar Git, confirma que `include/secrets.h` contiene tus credenciales locales y que `.gitignore` lo excluye. Después, desde una terminal en la carpeta del proyecto:

```sh
git init
git status --short
git check-ignore -v include/secrets.h
git add .
git status --short
git commit -m "Initial project version"
git branch -M main
git remote add origin https://github.com/USUARIO/REPOSITORIO.git
git push -u origin main
```

Crea primero un repositorio vacío en GitHub y reemplaza la URL de ejemplo por la dirección de ese repositorio. Antes de confirmar el commit, revisa `git status` y asegúrate de que `include/secrets.h` no aparezca entre los archivos preparados.
