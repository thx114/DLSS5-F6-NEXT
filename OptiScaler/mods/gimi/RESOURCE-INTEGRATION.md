# R495 resource identity, preload and exact draw cache

2026-11-01. Implemented isolated source; not integrated or built. This is a resource/identity layer for the R478 command IR, not proof of MOD rendering, skeleton compatibility or GPU completion. R507 owns the separate executor. This module has no hook installation, proxy, HackerContext, loader or external DLL dependency.

## Files and stable executor interface

| File | Responsibility |
| --- | --- |
| GimiIdentity.h/.cpp | CRC32C, original descriptors, creation/current identity, event invalidation and off-draw CPU refresh |
| GimiFormats.h/.cpp | One explicit typed DXGI format and surface-size table shared by identity and preload |
| GimiDds.h/.cpp | Bounded DDS container/mip decode into actual DX11 subresource descriptions |
| GimiResources.h/.cpp | Atomic immutable CPU preload; actual immutable Buffer/Texture2D/SRV creation and retained COM leases |
| GimiDrawCache.h/.cpp | Exact layout qualification, offline index windows, bounded positive/negative matching cache, R478 query adapter |

`Dx11LeaseView` contains borrowed `ID3D11Resource* resource`, `ID3D11Buffer* buffer`, `ID3D11ShaderResourceView* srv`, effective `Binding binding`, and `ResourceIdentity identity`. It does not own those pointers. `shared_ptr<const Dx11StaticResources>` owns them.

`Dx11StaticResources::Resolve(Binding, Dx11LeaseView&) const noexcept` resolves a static object/view lease in O(1). It validates the reserved ID range, device generation, matching view ID and canonical size/format/stride; a buffer offset must remain in bounds. It does not AddRef, call GetDesc/GetResource, allocate, hash, upload or perform IO. `Get(Id, view)` is the Plan Resource ID lookup. `Initialize(CommandState&)` checks exact Plan ownership and no open/pending invocation before setting file leases and known-null aliases.

`Binding.object/view` are opaque, globally unique lease IDs, never COM pointers or resource hashes. `generation` is the device generation. The runtime reserves `[firstLease, firstLease + Plan.Resources().size())` in a global, never-reused ID domain, including holes for alias IDs. A static SRV uses the same numeric ID in the distinct view field. A null object and null view resolve to known-null. Unknown IDs return false; they do not become null.

The executor's typed resolver should return `Dx11LeaseView` plus its real retained owner. It must additionally resolve game/captured-resource leases through its existing owner table. Static `Resolve` intentionally has no registry for game resources. Validate effective IA ranges, destination type, view compatibility, shader/layout/skeleton contract, state restoration, and hazards before the first action. Do not derive a pointer from an ID or query the whole DX11 pipeline on every draw.

Keep the static set alive while any command alias, pending batch or submitted GPU work can refer to it. `Action::previous` permits explicit retain/retire bookkeeping without scanning all aliases. Releasing a CPU owner does not establish GPU completion. Partial submissions require the real executor's restoration/quarantine/retirement logic; never replay the original as though no effects occurred.

## Load and publication sequence

1. Compile the actual selected dependency closure with R478. Preserve the independently delivered R496 Present provenance fix. Unknown selected dependencies reject the entire Plan.
2. Call `PreloadResources(plan, limits)`. It checks the entire compiled required set, proves finite GPU IniParams reads/writes at their actual expression/command source (including ORFix x70), rejects unknown/unbounded parameter inputs, resolves transitive file-resource binding roles through alias assignments, and reads/decodes every required file. It publishes no partial set. Default limits are 256 MiB per file and 512 MiB total. Files must remain stable during load; published bytes are independent immutable snapshots.
3. Build `LayoutRequest` from the selected component's actual known input layout and exact upstream VS identity. Set `source` to the selected INI source and supply a unique immutable contract ID. `CompileLayout` rejects unsupported layouts. On a successful real game input-layout creation/capture, `LayoutContract::Qualify` compares all elements, semantic spelling case-insensitively, indices, slots, explicit offsets, formats, per-vertex class and step rates, together with the exact upstream VS hash. Retain the returned layout lease/contract/device tuple. No string comparison or layout compilation occurs during draws.
4. Call `CreateDrawCache` before GPU publication. It validates bounds/topology and rejects unimplemented ShaderOverride duplicate/overrule merging at the actual section source. This is a product capability check, not a test or certification framework.
5. On the device owner's safe initialization thread, with the existing internal-hook suppression active, call `CreateDx11Resources(files, device, deviceGeneration, firstLease, policy)`. It really calls CreateBuffer/CreateTexture2D/CreateShaderResourceView. Any error returns diagnostics and the actual HRESULT, destroys the unpublished staged COM objects, and returns no set. No resource is labelled game/GPU Ready from parsing alone.
6. Create a fresh `CommandState` with that exact Plan and initialize it from the static set. Publish the complete Plan/resources/cache/state/owner collection atomically at an existing safe boundary. Enable the consumer only after every required capability and executor preflight is available. Default enable remains the runtime's responsibility and must be false.

