/*
 * Portfolio excerpt — not a standalone translation unit.
 * Production source: UGP_WorldGenSubsystem.
 *
 * Selected evidence: bordered snapshot, ThreadPool ownership transfer,
 * weak UObject reference, completion queue, generation validation,
 * safety collision classification, and collision queue priority.
 */

struct FGP_AsyncChunkRenderBuildResult
{
    FIntVector Key = FIntVector::ZeroValue;
    uint32 Generation = 0;
    FVector ChunkWorldOrigin = FVector::ZeroVector;
    FGP_ChunkRenderBuffers Buffers;
};

bool UGP_WorldGenSubsystem::BuildChunkRenderSnapshot(
    const FIntVector& RegionOrigin,
    const FIntVector& RegionSize,
    FGP_VoxelGrid& OutSnapshotGrid,
    FIntVector& OutSnapshotRegionOrigin) const
{
    if (RegionSize.X <= 0 || RegionSize.Y <= 0 || RegionSize.Z <= 0)
    {
        return false;
    }

    const FIntVector SnapshotMin = RegionOrigin - FIntVector(1, 1, 1);
    const FIntVector SnapshotSize = RegionSize + FIntVector(2, 2, 2);

    OutSnapshotGrid.Init(SnapshotSize, EGP_VoxelId::Air);

    for (int32 z = 0; z < SnapshotSize.Z; ++z)
        for (int32 y = 0; y < SnapshotSize.Y; ++y)
            for (int32 x = 0; x < SnapshotSize.X; ++x)
            {
                const int32 GX = SnapshotMin.X + x;
                const int32 GY = SnapshotMin.Y + y;
                const int32 GZ = SnapshotMin.Z + z;
                OutSnapshotGrid.Set(x, y, z, Grid.Get(GX, GY, GZ));
            }

    OutSnapshotRegionOrigin = FIntVector(1, 1, 1);
    return true;
}

// Coalesce same-key requests and move immutable inputs to the ThreadPool.
void UGP_WorldGenSubsystem::RequestChunkRenderBuild(
    const FIntVector& Key,
    bool bPreferImmediateCollision)
{
    // [Omitted: World/Actor validity, Actor spawn, and presentation setup.]

    FIntVector Origin;
    FIntVector Size;
    if (!GetChunkBoundsByKey(Key, Origin, Size))
    {
        return;
    }

    const FVector ChunkWorldOrigin(
        Origin.X * CachedVoxelSize,
        Origin.Y * CachedVoxelSize,
        Origin.Z * CachedVoxelSize);

    if (PendingAsyncChunkRenderBuildKeys.Contains(Key))
    {
        PendingAsyncChunkRenderRetryKeys.Add(Key);
        return;
    }

    FGP_VoxelGrid SnapshotGrid;
    FIntVector SnapshotRegionOrigin;
    if (!BuildChunkRenderSnapshot(Origin, Size, SnapshotGrid, SnapshotRegionOrigin))
    {
        return;
    }

    uint32& GenerationRef = ChunkRenderBuildGeneration.FindOrAdd(Key);
    ++GenerationRef;
    const uint32 BuildGeneration = GenerationRef;

    PendingAsyncChunkRenderBuildKeys.Add(Key);

    const TMap<EGP_VoxelId, FLinearColor> LocalOreTint = OreTintById;
    const float LocalVoxelSize = CachedVoxelSize;
    TWeakObjectPtr<UGP_WorldGenSubsystem> WeakThis(this);

    Async(EAsyncExecution::ThreadPool,
        [WeakThis, Key, BuildGeneration,
        SnapshotGrid = MoveTemp(SnapshotGrid),
        SnapshotRegionOrigin, Size, ChunkWorldOrigin,
        LocalOreTint, LocalVoxelSize]() mutable
        {
            FGP_ChunkRenderBuffers Buffers;
            AGP_VoxelChunkActor::GenerateRenderBuffers(
                SnapshotGrid, SnapshotRegionOrigin, Size,
                LocalVoxelSize, LocalOreTint, ChunkWorldOrigin, Buffers);

            if (!WeakThis.IsValid())
            {
                return;
            }

            TSharedPtr<FGP_AsyncChunkRenderBuildResult, ESPMode::ThreadSafe> Result =
                MakeShared<FGP_AsyncChunkRenderBuildResult, ESPMode::ThreadSafe>();

            Result->Key = Key;
            Result->Generation = BuildGeneration;
            Result->ChunkWorldOrigin = ChunkWorldOrigin;
            Result->Buffers = MoveTemp(Buffers);

            if (UGP_WorldGenSubsystem* Sub = WeakThis.Get())
            {
                FScopeLock Lock(&Sub->CompletedChunkRenderResultsLock);
                Sub->CompletedChunkRenderResults.Add(Result);
            }
        });

    // [Omitted: start the 0.01-second GameThread flush timer.]
}

