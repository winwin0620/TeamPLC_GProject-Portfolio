# Game Flow and Progression

개별 기능 목록이 아니라 Main Menu부터 Stage, 사망/탈출, 정산과 Tribute 결과까지 이어지는 서버 권위 Multiplayer Game Loop입니다.

## State Flow

```text
Main Menu → Create / Join Room → Lobby → Stage Selection
→ Cave: Mining / Combat / Loot
→ Death or Escape → Spectating → All Players Inactive
→ Settlement → Lobby → Normal Stage Progression
→ Tribute Pending → Tribute Stage
   ├─ satisfied → Lobby / Next Cycle
   └─ unsatisfied return → warning → retry → Game Over
```

## Client Entry and Shared State

`AGP_PlayerController::RequestCreateRoom()`과 `RequestJoinRoom()`이 진입 요청을 보내고, `SendLobbyReady()`와 `RequestSelectStage()`가 Lobby 선택을 전달합니다. `UClientNetSubsystem::PopRoomState()`로 받은 `S_RoomState`는 `UGP_GameInstance::ApplyRoomStateFromNet()`에 적용되어 UI와 presentation이 같은 서버 상태를 보게 합니다.

공유 상태의 중심은 `ERoomPhase`입니다.

| Phase | Server responsibility | Client response |
|---|---|---|
| `Lobby` | ready와 Stage 선택 수용 | Lobby/정산 UI |
| `InGame` | gameplay 입력과 Stage 진행 | World, combat, interaction |
| `CollapseDamage` | 제한시간 후 붕괴 피해 | warning/effect presentation |
| `RoundEnding` | 결과 확정 후 종료 연출 | camera/UI presentation |
| `GameOverPresentation` | 입력 정지와 전체 초기화 전 연출 | Tribute failure presentation |

## Stage Start and Runtime Loop

`DungeonRoom::OnReady()`는 전원 ready이고 progression이 허용할 때 `InGame`으로 전환합니다. `OnSelectStage()`도 Lobby에서 `CanSelectStageByProgressionLocked()`를 통과한 Stage만 시작하며, normal Stage에는 새 world seed를 배정합니다.

Server는 Room phase와 Player life state를 기준으로 Stage 진행, escape, round 종료와 progression 요청을 처리합니다. 제한시간이 끝나면 `DungeonRoom::Tick()`이 `CollapseDamage`로 전환하고 마지막 입력과 일반 interaction을 정리합니다.

## Death, Escape, and Spectating

Server life state의 `dead` 또는 `escaped`는 해당 플레이어를 inactive로 만듭니다. Client의 `HandleServerLifeState()`는 사망 연출 뒤 또는 탈출 즉시 `EnterServerSpectateMode()`로 전환하고, `CycleSpectateTarget()`에서 살아 있는 대상을 순환합니다.

```cpp
if (bEscaped)
{
    CancelDeathNoticeMode(false);
    if (!bServerSpectateMode)
        EnterServerSpectateMode();
    return;
}
```

이 관전 대상은 화면뿐 아니라 [Proximity Voice Chat](../Networking/ProximityVoiceChat.md)의 생존자 음성 청취 기준에도 사용됩니다.

## Round End, Persistence, and Settlement

`DungeonRoom::Tick()`은 `AreAllPlayersInactive()`가 true가 되면 `RoundEnding`으로 이동하고 `FinalizeRoundResultLocked()`을 한 번 호출합니다.

```cpp
PersistInactivePlayerProfilesLocked();
BroadcastSettlementResultLocked();

if (IsNormalGameplayStageType(completedStageType))
{
    MarkNormalStageEndedForProgressionLocked();
    BroadcastRoomProgressionStateLocked();
}
```

Runtime reset 전에 탈출/사망 결과를 영구 프로필에 반영하고 정산을 확정합니다. 종료 presentation이 끝나면 ready 상태와 round runtime을 초기화하고 `Lobby`로 돌아갑니다. 복원 순서의 상세는 [Server-Authoritative Persistence](../../CaseStudies/02_ServerAuthoritativePersistence.md)에 분리했습니다.

## Normal Progression and Tribute

`MarkNormalStageEndedForProgressionLocked()`이 normal Stage 완료 수를 갱신하고 기준에 도달하면 tribute pending 상태를 만듭니다. 이때 일반 Stage 선택은 막히고 Tribute Stage가 열립니다.

`OnTributeOfferRequest()`은 team currency, offer 값과 누적 공양을 서버에서 검증합니다. 요구량을 만족한 뒤 `OnTributeReturnToLobbyRequest()`가 성공하면 cycle을 증가시키고 다음 요구량을 정한 후 Lobby로 복귀합니다.

미달 상태의 첫 복귀 시도는 warning을 반환합니다. 같은 cycle에서 다시 미달 복귀를 시도하면 `GameOverPresentation`으로 전환하고 gameplay 입력을 중단한 뒤 전체 reset 흐름으로 연결합니다.

## Result and Evidence Scope

Room 생성/참가, Stage 권한, runtime life state, 관전, 정산, 영속화와 장기 progression을 하나의 서버 상태 기계로 연결했습니다. 각 기능의 상세 구현을 반복하지 않고, 이 문서는 전체 lifecycle과 책임 경계만 설명합니다.

주요 원본 함수: `RequestCreateRoom()`, `RequestJoinRoom()`, `ApplyRoomStateFromNet()`, `DungeonRoom::OnReady()`, `DungeonRoom::OnSelectStage()`, `HandleServerLifeState()`, `FinalizeRoundResultLocked()`, `MarkNormalStageEndedForProgressionLocked()`, `OnTributeOfferRequest()`, `OnTributeReturnToLobbyRequest()`.
