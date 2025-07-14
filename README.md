# oyes
## fatfs_sd
- GPIO out PC15 LED main()의 while(1)에서 LED를 깜박이게 하라.
- GPIO in PC13  SDMMC1_CD 하고 FATFS에서 선택해 주라.
- sdmmc 핀번호 반드시 확인 특히 DAT0 번이 잘못될 경우가 많다.
- sdmmc ClockDiv 가 0이면 5로 바꾸라
- sdmmc HardwareFlow Control -> Enable로 바꾸라.
```
void Test_SD_Write(void)
{
    FATFS fs;
    FIL file;
    FRESULT res;
    UINT bytesWritten;

    const char *filename = "my2.txt";
    const char *text = "Love2";

    // SD 카드 마운트
    res = f_mount(&fs, "", 1);
    if (res != FR_OK) {
        printf("f_mount failed: %d\n", res);
        return;
    }

    // 파일 생성 및 열기 (쓰기 모드)
    res = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK) {
        printf("f_open failed: %d\n", res);
        f_mount(NULL, "", 0);
        return;
    }

    // 파일에 문자열 쓰기
    res = f_write(&file, text, strlen(text), &bytesWritten);
    if (res != FR_OK || bytesWritten == 0) {
        printf("f_write failed: %d\n", res);
    }

    f_close(&file);
    f_mount(NULL, "", 0);  // 언마운트

    printf("File write done: %s\n", filename);
}
```
