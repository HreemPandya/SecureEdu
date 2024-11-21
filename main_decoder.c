/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : decoder_main.c
  * @brief          : Text Decoder System
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Constants */
#define AES_BLOCK_SIZE 16
#define KEY_SIZE 16
#define ACCESS_KEY_SIZE 8
#define MAX_DATA_SIZE 2048
#define UART_TIMEOUT 5000
#define MAX_RETRIES 3

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart2;

/* Decryption structures */
typedef struct {
    uint8_t key[KEY_SIZE];
    uint8_t access_key[ACCESS_KEY_SIZE + 1];
    size_t data_size;
    uint8_t *encrypted_data;
    uint8_t *decrypted_data;
    uint32_t timestamp;
} DecryptionInfo;

static DecryptionInfo decInfo = {0};

/* Private function prototypes */
void SystemClock_Config(void);
void MX_GPIO_Init(void);
void MX_USART2_UART_Init(void);
void Error_Handler(void);

/* UART write override */
int _write(int file, char *ptr, int len) {
    HAL_UART_Transmit(&huart2, (uint8_t *)ptr, len, HAL_MAX_DELAY);
    return len;
}

/* Clear screen helper */
void clearScreen() {
    printf("\033[2J\033[H");
}

/* Core decryption functions */
void deriveKeyFromAccessKey() {
    for(int i = 0; i < KEY_SIZE; i++) {
        decInfo.key[i] = decInfo.access_key[i % ACCESS_KEY_SIZE] ^
                        ((decInfo.timestamp >> (i % 32)) & 0xFF) ^ 0x5A;
    }
}

void decryptData(uint8_t* data, size_t length) {
    uint8_t keyStream[KEY_SIZE];
    memcpy(keyStream, decInfo.key, KEY_SIZE);

    for(size_t i = 0; i < length; i++) {
        if(i > 0 && (i % KEY_SIZE) == 0) {
            for(int j = 0; j < KEY_SIZE; j++) {
                keyStream[j] = keyStream[j] ^ decInfo.key[j] ^ (i & 0xFF);
            }
        }
        data[i] ^= keyStream[i % KEY_SIZE];
    }
}

/* Improved UART reception with retry mechanism */
HAL_StatusTypeDef receiveWithTimeout(uint8_t* buffer, size_t size, uint32_t timeout) {
    uint32_t startTick = HAL_GetTick();

    while (HAL_GetTick() - startTick < timeout) {
        if (HAL_UART_Receive(&huart2, buffer, size, 100) == HAL_OK) {
            return HAL_OK;
        }
        HAL_Delay(10);
    }
    return HAL_TIMEOUT;
}

HAL_StatusTypeDef receiveEncryptedData() {
    // Wait for start marker
    uint8_t startMarker;
    if (receiveWithTimeout(&startMarker, 1, UART_TIMEOUT) != HAL_OK ||
        startMarker != 0xAA) {
        return HAL_ERROR;
    }

    // Clean previous data if any
    if (decInfo.encrypted_data != NULL) {
        free(decInfo.encrypted_data);
        decInfo.encrypted_data = NULL;
    }

    // Receive access key
    if (receiveWithTimeout(decInfo.access_key, ACCESS_KEY_SIZE + 1, UART_TIMEOUT) != HAL_OK) {
        printf("Error: Failed to receive access key\r\n");
        return HAL_ERROR;
    }

    // Receive timestamp
    if (receiveWithTimeout((uint8_t*)&decInfo.timestamp, 4, UART_TIMEOUT) != HAL_OK) {
        printf("Error: Failed to receive timestamp\r\n");
        return HAL_ERROR;
    }

    // Receive data size
    if (receiveWithTimeout((uint8_t*)&decInfo.data_size, 4, UART_TIMEOUT) != HAL_OK) {
        printf("Error: Failed to receive data size\r\n");
        return HAL_ERROR;
    }

    // Validate data size
    if (decInfo.data_size > MAX_DATA_SIZE || decInfo.data_size == 0) {
        printf("Error: Invalid data size: %d\r\n", (int)decInfo.data_size);
        return HAL_ERROR;
    }

    // Allocate memory for data
    decInfo.encrypted_data = malloc(decInfo.data_size);
    if (!decInfo.encrypted_data) {
        printf("Error: Memory allocation failed\r\n");
        return HAL_ERROR;
    }

    // Receive data in chunks
    size_t bytesReceived = 0;
    size_t chunkSize = 32;

    while (bytesReceived < decInfo.data_size) {
        size_t remainingBytes = decInfo.data_size - bytesReceived;
        size_t currentChunk = (remainingBytes < chunkSize) ? remainingBytes : chunkSize;

        if (receiveWithTimeout(&decInfo.encrypted_data[bytesReceived],
                             currentChunk, UART_TIMEOUT) != HAL_OK) {
            printf("Error: Failed to receive data chunk at %d\r\n", (int)bytesReceived);
            free(decInfo.encrypted_data);
            return HAL_ERROR;
        }

        bytesReceived += currentChunk;
    }

    // Wait for end marker
    uint8_t endMarker;
    if (receiveWithTimeout(&endMarker, 1, UART_TIMEOUT) != HAL_OK ||
        endMarker != 0x55) {
        printf("Error: Failed to receive end marker\r\n");
        free(decInfo.encrypted_data);
        return HAL_ERROR;
    }

    return HAL_OK;
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
    printf("\r\n=== Text Decoder System ========================\r\n");
    printf("Waiting for encrypted data...\r\n");

    while (1) {
        // Try to receive data
        if (receiveEncryptedData() == HAL_OK) {
            printf("\r\n=== Received Encrypted Data ===================\r\n");
            printf("Access Key: %s\r\n", decInfo.access_key);
            printf("Data Size: %d bytes\r\n", (int)decInfo.data_size);

            // Allocate memory for decrypted data
            decInfo.decrypted_data = malloc(decInfo.data_size + 1);  // +1 for null terminator
            if (decInfo.decrypted_data) {
                // Copy encrypted data
                memcpy(decInfo.decrypted_data, decInfo.encrypted_data, decInfo.data_size);
                decInfo.decrypted_data[decInfo.data_size] = '\0';  // Ensure null termination

                // Derive key and decrypt
                deriveKeyFromAccessKey();
                decryptData(decInfo.decrypted_data, decInfo.data_size);

                // Print decrypted text
                printf("\r\n=== Decrypted Text ===========================\r\n");
                printf("%s", (char*)decInfo.decrypted_data);
                printf("\r\n=============================================\r\n");

                // Clean up
                free(decInfo.decrypted_data);
                free(decInfo.encrypted_data);
                decInfo.encrypted_data = NULL;
                decInfo.decrypted_data = NULL;

                // Ready for next transmission
                printf("\r\nWaiting for next transmission...\r\n");
            }
        } else {
            HAL_Delay(100);  // Wait before retrying
        }
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
  /* User can add his own implementation to report the file name and line number */
  printf("Wrong parameters value: file %s on line %d\r\n", file, line);
}
#endif /* USE_FULL_ASSERT */