// Move the queue under a short lock, validate, then apply on GameThread.
void UGP_WorldGenSubsystem::FlushCompletedChunkRenderResults()
{
    TArray<TSharedPtr<FGP_AsyncChunkRenderBuildResult, ESPMode::ThreadSafe>> LocalResults;
    {
        FScopeLock Lock(&CompletedChunkRenderResultsLock);
        if (CompletedChunkRenderResults.Num() > 0)
        {
            LocalResults = MoveTemp(CompletedChunkRenderResults);
            CompletedChunkRenderResults.Reset();
        }
    }

    for (const TSharedPtr<FGP_AsyncChunkRenderBuildResult, ESPMode::ThreadSafe>& Result : LocalResults)
    {
        if (!Result.IsValid())
        {
            continue;
        }

        const FIntVector Key = Result->Key;
        PendingAsyncChunkRenderBuildKeys.Remove(Key);

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

        if (!ShouldChunkHaveActor(Key))
        {
            DestroyChunkActorAndState(Key);
            continue;
        }

        TWeakObjectPtr<AGP_VoxelChunkActor>* Found = ChunkActors.Find(Key);
        AGP_VoxelChunkActor* Chunk = Found ? Found->Get() : nullptr;
        if (!Chunk)
        {
            // [Omitted: clear a pending retry key.]
            continue;
        }

        Chunk->SetActorLocation(Result->ChunkWorldOrigin);
        Chunk->ApplyRenderBuffers(Result->Buffers, CachedMaterial.Get());

        if (PendingAsyncChunkRenderRetryKeys.Contains(Key))
        {
            PendingAsyncChunkRenderRetryKeys.Remove(Key);
            RequestChunkRenderBuild(Key, false);
        }
    }

    // [Omitted: clear the flush timer when queues become empty.]
}

// Split changed chunks into immediate safety work and deferred dig work.
void UGP_WorldGenSubsystem::RebuildCollisionForDirtyKeys(const TSet<FIntVector>& DirtyKeys)
{
    if (!bCachedCollision) return;
    if (DirtyKeys.Num() <= 0) return;

    TSet<FIntVector> ImmediateCandidateKeys;
    TSet<FIntVector> DeferredKeys;

    for (const FIntVector& Key : DirtyKeys)
    {
        CollisionReadyChunkKeys.Remove(Key);

        if (DesiredCollisionSafeChunkKeys.Contains(Key))
        {
            const TWeakObjectPtr<AGP_VoxelChunkActor>* const Found = ChunkActors.Find(Key);
            if (Found && Found->IsValid() && ShouldChunkHaveActor(Key))
            {
                ImmediateCandidateKeys.Add(Key);
            }
        }
        else
        {
            DeferredKeys.Add(Key);
        }
    }

    TSet<FIntVector> ImmediateKeys;
    if (ImmediateCandidateKeys.Num() > 0)
    {
        const bool bFallingFast = (CachedChunkFocusVelocity.Z < -CachedFallingFastSpeedZThreshold);
        const int32 MaxImmediate = bFallingFast
            ? FMath::Max(1, CachedMaxImmediateCollisionPerBatchWhenFalling)
            : FMath::Max(1, CachedMaxImmediateCollisionPerBatch);

        TArray<FIntVector> SortedImmediate;
        SortKeysByDistanceToCenter(ImmediateCandidateKeys, SortedImmediate, true);

        int32 ImmediateCount = 0;
        for (const FIntVector& Key : SortedImmediate)
        {
            if (ImmediateCount < MaxImmediate)
            {
                ImmediateKeys.Add(Key);
                ++ImmediateCount;
            }
            else
            {
                DeferredKeys.Add(Key);
            }
        }
    }

    if (ImmediateKeys.Num() > 0)
    {
        RebuildCollisionByKeys(ImmediateKeys, true);

        for (const FIntVector& Key : ImmediateKeys)
        {
            PendingDigCollisionChunks.Remove(Key);
            PendingActivationCollisionChunks.Remove(Key);
            PendingImmediateCollisionChunks.Remove(Key);

            const TWeakObjectPtr<AGP_VoxelChunkActor>* const Found = ChunkActors.Find(Key);
            if (ShouldChunkHaveActor(Key) && Found && Found->IsValid())
            {
                CollisionReadyChunkKeys.Add(Key);
            }
            else
            {
                CollisionReadyChunkKeys.Remove(Key);
            }
        }
    }

    if (DeferredKeys.Num() > 0)
    {
        EnqueueCollisionRebuild(DeferredKeys, true, CachedDigCollisionDelay);
    }
}

// One flush selects the highest-priority collision queue and one batch.
void UGP_WorldGenSubsystem::FlushPendingCollisionRebuild()
{
    if (PendingImmediateCollisionChunks.Num() <= 0 &&
        PendingDigCollisionChunks.Num() <= 0 &&
        PendingActivationCollisionChunks.Num() <= 0)
    {
        return;
    }

    bool bProcessImmediate = (PendingImmediateCollisionChunks.Num() > 0);
    bool bProcessDig = (!bProcessImmediate && PendingDigCollisionChunks.Num() > 0);

    const bool bFallingFast =
        (CachedChunkFocusVelocity.Z < -CachedFallingFastSpeedZThreshold);

    const int32 MaxPerBatch = bProcessImmediate
        ? (bFallingFast
            ? FMath::Max(1, CachedMaxImmediateCollisionPerBatchWhenFalling)
            : FMath::Max(1, CachedMaxImmediateCollisionPerBatch))
        : (bProcessDig
            ? FMath::Max(1, CachedDigCollisionMaxPerBatch)
            : FMath::Max(1, CachedActivationCollisionMaxPerBatch));

    TSet<FIntVector>& SourceSet = bProcessImmediate
        ? PendingImmediateCollisionChunks
        : (bProcessDig ? PendingDigCollisionChunks : PendingActivationCollisionChunks);

    TSet<FIntVector> Batch;
    Batch.Reserve(MaxPerBatch);

    int32 Count = 0;
    for (const FIntVector& Key : SourceSet)
    {
        Batch.Add(Key);
        if (++Count >= MaxPerBatch) break;
    }

    RebuildCollisionByKeys(Batch, bProcessImmediate);

    // [Omitted: remove completed keys and schedule the next delayed flush.]
}
