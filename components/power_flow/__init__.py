"""power_flow — config schema and codegen.

This module is a straightforward transcription of the YAML into the flat,
index-addressed builder declared in ``power_flow.h``. It keeps no mirror data
structures beyond the index bookkeeping needed to turn declaration order into
the ``uint8_t`` indices the builder hands out, and the topology map needed for
the one-``auto``-per-node check.

Two things here are load-bearing and must not be "simplified":

* ``sum:`` and ``prefer:`` are separate keys (decision log #2). Parallel
  branches on one terminal are added; one line measured twice must not be.
  Merging them into a generic "list of entities" would silently double-count.
* Every ``switch:`` becomes a Home Assistant **text** sensor, never a binary
  sensor (§6.1, decision log #6). Only an explicit ``off`` opens an edge;
  ``absent``, ``unknown`` and ``null`` all mean closed, and a binary sensor
  cannot express that difference. Two of the smart plugs publish
  ``"state": null`` forever, so this is not a hypothetical.

Home Assistant subscriptions are created programmatically:
``setup_home_assistant_entity()`` is a public helper in
``components/homeassistant/__init__.py`` (§2), so the owner never has to
declare a parallel ``sensor:`` block for the meters named here.

Section references (§) point at the specification, task.md.
"""

import itertools
import re

from esphome import automation
import esphome.codegen as cg
from esphome.components import binary_sensor, font, sensor, text_sensor
from esphome.components.homeassistant import (
    HOME_ASSISTANT_IMPORT_SCHEMA,
    setup_home_assistant_entity,
)
from esphome.components.homeassistant.sensor import HomeassistantSensor
from esphome.components.homeassistant.text_sensor import HomeassistantTextSensor
from esphome.components.lvgl.types import lv_obj_t
import esphome.config_validation as cv
from esphome.const import (
    CONF_ENTITY_ID,
    CONF_ID,
    CONF_NAME,
    CONF_PLATFORM,
    CONF_TRIGGER_ID,
)
from esphome.core import CORE

CODEOWNERS = ["@stalexteam"]

# `api` for the Home Assistant state subscriptions, `lvgl` for the parent
# object the diagram is built into.
DEPENDENCIES = ["api", "lvgl"]

# The sensors and text sensors below are created in codegen rather than by the
# user, so the platforms they belong to have to be pulled in explicitly.
AUTO_LOAD = ["sensor", "text_sensor", "homeassistant"]

power_flow_ns = cg.esphome_ns.namespace("power_flow")
PowerFlow = power_flow_ns.class_("PowerFlow", cg.Component)

DeviceKind = power_flow_ns.enum("DeviceKind", is_class=True)
TerminalRole = power_flow_ns.enum("TerminalRole", is_class=True)
MeterCombine = power_flow_ns.enum("MeterCombine", is_class=True)
Side = power_flow_ns.enum("Side", is_class=True)
UnavailablePolicy = power_flow_ns.enum("UnavailablePolicy", is_class=True)
FigureMode = power_flow_ns.enum("FigureMode", is_class=True)
InferenceMode = power_flow_ns.enum("InferenceMode", is_class=True)

# Tapping a device box fires this, and the component deliberately knows nothing
# about what it is bound to — the YAML decides whether that shows an LVGL page,
# toggles something, or does nothing at all (§7). A bare `Trigger<>` rather than
# a component-specific subclass, because `Device::on_click` in power_flow.h is
# exactly that type; there is no C++ class here for a subclass to name.
DeviceClickTrigger = automation.Trigger.template()

