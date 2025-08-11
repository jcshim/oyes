
PS D:\> D:\mtp_diag.ps1 -USBVid 0483 -USBPid 5740 -DurationSec 30

좋아요! “그대로 복붙해 재현 가능한” 순차형 PowerShell 스크립트로 정리해서 파일로 만들어 뒀습니다.
아래 파일을 받아서 실행하시면 됩니다:


### 어떻게 쓰나요?

* 기본: `.\MTP_Device_Check.ps1 -VID 0483 -PID 5740`
* 또는 인스턴스ID 직접 지정: `.\MTP_Device_Check.ps1 -InstanceId "USB\VID_0483&PID_5740\..."`
* MTP/Portable Device 문자열 매칭만 보고 싶으면: `.\MTP_Device_Check.ps1 -NameFilter "(MTP|Portable Device)"`
* 최근 이벤트 로그 범위(분): `-LogMinutes 60`
* 드라이버 재시작 시도(관리자 권한 필요): `-TryRestartDevice`

### 스크립트에 들어있는 순서 (한 번에 실행)

1. 현재 연결된 USB 장치 표
2. 대상(VID/PID 또는 InstanceId) PnP 상태
3. 핵심 PnP 속성 조회: FriendlyName, Class, ClassGuid, Service, DriverVersion, ProblemCode
4. Win32\_PnPEntity에서 MTP/Portable Device 매칭
5. 최근 시스템 이벤트(USB/WPD/MTP 관련) 수집
6. MSC 여부 확인용 `Get-Disk`, `Get-Volume` (MTP는 일반적으로 여기에 안 나옴)
7. (옵션) 장치 재시작 `Disable/Enable-PnpDevice`
8. 요약: ClassGuid가 `{EEC5AD98-8080-425F-922A-DABF3DE3F69A}` 인지 등

실행이 끝나면 같은 폴더에 `MTP_Check_YYYYMMDD_HHMMSS.txt` 로그가 남습니다.
필요하면 이 스크립트에 **고정 VID/PID**를 박아 넣거나, **자동 재시도 루프**, **추가 DEVPKEY** 도 더해드릴게요.
