# Signed updates, protocol 1

The proxy calls `activate()` before loading UE4SS.dll. This is bounded local
file I/O and pure verification: no network, waits, thread/process creation or
engine calls under the loader lock. A background executable starts only from a
main-menu controller. It uses WinHTTP, TLS 1.2, HTTPS-only redirects and short
network timeouts. No Lua HTTP or downloaded scripts are executed.

Catalog and manifest: the existing published schema-1 JSON. Minisign `ED`
(BLAKE2b-512 prehash followed by standard Ed25519), including the trusted-comment
signature and key ID, is verified using Monocypher 4.0.2 optional **Ed25519**
(not Monocypher's different default EdDSA). SHA256 uses PicoSHA2 1.0.1. ZIP
extraction uses miniz 3.0.2. JSON parsing uses nlohmann/json 3.11.3 with duplicate
keys and excessive depth rejected. Vendor origins/hashes/licenses are adjacent.

The sole production key is compiled into `core.cpp`; a public-key file is only
for inspection. The worker has four fixed public repositories, no URL/key file
or environment override. `WIP_TEST_PUBLIC_KEY` / `WIP_TEST_ORIGIN` are **compile
options**, accepted only in a separate test build. No test key, executable,
fixture, endpoint override, or secret is part of the shipping stage.

Verified archives are expanded to `ue4ss/.witchit-updater/download/`, verified
again, then renamed to `pending/`. Activation verifies signed inventories,
archive and file hashes again. An interprocess lock excludes an old worker.
Every mutation is journaled before it starts; old files are renamed to backup,
then verified replacements are atomically published. Loaded DLLs (including the
proxy) are renamed, never overwritten/deleted. Recovery renames an executing
new image aside before restoring its predecessor. Backups survive successful
activation. Partial download/activation, crash or absent runtime receipt causes
recovery before packages load. Failed releases are quarantined. A trial is accepted
when the game reached its menu with the mods up: the menu's update check writes
`ready.marker`, and the next launch confirms. A launch that never reached the menu
rolls back. How the game exits is not considered.

Only signed explicit file removals are applied. No directory removals; no
private packages, user maps, save data, INI replacement, storage/presets,
settings or registration replacement. Seeds create missing files only; existing
mods.txt/mods.json and enable choices are retained. Enabled public features
update as one ABI-compatible set; a Core release must have newer matching
feature releases for every enabled public feature. The runtime publishes
`enabled_packages` in `ue4ss/witchit-update-settings.json` before the menu check
and when package switches change. Disabled or absent packages cause no catalog,
manifest or payload fetch and cannot activate a previously staged update. Older
settings without the switches conservatively check only Core (including its
bundled UIKit and Runtime). Disabled packages retain their installed versions;
re-enabling one requires a release compatible with the current Core ABI. Equal/older versions are
not automatically installed. Publisher preparation is `tooling/updater_release.py`;
it never signs or reads a private key, preserves historical catalog entries,
and refuses replacement of an existing version. Lead signs the resulting
catalog/manifest and publishes the five assets to each release.

Dependencies: a signed Core manifest may carry `dependencies`, a list of
`{id, canonical, repo}`, in `candidate/DEPENDENCIES.json` for the publisher tool.
Each is a public package on the same signing key, under
`ue4ss/Mods/<canonical>/`. Staging a Core update fetches them in the same
transaction, and the update stops if one is missing, so a package split reaches
players who update. The worker keeps the list in `.witchit-updater/packages.json`.
A dependency is on unless the player's `enabled_packages` switches it off.

Threat model: protects against network/CDN tampering, stale catalogs after a
seen sequence, hostile archives, interrupted writes and failed releases. It
trusts the pinned release signer and the local account/game directory; it is
not a sandbox against a malicious local administrator, another executable mod,
a compromised signing key, or a publisher who signs malicious allowed code.
Initial catalog replay is bounded by installed versions, then persistent seen
sequences. OS/drive failure preventing rollback stops mod startup with an error
instead of loading a mixed tree. Windows/Proton image-rename behavior is a live
acceptance check; portable tests establish the same transaction, not that OS.
