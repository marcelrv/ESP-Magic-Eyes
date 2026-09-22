# ESP Magic Eyes

A pair of robotic eyes that look around, blink, react to people nearby, and can be controlled from your phone or computer — built around the [nmrobots.com ε-SERIES animatronic eye mechanism](https://nmrobots.com/pages/designs), a small radar sensor, and an ESP32 microcontroller.

The eyes can move on their own (looking around, blinking, "sleeping"), react when someone walks up, or be puppeteered directly from a web page on your phone — including winks, surprised looks, and other expressions. Everything is controlled over your home WiFi, with no cables or extra hardware needed once it's set up.

## What's inside the box

- **6 small servo motors** move the eyes left/right, up/down, and open/close each eyelid independently.
- **A radar sensor** (no camera — just motion/presence sensing, so it works in the dark and doesn't raise camera-privacy concerns) detects when someone is nearby.
- **An RGB glow light** behind each eye can be added later for a colored lighting effect (optional, not required).
- **A web page**, hosted by the device itself, is how you set it up and control it — no app to install.

## Getting started

### 1. Flash the firmware

You'll need a computer with [Visual Studio Code](https://code.visualstudio.com/) and the [PlatformIO extension](https://platformio.org/platformio-ide) installed — both are free. Then:

1. Open this project folder in VS Code.
2. Connect the ESP32 board to your computer with a USB cable.
3. In the PlatformIO sidebar, choose the **ld2420** build (this matches the radar sensor that ships with the kit) and click **Upload**.
4. Also click **Upload Filesystem Image** once, so the built-in web pages get copied onto the device.

If you ever upgrade to the newer LD2450 radar sensor, switch to the **ld2450** build instead and re-upload — everything else about the setup stays the same.

### 2. Connect it to your WiFi

The very first time it powers on (and any time its WiFi is "forgotten"), the device creates its own temporary WiFi network so you can give it your home WiFi details:

1. On your phone or laptop, connect to the WiFi network named **`MagicEyes-Setup-XXXX`** (the last 4 characters vary per device). Password: `eyes-setup`.
2. A setup page should pop up automatically (like the "sign in to WiFi" screens hotels use). If it doesn't, open a browser and go to `http://192.168.4.1`.
3. Choose your home WiFi network from the list (or type its name), enter the password, and confirm.
4. The device restarts and joins your home network. From then on, it's reachable on that network — check your router's device list for its name/IP address, or use network-discovery tools if you're not sure.

### 3. Open the control page

Once it's on your home WiFi, open a browser (on your phone, tablet, or computer) and go to the device's address. You'll land on the home page, with two sections:

- **Control** — for everyday use: move the eyes, trigger a wink or blink, switch between behavior modes (idle, curious, sleepy, greeting, "watch for people").
- **Setup** — for calibration and maintenance: fine-tuning each servo's range of motion, checking the radar sensor is working, updating the firmware, and reconfiguring WiFi. These pages have an orange "maintenance" theme so they're never mistaken for the everyday controls.

## Everyday use

From the **Control** section:

- **Manual control** — drag on a touchpad to look in a direction, use sliders to open/close each eyelid, and tap buttons for expressions like wink, blink, surprised, or sleepy. There's also a switch for "natural mode," which makes the eyelids follow the eyes automatically (the way real eyelids do when you look up or down) — turn it off if you want full manual control over the eyelids instead.
- **Play modes** — pick a behavior and let the eyes run on their own:
  - **Idle** — quiet resting behavior with occasional blinks.
  - **Curious** — looks around more actively, as if paying attention to its surroundings.
  - **Sleep** — eyes ease shut and settle down.
  - **Greeting** — a one-time "waking up and noticing you" animation, good for a demo.
  - **Watch for people** (tracking) — reacts when the radar senses someone nearby. With the radar sensor included in the kit, it gives an alert glance when someone approaches (it can't yet tell *which direction* they're in); the optional upgraded radar sensor adds the ability to actually follow a person's position.
  - **Manual** — turns off all automatic behavior, so you're always in full control (recommended if you're driving the eyes from another app, a voice assistant, or your own code).
- **Status** — a live readout of what the eyes are currently doing and what the radar sensor sees, handy for checking everything's working.

## Setup & maintenance

From the **Setup** section:

- **WiFi** — reconnect to a different network, or "forget" the current one to reset back to the setup mode described above.
- **Calibration** — for each of the 6 servos, nudge and test its range of motion until the physical eye mechanism moves smoothly and doesn't strain against its mechanical limits, then save. Do this once after first assembling the mechanism, and again if you ever notice a servo straining or not reaching its full range.
- **Radar test** — a live view of what the radar sensor is currently detecting, useful for checking it's wired correctly and positioned well.
- **Firmware update** — update the device's software over WiFi, without needing to reconnect it to a computer. (You can also always update it the original way, by reconnecting the USB cable and using PlatformIO, as in the initial setup.)
- **LED (optional)** — once a glow light is wired in, turn it on/off and choose its color and brightness here.

## Controlling it from something else

Everything the web page does, it does by talking to the device over a simple web API — so anything that can make web requests (a phone shortcut, a voice assistant integration, a smart-home system, or a custom program) can drive the eyes the same way. If you're building an integration like that, see [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) for the full list of what it can be asked to do.

## Wiring and technical details

This README covers day-to-day use. For how everything is wired up (which servo goes on which pin, the radar and LED connections), and a full technical description of how the software is built, see **[architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md)**.
