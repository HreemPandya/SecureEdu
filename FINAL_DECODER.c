/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Secure Text System with Access Key Verification
 ******************************************************************************
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "liquidcrystal_i2c.h"

/* Constants -----------------------------------------------------------------*/
#define ACCESS_KEY_SIZE 8
#define MAX_DATA_SIZE 10240
#define KEY_SIZE 16
#define LCD_COLS 16
#define LCD_ROWS 2
#define SCROLL_DELAY 2000  // 2 seconds per line
#define KEYPAD_ROWS 4
#define KEYPAD_COLS 4
#define DEBOUNCE_DELAY 200  // ms

/* GPIO Pin Definitions */
#define ROW1_PIN GPIO_PIN_0
#define ROW2_PIN GPIO_PIN_1
#define ROW3_PIN GPIO_PIN_4
#define ROW4_PIN GPIO_PIN_3
#define ROW_PORT GPIOA

#define COL1_PIN GPIO_PIN_3
#define COL2_PIN GPIO_PIN_5
#define COL3_PIN GPIO_PIN_4
#define COL4_PIN GPIO_PIN_10
#define COL_PORT GPIOB

#define LCD_ADDR (0x27 << 1)

/* Private variables ---------------------------------------------------------*/
UART_HandleTypeDef huart1;  // For receiving data
UART_HandleTypeDef huart2;  // For debug output
I2C_HandleTypeDef hi2c1;

/* Keypad Configuration */
const uint16_t ROW_PINS[KEYPAD_ROWS] = {ROW1_PIN, ROW2_PIN, ROW3_PIN, ROW4_PIN};
const uint16_t COL_PINS[KEYPAD_COLS] = {COL1_PIN, COL2_PIN, COL3_PIN, COL4_PIN};

const char KEYPAD_MAP[KEYPAD_ROWS][KEYPAD_COLS] = {
		{'1', '2', '3', 'A'},
		{'5', '4', '6', 'B'},
		{'7', '8', '9', 'C'},
		{'*', '0', '#', 'D'}
};

/* Access Key Verification */
typedef struct {
	uint8_t key[ACCESS_KEY_SIZE + 1];
	uint8_t position;
	bool isComplete;
	bool isVerified;
} KeypadInput;

static KeypadInput keypadState = {0};
static uint8_t receivedAccessKey[ACCESS_KEY_SIZE + 1] = {0};
static bool accessKeyReceived = false;
static uint8_t decryption_key[KEY_SIZE] = {0};
static uint8_t encrypted_buffer[MAX_DATA_SIZE] = {1};

/* Function Prototypes */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_I2C1_Init(void);
void Error_Handler(void);
void Keypad_Init(void);
char Keypad_Scan(void);
bool ProcessKeypadInput(void);
void deriveKeyFromAccessKey(const uint8_t* access_key, uint32_t timestamp, uint8_t* key);
void decryptData(uint8_t* data, size_t length, const uint8_t* key);
void displayTextOnLCD(const char* text, size_t length);
HAL_StatusTypeDef UART_Receive_Safe(UART_HandleTypeDef *huart, uint8_t *data, uint16_t size, uint32_t timeout);

HAL_StatusTypeDef UART_Receive_Safe(UART_HandleTypeDef *huart, uint8_t *data, uint16_t size, uint32_t timeout) {
	uint32_t tickstart = HAL_GetTick();
	uint16_t received = 0;

	while (received < size) {
		if ((HAL_GetTick() - tickstart) >= timeout) {
			return HAL_TIMEOUT;
		}

		HAL_StatusTypeDef status = HAL_UART_Receive(huart, &data[received], 1, 100);
		if (status == HAL_OK) {
			received++;
		} else if (status != HAL_TIMEOUT) {
			return status;
		}
		HAL_Delay(1);
	}
	return HAL_OK;
}

/* Keypad Initialization */
void Keypad_Init(void) {
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	// Configure row pins as inputs with pull-up
	GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

	for(int i = 0; i < KEYPAD_ROWS; i++) {
		GPIO_InitStruct.Pin = ROW_PINS[i];
		HAL_GPIO_Init(ROW_PORT, &GPIO_InitStruct);
	}

	// Configure column pins as outputs
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;

	for(int i = 0; i < KEYPAD_COLS; i++) {
		GPIO_InitStruct.Pin = COL_PINS[i];
		HAL_GPIO_Init(COL_PORT, &GPIO_InitStruct);
		HAL_GPIO_WritePin(COL_PORT, COL_PINS[i], GPIO_PIN_SET);
	}
}

