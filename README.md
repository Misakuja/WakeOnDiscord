# Discord WOL — Wake-on-LAN via Discord Bot on ESP32-S3

Wake up your PC remotely by sending `!wol` in a Discord channel. An ESP32-S3 polls the channel and fires a magic packet at your target machine.

---

## Hardware Required

- **ESP32-S3** development board (the code uses pin 38 for the onboard NeoPixel, which matches the WaveShare ESP32-S3 board — adjust `LED_PIN` if yours differs)
- The target PC must have **Wake-on-LAN enabled** in its BIOS/UEFI
- Both the ESP32-S3 and the target PC must be on the **same local network**

---

## How It Works

1. On boot, the board connects to Wi-Fi and seeds the last Discord message ID (so it doesn't re-process old messages)
2. Every 3 seconds it polls your Discord channel for new messages
3. If a message from your configured user ID contains `!wol`, it sends a WOL magic packet to your target PC's MAC address
4. The bot replies in the channel and reacts with ✅ to confirm
5. The onboard RGB LED indicates status at a glance:

| Color | Meaning |
|---|---|
| 🔵 Blue (solid) | Connecting to Wi-Fi |
| 🟢 Green (solid) | Idle, polling normally |
| ⚪ White (flash) | Magic packet sent |
| 🔴 Red (solid) | Wi-Fi connection lost |
| 🔴 Red (blinking) | HTTP request error |

---

## Discord Bot Setup
As this is a private project, any external users will need to configure their own discord bot. This is a simple process, explained below:

### 1. Create the Bot

1. Go to the [Discord Developer Portal](https://discord.com/developers/applications) and click **New Application**
2. Give it a name, then go to the **Bot** tab
3. Click **Reset Token** and copy your bot token — paste this into `BOT_TOKEN` in `env.h`
4. Under **Privileged Gateway Intents**, enable **Message Content Intent** (required to read message text)

### 2. Invite the Bot to Your Server

1. Go to **OAuth2 → URL Generator**
2. Under **Scopes**, select `bot`
3. Under **Bot Permissions**, select:
   - `Read Messages`
   - `View Channels`
   - `Send Messages`
   - `Add Reactions`
4. Copy the generated URL, open it in your browser, and invite the bot to your desired server.

### 3. Get the Required IDs

You'll need to enable **Developer Mode** in Discord (Settings → Advanced → Developer Mode), then in `env.h` change the following variables:

- **CHANNEL_ID**: Right-click the channel you want the bot to monitor → *Copy Channel ID*
- **USER_ID**: Right-click your own username → *Copy User ID* ***(only messages from this user will trigger WOL)***

---

## Project Setup

### 1. Install Dependencies

This project uses [PlatformIO](https://platformio.org/). Install it via VS Code extension or the CLI.

The following libraries are required (add to your `platformio.ini`):

```ini
lib_deps =
    bblanchon/ArduinoJson
    a7md0/WakeOnLan
    adafruit/Adafruit NeoPixel
```

### 2. Fill out the `env.h`

There is a file called `env.h` in your `src/` directory, you will need to fill it out with your credentials:

```cpp
const char* WIFI_SSID = "Your WiFi SSID";
const char* WIFI_PASSWORD = "Your WiFi Password";

const char* BOT_TOKEN = "Your Discord Bot Token";
const char* USER_ID = "Your Discord User ID";

const char* TARGET_PC_MAC = "Your PCs MAC Address";;

const char* CHANNEL_ID = "Your Discord Channel ID";;
```

### 3. Configure `platformio.ini`
The currently uploaded `platformio.ini` works for the WaveShare ESP32-S3 board, however you may need to adjust it for your own.

It is important to keep the following lines:
```ini
[env:esp32-s3-devkitc-1]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200

lib_deps =
    bblanchon/ArduinoJson
    a7md0/WakeOnLan
    adafruit/Adafruit NeoPixel
```

Adjust the `board` value to match your specific ESP32-S3 board.

### 4. Flash & Run

```bash
pio run --target upload
pio device monitor
```

You should see the board connect to Wi-Fi and print the seeded message ID to serial. From that point, send `!wol` in your configured channel and watch the LED flash white.

---

## Finding Your PC's MAC Address

**Windows:**
```
ipconfig /all
```
Look for the *Physical Address* of your Ethernet adapter (WOL works over wired connections only — Wi-Fi WOL is unreliable).

**Linux/macOS:**
```
ip link show
# or
ifconfig
```

---

## Notes & Limitations

- WOL only works when the ESP32-S3 and target PC are on the **same subnet**
- The bot uses **polling** (no gateway/websocket) to keep the firmware simple — there is a ~3 second response delay by design
- TLS certificate verification is disabled (`setInsecure()`) for simplicity. For a more secure setup, pin the Discord root CA certificate
- Only messages from the configured `USER_ID` trigger WOL, so the bot won't respond to anyone else in the channel
