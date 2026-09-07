# Procedural Voxel World Generation

Config-driven Cave Generation, Authored Room Integration, and Runtime Voxel Hand-off

## Goal

이 시스템의 핵심은 랜덤 동굴 알고리즘 하나가 아니라 **생성 규칙과 Stage authoring 데이터를 분리한 것**입니다. `UGP_CaveGenConfig` DataAsset에서 동굴 규모, Room 구성, 자연 동굴 형태와 광물 분포를 조정하고, `UGP_WorldGenSubsystem::GenerateCave()`가 이를 Runtime Voxel World로 변환합니다.

## CaveGenConfig as an Authoring Layer

| Designer intent | Config examples | Generator result |
|---|---|---|
| World scale | `WorldSize`, `AuthorVoxelSize`, `VoxelSize`, Chunk count | 생성 범위, 해상도와 Chunk 분할 |
| Room layout | `StartRoom`, `SpecialRooms`, `ScatterRoomCount`, `RoomActorClass`, Z policy | authored 공간과 pocket room 배치 |
| 자연 동굴 성격 | `MainCaveCount`, step/radius, branch/chamber, boundary noise | 터널, 분기, chamber 형태 |
| 광물과 marker | `TotalOreNodeCount`, `OreRules`, `MarkerSpawnRules` | Stage별 광물/Gameplay marker |

즉, 생성 코드를 바꾸지 않고 Stage Config 교체로 World의 성격을 조정합니다.

Seed는 World scale이 아니라 generation input입니다. `bUseServerWorldSeed`가 꺼져 있으면 Config의 `Seed`를 사용하고, 켜져 있으면 `ResolveEffectiveWorldSeed()`가 준비된 Server Seed를 선택합니다. Server Seed가 아직 없으면 Config Seed로 돌아갑니다. 확정된 Seed는 Room, 자연 동굴과 Ore 생성 흐름에 사용되므로 동일 Config와 Seed로 같은 생성 순서를 재현할 수 있습니다.

## Author Space and Runtime Space

기획자가 다루는 공간 단위와 실제 Voxel 해상도를 분리했습니다. `AuthorVoxelSize` 기준으로 작성한 Room과 동굴 크기는 `VoxelSize`에 맞춰 변환됩니다.

```cpp
const int32 Seed = ResolveEffectiveWorldSeed(Config);
CachedSeed = Seed;
bHasCachedSeed = true;

const float AuthorVS = Config ? Config->AuthorVoxelSize : 100.f;
const float VoxelSize = Config ? Config->VoxelSize : 100.f;
const float Scale = (VoxelSize > 0.f) ? (AuthorVS / VoxelSize) : 1.f;
```

최소 경계는 `FloorToInt`, 최대 경계는 `CeilToInt`로 변환해 authored box가 Runtime 해상도에서 잘리지 않게 합니다. 디자이너가 Room 배치를 유지하면서 렌더 해상도를 조정할 수 있도록 한 분리입니다.

## Room and Prefab Integration

```text
Start / Special / Scatter Room + RoomActorClass + Marker
→ placement candidate → world bounds + XY padding + Z overlap 검증
→ room setup 중 carve / actor placement + protected area 등록
→ 자연 동굴 직전 protected voxel snapshot capture
→ natural cave + post-process → protected area restore
```

`TryPlaceRoom()`은 world border 안에서 후보를 만들고 `ResolveRoomZRange()`로 `Bottom`, fixed, random range 정책을 적용합니다. 기존 Room과는 padding을 확장한 XY rectangle과 Z range가 모두 겹칠 때만 충돌로 판정합니다.

```cpp
const bool bXYOverlap =
    RectsOverlap(ExpandRect(OutRect, Padding), ExpandRect(R.RectXY, Padding));

if (!bXYOverlap)
    continue;

if (ZRangesOverlap(OutZMin, OutZMax, R.ZMin, R.ZMax))
    return false;
```

