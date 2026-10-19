# PT1 HUD study: what BF3's HUD movies contain and how close we can get

Branch `pt1/hud` from main 6a4061e. This is a study, not a fix. The M2b playtest found that "the HUD isn't what
Battlefield 3 has". This file covers four things:

* what the retail HUD movies hold, measured through the repo's SWF reader;
* how our M2b HUD differs from them, element by element;
* the routes to close the gap, with what each costs;
* one recommendation with its first wave. Choosing the route is the user's decision.

The file holds words and numbers only. No BF3 pixels, bytes or dumps are committed. The renders and the side-by-side
images are in `b\shots\pt1-hud\`, which is git-ignored (§3). The tools that made them are scratch tools and are not
committed.

## 1. Summary

* BF3's HUD is fully described by data we already read. Positions, sizes, fonts, colours, filters, atlas pieces and
  timelines all come from the movies. Our M2b HUD took the positions of a few text slots from them (ammo, health)
  and **hand-designed everything else**:
  * the circular minimap;
  * the top-centre team counter;
  * the bottom-centre kill card;
  * the single-panel scoreboard;
  * the centre round-end panel;
  * the kit-card spawn screen.
  These look different from BF3 because they are not BF3's layouts.
* The art is simple. The movies hold 1,159 shapes, and their fill styles are almost all bitmap pieces of texture
  atlases: 1,088 of 1,403. The rest are 315 solid fills. There are **0 gradient fills** and 9 line styles in all 434
  movies. Text uses DefineFont3 glyphs (already converted, ui-swf-fonts.md). The only effects are:
  * drop shadow and glow filters;
  * colour transforms;
  * one blend mode (add);
  * a few clip masks;
  * Scaleform's 3D tilt on four HUD widgets (`_xrotation` / `_yrotation` -15/±15, `_perspfov` 25).
* The behaviour is in AS2. `actionscriptlibrary` holds 237 classes (643,485 bytes of AVM1 bytecode), plus Tweener and
  helpers. The engine pushes data into the widgets through about 164 `update…Data` entry points in the 23 classes
  these screens use (121,614 bytes).
* A scratch Python renderer of about 1,100 lines drew every state from the data alone, with no AS2:
  * in round;
  * the Tab scoreboard;
  * the kill card;
  * the spawn screen;
  * pre-round;
  * the two round-end screens.
  Each state takes 0.2-5 s at 2560 x 1440. The renders look like BF3's HUD in layout, typography and colour. This is
  the main evidence for the recommendation.
* **Recommendation: route (b)**, a C++ SWF display-list renderer in Godot with each widget's AS2 rewritten as a C++
  controller fed from `HudModel`. It takes 3-4 waves. The first wave (§7) is the renderer plus the in-round HUD.

## 2. Inventory (measured)

### 2.1 Census

`bf3dump swf census` (main's build) over the install:

| measure | value |
|---|---|
| SwfMovie resources | 434, 0 errors |
| PlaceObject2 | 27,832 |
| DefineShape 1 / 2 / 3 / 4 | 982 / 53 / 117 / 7 |
| DefineEditText | 1,488 |
| DefineSprite | 2,783 |
| DefineFont3 | 86 |
| GFx DefineSubImage (tag 1008) / DefineExternalImage2 (tag 1009) | 820 / 135 |
| DefineScalingGrid | 29 |
| DoAction / DoInitAction | 2,456 / 1,538 |

The tag totals equal ui-swf-fonts.md §4.2. GFx tag 1008 is the 12-byte sub-image record: id, image id, x1, y1, x2,
y2. Tag 1009 is the external image. Both are confirmed by the counts.

### 2.2 Shapes and fills

The scratch reader decodes the shape records that the repo's `SwfShape` only counts:

* 1,159 shapes.
* Fill styles:
  * solid: 315;
  * clipped bitmap 0x41: 1,014;
  * clipped bitmap 0x43 (non-smoothed): 74;
  * **linear or radial gradient: 0**.
* 9 line styles in the whole install.

So every gradient you see in BF3's UI (header bars, the kill-card header, the ticket meters) is a **bitmap piece**,
stretched by the matrix and tinted by a colour transform. Examples:

* `textures:HeaderGradient` = `textures_i1.tga` [242,0 200x16];
* `textures:BkgGradient`;
* `hudtextures:Gradients_Glow` = `hudtextures_i3.tga` [486,0 72x72].

### 2.3 Effects used by the HUD screens studied

Counted by the scratch renderer while drawing each state:

| state | sprites | shapes | bitmap fills | solid fills | texts | glyphs | filters | masks | add blend |
|---|---|---|---|---|---|---|---|---|---|
| in round | 129 | 51 | 63 | 8 | 44 | 224 | 35 | 1 | 0 |
| scoreboard (in round + 32 rows) | 234 | 87 | 97 | 10 | 248 | 678 | 37 | 1 | 0 |
| kill card (in round + KillCam) | 111 | 47 | 55 | 6 | 51 | 279 | 42 | 2 | 1 |
| spawn screen | 45 | 28 | 25 | 3 | 12 | 78 | 12 | 0 | 0 |
| pre-round (in round + waiting screen) | 122 | 53 | 63 | 10 | 32 | 215 | 23 | 1 | 0 |
| round end (winning team) | 3 | 0 | 0 | 0 | 3 | 36 | 3 | 0 | 0 |
| round end (top players, 3 rows) | 16 | 8 | 4 | 4 | 9 | 30 | 0 | 0 | 0 |

Filters are only DropShadow and Glow, which PlaceObject3 stores as FIXED 16.16 blur, distance and strength. Example:
the ticket count's drop shadow is (0,100,200), blur 5, distance 1, strength 1.7. Masks are clip-depth layers, for
example the compass strip window. The add blend is blend mode 8, used on the kill card's glow piece.

### 2.4 Engine-drawn parts

Some widget parts are drawn by the engine, not by the movie. The AS2 names these parts through `rendererString`:

| rendererString | where | what the engine draws there |
|---|---|---|
| `t2;iBgDraw` | HudBackground `renderHolder` | the dark panel behind the minimap and ticket rows (fades: horizontal 20 %, vertical 4.25 %) |
| `t2;iUIMinimapRender` | Minimap `map` | the map itself |
| `t2;iUISoldierCompRender` | Health `EKGRenderTarget` | the EKG trace |
| (OverviewMap) | spawn screen `OverviewMap_01/map` | the full overview map |
| image manager | KillCam weapon image, kit icons | item icons by name |

The renders draw these parts as flat placeholder rectangles. We already have matching sources for most of them: the
minimap renderer (ui-minimap-icons.md) and the `_small` item icons (m2a-hud.md).

### 2.5 3D tilt

Read from the AS2 of `Widget.Hud.*`, with constants from `fb.Defines`:

| widget | `_xrotation` | `_yrotation` | `_perspfov` |
|---|---|---|---|
| Minimap_01 (map and icons) | -15 | -15 | 25 (`DEFAULT_HUD_PERSPECTIVE`) |
| Compass_01 `m_heading` | -15 | -15 | 25 |
| Ammo_01 | -15 | +15 | 25 |
| Health_01 | -15 | +15 | 25 |
| menus (for reference) | -5 | ±5 | 55 |

The scratch renderer draws this as a homography:

* rotate about X, then about Y, around the widget's pivot;
* apply the perspective about the stage centre, with focal length = half the stage width / tan(fov / 2).

The projection centre is [inferred]. The result looks like BF3's tilted corner panels. `inround_flat` is the same
state without the tilt, for comparison.

### 2.6 AS2

| part | bytes |
|---|---|
| all AVM1 bytecode in the 434 movies | 1,638,265 |
| `actionscriptlibrary` (237 classes) | 643,485 |
| `menuactionscriptlibrary` (111 classes) | 281,447 |
| every other movie | 713,333 |

The packages of `actionscriptlibrary`:

| package | bytes |
|---|---|
| Widget.List | 132,690 |
| Widget.Hud (43 classes) | 91,289 |
| Widget.Message | 52,764 |
| fb (Tweener, helpers) | 51,289 |
| ScreenSpecific | 46,176 |
| Button | 35,707 |
| Bar | 29,489 |
| Icon3D | 25,485 |
| Grid | 22,978 |
| Text | 18,538 |
| TicketCounter | 12,520 |
| Main | 12,421 |
| Crosshair | 10,798 |

The widget movies themselves hold only a `registerClass` stub of a few hundred bytes. All their logic is in the
library.

The classes these screens use:

| class | bytes |
|---|---|
| Ammo | 5,085 |
| Compass | 3,776 |
| HudMessage | 3,709 |
| Minimap | 3,104 |
| SquadList | 2,178 |
| HudBackground | 1,777 |
| HudBackgroundWidget | 1,774 |
| Health | 1,343 |
| KillLogMessage | 2,329 |
| LocalKillLogMessage | 2,536 |
| HudAlertMessage | 7,928 |
| TicketCounter | 7,685 |
| KillCam | 6,250 |
| KitBar | 8,656 |
| ScoreBoardRow | 5,051 |
| ScoreboardHeader | 5,701 |
| TopPlayersRow | 3,658 |
| TicketCounterSpawnScreen | 4,835 |
| SpawnTimer | 3,779 |
| Crosshair | 2,305 |
| MatchPreroundInformation | 1,383 |
| List | 24,421 |
| ListLogic | 12,144 |
| **total, 23 classes** | **121,614** |

What the AS2 does, counted over the 237 library classes:

* `gotoAndStop` in 108 classes and `gotoAndPlay` in 45. Widget states are timeline labels, for example
  enemy / friendly / squad / neutral, active / inactive, onExit.
* `attachMovie` in 47 classes, for list rows, kill-feed rows and the compass strip.
* `textWidth` in 45 classes, for laying out texts in a row, such as killer, weapon and victim.
* `_alpha` in 26 classes.
* `_xrotation` in 17 classes.
* `Tweener` in 18 classes.
* `createEmptyMovieClip` in 19 classes.
* `setTextFormat` in 12 classes, `onEnterFrame` in 11, `setInterval` in 9.
* The drawing API (`beginFill`, `lineTo`) in only 6 classes.
* No `ExternalInterface` and no `fscommand`. The engine pushes data by calling the widget methods
  `setupRefreshData` / `update<Name>Data` directly. Ammo alone has 20 of them, for example `updateClipAmmoData`,
  `updateTotalAmmoData`, `updateFiringModeData` and `updateGrenadeAmmoData`.

**This is the boundary for a C++ replacement.** Each widget has a few named data inputs and a few timeline states.
Most of its code is "set a text, go to a label, place rows by `textWidth`".

## 3. Renders and comparison

**Method.** A scratch tool (`swfx.exe`, built against main's `bf3_core.lib`) opens the install through
`InstallReader` and `SwfLibrary::open`. It writes the decompressed movie bodies and the atlas texels (`MipSlice`) to
scratch. A scratch Python renderer then reads those files and draws each state:

* It parses shapes, edit texts, DefineFont3 glyphs, PlaceObject2/3 (matrix, cxform, filters, blend, clip depth) and
  the GFx sub-images. It follows imports across movies, for example `hudtextures`, `textures`, `iconsshared`,
  `compass` and `hudmessage`.
* For each sprite it plays the display list to its **rest frame**: the first frame with a `stop`. For most widgets
  this is the frame where the intro animation ends.
* It applies the label each widget's AS2 would pick for the state, for example `enemy` on the enemy ticket row,
  `outPlayer` on the own scoreboard row and `active` on the fire mode.
* It rasterises with 4 x 4 supersampling, nonzero fill, and bilinear atlas sampling through the inverse homography.
* It places rows the way the AS2 would (`attachMovie` plus `textWidth`), with example texts: bot names, 23 / 19
  tickets, 30 / 120 ammo.
* It hides design-time placeholders such as `posGuide`, `listSize`, `tmpSize` and the `ID_*` sample texts. It colours
  engine-drawn parts flat (§2.4).

Files in `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\pt1-hud\` (git-ignored):

* `bf3_<state>_2560x1440.png` and `bf3_<state>_1920x1080.png` for each of: inround, inround_flat, scoreboard,
  killcard, spawn, preround, roundend, roundend_top. Every state except inround_flat also has a
  `bf3_<state>_1280x720.png`, used for the comparisons.
* `cmp_<state>.png`: ours on the left (the M2b captures in `b\shots\integm2bw2\play2`, 1152 x 648 scaled to
  1280 x 720) and the BF3 render on the right. The pairs are:

  | cmp file | ours | BF3 render |
  |---|---|---|
  | cmp_preround | join | preround |
  | cmp_inround | join | inround |
  | cmp_spawn | spawn | spawn |
  | cmp_killcard | killcard | killcard |
  | cmp_scoreboard | scoreboard | scoreboard |
  | cmp_roundend | round_end | roundend |
  | cmp_roundend_top | round_end | roundend_top |

  The M2b captures have no plain in-round frame. `join` shows the in-round HUD under the pre-round box and the spawn
  kits.

**What the renders are not:**

* There are no animations. Each widget is drawn at its rest frame.
* There is no live AS2. Example texts and labels are set by hand per state.
* Engine-drawn parts are flat colour (§2.4).
* The renders have no game world behind the HUD; they use a flat grey background.
* The tilt projection centre is [inferred].
* Two known limits:
  * the voip icons of the squad list sit at the label field's right edge, because the AS2 moves them by
    `textWidth`;
  * the spawn screen's list, tab bar and kit item icons are empty, because they are filled by the AS2 and the engine.

## 4. Differences

"Stage" means BF3's 1280 x 720 stage, which both HUDs use: ours maps it by `hud_stage_to_window`. "Ours" is M2b
main 6a4061e.

| element | ours | BF3 | source |
|---|---|---|---|
| minimap | circular map, north-up, bottom left (`Bf3MinimapControl`) | square 170 x 170 render target at stage (-3,527), tilted x -15 / y -15 / fov 25, on the HudBackground panel, with the icons in the same tilted space | hudmpscreen `Minimap_01`, `Widget.Hud.Minimap` (`t2;iUIMinimapRender`) |
| compass | none | heading strip under the minimap at (85,696): `compass:mc_singlePlayerCompass`, a 361-frame `cps` strip (one frame per degree) behind a mask, `compass_i3.tga` 1024 x 64, tilted like the minimap | hudmpscreen `Compass_01`, `Widget.Hud.Compass` |
| ticket / kill counter | top centre, "RU 0 / 15 0 US", Number font, inferred colours | two rows above the minimap (TicketCounter at (-4,530)): enemy row y -46, friendly y -27. Each row has a team name ($Menu_medium 16), a count ($HUD_medium 16, #aeeaf8) and a `hudprogress` meter; drop shadow (0,100,200) blur 5 / distance 1 / strength 1.7. Rows 2-3 are for other modes | hudtdmscreen `TicketCounter`, `Widget.TicketCounter` |
| HUD background panel | none | `HudBackgroundWidget_01` at (-3,486.95), scale 1.288 x 3.689, engine-drawn `renderHolder` (`t2;iBgDraw`) behind the minimap, compass and tickets; fades in over 20 frames | hudmpscreen, `Widget.Hud.HudBackground*` |
| squad list | none | `SquadList` at (179,619): per member a kit icon, the name ($Menu_medium 16, #e4ff9d, green glow), a voip icon and a leader star; rows 21 px | hudmpscreen `SquadList_01`, `Widget.Hud.SquadList` |
| ammo | magazine 43 px / reserve 21 px at the Ammo_01 slots, flat, no panel (M2a `Bf3HudAmmo`) | the same slots, but inside a tilted (-15/+15) panel, with: "/" 18 px; `Gx` plus the grenade count; fire-mode indicators `fmd` single / burst / auto with active / inactive frames; a `weaponProgress` bar; a low-ammo state | hudmpscreen `Ammo_01`, `Widget.Hud.Ammo` (20 `update…Data`) |
| health | "100" + "%" at the Health_01 slots, flat | "100%" 21 px in a tilted (-15/+15) panel at (1193,605), with the engine EKG trace behind it (`t2;iUISoldierCompRender`) | hudmpscreen `Health_01`, `Widget.Hud.Health` |
| weapon name / icon | localised name and `_small` icon left of the digits, placement [inferred] (M2a) | not in Ammo's rest frame; BF3 shows the weapon through the ammo widget's own frames | m2a-hud.md; `Widget.Hud.Ammo` |
| crosshair | procedural: four ticks 9 x 2 at a dispersion gap | `crosshairdefault` imports `crosshair.swf` art: four bitmap ticks 6 x 13 at ±11 stage px, plus dot, circle and grenade variants (research §3.8 called it procedural; it is not) | crosshairdefault, `Widget.Crosshair` |
| kill feed | top right, names 14 px, own colours | `HudMessage_02` at (1280,0), right-aligned "Kill" rows every 26 px. Killer, msg and victim in $Menu_bold 20 (#ffeab9), tinted by the frames enemy / friendly / squad / neutral. Enemy: glow (204,51,0) and drop shadow (255,96,0) blur 8. Headshot icon. Texts laid out by `textWidth` with a gap | hudmpscreen `HudMessage_02`, `hudmessage:Kill`, `Widget.Message.KillLogMessage` |
| alerts / messages | none (except the pre-round box) | `HudAlertMessage` at (640,156) with bracket art and information / alert frames ("RETURN TO THE COMBAT AREA" in #ff7d7d with glow); message slots `HudMessage_01/04/05`; `LogMessage_01` at (0,534) | hudmpscreen, `Widget.Message.HudAlertMessage` (7,928 B) |
| score popup | none | `ScoreAggregator_01`: "+100" in $Menu_bold 24 with a blue (0,100,200) glow over the glow piece; frames hide / start / out | hudmpscreen, killscreen |
| pre-round | top-centre box "ROUND WILL START IN" / big number, orange border | `preroundwaitingscreen`: `MatchPreroundInformation_01` at (485,0) with "WARMUP PHASE", "Player Status", "x/y players ready" and "WARMUP TIME"; `HudAlertMessage_03` at (640,45) scale 0.8 for the countdown | preroundwaitingscreen, matchpreroundinformation |
| scoreboard layout | one panel (120,70) 1040 x 580, header 64, rows 14-26 tall, two columns drawn by hand | two lists at x 103 and 641, y 154, each 536 wide with 16 rows of 28 px on black α 0.25; `ScoreboardHeader` at y 124 | scoreboardtwoteamshud32screen |
| scoreboard columns | name, kills, deaths, score at inferred positions; header 12 px, rows ≤ 14 px | header in $Menu_bold 18 (team / WINNER with glow / tickets / K / D / SCORE / PING at x 9 / 262 / 166 / 352 / 386 / 420 / 492); rows in $Menu_medium 16 (number 5, name 84, K 352, D 386, score 420, ping 492, rank icon 49, voip 70) | `ScoreboardHeader`, `ScoreBoardRow` |
| own row on the scoreboard | yellow text | row background frame `outPlayer` (orange bar); squad mates `outSquad`; others `out` (40,55,65) | `ScoreBoardRow` bg frames |
| kill card | bottom-centre card (430,470) 420 wide: "KILLED BY" 12 px, killer 26 px, kit, weapon 15 px, health 28 px | `KillCam`, a full-width top band (x 113-1168, y 0-~225): orange header bar (248,134,19) "KILLED BY" plus the name; weapon image (image manager); weapon name $Menu_bold 20 with service stars; three customisation cells and the kit cell at y 151; HEALTH plus the value; "ME x - y FOE"; dog-tag pair 457 x 157; panels (17,38,51) α 0.8; nemesis and suicide variants | killscreen, `Widget.KillCam` (29 `update…Data`) |
| spawn screen | kit cards at the bottom left over the dimmed world, DEPLOY button at the bottom right, circular minimap | a full menu: `TabBar` (96,54); `List_01` (96,133) 670 x 390 for squads / spawn points; `OverviewMap_01` (769,132) engine-drawn; `TicketCounterSpawnScreen` (769,97) with US / RU in $Menu_medium 21 on gradient pieces (115,195,255) / (150,50,50); `KitBar` (96,557) with orange "DEPLOY AS" + kit name, five kit-item slots in grid brackets, "CHANGE ITEM", a progress bar and buttons | spawnscreenpc, `Widget.KitBar`, `List`, `TabBar` |
| round end | centre panel (340,150) 600 wide: won / lost 36 px, team kills 52 px, top three 18 px | `eorwinningteamscreen`: "YOUR TEAM WON/LOST!" in $Menu_bold 60, centred, glow (115,195,255), "ROUND DURATION -" 30 px. `eortopplayersscreen`: a header bar (419,159) in (48,110,149) and `TopPlayersRow` rows from (420,189) every 28 px (position, name, score). In the data these are separate screens, shown one after the other [inferred: they overlap when drawn together] | eorwinningteamscreen, eortopplayersscreen |
| fonts | three roles (Number / Text / Title), mapped to $HUD_medium / $Menu_medium / $Menu_bold, sizes picked by hand | per text field: $Menu_medium 16 / 18 / 21, $Menu_bold 18 / 20 / 24 / 60, $HUD_medium 16 / 21 / 43, align and colour per field | DefineEditText of each movie |
| text effects | one black shadow offset under each text | glow or drop-shadow filters on most HUD texts (35 filters in the in-round state): blue (0,100,200) on HUD numbers, green on squad names, orange-red on enemy kill-feed names | PlaceObject3 filters |
| colours | constants marked [inferred] (`kTdmFriendlyColour` 0.42/0.78/1.0, enemy 1.0/0.47/0.28, local 1.0/0.86/0.35, panel 0.02/0.05/0.07 α 0.74) | from the data: counts #aeeaf8; blue (115,195,255); orange (248,134,19); squad #e4ff9d; feed #ffeab9; panels (17,38,51) α 0.8 and (40,55,65); enemy tint by frames | edit-text colours, cxforms |
| tilt | none | four widgets tilted (§2.5) | AS2 `_xrotation` / `_yrotation` / `_perspfov` |
| animation | kill-feed fade only | timeline intros and outros (onExit labels on every widget), HudBackground's 20-frame fade, ScoreAggregator hide / start / out, Tweener in 18 classes | sprite timelines, AS2 |
| debug overlay | the M1 status lines at the top left are in every capture | none | (ours only) |
| hit / damage indicator | M2a hit-indicator art at Hitindicator_01 | Hitindicator_01 / DamageIndicator_01 (hidden in the renders, not compared) | hudmpscreen |

## 5. Options

The costs are in agent waves of the size of the M2b waves: one wave is a few tracks in parallel, then an
integration.

### (a) Route A+ corrected

**What.** Keep the hand-written Godot screens (`Bf3TdmHud`, `Bf3Hud`) but drive every element from the data:

* extend `swf_layout` to read, for each named element:
  * its edit-text font, size, colour, alignment and filters;
  * its atlas pieces (sub-image rectangles plus the matrix and cxform);
  * its label frames;
* draw the pieces as `AtlasTexture` quads;
* add glow and shadow through a shader;
* do the tilt by drawing each tilted widget into a `SubViewport` and putting that on a perspective quad;
* re-lay out the six screens and add the missing widgets: compass, squad list, background panel, fire mode,
  grenades, alerts, score popup, spawn menu parts.

**Fidelity.** Close for static layout wherever someone copied the data correctly. Every element is a hand
transcription, so drift is likely in the details: about 120 elements on the in-round screen alone. Animations are
approximations.

**Cost.** 2-3 waves: in round; scoreboard, kill card, pre-round and round end; the spawn menu.

**Risks.**

* The playtest complaint returns in smaller form (each element "almost" right).
* Every new screen (other modes, vehicles, menus) is new hand work.
* There are two sources of truth: the data and our code.

**What stays C++.** Everything. There is no new runtime machinery.

### (b) C++ SWF display-list renderer, AS2 replaced by C++ per widget (recommended)

**What.**

* **Core (Godot-free):**
  * `SwfShape` keeps its geometry: edge records into fill0 / fill1 polygons per style, triangulated once, with the
    bitmap fill matrices.
  * PlaceObject3 filters and blend modes are parsed. Today they are kept as flags only.
  * A resolved display tree follows imports and plays timelines to a frame or label, with matrix, cxform and
    clip-depth composition.
  * Text layout matches GFx's: left / centre / right, autoSize, `textWidth`, the HTML subset.
* **Godot:**
  * a `Bf3SwfView` that draws the tree through `RenderingServer` canvas items: triangle meshes with atlas UVs;
    cxform as a shader (multiply plus add);
  * add blend through `CanvasItemMaterial`;
  * masks through `clip_children` or a stencil pass;
  * glow and drop shadow as a blur shader on the text layers;
  * the four tilted widgets through a homography shader or a SubViewport on a perspective quad;
  * engine render targets filled from our sources: the minimap renderer, the panel, the EKG later, the item icons.
* **Widgets in C++:** one controller per AS2 class used, about 23 classes for TDM. Each one takes `HudModel`, sets
  texts, picks labels, attaches rows and lays them out by `textWidth`. It is written by reading that class's
  disassembly as data: its `update…Data` inputs and label names.

**Fidelity.** Layout, typography, colours, filters, art and timeline animations are exact to the data, because they
are the data. Behaviour is as good as each C++ controller. The scratch renders are the visual target. The tilt
centre and the timing of Tweener-driven moves are [inferred] until compared with retail footage.

**Cost.** 3-4 waves:

1. renderer plus in-round HUD (§7);
2. scoreboard, kill card, pre-round, round end, alerts and the score popup;
3. the spawn screen as a working menu (list, tab bar, kit bar, mouse);
4. if needed: polish and Tweener-style animations.

**Risks.**

* Godot 2D has no perspective. This needs a shader, which is a contained risk.
* GFx text metrics (leading, autoSize, glyph advance rounding) can shift text by a pixel or two.
* Reading 122 KB of AS2 to write the controllers takes judgement. The behaviour must be checked against the
  renders and footage.
* Masks with `clip_children` must nest correctly.
* Performance looks fine: 234 sprites and 678 glyphs in the worst state.

**What stays C++.** Everything. The SWF data becomes the layout and art source, and the behaviour stays in C++.

### (c) (b) plus an AVM1 interpreter

**What.** Same renderer, but run the retail AS2 instead of rewriting it. This needs:

* an AVM1 VM: about 100 opcodes, `DefineFunction2` registers, the prototype chain, `registerClass`;
* the AS2 object model: MovieClip, TextField, TextFormat, Array, Math, `setInterval` / `onEnterFrame`;
* the GFx extensions: `_xrotation`, `_perspfov`, `rendererString`, `gfxExtensions`;
* Tweener, which runs as bytecode;
* an engine binding that calls `setupRefreshData` / `update…Data` with values in the shapes the classes expect.

**Fidelity.** The highest, because the widgets behave as retail by construction, including the animations.

**Cost.** (b)'s renderer (1 wave), plus 5-8 waves for the VM, the object model, the GFx extensions and the binding.
Research D16 estimated 2-3 months plus 4-9 months for this route.

**Risks.**

* It is the largest by far: 643 KB of library code exercised.
* The engine-side data contract is undocumented. We would still read every class to learn what to pass, which is
  most of (b)'s per-widget work.
* Debugging a VM.
* Clean room: no GPL / LGPL players can be used.

**What stays C++.** The VM, the renderer and the binding. Widget behaviour moves into retail bytecode run at runtime.

### (d) Other routes found

* **(d1) Bake.** Port the scratch rasteriser to C++. Rasterise each widget's static frames into textures in
  `user://` at load, at the window size. The C++ widgets compose them and draw only the live texts.
  * Cheaper than (b) for wave 1: no Godot mesh, mask or filter work.
  * But it is tied to the resolution, needs a re-bake on resize, and gives poor animation and per-frame tint.
  * It is a fallback if (b)'s Godot path proves hard. It is not a separate destination.
