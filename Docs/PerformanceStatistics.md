# 성능 통계

`Run`은 끝난 렌더 프레임, `IGameClient::Update` 호출, 렌더된 프레임마다 기록한 GPU draw call을 세고,
메인 루프 한 바퀴의 길이와 그래픽 메모리를 잽니다.
1초마다 새 표본을 `UpdateContext::performance`로 게시합니다.

| 필드 | 뜻 |
| --- | --- |
| `framesPerSecond` | 구간 동안 렌더한 프레임 수(초당, 정수로 반올림) |
| `updatesPerSecond` | 구간 동안의 `Update` 호출 수(초당, 반올림) |
| `drawCallsPerFrame` | 렌더한 프레임마다 기록한 draw call의 평균(반올림) |
| `peakDrawCallsPerFrame` | 구간 안 한 프레임의 최대 draw call 수 |
| `longestLoopSeconds` | 구간 안에서 가장 길었던 메인 루프 한 바퀴(메시지 펌프, `Update`, 오디오 갱신, 마감이 된 `Render`) |
| `graphicsMemoryUsageBytes` | 구간 끝에 이 프로세스가 그래픽 어댑터에 들고 있는 메모리 |
| `graphicsMemoryBudgetBytes` | Windows가 지금 이 프로세스에 허용하는 양. 어댑터에 물을 수 없으면 두 값 모두 0 |
| `measurementIndex` | 표본을 게시할 때마다 1씩 늘어남 |
| `hasMeasurement` | 첫 구간이 끝날 때까지 false |

draw call 하나는 mesh·text·Visual2D 백엔드가 기록한 `DrawInstanced`/`DrawIndexedInstanced` 하나이며, 프레임이 열린 동안 기록한
render texture 패스도 포함합니다. 파이프라인과 텍스처 페이지가 같은 연속 primitive는 draw 하나로 묶이므로, 이 수는 화면의 그리기 순서가
사각형·그림·글자를 얼마나 번갈아 섞는지를 보여 줍니다. GPU 시간 측정이 아니라 기록 횟수입니다.

`longestLoopSeconds`는 초당 횟수로는 보이지 않는 한 번의 끊김을 보여 줍니다. 추가로 시각을 재지 않고 루프가 이미 재는 한 바퀴 간격의 최댓값을 씁니다.
그래픽 메모리는 1초에 한 번 어댑터에 묻습니다. 사용량이 예산을 넘으면 Windows가 자원을 시스템 메모리로 옮기기 시작해 그리기가 느려집니다.
Client는 이 표본을 보관해 두었다가 문제가 났을 때 직전 상황으로 남길 수 있습니다([엔진 진단 기록](EngineDiagnostics.md)).

Client는 `measurementIndex`가 바뀔 때만 문자열을 고치면 됩니다.

엔진은 키를 묶거나 글꼴을 읽거나 덮개를 그리지 않습니다. 그것은 Client가 정하는 표시 문제입니다. 그래서 디버그 UI·원격 측정이 다르거나
보이는 카운터가 없는 게임도 엔진을 그대로 쓸 수 있습니다.

`SceneGameClient`는 Scene 수명 메서드를 final로 유지하고, 루트 Client 동작을 위한 안전한 template hook 네 개를 제공합니다.

- `OnClientInitialized`: Client 전체 자원 만들기
- `OnClientUpdated`: 입력과 성능 데이터
- `OnClientRendered`: 활성 Scene이 그리기를 제출한 뒤
- `OnClientShuttingDown`: 엔진 서비스가 살아 있는 동안 해제

`PhantomCore.Collision.Tests --draw-statistics`는 묶인 사각형 Canvas 하나로 숨긴 엔진을 돌려 첫 표본이 프레임당 draw 하나를 보고하는지 확인합니다.
