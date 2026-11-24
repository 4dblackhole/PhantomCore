# 엔진 진단 기록

게임이 꺼졌을 때 무슨 일이 있었는지 알 수 있도록, `Run`이 아는 사실을 Client에 넘겨줍니다.
엔진은 무엇을 기록할지만 정합니다. 파일을 만들거나 창을 띄우지 않으며, 어디에 남길지는 Client가 정합니다.

선언은 `Engine/Core/Diagnostics/EngineDiagnostics.h`에 있고 `PhantomCore.h`에 포함됩니다.

## 연결

Client는 `IEngineDiagnosticSink`를 구현해 `EngineConfig::diagnostics.sink`에 넣습니다. 엔진은 받는 쪽을 소유하지 않으므로
`Run`이 끝날 때까지 살아 있어야 합니다. 넣지 않으면 아무것도 기록하지 않습니다.

```cpp
class MyLog final : public PhantomCore::IEngineDiagnosticSink
{
public:
    void OnEngineRecord(std::string_view line) noexcept override;
    void OnGraphicsDeviceLost(const PhantomCore::GraphicsDeviceLossReport& report) noexcept override;
    PhantomCore::FatalErrorPresentation OnFatalError(std::string_view message) noexcept override;
};

PhantomCore::EngineConfig MyGame::GetEngineConfig() const
{
    PhantomCore::EngineConfig config;
    config.diagnostics.sink = &log_;
    return config;
}
```

세 함수는 모두 `Run`을 부른 스레드에서 불립니다. 예외를 던지면 안 되고 빨리 돌아와야 합니다.

## 무엇이 언제 오는가

| 함수 | 언제 | 내용 |
| --- | --- | --- |
| `OnEngineRecord` | 그래픽 장치를 만든 직후, 그 뒤로는 값이 바뀔 때마다 | 줄바꿈 없는 ASCII 한 줄 |
| `OnGraphicsDeviceLost` | 그래픽 장치가 끊긴 것을 알았을 때 한 번. 엔진이 아무것도 해제하기 전 | `GraphicsDeviceLossReport` |
| `OnFatalError` | 실행을 끝내는 예외마다 한 번. Client `Shutdown`과 엔진 서비스 해제가 끝난 뒤 | 예외 문구 |

시작 때 오는 줄은 다음과 같습니다.

```text
graphics adapter: NVIDIA GeForce GTX 1060 3GB, vendor 10DE device 1C02, 3072 MB dedicated, driver 32.0.15.6094
swap chain: 1920x1080, 2 buffers, tearing allowed
device removed data: off
window: 1920x1080 Windowed, display 360 Hz, render rate Display
```

실행 중에는 창 크기(`window size: 1280x720`), 창 모드(`window mode: Borderless`), 모니터 주사율(`display: 144 Hz`),
렌더 속도 모드(`render rate: Unlimited`)가 바뀔 때 한 줄씩 옵니다. 값이 그대로면 줄이 오지 않으므로 평소에는 기록이 없습니다.

1초마다 달라지는 값(프레임 수, 가장 긴 루프 한 바퀴, 그래픽 메모리)은 줄로 보내지 않습니다.
`UpdateContext::performance`에 있으므로 Client가 필요한 만큼 보관합니다([성능 통계](PerformanceStatistics.md)).

## 그래픽 장치 끊김

D3D12·DXGI 호출이 장치 제거·멈춤·리셋 결과(`DXGI_ERROR_DEVICE_REMOVED`, `_HUNG`, `_RESET`, `_DRIVER_INTERNAL_ERROR`)로 실패하면
엔진은 `GraphicsDeviceLostError`를 던집니다. `std::runtime_error`를 상속하고 문구도 그대로(`<호출> failed with HRESULT 0x........`)이므로
기존 처리기는 바뀌지 않습니다. 장치 끊김만 구분하고 싶은 처리기는 이 형식으로 잡습니다.

