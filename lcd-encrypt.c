/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Text Selection System with LCD Display
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "liquidcrystal_i2c.h"

/* Constants */
#define MAX_PARAGRAPHS 3
#define MAX_SENTENCES 10
#define AES_BLOCK_SIZE 16
#define KEY_SIZE 16
#define ACCESS_KEY_SIZE 8

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;
UART_HandleTypeDef huart2;

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2C1_Init(void);
void Error_Handler(void);

/* Encryption structures and variables */
typedef struct {
    uint8_t key[KEY_SIZE];
    uint8_t access_key[ACCESS_KEY_SIZE + 1];
    size_t data_size;
    uint8_t *encrypted_data;
    uint32_t timestamp;
} EncryptionInfo;

static EncryptionInfo encInfo = {0};

/* Text Content */
const char* PARAGRAPHS[MAX_PARAGRAPHS][MAX_SENTENCES] = {
    {
        "The quick brown fox jumps over the lazy dog.",
        "It was a bright and beautiful day.",
        NULL
    },
    {
        "Computer science is the study of computation and information processing.",
        "It encompasses theory algorithms and computational systems.",
        NULL
    },
    {
        "Embedded systems are computer systems with dedicated functions within a larger mechanical or electrical system.",
        "They are designed to perform specific tasks efficiently.",
        NULL
    }
};

/* Selection State */
typedef struct {
    int paragraph;
    int line;
    uint8_t isSet;
} TextPosition;

typedef enum {
    INPUT_PARAGRAPH,
    INPUT_LINE
} InputState;

TextPosition startPos = {0, 0, 0};
TextPosition endPos = {0, 0, 0};
InputState currentState = INPUT_PARAGRAPH;
TextPosition* currentPos = &startPos;

/* Helper Functions */
void updateLCDStatus(const char* line1, const char* line2) {
    HD44780_Clear();
    HD44780_SetCursor(0,0);
    HD44780_PrintStr(line1);
    if(line2) {
        HD44780_SetCursor(0,1);
        HD44780_PrintStr(line2);
    }
}

int _write(int file, char *ptr, int len) {
    HAL_UART_Transmit(&huart2, (uint8_t *)ptr, len, HAL_MAX_DELAY);
    return len;
}

void clearScreen(void) {
    printf("\033[2J\033[H");
}

void printLegend(void) {
    printf("\r\n=== Button Legend =============================\r\n");
    printf("A - 0 | B - 1 | C - 2 | D - ENTER\r\n");
    printf("=============================================\r\n");
}

int readRow(void) {
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_SET) return 0;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1) == GPIO_PIN_SET) return 1;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4) == GPIO_PIN_SET) return 2;
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_0) == GPIO_PIN_SET) return 3;
    return -1;
}

void printPrompt(const char* phase, const char* promptType) {
    printf("\r\n%s", phase);
    printf("\r\n%s:", promptType);
    printf("\r\n_");
}

void updateInput(const char* promptType, int value) {
    printf("\r\033[K");
    printf("\r%d", value);
}

/* Encryption Functions */
void generateAccessKey(void) {
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    srand(HAL_GetTick());
    for(int i = 0; i < ACCESS_KEY_SIZE; i++) {
        int index = rand() % strlen(charset);
        encInfo.access_key[i] = charset[index];
    }
    encInfo.access_key[ACCESS_KEY_SIZE] = '\0';
}

void deriveKeyFromAccessKey(void) {
    uint32_t timestamp = HAL_GetTick();
    for(int i = 0; i < KEY_SIZE; i++) {
        encInfo.key[i] = encInfo.access_key[i % ACCESS_KEY_SIZE] ^
                        ((timestamp >> (i % 32)) & 0xFF) ^ 0x5A;
    }
}

void encryptData(uint8_t* data, size_t length) {
    uint8_t keyStream[KEY_SIZE];
    memcpy(keyStream, encInfo.key, KEY_SIZE);

    for(size_t i = 0; i < length; i++) {
        if(i > 0 && (i % KEY_SIZE) == 0) {
            for(int j = 0; j < KEY_SIZE; j++) {
                keyStream[j] = keyStream[j] ^ encInfo.key[j] ^ (i & 0xFF);
            }
        }
        data[i] ^= keyStream[i % KEY_SIZE];
    }
}

void transmitEncryptedData(void) {
    uint8_t startMarker = 0xAA;
    HAL_UART_Transmit(&huart2, &startMarker, 1, HAL_MAX_DELAY);
    HAL_Delay(10);

    HAL_UART_Transmit(&huart2, encInfo.access_key, ACCESS_KEY_SIZE + 1, HAL_MAX_DELAY);
    HAL_Delay(10);

    HAL_UART_Transmit(&huart2, (uint8_t*)&encInfo.timestamp, 4, HAL_MAX_DELAY);
    HAL_Delay(10);

    HAL_UART_Transmit(&huart2, (uint8_t*)&encInfo.data_size, 4, HAL_MAX_DELAY);
    HAL_Delay(10);

    size_t chunkSize = 32;
    for(size_t i = 0; i < encInfo.data_size; i += chunkSize) {
        size_t currentChunk = ((encInfo.data_size - i) < chunkSize) ?
                              (encInfo.data_size - i) : chunkSize;
        HAL_UART_Transmit(&huart2, &encInfo.encrypted_data[i], currentChunk, HAL_MAX_DELAY);
        HAL_Delay(5);
    }

    uint8_t endMarker = 0x55;
    HAL_UART_Transmit(&huart2, &endMarker, 1, HAL_MAX_DELAY);
    HAL_Delay(10);
}