# --------------------------------------------------------------------------
# Config keys
# --------------------------------------------------------------------------
CONF_AVERAGE_WINDOW = "average_window"
CONF_DISPLAY_WINDOW = "display_window"
CONF_BASELINE = "baseline"
CONF_BATTERY_DEADBAND = "battery_deadband"
CONF_BIDIRECTIONAL = "bidirectional"
CONF_CAPACITY = "capacity"
CONF_CEILING = "ceiling"
CONF_CONSUMERS = "consumers"
CONF_DEVICES = "devices"
CONF_DISCHARGE_ETA = "discharge_eta"
CONF_ENABLED = "enabled"
CONF_ENERGY = "energy"
CONF_FIGURE = "figure"
CONF_GRID_LOSS_FROM_BATTERY = "grid_loss_from_battery"
CONF_HOLD = "hold"
CONF_IDLE_BELOW = "idle_below"
CONF_INFERENCE = "inference"
CONF_KIND = "kind"
CONF_METER = "meter"
CONF_MIN_DISCHARGE = "min_discharge"
CONF_MODE = "mode"
CONF_ON_CLICK = "on_click"
CONF_PARENT_ID = "parent_id"
CONF_POWER = "power"
CONF_PREFER = "prefer"
CONF_REQUIRE_STALE_GRID = "require_stale_grid"
CONF_SIDE = "side"
CONF_SIGN = "sign"
CONF_SOC = "soc"
CONF_SOC_CUTOFF = "soc_cutoff"
CONF_SOURCE = "source"
CONF_STALE_AFTER = "stale_after"
CONF_STALENESS = "staleness"
CONF_STATUS = "status"
CONF_STYLE = "fonts"
CONF_F_S10 = "s10"
CONF_F_S14 = "s14"
CONF_F_S16 = "s16"
CONF_F_S18 = "s18"
CONF_F_S20 = "s20"
CONF_F_S22 = "s22"
CONF_F_M12 = "m12"
CONF_F_M14 = "m14"
CONF_F_M16 = "m16"
CONF_F_M22 = "m22"
CONF_F_M28 = "m28"
CONF_F_M72 = "m72"
CONF_F_ICON = "icon"
CONF_ICON = "icon"
CONF_SUM = "sum"
CONF_SWITCH = "switch"
CONF_TERMINALS = "terminals"
CONF_UNAVAILABLE = "unavailable"
CONF_UPDATE_INTERVAL = "update_interval"
CONF_VOLTAGE = "voltage"

# The literal accepted by `power:` in place of an entity id. It marks the one
# edge per node that the balance solves for (§4).
AUTO = "auto"

DEVICE_KINDS = {
    "grid": DeviceKind.GRID,
    "inverter": DeviceKind.INVERTER,
    "battery": DeviceKind.BATTERY,
    "bus": DeviceKind.BUS,
    "pv": DeviceKind.PV,
}

# Terminals are named from the device's point of view (decision log #1).
TERMINAL_ROLES = {
    "input": TerminalRole.INPUT,
    "output": TerminalRole.OUTPUT,
    "battery": TerminalRole.BATTERY,
    "self": TerminalRole.SELF,
    "pv": TerminalRole.PV,
    "tap": TerminalRole.TAP,
    "generic": TerminalRole.GENERIC,
}

SIDES = {"left": Side.LEFT, "right": Side.RIGHT}

UNAVAILABLE_POLICIES = {
    # Default. The meter is powered from the line it measures, so `unavailable`
    # really does mean the line is dead (§6.3, decision log #5).
    "de_energized": UnavailablePolicy.DE_ENERGIZED,
    # Telemetry bridge (the BLE gateway in front of the BMS): the reading is
    # gone but the thing it measures obviously is not.
    "no_data": UnavailablePolicy.NO_DATA,
}

# The component always computes both a loss in watts and an efficiency from
# the same pair of sums over the inverter node, and they are exactly
# complementary (pf_math.h §6.8). `auto` shows watts while the battery rests
# and a percentage while it charges or discharges; the rest force the choice.
FIGURE_MODES = {
    "auto": FigureMode.AUTO,
    "losses": FigureMode.LOSSES,
    "efficiency": FigureMode.EFFICIENCY,
    "both": FigureMode.BOTH,
}

INFERENCE_MODES = {
    "suspect": InferenceMode.SUSPECT,
    "assume_offline": InferenceMode.ASSUME_OFFLINE,
}

# `sign` orients a terminal at its own node: +1 flows in, -1 flows out. It is
# only emitted when the YAML states it, so the component keeps deriving the
# usual orientation from the terminal's role.
SIGNS = {"in": 1, "out": -1}

_power = cv.float_with_unit("power", "(W|w|watt|Watt|watts)?")


