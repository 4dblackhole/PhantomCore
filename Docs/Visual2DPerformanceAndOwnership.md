# Visual2D 성능·소유권 지침

## 곡면 입력 가속

`MeshUvVisual2DSurface`는 생성할 때 Shape의 위치·UV·인덱스를 복사하고 로컬 공간
BVH(Bounding Volume Hierarchy)를 한 번 만든다. 각 노드는 삼각형 묶음의 AABB를
보관한다. 포인터 입력 시 world ray를 메시의 로컬 공간으로 옮긴 다음 다음 순서로
충돌을 찾는다.

1. BVH 노드 AABB와 ray가 만나지 않으면 그 하위 트리를 건너뛴다.
2. leaf에서만 실제 ray-triangle 충돌과 barycentric UV 보간을 수행한다.
3. 이미 찾은 가장 가까운 거리보다 먼 AABB는 추가로 제거한다.

노드는 preorder 배열과 `escapeIndex`로 저장하므로 `Raycast` 중 스택이나 임시
컨테이너를 할당하지 않는다. QuadTree는 평면의 2D 영역 분할, Octree는 넓은 3D
공간의 동적 객체 조회에 더 알맞다. 현재 대상은 임의 방향·곡률의 정적 삼각형
메시와 ray 질의이므로 메시 로컬 BVH가 더 직접적이다. Surface의 world transform이
바뀌어도 BVH를 다시 만들 필요가 없다.

## Update 순회 수명

