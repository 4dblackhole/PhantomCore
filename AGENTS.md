# PhantomCore 작업 규칙

이 저장소 전체에 적용합니다.

## 시작

- 새 세션에서 엔진 코드를 바꾸기 전에 `Docs/EngineOverview.md`를 읽습니다. 기능·소유·소스 위치·Client 경계·
  검증과 나머지 상세 문서의 기준 색인입니다.
- 엔진과 Client를 함께 바꾸는 작업은 변경 위치를 정하기 전에 Client 저장소의 `AGENTS.md`,
  `Docs/Contributing/Workflow.md`, `Docs/ExecutionFlow.md`도 읽습니다.
- 주요 공개 기능·소유 경계·검증 규칙이 바뀌면 `README.md`, `Docs/EngineOverview.md`, 기능 문서를 함께 맞춥니다.

## 목적과 경계

- 이 저장소에는 재사용 가능한 Windows 게임 엔진 코드만 둡니다.
- C++20, Visual Studio 2022, Win32, DirectX 12로 빌드하고, FMOD는 backend-neutral 오디오 API 뒤에 둡니다.
- 지원하는 사용 프로젝트는 `Engine/SDK/PhantomCore.vcxproj`입니다. `PhantomCore.lib` 하나를 빌드하고
  단일 공개 헤더 `PhantomCore.h`를 생성합니다.
- 게임 전용 자산, Scene ID, 게임 규칙, Client 비공개 구현을 여기에 넣지 않습니다.
- FMOD SDK 파일·import library·런타임 DLL을 커밋하지 않습니다.

## 소스 규칙

- 기능 단위 실제 폴더를 유지하고 기능의 `.h`와 `.cpp`를 함께 둡니다. `.vcxproj.filters` 경로를 실제 경로와 맞춥니다.
- 공개 API에 FMOD·D3D12 구현 전용 타입을 노출하지 않습니다.
- Client에 공개하는 헤더와 선언 순서는 `Engine/SDK/PublicHeaders.props` 한 곳에 적고 MSBuild와
  `Engine/SDK/GenerateCoreHeader.ps1`이 함께 씁니다. 생성된 `Engine/SDK/PhantomCore.h`를 손으로 고치지 않습니다.
- RAII와 `Microsoft::WRL::ComPtr`를 우선합니다. API가 소유를 명시하지 않은 raw pointer는 소유하지 않는 포인터입니다.
- 제한 없는 Update 루프, QPC 입력 시각, 렌더 마감, 독립 오디오 갱신 마감, 프레임 자원 fence 모델을 유지합니다.
- Raw Input은 Win32 Virtual-Key 값을 그대로 씁니다. 엔진 키 enum을 추가하지 않습니다.
- `SceneManager`가 Scene 팩토리와 수명 정책을 소유합니다. `RegisterScene`으로 등록하고 지연된 `ChangeScene`으로 전환하며,
  Scene이 자기 자신을 지우게 하지 않습니다.
- 공용 셰이더는 `Engine/Graphics.D3D12/Shader`에 두고 빌드할 때 포함합니다.
- `Engine/Core/Visual2D`의 노드·컴포넌트·입력 라우팅·9개 앵커·surface UV 매핑은 backend-neutral로 유지합니다.
  월드 표시는 `Visual2DCanvas`를 조합하며 상속하지 않습니다. D3D12 그리기는 `Engine/Graphics.D3D12/Visual2D`에 둡니다.
- 마우스 절대 위치는 UI 배치용 데이터입니다. 리듬 판정은 계속 순서가 보장된 Raw Input QPC 이벤트를 씁니다.
- 함수가 커져도 읽기 쉽게 유지합니다. 여러 동작을 하면 동작마다 이름 있는 도우미·private 함수로 뽑습니다.
  길어도 하나의 응집된 동작이면 의미 있는 단계 경계마다 의도와 필요한 순서를 짧게 설명하고,
  코드를 되풀이하는 주석은 달지 않습니다.
- 새로 쓰거나 고치는 `if`/`while` 조건에는 `&&`/`||`를 두 개까지 씁니다. 더 길면 조기 반환, 이름 붙인 `bool`,
  술어로 나누고 평가 순서와 부작용을 유지합니다.

## Git

- 작업은 최신 `origin/main`에서 만든 작업 브랜치(`<세션>/<주제>`)와 자기 워크트리에서 합니다. main에 직접 커밋·push하지
  않습니다. 절차(병합 요청 형식, 지휘 세션의 병합)는 Client 저장소의 `Docs/Contributing/Workflow.md`를 따릅니다.
- 검증한 뒤 브랜치를 push하고 PR을 열고 지휘 세션에 병합 요청을 보냅니다. 자기 PR을 병합하지 않습니다.
- 엔진과 Client를 함께 바꾸면 엔진 PR을 먼저 병합하고, Client PR에서 서브모듈 gitlink를 병합된 엔진 `main` 커밋으로
  올립니다. Client gitlink와 체크아웃된 엔진 `HEAD`가 같은지 확인합니다.
- 병합 뒤에는 두 저장소의 `main`을 fast-forward하고 Client에서 서브모듈을 재귀적으로 sync/update합니다.
  보고할 때 정확한 커밋 ID를 적습니다.
- 필요한 PR이 열려 있는 동안 `main`이 최신이라고 하지 않습니다. 실패한 검사·병합 충돌·브랜치 보호를 우회하지 않고
  막힌 이유를 보고합니다. 동기화하면서 관련 없는 작업 파일을 보존합니다.

## 검증

엔진을 바꾸면 `PhantomCore.sln`을 Debug와 Release x64로 MSVC Code Analysis와 함께 다시 빌드하고, 새 진단은 실패로 봅니다.
두 테스트 실행 파일을 두 구성 모두 실행합니다.

- `bin/CpuTests/x64/<Configuration>/PhantomCore.Cpu.Tests.exe`: FMOD 없이 빌드되며 CPU CI 워크플로도 실행합니다.
- `bin/x64/<Configuration>/PhantomCore.Collision.Tests.exe`: 인자 없이 한 번, 이어서 `Docs/Testing.md`에 적힌 장치 옵션마다
  한 번씩 실행합니다. 옵션마다 별도 프로세스로 실행합니다.

테스트는 `PhantomCore.h`만 include합니다. 소스 파일을 추가·이동·삭제하면 기능 프로젝트, `Engine/SDK/PhantomCore.vcxproj`,
두 `.filters` 파일, CPU 소스라면 `Tests/PhantomCore.Cpu.Tests.vcxproj`도 고칩니다.
다른 빌드나 테스트가 PC를 쓰는 동안 시간에 민감한 장치 테스트를 돌리지 않습니다(Client 저장소의 공유 검증 잠금 참고).
자동 검사와 사람이 한 화면·입력·청음 확인을 구분해 보고합니다.
