# Gap r1: UI, HUD, minimap, spawn screen, scoreboard and localisation strings

Date 2026-10-14. Gap slug `ui-hud-minimap-localisation` (critic-r1.json). This report covers what the retail install holds
for UI, how each piece can be read at runtime, and which HUD route to take. It updates PORTING-RESEARCH §3.8 (lines 713-722),
which chose "own Godot UI, no EA art" without this inventory, and closes handoff HANDOFF.md §10 "Loc chunk format" and
"minimap texture location".

Tags: **[V]** verified here (re-run on 2026-10-14 unless noted), **[R]** report claim from another document,
**[I]** inferred.

**Scratch data** (no BF3 data is in the repo): `S` = `C:\Users\olku\AppData\Local\Temp\claude\C--Users-olku-BF3-reimplementation\192b4390-9a90-436b-abe1-9117ba9cb3e5\scratchpad\ui`.

- **Where the scratch work came from.** An earlier run of this gap (2026-10-10) left its work in `S` without a report, and this
  run reuses it:
  - an extended copy of the ebx-dump probe (`S\ebx-dump\src\ui.rs`: commands `bundles`, `rawres`, `rawchunk`, `tex`, `texhead`, `toc`, `locchunk`);
  - Python analysis scripts (`S\scripts\*.py`);
  - the decoded images (`S\img`, `S\tex`).
- **What this run re-ran.** I re-ran every number marked [V] below from those files, or against the install itself.

---

## 0. Answer in five lines

1. **Strings: solved [V].**
   - The English string table is a `UITextDatabase` (`localization/us_loc`, superbundle `Win32/Globals`). It points at two
     chunks stored in `Loc\en.sb`, and the patched copy in `Update\Patch\Data\Win32\Loc\en.sb` takes precedence.
   - Format: a `0x39000` string binary plus a `0x39001` histogram (character table). There are 15,515 entries, keyed by a
     32-bit hash of the SID: `h = 0xFFFFFFFF; for c: h = h*33 + c`.
   - 1,247 of the 1,271 SIDs referenced from UI EBX resolve.
2. **Minimap: solved [V].**
   - Every one of the 29 MP levels has an L8 `_satellite` image (512-2048²), building and vegetation distance fields, and
     499 per-mode combat-area masks (128² L8 signed-distance fields).
   - Mapping: `u = 0.5 − (x − Cx)/S`, `v = 0.5 − (z − Cz)/S`, where `C` is `UIMinimapData.WorldCenter` and `S` is
     `WorldSize`, both taken from the level's own `LevelData`. The match is 100% against flags/HQs and bot waypoints.
3. **The HUD and menus are Scaleform, and their logic is AS2 [V].**
   - 434 SwfMovies. They use the GFx v8 headers `CFX` (compressed) and `GFX`.
   - About 1.1 MB of AS2 bytecode (DoAction/DoInitAction) and no AS3. The behaviour lives in AS2 `__Packages.*` classes
     (e.g. `crosshairdefault` has no shapes but 10 KB of AS2).
   - The art sits in external DxTexture atlases: the SWFs reference them by name and cut icons out with `GFX_DefineSubImage`
     rectangles.
   - A SWF renderer that is useful needs an AVM1 interpreter, so it costs months.
4. **Many HUD icons exist outside SWFs [V].**
   - `ui/static/hud3dicons` holds 161 `UIHudIcon` types (93 with UV states) mapped onto `hud3diconswin32` (ARGB8888, 256×1024).
   - Weapon icons are plain DxTextures referenced from EBX by `IconTexturePath` (e.g. `UI/Art/Persistence/Weapons/M16A4_small`, DXT5 64×16).
   - Level thumbnails and loading images are plain DxTextures.
   - The `iconsingame` atlas (DXT5 4096×128) is cut by 162 parseable sub-image rectangles plus 427 frame labels.
5. **Fonts are SWF `DefineFont3` outlines, not TTF/OTF [V]**: Purista EA Medium/Semibold with 443 glyphs, and Venice-Medium/SemiBold with 33 glyphs.

**Recommendation (route A+).**
- Own C++ Godot `Control` layout, fed by a sim-side data model that mirrors BF3's `UI*CompData` data-source names (§5).
- Art at runtime from the install: textures, atlases with their EBX/SWF rectangles, the strings, and the fonts turned into
  glyphs at runtime.
- Procedural drawing wherever BF3 itself draws in AS2 (crosshairs, compass ticks).
- No SWF renderer.
- Own fallback art only when the install lacks the item.

This reverses the "no EA art" half of §3.8 for in-game use. It stays within the read-as-is principle and legal practice
08 §3.8 #2: everything is read at runtime and nothing goes into the repo.

---

## 1. Inventory of UI data (task 1)

**Source of the inventory [V].** `S\ui-entries.tsv` was built by `S\scripts\inv.py` from `...\scratchpad\probe-ebx\all-entries.tsv`. It covers
every ebx/res entry whose name starts with `ui/`, or matches `levels/*/ui/` or `*/ui/*`.

