# antdump

Godot-free CLI over `bf3_core`'s Ant animation reader (`src/bf3_core/include/bf3/core/ant/`). The reader and the
codecs are a clean-room implementation from `research/probes/ant-animation-probe.md` (decision D14): no code from
Rime, IceBloc or AssetBankPlugin.

## Input: the install (or an export directory)

Every command takes `<input>`:

* `--install` reads the retail install in place through `AntInstallSource` (`bf3/core/ant/ant_install_source.h`),
  located like the game does: `OPENBF3_INSTALL`, else `C:\Program Files\EA Games\Battlefield 3`; `--install=<dir>`
  names it. Opening it reads the bundle manifests of the 53 non-chunk superbundles and the 322 `AntPackageAsset`
  partitions (about 1 s); payloads are read on demand.
* `<export_dir>` reads a directory of exported payloads through `AntExportDirSource` (`ant_export_source.h`, a
  tool-side source; `bf3_core` opens files only through `InstallReader`/`ReadOnlyFile`). It holds BF3 data, so it
  lives outside the repo. It is kept as the parity oracle for the install source.

```
res.tsv        name, file, size, res_meta, superbundle, bundle      one row per AssetBank res (397)
packages.tsv   partition, type, superbundle, bundle, chunk_file, fields   one row per AntPackageAsset (322)
res/*.bin      res payloads          chunk/<guid>.bin   decompressed chunks (88 Chunk packages)
```

The export directory was produced with the research `ebx-dump` probe (`research/probes/tools/ebx-dump`) extended by an
`antexport <out_dir>` command (about 382 MB, 4.6 s).

### Selection rules (the export's, reproduced by `AntInstallSource`)

1. Superbundles in layout order, skipping chunk superbundles (name contains `chunks`) and `Win32/Loc/*`.
2. Per superbundle, every bundle manifest in bundle order. An AssetBank res (type `0x51A3C853`) or an EBX partition
   whose name starts with `animations/antanimations/` is taken from the **first superbundle that lists it**, and
   within it from the first bundle that lists it. So `s_basicassets` comes from `Win32/Globals`, most other banks
   from the first SP/COOP/MP level that carries them (e.g. `m9` and `b_basicassetsmp` from `COOP_002`).
