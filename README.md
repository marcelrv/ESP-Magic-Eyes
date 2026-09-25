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
3. In the PlatformIO sidebar, click **Upload**.
4. Also click **Upload Filesystem Image** once, so the built-in web pages get copied onto the device.

The same firmware works with the kit's LD2420 radar sensor, the newer LD2450, or no radar at all. It assumes the kit's LD2420 until you choose otherwise on the **Radar test** page in Setup (the device reboots to switch).

### 2. Connect it to your WiFi

The very first time it powers on (and any time its WiFi is "forgotten"), the device creates its own temporary WiFi network so you can give it your home WiFi details:

1. On your phone or laptop, connect to the WiFi network named **`MagicEyes-Setup-XXXX`** (the last 4 characters vary per device). Password: `eyes-setup`.
2. A setup page should pop up automatically (like the "sign in to WiFi" screens hotels use). If it doesn't, open a browser and go to `http://192.168.4.1`.
3. Choose your home WiFi network from the list (or type its name), enter the password, and confirm.
4. The device restarts and joins your home network. From then on, it's reachable on that network — check your router's device list for its name/IP address, or use network-discovery tools if you're not sure.

If the device can't reach your WiFi (for example after the network name or password changed), it falls back to the `MagicEyes-Setup-XXXX` network above and keeps retrying your saved network every minute. You can also set the WiFi details over the USB cable: open a serial monitor at 115200 baud (e.g. PlatformIO's **Monitor**) and type `wifi set <network-name> "<password>"` (quotes are needed if the name or password contains spaces). Type `help` for the other commands, such as `wifi status`.

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
- **Calibration** — teach the device where the eyes' key positions are (see [Calibrating the eyes](#calibrating-the-eyes) below). Do this once after first assembling the mechanism, and again if you ever notice a servo straining, the eyes not looking straight, or the eyelids not closing or matching.
- **Radar test** — choose which radar sensor is fitted (or none), and see a live view of what it is currently detecting, useful for checking it's wired correctly and positioned well.
- **Firmware update** — update the device's software over WiFi, without needing to reconnect it to a computer. (You can also always update it the original way, by reconnecting the USB cable and using PlatformIO, as in the initial setup.)
- **LED (optional)** — once a glow light is wired in, turn it on/off and choose its color and brightness here.
- **Security** — set the passwords (see below).

## Passwords

Out of the box the device has no password, so anyone on your WiFi network can use it. The home page shows a warning until you set one. On **Setup → Security** you can set two separate passwords:

- **Control password**: needed to open the pages at all and to move the eyes. When your browser asks you to log in, use the user name **`user`**.
- **Admin password**: needed for the Setup pages, including WiFi, calibration and firmware updates. The user name is **`admin`**. The admin password also works for the everyday controls. If you set only a control password, it protects the Setup pages as well.

Your browser remembers the login until you close it. If you update the firmware from PlatformIO over WiFi, add `upload_flags = --auth=<admin password>` to `platformio.ini` once an admin password is set.

**Forgot a password?** With the device in hand, hold the **BOOT** button for 5 seconds. This removes both passwords, and also forgets the WiFi network, so you set that up again as in step 2. If you'd rather keep the WiFi settings, connect the USB cable and type `auth reset` in the serial monitor instead.

## Calibrating the eyes

Every mechanism is assembled slightly differently, so the device needs to be shown a few reference positions. Everything else — gaze movements, blinks, winks, sleep, natural-mode eyelids — is measured from these points, so it's worth doing carefully.

Open **Setup → Calibration**. While this page is open, all automatic eye movement is paused, so each servo stays exactly where you put it. Every position is set with a slider (drag it, or use the **−10 / −1 / +1 / +10** buttons for fine tuning) and the servo moves live as you adjust it. Work through the steps in order, pressing that step's **Save** button when it looks right:

1. **Look straight ahead** — adjust *Pan* and *Tilt* until both eyes look straight forward. Use **Check: look right / look up** to confirm the eyes move the right way; if one goes the wrong way, tick **Reversed** for it and save again.
2. **Close the lids — just touching** — with the eyes straight ahead, move each of the four eyelids until the upper and lower lid of each eye *just touch*. Don't squeeze them together: this is the point blinks, winks and sleep close to, and pressing the lids against each other strains the servos.
3. **Open the lids** — move each lid to its fully open position.
4. **Half open — make both eyes match** — move each lid to half open so the left and right eye look identical. Eyelid linkages don't move evenly, and aren't identical left and right, so without this point in-between positions (such as the resting look in natural mode) can sit at different heights on each eye.
5. **Safety limits** (advanced, optional) — hard limits the servos will never be driven past. Lid limits are widened automatically to include the points above.

Handy while calibrating:

- **Reference poses** (top of the page) move all servos at once to *straight + closed*, *half open*, *open* or *resting*, using the current slider values — including changes you haven't saved yet.
- **Compare both eyes** sweeps all four eyelids together from closed to open. Both eyes should look the same at every position; if they drift apart somewhere, adjust the nearest calibration point (closed, half or open).

When you're done, press **Resume motion** (or just leave the page — movement resumes automatically about 30 seconds later).

> **Tip:** if the eyes ever twitch or jump back to a resting position while you calibrate, or the device drops off the network, open `http://<device-address>/api/system/info` — the `resetReason` field shows whether it restarted, and why (for example `brownout`, which usually points to a weak or shared servo power supply).

## Controlling it from something else

Everything the web page does, it does by talking to the device over a simple web API — so anything that can make web requests (a phone shortcut, a voice assistant integration, a smart-home system, or a custom program) can drive the eyes the same way. If you've set passwords, the integration needs to log in with "digest" authentication (for example `curl --digest -u user:<password> ...`). If you're building an integration like that, see [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) for the full list of what it can be asked to do.

## Wiring and technical details

This README covers day-to-day use. For how everything is wired up (which servo goes on which pin, the radar and LED connections), and a full technical description of how the software is built, see **[architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md)**.
