# 실행 중 창 크기 제어

`EngineServices::windowControl`은 Run이 소유하는 `IWindowControl`의
비소유 포인터입니다. Initialize부터 Shutdown 시작까지 게임 루프 스레드에서만
사용합니다. 창이 없는 offscreen/test context에서는 nullptr일 수 있습니다.

- `ClientSize()`는 현재 게임 화면의 픽셀 크기를 반환합니다. 테두리/제목 표시줄은 제외합니다.
- `DisplayResolutions()`는 창이 위치한 모니터의 실제 지원 크기를 나열하고
  같은 크기의 주사율 변형을 중복 제거합니다. 장치 조회 오류는 예외로 보고합니다.
- `SetClientSize(size, error)`는 창의 client 영역을 즉시 변경합니다. 양수이며
  D3D12의 texture dimension 한도인 16,384px 이내의 크기만 허용합니다.
  최대화된 창은 복원하고 Z 순서를 유지합니다. 숨김 실행을 표시하지 않습니다.
  실패는 false와 오류 원문을 반환합니다.
- `Mode()`/`SetMode(mode, error)`는 창 모드(`WindowMode`)를 읽고 바꿉니다. 모드를 바꿔도 client 크기는 그대로입니다.
  시작 값은 `EngineConfig::windowMode`입니다. 창을 놓는 규칙은 크기와 모드 변경에 똑같이 적용됩니다.
  - client 크기가 모니터 전체를 덮으면 모드와 상관없이 테두리 없이 모니터 왼쪽 위에 놓아 화면을 가득 채웁니다.
  - `Windowed`: 제목 표시줄과 테두리가 있습니다. 지금 위치에서 작업 영역 안에 들어가면 위치를 유지하고,
    아니면 작업 영역 가운데로 옮겨 제목 표시줄이 화면 밖으로 나가지 않게 합니다.
  - `Borderless`: 테두리가 없습니다. 게임 화면을 원래 크기로 모니터 가운데에 놓고, 그 모니터의 나머지는 검은 배경 창이
    덮습니다. 배경 창은 게임 창 바로 뒤에 있고 포커스를 받지 않으며, 누르면 게임 창으로 돌아갑니다. 게임 창이 숨겨지거나
    최소화되면 함께 숨습니다.
  - 모니터 해상도가 바뀌면(`WM_DISPLAYCHANGE`) 같은 규칙으로 다시 놓습니다.
- `MonitorSize()`는 창이 있는 모니터의 픽셀 크기, `Position()`은 client 영역(게임 화면)의 왼쪽 위가 그 모니터의 왼쪽 위에서
  떨어진 픽셀(`WindowPosition`)입니다.
- `SetPosition(position, error)`는 위치를 기억하고 창을 그곳으로 옮깁니다. 시작 값은 `EngineConfig::windowPosition`입니다.
  - 기억한 위치는 이후의 크기·모드 변경에도 적용됩니다. 빈 값(`std::nullopt`)을 주면 위치를 잊고 모드의 기본 배치
    (`Borderless`는 가운데, `Windowed`는 위의 규칙)로 돌아갑니다.
  - 들어가지 않는 위치는 가장 가까운 들어가는 위치로 고칩니다. `Borderless`는 게임 화면 전체가 모니터 안에,
    `Windowed`는 테두리를 포함한 창이 작업 영역 안에 있게 합니다.
  - client 크기가 모니터를 덮으면 위치와 상관없이 모니터 왼쪽 위에 놓습니다.
- `TakeUserMove(position)`은 사용자가 테두리 있는 창을 끌어 옮기기를 마치면 한 번 true를 주고 옮겨진 위치를 알려 줍니다.
  그 위치는 `SetPosition`처럼 기억됩니다. 저장은 Client의 일입니다.

Win32Window가 HWND와 메시지 처리를 소유합니다. WM_SIZE가 만든 변경 요청을
다음 Run 루프에서 renderer.Resize → Client.OnResize 순으로 전달하므로
옵션 콜백 안에서 Scene을 재진입하거나 GPU 자원을 교체하지 않습니다.
Client의 게임별 옵션·파일 저장·복구 정책은 엔진에 포함하지 않습니다.

`PhantomCore.Collision.Tests.exe --window-control`은 실제 숨김 D3D12/NoSound 실행으로
960×540 변경, 창 모드별 테두리와 위치(테두리 없는 가운데 배치, 모니터 크기에서 화면 채움), 위치 지정(모니터 밖 값 되돌림, 위치 잊기, 끌어 옮긴 위치 알림), native 지원 크기의 중복 제거, 잘못된 크기 거부,
지연된 OnResize 및 렌더러 크기·종료 수명을 검사합니다.

## Unicode 텍스트 입력

`CreateTextInput()`은 호출자가 `unique_ptr<ITextInput>`으로 소유하는 단일 행 Unicode
편집기를 만듭니다. 기본 구현은 미지원 context에서 nullptr을 반환하며 Win32 창은
네이티브 EDIT를 제공합니다. 부모 창보다 먼저, 창 스레드에서 해제해야 합니다.
`SetBounds`는 client 영역의 좌측 상단 원점 픽셀 좌표이며 `SetFont` 높이도 픽셀입니다.
`SetColors`는 0xRRGGBB입니다. View는 자체 배치/가시성에 맞춰 위치와 표시를 갱신합니다.

OS가 IME 조합·후보창, 커서·선택, 삭제와 클립보드 편집을 처리합니다.
`Focus`/`Blur`로 포커스를 제어하고 `TakeText`로 변경된 확정 문자열만 가져옵니다.
조합 중 Enter/Escape는 편집기 명령으로 보고하지 않으며, 조합 밖의 Enter/Escape는
`TakeSubmit`/`TakeCancel`에서 한 번 소비합니다. 편집 중 게임별 단축키 차단은 Client 책임입니다.
빈 편집기의 안내 문구는 포커스가 없을 때 표시하며, common-controls v6 manifest가 없는
소비 실행 파일에서도 그립니다. 앱 전체의 native 테마를 변경하지 않습니다.
숨기면 포커스와 대기 명령을 해제합니다. Raw Input 이벤트·판정 시각 경로는 유지합니다.

`--window-control`은 Unicode/서로게이트 삽입, 조합 중 변경 보류와 명령 분리,
명령 1회 소비, 안내 문구의 실제 픽셀 출력, 숨김 및 native handle 해제도 자동 검사합니다. 실제 IME 후보 선택과
클립보드 조작을 사람이 수행한 검증은 이 테스트에 포함되지 않습니다.
