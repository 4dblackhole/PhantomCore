# dr_mp3

Upstream: https://github.com/mackron/dr_libs
Revision: `dfe8377631000664666519fdb83da193fd8037f4`
File: `dr_mp3.h`(수정 없음)
SHA-256: `997b7ee18de6e6b81e2a83f1ea9fc62aef25c62b28d48db95635f49e65de0a2f`
License: MIT No Attribution(MIT-0). upstream의 이중 라이선스 중에서 골랐습니다.

어댑터는 Xing/Info와 LAME priming·끝 padding 처리를 포함한 고수준 float PCM API를 씁니다. 의도된 무음은 원본의 일부로 남습니다.
벤더 구현은 `ThirdParty/DrMp3.cpp`에서 한 번만 컴파일합니다. 벤더 헤더는 MSVC external include이고 벤더 구현은 소유 코드 정적 분석에서
빠집니다. 디코더 어댑터는 계속 /W4로 분석합니다.
