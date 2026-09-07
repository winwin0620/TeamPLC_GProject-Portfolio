# Server-Authoritative Persistence

Save 복원 직후 Client의 초기 Empty Sync가 서버에 복원된 인벤토리를 다시 비우는 순서 경합을 막은 사례입니다.

## Problem

```text
Server profile restore → carry / fixed slots 복원
Client 초기화        → empty sync 도착
보호가 없다면        → 복원 상태 overwrite
```

각 packet은 유효하지만, 복원 완료와 Client 초기화의 순서가 엇갈리면 늦게 도착한 빈 상태가 더 최신인 서버 상태를 덮을 수 있습니다.

## Design

`TryApplyLoadedPlayerProfileToPlayerLocked()`이 runtime state를 복원할 때 carry와 fixed slots 각각에 pending guard를 설치합니다.

- pending 중 Empty Sync가 도착하고 서버 복원 상태가 비어 있지 않으면 입력을 거부합니다.
- 거부로 끝내지 않고 authoritative snapshot을 다시 보내 Client를 서버 상태로 수렴시킵니다.
- 정상 non-empty sync가 오거나 서버 복원 상태도 실제로 비어 있으면 guard를 해제합니다.

```cpp
playerCarryStates[playerId] = std::move(carry);
playerFixedSlotStates[playerId] = std::move(fixed);

pendingLoadedProfileCarryRestorePlayers.insert(playerId);
pendingLoadedProfileFixedRestorePlayers.insert(playerId);
```

## Empty Sync Guard

`OnSyncFixedSlots()`은 equipment/quick slot 입력이 모두 비었는지 검사합니다. `OnSyncPlayerCarry()`는 `entryCount == 0`을 같은 정책으로 처리합니다. 두 handler 모두 서버에 복원 항목이 남아 있을 때 해당 snapshot을 재전송하고 return합니다.

```cpp
if (PendingIt != pendingLoadedProfileCarryRestorePlayers.end() && entryCount == 0)
{
    const auto CarryIt = playerCarryStates.find(playerId);
    if (CarryIt != playerCarryStates.end() && !CarryIt->second.entries.empty())
    {
        SendPlayerCarrySnapshotToPlayerLocked(playerId);
        return;
    }

    pendingLoadedProfileCarryRestorePlayers.erase(PendingIt);
}
```

통과한 carry entry는 Item spec, cell bounds, 겹침을 다시 검사하고 서버 mirror에 반영합니다. Fixed slots도 장비 호환성과 slot 상태를 검증한 뒤 guard를 종료합니다.

## Persist Before Runtime Reset

라운드 결과는 runtime container가 지워지기 전에 영구 프로필에 반영해야 합니다. `FinalizeRoundResultLocked()`은 `ResetRoundRuntimeLocked()`보다 먼저 아래 순서를 실행합니다.

```cpp
PersistInactivePlayerProfilesLocked();
BroadcastSettlementResultLocked();

if (IsNormalGameplayStageType(completedStageType))
{
    MarkNormalStageEndedForProgressionLocked();
    BroadcastRoomProgressionStateLocked();
}
```

`PersistInactivePlayerProfilesLocked()`은 사망/탈출 상태를 구분해 결과를 저장합니다. 탈출자는 carry/equipment/quick slots를 보존하고, 사망자는 해당 목록을 비운 프로필로 확정합니다.

## Lock Boundary

`OnLoadSaveSnapshotEnd()`은 payload 검증과 상태 교체를 `sessionLock` 안에서 처리합니다. 같은 mutex를 다시 획득하는 Shared Stash 전체 broadcast는 flag만 남긴 뒤 lock 밖에서 호출하고, 실제 전송도 상태 복사 잠금 밖에서 수행합니다.

## Result and Extension

복원 직후 Empty Sync는 서버 상태를 덮지 못하고, authoritative snapshot 재전송으로 Client가 수렴합니다. 정상 sync는 보호 구간 종료 후 기존 경로를 사용합니다.

현재 guard는 실제 충돌 지점인 초기 Empty Sync를 대상으로 합니다. 향후 non-empty 상태 사이의 순서까지 구분해야 하면 packet version 또는 restore epoch 검증을 추가할 수 있습니다. 영구 파일은 host Client가 담당하며 서버는 세션 중 gameplay state의 권한을 갖습니다.

## Actual Functions

- `TryApplyLoadedPlayerProfileToPlayerLocked()`
- `OnSyncFixedSlots()` / `SendPlayerFixedSlotsSnapshotToPlayerLocked()`
- `OnSyncPlayerCarry()` / `SendPlayerCarrySnapshotToPlayerLocked()`
- `FinalizeRoundResultLocked()` / `PersistInactivePlayerProfilesLocked()`
- `OnLoadSaveSnapshotBegin()` / `OnLoadSaveSnapshotChunk()` / `OnLoadSaveSnapshotEnd()`
