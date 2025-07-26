oyes_0.1 
RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK; // ✅ PLL 사용!

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
│   └── Target/
│       └── user_diskio.c
│
├── USB_DEVICE/
│   └── App/
│       └── usb_device.c
│
└── Middlewares/
    └── MTP/
        ├── usbd_mtp.c
        ├── usbd_mtp.h
        ├── usbd_mtp_if.c
        └── usbd_mtp_if.h
```
