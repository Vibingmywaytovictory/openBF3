# M2a minimal HUD (track hud), as built

Track `hud` of M2a wave 2 (docs/design/m2a.md §15 row hud, §11.3, §11.4, §16 Q7, gate E10), on branch `m2a/hud`
from the W2.0 contract b8bef3f (and 7220da0, the build.cmd change that skips the timing rows). The HUD reads ui2's
`HudModel` and nothing else (§11.3): `fill_hud` fills the model from the presented own weapon, the own soldier and
the drained presentation events. The Godot controls draw the model.

## 1. What the track adds

| part | files | owner |
|---|---|---|
| the feed and the Godot-free layout | `src/bf3_host/include/bf3/host/hud_feed.h`, `src/bf3_host/src/hud_feed.cpp` | hud |
| the Godot HUD: `Bf3Hud` and its widgets `Bf3HudAmmo`, `Bf3HudCrosshair`, `Bf3HudHitIndicator`, `Bf3HudWeaponName`, `Bf3HudHealth` | `src/bf3_godot/src/hud_controls.{h,cpp}` | hud |
| the check `Bf3HudCheck` and its scene | `src/bf3_godot/src/hud_check.{h,cpp}`, `bf3-opensource/scenes/m2a_hud.tscn` | hud |
| tests | `tests/ui_hud_feed_tests.cpp` (`ui.hud.feed.unit`, `ui.hud.feed.install`), `tests/godot_m2a_hud.cmake` (`godot.m2a_hud`) | hud |
| shared files, one `# M2a hud` / `// M2a hud` block at the end of each | `src/bf3_host/CMakeLists.txt`, `src/bf3_godot/CMakeLists.txt`, `tests/CMakeLists.txt` (two blocks: the feed tests, then godot.m2a_hud), `src/bf3_godot/src/register_types.cpp` (three lines at the end of `initialize_openbf3` that declare and call `openbf3_register_m2a_hud()`, which `hud_controls.cpp` defines), `README.md` | shared |

m2a.md is unchanged. The W2.0 contract of `hud_feed.h` is kept: the header was extended (fields at the ends of
structs, new functions), and nothing was renamed or removed. The m2a.contract pins
`kHitIndicatorSeconds == 0.3` and `kHitIndicatorLevel == 1` (§16 Q7), and both are still named constants.

## 2. HudStatic

`make_hud_static(table, defs)` makes one row per def of the match's `WeaponTable`. `p_defs[i]` must be the source of
`table.defs[i]`. It fails when the spans differ in length or when a def's unlock identifier differs from the table's.

| row field | from |
|---|---|
| `name_sid_hash` | `WeaponDef::name_sid_hash`. When that is 0 (the M67 has no weapon SID), `apply_item_names(static, ItemIconIndex)` fills it from the item's `UIKitItemDescription.Name`: `ID_P_INAME_M67` = 0x26436abd = "M67 GRENADE" [verified 2026-10-16, ui.hud.feed.install] |
| `item_id` | the unlock identifier, which is the key of `ItemIconIndex` and the `_small` icon |
| `weapon_class` | 1 + the index of `WeaponDef::weapon_class` in `kHudWeaponClassNames` (16 names), or 0. **Deviation:** the resolver keeps the class as its enum text, not as the EBX enum value, so this is openBF3's own stable numbering, not BF3's WeaponClass value |
| `clip_capacity`, `magazines`, `unlimited_ammo` | `WeaponSimDef` |
| `fire_modes`, `mode_kinds[4]` | `hud_fire_mode(FireLogic)` per mode: Single / Burst / Auto (`HudFireMode`, -1 = none). `fire_modes` sets bit `1 << kind` |
| `crosshair` | `wcKnife` gives Knife; `wcHgr` gives Grenade; a missile projectile gives Launcher; pellets > 1 give Shotgun; the `wabsPstl` animation set gives Pistol; anything else is Rifle |
| `show_ammo` | false for the knife (unlimited ammo) |

