# 05 — Multiplayer networking: BF3's netcode, Godot 4.8 options, and the architecture for the port

Written 2026-10-10 for the Godot 4.8 (C++/GDExtension) reimplementation of Battlefield 3 multiplayer. Scope: transport,
replication, prediction, lag compensation, scale (64 players incl. bots, ~40 vehicles, destruction), listen and dedicated
servers, the session layer, and how all of it maps onto Godot 4.8 in C++. Level/render pipelines are out of scope.

**The governing rule, restated:** every machine (client, listen host, dedicated server) reads the original BF3 files of
**its own** retail install as-is, read-only. Nothing in an install is written, patched or repacked; converted data lives in
`user://cache/<digest>/`. The network never ships BF3 content; it only proves that both ends read the same content (§7.6).

Tags: **[verified]** = checked myself in data, code, the API dump or a primary source I read; **[report claim]** = stated by
a source I did not re-check; **[inferred]** = my reading. Paths are absolute; `GODOT@c971f93` = Godot source at the exact
build commit `c971f93e7e76b0ef919bf6009e7b868bea04db7f` (2026-09-29, the 4.8-dev7 build), checked out read-only into my
scratch folder `…\scratchpad\networking\godot-src`; `API` = `C:\Users\olku\BF3 reimplementation\research\godot-api\extension_api.json`.

---

## 0. Decisions in one table

| question | answer | basis |
|---|---|---|
| Godot high-level multiplayer (SceneMultiplayer, MultiplayerSynchronizer/Spawner, RPC) | **Do not use for gameplay.** Full-state Variant sync per synchronizer, no per-client ack baselines, no priority/budget, main-thread, node-path ids | §3.1 [verified source] |
| Transport | **ENet 1.3.18 (lsalzman, MIT) compiled into the extension**, owned by the server thread / client net code; Godot's own `ENetConnection` is the same ENet and is an acceptable fallback; GameNetworkingSockets as the upgrade path if encryption + ICE NAT traversal become needs | §3.2–3.4 |
| Authority | Authoritative server, own fixed tick, clients send inputs only | §4 |
| Tick | **60 Hz simulation, 30 Hz snapshots** (configurable per server; 30 Hz sim as the low-CPU fallback). BF3 ran 30 Hz sim / ~10 Hz updates; BF4 settled on 30 Hz updates then offered up to 144 Hz | §1, §4.1 |
| Where the server runs | Its own `std::thread` (listen) or the same code in `godot --headless` (dedicated). **It never calls `PhysicsServer3D`**: Godot refuses space/body queries off the main thread. Server collision = **our own Jolt world** linked into the extension | §6 [verified source] |
| Own soldier / own vehicle | Predicted and reconciled by replaying commands through the same C++ sim (BF3 itself predicts both: `UsePrediction = true`) | §5 |
| Others | Interpolated at render time = server time − ~75–100 ms, Hermite + bounded extrapolation for low-rate (far) ghosts | §5.3 |
| Hits | **Server-validated client-side hit detection**: the client reports, per hit, the exact sample it rendered for that target; the server re-runs the bullet against that reconstructed pose (≤ 250 ms rewind). BF3 shipped client-side damage arbitration with a 0.85 s latency cap | §5.4 |
| Replication | Tribes/Frostbite-style **ghosts**: per-client scope, priority accumulator, per-ghost acked baselines, quantised field groups, ≤ 1,200 B per snapshot datagram | §7 |
| Reliable traffic | ENet reliable channels for events (kills, score, chat, spawn), a separate reliable bulk channel for join-time full sync; destruction state rides the ghost stream (reliable by ack) | §7.4–7.5 |
| Content check | Handshake compares digests computed from the SHA1s already in each install's bundle manifests plus a digest of the derived sim tables; language bundles excluded | §7.6 |
| Session | Tiny C++ master (heartbeat lister + UDP hole-punch introducer, optional relay), UPnP via Godot's `UPNP`, per-install keypair identity, server authority as the anti-cheat | §8 |

---

## 1. How BF3's own netcode worked

### 1.1 What the retail data says (BF3 PC, read from the install)

The retail client's patched boot filesystem `C:\Program Files\EA Games\Battlefield 3\Update\Patch\Data\initfs_Win32` holds
`Scripts/Game.cfg`. I decoded it read-only with the IW4L decoder (`C:\Users\olku\iw4L\context\bf3dump\06-containers\fb2.py`,
driven by my scratch script `…\scratchpad\networking\initfs_check.py`) and the lines match the IW4L dump
`C:\Users\olku\iw4L\context\bf3dump\06-containers\initfs_patch\Scripts\Game.cfg` [verified]. The per-level
`levels/<L>/<L>/settings_win32` EBX partition carries the settings classes' defaults (read from the IW4L JSON dump of the
retail EBX, `C:\Users\olku\iw4L\context\bf3dump\ebx\levels\mp_subway\mp_subway\settings_win32.json`; mp_001 identical)
[verified in dump].

| setting | value | where | reading |
|---|---|---|---|
| `GameTimeSettings.MaxSimFps` | **30.0** | settings_win32.json:432 | fixed **30 Hz simulation** [verified value; meaning inferred, consistent with VU's "regular = 30 Hz" below] |
| `GameTimeSettings.VariableSimTickTimeEnable` / `ClampTicks` / `MaxVariableFps` | false / 2 / 200 | :442, :434, :433 | fixed sim step; at most 2 sim ticks per rendered frame; render uncapped to 200 fps [inferred semantics] |
| `Client.VisualFrameInterpolation` | true | Game.cfg:130 | the renderer interpolates between 30 Hz sim states [inferred]; Frostbite's `UpdatePass` enum has `UpdatePass_PreSim, _PostSim, _PostFrame, _FrameInterpolation, _PreInput, _PreFrame` (`C:\Users\olku\VU-Docs\types\fb\UpdatePass.yaml`) [verified schema] |
| `Server.OutgoingFrequency` | **10.0** | Game.cfg:113 (EBX default 15.0, json:942) | server→client send rate, Hz [inferred]; community consensus "BF3/BF4-launch: server sends 10 Hz" agrees [report claim] |
| `Client.IncomingFrequency` | 15.0 | Game.cfg:109 (EBX default 10.0, json:2087) | client-side counterpart; which of the two wins is unknown [could not determine] |
| `Server.OutgoingRate` / `Client.IncomingRate` | 128000 / 128000 | Game.cfg:114, :110 | per-client downstream cap; units not stated. 128,000 **bits**/s fits `Network.MaxServerFrameSize 2048` B × 10 Hz × 8 = 163,840 bit/s [inferred] |
| `Client.OutgoingRate` / `Server.IncomingRate` | 48000 / 48000 | Game.cfg:111, :115 | upstream cap (≈ 6 KB/s if bits) [inferred] |
| `Network.MaxServerFrameSize` | **2048** | Game.cfg:359 | max bytes per server→client network frame [inferred] |
| `Network.MaxGhostCount` | **8192** | Game.cfg:356 | max replicated objects ("ghosts") [inferred: Frostbite uses the Tribes "ghost" term; `NetworkSettings` in `C:\Users\olku\VU-Docs\types\fb\NetworkSettings.yaml` also has `maxClientFrameSize`, `maxServerFrameSize`, `timeNudgeGhostFrequencyFactor`, `timeNudgeBias`, `useFrameManager`, `timeSyncEnabled`] |
| `Network.MaxClientCount`, `Game.MaxPlayerCount` | 68, 68 | Game.cfg:357, :376 ("68 = max regular players + max spectator players") | 64 players + 4 spectator slots [verified comment] |
| `Network.ClientPort` / `ServerPort` / `ProtocolVersion` | 25100 / 25200 / 67 (PC) | Game.cfg:351–355 | [verified] |
| `Network.TimeNudgeBias` / `TimeNudgeGhostFrequencyFactor` | 0.05 / 0 | `…\Scripts\PlaytestCommon.cfg`:73, :49 (internal playtest cfg) | client clock "nudge" relative to ghost update frequency, 50 ms bias [inferred] |
| `Server.ThreadingEnable` (dedicated branch) | false | Game.cfg:694 | the dedicated server ran its sim single-threaded in this config [verified line; effect inferred] |
| `Server/Client.IsInternetSimulationEnabled`, `MinLatency 0.1`, `MaxLatency 0.2`, `PacketDrops 0.01`, `DropSpikeChance 0.03`, `Min/MaxDropDuration 0.3/1.0`, `ReorderingChance 0.01`, `DuplicationChance 0.02`, `CorruptionChance 0.01` | false in Game.cfg:53/118 | Game.cfg:52–62, 117–127 | Frostbite had a built-in network-conditions simulator; **copy these knobs into our transport** (§9) [verified] |
| `SyncedGameSettings.AllowClientSideDamageArbitration` | **true** | settings_win32.json:2163 | **client-side hit detection** on [verified value]; VU's server `DamageInfo` has `isClientDamage` (`C:\Users\olku\VU-Docs\content\vext\ref\server\type\damageinfo.md`:33) [verified doc; semantics inferred] |
| `SyncedGameSettings.MaxAllowedLatency` | **0.85** s | :2157 | latency above which client-side damage is not trusted [inferred] |
| `ClientSettings.LocalVehicleSimulationEnabled` / `AsyncClientBulletEntity` | true / true | :2143–2144, Game.cfg:132 | the client simulates its own vehicle locally; bullets are client entities simulated asynchronously [inferred] |
| `SoldierEntityData.UsePrediction` (`characters/soldiers/mpsoldier`) | **true** | `…\bf3dump\ebx\characters\soldiers\mpsoldier.json`:9971 | own soldier predicted [verified] |
| `VehicleEntityData.UsePrediction` | **true on all 16 surveyed vehicles** (quadbike, RHIB ×2, BTR-90, Stryker, Star 1466, Humvee, DPV, Sprut, Vodnik, LAV-25, F-35B, skidloader, HIMARS, AAV-7A1, BMP-2) | `…\bf3dump\level-structure\json_survey\_veh\*.json`, `…\ebx\vehicles\bmp2\bmp2.json`:13614 | driver predicts own vehicle, jets included [verified] |
| `SoldierCameraComponentData.AuthoritativeEyePosition` | true | mpsoldier.json:12226 | [verified value; semantics unknown] |
| `CharacterPhysicsData.RadiusToPredictCollisionOnCharacters` (`Characters/Soldiers/DefaultSoldierPhysics`) | **0.0** | `…\bf3dump\level-structure\json\_byguid\235cd1da-….json` | prediction does not collide with other soldiers [inferred] |
| destruction | `NetworkIdCount ≈ 2×parts+2` per breakable; `StaticModelNetworkInfo.partNetworkIdRanges`, `StaticModelGroupMemberData.networkIdRange`, `PartComponentData.IsNetworkable` | handoff 04 §5.1–5.2; schema | BF3 replicates destruction **per part** [verified in handoff] |
| entity classes | `ClientGhostProjectileEntity`, `ClientProxyProjectileEntity`, `ClientBulletEntity`, `ServerGhostProjectileEntity`, `ServerBulletEntity` | `C:\Users\olku\VU-Docs\content\vext\entities.md`:239, 494, 755, 1750, 2119 | slow projectiles (grenades, rockets) are networked "ghosts"; bullets are local entities on each side [inferred] |
| variable rates | `BangerEntityData.useVariableNetworkFrequency` | schema `BangerEntityData.yaml`:77 | per-object variable update frequency existed [verified schema] |

Venice Unleashed (which runs the retail BF3 server binary) documents the server frequency modes as `regular` = 30 Hz,
`high60`, `high120`, and that "normally, the server will reduce the amount of updates it sends for players that are further
away from other players" unless `vu.HighPerformanceReplication true` (`C:\Users\olku\VU-Docs\content\hosting\commands.md`:73–79,
`hosting\config.md`:87–98) [report claim — VU's statement about BF3's server; I did not measure it]. That is BF3's
**distance-based priority/relevancy** in one sentence.

