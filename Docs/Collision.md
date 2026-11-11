# 충돌 연산

`collision`은 렌더링, 장면 객체 소유권, D3D12 리소스와 무관한 stateless
충돌 판정 기능이다. Client에서는 다른 엔진 기능과 마찬가지로
`PhantomCore.h` 하나를 포함해 사용한다.

```cpp
#include "PhantomCore.h"
```

## 지원 타입

### 2D

- `Line2D`: 양방향으로 무한한 직선
- `LineSegment2D`: `start`와 `end` 사이의 유한 선분
- `Circle2D`: 중심과 반지름
- `Obb2D`: 중심, 로컬 반크기, 반시계 방향 회전각

지원 판정:

- 원과 무한 직선
- 원과 선분
- 원과 OBB
- OBB와 OBB
- 점에서 선분 또는 OBB까지 가장 가까운 점

### 3D

- `Line3D`: 양방향으로 무한한 직선
- `Ray3D`: 원점에서 진행 방향으로만 뻗는 반직선
- `LineSegment3D`: 두 점 사이의 유한 선분
- `Plane3D`: `dot(normal, point) = distanceFromOrigin` 평면
- `Sphere3D`: 중심과 반지름
- `ViewFrustum`: 안쪽을 향하는 left/right/bottom/top/near/far 평면
- `Obb3D`: 중심, 로컬 반크기, quaternion 회전

지원 판정:

- 평면과 직선, ray 또는 선분
- 구와 직선, ray 또는 선분
- 구와 OBB
- OBB와 OBB
- ViewFrustum과 Sphere의 `Outside`/`Intersecting`/`Inside` 분류

`Plane3D`의 normal은 정규화하지 않아도 된다. `Obb3D::orientation`은
`(x, y, z, w)` quaternion이며 판정 전에 내부에서 정규화된다.

## 직선과 선분의 차이

충돌 범위를 의도에 맞게 선택해야 한다.

```cpp
PhantomCore::Circle2D note{{100.0F, 80.0F}, 16.0F};

// 화면 전체를 가로지르는 수학적 직선과 판정한다.
const bool touchesGuideLine = PhantomCore::Intersects(
    note,
    PhantomCore::Line2D{{0.0F, 64.0F}, {1.0F, 0.0F}});

// 실제 두 끝점 사이만 판정한다.
const bool touchesLaneSegment = PhantomCore::Intersects(
    note,
    PhantomCore::LineSegment2D{{20.0F, 64.0F}, {180.0F, 64.0F}});
```

방향 벡터의 길이는 1일 필요가 없다. 방향이 0인 `Line2D`, `Line3D`,
`Ray3D`는 유효한 직선이 아니므로 충돌하지 않는 것으로 판정한다. 시작점과
끝점이 같은 선분은 하나의 점으로 판정한다.

## 평면 교점

평면 판정은 `Intersects`로 bool만 얻거나 `Intersect`로 교점 정보를 얻을 수
있다.

```cpp
PhantomCore::Plane3D ground{{0.0F, 1.0F, 0.0F}, 0.0F};
PhantomCore::Ray3D mouseRay{{0.0F, 10.0F, 0.0F}, {0.0F, -2.0F, 0.0F}};

if (const auto hit = PhantomCore::Intersect(ground, mouseRay))
{
    const DirectX::XMFLOAT3 point = hit->point;
    const DirectX::XMFLOAT3 normalizedNormal = hit->normal;

    // ray: origin + direction * parameter
    const float rayParameter = hit->parameter;
}
```

선분의 `parameter`는 항상 `[0, 1]` 범위다. 직선이나 ray가 평면 위에 완전히
놓인 경우 교점이 무한히 많으므로 입력 시작점을 대표 교점으로 반환하고
`parameter`는 0이 된다.

## OBB 판정

```cpp
PhantomCore::Obb2D first{{0.0F, 0.0F}, {2.0F, 1.0F}, 0.25F};
PhantomCore::Obb2D second{{2.5F, 0.0F}, {1.0F, 1.0F}, -0.4F};
const bool overlap2D = PhantomCore::Intersects(first, second);

PhantomCore::Obb3D worldBox{
    {0.0F, 0.0F, 0.0F},
    {1.0F, 2.0F, 1.0F},
    {0.0F, 0.0F, 0.0F, 1.0F}};
PhantomCore::Sphere3D player{{1.5F, 0.0F, 0.0F}, 0.75F};
const bool overlap3D = PhantomCore::Intersects(player, worldBox);
```

2D OBB 대 OBB는 네 로컬 축을 검사하는 SAT를 사용한다. 3D OBB 판정은
Windows SDK의 DirectXCollision 구현을 사용하되 공개 API에는 해당 구현 타입을
노출하지 않는다.

## Camera 절두체와 Bounding Sphere

`MakeViewFrustum`은 DirectX의 left-handed row-vector ViewProjection 행렬에서
정규화된 여섯 평면을 추출한다. Sphere 판정은 bool 대신
`VolumeIntersection::Outside`, `Intersecting`, `Inside`를 반환한다.

```cpp
const PhantomCore::ViewFrustum& frustum = camera.Frustum();
const PhantomCore::Sphere3D worldBounds = PhantomCore::TransformSphere(
    localBounds,
    transform.WorldMatrix());
const PhantomCore::VolumeIntersection visibility = PhantomCore::Classify(frustum, worldBounds);
```

`TransformSphere`는 로컬 중심을 월드 행렬로 변환하고 반지름에는 월드 축 중 가장
큰 절댓값 배율을 적용한다. 부모 비균일 scale과 자식 회전의 조합이 shear를 만들면
largest singular value를 넘는 보수적 matrix-norm 상한을 사용해 반지름을
과소평가하지 않는다.

`GpuMesh`의 로컬 Sphere는 Shape의 canonical position 정점에서 GPU mesh 생성 시
한 번 계산된다. `Camera`는 View/Projection dirty 상태에 Frustum 캐시를 연결하고,
`MeshInstance::Submit`은 `Outside`일 때 `MeshRenderSystem` 제출 전에 반환한다.
판정 코드는 Collision/Core에 있으므로 D3D12 renderer가 Camera를 소유하거나 직접
참조하지 않는다.

Screen Canvas의 `XMMatrixOrthographicOffCenterLH` 투영과 2D Rect clipping은 이
Camera 절두체 경로를 사용하지 않는다.

## 경계와 잘못된 입력

- 서로 접하기만 해도 충돌로 판정한다.
- 기본 허용 오차는 `DefaultEpsilon`이다.
- 각 함수의 마지막 인자로 다른 epsilon을 전달할 수 있다.
- 음수 반지름이나 음수 half extent는 충돌 없음으로 처리한다.
- 길이가 0인 plane normal과 quaternion은 충돌 없음으로 처리한다.
- 판정 함수는 잘못된 기하 입력 때문에 예외를 던지지 않는다.

## 테스트

`Tests/PhantomCore.Collision.Tests.vcxproj`는 다음 항목을 Debug/Release에서 검증한다.

- 평면 교점, 평행·동일 평면 처리, ray 방향 제한
- 원과 직선·선분의 접촉 및 비접촉
- 회전된 2D/3D OBB 판정
- 원/구와 OBB 판정
- 가장 가까운 점과 잘못된 크기·방향 처리
- Perspective/Orthographic 절두체의 여섯 평면과 경계 교차
- 부모 이동, 회전, 비균일 scale이 적용된 월드 Sphere와 Camera dirty 갱신

테스트 프로젝트도 기능 헤더가 아닌 `PhantomCore.h`만 포함한다.