* **(d2) Embed an existing permissive Flash player** (Ruffle is MIT / Apache-2.0). It is not C++, it adds a Rust
  toolchain, and it has no support for GFx tags 1008 / 1009, `_xrotation`, `_perspfov` or `rendererString`. We would
  patch a large foreign codebase. Not recommended.

| option | fidelity | waves | main risk |
|---|---|---|---|
| (a) A+ corrected | static layout close if transcribed well; animations approximate | 2-3 | hand-transcription drift; repeated per screen |
| (b) display-list renderer + C++ widgets | exact art, layout and timelines; behaviour per controller | 3-4 | perspective / mask / filter shaders; GFx text metrics |
| (c) (b) + AVM1 | retail behaviour | 6-9 | VM size; undocumented engine binding |
| (d1) bake | exact static art at one size | 2-3 | resize, animation, tint |

## 6. Recommendation: (b)

Reasons:

1. The data is simple enough to render generally:
   * no gradients;
   * 9 lines;
   * almost all fills are atlas pieces;
   * two filter kinds;
   * one blend mode;
   * a few masks.
   About 1,100 lines of scratch Python drew every state. A C++ core of similar scope fits next to the existing
   `swf_movie` / `swf_atlas` / `swf_font` / `swf_layout`, which are about 3,000 lines.