## 3. fill_hud: the fields as fed

The data names its fields but not their units, so the units here are this track's choice [inferred].

* **With a valid weapon and a live soldier:**
  * Ammo:
    * `active_weapon_ammo` = magazine.
    * `_ammo_in_bag` = reserve (rounds).
    * `_ammo_total` = magazine + reserve.
    * `_clips_left` = ceil(reserve / capacity).
    * `_clips_total` = the row's `magazines`.
    * `_clip_capacity`.
    * `_critical_ammo` = magazine ≤ capacity × `kCriticalAmmoFraction` (0.25 [inferred]), for capacities above 1.
    * `_show_ammo`.
  * Reload: `_is_reloading` is true in the phases Reloading and ReloadShell. `_reload_percent` = phase progress × 100.
  * Identity:
    * `active_weapon_name` = the row's SID hash.
    * `active_weapon_data` = {item_id, name, class, fire_modes}.
    * `active_weapon_crosshair` = the `HudCrosshairKind`.
    * `active_weapon_index` = the slot.
  * Aim:
    * `dispersion` = `absolute_dispersion` = the presented spread in degrees.
    * `zoomed` = `active_weapon_zoomed` = the predicted zoom.
    * `firing_mode` = `mode_kinds[mode]`, and `firing_mode_types`.
    * `can_fire` = phase Ready.
    * `show_crosshair` = not zoomed and the weapon has a crosshair kind.
* **Without a weapon or a live soldier:** the weapon fields are cleared, so the crosshair and the ammo are off. Fields
  of later waves (secondary weapon, heat, range) are never touched.
* **Hit indicator (§16 Q7):**
  * Each drained `PresentKind::Hit` with `present_flags::kOwnShooter` sets the timer to `now_s + kHitIndicatorSeconds` (0.3 s).
  * `show_hitindicator` is true while `now_s` is before that time, and `hitindicator_level` is then `kHitIndicatorLevel` (1).
  * Other Hits are counted in `HudFeedState::hits_ignored` and never shown.
  * There are no client-predicted markers.
* **Soldier:** `is_alive`, `health` (finite, at least 0), `pose`, `first_person`, `show_hud_ammo`.
* `++serial` on every call.

## 4. Layout and drawing

* **Stage mapping.** BF3's HUD stage is 1280 × 720. A stage point maps to the window around its widget's anchor:
  `hud_stage_to_window`, scale = min(w / 1280, h / 720) (ui-swf-fonts.md §8.3). On 16:9 windows this is the stage
  point times the scale.
* **The text slots come from the install at run time.** They are not committed, because they are BF3 data. The
  loader reads the hudmpscreen tree (`read_layout`, imports followed) and `make_hud_screen_layout` walks it:
  * The walk is iterative and charges a WorkBudget per node.
  * It caps the depth at `kSwfLayoutMaxDepth × 4` and the node count at `kSwfLayoutMaxNodes`.
  * It finds the slots by instance path. The unnamed "/" and "%" are found by their initial text.
  * Retail values [verified, ui.hud.feed.install, 94 nodes, 0 missing]:

    | slot | position | size, alignment | bounds |
    |---|---|---|---|
    | Ammo_01 | (1194, 669), BottomRight anchor | | |
    | magazine (`Ammo_01/Ammo/clip/cltxt/txtDisplay`) | (1093.97, 625) | 43 px, right-aligned | x −2..102 |
    | reserve | (1199, 648) | 21 px, right-aligned | |
    | "/" | (1194, 649) | 18 px | |
    | health "100" | (1200.65, 588) | 21 px, right-aligned | |
    | "%" | (1253.66, 593) | 16 px | |

* **Text placement.** `hud_text_anchor` uses Flash's 2 px gutter. The baseline is one ascent below the top gutter,
  with the ascent measured from the drawn font. The right-aligned text ends at x_max − 2.