The CPU preload and DX11 creation are independent so device recreation can reuse the immutable file bytes. Resource files, layout catalogs and index windows are not rebuilt inside a draw.

## Actual resource formats

Raw file-backed Buffer supports explicit nonzero `stride`, or a supported typed `format` whose element size supplies the stride. ByteWidth must fit UINT and the payload must contain integral elements. Index roles require explicit R16_UINT/R32_UINT and matching stride. Vertex roles retain their exact declared stride. Typed buffer SRVs require a supported non-block format and integral element count; actual device creation decides format capability. Structured/raw/UAV/constant buffers are not implemented. The Buffer descriptor uses IMMUTABLE, VB/IB flags, optional SRV and zero CPU/misc/structure stride; binding stride is separate from StructureByteStride.

DDS support is a single Texture2D, one array element, 1x sampling, an explicit finite mip chain and tightly packed file rows. Legacy DXT1/3/5, ATI1/2, BC4U/S, BC5U/S, exact RGBA8/BGRA8 masks, R8 luminance and A8 are decoded. DX10 DDS accepts the explicitly listed typed single-plane formats, BC1-7 (including supported signed/sRGB variants), and the ordinary typed R8/R16/R32 vector families listed in `GimiFormats.cpp`. Native BC payloads stay compressed; the decoder validates headers, pitches, dimensions and every mip extent and creates real subresources/SRVs. No synthetic replacement image is generated.

Typeless/depth/planar/packed-video formats, cubemaps, arrays, volumes, MSAA, padded DDS file rows, automatic mip generation, trailing payloads, format reinterpretation/conversion and PNG/JPEG/WIC are Unsupported. There was no established reusable texture decoder found in the current dirty product tree; this narrow DDS decoder adds no new binary dependency. Missing/unsupported formats report the Resource filename/format/stride line (or the section header when the property is absent).

## Exact upstream identity and important distinctions

The authority is XXMI-Libs-Package commit `065dc746c21c71ef48bbc93cb6b90b3dcdf766e6`, not an arbitrary latest 3Dmigoto implementation. The package is SilentNightSound/GIMI-Package commit `83e75c02d809e3fa0482558af6da8f9cf34cc2b5`. Upstream semantic provenance and GPL notices must remain with subsequent source reuse/distribution; this layer does not import its loader.

Source mapping for maintenance:

| Upstream | Local implementation |
| --- | --- |
| `DirectX11/HackerDevice.cpp::CreateBuffer` | `HashBuffer`: seed-zero CRC32C over ByteWidth initial bytes, then the original 24-byte descriptor |
| `util.h::crc32c_hw`, `crc32c-hw-1.0.5/include/crc32c.h`, `src/crc32c.cpp` | `ResourceCrc32c`: Castagnoli 0x82f63b78, initial/final complement and append seed semantics; independent constexpr lookup table |
| `ResourceHash.cpp:417-476` | Original 44-byte Texture2D descriptor and exact MSVC SRES/SR*2/SR*4/SR*8/SR/2 constants |
| `ResourceHash.cpp:617-815` | `HashTexture2D`: version 0 legacy texel-count truncation and padded-BC length decision; version 1 exact first-surface rows, skipping CPU padding |
| `ResourceHash.cpp:933-940,1129-1281` | Buffer creation hash stays fixed; texture tracking mode 0 preserves creation hash, mode 1 permits CPU/copy updates |
| `ResourceHash.cpp:844-861,1913+` | Distinguish Custom resources' known-empty candidates from Unknown game identity; no fake hash-zero match |
| `CommandList.cpp:3098-3150,3225+` | Null filter -0, bound/no match +0, highest-priority explicit filter, matching/no filter fallback 1 |
| `IniHandler.cpp:4712,4722`, `CommandList.cpp:10311+` | texture update mode and optional region-hash behavior; region mode explicitly Unsupported here |

