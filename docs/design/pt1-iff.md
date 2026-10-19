# PT1 iff: telling friend from foe, as built

Playtest track "iff" (PT1), on branch `pt1/iff` from main 6a4061e. The playtest said: "can't tell who is friendly and
who isn't". This track finds what BF3 itself uses, from its data, and builds it:

* the 3p soldier appearance per team **and kit**;
* the friendly nametags and 3D icons (UINametagCompData and the hud3dicons Friendly / Squad / Enemy states);
* the name under the crosshair in the team colour;
* friendly and enemy arrows on the minimap.

It also checks the kill feed and scoreboard colours. Everything named [inferred] is a default with `measured = false`
(no VU, no retail measurement).

## 1. Commits

| commit | what |
|---|---|
| 19ae63a | `bf3/core/ui/nametags.{h,cpp}` (readers and rules), `bf3/core/gameplay/kit_appearance.{h,cpp}`, the `# PT1 iff` block of `src/bf3_core/CMakeLists.txt` |
| 6794636 | named insertion `[context]` in `level_session.h`: `pt1_iff_world()`, `pt1_iff_ui_assets()` (defined in `pt1_iff.cpp`) |
| aa7d49f | `src/bf3_godot/src/iff_frame.{h,cpp}` (Godot-free), `pt1_iff.{h,cpp}` (Pt1Iff, Bf3Nametags, `--iff-shots`), the `# PT1 iff` block of `src/bf3_godot/CMakeLists.txt` |
| 4a1a653 | named insertion `[register]` in `register_types.cpp` (Bf3Nametags) |
| 7f4a8fd | named insertion `[hud]` in `minimap_hud.{h,cpp}`: Bf3MinimapHud owns a Pt1Iff once a MatchState arrived (M2b only) |
| c37a882 | named insertion `[minimap]` in `minimap_hud.cpp` `update_view`: Pt1Iff draws the other soldiers; M1's loop runs only without a Pt1Iff |
| eb42b2e | `[kits]`: named insertions in `soldier_assets.{h,cpp}`, `skin_upload.{h,cpp}` and `game_crowd.cpp`, and a named replacement of `build_team_meshes` in `skin_upload.cpp` |
| b2e6a8b | rows `pt1.iff.unit` and `pt1.iff.install` (`tests/pt1_iff_tests.cpp`, the `# PT1 iff` block of `tests/CMakeLists.txt`) |
| 7b66459 | `--iff-shots`: ground spots for every view, a 240 s timeout; kit icons drawn in the relation's tint; frame cost in the summary |
| (docs) | this file and the README section "PT1 iff" |

## 2. Root causes (diagnosed before the fix)

**RC1. The minimap drew every other soldier as an enemy.** On main, `minimap_hud.cpp:463` runs M1's loop over
`present_others` with `ui::soldier_icon(false, 2, kM1LocalTeam)` ("M1's walkers are team 2"). That gives every other
soldier the orange UIHudIcon_Player Enemy arrow, teammates included. The M2b scene never replaced it.

Measured in the M2b scene (31 bots, `--preround=120`; Pt1Iff counts what the M1 rule would have drawn): **31.0 orange
arrows per frame, 15.0 of them teammates.** On recorded frames (`pt1.iff.unit`, `frames.before_after`), 41 frames
give 287 enemy arrows, 123 of them teammates.

**RC2. Nothing marked a soldier in the world.** On main, `git grep -i nametag -- src` finds only a comment in
`hud_sources.h`. No code reads UINametagCompData, and there is no 3D icon, name or kit icon over any soldier. The
data has all of it: `ui/uicomponents/uinametagcomp` (UINametagCompData) gives

* FriendlyTint (0, 0.8, 0.8), SquadTint (0.455, 0.863, 0.8), EnemyTint (1, 0.376, 0);
* DrawDistance 10000, FadeDistance 0 .. FadeEndDistance 10000, MaxFarFade 0, ScaleDistance 60, MaxScaleMod 0;
* IconSize 30, VerticalOffset 0.24, PixelOffset (0, -4);
* look-at thresholds 40 .. 240 within MinLookAtThresholdDistance 50;
* ShowLabelRange 15, NameFontSize 12, NameGlowSize 8.