**Counts [V].**
- 6,000 unique EBX partitions.
- 4,346 unique res entries: 3,912 `DxTexture`, 434 `SwfMovie`.
- The census shows only the res metas (≈0.5 MB). The pixel data lives in chunks.

**Where the textures are [V]:**

| group | DxTextures | what |
|---|---|---|
| `ui/art/persistence/dogtag` | 1,053 | dog tags (not needed for MP-with-bots) |
| `levels/*/ui/minimap` (+ `levels/mp_subway/ui/combatarea`, `levels/xp2_skybar/ui/`) | 574 (+) | satellite, detail/vegetation SDF, combat-area SDF |
| `ui/art/persistence/award`, `ranks`, `textures/ui/persistence/awards` | 473 / 292 / 94 | awards, ranks (end-of-round screen only) |
| `ui/art/persistence/weapons` | 289 | weapon images: `<w>` large, `<w>_small` (64×16 DXT5), `Fancy/<w>` |
| `ui/art/persistence/vehicleaccessory`, `weaponaccessory`, `vehicle`, `kititem`, `specializations`, `camo` | 164 / 95 / 41 / 39 / 55 / 22 | loadout/unlock icons |
| `ui/art/menu/levelthumbs` | 30 | level thumbnails (`UILevelDescription.ThumbnailTexturePath`) |
| `ui/ingamemanual/*` | 378 | console manuals (ignore) |
| `ui/assets/*` (SWF external images `<swf>_iN`) | 135 | SWF atlases (iconsingame_i1 4096×128 DXT5, hudtextures_i3 2048×128, compass_i3 1024×64, ammo_i3 64×32, …) |
| `ui/static`, `ui/hud` | 4 + 4 | `hud3diconswin32` (ARGB8888 256×1024) etc. |

**Standalone textures versus SWF-owned textures [V].**
- In the 265 SWFs extracted and censused (of 434), every external image (77 unique names) is a `GFX_DefineExternalImage2` reference to `<swf>_iN.tga`.
- Each such reference corresponds to a `ui/assets/<swf>/<swf>_iN` DxTexture.
- Everything outside `ui/assets` is standalone and reachable from EBX by path string or reference.

**UI EBX types found [V]** (`S\types-ui.tsv`, `S\ui-datasources.tsv`):
- `UILevelDescriptionComponent` ×79, `UIFontMappingCollection` ×33, `UITextDatabase` ×11 (11 languages).
- `LocalizationAsset` ×1, `UIBundlesAsset` ×1, `UIMinimapIconTextureAtlasAsset` ×1.
- 84 `UI*CompData` components, each with a `DataSources` list (§5).
- Flow assets: `UIGraphAsset`, `UIStateAsset`, screens.

**Metadata partitions that feed the menus [V]** (`S\out-comp\*.json`):
- `ui/uiweaponmetadata`: `UIWeaponDescription` with `Name`/`Description` SIDs, `IconTexturePath`, `TexturePath`, `Ammo`, `RateOfFire`, `Range`, `ItemIds`.
- `uikititemmetadata`, `uivehiclemetadata`, `uivehicleweaponmetadata`, `uilevelmetadata`
  (`UILevelDescription.LevelName`/`TexturePath`/`ThumbnailTexturePath`), `uigamemodemetadata`, `uicamometadata`.

**Corrections to the critic's scan [V].**
- XP2_Skybar does have a satellite, at `levels/xp2_skybar/ui/xp2_skybar_satellite` (no `minimap/` folder), so all 29 levels have one.
- Metro's combat areas are under `levels/mp_subway/ui/combatarea/`.

## 2. Minimap: decoding and alignment (task 2)

**Data path [V].**
- The level's own `LevelData` partition (`levels/<L>/<L>`) carries a `UILevelDescriptionComponent` whose `UIMinimapData` has these fields:
  - `DetailTexture` points to `…/ui/minimap/<id>_satellite`;
  - `Detail.DistanceField` points to `<id>_detail_distancefield`;
  - `Vegetation.DistanceField` points to the vegetation SDF;
  - `WorldCenter` and `WorldSize`, with `WorldRotation` 0;
  - `CombatAreaAlphaThreshold` 0.496, `CombatAreaDistanceScale` 5, `AirRadarRange` 700.
- The `…/description_win32` partition holds defaults only (`WorldCenter` 0,0, `WorldSize` 4096). **Do not read it for geometry.**
- Values seen:

  | level | `WorldCenter` | `WorldSize` |
  |---|---|---|
  | Metro (`mp_subway`) | (0, −162) | 1024 |
  | Caspian (`mp_007`) | (0, 0) | 2048 |
  | Bazaar (`mp_001`) | (0, 0) | 512 |

