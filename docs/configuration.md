# Configuration

The YAML declares *what exists* — entities, names, icons, fonts. Layout,
colours and arithmetic live in the component. The governing rule: **a value
whose validity condition is unmet shows a dash, never an estimate.**

For a complete working config see
[`examples/JC4880P443.yaml`](../examples/JC4880P443.yaml).

## The model

The installation is a graph of **devices** (grid, inverter, battery, bus, pv)
whose **terminals** carry power. Every meter is a Home Assistant entity the
component subscribes to by itself — you never write a `sensor:` block for
them. Per node, the balance can solve for **exactly one** edge declared
`power: auto`; a second one is rejected at validation.

Switches are read as *text* sensors: only an explicit `off` opens an edge,
while `absent`, `unknown` and `null` all mean closed. A device that reports no
usable state simply declares no `switch:`.

## Component options

| Option | Default | |
|---|---|---|
| `parent_id` | **required** | empty LVGL `obj` the flow diagram is built into |
| `battery_parent_id` | — | root for the battery screen; omit to have none |
| `load_parent_id` | — | root for the node detail screen |
| `time_id` | — | `time:` component, for the "off since 09:12" lines |
| `status` | — | a `binary_sensor: platform: status` id; distinguishes "HA is gone" from "the mains are gone" |
| `on_back` | — | automation run by every Back button |
| `on_node_click` | — | automation run when any node is tapped (a device's own `on_click` overrides it) |
| `average_window` | `60s` | window differences are computed over |
| `display_window` | `10s` | window measured values are *shown* over |
| `idle_below` | `3W` | flow below this renders the edge idle |
| `battery_deadband` | `15W` | \|P_battery\| below this counts as at rest |
| `figure` | `auto` | headline figure: `auto` / `losses` / `efficiency` / `both` |
| `update_interval` | `250ms` | recompute period |
| `fonts` | **required** | the thirteen font slots, see below |
| `inference` | — | see below |
| `devices` | **required** | list, see below |
| `consumers` | `[]` | list, see below |

## Devices

Every device: `id` (a graph name, not an ESPHome id) and `kind`
(`grid` / `inverter` / `battery` / `bus` / `pv`), plus optionally `name`,
`icon` (an MDI glyph present in the icon font), and `key` (shown in logs and
on the detail screen).

Detail-screen readings, on any device: `voltage`, `temperature`, `link`.
`control:` names a Home Assistant switch entity and puts a working toggle on
the device's detail screen — see the permission note in the README.
`on_click:` runs an automation when the device's box is tapped; the component
knows nothing about LVGL pages, the YAML decides what a tap does.

- **battery** adds `soc`, `voltage`, `capacity` and a `details:` block for the
  battery screen.
- **bus** takes `source: inv.output` — a device-qualified reference to the
  terminal that feeds it. Consumers attach to the (single) bus.
- a bare `meter:` on a device is sugar for one generic terminal.

### Terminals

`terminals:` maps a role — `input`, `output`, `battery`, `self`, `pv`, `tap`,
`generic` — to:

| Key | Default | |
|---|---|---|
| `meter` / `power` / `sum` / `prefer` | — | at most one. `meter`/`power` are one entity; `sum` adds a list of entities (parallel strings), `prefer` takes the first valid of a list (redundant meters). `power: auto` marks the node's one solved edge |
| `energy` | — | energy counter entity for the same line |
| `switch` | — | breaker in series with *this* line; a switch belongs to the terminal it actually interrupts |
| `unavailable` | `de_energized` | what entity-unavailable means: `de_energized` (meter powered from the line it measures) or `no_data` (telemetry bridge; the line is fine) |
| `enabled` | `true` | `false` removes the edge and its node from the diagram |
| `baseline` | — | `learn` fits the standing draw from the node balance; only valid with `power: auto` |
| `ceiling` | — | upper bound applied to a solved value; only valid with `power: auto` |
| `stale_after` | learned | override the per-entity staleness detector |
| `staleness` | `false` | show the staleness marker for this edge |
| `bidirectional` | — | the line legitimately flows both ways |
| `sign` | by role | `in` / `out`, when the meter's sign convention disagrees with the role |

## Consumers

Consumers are terminals of the bus with a required `name`, a `side`
(`left` / `right`, default `right`), and the same measurement keys as any
terminal — including `power: auto` for the one unmetered remainder. Optional:
`icon`, `key`, `voltage`, `temperature`, `link`, `control`.

## Battery `details:`

All optional: `charge_status`, `errors`, `current`, `power_sensor`,
`capacity_total`, `cycles`, `cell_delta` (volts in, millivolts on screen),
`balancing` (numeric; zero reads OFF), `energy_charged`, `energy_discharged`,
`temperatures` (list of `{label, sensor}`), and `cells` — either an explicit
sensor list or `{prefix: sensor.bms_cell_, count: 8}` when the ids follow a
pattern (1..count, up to 32). `highlight_above` (default `10.0` mV) sets the
cell-spread highlight threshold.

## Inference

```yaml
inference:
  grid_loss_from_battery:
    enabled: true          # default
    mode: suspect          # or assume_offline
    min_discharge: 50W
    hold: 20s
    require_stale_grid: false
```

A battery discharging harder than `min_discharge` for `hold` while the grid
meter still claims power means the grid reading is lying (the meter froze at
its last value). `suspect` dashes the grid figure; `assume_offline` also
recolours the diagram as an outage.

## Fonts

Thirteen slots, all required — ESPHome builds fonts at codegen time, so they
are the one piece of appearance that cannot live in the component. Two
families, one rule: numerals monospaced, words proportional.

| Slot | Face | Size | | Slot | Face | Size |
|---|---|---|---|---|---|---|
| `s10` | Montserrat 600 | 10 | | `m12` | JetBrains Mono 500 | 12 |
| `s14` | Montserrat 500 | 14 | | `m14` | JetBrains Mono 600 | 14 |
| `s16` | Montserrat 600 | 16 | | `m16` | JetBrains Mono 600 | 16 |
| `s18` | Montserrat 600 | 18 | | `m22` | JetBrains Mono 700 | 22 |
| `s20` | Montserrat 700 | 20 | | `m28` | JetBrains Mono 700 | 28 |
| `s22` | Montserrat 700 | 22 | | `m72` | JetBrains Mono 700 | 72 |

plus `icon` — Material Design Icons at 26px, containing the glyphs your
devices declare. The example carries the exact `font:` block, including the
minimal glyph sets that keep twelve faces affordable in flash.

## LVGL scaffolding

The component draws with the raw LVGL C API into empty full-screen `obj`
widgets — one per screen, each on its own page. The pages, the `obj`s, an
`api:`, a `time:` and the `status` binary sensor are ordinary ESPHome YAML;
copy them from the example. Page switching is wired through `on_back` /
`on_node_click` / `on_click`, so a panel laid out differently binds different
actions.