def _ratio(value):
    """A dimensionless efficiency in (0, 1]."""
    return cv.float_range(min=0, max=1, min_included=False)(cv.float_(value))


def _percent(value):
    """A percentage on the 0..100 scale, matching Home Assistant's SOC."""
    if isinstance(value, str) and value.strip().endswith("%"):
        value = value.strip()[:-1]
    return cv.float_range(min=0, max=100)(cv.float_(value))


# --------------------------------------------------------------------------
# Home Assistant entities
#
# Every entity id in this config expands into a full `homeassistant` platform
# config, validated by that platform's own schema. The declared ids land in the
# validated tree, so ESPHome's ID pass resolves and registers them exactly as
# it would for a hand-written `sensor:` entry — which is what makes
# `cg.register_component()` legal for them in to_code().
# --------------------------------------------------------------------------
_HA_SENSOR_SCHEMA = sensor.sensor_schema(
    HomeassistantSensor, accuracy_decimals=1
).extend(HOME_ASSISTANT_IMPORT_SCHEMA)

_HA_TEXT_SENSOR_SCHEMA = text_sensor.text_sensor_schema(HomeassistantTextSensor).extend(
    HOME_ASSISTANT_IMPORT_SCHEMA
)


_ha_entity_counter = itertools.count(1)


def _ha_entity(schema, value):
    """Expand a bare entity id into a config the platform's schema accepts.

    The id has to be spelled out rather than left to `cv.GenerateID()`: an
    entity with neither a manual id nor a name is rejected by ESPHome's entity
    base schema. The counter keeps the ids unique when the same Home Assistant
    entity is referenced from two places.
    """
    if isinstance(value, dict):
        value = dict(value)
    else:
        value = {CONF_ENTITY_ID: cv.entity_id(value)}
    if CONF_ID not in value and CONF_NAME not in value:
        slug = re.sub(r"[^a-z0-9]+", "_", str(value[CONF_ENTITY_ID]).lower()).strip("_")
        value[CONF_ID] = f"pf_{slug}_{next(_ha_entity_counter)}"
    return schema(value)


def _ha_sensor(value):
    """`sensor.acin_power` -> a complete homeassistant sensor config."""
    return _ha_entity(_HA_SENSOR_SCHEMA, value)


def _ha_text_sensor(value):
    """`switch.acin` -> a complete homeassistant text sensor config.

    A switch is imported as a text sensor on purpose: the component needs a
    genuine tri-state, because only an explicit `off` opens an edge (§6.1).
    """
    return _ha_entity(_HA_TEXT_SENSOR_SCHEMA, value)


def _power_source(value):
    """`power:` is `meter:` plus the literal `auto` (§5)."""
    if isinstance(value, str) and value.strip().lower() == AUTO:
        return AUTO
    return _ha_sensor(value)


def _meter_list(value):
    return cv.All(cv.ensure_list(_ha_sensor), cv.Length(min=1))(value)


# --------------------------------------------------------------------------
# Terminals
# --------------------------------------------------------------------------
# `meter:` and `power:` are the same channel under two names — `meter:` reads
# naturally where a wattmeter physically sits, `power:` on terminals and
# consumers. `sum:` and `prefer:` are the multi-source forms and mean
# different things; see the module docstring.
_MEASUREMENT_KEYS = (CONF_METER, CONF_POWER, CONF_SUM, CONF_PREFER)

# `name:` is deliberately not in here: it is optional on a terminal and
# required on a consumer, and voluptuous markers collide on key equality, so
# each schema adds its own.
_TERMINAL_KEYS = {
    cv.Optional(CONF_METER): _ha_sensor,
    cv.Optional(CONF_POWER): _power_source,
    cv.Optional(CONF_SUM): _meter_list,
    cv.Optional(CONF_PREFER): _meter_list,
    cv.Optional(CONF_SWITCH): _ha_text_sensor,
    cv.Optional(CONF_ENERGY): _ha_sensor,
    cv.Optional(CONF_UNAVAILABLE, default="de_energized"): cv.enum(
        UNAVAILABLE_POLICIES, lower=True
    ),
    cv.Optional(CONF_ENABLED, default=True): cv.boolean,
    cv.Optional(CONF_CEILING): _power,
    cv.Optional(CONF_BASELINE): cv.one_of("learn", "none", lower=True),
    cv.Optional(CONF_STALE_AFTER): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_STALENESS, default=False): cv.boolean,
    cv.Optional(CONF_BIDIRECTIONAL): cv.boolean,
    cv.Optional(CONF_SIGN): cv.enum(SIGNS, lower=True),
}