**Combat-area masks [V].**
- A `VeniceUICombatAreaAsset` (`ca<mode><n>`, `Prefix` e.g. "cs") points to a `DistanceField` plus a `SurroundingDistanceField` (`dfsurr`).
- Each of those is a `UIDistanceFieldAsset`, which points to a `TextureAsset` named `df…_win32_sdf` (L8 128²).
- There are 222 `ca*` EBX and 499 combat-area res entries.
- Name stems: `dfra`/`dfrd` 80 each (Rush attack/defend per stage), `dfsra`/`dfsrd` 42, `dfcs` 34, `dfcl` 32, `dftdm` 22,
  `dfsdm` 13, `dfts`/`dfsurr`/`dfsqdm` 8, `dfcq` 6, `dfctf` 4, and others.
- An SDF value below 0.496 means inside the play area.

**Decoded images [V]** (`S\tex\*.rgba|json`):
- The satellite images are L8 (greyscale; the tint comes from `DetailTextureTint` and `ColorTint`).
- Metro 2048², Caspian 2048², Bazaar 512². Detail/vegetation SDFs are L8 1024².

**Orientation test [V]** (re-run 2026-10-14):
- `S\scripts\orient.py` tried all 8 sign/transpose combinations, testing whether each flag/HQ lands inside the union of the Conquest Small masks:
  - only `sx=−1, sz=−1, no transpose` puts every point inside (Metro 5/5, Caspian 7/7);
  - Metro's TDM mask against its known TDM bounding box gives IoU 0.70 for that mapping, and 0.00 for every other.
- **Second independent check** (`S\scripts\wpcheck.py`): fun-bots waypoint files, read from `C:\Users\olku\unfun-bots\mapfiles` as knowledge only.
  - The same mapping puts 100.0% of waypoints inside the mode's mask for Bazaar CQL (4,196 waypoints), Bazaar TDM (2,253), Caspian CQS (16,624) and Metro TDM (2,012).
  - The next-best mapping scores 0.71-0.93, or 0.00.
- Visual check: `S\img\mp_007_overlay.png` shows the flags on the roads and buildings of the satellite image, inside the mask outline.

**Formula [V]:** `u = 0.5 − (x − WorldCenter.x)/WorldSize`, `v = 0.5 − (z − WorldCenter.y)/WorldSize`.
- Coordinates are Frostbite world x and z in metres.
- Image row 0 is the top. Both axes are mirrored relative to the naive mapping.

## 3. SwfMovie payloads (task 3)

**Census [V]** (`S\scripts\swf.py`, 265 of the 434 SWFs extracted to `S\swf`):

- **Headers.** 220 `CFX` (zlib) and 45 `GFX` (uncompressed). Version 8 on 264 movies, 7 on one. `GFX_ExporterInfo` version `0x36f`, bitmap format 13.
- **Tag totals.**

  | tag | count |
  |---|---|
  | PlaceObject2 | 20,621 |
  | ShowFrame | 20,057 |
  | FrameLabel | 2,096 |
  | DefineSprite | 1,974 |
  | DoAction | 1,876 |
  | ExportAssets | 1,299 |
  | DoInitAction | 1,035 |
  | DefineEditText | 917 |
  | DefineShape (1-4) | 958 |
  | ImportAssets2 | 753 |
  | PlaceObject3 | 718 |
  | GFX_DefineSubImage | 683 |
  | CSMTextSettings | 671 |
  | GFX_DefineExternalImage2 | 77 |
  | DefineFont3 | 74 |
  | DefineScalingGrid | 24 |
  | DoABC | 0 |

