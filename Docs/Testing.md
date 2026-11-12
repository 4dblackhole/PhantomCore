# CPU 테스트와 장치 통합 테스트

`Tests/PhantomCore.Cpu.Tests.vcxproj`는 Camera·충돌·입력·Visual2D 트리/위젯/표면과
화면 관리자의 CPU 계약을 검사합니다. 테스트는 공개 `PhantomCore.h`만 사용하며,
기존 SDK 테스트와 동일한 테스트 본문 및 실제 엔진 CPU 소스를 빌드합니다.
별도 엔진 동작이나 테스트용 오디오 backend를 제공하지 않습니다.

이 프로젝트는 통합 SDK 프로젝트를 참조하지 않고 FMOD 헤더·import library·DLL을
사용하지 않습니다. 공개 헤더는 SDK와 같은 manifest와 생성기로 구성별 중간 디렉터리에 만듭니다.
전체 SDK 빌드와 같은 출력 파일에 동시에 쓰지 않습니다.
Windows SDK와 MSVC v143/C++20은 필요합니다. DirectX 수학 기반 CPU 질의가 포함되며,
실제 그래픽 장치나 오디오 장치를 초기화하지 않습니다.

x64 Developer PowerShell에서 구성별로 실행합니다.

```powershell
foreach ($configuration in 'Debug', 'Release') {
    msbuild Tests/PhantomCore.Cpu.Tests.vcxproj /m /t:Rebuild `
        /p:Configuration=$configuration /p:Platform=x64 /p:RunCodeAnalysis=true
    if ($LASTEXITCODE -ne 0) { throw 'CPU test build failed' }
    & "./bin/CpuTests/x64/$configuration/PhantomCore.Cpu.Tests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'CPU tests failed' }
}
```

산출물은 FMOD 런타임을 복사하는 통합 테스트와 별도 디렉터리에 생성됩니다.
[CPU CI](../.github/workflows/cpu-tests.yml)는 Debug·Release에서 존재하지 않는
`FmodRoot`를 명시해 빌드하고 실행합니다.

`Tests/PhantomCore.Collision.Tests.vcxproj`는 통합 SDK를 사용합니다. 기본 실행은
공유 CPU 검사와 실제 FMOD NoSound 서비스 기반 재생 관리 검사를 모두 수행합니다.
장치를 쓰는 검사는 옵션 하나씩 별도 프로세스로 실행합니다.

| 옵션 | 검사 내용 |
| --- | --- |
| `--input-state` | 순서 있는 Raw Input 이벤트·키 전이·버퍼 재사용 |
| `--texture-lifetime` | 텍스처 변경 권한, descriptor 상한·재사용, fence 수명 |
| `--texture-assets <자산 루트>` | 지정한 WIC 이미지 전체의 등록·재사용·Canvas 렌더링 |
| `--text-rendering` | 글꼴·배치 캐시·아틀라스 회수와 GPU 진단 |
| `--text-pixels` | 글자 합성 픽셀 |
| `--visual2d-pixels` | Canvas의 사각형·이미지·글자 합성 순서와 깊이, 월드 패널 |
| `--image-readback` | 완료 프레임의 RGBA 읽기와 취소 |
| `--draw-statistics` | 성능 표본의 프레임당 draw 수 |
| `--scene-manager` | 지연 전환, 보존 정책과 종료 순서 |
| `--window-control` | 창 크기 변경과 해상도 조회 |
| `--headless-error` | 숨김 실행의 Update 예외 정리 |
| `--audio-pcm [경로]` | 공통 PCM 디코딩과 고정 속도 변환 |

이 프로젝트와 전체 솔루션의 빌드에는 FMOD SDK가 필요합니다. CPU CI 통과는 실제
GPU·오디오 실행의 검증을 대신하지 않습니다.

`--texture-lifetime`은 렌더 스레드·현재 관리자 세대의 변경 권한, 같은 handle append와 기존
슬롯 보존, 이전 세대 handle 해제 후 새 heap 격리, descriptor 상한·재사용·fence 수명을 검사합니다.
독립된 엔진 실행은 각각 새 스레드에서 수행합니다.

`--texture-assets <자산 루트>`는 명시한 경로의 WIC 이미지들을 모두 등록하고 같은 경로의 handle
재사용·크기를 확인한 뒤 실제 Canvas로 렌더링합니다. 게임 자산 경로를 엔진에 내장하지 않습니다.
두 검사는 공개 SDK만 사용하며 GPU 오류·손상 진단의 수집 성공/불가/실패를 구분합니다.
진단 수집 불가는 오류가 없다는 검증을 뜻하지 않습니다.

새 CPU 구현 의존성이 생기면 CPU 프로젝트의 소스/필터 목록도 함께 갱신하고,
장치 초기화가 필요한 검사는 통합 실행 경로에 둡니다.
