# Visual2D Renderer 소유권과 수명

## 소유 관계

Visual2D 렌더러는 `D3D12Renderer`가 하나만 소유한다. Scene이 렌더러를
생성하거나 `Initialize`, `Shutdown`을 호출하지 않는다. Client에는
`Visual2DRenderSystem`만 공개되고, 생명주기를 가진
`D3D12Visual2DRenderer`는 엔진 내부 헤더에 있으므로 소유권 제어를 위해
`friend class D3D12Renderer`를 사용하지 않는다.

```text
Run
└─ D3D12Renderer
   ├─ MeshRenderSystem
   ├─ TextRenderSystem
   └─ D3D12Visual2DRenderer (내부 구현)

SceneGameClient
└─ ScreenVisual2DManager
   ├─ screen Visual2DCanvas
   ├─ path별 ImageHandle 등록
   └─ Update / resize / SubmitScreen

Scene
├─ ScreenCanvasHandle과 빌린 node 포인터
├─ Canvas별 Visual2DInputRouter
└─ world/off-screen Canvas와 RenderTargetTextureHandle
```

초기화 순서는 D3D12 장치, Mesh, Text, Visual2D 순서이고 종료는 그 반대다.
따라서 Visual2D가 공유 Mesh, Material, Texture, Font 핸들을 정리하는 동안
하위 렌더 시스템과 D3D12 장치는 항상 유효하다.

Mesh/Text의 공개 클래스는 생성·제출 계약을 제공하고 GPU 상태는 비공개 구현이
소유한다. 내부 MeshRenderingBackend와 TextRenderingBackend가 장치 초기화와 기록을
연결한다. Renderer, Mesh, Text, Visual2D는 Renderer/FrameResources.h의 같은 프레임
개수를 사용하며 프레임 선택 전 GPU fence 대기 순서는 유지한다.

## SceneGameClient 화면 사용법

`SceneGameClient` 파생 클래스는 Scene factory에 관리자를 주입한다.

```cpp
scenes.RegisterScene<MyScene>(
    "Main", PhantomCore::SceneRetention::KeepAlive, std::ref(ScreenVisuals()));
```

Scene은 관리자가 소유할 Canvas와 이미지를 등록한다. `BeginScene`과 `EndScene`은
KeepAlive Canvas의 표시 상태만 전환하며, `Render`에서 제출하지 않는다.

```cpp
void MyScene::Initialize(const PhantomCore::EngineServices&)
{
    canvasHandle_ = visuals_.CreateOwnedCanvas();
    canvas_ = canvasHandle_.Get();
    const auto image = visuals_.RegisterImage("assets/images/player.png");
    PhantomCore::CreateSprite(
        canvas_->Root(), {-64.0F, -64.0F, 128.0F, 128.0F}, image);
}

void MyScene::BeginScene()
{
    canvasHandle_.SetVisible(true);
}

void MyScene::EndScene() noexcept
{
    canvasHandle_.SetVisible(false);
}

void MyScene::Render(const PhantomCore::RenderContext&)
{
}
```

`Shutdown`에서는 InputRouter를 먼저 Reset한 뒤 `canvasHandle_.Reset()`을 호출한다.
초기화 도중 예외가 발생하거나 명시적 Reset을 빠뜨려도 이동 전용 handle의 소멸자가
Canvas를 제거한다. handle은 관리자를 빌려 쓰므로 `ScreenVisual2DManager`보다 오래
살아서는 안 된다. 관리자는 Client 종료 시 남은 Canvas와 이미지 등록도 모두 정리한다.

## 저수준·월드 Canvas 사용법

직접 `IGameClient`를 구현하거나 월드·곡면 UI를 만들 때는 엔진 서비스를 통해
이미지와 Canvas 렌더 타깃을 만든다.

```cpp
void ExampleScene::Initialize(const PhantomCore::EngineServices& services)
{
    panelImage_ = services.visual2DRendering.LoadImageFile(imagePath);
    panelTarget_ =
        services.visual2DRendering.CreateCanvasRenderTarget(960, 630);
}
```