The values were read with `pt1.iff.install` and `bf3dump dump`. The soldier blueprint (`characters/soldiers/mpsoldier`)
has SpottingTargetComponentData (ActiveSpottedTime 7, PassiveSpottedTime 1.5, SpotOnFireMultiplier 0.45) and
SoldierEntityData.ShowNametag true.

**RC3. No name under the crosshair.** It is the same gap as RC2: a name is part of the nametag (enemies: only while
looked at).

**RC4. One 3p model per team for every kit.** `game_crowd.cpp` `load_body` uses the default `render::SoldierSpec`.
Its two `SoldierTeamSpec::appearance` entries are the assault appearances (`mp_us_assault_appearance01`,
`mp_ru_assault_appearance01`; `soldier_assets.h:78-80`). So the crowd drew **2 meshes**, one per team, and engineers,
supports and recons looked like assaults.

The teams did differ, by design (US / RU appearances). That was not enough on Metro: both teams wear desert camo, and
the lower body is the same mesh `us_lb03_mesh` (variations `us_lb03_desert` / `ru_lb03_desert`). The data's kit chain
gives each kit its own appearance:

* `gameplay/kits/<kit>` (VeniceSoldierCustomizationAsset) leads to the VisualTable;
* the VisualTable's `UnlockParts[0].SelectableUnlocks[0]` is the default camo `u_camo_default`, gated by no progression
  table;
* the camo's `.LinkedTo` names the 8 `mp_<us|ru>_<kit>_appearance01`.

The kit's own appearance is picked from those by name (§3). `pt1.iff.install` measures 8 distinct appearances, 4 per
team: US 4 / 4 / 4 / 3 parts, RU 3 each.

**RC5 (no change needed). The kill feed and scoreboard colours** are already relative to the local team:
`tdm_player_colour` gives the local player yellow, the local team blue and the other team orange. The data does not
name these colours. A `bf3dump dump` of every `ui/uicomponents/*` partition on Metro found tints only in
UINametagCompData; the kill log and the scoreboard are SWF screens (`ui/assets/scoreboard*`). So the
`kTdm*Colour` constants stay [inferred] and unchanged.

## 3. What the data does not say (inferred, `measured = false`)

* **Units.** IconSize, PixelOffset, the look-at thresholds and the font sizes are units of BF3's 1280 x 720 HUD stage
  (`ui::kNametagStageHeight`). The Godot side scales them by canvas height / 720.
* **Anchor.** VerticalOffset is metres above the eye; then PixelOffset on screen.
* **Look-at radius.** It is linear from MaxLookAtThreshold (240) at 0 m to MinLookAtThreshold (40) at 50 m and beyond.
  At 35 m it is 100 stage units.
