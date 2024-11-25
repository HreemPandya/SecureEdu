/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "liquidcrystal_i2c.h"
/* Constants -----------------------------------------------------------------*/
#define ACCESS_KEY_SIZE 8
#define MAX_DATA_SIZE 10240
#define KEY_SIZE 16
#define LCD_COLS 16
#define LCD_ROWS 2
#define SCROLL_DELAY 2000  // 2 seconds per line

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;  // For receiving data
UART_HandleTypeDef huart2;  // For debug output
I2C_HandleTypeDef hi2c1;

/* Function Prototypes ------------------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
void Error_Handler(void);
void deriveKeyFromAccessKey(const uint8_t* access_key, uint32_t timestamp, uint8_t* key);
void decryptData(uint8_t* data, size_t length, const uint8_t* key);
void MX_I2C1_Init(void);
void displayTextOnLCD(const char* text, size_t length);



/* Decryption functions */
void deriveKeyFromAccessKey(const uint8_t* access_key, uint32_t timestamp, uint8_t* key) {
	for(int i = 0; i < KEY_SIZE; i++) {
		key[i] = access_key[i % ACCESS_KEY_SIZE] ^
				((timestamp >> (i % 32)) & 0xFF) ^ 0x5A;
	}
}

void decryptData(uint8_t* data, size_t length, const uint8_t* key) {
	uint8_t keyStream[KEY_SIZE];
	memcpy(keyStream, key, KEY_SIZE);

	for(size_t i = 0; i < length; i++) {
		if(i > 0 && (i % KEY_SIZE) == 0) {
			for(int j = 0; j < KEY_SIZE; j++) {
				keyStream[j] = keyStream[j] ^ key[j] ^ (i & 0xFF);
			}
		}
		data[i] ^= keyStream[i % KEY_SIZE];
	}
}

/* UART receive function */
HAL_StatusTypeDef UART_Receive_Safe(UART_HandleTypeDef *huart, uint8_t *data, uint16_t size, uint32_t timeout) {
	HAL_StatusTypeDef status;
	uint32_t tickstart = HAL_GetTick();

	while ((HAL_GetTick() - tickstart) < timeout) {
		status = HAL_UART_Receive(huart, data, size, 100);
		if (status == HAL_OK) {
			return HAL_OK;
		}
		HAL_Delay(10);
	}
	return HAL_TIMEOUT;
}

