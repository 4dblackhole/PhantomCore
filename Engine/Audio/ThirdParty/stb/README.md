# stb_vorbis

Upstream: https://github.com/nothings/stb
Revision: `2c980bb59875b0d32144a71867fbdebb2f77cd20`
File: `stb_vorbis.c`, v1.22(수정 없음)
SHA-256: `4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636`
License: MIT, LICENSE에 있는 그대로.

Client 에디터에서 엔진의 공유 순차 PCM 디코더로 옮겼습니다. 기존 external header·정적 분석 정책과 문서화한 upstream C4701 루프 불변식
우회는 벤더 구현에만 적용합니다. 이 디코더는 Opus가 아니라 Vorbis를 지원합니다. 연결(chained)·floor-0 Vorbis 스트림은 upstream이 문서화한 제한을 그대로 가집니다.