def _validate_terminal(config):
    cv.has_at_most_one_key(*_MEASUREMENT_KEYS)(config)
    if config.get(CONF_BASELINE) == "learn" and config.get(CONF_POWER) != AUTO:
        raise cv.Invalid(
            "`baseline: learn` fits the inverter's standing draw from the node "
            "balance, so it only means anything on an edge declared "
            "`power: auto` (§6.6).",
            [CONF_BASELINE],
        )
    if CONF_CEILING in config and config.get(CONF_POWER) != AUTO:
        raise cv.Invalid(
            "`ceiling:` bounds a solved value, so it only applies to an edge "
            "declared `power: auto` (§4).",
            [CONF_CEILING],
        )
    return config


TERMINAL_SCHEMA = cv.All(
    cv.Schema({**_TERMINAL_KEYS, cv.Optional(CONF_NAME): cv.string, cv.Optional(CONF_ICON): cv.string}),
    _validate_terminal,
)

CONSUMER_SCHEMA = cv.All(
    cv.Schema(
        {
            **_TERMINAL_KEYS,
            cv.Required(CONF_NAME): cv.string,
            cv.Optional(CONF_ICON): cv.string,
            cv.Optional(CONF_SIDE, default="right"): cv.enum(SIDES, lower=True),
        }
    ),
    _validate_terminal,
)


# --------------------------------------------------------------------------
# Devices
# --------------------------------------------------------------------------
def _terminal_ref(value):
    """`inv.output` — a device-qualified terminal reference (§5)."""
    value = cv.string_strict(value)
    parts = value.split(".")
    names = sorted(set(TERMINAL_ROLES) | {CONF_METER})
    if len(parts) != 2 or not parts[0] or parts[1] not in names:
        raise cv.Invalid(
            f"Expected a device-qualified terminal reference such as "
            f"'inv.output', got '{value}'. The terminal must be one of "
            f"{', '.join(names)}."
        )
    return value


DEVICE_SCHEMA = cv.Schema(
    {
        # A graph node name, not an ESPHome id — it is never a C++ variable.
        cv.Required(CONF_ID): cv.string_strict,
        cv.Required(CONF_KIND): cv.enum(DEVICE_KINDS, lower=True),
        cv.Optional(CONF_NAME): cv.string,
        cv.Optional(CONF_ICON): cv.string,
        cv.Optional(CONF_VOLTAGE): _ha_sensor,
        cv.Optional(CONF_SWITCH): _ha_text_sensor,
        cv.Optional(CONF_SOC): _ha_sensor,
        cv.Optional(CONF_CAPACITY): _ha_sensor,
        cv.Optional(CONF_SOURCE): _terminal_ref,
        # Sugar for a plain device meter: it becomes a GENERIC terminal. Needed
        # when a `tap:` hangs off the node (§5).
        cv.Optional(CONF_METER): _ha_sensor,
        cv.Optional(CONF_TERMINALS): cv.Schema(
            {cv.Optional(role): TERMINAL_SCHEMA for role in TERMINAL_ROLES}
        ),
        # Tapping this device's box runs whatever the YAML puts here. There is
        # deliberately no `page:` key: the component must not know what an LVGL
        # page is, so a panel laid out differently binds something else (§7).
        cv.Optional(CONF_ON_CLICK): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DeviceClickTrigger)}
        ),
    }
)

