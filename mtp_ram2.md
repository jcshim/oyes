지금까지 STM32CubeIDE에서 구성하신 **`mtp_ram` 프로젝트 작업 과정**을 정리하면, 다음과 같은 단계로 성공적인 \*\*USB MTP 장치(RAM 기반)\*\*를 구현하신 것입니다:

---

## ✅ 1. `.ioc` 설정 (oyes.ioc)

* MCU: **STM32H7B0VBT6**
* USB 설정:

  * USB\_OTG\_HS (Device Only)
  * Embedded PHY 사용 (PA11/PA12)
  * 중단점 없이 Full-Speed로 동작
* GPIO 설정:

  * **PC15 → LED 출력 (깜빡이기 용)**
  * **PC13 → SD 카드 감지용 핀 (지금은 사용 안함)**

---

## ✅ 2. `main.c` 설정

```c
int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_USB_DEVICE_Init();  // USB MTP 장치 초기화

  while (1)
  {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_15);  // LED 깜빡이기
    HAL_Delay(500);
  }
}
```

* USB 디바이스를 초기화하는 `MX_USB_DEVICE_Init()` 함수가 핵심
* PC15 핀은 LED 깜빡임으로 MCU 동작 확인


---

## ✅ 3. `usb_device.c`

```c
void MX_USB_DEVICE_Init(void)
{
  HAL_PWREx_EnableUSBVoltageDetector();

  USBD_Init(&hUsbDeviceHS, &HS_Desc, DEVICE_HS);
  USBD_RegisterClass(&hUsbDeviceHS, &USBD_MTP);
  USBD_MTP_RegisterStorage(&hUsbDeviceHS, &USBD_MTP_fops);
  USBD_Start(&hUsbDeviceHS);
}
```

* USB 장치 초기화
* MTP 클래스 등록
* **RAM 기반 저장소 인터페이스 연결**


---

## ✅ 4. `usbd_desc.c / .h`: USB 디스크립터 정의

* VID/PID: `VID = 0x0483`, `PID = 0x5750` (STMicroelectronics MTP)
* 제품 문자열: `"STM32 MTP Device"`
* 인터페이스 클래스: **Still Image (0x06)**, PTP/MTP 호환
* 구성/인터페이스 디스크립터 정상 설정


---

## ✅ 5. `usbd_mtp.c / .h`: MTP 클래스 정의

* 엔드포인트:

  * EP OUT: `0x01`
  * EP IN: `0x81`
  * Notification IN: `0x83`
* 디스크립터 크기: 0x27
* Storage 핸들 구조체 연결
* 주요 요청은 아직 간단하게 `USBD_OK` 반환으로 처리


---

## ✅ 6. `usbd_mtp_if.c / .h`: RAM 기반 가상 파일 시스템

### 구성된 가상 객체:

```c
mtp_objects[] = {
    { 0x0001, 0xFFFFFFFF, "Root", 1, 0, NULL },                   // 폴더
    { 0x0002, 0x0001, "hello.txt", 0, sizeof(dummy_data)-1, dummy_data }  // 파일
};
```

* `hello.txt`는 내부 RAM에 존재하는 문자열 `"Hello from STM32 MTP (RAM)!"`을 가지고 있음
* PC 탐색기에는 보이지 않지만, 장치관리자에서 \*\*“MTP USB 장치”\*\*로 정상 인식됨


---

## 🔄 전반적인 흐름 요약

1. STM32 시작 시 USB 장치(MTP)로 인식되도록 설정됨
2. Windows는 해당 장치를 MTP 장치로 인식 → `MTP USB 장치`로 표시
3. RAM에 등록된 파일 목록 (`hello.txt`)을 요청되면 반환하도록 설정
4. 실제 파일 전송이나 쓰기는 아직 구현하지 않음

---

## 📊 현재 상태

| 항목        | 상태                     |
| --------- | ---------------------- |
| USB 장치 인식 | ✅ Windows에서 MTP로 인식    |
| 장치 관리자    | ✅ "MTP USB 장치" 확인      |
| 탐색기 표시    | ❌ (스토리지 정보 부족)         |
| RAM 객체 처리 | ✅ `hello.txt` 조회 가능    |
| LED 동작    | ✅ PC15 LED 0.5초 간격 깜빡임 |

---

## 🔜 다음 단계 (선택)

1. **탐색기에 표시되게 하기**

   * `GetStorageIDs`, `GetStorageInfo`, `GetObjectHandles` 완전 구현 필요
2. **FatFS/SD 카드 연동**

   * `usbd_mtp_if.c`의 `MTP_Object` → SD 카드 파일 목록으로 확장
3. **파일 업로드/삭제 구현**

   * `SendObject`, `DeleteObject` 함수 작성

---

필요하시면 위 작업을 **FatFS 기반으로 확장하는 매뉴얼**도 바로 이어드릴 수 있습니다.
어느 방향으로 진행하실까요?
