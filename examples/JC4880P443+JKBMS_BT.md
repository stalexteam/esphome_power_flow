# JC4880P443 + JK-BMS over Bluetooth — the chimera

One firmware that does two jobs on a single Guition JC4880P443 wall panel
(ESP32-P4 + ESP32-C6): it renders the power-flow panel of this repository
**and** it is the JK-BMS bridge — the panel connects to the BMS over BLE,
publishes every battery reading to Home Assistant, and then subscribes to
those same entities back for its own screens. It replaces the usual separate
ESP32 dongle running [syssi/esphome-jk-bms](https://github.com/syssi/esphome-jk-bms).

Config: [`JC4880P443+JKBMS_BT.yaml`](JC4880P443+JKBMS_BT.yaml).

## How the data flows

```
JK-BMS  --BLE-->  ESP32-C6  --SDIO-->  ESP32-P4  --api-->  Home Assistant
                                          ^                      |
                                          +----- subscriptions --+
```

The round-trip through Home Assistant is deliberate, not an accident: HA
stays the single source every displayed number comes from, exactly as in the
plain panel. The cost is one HA round-trip of latency on the battery figures;
the win is that the display logic has one upstream, and the BMS data lands in
HA whether or not anything is being displayed.

Consequences to accept before merging the two devices into one:

- **Shared fate.** With a separate dongle, the display and the battery
  monitor fail independently. In the chimera, rebooting or reflashing the
  panel also interrupts battery reporting for those seconds.
- **Entity ids move.** The readings are published by the panel's device now,
  so `sensor.bms24v_power` (or whatever the old dongle was called) becomes
  `sensor.<panel_device>_power`. HA history starts fresh; automations
  pointing at the old ids need updating.
- **One BLE client.** The JK-BMS accepts a single BLE connection. Power off
  the old dongle and close the phone app, or the panel never connects.

## Requirements

- ESPHome **≥ 2025.12** (the JC4880P443 display model; hosted BLE on the P4
  landed just before it). The component pins
  [syssi/esphome-jk-bms **3.0.0**](https://github.com/syssi/esphome-jk-bms/releases/tag/3.0.0),
  which itself wants ≥ 2025.11.
- `secrets.yaml` next to the config with:

  ```yaml
  wifi_ssid: "..."
  wifi_password: "..."
  fallback_password: "..."   # the captive-portal AP
  bms_mac: "AA:BB:CC:DD:EE:FF"   # the JK-BMS's BLE address
  ```

  The BMS's MAC is shown in the JK phone app, or in the log of whatever
  bridge served it before.

## Bring-up

### 1. First flash — over the cable

Compile and flash `firmware.factory.bin` over USB. OTA works for every
flash after this one.

### 2. The C6 Bluetooth firmware — the one manual step you cannot skip

The P4 has no radio of its own; Wi-Fi *and* BLE run on the on-board ESP32-C6
over SDIO (esp-hosted). **Guition ships the C6 with a Wi-Fi-only firmware**,
so on first boot Wi-Fi works but BLE is dead. The log shows:

```
[C][esp32_ble]: Bluetooth stack is not enabled
[E][component]: esp32_ble is marked FAILED
[W][jk_bms_ble]: [<mac>] Not connected     (repeating forever)
```

The fix is built into the config — an `update:` entity named **"C6
firmware"**:

1. Let the panel join Wi-Fi and appear in Home Assistant.
2. Open the device page in HA and press **Install** on *C6 firmware*. The
   panel reflashes the co-processor over SDIO with a Bluetooth-capable image
   from ESPHome's own prebuilt feed
   ([esphome/esp-hosted-firmware](https://github.com/esphome/esp-hosted-firmware));
   the highest version compatible with the compiled-in host library is
   chosen automatically.
3. Wait for the panel to come back. `esp32_ble` now sets up cleanly and the
   BMS connects within a scan cycle — the battery entities in HA fill with
   numbers.

Two warnings, once: there is **no published copy of Guition's original C6
image**, so the replacement is one-way; and a failed co-processor flash
costs Wi-Fi until you recover over the USB cable. Done once, it stays done —
OTA updates of the panel itself never touch the C6 again.

### 3. Point the panel at its own entities

HA prefixes the published entity ids with the device's name *as registered
in HA's registry* — which is whatever the device was named or renamed to,
not necessarily the ESPHome `name:`. Check one real id in *Developer Tools →
States* (e.g. the `power` sensor) and set `name_slug:` in section 1 of the
YAML to match. Get this wrong and the flow diagram shows the battery as
`no data` while HA happily shows every reading — the panel is subscribed to
ids that do not exist.

## Adapting to another installation

| What differs | Where to change |
|---|---|
| BMS address | `bms_mac` in `secrets.yaml` |
| JK protocol | `bms_protocol_version:` — `JK02_32S` for current firmware, `JK02_24S` for JK hardware < 11.0 |
| Pack size (e.g. 16s) | Uncomment `cell_voltage_9..16` in section 3 **and** set `cells: count: 16` in section 1 — the two must agree |
| Entity id prefix | `name_slug:` in section 1 (see above) |
| What the diagram shows | Section 1, exactly as in the plain `JC4880P443.yaml` |

## Troubleshooting

- **`Bluetooth stack is not enabled` / `esp32_ble is marked FAILED`** — the
  C6 is still on a Wi-Fi-only firmware. Run the *C6 firmware* update
  (bring-up step 2).
- **BLE is up but `[<mac>] Not connected` repeats** — the BMS's single BLE
  slot is taken (old dongle still powered, phone app open), the MAC in
  `secrets.yaml` is wrong, or the BMS is out of radio range of the panel.
  Bump `logger: level:` to `DEBUG` and reflash over OTA to watch the scan
  and the connection handshake; the panel's web server (port 80) streams the
  log without the serial cable.
- **Entities exist in HA but the panel's battery screen is dashes** —
  `name_slug` does not match the real entity prefix (bring-up step 3).
- **Daily energy did not reset at midnight** — the clock comes from HA
  (`ha_time`); if the panel booted while HA was down and stayed that way
  past midnight, that day's reset is skipped. Accepted trade-off: this
  panel lives off HA anyway.

## Migrating off a separate reporter

1. Flash the chimera and finish the bring-up above **while the old dongle
   still runs** — the BMS serves one client, so the panel will log
   `Not connected` until the dongle is powered off, but everything else
   (Wi-Fi, HA, the C6 update) can be completed.
2. Power off the dongle. The panel connects within a scan cycle.
3. Repoint anything that referenced the old entity ids (automations,
   dashboards, this panel's own `name_slug` subscriptions) and delete the
   dongle's device from HA when the history stops mattering.
