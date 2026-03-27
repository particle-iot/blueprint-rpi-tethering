# Raspberry Pi Cellular Tethering

### Description

Provide cellular internet connectivity to a Raspberry Pi using a Particle B5-SoM module and M-Hat carrier board over a UART PPP link.

**Difficulty:** Intermediate
**Estimated Time:** 45 minutes
**Hardware Needed:** B5-SoM (B504E or B524) + M-Hat carrier board + Raspberry Pi 4 or 5 + cellular antenna + USB-C PD power supply
**Code Link:** https://github.com/particle-iot/blueprint-rpi-tethering

---

### Overview

Learn how to use the Particle B504e (or B524) and M-Hat to give a Raspberry Pi cellular internet access over a standard PPP network interface. The B504e firmware exposes its cellular connection as a PPP endpoint over the `Serial1` UART pins. The M-Hat physically routes those UART pins to the Raspberry Pi's 40-pin GPIO header. On the Raspberry Pi side, NetworkManager and `pppd` treat the connection as a standard GSM modem, bringing up a `ppp0` network interface that routes traffic over cellular.

```
Internet
   ↕
Particle Network (cellular)
   ↕
B504e module (EG91 modem)
   ↕  Serial1 UART @ 921600 baud (RTS/CTS)
M-Hat GPIO header
   ↕
Raspberry Pi (ppp0 network interface)
   ↕
Linux applications
```

Once `ppp0` is up, the Raspberry Pi has a standard network interface for cellular connectivity — no Particle SDK or cloud dependency is required on the Linux side.

---

### Tools & Materials

- Particle B504e module (tested) or B524
- M-Hat carrier board
- Raspberry Pi 4 or Raspberry Pi 5 (tested on RPi 5)
- Cellular antenna PARANTCW1 (required)
- USB-C PD power supply (USB-C to USB-C cable + PD-capable charger)
- Particle Workbench (VS Code) or Particle CLI
- Active Particle account with a claimed device

| Device | Modem | Testing Status |
|--------|-------|----------------|
| B504e | EG91-NAX | Tested |
| B524 | EG91-E | Not tested |

---

### Prerequisites

- Particle account and a claimed B504e or B524 device
- Device OS 6.2.1 or later
- Raspberry Pi OS (Bookworm or later) on the Raspberry Pi
- Familiarity with flashing firmware via Workbench or CLI

---

### Dependencies

The following packages must be installed on the Raspberry Pi before running the setup script. All are available via `apt` on Raspberry Pi OS:

```bash
sudo apt update
sudo apt install -y ppp modemmanager network-manager
```

| Package | Purpose |
|---------|---------|
| `ppp` | PPP daemon; creates `/etc/ppp/` structure that the setup script writes options into |
| `modemmanager` | Detects and initializes the cellular modem |
| `network-manager` | Creates and manages the `particle` GSM connection profile that auto-connects on boot |

> `raspi-config` is pre-installed on Raspberry Pi OS and is used by the script to disable the serial console, freeing the UART for PPP.

---

### Steps

1. **Assemble the hardware**

   - Seat the B504e module into the M-Hat SoM socket
   - Stack the M-Hat onto the Raspberry Pi's 40-pin GPIO header
   - Connect the cellular U.FL antenna PARANTCW1 (required)
   - Power the M-Hat via USB-C with Power Delivery

   > **Important:** A USB-A to USB-C cable will **not** power the M-Hat. Use a USB-C to USB-C cable with a PD-capable power supply, or use the optional 5V–12V DC screw terminal input.

   **HAT Power Direction Jumper** (bottom side of M-Hat):

   | Jumper Position | Behavior |
   |-----------------|----------|
   | `5V_OUT` (default) | M-Hat powers the Raspberry Pi |
   | `5V_IN` | External HAT above powers the M-Hat — do not connect M-Hat USB-C simultaneously |

   > **Warning:** These two modes are mutually exclusive. In 5V_OUT mode, only the M-HAT should be supplying power, do not simultaneously power the system from a HAT below or the Raspberry Pi's own power input. In 5V_IN mode, only the external HAT or Pi should be supplying power, do not simultaneously connect the M-HAT USB-C, DC IN, or rely on LiPo as the primary source. Mixing power directions on the 5V rail can damage the M-HAT, the Raspberry Pi, or both.

2. **Open the project** in Particle Workbench (VS Code) by opening this folder.

3. **Select your target platform:** Open the Command Palette (`Ctrl+Shift+P`) → `Particle: Configure Project for Device` → choose `b5som` and Device OS 6.2.1 or later.

4. **Flash the firmware** using `Particle: Cloud Flash` or `Particle: Local Flash` from the Command Palette.

   CLI alternative:
   ```bash
   particle compile b5som --target 6.2.1
   particle flash <device-name> firmware.bin
   ```

5. **Install Raspberry Pi dependencies:**
   ```bash
   sudo apt update
   sudo apt install -y ppp modemmanager network-manager
   ```