FONTS_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_F_S10): cv.use_id(font.Font),
        cv.Required(CONF_F_S14): cv.use_id(font.Font),
        cv.Required(CONF_F_S16): cv.use_id(font.Font),
        cv.Required(CONF_F_S18): cv.use_id(font.Font),
        cv.Required(CONF_F_S20): cv.use_id(font.Font),
        cv.Required(CONF_F_S22): cv.use_id(font.Font),
        cv.Required(CONF_F_M12): cv.use_id(font.Font),
        cv.Required(CONF_F_M14): cv.use_id(font.Font),
        cv.Required(CONF_F_M16): cv.use_id(font.Font),
        cv.Required(CONF_F_M22): cv.use_id(font.Font),
        cv.Required(CONF_F_M28): cv.use_id(font.Font),
        cv.Required(CONF_F_M72): cv.use_id(font.Font),
        cv.Required(CONF_F_ICON): cv.use_id(font.Font),
    }
)

GRID_LOSS_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ENABLED, default=True): cv.boolean,
        cv.Optional(CONF_MODE, default="suspect"): cv.enum(
            INFERENCE_MODES, lower=True
        ),
        cv.Optional(CONF_MIN_DISCHARGE, default="50W"): _power,
        cv.Optional(CONF_HOLD, default="20s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_REQUIRE_STALE_GRID, default=False): cv.boolean,
    }
)

INFERENCE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_GRID_LOSS_FROM_BATTERY, default={}): GRID_LOSS_SCHEMA,
    }
)


# --------------------------------------------------------------------------
# Topology validation
#
# The headline check: exactly one `power: auto` per node. A node's edges are
# not all in one YAML block — the bus collects the inverter's `output` terminal
# plus every consumer — so this has to look at `devices:` and `consumers:`
# together, which makes it a top-level validator.
# --------------------------------------------------------------------------
def _terminals_of(device):
    """(role_key, terminal_config) for one device, in emission order."""
    out = []
    if CONF_METER in device:
        out.append((CONF_METER, {CONF_METER: device[CONF_METER]}))
    for role, term in device.get(CONF_TERMINALS, {}).items():
        out.append((role, term))
    return out


