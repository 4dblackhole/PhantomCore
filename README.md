# PhantomCore

PhantomCore은 Visual Studio 2022, C++20, DirectX 12 기반의 Windows 게임 엔진
저장소다. 게임 전용 코드는 포함하지 않으며, 소비 프로젝트에는 통합 정적
라이브러리 `PhantomCore.lib`와 단일 공개 헤더 `PhantomCore.h`를 제공한다.
공개 타입과 함수는 `PhantomCore` 네임스페이스에 있으며 이전 전역 별칭은 제공하지 않는다.
3D `MeshInstance`는 Camera의 Perspective/Orthographic 절두체와 로컬 Bounding
Sphere를 사용해 화면 밖 인스턴스를 제출 전에 제외한다.
Visual2D Canvas는 화면과 월드 표면 모두 정중앙 원점과 Y-up 논리 좌표를 사용한다.
화면의 사각형·이미지·글자는 Canvas Z와 트리 순서대로 알파 합성한다.
월드 평면 Canvas도 내부를 트리 순서로 합성하고 한 장의 패널 평면에서 3D 깊이를 검사한다.
공개 수학 값은 엔진 소유 Float2/3/4와 Float4x4를 사용한다([수학 계약](Docs/Math.md)).
세부 순서와 배치는 [Visual2D 구조](Docs/Visual2DArchitecture.md)를 따른다.
프레임 공개 계약은 크기·렌더 서비스·ReadImage를 제공하는 RenderContext이며,
기록 상태와 구체 Renderer는 엔진 내부에서 소유한다.
Visual2D 텍스트와 ComboBox는 renderer 기본 글꼴, 시스템 family 또는 배포 글꼴
파일을 backend-neutral `TextFont`로 선택할 수 있다.
불변 사각형 묶음 `RectangleBatch`는 geometry를 공유하여 매 프레임 명령 복사를 줄인다.
`SceneGameClient::AudioPlayback()`은 Scene 사이에서 공유하는 재생 관리자를 제공한다.
사용법과 Clip·Voice 수명은 [AudioPlayback.md](Docs/AudioPlayback.md)에 있다.
`StreamAsync`는 파일 열기를 비동기로 요청하고 `AudioClip::LoadState`로 준비 상태를 조회한다.
더 필요 없는 로딩은 `AudioClip::CancelLoading`으로 디코딩을 멈추고 Failed로 끝낸다.
Voice의 `PreparationState`는 재생/위치 변경의 비동기 준비를 진행한다. 음악 재개는
paused Voice의 준비 완료 후 clock을 예약하며 종료된 샘플은 관리자가 같은 ID로 교체한다.
`SampleAsync`도 준비 상태를 확인한 뒤 재생한다. Visual2D의 `PrepareImages`는 CPU 이미지
디코드를 비동기로 준비하며 GPU 업로드는 fence까지 자원을 보관해 이미지별 대기를 제거한다.
해제된 텍스처의 SRV 블록은 업로드·GPU 프레임의 마지막 참조 이후 재사용한다.
Raw Input 이벤트 버퍼는 한 Update의 모든 키 전이를 순서대로 유지하도록 확장된다.
`IWindowControl::CreateTextInput`은 IME·커서·선택·붙여넣기를 지원하는 Unicode 편집기를
제공한다. 소유권과 입력 명령 계약은 [창·텍스트 입력](Docs/WindowControl.md)에 있다.
텍스트는 유지된 배치와 2,048항목 보조 LRU를 사용하고 clip 밖의 글리프를 제외한다.
회색조 아틀라스는 기본 16MiB에서 필요한 만큼 확대·회수하며 새 글리프를 upload arena에 묶는다.
TextStyle/TextVisualComponent는 1~999 굵기를 지원하며 글꼴 배치·글리프 캐시를 굵기별로 구분한다.
상세 계약은 [Visual2D 성능·소유권](Docs/Visual2DPerformanceAndOwnership.md)과
[텍스트 렌더링](Docs/TextRendering.md)에 있다.
`StreamAsyncAccurate`는 정확한 길이/PCM seek 준비도 비동기로 수행한다. Clip 길이 조회,
마이크로초 시작 위치와 준비된 Voice의 DSP 예약, 취소 Clip의 지연 해제를 지원한다.
화면 Sprite는 `SceneGameClient::ScreenVisuals()`가 Canvas와 이미지 등록을 소유하고
Scene 대신 매 프레임 제출한다. 월드·곡면 Canvas는 명시적 pass로 유지한다.
`RenderContext::ReadImage()`로 완료된 화면 프레임을 RGBA 이미지로 읽을 수 있다.
[프레임 이미지 읽기](Docs/ImageReadback.md)
`UpdateContext::performance`는 FPS/UPS, 프레임당 draw 수, 가장 긴 루프 시간과 그래픽 메모리를 1초마다 제공한다.
[성능 통계](Docs/PerformanceStatistics.md)
`EngineConfig::diagnostics`의 받는 쪽은 시작 기록, 그래픽 장치 끊김 보고서, 치명적 오류와 메인 루프 멈춤을 받는다. 엔진은 파일을 만들지 않는다.
`CrashDumpService`는 프로세스가 죽을 때와 요청할 때 미니덤프를 Client가 정한 폴더에 쓴다.
`BackgroundWork`는 메인 루프 밖에서 도는 작업(그림 준비, 소리 읽기 등)을 표시해, 멈춤·장치 끊김 때 진행 중이던 작업을 알 수 있게 한다.
시험 전용 `ThrownExceptionWatch`는 정상 실행이 C++ 예외를 던지지 않는지 테스트·smoke에서 센다.
[엔진 진단 기록](Docs/EngineDiagnostics.md)