void encryptSelectedText(void) {
    char tempBuffer[1024] = {0};
    size_t totalLen = 0;

    for (int p = startPos.paragraph; p <= endPos.paragraph; p++) {
        for (int l = 0; PARAGRAPHS[p][l] != NULL; l++) {
            if ((p == startPos.paragraph && l >= startPos.line) ||
                (p == endPos.paragraph && l <= endPos.line) ||
                (p > startPos.paragraph && p < endPos.paragraph)) {
                strcat(tempBuffer + totalLen, PARAGRAPHS[p][l]);
                totalLen += strlen(PARAGRAPHS[p][l]);
                strcat(tempBuffer + totalLen, "\n");
                totalLen += 1;
            }
        }
    }

    size_t paddedSize = ((totalLen + AES_BLOCK_SIZE - 1) / AES_BLOCK_SIZE) * AES_BLOCK_SIZE;

    if (encInfo.encrypted_data != NULL) {
        free(encInfo.encrypted_data);
        encInfo.encrypted_data = NULL;
    }

    encInfo.encrypted_data = malloc(paddedSize);
    if (encInfo.encrypted_data == NULL) {
        printf("\r\nError: Memory allocation failed\r\n");
        updateLCDStatus("Error:", "Memory Full!");
        return;
    }
    encInfo.data_size = paddedSize;

    memcpy(encInfo.encrypted_data, tempBuffer, totalLen);
    memset(encInfo.encrypted_data + totalLen, 0, paddedSize - totalLen);

    updateLCDStatus("Generating", "Access Key...");
    generateAccessKey();
    encInfo.timestamp = HAL_GetTick();
    deriveKeyFromAccessKey();

    updateLCDStatus("Encrypting...", "Please Wait");
    encryptData(encInfo.encrypted_data, paddedSize);

    printf("\r\n=== Starting Encryption ========================\r\n");
    printf("Access Key: %s\r\n", encInfo.access_key);
    printf("Data Size: %d bytes\r\n", (int)paddedSize);
    printf("Transmitting data...\r\n");

    transmitEncryptedData();

    char keyBuffer[16];
    snprintf(keyBuffer, 16, "Key: %s", encInfo.access_key);
    updateLCDStatus("Encrypted!", keyBuffer);

    printf("Transmission complete!\r\n");
    printf("Store this access key to decrypt the text!\r\n");
    printf("===============================================\r\n\n");
}

