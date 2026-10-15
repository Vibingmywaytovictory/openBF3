# UI groundwork 1 design: SWF symbol tables, atlases and fonts

Status: implemented 2026-10-16 (§21: the measured gates, one command each, and what the implementation changed in
this design); design revision 2, 2026-10-15 (after review; §20 lists what changed). Track ui1, branch
`ui/swf-fonts`. Scope: the first UI groundwork wave of route A+ (plan
[research/PORTING-RESEARCH.md](../../research/PORTING-RESEARCH.md) §3.8, D16; §2.2 "UI readers: SWF symbol table,
`DefineFont3`"; §7.8): BF3's own SwfMovie data read at run time as symbol tables, its external atlases cut into named
regions, its `DefineFont3` fonts converted to TrueType that Godot loads, the font mapping, and the HUD positions from
the movies' timelines. No SWF renderer and no AS2 interpreter. The HUD screens themselves (ammo, crosshair, hit
indicator, kill feed, scoreboard, spawn screen) come in later M2a/M2b waves on top of this; the sibling track ui2
(minimap, `hud3dicons`, HUD data model) runs beside it.

The interface headers named in §12 are in the tree and compile (ctest `ui1.contract`); their bodies are the
implementation's work.

Tags: **[verified]** = measured on the retail install or read from code in this tree on the date above (the probe
numbers come from a scratch Python probe written for this design, `swfprobe.py`, over every SwfMovie payload read
from the install; nothing of it is in the repo). **[plan]** = taken from the plan or a research report, with its
section. **[inferred]** = a design choice or estimate made here.

---

## 1. Goal and exit gates

Goal: a later HUD wave can ask "which font does `$HUD_medium` mean, as a Godot `FontFile`", "which atlas rectangle
is `mc_iconsIngameBig/FriendlyPlayer`, as an `AtlasTexture`" and "where does BF3 put the ammo widget on its
1280 × 720 stage, and in which font and size are its digits" and get the answer from the user's install at run
time.

Every gate is measured by one command. "Install tests" exit 77 (ctest *Skipped*) without an install; the Godot tests
also need `OPENBF3_GODOT`. Timings on this machine are noisy (other tracks build and test in parallel), so timing
gates have wide margins and every run prints its measured value.

| # | gate (task text) | how it is measured | pass threshold |
|---|---|---|---|
| G1 | all 434 SwfMovie res parse with 0 errors; the census is complete | `ctest -R "^core\.ui\.swf$"` (install): `SwfLibrary::open` + `run_swf_census` over every SwfMovie of the install; also `bf3dump swf census` (exit 1 on any error) | 434 movies, 434 parsed, 0 errors; `distinct_sha1s == 1` for every name (`multi_sha1_names` 0); 0 labels after their timeline's last ShowFrame; the tag totals equal the §4.2 table exactly (they are the probe's numbers; a difference is either a reader bug or a probe bug and must be explained in README before the gate is changed). Identical bytes per name are verified so far only across the 11 superbundles that cover all names (§4.1); the census is the first check over all 10,303 entries. If it finds a name with two SHA-1s, G1 **fails** and lists the names: there is no automatic "first holder" fallback, and only the user (through the orchestrator) can accept one, recorded in README with the names |
| G2 | iconsingame 162 rectangles and 427 labels, the complete label → rectangle map | `core.ui.swf`: `build_atlas_map` of `ui/assets/iconsingame`; `bf3dump swf labels ui/assets/iconsingame` prints the map | 1 external image `iconsingame_i1.tga` 4096 × 128 whose DxTexture exists with that size (DXT5); 162 sub-images, 0 outside the atlas; 427 labels on 6 sprites, of which 135 draw art (110 one piece, 25 two) and 292 are empty frames; 95 sub-images reached from labels, 96 with the exported symbols, 162 from frames; spot checks of §6.3 |
| G3 | Purista (443) and Venice (33) convert to TrueType | `ctest -R "^core\.ui\.font$"` (install): `UiFontLibrary::load` for `localization/us_loc` on a fresh scratch cache root, then again (warm) | 4 fonts; TrueType `numGlyphs` 444 / 444 / 34 / 34 (source + `.notdef`); cmap 442 / 442 / 32 / 32 codes; U+00C9 mapped in both Purista; `contours_reversed` 0; `read_truetype` round trip: every point within 0.5 font unit of the scaled source, every advance exact after scaling, kerning pairs 335 / 324 / 0 / 0; cold → 4 `converted`, warm → 4 `hit`; a damaged entry → `dropped_corrupt`, reconverted, then `hit` |
| G4 | the fonts load in Godot (`FontFile.set_data` succeeds, glyph count matches) | `ctest -R "^godot\.ui_fonts$"` (install + Godot, headless): `res://scenes/ui_fonts.tscn -- --ui-check` | rows `fonts.set_data`, `fonts.glyphs`, `fonts.contours`, `fonts.metrics` (every glyph's advance within 0.05 px and each sample string's shaped width (the sum of its shaped glyph advances, §11.1) within 0.5 px of the source layout at 64 px, measured on the §10 FontFile settings), `fonts.kerning` (AV narrower by 3.84 ± 0.25 px), `fonts.metro` pass for all 4 fonts (§11.1); last line `UI1 CHECK: PASS (n/n)`, exit 0 |
| G5 | sample strings rendered windowed, screenshots inspected (no inverted contours, right baseline and advances) | `ctest -R "^godot\.ui_fonts_shots$"` (install + Godot, **windowed**; 77 without a window): the same check plus `--shot-dir=<dir>`, three PNGs; `<dir>` = `$ENV{OPENBF3_UI1_SHOTS}`, default `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\ui1\` (absolute, gitignored, outside any worktree; ui2's `OPENBF3_UI2_SHOTS` convention) | the check passes and the three PNGs exist; the implementer opens them and records in README what they show (holes of O/A/0/8 open, glyphs on one baseline, no overlap or gaps at 16 / 32 / 64 px, "MÉTRO" with its accent). The visual judgement is manual; the automated parts of the same claim are G3's round trip and G4's `fonts.contours` / `fonts.metrics` rows |
| G6 | the font mapping resolves every `$MENU_*` / `$HUD_*` name | `core.ui.swf`: every font name the 434 movies use (gfxfontlib imports, glyphless `DefineFont3` names, edit-text font classes) through `resolve_font_name` on the en collection | 0 `Unresolved`; the per-rule counts of §7.4 printed and equal: Exact 5 (revision 2 said 4: the en font library's own edit texts name all four long names, Venice-SemiBold included; §21), CaseFolded 2, Family 4, Device 5, counted as **distinct names after dropping one trailing `*`** (`$FONT_TITLE` and `$FONT_TITLE*` are one name, `$Menu_light*` counts as `$Menu_light`); the Family-resolved names are printed on a line of their own (`G6 family-inferred: $FONT_TITLE $HUD $HUD_light $Menu_light`), so "every name resolves" is not read as "every name has a mapping entry" |
| G7 | the HUD positions table for the in-game HUD movies | `core.ui.swf`: `read_hud_placements` for the 14 screens of §8; `bf3dump swf hud` prints the table | 86 placements; every screen's stage 1280 × 720; the spot values of §8.2 within 0.05 px |
| G8 | SWF and font parsers under asan with truncation / fuzz tests | `ctest --preset asan -R "^core\.ui\.(swf\|font)"` | `core.ui.swf.unit`, `core.ui.font.unit` (synthetic: every truncation, 2,000 mutants each, the hostile cases of §9) and the install fuzz `core.ui.swf.fuzz` pass with no ASan report; in `dev` each hostile case ends in under 1 s (bounded by the budgets of §9.1). The asan run also includes the install tests `core.ui.swf` / `core.ui.font`; under ASan (`__SANITIZE_ADDRESS__`) their T1 / T2 rows print `reported (asan)` and are not checked, so an ASan slowdown cannot fail G8 on timing |
| G9 | install unchanged | an `install.unchanged` row in `core.ui.swf`, `core.ui.font` and `godot.ui_fonts` (`take_install_snapshot` before / after, `diff_snapshots`) | sizes and mtimes of the 256 index files equal |
| T1 | census time (reported, gated loosely) | `core.ui.swf` prints the library open time and the census wall time | open + census ≤ 15 s on 16 threads (the probe takes 34 s in Python; the C++ estimate is under 3 s); checked in `dev`, reported only under asan; the machine is shared and noisy |
| T2 | font load time | `core.ui.font` prints cold and warm `UiFontLibrary::load` times | cold ≤ 1.0 s, warm ≤ 0.25 s (estimate: tens of ms); checked in `dev`, reported only under asan; the machine is shared and noisy |

All existing tests must keep passing: the acceptance run is the full `ctest --preset dev` with `OPENBF3_GODOT` set.

---

## 2. Module ownership

| library | ui1 adds | Godot? |
|---|---|---|
| `bf3_core` (`include/bf3/core/ui/`) | `swf_movie.h` (payload and tag reader, display lists), `swf_library.h` (every SwfMovie of the install, first holder, atlas textures), `swf_census.h`, `swf_atlas.h` (the atlas cutter), `swf_font.h` (DefineFont2/3 outlines, winding), `font_ttf.h` (TrueType writer and reader), `ui_fonts.h` (UIFontMappingCollection, name resolution, the cached font library), `swf_layout.h` (layout trees, HUD placement table) | no |
| `bf3_godot` | `ui_assets.{h,cpp}` (`Bf3UiAssets`: FontFiles, atlas textures and `AtlasTexture` regions, the seams of §12.3), `ui_font_check.{h,cpp}` (`Bf3UiFontCheck`), scene `res://scenes/ui_fonts.tscn` | **yes** |
| `bf3dump` | `swf_commands.{h,cpp}`: `swf census`, `swf movie`, `swf labels`, `swf layout`, `swf hud`, `swf fonts` (§12.4; no command writes a converted font anywhere, §13) | no |