Update의 각 목록은 시작 시 방문 표식을 정하고 벡터의 참조/반복자를 콜백 밖으로
보관하지 않는다. 삭제로 인덱스가 바뀌면 표식으로 재방문을 막는다. 삭제한 객체는
객체 내부의 소유 링크로 연결해 Canvas Update 종료 시 해제하므로 정상 순회마다
스냅샷 벡터를 할당하지 않고 noexcept 삭제 경로에서 퇴역 목록을 확장 할당하지 않는다.
삭제된 컴포넌트를 먼저 해제해 지연 파괴 중 Owner가 살아 있도록 한다.
추가 시점과 예외 계약은 [Visual2DArchitecture](Visual2DArchitecture.md#update-중-트리-변경과-콜백-오류)를 따른다.

## Canvas 크기와 화면 원점

`Visual2DCanvas`는 전체 화면일 필요가 없다.

- HUD 전체 배치는 기본 `FixedHeight` Canvas와 `SetViewportSize`를 사용한다.
- 독립 패널은 실제 콘텐츠에 필요한 크기의 `Fixed` Canvas를 사용한다.
- 패널의 화면 위치는 내부 루트 노드를 이동시키지 않고 `SubmitScreen`의
  `screenOrigin`으로 지정한다.
- 입력에도 동일한 `screenOrigin`을 `MapScreenPointer`에 전달한다.

Canvas 내부 좌표는 정중앙 원점과 Y-up을 사용한다. `screenOrigin`은 Canvas
좌표가 아니라 Win32 화면 배치 경계이므로 예외적으로 패널의 좌상단 픽셀을
뜻한다. `MapScreenPointer`가 좌상단 원점 픽셀을 중앙 원점 Canvas 좌표로 바꾼다.

`MapScreenPointer`는 물리 viewport 밖의 점만 거부한다. Canvas 영역 밖이지만
화면 안인 좌표는 반환하므로 Slider·ComboBox가 pointer capture를 가진 동안에도
드래그와 release 좌표가 끊기지 않는다. capture가 없을 때 Canvas 영역을 차단할지는
Scene의 Canvas dispatcher가 결정한다.

## 두 단계 Z-Order

화면 Visual2D의 순서는 다음 두 단계다.

1. `SubmitScreen(..., canvasZOrder)`의 Z 오름차순으로 Canvas를 합성한다.
   같은 Z에서는 제출 순서를 유지한다.
2. `Visual2DCanvas::BuildDrawList`의 부모·자식 트리와 `ZIndex` 순서로
   사각형·이미지·글자를 기록한다.

화면 요소는 깊이를 쓰지 않으며 월드 깊이 검사에는 기존 0.10 범위의 band를 사용한다.
이 검사에 쓰는 Canvas Z만 최대 31로 제한한다. 화면 합성 정렬은 전체 Z 값을 사용한다.
연속된 같은 종류와 이미지 페이지만 묶으므로 종류가 교차하면 draw 수가 증가한다.

따라서 뒤쪽 Canvas의 높은 `ZIndex` 노드가 앞쪽 Canvas의 낮은 `ZIndex` 노드보다
앞으로 나올 수 없다. 입력 dispatcher도 앞쪽 Canvas부터 검사해야 화면 표시와
클릭 우선순위가 일치한다.

## GPU 자원 소유권

`Visual2DRenderSystem::PrepareImages(paths, error)`는 D3D12에서 복사한 파일 경로의 WIC
디코드를 worker로 보냅니다. false는 준비 중이며, true라도 error가 있으면 실패입니다.
Ready 뒤 기존 LoadImage/RegisterImage로 GPU 자원을 만듭니다. 다른 renderer의 기본 구현은
동기 LoadImage 계약을 유지합니다. worker는 그래픽/Scene observer를 빌리지 않습니다.
PrepareImages/LoadImage/poll은 기존 렌더 소유 스레드에서 호출합니다.

텍스처 업로드는 렌더 queue에 제출한 뒤 이미지별 fence 대기를 하지 않습니다. 같은 queue의
후속 draw가 copy 뒤에 실행되며, command allocator/list·upload buffer·destination handle을
완료 fence까지 보관합니다. 정상 프레임에서 완료분을 해제하고 종료 시 남은 작업을 기다립니다.
TextureSet은 64칸 SRV 블록의 공유 lease를 소유합니다. 마지막 참조가 없어지면
원래 heap의 pool로 반환하며, 업로드·material·RenderToTexture의 프레임 참조가 GPU
fence까지 이를 유지하므로 사용 중인 descriptor는 재사용하지 않습니다. 관리자를
재초기화해도 이전 handle은 새 heap의 pool에 블록을 반환하지 않습니다.
관리자의 이미지 준비·로드·append·타깃 생성 및 GPU pass/descriptor 접근은 초기화한
renderer 스레드에서 수행합니다. 다른 스레드의 변경 요청은 작업 전에 logic_error로 거부합니다.
이 스레드 제한은 마지막 handle 해제에는 적용하지 않으며, lease pool의 반환은 mutex로 보호합니다.
append 권한은 현재 관리자 세대가 만든 집합의 약한 typed registry로 확인합니다. 읽기 전용
공개 handle을 const_cast하지 않으며 다른 관리자/이전 세대 handle은 디코드·업로드 전에 거부합니다.
정상 append는 같은 handle과 기존 슬롯을 유지합니다. Info가 반환한 참조는 이후 append 때
무효화될 수 있으므로 append 전후를 비교할 때는 TextureInfo 값을 복사합니다.
화면 SubmitScreen은 크기·배율·화면 원점·Z와 불변 payload를 포함한 draw 목록을
복사해 보관합니다. EndFrame에서 기록하므로 제출 뒤 Canvas와 node를 제거해도
그 프레임의 내용은 유지됩니다. clip 밖의 primitive는 기록 시 텍스트 shaping/instance
생성 전에 거릅니다.
Canvas 트리 자체의 수집 비용은 남으므로 큰 목록은 Client에서 가시 항목만 생성해야 합니다.

`DrawPacket::text`는 불변 문자열·글꼴·크기·정렬을 소유하는 `TextContentHandle`입니다.
TextVisual/ComboBox는 같은 내용을 반복 게시할 때 payload를 유지합니다.
D3D12는 payload의 약한 참조와 배치 handle을 별도로 유지하고 원본이 사라지면 회수합니다.
내용/스타일/배치 영역 변경 때만 shaping하며 색상·transform·clip·텍스트 offset은 재사용합니다.
`MeasuredSize()`는 마지막 렌더의 논리 크기로 첫 렌더 전에는 비어 있습니다.
GPU UV는 배치에 고정하지 않으며 글리프 residency와 upload/descriptor 수명은
[TextRendering](TextRendering.md)의 페이지 회수·frame fence 계약을 따릅니다.

`DrawPacketType::RectangleBatch`는 불변 사각형 geometry를 공유한다. component는
`shared_ptr<const vector<RectanglePrimitive>>`를 게시한 후 내용을 변경하지 않는다.
Canvas 수집은 geometry handle만 복사하며 node transform과 clip을 packet에 적용한다.
`ForEachDrawPrimitive`는 원래 paint 순서대로 사각형을 빌려 방문하며 별도의 펼친 배열을
할당하지 않는다. D3D12의 화면/평면/render texture 경로 모두 같은 방문 함수를 사용한다.
이 경계는 CPU 명령 복사를 줄이지만 GPU primitive 수나 draw call을 줄이는 기능은 아니다.
많은 사각형과 텍스트를 교차해 사용할 때는 묶음을 나누어 원래 그리기 순서를 유지한다.
명령을 보관하는 외부 renderer는 `DrawPrimitiveCount`로 실제 primitive 수를 셀 수 있다.


엔진 소유 `D3D12Renderer` 하나가 `D3D12Visual2DRenderer` 하나를 소유한다.
Scene은 renderer 수명에 관여하지 않고 Canvas, InputRouter, 이미지/렌더 타깃의
공유 핸들만 소유한다.

화면 Canvas는 `ScreenVisual2DManager::CreateOwnedCanvas`가 반환하는 이동 전용
`ScreenCanvasHandle`로 소유한다. handle을 Reset하거나 Scene이 소멸하면 등록도
자동 해제되므로 예외 경로와 `DestroyOnExit` 전환에서 Canvas가 남지 않는다.
Node 포인터는 여전히 Canvas가 소유하는 비소유 관찰자이며 handle보다 먼저 버린다.

`GpuMesh`와 `MaterialTemplate`의 공유 핸들이 각각의 비공개 GPU 구현을 보유한다.
Mesh 프레임의 보관 참조는 버퍼와 재질 파이프라인을 GPU fence까지 유지한다.
`TextureManager`도 비공개 구현을 소유하고 이미지 준비·로드·추가와 타깃 생성을 제공한다.
엔진 내부 `TextureRenderingBackend`는 장치·descriptor 바인딩과 타깃 pass를 기록한다.

`TextureSet`과 `RenderTargetTexture`의 공개 계약은 읽기 전용 메타데이터와 수명만
노출한다. 실제 `ID3D12Resource`, descriptor handle, RTV heap, resource state는
`TextureManager.cpp` 내부의 D3D12 구현 객체가 소유한다. `TextureManager`는
`friend` 접근 없이 자신이 만든 구현 타입을 검증한 뒤 upload와 state transition을
수행한다. 이 경계 덕분에 Client가 D3D12 자원 소유권을 오해하거나 직접 해제할 수
없다.

## Client 경계

Canvas 트리, 입력 라우팅, Surface/BVH, D3D12 배치, texture upload는 재사용 가능한
엔진 기능이다. 특정 패널의 크기·이미지·문구·오디오 선택 동작과 여러 Canvas 사이의
입력 우선순위는 게임 정책이므로 Client에 둔다. 새 게임을 만들 때 엔진 코드를
수정하지 않고 이 정책과 Scene만 교체한다.