static void HD44780_Write_Cmd(uint8_t cmd) {
	uint8_t data_u, data_l;
	uint8_t data_t[4];
	data_u = (cmd & 0xf0);
	data_l = ((cmd << 4) & 0xf0);
	data_t[0] = data_u | 0x0C;  // en=1, rs=0
	data_t[1] = data_u | 0x08;  // en=0, rs=0
	data_t[2] = data_l | 0x0C;  // en=1, rs=0
	data_t[3] = data_l | 0x08;  // en=0, rs=0
	HAL_I2C_Master_Transmit(&hi2c1, LCD_ADDR, (uint8_t)data_t, 4, 100);
}

static void HD44780_Write_Data(uint8_t data) {
	uint8_t data_u, data_l;
	uint8_t data_t[4];
	data_u = (data & 0xf0);
	data_l = ((data << 4) & 0xf0);
	data_t[0] = data_u | 0x0D;  // en=1, rs=1
	data_t[1] = data_u | 0x09;  // en=0, rs=1
	data_t[2] = data_l | 0x0D;  // en=1, rs=1
	data_t[3] = data_l | 0x09;  // en=0, rs=1
	HAL_I2C_Master_Transmit(&hi2c1, LCD_ADDR, (uint8_t*)data_t, 4, 100);
}

void HD44780_Write(uint8_t data, uint8_t rs) {
	if (rs == 0) {
		HD44780_Write_Cmd(data);
	} else {
		HD44780_Write_Data(data);
	}
	HAL_Delay(1);  // Short delay for command processing
}

void HD44780_PrintChar(char c) {
	uint8_t data = (uint8_t)c;
	HD44780_Write(data, 1);  // Assuming HD44780_Write is your existing write function
}
/* Keypad Scanning */
char Keypad_Scan(void) {
	static uint32_t lastDebounceTime = 0;

	if(HAL_GetTick() - lastDebounceTime < DEBOUNCE_DELAY) {
		return 0;
	}

	for(int col = 0; col < KEYPAD_COLS; col++) {
		// Set current column LOW, others HIGH
		for(int j = 0; j < KEYPAD_COLS; j++) {
			HAL_GPIO_WritePin(COL_PORT, COL_PINS[j], j == col ? GPIO_PIN_RESET : GPIO_PIN_SET);
		}

		HAL_Delay(1);  // Stabilization delay

		// Check each row
		for(int row = 0; row < KEYPAD_ROWS; row++) {
			if(HAL_GPIO_ReadPin(ROW_PORT, ROW_PINS[row]) == GPIO_PIN_RESET) {
				lastDebounceTime = HAL_GetTick();

				// Wait for key release
				while(HAL_GPIO_ReadPin(ROW_PORT, ROW_PINS[row]) == GPIO_PIN_RESET);
				HAL_Delay(10);

				return KEYPAD_MAP[row][col];
			}
		}
	}
	return 0;
}

/* Process Keypad Input */
bool ProcessKeypadInput(void) {
	char key = Keypad_Scan();

	if(key && !keypadState.isComplete && keypadState.position < ACCESS_KEY_SIZE) {
		// Store the key
		keypadState.key[keypadState.position] = key;
		printf("Key pressed: %c\r\n", key);

		// Display asterisk on LCD
		HD44780_SetCursor(keypadState.position, 1);
		HD44780_PrintChar('*');

		keypadState.position++;

		// Check if we've received all required digits
		if(keypadState.position == ACCESS_KEY_SIZE) {
			keypadState.isComplete = true;

			printf("Entered key: ");
			for(int i = 0; i < ACCESS_KEY_SIZE; i++) {
				printf("%c", keypadState.key[i]);
			}
			printf("\r\nReceived key: ");
			for(int i = 0; i < ACCESS_KEY_SIZE; i++) {
				printf("%c", receivedAccessKey[i]);
			}
			printf("\r\n");

			// Verify the access key
			bool match = true;
			for(int i = 0; i < ACCESS_KEY_SIZE; i++) {
				if(keypadState.key[i] != receivedAccessKey[i]) {
					match = false;
					break;
				}
			}

			if(match) {
				keypadState.isVerified = true;
				HD44780_Clear();
				HD44780_SetCursor(0,0);
				HD44780_PrintStr("Access Granted!");
				printf("Access key verified! Proceeding with decryption...\r\n");
				HAL_Delay(1000);
				return true;
			} else {
				// Invalid key
				HD44780_Clear();
				HD44780_SetCursor(0,0);
				HD44780_PrintStr("Invalid Key!");
				HAL_Delay(1000);

				// Reset state
				memset(&keypadState, 0, sizeof(KeypadInput));

				// Redisplay prompt
				HD44780_Clear();
				HD44780_SetCursor(0,0);
				HD44780_PrintStr("Enter Access Key:");
				HD44780_SetCursor(0,1);
			}
		}
	}
	return false;
}

