/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Text Selection System with Encryption
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Constants */
#define MAX_PARAGRAPHS 3
#define MAX_SENTENCES 10
#define AES_BLOCK_SIZE 16
#define KEY_SIZE 16
#define ACCESS_KEY_SIZE 8

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart2;

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_GPIO_Init(void);
void MX_USART2_UART_Init(void);
void Error_Handler(void);

/* Encryption function prototypes */
void generateAccessKey(void);
void deriveKeyFromAccessKey(void);
void encryptData(uint8_t* data, size_t length);
void encryptSelectedText(void);

/* Encryption structures and variables */
typedef struct {
    uint8_t key[KEY_SIZE];
    uint8_t access_key[ACCESS_KEY_SIZE + 1];
    size_t data_size;
    uint8_t *encrypted_data;
    uint32_t timestamp;     // Make sure this is included
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

/* Override _write to enable printf over UART */
int _write(int file, char *ptr, int len) {
    HAL_UART_Transmit(&huart2, (uint8_t *)ptr, len, HAL_MAX_DELAY);
    return len;
}

/* Helper function to clear screen using UART */
void clearScreen() {
    printf("\033[2J\033[H");
}

/* Print legend */
void printLegend() {
    printf("\r\n=== Button Legend =============================\r\n");
    printf("A - 0 | B - 1 | C - 2 | D - ENTER\r\n");
    printf("=============================================\r\n");
}

/* Read the row pins to detect a press */
int readRow() {
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_0) == GPIO_PIN_SET) return 0;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_1) == GPIO_PIN_SET) return 1;
    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_4) == GPIO_PIN_SET) return 2;
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_0) == GPIO_PIN_SET) return 3;
    return -1;
}

/* Encryption Functions */
void generateAccessKey() {
    const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    srand(HAL_GetTick());  // Initialize random seed
    for(int i = 0; i < ACCESS_KEY_SIZE; i++) {
        int index = rand() % strlen(charset);
        encInfo.access_key[i] = charset[index];
    }
    encInfo.access_key[ACCESS_KEY_SIZE] = '\0';
}

void deriveKeyFromAccessKey() {
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
        // Update key stream for each block
        if(i > 0 && (i % KEY_SIZE) == 0) {
            for(int j = 0; j < KEY_SIZE; j++) {
                keyStream[j] = keyStream[j] ^ encInfo.key[j] ^ (i & 0xFF);
            }
        }
        // Encrypt data using key stream
        data[i] ^= keyStream[i % KEY_SIZE];
    }
}

void encryptSelectedText() {
    // Concatenate selected text
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

    // Calculate padded size
    size_t paddedSize = ((totalLen + AES_BLOCK_SIZE - 1) / AES_BLOCK_SIZE) * AES_BLOCK_SIZE;

    // Free previous allocation if exists
    if (encInfo.encrypted_data != NULL) {
        free(encInfo.encrypted_data);
        encInfo.encrypted_data = NULL;
    }

    // Allocate and prepare encrypted data
    encInfo.encrypted_data = malloc(paddedSize);
    if (encInfo.encrypted_data == NULL) {
        printf("\r\nError: Memory allocation failed\r\n");
        return;
    }
    encInfo.data_size = paddedSize;

    // Copy and pad data
    memcpy(encInfo.encrypted_data, tempBuffer, totalLen);
    memset(encInfo.encrypted_data + totalLen, 0, paddedSize - totalLen);

    // Generate keys and timestamp FIRST
    generateAccessKey();
    encInfo.timestamp = HAL_GetTick();  // Set timestamp before encryption
    deriveKeyFromAccessKey();

    // Encrypt the data
    encryptData(encInfo.encrypted_data, paddedSize);

    // Print encryption results and transmit
    printf("\r\n=== Starting Encryption ========================\r\n");
    printf("Access Key: %s\r\n", encInfo.access_key);
    printf("Data Size: %d bytes\r\n", (int)paddedSize);
    printf("Transmitting data...\r\n");

    // Transmit data
    transmitEncryptedData();

    printf("Transmission complete!\r\n");
    printf("Store this access key to decrypt the text!\r\n");
    printf("===============================================\r\n\n");
}

