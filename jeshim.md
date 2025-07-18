요청하신 키워드를 기반으로 STM32 프로젝트를 위한 핵심 구성 요소들을 연결하여 정리해드리겠습니다. STM32H7 시리즈를 기준으로, **USB MTP 장치**를 **ThreadX + USBX Device + FATFS + SDMMC1** 구조로 구현할 때 필요한 요소들을 이해하기 쉽도록 체계적으로 설명합니다.

---

## ✅ 전체 구조 개요

```
[PC 탐색기]
   ↓ MTP 요청 (USB)
[STM32 USB_OTG_FS (Device)]
   ↓ USBX (Device Stack)
[USBX MTP Class (Custom)]
   ↓ 사용자 정의 처리 (usbd_mtp_if.c)
[ThreadX Task 내에서 처리됨]
   ↓ FatFS API 호출
[FATFS → SDMMC1]
   ↓ SD 카드 접근
[microSD 카드]
```

---

## 🧩 핵심 구성 요소 설명

| 구성요소                              | 설명                                                                             |
| --------------------------------- | ------------------------------------------------------------------------------ |
| **RTOS**                          | `ThreadX` 사용. `tx_kernel_enter()` → `MX_ThreadX_Init()` 에서 USBX Task 등 생성      |
| **ThreadX**                       | USB MTP 처리 등 백그라운드 작업을 RTOS Task로 처리함                                          |
| **USBX Device**                   | USB 장치 스택. `ux_device_stack_initialize()` 및 `ux_device_class_register()` 사용    |
| **USB\_OTG\_FS (Full Speed)**     | USB Full Speed 모드 (PA11: DM, PA12: DP). STM32H7B0VBT6는 내부 Full Speed PHY 사용 가능 |
| **MTP (Media Transfer Protocol)** | USBX에 커스텀 클래스로 구현. Windows에서 “MTP USB Device”로 인식됨                             |
| **FATFS**                         | MTP를 통해 접근할 파일 시스템 구현. `f_open`, `f_read`, `f_stat` 등 사용                       |
| **SDMMC1**                        | microSD 카드 인터페이스. PC8~~PC12 + PD2 (CMD/CLK/D0~~D3 + CD 핀)                      |
| **STM32CubeIDE 설정**               | USBX Device + ThreadX 활성화. FileX는 비활성화 (FATFS 사용 예정)                           |

---

## 📁 STM32CubeIDE 설정 요약 (v1.19.0 기준)

### 1. **Middleware 설정**

* `ThreadX`: ✅ Enable (`CMSIS RTOS2 wrapper` 활성화)
* `USBX Device`: ✅ Enable

  * Class: `Manual Custom Class`
  * 이름: `MTP` (또는 직접 구현)
  * Storage Class: ⛔ 비활성화 (FileX 제외)
* `FileX`: ⛔ 비활성화 (FATFS 사용)

### 2. **USB\_OTG\_FS 설정**

* Mode: **Device Only**
* PHY: **Embedded PHY**
* VBUS: **Software** or **Disable**
* SOF, ID pin 필요 없음 (Full Speed만 사용)

### 3. **SDMMC1 설정**

* Pins: PC8~~PC12 (D0~~D3, CLK, CMD), PD2 (CD)
* DMA 활성화 (TX/RX)
* ClockDiv: 5 (또는 안정적인 속도 설정)

---

## 🛠 작업 순서 요약

1. `.ioc`에서 ThreadX + USBX Device + FATFS + SDMMC1 구성
2. USBX MTP 커스텀 클래스 (`usbd_mtp.c/h`, `usbd_mtp_if.c/h`) 구현
3. USB 장치 초기화 (`MX_USB_DEVICE_Init()` 내부에서 `ux_device_stack_initialize()` 등 호출)
4. SD 카드 FatFS 마운트 및 파일 열기 테스트 (`f_mount`, `f_open`, `f_read`)
5. `GetObjectHandles`, `GetObjectInfo` 등 MTP 명령을 구현하여 PC 탐색기에서 보이게 함

---

## 🔌 디버깅 포인트

| 점검 항목    | 설명                                                             |
| -------- | -------------------------------------------------------------- |
| USB 인식   | 장치 관리자 → "MTP USB Device"로 인식되어야 함                             |
| SD 카드    | `f_mount`, `f_open` 성공 여부 확인                                   |
| USB 전송   | MTP 요청 처리 로그 추가 (예: `GetStorageIDs`, `GetObjectHandles` 호출 여부) |
| Stack 생성 | ThreadX Task에서 MTP 처리 루틴 실행 여부 확인                              |

---

## 📌 예시 MTP 경로 구조

```
- USB_DEVICE/App/usbd_mtp.c
- USB_DEVICE/App/usbd_mtp_if.c
- USB_DEVICE/App/usbd_desc.c
- Core/Src/main.c → USB + SD Init
- FATFS/Target/user_diskio.c → FatFS ↔ SDMMC1 연결
```

---

## 📎 참고 자료

* STM32 USBX Example: `STM32H735G` → USBX + MTP (ST GitHub)
* ST Application Note: **AN4879** (USB Device Stack)
* USB MTP Spec: PIMA 15740 (Windows에서 요구하는 필수 명령 확인)

---

필요하신 경우:

* `.ioc` 예제 파일 생성
* `usbd_mtp_if.c`, `usb_device.c`, `main.c` 초기화 코드
* `MTP Dummy Object` 구현 샘플

등을 단계별로 생성해드릴 수 있습니다. 원하시면 "MTP SD 카드 전체 구성 예제 생성해 줘" 라고 요청해 주세요.