/* Decryption Functions */
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

/* LCD Display Function */
void displayTextOnLCD(const char* text, size_t length) {
	char line_buffer[LCD_COLS + 1];
	const char* text_ptr = text;
	size_t chars_processed = 0;
	size_t line_count = 0;

	HD44780_Clear();
	HD44780_SetCursor(0,0);
	HD44780_PrintStr("Decrypted Text:");
	HAL_Delay(1000);

	while (chars_processed < length) {
		size_t line_length = 0;
		while (line_length < LCD_COLS && chars_processed + line_length < length &&
				text_ptr[line_length] != '\n') {
			line_length++;
		}

		memset(line_buffer, 0, sizeof(line_buffer));
		memcpy(line_buffer, text_ptr, line_length);

		HD44780_Clear();
		HD44780_SetCursor(0,0);
		char num_str[8];
		snprintf(num_str, sizeof(num_str), "Line %u:", (unsigned int)++line_count);
		HD44780_PrintStr(num_str);

		HD44780_SetCursor(0,1);
		HD44780_PrintStr(line_buffer);

		HAL_Delay(SCROLL_DELAY);

		if (text_ptr[line_length] == '\n') {
			line_length++;
		}
		text_ptr += line_length;
		chars_processed += line_length;
	}

	HD44780_Clear();
	HD44780_SetCursor(0,0);
	HD44780_PrintStr("End of Message");
	HAL_Delay(SCROLL_DELAY);
}
/* Main Function */
/* Main Function */
int main(void) {
	HAL_Init();
	SystemClock_Config();
	MX_GPIO_Init();
	MX_I2C1_Init();
	MX_USART1_UART_Init();
	MX_USART2_UART_Init();

	// Initialize LCD and Keypad
	HD44780_Init(2);
	HD44780_Clear();
	Keypad_Init();

	HD44780_SetCursor(0,0);
	HD44780_PrintStr("System Ready");
	HAL_Delay(2000);

	printf("\r\nSecure Text System v2.1\r\n");
	printf("Waiting for data...\r\n");

	uint8_t *decrypted_data = NULL;
	uint32_t received_timestamp = 0;
	uint32_t received_data_size = 0;

	while(1) {
		uint8_t startMarker = 0;

		// Wait for start marker
		printf("Waiting for start marker (0xAA)...\r\n");
		do {
			if(HAL_UART_Receive(&huart1, &startMarker, 1, 100) == HAL_OK) {
				if(startMarker == 0xAA) {
					printf("Start marker received!\r\n");
					break;
				}
			}
		} while(1);

		HAL_Delay(50);

		// Receive and store all data first
		HAL_StatusTypeDef status;

		// Get access key
		printf("Receiving access key...\r\n");
		memset(receivedAccessKey, 0, sizeof(receivedAccessKey));
		status = UART_Receive_Safe(&huart1, receivedAccessKey, ACCESS_KEY_SIZE, 1000);
		if(status != HAL_OK) {
			printf("Failed to receive access key\r\n");
			continue;
		}
		receivedAccessKey[ACCESS_KEY_SIZE] = '\0';
		printf("Access key received: %s\r\n", receivedAccessKey);

		// Get timestamp
		status = HAL_UART_Receive(&huart1, (uint8_t*)&received_timestamp, sizeof(received_timestamp), 1000);
		if(status != HAL_OK) {
			printf("Note: Timestamp reception skipped\r\n");
			received_timestamp = HAL_GetTick(); // Use current time as timestamp
		}

		// Get data size
		status = HAL_UART_Receive(&huart1, (uint8_t*)&received_data_size, sizeof(received_data_size), 1000);
		if(status != HAL_OK || received_data_size == 0 || received_data_size > MAX_DATA_SIZE) {
			printf("Invalid data size\r\n");
			continue;
		}

		// Allocate memory for encrypted data
		decrypted_data = malloc(received_data_size + 1);
		if(decrypted_data == NULL) {
			printf("Memory allocation failed\r\n");
			continue;
		}

		// Receive encrypted data
		size_t received = 0;
		while(received < received_data_size) {
			uint16_t chunk_size = (received_data_size - received > 32) ? 32 : received_data_size - received;
			status = HAL_UART_Receive(&huart1, &decrypted_data[received], chunk_size, 1000);
			if(status != HAL_OK) break;
			received += chunk_size;
		}

		// Get end marker
		uint8_t endMarker;
		status = HAL_UART_Receive(&huart1, &endMarker, 1, 1000);
		if(status != HAL_OK || endMarker != 0x55) {
			printf("Invalid end marker\r\n");
			free(decrypted_data);
			continue;
		}

		// Now prompt for keypad input
		HD44780_Clear();
		HD44780_SetCursor(0,0);
		HD44780_PrintStr("Enter Access Key:");
		HD44780_SetCursor(0,1);
		memset(&keypadState, 0, sizeof(KeypadInput));

		bool key_verified = false;
		while(!key_verified) {
			if(ProcessKeypadInput()) {
				key_verified = true;

				// Decrypt data
				uint8_t decryption_key[KEY_SIZE];
				deriveKeyFromAccessKey(receivedAccessKey, received_timestamp, decryption_key);
				decryptData(decrypted_data, received_data_size, decryption_key);
				decrypted_data[received_data_size] = '\0';

				// Display decrypted text
				printf("\r\n=== Decrypted Text ===\r\n%s\r\n===================\r\n",
						(char*)decrypted_data);

				// Show on LCD
				displayTextOnLCD((char*)decrypted_data, received_data_size);

				// Clean up
				free(decrypted_data);
				decrypted_data = NULL;
				break;
			}
			HAL_Delay(10);
		}

		// Ready for next message
		HD44780_Clear();
		HD44780_SetCursor(0,0);
		HD44780_PrintStr("Ready for");
		HD44780_SetCursor(0,1);
		HD44780_PrintStr("next message");
		printf("\r\nReady for next transmission\r\n");
	}
}

