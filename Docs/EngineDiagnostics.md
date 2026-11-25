# 엔진 진단 기록

게임이 꺼졌을 때 무슨 일이 있었는지 알 수 있도록, `Run`이 아는 사실을 Client에 넘겨줍니다.
엔진은 무엇을 기록할지만 정합니다. 파일을 만들거나 창을 띄우지 않으며, 어디에 남길지는 Client가 정합니다.

선언은 `Engine/Core/Diagnostics/EngineDiagnostics.h`(받는 쪽), `Engine/Platform.Win32/System/CrashDump.h`(덤프),
`Engine/Platform.Win32/System/ThrownExceptionWatch.h`(시험 전용 예외 계수)에 있고 `PhantomCore.h`에 포함됩니다.

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
    void OnMainLoopStalled(const PhantomCore::MainLoopStallReport& report) noexcept override;
    void OnMainLoopResumed(double stalledSeconds) noexcept override;
};

PhantomCore::EngineConfig MyGame::GetEngineConfig() const
{
    PhantomCore::EngineConfig config;
    config.diagnostics.sink = &log_;
    return config;
}
```

어느 함수도 예외를 던지면 안 됩니다. 앞의 세 함수는 `Run`을 부른 스레드에서 불리고 빨리 돌아와야 합니다.
멈춤을 알리는 두 함수는 엔진의 감시 스레드에서 불립니다. 멈춘 것이 바로 `Run`을 부른 스레드이기 때문입니다.

## 무엇이 언제 오는가

| 함수 | 언제 | 내용 |
| --- | --- | --- |
| `OnEngineRecord` | 그래픽 장치를 만든 직후, 그 뒤로는 값이 바뀔 때마다 | 줄바꿈 없는 ASCII 한 줄 |
| `OnGraphicsDeviceLost` | 그래픽 장치가 끊긴 것을 알았을 때 한 번. 엔진이 아무것도 해제하기 전 | `GraphicsDeviceLossReport` |
| `OnFatalError` | 실행을 끝내는 예외마다 한 번. Client `Shutdown`과 엔진 서비스 해제가 끝난 뒤 | 예외 문구 |
| `OnMainLoopStalled` | 메인 루프가 정한 시간 동안 한 바퀴도 끝내지 못했을 때, 멈춤마다 한 번(감시 스레드) | `MainLoopStallReport` |
| `OnMainLoopResumed` | 알린 멈춤이 풀렸을 때(감시 스레드) | 멈춰 있던 시간 |

시작 때 오는 줄은 다음과 같습니다.

```text
graphics adapter: NVIDIA GeForce GTX 1060 3GB, vendor 10DE device 1C02, 3072 MB dedicated, driver 32.0.15.6094
swap chain: 1920x1080, 2 buffers, tearing allowed
device removed data: off
graphics options: validation off, adapter default
window: 1920x1080 Windowed, display 360 Hz, render rate Display
```

`swap chain` 줄과 `graphics options` 줄은 아래 [그래픽 장치 시험 설정](#그래픽-장치-시험-설정)의 실제 값을 보여 줍니다.

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

## 그래픽 장치 시험 설정

`EngineConfig::diagnostics.graphics`(`GraphicsDeviceDiagnosticsConfig`)는 그래픽 장치가 멈추거나 끊기는 원인을 가르는 시험용
스위치입니다. 기본값은 모두 지금까지의 엔진 동작과 같으므로, 아무것도 정하지 않은 Client의 렌더링은 바뀌지 않습니다.
모두 장치와 swap chain을 만들 때 정해지고 실행 중에는 바뀌지 않습니다.

| 필드 | 기본 | 뜻 |
| --- | --- | --- |
| `validation` | `Off` | `DebugLayer`: D3D12 디버그 레이어가 모든 호출을 CPU에서 검사. `GpuBased`: 셰이더의 자원 접근까지 GPU에서 검사(몇 배 느림). Debug 빌드는 예전처럼 최소 `DebugLayer` |
| `validationMessageLimit` | 100 | 한 실행에서 기록으로 보내는 검증 메시지 수. 같은 종류는 세 번까지 |
| `adapter` | `Default` | `Warp`: 소프트웨어 래스터라이저(그래픽 드라이버를 거치지 않음, 느림). `Index`: `adapterIndex`번 어댑터(DXGI 목록 순서). `Name`: 이름에 `adapterName`이 들어간 첫 어댑터(ASCII, 대소문자 무시) |
| `allowTearing` | true | false면 찢어짐 허용 없이 만들고(`CreateSwapChainForHwnd`·`ResizeBuffers`) 표시합니다(`Present`) |
| `swapChainBuffers` | 2 | back buffer 수. 2 또는 3(그 밖의 값은 가까운 쪽으로) |
| `waitableSwapChain` | false | 대기 가능 swap chain, 표시 대기열 최대 1프레임. 프레임마다 swap chain이 받을 수 있을 때까지 기다림(최대 1초) |

### 시작 기록

```text
swap chain: 1280x720, 3 buffers, tearing off by setting, waitable (latency 1)
graphics options: validation debug layer + GPU-based, adapter index 3 (not found, default used)
```

- 찢어짐: `allowed`, `off by setting`(설정으로 끔), `not supported`(DXGI가 지원하지 않음).
- 검증: `off`, `debug layer`, `debug layer + GPU-based`, `debug layer (debug build)`(Debug 빌드의 기본).
  디버그 레이어가 없는 PC(Windows 선택 기능 "Graphics Tools"가 설치되지 않음)에서는
  `debug layer unavailable: D3D12 debug layer not installed (Windows Graphics Tools), running unchecked`로 남기고 검사 없이 평소대로 실행합니다.
  엔진은 그 기능을 설치하거나 시스템 설정을 바꾸지 않습니다.
- 어댑터: `default`, `WARP`, `index N`, `name "..."`. 찾지 못했거나 그 어댑터로 장치를 만들 수 없으면 기본 선택으로 돌아가고
  `(not found, default used)`·`(no device, default used)`를 붙입니다. 실제로 고른 어댑터는 `graphics adapter` 줄에 있습니다.

### 검증 메시지

`validation`을 켠 실행에서는 디버그 레이어의 경고·오류·손상 메시지를 `OnEngineRecord` 한 줄씩으로 보냅니다.
렌더러가 프레임을 마칠 때마다(`EndFrame`) 장치의 메시지 대기열에서 새로 쌓인 것을 꺼내므로 `Run`을 부른 스레드에서만 옵니다.
메시지 콜백(`ID3D12InfoQueue1`)은 쓰지 않습니다. 콜백은 메시지를 만든 스레드에서 불리는데, 받는 쪽은 `Run` 스레드에서만 부른다는
약속이 있기 때문입니다. 그래서 장치가 멈추기 직전의 메시지는 그 프레임이 끝나지 못하면 렌더러가 끝날 때 함께 나옵니다.

```text
graphics validation error #1234: <디버그 레이어 설명>
graphics validation: 100 messages recorded; later ones are only counted
graphics live object: Live ID3D12Device at 0x..., Refcount: 3
graphics validation summary: 0 corruptions, 0 errors, 2 warnings, 2 recorded, 0 dropped by the queue; 1 live object lines at shutdown
```

- 같은 종류(메시지 번호)는 세 번까지, 전체는 `validationMessageLimit`까지 보내고 나머지는 셉니다. 정보 수준 메시지는 보내지 않습니다.
- 렌더러가 끝날 때 장치를 뺀 모든 자원을 먼저 놓고 `ReportLiveDeviceObjects`를 불러 아직 살아 있는 객체를 `graphics live object` 줄로
  (최대 40줄) 보낸 뒤, 요약 한 줄로 끝냅니다. 장치 자신은 늘 한 줄 나옵니다. 실패로 끝나는 실행에서도 같습니다.
- 엔진이 만드는 자원·힙·할당자에는 이름이 있습니다(`Back buffer 0`, `Frame allocator 1`, `Glyph atlas page`, `Image texture` 등).
  검증 메시지, 살아 있는 객체 보고, 장치 제거 추가 정보에 그 이름이 나옵니다.
- `validation`을 정하지 않은 Debug 빌드는 예전처럼 메시지를 대기열에만 남깁니다(`RunWithGraphicsDiagnostics`가 읽음).
- 디버그 레이어와 GPU 기반 검증은 DRED처럼 프로세스 전체 설정입니다. 한 번 켠 프로세스에서는 다음 실행도 켜진 채입니다.

### 주의

- **찢어짐 끔:** 창이 모니터를 혼자 덮어 Windows가 합성을 건너뛰는 상태에서는 렌더와 Update가 화면 주사율에 묶입니다
  ([창 제어](WindowControl.md)). 엔진은 그 상태를 피하지만, 이 설정의 시험 결과를 볼 때 Update 수가 줄었는지 함께 봅니다.
- **버퍼 3장:** 프레임 링(명령 할당자, 업로드 영역, fence)은 `FrameCount`(2) 칸 그대로이고 back buffer만 늘어납니다.
  링 번호는 프레임마다 하나씩 돌고, 그리는 back buffer는 swap chain이 알려 주는 번호입니다. 한 back buffer를 다시 쓰는 것은
  그것을 마지막으로 쓴 프레임(3프레임 전)이 끝난 뒤이고, `BeginFrame`은 2프레임 전 프레임이 끝나기를 기다리므로 안전합니다.
  텍스트 아틀라스와 이미지 칸의 회수 규칙(`FrameCount` 기준)도 링 기준이라 바뀌지 않습니다.
- **대기 가능 swap chain:** 기다림이 메인 루프 안에서 일어나므로 렌더할 때마다 Update도 그만큼 늦습니다.

## 치명적 오류 창

`OnFatalError`가 `FatalErrorPresentation::Handled`를 돌려주면 엔진은 자기 오류 창을 띄우지 않습니다. Client가 기록 파일 위치를 함께
알리는 창을 직접 띄울 때 씁니다. `EngineDialog`를 돌려주면 지금까지처럼 엔진이 문구만 담은 창을 띄웁니다.
숨김 실행(`showWindow=false`)에서는 어느 쪽이든 엔진 창이 없습니다.

엔진은 `OnFatalError`를 부르기 전에 이 스레드의 메시지 큐를 비웁니다. 예외가 올라오는 동안 창이 파괴되며 종료 메시지를 남기는데,
그것이 남아 있으면 메시지 상자가 뜨자마자 닫히기 때문입니다. 그래서 Client는 `OnFatalError` 안에서 바로 `MessageBox`를 띄워도 됩니다.

## 메인 루프 멈춤 감시

`EngineConfig::diagnostics.mainLoopStallSeconds`에 0보다 큰 값을 넣으면 엔진이 메인 루프를 지켜봅니다.
루프는 한 바퀴마다 숫자 하나를 올리고, 감시 스레드가 0.25초마다 그 숫자를 봅니다. 정한 시간 동안 그대로면 `OnMainLoopStalled`를 부릅니다.
이때 Client가 덤프를 쓰면(아래 `CrashDumpService::WriteNow`) 메인 스레드가 어디에서 기다리는지 남습니다.

다음은 멈춤으로 보지 않습니다.

- **창 자신의 루프**: 창을 끌거나 크기를 바꾸는 동안, 메뉴, 모달 대화 상자. 이때는 메인 루프가 돌지 않아도 창이 메시지에 답합니다.
  감시 스레드는 알리기 전에 창에 빈 메시지를 보내 보고, 답이 오면 알리지 않습니다.
- **디버거**: 디버거가 붙어 있으면 알리지 않습니다(중단점).
- **PC 절전**: 시계가 아니라 본 횟수로 재므로, 절전에서 돌아온 뒤 없던 멈춤이 생기지 않습니다.
  창에 확인 메시지를 보내며 기다린 시간(최대 0.5초)은 멈춘 시간에 더합니다.
  본 횟수로 세는 만큼 알려 주는 길이는 실제보다 0.5초까지 짧을 수 있습니다.
- **루프 밖**: 감시는 메인 루프가 도는 동안에만 합니다. Client `Initialize`·`Shutdown`과 오류 창은 대상이 아닙니다.

정한 시간보다 긴 동기 작업(예: 한 번의 Update 안에서 하는 긴 로딩)은 멈춤으로 알립니다. 그런 작업보다 넉넉히 큰 값을 쓰십시오.
창 메시지에 답하면서 멈춘 경우(예: 메시지를 처리하며 기다리는 COM 호출)는 잡지 못합니다.

평소 비용은 루프 한 바퀴마다 정수 하나를 쓰는 것뿐입니다. 받는 쪽이 없거나 값이 0이면 스레드도 만들지 않습니다.

## 크래시 덤프

`CrashDumpService`가 살아 있는 동안 프로세스가 죽으면 `<폴더>/<접두사>-YYYYMMDD-HHMMSS-<사유>.dmp`를 남깁니다.
엔진 실행(`Run`)과 무관하게 쓸 수 있으므로 Client는 `wWinMain` 맨 앞에서 만듭니다.

```cpp
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    PhantomCore::CrashDumpService dumps(PhantomCore::ExecutableDirectory() / L"Crash", L"mygame", 5, &observer);
    return PhantomCore::Run(std::make_unique<MyGame>());
}
```

| 사유(파일 이름) | 언제 |
| --- | --- |
| `crash` | 처리되지 않은 구조적 예외: 접근 위반, 스택 넘침 등 |
| `terminate` | `std::terminate`: 아무도 잡지 않은 C++ 예외, `noexcept` 함수 밖으로 나간 예외 |
| `purecall` | 순수 가상 함수 호출 |
| `invalid-parameter` | C 런타임이 인자를 거부함 |
| `requested` 또는 준 이름 | `WriteNow`로 요청한 덤프. 프로세스는 계속 돕니다 |

- 덤프에는 모든 스레드의 호출 스택과 로드된 모듈이 들어가고 힙은 들어가지 않습니다. 스레드와 모듈 수에 따라 수십 KB에서 수 MB입니다.
- 프로세스 안에서만 처리합니다. 시스템 설정(오류 보고, 레지스트리)을 읽거나 바꾸지 않고, 파일은 PC 밖으로 나가지 않습니다.
  덤프를 쓴 뒤에는 Windows가 서비스가 없을 때와 같은 방식으로 프로세스를 끝냅니다.
- 죽는 스레드는 미리 만들어 둔 덤프 스레드에 일을 넘기기만 합니다. 그래서 스택 넘침이나 잠금을 쥔 채 죽은 경우에도 쓸 수 있습니다.
  덤프 라이브러리(`dbghelp.dll`, 시스템 폴더의 것)는 생성자에서 미리 로드합니다. Client는 import library를 추가하지 않습니다.
- 생성자는 폴더를 만들고 같은 접두사의 오래된 덤프를 `keepNewest`개만 남깁니다. `WriteNow` 뒤에도 같은 한도를 지킵니다.
- 서비스는 프로세스에 하나만 둘 수 있습니다.
- `ICrashDumpObserver::OnCrashDump`는 덤프 시도 뒤 덤프 스레드에서 불립니다. 크래시 뒤에는 프로세스가 손상된 상태이므로
  미리 연 핸들과 고정 버퍼만 쓰고, 할당하거나 다른 스레드가 쥘 수 있는 잠금을 잡지 않습니다.
- 덤프에는 실행 파일과 모듈의 전체 경로가 들어 있습니다. 사용자 폴더 아래에 설치했다면 사용자 이름이 보입니다.

잡지 못하는 경우도 있습니다. 런타임이 처리기를 거치지 않고 프로세스를 바로 끝내는 실패(`__fastfail`, 스택 버퍼 검사 실패),
덤프 스레드 자신의 고장, 그리고 죽은 스레드가 힙 잠금을 쥐고 있어 덤프 쓰기가 20초 안에 끝나지 않는 경우입니다.

### 덤프 읽기

덤프의 호출 스택을 함수 이름으로 보려면 그 빌드의 실행 파일과 PDB가 필요합니다. Client는 배포한 빌드마다 둘을 함께 보관합니다
(배포 폴더에 넣을 필요는 없습니다). Visual Studio에서 `.dmp`를 열거나 다음처럼 봅니다.

```powershell
cdb -z mygame-20261125-061530-crash.dmp -y <PDB가 있는 폴더> -c ".ecxr; k; q"
```

요청한 덤프(`WriteNow`)에는 예외 정보가 없으므로 `.ecxr` 대신 `~*k`로 모든 스레드를 봅니다. 메인 스레드는 보통 0번입니다.

## 던진 예외 세기 (시험 전용)

정상 실행은 C++ 예외를 던지지 않습니다. 예상할 수 있는 실패(취소, 없을 수 있는 파일 등)는 반환값·상태로 알립니다.
`ThrownExceptionWatch`는 그것을 시험·smoke 실행에서 확인하는 도구입니다. 배포 실행은 만들지 않으므로 비용이 없습니다.

- 살아 있는 동안 벡터 예외 처리기 하나가 체인의 맨 뒤에서 C++ 예외(0xE06D7363)를 셉니다.
  예외를 처리하지 않고, 할당·잠금을 하지 않으므로 프로그램의 동작과 크래시(위 `CrashDumpService`)는 그대로입니다.
- 셈 대상: 이 실행 파일(엔진 포함)과 그 C++ 런타임 라이브러리(Debug는 DLL)가 던진 것.
  던진 이미지는 예외 레코드의 `ExceptionInformation[3]`으로 가립니다.
  시스템 라이브러리(Media Foundation, WIC, 그래픽 드라이버 등)가 안에서 던지고 스스로 잡는 것은 세지 않습니다.
- 다시 던지기도 C++ 런타임에는 새 throw이므로 한 번 더 셉니다. 그냥 `throw;`와 `std::rethrow_exception`(`std::future::get`이 씀)이 모두 그렇습니다.
- 처음 16개는 스레드·형식 이름·`what()`(std::exception 파생일 때)·스택 16칸을 고정 배열에 남깁니다(`Records()`).
- 한 번에 하나만 만들 수 있습니다. 출력 방식은 정하지 않으므로, 엔진 테스트는 stderr에, Client는 자기 기록에 씁니다.
- 일부러 예외를 일으키는 시험 실행(예: 크래시 시험)에는 만들지 않습니다.

## 일부러 끊어 보기

`EngineConfig::diagnostics.removeGraphicsDeviceAfterRenderedFrames`에 0이 아닌 값을 넣으면 그만큼 렌더한 뒤 엔진이 장치를 일부러
제거합니다(`ID3D12Device5::RemoveDevice`). 위 경로 전체를 Release 빌드에서도 그대로 시험할 수 있습니다.
Client는 이 값을 숨은 실행 인자에만 연결하고, 사용자의 설정 파일에는 두지 않습니다.

멈춤과 크래시는 엔진 설정 없이 Client 코드로 일으킬 수 있습니다(Update에서 오래 자기, 예외 던지기).
`PhantomCore.Collision.Tests --main-loop-watch`는 Update 하나를 1.7초 붙잡아 멈춤이 한 번, 풀림이 한 번 오는지와,
같은 시간 동안 창 메시지 루프를 돌리면 오지 않는지를 봅니다. `--crash-dump`는 자식 프로세스를 다섯 가지 방법으로 죽이고
(구조적 예외, 스택 넘침, `std::terminate`, 순수 가상 호출, 잘못된 인자) 덤프 파일과 관찰자 통지를 확인하며,
요청한 덤프 세 개 가운데 두 개만 남는지도 봅니다. 임시 폴더에서 돌리고 끝나면 지웁니다.

`PhantomCore.Collision.Tests --device-loss`가 이 값으로 숨긴 엔진을 세 번 돌려 확인합니다.

- 정상 실행: 시작 기록 다섯 줄이 오고(기본 설정은 버퍼 2장), 끊김·치명적 오류는 오지 않으며, 성능 표본에 가장 긴 루프 시간이 들어 있습니다.
- 장치 제거(추가 정보 꺼짐·켜짐): 실행이 실패로 끝나고, 보고서가 한 번, 치명적 오류가 한 번 오며,
  순서가 보고서 → Client `Shutdown` → 치명적 오류입니다.