2. Under (a), the playtest's complaint is fixed only as well as someone copies about 120 elements per screen, and
   then again for every new screen. Under (b), the data is the layout.
3. The AS2 behaviour of these screens is small: 23 classes, 122 KB, 164 data entry points. It is mostly "set text,
   go to label, place rows". Rewriting it in C++ costs less than an AVM1 VM and keeps the code ours.
4. (b) does not close (c). An AVM1 interpreter could be added later on the same renderer if menus need it.

## 7. First wave of (b): renderer core plus the in-round HUD

Scope (tracks in parallel after a small contract):

1. **core: swf geometry.**
   * `SwfShape` keeps polygons per fill style: edges to closed contours, fill0 / fill1 orientation, curves flattened
     at a fixed tolerance, then triangulated.
   * Bitmap fills keep their matrices. PlaceObject3 filters (DropShadow and Glow values) and blend modes are parsed.
   * Unit tests on install data:
     * the census counts (solid 315, bitmap 1,088, gradients 0, 9 line styles);
     * one hand-checked shape, for example `HeaderGradient`'s 200 x 16 piece.
2. **core: resolved scene.**
   * Build the display tree of a movie across imports at a frame or label, with composed matrices, cxforms and clip
     depths, and per-node instance paths.
   * Edit texts get font, size, colour, alignment and bounds, plus GFx-style `textWidth`.
   * Test: the in-round tree's node count and the positions of §4's elements, within 0.05 px.
