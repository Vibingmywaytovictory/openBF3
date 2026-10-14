# antdump

Godot-free CLI over `bf3_core`'s Ant animation reader (`src/bf3_core/include/bf3/core/ant/`). The reader and the
codecs are a clean-room implementation from `research/probes/ant-animation-probe.md` (decision D14): no code from
Rime, IceBloc or AssetBankPlugin.

## Input: an export directory

Until the container layer (res by name, chunk by GUID) is wired in through `AntPackageSource`, the tools and tests
read an *export directory* of the raw Ant payloads (`bf3/core/ant/ant_source.h`):

```
res.tsv        name, file, size, res_meta, superbundle, bundle      one row per AssetBank res (397)
packages.tsv   partition, type, superbundle, bundle, chunk_file, fields   one row per AntPackageAsset (322)
res/*.bin      res payloads          chunk/<guid>.bin   decompressed chunks (88 Chunk packages)
```

The directory holds BF3 data, so it lives outside the repo (e.g. a scratch folder). It was produced with the
research `ebx-dump` probe (`research/probes/tools/ebx-dump`) extended by an `antexport <out_dir>` command that
mounts every non-chunk superbundle read-only, writes each AssetBank res once (first superbundle in layout order wins,
so `s_basicassets` comes from `Win32/Globals`) and, for every `AntPackageAsset` of packaging type Chunk, the chunk
`StreamingGuid` (about 382 MB, 4.6 s).

## Commands

```
antdump census  <export_dir> [--mp]           codec census, DCT checks, channel mapping (probe §2, §4.3, §5)
antdump list    <export_dir> <package>        clips of a package: codec, keys, controller, target, rig
antdump clip    <export_dir> <package> <clip> [--digits N] [--out f.json]   clip JSON (the probe's shape)
antdump compare <export_dir> <oracle.json>... dump the oracle's clip and compare value by value (1e-5)
antdump bench   <export_dir> [--mp] [--repeat N]   parse/decode/sample throughput (single thread)
antdump sample  <export_dir> <package> <clip> <frame>  one interpolated pose
```

`--mp` restricts to the probe's MP-relevant set (every package except `b_sp*`, `b_coop*`, `b_faceposer*`, `b_vo*`).
The clip JSON has `source`, `fps`, `fpsScale`, `numTicks`, `endFrame`, `keyTimes`, `joints` (rig rest pose) and
`tracks` (one per joint channel, values per key) like the probe's files, plus `floatTracks` for named float DOFs.

## Tests

`ctest -R core.ant` runs `core.ant.unit` (synthetic archives and DCT streams, no data) and `core.ant`
(`OPENBF3_ANT_DATA` = export directory, `OPENBF3_ANT_ORACLE` = the probe's output folder with the clip JSONs).
`core.ant` exits 77 (skipped) when the data is absent.