The additional fixed-commit HackerDevice/util/CRC sources above were read directly; they were not installed into the product or copied as a new loader. The five existing local core files remain the main reference snapshot. When updating upstream, compare these functions/config defaults first, then expand formats or instructions in the owning module. Role/hash/character rules stay in real INI files; there is no character keyword table in C++.

`ResourceIdentity` separates `Knowledge::{Unknown,Null,Known}`, `ResourceOrigin::{Unknown,Game,Custom}`, `hashKnown`, `creationHashKnown` and `contentKnown`. Zero CRC is valid when hashKnown is true. A successful no-initial-data create has a genuine descriptor-only upstream hash, but contentKnown=false: the supported geometry path refuses it. A late-attached, GPU-only or incompletely observed resource remains Unknown. Nothing performs readback to fill the gap.

Preloaded Custom resources keep their real CPU data/descriptor creation fingerprint for bounds and offline index windows, but have origin=Custom, upstreamHash=0 and hashKnown=false. Their known-bound filter is +0 and their candidate list is empty, matching upstream custom-resource behavior. They are not Unknown, not null, and not hash-zero TextureOverride candidates. Referencing a retained game lease into an alias keeps its Game identity.

Always pass ORIGINAL creation descriptors, not normalized GetDesc values, and the complete accessible CPU span for the hashed first subresource. Texture identity currently requires resolved 1..15 mip levels, ArraySize=1, SampleDesc={1,0}, and a listed format. Overflowing legacy arithmetic is Unsupported rather than an out-of-bounds emulation. Only subresource zero contributes to the upstream hash.

`HashPolicy` defaults to package texture_hash=0, track_texture_updates=0, region hashing off, and screen-relative normalization on. Supply the actual swap-chain width/height and resolutionKnown=true. With active normalization but unknown dimensions, texture identity fails explicitly. Set normalizeResolution=false only when upstream resolution normalization is genuinely disabled/INVALID. Do not silently substitute window size or a previous device's resolution. Resolution/hash-policy changes require disabling/rebuilding the consumer's identity qualification; a cache generation alone cannot make an old hash correct.

## Event calls and invalidation

`IdentityRecord` is embedded in a retained game-resource lease. It allocates nothing and owns no COM reference. Its owner serializes it with immediate-context state. Keep device/lifetime IDs unique; no pointer-reuse identity.

| Event | Required action |
| --- | --- |
| Successful CreateBuffer/CreateTexture2D with visible CPU initial data | Off-draw initialization with original descriptor, policy, non-reused object ID and device generation; retain snapshot metadata |
| Successful writable Map | `MapSucceeded(subresource, mode)` before another draw can use the object; read-only Map leaves identity unchanged |
| Unmap | `Unmap(subresource)`; it does not hash the mapped address or assume it stays valid |
| UpdateSubresource / partial writes | `Update(subresource)` before use, even for mip writes; no optimistic partial CRC patching |
| Whole CopyResource | `CopyResource(sourceRecord)`; equal shapes/current CPU-derived data can propagate without IO/hash |
| CopySubresourceRegion | `CopySubresource(...)`; only exact full coverage, matching shape, zero origins and permitted subresource topology propagate; partial copies invalidate |
| Resolve, UAV/RTV/SO writes, GenerateMips, unknown mutation | `OpaqueWrite()` |
| Destroy/resource lifetime end | `Destroy()` and retire the real retained lease; never reuse its object ID |
| Device/context replacement, deferred/opaque execution, unobserved external writes | Disable this consumer before further MOD draws, invalidate the runtime owner epoch/captured leases, and rebuild from real observed state outside draws |

