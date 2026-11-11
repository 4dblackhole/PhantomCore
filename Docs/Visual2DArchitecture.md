# Visual2D 스프라이트·위젯 통합 구조

## 책임과 프로젝트 경계

`Visual2D`는 일반 Sprite와 옵션 위젯을 같은 노드, Transform, 입력 및 렌더링
경로로 처리한다. 독립 `PhantomCore.UI` 프로젝트와 `UiElement` 상속 계층은 존재하지 않는다.

```text
PhantomCore.Engine/Core/Visual2D
  Visual2DNode + Components + Canvas + Input + Surface + Widget factories

PhantomCore.Graphics.D3D12/Visual2D
  D3D12Visual2DRenderer
```

`D3D12Visual2DRenderer`는 Scene이 생성하는 객체가 아니다. 엔진의
`D3D12Renderer`가 실행 중 하나만 생성하고 Mesh/Text 렌더러 다음에
초기화한다. 구체 D3D12 클래스는 엔진 내부 헤더에만 있으며 Client에는
생명주기 함수가 없는 `Visual2DRenderSystem` 계약만 공개된다. Client는
`EngineServices::visual2DRendering`으로 이미지와
렌더 타깃을 만들고, Render 중에는
`SceneGameClient`를 쓰는 화면은 `ScreenVisuals()`에 Canvas와 이미지를 등록한다.
관리자는 Update·resize·화면 제출과 자원 레지스트리를 소유하며 Scene은 Canvas ID,
입력 라우터와 빌린 node 포인터만 가진다. 직접 `IGameClient`를 구현하거나
월드·곡면 pass를 구성할 때에만 RenderContext의 저수준 제출 API를 사용한다.

`Visual2DNode`는 상속용 인터페이스가 아니라 구체적인 컴포넌트 컨테이너다.

```text
일반 Sprite = Visual2DNode + SpriteVisualComponent
클릭 Sprite = 위 구성 + Collider + PointerReceiverComponent
Button       = 위 구성 + TextVisualComponent + ButtonBehaviorComponent
ComboBox     = Sprite + PointerReceiver + ComboBoxBehaviorComponent
```

`CreateSprite`, `CreateButton`, `CreateComboBox` 등의 팩토리는 자주 쓰는 조합을
안전하게 구성할 뿐 별도 Widget 상속 객체를 생성하지 않는다. 컴포넌트는
`AddComponent<T>`, `GetComponent<T>`, `RemoveComponent<T>`로 실행 중 변경할 수 있다.

## Update 중 트리 변경과 콜백 오류

Update는 노드의 컴포넌트 목록을 먼저 방문하고 자식 목록을 방문한다.
각 목록의 순회가 시작되기 전에 추가한 항목은 이번 Update에 포함하며,
해당 목록의 순회가 이미 시작된 뒤 추가한 항목은 다음 Update부터 방문한다.
따라서 컴포넌트 Update에서 만든 자식은 자식 순회가 아직 시작되지 않았다면
이번 Update에 포함된다.

Update 중 삭제한 노드/컴포넌트는 FindNode/GetComponent와 후속 순회에서 즉시 제외된다.
현재 콜백과 같은 Canvas의 Update가 끝날 때까지 실제 파괴를 미뤄 자기 자신·형제·
조상을 삭제해도 실행 중인 객체의 수명이 유지된다. 삭제한 조상의 남은 컴포넌트와
자손은 방문하지 않는다. 예외로 Update가 끝날 때도 지연 파괴를 정리한다.
Update 밖에서 삭제하면 즉시 파괴한다.

같은 Canvas의 Update를 콜백에서 다시 호출하면 logic_error를 던진다.
Canvas 자체의 수명은 진행 중인 Update를 포함해야 하며 Scene 전환은 기존 지연 전환을 사용한다.

CustomCollider2DComponent의 사용자 HitTestHandler는 예외를 던질 수 있다.
기반 가상 HitTest, Node/Canvas의 HitTest와 입력 Router 경로는 이를 호출자에게 전달한다.
실패한 hit test는 성공 결과로 캐시하지 않으므로 호출자가 예외를 처리하면 같은
포인터 상태에서도 재시도할 수 있다. 단순 사각형·원 충돌처럼 예외를 던지지 않는
구현은 noexcept를 유지한다. Run의 치명적 예외 처리는 종료 코드와 정리 계약을 따른다.

회귀: [Visual2DUpdateTests](../Tests/Collision/Visual2DUpdateTests.cpp)와
[공유 CPU/SDK 검사](../Tests/Collision/CollisionTests.cpp).
FMOD 없는 실행과 실제 장치 검사는 [Testing](Testing.md)을 따른다.

## Transform과 소유권

Canvas의 `Visual2DNode` 트리가 모든 노드의 유일한 소유자다. 각 노드는
`TransformNode`를 값으로 소유하고 Transform의 부모·자식 연결은 비소유 관계다.
따라서 노드와 Transform이 같은 자식을 각각 `unique_ptr`로 소유하지 않는다.

