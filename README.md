# MEDINOSA — Real-Time ESP32 Dashboard

This is a simple prototype architecture for MEDINOSA:

ESP32 (buttons + servos) -> Wi-Fi -> Node.js backend -> Socket.IO -> Dashboard

## 1. Requirements

- Node.js 18+ recommended
- Arduino IDE
- ESP32 board package
- ESP32Servo library
- ESP32 and laptop must be on the same Wi-Fi for this local prototype

## 2. Start dashboard

Open this folder in Antigravity.

Open terminal:

```bash
npm install
npm start
```

Then open:

http://localhost:3000

## 3. Configure ESP32

Open:

`esp32/medinosa_esp32.ino`

Change:

```cpp
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

Upload to ESP32.

Open Serial Monitor at 115200 baud.

You will see something like:

`ESP32 IP: 192.168.1.105`

## 4. Connect dashboard to ESP32

Open `server.js`.

Change:

```js
const ESP32_URL = process.env.ESP32_URL || "http://192.168.1.105";
```

to your actual ESP32 IP.

Example:

```js
const ESP32_URL = process.env.ESP32_URL || "http://192.168.1.105";
```

Restart:

```bash
npm start
```

## 5. Test

1. ESP32 and laptop on the same Wi-Fi.
2. Open `http://localhost:3000`.
3. Badge should become `ESP32 Online`.
4. Press physical Button 1 -> Servo 1 moves -> quantity decreases.
5. Dashboard updates automatically.
6. You can also press the dashboard's `Dispense` button.

## Important hardware note

Do NOT power four servos directly from the ESP32 5V/3.3V pin.

Use an appropriate external 5V servo power supply and connect:

- external supply GND -> ESP32 GND
- servo signal -> ESP32 servo pin

Start with one servo/button for testing before connecting all four.

## What this prototype does

- ESP32 Wi-Fi connection
- 4 physical buttons
- 4 servo channels
- Medicine quantity
- Real-time online/offline status
- Real-time dashboard updates using Socket.IO
- Schedule: 08:00 / 12:00 / 16:00 / 20:00
- Taken / Missed / Pending
- Adherence percentage
- Dashboard-side manual dispense control

## Important limitation

This first prototype keeps "taken today" in the Node.js server memory. If the server restarts, today's taken state resets.

For the final SIH version, add Supabase/PostgreSQL so history, schedules, patient data and medicine inventory persist permanently.