void printSelectedText(void) {
    clearScreen();
    printLegend();
    printf("\r\n=== Selected Text ===============================\r\n");
    printf("From: P%d, L%d\r\n", startPos.paragraph, startPos.line);
    printf("To:   P%d, L%d\r\n", endPos.paragraph, endPos.line);
    printf("=============================================\r\n\n");

    char lcdBuffer[16];
    snprintf(lcdBuffer, 16, "From P%d,L%d", startPos.paragraph, startPos.line);
    updateLCDStatus(lcdBuffer, "Processing...");

    if (endPos.paragraph < startPos.paragraph ||
        (endPos.paragraph == startPos.paragraph && endPos.line < startPos.line)) {
        printf("Invalid selection! End must be after start.\r\n");
        updateLCDStatus("Error:", "Invalid Range!");
        return;
    }

    for (int p = startPos.paragraph; p <= endPos.paragraph; p++) {
        for (int l = 0; PARAGRAPHS[p][l] != NULL; l++) {
            if ((p == startPos.paragraph && l >= startPos.line) ||
                (p == endPos.paragraph && l <= endPos.line) ||
                (p > startPos.paragraph && p < endPos.paragraph)) {
                printf("%-75s\r\n", PARAGRAPHS[p][l]);
            }
        }
    }
    printf("\r\n");

    encryptSelectedText();
}

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
    /* MCU Configuration--------------------------------------------------------*/
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_I2C1_Init();
    MX_USART2_UART_Init();

    /* Initialize LCD */
    HD44780_Init(2);
    HD44780_Clear();
    updateLCDStatus("Text Selection", "System Ready");
    HAL_Delay(2000);

    clearScreen();
    printf("\r\n=== Text Selection System ========================");
    printLegend();
    printPrompt("START INDEX", "ENTER PARAGRAPH #");
    updateLCDStatus("Start Index", "Enter Para #");

    uint8_t inputReceived = 0;
    uint8_t buttonReleased = 1;

    while (1)
    {
        int row = readRow();

        if (row != -1 && buttonReleased) {
            buttonReleased = 0;
            char lcdBuffer[16];

            if (row >= 0 && row <= 2 && !inputReceived) {
                switch (currentState) {
                    case INPUT_PARAGRAPH:
                        if (row < MAX_PARAGRAPHS) {
                            currentPos->paragraph = row;
                            updateInput("ENTER PARAGRAPH #", row);
                            snprintf(lcdBuffer, 16, "Para #%d", row);
                            updateLCDStatus(lcdBuffer, "Press Enter");
                            inputReceived = 1;
                        }
                        break;

                    case INPUT_LINE:
                        if (PARAGRAPHS[currentPos->paragraph][row] != NULL) {
                            currentPos->line = row;
                            updateInput("ENTER LINE #", row);
                            snprintf(lcdBuffer, 16, "Line #%d", row);
                            updateLCDStatus(lcdBuffer, "Press Enter");
                            inputReceived = 1;
                        }
                        break;
                }
            }
            else if (row == 3 && inputReceived) {
                switch (currentState) {
                    case INPUT_PARAGRAPH:
                        currentState = INPUT_LINE;
                        printPrompt("", "ENTER LINE #");
                        updateLCDStatus("Enter Line #", "");
                        inputReceived = 0;
                        break;

                    case INPUT_LINE:
                        currentPos->isSet = 1;
                        if (currentPos == &startPos) {
                            currentPos = &endPos;
                            currentState = INPUT_PARAGRAPH;
                            printPrompt("\nEND INDEX", "ENTER PARAGRAPH #");
                            updateLCDStatus("End Index", "Enter Para #");
                        } else {
                            printSelectedText();
                        }
                        inputReceived = 0;
                        break;
                }
            }
        }
        else if (row == -1) {
            buttonReleased = 1;
        }

        HAL_Delay(10);
    }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /** Configure the main internal regulator output voltage */
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

    /** Initializes the RCC Oscillators according to the specified parameters
    * in the RCC_OscInitTypeDef structure.
    */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
        RCC_OscInitStruct.PLL.PLLM = 16;
        RCC_OscInitStruct.PLL.PLLN = 336;
        RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
        RCC_OscInitStruct.PLL.PLLQ = 7;
        if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
        {
            Error_Handler();
        }

        /** Initializes the CPU, AHB and APB buses clocks */
        RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                                    |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
        RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
        RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
        RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
        RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

        if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
        {
            Error_Handler();
        }
    }

    /**
      * @brief I2C1 Initialization Function
      * @param None
      * @retval None
      */
    static void MX_I2C1_Init(void)
    {
        hi2c1.Instance = I2C1;
        hi2c1.Init.ClockSpeed = 100000;
        hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
        hi2c1.Init.OwnAddress1 = 0;
        hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
        hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
        hi2c1.Init.OwnAddress2 = 0;
        hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
        hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
        if (HAL_I2C_Init(&hi2c1) != HAL_OK)
        {
            Error_Handler();
        }
    }

    /**
      * @brief USART2 Initialization Function
      * @param None
      * @retval None
      */
    static void MX_USART2_UART_Init(void)
    {
        huart2.Instance = USART2;
        huart2.Init.BaudRate = 115200;
        huart2.Init.WordLength = UART_WORDLENGTH_8B;
        huart2.Init.StopBits = UART_STOPBITS_1;
        huart2.Init.Parity = UART_PARITY_NONE;
        huart2.Init.Mode = UART_MODE_TX_RX;
        huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
        huart2.Init.OverSampling = UART_OVERSAMPLING_16;
        if (HAL_UART_Init(&huart2) != HAL_OK)
        {
            Error_Handler();
        }
    }

    /**
      * @brief GPIO Initialization Function
      * @param None
      * @retval None
      */
    static void MX_GPIO_Init(void)
    {
        GPIO_InitTypeDef GPIO_InitStruct = {0};

        /* GPIO Ports Clock Enable */
        __HAL_RCC_GPIOC_CLK_ENABLE();
        __HAL_RCC_GPIOH_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();

        /* Configure GPIO pin Output Level */
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_5, GPIO_PIN_RESET);

        /* Configure I2C1 GPIO Configuration */
        GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9;
        GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

        /* Configure input pins */
        GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4;
        GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
        GPIO_InitStruct.Pull = GPIO_PULLDOWN;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        GPIO_InitStruct.Pin = GPIO_PIN_0;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

        /* Configure UART pins */
        GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_3;  // PA2 is TX, PA3 is RX
        GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
        GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
    }

    /**
      * @brief  This function is executed in case of error occurrence.
      * @retval None
      */
    void Error_Handler(void)
    {
        __disable_irq();
        while (1)
        {
        }
    }

    #ifdef  USE_FULL_ASSERT
    void assert_failed(uint8_t *file, uint32_t line)
    {
        /* User can add his own implementation to report the file name and line number,
           ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
    }
    #endif /* USE_FULL_ASSERT */