/* Peripheral Initialization Functions */
static void MX_USART1_UART_Init(void) {
	huart1.Instance = USART1;
	huart1.Init.BaudRate = 115200;
	huart1.Init.WordLength = UART_WORDLENGTH_8B;
	huart1.Init.StopBits = UART_STOPBITS_1;
	huart1.Init.Parity = UART_PARITY_NONE;
	huart1.Init.Mode = UART_MODE_RX;
	huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart1.Init.OverSampling = UART_OVERSAMPLING_16;

	if (HAL_UART_Init(&huart1) != HAL_OK) {
		Error_Handler();
	}
}

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

static void MX_I2C1_Init(void) {
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

static void MX_GPIO_Init(void) {
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	/* Enable GPIO Ports Clock */
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	/* Configure UART pins */
	GPIO_InitStruct.Pin = GPIO_PIN_10;  // PA10 is RX for UART1
	GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
	GPIO_InitStruct.Pull = GPIO_PULLUP;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
	GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
	HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

	GPIO_InitStruct.Pin = GPIO_PIN_2|GPIO_PIN_3;  // PA2/PA3 for UART2
	GPIO_InitStruct.Alternate = GPIO_AF7_USART2;
	HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

	/* Configure I2C pins */
	GPIO_InitStruct.Pin = GPIO_PIN_8|GPIO_PIN_9;  // PB8/PB9 for I2C1
	GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
	GPIO_InitStruct.Pull = GPIO_NOPULL;
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
	HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

void SystemClock_Config(void) {
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
	RCC_OscInitStruct.PLL.PLLQ = 7;

	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
		Error_Handler();
	}

	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
			|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
		Error_Handler();
	}
}

/* Error Handler */
void Error_Handler(void) {
	while(1) {
		HAL_Delay(100);
	}
}

/* Printf Output via UART2 */
int _write(int file, char *ptr, int len) {
	HAL_UART_Transmit(&huart2, (uint8_t*)ptr, len, HAL_MAX_DELAY);
	return len;
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {
	printf("Wrong parameters value: file %s on line %lu\r\n", file, (unsigned long)line);
}
#endif /* USE_FULL_ASSERT */
