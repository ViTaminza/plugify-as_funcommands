# as_funcommands (Plugify)

Fun commands for admins: gravity, hp, armor, scale, speed, god mode, freeze, blind, bury and unbury, as console commands (mm_gravity, mm_hp, mm_armor, mm_scale, mm_speed, mm_god, mm_freezy, mm_unfreezy, mm_blind, mm_unblind, mm_bury, mm_unbury) and as the 'players' category of the admin menu (target picker + value menus, like the original). Every command needs its @admin/<name> permission from the admin_system core. A port of the Metamod:Source module 'FunCommands' of the Pisex cs2-admin_system plugin.

Plugify port (fork) of **FunCommands** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, admin_system, hudmenu (optional)

## Installation
Download `as_funcommands-1.1.2.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_funcommands`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