처음 사용하는 개발자나 새 작업 세션은
[Docs/EngineOverview.md](Docs/EngineOverview.md)부터 읽는다. 주요 기능, 코드 위치,
객체 수명, Client 경계, 기능별 문서와 작업 체크리스트를 한곳에서 확인할 수 있다.

## 저장소 구성

```text
Engine/       플랫폼, 오디오, 충돌, 도형, D3D12 그래픽, 코어와 SDK
Tests/        공개 SDK 헤더만 사용하는 엔진 테스트
Docs/         엔진 통합 및 기능 문서
PhantomCore.sln
```

기능별 모듈 프로젝트는 독립 개발과 탐색에 사용한다. 실제 게임은
`Engine/SDK/PhantomCore.vcxproj` 하나만 프로젝트 참조로 연결한다.

## 요구 사항

- Visual Studio 2022, MSVC v143, Windows 10/11 SDK
- FMOD Studio API for Windows
- `FMOD_ROOT` 환경 변수 또는 `Directory.Build.props`가 탐지하는 기본 FMOD 경로

FMOD SDK 헤더, import library와 DLL은 라이선스상 이 저장소에 포함하지 않는다.

## 빌드

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' `
  PhantomCore.sln /m /t:Rebuild /p:Configuration=Debug /p:Platform=x64

& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' `
  PhantomCore.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=x64
```

산출물은 `bin/x64/<Configuration>/`에 생성된다. 엔진 전체 안내는
[Docs/EngineOverview.md](Docs/EngineOverview.md), 자세한 연결 방법은
[Docs/EngineIntegration.md](Docs/EngineIntegration.md)를 참고한다.

## Client 저장소에서 사용

현재 `YOUMU`는 이 저장소를
`Dependencies/PhantomCore` Git submodule로 고정한다. Client 프로젝트는
submodule 내부의 `Engine/SDK/PhantomCore.vcxproj`만 참조하며 엔진의 기능 헤더를
직접 포함하지 않는다.

엔진의 공개 API 검증은 `Tests/PhantomCore.Collision.Tests.vcxproj`의 SDK 테스트와
옵션별 장치 검사를 사용한다. FMOD 없는 CPU 검사는 `Tests/PhantomCore.Cpu.Tests.vcxproj`로
분리되어 있으며 Debug·Release CI에서 빌드·실행한다. 범위와 실행법은 [Testing](Docs/Testing.md)을 참고한다.

실행 중 창 크기 변경과 모니터 해상도 조회는 EngineServices의 비소유
[IWindowControl 서비스](Docs/WindowControl.md)를 사용합니다. 게임별 저장 정책은 Client가 소유합니다.

Render 호출 빈도는 모니터 실제 주사율의 1배·2배·무제한 가운데 고릅니다(`EngineConfig::renderRateMode`,
실행 중에는 `EngineServices::renderRateControl`). 계약은 [EngineOverview](Docs/EngineOverview.md)의 시간·입력·렌더 스케줄에 있습니다.

오디오 재생과 외부 파형 분석은 공통 `DecodeAudioPcm` 시간축을 사용할 수 있습니다. MP3 패딩 처리와 메모리 비용은 [AudioPlayback](Docs/AudioPlayback.md#공통-pcm-시간축)을 참고합니다.

고정 속도 PCM 음정 유지와 원본 좌표 탐색은 [AudioPlayback](Docs/AudioPlayback.md)의 PrepareTimeStretch를 참고합니다.