* **Teammates.** The UIHudIcon_Player Friendly (Squad) frame shows always, through walls (DrawDistance 10000). The kit
  icon (the kit's UIHudIcon, Default frame in the tint) and the name in FriendlyTint show within ShowLabelRange
  (15 m) or when looked at.
* **Enemies.** Nothing shows, except the name in EnemyTint while looked at **and** a static-world ray (Collision +
  Raycast, the bullets' mask) from the eye reaches the chest or head. At most `kMaxLosRaysPerFrame` = 4 rays per frame.
  BF3 marks enemies only when spotted, and M2b has no spotting.
* **Minimap.** It shows every live teammate (Friendly arrow) and an enemy only while revealed by its own fire, for
  ActiveSpottedTime x SpotOnFireMultiplier = **3.15 s** after each shot. "A shot" means a firing sample with a new
  sample tick.
* **The kit's appearance by name.** Kit `gameplay/kits/<faction><role>` takes `mp_<faction>_<r>_appearance<n>` where
  `<role>` starts with `<r>` (`usengineer` -> `mp_us_engi_appearance01`). Exactly one candidate must match, or the
  reader fails and the kit wears the team's assault appearance (a note).
* **Name glow.** NameGlowSize is drawn as a black outline of half its size.

Squads do not exist in M2b (squad 0 everywhere), so the Squad states never show. The rules already handle them.

## 4. The fix

* **Readers** (`bf3_core`, Godot-free, hostile-safe):
  * `ui::read_nametag_style` and `ui::read_spotting_rules`: a value that is not finite or out of range keeps the
    retail default and is counted in `clamped`. A missing or wrong-typed partition is an error, and the user of the
    reader falls back to the retail defaults.
  * `gameplay::read_kit_appearance`: WorkBudget-charged, at most 4096 LinkedTo entries.
* **Rules** (pure): `tag_relation`, `look_at_threshold`, `decide_nametag`, `tag_tint`, `minimap_relation`.
* **`iff_frame`** (Godot-free, on recorded frames in the unit row):
  * `iff_soldiers` gives each presented soldier its team (the weapon sample's 2-bit team, else the PlayerList row),
    kit, name and bot flag;
  * `IffReveal` tracks the fire reveal;
  * `iff_minimap` lists what the minimap draws.
* **Pt1Iff** (owned by Bf3MinimapHud in an M2b match):
  * A loader thread reads the style, the spotting rules and the kits' UIHudIcon from the level superbundle (0.13-0.15 s
    measured).
  * Every frame it appends the minimap arrows by relation (replacing M1's loop) and feeds the nametags.
  * It draws through **Bf3Nametags**, a full-screen Control under the minimap. The hud3dicons atlas frames are drawn
    at 1 texel per stage unit, capped by IconSize. The names use the `$Menu_medium` BF3 font.
  * `--iff-shots=<dir>` does the capture in §5.
* **Kits** (`[kits]`):
  * `SoldierTeamSpec::team_data` (empty means S3, unchanged) joins every kit's appearance parts to the team's parts,
    and `SoldierPart::kit_mask` says which kits wear each part.
  * `build_team_meshes` makes one mesh per (team, distinct part set). Textures and materials are made once.
  * `SkinUploader::set_soldier_kit` swaps an instance's base: `instance_set_base` for RS, `set_mesh` for Skeleton3D.
    The skeleton stays attached because the binds are per team.
  * `Bf3GameCrowd` names `gameplay/teams/us` and `/ru`. Each crowd index wears its PlayerRow's kit while alive, and a
    corpse keeps the kit of the life it died in. The own corpse wears `own_kit`.
  * M2a's server runs no TDM rules (rules mode None), so it sends no PlayerList and no MatchState. Its crowd stays on
    kit 0, so its look is unchanged. It still loads the kit parts; the soldier assets load in 1.34-1.45 s, against
    1.35 s without them, which is within the noise.

## 5. Measured before / after

Shared machine, 2026-10-19, `b\dev`, warm cache. The Godot numbers come from `--iff-shots` runs in the M2b scene with
31 bots and `--preround=120`, run windowed four times. The last run had 1956 frames.

| | before (main 6a4061e) | after (`pt1/iff`) |
|---|---|---|
| minimap arrows per frame | 31.0 orange, 15.0 of them teammates | 15.0 Friendly (blue) + 0.0 revealed enemies (16.0 enemies hidden; nobody fires in the pre-round) |
| recorded frames (`frames.before_after`, 41 frames) | 287 enemy arrows, 123 teammates as enemies | 123 Friendly + 30 revealed-enemy arrows (one shot, 3.15 s), 0 teammates as enemies |
| nametags per frame | none (no code) | 10.1 friendly icons, 1.30 friendly names, 0.363 enemy names |
| LOS rays (1956 frames) | none | 721 rays, 5 blocked |
| Pt1Iff main-thread cost | none | 27.5 us per frame on average, 603.9 us at most |
| 3p meshes | 2 (one per team; all kits assault) | 8 distinct (team, kit) meshes; US 15 body parts, 118 binds (was 113); RU 12 body parts, 113 binds (was 107); cap 256 |
| soldier assets load (`render.crowd_weapons` vs `pt1.iff.install`, indicative only) | 1.35 s (S3 default spec) | 1.34-1.45 s with every kit, 55 textures |
| process start to first controllable frame (indicative only) | not re-measured here (5.9 s in the M2b notes) | 6.22-6.34 s in the four capture runs |

**Screenshots** are in `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\pt1-iff\` (gitignored, BF3 content, never
committed). Each nominal distance is followed by the measured eye-to-anchor distance:

* `iff_teammate_06m.png` (6.0 m): Friendly arrow, kit icon and name in cyan;
* `iff_teammate_15m.png` (12.8 m): the same, half behind a car;
* `iff_teammate_35m.png` (35.0 m): the same with the crosshair on him;
* `iff_enemy_06m.png` (6.0 m), `iff_enemy_15m.png` (12.8 m), `iff_enemy_35m.png` (29.8 m): no icon, the name in
  orange because the crosshair is on him;
* `iff_atlas_frames.png`: the hud3dicons frames used, x4.

In the teammate shots, the far teammates show only the small blue arrow. The minimap in every shot shows blue arrows
only.

What the user has to judge: the size of the 12-stage-unit names and the 16-unit kit icons (10.8 px and 14.4 px at the
648 px canvas), and whether the arrow-only far tag reads well.

## 6. Checks (one command each, from the worktree)

* `ctest --test-dir b\dev -R "^pt1\.iff\.unit$"` (data-free, 0.24 s) covers:
  * the relation;
  * the look-at radius: 240 / 140 / 40 / 40 at 0 / 25 / 50 / 500 m;
  * friendly nametags at 6 / 15 / 35 / 900 m, looked at, dead, behind;
  * enemy nametags at 6 / 15 / 35 m with and without a clear ray;
  * hostile numbers;
  * the minimap's reveal window;
  * the naming rule (17 cases);
  * recorded frames (teams, kits, names, one shot revealing for 30 frames, a death, before / after counts, a hostile
    frame).
* `ctest --test-dir b\dev -R "^pt1\.iff\.install$"` (77 without an install, 1.7 s) covers:
  * style and spotting values from data, 0 clamped;
  * 8 distinct kit appearances, the default camo ungated 8/8, every appearance linking parts, assault = the S3
    appearance;
  * the crowd's assets with `team_data`: 4 distinct part sets per team, binds <= 256;
  * 250 / 50 / 250 seeded mutants of uinametagcomp / mpsoldier / usengineer (an error or a value each);
  * install.unchanged.
* Asan: `cmake --build --preset asan --target test_pt1_iff`, then `ctest --preset asan -R "^pt1\.iff"`.
* The capture (windowed, under two minutes; never in a pass):
  `"%OPENBF3_GODOT%" --path bf3-opensource res://scenes/m2b_metro.tscn -- --deploy-screen=0 --preround=120 --iff-shots=<dir>`.
  It exits 0 when every view was saved. The log carries `[pt1-iff]` lines: the data read, each view's decision, and
  the summary with the per-frame counts above.

## 7. Merge-pass rows the diff touches

* `godot.m2b.play`, `godot.m2b_load` (WINDOW): the M2b minimap and nametags, and the kit meshes (load). Expected to
  pass. The crowd's body thread loads the kit parts too. It ran for 2.49 s, and the session did not wait on it
  ("waited 0.00 s").
* `godot.m2a`, `godot.m2a.kits`, `godot.m2a.play`, `godot.m2a_firefight`, `godot.m2a_load`, `godot.m2a_hud` (WINDOW)
  and `godot.m2a_tp_crowd` (HEADLESS, branch pass): the crowd loads every kit's parts. The look is unchanged (kit 0,
  no PlayerList in M2a), and the minimap is unchanged (no MatchState, so no Pt1Iff).
* `godot.s3_crowd` (WINDOW), `render.s3*`, `render.crowd_weapons`: `build_team_meshes` was replaced, but with S3's
  spec every part has mask 0x0F, so the meshes are one per team, as before.
* `godot.ui2` (WINDOW): `minimap_hud.cpp` changed, but without a MatchState the M1 loop runs as before.

## 8. Open issues

* **No spotting (Q) in M2b.** Enemies are revealed on the minimap only by their own fire. Spotted enemies (the Enemy
  3D icon over them, 7 s) wait for a spotting mechanic.
* **No squads.** The Squad tint and frames are ready but never used.
* **No revive marker over downed teammates.** UIHudIcon states for "needs revive" are not wired.
* **Bot names.** The server names every bot "bot" (`tdm_rules.cpp:727`), so every bot's tag reads "bot".
* **The kill feed and scoreboard colours stay [inferred].** No data names them (RC5).
* **The weapon in a 3p kit's hands** comes from the crowd's item lists (as before). The appearance is per kit, but no
  appearance-specific gear (kit pouches) beyond the appearance's meshes is drawn.
* **Every rule in §3 is inferred** (`measured = false`). VU measurement is parked.