def _validate_topology(config):
    devices = config[CONF_DEVICES]
    consumers = config[CONF_CONSUMERS]

    by_id = {}
    for i, device in enumerate(devices):
        did = device[CONF_ID]
        if did in by_id:
            raise cv.Invalid(
                f"Duplicate device id '{did}'.", [CONF_DEVICES, i, CONF_ID]
            )
        by_id[did] = device

    buses = [d[CONF_ID] for d in devices if d[CONF_KIND] == "bus"]
    if consumers and len(buses) != 1:
        raise cv.Invalid(
            f"`consumers:` attach to the bus, so exactly one device with "
            f"`kind: bus` is required; found {len(buses)}.",
            [CONF_CONSUMERS],
        )

    # node id -> [(edge label, is_auto, config path)]
    nodes = {did: [] for did in by_id}

    for i, device in enumerate(devices):
        did = device[CONF_ID]
        for role, term in _terminals_of(device):
            if not term.get(CONF_ENABLED, True):
                continue
            path = [CONF_DEVICES, i, CONF_TERMINALS, role]
            nodes[did].append(
                (f"{did}.{role}", term.get(CONF_POWER) == AUTO, path)
            )

    # `source: inv.output` — the same edge also hangs off the referencing node.
    for i, device in enumerate(devices):
        ref = device.get(CONF_SOURCE)
        if ref is None:
            continue
        path = [CONF_DEVICES, i, CONF_SOURCE]
        src_id, src_role = ref.split(".")
        source = by_id.get(src_id)
        if source is None:
            raise cv.Invalid(f"`source: {ref}` names no known device.", path)
        term = dict(_terminals_of(source)).get(src_role)
        if term is None:
            raise cv.Invalid(
                f"`source: {ref}` — device '{src_id}' declares no '{src_role}' "
                f"terminal.",
                path,
            )
        if not term.get(CONF_ENABLED, True):
            raise cv.Invalid(
                f"`source: {ref}` points at a terminal declared "
                f"`enabled: false`.",
                path,
            )
        nodes[device[CONF_ID]].append((ref, term.get(CONF_POWER) == AUTO, path))

    for i, consumer in enumerate(consumers):
        if not consumer[CONF_ENABLED]:
            continue
        nodes[buses[0]].append(
            (
                f"consumer '{consumer[CONF_NAME]}'",
                consumer.get(CONF_POWER) == AUTO,
                [CONF_CONSUMERS, i, CONF_POWER],
            )
        )

    for node, edges in nodes.items():
        autos = [e for e in edges if e[1]]
        if len(autos) > 1:
            first, second = autos[0], autos[1]
            raise cv.Invalid(
                f"Node '{node}' has two edges declared `power: auto`: "
                f"{first[0]} and {second[0]}. A node's balance can solve for "
                f"exactly one unknown, so two of them is unsolvable — the "
                f"component must reject this here rather than draw a made-up "
                f"number at runtime (§4, §6.9). Give one of them a meter.",
                second[2],
            )

    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(PowerFlow),
            # An empty `obj` declared on the LVGL page — the only coupling to
            # the page layout (§8).
            cv.Required(CONF_PARENT_ID): cv.use_id(lv_obj_t),
            cv.Optional(
                CONF_AVERAGE_WINDOW, default="60s"
            ): cv.positive_time_period_milliseconds,
            # What the screen shows for a measured terminal. Much shorter than
            # the averaging window on purpose: the long one exists so that a
            # difference between two meters sampled at different instants means
            # something, and a directly measured value is never subtracted.
            cv.Optional(
                CONF_DISPLAY_WINDOW, default="10s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_IDLE_BELOW, default="3W"): _power,
            # Consumed by cg.register_component(), which emits
            # set_update_interval() for us.
            cv.Optional(
                CONF_UPDATE_INTERVAL, default="250ms"
            ): cv.positive_time_period_milliseconds,
            # `binary_sensor: platform: status` — the only sound "HA is
            # connected" flag (§2).
            cv.Optional(CONF_STATUS): cv.use_id(binary_sensor.BinarySensor),
            cv.Optional(CONF_SOC_CUTOFF): _percent,
            # |P_battery| below which the battery counts as at rest. One key,
            # not two: it gates both the headline energy figure and the
            # baseline fit's sampling condition (§6.6, §6.8).
            cv.Optional(CONF_BATTERY_DEADBAND, default="15W"): _power,
            # Which of the two complementary headline figures to display.
            cv.Optional(CONF_FIGURE, default="auto"): cv.enum(
                FIGURE_MODES, lower=True
            ),
            # No default on purpose: the runtime estimate is the most
            # important line on the screen, and without a measured figure it
            # must stay a dash rather than inherit a guessed 0.9 (§6.9).
            cv.Optional(CONF_DISCHARGE_ETA): _ratio,
            cv.Optional(CONF_STYLE, default={}): FONTS_SCHEMA,
            cv.Optional(CONF_INFERENCE, default={}): INFERENCE_SCHEMA,
            cv.Required(CONF_DEVICES): cv.All(
                cv.ensure_list(DEVICE_SCHEMA), cv.Length(min=1)
            ),
            cv.Optional(CONF_CONSUMERS, default=[]): cv.ensure_list(CONSUMER_SCHEMA),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate_topology,
)


# --------------------------------------------------------------------------
# Codegen
# --------------------------------------------------------------------------
async def _new_ha_sensor(conf):
    var = await sensor.new_sensor(conf)
    await cg.register_component(var, conf)
    setup_home_assistant_entity(var, conf)
    return var


async def _new_ha_text_sensor(conf):
    var = await text_sensor.new_text_sensor(conf)
    await cg.register_component(var, conf)
    setup_home_assistant_entity(var, conf)
    return var


def _channel(conf):
    """Normalise the four measurement keys into (is_auto, meters, combine).

    `meter:` and `power:` are one channel under two names; `sum:` adds parallel
    branches; `prefer:` fails over down a list of readings of the same line.
    """
    if conf.get(CONF_POWER) == AUTO:
        return True, [], None
    for key in (CONF_METER, CONF_POWER):
        if key in conf:
            return False, [conf[key]], MeterCombine.SUM
    if CONF_SUM in conf:
        return False, list(conf[CONF_SUM]), MeterCombine.SUM
    if CONF_PREFER in conf:
        return False, list(conf[CONF_PREFER]), MeterCombine.PREFER
    return False, [], None


