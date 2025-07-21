```
#include "fatfs.h"
#include <string.h>

void WriteTestFileToSD(void)
{
    FATFS fs;
    FIL file;
    UINT written;

    if (f_mount(&fs, "", 1) != FR_OK) {
        Error_Handler();
    }

    if (f_open(&file, "test.txt", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) {
        Error_Handler();
    }

    const char *msg = "Hello from STM32 root folder!\r\n";
    if (f_write(&file, msg, strlen(msg), &written) != FR_OK || written == 0) {
        Error_Handler();
    }

    f_close(&file);
}
```