Render에서는 현재 프레임이 보유한 렌더러 포인터를 사용한다.

```cpp
void ExampleScene::Render(const PhantomCore::RenderContext& context)
{
    context.visual2DRendering->SubmitScreen(canvas_, context);
    context.visual2DRendering->RenderToTexture(
        curvedCanvas_,
        panelTarget_,
        context);
}
```

`RenderContext::visual2DRendering`은 엔진이 연 프레임에서만 사용하는
비소유 포인터다. Context의 크기·프레임 번호·렌더 서비스·ReadImage는 공개 계약이다.
명령 목록·viewport·scissor·descriptor와 이미지 읽기 queue의 연결은 내부
`RenderFrameContext`를 통해 현재 Renderer가 보유한다. Context의 유효 범위는
해당 BeginFrame/EndFrame 쌍이다. ReadImage의 완료된 CPU 결과는 독립적으로 소유한다.

## 여러 Canvas와 오프스크린 패스

하나의 렌더러로 한 프레임에 여러 Canvas를 화면이나 서로 다른 렌더
타깃에 그릴 수 있다. 프레임별 업로드 아레나는 각 Draw가 사용하는
인스턴스 범위를 선형 할당하므로 나중 패스가 앞선 패스의 데이터를
덮어쓰지 않는다. Text 렌더러도 같은 `renderIndex`의 여러 패스에 별도
범위를 할당한다.

SubmitScreen은 draw 목록과 배치를 프레임용 snapshot으로 보관한다.
EndFrame은 월드 Mesh와 평면, 정렬된 화면 Canvas, 직접 제출된 Text 순서로 기록한다.
ReadImage 복사는 이 기록이 모두 끝난 뒤 수행한다. 화면과 RenderToTexture는
트리 순서를 유지하면서 연속된 Rectangle, 같은 이미지 Descriptor 페이지, Text를
각각 배치하는 코드를 공유한다.
렌더 타깃 핸들은 해당 frame-resource fence가 완료될 때까지 렌더러가
보관하므로 동적 Scene이 프레임 직후 삭제되어도 GPU 참조가 유효하다.

Node-local clip은 Core가 중첩 교집합을 계산하고 renderer가 현재 화면 또는 렌더
타깃의 픽셀 좌표로 바꾼다. clip과 둥근 모서리는 인스턴스 데이터이므로 서로 다른
viewport와 radius를 사용하는 Sprite도 연속된 호환 배치에 함께 기록한다.

## 이미지 캐시

이미지는 Windows 경로 규칙에 맞게 정규화하고 대소문자를 구분하지 않은 경로를
키로 공유한다. 같은 경로는 놓기 전까지 같은 handle을 돌려준다. 한 Descriptor
Table에는 서로 크기가 다른 Texture2D를 최대 64개 넣고, 64개를 넘으면
새 페이지를 만든다. 같은 페이지에 이미지를 추가할 때는 기존 64칸
Descriptor 블록의 빈 슬롯만 채우므로 이미지마다 새 블록을 만들지 않는다.

기본 정책은 실행 중 로드한 고유 이미지를 엔진 종료까지 캐시한다. 이 정책은
Scene 전환 중 GPU 리소스 파괴 문제를 피하고 재진입 시 업로드를 생략한다.
텍스처 Descriptor 힙은 1,024칸(64칸 블록 16개)이고 픽셀 페이지·렌더 타깃·메시 재질도 같은 블록을 쓰므로,
서로 다른 파일을 끝없이 로드하는 소비자(예: 곡마다 다른 배경)는 다 쓴 이미지를 놓아야 한다.

- 놓기: `ReleaseImage(handle)`는 파일 이미지도 놓는다. 그 경로는 캐시에서 빠지고, 다음 로드는 새 handle이 된다.
  `ScreenVisual2DManager::ReleaseImage`는 자기 경로 등록도 지운다. 한 경로의 handle은 그 경로를 등록한 모든 곳이
  함께 쓰므로, 아무도 그리지 않을 때만 놓는다.
