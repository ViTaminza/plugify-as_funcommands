# as_funcommands (Plugify)

Admin fun commands for admin_system (port of the Pisex Metamod:Source module 'FunCommands'): gravity, hp, armor, scale, speed, god mode, freeze, blind, bury and unbury as console commands (mm_gravity, mm_hp, mm_armor, mm_scale, mm_speed, mm_god, mm_freezy, mm_unfreezy, mm_blind, mm_unblind, mm_bury, mm_unbury) and as the 'players' category of the admin menu (target picker + value menus, like the original). Every command and item needs its @admin/<name> permission from the admin_system core; the admin-data readiness of the caller is checked before every permission decision. Written directly against s2sdk: no pisexcompat and no shared wrapper library. The menus use the s2sdk menu manager; the player's own '!menu' type comes from the optional hudmenu plugin API, otherwise the Pisex default (the 'hud' type when its backend is registered, else 'button'). No exported methods.

Plugify port (fork) of **FunCommands** by Pisex (Metamod (C++)).
Original: https://github.com/Pisex/cs2-admin_system_modules
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.2.0
- Plugify dependencies: s2sdk (>=2.18.2), admin_system (>=1.1.0), hudmenu (>=1.0.0, optional)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `as_funcommands-1.2.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/as_funcommands`.

`plugins/admin_system` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

