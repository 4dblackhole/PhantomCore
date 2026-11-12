# 텍스트 렌더링

`TextRenderSystem`은 글꼴, 색상, 크기, 정렬을 바꿀 수 있는
2D 텍스트를 D3D12 렌더 타깃에 그린다. DirectWrite는 글꼴 선택, 문자열
레이아웃, shaping과 글리프 분석을 담당하며, 실제 화면 합성은 엔진의 D3D12
글리프 아틀라스 파이프라인이 담당한다. Direct2D 및 D3D11-on-12는 사용하지
않는다.

공개 TextRenderSystem은 글꼴·레이아웃 생성, Submit과 통계를 제공한다. 장치·포맷을
받는 초기화와 프레임 준비·Flush·Canvas 배치 기록은 엔진 내부 TextRenderingBackend에
있다. Text의 프레임 버퍼와 atlas 회수는 내부 공통 FrameCount를 사용한다.

비공개 `TextPipeline`은 루트 서명과 PSO를 소유하고 초기화·바인딩·해제를 담당한다.
TextRenderSystem의 Impl은 이 객체를 조립하며 글리프 cache나 프레임 업로드와
파이프라인 소유를 섞지 않는다. 리소스 해제는 기존 GPU 대기 뒤의 같은 단계에서 수행한다.

비공개 `LayoutCache`는 DirectWrite factory, 글꼴 로드, TextFormat과 준비된 배치의
2,048항목 LRU를 소유한다. Impl은 입력 검증과 제출을 조립하고 통계를 전달한다.
아틀라스는 이 factory를 빌려 사용하며 cache 정리와 factory 해제의 기존 순서를 유지한다.

비공개 `GlyphAtlas`는 글리프 cache·아틀라스 texture/descriptor·업로드와
프레임 arena를 소유한다. Impl은 준비된 배치를 CPU에서 제출하고 GPU 기록을 위임한다.
아틀라스는 구체 TextPipeline과 통계만 전달받으며 Impl 전체를 역참조하지 않는다.
frame arena·cache·페이지·pipeline·descriptor·factory의 기존 정리 순서를 유지한다.

## 수명과 호출 시점

1. `D3D12Renderer::Initialize`가 `TextRenderSystem`을 초기화한다.
2. Client의 `Initialize` 또는 장면의 `Initialize`에서 글꼴을 로드하고
   `FontHandle`을 보관한다.
3. 렌더 프레임마다 `D3D12Renderer::BeginFrame`이 텍스트 제출 목록을
   초기화한다.
4. Client/Scene의 `Render`에서 `RenderContext::textRendering`에 문자열을
   제출한다.
5. 루트 Client는 필요하다면 `SceneGameClient::OnClientRendered`에서 활성 Scene
   뒤에 전역 디버그 텍스트를 제출한다.
6. `D3D12Renderer::EndFrame`은 월드 메시, 화면 Canvas, 직접 제출한 텍스트
   순서로 기록한다. Canvas 글자는 사각형·이미지 사이의 트리 순서에 따라 기록한다.
   직접 제출한 FPS/UPS 등의 텍스트는 최종 pass에서 기록한다.

화면 Canvas의 여러 텍스트 배치와 직접 제출 목록은 같은 glyph cache와 프레임
업로드 아레나를 공유한다. Canvas 배치를 기록해도 직접 제출 목록은 최종 pass까지
보관한다. 새 글리프는 필요한 배치 앞에서 한 번 업로드하고 같은 프레임의 다음
배치는 업로드·instance 범위를 이어서 할당한다.

`Submit`은 command list를 즉시 실행하지 않는다. 화면 clip과 겹치는 글리프를
CPU 제출 목록에 저장하고 내부 `TextRenderingBackend::Flush`가 `DrawInstanced`를 기록한다.
제출 순서를 유지하면서 연속된 같은 아틀라스 페이지만 묶는다. 페이지별 정렬로
반투명 텍스트의 앞뒤 합성 순서를 바꾸지 않는다.

## 글꼴 로드

```cpp
PhantomCore::FontHandle fileFont =
    services.textRendering.LoadFontFile(
        L"assets/fonts/MyFont-Regular.ttf");

PhantomCore::FontHandle systemFont =
    services.textRendering.LoadSystemFont(L"Segoe UI");
```

파일 글꼴은 DirectWrite custom font set으로 읽으므로 Windows에 설치하지
않아도 된다. 상대 경로의 기준은 실행 파일의 디렉터리다. 장면을 종료할 때
`FontHandle`을 해제하면 되며 DirectWrite COM 타입은 공개 API에 노출되지
않는다.

Visual2D는 직접 `FontHandle`을 보관하지 않고 backend-neutral `TextContent`에
문자열·글꼴 descriptor·크기·정렬을 담아 공유합니다.

