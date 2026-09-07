# 바나나바나나바나나! (BananaBananaBanana!)

Unreal Engine 5 C++ 기반의 3인 협동 동굴 생존 게임입니다.

Config-driven Voxel World를 생성하고, authored Room과 자연 동굴을 결합한 뒤, Surface Nets와 Chunk Runtime으로 표현해 Realtime Mining까지 연결했습니다. 플레이어는 동굴에서 얻은 자원을 정산하고 다음 Stage와 공양(Tribute) 주기로 진행합니다.

**Role:** Main Programmer / UE5 Client Programmer

이 저장소는 전체 게임 소스가 아니라 **문제와 시스템 구조 → 설계 결정 → 실제 코드 근거**를 빠르게 확인할 수 있도록 정리한 기술 포트폴리오입니다.

## Project Overview

핵심 기술 흐름은 하나의 Runtime World와 Multiplayer Game Loop를 끝까지 연결한 것입니다.

`CaveGenConfig / Seed → Room + Natural Cave → Voxel Grid → Surface Nets / Chunk → Realtime Mining → Stage Progression`

## Key Contributions

- **World / Runtime:** Procedural Voxel World, CaveGenConfig authoring, Room/Prefab integration, Surface Nets, Chunk Runtime, Realtime Mining, Async Meshing/Collision scheduling
- **Gameplay:** Grid Inventory, Equipment/QuickSlot, World Item/Container/Corpse Loot, Shop/Shared Storage/Upgrade
- **Multiplayer Game Systems:** Save/Load, server-authoritative persistence, Lobby/Stage/Escape/Death/Spectating, Settlement/Tribute progression
- **Integration:** UE5 Client, Shared gameplay protocol 확장, gameplay server 연동, GameThread/runtime system integration

## System Flows

| System | Flow |
|---|---|
| World | `CaveGenConfig → Voxel Generation → Chunk / Surface Nets → Realtime Mining` |
| Game Loop | `Inventory / Loot → Persistence → Stage / Spectating → Settlement / Tribute` |

## Portfolio Guide

| Topic | Document | Key Points |
|---|---|---|
| Procedural Voxel World | [Procedural Voxel World Generation](CodeSamples/World/ProceduralCaveGeneration.md) | CaveGenConfig, Seed, Room/Prefab, Natural Cave, Ore, Runtime hand-off |
| Realtime Voxel World | [Realtime Destructible Voxel World](CaseStudies/01_RealtimeVoxelWorld.md) | Dirty Chunk, Snapshot, Async Meshing, Generation validation, Collision |
| Surface Nets | [Surface Nets Core](CodeSamples/Voxel/SurfaceNets_Core.cpp) | Dual cell, surface vertex, quad, collision |
| Async Meshing | [Async Meshing Core](CodeSamples/Voxel/AsyncMeshing_Core.cpp) | Snapshot, ThreadPool, stale result, collision scheduling |
| Persistence | [Server-Authoritative Persistence](CaseStudies/02_ServerAuthoritativePersistence.md) | Restore ordering, Empty Sync guard, persist-before-reset |
| Game Flow | [Game Flow and Progression](CodeSamples/Gameplay/GameFlowAndProgression.md) | Room, Stage, death, escape, spectate, settlement, tribute |
| Grid Inventory | [Grid Inventory Core](CodeSamples/Gameplay/GridInventory_Core.cpp) | Footprint, placement, first-fit, stack, merge |

## Technical Highlights

- 생성 알고리즘과 Stage authoring 데이터를 분리해 동굴 규모, Room 구성, 자연 동굴 형태와 광물 분포를 Config에서 조정합니다.
- 변경 Chunk의 bordered snapshot을 Worker에서 메시화하고, generation 검증 뒤 GameThread에서 적용합니다. Collision은 플레이 안전성과 작업량에 따라 별도로 scheduling합니다.
- Save 복원 순서와 Round/Tribute lifecycle은 서버 권위 상태로 관리하고, Grid Inventory를 실제 Multiplayer Game Loop에 연결했습니다.

## Contribution Note

CPP 파일은 Production source에서 핵심 구간만 선별한 Portfolio excerpt이며 독립 실행용 translation unit이 아닙니다. 생략 구간은 주석으로 표시했고, 실제 코드에 없는 자료형이나 함수를 만들지 않았습니다.
