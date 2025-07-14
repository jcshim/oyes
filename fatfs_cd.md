아래는 현재 STM32H7B0VBT6 보드를 기반으로 SD 카드에 문자열 `"Love2"`를 `my2.txt` 파일에 쓰는 예제 코드(`main.c`)와 `.ioc` 설정을 바탕으로 구성된 **개발자 매뉴얼 (Developer Manual)** 입니다.

---

## 📘 STM32H7B0VBT6 SD Card + FatFs 개발자 매뉴얼

### ✅ 1. 개발환경 정보

* **Toolchain**: STM32CubeIDE 1.19.0
* **MCU**: STM32H7B0VBT6
* **사용한 미들웨어**: FatFs
* **I/O 기능**: SDMMC1 (4bit) + PC15 LED GPIO Toggle

---

### 🛠️ 2. 하드웨어 연결

| 기능          | 핀 번호 | 설명            |
| ----------- | ---- | ------------- |
| SDMMC1\_D0  | PC8  | 데이터 라인 0      |
| SDMMC1\_D1  | PC9  | 데이터 라인 1      |
| SDMMC1\_D2  | PC10 | 데이터 라인 2      |
| SDMMC1\_D3  | PC11 | 데이터 라인 3      |
| SDMMC1\_CK  | PC12 | SD 카드 클럭      |
| SDMMC1\_CMD | PD2  | 명령 라인         |
| SD Detect   | PC13 | SD 카드 삽입 감지 핀 |
| LED         | PC15 | 상태 표시용 LED    |

> 💡 `.ioc` 파일에서 정확한 핀 매핑을 자동으로 설정해야 합니다.

---

### ⚙️ 3. Clock 설정 요약

```c
RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
PLL 설정: M=4, N=8, P=2, Q=2, R=2
System Clock Source = HSI
AHB/APB 모두 DIV1
```

---

### 📁 4. 주요 함수 설명

#### 📌 `Test_SD_Write()`

SD 카드에 `"Love2"` 문자열을 `my2.txt` 파일로 저장하는 함수.

```c
void Test_SD_Write(void)
{
    FATFS fs;
    FIL file;
    FRESULT res;
    UINT bytesWritten;

    const char *filename = "my2.txt";
    const char *text = "Love2";

    res = f_mount(&fs, "", 1);
    if (res != FR_OK) return;

    res = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK) {
        f_mount(NULL, "", 0);
        return;
    }

    res = f_write(&file, text, strlen(text), &bytesWritten);
    f_close(&file);
    f_mount(NULL, "", 0);
}
```

#### 📌 `main()`

```c
int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_SDMMC1_SD_Init();
  MX_FATFS_Init();

  Test_SD_Write(); // SD 카드 쓰기 테스트 실행

  while (1)
  {
    HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_15);  // LED 깜빡임
    HAL_Delay(500);
  }
}
```

---

### 🧪 5. 테스트 방법

1. SD 카드 삽입 후 보드 전원 ON
2. LED(PC15)가 0.5초 간격으로 깜빡임 → 정상 동작 확인
3. SD 카드 제거 후 PC에 연결 → `my2.txt` 파일 확인
4. 파일 내용: `Love2` 포함되어 있어야 함

---

### 📦 6. 주의사항

* `MX_SDMMC1_SD_Init()`의 `ClockDiv = 5`는 안정적인 통신 속도를 위해 사용됨.
* `HardwareFlowControl`는 `ENABLE`로 설정됨.
* `fatfs.c/h`, `ffconf.h`, `sdmmc.c/h`는 자동 생성되므로 `.ioc` 수정 시 재생성 필요.

---

### 📌 7. 향후 확장 아이디어

* SD 카드 내 여러 파일 읽기/쓰기 지원
* MTP 또는 MSC를 통해 USB 파일 전송
* 파일 삭제 및 수정 기능
* SD 카드 용량 확인 및 사용량 표시

---

필요하시면 위 내용을 PDF로 정리하거나 PPT 버전도 제작해드릴 수 있습니다. 추가 요청 주세요!