```cpp
auto& label = PhantomCore::CreateLabel(parent, bounds, L"Text", "Label");
label.GetComponent<PhantomCore::TextVisualComponent>()->SetFont(
    {PhantomCore::TextFontSource::System, L"Segoe UI"});

combo.GetComponent<PhantomCore::ComboBoxBehaviorComponent>()->SetFont(
    {PhantomCore::TextFontSource::File,
     L"assets/fonts/MyFont-Regular.ttf"});
```

`Default`는 Visual2D renderer의 기본 글꼴을 사용합니다. `System`의 value는
시스템 family 이름, `File`의 value는 실행 파일 기준 경로입니다. D3D12
Visual2D renderer는 화면용·texture용 `TextRenderSystem`마다 descriptor를
`FontHandle`로 해석합니다. 살아 있는 `TextContent`별로 글꼴과 배치 결과를 유지해
매 프레임 경로 정규화·문자열 복사·DirectWrite Draw를 반복하지 않습니다.

## 문자열 제출

```cpp
PhantomCore::TextDrawCommand command;
command.positionPixels = {24.0F, 24.0F};
command.layoutSizePixels = {640.0F, 96.0F};
command.horizontalAlignment =
    PhantomCore::TextHorizontalAlignment::Center;
command.verticalAlignment =
    PhantomCore::TextVerticalAlignment::Center;
command.style.font = fileFont;
command.style.fontSizePixels = 32.0F;
command.style.color = {0.9F, 0.2F, 0.4F, 1.0F};

renderContext.textRendering->Submit(L"Hello, rhythm!", command);
```

`positionPixels`는 클라이언트 영역 좌상단 기준 위치다.
`layoutSizePixels`는 DirectWrite가 정렬과 줄바꿈에 사용할 사각형 크기다.
색상은 선형 RGBA 값이며 알파 블렌딩을 지원한다. `FontHandle`이 비어 있거나
크기/레이아웃이 유효하지 않으면 제출은 오류로 처리된다.

`TextStyle::fontWeight`와 Visual2D `TextVisualComponent::SetFontWeight`는
OpenType 굵기 1~999를 받습니다(기본 400, bold 700). 문자열의 내용·크기와 함께
TextFormat/배치 캐시 키에 포함되며 굵기를 바꾸면 payload와 측정을 갱신합니다.
파일/시스템 글꼴에서 제공하는 face·가변 굵기를 DirectWrite가 선택합니다.
범위 밖 값은 이전 스타일을 변경하지 않고 오류로 거부합니다.
[DirectWrite의 유효 굵기 범위](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextlayout-setfontweight)를 따릅니다.

## 배치 유지와 Visual2D 계약

변경되지 않는 문구는 `CreateLayout`의 `TextLayoutHandle`을 보관하고 매 프레임
`Submit(handle, command)`로 제출할 수 있습니다. `SizePixels()`는 배치의 실제
문자열 폭과 높이를 반환합니다. DirectWrite 객체는 공개하지 않습니다.

```cpp
// 글꼴·문자열·크기·배치 영역·정렬이 바뀔 때 준비한다.
auto layout = services.textRendering.CreateLayout(L"Hello, rhythm!", command);
// Render에서는 위치·transform·색상·depth·clip만 바꿔 제출한다.
renderContext.textRendering->Submit(layout, command);
```

준비된 handle 제출은 command의 글꼴·크기·영역·정렬을 다시 적용하지 않습니다.
이 값이 달라지면 새 handle을 준비해야 합니다. 문자열 제출과 `CreateLayout`은
문자열·글꼴·크기·영역·정렬을 키로 하는 2,048항목 LRU를 공유합니다.
스타일만 공유하는 TextFormat에도 LRU를 사용합니다. 살아 있는 라벨의 handle은
LRU 축출 이후에도 유효하므로 2,049개 이상의 고정 라벨이 배치를 매번 만들지 않습니다.
첫 배치 생성과 처음 필요한 글리프 rasterization 비용은 남습니다.

`DrawPacket::text`는 `TextContentHandle`입니다. 직접 packet을 만드는 소비자도
문자열·글꼴·크기·정렬을 이 payload에 담고 게시 후 변경하지 않아야 합니다.
`TextVisualComponent`와 ComboBox는 실제 내용/스타일 변경 때만 payload를 교체합니다.
색상·위치·clip·`SetTextOffset`·`SetRepeatOffset` 변경은 배치를 재생성하지 않습니다.
repeat는 같은 배치를 두 번 제출할 뿐이며 애니메이션은 소비 프로젝트 책임입니다.

renderer는 payload를 약한 참조로 추적하고 화면/texture별 배치를 유지합니다.
원본이 사라지면 해당 유지 상태를 회수하며 resize 때 사용하지 않는 크기 변형도 교체합니다.
`TextVisualComponent::MeasuredSize()`는 마지막 렌더에서 계산한 논리 크기이고,
첫 렌더 전에는 값이 없습니다. 여러 영역에 같은 payload를 그리면 마지막 pass의 값입니다.
화면 Canvas의 측정과 제출 통계는 EndFrame의 Canvas 기록에서 갱신됩니다.
이 측정 캐시의 쓰기와 UI 읽기는 엔진 Update/Render 스레드에서 수행합니다.