Every content event increments contentGeneration. Current content becomes Unknown immediately on writes. Creation identity remains separately available. Writable Map bits are bounded and prevent CPU revalidation while a relevant write map is open. Failed Maps must not call MapSucceeded. Call event APIs for relevant observed resources only; do not hash every dynamic CB or scan the full registry on every draw.

`RefreshCpu(expectedRevision, fullCpuSnapshot)` is ONLY for load/frame preparation with already-owned CPU bytes. It recomputes the full hashed range and descriptor, checks the revision, and invalidates negative cache entries by advancing generation. It never treats the Map pointer as persistent. Buffer upstream identities remain their creation hashes. Texture mode 0 also preserves the creation hash; mode 1 updates current upstream identity.

Copy propagation is deliberately narrower than upstream's permissive guesses: equal ByteWidth buffers, or exact-shape Texture2D resources using accurate version 1. Version 0 texture copies, incomplete regions, incompatible shapes and unknown sources remain Unknown. In tracked texture mode, the event copies only the known data hash and leaves hashKnown=false; call `FinalizeCopy(expectedRevision)` OUTSIDE the draw to append the destination descriptor. Events do not calculate even descriptor CRCs. Full-copy propagation is not GPU completion.

The first slice rejects potentially GPU-writable/shared descriptor classes for queried/matched data. Missing event coverage or a deferred/opaque boundary must not be repaired by simply changing a cache epoch while retaining stale Known snapshots. Leave the gate disabled until a real reconstruction is possible. No deferred recording/replay is claimed.

## Exact DrawIndexed matching and command queries

Only an immediate, non-internal, single-instance DrawIndexed triangle list is supported. Instanced/indirect/auto/deferred draws and other topologies are Unsupported. Layouts use explicit offsets, per-vertex elements and at most eight used IA slots; APPEND/per-instance layouts are Unsupported in this slice. A known layout/VS contract is a structural check, not proof that the selected character's skinning, skeleton, LOD, shadow or material pass is compatible; the executor/runtime still needs that actual contract.

Build `IndexWindow` OFF-DRAW with `CompileIndexWindow` from a complete CPU IB snapshot, exact index format, binding offset, first index and count. It checks the snapshot's data fingerprint against the retained identity and computes real min/max indices for that exact range. Keep windows with their object/content generation, not a guessed vertexCount. Build replacement-IB windows for the finite explicit ranges needed by the compiled tape during initialization. A dynamic range without a precompiled window is Unsupported; the executor must not scan/upload/read back its indices during PrepareDraw.

`IndexedDrawSnapshot` supplies original DrawContext (including signed baseVertex), exact device/context/consumer/Plan/binding epochs, qualified layout, topology, upstream shader identities, retained IB/VB bindings/identities and the exact compiled index window. Matcher checks IB byte bounds, window revision and signed baseVertex+min/max, then exact required strides and VB byte ends for at most eight used slots. It never substitutes IndexCount for index values. `ShaderIdentity` must come from the real upstream shader_hash=3dmigoto algorithm/capture; this module does not implement shader capture or equate an FSR classifier hash with it.

`DrawCache::Lookup` looks up exact resource or shader hashes in Plan's compiled ordered candidate ranges and applies exact first-index/index-count qualifiers. Multiple resource matches retain Plan priority/name order. The candidate range is bounded by matchesPerCheck <=128; overflow returns Limit, never truncated success. Equal-hash ShaderOverride merging (including overrule duplicates), fuzzy descriptor/range expressions and optional track_region_hashes are Unsupported. Raw resource hash includes the original descriptor; layout/range qualification adds structural constraints but does not eliminate CRC collision possibilities.

