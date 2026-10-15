# UI groundwork 2: minimap, HUD icons, HUD data model

Status: design for implementation, revision 2, 2026-10-15 (revision 1 reviewed; revision 2 makes G9 / G10 measure
the drawn map through a UV read-back, settles G7 and Q4, and keeps the node out of other runs' measurements).
Track `ui2`, branch `ui/minimap-icons`. Scope: the second UI groundwork wave of the plan ([research/PORTING-RESEARCH.md](../../research/PORTING-RESEARCH.md) §3.8 "Route A+",
D16, §7.8 "UI"), built on the research report
[research/topics/gap-r1-ui-hud-minimap-localisation.md](../../research/topics/gap-r1-ui-hud-minimap-localisation.md)
(below "gap UI"). The interface headers named in §11 are in the tree and compile (ctest `ui2.contract`); their
bodies are the implementation wave's work (§15).

Tags: **[verified]** = read from code, data or a measurement in this tree on the date above (the command is named).
**[plan]** = taken from the plan or gap UI, with its section. **[inferred]** = a design choice or estimate made here.

Corrections to the plan and gap UI found while verifying (each one is used below; the plan's and gap UI's text is
left for its owner to fix):

| # | plan / gap UI says | the install says [verified] | how |
|---|---|---|---|
| C1 | Bazaar satellite 512² (gap UI §2 "Decoded images"; the task's exit gate) | **2048² L8**, one mip. 512 is Bazaar's `WorldSize` | DxTexture header of `levels/mp_001/ui/minimap/mp01_satellite`; gap UI's own decoded json says 2048 too |
| C2 | "499 combat-area res entries" (gap UI §2) | 562 `*_win32_sdf` textures under `levels/*/ui`: **498** distance fields that `VeniceUICombatAreaAsset`s reference, 50 detail / vegetation fields, 14 XP2 floor fields | §4.3; 499 is not reproduced by any filter tried (one unreferenced or one counted twice, open question Q1) |
| C3 | combat-area distance scale 5 | `CombatAreaDistanceScale` **150** on 26 levels, 5 only on XP2_Factory, XP2_Office, XP2_Skybar | §4.1 |
| C4 | hud3dicons "161 types" | 161 entries but **159 distinct** `IconType`s (`UIHudIcon_RadarSweepComponent` and `UIHudIcon_LocalPlayerBigIcon` twice) | §4.4 |
| C5 | minimap data in `description_win32` | `levels/<l>/<l>/description_win32` holds defaults (WorldCenter 0,0, WorldSize 4096). The real `UIMinimapData` is in the **LevelData partition** `levels/<l>/<l>` (`LevelDescription.Components` → `UILevelDescriptionComponent.MinimapData`) | §4.1; `level::read_level_info` already reads the LevelData copy first |
| C6 | gap UI's orientation reference "Metro's known TDM bounding box" (−282..−82, −584..−429) | that box is the bounding box of Metro TDM's **gameplay combat area**, a 23-point `VolumeVectorShapeData` in `levels/mp_subway/teamdeathmatch/layer0_teamdeathmatch_logic`. Against the polygon itself the UI mask's IoU is **1.000** for the research mapping and 0.000 for the other seven | §5.3 |

**For the acceptance reviewer:** G1 (Bazaar satellite 2048², C1), G3 (455 / 498 / 562 instead of "499 masks", C2)
and G6 (159 distinct types besides the 161 entries, C4) differ from the task's wording. Each follows the install data
verified above; none is a weakening (each gate checks at least what the task's numbers would).

---

## 1. Goal and exit gates

**Goal.** In the running M1 game (res://scenes/m1_metro.tscn) a BF3 minimap: Metro's satellite image tinted as BF3
does, the mode's combat area shaded outside, flags and HQs with BF3's own hud3dicons, the player marker at the
right place turning with the view; flag icons over the flags in the 3D view. Behind it, Godot-free and tested on the
install: the minimap data of all 29 MP levels and every combat-area mask, the hud3dicons atlas, the `_small` item
icons for every catalog weapon, and the HUD data model (`HudModel`) whose field names are BF3's own DataSources,
with every DataSource of the in-game components accounted for. Plan §7.8: "minimap from UIMinimapData, HUD icons
from hud3dicons, a HUD data model named after DataSources" [plan].

Each gate is measured by **one** ctest (or one command) and has a threshold. "install" tests exit 77 (Skipped)
without an install; `godot.*` tests also exit 77 without `OPENBF3_GODOT` or a window.

| id | gate | measured by | threshold |
|---|---|---|---|
| G1 | UIMinimapData of the three reference levels | `ctest -R "^ui\.minimap$"` (install), row `minimap.reference` | Metro (MP_Subway) WorldCenter (0, −162), WorldSize 1024, satellite L8 2048 × 2048; Caspian (MP_007) (0, 0), 2048, L8 2048²; Bazaar (MP_001) (0, 0), 512, **L8 2048²** (C1); exact |
| G2 | all MP levels | `ui.minimap`, row `minimap.all_levels` | 29 / 29 levels: UIMinimapData read from `levels/<l>/<l>`, WorldSize > 0, WorldRotation 0, threshold in [0, 1], satellite decodes L8 with side ≤ 4096 |
| G3 | every combat-area mask resolves | `ui.minimap`, row `minimap.masks` | over `@all-mp`: 455 / 455 `VeniceUICombatAreaAsset` read, 498 / 498 referenced distance fields decode L8 (C2), 562 / 562 `UIDistanceFieldAsset`s resolve to a decodable L8 texture; 0 failures; the counts printed |
| G4 | objectives inside the mode's area | `ui.minimap`, row `minimap.objectives` | Metro ConquestSmall0: 5 / 5 CapturePoint placements (3 flags, 2 HQs) inside the union of the mode's masks (cacs1, cacs2) through the research mapping; Caspian ConquestSmall0 7 / 7; on Metro every other mapping variant < 5 / 5 |
| G5 | orientation | `ui.minimap`, row `minimap.orientation` | Metro TeamDeathMatch0 mask (catdm → dftdm) against the gameplay area: bounding-box IoU ≥ 0.65 for variant 0 (gap UI: 0.70) and ≤ 0.05 for the other 7; polygon IoU ≥ 0.95 for variant 0 (measured 1.000, C6) and ≤ 0.05 for the other 7 (measured 0.000) |
| G6 | hud3dicons | `ctest -R "^ui\.hud\.icons$"` (install), row `icons.atlas` | 161 entries, 159 distinct types, 93 with states; every frame UV finite, in [0, 1], min ≤ max, rect non-empty inside 256 × 1024; atlas decodes RGBA8 256 × 1024; `UIHudIcon_LocalPlayer` Default rect (35, 633, 17, 19) |
| G7 | weapon `_small` icons | `ui.hud.icons`, row `icons.weapons` | every MP_Subway catalog variant (172) maps through its unlock identifier (`SoldierWeaponUnlockAsset.Identifier` in `ItemIds`) to an `ItemIcon` whose texture exists in Win32/UI and decodes (format and size printed; DXT5 64 × 16 for the weapons): **172 / 172; any miss fails the row** and is listed by variant name. Measured for this design (§4.5): 158 through the weapon metadata, 14 through the kit-item metadata, 0 misses |
| G8 | DataSources table complete | `ctest -R "^ui\.hud\.sources$"` (install) | 58 state components, all resolve; 84 component types over `Win32/UI,@all-mp`; source counts UIWeaponCompData 63, UISoldierCompData 16, UIVehicleCompData 82; `check_hud_bindings`: 235 rows, 0 rows missing from the install, 0 install sources of the 11 bound types without a row; Model / Client / Unused = 150 / 35 / 50 |
| G9 | the map and the player marker in game, measured on the viewport image | `ctest -R "^godot\.ui2$"` (windowed; runs `--minimap-check --minimap-mode=ConquestSmall0`), rows `readback.calibration`, `marker.spawn`, `map.uv.north_up`, `map.uv.rotating`, `marker.pixels` | calibration patch reads back byte-exact; the own position at the first frame within 0.5 m of a spawn point of `shared_level()->spawns`; with the shader's `debug_uv` on and the icons hidden, at 49 sample pixels (centre + 3 rings of 16) the decoded UV equals `world_to_minimap_uv(minimap_unproject(sample))` within **1 satellite texel** per axis in the north-up view and after a scripted 90° turn in the rotating view (the centre sample: the map under the marker is the spawn's texel), and each of the 7 other mapping variants misses by **> 10 texels** at some sample; the marker's pixels centred within 1 canvas unit of the control centre in both views (§10.4) |
| G10 | flag / HQ icons in game | `godot.ui2`, rows `icons.minimap`, `icons.world` | `--minimap-mode=ConquestSmall0`, both views: the debug-UV read-back at each drawn on-disc objective icon decodes to `world_to_minimap_uv(objective)` within 1 texel (rim-clamped ones on the rim within 1 unit and within 2° of the objective's direction); icon pixels present in the image inside each drawn rect; entry and state = `objective_icon(hq, owner, kM1LocalTeam)` (§10.2); world icons: every objective not behind the camera centred at the check's own pinhole projection of `position + (0, 5.5, 0)` plus `(0, −28)` within 0.5 canvas units, behind ones not drawn, icon pixels present |
| G11 | screenshots | `godot.ui2` with `OPENBF3_UI2_SHOTS` set (default `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\ui2\`, gitignored) | ≥ 4 PNGs (rotating + north-up at the spawn, at an HQ, at a flag), non-empty. **Inspected by eye** at acceptance (marker on the right spot, flags on the satellite's flags); the automatic part is G9 / G10 |
| G12 | parsers under asan | `ctest --preset asan -R "^ui(2\.contract|\.minimap|\.hud)"` | this track's tests (`ui2.contract`, `ui.minimap.unit`, `ui.hud.unit`, `ui.minimap`, `ui.hud.icons`, `ui.hud.sources`, `ui.minimap.fuzz`, `ui.hud.fuzz`) pass with 0 AddressSanitizer reports; the regex leaves ui1's `ui.swf.*` / `ui.text.*` out on purpose (they have their own gate) |
| G13 | install unchanged | an `install.unchanged` row in `ui.minimap`, `ui.hud.icons`, `ui.hud.sources`, `ui.minimap.fuzz`, `ui.hud.fuzz`, `godot.ui2` | sizes and mtimes of the 256 index files equal at start and end (layout.toc, cas.cat, every .sb / .toc) |
| G14 | load cost (report, noisy) | `godot.ui2`, row `minimap.loaded` prints the loader's wall time and the process peak-working-set delta across the loader | wall time ≤ 5 s on the warm OS cache with 2 parse lanes (Metro load_level + traverse measured 0.34 s + 0.04 s with all lanes, `bf3dump level MP_Subway ConquestSmall0`, shared machine; timings are noisy, the bound is loose on purpose); the memory delta is reported only (no threshold: not measured yet); `godot.m1_load` E4 / E5 and its frame / memory rows unchanged because the node is inert under `--m1-load-check` (§10.1) |

Excluded on purpose: the fun-bots waypoint cross-check gap UI §2 used (clean room); G5's polygon replaces it with
our own data.

---

## 2. Module ownership

| module | owns (this track) | does not touch |
|---|---|---|
| `bf3_core` `ui/` | `minimap.h` (UIMinimapData, combat areas, objectives, mapping, view, gates' geometry), `hud_icons.h` (atlas, item icons, vehicle HUD icons), `hud_sources.h` (the generated DataSources table), `hud_model.h` (HudModel, binding rows); their `.cpp` (§12) | `level/` (read through `load_level`, `World::traverse`, `Scopes`), `res/` (through `load_texture`), `text_db` |
| `bf3_godot` | `Bf3MinimapHud` (CanvasLayer), `Bf3MinimapControl`, `Bf3WorldIcons`, `MinimapCheck` | `level_session.*`, `level_upload.*` (lk track), the walkers (s3): read through the session's public accessors only |
| `tools/bf3dump` | `ui_commands.{h,cpp}`: `minimap`, `hud-icons`, `item-icons`, `hud-sources` | other commands; the dispatch gets one additive `// UI ui2` block |
| `tests` | `ui2_contract_tests.cpp`, `ui_*_tests.cpp`, `godot_ui2.cmake` | other tests |
| `bf3-opensource` | one node line block appended to `scenes/m1_metro.tscn` | the session node |

No new module, no new dependency. Everything Godot-free sits in `bf3_core` (plan §2.2: the readers Godot-free, the
Godot side thin) [plan].

## 3. Threads

* **Loader** (one `std::thread` owned by `Bf3MinimapHud`, no Godot API, below-normal priority): started from
  `_process` once the session reports `done()`; opens its own `Install` from the session's `InstallReader` (Install
  is cheap: 0.04 s [verified: bf3dump]), runs `load_minimap_level` (`level::load_level` with
  `kMinimapHudLoaderThreads` = 2 parse lanes, `World::traverse`, the reads), `load_minimap_images`, the satellite
  mip chain, the Win32/UI atlas, and publishes one immutable `MinimapAssets` (no pixels) and the `MinimapPixels`
  under a mutex plus a release flag. Plan §2.3 keeps loader threads low priority and few [plan]: 2 lanes at
  below-normal priority leave the frame and the 60 Hz server thread alone on the 8-core machine. `stop_` is checked
  between steps; the destructor joins, so a quit during the load waits for the current step (at most one
  `load_level`, ~0.4 s warm). Starting after Done keeps the M1 load gates clean; the cost is a second `load_level`
  of Metro (0.34 s warm with all lanes [verified]; slower with 2) and its transient memory (G14 reports the delta,
  risk R2).
* **Main thread** (Godot): after the publish, `Image::create_from_data` (L8 with the mip chain, RGBA8 atlas),
  `ImageTexture::create_from_image`, the `ShaderMaterial`; then the `MinimapPixels` (~10 MB on Metro) are freed (the
  check reads the viewport, not CPU copies). Every frame the view from the session's presentation frame and
  `InputMapper::view()`, `queue_redraw`. All `_draw` calls and every read-back are main-thread.
* **Tests and bf3dump**: plain calls on the calling thread; `load_level` uses its own lanes.

No shared mutable state with the session: the HUD copies what it reads (`frame()` returns the double-buffered
presentation frame by reference on the main thread [verified: level_session.h]).

## 4. Data sources in the install

All counts **[verified]** on 2026-10-15 with `bf3dump find / types / dump / level / spawns` and the scratch scripts
of this design (outputs in the scratch directory, never the repo).

### 4.1 UIMinimapData (per level)

`levels/<l>/<l>` (LevelData partition, the level superbundle) → `LevelData.LevelDescription.Components[]` →
`UILevelDescriptionComponent.MinimapData` (`UIMinimapData`). 78 `UILevelDescriptionComponent`s in `@all-mp` (the
LevelData and `description_win32` copies and others not examined); the 29 MP level roots all carry one. Fields used:

| field | meaning | retail range over the 29 levels |
|---|---|---|
| `WorldCenter` (Vec2) | Frostbite (x, z) of the image centre | e.g. Metro (0, −162), Caspian (0, 0), Bazaar (0, 0), MP_011 (0, 88), XP3_Alborz (600, 250), XP4_FD (75, −75), XP5_002 (−1800, 0), XP5_004 (−920, −900) |
| `WorldSize` | metres across the image | 128 (XP2_Factory, Palace, Skybar), 256 (XP2_Office), 512 … 2048 |
| `WorldRotation` | | 0 on all 29 |
| `WorldRange` / `AirRadarRange` | radar ranges | 1200 (500 on XP2) / 700 (350 MP_017, XP1_003; 800 XP3_Alborz, Desert, Shield, XP4_Quake) |
| `DetailTexture` | TextureAsset → DxTexture res: the satellite | L8, 1 mip: Metro, Caspian, Bazaar 2048², XP2_Factory 1024² |
| `DetailTextureTint` | tint of the L8 satellite | |
| `Detail`, `Vegetation` | `UIMinimapDistanceFieldParams` → `UIDistanceFieldAsset` → L8 SDF | 1024² on Metro; **null** on the 4 XP2 levels |
| `CombatAreaAlphaThreshold` | inside when SDF/255 < it | 0.496 on all 29 |
| `CombatAreaDistanceScale` | | 150; 5 on XP2_Factory, XP2_Office, XP2_Skybar (C3) |
| `CombatAreaColor`, `CombatAreaMultiplyTexture`, `FadeTexture` | shading, the round mask `ui/hud/minimap/minimapmask` | |
| `UseStreamingMinimap` | XP2 floor fields | true on the 4 XP2 levels |

### 4.2 Combat areas (per level × mode)

Each sub-world a mode includes places `UICombatAreaEntityData {Asset → VeniceUICombatAreaAsset}`; `World::traverse`
lists them as `ObjectKind::Other` labelled `UICombatAreaEntityData` [verified: `bf3dump level`]. Metro
ConquestSmall0: two (cacs1, cacs2) in `levels/mp_subway/conquest_small/layer1_conquest_full_4_flags_small`; Metro
TeamDeathMatch0: one (catdm) in `levels/mp_subway/teamdeathmatch/layer1_teamdeathmatch_spawners`.
`VeniceUICombatAreaAsset {Prefix, DistanceField, SurroundingDistanceField}`, each field a `UIDistanceFieldAsset →
DistanceFieldTexture` (TextureAsset `…_win32_sdf`, DxTexture L8 128² on the ones decoded). Census over `@all-mp`:
455 assets on 25 levels (none on the 4 XP2 levels), 498 distinct distance fields referenced, 92 null
`SurroundingDistanceField`s. `Prefix` naming is inconsistent (cs / cl / tdm against caCS11 / cacs01 …), so it is
never used as a mode key: the mode's own placements decide.

The gameplay area (for G5) is separate: `CombatAreaTriggerEntityData` (`gameplay/gamemodes/deathmatch`, two, one per
team) whose area the TDM sub-world links in: `SubWorldData.LinkConnections[0]` from the logic layer's
`ReferenceObjectData` (source field hash 1486550654) to the `VolumeVectorShapeData` `c304182e-…` in
`levels/mp_subway/teamdeathmatch/layer0_teamdeathmatch_logic` (23 points, closed, identity transform, bbox
x −282.0 … −82.0, z −584.3 … −428.8). **Selection rule** (`read_area_polygon`, minimap.h): the traversal of Metro
TeamDeathMatch0 lists 124 `ObjectKind::VolumeVectorShape` placements, exactly **one** of them with its entity in that
logic partition (traversal object `layer0_teamdeathmatch_logic#104`) [verified: `bf3dump level MP_Subway
TeamDeathMatch0`]; the reader takes the one placement in the named partition (0 or more than 1 fail), reads `Points`
through the placed transform and keeps (x, z). The link connection is the justification, the partition rule the
implementation (it needs no link-connection resolver). Points are capped at `kMaxAreaPolygonPoints` (1024) and the
mask side at `kMaxIouSide` (1024), so `mask_polygon_iou`'s side² × points cost is bounded.

### 4.3 Distance-field census

562 `UIDistanceFieldAsset` in `@all-mp`; their textures: 498 combat-area (above), 50 for the `Detail` /
`Vegetation` fields, 14 XP2 floor fields. Gap UI's stem counts (dfra / dfrd 80 each, …) stay
as an orientation aid only.

### 4.4 HUD icon atlas

`ui/static/hud3dicons` (Win32/UI; not in Metro's superbundle) is one `UIMinimapIconTextureAtlasAsset`:
`Icons[]` of `UIMinimapIconTexture {IconType, States[]}`: 161 entries, 159 distinct types, 93 with states. States:
Default 54, Squad 38, Neutral 38, Friendly 35, Enemy 34, Blink 3. Frames per state: 1 on 180 states, 2 on 18, 3 on
2, 10 on 2. ShouldRotate true on 81, false on 121. UV range 0.0029 … 0.992. `TextureAtlas` →
`ui/static/hud3diconswin32`: DxTexture format 9 (ARGB8888; `res_loader` hands R, G, B, A bytes) 256 × 1024, one
mip. Icons this track draws: `UIHudIcon_LocalPlayer` (Default, Squad), `UIHudIcon_Player` (4 states),
`UIHudIcon_NeutralFlag` (Blink, Neutral), `UIHudIcon_FriendlyFlag`, `UIHudIcon_EnemyFlag`, `UIHudIcon_Flag`
(Neutral), `UIHudIcon_Base` (Friendly, Enemy), `UIHudIcon_FriendlyBase`, `UIHudIcon_EnemyBase`.

### 4.5 Item icons

* `ui/uiweaponmetadata`: 111 `UIWeaponDescription {Name (SID), ItemIds[], IconTexturePath}`; every
  `IconTexturePath` ends `_small`. Win32/UI has 289 `ui/art/persistence/weapons/*` partitions, 95 of them `*_small`.
  `ui/art/persistence/weapons/m16a4_small`: DXT5 64 × 16, one mip.
* MP_Subway's weapon catalog: 172 variants. `ItemIds` holds `SoldierWeaponUnlockAsset.Identifier` values. The
  172 unlock partitions (`bf3dump dump Win32/Levels/MP_Subway/MP_Subway <dir> 0 - <the 172 u_* partitions>`),
  matched by identifier against the three metadata partitions, give 158 through `UIWeaponDescription` (its `Name`
  equals the variant's name SID on all 158) and 14 through `UIKitItemDescription` (ammobag, c4, claymore, defib,
  eodbot, m15, m224, mav, medkit, radiobeacon, repairtool, soflam, ugs → `ui/art/persistence/kititem/*_small`; m67 →
  `ui/art/persistence/weapons/grenade_small`), 0 misses; all 14 kit-item textures exist in Win32/UI as
  TextureAssets (`bf3dump find Win32/UI ui/art/persistence/kititem/ grenade_small`); 2 ids are listed by two
  descriptions (first wins) [verified]. Revision 1 matched by name SID; the reader matches by identifier.
* `ui/uikititemmetadata`: 17 `UIKitItemDescription {ItemIds, IconTexturePath (e.g. RepairTool_small,
  Grenade_small), HudIcon}`.
* `ui/uivehiclemetadata`: 56 `UIVehicleDescription {TexturePath, ItemIds}` (no small icon).
* `VehicleEntityData.HudData` (`VehicleHudData`): `MinimapIcon` (e.g. `UIHudIcon_TankAA`), `Texture` (null on the
  Tunguska), `VehicleItemHash`. `vehicle_def.h` already reads `MinimapIcon` [verified].

### 4.6 HUD components and DataSources

`ui/flow/state/ingamestatemp` (`UIStateAsset`, Win32/UI) lists 58 `UIComponents`, all resolving in Win32/UI. Each
component type has one partition under `ui/uicomponents/` whose primary instance carries `DataSources` (CString
list), `UpdateType`, `UpdatesPerSecond`. Over `Win32/UI,@all-mp` (first superbundle wins): 84 partitions, 84 distinct
primary types (first hits: 55 MP_001, 27 Win32/UI, 1 MP_012, 1 XP5_001). The 58 in-game components and their
source counts:

HudCameraShakeData 0, UIAlerttagCompData 1, UICapturepointCompData 12, UICapturepointtagCompData 1,
UICombatAreaCompData 4, UIDamageIndicatorCompData 1, UIDataInterfaceCompData 0, UIDirectAccessCompData 0,
UIInteractionCompData 4, UIKillCardCompData 27, UIKitCompData 4, UIMapmarkerCompData 1, UIMapmarkertagCompData 1,
UILifeCounterCompData 16, UIMessageCompData 22, UINametagCompData 4, UIObjectivesCompData 1, UIScreenEventCompData
0, UISoldierCompData 16, UISpawnLogicCompData 4, UISpawnPointCompData 6, UITeamCompData 35, UITicketCounterCompData
13, UIWeaponCompData 63, UIVehicleCompData 82, UIVoiceCompData 12, UIPostProcessComponentData 1, UIPopupCompData 7,
UIMemCardCompData 2, UserComponentData 15, UIWidgetEventCompData 0, UIMinimapCompData 8, UIEndOfRoundCompData 52,
UISettingsCompData 89, UICustomizationCompData 59, UITrackingtagCompData 6, UISquadCompData 30, UISpectatorCompData
2, UIKillCounterCompData 26, UILevelCompData 23, UITextureStreamingComponentData 0, UICoopCompData 13,
UIPreRoundCompData 3, UIUnlocksCompData 23, UITeamSupportTagCompData 1, UILocalizeCompData 0,
UIDetailedServerInfoCompData 32, UIManagePlayersCompData 14, UIManageMapsCompData 37, UIHardcoreCompData 5,
UICoopEndOfRoundCompData 19, UI3dLaserTagCompData 1, UIPlaygroupCompData 10, UIRenderCompData 1, UIMatchCompData 4,
UIIngameMenuCompData 1, UIFlagCounterCompData 10, UIGunMasterCompData 11.

7 of the 58 have an empty `DataSources` list (HudCameraShakeData, UIDataInterfaceCompData, UIDirectAccessCompData,
UIScreenEventCompData, UIWidgetEventCompData, UITextureStreamingComponentData, UILocalizeCompData); over all 84 types
10 do (the three others: DiscComponentData, UINametagSPCompData, UIOnDemandFontComponentData) [verified: the 84
primary instances dumped]. Other facts used: `UICombatAreaCompData.OutOfAreaSidPrefix` "ID_H_COMBAT_AREA_";
`UISquadCompData` names Squad1 … Squad32 (Alpha … Celeste); `ui/uicomponents/uiweaponcomp` is also in Metro's
superbundle, the atlas and metadata are not.

### 4.7 Objectives

`bf3dump level MP_Subway ConquestSmall0`: 5 `CapturePoint` placements: HQs at (−108.03, 58.8, −147.5) and
(−193.69, ≈63, −610.25), a `flagpole` and two `flagpole4m` (CAFE, PLATFORM, TICKET_HALL in gap UI's list), matching
gap UI §2's points. Metro CQS has 83 spawn points (`bf3dump spawns MP_Subway ConquestSmall0`). `mode_def.h`'s
`CapturePointDef` has no position [verified], so positions come from the traversal.

---

## 5. Minimap: algorithms and decisions

### 5.1 Reading (bf3_core)

`read_minimap_data(db, LevelData instance, level_id, budget)`: walks `LevelDescription.Components`, takes the
first `UILevelDescriptionComponent` with non-null `MinimapData`, reads the fields of §4.1, resolves `DetailTexture`
and the two field params to partition names (= res names). Never reads `description_win32` for geometry (C5).
Refuses WorldSize ≤ 0 or non-finite, WorldRotation ≠ 0 (no retail level uses it; drawing a rotated map needs a
decision that has no data to check it against), thresholds outside [0, 1].

`read_minimap_level(LoadedLevel, World)`: `read_minimap_data` on the level root; every `PlacedObject` with label
`UICombatAreaEntityData` → `Asset` → `read_combat_area`; every `ObjectKind::CapturePoint` → `MinimapObjective`
(position from the placed transform, `CapturableType` CTUnableToChangeTeam → Hq, `InitialOwnerTeam`,
`LocationNameSid`, `LocationTextSid` with `Scopes::property` applied so interface overrides win, as the spawn code
does). Order: traversal order (deterministic).

Decision **D1**: the mode's masks come from the mode's own `UICombatAreaEntityData` placements, not from asset
names. Justification: §4.2 (Prefix inconsistent; one level can hold several modes' areas).

### 5.2 Mapping

`u = 0.5 − (x − WorldCenter.x) / WorldSize`, `v = 0.5 − (z − WorldCenter.y) / WorldSize`, row 0 at the top [gap UI
§2, re-verified by G4 / G5]. `minimap.h` holds it inline (`world_to_minimap_uv`, `minimap_uv_to_world`) so the
readers, the gates and the Godot side use one definition; `world_to_minimap_uv_variant` enumerates the 8 sign /
transpose alternatives for the gates (variant 0 = the research mapping).

### 5.3 Orientation evidence

* G4 (objectives in mask union): Metro 5 / 5 only for variant 0; Caspian 7 / 7 for variant 0 but alternatives give
  4–6 / 7, a weak discriminator on its own [verified: gap UI's orient.py re-run].
* G5: the TDM mask against the gameplay polygon (§4.2): IoU 1.000 for variant 0, 0.000 for all others; against its
  bounding box 0.70 vs 0.00 [verified: `polyiou.py` and orient.py on the decoded 128² mask, 351 inside texels].
  The polygon is our own data, so G5 needs no third-party reference (the fun-bots check of gap UI is excluded).

### 5.4 Combat-area masks in a mode with two

Metro CQS places two masks (cacs1, cacs2); BF3 presumably shows each team its own [inferred]. Which team reads which
is not in the data found so far (the entity has no team field visible in the dump; open question Q3). Decision
**D2**: this wave draws the **union** (per texel min of the two distance fields: inside either is inside), which is
also what G4 measures; per-team selection moves to the mode-logic wave (M4, Conquest) when teams exist. Lead for M4:
both of Metro's CQS `UICombatAreaEntityData` objects carry `IsEventConnectionTarget` 2, so another partition's event
connections probably enable one per team (both also carry `IsPropertyConnectionTarget` 3) [verified: `bf3dump dump`
of `levels/mp_subway/conquest_small/layer1_conquest_full_4_flags_small`].

### 5.5 The on-screen view

Godot canvas space (x right, y down, px). The player sits at the control's centre. On the image +Z is up and +X
is left; Frostbite and Godot map 1 : 1 (render_bridge.h) and, looking down +Z in Godot, +X is on the left, so the
image is not mirrored against the 3D view [verified: input_mapper.h, "moving the mouse right ... lowers the yaw"].
Heading is the sim yaw (0 = +Z, a higher yaw turns left).

* north-up offset of a world point: `sx = −(x − px)·ppm`, `sy = −(z − pz)·ppm`; marker arrow at clockwise angle
  `−heading`;
* rotating (BF3's in-game minimap, `UIMinimapCompData` PlayerIsCenter [inferred from its source name]): the map turns
  clockwise by `+heading` about the player, the view direction points up, the marker does not turn.

`minimap_project` / `minimap_unproject` / `minimap_marker_angle` implement exactly this, inline; `ui2.contract`
checks forward-is-up and left-is-left for 5 headings and the round trip. Scale: `ppm = size_px / (2 · range_m)`,
range 150 m at the default size [inferred; BF3's zoom levels are `UIMinimapCompData` sources, Client rows].

SDF edge width for the shader: `w = 0.01` in distance-field units (the shader's default `edge_width`, a soft edge of
about a texel; tuned by eye on the shots) [inferred]; the shader
computes `alpha = smoothstep(thr − w, thr + w, sdf)` with `thr = CombatAreaAlphaThreshold` (gap UI §6) [plan].

### 5.6 Images

`load_l8_texture` refuses anything but L8, 2D, one face, side ≤ 4096; takes mip 0. The satellite ships one mip, so
`l8_mip_chain` box-filters a chain (2048² → 1²: 5.6 MB in all) for `Image::create_from_data(…, mipmaps = true,
FORMAT_L8, …)`; Godot's own `generate_mipmaps` would do the same on the main thread, which this avoids. Masks and
SDFs are not mipmapped (128², linear filter).

## 6. HUD icons

### 6.1 Atlas

`read_hud_icon_atlas(ui resolver)` reads §4.4 into `HudIconAtlas` (data order, duplicates kept, `find` returns the
first entry with states, else the first). `hud_icon_rect(uv, 256, 1024)` rounds to whole texels (retail UVs are
multiples of 1/256 and 1/1024); `hud_icon_frame(state, t)` picks the frame from `FrameRate`. The atlas pixels go to
Godot as FORMAT_RGBA8 unchanged.

### 6.2 Item icons

`ItemIconIndex::read` collects every `ItemIds` entry of the weapon, kit-item and vehicle metadata (first description
listing an id wins: weapon, kit item, vehicle; duplicates counted). `find(WeaponDef unlock identifier)` →
`ItemIcon{res_name = lowercase(IconTexturePath)}` → `load_item_icon` (DXT5 kept as blocks: Godot FORMAT_DXT5, no
transcode). All 172 catalog variants resolve (§4.5). A miss on another level fails G7; at run time the kill feed
(M2b) would then show the item's localised name without an icon (no invented fallback art).

### 6.3 Vehicles

`read_vehicle_hud_icon` reads `HudData` for the minimap (vehicle icons on the map are M5 work; the reader exists so
the minimap's icon set is complete when vehicles spawn).

## 7. HUD data model

### 7.1 Shape

`HudModel` (hud_model.h): one fixed-size, trivially copyable struct per presented frame, like `PresentationFrame`:
a player table (64 rows) and one sub-struct per bound component type, each field named `snake_case(DataSource)`
and in the data's order: `HudWeapon` (UIWeaponCompData), `HudSoldier`, `HudCapturePoints`, `HudTickets`,
`HudKillCounter`, `HudMessages`, `HudMinimap`, `HudSpawnPoints`, `HudSpawnLogic`, `HudSquad`, `HudTeam`. Text travels
as SID hashes (the HUD translates), items as unlock identifiers (→ §6.2 icons), players as indices.

Decision **D3**: the model lives in bf3_core (Godot-free) and is filled by the client (bf3_host) in the waves that
own the data; the Godot HUD screens read a copy. Justification: plan §3.8 "our sim/HUD bridge exposes the same
names" [plan]; the copy keeps the main thread off the client's locks (the PresentationFrame pattern, m1.md §9.5).

### 7.2 Binding rows

`kHudBindings` (constexpr, 235 rows) lists **every** DataSource of the 11 bound types: `{component, source, kind,
wave, field}`. Kinds: Model 150 (a field above), Client 35 (state the HUD screen owns: selections, layouts, toggles,
zoom), Unused 50 (persistence, VOIP, console, the Scaleform crosshair init handshakes, GPS, teams 3–8, …). Per type:
Weapon 63, Soldier 16, Capturepoint 12, TicketCounter 13, KillCounter 26, Message 22, Minimap 8, SpawnPoint 6,
SpawnLogic 4, Squad 30, Team 35. Waves: M2a (gunplay HUD), M2b (Metro TDM with bots: health, death, kill feed,
scoreboard, spawn screen), M3 (chat), M4 (Conquest), M5 (air / sea), M7 (breadth) [plan §6].

The rows were generated from the install's ingamestatemp closure by a scratch script that asserts set equality
with the install; ctest `ui.hud.sources` asserts it again on every run (G8), so a data or row change fails loudly.

Decision **D4**: the repo holds only the names the model binds (rows and struct fields), never the generated table
(BF3 data). The table (`HudSourceTable`) is built at run time by `read_hud_source_table` for bf3dump and the test.

### 7.3 Unbound components

The other 47 in-game components are not bound in this wave. `bf3dump hud-sources --unbound` lists their sources.
Their waves: UIVehicleCompData (82) M5; UIKillCardCompData (27), UILifeCounterCompData (16), UINametagCompData,
UIDamageIndicatorCompData, UIEndOfRoundCompData (52), UIPreRoundCompData, UIInteractionCompData, UIKitCompData,
UICombatAreaCompData, UISpectatorCompData M2b; UIVoiceCompData, UIManagePlayersCompData, UIManageMapsCompData,
UIDetailedServerInfoCompData M3; UIFlagCounterCompData, UICapturepointtagCompData M4; UIGunMasterCompData,
UICoopCompData, UICoopEndOfRoundCompData, UIUnlocksCompData, UICustomizationCompData, UISettingsCompData M7 or never
(persistence) [inferred]; the 7 empty ones bind nothing.

## 8. Caching

Decision **D5: nothing new is cached.** Metro's `load_level` + `traverse` take 0.34 s + 0.04 s (`bf3dump level
MP_Subway ConquestSmall0`, warm OS cache, shared machine) [verified]; the minimap reads after them resolve a handful
of instances; textures are decoded at each load (L8 / RGBA8 / DXT5 need no conversion). A cache entry would need a
key from the level source digest, which itself needs the loaded level, so it would save only the 0.04 s traverse.
The hud-sources table is never needed by the game (it binds `kHudBindings`), so it is not cached either. If a later
wave wants a cache (e.g. all 29 levels' minimaps for a level browser), the kind would be `ui-minimap`, sources = the
LevelData partition's and the mode's sub-world partitions' manifest SHA1s, `params = "level=<id>;mode=<mode>"`,
`converter_version` and a blob `format_version` both 1 (cache_key.h conventions) [inferred].

## 9. Hostile-input bounds

| input | bound | on violation |
|---|---|---|
| any EBX walk (minimap, combat areas, objectives) | `level::WorkBudget` charged per iteration, as the level code does | error `budget` |
| atlas / metadata / sources reads | `gameplay::WorkBudget` per iteration | error |
| image side | ≤ 4096 (`kMaxMinimapImageSide`, `kMaxHudIconImageSide`); payload length checked against w·h (L8), w·h·4 (ARGB8888), blocks (DXT5) before allocating | error |
| combat areas / objectives per mode | ≤ 64 / 64 | error |
| atlas | ≤ 1024 icons, ≤ 16 states / icon, ≤ 64 frames / state; UVs finite, in [0, 1], min ≤ max; unknown state names counted (`unknown_states`) | error / counted |
| item icons | ≤ 4096 ids; paths ≤ 512 bytes | error |
| component types / sources / state components | ≤ 1024 / 4096 / 1024 | error |
| model text | fixed arrays (32 / 128 bytes, NUL padded, truncated at a UTF-8 boundary) | truncated |
| world values | WorldSize finite > 0, WorldRotation 0, thresholds in [0, 1], positions finite | error |

All new parsers run under the asan preset (G12) and `ui.minimap.fuzz` / `ui.hud.fuzz` mutate real partitions and
texture headers (in memory only) through every reader. Polygons: at most `kMaxAreaPolygonPoints` (1024) points, IoU
masks at most `kMaxIouSide` (1024) a side. `L8Image::at_uv` returns 255 for a non-finite coordinate.

## 10. Godot side

### 10.1 Hook

One node appended to `bf3-opensource/scenes/m1_metro.tscn` (additive, after the Session node):
`[node name="MinimapHud" type="Bf3MinimapHud" parent="."]`, and `GDREGISTER_CLASS` lines for the four classes in
`register_types.cpp` under a `// UI ui2` comment. The node finds `../Session` (`Bf3LevelSession`) and reads only its
public accessors (`done()`, `scene_enabled()`, `level_id()`, `mode_id()`, `install()`, `frame()`, `mapper().view()`,
`cameras()`, `save_screenshot()`, `finish()`) [verified: level_session.h]. Nothing in level_session changes (the lk
track edits it); the walkers (s3) appear through `frame()` like any other.

**Inert runs.** The session has no accessor for its args, so the node parses `OS::get_cmdline_user_args` itself. It
creates nothing, starts no loader and draws nothing under `--headless` or without a scene (`--m1-check`), with
`--no-minimap` or `--no-hud`, and in the session's measurement and reference-shot runs: `--m1-load-check`
(`godot.m1_load` reports frame times for 5 s after Done and the peak working set; a second Metro load in that window
would skew both), `--shot-spawns` and `--shots` (other tracks' reference shots, e.g. lk's `godot.l1_look`, stay free of
the minimap). `--minimap-check` / `--minimap-shots` turn it on for their own runs, which never combine with
`--m1-load-check`. In a plain windowed game it is on. User args in `minimap_hud.h`: `--no-minimap`,
`--minimap-mode=<mode>`, `--minimap-size=<px>`, `--minimap-north-up`, `--minimap-check`, `--minimap-shots=<dir>`;
key M toggles north-up.

### 10.2 Bf3MinimapControl

A Control (bottom-left; its side is 256 / 1080 of the viewport height by default, 341 screen pixels on the
2560 × 1440 display). Positions are canvas units: the project stretches `canvas_items` (project.godot, base 1152 × 648,
aspect expand), so one unit is the viewport's final-transform scale in screen pixels (about 2.2 on that display);
every read-back goes through `canvas_to_viewport()` (§10.4). Two inner canvas
items:

* the **map item** carries the ShaderMaterial (`kMinimapShader` in `minimap_control.h`: L8 satellite ×
  DetailTextureTint, optional detail SDF overlay, `smoothstep(thr − w, thr + w, sdf)` of the combat-area union mixed
  in with CombatAreaColor) and draws the satellite with `draw_set_transform(centre, ±heading, scale)` +
  `draw_texture_rect_region` so texel (u, v) lands where `minimap_project` puts its world point. The circular frame is
  a discard in the shader: `distance(FRAGCOORD.xy, clip_center_px) > clip_radius_px`, both uniforms in viewport
  pixels, set every frame from the control's final transform (FRAGCOORD is the viewport pixel, so this holds under
  any stretch or rotation). The `debug_uv` uniform replaces the colour by the packed texture coordinate
  (`encode_minimap_debug_uv`, minimap.h: 12 bits per axis in R, G, B, alpha 1) for the check;
* the **icon item** (no material: a CanvasItem material applies to all its item's draws) draws the objectives with
  `draw_texture_rect_region` of the atlas rect of their state, clamped to the rim, the marker
  `UIHudIcon_LocalPlayer` Default at the centre rotated by `minimap_marker_angle`, and the others.
  `set_icons_visible(false)` hides it for the check's map frames.

**Icon states** (`objective_icon` / `soldier_icon`, hud_icons.h, Godot-free and unit-tested). M1 has teams but no
selection: the local player is on team 1 (`kM1LocalTeam`, m1.md §8.2 [verified]) and the walkers on team 2 (m1.md
§8.3). Owner team 0 is neutral, the local team friendly, any other non-zero team enemy. Flags: `UIHudIcon_NeutralFlag`
Neutral, `UIHudIcon_FriendlyFlag` Friendly, `UIHudIcon_EnemyFlag` Enemy (each also has Neutral and Blink states; Blink,
a flag being captured, waits for the capture logic, M4). HQs: `UIHudIcon_Base` Friendly / Enemy by
`InitialOwnerTeam` (`UIHudIcon_FriendlyBase` {Friendly, Neutral} and `UIHudIcon_EnemyBase` {Default, Neutral} are not
used). The local player: `UIHudIcon_LocalPlayer` Default (Squad when squads exist, M2b). Others:
`UIHudIcon_Player` Friendly / Enemy / Neutral by team [atlas states verified: `bf3dump dump ui/static/hud3dicons`].
G10 expects exactly these entries and states.

### 10.3 Bf3WorldIcons

A full-screen Control: for each objective, `p = position + (0, VerticalOffset, 0)` (5.5 m); skipped when
`Camera3D::is_position_behind(p)`, else the icon centre is `Camera3D::unproject_position(p) + PixelOffset` (0, −28
canvas units), and it is scaled and faded with distance
by `UICapturepointtagCompData`'s own fields (IconSize 30, ScaleDistance 500, MaxScaleMod 0.4, FadeDistance 200 →
FadeEndDistance 800, MaxFarFade 0.5; `SnapIcons` true [field values verified, semantics inferred]). Off-screen
snapping is M2b.

All Godot API used exists in the pinned `extension_api.json` [verified]: `unproject_position`,
`is_position_behind`, `draw_set_transform`, `draw_texture_rect_region`, `draw_colored_polygon`, `draw_polyline`,
`draw_circle`, `Image.create_from_data`, `FORMAT_L8 / RGBA8 / DXT5`, `ShaderMaterial.set_shader_parameter`,
`get_viewport_rect`, `Image.save_png`.

### 10.4 The check (G9–G11)

`MinimapCheck` (minimap_check.h) runs in `godot.ui2` with `--minimap-check --minimap-mode=ConquestSmall0` (the M1
game's TDM has no objectives). **Its evidence is the viewport image**, read back with
`get_viewport()->get_texture()->get_image()` two frames after each change; the control's own record of where it drew
(`last_marker`, `last_icons`) only says where to look. Revision 1 compared the marker with `minimap_project` of the
player's own position, which is the control centre by definition, so it could not catch a misplaced satellite; this
revision measures the satellite's placement directly:

1. `readback.calibration`: a 4 × 4 opaque ColorRect of Color8(17, 34, 51) must read back as exactly those bytes (no
   colour conversion, debanding or blending between the canvas and the image; the project renders 2D without HDR).
   Every UV row depends on it, so it runs first.
2. `marker.spawn`: at the first drawn frame after Done (no input yet) the own presented (x, z) is within 0.5 m of a
   point of `shared_level()->spawns` (the session's spawn list, TDM 102 points).
3. `map.uv.north_up`: icon item hidden, `debug_uv` on. 49 sample pixels: the centre and three rings of 16 at 25, 50
   and 85 % of the disc radius. Each sample is a viewport pixel centre mapped to control-local units with the inverse
   of `canvas_to_viewport()`; samples whose expected UV is outside [0, 1] are skipped (the count is printed). The
   decoded UV must equal `minimap_expected_uv(view, mapping, sample)` = `world_to_minimap_uv(minimap_unproject(sample))`
   within `kMinimapUvCheckTexels` (1 satellite texel) per axis. As a control, for each of the 7 other mapping variants
   the largest error over the samples must exceed `kMinimapUvMirrorTexels` (10 texels), so the row can tell the
   research mapping from its mirrors. The centre sample is under the marker, which makes this the task's "marker
   against the spawn position mapped through the formula" check. Precision: the shader writes the UV interpolated at
   the pixel centre (not a filtered texel value) and the check computes the expected UV at that same pixel centre, so a
   correct draw differs only by the packing (at most 0.25 texel on a 2048² satellite) and float rounding, whatever the
   screen scale (at the default size one screen pixel covers about 1.8 texels of Metro's satellite); an offset, scale,
   mirror or rotation error of a texel or more fails.
4. `map.uv.rotating`: the same after a scripted 90° turn (`InputMapper::script_view`, yaw + π/2) in the rotating view;
   in addition the samples straight above the centre decode to world points in the view direction.
5. `marker.pixels`: normal shading, icons visible, against the icons-hidden frame: the changed pixels near the centre
   have their bounding-box centre within 1 canvas unit of the control centre, both views.
6. `icons.minimap` and `icons.world` as G10 states; the world-icon reference point is the check's own pinhole
   projection (the camera's `get_camera_projection()` and global transform multiplied out in the check) of the lifted
   point plus the pixel offset, so the comparison is not `unproject_position` against itself.
7. Screenshots (`--minimap-shots`): at the spawn, then with the view re-centred on an HQ and on a flag
   (`set_center_override`, the HUD only), each rotating and north-up; debug and icons-hidden frames are never saved.
8. `install.unchanged`; then `UI2 CHECK PASS|FAIL <row> <detail>` lines, `UI2 MINIMAP CHECK: PASS (n/n)`, and the run
   ends through `finish()`.

What stays for the eye (G11): the tint, the shading look and the icons' art; the positions are measured above.

## 11. Interfaces (in the tree, compiling)

| header | key declarations | seams for later |
|---|---|---|
| `src/bf3_core/include/bf3/core/ui/minimap.h` | `MinimapData`, `MinimapFieldParams`, `CombatAreaMask`, `MinimapObjective`, `MinimapLevel`; inline `world_to_minimap_uv(_variant)`, `minimap_uv_to_world`, `MinimapView`, `minimap_project / unproject / marker_angle`, the read-back contract `encode / decode_minimap_debug_uv`, `minimap_expected_uv`, `minimap_uv_error_texels`, `kMinimapUvCheckTexels`, `kMinimapUvMirrorTexels`, `sdf_inside`, `L8Image::at_uv` (non-finite safe); `read_minimap_data`, `read_combat_area`, `read_minimap_level`, `load_minimap_level` (`kMinimapHudLoaderThreads`), `load_l8_texture`, `load_minimap_images`, `l8_mip_chain`, `combat_area_union`, `mask_box_iou`, `mask_polygon_iou`, `read_area_polygon` | the large map / spawn screen (M2b) reuses `MinimapView` with `rotate = false`; per-team masks (D2) |
| `.../ui/hud_icons.h` | `HudIconState`, `HudIconUv / Rect`, `hud_icon_rect`, `hud_icon_frame`, `HudIcon`, `HudIconAtlas`, `read_hud_icon_atlas`, `HudIconChoice`, `objective_icon`, `soldier_icon`, `Rgba8Image`, `load_hud_icon_texture`, `ItemIconIndex`, `load_item_icon`, `VehicleHudIcon`, `read_vehicle_hud_icon` | kill feed / kill card / loadout screens (M2b) use `ItemIconIndex` |
| `.../ui/hud_sources.h` | `HudComponentType`, `HudSourceTable`, `hud_component_partitions`, `read_hud_source_table`, `format_hud_source_table` | |
| `.../ui/hud_model.h` | `HudModel` and sub-structs, `HudBinding`, `kHudBindings`, `hud_binding_count`, `hud_snake_case`, `HudBindingReport`, `check_hud_bindings` | the client fills `HudModel` (M2a+); HUD screens are Controls reading a copy |
| `src/bf3_godot/src/minimap_hud.h` | `Bf3MinimapHud` (`set_center_override`), `MinimapAssets` (kept, no pixels), `MinimapPixels` (freed after upload), `kM1LocalTeam` | the client's team replaces `kM1LocalTeam` (M2b) |
| `.../minimap_control.h` | `Bf3MinimapControl` (`set_debug_uv`, `set_icons_visible`, `canvas_to_viewport`), `kMinimapShader` (clip and `debug_uv` uniforms), `MinimapIconDraw` | |
| `.../world_icons.h` | `Bf3WorldIcons`, `WorldIconStyle` | other tag components (nametags, M2b) |
| `.../minimap_check.h` | `MinimapCheck` | |

`src/bf3_godot/src/ui2_header_check.cpp` compiles the four Godot headers until their `.cpp` files exist (deleted
then). No stub `.cpp` in bf3_core: nothing calls the declared functions yet.

## 12. Files

Created (this commit): the 8 headers above, `ui2_header_check.cpp`, `tests/ui2_contract_tests.cpp`, this document.
Modified additively (this commit; blocks headed `# UI ui2`): `src/bf3_core/CMakeLists.txt`,
`src/bf3_godot/CMakeLists.txt`, `tests/CMakeLists.txt`, `README.md` (own section, one line under the ctest
paragraph).

Implementation wave creates: `src/bf3_core/src/ui/{minimap,minimap_images,hud_icons,item_icons,hud_sources,
hud_model}.cpp`; `src/bf3_godot/src/{minimap_hud,minimap_control,world_icons,minimap_check}.cpp`;
`tools/bf3dump/ui_commands.{h,cpp}`; `tests/{ui_minimap_tests,ui_hud_tests,ui_install_tests,ui_fuzz_tests}.cpp`,
`tests/godot_ui2.cmake`. Modifies additively: `register_types.cpp` (`// UI ui2`), `bf3dump.cpp` (usage + dispatch
block), `bf3-opensource/scenes/m1_metro.tscn` (one node), the CMake blocks above, README.

## 13. Test plan

| ctest | data | what |
|---|---|---|
| `ui2.contract` (now) | none | the headers compile together; binding counts (static_assert), Model fields = snake_case(source), no duplicate rows; mapping on Metro's numbers, inverse, 8 distinct variants; view orientation (forward up, left left, 5 headings), round trip, north-up vs image agreement; `sdf_inside` at 126 / 127; `at_uv` clamping and non-finite input; debug-UV packing round trip (≤ 0.26 texel at 2048), edges and hostile values; `minimap_expected_uv` (centre = the player's texel, the 7 mirrored variants miss > 10 texels on a ring, forward is up); `objective_icon` / `soldier_icon`; `hud_icon_rect` on the LocalPlayer UV; `hud_icon_frame` |
| `ui.minimap.unit` | none (Godot-free doctest) | `l8_mip_chain` sizes and box filter, `combat_area_union`, `mask_box_iou` / `mask_polygon_iou` on synthetic masks (identity → 1, mirrored → 0; refused past `kMaxIouSide` / `kMaxAreaPolygonPoints`), `load_l8_texture` refusals on synthetic DxTexture headers (wrong format, side 8192, short payload) |
| `ui.hud.unit` | none | state name parsing, `ItemIconIndex` ordering / duplicates on synthetic rows, `check_hud_bindings` on a synthetic table (missing, extra), `format_hud_source_table` |
| `ui.minimap` | install, 77 without | G1–G5 (G5's polygon through `read_area_polygon` on Metro TeamDeathMatch0), G13 |
| `ui.hud.icons` | install | G6, G7, G13 |
| `ui.hud.sources` | install | G8, G13 |
| `ui.minimap.fuzz` | install | seeded mutants (default 250 per input, `OPENBF3_FUZZ_ITERS`) of `levels/mp_subway/mp_subway`, a ca asset, three DxTexture headers through the minimap readers: no crash, no hang (budget), G13 |
| `ui.hud.fuzz` | install | the same over `ui/static/hud3dicons`, `ui/uiweaponmetadata`, `ui/uikititemmetadata`, `ui/flow/state/ingamestatemp` and a component partition through the HUD readers; G13. Both fuzz tests are one executable with a filter argument; the names follow ui1's `ui.swf.fuzz` / `ui.text.fuzz` pattern so no name is ambiguous after the merge |
| `godot.ui2` | install + `OPENBF3_GODOT` + window; probe then `--minimap-check --minimap-mode=ConquestSmall0` like `godot.m1_load`; 77 otherwise; RUN_SERIAL, RESOURCE_LOCK `openbf3_godot_project` | G9–G11, G13, G14 |

The asan preset builds no GDExtension; `ctest --preset asan -R "^ui"` covers every bf3_core reader (G12). Every
ctest above gets the asan PATH block of the existing tests.

## 14. Deferred

| item | wave |
|---|---|
| per-team combat-area mask (D2), out-of-area warning (`UICombatAreaCompData`, ID_H_COMBAT_AREA_*) | M4 / M2b |
| Rush stage masks (a1–a5 / d1–d5 per stage) | M7 (Rush) |
| XP2 streaming floors (`UseStreamingMinimap`) | M7 |
| large map / spawn screen map (north-up, zoom, spawn points from `UISpawnPointCompData`) | M2b |
| vehicle icons on the map, air radar, radar sweep | M5 |
| kill feed / kill card with `_small` icons, scoreboard | M2b |
| filling `HudModel` from the client; HUD screens | M2a (weapon, soldier) / M2b / M3 / M4 |
| unbound components (§7.3) | per §7.3 |
| world-icon edge snapping, nametags | M2b |
| sharing the session's `LoadedLevel` instead of a second load | after the lk track lands (needs a level_session accessor) |

## 15. Implementation order

1. bf3_core minimap readers + images + gate helpers; `bf3dump minimap <level> [mode]` and `minimap --census`;
   `ui.minimap.unit`, `ui.minimap` (G1–G5).
2. hud icons + item icons; `bf3dump hud-icons`, `item-icons <level>`; `ui.hud.unit`, `ui.hud.icons` (G6, G7).
3. hud sources + `check_hud_bindings`; `bf3dump hud-sources [--unbound]`; `ui.hud.sources` (G8).
4. `ui.minimap.fuzz`, `ui.hud.fuzz`; asan run (G12).
5. Godot nodes, registration, scene line; `godot.ui2` (G9–G11, G14): calibration and UV read-back first, then the
   icons; screenshots to `b\shots\ui2\`, inspected.

## 16. Risks

| # | risk | mitigation |
|---|---|---|
| R1 | the M1 game runs TeamDeathMatch0, which has no flags | `--minimap-mode=ConquestSmall0` reads another mode's objectives and masks for the HUD only; the session's mode is untouched (Q5) |
| R2 | second `load_level` after Done: transient memory and a hitch | own thread at below-normal priority with 2 parse lanes, after Done; inert in the measurement and shot runs (§10.1); G14 reports its time and memory delta; the shared-level seam is deferred (§14) |
| R3 | (closed) the 14 gadget / M67 icons | measured by identifier: 172 / 172 (§4.5); G7 fails on any miss |
| R4 | team → mask mapping unknown | union (D2); Q3 |
| R5 | parallel tracks edit `m1_metro.tscn`, `register_types.cpp`, the CMake files | strictly additive blocks at the end, own node, no reflow |
| R6 | Godot 4.8 dev API drift (pinned dev7) | only APIs checked in the pinned `extension_api.json` (§10.3) |
| R7 | L8 sampling in the canvas shader returns luminance in `.r` [inferred]; the read-back could alter bytes | `readback.calibration` proves byte-exact read-back before any UV row; the satellite's look is checked by eye on the shots (G11) |
| R9 | merge: ui1 and ui2 both append to the same CMake files and README, s3 adds lines to `register_types.cpp`, and ui2 adds a node to `m1_metro.tscn` | trivial textual conflicts at the file ends, no semantic ones: no type names collide between ui1 and ui2 in `bf3::core::ui` / `bf3::godot_side`; lk's level_session changes leave every accessor this node uses unchanged (review of revision 1) |
| R8 | timings on the shared machine are noisy | the only timing row (G14) is loose and reported |

## 17. Open questions

* Q1: gap UI's 499 combat-area res entries vs 498 referenced fields found here (C2).
* Q2: plan / gap UI / the task state Bazaar's satellite as 512²; it is 2048² (C1). The gate uses 2048².
* Q3: which of a mode's two masks belongs to which team (lead: both entities are event-connection targets, §5.4).
* Q4: closed: all 14 gadget / M67 variants find their icon through `ui/uikititemmetadata` by identifier (§4.5).
* Q5: can the M1 game simply run `--mode=ConquestSmall0` (83 spawn points exist) instead of the HUD-only mode
  override?
