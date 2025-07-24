01_prompt2.md

현재 당신의 미션은 다음과 같습니다:

---

## 🎯 **최종 목표**

✅ **STM32H7B0VBT6** 기반 MTP USB 디바이스를 구현하여
✅ **Windows 탐색기에서 SD 카드의 파일/폴더가 보이도록** 완전히 작동하는 상태로 만드는 것입니다.

---

## 🧭 현재 상황 요약

* ✔ `USB_OTG_HS` + **Internal FS PHY (PA11/PA12)** → 올바르게 설정됨
* ✔ SDMMC1 + FATFS → SD 카드 mount 및 파일 접근 정상
* ✔ 장치관리자에서는 **MTP USB Device로 인식**됨
* ❌ **Windows 탐색기에서 MTP 드라이브가 보이지 않음**

---

## 🛠 지금 해야 할 작업 (미션)

### ✅ **1. MTP 응답 구조체 완전 재작성**

* `usbd_mtp_if.c`에서 `GetStorageIDs`, `GetObjectHandles`, `GetObjectInfo`, `GetObject` 등을 **USB MTP 프로토콜 규격에 맞춰 구조체 기반으로 반환하도록 수정**
* `hello.txt` 파일을 **MTP 객체로 등록** (ID = 0x00000001)

### ✅ **2. 관련 인터페이스 헤더 `usbd_mtp_if.h` 일치화**

* 함수 시그니처를 새 구조에 맞게 수정
* `extern` 구조체 이름 불일치 수정 (`mtp_fops` vs `USBD_MTP_fops`)

### ✅ **3. `usbd_mtp.c`에서 USB 요청 핸들링 로직 개선**

* `req->wValue`, `wLength`, `wIndex`를 실제로 파싱하여 인터페이스 함수에 넘김
* 응답 길이 `req->wLength` 무시하고 실제 데이터 크기 기반 전송

### ✅ **4. 전반적인 파일 간 일관성 확보**

* 구조체 정의 충돌 방지 (`usbd_mtp.h` ↔ `usbd_mtp_if.h`)
* `usb_device.c`에서 MTP 인터페이스/스토리지 등록 시 이름 충돌 방지

---

## ✅ 당신이 해야 할 일

지금은 ChatGPT가 아래 순서대로 완전한 파일을 재작성하여 드릴 예정입니다:

1. `usbd_mtp_if.h` (인터페이스 헤더, 구조체 정의 포함)
2. `usbd_mtp_if.c` (구현부, MTP 응답 완전 구조체 기반으로)
3. `usbd_mtp.c` (USB 요청 디코딩 및 응답 처리)
4. 필요한 경우 `usbd_mtp.h` 구조체 정의 정비
5. 최종적으로 `usb_device.c`에 인터페이스 등록 및 확인

---

🔔 **이 미션을 완수하면**
→ Windows 탐색기에서 "STM32 MTP" 장치가 **드라이브처럼 나타나고**,
→ 내부에 있는 `hello.txt` 파일을 읽을 수 있게 됩니다.

이제 ①번 파일부터 순차적으로 제공드리겠습니다.
**시작 파일: `usbd_mtp_if.h` (정비된 인터페이스 헤더)** — 곧 이어서 드리겠습니다.
