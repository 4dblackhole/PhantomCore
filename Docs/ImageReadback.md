# 프레임 이미지 읽기

`RenderContext::ReadImage()`는 현재 프레임의 최종 렌더링 결과를
`std::shared_future<RgbaImage>`로 요청합니다. 공개 API에는 그래픽 backend의 장치,
명령 목록, 자원 또는 fence를 추가로 노출하지 않습니다.

## 결과와 호출 시점

- `IGameClient::Render`가 받은 현재 context에서 렌더 스레드로 호출합니다.
- 요청 뒤 제출한 그리기도 포함해 프레임 끝의 결과를 캡처합니다. 화면 합성 순서는
  바꾸지 않으며, 별도 오프스크린 대상 읽기는 제공하지 않습니다.
- 같은 프레임에서 여러 번 호출하면 같은 이미지 결과를 공유합니다.
- `RgbaImage`는 너비, 높이, `pixels`를 소유합니다. 픽셀은 왼쪽 위부터 행 순서로
  RGBA 8비트 채널 네 개가 연속되며 행 padding은 없습니다. 출력 채널에 별도의
  색 공간 변환을 적용하지 않습니다.
- 완료된 이미지는 엔진이나 원래 GPU 프레임 자원이 소멸한 뒤에도 future와 함께 유지됩니다.

```cpp
std::shared_future<RgbaImage> capture;

// Render(context) 안에서 요청합니다.
capture = context.ReadImage();

// 이후 Update 또는 Run 반환 뒤에 완료 여부를 확인합니다.
if (capture.valid() &&
    capture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
{
    const RgbaImage& image = capture.get();
    // image.pixels[(y * image.width + x) * 4 + channel]
}
```

## 완료와 오류

요청한 프레임을 제출하기 전에는 완료되지 않습니다. 엔진의 기존 프레임 자원 재사용
대기 또는 resize/종료의 GPU 대기에서 픽셀을 회수합니다. 캡처 때문에 매 프레임
전체 GPU 대기를 추가하지 않습니다. 렌더 루프가 다음 프레임으로 진행하기 전에
future를 기다리면 완료를 지연시킬 수 있으므로 `Render`/`Update`에서는 준비 여부만
조회하고, 준비된 결과만 읽습니다. 정상 종료에서는 `Client::Shutdown` 전에 완료됩니다.

비활성 context에서의 요청과 자원 할당 실패는 호출 시 예외를 던집니다. 요청 후
프레임 제출이 취소되거나 CPU 읽기가 실패하면 future의 `get()`이 예외를 전달합니다.
실패한 요청을 완료된 이미지로 취급하지 않습니다.

## 검증

`--image-readback`은 프레임 자원 수를 넘는 연속 캡처, 동일 프레임 중복 요청,
요청 뒤의 그리기 포함, RGBA 행 packing, 엔진 종료 뒤 결과 수명과 제출 취소를
검사합니다. `--text-pixels`는 이 API를 통해 실제 글리프 겹침의 픽셀 합성 순서를
검사합니다. 두 테스트는 `PhantomCore.h`만 사용합니다.

`--texture-lifetime`는 200개 타깃 해제·재할당, 동시 16세트 상한과 64프레임의
소유권을 검사합니다. 각 프레임에 빨강·파랑 타깃을 별도로 그리고 같은 화면에서
샘플링한 뒤 공개 ReadImage 결과의 두 색상을 확인합니다.