### 1.2 BF4 numbers (same engine family, later; do not mix with BF3)

| item | value | source |
|---|---|---|
| launch server→client update rate | 10 Hz, client→server 30 Hz | MP1st CTE report "Server tick rate improved from 10hz to 30hz" (2014-05-15, https://mp1st.com/news/bf4-cte-results-tick-rate-improvements-custom-ads-sensitivity-reduced-screen-shake) [report claim]; forum posts [report claim] |
| High Frequency Network Update (June 2014) | "Within a certain radius of the player … update the clients at a higher rate from the server"; "updating player movement, stance, rotation, damage and projectiles at a separate rate on foot and in vehicles"; LOW/MEDIUM/HIGH; "faster than 1Mbit upload and 1Mbit download should be fine using the HIGH setting"; off by default | DICE, https://www.ea.com/games/battlefield/news/new-update-addressing-netcode-rolling-out [verified] |
| simulation | "Normally, servers in Battlefield 4 run at 30hz"; high-tickrate beta up to 144 Hz; DICE recommends **64-player servers: one instance per machine, max 60 Hz**; 48/32 slots ≤ 120 Hz; < 20 slots ≤ 144 Hz | DICE PDF (David Sirland), https://eaassets-a.akamaihd.net/dice-commerce/battlefield4/assets/patch_notes/HighTickrat_BETA_R1_R4_DS.pdf [verified] |
| lessons DICE listed | at > 30 Hz: "Sprint acceleration is not 1:1 with 30hz", "Weapon spread/recoil is not 1:1", "Flying with mouse is not 1:1", "Higher tickrates use quite a lot more bandwidth"; HUD warns when client FPS < tickrate | same PDF [verified] → **write every rate in per-second units from day one** |
| client prediction of damage | "a portion of damage dealt was rejected by the server … bullets … fired after the point of death"; kill card showed "health as predicted by your game client, rather than the health confirmed by the server"; "latency compensation … makes sure players move around naturally on your screen" | DICE, https://battlelog.battlefield.com/bf4/news/view/addressing-netcode-in-bf4/ (March 2014) [verified] |
| BF1 | 60 Hz standard tick | https://www.gamestar.de/artikel/battlefield-1-starker-netcode-tickrate-wird-gegenueber-bf4-verdoppelt,3274856.html [report claim] |

### 1.3 BF3 model, as far as it can be reconstructed

30 Hz fixed simulation with render-frame interpolation; client predicts its own soldier and vehicle; server sends
"ghost" updates at ~10–15 Hz under a per-client byte budget (~2 KB frames) with lower rates for distant objects; client
clock nudged against the ghost frequency; hits decided on the shooter's client and arbitrated by the server under a
latency cap; slow projectiles are ghosts, bullets are local per machine; destruction is per-part network state. This is
the model the port keeps, with the rates BF4/BF1 later moved to [inferred synthesis of §1.1–1.2].

---

## 2. What IW4L built and what to keep (C:\Users\olku\iw4L\crates)

| thing | IW4L | keep? |
|---|---|---|
| authority tick | 20 Hz, integer-ms command time (`net\src\authority\inbox.rs`:8–10) | concept yes, rate no (§4.1) |
| CPU lesson | 16 bots: 13 ms/tick inside one frame → every 5th frame 20–26 ms (`…\12-MEASURE-FIX-PART\measure.md`:160–194; handoff 04 §10) | **yes: server on its own thread** |
| handshake | `HandshakeHello{protocol_version, ContentFingerprint{gameplay,map,models,weapons,classes}, ProtocolLimits{authority_hz,…}}`, named rejects (`net\src\transport\protocol.rs`:14–209) | yes, with BF3 manifest digests (§7.6); IW4L's map digest omitted the clip mesh (`1-RESEARCH-PART\05-iw4l-vehicles-sim-net.md` §10 item 6) → include collision |
| delta coding | per-client `AckedBaselineTable` (encode against highest acked snapshot, `transport\acked_baseline.rs`), zstd level 1 when it saves bytes (`protocol.rs`:575–598) | ack baselines yes (per ghost, §7.3); whole-payload zstd optional |
| commands | redundant cmds (history cap 256), queue caps 1 s, stall detection (`inbox.rs`:32–42) | yes, smaller redundancy (3–4) |
| prediction | `MoveHistory` of (seq, tick, cmd, input state, output state, vehicle) + replay; error decay 100 ms, snap < 0.1 (`client\predict.rs`, `client\predicted_error.rs`:35–37); driver vehicle predicted bit-exact (handoff 04 §10) | **yes** |
| reconciliation contract | state classes Authoritative/Predicted/PresentationOnly; identities `(ClientId, LifeSequence)`, `EntityRef(number, generation)`, `ProjectileId`, `EventSequence` dedupe (`net\src\reconciliation.rs`) | **yes, verbatim as design rules** |
| lag compensation | client reports `ShotSampleProvenance{left tick, right tick, alpha, quality}`; server rewinds collision history (32 ticks) clamped to 4 ticks = 200 ms; refusal reasons (`sim\src\bullet_collision.rs`:37–104, `sim\src\world.rs`:1869–1918) | **yes**, per target, 250 ms cap (§5.4) |
| remote players | fixed 100 ms interpolation delay (`client\proxy.rs`:9–11) | adaptive instead (§5.3) |
| reliable events | own u16-seq queue, **drops when > 64 pending** (`transport\reliable.rs`:6, 77–89) | no: never drop reliable data; use ENet reliable channels |
| destruction | sorted `(slot, state)` rows, full sync every 200 ticks (`transport\meta_wire.rs`:19; handoff 04 §5.6) | rows yes; periodic full sync replaced by ack baselines (§7.5) |
| bandwidth | ~144 B/EntityState row/tick/client; moving vehicle 180–250 B/client/tick unquantised (`4-INTEGRATION-PART\vehicles-sim.md`:318) | quantise (§7.2) |
| master | QUIC (quinn) rooms + relay datagrams + server browser, CA pinning without a domain, ≤ 18 members (`crates\master_protocol\src\lib.rs`:5–26, `C:\Users\olku\iw4L\docs\MASTER.md`) | relay idea yes; QUIC not needed for game traffic |
| replay/killcam | demo files with `MatchRecordIdentity{rng_scheme, root_seed, content_digest, zone}` (`crates\replay\src\file.rs`:9–30); CoD-style killcam from a 1,200-tick archive (`transport\archive.rs`:9–17) | demos yes; **CoD killcam no** (BF3's killcam is a camera on the killer, §8.5) |
| listen loopback | `LoopbackTransport`, `ListenLoopback` | yes: the local player is an ordinary client over an in-process transport |

---

## 3. Godot 4.8 networking options (verified against the API dump and source)

### 3.1 SceneMultiplayer / MultiplayerSynchronizer / MultiplayerSpawner / RPC — not for gameplay

API: `SceneMultiplayer` (22 methods: `send_bytes`, `set_auth_callback`, `complete_auth`, `max_sync_packet_size`,
`max_delta_packet_size`, `server_relay`), `MultiplayerSynchronizer` (`replication_interval`, `delta_interval`,
`visibility_update_mode`, `add_visibility_filter`, `set_visibility_for`), `SceneReplicationConfig`
(`REPLICATION_MODE_NEVER/ALWAYS/ON_CHANGE`, `property_set_spawn/sync/watch`), `MultiplayerSpawner` [verified API].

How it works at `GODOT@c971f93` [verified source]:
* Per peer, per synchronizer, every interval, `_send_sync` sends the **full** value of every sync property, unreliable,
  with 8 B per synchronizer (`net_id` u32 + size u32) (`modules\multiplayer\scene_replication_interface.cpp`:802–853);
  packets split at `sync_mtu = 1350`, and a single state above it is **dropped with an error** (`.h`:93, `.cpp`:832).
* "On change" (watch) properties go through `_send_delta` on the **reliable** channel (`.cpp`:710–763, `_send_raw(…, true)`).
* Values use `MultiplayerAPI::encode_and_compress_variants`: only `bool`/`int` are compacted (1-byte meta, 8/16/32/64-bit
  ints); everything else falls back to `encode_variant` with a **4-byte type header** + raw payload
  (`scene\main\multiplayer_api.cpp`:59–127; `core\io\marshalls.cpp`:1323–1397): a `Vector3` costs 16 B, a `Quaternion` 20 B.
* Ids are node-path based with a confirmation round trip (`_verify_synchronizer`, `.cpp`:683–697); processing runs in the
  SceneTree on the main thread (`.cpp`:143–153).

Fit for 64 players + 40 vehicles + ~13 k destruction slots: no per-client acked baselines, no priority/budget, no
quantisation, no prediction/rewind hooks, main-thread bound, scene-node coupled. A rough count: 104 synchronizers ×
(8 + ~50 B) ≈ 6 KB per client per interval → ≈ 180 KB/s per client at 30 Hz, 5–8× the design in §7 [inferred]. It could
carry lobby chat, but our own reliable channel does that with less machinery. `MultiplayerAPIExtension` /
`MultiplayerPeerExtension` exist (custom `_rpc`, `_poll`; `_put_packet`, `_get_packet`, …) [verified API] but buy nothing
for a custom snapshot protocol.

### 3.2 ENet inside Godot

* Bundled ENet **1.3.18** (`thirdparty\enet\enet\enet.h`:30–32), patched only to use Godot sockets
  (`thirdparty\enet\patches\0001-godot-socket.patch`, `enet_godot.cpp`); default MTU **1392** (`enet.h`:222); ≤ 255
  channels, ≤ 4095 peers (`protocol.h`:18–19) [verified].
* `ENetMultiplayerPeer` reserves 2 system channels (`SYSCH_RELIABLE=0`, `SYSCH_UNRELIABLE=1`, `SYSCH_MAX=2`,
  `modules\enet\enet_multiplayer_peer.h`:49–51); `TRANSFER_MODE_UNRELIABLE` → `UNSEQUENCED|UNRELIABLE_FRAGMENT`,
  `UNRELIABLE_ORDERED` → `UNRELIABLE_FRAGMENT`, `RELIABLE` → `RELIABLE`; user channel n → `SYSCH_MAX + n − 1`; warns when an
  unreliable send exceeds the MTU (`.cpp`:340–363) [verified].
* Raw `ENetConnection` (`create_host_bound`, `service`, `flush`, `bandwidth_limit`, `channel_limit`, `broadcast`,
  `compress(COMPRESS_NONE/RANGE_CODER/FASTLZ/ZLIB/ZSTD)`, `dtls_server_setup`, `dtls_client_setup`, `pop_statistic`,
  **`socket_send(address, port, packet)`** — raw UDP from the ENet socket, usable for NAT punching) and `ENetPacketPeer`
  (`send(channel, packet, flags)` with `FLAG_RELIABLE=1`, `FLAG_UNSEQUENCED=2`, `FLAG_UNRELIABLE_FRAGMENT=8`;
  `get_statistic(PEER_ROUND_TRIP_TIME, PEER_PACKET_LOSS, …)`, `set_timeout`, `throttle_configure`) [verified API].
* No mutex anywhere in `modules\enet\*.cpp` [verified grep] → single-thread ownership; `service()` returns an `Array`
  and packets come as `PackedByteArray` (Variant allocations per event) [verified signatures; cost inferred].
* `PacketPeerUDP` (`bind`, `wait`, `set_dest_address`, broadcast, multicast), `UDPServer`, `PacketPeerDTLS`/`DTLSServer`
  [verified API] — too low-level to be worth it over ENet.

### 3.3 Libraries linked directly into the GDExtension

| library | licence (verified LICENSE) | latest tag (git ls-remote, 2026-10-10) | gives | threading | NAT / crypto | fit |
|---|---|---|---|---|---|---|
| **ENet** (lsalzman) | MIT | v1.3.18 | connections, channels, reliable/unreliable/unsequenced, fragmentation, RTT/loss stats, bandwidth throttle | caller-owned, single thread | none built in; raw socket access for punching | **chosen**: tiny, proven, identical to what Godot ships |
| **GameNetworkingSockets** (Valve) | BSD-3-Clause | v1.6.1 | reliable/unreliable messages, fragmentation, ack-vector reliability, **AES-GCM-256 + Curve25519**, lanes with priorities/weights, IPv6, latency/loss simulation, **P2P via WebRTC ICE with your own signaling** [verified README] | own service thread | ICE needs STUN/TURN + signaling; Steam's auth/signaling/SDR relays are Steam-only [verified README] | upgrade path; heavy deps: protobuf (+Abseil ≥ 22), OpenSSL/libsodium/bcrypt, webrtc [verified BUILDING.md] |
| **yojimbo + netcode + reliable** (Más Bandwidth) | BSD-3-Clause | v1.13.5 / v1.4.8 / v1.4.5 | connect tokens, encrypted+signed packets (libsodium), reliable-ordered + unreliable messages, bitpacker | "single-threaded. Call all client and server functions from the same thread" [verified README] | none | good, but "designed for … 100 players or less" and the connect-token model assumes a backend that holds the server key [verified README] — awkward for community listen servers |
| Steamworks networking | proprietary SDK | — | SDR relays, Steam auth | — | — | **no**: needs a Steam app ID; a fan reimplementation of an EA game will not have one |
| QUIC (msquic/quiche) | MIT/BSD | not checked | reliable streams + datagrams | — | TLS 1.3 | overkill for game traffic; fine for a master service (IW4L used QUIC) |

### 3.4 Threading of the Godot network classes

`ENetConnection`/`ENetPacketPeer` are `RefCounted` with no internal locking: usable from one owning thread [inferred from
source]. The project avoids the question by linking ENet itself into `bf3net` (pure C++, no Variant), so the server
thread, the dedicated server and any future standalone server share one transport implementation [inferred design].

---

## 4. Recommended architecture

### 4.1 Authoritative server, fixed tick

* **Simulation 60 Hz** (16.667 ms), **snapshots 30 Hz** (every 2nd tick), **client command packets 60 Hz** each carrying the
  last 3–4 commands. Server-configurable `{sim_hz ∈ 30/60, snapshot_divisor ∈ 1/2/3}` fixed per match and carried in the
  handshake.
* Why 60 rather than BF3's 30: BF1 standardised 60 Hz; DICE's own BF4 data shows 30 Hz caused "dying behind cover" and
  burst damage that higher rates "significantly improve" [verified PDF]; a jet at 250 m/s moves 4.2 m per 60 Hz tick vs
  8.3 m at 30 Hz; input-to-tick quantum 16.7 ms; Jolt and Godot default to 60 Hz. Cost: ~2× server CPU (§10) and 2× upstream
  packets (~3 KB/s per client) [inferred].
* Everything time-dependent is written per second (fire rate as RPM with sub-tick fire timestamps, sprint acceleration,
  recoil recovery, flight controls) so 30 ↔ 60 does not change feel — the exact bugs DICE listed for BF4 [verified PDF].
* Ticks are integers; server time = tick × step; the server never waits for late commands (repeats the last command, marks
  it, like IW4L).

### 4.2 Time synchronisation

* Client estimates the server tick from snapshot ticks and ENet RTT, and runs **ahead** by RTT/2 + an input buffer so that
  command N arrives just before the server simulates tick N.
* Each snapshot reports, per client, how many ticks early its newest command arrived (signed i8); the client speeds up or
  slows its tick clock by up to ±5 % to hold 1–2 ticks of buffer; hard reset when > 250 ms off (IW4L used reset 500 ms /
  fast 100 ms, `client\frame_clock.rs`:89–91). This is the role of BF3's `timeNudgeBias`/`timeNudgeGhostFrequencyFactor`
  [inferred].
* Render clock for remote objects = estimated server time − interpolation delay (§5.3).

### 4.3 Threads in one listen-server process

```
main thread (Godot)        input → Command; client prediction + reconciliation; interpolation; presentation (RS
                           instances, animation, audio, HUD); Godot physics only for cosmetic debris/ragdolls
server thread (std::thread) fixed 60 Hz: ENet host service → commands → bots → sim (own Jolt world) → destruction →
                           rules → lag-comp history → ghost scoping/encoding per client → send; recording
server job pool            N threads (listen: ≤ cores−3) for bot thinking and per-client snapshot encoding
Godot WorkerThreadPool      rendering, texture streaming, Godot's Jolt jobs (WorkerThreadPool-backed,
                           modules\jolt_physics\spaces\jolt_job_system.cpp:99–110 [verified])
loader threads             BF3 container/EBX/resource decode, conversion into user://cache
```

The local player is a normal client connected through an in-process lock-free transport (IW4L's loopback), so every code
path is the remote path [inferred design].

---

## 5. Prediction, interpolation and hits

### 5.1 Own soldier

* Client keeps a ring of `(tick, Command, predicted state after the command)`.
* The snapshot carries, for the receiving client only, the authoritative state of its soldier **at the last command tick
  the server consumed** in full precision (position f32×3, velocity f32×3, controller mode/ground/stance/sprint/jump timers,
  weapon state: ammo, reload and fire timers, current spread). On mismatch beyond ε or any discrete difference: reset to it
  and replay the newer commands through the **same** C++ step; hide the jump with a decaying visual offset (IW4L: 100 ms
  decay, snap under 0.1).
* Requirement on the soldier topic: the controller is a pure function `state' = step(state, command, world, dt)` with no
  hidden state outside `state`, and the world it queries is our Jolt world (static shapes + destructible pieces in their
  replicated states + vehicles as kinematic obstacles). Other soldiers are **not** obstacles in prediction (BF3:
  `RadiusToPredictCollisionOnCharacters = 0.0`) [verified value; rule inferred].
* Before replaying, adopt the snapshot's destruction rows so traces see the same pieces (handoff 04 §5.6) [verified lesson].

### 5.2 Vehicles

* **Driver predicts** (BF3 data: `UsePrediction = true` on every surveyed `VehicleEntityData`; `LocalVehicleSimulationEnabled
  = true`) [verified]. The client prediction world holds the local vehicle as the only dynamic body; on each snapshot it
  restores the body (and `VehicleConstraint`, engine/gear, rotor/flight state) to the authoritative state at the acked tick
  and replays. Jolt supports exactly this: `PhysicsSystem::SaveState/RestoreState` with a `StateRecorderFilter` to save one
  body, constraints saved separately; simulation is deterministic when API calls are made in the same order; broadphase
  query order and contact callbacks are not deterministic (Jolt `Docs/Architecture.md`:785–851,
  https://github.com/jrouwe/JoltPhysics/blob/master/Docs/Architecture.md) [verified doc]. Flight models (heli/jet) are our
  own C++ force code on the body, replayed the same way.
* Passengers ride the interpolated vehicle; gunners predict only their own turret/view angles (input), firing is resolved
  by the server like soldier fire.
* Other vehicles are kinematic obstacles at their presented pose in the driver's prediction world; vehicle–vehicle
  contacts are settled by the server and corrected on the client (IW4L did the same: "other drivers' vehicles are frozen
  obstacles in prediction", handoff 04 §10) [verified lesson].
* Seat changes, entry/exit, roadkill, crash and rotor strike are server-only decisions (IW4L's "touch phase", handoff 04 §10).

### 5.3 Everything else: interpolation

* Common render time `t_r = t_server_est − d`, with `d = 2 × snapshot interval + jitter allowance`, clamped 50–150 ms
  (≈ 75–100 ms at 30 Hz snapshots). Near ghosts (updated every snapshot) are linearly/Hermite interpolated between the two
  samples around `t_r`; far ghosts updated at 5–10 Hz use Hermite with their replicated velocity and, when `t_r` is past
  their newest sample, bounded dead-reckoning (≤ 250 ms) blended back on the next sample [inferred design; BF4 called its
  equivalent "latency compensation … players move around naturally on your screen", verified Battlelog post].
* Animation is driven locally from the replicated movement state (speed, stance, aim pitch/yaw, weapon action), not
  replicated per bone [inferred].

### 5.4 Hit detection: server-validated client hits (recommended)

The two candidates and the choice:

| | server rewind (Source/IW4L) | BF3 client arbitration |
|---|---|---|
| who decides | server traces against rewound hitboxes | shooter's client decides; server sanity-checks (`AllowClientSideDamageArbitration`, `MaxAllowedLatency 0.85 s`) |
| fidelity | exact only if the server reconstructs what the client drew | exactly what the client drew |
| cheat surface | none beyond aim | hit claims within the server's tolerance |
| problem cases | low-rate far ghosts (client interpolation ≠ server guess) | long latency caps → "dying behind cover" (what BF4 fought, verified PDF) |

**Recommendation: the IW4L provenance method extended per target, which combines both.** Every fire command carries the
fire time (tick + sub-tick fraction) and the weapon's seeded spread index, so client and server spawn the **same** bullet
(BF3 bullets are projectiles: `BulletEntityData` speed, gravity, `instantHit`, damage falloff — weapons topic). The client
flies its bullet against its presented world; when it hits a soldier or vehicle it sends `HitReport{fire_seq, bullet_id,
target ghost, sample (tick_a, tick_b, alpha) it rendered for that target, hit part}`. The server flies the same bullet
through its own world (static + destruction state as of now) and, for that target, tests against the pose reconstructed
from its history at exactly that sample; it accepts if the ray/segment intersects within a small tolerance and the sample is
not older than **250 ms** of rewind (configurable; BF3's 0.85 s is too generous). Targets the client did not report are
tested at the shooter's render time (plain rewind). Damage, death, armour and `MaterialRelationDamageData` stay
server-side; hit markers and kill feed come back on the reliable channel (BF4's lesson: do not show client-predicted health
as truth, verified post).
* History: per tick per soldier root transform, stance, aim angles, pose phase (hit capsules recomputed on demand); per tick
  per vehicle transform. 32 ticks at 60 Hz = 533 ms ring [inferred sizing; IW4L kept 32 ticks at 20 Hz].
* Destruction is not rewound (current state used) [inferred simplification].
* Grenades, rockets, tank shells: server-simulated ghosts (BF3's `…GhostProjectileEntity`), client shows its own predicted
  launch and adopts the ghost; explosions server-side.

---

## 6. Godot 4.8 integration: separating server and client in one process

### 6.1 Why the server cannot use Godot's PhysicsServer3D [verified source unless noted]

* Jolt is always wrapped: `create_jolt_physics_server()` returns `PhysicsServer3DWrapMT(JoltPhysicsServer3D(run_on_separate_thread), run_on_separate_thread)` (`modules\jolt_physics\register_types.cpp`:41–51).
* The wrapper refuses queries off the main thread **regardless of the thread setting**: `space_get_direct_state`,
  `body_test_motion`, `body_get_direct_state` start with `ERR_FAIL_COND_V(!Thread::is_main_thread(), …)`
  (`servers\physics_3d\physics_server_3d_wrap_mt.h`:120–124, 273–282). Other calls from a foreign thread are pushed into a
  command queue; calls with a return value block until the main thread flushes it at `sync()` once per physics tick
  (`physics_server_3d_wrap_mt.cpp`:68–72, 89–111; `server_wrap_mt_common.h`:44–56).
* Even on the main thread, `JoltPhysicsServer3D::space_get_direct_state` fails while a space is stepping or, with the
  separate-thread option, outside sync (`modules\jolt_physics\jolt_physics_server_3d.cpp`:231–236).
* `space_create()` works and creates a **separate `JPH::PhysicsSystem`** per space (own `max_bodies` budget,
  `spaces\jolt_space_3d.cpp`:112–119), but there is **no public per-space step**: `PhysicsServer3D` exposes no `step` to
  GDExtension (only `PhysicsServer3DExtension::_step` for implementers) [verified API]; `JoltPhysicsServer3D::step()` steps
  **every active space** with the same delta, sequentially, from `Main::iteration` after `_physics_process`
  (`jolt_physics_server_3d.cpp`:1643–1655; `main\main.cpp`:5017–5062).

Consequence: a second Godot space for the server would tick at Godot's physics rate on the main thread, inside the render
frame — the exact IW4L failure. **Decision: `bf3sim` links its own Jolt (MIT, v5.x) and owns its worlds.** One server world;
one client prediction world; on a listen server both can share the immutable static `JPH::Shape` objects (ref-counted,
const after creation) to avoid duplicating 0.4–3.8 M collision triangles (handoff 04 §3.2) [inferred]. Godot's own
Jolt/PhysicsServer3D is used only for client-side cosmetics (debris clusters, bangers, ragdolls) on the main thread.
A second Jolt copy inside the extension DLL does not clash with the engine's statically linked one on Windows (no exported
symbols) [inferred; verify at link time, and build with hidden visibility on Linux].

### 6.2 Settings and servers (defaults read from the exact build via `--doctool`, descriptions from `GODOT@c971f93\doc\classes\ProjectSettings.xml`)

| setting / server | default | use | evidence |
|---|---|---|---|
| `physics/3d/run_on_separate_thread` | false | **leave false**: "restricts API access to only physics process"; with Jolt it also loses error context | ProjectSettings.xml:2677–2680 [verified] |
| `physics/common/physics_ticks_per_second` | 60 | presentation physics only (debris); unrelated to the server tick | :2734–2741 [verified] |
| `physics/common/max_physics_steps_per_frame` | 8 | keep | :2717–2721 [verified] |
| `physics/jolt_physics_3d/limits/max_bodies` | 10240 (per space, see §6.1) | cosmetics budget | doctool dump [verified] |
| `rendering/driver/threads/thread_model` | 1 (safe) | keep; "Separate" is marked experimental, "not recommended for use in production" | :2949–2951 [verified] |
| `threading/worker_pool/max_threads` | −1 (all cores) | cap on listen servers so Godot's pool and the server pool do not oversubscribe | doctool dump [verified default; advice inferred] |
| `application/run/max_fps` | 0 | dedicated: set e.g. 10–30 so the headless main loop does not spin | doctool dump [verified default; advice inferred] |
| RenderingServer | — | server thread never touches it; `is_on_render_thread`, `call_on_render_thread`, `force_sync` exist | API [verified] |
| NavigationServer3D | `navigation/world/map_use_async_iterations` true, `navigation/pathfinding/max_threads` 4, baking multi-threaded | docs: "NavigationServer2D and NavigationServer3D are both thread-safe and thread-friendly … query functions can be called by threads" → bots on the server thread may call `query_path`; `bake_from_source_geometry_data(_async)` + `NavigationMeshSourceGeometryData3D.add_faces/append_arrays` take raw triangles | https://docs.godotengine.org/en/latest/tutorials/performance/thread_safe_apis.html [verified]; API [verified] |
| servers in general | — | "Physics simulation is not thread-safe by default"; "Interacting with the active scene tree is not thread-safe" | same docs page [verified] |

For a Godot-independent `bf3sim` (testable without Godot, reusable in a standalone server), Detour from
`C:\Users\olku\recastnavigation` linked directly is the cleaner path for server-side queries; NavigationServer3D remains
fine for baking/caching [inferred; the bots topic decides].

### 6.3 Dedicated server: `godot --headless`

* `--headless` = audio driver `Dummy` + display driver `headless` (`main\main.cpp`:1464–1468); the headless display server
  offers only the `dummy` rendering driver and calls `RasterizerDummy::make_current()`
  (`servers\display\display_server_headless.h`:44–48, `.cpp`:40) [verified].
* Physics and navigation servers are chosen by project settings, not by headless; NavigationServer3D falls back to a dummy
  only when no server is registered (`servers\navigation_3d\navigation_server_3d_manager.cpp`:40–60) [verified].
* So yes: a headless server can load collision (our Jolt, CPU only) and bake navigation (CPU) from BF3 data with no GPU.
  It must skip textures, meshes, Enlighten, audio and scatter entirely — server mode loads only collision, gameplay EBX,
  destruction tables and nav — which also avoids the ~1.5 GiB decoded image set (handoff §0) [inferred].
* Entry point: a GDExtension `MainLoop` subclass (`MainLoop` virtuals `_initialize/_physics_process/_process/_finalize`
  [verified API]) selected by `application/run/main_loop_type`, or a single root node; it starts the same server thread as
  the listen host. The server operator needs their own BF3 install (read as-is) [inferred, follows from the core rule].
* Escape hatch: because `bf3`, `bf3sim`, `bf3net`, `bf3server` are Godot-free, a plain C++ console server is a link step
  away if Godot-headless overhead or Linux packaging ever hurts; the user decides whether that still counts as "in Godot".

### 6.4 GDExtension thread lifetime

Start the server thread from the session node; stop and **join** it in the node's exit/`NOTIFICATION_PREDELETE` and again
in the extension's deinitialisation (`GDEXTENSION_INITIALIZATION_SCENE`, `gdextension_interface.h`:696–699) before Godot
singletons go away; the server thread calls no Godot API except optional thread-safe NavigationServer3D queries and
logging [inferred].

---

## 7. Replication design

### 7.1 Channels (ENet)

| ch | flags | direction | carries |
|---|---|---|---|
| 0 | unreliable, unsequenced | S→C | snapshots (one datagram per snapshot, ≤ 1,200 B payload) |
| 0 | unreliable, unsequenced | C→S | commands (60 Hz, 3–4 redundant), hit reports ride along (also resent until acked by snapshot) |
| 1 | reliable ordered | both | control + events: hello/auth/match info/ready, spawn/loadout, kill feed, damage confirms, score/tickets, chat, admin, level change |
| 2 | reliable ordered | S→C | bulk: join-time full destruction rows, scoreboard dump; separate so it never blocks ch 1 |

Keep snapshot datagrams under the ENet MTU (1392) to avoid unreliable fragmentation; Godot itself warns about that case
(`enet_multiplayer_peer.cpp`:361) [verified]; 1,200 B also fits the IPv6 minimum MTU [inferred].

### 7.2 Quantisation (per field group; all integer bit-packed)

| field | encoding | bits |
|---|---|---|
| position x/z | level-relative, 1/256 m, ±2,048 m (absolute) or small delta vs acked baseline | 20 abs / 1+10 delta |
| position y | 1/128 m, −256…+1,792 m (jets) | 18 abs / 1+9 delta |
| soldier yaw / pitch | uniform | 14 / 12 |
| soldier velocity | 1/32 m/s, ±32 m/s (or derived from position deltas) | 3×11 |
| stance, sprint, ADS, firing, reloading, weapon slot, health | enums/flags | ~24 |
| vehicle orientation | smallest-three quaternion | 2+3×11 = 35 |
| vehicle linear / angular velocity | 1/64 m/s ±512 m/s / 1/256 rad/s ±16 | 3×16 / 3×13 |
| turret yaw/pitch per seat | uniform | 12+10 |
| health / damage state | per ghost class | 7–10 |
| destruction row | slot delta varint + state | ~2 B (handoff 04 §5.6) |

Typical sizes: moving soldier delta ≈ 10–14 B, full ≈ 17 B; moving vehicle ≈ 25–35 B (IW4L sent 180 B unquantised);
parked vehicle 0 after first send [inferred].

### 7.3 Ghosts, scope, priority (interest management for 64 players on a 2 km map)

* **Ghost** = a replicated object with a stable id: soldier slots 0–67 by player slot (`ClientId, LifeSequence`);
  vehicles by spawner index from the level data (identical on both machines, digest-protected, like `PartComponentIndex`);
  projectiles/pickups dynamic `(number, generation)`; one "world" ghost for flags/tickets/destruction rows.
* **Per client, per ghost**: scope flag, priority accumulator, last-acked state (per field group), and a packet→contents
  record. On ack, contents become the new baselines; on loss the groups are marked dirty again and the **latest** state is
  sent (never stale resends). This is the Tribes ghost model Frostbite's naming points to [inferred lineage].
* **Priority** per tick: `acc += base(class) × dist_factor × view_factor × event_boost`; dist tiers in the spirit of BF4's
  "bubble" and BF3's distance-reduced updates: ≤ 100 m every snapshot (30 Hz), 100–300 m ~10–15 Hz, 300–800 m ~5 Hz,
  beyond / occluded 1–2 Hz minimap fidelity; boosts for ghosts within the aim cone while zoomed (snipers), shooting at or
  damaging this client, same vehicle, same squad. Fill the datagram in accumulator order until the 1,200 B budget is spent;
  sent ghosts reset their accumulator [inferred design].
* **Relevancy**: all teammates and all vehicles at low rate (minimap/compass; BF3 shows vehicles on the compass, handoff 04
  §9); enemies only when within ~1.5 km or spotted; never send what the client cannot use (reduces wall-hack value).

### 7.4 Per-client bandwidth estimate (Caspian, 64 players, ~40 vehicles of which ~15 moving)

| component per 30 Hz snapshot | count × size × rate share | bytes |
|---|---|---|
| headers (ENet ~10 + tick, seq, cmd ack, buffer hint, ack bits) | — | 24 |
| soldiers ≤ 100 m | 12 × 13 B × 1 | 156 |
| soldiers 100–300 m | 25 × 15 B × ⅓ | 125 |
| soldiers > 300 m | 26 × 17 B × ⅙ | 74 |
| moving vehicles | 15 × 30 B × ½ | 225 |
| projectile ghosts (rockets, shells, grenades) | 5 × 18 B | 90 |
| world ghost (tickets, flags, destruction rows) | average | 30 |
| **average** | | **≈ 720 B → 21.6 KB/s ≈ 173 kbit/s** |
| **cap** (budget-full datagrams) | 1,200 B × 30 Hz | **36 KB/s ≈ 288 kbit/s** |

Plus reliable events ≈ 0.5–2 KB/s. Upstream per client ≈ 60 × (28 B UDP/IP + ~50 B) ≈ 4.7 KB/s. Server upstream with 63
remote humans: ≈ 1.4 MB/s average, ≤ 2.3 MB/s (≈ 18 Mbit/s) at the cap. A home listen server with 31 remote humans needs
≈ 9 Mbit/s up at the cap; **bots cost no bandwidth**. For comparison BF3 capped ~128 kbit/s per client at 10 Hz and DICE
said BF4's HIGH setting needs ~1 Mbit/s [verified sources; all our numbers inferred, to be measured in N3].

### 7.5 Destruction state replication (handoff 04 §5.6, adapted)

* State = sorted `(slot u32, state u8)` rows for slots out of their initial state; health stays on the server; slot numbering
  = `PartComponentIndex` order, identical on every machine (handoff 04 §5.2) [verified there].
* Carried in the world ghost as "rows changed since this client's acked baseline" (reliable by ack, so the 200-tick full
  resync of IW4L is unnecessary); a 45-slot RPG burst ≈ 90 B [verified estimate in handoff].
* Late join: the full row set (worst case 13.8 k rows ≈ 28 KB) on reliable bulk ch 2, then deltas.
* Clients adopt rows before prediction, update the prediction world's destructible compound shapes (Jolt
  `MutableCompoundShape` add/remove, handoff 04 §5.3), and derive FX/sound/debris locally from transitions with
  `seed = hash(slot, state, server_tick)` so killcams, demos and late joiners agree without events [verified design in handoff].
* `OnDestroyed` wiring delays (`SequenceEntityData` timings, random delays) run on the server; their results arrive as rows.

### 7.6 Handshake: proving identical data without touching the files

```
C→S ch1  Hello      {proto_ver, game_build, sim_digest_scheme, client_pubkey, name}
S→C ch1  Challenge  {nonce, server_pubkey}
C→S ch1  Auth       {sign(nonce)}                      ; identity only (§8.3)
S→C ch1  MatchInfo  {level "Levels/MP_Subway/MP_Subway", layer/mode "TeamDeathMatch0", sim_hz, snap_div,
                     required_xpacks bitset, source_digest, sim_digest, per-component digests, round_id, client slot}
         client mounts its own install read-only, computes/loads digests (cached by toc/sb/cat path+size+mtime),
         converts or loads user://cache/<source_digest>/, compares
C→S ch1  Ready      {source_digest, sim_digest, component digests}   or  Mismatch{component list}
S→C ch2  FullSync   {destruction rows, scores, flags}  then ghosts start on ch0 (no baselines → full states, priority-paced)
```

* **source_digest** = hash over the sorted `(bundle name, bundle digest)` of every bundle the level+mode mounts (base +
  patch + xpack layering as in handoff 01), where bundle digest = hash of its manifest entries `(ebx name, sha1)`,
  `(res name, resType, sha1)`, `(chunk id, sha1)`. These SHA1s are already in the toc/sb metadata (handoff 01 lines
  91–116), so no payload is re-hashed and nothing is written [verified that the fields exist; scheme inferred].
* **Exclude language/VO/localisation bundles** so an English and a German install can play together; they never affect the
  sim [inferred].
* **sim_digest** = hash of the derived tables the server simulation uses: collision soup (quantised vertices, materials,
  piece ids), destruction slot table, spawns/flags/combat area, vehicle spawners, soldier/weapon/vehicle/projectile stats,
  converter version. Must match exactly; it is what IW4L's map digest was missing (collision) [verified lesson].
* Reject reasons are explicit (protocol, missing XP pack, patch mismatch, modified install); per-component digests say
  which bundle differs. A Frosty-modded install simply fails the check — we never "fix" it.
* Level change: `LevelChange{level, mode, sim_digest, round_id}` on ch1; clients unload, load from their own install, send
  `Ready`; server waits ≤ 120 s (IW4L `LOADING_DEADLINE_MS`, `net\src\session_core.rs`:56) [verified].

### 7.7 Message format sketch

```
Snapshot (S→C, ch0)          bit-packed
  u16 snap_seq | u32 server_tick | u16 last_cmd_seq_consumed | i8 cmd_buffer_ticks | u16 rtt_echo
  own-state block (full precision, at last_cmd tick) : soldier or vehicle controller state, weapon state
  varint n_ghosts
    per ghost: idx delta (varint) | op {update, create(class, create-args), delete}
               group mask (class-specific) | quantised fields (delta vs acked baseline or absolute)
  world block: tickets, flag owners/progress, destruction rows delta (count, slot deltas, states)
  hit/damage confirms are NOT here (ch1)

Command packet (C→S, ch0)
  u16 newest_snap_seq_received | u32 snap_ack_bits | u16 first_cmd_seq | u8 n (3–4)
  per cmd: tick | buttons (24 bits) | yaw u16 | pitch u14 | move fwd/strafe i8,i8 | weapon slot u4
           [vehicle: throttle i8, steer i8, collective i8, yaw/pedals i8, roll i8, seat-specific bits]
           [fire: sub-tick fraction u8, spread seed index]  [render sample: snap tick + alpha u8]
  hit reports (until acked): fire_seq, bullet idx, target ghost, (tick_a, tick_b, alpha), hit part

Reliable (ch1): typed messages {u8 type, payload}, EventSequence u32 for dedupe in demos/killcam
```

---

## 8. Session layer

### 8.1 Server browser / master

Small C++ service (not shipped to players, but kept in C++ for the rule): servers send a UDP heartbeat every 30 s
`{name, ip:port, level, mode, humans, bots, max, sim_hz, proto_ver, required_xpacks, sim_digest, password?}`; clients fetch
the list (HTTP JSON or UDP) and ping each server directly for latency. No accounts, no history (IW4L's master stored
nothing on disk either, `C:\Users\olku\iw4L\docs\MASTER.md`) [verified]. LAN discovery by UDP broadcast
(`PacketPeerUDP.set_broadcast_enabled` exists [verified API]) or our own socket.

### 8.2 NAT traversal, in order

1. Dedicated servers on public IPs: nothing to do.
2. Listen host: UPnP port mapping through Godot's `UPNP` (`discover`, `add_port_mapping`, `query_external_address`)
   [verified API].
3. Master-introduced UDP hole punching: client asks master to introduce it to server X; master tells both public
   endpoints; both send a few packets from their **game socket** (raw `enet_socket_send` on the host socket; Godot's
   `ENetConnection.socket_send` shows the same capability [verified API]) then ENet connects. Fails on symmetric NAT/CGNAT
   [inferred].
4. Relay through the master as fallback (IW4L ran all traffic through its relay and it worked; budget ~1.5–2.3 MB/s per
   relayed 64-player server, so relays suit small matches) [verified IW4L design; cost inferred].
5. GNS + ICE only if 3–4 prove insufficient (needs STUN/TURN and a signaling service) [verified README].

### 8.3 Identity and authentication (simple)

Per-install keypair generated on first run and stored in `user://` (Godot `Crypto.generate_rsa` + `sign`/`verify`
[verified API], or Ed25519 via a small library); the server challenges with a nonce; bans/admins keyed by public-key
fingerprint; optional server password. **No EA/Origin accounts, no ownership checks, no contact with EA services** — owning
the install is what makes the game run at all.

### 8.4 Anti-cheat stance

Server authority first: movement from inputs only (no teleport/speed), fire rate/ammo/reload/spread server-side, damage and
health server-side, hit reports validated against server-side bullets and history (§5.4), rewind cap, relevancy culling,
rate limits on commands/reports (IW4L's per-peer caps). Admin tools (kick, ban, vote-kick), server-side demos for review.
No kernel anti-cheat; aim assistance by cheats cannot be prevented by design [inferred].

### 8.5 Replays and killcam

* Demos: the server records a spectator stream (all ghosts at 15–30 Hz, delta-coded against the previous frame) plus
  reliable events into `user://replays/` with a header `{proto_ver, source_digest, sim_digest, level, mode, sim_hz,
  root_seed}` (IW4L's `MatchRecordIdentity`) [verified IW4L]; playback = a client in spectator mode that loads the level from
  its own install, checks digests and feeds the decoder.
* Killcam: BF3's is a camera move to the killer's position with the killer's health, not a replay of the killer's view
  [report claim — general BF3 gameplay knowledge, not re-checked]; needs only a high-priority killer ghost for a few
  seconds. IW4L's CoD-style archive killcam is not needed.
* Voice: out of scope.

---

## 9. C++ module layout

```
bf3/            (data topics) containers, EBX, resources, level → scene; Godot-free
bf3sim/         Godot-free authoritative simulation
  clock/        tick, sim_hz, integer time
  collision/    own Jolt: static shapes (shareable), destructible MutableCompound, layers (STATIC, PLAYER_CLIP,
                VEHICLE_CLIP, WATER, GLASS, DESTRUCTIBLE), queries
  soldier/      replayable controller (soldier topic), hit capsules
  vehicle/      Jolt bodies + VehicleConstraint, tracked/heli/jet/boat models, SaveState/RestoreState
  weapons/      fire, seeded spread, bullets/projectiles, damage + MaterialRelationDamage (weapons topic)
  destruction/  slots, states, health, OnDestroyed wiring, collapse
  rules/        TDM, Conquest, Rush; spawns, tickets, flags
  bots/         perception, tactics, nav (Detour or NavigationServer3D) → BotCommand = same Command type
  lagcomp/      per-tick history rings, sample reconstruction
bf3net/         Godot-free
  transport/    ENet host/peer wrapper, loopback transport, netsim (BF3's knobs: latency min/max, drops,
                drop spikes, reorder, duplicate, corrupt), stats
  bits/         bit writer/reader, quantisers, varint, smallest-three
  proto/        message types, versioning, Hello/Challenge/MatchInfo/Ready/LevelChange, Command, Snapshot, events
  ghost/        server GhostManager (scope, priority, baselines, ack records); client GhostReceiver
  session/      handshake, digests (source/sim/component), time sync, auth
  replay/       recorder, reader
bf3server/      ServerHost: thread + fixed-tick loop, client sessions, bot fill, job pool, admin, master heartbeat
bf3client/      ClientSession: connect, command build, prediction/reconciliation, interpolation buffers,
                presentation state (double-buffered for the main thread)
bf3_godot/      GDExtension: GameSession node, ListenServer (owns ServerHost thread), input → Command, presentation
                (RenderingServer instances, Skeleton3D, audio), HUD bindings, DedicatedMainLoop (--headless),
                browser/UPnP glue, debug overlay (RTT, loss, buffer, bandwidth per channel)
tools/master/   C++ heartbeat lister + introducer + optional relay
tools/loadbot/  headless fake clients (bf3net + bf3client without rendering) for 64-client load tests
```

---

## 10. CPU budget (to be measured; numbers are estimates)

| per 60 Hz server tick, 64 soldiers (mostly bots), 40 vehicles (~15 active) | estimate |
|---|---|
| commands + 64 soldier controller steps | 1–2 ms |
| Jolt step (active vehicles, debris off on server) | 0.3–1 ms |
| bots (staggered thinking at 5–10 Hz each, parallel) | 1–2 ms wall on the pool |
| destruction + rules + lag-comp history write | < 0.5 ms |
| ghost scoping/encode for 64 clients (every 2nd tick, parallel) | ~1.3 ms per snapshot tick |
| **total** | **≈ 4–6 ms of 16.7 ms** → one core plus pool help |

IW4L measured 0.36 ms snapshot fan-out for one client and 0.2–2 ms bot think per 20 Hz tick for 16 bots (measure.md §3.2)
[verified]; DICE's own 2015 guidance was one 64-player instance per machine at ≤ 60 Hz [verified]. If the listen host
cannot hold 60 Hz with 64 slots, drop to 30 Hz sim (same feel by §4.1) [inferred].

---

## 11. Build order (each step playable and verified in the running game)

| step | content | done when |
|---|---|---|
| **N1 loopback** | Metro TDM, listen host only: ServerHost thread at 60 Hz, local client over loopback, capsule soldier (soldier topic), 8 bots; snapshots interpolated; then own-soldier prediction + reconciliation | you run and shoot bots smoothly; netsim at 150 ms/2 % loss shows corrections but no rubber-banding at rest |
| **N2 two machines** | ENet transport, handshake with digests, 2 human clients on LAN + bots on Metro TDM; weapons with seeded bullets, hit reports + server validation, kills/score on ch1; dedicated `--headless` variant of the same match | two players on two PCs kill each other with correct hit-reg at 100 ms simulated RTT; a modified/mismatched install is rejected with a reason |
| **N3 scale soldiers** | quantisation, ghost priority/relevancy, budgeted datagrams, bandwidth/RTT overlay, loadbot to 32 then 64 clients on Metro/Bazaar; master list + browser + UPnP | 64 connections (bots + loadbots + 2 humans) hold ≤ 36 KB/s/client and the server tick < 8 ms |
| **N4 vehicles** | Caspian: jeep → LAV/tank (gunners) → heli → jet → boat; driver prediction with Jolt save/restore, passengers, vehicle ghosts, entry/exit on server | drive/fly at 100 ms RTT without visible corrections on flat ground; passenger and gunner see consistent motion |
| **N5 destruction** | slot rows in the world ghost, late-join full sync, prediction world collision updates, client-derived FX/debris | an RPG hole appears identically for a late joiner; walking through a fresh hole predicts correctly |
| **N6 64 on Caspian** | Conquest Large, 64 slots (bots fill), all vehicles; parallel encode, priority tuning, hole punching + relay, demos | a full 64-slot round runs to the end with the tick budget and bandwidth inside §7.4/§10 |

---

## 12. What I could NOT determine

* Which of the two BF3 rate pairs is effective at runtime: `Server.OutgoingFrequency 10` / `Client.IncomingFrequency 15`
  (Game.cfg) vs the EBX defaults 15 / 10, and whether retail dedicated servers (separate binaries, own config) overrode them.
  BF3's client→server send rate is not in either file. No first-hand BF3 measurement (packet capture) was made.
* Units of `OutgoingRate`/`IncomingRate` (bits vs bytes per second) and the exact meaning of `MaxServerFrameSize`,
  `UseFrameManager`, `MaxGhostCount` (per connection or global), `TimeNudgeBias`, `TimeNudgeGhostFrequencyFactor`,
  `AuthoritativeEyePosition`, `MaxAllowedLatency` (cap on client damage or on connection).
* BF3's interpolation delay and how distance tiers map to update rates; VU's statement about distance-reduced updates is a
  report claim. No DICE talk on Frostbite 2 networking was found (searches returned none).
* BF3's actual per-client bandwidth on a full 64-player server (no capture); a forum-quoted Battle(non)sense figure of
  ~27 KB/s at 30 Hz for BF4 was secondhand and not used.
* Whether Godot 4.8's bundled Jolt version and our linked Jolt can coexist in one process on Linux without symbol
  interposition (Windows DLLs should be fine); needs a link test.
* Real CPU costs in §10 and bandwidth in §7.4 — estimates until N3/N6 measure them.
* Whether `ENetConnection` from Godot may be serviced from a non-main thread safely in all cases (no locks seen; not tested);
  the design avoids it by linking ENet directly.
* Jolt `CharacterVirtual` replay determinism across many corrections (broadphase result order is not deterministic per Jolt
  docs); the soldier topic must test bit-exactness or accept ε-tolerant reconciliation.
* Exact list of bundles that are language-specific (needed for the digest exclusion list) — the data topic should enumerate
  them from the layout.

---

## Independent verification (2026-10-10)

Done by a separate verifier who tried to refute each claim. Godot source = a full checkout at the exact build commit
(`git rev-parse HEAD` = `c971f93e7e76b0ef919bf6009e7b868bea04db7f`, `…\scratchpad\godot-engine\godot-src`), read with
`git show HEAD:<path>` so edits to the working tree cannot affect it; written below as `G:`. Install reads were read-only (the file
was opened `rb`, nothing written; scripts in `…\scratchpad\verify-networking\`). Web sources were re-downloaded or re-fetched. Verdicts:
**confirmed** / **corrected** (partly wrong; the corrected statement is given) / **refuted** / **unverifiable**.

| claim id | claim (short) | verdict | evidence (with source) | correction |
|---|---|---|---|---|
| K1 | BF3 fixed 30 Hz sim (`MaxSimFps 30`, `VariableSimTickTimeEnable false`, `ClampTicks 2`, `MaxVariableFps 200`), `VisualFrameInterpolation true`; VU "regular" = 30 Hz | confirmed | `…\bf3dump\ebx\levels\mp_subway\mp_subway\settings_win32.json`:432–434, 442 [verified]. Game.cfg `Client.VisualFrameInterpolation true` found both in the IW4L dump (:130) and in my own read-only decode of `…\Battlefield 3\Update\Patch\Data\initfs_Win32` (`initfs_grep2.py`) [verified]. The EBX `ClientSettings.VisualFrameInterpolation` default is **false** (json:2147), so the cfg override is the value that applies. No cfg in that initfs overrides `GameTime.*` except `DebugSettings.lua`:73 (`ForceUseSleepTimer`) and a commented line in `LocalE3Win32.cfg`:9 [verified]. VU `hosting\commands.md`:75: "For 30Hz it returns `regular`" [verified doc] | none. Reading `MaxSimFps` as the sim rate is still [inferred] |
| K2 | Server sends ~10 Hz (`Server.OutgoingFrequency 10`, `Client.IncomingFrequency 15`); rates 128000/48000, `MaxServerFrameSize 2048`, `MaxGhostCount 8192`, `MaxClientCount 68` | confirmed | Same values in my install decode and in the dump Game.cfg:109–115, 356–359 [verified]; EBX defaults are 15.0 (json:942) and 10.0 (json:2087) [verified]. Not in the report: the internal `Scripts/PlaytestCommon.cfg` raises `Server.OutgoingRate`/`Client.IncomingRate` to 150000 (a commented line has 512000) and keeps `MaxServerFrameSize 2048`; the non-Win32 dedicated branch sets `MaxClientCount 24` (Game.cfg:686) [verified] | none. Which frequency wins, and the rate units, are still open |
| K3 | Client-side hit detection arbitrated by the server (`AllowClientSideDamageArbitration true`, `MaxAllowedLatency 0.85`); VU `isClientDamage` | confirmed (values) | json:2157 `MaxAllowedLatency 0.85`, :2163 `AllowClientSideDamageArbitration true` [verified]; `VU-Docs\content\vext\ref\server\type\damageinfo.md`:33 `isClientDamage` bool [verified]. A web search found no DICE primary source on BF3's hit model, only forum posts | The meaning ("the shooter's client decides, the server arbitrates under a latency cap") is still [inferred] from the field names |
| K4 | Own soldier and own vehicle predicted (`UsePrediction true`, 16 vehicles incl. F-35B; `LocalVehicleSimulationEnabled true`; `RadiusToPredictCollisionOnCharacters 0.0`) | confirmed | My script (`veh_pred.py`) over the 15 `json_survey\_veh\*.json` files plus `bmp2.json`:13614: `VehicleEntityData.UsePrediction` is true on all 16 [verified]. A second, independent dumper (EBX probe, `…\scratchpad\probe-ebx\out4`) also gives true for AH-1Z (`vehicles__ah1z__ah1z.json`:13380), M1 Abrams (`…m1abrams.json`:18899) and the armoured Humvee (:12478). mpsoldier `UsePrediction true` (IW4L `mpsoldier.json`:9971; probe `out2\…mpsoldier.json`:12735). `RadiusToPredictCollisionOnCharacters 0.0` (IW4L `_byguid\235cd1da…`:533; probe :26550). `LocalVehicleSimulationEnabled true` json:2143 [all verified] | Adds an attack helicopter and an MBT. The Su-35/F/A-18 jets were not checked |
| K5 | BF4 normally 30 Hz, beta up to 144 Hz, DICE: at most 60 Hz and one instance per machine for 64 players; per-30 Hz tuning bugs | confirmed | The PDF re-downloaded today is byte-identical (253,852 B); metadata CreationDate 2015-09-24. Text: "Normally, servers in Battlefield 4 run at 30hz", "All the way up to 144hz", "For 64 player servers we recommend a single instance per hardware, limiting max tickrate to 60hz", 48/32 slots ≤ 120 Hz, < 20 ≤ 144 Hz, sprint / spread-recoil / mouse flying "not 1:1 with 30hz" [verified] | none |
| K6 | Jolt is always in `PhysicsServer3DWrapMT`; `space_get_direct_state`/`body_test_motion`/`body_get_direct_state` main-thread only; value-returning calls from other threads block until the main thread flushes | confirmed | G:`modules/jolt_physics/register_types.cpp`:42–52. G:`servers/physics_3d/physics_server_3d_wrap_mt.h`:121–124, 273–282 have an unconditional `ERR_FAIL_COND_V(!Thread::is_main_thread(), …)`. `FUNC*R` → `push_and_ret` when caller ≠ `server_thread` (G:`servers/server_wrap_mt_common.h`:44–56, 153–165). With the default setting `server_thread = Thread::MAIN_ID` (G:`physics_server_3d_wrap_mt.cpp`:96–98), and the queue is flushed in `sync()` (:68–75) or by any main-thread call (`flush_if_pending`). The waiter blocks on a condition variable (G:`core/templates/command_queue_mt.h`:215–223, 245–250) [verified] | Nuance: with `run_on_separate_thread = true` these calls wait for the physics pump task, not the main thread. The gate applies to *obtaining* a direct state, not to queries through one (extra-1) |
| K7 | `space_create()` = separate `PhysicsSystem`; no way to step one space; all active spaces stepped with one delta after `_physics_process` | confirmed | G:`modules/jolt_physics/jolt_physics_server_3d.cpp`:183–195 (`memnew(JoltSpace3D…)`); G:`spaces/jolt_space_3d.cpp`:112–119 (`new JPH::PhysicsSystem()` + `Init(max_bodies,…)` per space); `step()`:1643–1655 loops `active_spaces` with the same `p_step`; G:`main/main.cpp`:5017–5018 `sync`/`flush_queries`, :5028 `physics_process`, :5061–5062 `end_sync` + `step` [verified]. API dump: `PhysicsServer3D` has no `step`, only `space_set_active`/`set_active`; `_step` exists only on `PhysicsServer3DExtension` [verified] | Nuances: a space with `space_set_active(false)` is never stepped, so a second Godot space could serve as a static query world, but never as an independently stepped server world. With the separate-thread option, `step` runs on the pump task, not the main thread |
| K8 | SceneMultiplayer: full state per synchronizer each interval, unreliable, 8 B each; only bool/int compacted; on-change reliable; split 1350 B, oversize dropped; path ids; main thread | confirmed | G:`modules/multiplayer/scene_replication_interface.cpp`:143–153 (per peer `_send_sync` + `_send_delta`). :802–853 `_send_sync` does a full `get_state` of every sync property, 4 B `net_id` + 4 B size per synchronizer, and drops an oversize state with `ERR_CONTINUE_MSG` (:833). `_send_raw(…, false)` → `TRANSFER_MODE_UNRELIABLE` (:457–465). `.h`:93–94 `sync_mtu 1350`, `delta_mtu 65535`. `_send_delta` :710–765 is reliable. Path ids are confirmed through `_verify_synchronizer` :683–697. G:`scene/main/multiplayer_api.cpp`:66–133: only BOOL/INT are compacted, everything else goes through `encode_variant` with a 4 B header. Polling happens in `SceneTree::process` (G:`scene/main/scene_tree.cpp`:707–708) [verified] | Nuance: the on-change path sends only the watched properties that changed (`get_delta_state`), so it is a real delta, but it is reliable and costs 16 B per synchronizer |
| K9 | ENet 1.3.18, MTU 1392, ≤ 255 channels, ≤ 4095 peers; 2 system channels; mode mapping; `socket_send`, compression, DTLS; no locking | confirmed | G:`thirdparty/enet/enet/enet.h`:30–32, 222; G:`protocol.h`:18–19 (`MAXIMUM_PEER_ID 0xFFF`); G:`thirdparty/README.md`:243–258 (upstream `2662c0de`, one patch `0001-godot-socket`); G:`modules/enet/enet_multiplayer_peer.h`:48–52; `.cpp`:339–358 mapping, :360–364 MTU warning (DEBUG builds only). API: `ENetConnection.socket_send`, `compress` (NONE/RANGE_CODER/FASTLZ/ZLIB/ZSTD), `dtls_server_setup`/`dtls_client_setup`. No `Mutex` in `modules/enet` (grep) [verified] | Add: Godot's socket shim ignores the socket buffer sizes (extra-2) |
| K10 | `--headless` = headless display + dummy rasterizer + Dummy audio; physics/navigation not swapped, so a GPU-less server can build collision and bake nav | confirmed | G:`main/main.cpp`:295–297 (`NULL_DISPLAY_DRIVER "headless"`, `NULL_AUDIO_DRIVER "Dummy"`), :1464–1467. G:`servers/display/display_server_headless.h`:44–48 (only `"dummy"`), `.cpp`:40 `RasterizerDummy::make_current()`. G:`servers/navigation_3d/navigation_server_3d_manager.cpp`:40–60 (the dummy is only a fallback) [verified]. Hands-on: the GDExtension probe ran Jolt queries and NavigationServer3D bakes headless (`research\probes\godot-gdextension-probe.md`:216–225, 265–278) [verified probe] | none |
| K11 | NavigationServer3D thread-safe with queries callable from threads; physics and the active scene tree not thread-safe by default | confirmed | Fetched https://docs.godotengine.org/en/latest/tutorials/performance/thread_safe_apis.html: "NavigationServer2D and NavigationServer3D are both thread-safe and thread-friendly"; "The navigation-related query functions can be called by threads and run in true parallel"; "Physics simulation is not thread-safe by default"; "Interacting with the active scene tree is not thread-safe" [verified] | Caveat: the same page says the separate-thread setting makes physics "thread-safe", but the 4.8 source still restricts direct-state access to the main thread (K6). The probe found that the async bake callback runs on the main thread |
| K12 | Jolt deterministic with the same API-call order; `SaveState`/`RestoreState` + `StateRecorderFilter`; broadphase query order and callbacks not deterministic | confirmed | `Docs/Architecture.md` re-downloaded from raw.githubusercontent.com (identical to the earlier copy). :787–790: deterministic if API calls come in the same order **and the same binary code runs**. :792: `CROSS_PLATFORM_DETERMINISTIC` (~8 % slower) for cross-compiler/OS determinism. :804: broadphase queries are not deterministic. :806: listeners are called from many threads. :840: rollback via `SaveState`/`RestoreState`, `StateRecorderFilter::ShouldSaveBody`, and `RestoreState` does not work if bodies or constraints were added or removed in the window. :851: constraint-order caveat [verified] | Caveats the report omits: a Windows client and a Linux server need `CROSS_PLATFORM_DETERMINISTIC`. The client prediction world (one dynamic body, the others kinematic) is not the server world, so replay matches the server only within a tolerance. Reconcile with an ε, not bit-exactness [inferred] |
| K13 | Server links its own Jolt on a `std::thread` (listen) or under `--headless`; the prediction world shares static shapes; Godot physics only for cosmetic debris and ragdolls | corrected | Premises hold: K6, K7 and K10 are confirmed. Jolt docs :853–866 allow several `PhysicsSystem`s in one process that share only `Shape`s; Factory, default material and allocator hooks are global. Godot bundles Jolt 5.6.0 (G:`thirdparty/README.md`:512–516), and the newest upstream tag is v5.6.0 (git ls-remote). The API dump has 0 matches for "Jolt" [verified] | (1) Godot's physics *can* be queried from worker threads inside a window the main thread opens: the probe (`godot-gdextension-probe.md`:128–146) measured 3.8–3.9k rays/ms on 4 threads with no errors. The own-Jolt choice rests on independent stepping and timing (K7), not on a query ban. (2) Using Godot physics for ragdolls/debris needs a **second copy of the static level collision** inside Godot's space, because shapes cannot be shared across the two Jolt copies (the probe measured 66–82 ms to attach a 100k-triangle body). Run cosmetics and ragdolls in the extension's own client Jolt world too, which keeps one copy of the static shapes per process. (3) Initialise the extension's Jolt globals once and share them between the server and client worlds of a listen host [inferred] |
| K14 | 60 Hz sim / 30 Hz snapshots, ≤ 1,200 B → ~22 KB/s avg, 36 KB/s peak per client; server 11–18 Mbit/s for 63 humans; bots cost no bandwidth | corrected | Recomputed: the rows sum to 723.7 B → 21.7 KB/s (173.7 kbit/s); cap 36.0 KB/s; 63 clients 10.9 Mbit/s average and 18.1 Mbit/s (2.27 MB/s) at the cap; 31 clients 8.9 Mbit/s at the cap. The arithmetic is right. The DICE "faster than 1Mbit upload and 1Mbit download should be fine using the HIGH setting" was re-read on the EA news page [verified] | (1) The downstream numbers leave out UDP/IP (28 B) and ENet (~12 B) per datagram, which the upstream number includes: the cap is ≈ 37.2 KB/s ≈ 298 kbit/s on the wire. (2) Bots are not free downstream. Every bot is a soldier ghost in each human's snapshot (already inside the 63 soldier rows); what they don't need is a connection or upstream. (3) Every input is an unmeasured assumption, so the result is an estimate [inferred] |
| K15 | Digest of manifest SHA1s per level+mode (minus language bundles) + digest of derived sim tables incl. collision, "which IW4L's digest left out" | corrected | SHA1 fields exist in CAS manifests (`ebx/res/chunks` `sha1`) and in binary manifests (`sha1[totalCount]`) (handoff `01-containers-ebx.md`:107–116, 136). Language data lives in separate `Loc/*` superbundles (:97), so excluding it is feasible. IW4L `net\src\transport\protocol.rs`:15–24 `ContentFingerprint`, :111 `HandshakeHello` [verified] | (1) **Stale**: IW4L's final `content_digest` *does* hash collision. `clip_world_digest` covers the brushes, the clip mesh (verts, indices, walkable/surface/content flags, AABB tree, partitions, borders) and the destruction digest (`C:\Users\olku\iw4L\crates\sim\src\content.rs`:208–222 and following, `world.rs`:357–361, used at 1389–1402). The omission held only at research time (`1-RESEARCH-PART\05-iw4l-vehicles-sim-net.md`:899). (2) Manifest SHA1s prove that the manifests match (patch level, xpacks), not that the payload bytes do, and a client can echo any expected digest. This is a consistency check, not anti-cheat. Payload integrity would need the stored bytes hashed on read (the catalog SHA1 is the SHA1 of the stored bytes, handoff :116 [report claim]). (3) Delta-patched bundles (XP2–4 levels, `Loc/*`) need base+delta reconstruction before their SHA1 table exists. All of it stays read-only, so the core principle holds |
| K16 | IW4L lag comp (client reports left/right/alpha; rewind ≤ 4 ticks) worked; reliable queue silently dropped events beyond 64 | corrected | `crates\sim\src\bullet_collision.rs`:37 (32-tick history), :39 `LAGCOMP_MAX_REWIND_TICKS 4`, :42–47 `ShotSampleProvenance{left,right,alpha,quality}`. `world.rs`:1869–1918: samples older than current−4 are *clamped*, while malformed and after-shot samples are refused. `net\src\transport\reliable.rs`:6 `MAX_PENDING_RELIABLE 64`; :77–81 refuses the **new** row (returns 0) and increments `dropped_oldest` [verified] | Not silent: `dropped_oldest` is serialised (`protocol.rs`:551, `frame.rs`:103), and the client fails the session with "ReliableControlOverflow: outcome unknown" (`net\src\client\runtime.rs`:610–613). The row lost is the newest one, despite the field name. I found no measurement showing the lag compensation "worked", only the code. The lesson (never cap reliable traffic) stands |
| K17 | GNS (BSD-3, v1.6.1) AES-GCM + ICE, heavy deps; yojimbo (BSD-3, v1.13.5) single-threaded, ≤ 100 players, connect tokens; Steam needs an app ID and Steam-only relays | confirmed | git ls-remote today: GNS v1.6.1, yojimbo v1.13.5, netcode v1.4.8, reliable v1.4.5, ENet v1.3.18. GNS LICENSE is BSD-3 (Valve 2018). GNS README: AES-GCM-256 + Curve25519, lanes, IPv6, loss/latency simulation, ICE via WebRTC with your own signaling; Steam auth, signaling and SDR relays are Steam-only (README:60–63). GNS BUILDING.md: protobuf (22+ pulls in Abseil), one crypto backend (OpenSSL, libsodium or bcrypt), WebRTC for P2P. yojimbo README:17 connect tokens, :31 "100 players or less", :35 "single-threaded"; LICENCE BSD-3 (Más Bandwidth) [verified] | The Steam app-ID requirement remains [report claim] |
| extra-1 | §0/§6.1 "Godot refuses space/body queries off the main thread" | corrected | Only *obtaining* the state is gated (wrap_mt.h:121–124, 273–282). Probe `godot-gdextension-probe.md`:128–146: rays through a direct state obtained on the main thread ran on 1–8 `std::thread`s with no errors while the space was idle. During stepping, Jolt refuses about 1 query per step (`is_stepping` check, not a lock), and every query mutates the space (`flush_pending_objects`) [verified probe + source] | "Godot's physics can be queried off-thread only inside a window the main thread controls, and it cannot be stepped independently." The own-Jolt decision is unchanged |
| extra-2 | §0/§3.2 "Godot's `ENetConnection` is the same ENet … acceptable fallback" | corrected | G:`thirdparty/enet/enet_godot.cpp`:137–143 returns −1 for `ENET_SOCKOPT_RCVBUF`/`SNDBUF`, while G:`thirdparty/enet/host.c`:65–66 requests 256 KB (`enet.h`:219–220). The probe measured 39–94.5 % loss on unreliable localhost bursts (~66 KB delivered) and 0 % when paced [verified] | Same protocol, but a different socket layer (Godot sockets, IPv6, DTLS, no buffer sizing). The fallback works only with paced sends. Linking upstream ENet (the report's choice) gets the 256 KB buffers |
| extra-3 | §7.4 "BF3 capped ~128 kbit/s per client … [verified sources]" | corrected | Game.cfg:114 says only `Server.OutgoingRate 128000`, and the report's own §12 lists the units as unknown | "128000 per client in unknown units (128 kbit/s if bits)" [inferred] |
| extra-4 | §1.1 "64 players + 4 spectator slots [verified comment]" | corrected | Game.cfg:375–377: "// 68 = max regular players + max spectator players, see ClientLimits.h", then `Game.MaxPlayerCount 68`, `Game.MaxSpectatorCount 0` [verified] | The comment gives only the sum; the 64 + 4 split is [inferred] |
| extra-5 | §4.1 upstream "~3 KB/s per client" | corrected | §7.4 itself computes 60 × (28 + ~50) B = 4.68 KB/s (recomputed) | Use ≈ 4.7 KB/s in §4.1 as well |
| extra-6 | §10 "DICE's own 2015 guidance" | confirmed | PDF metadata CreationDate/ModDate 2015-09-24 [verified] | none |
| extra-7 | Core principle (BF3 files read as-is) across all recommendations | confirmed (no violation) | Every machine mounts its own install read-only. Caches, replays and keys live in `user://`. Digests come from manifest metadata with no writes. A modified install is rejected, never "fixed". The network carries no BF3 content. VU is used as documentation only. A dedicated operator needs their own install. The converted cache is keyed by `source_digest` (content-derived), as the rule requires | none. Only caution: the per-file digest cache (path+size+mtime) must re-hash whenever any of the three changes [inferred] |

Not determined by this verification:
* Whether `Server.OutgoingFrequency 10` or `Client.IncomingFrequency 15` governs retail traffic, the rate units, and whether retail PC dedicated servers (separate binaries) used this client initfs. No packet capture was made.
* Any DICE primary source on BF3's hit model; `AllowClientSideDamageArbitration` and `MaxAllowedLatency` are read from field names only.
* `UsePrediction` on the remaining jets and helicopters (Su-35, F/A-18, A-10, Su-25, Mi-28, Z-11, AH-6, KA-60, UH-1Y) was not dumped.
* Whether a second Jolt linked into the extension coexists with Godot's statically linked Jolt 5.6.0 without symbol clashes on Linux. No link test was run.
* Real bandwidth and CPU figures (K14, §10). These remain estimates until measured.
* Whether IW4L's provenance lag compensation was ever validated by a measurement; the journal has the code but no result.