async def _attach_terminal(var, index, conf, role_key):
    """Everything that can hang off a terminal, by index."""
    is_auto, meters, combine = _channel(conf)

    for meter in meters:
        cg.add(var.add_terminal_meter(index, await _new_ha_sensor(meter)))
    if combine is not None and meters:
        cg.add(var.set_terminal_combine(index, combine))

    if is_auto:
        ceiling = conf.get(CONF_CEILING)
        cg.add(
            var.set_terminal_auto(
                index, cg.RawExpression("NAN") if ceiling is None else ceiling
            )
        )

    if (sw := conf.get(CONF_SWITCH)) is not None:
        cg.add(var.set_terminal_switch(index, await _new_ha_text_sensor(sw)))
    if (energy := conf.get(CONF_ENERGY)) is not None:
        cg.add(var.set_terminal_energy(index, await _new_ha_sensor(energy)))

    if (policy := conf.get(CONF_UNAVAILABLE)) is not None:
        cg.add(var.set_terminal_unavailable(index, policy))
    if not conf.get(CONF_ENABLED, True):
        cg.add(var.set_terminal_enabled(index, False))
    if (stale_after := conf.get(CONF_STALE_AFTER)) is not None:
        cg.add(var.set_terminal_stale_after(index, stale_after))
        if conf.get(CONF_STALENESS) or CONF_STALE_AFTER in conf:
            cg.add(var.set_terminal_detect_stale(index, True))
    if conf.get(CONF_BASELINE) == "learn":
        cg.add(var.set_terminal_baseline_learn(index, True))

    # The battery terminal carries direction in the sign of its reading; say so
    # explicitly rather than leaving it to be inferred from the role.
    bidirectional = conf.get(CONF_BIDIRECTIONAL)
    if bidirectional is None and role_key == "battery":
        bidirectional = True
    if bidirectional is not None:
        cg.add(var.set_terminal_bidirectional(index, bidirectional))

    # Only emitted when the YAML states it: the default orientation follows
    # from the terminal's role and belongs to the component, not here.
    if (sign := conf.get(CONF_SIGN)) is not None:
        cg.add(var.set_terminal_sign(index, sign))


async def _style_to_code(var, config):
    """Twelve rasters plus the icon face, named for the spec's two tables so the
    transcription can be checked line by line (§8). Required rather than
    optional: a missing face is a blank label, and a blank label on a wall panel
    is indistinguishable from a value that failed to arrive."""
    fonts = config[CONF_STYLE]
    for key in (CONF_F_S10, CONF_F_S14, CONF_F_S16, CONF_F_S18, CONF_F_S20, CONF_F_S22, CONF_F_M12, CONF_F_M14, CONF_F_M16, CONF_F_M22, CONF_F_M28, CONF_F_M72, CONF_F_ICON):
        fnt = await cg.get_variable(fonts[key])
        cg.add(cg.RawStatement(f"{var}->style().{key} = {fnt}->get_lv_font();"))


