# 엔진 수학 값과 변환

공개 SDK는 `PhantomCore::Float2`, `Float3`, `Float4`, `Float4x4`를 사용한다.
이 타입은 표준 레이아웃이며 trivially copyable한 float 저장 값이다.
벡터 성분은 x/y/z/w, 행렬 원소는 m[행][열]로 접근한다.
크기는 각각 8/12/16/64바이트이며 SIMD 정렬을 요구하지 않는다.

행렬은 행 우선 저장과 행 벡터 곱을 사용한다. `Multiply(a, b)`는 a 변환 후 b 변환을 적용한다.
Camera의 시야·투영은 기존 왼손 좌표계를 유지하며 ViewMatrix/ProjectionMatrix/
ViewProjectionMatrix도 `Float4x4` 값으로 반환한다.

`IdentityMatrix`, `TranslationMatrix`, `ScalingMatrix`, `RotationYMatrix`,
`OrthographicLHMatrix`, `Multiply`, `Inverse`, `TransformPoint`, `TransformDirection`을 제공한다.
점 변환은 동차 나눗셈을 수행하며 방향 변환에는 평행 이동을 적용하지 않는다.
역행렬 계산은 특이 행렬에서 IEEE 부동소수점 결과를 반환하므로 가역성이 필요한 호출부는
별도로 확인한다. 각도는 라디안이고 `Pi`, `HalfPi`, `QuarterPi`를 사용한다.

저장 타입과 공개 선언은 Engine/Math/Math.h, 연산은 Math.cpp에 있다.
Math.cpp는 기존 Core 프로젝트와 통합 SDK, FMOD 없는 CPU 테스트에 등록한다.
Geometry와 Collision은 공통 저장 타입만 참조하므로 Core 역의존을 추가하지 않는다.
DirectXMathBridge.h와 SIMD 연산은 엔진 구현 내부에 한정한다.
변환은 크기·정렬을 확인한 값 복사로 수행하며 서로 다른 구조체를 포인터로 alias하지 않는다.
정점과 GPU 인스턴스의 기존 크기·필드 오프셋은 static_assert로 검사한다.

이 경계 정리는 공개 수학 타입의 DirectX 의존을 제거한다. 실행 플랫폼은 Windows다.