/* Add new transmission function */
void transmitEncryptedData() {
	// Send start marker with delay
	    uint8_t startMarker = 0xAA;
	    HAL_UART_Transmit(&huart2, &startMarker, 1, HAL_MAX_DELAY);
	    HAL_Delay(10);

	    // Send access key
	    HAL_UART_Transmit(&huart2, encInfo.access_key, ACCESS_KEY_SIZE + 1, HAL_MAX_DELAY);
	    HAL_Delay(10);

	    // Send timestamp
	    HAL_UART_Transmit(&huart2, (uint8_t*)&encInfo.timestamp, 4, HAL_MAX_DELAY);
	    HAL_Delay(10);

	    // Send data size
	    HAL_UART_Transmit(&huart2, (uint8_t*)&encInfo.data_size, 4, HAL_MAX_DELAY);
	    HAL_Delay(10);

	    // Send encrypted data in chunks
	    size_t chunkSize = 32;
	    for(size_t i = 0; i < encInfo.data_size; i += chunkSize) {
	        size_t currentChunk = ((encInfo.data_size - i) < chunkSize) ?
	                              (encInfo.data_size - i) : chunkSize;
	        HAL_UART_Transmit(&huart2, &encInfo.encrypted_data[i], currentChunk, HAL_MAX_DELAY);
	        HAL_Delay(5);
	    }

	    // Send end marker
	    uint8_t endMarker = 0x55;
	    HAL_UART_Transmit(&huart2, &endMarker, 1, HAL_MAX_DELAY);
	    HAL_Delay(10);
}

void printSelectedText() {
    clearScreen();
    printLegend();
    printf("\r\n=== Selected Text ===============================\r\n");
    printf("From: P%d, L%d\r\n", startPos.paragraph, startPos.line);
    printf("To:   P%d, L%d\r\n", endPos.paragraph, endPos.line);
    printf("=============================================\r\n\n");

    // Validate selection
    if (endPos.paragraph < startPos.paragraph ||
        (endPos.paragraph == startPos.paragraph && endPos.line < startPos.line)) {
        printf("Invalid selection! End must be after start.\r\n");
        return;
    }

    // Print paragraphs and lines within selection
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

    // Start encryption process immediately after showing selection
    encryptSelectedText();
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

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void) {
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART2_UART_Init();

    clearScreen();
    printf("\r\n=== Text Selection System ========================");
    printLegend();
    printPrompt("START INDEX", "ENTER PARAGRAPH #");

    uint8_t inputReceived = 0;
    uint8_t buttonReleased = 1;

    while (1) {
        int row = readRow();

        if (row != -1 && buttonReleased) {
            buttonReleased = 0;

            if (row >= 0 && row <= 2 && !inputReceived) {
                switch (currentState) {
                    case INPUT_PARAGRAPH:
                        if (row < MAX_PARAGRAPHS) {
                            currentPos->paragraph = row;
                            updateInput("ENTER PARAGRAPH #", row);
                            inputReceived = 1;
                        }
                        break;

                    case INPUT_LINE:
                        if (PARAGRAPHS[currentPos->paragraph][row] != NULL) {
                            currentPos->line = row;
                            updateInput("ENTER LINE #", row);
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
                        inputReceived = 0;
                        break;

                    case INPUT_LINE:
                        currentPos->isSet = 1;
                        if (currentPos == &startPos) {
                            currentPos = &endPos;
                            currentState = INPUT_PARAGRAPH;
                            printPrompt("\nEND INDEX", "ENTER PARAGRAPH #");
                        } else {
                            printSelectedText();  // This will now also handle encryption
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

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 16;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_CLOCKTYPE_HCLK;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
void MX_USART2_UART_Init(void)
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

	    // Configure GPIO pins for UART2
	    GPIO_InitTypeDef GPIO_InitStruct = {0};

	    /* Enable GPIO Ports Clock */
	    __HAL_RCC_GPIOA_CLK_ENABLE();

	    /* Configure UART pins */
	    GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_3;  // PA2 is TX, PA3 is RX
	    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	    GPIO_InitStruct.Pull = GPIO_NOPULL;
	    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	    GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
	    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* Configure GPIO pins for Rows as Input with Pull-Down */
  GPIO_InitStruct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_4;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = GPIO_PIN_0;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
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
