# esphome_power_flow

An ESPHome external component that draws an animated power-flow diagram of a
home electrical installation, plus detail screens for the battery and for any
node on it. Built for a Guition JC4880P443 (ESP32-P4, 480 × 800) but tied to
nothing about that panel except the screen size.

The YAML stays declarative — entities, names, icons, fonts — and all geometry,
arithmetic and animation live in C++.

## Using it

```yaml
external_components:
  - source: github://stalexteam/esphome_power_flow@v1.0.0
    components: [power_flow]
```

Reference a tag rather than a branch. A branch means the firmware you build next
month may not be the firmware you built today, and on a wall panel that surfaces
as something quietly not working.

## Prerequisites

**Home Assistant must be allowed to accept actions from the device**, if any node
declares a `control:` switch. *Settings → Devices → your panel → Configure →
«Allow the device to perform Home Assistant actions».*

It is **off by default**, and its failure mode is silent: the panel sends the
service call, Home Assistant discards it, no error appears anywhere, and the
relay does not move. Everything else works without it.

## What is where

| File | |
|---|---|
| `components/power_flow/pf_math.*` | the arithmetic, free of ESPHome and LVGL so it can be tested off-target |
| `components/power_flow/power_flow.*` | the component: subscriptions, balance, edge states |
| `components/power_flow/power_flow_render.*` | the flow diagram |
| `components/power_flow/battery_render.*` | the battery screen |
| `components/power_flow/load_render.*` | the node detail screen |
| `components/power_flow/pf_palette.h` | the palette, shared by all three screens |

The specification and the UI documents that govern every coordinate live outside
this repository, with the installation they were written for.