Plan §2.2 puts the UI readers (SWF symbol table, `DefineFont3`) in `bf3_core` and the font conversion glue and
Controls in `bf3_godot`. The TrueType writer is Godot-free and lives in `bf3_core` too, so the conversion and the
cache run on a loader thread and are tested without Godot; `bf3_godot` only hands the bytes to `FontFile.set_data`
[inferred, consistent with §2.3's loader-thread rule].

No new link edge: `bf3_core` already links libdeflate (zlib inflate for CFX bodies, `libdeflate_zlib_decompress`
with the declared size as the output size) and BCrypt; `bf3_godot` already links `bf3_core` and `bf3_render`
(`prepare_texture` for the atlases).

Shared with ui2 [inferred]: both tracks need "an uploaded atlas texture cut into `AtlasTexture` regions". ui1's
`make_atlas_region` takes a rectangle; ui2's hud3dicons UV states can call it with their own rectangles. If ui2
lands its own helper, the merge keeps one (a one-function change).

---

## 3. Threads

| thread | ui1 work | rules |
|---|---|---|
| Godot main | `Bf3UiAssets` builds `FontFile`s (`set_data`), `ImageTexture`s (`render_bridge::make_texture`), `AtlasTexture`s; answers the getters; `Bf3UiFontCheck` draws and checks | Godot API only here |
| `Bf3UiAssets` loader (`std::thread`, one) | `SwfLibrary::open` (manifest reads on `parallel_for` lanes), `acquire_cache`, `UiFontLibrary::load` (parse the font library, convert or hit the cache), `build_atlas_map` and `prepare_texture` of the requested atlas movies, `read_hud_placements` | no Godot API; joined in `_exit_tree` / `NOTIFICATION_PREDELETE`; uses the `Install` that `Bf3Localisation` opened (read-only, shared) and its `reader().write_guard()` as the cache's `PathGuard` |
| `parallel_for` lanes | `SwfLibrary::open` (one manifest per lane), `run_swf_census` (one movie per lane) | read-only on the mount; each lane its own `WorkBudget` per movie |

The work is small (the font library is 115,603 bytes; four 2048-wide atlases are ~0.6 MB of DXT5), so one loader
thread is enough; the census runs in tools and tests only.

**Cache sharing.** `acquire_cache(default_cache_root(), guard)` (`godot_cache.h`) is called on the loader thread
with the guard above. In one process `Bf3UiAssets` and `LevelSession` share one `Cache` on `user://bf3cache`.
Another process (a parallel worktree's Godot test uses the same `%APPDATA%` root) holding the root gives `Busy`:
then the fonts are converted without a cache (logged once). G4 / G5 therefore never depend on a cache hit; only G3
(`core.ui.font`, its own scratch root) checks hits.

---

## 4. Data sources in the install

All [verified] on 2026-10-15 with `bf3dump list` / `types` / `dump` (this tree's `bf3dump`), the research ebx-dump
probe's `rawres` (payload bytes into scratch) and `swfprobe.py`.

### 4.1 SwfMovie res

* **Type** `SwfMovie`, res type `0x2D47A5FF` (`res_type::kSwfMovie`, already in `res_types.h`). The 16-byte
  resMeta is zero on every entry read (1,621 entries).
* **Where**: 434 distinct lowercase names, 10,303 manifest entries in 50 superbundles. 401 names under
  `ui/assets/`, 33 under `ui/static/` (the font libraries). Win32/UI holds 58 (the 33 font libraries in their own
  `ui/static/...fontlib_bundle`s, and shared widgets in `ui/flow/bundle/{ingamebundlemp, ingamebundlesp,
  mainmenubundle, eorbundle, preeorbundle, loadingbundlemp, loadingbundlesp, staticbundle}`); `Levels/FrontEnd`
  232; each of the 29 MP levels 194–227 distinct names (bundles `<level>_uiplaying` 174–205, `_uiendofround`,
  `_uipreendofround`, `_uiloadingmp`); SP, COOP and `Web_Loading` the rest.
* **Search order** (first holder wins): Win32/UI, `Levels/FrontEnd/FrontEnd`, the MP/XP level superbundles in
  layout order, then all others in layout order. First holders: FrontEnd 227, MP_001 112, Win32/UI 58, MP_007 13,
  COOP_002 9, MP_012 5, XP5_001 5, and SP_Bank, SP_Finale, SP_Jet, Web_Loading, XP3_Alborz 1 each. No single
  superbundle holds all; 11 superbundles cover all 434.
* **Same bytes per name**: in those 11, every name's payload is byte-identical across holders. Over all 10,303
  entries this is **not yet verified** (`bf3dump list` prints no SHA-1s); the census counts the distinct manifest
  SHA-1s per name over every entry (`distinct_sha1s`) and G1 fails on any name with two.
* **Frames**: 718 sprites declare FrameCount 1 and contain no tag at all (0 ShowFrames: AS2 class placeholders);
  0 of the 2,441 labels lie after their timeline's last ShowFrame [verified, probe].
* **Format**: 371 `CFX` v8, 62 `GFX` v8, 1 `GFX` v7. The declared length equals header + body for all 434 (CFX
  inflates to exactly it). Bodies 1,342 B … 4,997,806 B (the zh EULA font library), 24.3 MB in all; the largest
  inflate ratio is 4.1. Stage 1280 × 720 on every HUD and menu screen checked.
* **Movie EBX**: each movie has an EBX partition of the same name, type `UIAsset` with only `Name`
  (`UI/Assets/IconsIngame`, `UI/Static/FontCollection_En_FontLib`; checked on these two). The reader does not need
  it.

### 4.2 Tag census (all 434, probe)

Root and sprite timelines together; this is G1's golden table.

| tag | count | tag | count |
|---|---|---|---|
| PlaceObject2 | 27,832 | RemoveObject2 | 629 |
| ShowFrame | 23,917 | GFX_ExporterInfo | 434 |
| End | 3,217 | FileAttributes | 434 |
| DefineSprite | 2,783 | Metadata | 401 |
| DoAction | 2,456 | SetBackgroundColor | 401 |
| FrameLabel | 2,441 | GFX_DefineExternalImage2 | 135 |
| ExportAssets | 1,855 | DefineShape3 | 117 |
| DoInitAction | 1,538 | DefineFont3 | 86 |
| DefineEditText | 1,488 | DefineShape2 | 53 |
| ImportAssets2 | 1,483 | DefineScalingGrid | 29 |
| DefineShape | 982 | DefineText | 12 |
| PlaceObject3 | 980 | DefineShape4 | 7 |
| CSMTextSettings | 916 | ImportAssets | 1 |
| GFX_DefineSubImage | 820 | DoABC, DefineFont2, DefineFontAlignZones, DefineFontName, other GFX tags | 0 |

AS2 bytecode (DoAction + DoInitAction − 2) 1,638,265 bytes; AS3 0. Sprites never nest (maximum nesting 1:
DefineSprite only on the root timeline); at most 8,300 tags in a movie and 526 frames in a sprite.
`DefineFontAlignZones` and `DefineFontName` do not occur, and every `DefineFont3` carries its name in the tag;
the reader parses both by the specification for completeness (synthetic tests only).

The research census (265 movies) is a subset of this one; its totals were smaller accordingly (e.g. 20,621
PlaceObject2).

### 4.3 GFx external images and sub-images

* 135 movies have exactly one `GFX_DefineExternalImage2` (none has two; `GFX_DefineExternalImage` v1: 0). Layout as
  measured (§12.1 header comment): `u16 image id` (0 on 133, 1 on 2), `u16` 9 on all (meaning unknown; the research
  probe read these four bytes as one u32 id 0x90000, which does not match `DefineSubImage.imageId`), `u16 bitmap
  format` 13, `u16 width`, `u16 height`, empty export name, file name `<swf>_iN.tga`, one trailing 0 byte. `N` is not
  always a decimal digit: of the 135, 118 end in `_i1`…`_i9`, 2 in `_i12` and 1 in `_i23`, and 14 in a letter
  `_ia`…`_ie` (e.g. `killcam_ie`, `stepper_ib`, `gamemodeinfo_ia`) [verified, `bf3dump list`]. The texture name is
  derived from the file-name string (`swf_atlas_texture_name`), never by parsing `N` as a number.
* Each `<swf>_i<N>.tga` resolves to the DxTexture `ui/assets/<swf>/<swf>_iN` in the same bundle as the movie
  (135 / 135), with the declared width and height (135 / 135). Formats: 132 DXT5, 3 ARGB8888.
* 820 `GFX_DefineSubImage` (12 bytes each): `u16 id, u16 imageId, u16 x1, y1, x2, y2`; all 820 inside their atlas
  and all with x1 ≤ x2, y1 ≤ y2 (the parser refuses a reversed rectangle, §9);
  imageId matches the movie's external image (811 × 0, 9 × 1).
* x2 / y2 are exclusive: of the shapes filled by one sub-image, 737 have bounds of exactly (x2 − x1) × (y2 − y1)
  pixels; the fill matrix scale is 20 (twips per pixel) on 814 fills. Fill types 0x41 (1,865) and 0x43 (124);
  65535 as a bitmap id means "no bitmap".
* Examples: `iconsingame_i1` 4096 × 128, `hudtextures_i3` 2048 × 128, `compass_i3` 1024 × 64, `ammo_i3` 64 × 32
  (research §1 [plan], confirmed).

### 4.4 Fonts

* 86 `DefineFont3` tags: 63 with outlines (flags 0x8C: HasLayout, WideOffsets, WideCodes; all in the 33 `ui/static`
  font libraries), 21 with 0 glyphs and 2 in a stripped form (flags 0x04, below).
* **Font libraries**: `ui/static/fontcollection_<lang>_fontlib` for 11 languages (cs de en es fr it ja ko pl ru zh)
  and `ui/static/ondemandfontcollection/{eula,store}fontcollection_<lang>_fontlib` (22). The en library
  (115,603 bytes, GFX v8, 1 frame) defines and exports 4 fonts, ids 1–4, under their long names, and 4 sample
  `DefineEditText`s listing every code:

  | font | glyphs | distinct codes | coverage | kerning pairs |
  |---|---|---|---|---|
  | Purista EA Medium | 443 | 442 (U+0020 twice) | all printable Latin-1 incl. U+00C9 É, Latin Extended-A, Cyrillic (Ё…ё, Ґґ), ƒ, Ǻ–ǿ, € ™ № ∆ √ ∞ ≈ ≠ ≤ ≥ █, ‒ – — ‘ ’ “ ” „ † •, max U+2588 | 335 (−3482 … 1229) |
  | Purista EA Semibold | 443 | 442 | the same | 324 |
  | Venice-Medium | 33 | 32 (U+0020 twice) | space ! " # $ % & ' ( ) * + , - . / 0–9 : ; < = > ? (no letters) | 0 |
  | Venice-SemiBold | 33 | 32 | the same | 0 |

  Codes UCS-2, ascending, in all 63 outline fonts. Ascent 22,240, descent 4,320, leading 0 (units of 20,480 per em:
  1.086 / 0.211 em). Cap height 14,337 (0.70 em). Examples: Purista Medium "A" advance 12,658, "W" 17,614, space
  6,145; Venice "0" 12,903 (tabular: "." also 12,904). Kerning: AV −1229, AT −1638, To −1638, LT −1843, Yo −2048.
  Contours: Purista 834 (676 outer, 158 holes), at most 5 per glyph and 63 points; over all 63 outline fonts at most
  24 contours and 281 points (CJK).
* **Winding** [verified]: in SWF's y-down units, every outer contour has positive shoelace area and every hole
  negative (0 exceptions in the 4 en fonts, nesting depth computed by point-in-polygon); every contour is closed.
  That is TrueType's clockwise-outer once y is negated.
* **Glyphless fonts**: 21 tags with NumGlyphs 0 name a font for edit texts (`Arial` 13, `Purista SemiBold` 3,
  `$FONT_TITLE*` 2, `Verdana` 1, `$Menu_light*` 1, `Times New Roman` 1); several carry trailing bytes (up to
  5,477) after CodeTableOffset, which the reader counts and skips. 2 tags (`Calibri`, in `trackingandlockingmanager`
  and `jettrackingandlockingmanager`) have 17 glyphs but no shapes: `OffsetTable[0] = 0`, then `CodeTableOffset`
  (4), then the 17 codes. The research probe crashed on these two; the reader recognises the form (§7.1).
* **Font use** in the 434 movies: imports from `../Static/gfxfontlib.swf` (216 import tags): `$Menu_bold` 102,
  `$Menu_medium` 89, `$HUD_medium` 20, `$FONT_TITLE` 3, `$HUD` 1 (hudcoopscreen), `$HUD_light` 1 (hudctfscreen).

### 4.5 UIFontMappingCollection

`bf3dump types @all-mp UIFontMappingCollection`: 33 partitions in Win32/UI, one per font library:
`ui/static/fontcollection_<lang>_fontmapcollection` and `ui/static/ondemandfontcollection/{eula,store}
fontcollection_<lang>_fontmapcollection`. Fields: `Name`, `TextDatabase` (a reference to the language's
`UITextDatabase`; `localization/us_loc` for en, `localization/jp_loc` for ja, empty in the on-demand ones), `Fonts[]`
of `UIFontMapping {FontLongName, ScaleformFontName[]}`.

| collection | mappings |
|---|---|
| `fontcollection_en_fontmapcollection` | Purista EA Medium → `$MENU_medium`; Purista EA Semibold → `$MENU_bold`; Venice-Medium → `$HUD_medium`; Venice-SemiBold → `$HUD_bold` |
| `fontcollection_ja_fontmapcollection` | Venice-Medium → `$HUD_medium`; Venice-SemiBold → `$HUD_bold`; DFP_sougeitai_W5 → `$MENU_medium`, `$MENU_bold` |
| on-demand eula / store | `Purista EA Medium Eula` → `$Eula`; `Purista EA Medium Store` → `$Store` (per language) |

The whole vocabulary over the 33: `$MENU_medium`, `$MENU_bold`, `$HUD_medium`, `$HUD_bold` (11 each), `$Eula`,
`$Store` (11 each). The movies' spellings differ (`$Menu_bold`) and four names have no entry (`$HUD`, `$HUD_light`,
`$FONT_TITLE`, `$Menu_light`): §7.4.

### 4.6 HUD screens

The in-game MP HUD movies (in `<level>_uiplaying`, e.g. MP_Subway's 174 SWFs): `hudmpscreen`, `hudscreen`,
`hud{conquest,tdm,sdm,rush,ctf,gm}screen`, `hudmatchpreroundscreen`, `hudpreroundscreen`, `killscreen`,
`spawnscreenpc`, `spectatorscreen`, `weaponcommonscreen`, the widgets they import (`ammo`, `health`, `minimap`,
`compass`, `hitindicator`, `hudmessage`, `ticketcounter`, `squadlist`, …) and the crosshair movies
(`crosshairdefault`, …). Every root timeline places one instance (`instance1`) of the exported screen sprite at
identity; that sprite's frame 0 places the widgets as imported symbols (§8). All 1,268 imported symbols (1,268
ImportAssets(2) tags of one symbol each that name a movie; the 216 tags that import from `gfxfontlib` are font
imports, counted separately in §4.4 and not part of the 1,268) resolve to `ui/assets/<lowercase file stem>` and an
export of it, except one (`deckofcards` imports `IconsShared_Check`, which `iconsshared` does not export: a retail
defect the census lists).

### 4.7 Godot 4.8 dev7 API (from `engine-api/4.8.dev7.c971f93e7/extension_api.json`)

`FontFile.set_data(PackedByteArray)`, `set_multichannel_signed_distance_field`, `get_glyph_index(size, char,
variation_selector)`; `Font.get_font_name`, `get_supported_chars`, `has_char`, `get_string_size`, `get_ascent`;
`TextServer.font_get_glyph_contours(font, size, index) -> Dictionary`, `font_get_supported_glyphs(font) ->
PackedInt32Array`, `font_get_kerning`; `AtlasTexture.set_atlas / set_region / set_filter_clip`;
`NinePatchRect.set_patch_margin` (the `DefineScalingGrid` analogue); `Image.create_from_data` with DXT5 / RGBA8.

---

## 5. SwfMovie reader, library and census

### 5.1 Reader (`swf_movie.h`)

* **Header and body.** Signature `CFX` (inflate the rest with libdeflate into exactly `file_length − 8` bytes;
  anything else is an error) or `GFX` (the rest is the body; its length must equal `file_length − 8`). `FWS`/`CWS`
  and any other signature are refused (none in retail). `file_length − 8 ≤ kSwfMaxBodyBytes` (16 MiB, 3.4 × the
  retail maximum of 4,997,806 bytes) is checked before anything is allocated.
* **Tags.** One linear walk: `u16 code << 6 | len`, long form; every tag must end inside its timeline; `End` closes
  a timeline (bytes after the root `End` are counted, not parsed). `DefineSprite` opens a nested timeline of its
  control tags; a `DefineSprite` inside a sprite is an error (the specification forbids it; retail has none).
* **Kept per tag**: the fields in `swf_movie.h` (`SwfDisplayEvent` for PlaceObject/2/3 and RemoveObject/2 with the
  matrix, colour transform, ratio, clip depth, instance name and PlaceObject3's class name; labels with their frame;
  exports, imports (both versions), ExporterInfo, external images, sub-images, shapes with their bitmap fills from the
  initial style arrays and every `StyleChangeRecord`'s new styles, edit texts, static text ids, scaling grids, fonts
  (header facts plus the tag's byte range), align zones, font names, CSMTextSettings count, DoAction / DoInitAction
  byte counts, DoABC byte counts). PlaceObject3's filter list, blend mode and bitmap cache and the clip actions of
  PlaceObject2/3 are skipped inside the tag's length (flags kept). Unknown codes are counted. At most
  `kSwfMaxDisplayEvents` (2^18; retail maximum 4,075 PlaceObject events in one movie, `compass`) display events per
  movie, so a hostile 16 MiB body of tiny PlaceObject2 tags cannot allocate ~130 bytes of `SwfDisplayEvent` for every
  5 body bytes: the events are bounded at about 34 MB per movie.
* **Shapes** are walked record by record only to collect bitmap fills and count edges: no geometry is kept (no
  renderer, plan D16).
* **Display lists** (`display_lists_at`): the events replayed once per timeline, snapshots taken at the requested
  frames in ascending order. Place puts a character at a depth (normally an empty one; a Place on an occupied depth
  replaces what is there, as Flash players do); Modify changes the given fields; Replace swaps the character and keeps
  unset fields; Remove clears the depth. Cost O(events + Σ snapshot sizes), every event and every copied entry charged
  — the naive "replay from frame 0 for each label" is O(labels × events) and is not allowed (WorkBudget rule). A
  requested frame may be `timeline.frames` itself: the state after every event (a label after the last ShowFrame names
  the frame a closing ShowFrame would show; 0 such labels in retail, a unit case).
* **Units**: pixels (twips / 20) everywhere in the public types, except `SwfBitmapFill::matrix` a..d (twips per
  bitmap pixel) and the font units of `swf_font.h`.

Estimated size ≈ 1,000 LOC with the display lists [inferred].

### 5.2 Library (`swf_library.h`)

`SwfLibrary::open` lists every non-chunk, non-Loc superbundle's bundle manifests on `parallel_for` lanes (as
`bf3dump list` does: the whole install's manifests in 0.5 s, measured), keeps the `SwfMovie` res entries, groups them by
lowercase name, picks the first holder in the §4.1 search order and counts holders and distinct SHA-1s. `read_payload`
/ `load` mount the holder superbundle lazily (one `LevelMount` per holder, `load_bundles` of the holder bundle only)
and read through `LevelMount::read_res`. The lazy creation is guarded per holder: each holder slot has its own
`std::once_flag`-style guard (a `std::mutex` + created flag in the slot, so a failed mount is memoised as its
`Status`), and parallel census lanes that need the same superbundle wait for the one creation; afterwards the mount is
shared read-only (`LevelMount` reads are internally locked). `load_atlas_texture` uses the same mount and
`res::load_texture`. Parsed movies are memoised. Import URLs map to movies by `swf_movie_name_for_url` (lowercase file
stem under `ui/assets/`), the font library URL by `is_font_library_url`.

### 5.3 Census (`swf_census.h`)

`run_swf_census` parses every movie on `parallel_for` lanes (one `WorkBudget` per movie, default size) and runs the
cross-movie checks: each import's movie and export exist; each external image's DxTexture exists with the declared
size (header only, `DxTexture::parse` on the res payload; no chunk read); each sub-image lies inside its image; each
font name resolves (§7.4). Per movie it keeps the fields of `SwfMovieCensus`; the totals are §4.2's table plus:
135 external images, 820 sub-images, 2,441 labels, 1,268 imported symbols with 1 missing, 0 missing atlas
textures, fonts 63 / 21 / 2 (outlines / no glyphs / codes only), AS2 1,638,265 bytes. `format_census_tsv` writes
one row per movie (name, superbundle, holders, sha1s, signature, version, payload and body bytes, stage, frames,
sprites, labels, placements, edit texts, shapes, scaling grids, exports, imports, external images, sub-images,
fonts, AS2 bytes, budget units, retained bytes, parse ms, error). `retained_bytes` is the parsed movie's own
estimate of what it holds (body plus every vector's capacity × element size plus string capacities); the totals
print its maximum and sum, the memory figure the reviewers asked for (no process-level peak, which other lanes
blur).

---

## 6. Atlas cutter

### 6.1 Rule

A labelled frame's art is what its display list draws: for each entry, a shape contributes its bitmap fills that
name a sub-image (as `SwfAtlasPiece`s with the composed placement), a sprite contributes its frame 0 recursively
(≤ `kSwfAtlasMaxNesting` = 16 levels), anything else (edit text, import, static text) nothing. Each sprite's
frame-0 piece list is computed once per map and memoised (in sprite-local coordinates; a referrer copies it with
its own matrix composed, charged per copied piece), so a 16-deep binary DAG of sprites costs O(sprites + pieces),
not 2^16 per reference; a sprite reached again while its own list is being built (a cycle: a sprite placing itself,
directly or through others) is an error. Pieces are capped at `kSwfAtlasMaxPiecesPerRegion` (4,096) per region and
`kSwfAtlasMaxPieces` (2^20, about 64 MB) per map; exceeding either fails the map with an error (a binary DAG
whose leaf holds one piece reaches 4,096 pieces at depth 12 and fails there, cheaply). The same rule gives an
exported symbol's frame 0 (`symbols`), any frame of any sprite (`frames`) and each bare sub-image (`bare`). Names:
`<symbol>/<label>`, `<symbol>`, `<symbol>@<frame>`, `#<id>`, where `<symbol>` is the export name or `#<sprite id>`.

Why not "label → the next sub-image in tag order" or "label i → sub-image i": the probe tried the display-list rule
first because SWF semantics demand it, and the result explains the data: the 162 rectangles and 427 labels are not
in 1:1 correspondence at all (§6.2), so any positional rule would invent a mapping.

### 6.2 iconsingame [verified]

One external image (`iconsingame_i1.tga`, 4096 × 128, DXT5), 162 sub-images, 156 + 17 + 2 shapes, 48 sprites, 9
exports (`IconsIngame_Voip`, `mc_iconHUDVehicleFriend`, `IconsIngame_SquadSpecialization`, `mc_iconsIngameSmall`,
`mc_iconObjectiveMessageSmall`, `mc_iconObjectiveMessageBig`, `IconsIngame_Menu`, `mc_iconsIngameBig`,
`__Packages.Main.Empty`), 427 labels on 6 sprites, none on the root.

| sprite | labels | frames with art |
|---|---|---|
| `mc_iconHUDVehicleFriend` | 9 | 9 |
| `mc_iconsIngameSmall` | 144 | 32 |
| `mc_iconObjectiveMessageSmall` | 3 | 3 |
| `mc_iconObjectiveMessageBig` | 3 | 3 |
| `IconsIngame_Menu` | 122 | 8 |
| `mc_iconsIngameBig` | 146 | 80 |
| **all** | **427** | **135** (110 one piece, 25 two) |

`mc_iconsIngameSmall` and `IconsIngame_Menu` name the same states as `mc_iconsIngameBig` (135 labels in common) but
leave most frames empty: their art is either placed at run time by AS2 or unused. A label's span (its frame up to
the next label) adds no art. 95 sub-images are reached from labels, 96 adding the exported symbols, all 162 from
some frame of some sprite (the 42 unlabelled child sprites hold the rest).

### 6.3 Spot checks for G2 (`mc_iconsIngameBig`, rectangles x1, y1, x2, y2)

| region | pieces |
|---|---|
| `mc_iconsIngameBig/LocalPlayer` | #123 (576, 58, 599, 83) |
| `mc_iconsIngameBig/FriendlyPlayer` | #125 (1070, 58, 1089, 80) |
| `mc_iconsIngameBig/EnemyPlayer` | #127 (1028, 58, 1047, 80) |
| `mc_iconsIngameBig/AmmoCrate` | #157 (498, 0, 550, 52) |
| `mc_iconsIngameBig/EnemyFlag` | #152 (114, 0, 168, 54) |
| `mc_iconsIngameBig/Tank` | #197 (2386, 0, 2411, 34) |
| `mc_iconsIngameBig/CTFFlagEnemyDropped` | #376 (780, 0, 811, 43), #370 (961, 58, 984, 80) |
| `mc_iconHUDVehicleFriend/friendDriver` | #9 (1091, 58, 1115, 79) |
| `mc_iconsIngameSmall/EnemyFlag` | #46 (2013, 0, 2047, 34) |
| `mc_iconsIngameSmall/FriendlyPlayer` | (empty) |

### 6.4 Godot side

`Bf3UiAssets` loads each requested atlas movie's texture with `SwfLibrary::load_atlas_texture`, prepares it with
`bf3::render::prepare_texture(texture, TextureRole::Color)` (DXT5 kept as BC3 with its mips; ARGB8888 as RGBA8) on the
loader thread and uploads it with `render_bridge::make_texture` on the main thread. A region is an `AtlasTexture`
(`set_atlas`, `set_region(Rect2(x1, y1, x2 − x1, y2 − y1))`, `set_filter_clip(true)`, which clamps sampling to the
region at the level being sampled). The HUD draws regions with the canvas default `TEXTURE_FILTER_LINEAR` (no mipmaps:
level 0 only), where filter_clip removes all bleeding. The atlas keeps its mips (prepare_texture keeps them), but its
lower levels already mix neighbouring sub-images, so a Control that opts into a mipmap filter gets bleeding at mip >
0; the HUD wave must not do that for atlas regions (stated in `ui_assets.h`). Multi-piece regions return every piece
with its placement; the HUD draws them in order. Textures are not cached (plan D13: resident BC from the install at
load).

---

## 7. Fonts

### 7.1 DefineFont3 outlines (`swf_font.h`)

`FontID, flags, language, name (u8-prefixed), NumGlyphs, OffsetTable (u32 with WideOffsets), CodeTableOffset,
GlyphShapeTable, CodeTable (u16 with WideCodes), [Ascent, Descent, Leading, AdvanceTable, BoundsTable,
KerningCount, KerningRecords]`. Checks: offsets ascending, the first ≥ (NumGlyphs + 1) × offset size, the last ≤
CodeTableOffset, each glyph's SHAPE ends inside its slot; the code table, the layout block and the kerning table fit
the tag. Two non-standard forms seen in retail are accepted: NumGlyphs 0 with trailing bytes (`NoGlyphs`, trailing
bytes counted) and `OffsetTable[0] = 0` followed by CodeTableOffset and the codes (`CodesOnly`). Glyph SHAPEs: one
fill bit, StraightEdge / CurvedEdge / StyleChange move-to; points become TrueType-style on/off-curve points (a
curved edge adds its control point off-curve and its anchor on-curve), contours closed implicitly.

### 7.2 TrueType (`font_ttf.h`)

Route: in-memory TrueType for `FontFile.set_data` (gap UI §6 option 1) rather than an MSDF atlas built by hand
(option 2). Reasons [inferred]: the curves are already TrueType's quadratic Béziers, so the conversion is lossless
up to rounding; Godot's TextServer then does shaping (HarfBuzz), kerning, hinting, oversampling and caching; MSDF
remains one call away (`FontFile.set_multichannel_signed_distance_field(true)` on the same data) if the HUD needs
crisp scaling; no own rasteriser to maintain.

Decisions:

* **unitsPerEm 2048**, coordinates = round(SWF units / 10), y negated. 20,480 / 10 is exact; the error is at most
  1/4096 em (0.016 px at 64 px). TrueType coordinates are int16: ±32,767 holds every retail value (max |y| 22,240 →
  2,224).
* **Glyph order**: glyph 0 `.notdef` (a hollow box 0.6 × 0.7 em; [inferred], so a missing character is visible),
  source glyph i = glyph i + 1, so glyph ids and source indices never need a map. Both U+0020 glyphs are kept; the
  cmap maps U+0020 to the first (glyph 1). `numGlyphs` = 444 / 34.
* **Winding**: `normalise_winding` per glyph by nesting depth (§4.4 shows the data is already consistent; the pass
  costs nothing on retail and makes a hostile or odd font render with holes intact under the non-zero rule).
* **Metrics**: hhea / OS/2 typo and win ascent 2,224, descent 432 (OS/2 `fsSelection` USE_TYPO_METRICS), line gap 0;
  advances from the layout table / 10, lsb = the glyph's x_min. The SWF bounds table is not trusted; the writer
  recomputes every bounding box from the points.
* **Kerning**: a version-0 `kern` table, one format-0 subtable of (left glyph, right glyph, value / 10), sorted.
  HarfBuzz (Godot's TextServerAdvanced) applies `kern` when no GPOS is present [inferred from HarfBuzz's documented
  behaviour; checked by G4's `fonts.kerning` row: the shaped "AV" at 64 px is narrower than "A" + "V" by 1229 × 64
  / 20480 = 3.84 ± 0.25 px with the §10 settings; measured 3.844 px, so HarfBuzz applies `kern` (§21)]. Fallback if it is not applied: `FontFile.set_kerning` per size in
  `make_font_file` (one call per pair, 335 pairs).
* **name**: IDs 1 (family = the long name), 2 (Regular / Bold), 3, 4, 6 (PostScript name without spaces);
  `Font.get_font_name()` must read back the long name (G4).
* **Determinism**: `head.created` and `head.modified` are 0, `fontRevision` 1.0, no time, path or machine value
  anywhere; the same input always gives the same bytes, so the SHA-256 that `bf3dump swf fonts` prints is
  reproducible (a `core.ui.font.unit` case writes a font twice and compares).
* **OS/2 weight**: Medium 500, Semibold / SemiBold 600, else 400 (bold flag 700).

The writer is about 600 LOC, the reader (validation, tests, cache accept) about 300 [inferred].

### 7.3 Coverage and "MÉTRO"

Purista covers U+00C9, so "OPERATION MÉTRO" (`ID_M_LEVELNAME_MP_SUBWAY`) renders in BF3's menu font; Venice has no
letters at all, so any HUD text with letters uses Purista (`$MENU_*`) as BF3 itself does (the ammo widget's "Gx"
and reload hint are `$Menu_medium`, its digits `$HUD_medium`). The coverage table of each font (its distinct codes)
is `ConvertedFont::codes`, drawn by the check (G5) and printed by `bf3dump swf fonts`. Characters the text database
uses that a font lacks fall back to a system font: `make_font_file` leaves `allow_system_fallback` true (§10), so
TextServer's shaping substitutes them; they are listed by the check (`fonts.coverage` row, reported, not gated).
The BF3 font's own coverage checks (`fonts.glyphs`, `fonts.metro`) look up glyphs on the FontFile's own data
(`get_glyph_index`, `TextServer.font_get_supported_glyphs`), which system fallback does not touch.

### 7.4 Name resolution (`resolve_font_name`)

| rule | names (of the 434 movies, en collection) |
|---|---|
| Exact | `$HUD_medium`; the long names `Purista EA Medium`, `Purista EA Semibold`, `Venice-Medium`, `Venice-SemiBold` (edit texts of the font library itself; revision 2 missed the fourth) |
| CaseFolded (ASCII case, one trailing `*`) | `$Menu_bold` → `$MENU_bold`, `$Menu_medium` → `$MENU_medium` |
| Family [inferred] | `$HUD`, `$HUD_light` → `$HUD_medium`; `$Menu_light*` → `$MENU_medium`; `$FONT_TITLE`, `$FONT_TITLE*` → `$MENU_bold` |
| Device (system font) | `Arial`, `Purista SemiBold`, `Calibri`, `Verdana`, `Times New Roman` |
| Unresolved | none (G6) |

Counting key for G6: distinct names after dropping one trailing `*` (`$FONT_TITLE*` → `$FONT_TITLE`,
`$Menu_light*` → `$Menu_light`), so the rows count Exact 5, CaseFolded 2, Family 4 (`$FONT_TITLE`, `$HUD`,
`$HUD_light`, `$Menu_light`), Device 5. The census and `bf3dump swf fonts` print the Family names on their own line.

The Family rule is a guess about BF3's own runtime substitution (Scaleform resolves unknown `$` names through the
font library's map; BF3's map lacks these four). It affects one MP HUD screen (`hudctfscreen`, `$HUD_light`) and
menus. `Purista SemiBold` (3 menu screens) could be mapped to `$MENU_bold` by family similarity; it stays a device
font until a screen needs it [inferred].

### 7.5 Font library (`ui_fonts.h`)

`UiFontLibrary::load`: `font_mapping_for_text_database(ui_ebx, "localization/us_loc")` (Win32/UI EbxLibrary, lazy)
→ the en collection → `ui/static/fontcollection_en_fontlib` (`SwfLibrary::load`) → for each mapped font
`convert_library_font` (find the `DefineFont3` by long name, `load_font_outlines`, `write_truetype`, through
`cached_convert`). The language follows the text database the game loaded (`Bf3Localisation`), so a German install
picks `fontcollection_de_*` with no code change; only en is installed here, so only en is tested.

---

## 8. HUD layout facts

### 8.1 Rule

`read_layout(movie)` builds the tree of the root timeline at frame 0: each display entry becomes a node with its
instance name, kind, local and stage matrix; a sprite's children are its frame 0; an imported symbol is followed
into the exporting movie (`swf_movie_name_for_url`, export by name) up to `kSwfLayoutMaxDepth` = 8; edit texts carry
font name, size, colour, alignment, bounds and initial text; a sprite with a `DefineScalingGrid` carries its
splitter (for a `NinePatchRect`). `read_hud_placements(movie)` flattens the screen level: the children of the screen
sprite that the root's `instance1` places, with stage matrices and an inferred anchor (`hud_anchor_for`: the origin
in thirds of the stage). The table is read from the install at run time; it is never written into the repo (it is
BF3 data). The numbers below are design facts like the other measured values in this tree.

### 8.2 The table [verified] (stage pixels, 1280 × 720; scale 1 unless given)

| screen | placements | widgets (instance → widget at x, y) |
|---|---|---|
| `hudmpscreen` | 14 | LatencyIndicator_01 (1274, 3); Hitindicator_01 (640, 360); PassangerList_01 (707, 547); Ammo_01 (1194, 669); Health_01 (1193, 605); VehicleHealth_01 (1195, 555); HudMessage_05 (640, 632.2), _04 (500, 120, sy 0.999), _02 (1280, 0), _01 (640, 506); ScoreAggregator_01 (773, 486); SupportIconManager_01 (0, 0); HudAlertMessage_02 / _01 (640, 156) |
| `hudtdmscreen` | 7 | HudBackgroundWidget_01 (−3, 486.95, scale 1.288 × 3.689); Compass_01 (85.05, 696, scale 0.99985 × 1); Minimap_01 (−3, 527); TicketCounter_01 (−4, 530); SquadList_00 (179, 619); HudMessage_00 (640, 675); MapmarkerManager_00 (0, 0) |
| `hudconquestscreen` | 9 | HudBackgroundWidget_01 (−3, 456, 1.288 × 4.172); Compass_01 (85.05, 696, 0.99985 × 1); Minimap_01 (−3, 527); TicketCounter_01 (−4, 530); ObjectiveBar_01 (−4, 459); SquadList_00 (179, 619); HudMessage_00 (640, 675); CapturepointManager_01, MapmarkerManager_01 (0, 0) |
| `hudscreen` | 8 | HudMessage_02 (640, 711), _03 (640, 579); ObjectiveMessage_01 (0, 0); LogMessage_01 (0, 534); TooltipMessage_01 (1280, 360); DamageIndicator_01 (640, 360); AlertManager_01, InteractionManager_01 (0, 0) |
| `killscreen` | 4 | KillCam_01 (0, 0); HudMessage_01 (640, 514), _02 (640, 604); ScoreAggregator_01 (773, 494) |
| `hudsdmscreen`, `hudrushscreen`, `hudctfscreen`, `hudgmscreen`, `spawnscreenpc`, `hudpreroundscreen`, `spectatorscreen` | 8, 9, 10, 7, 8, 1, 1 | (in the test; `bf3dump swf hud`) |
| `hudmatchpreroundscreen`, `weaponcommonscreen` | 0, 0 | nothing at frame 0: placed by AS2 at run time (crosshairs stay procedural, plan §3.8) |

86 placements in all (G7). Inside a widget, `read_layout("ui/assets/ammo")`: the `Ammo` sprite at the stage centre
(640, 360) holding `bgAmmo` (HudBackgroundWidget, (−89, −42), 1.326 × 1.25), `clip` (2, −46), `amholder` (3, −23),
`reload` (−14, 18), `fmd` (83, 12), `atp` (71, −25), `grtxt` (−37, −3), `weaponProgress` (−84, 25); edit texts in
`$HUD_medium` 21 px and 43 px (right-aligned "000"), `/` 18 px, `$Menu_medium` 20 / 21 px ("Gx", `ID_H_RELOAD_HINT`),
all white, `UseOutlines` set. Widget instances' own positions inside their movies' stages (the ammo widget's movie
places it at 640, 360 and the screen at 1194, 669) are composed by the tree, not by the table [verified on ammo].

### 8.3 What the HUD wave does with it [inferred]

The reference resolution is BF3's stage (1280 × 720). A HUD Control keeps each widget's offset from its anchor and
scales by `min(width / 1280, height / 720)` (BF3's own scale mode is set by AS2 and is not in the data; the
research's 16:9 screenshots are the oracle). The project's window stretch mode (`canvas_items`, `expand`) already
scales Controls; whether the HUD takes 1280 × 720 or the window as its base is the HUD wave's call.

---

## 9. Hostile-input bounds

The WorkBudget rule: every loop over data charges before it runs; superlinear work is the blocking criterion. The
budget type is `level::WorkBudget` (atomic, sticky; reused rather than a third copy).

| where | bound |
|---|---|
| payload | signature `CFX` / `GFX`; declared length ≤ 16 MiB (`kSwfMaxBodyBytes`) **before** inflating; CFX must inflate to exactly the declared length (libdeflate with a fixed output size: a zlib bomb fails at the cap); GFX length exact |
| tags | header inside the timeline; long length ≤ bytes left; ≤ `kSwfMaxTags`; one unit per tag; sprites one level deep (a nested `DefineSprite` is an error, so no recursion on data) |
| strings | NUL inside the tag; charged `1 + length / 16`; u8-prefixed strings inside the tag |
| records | RECT / MATRIX / CXFORM bit fields read through a bounds-checked bit reader; style arrays: count × minimum record size ≤ bytes left before reading; one unit per shape record (≥ 6 bits each, so ≤ 1.4 units per body byte) |
| frames | ShowFrame count ≤ `kSwfMaxFrames`; a label's frame ≤ the timeline's ShowFrame count (= is allowed, §5.1) |
| display lists | ≤ `kSwfMaxDisplayEvents` (2^18) events per movie; one pass per timeline (§5.1); every event charged 8 units (≈ its 130 bytes / 16) and every copied entry 4 units, so the budget bounds memory as well as time |
| sub-images | x1 ≤ x2 and y1 ≤ y2 or the parse fails (`width()` / `height()` can then never wrap); inside the atlas is a census / atlas-map check (`rects_outside_atlas`) |
| atlas map | nesting ≤ 16; per-sprite frame-0 lists memoised, a cycle is an error; each visited display entry and each piece (copies included) charged 4 units; ≤ 4,096 pieces per region and 2^20 per map |
| layout tree | depth ≤ 8, ≤ 65,536 nodes, imports followed only to existing movies, each movie parsed once per call |
| fonts | NumGlyphs ≤ 65,535; offsets ascending and inside; points per glyph ≤ 16,384, contours ≤ 4,096; codes and kerning counts × record size ≤ bytes left; one unit per edge, point, code and kerning pair |
| winding | point-in-polygon of one point per contour against every other contour: c × (total points) edge tests per glyph, charged before it runs (retail: ≤ 24 × 281; a hostile 4,096-contour, 16,384-point glyph would need 67M and ends on the font budget below) |
| TrueType writer | glyphs ≤ 65,534 (+ `.notdef`), points ≤ 65,535 and contours ≤ 32,767 per glyph, coordinates in int16 after scaling; the output is O(input) (≤ 9 bytes per point plus fixed tables) |
| TrueType reader | table directory ≤ 64 tables, every table inside the file, checksums, `loca` ascending and inside `glyf`, flag repeats bounded by the point count, cmap segments ≤ 8,192 |

### 9.1 Budgets by caller

Every function that takes `WorkBudget &r_budget` gets it from its caller; these are the sizes (constants in the
headers, so the G8 "< 1 s" cases are bounded by a documented number; a unit is one tag, record, event, edge, point,
16 string bytes or one inner-loop step, a few ns to a few tens of ns each).

| work | budget | who creates it |
|---|---|---|
| `SwfMovie::parse` | `swf_default_budget_units(declared length)` = 2^20 + 2 × declared bytes (≤ 34.6M at the 16 MiB cap) | the parse itself (null budget), `SwfLibrary::load` (one per movie), the census (one per movie lane) |
| `build_atlas_map` | `kSwfAtlasBudgetUnits` = 2^24 per map | `Bf3UiAssets` loader, `core.ui.swf`, `bf3dump swf labels`, the census (per atlas movie) |
| `load_font_outlines` + `normalise_winding` + `write_truetype` + `read_truetype` of one font | `kUiFontConvertBudgetUnits` = 2^25 per font, one budget for the whole conversion | `convert_library_font` (so `UiFontLibrary::load`, `Bf3UiAssets`, `bf3dump swf fonts`) |
| `read_truetype` alone (cache accept, tests) | `kTrueTypeReadBudgetUnits` = 2^22 per call | the cache accept step, `core.ui.font(.unit)` |
| `read_layout` / `read_hud_placements` | `kSwfLayoutBudgetUnits` = 2^22 per call (the movies it follows are parsed through `SwfLibrary::load`, each with its own parse budget) | `Bf3UiAssets` loader, `core.ui.swf`, `bf3dump swf layout` / `hud` |

The census prints the largest use per movie for each budget (retail is expected to stay near 1 unit per body byte,
half the default). At ~10–30 ns per unit, 2^25 units end in under 1 s in `dev`; the unit tests time each hostile case
against 1 s.

---

## 10. Godot side (`ui_assets.h`)

`Bf3UiAssets` (Node) — load sequence in §3. Getters (§12.3). `make_font_file`: `FontFile.new()`, `set_data`,
then fixed settings that the HUD and the G4 check share (the check measures the very FontFile the getters return):

| setting | value | why |
|---|---|---|
| `antialiasing` | `FONT_ANTIALIASING_GRAY` | default; no LCD subpixel (HUD over 3D) |
| `hinting` | `HINTING_NONE` | Scaleform draws unhinted outlines; with hinting on, FreeType can round each glyph's advance (`font_get_glyph_advance`) to whole pixels |
| `subpixel_positioning` | `SUBPIXEL_POSITIONING_ONE_QUARTER` | the default `AUTO` turns subpixel positioning off above 20 px (`SUBPIXEL_POSITIONING_ONE_HALF_MAX_SIZE`), and TextServerAdvanced then rounds every shaped advance to a whole pixel, carrying the remainder only up to the next blank (`keep_rounding_remainders`, reset at spaces): "OPERATION MÉTRO" at 64 px can be ~1 px off and per-glyph rounding without the carry would be up to 4.4 px off (review probe). With subpixel positioning on, the shaped advance is HarfBuzz's unrounded 26.6 value |
| `keep_rounding_remainders` | left at its default (true) | irrelevant while subpixel positioning is on; the check does not depend on it |
| `allow_system_fallback` | true | characters a BF3 font lacks (Venice has no letters) come from a system font in shaping (§7.3) |
| `multichannel_signed_distance_field` | false | a switch for the HUD wave (§15) |
| `oversampling` | default (follows the viewport) | |

[verified against the TextServerAdvanced source of Godot 4.8 dev7 (`modules/text_server_adv/text_server_adv.cpp`
at `c971f93e7`, MIT): `_shape_substr` rounds `gl.advance` unless `subpos`, and `subpos` is false for `AUTO` above
`SUBPIXEL_POSITIONING_ONE_HALF_MAX_SIZE` = 20; the shaping font is `hb_ft_font_create(face)` with no load flags
set, i.e. HarfBuzz's documented default `FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING`, so shaped advances are unhinted
whatever `hinting` says; `_ensure_glyph` passes the `hinting` load flags to `FT_Get_Advance` for the per-glyph
advance. Read for this design, nothing copied.] Then the check that
`get_font_name()` equals the long name; `make_atlas_region` as §6.4. Missing items (no install, a movie or font that
is absent): the getter returns null and logs once; the HUD falls back to its own art and Godot's default font
(plan §3.8 route C per item).

The extension registers `Bf3UiAssets` and `Bf3UiFontCheck` (two `GDREGISTER_CLASS` lines in an additive `// UI ui1`
block of `register_types.cpp`).

---

## 11. Godot check and screenshots (`ui_font_check.h`, `res://scenes/ui_fonts.tscn`)

### 11.1 Rows (`-- --ui-check`)

| row | check |
|---|---|
| `fonts.set_data` ×4 | `FontFile.set_data` accepted, `get_font_name()` = the long name |
| `fonts.glyphs` ×4 | the TrueType `numGlyphs` = source + 1 (444 / 34); `get_supported_chars().length()` = distinct codes (442 / 32); `has_char` for every code; `TextServer.font_get_supported_glyphs` printed |
| `fonts.contours` ×4 | for every glyph, `TextServer.font_get_glyph_contours(rid, 64, glyph)` has the writer's contour count and each contour's orientation (signed area sign) equals the writer's (outer clockwise, holes counter-clockwise) |
| `fonts.metrics` ×4 | on the §10 settings at 64 px: `get_ascent(64)` = 2224 × 64 / 2048 = 69.5 ± 1 px (FreeType rounds size metrics to whole pixels); for **every glyph** `TextServer.font_get_glyph_advance(rid, 64, glyph).x` = source advance × 64 / 20480 ± 0.05 px (26.6 resolution is 1/64 px); the shaped width of `"0123456789"` and (Purista) `"OPERATION MÉTRO"` (the sum of `TextServer.shaped_text_get_glyphs()` advances of one shaped line) = Σ source advances × 64 / 20480 + Σ kerning pairs × 64 / 20480 ± 0.5 px (15 glyphs × 1/128 px of 26.6 rounding ≈ 0.12 px worst case). Not `Font.get_string_size()`: it and `shaped_text_get_width()` return the line width ceil()ed to whole pixels on dev7 (measured 367.000 px for a 366.519 px line), while the shaped advances themselves are fractional (39.8125 px) |
| `fonts.kerning` ×2 | Purista: the shaped `"AV"` narrower than `"A"` + `"V"` by 1229 × 64 / 20480 = 3.84 ± 0.25 px at 64 px (if HarfBuzz ignores `kern`, 0 px: the row fails and the §7.2 fallback applies) |
| `fonts.metro` | `tr("ID_M_LEVELNAME_MP_SUBWAY")` = "OPERATION MÉTRO" (Bf3Localisation), and every character of it maps to a non-zero glyph index in Purista EA Medium |
| `fonts.mapping` | the census's font names resolve, 0 unresolved |
| `fonts.coverage` | (reported) characters of the 15,515 text database strings that Purista lacks |
| `atlas.iconsingame` | the §6.2 / §6.3 numbers through `Bf3UiAssets`; the atlas `ImageTexture` is 4096 × 128 |
| `atlas.regions` | `get_region` of the §6.3 rows returns `AtlasTexture`s with those rectangles |
| `hud.table` | 86 placements on the 14 screens; the §8.2 spot values |
| `install.unchanged` | 256 index files |

### 11.2 Screenshots (G5)

Windowed run with `--shot-dir`, three pages at 1280 × 720, dark grey background, white text:
`ui1_fonts.png` (each font at 16 / 32 / 64 px: "OPERATION MÉTRO", "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz",
"0123456789 31 / 155", "AV AT To LT Yo" kerning pairs; Venice: "0123456789", "31/155 100%"; a 1 px red baseline
under each line), `ui1_coverage.png` (every code of each font in a grid with its hex value), `ui1_icons.png` (the
iconsingame labels with art, each drawn from its `AtlasTexture` with its name). The test writes to
`$ENV{OPENBF3_UI1_SHOTS}` (default `C:\Users\olku\BF3 reimplementation\openBF3\b\shots\ui1\`, an absolute path
outside the worktree, gitignored, never committed; `tests/godot_ui_fonts_shots.cmake` passes it as `--shot-dir`
and creates it); the implementer inspects them and writes the findings into README.

`tests/godot_ui_fonts.cmake` (headless) and `tests/godot_ui_fonts_shots.cmake` (windowed: a probe run first, as
`godot_m1_load.cmake` does, so a session without a desktop exits 77 instead of hanging on the rendering-device
alert) run the scene; both hold the `openbf3_godot_project` resource lock.

---

## 12. Interfaces

### 12.1 Headers (in the tree, compiling; `ui1.contract`)

| header | contents |
|---|---|
| `bf3/core/ui/swf_movie.h` | `SwfMovie::parse`, tag codes, `SwfMatrix` / `swf_concat`, timelines and `SwfDisplayEvent`, `display_lists_at`, exports / imports, ExporterInfo, `SwfExternalImage`, `SwfSubImage`, `SwfShape` (bitmap fills), `SwfEditText`, `SwfScalingGrid`, `SwfFontInfo` (+ kinds), lookups |
| `bf3/core/ui/swf_library.h` | `SwfLibrary` (open / find / read_payload / load / load_atlas_texture; added by the implementation: `read_atlas_texture_header` for the census's size check and `from_memory` for the unit and fuzz tests' in-memory movies), `swf_superbundle_order`, `swf_movie_name_for_url`, `is_font_library_url`, `swf_atlas_texture_name` |
| `bf3/core/ui/swf_census.h` | `run_swf_census`, `SwfMovieCensus`, totals, TSV (added: `missing_import_symbols`, `font_names`, `max_atlas_budget_used`, the other font libraries' names checked against their own collection (`library_mappings`, `library_font_names*`), `build_atlas_maps`) |
| `bf3/core/ui/swf_atlas.h` | `build_atlas_map`, `SwfAtlasMap` (images, labels, symbols, frames, bare; `find`), `SwfAtlasPiece`, stats |
| `bf3/core/ui/swf_font.h` | `parse_font_outlines`, `load_font_outlines`, `SwfFontOutlines`, `GlyphOutline`, `normalise_winding` |
| `bf3/core/ui/font_ttf.h` | `write_truetype`, `TrueTypeOptions`, `TrueTypeStats`, `read_truetype`, `TrueTypeFont`, `kTrueTypeWriterVersion` |
| `bf3/core/ui/ui_fonts.h` | `read_font_mapping`, `font_mapping_partitions`, `font_mapping_for_text_database`, `read_all_font_mappings` (added), `resolve_font_name` (`FontResolution`), `UiFontLibrary`, `ConvertedFont`, `convert_library_font`, `kUiFontConverterVersion`, `kUiFontCacheKind` |
| `bf3/core/ui/swf_layout.h` | `read_layout` (`SwfLayoutNode`, `SwfLayoutText`), `read_hud_placements` (`HudPlacement`, `HudAnchor`), `hud_screen_movies` |
| `src/bf3_godot/src/ui_assets.h` | `Bf3UiAssets`, `make_font_file`, `make_atlas_region` (added: `movies()`, `hud_placements()`, `atlas_texture()` for the check) |
| `src/bf3_godot/src/ui_font_check.h` | `Bf3UiFontCheck` (added private members: the check's worker thread and page drawing) |

### 12.2 Error and data conventions

`Result` / `Status` values, messages `swf <movie>: <what> at <offset>` (container errors keep `LevelMount`'s text);
no exceptions; plain structs, sorted vectors; strings copied out of the body (a `SwfMovie` owns everything except
`SwfDisplayEntry::name`, a view into its own events).

### 12.3 Seams for the later HUD screens and flows

* **Fonts**: `Bf3UiAssets::get_font("$HUD_medium")` (and `$MENU_*`); the HUD sets it on its `Theme` / `Label`s.
  Sizes come from the widgets' edit texts (`get_layout`), e.g. the ammo count 43 px `$HUD_medium` right-aligned.
* **Icons**: `get_region("ui/assets/iconsingame", "mc_iconsIngameBig/<state>")`; the kill feed's weapon icons are
  plain DxTextures (`UIWeaponDescription.IconTexturePath`, research §5) and need no atlas; the minimap's world icons
  are ui2's `hud3dicons`.
* **Positions**: `get_hud_placements("ui/assets/hudmpscreen")` + the mode screen (`hudtdmscreen`, ...) give the
  widget anchors; `get_layout("ui/assets/<widget>")` the widget's inner layout, fonts, colours, nine-slice grids
  (`NinePatchRect.set_patch_margin` from `scaling_grid`) and the atlas pieces of its `hudtextures` art.
* **Strings**: edit texts' initial texts that are SIDs go through `tr()` (`Bf3Translation`, M0).
* **Flows** (spawn, death, kill card, scoreboard, end of round) are C++ state machines in later waves (plan §3.8);
  they use the same getters for their screens (`spawnscreenpc`, `killscreen`, `scoreboardtwoteamshud*screen`).
* **Data model**: ui2's `UI*CompData`-named signals feed the widgets; ui1 provides no signals.

### 12.4 bf3dump (`swf_commands.{h,cpp}`; output is BF3 data: keep it out of the repo)

| command | output |
|---|---|
| `swf census [--tsv <out.tsv>] [--threads=N]` | the census totals (and per-movie TSV); exit 1 on any error |
| `swf movie <name>` | header, ExporterInfo, imports, exports, external images, sub-images, fonts, edit texts, scaling grids, sprites (frames, labels), root placements |
| `swf labels <name>` | the atlas map: images, every region with its pieces and the atlas texture res |
| `swf layout <name> [--depth=N]` | the layout tree, one line per node |
| `swf hud` | the HUD placements table of the 14 screens |
| `swf fonts [--text-db=<partition>] [--cache=<dir> \| --no-cache]` | the mapping, each font's glyphs, codes, coverage, kerning, winding stats, TrueType size and SHA-256, cache outcome; the name-resolution table of §7.4 (Family-inferred names on their own line). `--cache=<dir>` names a derived-data Cache root exactly as `bf3dump prepare --cache` does (default: the game's `user://bf3cache`, `%APPDATA%\Godot\app_userdata\bf3-opensource\bf3cache`; guarded by `InstallReader::write_guard()`); entries are digest-keyed Cache records, never a loose `.ttf`, and the command writes no font file anywhere else |

There is deliberately no command that writes a converted font to a path of the caller's choice (revision 1's
`swf ttf` was dropped in review): plan §2.1 keeps a converted font in `user://` / the Cache only, and §13 below.
Inspection needs are covered by `swf fonts` (sizes, SHA-256, every table's stats) and by `read_truetype` in the
tests.

---

## 13. Caching

| kind | key sources | versions | params | payload | accept |
|---|---|---|---|---|---|
| `ui-font` | manifest SHA-1 of the font library SwfMovie res (`SwfMovieSource::sha1`) | converter `kUiFontConverterVersion` (1), engine id (`cache_engine_id()`), format `kTrueTypeWriterVersion` (1) | `font=<long name>;upem=2048` | the TrueType file | `read_truetype` succeeds and its family is the long name |

Not cached: SWF parses (the en font library parses in about a millisecond and the HUD movies are a few KB; the
census of everything is a tool), atlas textures (plan D13), atlas maps and layout tables (derived in microseconds
from parsed movies) [inferred; measured by T1 / T2]. The converted fonts never leave the cache root (plan §2.1: a converted
font "lives only in user://, never in the repo or anything that leaves the machine"): no API or command writes the
TrueType bytes anywhere but the Cache (and memory, for `FontFile.set_data`); the unit tests' synthetic fonts are not
EA fonts, and `core.ui.font`'s scratch cache root under `%TEMP%` is deleted at the end of the test.

---

## 14. Test plan

| test | data? | content |
|---|---|---|
| `ui1.contract` | no | every ui1 header compiles together; static_asserts (limits, units, tag codes, trivially copyable types); `swf_concat`; the cache kind is valid and the key moves with the converter version (exists now) |
| `core.ui.swf.unit` | no | a small in-test SWF writer builds movies: GFX and CFX headers, every tag the reader keeps (PlaceObject 1/2/3 with all optional fields, RemoveObject 1/2, labels, sprites, exports, both imports, ExporterInfo, external image v1 / v2, sub-images, shapes 1–4 with bitmap fills in the initial arrays and in StyleChangeRecords, edit texts with every flag, scaling grids, DefineFont3 in all three forms, align zones, font names); display lists (Place / Modify / Replace / Remove, a Place on an occupied depth, queries in any order, one-pass cost); the atlas rule (nested sprites, composed matrices, empty frames, multi-piece); layout trees with a fake library; hostile: every truncation of each synthetic movie, 2,000 seeded mutants, a CFX that inflates past its declared length, a 2^32-declared length, a tag past the end, a nested DefineSprite, 2^22 one-byte tags, a sprite placing itself (cycle error) and a 16-deep binary DAG of sprites (memoised: linear, and its piece count capped), 65,535 frames, a label after the last ShowFrame (frame == frames), a reversed sub-image rectangle (x2 < x1, refused), 2^18 + 1 display events (refused), a 16 MiB + 1 declared length (refused before inflating); each hostile case timed against 1 s; asan |
| `core.ui.font.unit` | no | synthetic DefineFont3 (squares, a ring with a hole, quadratic curves, duplicate codes, kerning) → outlines → `write_truetype` → `read_truetype` round trip (points, flags, advances, cmap, kern, name, checksums); `normalise_winding` on reversed and nested contours; TrueType limits (65,535 points, int16 overflow) refused; a 4,096-contour, 16,384-point glyph ends on the 2^25 font budget in under 1 s; the same font written twice gives the same bytes; every truncation of a written font refused by `read_truetype`; 2,000 mutants of each; asan |
| `core.ui.swf` | install | G1 (census, totals = §4.2), G2 (§6.2, §6.3), G6 (§7.4), G7 (§8.2), T1 (checked in `dev`, reported under asan), `install.unchanged` |
| `core.ui.swf.fuzz` | install | 300 seeded in-memory mutants each of `iconsingame`, `hudmpscreen`, `ammo` and `fontcollection_en_fontlib` through `SwfMovie::parse`, `build_atlas_map`, `read_layout` (over the original library), `load_font_outlines` and `write_truetype`: no crash, every failure an error; asan |
| `core.ui.font` | install | G3, T2 (checked in `dev`, reported under asan), cache cold / warm / damaged on a scratch root under `%TEMP%` (deleted afterwards), `install.unchanged` |
| `godot.ui_fonts` | install + Godot | G4 (§11.1), headless |
| `godot.ui_fonts_shots` | install + Godot, windowed | G5 (§11.2), shots to `OPENBF3_UI1_SHOTS`; 77 without a window |

The install tests exit 77 without an install (`bf3::test::skip_install_case`). New test executables: `test_ui1`
(suites `ui-swf-unit`, `ui-font-unit`, `ui-swf`, `ui-swf-fuzz`, `ui-font`) next to `test_ui1_contract`. All
`core.ui.*` tests also run under the `asan` preset (G8); the fuzz suites get a longer TIMEOUT there.

---

## 15. Deferred

| item | to |
|---|---|
| HUD widgets and screens (ammo, health, crosshair, hit indicator, compass, kill feed, messages) laid out from §8 | M2a HUD wave (route A+ minimal HUD) |
| spawn screen, scoreboard, kill card, end of round, their flows | M2b and later UI waves |
| minimap, `hud3dicons` UV atlas, `UI*CompData` signal model | ui2 (this wave) |
| vector art (DefineShape geometry: backgrounds, frames without atlas art), gradients, colour transforms applied, morphs | not planned: the HUD draws those with Controls / procedural drawing (plan D16); revisit only if a screen cannot do without |
| AS2-driven behaviour (frame scripts, `attachMovie`, run-time placement: crosshairs, hudmatchpreroundscreen) | never interpreted; each behaviour is reimplemented in C++ where the HUD needs it |
| CJK, Cyrillic-heavy and other languages (the readers are language-independent; only en is installed and tested) | when a non-English install is available (plan §3.8 open item) |
| MSDF rendering of the fonts | a switch on the same `FontFile` if the HUD needs it |
| the exact HUD scaling policy for non-16:9 windows | M2a HUD wave |

---

## 16. Risks

| risk | effect | mitigation |
|---|---|---|
| Godot's TextServer does not apply the legacy `kern` table | kerning missing in menus (Purista; HUD digits have none) | G4 row `fonts.kerning`; fallback `FontFile.set_kerning` per size (§7.2) |
| FreeType / Godot reject an unusual TrueType detail (e.g. a missing table it expects) | `set_data` fails | G4 runs every font through Godot; the reader mirrors the tables Godot and FreeType require; keep the table set conventional (§7.2) |
| The Family rule for `$HUD_light` / `$FONT_TITLE` picks a different weight than BF3 | a CTF HUD line or a menu title in the wrong weight | listed by the census; a screenshot of the user's own game can settle it; one-line change in `resolve_font_name` |
| Labelled frames that AS2 fills at run time (292 empty in iconsingame) | the HUD needs a state the atlas map leaves empty | `mc_iconsIngameBig` has art for 80 of its 146 states and the HUD can use the Big set at small scale; ui2's `hud3dicons` covers the world and minimap icons |
| Anchors are inferred from positions (BF3's anchoring is AS2) | widgets drift on 21:9 or 4:3 | the HUD wave compares against screenshots; the layout tree keeps raw positions so any rule can be applied |
| A patched or modded install changes a movie | parse error or different numbers | G1 reports per movie; the cache key moves with the SHA-1; fallbacks per item |
| Font conversion inside Godot's text cache uses more memory than expected | none expected (4 fonts, ~150 KB of TrueType) | measured by the check (reported) |
| Overlap with ui2 helpers at merge | duplicate atlas helper | §2: one `make_atlas_region`, merged at integration |
| ui1 and ui2 both append blocks at the end of `tests/CMakeLists.txt`, `src/bf3_core/CMakeLists.txt`, `src/bf3_godot/CMakeLists.txt` and README | trivial textual merge conflicts | keep both blocks; no name collides (`test_ui1*` / `test_ui2_contract`, `core.ui.swf/font` beside the existing `core.ui.text`) |
| Godot's text layout changes (subpixel / rounding rules) in a later engine pin | `fonts.metrics` drifts | the §10 settings are explicit, not defaults; the row prints measured vs expected |

---

## 17. Files

Created (design, in the tree now): `docs/design/ui-swf-fonts.md`; `src/bf3_core/include/bf3/core/ui/{swf_movie,
swf_library, swf_census, swf_atlas, swf_font, font_ttf, ui_fonts, swf_layout}.h`; `src/bf3_godot/src/ui_assets.h`,
`ui_font_check.h`, `ui_assets.cpp` (includes only, no bodies yet); `tests/ui1_contract_tests.cpp`.

Created by the implementation: `src/bf3_core/src/ui/{swf_movie, swf_display, swf_library, swf_census, swf_atlas,
swf_font, font_ttf_write, font_ttf_read, ui_fonts, swf_layout}.cpp` (+ a private `swf_bits.h` bit reader);
`src/bf3_godot/src/ui_assets.cpp` bodies, `ui_font_check.cpp`; `bf3-opensource/scenes/ui_fonts.tscn`;
`tools/bf3dump/swf_commands.{h,cpp}`; `tests/ui_swf_tests.cpp`, `tests/ui_font_tests.cpp`, `tests/ui_swf_writer.h`
(the in-test SWF writer), `tests/godot_ui_fonts.cmake`, `tests/godot_ui_fonts_shots.cmake`.

Modified additively (blocks headed `# UI ui1` / `// UI ui1`; existing lines untouched): `src/bf3_core/CMakeLists.txt`
(headers and the `# UI ui1 implementation` sources block), `src/bf3_godot/CMakeLists.txt` (the ui1 block, now with
`ui_font_check.cpp`), `tests/CMakeLists.txt` (`ui1.contract`, then the `# UI ui1 implementation` block: `test_ui1` and
the two Godot tests), `src/bf3_godot/src/register_types.cpp` (two `GDREGISTER_CLASS`),
`tools/bf3dump/CMakeLists.txt` and `tools/bf3dump/bf3dump.cpp` (usage lines for the six `swf` commands of §12.4
and the `swf` dispatch), `README.md` (a "UI groundwork 1" section and one line
under the ctest list paragraph), `NOTICE` only if third-party code is consulted (none was: the SWF and TrueType
specifications are documentation).

---

## 18. Facts checked for this design

| fact | value | how |
|---|---|---|
| SwfMovie entries | 434 names, 10,303 entries, 50 superbundles; first holders §4.1 | `bf3dump list` into scratch, filtered: "53 superbundles, 1159 bundles, 1093563 entries, 0 errors" (the TSV has 1,094,723 lines = header + 1,093,563 entries + 1,159 bundle rows; revision 1 quoted the line count) |
| payload format and size | 371 CFX v8, 62 GFX v8, 1 GFX v7; declared length exact on 434; resMeta 0 | ebx-dump probe `rawres` of the 11 covering superbundles + `swfprobe.py` |
| identical bytes across holders | 0 names with 2 versions among the 11 superbundles | SHA-1 of every extracted payload |
| tag census | §4.2 | `swfprobe.py census` (434 parsed, 0 errors after the CodesOnly fix) |
| external images, atlases | 135; DxTexture in the same bundle 135; size equal 135; 132 DXT5 + 3 ARGB8888; 820 / 820 rectangles inside | probe + the DxTexture res headers (offsets 0x08 format, 0x10 / 0x12 size, as `DxTexture::parse`) |
| sub-image rectangle convention | x2 / y2 exclusive (737 shapes exact) | probe: shape bounds vs rectangles |
| iconsingame map | §6.2, §6.3 | probe `icons2` with RemoveObject handling and nested sprites |
| fonts | §4.4 (glyphs, codes, coverage, metrics, kerning, winding) | probe `font` on `fontcollection_en_fontlib`; all 63 outline fonts for sorting / maxima |
| atlas file names | 135; suffixes `_i1`…`_i9` 118, `_i12` 2, `_i23` 1, `_ia`…`_ie` 14 | `bf3dump list` DxTexture names + probe `ext_images` |
| labels and frames | 0 of 2,441 labels after the last ShowFrame; 718 empty sprites (FrameCount 1, no tags); 0 reversed sub-image rectangles; at most 4,075 PlaceObject events in a movie (`compass`) | probe (review revision) |
| Godot text layout | §10 table (AUTO → no subpixel above 20 px, rounded advances, remainder reset at blanks, hb_ft unhinted advances) | Godot `text_server_adv.cpp` and `text_server.h` at `c971f93e7`, read only |
| font mapping | §4.5 | `bf3dump types @all-mp UIFontMappingCollection`, `bf3dump dump` of all 33 |
| font names used | §4.4 last bullet, §7.4 | probe over all imports and glyphless fonts |
| imports resolve | 1,268 symbols, 1 missing export | probe `imports_check.py` |
| HUD placements | §8.2 (86 over 14 screens) | probe `layout` |
| Godot API | §4.7 | `engine-api/4.8.dev7.c971f93e7/extension_api.json` |
| movie EBX | `UIAsset` with `Name` | `bf3dump dump Win32/UI ... ui/assets/iconsingame ui/static/fontcollection_en_fontlib` |
| existing readers | `res_type::kSwfMovie`, `LevelMount::read_res`, `BundleManifest` res SHA-1s, `res::load_texture`, `prepare_texture`, `make_texture`, `cached_convert`, `level::WorkBudget`, `TextDatabase` | headers in this tree |

---

## 19. Open questions

For the user / orchestrator:

1. **Font name fallbacks (§7.4).** `$HUD`, `$HUD_light`, `$Menu_light*` and `$FONT_TITLE(*)` have no mapping entry.
   The design maps them by family (`$HUD*` → `$HUD_medium`, `$MENU*` → `$MENU_medium`, `$FONT_TITLE` → `$MENU_bold`).
   Accept, or should they count as unresolved (then G6 fails on 4 names by construction)? (Reviewers: acceptable
   provided the Family names are reported separately, which G6 now does.)
2. **G5's visual judgement** is manual (the implementer inspects the PNGs and writes what they show). The
   automated proxies are G3's round trip and G4's contour / metric rows. Is that acceptable as the gate?
3. **Empty icon frames.** 292 of iconsingame's 427 labels draw nothing at their frame. The design reports them as
   empty regions rather than guessing art for them. Accept?

For the implementation (answered by doing it):

4. Does Godot apply the `kern` table (§7.2, §16)? Yes: "AV" is 3.844 px narrower at 64 px (source 3.841 px; §21).
5. The Godot-side cost of the four FontFiles and the atlases (reported by the check): about 1 ms on the main thread
   after a 0.44 s loader (§21).
6. The meaning of DefineExternalImage2's second u16 (9 on all 135 tags): kept as `field_2`, not interpreted.

---

## 20. Revision 2 (review findings addressed)

| finding | change |
|---|---|
| `swf ttf` wrote a converted EA font to any path (plan §2.1, §13) — blocking | dropped; no API or command writes TrueType bytes outside the Cache (§2, §12.4, §13, §17) |
| G4 `fonts.metrics` would fail by construction: AUTO subpixel → rounded advances above 20 px — blocking | §10 fixes the FontFile settings (hinting none, subpixel one quarter) shared by HUD and check, verified against the dev7 TextServerAdvanced source; the row compares every glyph's advance (± 0.05 px) and the strings (± 0.5 px); kerning ± 0.25 px (§1, §11.1) |
| G1 pointed at §5.3; "first holder" escape clause | §4.2; distinct SHA-1s over all entries marked unverified until the census; a mismatch fails G1 and only the user can accept a fallback |
| G6 counting key ambiguous | distinct names after one trailing `*` dropped; Family names printed separately (§1, §7.4) |
| system fallback contradiction | `allow_system_fallback` true everywhere; coverage rows use the font's own glyph lookups (§7.3, §10, `ui_assets.h`) |
| filter_clip and mip bleeding | regions drawn without mipmap filters; bleeding at mip > 0 stated (§6.4, `ui_assets.h`) |
| T1 / T2 under asan | checked in `dev`, reported only under ASan (§1, §14) |
| unspecified budgets; winding 67M units on one glyph | §9.1 names every budget and its creator; constants in the headers |
| memory constant (130-byte events, 64 MiB body) | `kSwfMaxBodyBytes` 16 MiB, `kSwfMaxDisplayEvents` 2^18, events charged 8 units; census prints `retained_bytes` (§5.1, §5.3, §9) |
| atlas recursion not memoised | per-sprite frame-0 lists memoised, cycles an error, 2^20 pieces per map (§6.1, §9) |
| label at frame == frames | allowed: the state after every event; 0 in retail; unit case (§5.1, §9) |
| sub-image x2 < x1 wraps `width()` | refused at parse (§4.3, §9, `swf_movie.h`) |
| TrueType dates | `head.created` / `modified` 0, bytes deterministic (§7.2, `font_ttf.h`) |
| relative shot dir | `OPENBF3_UI1_SHOTS`, absolute default (§1, §11.2) |
| LevelMount creation from parallel lanes | per-holder guarded creation, failures memoised (§5.2, `swf_library.h`) |
| 1,268 vs gfxfontlib imports; Place comment; `_iN` not decimal; `bf3dump list` count; cache Busy / PathGuard; merge conflicts | clarified (§4.3, §4.6, §5.1, §18, §3, §16) |

---

## 21. Implementation results (2026-10-16)

Measured on this machine: 16 threads shared with other tracks' builds and tests, so every time below is noisy; each
test prints its own values. Commands run from the worktree root after `tools\build.cmd dev`; the Godot tests need
`OPENBF3_GODOT` (the 4.8 dev7 console executable). One command per gate:

| gate | threshold (§1) | measured | met | command |
|---|---|---|---|---|
| G1 | 434 / 434 / 0 errors, 0 multi-SHA-1 names, 0 labels after the last ShowFrame, §4.2 totals exact | 434 movies, 434 parsed, 0 errors (CFX 371, GFX 63); 0 names with two SHA-1s over all manifest entries; 0 late labels; every §4.2 count exact and no other tag code; AS2 1,638,265 bytes, AS3 0; 135 external images, 0 textures missing or of another size; 820 sub-images, 0 outside; 1,268 imported symbols, 1 missing (`deckofcards` → `iconsShared.swf:IconsShared_Check`); fonts 63 / 21 / 2; parse budget max 127,948 units, atlas budget max 578,000, retained max 5.17 MB (sum 31.9 MB) | yes | `ctest --preset dev -R "^core\.ui\.swf$" -V` |
| G2 | §6.2 / §6.3 | `iconsingame_i1.tga` 4096 × 128, DxTexture DXT5 4096 × 128; 162 sub-images, 0 outside; 427 labels on 6 sprites, 135 with art (110 / 25), 292 empty; 95 / 96 / 162; the 10 spot rows exact | yes | `ctest --preset dev -R "^core\.ui\.swf$" -V` |
| G3 | 444 / 444 / 34 / 34, 442 / 442 / 32 / 32, U+00C9, 0 reversed, round trip, kerning 335 / 324 / 0 / 0, cache cold / warm / damaged | all as required; contours 836 / 836 / 30 / 30 (with `.notdef`'s 2); TrueType 47,012 / 44,292 / 2,972 / 3,048 bytes; cold 4 `converted`, warm 4 `hit`, damaged entry `dropped_corrupt` + converted, then 4 `hit` | yes | `ctest --preset dev -R "^core\.ui\.font$" -V` |
| G4 | the §11.1 rows, `UI1 CHECK: PASS (n/n)` | `UI1 CHECK: PASS (25/25)`; ascent 70 px (69.5 ± 1); worst glyph advance 0.0156 px off; shaped "0123456789" 366.500 / 374.000 / 403.125 / 403.125 px vs source 366.519 / 374.034 / 403.222 / 403.222; "OPERATION MÉTRO" 547.031 / 555.531 vs 547.019 / 555.559; "AV" 3.844 px narrower (3.841) in both Purista weights; 0 contour count or orientation differences | yes | `ctest --preset dev -R "^godot\.ui_fonts$" -V` |
| G5 | three PNGs, inspected | written at 1280 × 720 by the windowed check (25/25); findings in README "UI groundwork 1 (ui1)" | yes | `ctest --preset dev -R "^godot\.ui_fonts_shots$" -V` |
| G6 | Exact 5 (see below), CaseFolded 2, Family 4, Device 5, Unresolved 0 | Exact 5, CaseFolded 2, Family 4, Device 5, Unresolved 0; `G6 family-inferred: $FONT_TITLE $HUD $HUD_light $Menu_light`; the other 32 font libraries' 59 names all Exact under their own collections | yes | `ctest --preset dev -R "^core\.ui\.swf$" -V` |
| G7 | 86 placements, 1280 × 720, §8.2 within 0.05 px | 86 (14, 8, 9, 7, 8, 9, 10, 7, 0, 1, 4, 8, 1, 0), every stage 1280 × 720, 31 spot values within 0.05 px; the ammo tree as §8.2 | yes | `ctest --preset dev -R "^core\.ui\.swf$" -V` |
| G8 | asan, no report; hostile cases < 1 s in dev | 6 of 6 pass under ASan (`ui1.contract`, `core.ui.swf.unit` 9.5 s, `core.ui.font.unit` 1.7 s, `core.ui.swf` 4.5 s, `core.ui.swf.fuzz` 24.8 s, `core.ui.font` 5.3 s); dev: slowest hostile case 2^18 + 1 display events ≈ 0.55 s, 2^22 + 1 tags ≈ 0.48 s, the rest under 0.45 s | yes | `ctest --preset asan -R "^core\.ui\.(swf\|font)"` |
| G9 | 256 index files unchanged | clean in `core.ui.swf`, `core.ui.swf.fuzz`, `core.ui.font`, `godot.ui_fonts` | yes | the `install.unchanged` line of each of those tests |
| T1 | ≤ 15 s (dev) | 0.28 s (open 0.25 s + census 0.037 s) | yes | `ctest --preset dev -R "^core\.ui\.swf$" -V` |
| T2 | cold ≤ 1.0 s, warm ≤ 0.25 s (dev) | cold 0.031 s, warm 0.003 s | yes | `ctest --preset dev -R "^core\.ui\.font$" -V` |

What the implementation changed in this design (each where the rule is stated):

* **G6 Exact 5, not 4.** The en font library's own sample edit texts name all four long names; revision 2's §7.4
  listed three and missed `Venice-SemiBold`. The other rules' counts are unchanged.
* **§8.2 values.** `hudtdmscreen` `HudBackgroundWidget_01` is at y 486.95 (revision 2 rounded to 486.9) and both
  `Compass_01`s at x 85.05 with scale 0.99985 (rounded to 85); with the 0.05 px tolerance the rounded values fail.
* **G4 string widths.** `Font.get_string_size()` and `TextServer.shaped_text_get_width()` return the line width
  ceil()ed to whole pixels on dev7 (367.000 px for a 366.519 px line; 404 px for Venice's 10 × 40.3125 px), so the
  0.5 px and 0.25 px tolerances are measured on the sum of the shaped glyph advances (`shaped_text_get_glyphs`),
  which are HarfBuzz's fractional values with the kerning applied. The §10 settings were right: the shaped advances
  are unrounded with `SUBPIXEL_POSITIONING_ONE_QUARTER`.
* **Added APIs** (§12.1): `SwfLibrary::read_atlas_texture_header`, `SwfLibrary::from_memory`, `read_all_font_mappings`,
  the census's library-font check and extra totals, `Bf3UiAssets::movies / hud_placements / atlas_texture`.
* **Finding: Venice's empty glyphs.** Venice-Medium and Venice-SemiBold have outlines only for `#`, `%`, `-`, `.`,
  `/`, `0`-`9` and `:`; their other 16 codes (`!"$&'()*+,;<=>?`) are empty glyphs in BF3's font library, so
  `has_char` is true and system fallback does not replace them. HUD text with those characters belongs in `$MENU_*`.
