# Witch It+

The shared foundation for Witch It+: the Witch It UE4SS host, native producer, runtime, Core and UIKit. These components stay enabled together. Install Core first, then any features you want.

- [WitchItPlus-FaPplus](https://github.com/monroth/WitchItPlus-FaPplus)
- [WitchItPlus-EditorTweaks](https://github.com/monroth/WitchItPlus-EditorTweaks)
- [WitchItPlus-MatchRulesPP](https://github.com/monroth/WitchItPlus-MatchRulesPP)

Install

1. Close Witch It. Back up your game mod folder and saved maps.
2. Download the ZIP from this repository's Releases page. For the whole pack, download Core and each of the three feature ZIPs linked above (or on the Core page).
3. Open the game's local files through Steam and find `WitchIt/Binaries/Win64`.
4. Extract the ZIP's `Win64` contents into that game folder, preserving directories. Install Core before features. This pack includes its UE4SS loader; an existing custom UE4SS installation needs a separate backup before replacement.
5. Start the game. Open Mods to choose features. Core, UIKit and the runtime always remain enabled. Do not register individual WIR packages as UE4SS Lua mods.

Update

Close the game, back up your current mod files, then install matching Core and feature release ZIPs in the same way. Preserve your personal settings and saved maps. The release manifest lists managed files and explicit obsolete files; remove an obsolete file only if it belongs to your previous pack installation. Do not delete whole mod folders or saved-map companions.

Automatic updates are optional and off by default. This release uses the manual update steps above; the in-game update option is not operational yet. Preparing host versions never joins a server automatically.

All players in a modded session should use the same required package versions. Keep your backup to restore the previous pack with the game closed.
