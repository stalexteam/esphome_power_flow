#pragma once
//
// The palette, transcribed from DEV/UI/POWER_FLOW_UI_SPEC.md §2.
//
// Shared by both screens and deliberately not configurable: there is a document
// that specifies every colour, and a YAML key per colour would be twenty-six
// ways to disagree with it. Two families carry meaning — role (grid / pv /
// battery / load) and state (idle / off / no-data) — and green and amber are
// reserved for the battery-and-solar axis, where they mean *good* and *worth a
// look*.
//
#include <cstdint>

namespace esphome {
namespace power_flow {

// ---------------------------------------------------------------------------
namespace pal {
static const uint32_t bg = 0x0E1720;           ///< screen, badge fill
static const uint32_t card = 0x16222E;         ///< live node fill
static const uint32_t card_dead = 0x131C25;    ///< off / no-data node fill
static const uint32_t border = 0x27394B;       ///< live node border
static const uint32_t border_dead = 0x23303C;  ///< off / no-data node border
static const uint32_t divider = 0x22323F;      ///< rule in the inverter, empty ring track
static const uint32_t text = 0xE8F0F6;         ///< node names, values
static const uint32_t text_dim = 0x6F8699;     ///< units, captions, OFF
static const uint32_t text_off = 0x5C7285;     ///< names of de-energized nodes, the dash
static const uint32_t icon_off = 0x4C6274;     ///< icons of de-energized nodes
static const uint32_t grid = 0x5B9DFF;         ///< grid icon, grid dots
static const uint32_t grid_line = 0x2E5B8F;    ///< grid edges
static const uint32_t grid_val = 0x8FC0FF;     ///< grid badge text
static const uint32_t tap_line = 0x3A5570;     ///< City grid -> OnGrid edge
static const uint32_t tap_val = 0xA8C0D2;      ///< its badge text, OnGrid icon
static const uint32_t pv = 0xF5C452;           ///< PV icon — identifies a device, not a state
static const uint32_t pv_line = 0x7A6A32;      ///< kept per §2; the PV *edge* is green now
static const uint32_t pv_border = 0x3D3A28;    ///< PV card border
static const uint32_t batt = 0x4FD98A;         ///< SOC ring, charge badge, PV badge and dots
static const uint32_t batt_line = 0x2E6B47;    ///< charging edge, PV edge
static const uint32_t discharge = 0xF5C452;        ///< discharge badge text and dots
static const uint32_t discharge_line = 0x7A6A32;   ///< discharging edge
static const uint32_t load = 0x7ED4F0;         ///< consumer icons, bus badges, bus dots
static const uint32_t load_line = 0x3E7FA8;    ///< bus and consumer edges
static const uint32_t idle_line = 0x28404F;    ///< edge at 0 W
static const uint32_t off_line = 0x2A3540;     ///< deliberately open edge
static const uint32_t nodata_line = 0x3A4450;  ///< de-energized edge
static const uint32_t alert = 0xF0A94D;        ///< the ✕, and the three states §2 does not draw
}  // namespace pal

}  // namespace power_flow
}  // namespace esphome
