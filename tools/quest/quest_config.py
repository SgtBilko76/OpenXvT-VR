#!/usr/bin/env python3
"""Swap in the Quest/PICO Touch controller defaults when staging the APK runtime.

OpenXvT reads its shipped gamepad defaults from resources/config.yaml
(`input.gamepad_defaults`). The VR build exposes both Touch controllers as one
virtual gamepad (src/xvt_vr/xvt_vr.c, vr_update_gamepad; the layout follows
the X-Wing Alliance VR port):

  left stick       -> rightx / righty (pitch / yaw)
  right stick x    -> leftx (roll)
  right stick y    -> lefty (throttle lever, stays where it is left)
  triggers         -> lefttrigger / righttrigger
  A / B / X / Y    -> south / east / west / north
  right grip       -> rightshoulder
  right / left stick click -> guide / rightstick
  Menu             -> start
  left grip held (shift layer): A/B/X/Y -> dpaddown/dpadright/dpadleft/dpadup,
                   left trigger -> leftshoulder, right grip -> misc1,
                   right stick click -> leftstick, left stick click -> back

Usage: quest_config.py <staged resources/config.yaml>
Only the VR APK is affected; desktop builds keep the upstream defaults.
"""
import re
import sys

QUEST_GAMEPAD_DEFAULTS = """\
  gamepad_defaults:
    axes:
      yaw: {source: rightx, invert: false, deadzone: 0.08}
      pitch: {source: righty, invert: false, deadzone: 0.08}
      roll: {source: leftx, invert: false, deadzone: 0.15}
      throttle: {source: lefty, invert: false}
    buttons:
      fire_weapon:
        - {axis: righttrigger, direction: positive, threshold: 0.5}
      target_roll_modifier:
        - {axis: lefttrigger, direction: positive, threshold: 0.5}
      auto_target: rightshoulder
      target_next: south
      cycle_weapon_group: east
      target_my_attacker: west
      target_nearest_fighter_or_mine: north
      cycle_weapon_firing_mode: rightstick
      hyperspace_or_map_help: guide
      escape: start
      cycle_cannon_recharge_rate: dpaddown
      cycle_shield_recharge_rate: dpadright
      cycle_shield_mode: dpadleft
      xfer_cannon_to_shields: dpadup
      toggle_beam: leftshoulder
      target_component_next: misc1
      match_speed: leftstick
      target_objective: back
"""


def main() -> int:
    path = sys.argv[1]
    text = open(path, encoding="utf-8").read()
    # The block runs from "  gamepad_defaults:" to the next key at the same
    # indentation (two spaces).
    pattern = re.compile(r"^  gamepad_defaults:\n(?:^(?:    .*)?\n)*", re.M)
    if not pattern.search(text):
        print("quest_config: input.gamepad_defaults not found", file=sys.stderr)
        return 1
    text = pattern.sub(QUEST_GAMEPAD_DEFAULTS, text, count=1)
    open(path, "w", encoding="utf-8").write(text)
    print(f"quest_config: Touch controller defaults written to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