* **Font sizes** are the slot size × scale, rounded to whole pixels, because Godot font sizes are integers.
* **Fonts:**
  * The digits use the slot's own font, `$HUD_medium`, which is Venice-Medium as a FontFile.
  * The weapon name uses `$Menu_medium` (Purista EA Medium). Venice lacks letters. **Deviation [inferred].**
* **Crosshair:**
  * Four ticks at gap = `crosshair_gap_px(h, dispersion, vfov)` = (h/2)·tan(spread)/tan(vfov/2) around the window centre.
  * The ticks are 9 × 2 stage px, scaled, on a faint shadow [inferred]. The knife gets a dot. Shotguns also get a ring of radius gap.
  * vfov is `Bf3Hud::set_vfov_deg`. Without it the HUD uses the viewport's Camera3D (converted when the camera keeps the width), else 55° [inferred].
* **Hit indicator:**
  * The hitindicator movie's `#4/level1` art: 4 atlas pieces, four mirrored 15 × 15 sub-images at ±18 stage px, drawn at Hitindicator_01 (640, 360), scaled.
  * The art needs `ui/assets/hitindicator` among Bf3UiAssets' atlas movies (`Bf3Hud::required_atlas_movies()`). Without it the HUD draws procedural diagonal strokes and logs a warning.
* **Weapon name and icon** (no slot in what this track reads) [inferred]:
  * Both are right-aligned at stage x 1090, left of the magazine field, which starts at 1092.
  * The icon is 64 × 16 at y 638..654. The name sits below it, with its baseline at 676, at 16 px.
  * Icons: the loader reads the Win32/UI `ItemIconIndex` and loads the `_small` icon of every weapon and kit-item description: 203 item ids, 0 failures. They are kept as DXT5, as Godot `FORMAT_DXT5` ImageTextures with no transcode.
* **Health:** round(health) and "%" at the Health_01 slots, while the soldier is alive.
* **Threads.** `Bf3Hud` waits for Bf3Localisation and Bf3UiAssets. Its own thread then reads the layout, the item index and the icons, with no Godot API on that thread. Back on the main thread it makes the textures and emits `hud_loaded`. Measured load time: 0.02 s after the UI assets, 0.56–0.64 s from scene start. Until the load finishes, and for anything that failed, it draws from fallback placement, which is the retail values rounded.
* **Godot note.** The widgets are second-level extension classes (`Bf3HudWidget` → Control). Their registered `_draw`
  overrides were never called in this Godot 4.8 dev7 build: the first run had 0 draws on every widget. They now draw
  on `NOTIFICATION_DRAW` through a C++ virtual `draw_widget()`.

## 5. The check: godot.m2a_hud

`res://scenes/m2a_hud.tscn` has three nodes: Localisation, UiAssets and Check (`Bf3HudCheck`). Run it as
`-- --hud-check --hud-shots=<dir>`.

**Setup.**
* The check sets the atlas movies, then resolves the 12 kit items on its own thread: `open_install_weapons`, the catalog, `make_weapon_table` and `make_hud_static`.
* It applies the HUD's item index (`apply_item_names` fills 1 row, the M67).
* It drives a Bf3Hud inside a SubViewport at 1280 × 720, 1920 × 1080 and 2560 × 1440, shown scaled in a 1280 × 720 window with vsync off.

**The scripted model.** For each kit item, 96 frames of a 1/64 s model clock. At 1/64 s, 0.3 s is 19.2 frames, so a frame never ties with the boundary.

| frames | what happens |
|---|---|
| 0–7 | deploy |
| 8–39 | fire: magazine − 1 every 4 frames, spread rising from 0.4° to 2.6° |
| 40–55 | ADS at a 40° vertical FOV |
| 56–71 | reload, then refill from the reserve |
| 72–87 | hip at 75°, health 67, crouch then prone |
| 88–95 | dead |

Own Hit events come at frames 12, 44 and 76; other Hits (the own soldier as victim) at 26 and 50.

