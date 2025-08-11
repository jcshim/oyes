oyes_3.0 

공식 사이트 Ux_Device_PIMA_MTP
RTOS, ThreadX, USBXDevice, Device, USB_OTG, Full Speed, MTP, SD Card, SDMMC

https://www.st.com/resource/en/user_manual/um1847-getting-started-with-stm32cubef1-firmware-package-for-stm32f1-series-stmicroelectronics.pdf
https://github.com/STMicroelectronics/x-cube-azrtos-h7/blob/main/Projects/STM32H735G-DK/Applications/USBX/Ux_Device_PIMA_MTP/README.md

MTPFFATFS_SD


PC  ⇄  USB 케이블  ⇄  STM32 (USB MTP 장치)  ⇄  MTP 프로토콜  
     ⇄  FATFS 파일 시스템  ⇄  SD 카드 슬롯  ⇄  microSD 카드
```
[PC 탐색기]
  ↓ MTP 요청 (USB)
[STM32 USB]
  ↓ USB 수신 → MTP 파싱
[MTP 핸들러 (usbd_mtp_if.c)]
  ↓ FATFS API 호출
[FATFS]
  ↓ 읽기 요청
[SDMMC1 → SD 슬롯]
  ↓ 데이터 가져오기
[microSD 카드]
  ↑ 읽은 데이터
[STM32]
  ↑ USB 응답
[PC 탐색기]
```

```
STM32_MTP_SD_FATFS/
│
├── STM32H7B0VBT6_MTP_SD_FATFS.ioc        ← CubeMX 설정 파일 (미완성)
├── STM32H7B0VBTX_FLASH.ld                ← 링커 스크립트
├── makefile
├── .project
│
├── Core/
│   ├── Inc/
│   │   └── main.h
│   └── Src/
│       ├── main.c
│       └── stm32h7xx_hal_msp.c
│
├── FATFS/
│
├── USB_DEVICE/
│       └── App/
│       │   └── usb_device.c
│       │   └── usb_device.c
│       │   ├── usbd_desc.c
│       │   ├── usbd_desc.h
│       │   ├── usbd_mtp.c
│       │   ├── usbd_mtp.h
│       │   ├── usbd_mtp_if.c
│       │   └── usbd_mtp_if.h
│       └── Target/
│             └── usbd_conf.c
│             └── usbd_conf.h
```
정리하면, 지금 구현된 MTP 기능은 이렇게 동작합니다.

1. **SD카드 + MTP 기반**

   * `main.c`에서 부팅 시 SDMMC1, FatFs, USB 장치를 초기화하고, 테스트용 `hello.txt`를 SD카드에 생성.
   * SD카드는 MTP 세션이 열릴 때 `f_mount()`로 마운트됨.

2. **USB MTP 클래스 구조**

   * `usb_device.c`에서 **MTP 클래스**(`USBD_MTP`)를 등록하고 시작.
   * `usbd_desc.c`에서 VID/PID, 제조사, 제품명, 시리얼 등 USB 디스크립터 문자열 설정.
   * `usbd_mtp.c`에서 MTP 프로토콜(PTP 기반) 최소 명령만 처리:

     * `GetDeviceInfo`, `OpenSession`, `GetStorageIDs`, `GetStorageInfo` 지원.
   * `usbd_mtp_if.c`에서 실제 SD카드와 연결:

     * 세션 열기 시 SD카드 마운트, 저장소 ID/정보 반환, 용량 계산.
     * Windows 탐색기에 "이동식 장치"로 나타나도록 StorageAdded 이벤트 전송.

3. **하드웨어/저수준 USB 설정**

   * `usbd_conf.c`에서 USB OTG HS 코어를 **Full-Speed + 내부 PHY**로 설정, PA11/PA12 사용.
   * 엔드포인트 3개 구성: Bulk IN(0x81), Bulk OUT(0x01), Interrupt IN(0x83).

4. **PC 연결 시 결과**

   * Windows 탐색기에서 **"STM32H735 MTP (FS)"** 라는 장치로 인식.
   * SD카드 내용 확인 가능, 초기 파일 `hello.txt` 즉시 표시.

즉, 현재는 **SD카드를 PC에 MTP 장치로 연결해서 파일을 읽을 수 있는 최소 기능**이 구현된 상태이며,
Windows에서 정상 인식·파일 접근이 가능하도록 초기화, 디스크립터, 프로토콜 최소 명령, 이벤트 전송이 갖춰져 있습니다.

