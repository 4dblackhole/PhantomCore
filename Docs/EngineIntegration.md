# PhantomCore 통합 가이드

새 게임은 엔진 소스의 폴더 구조를 알 필요 없이 `PhantomCore.h`와
`PhantomCore.lib`만으로 엔진 API를 사용한다.

## 같은 솔루션에서 사용하는 방법

Visual Studio 2022의 Client 프로젝트에 다음 설정을 적용한다.

1. `PhantomCore.vcxproj`를 프로젝트 참조로 추가한다.
2. **C/C++ > 일반 > 추가 포함 디렉터리**에 `Engine/SDK`를 추가한다.
3. Client 소스에서는 엔진 헤더를 다음 하나만 포함한다.

```cpp
#include "PhantomCore.h"
```

프로젝트 참조가 빌드 순서와 `PhantomCore.lib` 입력을 제공한다.
`PhantomCore.h`의 자동 링크 지시문은 D3D12, DirectWrite 및 FMOD import
library도 연결한다.
FMOD import library를 찾을 수 있도록 `Directory.Build.props`의 `FmodRoot`
탐색 설정을 유지해야 한다.

## 빌드 산출물만 전달하는 방법

다른 저장소나 솔루션에 엔진을 전달할 때에는 같은 구성의 다음 파일을 제공한다.

```text
SDK/
├─ include/PhantomCore.h
└─ lib/x64/Debug/PhantomCore.lib
   또는 lib/x64/Release/PhantomCore.lib
```

소비 프로젝트에는 다음 경로를 설정한다.

- **C/C++ > 추가 포함 디렉터리**: `PhantomCore.h`가 있는 폴더
- **링커 > 추가 라이브러리 디렉터리**: 해당 구성의 `PhantomCore.lib` 폴더
- **링커 > 추가 라이브러리 디렉터리**: FMOD x64 import library 폴더

Debug Client는 Debug 엔진과 `fmodL_vc.lib`/`fmodL.dll`을, Release Client는
Release 엔진과 `fmod_vc.lib`/`fmod.dll`을 사용한다. 런타임 라이브러리 설정
(`/MDd`, `/MD`)도 구성을 맞춘다.

자동 링크를 사용하지 않으려면 Client 전처리기에
`PHANTOMCORE_NO_AUTOLINK`를 정의하고 필요한 라이브러리를 직접 링크한다.

## 런타임에 별도로 필요한 파일

“하나의 lib와 헤더”는 엔진의 컴파일·링크 경계를 뜻한다. 다음 파일은 정적
라이브러리에 합칠 수 없거나 게임별 데이터이므로 별도로 배포한다.

- FMOD 런타임 DLL (`fmodL.dll` 또는 `fmod.dll`)
- Client가 사용하는 PNG와 그 밖의 게임 asset
- Client가 사용하는 글꼴과 해당 글꼴의 라이선스 문서
- FMOD 라이선스와 표기 요건에 따른 문서

공통 D3D12 셰이더는 빌드 시 `PhantomCore.lib`에 내장되므로 별도의 engine HLSL
파일은 필요하지 않다.

## 이름 표기

엔진의 공개 이름은 `PhantomCore` 네임스페이스에 있다. 소비자 헤더는
`PhantomCore::IGameClient`, `PhantomCore::RenderContext`처럼 명시한다. 구현 파일에서
짧은 이름을 쓰려면 모든 include 뒤에 `using namespace PhantomCore;`를 둘 수 있다.
공용 헤더에는 이 using 지시문을 두지 않으며 이전 전역 이름의 별칭은 제공하지 않는다.
Win32의 `LoadImage` 같은 매크로는 네임스페이스와 별개로 적용된다.

## 최소 Client 예시

```cpp
#include "PhantomCore.h"

#include <memory>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    auto game = std::make_unique<MyGameClient>();
    return PhantomCore::Run(std::move(game));
}
```

실제 게임에서는 `IGameClient`를 직접 구현하거나
`SceneGameClient`를 상속해 장면을 등록한다. 기능별 원본 헤더나
FMOD/D3D12 구현 전용 헤더를 Client에서 직접 포함하지 않는다.