- **Script.** AS2 bytecode totals 1,097,626 B and there is no AS3. That fits Scaleform's history: AS3 arrived only with GFx 4.0
  ([Autodesk migration guide](https://help.autodesk.com/cloudhelp/ENU/Scaleform-Help/scaleform_help/migration_guide.html),
  [Gamasutra 2010](https://gamedeveloper.com/programming/new-scaleform-gfx-4-0-supports-actionscript-3-flash-10)) [V web].
- **Shared libraries.** Most-imported: `../Static/gfxfontlib.swf` (124), `Textures.swf` 45, `HudTextures.swf` 34, `iconsIngame.swf` 23,
  `ImageManager.swf` 21, `VehicleCrosshair.swf` 14, `Compass.swf` 12. `ui/assets/actionscriptlibrary` (662 KB) carries the
  `__Packages.Widget.*` classes, e.g. `Widget.Hud.VehicleWeaponReload`, `Widget.ScreenSpecific.KillCam`, `Widget.Message.MessageTypes.LocalKillLogMessage`.

**Examples [V]:**

| movie | contents |
|---|---|
| `minimap` | 5.5 KB, stage 512², 9 sprites, 473 B AS2. It is only a frame; the map itself is engine-drawn (see `UIMinimapCompData`). |
| `crosshairdefault` | 0 shapes, 10,324 B AS2, so the crosshair is drawn by AS2 code |
| `iconsingame` | 1 external image `iconsingame_i1.tga` 4096×128, 162 `GFX_DefineSubImage` |
| `hudtextures` | 51 sub-images, labels `single`/`burst`/`auto`/`dead`/`check`/… |
| `compass` | `compass_i3` 1024×64, 12 sub-images |
| `hudmessage` | exports `Kill`/`LocalKill`; imports `ImageManager`, `DogTagPair`, `mc_iconsIngameSmall` |

- **iconsingame detail.** Each `GFX_DefineSubImage` is 12 B: `u16 charId, u16 imageId, u16 x1, y1, x2, y2` in atlas pixels,
  all within 4096×128. The movie has 427 frame labels (`FriendlyPlayer`, `EnemyFlag`, `AmmoCrate`, `Tank`, …).

**What a SWF subset renderer would cost [I]:**
- **Static draw.** Shape records (4 shape versions), bitmap/gradient fills and line styles, tessellation, sprite timelines,
  `DefineEditText` with Scaleform HTML, CSM text, `DefineScalingGrid`, and color transforms. Roughly 8-12 kLOC C++ and 2-3 months.
- **Behaviour.** Nothing animates or shows live values without an AVM1 interpreter: bytecode, MovieClip/TextField API,
  Scaleform `_global.gfxExtensions`, and the engine bridge. On top of that comes the Frostbite UI graph glue: `UIGraphAsset`
  `ActionNode`s keyed by numeric hashes (e.g. `ActionKey −368863171` = "Disable GameInput"), and `DataSetNode`/`WidgetNode`
  data bindings. That adds roughly 15-30 kLOC and 4-9 months.
- **Reference.** Ruffle's AVM1 is Rust, so it is a reference only; the shipped project is C++.
- **Verdict.** Not justified for a HUD whose data model is fully known (§5).

**The cheap alternative [V shape, I cost]:** a SWF *symbol-table* reader of about 400 LOC.
- It parses the header and zlib, plus `ExportAssets`, `FrameLabel`, `DefineSubImage`, `DefineShape` bitmap-fill ids and
  `PlaceObject2` within sprites.
- That is enough to map a frame label to an atlas rectangle. It needs no renderer and no script.

## 4. Localisation (task 4)

**Superbundle [V].**
- `Data\Win32\Loc\en.toc` (210,614 B) has `bundles=0, chunks=4285`, all of them inside `en.sb` (1,209,346,277 B).
- `Update\Patch\Data\Win32\Loc\en.toc` (827 B) has 5 chunks.
- English is the only language installed.
- Chunk census (`S\loc\en-chunk-census.tsv`):
  - 3,483 chunks start with `48 00 00 0C`: EA SNR/SPS audio, i.e. localised voice-over;
  - 803 are unframed audio continuations or empty prefixes;
  - only the text database pair is framed.
- **The patch copy wins.** It holds `50035522-22d3-b8c3-9aac-ce617deae923` (text binary, decoded 599,272 B) and
  `8ba57686-5a81-a229-726b-2d0f7ecb9b37` (histogram, 6,156 B). Read the patch first, then the base, as with every other superbundle.

**Indirection [V].**
- `localization/localization` (`LocalizationAsset`, `Win32/Globals`) has `LocalizedTexts` pointing to 11 `UITextDatabase` entries (`us_loc`, `ge_loc`, …).
- `localization/us_loc` has `BinaryChunk 50035522…`, `BinaryChunkSize 599272`, `HistogramChunk 8ba57686…`, `HistogramChunkSize 6156`, `Language LanguageFormat_English`.
- `UIFontMappingCollection` (`ui/static/fontcollection_en_fontmapcollection`) also references `us_loc`.

**Format [V]** (decoder `S\scripts\loc.py`; 15,515 entries, tag "Global", 0 decode errors):

- **Histogram chunk.**
  - Header: `u32 magic 0x00039001, u32 size, u32 count`.
  - Body: then `u16` code units.
- **Binary chunk.**
  - Header: `u32 magic 0x00039000, u32 size(=file−8), u32 count, u32 dataOff, u32 strOff`, then a NUL-terminated tag at 0x14 ("Global").
  - Entries: `count × (u32 hash, u32 offset)` at `dataOff+8`.
  - Strings: at `strOff+8+offset`, NUL-terminated.
- **String decoding.**
  - A byte below 0x80 is ASCII.
  - Otherwise look up `t = hist[byte]`. If `t ≥ 0x80`, `t` is the character.
  - Else `t` is a shift value: the character is `hist[(next − 0x80) + (t << 7)]`.
- **Second source [V web].** The layout, the magic values and the histogram decoding match flamethrower's
  `localization.py` (BF1 tooling, [github.com/zyf722/flamethrower](https://github.com/zyf722/flamethrower), file
  `flamethrower/localization.py`), which was written independently of this decoder.

**SID hash [V]:** `h = 0xFFFFFFFF; for each byte c: h = (h*33 + c) mod 2^32`, applied to the SID exactly as written (upper case).
- 1,247 of the 1,271 SIDs referenced from UI/level EBX (`S\loc\sid-resolution.tsv`) resolve.
- The other 24 have no entry at all (18 of them vehicle-weapon names `ID_P_VWNAME_*`). No entry reads `<MISSING>`: that was this decoder script's placeholder for an absent key (corrected 2026-10-15 with `bf3dump text --census`).
- BF1's hash in flamethrower (`hash.py`: seed 5381, `h*33 ^ c`) gives **0** hits on BF3, so the variant differs per game [V].
- Self-consistency check: `ID_H_US_A`→`34a54e7a` and `ID_H_US_B`→`34a54e7b`, adjacent as an additive last step predicts.

**Resolved samples [V]:**

| SID | text |
|---|---|
| ID_P_WNAME_M16A4 | M16A3 |
| ID_P_WNAME_AK74M | AK-74M |
| ID_P_WNAME_M249 | M249 |
| ID_P_WNAME_RPG7 | RPG-7V2 |
| ID_P_WNAME_SMAW | SMAW |
| ID_P_WNAME_M98B | M98B |
| ID_P_WNAME_M9 | M9 |
| ID_P_WNAME_870 | 870MCS |
| ID_P_WNAME_40MM | M320 |
| ID_P_INAME_M67 | M67 GRENADE |
| ID_P_VNAME_M1ABRAMS | M1 ABRAMS |
| ID_P_VNAME_T90 | T-90A |
| ID_P_VNAME_AH1Z | AH-1Z VIPER |
| ID_P_VNAME_HUMVEE | M1114 HMMWV |
| ID_H_US_A / ID_H_US_B / ID_H_RU_A | A / B / A |
| ID_H_MAP_MP_SUBWAY_TICKET_HALL / _CAFE / _PLATFORM | TICKET HALL / CAFE / PLATFORM |
| ID_H_MAP_MP_007_GAS_STATION / _HILLTOP / _ANTENNA | GAS STATION / HILLTOP / ANTENNA |
| ID_M_LEVELNAME_MP_SUBWAY / _MP_007 / _MP_001 / _XP2_SKYBAR | OPERATION MÉTRO / CASPIAN BORDER / GRAND BAZAAR␠ / ZIBA TOWER |
| ID_M_ASSAULT / ENGINEER / SUPPORT / RECON | ASSAULT / ENGINEER / SUPPORT / RECON |
| ID_H_MES_SOLDIERS_HOLD_TO_RESPAWN | HOLD TO RESPAWN |

- **SIDs to avoid.** The guessed SIDs `ID_P_MAP_*` and `ID_P_VNAME_M1A2` do not exist. (`ID_P_WNAME_M16` does exist: it is the M16A4 burst variant's name SID and reads "M16A4".) Always take SIDs from EBX fields, never by guessing.
- **Where SIDs come from [V/I].** SIDs arrive as strings in EBX: `UIWeaponDescription.Name`, `ID_M_LEVELNAME_*` in the level
  partition, `ID_H_MAP_*` in capture-point data. The runtime hashes them, so no SID list needs to ship.

## 5. HUD signals and screen flows (task 5)

**The in-game data model [V].**
- `ui/flow/state/ingamestatemp` (`UIStateAsset`) lists 58 UI components.
- Each `UI*CompData` declares its `DataSources`: the exact signals BF3's HUD consumes (`S\ui-datasources.tsv`).
- Our sim/HUD bridge should expose the same names. The MP-relevant ones:

| component | data sources (count) — the ones we need |
|---|---|
| `UIWeaponCompData` (63) | ActiveWeaponAmmo/ClipCapacity/AmmoInBag/ClipsLeft/CriticalAmmo/IsReloading/ReloadPercent, ActiveWeaponName, FiringMode(+Types), Zoomed, Dispersion/AbsoluteDispersion/CrosshairScale, Heat/HeatPenalty, ShowHitindicator/HitindicatorLevel, AimOnEnemy/AimOnTeammate, CanFire, Secondary* (UGL) |
| `UISoldierCompData` (16) | IsAlive, Health, Pose, Stamina, Supressing, CompassAngle, ShowMedicRevive, IsBeeingRevived, TimeUntilRevive, TimeUntilDeath, IsInteractiveManDown, HudState |
| `UIVehicleCompData` (82) | Health/Damage/CriticalHealth, Velocity, Throttle, Thrust, RadioAltitude/SeaLevelAltitude, ClimbRate, GForce, BankAngle/PitchAngle/YawAngle, Entry1-4Rotation, PassengerList, ActiveWeaponName, Locked/LockAmount, RangeMeter, ZoomState, FlirState, PredictedSightX/Y, VelocityVectorX/Y |
| `UICapturepointCompData` (12) | CapturepointsStatusData, MinimapCapturepointsStatusData, CaptureInProgress, CaptureProgress, CapturedByTeam, Friendly/Neutral/EnemyCapturepoints |
| `UITicketCounterCompData` (13) | FriendlyTeamTickets/EnemyTeamTickets, Team1-4Tickets, *MaxTickets |
| `UIKillCounterCompData` (26) | TeamNKills, TeamNMaxKills, PlayerTeamKills, LeadingTeamKills (TDM, SQDM) |
| `UIMessageCompData` (22) | KillMessage, LocalKillMessage, ScoreMinorMessage/ScoreMajorMessage, KillScoreMessage, GameMessage, OutOfBoundsMessage, ChatMessage, RewardMessage, GunMasterMessage |
| `UICombatAreaCompData` (4) | Timer, TimeLeft, ShowTimer, Text |
| `UIDamageIndicatorCompData` (1) | IndicatorData |
| `UIMinimapCompData` (8) | IsSmall, Zoom, PlayerIsCenter, AirRadarMaskEnabled, RadarSweepDegree, MinimapZoomWhenMapIsLarge/Small |
| `UINametagCompData` (4) | ShowNametags, ShowNametagsOnActors, ShowNametagsEnemyActors |
| `UISpawnPointCompData` (6) | SpawnPoints, SelectedSpawnType, SelectedSpawnPoint, SelectedSpawnListItem, CanSpawn, HasValidSpawn |
| `UISpawnLogicCompData` (4) | TimeToRespawn, StartTimeToRespawn, DisableSuicide, AutoDeployEnabled |
| `UIKillCardCompData` (27) | KillerName, KillerHealth, KillerKit, KillerWeaponName, KillerWeaponIcon/ImagePath, KillerVehicleSeat, isSuicide, KillerPlayerKills/PlayerKillerKills |
| `UITeamCompData` (35) | Team1-4Players, SortedPlayers, Team names, ScoreboardEnabled, Spectators |
| `UISquadCompData` (30) | SquadMembers, SquadName, LocalPlayerIsSquadLeader, AvailableSquads |
| `UIEndOfRoundCompData` (52) | WinningTeam, TimeUntilLevelChange, TeamResults, Kills, Deaths, scores per kit, BestWeaponInfo (ranks/unlocks: not needed) |
| `UISpectatorCompData` (2) | IsSpectator, ScoreboardFriendly |

**Per-entity HUD data (schemas: `C:\Users\olku\VU-Docs\types\fb\*.yaml`) [V].**
- `HudData`: crosshairScaleMin/Max, crosshairOpacity*, crosshairTypeId, weaponClass, lowAmmoWarning, reloadPrompt,
  showMinimap, hideAmmo, useRangeMeter/usePredictedSight/useVelocityVectorMarker/useLockingController/useThrust/useGForce/useClimbRate/useAimWarning.
- `VehicleHudData`: `minimapIcon` (UIHudIcon enum), `texture` (TextureAsset).
- `EntryComponentHudData`: seat type.
- `CharacterCustomizationAsset` / kits: `UIHudIcon` (e.g. `UIHudIcon_KitAssault`, `probe-ebx\out2\gameplay__kits__usassault.json`).

**Icons [V].** The `UIHudIcon` enum indexes `ui/static/hud3dicons`, which holds a UV rectangle per icon and per state
(Friendly/Enemy/Squad/Neutral/Default) on `hud3diconswin32`:
- player, squad leader, objectives;
- flags (Neutral/Friendly/Enemy), bases;
- the vehicle classes (Car, Jeep, Tank, TankIFV, TankAA, HeliAttack/Scout/Trans, Jet, JetBomber, StaticAA/AT, Boat, ATV, DirtBike, …);
- kits, NeedAmmo/Medic/Repair, C4/ATMine/Claymore/Mav/RadioBeacon, Mcom-style attacker/defender, CTF.

So minimap and world-space icons need no SWF at all.

**Kill feed [I, strong].**
- `hudmessage` exports `Kill`/`LocalKill` and imports `ImageManager`, which loads textures by path at runtime.
- `UIWeaponDescription.IconTexturePath` names `UI/Art/Persistence/Weapons/<w>_small`, a 64×16 DXT5 [V] standalone texture.
- So kill-feed weapon icons are the `_small` DxTextures.
- The exact `KillMessage` payload fields were not decoded.

**Flows [V data, I semantics]** (graphs in `S\out-flow`):

- **Spawn screen** (`ui/flow/screen/spawnscreenpc`):
  - widgets: `KitBar` (binds SelectedKit, Kits, ScoreboardEnabled), `OverviewMap`, `SpawnPointList`, `Grid` (GridItems, selectedIndex), `TabBar`, `SpawnPointsHeader` (GameMode, IsAttacker, TeamName, TeamNumber);
  - fed by `UISpawnPointComp` and `UISpawnLogicComp`;
  - spawn candidates are HQ/flag spawn points, the squad leader, the radio beacon and vehicles: the `SpawnPoints` list (plain logic for us);
  - the overview camera is the minimap texture itself;
  - `CameraEntityData` spawn cameras near flags exist (handoff 04 §2.5 [R]) but are optional.
- **Death**: `ui/flow/graph/ingame/killedgraph` runs `Death` → ActionNode "Disable GameInput" → `KillScreen` state.
- **Kill cam**: prefab `gameplay/level_setups/components/killer_camera` (`LogicPrefabBlueprint`, client realm). It has a
  `DelayEntityData Delay 4.0`, event gates, and the server setting `BFServer.IsKillerCameraEnabled`. The UI shows `killcam`/`killscreen` and the `UIKillCardComp` data.
- **Scoreboard**: `ui/flow/graph/ingame/mp/ingamescoreboardgraph` (329 KB of graph) binds the `UITeamComp`/`UISquadComp` lists.
- **Spectator**: `spectatorgraph` has a single `Spectating` → `DataSetNode IsSpectating`; `UISpectatorComp` has 2 fields.
- **Minimap init**: `minimapinitgraph`.

Graph `ActionNode`s carry numeric `ActionKey` hashes plus string params. We reimplement each flow as a small C++ state
machine, not as an interpreter of these graphs.

## 6. Godot 4.8 APIs (task 6)

All of these were checked in `C:\Users\olku\BF3 reimplementation\openBF3\research\godot-api\extension_api.json`
("Godot Engine v4.8.dev7.official") [V].

**Images.**
- `Image.create_from_data(width, height, use_mipmaps, format, data)`.
- Formats `FORMAT_L8`, `FORMAT_DXT1`, `FORMAT_DXT5`, `FORMAT_RGBA8` all exist: satellite and SDF upload as L8, icons as DXT5 with no transcoding.
- `ImageTexture.create_from_image` and `update`; `AtlasTexture.set_atlas` and `set_region`, for hud3dicons UVs and SWF sub-image rectangles.

**Minimap rendering.**
- `CanvasItem` draw calls (`draw_texture_rect_region`, `draw_set_transform`, `draw_string`, `draw_polyline`, `draw_circle`, `queue_redraw`, `set_material`).
- `Shader.set_code` and `ShaderMaterial.set_shader_parameter`.
- The SDF mask is one `canvas_item` shader:

  ```
  float d = texture(sdf, uv).r;
  alpha = smoothstep(thr - w, thr + w, d);   // thr = 0.496, then tint/outline from UIMinimapDistanceFieldParams
  ```

**Controls.**
- `Control.set_theme`, `set_anchors_preset`, `_gui_input`.
- `Theme.set_font`, `set_color`, `set_stylebox`, `set_default_font`.
- `TextureRect`, `NinePatchRect.set_patch_margin` (BF3 `DefineScalingGrid` analogue), `StyleBoxTexture`, `Label`, `RichTextLabel.push_font`.
- `SubViewport`; `Camera3D.unproject_position` and `is_position_behind` for world-space icons and name tags.

**Strings.**
- `Translation` has a virtual `_get_message(src_message: StringName, context: StringName)`, so a GDExtension `Translation`
  subclass can resolve `tr("ID_P_WNAME_M16A4")` by hashing the SID and looking up the decoded table lazily.
- Register it with `TranslationServer.add_translation`.
- `Node.set_auto_translate_mode` exists. (It is on `Node`, not `Control`.)

**Fonts.**
- `FontFile.set_data(PackedByteArray)` takes an in-memory TTF/OTF.
- Alternatively, build a bitmap/MSDF font by hand: `set_texture_image`, `set_glyph_uv_rect(cache_index, size, glyph, uv_rect)`,
  `set_glyph_advance`, `set_glyph_size`, `set_glyph_offset`, `set_glyph_texture_idx`, `set_multichannel_signed_distance_field`, `set_msdf_pixel_range`.
- **The fonts are not TTF/OTF [V].**
  - The font SWFs contain only `DefineFont3` tags; a byte scan of all `*fontlib*` payloads found no `OTTO`, `glyf` or `cmap`.
  - `fontcollection_en_fontlib` has Purista EA Medium and Semibold (443 glyphs each) and Venice-Medium and SemiBold (33 glyphs: HUD digits and symbols).
  - The `ondemandfontcollection` entries are store/EULA font libraries, also `DefineFont3`.
  - `UIFontMappingCollection` maps them to Scaleform names `$MENU_medium`, `$MENU_bold`, `$HUD_medium`, `$HUD_bold`.
- Converting is cheap [I]. `DefineFont3` glyphs are quadratic-Bézier shape records (20 twips × 1024 em), the same curve type as TrueType `glyf`.
  - Option 1: write a minimal in-memory TTF (head/hhea/maxp/cmap/hmtx/loca/glyf; codes from the code table, advances and kerning from the layout block) and hand it to `FontFile.set_data`.
  - Option 2: rasterise to an MSDF atlas.
  - Either way, cache the result in `user://cache/<digest>/fonts/`. About 600-900 LOC.
  - Hangul, Japanese and Chinese fonts are irrelevant with an English-only install.

## 7. Legal (task 7)

- **Runtime only.** Reading these textures, strings and fonts at runtime from the user's own install is the same act as reading meshes (08 §3.8 #2) [R].
- **Nothing in the repo.** No textures, strings, fonts or screenshots go into the repo, CI or releases (08 §3.8 #1) [R].
  Golden test values are counts and hashes, e.g. "15,515 entries", "`ID_P_WNAME_M16A4` → hash `0fd83dfa`".
- **Not for branding.** The project's own icon, site and store pages must not use BF3 UI art or fonts (08 §3.8 #6) [R].
- **Fallback when data is missing.** If a texture, font or string is missing (the install is not a "matching" one), fall back to own art, a system font and the raw SID.
- **Fonts.** Purista and Venice are licensed to EA. Rendering them from the user's install at runtime is the same position as the other assets.
  Never embed them in the repo or in a converted-font cache that leaves the machine [I].

## 8. Decision table

| route | what ships / what is read | build cost [I] | fidelity | risk |
|---|---|---|---|---|
| **A+: own `Control` layout + install textures, strings and fonts + SWF symbol-table reader** (recommended) | C++ HUD widgets; runtime reads of the minimap satellite and SDFs, hud3dicons UVs, `_small` weapon icons, SWF atlases cut by label, the `UITextDatabase`, and `DefineFont3`→font | text DB reader ~150 LOC; minimap shader+widget ~600; icon atlas/symbol reader ~400; font conversion ~800; HUD widgets ~4-6 k; spawn/scoreboard/kill card/end-of-round screens ~3-4 k. **≈6-8 weeks, one developer** | BF3 look for icons, map, names and fonts; layout approximated by hand from the SWF stage sizes and screenshots of the user's own game | layout is approximate; the crosshair is procedural (BF3 draws it in AS2 too) |
| B: SWF subset renderer + AVM1 + UI graph | runs BF3's own SWFs | static renderer 2-3 months + AVM1/Scaleform extensions/data-binding bridge 4-9 months | closest to identical | large; behaviour depends on undocumented engine ↔ AS2 calls; blocks the HUD for months |
| C: own art only (current §3.8) | nothing read for UI except names | widgets same as A, minus loaders; art time instead | generic look | still needs strings (else raw SIDs); minimap without the satellite is poor |

**Answer:** route A+. Route C remains the fallback per item. Route B is rejected for v1; revisit only if the rest of the
project is done and people want a pixel-exact HUD.

**Milestone placement [I].**
- The minimap and strings belong with M2/M3 (first playable level + flags), since names and capture UI need them.
- Fonts and the spawn screen come next.
- Scoreboard, kill card and end-of-round screen go last.

## 9. Re-check of the 5 most load-bearing claims against a second source

1. **Text DB layout and histogram decoding.** Our decoder (0 errors on 15,515 strings) agrees with flamethrower's independent `localization.py` [V].
2. **SID hash.**
   - 98.1% hit rate over 1,271 EBX-sourced SIDs; the adjacent-hash check holds (`…7a`/`…7b`) [V].
   - The BF1 hash from flamethrower gives 0 hits, which shows the variant is game-specific.
   - **No external BF3 source was found**, so this rests on the empirical hit rate.
3. **Minimap mapping.** Flags/HQs (from level EBX) give 12/12, and fun-bots waypoints (an independent dataset) give 100% on 4 level/mode pairs [V].
4. **SWFs are AS2-only.** The tag census shows 0 `DoABC`, and Autodesk's docs say AS3 arrived in GFx 4.0 [V].
5. **Fonts are `DefineFont3`, not TTF/OTF.** The tag census and the byte scan agree, and the EBX `UIFontMappingCollection` long names match the `DefineFont3` names [V].

## 10. Could not determine

- **Kill-feed payload.** The exact `KillMessage` data and whether the kill feed uses `IconTexturePath` (`_small`) or an atlas inside a SWF. This is strongly inferred, not proven.
- **Unparsed SWFs.** 169 of the 434 SWFs were not extracted or censused (mostly front-end screens).
- **Label-to-rectangle mapping.** The full mapping from `iconsingame` frame label to sub-image rectangle is not built; only the rectangles are parsed.
- **Absent SIDs.** Why 24 SIDs have no entry (e.g. `ID_P_VWNAME_M1A2SMOKE`). Probably cut content.
- **Spawn screen.** Which `CameraEntityData`, if any, the PC spawn screen uses. It appears to draw `OverviewMap` from the minimap.
- **Squad and vehicle spawn rules.** Exact squad-spawn and vehicle-spawn eligibility rules (open in PORTING-RESEARCH §3.7 already).
- **Exact HUD layout.** Positions and sizes of HUD elements. Only stage sizes are known (e.g. minimap 512²). Taking them from `PlaceObject` matrices needs the symbol reader extended to the root timelines.
- **`DefineFont3` code table.** Whether the glyph code table is UCS-2 and covers Latin-1 plus the characters seen in strings (`É` in MÉTRO). Assumed, not tested.
- **Other languages.** Behaviour on a non-English install (the `Loc\<lang>` name and the `LanguageFormat` to pick). Only `en` is present here.