int main(void) {
	HAL_Init();
	SystemClock_Config();
	MX_GPIO_Init();
	MX_I2C1_Init();  // Add I2C initialization
	MX_USART1_UART_Init();
	MX_USART2_UART_Init();

	// Initialize LCD
	HD44780_Init(2);
	HD44780_Clear();
	HD44780_SetCursor(0,0);
	HD44780_PrintStr("Decoder Ready");
	HD44780_SetCursor(0,1);
	HD44780_PrintStr("Waiting...");

	printf("\r\nText Selection System Decoder v2.1\r\n");
	printf("Waiting for data...\r\n\n");

	while(1) {
		uint8_t startMarker = 0;
		uint8_t access_key[ACCESS_KEY_SIZE + 1] = {0};
		uint32_t timestamp = 0;
		uint32_t data_size = 0;
		uint8_t *encrypted_data = NULL;
		uint8_t key[KEY_SIZE];
		HAL_StatusTypeDef status;

		// Clear any pending data
		while (HAL_UART_Receive(&huart1, &startMarker, 1, 1) == HAL_OK) {
			// Empty receive buffer
		}
		HAL_Delay(100);

		// 1. Wait for start marker
		printf("Waiting for start marker...\r\n");
		while (startMarker != 0xAA) {
			status = HAL_UART_Receive(&huart1, &startMarker, 1, 100);
			if (status != HAL_OK) continue;
		}
		printf("Received start marker: 0x%02X\r\n", startMarker);
		HAL_Delay(50);

		// 2. Get access key
		printf("Waiting for access key...\r\n");
		memset(access_key, 0, sizeof(access_key));
		status = UART_Receive_Safe(&huart1, access_key, ACCESS_KEY_SIZE, 1000);
		if (status != HAL_OK) {
			printf("Failed to receive access key (status: %d)\r\n", status);
			continue;
		}
		access_key[ACCESS_KEY_SIZE] = '\0';
		printf("Received access key: %s\r\n", access_key);
		HAL_Delay(50);

		// 3. Get timestamp
		printf("Waiting for timestamp...\r\n");
		status = UART_Receive_Safe(&huart1, (uint8_t*)&timestamp, sizeof(timestamp), 1000);
		if (status != HAL_OK) {
			printf("Failed to receive timestamp\r\n");
			continue;
		}
		printf("Received timestamp: %lu\r\n", (unsigned long)timestamp);
		HAL_Delay(50);

		// 4. Get data size
		printf("Waiting for data size...\r\n");
		status = UART_Receive_Safe(&huart1, (uint8_t*)&data_size, sizeof(data_size), 1000);
		if (status != HAL_OK) {
			printf("Failed to receive data size\r\n");
			continue;
		}
		printf("Received data size: %lu bytes\r\n", (unsigned long)data_size);

		// Validate data size
		if (data_size == 0 || data_size > MAX_DATA_SIZE) {
			printf("Invalid data size\r\n");
			continue;
		}

		// 5. Allocate memory for encrypted data
		printf("Allocating memory for data...\r\n");
		encrypted_data = malloc(data_size);
		if (encrypted_data == NULL) {
			printf("Memory allocation failed\r\n");
			continue;
		}

		// 6. Receive encrypted data
		printf("Receiving encrypted data...\r\n");
		size_t received = 0;
		while (received < data_size) {
			uint16_t chunk_size = (data_size - received > 32) ? 32 : data_size - received;
			status = UART_Receive_Safe(&huart1, encrypted_data + received, chunk_size, 1000);

			if (status != HAL_OK) {
				printf("Error receiving data at byte %zu\r\n", received);
				break;
			}

			received += chunk_size;
			if (received % 128 == 0 || received == data_size) {
				printf("Received %zu of %lu bytes\r\n", received, (unsigned long)data_size);
			}
			HAL_Delay(1);
		}

		if (received != data_size) {
			printf("Failed to receive complete data\r\n");
			free(encrypted_data);
			continue;
		}

		// 7. Wait for end marker
		uint8_t endMarker;
		status = UART_Receive_Safe(&huart1, &endMarker, 1, 1000);
		if (status != HAL_OK || endMarker != 0x55) {
			printf("Invalid end marker\r\n");
			free(encrypted_data);
			continue;
		}

		// 8. Decrypt the data
		printf("\r\nDecrypting data...\r\n");
		deriveKeyFromAccessKey(access_key, timestamp, key);
		decryptData(encrypted_data, data_size, key);

		printf("\r\n=== Decryption Summary ============================\r\n");
		printf("Access Key: %s\r\n", access_key);
		printf("Data Size : %lu bytes\r\n", (unsigned long)data_size);
		printf("Timestamp : %lu\r\n", (unsigned long)timestamp);
		printf("\r\n=== Decrypted Text ==============================\r\n");

		// Print to terminal with formatting
		char *text = (char*)encrypted_data;
		char *line_start = text;
		size_t pos = 0;

		while (pos < data_size) {
			if (text[pos] == '\n' || pos == data_size - 1) {
				if (pos == data_size - 1 && text[pos] != '\n') {
					pos++;
				}
				printf("  %.*s\r\n", (int)(text + pos - line_start), line_start);
				line_start = text + pos + 1;
			}
			pos++;
		}

		printf("================================================\r\n");
		printf("Decryption complete! Use access key '%s' to verify.\r\n", access_key);
		printf("================================================\r\n\n");

		// Display on LCD
		displayTextOnLCD((char*)encrypted_data, data_size);

		// Cleanup and continue as before
		free(encrypted_data);
		printf("Ready for next transmission...\r\n\n");

		// Show ready message on LCD
		HD44780_Clear();
		HD44780_SetCursor(0,0);
		HD44780_PrintStr("Ready for");
		HD44780_SetCursor(0,1);
		HD44780_PrintStr("next message");
	}
}

