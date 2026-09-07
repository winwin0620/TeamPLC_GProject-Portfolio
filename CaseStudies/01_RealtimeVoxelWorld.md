# Realtime Destructible Voxel World

Runtime Voxel Grid가 채굴 등으로 바뀔 때 Render와 Collision을 갱신하면서 발생한 GameThread 집중 비용을 줄인 사례입니다.

## Problem

Voxel 하나의 변화도 Chunk 경계 보정, Surface Nets 계산, Procedural Mesh 적용, Collision geometry와 cooking을 유발합니다. 연속 변경마다 이 과정을 동기 실행하면 한 프레임에 비용이 몰립니다.

또한 계산은 Worker로 보낼 수 있지만 `AActor`와 `UProceduralMeshComponent` 변경은 GameThread에서 수행해야 합니다.

## Design

```text
Voxel Change → Affected Chunk → bordered Snapshot → ThreadPool mesh build
             → Generation validation → GameThread apply
             → safety / dig / activation Collision scheduling
```

핵심은 Render 계산과 UObject 적용, Collision 갱신을 서로 다른 단계로 분리한 것입니다.

## Snapshot and Async Build

`BuildChunkRenderSnapshot()`은 Chunk 영역 앞뒤에 1 Voxel border를 더해 값 복사본을 만듭니다. Surface Nets가 경계의 dual cell을 계산할 때 live Grid나 인접 Chunk Actor를 Worker에서 읽지 않아도 됩니다.

```cpp
const FIntVector SnapshotMin = RegionOrigin - FIntVector(1, 1, 1);
const FIntVector SnapshotSize = RegionSize + FIntVector(2, 2, 2);

OutSnapshotGrid.Init(SnapshotSize, EGP_VoxelId::Air);
// Grid 값을 Snapshot에 복사
OutSnapshotRegionOrigin = FIntVector(1, 1, 1);
```

`RequestChunkRenderBuild()`은 같은 Key의 진행 중 요청을 retry flag로 합칩니다. Snapshot은 `MoveTemp`로 ThreadPool 작업에 넘기고, Subsystem은 `TWeakObjectPtr`로 캡처합니다. Worker는 정적 계산 함수 `AGP_VoxelChunkActor::GenerateRenderBuffers()`만 실행합니다.

완료 Buffer는 lock으로 보호된 Queue에 넣고, `FlushCompletedChunkRenderResults()`가 Queue ownership을 짧은 잠금 안에서 지역 배열로 옮긴 뒤 GameThread에서 적용합니다.

## Stale Result Rejection

Chunk마다 build generation을 증가시키고 결과에도 기록합니다. 완료 시 최신 generation과 다르면 오래된 Buffer를 적용하지 않고, 합쳐 둔 재요청이 있으면 다시 제출합니다.

```cpp
const uint32* LatestGeneration = ChunkRenderBuildGeneration.Find(Key);
if (!LatestGeneration || *LatestGeneration != Result->Generation)
{
    if (PendingAsyncChunkRenderRetryKeys.Contains(Key))
    {
        PendingAsyncChunkRenderRetryKeys.Remove(Key);
        RequestChunkRenderBuild(Key, false);
    }
    continue;
}
```

## Collision Scheduling

Render 갱신은 비동기 Buffer 계산을 사용하지만 Collision은 별도 정책으로 다룹니다.

- `RebuildCollisionForDirtyKeys()`는 플레이어 safety 범위의 Chunk를 immediate 후보로 분리합니다.
- immediate 후보는 플레이어 중심 거리순으로 제한된 수만 즉시 처리하고 나머지는 지연 Queue로 보냅니다.
- 빠르게 낙하 중이면 별도 batch 상한을 사용해 한 프레임의 collision 작업량을 더 제한합니다.
- `FlushPendingCollisionRebuild()`는 `Immediate → Dig → Activation` 순으로 한 Queue와 한 batch만 처리합니다.

Collision geometry는 `BuildCollisionOnlyRegion()`에서 Solid/Air 경계의 같은 방향 면을 greedy rectangle로 합칩니다. 시각 메시의 세밀한 Surface Nets geometry를 그대로 물리에 사용하지 않습니다.

## Result and Trade-offs

전체 World가 아니라 영향을 받은 Chunk만 다시 계산하고, CPU 메시 계산을 Worker로 옮기면서 UObject 적용 경계를 명확히 했습니다. Collision은 플레이 안전성과 프레임 분산을 별도 Queue 정책으로 조절합니다.

현재 snapshot 값 복사는 Worker가 live state를 공유하지 않게 하는 단순하고 안전한 선택입니다. World와 동시 작업 규모가 커지면 snapshot 재사용이나 제출 budget을 추가할 수 있습니다. 성능 수치는 저장소에 전후 프로파일이 없어 별도로 주장하지 않습니다.

## Code Evidence

- [AsyncMeshing_Core.cpp](../CodeSamples/Voxel/AsyncMeshing_Core.cpp): snapshot, ThreadPool, generation 검증, collision 우선순위
- [SurfaceNets_Core.cpp](../CodeSamples/Voxel/SurfaceNets_Core.cpp): dual cell 메시와 greedy collision geometry
- [Procedural Voxel World Generation](../CodeSamples/World/ProceduralCaveGeneration.md): 이 runtime Grid가 생성되는 과정