def _ensure_ha_platform_sources():
    """Make sure the `homeassistant` platform C++ lands in the build tree.

    Source files are copied per platform present in the config, and the
    subscriptions above are created in codegen rather than from a `sensor:`
    block. Appending the platform marker here — after every to_code() has been
    scheduled, before writer.copy_src_tree() reads the config again — is what
    keeps a config that declares no homeassistant entities of its own from
    failing to link. A no-op when the user already has such a block.
    """
    for domain in ("sensor", "text_sensor"):
        entries = CORE.config.get(domain)
        if not isinstance(entries, list):
            continue
        if any(
            isinstance(e, dict) and e.get(CONF_PLATFORM) == "homeassistant"
            for e in entries
        ):
            continue
        entries.append({CONF_PLATFORM: "homeassistant"})


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    parent = await cg.get_variable(config[CONF_PARENT_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_average_window(config[CONF_AVERAGE_WINDOW]))
    cg.add(var.set_display_window(config[CONF_DISPLAY_WINDOW]))
    cg.add(var.set_idle_below(config[CONF_IDLE_BELOW]))
    cg.add(var.set_battery_deadband(config[CONF_BATTERY_DEADBAND]))
    cg.add(var.set_figure_mode(config[CONF_FIGURE]))
    if (soc_cutoff := config.get(CONF_SOC_CUTOFF)) is not None:
        cg.add(var.set_soc_cutoff(soc_cutoff))
    if (discharge_eta := config.get(CONF_DISCHARGE_ETA)) is not None:
        # Absent, PowerFlow::discharge_eta_ stays NaN and the runtime estimate
        # shows a dash rather than a guess (§6.9).
        cg.add(var.set_discharge_eta(discharge_eta))
    if (status := config.get(CONF_STATUS)) is not None:
        cg.add(var.set_status_sensor(await cg.get_variable(status)))

    await _style_to_code(var, config)

    grid_loss = config[CONF_INFERENCE][CONF_GRID_LOSS_FROM_BATTERY]
    cg.add(
        var.configure_inference(
            grid_loss[CONF_ENABLED],
            grid_loss[CONF_MODE],
            grid_loss[CONF_MIN_DISCHARGE],
            grid_loss[CONF_HOLD],
            grid_loss[CONF_REQUIRE_STALE_GRID],
        )
    )

    # Indices are assigned in declaration order and are stable for the life of
    # the component (power_flow.h), so the plan below mirrors what the builder
    # hands back at runtime.
    device_index = {}
    terminal_index = {}
    next_terminal = 0
    for i, device in enumerate(config[CONF_DEVICES]):
        device_index[device[CONF_ID]] = i
        for role, _ in _terminals_of(device):
            terminal_index[(device[CONF_ID], role)] = next_terminal
            next_terminal += 1

    for device in config[CONF_DEVICES]:
        did = device[CONF_ID]
        index = device_index[did]
        cg.add(var.add_device(device[CONF_KIND], did, device.get(CONF_NAME, did)))
        if CONF_ICON in device:
            cg.add(var.set_device_icon(index, device[CONF_ICON]))

        if (sw := device.get(CONF_SWITCH)) is not None:
            cg.add(var.set_device_switch(index, await _new_ha_text_sensor(sw)))
        if (voltage := device.get(CONF_VOLTAGE)) is not None:
            cg.add(var.set_device_voltage(index, await _new_ha_sensor(voltage)))
        if (soc := device.get(CONF_SOC)) is not None:
            cg.add(var.set_device_soc(index, await _new_ha_sensor(soc)))
        if (capacity := device.get(CONF_CAPACITY)) is not None:
            cg.add(var.set_device_capacity(index, await _new_ha_sensor(capacity)))

        # The trigger is the whole of the component's knowledge about a tap: it
        # fires, and the actions the YAML listed run. What they do is the
        # panel's business, not the diagram's (§7).
        for conf in device.get(CONF_ON_CLICK, []):
            trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID])
            cg.add(var.set_device_on_click(index, trigger))
            await automation.build_automation(trigger, [], conf)

        for role, term in _terminals_of(device):
            t_index = terminal_index[(did, role)]
            cg.add(
                var.add_terminal(
                    index,
                    TERMINAL_ROLES.get(role, TerminalRole.GENERIC),
                    term.get(CONF_NAME, ""),
                )
            )
            await _attach_terminal(var, t_index, term, role)

    # `bus: source: inv.output` — resolved to a terminal index here (§5).
    for device in config[CONF_DEVICES]:
        if (ref := device.get(CONF_SOURCE)) is None:
            continue
        src_id, src_role = ref.split(".")
        cg.add(
            var.set_device_source(
                device_index[device[CONF_ID]], terminal_index[(src_id, src_role)]
            )
        )

    for consumer in config[CONF_CONSUMERS]:
        index = next_terminal
        next_terminal += 1
        cg.add(var.add_consumer(consumer[CONF_NAME], consumer[CONF_SIDE]))
        if CONF_ICON in consumer:
            cg.add(var.set_terminal_icon(index, consumer[CONF_ICON]))
        await _attach_terminal(var, index, consumer, None)

    _ensure_ha_platform_sources()