3. The bundles a superbundle supplies are loaded in sorted name order (the export's `BTreeSet`), so a res read
   resolves to the same bundle entry as the export's.
4. A partition is taken where it reads and parses, else the next superbundle is tried; only primary instances of
   type `AntPackageAsset` become packages (`Name`, `PackagingType`, `StreamingGuid`, `ChunkSize`).
5. Static and Bundle packages are the res `lowercase(Name)`. A Chunk package is the chunk `StreamingGuid`, read
   through the mount of its partition's superbundle, and when that fails through `Win32/Weapons` with every bundle
   loaded (the export's fallback; no package needs it on the retail install).
6. Banks without an `AntPackageAsset` (the 163 animation-set banks) are listed by `all_banks()` with
   `is_asset_package = false`.

One difference, documented rather than emulated: the export took a res only after its payload read succeeded (a
failed read let a later superbundle supply it); `AntInstallSource` takes the first superbundle that *lists* it and
reports a read failure from `read()`. On the retail install no read fails (parity below).

## Commands

```
antdump census  <input> [--mp]           codec census, DCT checks, channel mapping (probe §2, §4.3, §5)
antdump list    <input> <package>        clips of a package: codec, keys, controller, target, rig
antdump clip    <input> <package> <clip> [--digits N] [--out f.json]   clip JSON (the probe's shape)
antdump compare <input> <oracle.json>... dump the oracle's clip and compare value by value (1e-5)
antdump bench   <input> [--mp] [--repeat N]   parse/decode/sample throughput (single thread)
antdump sample  <input> <package> <clip> <frame>  one interpolated pose
antdump rigmap  <input> <package> <clip> [--skeleton <ebx SkeletonAsset partition>]...
                                         timing, channel -> DOF -> rig joint table; with the install, each
                                         joint's bone index in the named EBX skeletons (EbxLibrary over @all-mp)
antdump sources <input>                  every package and bank with its provenance (tsv)
antdump parity  <export_dir> --install[=<dir>] [--rows-only]   install source vs export: rows and payload bytes
<input> = <export_dir> | --install | --install=<dir>
```

`--mp` restricts to the probe's MP-relevant set (every package except `b_sp*`, `b_coop*`, `b_faceposer*`, `b_vo*`).
The clip JSON has `source`, `fps`, `fpsScale`, `numTicks`, `endFrame`, `keyTimes`, `joints` (rig rest pose) and
`tracks` (one per joint channel, values per key) like the probe's files, plus `floatTracks` for named float DOFs.
A command exits 77 when its input does not exist (no install found / no `res.tsv`), like `bf3dump`.

## Parity: install = export = probe (2026-10-14, patch v7 + Xpack1-5)

| check | result |
|---|---|
| `parity <export> --install` | packages 322 = 322 (every field and provenance equal), banks 397 = 397, payloads 485 of 485 byte-identical (397 res + 88 chunks, 398.9 MB) |
| `census --install` vs `census <export>` | identical output (timings aside): 321 non-static packages + `s_basicassets` (62,766 objects, 261 layouts), 0 failures; DCT 3,778/3,778, 0 overruns, 9 clips with mean \|q\|-1 > 2e-3 (worst 0.00745); Chunk packages 88, length = `ChunkSize` 88 |
| `census --install --mp` vs `census <export> --mp` | identical: 125 packages, DCT 3,298, Raw 1,490, Frame 659, VBR 146, Curve 35; 5,592 clips with a controller, 684,459/684,459 channels on a DOF of their kind |
| probe `census.txt` / `census-mp.txt` | same codec counts, DCT histograms (0:182/1:3,596 and 0:156/1:3,142) and 37,467 / 22,863 KiB of DCT bitstream |
| `compare --install` on the probe's 5 clip JSONs | PASS: max \|diff\| 5.1e-6 on tracks, 5.0e-6 on joints (the JSONs carry 5 decimals) |

## Tests

`ctest -R core.ant` runs `core.ant.unit` (synthetic archives and DCT streams, no data) and `core.ant`.
`core.ant` reads the install, or the export directory named by `OPENBF3_ANT_DATA` (when that names none, the
install). It exits 77 (skipped) only when neither is available. With `OPENBF3_ANT_ORACLE` = the probe's output folder
it also compares the clip JSONs; with both `OPENBF3_ANT_DATA` and an install it checks that the install source
selects the same packages and banks as the export. A missing optional input prints `NOT RUN:` and does not skip
the suite.

## M9 1P empty reload (for the Godot wave)

`antdump rigmap --install m9 "ReloadEmpty Anim" --skeleton animations/skeletons/venice1pske01 --skeleton
animations/skeletons/weapon/weaponske01` prints everything below; the `core.ant` case "M9 1P ReloadEmpty decodes
and binds to the 1P rig" asserts it from the install.

**Where it is.** Package `m9`: `AntPackageAsset` `animations/antanimations/m9`, packaging Chunk, chunk of 51,948
bytes (= `ChunkSize`), 11 objects. Its clips: `ReloadEmpty Anim`, `ReloadFull Anim`, `Fire Anim`, `Deploy Anim`,
`Undeploy Anim` (DCT); `IdlePose Anim`, `ZoomPose Anim`, `SprintPose Anim` (Frame); `zoomFire Anim`,
`zoomInOut Anim`, `ZoomOut Anim` (VBR, not decoded). The controllers, DOF sets and the rig live in `s_basicassets`.

**The clip.** `ReloadEmpty Anim`, `DctAnimationAsset`, not cyclic, not additive. 57 keys at frames 0..50 then
52, 55, 57, 60, 62, 65 (sparse: interpolate between keys, `sample_clip`). `EndFrame` 65. Controller
`ReloadEmpty`: `FPS` 30, `FPSScale` 0.5, `NumTicks` 130 (= 2 x EndFrame). Play frame `f = t * 30` for
`t` in [0, 65/30 = 2.167 s] (probe §5 timing, inferred). The last key returns to the first within 0.29 deg.

**Rig.** Target `1P_Upperbody_DOF` (146 DOFs), rig `Venice1pAntSke01`, Ant skeleton `Venice1pAntSke01_New`, 181
joints; every joint's parent comes before it, so the joint list can be added to a `Skeleton3D` in order. Rest pose
= the rig defaults (`AntJoint::rest_t` / `rest_q`, from `DefaultVector3Values` / `DefaultVector4Values`
`"<Layout>.<Joint>.t/.q"`), local to the parent, metres, Y up, quaternions xyzw.

**Channels.** 135 = 96 rotations then 39 translations, no floats; every one lands on a rig joint, on a DOF of its
own kind, none on the delta trajectory; 96 joints are driven, the other 85 keep the rest pose.

| channels | kind | joints |
|---|---|---|
| 0-2 | rotation | `Neck`, `Neck1`, `Head` |
| 3-26 | rotation | weapon: `Wep_Root`, `IK_Joint_LeftHand`, `IK_Joint_RightHand`, `Wep_Extra1`, `Wep_Trigger`, `Wep_Slide`, `Wep_Grenade1/2`, `Wep_Mag`, `Wep_Mag_Ammo`, `Wep_Physic1-3`, `Wep_Belt1-5`, `Wep_Bipod1-3`, `Wep_Extra2/3`, `Wep_Aim` |
| 27-28 | rotation | `CameraJoint`, `CameraBase` |
| 29-31 | rotation | `SpineX`, `SpineXLeft`, `SpineXRight` |
| 32-63 | rotation | left arm `LeftShoulder`, `LeftArm`, `LeftArmRoll`, `LeftForeArm`, `LeftForeArmRoll`, `LeftHand`; fingers `LeftHand{Middle,Ring,Pinky,Index}0-4`, `LeftHandThumb1-4`; `LeftForeArmRoll1`, `LeftElbowRoll` |
| 64-95 | rotation | the same 32 joints on the right side |
| 96-119 | translation | the 24 weapon joints of channels 3-26, same order |
| 120-121 | translation | `CameraJoint`, `CameraBase` |
| 122-124 | translation | `SpineX`, `SpineXLeft`, `SpineXRight` |
| 125-128 | translation | `LeftHandMiddle0`, `LeftHandRing0`, `LeftHandPinky0`, `LeftHandIndex0` |
| 129 | translation | `LeftShoulder` |
| 130-133 | translation | `RightHandMiddle0`, `RightHandRing0`, `RightHandPinky0`, `RightHandIndex0` |
| 134 | translation | `RightShoulder` |

`ClipBinding::tracks[c]` gives channel `c`'s rig joint index; `rig->joints[j].name` its name.

**Against the mesh skeletons (EBX).** The 1P arms mesh skins to EBX `animations/skeletons/venice1pske01` (179 bones)
and the M9 mesh to `animations/skeletons/weapon/weaponske01` (25 bones), by bone name (probe §7).
- `venice1pske01`: all 96 driven joints have a bone of the same name; 179 of the 181 rig joints do (the two
  without are `LeftAnkleEffectorAux`, `RightAnkleEffectorAux`). Same parent name for 95 of 96; `Wep_Aim` hangs under
  `SpineX` in the rig and under `Wep_Root` in the EBX skeleton.
- `weaponske01`: 24 of its 25 bones are driven rig joints (all `Wep_*` plus the two `IK_Joint_*Hand`); its root
  `Wep_Root` has parent `exportAnimation` there and `SpineX` in the rig.
- The indices differ (rig `Wep_Root` = 154, `venice1pske01` = 153, `weaponske01` = 1): map by **name**, never by
  index.

**Recipe.** Build one `Skeleton3D` from the rig's 181 joints (names, parents, rig rest pose). Each frame, sample the
decoded clip at `t * 30` and set `set_bone_pose_rotation` / `set_bone_pose_position` on `tracks[c].joint` for
the 96 rotation and 39 translation channels. Skin the arms and the M9 meshes to that skeleton by bone name, with
inverse binds from the EBX `ModelPose` of the mesh's own skeleton. Put the 1P camera on `CameraJoint` (rotated
180 deg about Y: Frostbite looks down +Z). Roll and twist joints driven by rig ops are not computed (probe §8).
