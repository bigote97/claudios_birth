# Claudio's Birth

Un compañero de escritorio en un ESP32-C3. Cuando Cursor o Claude necesitan una aprobación, terminan una tarea o se traban, el aparato suena, pone cara y lo escribe en una pantalla OLED. En reposo es un reloj con el clima y quién está trabajando.

No hace falta anotar la IP. El ESP32 entra al Wi-Fi y se anuncia como **Claudio's Birth de {tu nombre}**. Un daemon en la PC, la laptop o la Mac del dueño lo reconoce en la red y le entrega los avisos.

```text
Cursor / Claude
      │  hook
      ▼
daemon en 127.0.0.1:8765
      │  descubre el beacon UDP :8766
      ▼
ESP32  ·  sonido → cara → mensaje → reloj
```

## Qué necesitás

- ESP32-C3 SuperMini
- OLED SSD1306 de 128×64 por SPI (módulo de 6 pines, CS atado a GND)
- Buzzer pasivo en GPIO10. Uno activo no toca melodías
- El botón BOOT de la placa (GPIO9)
- Python 3 en la computadora donde usás Cursor o Claude Code
- Las dos máquinas en el mismo Wi-Fi

## Cableado

| OLED | ESP32-C3 |
| --- | --- |
| GND | GND |
| VCC | 3.3V |
| SCL / D0 | GPIO4 (SCK) |
| SDA / D1 | GPIO6 (MOSI) |
| RES | GPIO3 |
| DC | GPIO2 |
| CS | no se cablea. GPIO7 existe solo para la librería |

## El nombre en la red

La primera vez que arranca, el ESP32 abre el portal **ClaudioBirth-Setup**. Ahí cargás el Wi-Fi y el campo **Tu nombre**.

Con el nombre `Augusto`, el dispositivo aparece como:

```text
Claudio's Birth de Augusto
```

Ese texto viaja en el beacon UDP, en mDNS y en el hostname. El daemon escucha el puerto **8766**, se queda con ese equipo y le reenvía los avisos a `POST /notify`. Si el ESP32 todavía no está, los eventos esperan en una cola.

Si más adelante hay dos aparatos en la misma red, en `notification-bridge/config.json` podés fijar `owner` con el mismo nombre del portal.

## Firmware

En el Arduino IDE:

1. Placa: **ESP32C3 Dev Module**
2. USB CDC On Boot: **Enabled**
3. Partition Scheme: **Huge APP (3MB No OTA / 1MB SPIFFS)**
4. Librerías: Adafruit SSD1306, Adafruit GFX, ArduinoJson v7, WiFiManager (tzapu)
5. Abrí `ai_desk_companion/ai_desk_companion.ino` y subilo

El botón BOOT:

| Gesto | Qué hace |
| --- | --- |
| Toque corto | Muestra la IP y el nombre |
| Soltar entre 1 y 5 segundos | Limpia la cola y el estado "trabajando" |
| Mantener 5 segundos | Vuelve a abrir el portal para cambiar el Wi-Fi o tu nombre |

En reposo muestra la hora (NTP, UTC−3), el clima de OpenWeather y si Cursor o Claude siguen en una tarea. La ciudad y la API key del clima también se cargan en el portal y quedan en la memoria del ESP32.

## Daemon

Desde `notification-bridge`:

```bash
python install_daemon.py
```

Arranca con la sesión y, si se cae, vuelve a levantarse.

| Sistema | Cómo queda |
| --- | --- |
| Windows | Tarea al iniciar sesión, o la carpeta Inicio si el Programador de tareas no da permiso |
| macOS | LaunchAgent del usuario |
| Linux | Servicio systemd de usuario |

```bash
python install_daemon.py status
python install_daemon.py remove
```

`status` dice a quién tiene conectado. La IP que aparece en `config.json` es la última que vio, no una dirección fija. Ese archivo, el log y el XML de la tarea no van al repositorio.

Para probar a mano:

```bash
python notify.py cursor approval
python notify.py claude finished "Claude termino su tarea"
```

Eventos: `working`, `approval`, `finished`, `slow`, `error`, `idle`.  
Orígenes: `cursor`, `claude`.

## Hooks

Los hooks avisan al daemon sin decidir permisos. El prompt de aprobación de Cursor o Claude sigue igual.

```bash
python install_hooks.py
```

Usa el Python con el que lo ejecutás. Cursor engancha el prompt, el stop y las ejecuciones de shell o MCP. Claude Code engancha el prompt, los pedidos de permiso y el stop.

## Secuencia de un aviso

1. Suena una melodía distinta según el origen y el tipo.
2. Tres segundos de cara.
3. Tres segundos de mensaje.
4. Otra vez cara y mensaje.
5. Vuelve al reloj.

`working` e `idle` no ocupan la pantalla: solo prenden o apagan el indicador de quién está laburando.
