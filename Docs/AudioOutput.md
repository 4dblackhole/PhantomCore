# 오디오 출력과 Client 소유 Clip

`AudioSystem`은 Client에 공개하는 backend-neutral 서비스다. FMOD 타입은
`Engine/Audio/Backend/Fmod` 내부에만 존재하며, 생성된 `PhantomCore.h`에는 공개
계약만 포함한다.

## FMOD system 수명

`FmodAudioBackend`는 정확히 하나의 `FMOD::System*`만 소유한다. 초기화 순서는
다음과 같다.

1. `FMOD::System_Create`와 runtime version 확인
2. 요청한 output API, software sample rate, DSP buffer 설정
3. `FMOD::System::init`
4. 실제 output/sample/buffer 상태 확인
5. 현재 output API의 driver 열거
6. 명시적인 driver index가 있으면 해당 driver 선택

FMOD 제약상 `setSoftwareFormat`과 `setDSPBufferSize`는 `System::init` 전에
호출해야 한다. 반면 장치 열거는 초기화가 성공한 다음에만 실행한다. Backend는 임시
probe system을 만들지 않으며, 시작할 때 WASAPI와 ASIO를 함께 미리 조사하지 않는다.

## Output API와 driver 선택

`Automatic`은 요청 정책이고 `ActiveOutput()`은 FMOD가 선택한 실제 output이다.
Windows에서는 일반적으로 WASAPI가 선택되지만 두 값을 분리해서 제공한다.

```cpp
audio.RequestedOutput(); // Automatic, Wasapi, Asio, NoSound
audio.ActiveOutput();    // FMOD가 보고한 실제 output
```

`SetOutputBackend()`는 실행 중인 하나의 FMOD system에서 output을 변경한다. 이미
같은 요청값이 선택되어 있으면 아무것도 조사하지 않고 반환한다. 변경에 성공한
경우에만 새 현재 output에 대해 `getNumDrivers`와 `getDriverInfo`를 호출하고 다음
값을 캐시한다.

- `DriverCount()`: FMOD가 보고한 원래 driver 개수
- `OutputDrivers()`: 정보를 정상적으로 읽은 driver와 해당 FMOD index
- `ActiveDriverIndex()`: 현재 사용 중인 driver index

`SetOutputDriver()`는 캐시한 개수로 index를 검증한 뒤 `setDriver`를 호출하며
목록을 다시 조사하지 않는다. 따라서 Client의 위 ComboBox는 output API를 실제로
변경하고, 아래 ComboBox에는 현재 API에서 조사한 driver만 표시한다.

시작할 때 ASIO에서 WASAPI 또는 no-sound로 fallback하는 것은 `AudioSystem`의
정책이다. `FmodAudioBackend`는 전달받은 output 요청만 실행하며 내부에서 다른
API를 조용히 선택하지 않는다.

## Sample rate와 DSP buffer

`AudioConfig::sampleRate`, `dspBufferLength`, `dspBufferCount`는 초기 mixer 설정을
결정한다. 초기 sample rate가 0이면 output driver의 선호값을 사용하고, 명시적인
값은 8000~192000 Hz 범위여야 한다. 요청값과 실제 사용값은 다음 함수로 확인한다.

```cpp
audio.RequestedSampleRate();
audio.SampleRate();
audio.DspBufferLength();
audio.DspBufferCount();
audio.EstimatedDspLatencyMilliseconds();
```

`SetSampleRate()`와 `SetDspBufferSize()`는 FMOD 제약에 맞춰
`System::close`/`System::init` 과정을 실행한다. 새 설정이 실패하면 이전에
정상적으로 작동하던 설정으로 복구한다. `System::close`는 FMOD 자식 객체를
무효화하므로 Client 소유 clip·voice·bus·effect가 하나라도 남아 있으면 변경 요청을 거절한다.

여기서 DSP buffer는 FMOD software mixer의 buffer다. 오디오 인터페이스 제조사
제어판의 ASIO/USB streaming buffer는 driver 전용 설정이므로 FMOD Core의 공통
API로 변경할 수 없다.

## Client 소유 sound 수명

`FmodAudioBackend`는 WAV handle 목록을 소유하지 않는다. `AudioSystem::LoadSound`는
RAII `AudioClip`을 반환하며 Client가 그 `std::unique_ptr`의 수명을 관리한다.

```cpp
std::string error;
std::unique_ptr<PhantomCore::AudioClip> hitSound =
    audio.LoadSound(path, error);

if (hitSound != nullptr)
{
    hitSound->Play(error);
}

hitSound.reset();
```

Backend 내부의 `FmodAudioClip`이 `FMOD::Sound*`를 소유하므로 Client에는 FMOD
타입이 노출되지 않는다. Output API와 driver 변경은 같은 FMOD system에서
이루어지므로 이미 로드한 clip을 다시 등록할 필요가 없다. Mixer sample/buffer를
바꾸기 전에는 모든 clip을 해제해야 한다. 엔진 종료 시에는 공유 FMOD generation을
무효화하여 늦게 파괴되는 clip이 이미 해제된 system을 참조하지 못하게 한다.

ASIO 장치 목록은 Client가 ASIO를 선택하고 FMOD가 그 output을 활성화한 다음에만
만든다. 잘못 등록된 driver 하나의 `getDriverInfo`가 실패하면 Visual Studio Debug
Output에 `[PhantomCore.Audio]` 메시지를 기록하고 해당 항목만 건너뛴다.