The runtime checks `DrawCache::Enabled(sharedGate)` BEFORE entering its existing owner lock. Lookup(false) returns Disabled before inspecting state/resources/key storage. One serial context owns each cache. Fixed direct mapping defaults to 256 slots (configurable power-of-two 1..4096); full numeric key equality resolves bucket collisions. The key includes all consumed draw arguments, subject target/binding, Plan/consumer/device/context/binding generations, layout contract/lease, VS/PS identity generations and every relevant resource object/content/descriptor generation. Positive and negative results become stale automatically when these values change. Scratch key storage is allocated at construction and reused without a full fixed-array clear on hits. A miss visits only a binary-searched equal-hash range, at most 128 entries; it does not scan the whole Plan.

Cache only program IDs and static qualification/filter metadata. Never cache a prepared action tape, variable value, alias binding or branch result. `DrawQueryAdapter` provides the implemented bridge to R478 CommandQueries from allocation-free `CachedIdentityQueries::{readBinding,resource,shader}` providers. Resource and shader providers resolve retained lease IDs in O(1); they must preserve Unknown vs known-null. The adapter handles simulated reference/alias values supplied by R478, so later checks observe the current semantic binding. The executor supplies `readParameter` from the authenticated initial immutable page for this invocation; the command interpreter simulates later writes. `DrawQueryAdapter` itself does not invent parameter values.

Schedule only the manually selected eligible part and its actual required ordered shader/command roots. Pass those `ProgramInvocation`s to `PrepareDraw`, validate the ENTIRE returned BatchView (PRE -> original boundary -> POST), then execute/restore/Accept through R507. Unknown/Unsupported/Limit rejects the ENTIRE selected component. Outside the hook, `DescribeMatchFailure(plan, selectedProgram, status)` gives an explicit Unsupported diagnostic at that selected INI file/line. NoMatch is an actual known mismatch, and Disabled means disabled; neither is a rendering-success signal. Per-draw diagnostics are numeric only; do not add hot logs.

## Immutable parameter page ownership

`ResourceFiles::Parameters()` contains the static initial values and finite scalar domains. `Dx11StaticResources` creates all required `R32G32B32A32_FLOAT` Texture1D/SRVs on the existing device preparation thread under existing internal suppression. A creation failure publishes nothing; pages are immutable for their entire lifetime. No page uses the game-resource identity registry, and no parameter byte hash is computed in a draw.

`Dx11ExecutionRequest::parameterState` must equal the runtime's committed state. The execution packet exposes the exact parameter owner and initial/final page IDs; `Session::Reserve` authenticates them before any setter. Submitted packets keep the complete set, both old t120 bindings and the original pipeline alive through the existing real fence. Cancelled reservations do not commit state, partial work quarantines the packet, and `UINT64_MAX` remains device-removal failure. Disabling/replacing a session does not release its unresolved submitted owners.

Original shaders must be reflected unused at t120, and parameter plans cannot also reference/check/bind game `ps-t120`. Used t120, GS parameter routes and unknown shader replacement ABI remain Unsupported. This transport is not an ORFix shader-loader or multi-component compatibility claim.

## Existing FSR path versus remaining integration

The actual dirty product `work/r294-r266-build/OptiScaler` already supplies the DrawIndexed first-index/count/baseVertex entry, Before/After pairing, internal-draw suppression, cached draw classification, IA/PS observation and resource mutation observation. The R477 references identify GenericDepth_Dx11.cpp, GenshinLegacyCapture and existing raster/depth state capture. These are read-only reuse points here; FSR's count/PS early rejection must not suppress an enabled MOD consumer when hooks are later integrated.

Still needed: real game-resource create/lifetime observation, proven event coverage, retained shader/layout/lease publication, correct consumer epoch/gate updates, selected-part scheduling, GPU ownership/retirement and R507 execution/restoration. Static resources and the new identity/cache code must be connected to those existing owners; no second FSR pipeline is needed. Do not infer GPU writes or skeleton contents from an FSR classifier.

No shared Config/GenericDepth/LegacyCapture/project/menu/UI/profile file was changed. No tests/build/deploy/hash-verification/manifest/fixture/game operations were run. Resource CRC implementations are product functionality and were not run as a deployment verification gate. Runtime cost, MOD rendering and SR/NR/FG coexistence remain UNKNOWN.