## 글리프 캐시와 GPU 처리

- 배치에는 font-face 정체성·글리프·위치·bounds를 유지합니다. 64개 단위 bounds와
  개별 bounds로 clip 밖의 글리프를 캐시 조회·rasterization·instance 생성 전에 거릅니다.
- 실제 face 정체성은 run마다 한 번 해석합니다. fallback face를 구분하고
  glyph index·정확한 pixel size·measuring mode를 키로 회색조 이미지를 공유합니다.
- DirectWrite `GRAYSCALE` + `DWRITE_TEXTURE_ALIASED_1x1`의 한 채널 alpha를
  1024×1024 R8 페이지에 저장합니다. ClearType 채널의 최댓값으로 변환하지 않습니다.
- 기본 픽셀 예산은 16페이지, 16MiB입니다. CPU 캐시, descriptor, 글꼴과 업로드
  버퍼는 별도이므로 전체 텍스트 메모리의 상한은 아닙니다.
- 현재 제출 또는 GPU 사용 중인 페이지는 회수하지 않습니다. 예산을 넘겨야
  현재 글자를 표시할 수 있으면 일시 확대합니다. 안전해진 미사용 페이지는 재사용하고
  뒤쪽의 초과 페이지는 이후 `BeginFrame`에서 해제합니다.
- 배치는 영구 UV를 보관하지 않습니다. 페이지 회수 때 그 페이지의 글리프 캐시를
  삭제하고 다음 제출에서 이미지를 다시 준비하므로 오래 보관한 handle도 안전합니다.
- 새 글리프는 프레임별 persistently mapped upload arena에 함께 배치합니다.
  row pitch는 256바이트, footprint 시작은 512바이트 정렬이며 arena를 fence 완료
  전에 덮어쓰지 않습니다. 같은 renderIndex의 여러 off-screen pass는 이어서 할당합니다.
- instance 버퍼도 프레임별로 재사용합니다. 사용한 descriptor heap은 해당 프레임의
  GPU 완료까지 유지하고 확장한 heap의 SRV는 원본 texture에서 새로 만듭니다.
  글리프 추가 때문에 전체 GPU fence를 별도로 기다리지 않습니다.

`Statistics()`는 해당 renderIndex의 배치 생성·글리프 rasterization·제출 수,
업로드 arena 생성·업로드 바이트·현재 페이지 수·축출 수를 제공합니다.
GPU 시간이나 총 프로세스 메모리를 나타내지는 않습니다.

현재 구현은 수평 단색 글리프를 대상으로 합니다. 컬러 이모지, 세로쓰기,
underline, strikethrough, inline object, SDF 확대 렌더링은 지원하지 않습니다.

## SDK 회귀 검증

Debug/Release x64 빌드 후 `PhantomCore.Collision.Tests.exe`를 실행합니다.
기본 경로는 Visual2D payload 재사용·무효화·측정·repeat 계약을 검사합니다.
`--text-rendering`은 실제 D3D12에서 LRU보다 많은 유지 배치, 긴 clip 문구,
512개 한글의 단일 upload arena, 아틀라스 확대·회수·재생성을 검사합니다.
서로 다른 굵기의 TextFormat·레이아웃·글리프가 혼동되지 않는 것도 검사합니다.
`--text-pixels`는 다중 페이지의 겹친 글자 합성 순서를 GPU readback으로 검사합니다.
`--visual2d-pixels`는 공개 ReadImage와 그래픽 진단 API로 화면 알파, 글자 위 패널·이미지,
Canvas Z와 동일 Z의 제출 순서, snapshot 및 프레임 자원 재사용을 검사합니다.
월드 평면 Canvas를 포함한 모든 합성·깊이 사례가 종료 코드에 반영되는 회귀 가드입니다.
각 경로는 별도 프로세스로 실행합니다.

## 성능 오버레이

엔진은 `UpdateContext::performance`로 QPC 기반 FPS/UPS와 프레임당 draw 수 표본만 전달한다.
글꼴 선택, 단축키, 표시 여부와 화면 배치는 게임마다 달라지는 Client 책임이다.
따라서 엔진은 성능 오버레이 텍스트를 자동으로 제출하지 않는다.

`SceneGameClient`를 쓰는 Client는 `OnClientInitialized`, `OnClientUpdated`,
`OnClientRendered` 훅으로 글꼴을 생성하고, 최신 성능 표본을 문자열로 바꾸고,
활성 Scene 뒤에 텍스트를 제출할 수 있다. 자세한 계약은
[`PerformanceStatistics.md`](PerformanceStatistics.md)를 참고한다.