배치된 authored 공간은 `MarkNaturalCaveProtectedAuthorBox()`가 보호 index로 기록합니다. 자연 동굴과 후처리 사이에 `RestoreNaturalCaveProtectedSnapshot()`을 적용해 Start Room 바닥, prefab 접합부와 Gameplay 공간이 우연히 깎이지 않게 합니다.

## Natural Cave Generation

Grid를 `Stone`으로 초기화하고 `ApplyBedrockBorder()`로 외곽 불변 조건을 만든 뒤 공간을 파냅니다.

`GenerateCave()`의 자연 동굴 단계는 다음 순서입니다.

1. Main cave의 시작점과 step 수를 정합니다.
2. 진행 방향을 조정하며 이전 중심과 다음 중심을 swept ellipsoid로 연결합니다.
3. 설정된 간격과 확률에 따라 radius를 바꾸고 branch와 chamber를 추가합니다.
4. 자연 동굴이 만진 bounds에 boundary noise, artifact cleanup, small-air-pocket fill을 적용합니다.
5. 각 후처리 뒤 authored 공간의 snapshot을 복원합니다.

carving은 voxel을 segment에 투영하고 보간된 XYZ radius 안에 있는지 검사합니다. 중심마다 분리된 구를 찍는 대신 연속된 volume을 사용해 step 사이 터널이 끊기는 것을 줄였습니다.

## Ore and World Content Distribution

`GenerateOresFromRules()`는 hard-coded 광물 목록 대신 활성화된 `OreRules`의 `OreId`, `SpawnPercent`, `MaxNodeCount`를 사용합니다. Air와 맞닿은 Stone 중 주변 지지가 있는 위치를 후보로 모으고 Chunk별로 나눕니다.

전체 목표 수, 종류별 상한, 가중 선택을 적용하며 상대적으로 적게 배치된 Chunk 집합을 먼저 선택해 한 구역 집중을 줄입니다. `MarkerSpawnRules`는 marker type과 후보를 Stage 데이터로 묶어 생성 결과에 Gameplay 배치 규칙을 연결합니다.

## Runtime Hand-off

완성된 Grid는 spawn 위치와 seed를 캐시한 뒤 `BuildOrRebuildMesh()`에서 Chunk로 분할됩니다.

`Voxel Grid → Chunk partition → Surface Nets render buffers → Runtime World`

Chunk activation을 사용하는 Stage는 `UpdateChunkActivation()` timer로 가까운 Chunk를 단계적으로 활성화합니다.

## Result, Scope, and Related Evidence

Config authoring, authored Gameplay 공간 보호, 자연 동굴, 광물 분포와 Runtime Chunk 전환을 하나의 재현 가능한 생성 순서로 연결했습니다. Dense Grid와 후보 배열은 World 부피에 비례하는 메모리를 사용하므로, 더 큰 World가 필요해지면 sparse storage나 단계별 streaming을 검토할 수 있습니다.

- [SurfaceNets_Core.cpp](../Voxel/SurfaceNets_Core.cpp): Voxel Grid에서 surface mesh를 만드는 실제 코드
- [AsyncMeshing_Core.cpp](../Voxel/AsyncMeshing_Core.cpp): 변경 Chunk의 비동기 rebuild와 collision scheduling
- [Realtime Destructible Voxel World](../../CaseStudies/01_RealtimeVoxelWorld.md): 생성 후 실시간 변경 비용을 해결한 Case Study

주요 원본 함수/내부 lambda: `GenerateCave()`, `ResolveEffectiveWorldSeed()`, `TryPlaceRoom()`, `ResolveRoomZRange()`, `MarkNaturalCaveProtectedAuthorBox()`, `CarveRuntimeSweptEllipsoidAir()`, `RestoreNaturalCaveProtectedSnapshot()`, `GenerateOresFromRules()`, `BuildOrRebuildMesh()`.
