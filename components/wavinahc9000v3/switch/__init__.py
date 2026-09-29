import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_CHANNEL

from .. import WavinAHC9000, ns

CONF_PARENT_ID = "wavinahc9000v3_id"
CONF_TYPE = "type"

WavinLockSwitch = ns.class_("WavinLockSwitch", switch.Switch)
LockKind = ns.enum("LockKind")

# Both switches act on the PACKED CONFIGURATION register of the channel (spec 1.6.9).
#   child_lock -> INT LOCK  (bit 11): the thermostat's own lock (padlock in the display, dial disabled).
#                 Verified on Wavin AHC 9000 hardware; the spec's wording ("service menu") does not match.
#   ctrl_lock  -> CTRL LOCK (bit 10): menu lock. Does not lock the dial by itself; together with child_lock
#                 the thermostat is locked completely and can only be unlocked from the bus.
LOCK_TYPES = {
    "child_lock": LockKind.LOCK_CHILD,
    "ctrl_lock": LockKind.LOCK_CTRL,
}

CONFIG_SCHEMA = switch.switch_schema(WavinLockSwitch).extend(
    {
        cv.GenerateID(CONF_PARENT_ID): cv.use_id(WavinAHC9000),
        cv.Required(CONF_CHANNEL): cv.int_range(min=1, max=16),
        cv.Optional(CONF_TYPE, default="child_lock"): cv.enum(LOCK_TYPES, lower=True),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_PARENT_ID])
    var = await switch.new_switch(config)
    cg.add(var.set_parent(hub))
    cg.add(var.set_channel(config[CONF_CHANNEL]))
    cg.add(var.set_lock_kind(config[CONF_TYPE]))
    cg.add(hub.add_channel_lock_switch(var))
    # Optimistic publish handled in write_state override; hub refresh will reconcile.
