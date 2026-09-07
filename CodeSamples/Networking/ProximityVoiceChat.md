# Proximity Voice Chat

Mic 입력부터 Server routing, 관전자 청취 기준, Client playback까지 실제 Multiplayer Voice 경로를 연결한 구현입니다.

## Goal and Flow

```text
Mic capture → PTT / Open Mic gate → C_VOICE_FRAME → SendVoiceFrame()
→ DungeonRoom::OnVoiceFrame() → receiver classification / attenuation
→ S_VOICE_FRAME → bounded pending queue → PopVoiceFrames() → playback
```

Voice는 Client가 임의의 수신자를 정하지 않습니다. Server가 송신자와 수신자의 생존 상태, 거리, 관전 대상을 기준으로 routing합니다.

## Capture and Transmit Gate

`UGP_LocalVoiceSubsystem::CalculateTransmitGateOpen()`은 Muted, Open Mic, Push To Talk 모드를 구분합니다. Open Mic의 active 상태는 loudness threshold와 짧은 hold time으로 결정하고, PTT는 설정된 key의 실제 pressed 상태를 사용합니다.

`AGP_PlayerController::TickLocalVoiceFrameSend()`은 활성 gate에서 `UGP_MicInputProvider::PopPcm16VoiceFrame()`을 읽어 `C_VoiceFrame`을 구성하고 `UClientNetSubsystem::SendVoiceFrame()`으로 보냅니다. 한 tick에 처리할 frame 수도 제한합니다.

```cpp
C_VoiceFrame Frame{};
Frame.sequence = VoiceFrameSequence++;
Frame.codec = static_cast<uint8>(EVoiceCodecNet::Pcm16);
Frame.channels = 1;
Frame.dataBytes = static_cast<uint16>(DataBytes);
Frame.sampleRate = static_cast<uint32>(SampleRate);

const bool bSent = Net->SendVoiceFrame(Frame);
```

## Server Routing

`DungeonRoom::OnVoiceFrame()`은 packet의 player identity와 byte 범위를 확인하고 송신자 자신은 대상에서 제외합니다.

- **Alive → Alive:** proximity radius 안에서 거리에 따라 `volume01`을 선형 감쇠합니다.
- **Spectator → Spectator:** 별도 spectator channel로만 전달하며 생존자에게 보내지 않습니다.
- **Alive → Spectator:** 관전자의 과거 사망 위치가 아니라 현재 관전 중인 살아 있는 target의 위치를 청취 기준으로 사용합니다.

```cpp
if (receiverPlayerId == 0 || receiverPlayerId == playerId)
    continue;

const float dist = std::sqrt(distSq);
const float volume01 =
    std::clamp(1.0f - (dist / radius), 0.0f, 1.0f);

if (volume01 < playerVoiceMinSendVolume)
    continue;
```

`S_VoiceFrame`에는 speaker id, sequence, codec/sample 정보, life state와 서버가 계산한 `volume01`이 들어갑니다.

## Spectator Listening Anchor

`AGP_PlayerController::CycleSpectateTarget()`은 선택한 player id를 `SendSpectateTarget()`으로 알립니다. Server의 `OnSpectateTarget()`은 요청자가 spectator이고 대상이 살아 있을 때만 `spectateTargetByPlayerId`를 갱신합니다.

```cpp
const auto spectateIt = spectateTargetByPlayerId.find(receiverPlayerId);
if (spectateIt == spectateTargetByPlayerId.end() || spectateIt->second == 0)
    continue;

const auto anchorIt = playerSnapshot.find(spectateIt->second);
if (anchorIt == playerSnapshot.end() || IsSpectatorVoiceState(anchorIt->second))
    continue;

const PlayerState& anchorState = anchorIt->second;
```

이 기준점 덕분에 관전 카메라와 청취 공간이 일치합니다. 유효하지 않거나 사망한 target은 청취 anchor로 사용하지 않고, target 변경/이탈 시 mapping을 정리합니다.

## Pending Queue and Playback

Client receive 경로는 `S_VoiceFrame`의 길이를 검사한 뒤 `PendingVoiceFrames`에 복사합니다. Queue는 최대 48 frame으로 제한되고 초과 시 오래된 frame부터 제거합니다.

GameThread의 `PopVoiceFrames()`는 `MoveTemp`로 batch를 넘깁니다. `UGP_RemoteVoicePlaybackSubsystem::PushVoiceFrame()`은 codec, channel, sample rate, byte 수와 volume을 다시 검증하고, speaker별 procedural sound stream에 PCM을 넣습니다. 재생 byte queue도 상한을 넘으면 stream을 재생성합니다.

## Result and Scope

입력 모드, Server 권위 routing, spectator channel, 관전 대상 기반 proximity와 bounded playback을 하나의 경로로 연결했습니다. 현재는 PCM16 frame을 전달하므로 bandwidth 확장이 필요하면 codec 압축과 jitter buffer를 추가할 수 있습니다.

주요 원본 함수: `CalculateTransmitGateOpen()`, `TickLocalVoiceFrameSend()`, `SendVoiceFrame()`, `DungeonRoom::OnVoiceFrame()`, `DungeonRoom::OnSpectateTarget()`, `PopVoiceFrames()`, `PushVoiceFrame()`.