예외가 `Run`까지 올라오면 엔진은 렌더러가 살아 있는 동안 보고서를 만들어 `OnGraphicsDeviceLost`로 넘깁니다.
다른 예외를 처리하다가 장치가 이미 끊겨 있는 것을 알게 된 때도 같습니다(이때 `operation`은 비어 있습니다).
그다음 순서는 평소의 실패 종료와 같습니다: Client `Shutdown` → 엔진 서비스 해제 → `OnFatalError`.

| 필드 | 뜻 |
| --- | --- |
| `operation`, `resultCode` | 실패한 엔진 호출의 이름과 결과 |
| `removedReason`, `removedReasonName` | 장치가 밝힌 제거 사유(숫자와 이름) |
| `renderIndex`, `width`, `height` | 마지막으로 시작한 프레임 번호와 그때의 back buffer 크기 |
| `lastFrameDrawCalls` | 마지막으로 끝낸 프레임의 draw call 수 |
| `memory` | 이 프로세스의 그래픽 메모리 사용량·예산·시스템 메모리로 밀려난 양 |
| `extendedDataEnabled`, `extendedData` | 아래 장치 제거 추가 정보 |

엔진은 끊긴 장치를 되살리지 않습니다. 실행은 실패로 끝납니다.

## 장치 제거 추가 정보

`EngineConfig::diagnostics.graphicsDeviceRemovedData`를 켜면 D3D12의 DRED를 켭니다. 장치가 끊겼을 때 명령 목록마다
GPU가 마지막으로 끝낸 동작과, 페이지 폴트가 있었다면 그 주소 주변의 할당을 `extendedData`에 한 줄씩 담습니다.

```text
command list Frame on queue Main queue: completed 41 of 57 operations; DrawInstanced DrawInstanced >> DrawInstanced ResourceBarrier
```

`>>` 뒤가 GPU가 끝내지 못한 첫 동작입니다. 명령 목록 이름은 엔진이 붙입니다(`Frame`, `Texture upload`).

기본은 꺼짐입니다. 모든 그리기 명령에 표시를 남기므로 비용이 조금 있고, 장치를 만들기 전에만 정할 수 있습니다.
크래시를 쫓을 때만 Client 설정으로 켜는 것을 권합니다. DRED가 없는 Windows에서는 켜도 꺼진 것으로 기록됩니다.

## 치명적 오류 창

`OnFatalError`가 `FatalErrorPresentation::Handled`를 돌려주면 엔진은 자기 오류 창을 띄우지 않습니다. Client가 기록 파일 위치를 함께
알리는 창을 직접 띄울 때 씁니다. `EngineDialog`를 돌려주면 지금까지처럼 엔진이 문구만 담은 창을 띄웁니다.
숨김 실행(`showWindow=false`)에서는 어느 쪽이든 엔진 창이 없습니다.

## 일부러 끊어 보기

`EngineConfig::diagnostics.removeGraphicsDeviceAfterRenderedFrames`에 0이 아닌 값을 넣으면 그만큼 렌더한 뒤 엔진이 장치를 일부러
제거합니다(`ID3D12Device5::RemoveDevice`). 위 경로 전체를 Release 빌드에서도 그대로 시험할 수 있습니다.
Client는 이 값을 숨은 실행 인자에만 연결하고, 사용자의 설정 파일에는 두지 않습니다.

`PhantomCore.Collision.Tests --device-loss`가 이 값으로 숨긴 엔진을 세 번 돌려 확인합니다.

- 정상 실행: 시작 기록 네 줄이 오고, 끊김·치명적 오류는 오지 않으며, 성능 표본에 가장 긴 루프 시간이 들어 있습니다.
- 장치 제거(추가 정보 꺼짐·켜짐): 실행이 실패로 끝나고, 보고서가 한 번, 치명적 오류가 한 번 오며,
  순서가 보고서 → Client `Shutdown` → 치명적 오류입니다.