3. **godot: `Bf3SwfView`.**
   * Draws a resolved scene: meshes with atlas UVs, a cxform shader, add blend, `clip_children` masks, a glow /
     shadow shader for texts, and the homography tilt.
   * Engine targets: the existing minimap drawn into Minimap_01's square target, and the `iBgDraw` panel as a dark
     rectangle.
   * Check: a windowed capture at 1280 x 720 and 2560 x 1440 (with the user-active rule).
4. **godot: in-round widgets.**
   * C++ controllers for TicketCounter, Ammo (clip, reserve, grenades, fire mode, low ammo), Health, Compass (the
     `cps` frame from yaw), Minimap, SquadList, the kill feed (attached Kill rows, `textWidth` layout, team frames,
     headshot) and the default crosshair (bitmap ticks at the dispersion gap).
   * They are fed from the existing `HudModel`. The M2a `Bf3Hud` widgets and the M2b `Bf3TdmHud` in-round parts
     they replace are switched off. The scoreboard, kill card, spawn, pre-round and round-end screens stay M2b until
     wave 2.

Out of scope for wave 1: menus, Tweener animations (rest frames and timeline intros only), alerts and the score
popup, other modes' rows, and the EKG trace.

## 8. Open questions

1. **Route.** (a), (b) or (c). The recommendation is (b). The user decides.
2. **Tilt projection centre.** It is [inferred]: the stage centre with focal length = half width / tan(25 / 2).
   Scaleform could project around the widget or the movie origin instead. Retail footage would settle it.