/* USART1 Initialization Function */
static void MX_USART1_UART_Init(void) {
	huart1.Instance = USART1;
	huart1.Init.BaudRate = 115200;
	huart1.Init.WordLength = UART_WORDLENGTH_8B;
	huart1.Init.StopBits = UART_STOPBITS_1;
	huart1.Init.Parity = UART_PARITY_NONE;
	huart1.Init.Mode = UART_MODE_RX;  // RX only for decoder
	huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart1.Init.OverSampling = UART_OVERSAMPLING_16;

	if (HAL_UART_Init(&huart1) != HAL_OK) {
		Error_Handler();
	}
}

/* USART2 Initialization Function */
static void MX_USART2_UART_Init(void) {
	huart2.Instance = USART2;
	huart2.Init.BaudRate = 115200;
	huart2.Init.WordLength = UART_WORDLENGTH_8B;
	huart2.Init.StopBits = UART_STOPBITS_1;
	huart2.Init.Parity = UART_PARITY_NONE;
	huart2.Init.Mode = UART_MODE_TX_RX;
	huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart2.Init.OverSampling = UART_OVERSAMPLING_16;

	if (HAL_UART_Init(&huart2) != HAL_OK) {
		Error_Handler();
	}
}


static void MX_GPIO_Init(void) {
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	/* GPIO Ports Clock Enable */
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();  // Added for I2C pins

	/* Configure UART1 pins */
	GPIO_InitStruct.Pin = GPIO_PIN_10;  // PA10 is RX
	GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
	GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
	HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

	/* Configure UART2 pins */
	GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_3;  // PA2 is TX, PA3 is RX
	GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
	GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
	HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

	/* Configure I2C1 pins */
	GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9;  // PB8 is SCL, PB9 is SDA
	GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
	HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/* Add this new function for LCD display */
void displayTextOnLCD(const char* text, size_t length) {
	char line_buffer[LCD_COLS + 1];
	char *text_ptr = (char*)text;
	size_t chars_processed = 0;
	int line_count = 0;

	// Clear LCD and show initial message
	HD44780_Clear();
	HD44780_SetCursor(0,0);
	HD44780_PrintStr("Decrypted Text:");
	HAL_Delay(1000);

	while (chars_processed < length) {
		size_t line_length = 0;
		// Find the length of the next line
		while (line_length < LCD_COLS &&
				chars_processed + line_length < length &&
				text_ptr[line_length] != '\n') {
			line_length++;
		}

		// Copy the line to buffer and null terminate
		memset(line_buffer, 0, sizeof(line_buffer));
		memcpy(line_buffer, text_ptr, line_length);

		// Display the line
		HD44780_Clear();
		HD44780_SetCursor(0,0);
		HD44780_PrintStr("Line ");
		char num[4];
		snprintf(num, sizeof(num), "%d:", ++line_count);
		HD44780_PrintStr(num);

		HD44780_SetCursor(0,1);
		HD44780_PrintStr(line_buffer);

		// Wait before showing next line
		HAL_Delay(SCROLL_DELAY);

		// Move to next line
		if (line_length < LCD_COLS) {
			// If we ended due to newline, skip it
			if (text_ptr[line_length] == '\n') {
				line_length++;
			}
		}

		text_ptr += line_length;
		chars_processed += line_length;
	}

	// Show completion message
	HD44780_Clear();
	HD44780_SetCursor(0,0);
	HD44780_PrintStr("End of Message");
	HAL_Delay(SCROLL_DELAY);
}

/* Add I2C initialization function */
void MX_I2C1_Init(void) {
	hi2c1.Instance = I2C1;
	hi2c1.Init.ClockSpeed = 100000;
	hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
	hi2c1.Init.OwnAddress1 = 0;
	hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
	hi2c1.Init.OwnAddress2 = 0;
	hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
	hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
	if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
		Error_Handler();
	}
}


/* Error Handler */
void Error_Handler(void) {
	while(1) {
		HAL_Delay(100);
	}
}

/* For printf output via UART2 */
int _write(int file, char *ptr, int len) {
	HAL_UART_Transmit(&huart2, (uint8_t*)ptr, len, HAL_MAX_DELAY);
	return len;
}

/* System Clock Configuration */
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