노드 위치는 부모 또는 화면 앵커에서 Pivot까지의 거리다. 실제 Transform의
좌측 하단 위치는 다음과 같이 계산된다.

```text
Transform lower-left = node position - normalized pivot × node size
```

XYZ 회전, 크기, 위치와 부모 Transform이 Sprite, 글자, 충돌 영역에 동일하게
적용된다. 화면 포인터는 Canvas 광선으로 만든 뒤 각 노드의 월드 행렬 역변환을
거쳐 로컬 Collider에 전달하므로 회전된 사각형의 표시와 hit box가 일치한다.

## 중앙 원점 Y-up 좌표와 9개 앵커

기본 Canvas 기준은 1280×720이고 `CanvasScaleMode::FixedHeight`를 사용한다.

```text
pixelScale   = viewportHeight / 720
logicalHeight = 720
logicalWidth  = viewportWidth / pixelScale
```

100 논리 단위는 720p에서 100px, 1080p에서 150px다. 가로·세로에 같은 배율을
적용하므로 화면비가 바뀌어도 이미지와 글자가 찌그러지지 않는다.

Canvas 논리 원점은 정중앙이고 `+X`는 오른쪽, `+Y`는 위쪽이다. 따라서 논리
범위는 `[-width/2, +width/2] × [-height/2, +height/2]`다. Win32 마우스 위치와
`SubmitScreen`의 `screenOrigin`만 운영체제 규약에 따라 좌상단 픽셀 원점을
사용하며, `MapScreenPointer`가 이를 중앙 원점 Y-up 좌표로 변환한다.

Canvas는 렌더링과 hit-test를 하지 않는 다음 9개 앵커 노드를 항상 소유한다.

```text
TopLeft       TopCenter       TopRight
MiddleLeft    Center          MiddleRight
BottomLeft    BottomCenter    BottomRight
```

`CreateNode(Anchor::BottomRight)`로 만든 노드는 기본 Pivot이 `(1,0)`이다.
`SetBounds({-20,20,w,h})`로 두면 화면비가 달라져도 우측·하단에서 20 논리 단위
안쪽에 고정된다. `TopLeft`의 기본 Pivot은 `(0,1)`이고, 화면 안쪽 배치는
양의 X와 음의 Y 오프셋을 사용한다.

RenderTexture나 월드 표면처럼 크기가 고정되어야 하는 Canvas는
`CanvasScaleMode::Fixed`를 사용한다.

## 입력과 Z-Order

`Visual2DInputRouter`는 Canvas hit-test 결과를 enter, leave, press, move, release,
click, wheel 이벤트로 바꾼다. 누른 노드는 release까지 ID로 capture되므로 Slider와
ComboBox 드래그가 영역 밖에서도 이어진다. 삭제 전에 `Reset`하면 hover/capture가
안전하게 해제된다.

Router는 무제한 Update 루프에서 동일한 포인터 스냅샷이 반복되면 이전 hover와
capture를 재사용하고 hit-test와 move 이벤트를 생략한다. 포인터가 정지한 동안
상호작용 가능한 노드를 이동하거나 크기를 바꾼 경우에는 변경 후
`InvalidateHitTest()`를 호출한다. 다음 `Process`가 새 트리 위치를 한 번 검사한다.

각 부모는 하나의 stacking context다. 형제는 `ZIndex` 오름차순으로 그리고,
같은 값은 삽입 순서를 유지한다. Hit-test는 그 정확한 역순이다. 렌더러는 투명
순서를 깨지 않도록 트리에서 평탄화된 순서를 유지한다. 정렬된 형제 순서는
자식 추가·삭제 또는 `SetZIndex` 전까지 캐시되므로 매 Update/Render마다 다시
할당하고 정렬하지 않는다.

## Sprite 이미지와 인스턴싱

`Visual2DRenderSystem::LoadImage`는 서로 크기가 다른 PNG를 각각 독립된
`Texture2D`로 올린다. 같은 Descriptor Table 안의 texture index를 인스턴스 데이터로
전달하므로 크기를 강제로 통일하는 `Texture2DArray`가 필요하지 않다. 화면 Sprite는
Canvas 표시 순서에 따라 `DrawInstanced`로 기록한다. 평면 Sprite는
mesh/material 조합별 `DrawIndexedInstanced` 배치에 들어간다.

`GetImageSize(ImageHandle)`은 WIC가 읽은 원본 픽셀 크기를 돌려준다. Sprite Bounds는
여전히 Client가 정하지만, 이 크기로 목표 폭 또는 높이에 맞는 다른 축을 계산하면
스킨 교체 뒤에도 원본 비율을 유지할 수 있다. 잘못되었거나 더 이상 유효하지 않은
handle은 `{0, 0}`을 돌려준다.
PNG의 위쪽은 Sprite 로컬 `+Y`에 대응하므로, Y-up Canvas에서는 자산별 뒤집기나
추가 회전 없이 부모 Transform만 상속하면 된다.