6. **Run the tethering setup script** as root:
   ```bash
   sudo bash tethering.sh
   ```

   The script automatically:
   - Detects RPi model and sets the correct UART device (`ttyS0` for RPi 4, `ttyAMA0` for RPi 5)
   - Enables UART and adds CTS/RTS device tree overlays in `/boot/firmware/config.txt`
   - Disables the serial console to free the UART for PPP
   - Configures PPP options in `/etc/ppp/options.<TTY>`
   - Creates ModemManager udev rules to probe at 921600 baud
   - Creates a NetworkManager connection named `particle` with APN `particle` and auto-connect enabled

   To revert all changes:
   ```bash
   sudo bash tethering.sh --disable
   ```

7. **Reboot the Raspberry Pi:**
   ```bash
   sudo reboot
   ```

   After rebooting, the `particle` connection activates automatically and `ppp0` comes up.

---

### How It Works

The firmware uses a `TetherStateMachine` class to manage the connection lifecycle without blocking `loop()`.

**UART Enable (SEL Pin):** The M-Hat UART path between the SoM and Raspberry Pi is isolated by default. The firmware drives pin `A0` (SEL) HIGH at startup to enable it:

```cpp
pinMode(A0, OUTPUT);
digitalWrite(A0, HIGH);
```

**State Machine:**

| State | Description |
|-------|-------------|
| `CONNECTING` | Waiting for `Cellular.ready()` and `Particle.connected()` |
| `TETHERING_ACTIVE` | All connected — RPi has cellular internet access |
| `TETHER_RECONNECTING` | Cellular dropped — attempting to restore connection |
| `ERROR_RECOVERY` | Max reconnect attempts reached — 60s back-off before retry |
| `FIRMWARE_UPDATE` | OTA update in progress — tethering continues, device auto-resets on completion |

```
CONNECTING
    ↓ (Cellular.ready() && Particle.connected())
TETHERING_ACTIVE  ←──────────────────────────┐
    ↓ (cellular lost)           ↓ (OTA begin) │
TETHER_RECONNECTING         FIRMWARE_UPDATE   │
    ↓ (restored) ────────────────────────────┘
    ↓ (max attempts)
ERROR_RECOVERY
    ↓ (60s elapsed)
TETHER_RECONNECTING  (retry cycle)
```

The state machine logs all transitions and prints a periodic status line every 30 seconds:

```
State: TETHERING_ACTIVE | Cloud: connected | Cellular: ready | Signal: 72% | Uptime: 182s
```

Adding new behavior to `loop()` is straightforward:

```cpp
void loop() {
    updateStateMachine();   // Connection monitoring (unchanged)

    checkMotionSensor();    // Your code here
    readTemperature();      // Your code here
}
```

---

### Usage

After rebooting, verify the connection:

```bash
# Check that the 'particle' connection is active
nmcli connection show

# Confirm ppp0 is up and has an IP address
ip addr show ppp0

# Test cellular internet connectivity
ping -I ppp0 8.8.8.8
```

A successful `ping` confirms end-to-end cellular connectivity through the B504e.

> **Note:** Wi-Fi and cellular can run simultaneously — the setup script does not disable Wi-Fi. To isolate cellular during testing: `nmcli connection down <wifi-connection-name>`.

**Particle Console:**
1. Log in at [console.particle.io](https://console.particle.io)
2. Navigate to your device → **Events** tab
3. Look for `tether/connected` and `tether/heartbeat` events

**Remote reconnect via CLI:**
```bash
particle call <device-name> reconnect
```

**Published events:**

| Event | When | Payload |
|-------|------|---------|
| `tether/connected` | On initial connection | _(none)_ |
| `tether/heartbeat` | Every 5 minutes while active | `{"signal":72,"uptime":300,"state":"TETHERING_ACTIVE"}` |

---

### Topics Covered

- [`Tether`](https://docs.particle.io/reference/device-os/tethering/) — Particle Device OS tethering API
- Non-blocking state machine pattern
- `Particle.publish()`, `Particle.function()`, `Particle.variable()`
- NetworkManager GSM connection profile with `nmcli`
- PPP over UART with RTS/CTS hardware flow control
- ModemManager udev rules

---

### Extensions

- **Sleep management:** Add a `SleepStateMachine` to enter low-power mode when the RPi is idle, using `System.sleep()` and `TetherStateMachine` state checks
- **Custom sensor data:** Register `Particle.variable()` entries and publish readings alongside the heartbeat
- **Remote SSH over cellular:** Use a reverse tunnel tool like [bore](https://github.com/ekzhang/bore) on the RPi to expose SSH through the `ppp0` interface
- **Internet gateway:** Configure the RPi as a Wi-Fi hotspot that shares cellular via `hostapd`, `dnsmasq`, and IP forwarding
- **VPN over cellular:** Install WireGuard and route traffic through an encrypted tunnel over the PPP interface
- **Send RPi data to cloud:** With `ppp0` up, standard HTTP/MQTT clients (Python `requests`, `paho-mqtt`) work over cellular without any Particle SDK

---

### Additional Resources

- [Particle Tethering Documentation](https://docs.particle.io/reference/device-os/tethering/)
- [B5 SoM Datasheet](https://docs.particle.io/reference/datasheets/b-series/b5som-datasheet/)
- [M-Hat Datasheet](https://docs.particle.io/reference/datasheets/m-series/m-hat-datasheet/)
- [Particle Developer Docs](https://docs.particle.io)
- [Particle Community](https://community.particle.io)