**Each frame.** The script's inputs go through `fill_hud` into the model. On the next frame every widget's draw record
is compared with the script's own expectations, not with the model.

| widget | what must hold |
|---|---|
| ammo | visible iff alive with an ammo weapon; magazine and reserve texts; the `$HUD_medium` FontFile; sizes = round(slot × scale); the clip's right end mapped back to stage x 1194 ± 2 |
| crosshair | visible iff alive and not ADS; gap equals the formula ± 0.5 px (the check recomputes it); centred |
| hit | visible iff the last own Hit was less than 0.3 s ago (counted in frames); visible on the frame its event is drained; one rising edge per own Hit; other Hits not shown; the atlas art drawn |
| name / icon | the expected localised string (the 12 names of E10) and the item's icon drawn |
| health | visible iff alive; round(health) |
| serial | every widget drew this frame's model |

**Shot frames.** Their state is held for 2 frames. The check then reads the viewport back and runs pixel checks:
* The crosshair's first lit pixel on the centre row lies within 1 px of centre + gap, beyond the half-pixel rasterisation.
* No crosshair pixels in ADS.
* Hit-indicator ink is present or absent in the four diagonal squares, as expected.
* Ammo digits and icon pixels are present.

The check then saves `hud_<w>x<h>_<label>.png`: hit, hip, ads and reload at each size, and one per kit item at
1920 × 1080, 24 PNGs in all. The shots show BF3 fonts and icons, so they go to
`C:\Users\olku\BF3 reimplementation\openBF3\b\shots\hud\` (gitignored), never into the repo. The last row is
install.unchanged.

## 6. Gates and measured results (2026-10-16)

The machine is shared. godot.m2a_hud draws frames and is not a timing gate; the times below are informational.

| gate | threshold | measured | command |
|---|---|---|---|
| E10 ammo | magazine / reserve every frame, 0 mismatches; `$HUD_medium`; at Ammo_01 (1194, 669) × min(w/1280, h/720) | 0 text, 0 font, 0 size, 0 placement mismatches over 1152 frames at each of the three sizes; clip right end at stage x 1194.00 at all three | `ctest --preset dev -R godot.m2a_hud -V` (OPENBF3_GODOT set) |
| E10 crosshair | gap = (h/2)·tan(spread)/tan(vfov/2) ± 0.5 px every frame; hidden in ADS | drawn gap vs the check's formula: max error 0.0000 px over 864 frames per size; pixel inner edge max 0.45 / 0.92 / 0.65 px (720p / 1080p / 1440p, tolerance 1 + 0.5 px); hidden on 192/192 ADS frames per size | same |
| E10 hit indicator | one per Hit event, within 1 frame, 0.3 s | 36 own hits → 36 indicators per size, 0 late (shown on the drain frame), 24 other hits never shown, 20 frames of 1/64 s per hit (shown until t + 0.297 s, hidden at t + 0.3125 s), atlas art on every shown frame | same |
| E10 weapon name | the localised string for 12/12 kit items, the M67 through its UI description | 12/12 at each size, 0 mismatching frames | same; also `ctest --preset dev -R ui.hud.feed.install -V` |
| E10 icon | the `_small` icon for 12/12 | 12/12 drawn at each size; 12/12 decode in ui.hud.feed.install | same |
| health shown | round(health) while alive | 0 mismatches per size | `ctest --preset dev -R godot.m2a_hud -V` |
| feed unit | fill_hud every frame, 0 mismatches; Q7 0.3 s / level 1; hostile inputs clamped; hostile trees fail | see §6.1 | `ctest --preset dev -R ui.hud.feed.unit -V` |
| asan | the feed and layout parser clean under ASan | see §6.1 | `tools\build.cmd asan --no-test` then `ctest --preset asan -R ui.hud.feed` |
| install.unchanged | layout.toc, cas.cat, .sb/.toc unchanged | 256 index files unchanged (godot.m2a_hud and ui.hud.feed.install) | the two commands above |

godot.m2a_hud: `M2A HUD CHECK: PASS (32/32)` in 5.0 s, with the HUD ready 0.56 s after scene start.

### 6.1 Unit and ASan results

`ui.hud.feed.unit` (no install) passes 8/8 cases on dev and on asan, with no AddressSanitizer report.

| case | result |
|---|---|
| static rows | 7 synthetic rows: kinds, modes, capacities, refusals |
| fill_hud | 600 scripted frames, 0 mismatches (magazine, reserve, name, ADS, reload, switch, death) |
| hit indicator | 3 own hits shown on a 64 Hz clock, 50 frames, restarted once; 2 other hits ignored |
| hostile feed | NaN spread and health, negative ammo, mode index 200 and an out-of-table def are clamped or cleared |
| crosshair gap | h 1080, 1°, vfov 55 gives 18.1067 px |
| stage mapping | 16:9 at the three sizes, plus 4:3 and 21:9 |
| synthetic screen tree | 35 nodes |
| hostile trees | a depth-200 chain and 5000 siblings on a 1000-unit budget both fail with an error |

`ui.hud.feed.install` passes on dev and asan:
* 12/12 names and 12/12 `_small` icons (64 × 16).
* Clip × spare magazines: 31 × 5 (M16A3, M4A1, AK-74M, AKS-74u), 46 × 4 (M27, RPK), 11 × 6 (MK11, SVD), 16 × 4 (M9), 18 × 4 (MP443), 1 × 0 (knife), 1 × 1 (M67).
* The hudmpscreen slots of §4: 94 nodes, 0 missing.
* install.unchanged: 256 files.

Wall time: unit 0.27 s on dev, 2.8 s on asan; install 0.5 s on dev, 6.5 s on asan.

### 6.2 Acceptance runs (2026-10-16, dev preset, OPENBF3_GODOT set)

* **Full ctest without the timing rows** (build.cmd's TIMING regex, `-j 6`): 90/90 passed. `core.cache_data` and
  `core.res` were skipped as before.
* **The timing rows, serial (`-j 1`) under this track's quiet lock** (held 13:14 to 13:39): 23/23 passed. This
  includes godot.m1 (348 s), godot.m1_load, godot.ui2, godot.ui_fonts(_shots), godot.s3_crowd, godot.l1_look,
  godot.m9_reload, godot.s0_selftest, godot.inspector, godot.collision, host.session(.unit), host.server,
  host.scale(.unit), sim.nav.bake, sim.weapons.perf, tools.fx_bench, logic.perf, tools.ebx_memory, tools.m1_prepare
  and render.scene.
* **godot.m2a_hud under the same lock:** `M2A HUD CHECK: PASS (32/32)` in 4.5 s (6.5 s for the ctest row).

Timing rows are wall-clock and noisy on this shared machine. The figures above come from one run each.

## 7. Open items and handoffs

* **godot.m2a's "hud" case** (m2a.md §1 E10, the integrated game) is the integrator's to do. That case needs a
  Bf3Hud in the game scene, with the following wiring:
  * `Bf3UiAssets.set_atlas_movies(Bf3Hud.required_atlas_movies())` before the assets load.
  * Each display frame, build a `HudFeedInput` from cl's `present_weapon` and the own `PresentedSoldier`, run `fill_hud` with the drained events, and call `set_model`.
  * `set_vfov_deg`, or leave the HUD to read the scene's Camera3D.
  * Build `HudStatic` once per match from the table, plus `apply_item_names(static, *hud->item_index())` after `hud_loaded`.

  This track did not edit cl's or the game's files.
* **Not drawn:**
  * the fire-mode indicator (the fields are fed);
  * the reload progress (fed as `_reload_percent`);
  * the HUD background panels and frames of the ammo and health widgets;
  * the grenade count, the secondary weapon and the compass.
* **[inferred] choices to revisit against retail footage:** the critical-ammo fraction, the colours, the crosshair tick
  size, the name and icon placement, and the 55° default vfov.
