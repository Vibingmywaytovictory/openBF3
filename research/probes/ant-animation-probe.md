# Probe: BF3 Ant animations decoded from the unmodified install

> **Repo note (2026-10-14):** the probe's outputs (`research\probes\ant\`: decoded clips, logs, renders) are BF3-derived
> data, and its tool (`research\probes\tools\ant-probe\`) contains a DCT decoder written from Rime's unlicensed
> `Decompressor.cs`. Both were removed from the git repo and its history (decision D14). Local copies are in
> `C:\Users\olku\BF3 reimplementation\research-data\probes\` (outside the repo). The clean-room C++ port must be written
> from the format and DCT specification in this report, not from that tool's code.

Date: 2026-10-10. Probe source: `research\probes\tools\ant-probe\`; it is C# against a scratch clone of Rime, and
its viewer is GDScript. Outputs: `research\probes\ant\`, with shots in `ant\shots\`.

Tags: **[V]** I verified it in data, code or a primary source I ran or read. **[R]** a source states it and I did
not re-check it. **[I]** my inference. All counts come from the user's retail install, opened read-only.

## 0. Verdict

**It works.** BF3's first-person weapon animations and its third-person soldier animations decode from the install
as is, without changes, and play correctly on BF3's own skinned meshes in Godot 4.8 dev7:

- M9 reload (empty): the slide stays locked back, the magazine drops, the left hand inserts a new one and the slide releases. See `shots\m9_1p_ReloadEmpty_*.jpg`.
- M9 fire shows the recoil kick.
- Third-person rifle run cycle, and a third-person M16 reload with the magazine swap. See `shots\soldier_3p_*.jpg`.

This covers three of the five clip codecs. They make up 95 % of the clips in multiplayer-relevant packages:

| codec | clips in MP-relevant packages | status |
|---|---|---|
| `DctAnimationAsset` | 3,298 | decodes bit-exactly |
| `RawAnimationAsset` | 1,490 | decodes (raw floats) |
| `FrameAnimationAsset` | 659 | decodes (raw floats) |
| `VbrAnimationAsset` | 146 | only its constant channels decode; mostly pistol and launcher zoom clips |
| `CurveAnimationAsset` | 35 | not attempted |

Every channel of all 5,592 MP clips that have a controller maps onto the correct skeleton DOF:
684,459 of 684,459 channels [V].

**Recommendation for M7:** port the reader and the DCT/Raw/Frame decoders to C++ (sizing in §9). Do not author
replacement animations. VBR is the only gap that touches MP weapons. Close it later, or cover aim-down-sights with
the `ZoomPose`/`IdlePose` Frame clips that every weapon package also carries (§6.3).

## 1. What was run

1. Rime (`C:\Users\olku\Rime`, commit b3145da5) was `git clone`d into scratch and built with `dotnet build`
   (.NET SDK 9.0.318 / 10.0.401, net8.0 target). The build gave 0 errors. **No source change to Rime was needed** [V].
   The empty `External/GameTools.Net` submodule only matters to the GUI app.
2. A console probe (`AntProbe`) references `RimeLib.Content.Frostbite2_0`, `RimeLib.Animation.Frostbite2_0` and
   `RimeLib.Serialization.Frostbite2_0`.
   - It mounts the whole install: 41,278 res, 73,371 EBX and 61,101 chunks in about 30 s [V].
   - Rime's mounter opens every file with `FileAccess.Read` [V]: `rime\RimeLib.Content.Frostbite2_0\Mounting\EngineMounter.cs:411,673,712,727,868`, `Frostbite\Cas\Catalog.cs:41,136`.
3. AssetBanks are parsed with Rime's `AssetBank.Load(reader, bank)`
   (`RimeLib.Animation.Frostbite2_0\Frostbite\AssetBank.cs:43-61`). Do not use the `GeneralDataReader` variant: it
   still reads container pointers as u32 (`EA\GeneralDataReader.cs:137,209,356`), and `AssetBank.Load(reader)` throws
   `NotImplementedException` (`AssetBank.cs:524-527`) [V].
4. DCT is decoded by my own parameterised decoder, `tools\ant-probe\DctDecoder.cs`. It started from Rime's
   `EA\Compression\DCT\Decompressor.cs`, which only prints CSV and has no tests. I ran every open choice as a hypothesis
   against the data (§4.4).
5. A minimal skinned-MeshSet reader (`MeshExport.cs`) uses the handoff layout from `sections\02-resources.md` §2. Clip,
   skeleton and mesh are written as JSON, then a throwaway GDScript scene renders them in Godot 4.8 dev7 (D3D12, RX 9070 XT).

## 2. Where the animation data lives [V] (`ant\res-anim.tsv`, `ant\ebx-anim.tsv`)

| item | count / size |
|---|---|
| `AssetBank` res (`0x51A3C853`) | 397 res, 390.6 MB. 386 are under `animations/antanimations/` (371.9 MB, most of it SP cutscenes) |
| `AnimTrackData` res (`0xD070EED1`) | 1,485 res, 10.4 MB (EBX timeline tracks, not Ant) |
| `RagdollResource` (`0x319D8CD0`) | 1 res (41,744 B) |
| `AntPackageAsset` EBX (`animations/antanimations/<pkg>`) | 322: 1 `AntPackagingType_Static`, 233 `_Bundle`, 88 `_Chunk` |
| `AntProjectAsset` | 1 (`animations/antanimations`) |
| `AntAnimationSetAsset` / `SkeletonAsset` / `MasterSkeletonAsset` EBX | 152 / 135 / 4 |

How a package's data is found:

- `AntPackageAsset.PackagingType` selects the source.
  - Static and Bundle packages are a **res** named `lowercase(AntPackageAsset.Name)`.
  - Chunk packages are the **chunk** `AntPackageAsset.StreamingGuid`. Its length equals `ChunkSize` for all 88.
- The packages that matter:
  - **`s_BasicAssets`** (Static, 22,481,708 B, bundle `win32/gameconfigurations/game`). It holds the whole graph: 9,003
    `ClipControllerAsset`, 5,729 `ActorControllerAsset`, the skeletons, rigs and layouts, and 228 DCT clips.
  - **`b_BasicAssetsMP`** (Bundle, 15,929,912 B, in 194 MP level/mode bundles). It holds the 3P soldier clips:
    1,250 DCT, 981 Raw, 168 Frame and 6 VBR clips, plus 30 clip controllers.
  - **One Chunk package per weapon or gadget**: `m9` 51,948 B, `ak74` 183,068 B, `m16a2` 179,792 B … 18–244 KB, 8.26 MB
    for all 88, superbundle `win32/weapons`. These hold only that weapon's clips. For example `m9` has 5 DCT clips
    (Fire, Deploy, Undeploy, ReloadFull, ReloadEmpty), 3 VBR clips (zoomFire, zoomInOut, ZoomOut) and 3 Frame poses
    (IdlePose, ZoomPose, SprintPose). Their `ClipControllerAsset`s live in `s_BasicAssets`.
  - Vehicle seat animations are Bundle packages such as `b_Vehicle_ah-1z` and `b_Vehicle_humvee`, plus
    `b_Shared1P`, `b_1PLegs_SP` and others.
- All 321 packages besides `s_BasicAssets` parse with zero errors in 11.7 s in total, and `s_BasicAssets` itself
  parses in 1.2–1.4 s (62,766 objects) [V] (`ant\census.txt`, `ant\bank-log.txt`).

## 3. Container format (as read; offsets from Rime's readers, confirmed by successful parses)

### 3.1 Package header [V]

- `u32 BE packagingType` (0 Static, 1 Chunk, 2 Bundle, 3 AnimationSet).
- Unless the type is AnimationSet, a `PackageMeta` follows: `u32 BE metaBytes, packageType, assetCount, importCount, assetBytes, extraBytes`.
- When `extraBytes ≠ 0` (Static only), these follow: `totalNonStaticAssetCount, maxNonStaticAssetCount, maxNonStaticImportCount, staticBundleImportBucketSize, staticBundleImportBucketEntries, staticBundleFlatImportCount, staticBundleImportCountInSlots, staticGuidToIntSize`, then a GUID→int map and a static import hash table of `{u32 BE key, u32 LE ptr, i32 LE tid}` entries.
- Source: `RimeLib.Animation.Frostbite2_0\Frostbite\PackageMeta.cs:57-131`.
- Observed first bytes:
  - `m9`: `00000001 00000018 00000002 0000000B`, i.e. Chunk, meta 0x18 bytes, 11 assets.
  - `b_BasicAssetsMP`: `00000002 00000018 00000002 0000098C`.
  - `s_BasicAssets`: `00000000 0001A434 …`, i.e. 107,572 bytes of meta.

### 3.2 EA GenericData archive [V]

- RIFF-like blobs: `char[7] id` + `char endian` + `u32 size` (size includes the 12-byte header).
  - Ids: `GD.STRM`, `GD.REFL`, `GD.DATA`; `REFPACK`, `DAT2`, `REF2`, `INFO` are known but unused.
  - **On BF3 PC the endian byte is `'l'` (little-endian)** for the stream and reflection blobs of `m9` and `b_BasicAssetsMP`.
  - Only the package header above is big-endian. This corrects the handoff's "big-endian archive" (sections/02 §10).
- Layout of a `GD.STRM`: `u32 sizeOfLargestBlock`, then one `GD.REFL` blob, then N `GD.DATA` blobs (one object each).
  Source: `RimeLib.Animation\EA\GenericData\Archive.cs`, `Blob.cs`.
- `GD.REFL` is the self-describing type schema: `u64 count` + RelocPtr per `LayoutHeader`.
  - LayoutHeader: `i32 minSlot, maxSlot; u32 dataSize, alignment, stringTableOffset, stringTableSize; bool recorded, native; pad2; u32 hash`, then one 32-byte `EntryHeader` per slot.
  - EntryHeader: `u32 layoutHash, elementSize, offset, nameOffset; u16 count, flags(1 Array, 2/4/8 ForceAlign); u16 elementAlign; i16 rle; u64 layoutOffset`.
  - Then the string table.
  - Slot −3 = `__guid` (16 B), −2 = `__name` (string), −1 = `__base` (DataRef to the base-class part).
  - Primitive layout hashes 1–19: Bool … Matrix44, Guid=16, String=17, DataRef=18.
  - Source: `RimeLib.Animation\EA\Reflection\LayoutHeader.cs`, `EntryHeader.cs`, `LayoutType.cs`.
- Each archive carries its own schema: `s_BasicAssets` has 261 layouts, `b_BasicAssetsMP` 22, `m9` 13.
  The dump of all 261 is `ant\layouts-s_basicassets.txt` [V].
- `GD.DATA` object header (`EA::GD::LayoutData`): `u32 magic 0x030E6205, pad4, u64 allocator, u32 layoutHash, pad4, i32 refCount, u16 dataOffset, bool mutable`.
  Fields sit at `dataOffset + slot.offset` (aligned to `elementAlign`).
  - Strings and arrays are `{u32 capacity, u32 count, u64 ptr}`, with ptr relative to the blob.
  - DataRef is a u64 blob offset to another `LayoutData`.
  - Source: `Data.cs`; `AssetBank.cs:115-285`.
- **Cross-object references are 16-byte `Guid` fields.**
  - The first 4 bytes are a big-endian u32 reference index.
  - Bit `0x80000` set means a static reference into `s_BasicAssets`; otherwise the target is in the same bundle or chunk package.
  - Source: `RimeLib.Animation\EA\Types\AntGuid.cs`; `Frostbite\Resolver\LocalAssetResolver.cs`.
  - Every `ChannelToDofAsset` reference resolved, 0 unresolved [V] (`ant\census-mp.txt`).
  - Pitfall: Rime's process-wide `AssetResolver` keys bundle objects by index only. Load each bundle package with its
    own resolver plus the static objects, or indices from different packages collide (`MapCheck.cs` does this).

### 3.3 Object types that matter (reflection as dumped; field ids and names exact) [V]

| type (layout hash) | fields | role |
|---|---|---|
| `AnimationAsset` (0xCEAFE7C0) base | `CodecType u32`, `AnimId u32`, `TrimOffset f32`, `EndFrame u16`, `Additive bool`, `ChannelToDofAsset Guid` | common clip header. `CodecType` FourCC: DCT 0x44435420 "DCT ", VBR 0x56425220 "VBR ", Frame 0x4652414D "FRAM" |
| `DctAnimationAsset` (0xB5A06C02) | `NumKeys, NumQuats, NumVec3, NumFloat, NumFloatVec u16; Cycle bool; QuantizeMultBlock u16; KeyTimes u16[]; QuantizeMultSubblock u8; CatchAllBitCount u8; DofTableDescBytes u8[]; DeltaBaseX/Y/Z/W i16[]; BitsPerSubblock u16[]; DataSize u32; Data u8[]` | DCT codec (§4) |
| `RawAnimationAsset` (0x5497013C) | `KeyTimes i16[]; Data f32[]; FloatCount, Vec3Count, QuatCount, NumKeys u32; Cycle bool` | uncompressed. `Data.Count == NumKeys·(4·Quat + 4·Vec3 + Float)` for 1,490/1,490 MP clips (vec3 padded to 4) |
| `FrameAnimationAsset` (0x1D6C4F71) | `Data f32[]; FloatCount, Vec3Count, QuatCount u32` | single pose; `Data.Count == 4·Quat + 4·Vec3 + Float` for 659/659 |
| `VbrAnimationAsset` (0xF997A7A3) | `QuatMin, TrajMin, Vec3Min, FloatMin, QuatMax, TrajMax, Vec3Max, FloatMax, VectorOffsetScale, FloatOffsetScale, Dct f32; QuaternionCount, Vector3Count, FloatCount, ConstQuaternionCount, ConstVector3Count, ConstFloatCount, KeyTimeSize, NumKeys, ConstChanMapSize, ConstPaletteSize u16; …ConstantPalette f32[], FrameBlockSizes u16[], Data u8[], Flags` | §6 |
| `ChannelToDofAsset` (0xC9DEDB8F) | `StorageType u32; IndexData u8[]` | channel → DOF index (§5) |
| `ClipControllerAsset` (0xFC431BE4) | `Target Guid→LayoutHierarchyAsset; Anim Guid→AnimationAsset; NumTicks, FPS, FPSScale f32; DeltaTrajectory u8; Distance f32`. Slot 2 is an empty `Invalid` slot | plays one clip on one DOF set |
| `LayoutHierarchyAsset` (0x08DD313B) | `LayoutAssets Guid[]; Children Guid[]` | DOF set = its `LayoutAssets` in order |
| `LayoutAsset` (0x0FE4702A) | `NoReorder bool; MinAlignment u32; Slots LayoutEntry[] {Name string, Type u32}` | slot names `<Joint>.q`, `.t`, `.s` or a named float. Type 14 = quaternion, 10 float, 6 int, 0x7A2E5A97 = vec3 translation, 0x7A2E59C6 = vec3 scale |
| `SkeletonAsset` (Ant, 0x931FE7FB) | `Joints JointInfo[] {JointName, ParentIndex i32, JointType i32}; TrajJointIndex i32` | joint names and hierarchy, no pose |
| `RigAsset` (0x95C07894) | `FeatureCollection, Skeleton, DofSetLists Guid[], DefaultFloatValues, DefaultIntValues, DefaultVector3Values, DefaultVector4Values {DofName, Value}[], DeltaTrajDofSet` | **rest pose as DOF defaults** (`"<Layout>.<Joint>.q/.t/.s"`) |

The rigs in `s_BasicAssets` are `Venice1pAntSke01` (skeleton `Venice1pAntSke01_New`, 181 joints), `VeniceAntSke01` (skeleton
`RIGWep`, 215 joints), `ParachuteSke01`, `CanopyParachute` and `VehicleParachute` [V] (`ant\explore-m9.txt`).

## 4. The DCT codec, verified bit-exact

### 4.1 Layout [V]

`dofCount = NumQuats + NumVec3 + NumFloatVec`, in that order: all quaternions, then all vec3s, then the floats packed
4 per `FloatVec`. For each DOF:

- `subblockCount = DofTableDescBytes[d] >> 4`, in 0..8. The low nibble is always 0 in what I saw; its meaning is unknown.
- `DeltaBase{X,Y,Z,W}[d]` is i16.
- `subblockCount` entries of `BitsPerSubblock` (u16, consumed in order) give 4-bit widths for X|Y|Z|W, from nibble 3 down to nibble 0. A width of 15 means `CatchAllBitCount`.

`Data` is the bitstream:

- Bits are read MSB-first, byte by byte, with no 64-bit word swap. Each value is two's-complement in its width; width 0 means value 0.
- Clips are coded in blocks of 8 keys, `ceil(NumKeys/8)` blocks. Within a block, for each DOF in order, it holds coefficients `j = 0..subblockCount-1`, each as 4 components X, Y, Z, W.
- **In block 0 the DC coefficient (`j = 0`) is not stored.**
- In every block, `C0 += DeltaBase`. It is relative to the base, not to the previous block.
- Coefficients beyond `subblockCount` are 0.

Reconstruction of key `k = 8b + n`:

`value = Σ_{i=0..7} C_i · c_i · cos((2n+1)·i·π/16) · (1 + 0.1·QuantizeMultSubblock·i) / QuantizeMultBlock`, with
`c_0 = 1/4` and `c_i = 1/2`.

With `QuantizeMultBlock = 4096` a DC value of 16384 gives 1.0, so quaternions come out unit length and vec3s come out in
metres. Normalise quaternions afterwards. Implementation: `tools\ant-probe\DctDecoder.cs:17-44, 64-80, 98-133`. The basis
equals Rime's table (`Decompressor.cs:16-25`) and IceBloc's (§10).

`DataSize = 12 + dofCount + 8·dofCount + 2·Σsubblocks + len(Data) (+1 pad)`. This holds for every DCT clip
(histogram 0:182, 1:3,596) [V], so `DataSize` is the size of the original serialized stream: a 12-byte header, the
desc bytes, the 4×i16 delta bases, the bit widths and the bitstream.

`KeyTimes` holds frame numbers and can be sparse. For example M9 `Undeploy` has 9 keys at frames 0,2,3,4,5,6,7,9,10
for `EndFrame` 10. Interpolate between keys.

### 4.2 Hypothesis test (`ant\validate-m9.txt`) [V]

Eight variants were tested on the 5 M9 DCT clips: two bit orders × two DC rules × whether block 0 skips the DC.
Only "MSB-first bytes / base-relative DC / skip DC in block 0" passes all of these:

| test | result |
|---|---|
| bits consumed = bits predicted from the tables | e.g. ReloadFull 68,466 = 68,466 of 68,608; Fire 7,327 = 7,327 of 7,552 |
| variants that don't skip DC in block 0 | overrun the data |
| quaternion length before normalisation | mean \|q\|−1 = 3.2–4.7·10⁻⁵, against 0.002–0.06 for wrong variants |
| Undeploy frame 0 vs the uncompressed `IdlePose` Frame clip | max 0.07° per quaternion and 0.4 mm per translation |
| ReloadFull / ReloadEmpty frame 0 vs IdlePose | 0.09° / 0.06° |
| trajectories | smooth, \|q\| stays 1.000 ± 0.0003 per frame. Finger joints rotate about one axis only (`LeftHandIndex2.q` has x = y = 0.0000 in all 50 keys). Reload clips return to frame 0's pose within 0.08° (`Inspect.cs` output, ReloadFull) |

### 4.3 Whole-install run (`ant\census.txt`) [V]

All 3,778 DCT clips in all 322 packages (37.5 MB of bitstream) decode with 0 errors and 0 bit-count mismatches. Total
decode time is 1.7 s in C#, single-threaded.

9 clips have a mean \|q\|−1 between 2.2·10⁻³ and 7.5·10⁻³: 6 brutal-melee clips, 1 vehicle clip and 2 SP clips. All of
them have a low `QuantizeMultBlock` (18–83, i.e. coarse quantisation), so this is precision rather than a decode error
[I]. They are fine after normalisation.

## 5. Channel → DOF → joint mapping, verified on every MP clip

1. **The DOF set of a clip is its `ClipControllerAsset.Target` hierarchy's `LayoutAssets`, in order, with each
   layout's `Slots` in stored order.**
   - `Children` hierarchies repeat subsets of the parent's layouts. They are masks and are not appended [V].
2. **A `DeltaTrajLayoutAsset` occupies 8 DOFs even though it has no slots in the data.** Its base `LayoutAsset` part
   is null.
   - Found by fitting `Rifle_RunFwd` [V]. It is the root-motion delta; its 8 DOFs carried no channel in any MP clip [V].
   - The slot meaning of those 8 DOFs is not known.
3. **`ChannelToDofAsset.StorageType`: 1 = one u8 per channel, 2 = one u16 big-endian per channel.**
   - Used by the large AI/face hierarchies with more than 255 DOFs [V].
   - Channel `c` drives `dof[IndexData[c]]`. The channel order follows the codec: quaternions, then vec3s, then floats.
   - Implementation: `Layouts.cs:10-12, 43`.
4. **Result: 5,592 of 5,592 clips with a controller in MP-relevant packages land every channel on a slot of their own
   kind: 684,459 of 684,459 channels [V]** (`ant\mapcheck-delta8.txt`).
   - Per codec: DCT 3,297, Raw 1,490, Frame 659, VBR 146.
   - Without rule 2 only 2,923 clips pass (`ant\mapcheck-delta0.txt`). The u16 rule fixed the last 108.
   - Kind-consistency is a strong check but not a proof of exact slot identity. The renders (§8) confirm the joints visually.
5. **The joint for a slot `<Joint>.q|.t|.s` is the Ant `SkeletonAsset` joint of that name.**
   - Rest pose: the rig's `DefaultVector4Values`/`DefaultVector3Values` entry for `<Layout>.<Joint>.q/.t`.
   - Compared with EBX `SkeletonAsset.LocalPose` (`animations/skeletons/venice1pske01`, 179 bones):

     | | rest rotation within 0.5° | rest translation within 1 mm |
     |---|---|---|
     | 1P (179 bones) | 168 | 167 |
     | 3P, `veniceantske01` (213 bones) | 207 | 211 |

   - The remaining joints are root-level joints, where the Ant rig re-parents to `AITrajectory`/`SpineX` with a 90°
     offset, plus a few weapon helpers [V] (`ant\skel-*.txt`).
   - Use the rig defaults for animation and the EBX `ModelPose` for mesh binding (§7).

**Timing.**

- `ClipControllerAsset.FPS` is 30 for these clips, `FPSScale` is 0.5 and `NumTicks = 2·EndFrame`. So the clip plays at
  30 key-frames per second, i.e. `EndFrame / 30` seconds [I].
- Cross-check: M9 `ReloadEmpty` is 65 frames = 2.17 s, and `FiringFunctionData.FireLogic.ReloadTime` is 1.6 s with
  `ReloadThreshold` 0.75. 2.17 s × 0.75 = 1.63 s, so the threshold is plausibly the fraction of the clip at which the
  magazine counts as refilled [I] ([V] for the values, `ReloadCheck.cs`).
- The M16A4's `ReloadTime` is 2.37/2.3 s.

## 6. What does not decode yet

### 6.1 VBR

Partly understood [V for the constant part] (`ant\vbr-m9.txt`).

What is known:

- Channels are split into animated and constant channels:
  - `QuaternionCount + ConstQuaternionCount = 96` and `Vector3Count + ConstVector3Count = 39` for M9, matching the DCT clips.
- **The constant channels are byte indices into `ConstantPalette`.**
  - They are at the start of `Data`: 4 bytes per constant quaternion, then 3 per constant vec3.
  - The palette is normalised: `value = Min + palette[i]·(Max − Min)`, using QuatMin/Max or Vec3Min/Max.
  - Check: 75/75, 79/79 and 81/81 constant quaternions decode to unit quaternions within 0.00° of the IdlePose pose;
    33/34 and 34/35 vec3s are within 5 mm.
- **Then come `ConstChanMapSize` bytes of run lengths.** They alternate animated and constant runs, starting with
  animated; a first run may be 0. They sum to 135 = all channels in both M9 clips checked.
  - zoomFire: `01 02 01 16 02 01 01 02 06 18 08 18 03 04 01 11 01 09 01 04 01`. The animated quaternion runs sum to 21 = `QuaternionCount`.

What is not known:

- The rest: 1,174 bytes in zoomFire, of which 778 are `FrameBlockSizes` (2 blocks for 16 keys).
- It looks like per-channel nibble bit widths followed by variable-bit-rate frame blocks. The float `Dct ≈ 4.0` hints at
  a DCT inside each block. Not decoded.

Where VBR is used:

| place | VBR clips |
|---|---|
| `s_BasicAssets` | 27 |
| `b_SharedAISoldier` | 15 |
| `b_Shared_COOP` | 8 |
| `b_BasicAssetsMP` | 6 |
| vehicle bundles | 4 each |
| pistol/launcher chunks (m9, m93r, m1911, mp443grach, javelin, …) | 3 each |

By clip name: ZoomOut 18, zoomFire 18, ZoomIn 16, zoomInOut 12, ProneToStand 6, ZoomStandToProne 4 [V] (`ant\census-mp.txt`).

### 6.2 Curve

`CurveAnimationAsset` (35 MP clips, e.g. `ClimbLadderUpConnect`) was not attempted. Its sub-structs are
`CurveAnimationAsset::ChannelGroup` and `::KeysBlock` (Rime `ant\CurveAnimationAsset__*.cs`) [R].

### 6.3 Workaround for VBR

Each weapon package also has uncompressed `ZoomPose Anim` and `IdlePose Anim` Frame clips [V for m9]. Blending
`IdlePose` → `ZoomPose` over the zoom time is a faithful-looking stand-in for the zoom-in/out clips until VBR is
decoded [I].

## 7. Skinned mesh binding, now verified (closes a handoff unknown)

- The skinned MeshSet LOD bone table (`0x94` count, `0x98` u32 skeleton indices, `0xA0` u32 hashes) has
  **`hash = hashQuick(lowercase(boneName))`**, with `h = 5381; h = h*33 ^ c`.
  - Arms `arms1p_rus_mesh`: 70/70 bones, with indices into EBX `venice1pske01`.
  - `m9_1p_mesh`: 5/5 bones (Wep_Root, Wep_Trigger, Wep_Slide, Wep_Mag, Wep_Physic1), with indices into EBX `animations/skeletons/weapon/weaponske01` (25 bones).
  - Case-sensitive hashQuick, FNV-1 and FNV-1a all give 0 matches [V] (`ant\meshbones.txt`).
- **A subset's u16 bone slots (subset `0x28`) hold the skeleton bone index directly**, not an index into the LOD bone list.
  - Values reach 147–203, above the LOD bone count of 70 [V].
  - Map slot → LOD entry with that skeleton index → name hash → Ant joint.
  - Result: 0 vertex weights left unresolved on 5 meshes (`MeshExport.cs:23-45`).
- **Binding:** inverse bind = inverse of the EBX `SkeletonAsset.ModelPose[bone]` of the mesh's own skeleton.
  - That is `venice1pske01` for 1P arms, `veniceantske01` for 3P bodies and `weaponske01` for weapons.
  - Weapon parts are joints of the character rig (`Wep_*` sit inside `Venice1pAntSke01_New`/`RIGWep`), so the weapon
    mesh skins to the same `Skeleton3D` by name [V by render].
- Godot needs the triangle winding reversed; the renders used CCW → CW [V].

## 8. Visual check in Godot 4.8 dev7 [V]

`tools\ant-probe\godot-view` (GDScript, throwaway) builds a `Skeleton3D` from the rig and its defaults. It skins the meshes
through `SurfaceTool` with `set_bones`/`set_weights` and `Skin.add_named_bind`, sets `set_bone_pose_rotation`/
`set_bone_pose_position` per frame, and saves the viewport. Frostbite data is used verbatim (metres, Y up, quaternion xyzw).

| clip | meshes | result (`ant\shots\`) |
|---|---|---|
| M9 `ReloadEmpty` (DCT, 57 keys) | `arms1p_rus_mesh` + `m9_1p_mesh` | 1P camera on `CameraJoint` (rotated 180° about Y, since Frostbite looks down +Z) plus a side view. Reads correctly: slide locked back, magazine out, new magazine in, slide release, return to idle |
| M9 `Fire` (DCT, 20 keys) | same | recoil kick |
| `Rifle_RunFwd` (DCT, `b_BasicAssetsMP`, rig `VeniceAntSke01`) | `us_assault_ub_mesh` + `us_assault_lb_mesh` + `m16a4_3p_mesh` | clean run cycle with the rifle at the shoulder |
| `Reload_NoAddon_Stand` (DCT, upper body) | same | magazine swap. In some frames the upper arm/shoulder looks pinched: twist and roll joints are normally driven by rig ops (`RigOpListAsset`, `RotateAroundPivotRigOp`, `JointToEffectorRigOpAsset`) that the probe does not run [I] |

## 9. Porting to the C++ runtime (reads the AssetBanks as is)

| piece | size (C++) | notes |
|---|---|---|
| package header + PackageMeta skip | ~60 lines | read Static meta only to skip it; import tables are not needed for playback [I] |
| GenericData blob walk + reflection-driven field reader | ~400 lines | generic: one reader for all 261 layouts. Strings/arrays `{u32 cap, u32 count, u64 ptr}`, DataRef u64, base via slot −1 |
| object model for ~12 types (Anim base, Dct, Raw, Frame, Vbr later, ChannelToDof, ClipController, LayoutHierarchy, Layout, DeltaTrajLayout, Ant Skeleton, Rig) | ~300 lines | read only these; skip the other ~250 controller/state-graph types until the animation graph is wanted |
| reference resolution (static index table + per-package) | ~80 lines | |
| DCT decoder | ~120 lines | the exact algorithm in §4.1; decode fully at load (37.5 MB of bitstream decodes in 1.7 s in C#) or per block on demand |
| channel → DOF → joint + rest pose | ~120 lines | §5 rules; precompute a per-clip track table |
| Godot side | — | `Animation` resources (`TYPE_ROTATION_3D`/`TYPE_POSITION_3D` via `rotation_track_insert_key`) driven by an `AnimationMixer`/`AnimationTree`, or a custom `SkeletonModifier3D` (`_process_modification_with_delta`) that samples decoded clips directly. All are present in this build's `extension_api.json` (lines 31398, 31839, 33595, 285631, 285831; `Skeleton3D.set_bone_pose_rotation` 282417; `Skin.add_named_bind` 286517) [V] |

Estimate [I]:

- About 1–1.5 k lines of C++ and 3–5 working days for parse + decode + Godot playback of single clips, with unit tests
  against this probe's JSON outputs as oracles. Use frame-0 vs IdlePose and the \|q\| statistics as the tests.
- VBR: another 2–5 days of reverse engineering, uncertain.
- Curve: unknown.
- BF3's actual *animation graph* is a separate, much larger job: 5,729 `ActorControllerAsset`, state flows, blend
  spaces, locomotion, rig ops, foot plant, IK. Recommendation: drive the decoded clips from our own small C++ state
  logic (idle/fire/reload/deploy/zoom for 1P; locomotion blend plus upper-body overlays for 3P) and do not interpret
  Ant graphs.

Runtime vs cache:

- Decoding is cheap, so reading the banks directly at runtime is viable.
  - `s_BasicAssets` is 22.5 MB and parses in about 1.3 s in C#; C++ should be well under that [I].
  - A weapon chunk parses in about 2 ms.
- If a cache is wanted, store decoded per-clip track arrays under `user://cache/anim/<sha1 of package bytes>`. The BF3
  files stay untouched.

Licensing: Rime has no LICENSE file in the repo; IceBloc is GPL-2.0. Reimplement from this written spec; do not copy their code.

## 10. Other decoders (web, brief)

- **IceBloc** (github.com/marv7000/icebloc, C#, GPL-2.0, last push 2024-04-15) [V, read via `gh api`].
  - Exports FB2 FrameAnimation, RawAnimation and DctAnimation (README).
  - Its `IceBlocLib.Frostbite2/Animations/DCT/DctDecompressor.cs` uses the same basis, the same base-relative DC and the
    same "skip DC in block 0" rule as Rime and as I verified.
  - Its `VbrAnimation.cs` is an empty class, so VBR is undecoded there too.
- Frosty Toolsuite documentation does not mention these classes. No public description of the VBR bitstream turned up.
- Sources: https://github.com/marv7000/icebloc ; https://www.tapatalk.com/groups/daitools/animations-t215.html (2015 forum speculation; not used).

## 11. Could NOT determine

- The VBR animated-channel bitstream (§6.1), `VbrAnimationAsset.Flags` (6), and `KeyTimeSize` when it is not 0.
- `CurveAnimationAsset` layout and semantics.
- The meaning of `DofTableDescBytes` low nibble (always 0 in the clips I looked at; not checked over all clips).
- The 8 `DeltaTrajLayoutAsset` DOFs: their order and kinds. They carry no channel in MP clips.
- The exact playback-rate semantics of `NumTicks`/`FPS`/`FPSScale`, and how `ReloadThreshold` relates to the clip (§5 is inferred).
- Additive clips (`Additive = true`): how they are layered. `TrimOffset` use.
- How rig ops compute roll and twist joints and IK, which causes the shoulder pinch in 3P renders. The data is in
  `RigOpListAsset`/`*RigOp` objects; not examined.
- Which 3P clip and weapon-set combinations the Ant graph selects in gameplay (the `ActorControllerAsset`/`StateFlow*` graph). Not examined.
- Whether the 9 low-precision DCT clips (§4.3) are exactly right. Precision loss is inferred, not proven.
- The `RagdollResource` layout. The Static package's import tables and `staticGuidToInt` map are not interpreted (not needed so far).