- 칸 재사용: 놓은 칸은 픽셀 이미지와 같이 프레임 링(놓은 렌더 인덱스 + `FrameCount` + 1)을 지난 뒤에만 다음
  파일 로드가 다시 쓴다(`TextureManager::ReplaceTexture`의 파일 경로판, 디코드를 먼저 하므로 깨진 파일은 칸을 바꾸지
  않음). 빈 칸이 있으면 새 블록을 잡지 않으므로, 로드와 놓기를 이어 해도 블록 수는 동시에 살아 있는 이미지 수를 따른다.
- 그래픽 메모리: 놓은 칸의 텍스처는 그 칸을 다음 파일이 쓸 때 풀린다. 정확히는 새 그림의 업로드와 함께 보관되어
  업로드 fence가 지난 뒤(그때까지 제출된 모든 프레임이 끝난 뒤) 해제된다. 프레임 수를 세는 칸 재사용 규칙이 첫 번째
  보호이고, 이것은 그 계산에 기대지 않는 두 번째 보호다.
- 놓은 handle을 아직 가리키는 packet은 아무것도 그리지 않는다. handle 값은 다시 쓰지 않는다.

검사: `PhantomCore.Collision.Tests.exe --file-images`([Testing](Testing.md)).

## 픽셀 이미지

`Visual2DRenderSystem::CreatePixelImage(width, height, rgba8)`는 CPU가 만든 RGBA8 픽셀(위 행부터, 정확히
`width * height * 4`바이트, 복사함)로 이미지를 만든다. 결과 handle은 파일 이미지와 똑같이 Sprite와 draw packet에 쓴다.
`ScreenVisual2DManager`에도 같은 이름의 전달 함수가 있다. 많은 사각형으로 그리던 그림(예: 스펙트로그램 타일)을
텍스처 한 장으로 바꿀 때 쓴다.

- 스레드: 파일 이미지와 같이 엔진 스레드(Initialize·Update·Render)에서 만들고 해제한다.
- 페이지: 픽셀 이미지는 파일 이미지와 섞이지 않는 자기 페이지(64칸 Descriptor 블록)를 쓰며, 블록은 필요할 때 하나씩 잡는다.
  한 번에 살아 있을 수 있는 수는 `MaxPixelImages`(128)이고, 넘으면 `std::length_error`를 던진다. 크기가 0이거나 바이트 수가
  다르면 업로드 전에 `std::invalid_argument`를 던진다.
- 수명: `ReleaseImage(handle)`로 놓는다. 이미 제출한 프레임이 그 칸을 샘플링할 수 있으므로, 칸은 프레임 링
  (`FrameCount`)을 지난 뒤(놓은 렌더 인덱스 + `FrameCount` + 1)에만 다른 픽셀 이미지에 다시 쓴다. 그전까지는 용량에
  포함된다. 놓은 handle을 아직 가리키는 packet은 아무것도 그리지 않는다. 알 수 없는 handle의 해제는 무시한다
  (파일 이미지의 해제는 [이미지 캐시](#이미지-캐시)).
- 갱신: 픽셀을 바꾸려면 새 이미지를 만들고 이전 것을 놓는다. 칸 교체(`TextureManager::ReplaceTexture`)는 렌더러가 프레임 링을
  지난 칸에만 한다.
- 텍스처가 없는 backend(테스트용 가짜)의 기본 구현은 빈 handle을 돌려주고 해제는 아무것도 하지 않는다.
  호출하는 쪽은 빈 handle이면 다른 방법으로 그린다.

검사: `PhantomCore.Collision.Tests.exe --pixel-images`([Testing](Testing.md)).

## 입력 소유권

렌더러를 하나로 통합해도 `Visual2DInputRouter`는 Canvas별로 유지한다.
Router에는 hover와 pointer capture 상태가 있으므로 서로 다른 Canvas가
하나의 Router를 공유해서는 안 된다. 여러 Canvas 사이의 우선순위는
Scene 수준 InputDispatcher 또는 LayerStack에서 결정한다.