3. **Kill-feed `msg` field.** Retail fills it with the weapon name or an icon (`KillLogMessage` has `setupIcon` /
   `tintIcon`). The render shows the name in brackets.
4. **Engine-drawn parts.** The exact look of `iBgDraw` (colour and alpha of the HUD panel), the minimap tint and the
   EKG trace are drawn by the engine and are not in the movies.
5. **Round-end sequence.** `eorwinningteamscreen` and `eortopplayersscreen` overlap when drawn together. The order
   and timing in which retail shows them is [inferred].
6. **Spawn screen scope.** BF3's spawn screen is a full menu (squad list, tab bar, kit customisation). Is a working
   menu wanted in wave 3, or a static look with our current kit choice?
7. **Debug overlay.** The M1 status lines should leave the default play view, behind a key, whichever route is
   chosen.

## 9. Checks (one command each)

| check | command | result |
|---|---|---|
| census totals (§2.1) | `b\dev\tools\bf3dump\bf3dump.exe swf census` (main build) | 434 movies, 0 errors; the totals of §2.1 |
| HUD placements (§4 positions) | `b\dev\tools\bf3dump\bf3dump.exe swf hud` | the ui-swf-fonts.md §8.2 table |
| fill styles, AS2 sizes, effects (§2.2-2.6) | scratch only: `python <scratch>\pt1-hud\tool\states.py` and the `swfp` / `as2` helpers (not committed; they read the files `swfx.exe` wrote from the install) | the numbers in §2 |
| renders (§3) | scratch only: `python <scratch>\pt1-hud\tool\states.py`, then `python <scratch>\pt1-hud\tool\cmp.py` | 23 `bf3_*.png` + 7 `cmp_*.png` in `b\shots\pt1-hud\` |