`SpriteVisualComponent::SetUvTransform`으로 cover/crop UV를 인스턴스별로 지정할
수 있다. 화면 Visual2D는 Depth Write 대신 트리 순서를 기준으로 하며, 월드의
불투명 Mesh는 기존 Mesh 렌더러의 Depth 상태를 사용한다.

`SpriteVisualComponent::SetCornerRadius`는 단색과 이미지 Sprite 모두에 로컬
Canvas 단위의 둥근 모서리를 적용한다. 반지름은 draw packet의 인스턴스 데이터로
전달되므로 별도 mesh나 material을 만들지 않고 기존 batching을 유지한다.

## 자식 clip rect

`Visual2DNode::SetClipRect`는 node-local 사각형으로 해당 node의 전체 subtree를
자른다. 부모 크기로 자르려면 `{0, 0, width, height}`를 지정하고, 해제할 때는
`ClearClipRect`를 호출한다. 중첩 clip은 Core에서 Canvas 좌표 교집합으로 합쳐진 뒤
`DrawPacket::clipBounds`로 backend에 전달된다. Hit-test도 같은 clip을 사용하므로
스크롤 viewport 밖에 있는 버튼은 입력을 받지 않는다.

화면과 render texture D3D12 경로는 clip rect를 인스턴스별 픽셀 범위로 변환해
rectangle, image와 glyph shader에서 잘라낸다. clip은 axis-aligned screen Canvas
viewport 용도이며 회전된 node clip은 Canvas AABB로 정규화한다.

ComboBox는 `SetTextColor`, `SetSelectedTextColor`,
`SetPopupBackgroundColor`로 밝거나 어두운 화면 테마에 맞출 수 있다.

## 화면과 곡면

화면 입력은 다음 경로를 사용한다. 첫 좌표는 좌상단 원점의 Win32 픽셀이고,
마지막 좌표는 중앙 원점 Y-up Canvas 좌표다.

```text
Mouse pixel → MapScreenPointer → Canvas logical point → InputRouter
```

월드 또는 곡면 입력은 다음 경로다.

```text
Mouse pixel → CreateWorldPointerRay
→ PlaneVisual2DSurface 또는 MeshUvVisual2DSurface
→ UV → Canvas logical point → InputRouter
```

`WorldSpaceVisual2DCanvas`는 Canvas를 상속하지 않고 `Visual2DCanvas`와
`IVisual2DSurface`를 합성한다. 표시 Mesh와 입력 Surface에는 같은 Shape, UV 및
world transform을 사용해야 한다.

곡면은 `RenderToTexture`로 Canvas의 사각형, 서로 다른 크기의 Sprite 이미지와
글자를 먼저 그린 뒤 그 Texture를 Mesh에 샘플링한다. 이미지와 Text glyph도
노드 Transform을 받으므로 부모의 XYZ 회전과 크기 변경을 함께 따른다.

## 간단한 사용 예

```cpp
PhantomCore::Visual2DCanvas canvas;
canvas.SetViewportSize({1920.0F, 1080.0F});

auto& sprite = canvas.CreateNode(
    PhantomCore::Anchor::BottomCenter,
    "Character");
sprite.SetBounds({0.0F, 48.0F, 128.0F, 128.0F});
sprite.AddComponent<PhantomCore::SpriteVisualComponent>().SetImage(image);
sprite.AddComponent<PhantomCore::RectangleCollider2DComponent>();
sprite.AddComponent<PhantomCore::PointerReceiverComponent>(handler);

PhantomCore::Visual2DInputRouter input;
input.Process(canvas, pointerSnapshot);

renderContext.visual2DRendering->SubmitScreen(canvas, renderContext);
```

## Transparent PNG composition

화면 Canvas는 `canvasZOrder` 오름차순, 같은 Z에서는 제출 순서대로 합성한다.
각 Canvas 안에서는 트리의 paint 순서를 유지하며 사각형·이미지·글자를 번갈아 기록한다.
연속된 같은 종류만 배치하고 이미지 descriptor 페이지가 바뀌면 배치를 닫는다.
사각형과 이미지의 알파는 배경과 이미 그린 글자에 함께 적용된다.

화면 요소는 기존 depth band로 월드 깊이를 검사하고 깊이 쓰기는 끈다.
Z 31을 넘는 Canvas도 전체 Z 값으로 정렬하며 월드 검사에 쓰는 band만 31로 제한한다.
`TextRenderSystem`에 직접 제출한 텍스트는 화면 Canvas 뒤 최종 pass에서 기록한다.
`SubmitPlane`은 Mesh 렌더러의 재질·깊이 경로를 사용한다.
