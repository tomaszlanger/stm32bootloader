/**
 * @file    bootloader.c
 */

#include <stdlib.h>
#include <string.h>
#include "bootloader.h"
#include "flash.h"

#define	False			0
#define	True			1

#define SEED_OFFSET	96U

#define	RX_TIMEOUT_MS		100
#define RX_BUFFER_SIZE 280U

#define CRC_LENGTH 2U
#define COMMAND_LENGHT	1U
#define COMMAND_ADDRESS_LENGTH	4U
#define COMMAND_LENGTH_LENGHT 1U
#define EXTERASE_MODE_LENGTH	2U
#define EXTERASE_PAGE_LENGTH	2U
#define ENCRYPTED_RANDOM_VALUE_LENGTH	8U

#define EXTERASE_MODE_GLOBAL	0xFFFF
#define EXTERASE_MODE_BANK1	0xFFFE
#define EXTERASE_MODE_BANK2	0xFFFD

#define CALCULATE_CRC_FLAG 0x01
#define INIT_CRC_FLAG 0x02

#define CRC_INIT_VALUE 0x0000

#define	ACK	0x79
#define	NACK 0x1F

#define READ_BOOTLOADER_COMMAND	0x11
#define WRITE_BOOTLOADER_COMMAND 0x31
#define READ_UNPROTECT_BOOTLOADER_COMMAND	0x92
#define WRITE_UNPROTECT_BOOTLOADER_COMMAND	0x73
#define GO_BOOTLOADER_COMMAND	0x21
#define GETID_BOOTLOADER_COMMAND	0x02
#define GET_BOOTLOADER_COMMAND	0x00
#define EXTERASE_BOOTLOADER_COMMAND	0x44
#define ERASE_BOOTLOADER_COMMAND 0x43
#define RANDOMIZE_BOOTLOADER_COMMAND 0x93
#define AUTHORIZE_HOST_COMMAND 0xFE

typedef enum {
	WAIT_FOR_COMMAND_PROTOCOL_STATE = 0,
	WRITE_COMMAND_RECEIVE_ADDRESS_PROTOCOL_STATE = 2,
	WRITE_COMMAND_RECEIVE_LENGTH_PROTOCOL_STATE = 3,
	WRITE_COMMAND_RECEIVE_DATA_PROTOCOL_STATE = 4,
	READ_COMMAND_RECEIVE_ADDRESS_PROTOCOL_STATE = 6,
	READ_COMMAND_RECEIVE_LENGTH_PROTOCOL_STATE = 7,
	ERASE_COMMAND_RECEIVE_MODE_PROTOCOL_STATE = 8,
	ERASE_COMMAND_RECEIVE_PAGES_NUMBERS = 9,
	ERASE_COMMAND_RECEIVE_CRC = 10,
	GO_COMMAND_RECEIVE_ADDRESS_PROTOCOL_STATE = 11,
	RANDOMIZE_COMMAND_RECEIVE_ENCRYPTED_RANDOM_VALUE_STATE = 12
} PROTOCOL_STATE;

typedef enum {
	NO_RECEIVED_DATA_TYPE = 0,
	VALID_RECEIVED_DATA_TYPE = 1,
	INVALID_RECEIVED_DATA_TYPE = 2,
	TIMEOUT_RECEIVED_DATA_TYPE = 3
}	RECEIVED_DATA_TYPE;

typedef enum {
	INIT_RECEIVE_STATE = 0,
	WAIT_FOR_DATA_RECEIVE_STATE = 1
} RECEIVE_STATE;

static uint8_t hostAuthorized = False;

static volatile uint32_t *bootloaderTag = (volatile uint32_t *)((SRAM_BASE | SRAM_SIZE_MAX) - sizeof(uint32_t));	
static volatile uint32_t *validApplicationTag;

static const int16_t cryptoKeyBmsN = 2651;
static const int16_t cryptoKeyBmsE = 7;
//static const int16_t cryptoKeyBmsD = 2743;

static const int16_t cryptoKeyHostN = 1243;
//static const int16_t cryptoKeyHostE = 3;
static const int16_t cryptoKeyHostD = 1867;

static volatile uint32_t randomValue = 0;
static volatile uint32_t receivedRandomValue = 0;

static uint8_t rxCharacter;
static  uint16_t rxPointer;
static uint8_t rxDataReady = False;
static volatile uint8_t txDataInProgress = False;

static uint8_t transmitBuffer[RX_BUFFER_SIZE];
static uint16_t expectedDataLength;

static volatile uint16_t rxFrameTimeout = 0;
static volatile uint32_t bootloaderExitTimeout = BOOTLOADER_TIMEOUT_MS;
static volatile uint32_t randomValueSeed = 0;

static uint16_t receivedDataCRC = CRC_INIT_VALUE;

static PROTOCOL_STATE protocolState = WAIT_FOR_COMMAND_PROTOCOL_STATE;
static uint16_t segmentDataLength = 0;
static uint32_t memoryAddress = 0;
static uint16_t externalEraseMode = 0;
static uint16_t externalErasePageNumber = 0;

static UART_HandleTypeDef* uartHandle;
static DISABLE_HARDWARE_FUNC_PTR disableHardwareCallback = NULL;

static void encrypt(uint8_t* source, uint8_t* destination, uint8_t length, int16_t n, int16_t e) {
	int32_t pt, ct, k;
	int32_t i = 0, j = 0;
	while (i != length) {
		pt = source[i];
		pt = pt - SEED_OFFSET;
		k = 1;
		for (j = 0; j < e; j++) {
			k = k * pt;
			k = k % n;
		}
		ct = k + SEED_OFFSET;		
		memcpy((uint8_t*)(destination + i*sizeof(int16_t)), (uint8_t*)(&ct), sizeof(int16_t));
		i++;
	}	
}

static void decrypt(uint8_t* source, uint8_t* destination, uint8_t length, int16_t n, int16_t d) {
	int32_t pt, ct, k;
	int32_t i = 0, j = 0;	
	while (i != length) {
		ct = *(int16_t*)(source + i*sizeof(int16_t)) - SEED_OFFSET;		
		k = 1;
		for (j = 0; j < d; j++) {
			k = k * ct;
			k = k % n;
		}
		pt = k + SEED_OFFSET;
		destination[i] = pt;
		i++;
	}	
}

static uint8_t executeCommand(uint8_t commandCode, uint8_t *data, uint32_t startAddress, uint32_t dataLength) {
	uint8_t result = False;
	uint16_t eraseMode = 0;
	
	switch (commandCode) {
		case GET_BOOTLOADER_COMMAND:
			data[0] = 8;	// Nubmer of following bytes-1
			data[1] = BOOTLOADER_VERSION;
			data[2] = READ_BOOTLOADER_COMMAND;
			data[3] = WRITE_BOOTLOADER_COMMAND;
			data[4] = READ_UNPROTECT_BOOTLOADER_COMMAND;
			data[5] = WRITE_UNPROTECT_BOOTLOADER_COMMAND;
			data[6] = GO_BOOTLOADER_COMMAND;
			data[7] = GETID_BOOTLOADER_COMMAND;
			data[8] = GET_BOOTLOADER_COMMAND;
			data[9] = EXTERASE_BOOTLOADER_COMMAND;			
			result = True;
			break;	
		case GETID_BOOTLOADER_COMMAND:
			data[0] = 1;	// Nubmer of following bytes-1
			data[1] = DEVICE_ID >> 8;
			data[2] = DEVICE_ID & 0x00FF;
			result = True;
			break;
		case EXTERASE_BOOTLOADER_COMMAND:
			if (hostAuthorized == True) {	
				eraseMode = (uint16_t)(data[0] << 8) + data[1];
				// only global erase from defined address is supported
				if (eraseMode == 0xFFFF) {
					if (flash_erase(FLASH_APP_START_ADDRESS) == FLASH_STATUS_OK) {			
						result = True;
					}
				}	
			}			
			break;
		case WRITE_BOOTLOADER_COMMAND:
			if (hostAuthorized == True) {			
				// Clear last page of flash to clear valid application code flag
				if (startAddress == UINT32_MAX - 1) {				
					if (flash_erase(FLASH_END - FLASH_PAGE_SIZE) == FLASH_STATUS_OK) {			
						result = True;
					}			
				} else {
					// special write command to set valid application code flag
					if (startAddress == UINT32_MAX) {
						startAddress = FLASH_APP_END_ADDRESS - sizeof(uint32_t) + 1;
					}
					// regular write
					if (flash_write(startAddress, (uint32_t*)(data), dataLength/4) == FLASH_STATUS_OK) {
						result = True;
					}
				}	
			}				
			break;
		case READ_BOOTLOADER_COMMAND:
			if (hostAuthorized == True) {
				flash_read(startAddress, data, dataLength); 
				result = True;
			}
			break;
		case GO_BOOTLOADER_COMMAND:			
			if (*validApplicationTag == VALID_FIRMWARE_VALUE) {
				*bootloaderTag = 0;	
				disableHardwareCallback();
				flash_jump_to_app();
				result = True;	
			}
			break;
		case RANDOMIZE_BOOTLOADER_COMMAND:			
			srand((uint16_t)(startAddress));
			randomValue = rand() + (((uint32_t)rand()) << 16) + 1;			
			encrypt((uint8_t*)(&randomValue), data, 4, cryptoKeyBmsN, cryptoKeyBmsE);
			result = True;
			break;	
		case AUTHORIZE_HOST_COMMAND:						
			decrypt(data, (uint8_t*)(&receivedRandomValue), 4, cryptoKeyHostN, cryptoKeyHostD);		
			if (receivedRandomValue == randomValue)	{			
				hostAuthorized = True;
				result = True;
			}		else {
				result = False;
			}
			
			break;			
	}
	return result;
}

static uint16_t crcCalculation(uint16_t crc, uint8_t byte) {
	uint16_t i = 0;
	crc = crc ^ byte;
	for(i = 0; i < 8; i++) {
		if (crc & 0x01) crc = (crc >> 1) ^ 0xA001;
		else crc = (crc >> 1);
	}
	return  crc;
}

static void sendData(uint8_t* data, uint16_t lenght) {
	txDataInProgress = True;
	TRANSMIT_DRIVER_ON;
	HAL_UART_Transmit_IT(uartHandle, data, lenght);	
	while (txDataInProgress) {};
}

static void sendConfirmation(uint8_t data) {
	sendData(&data, 1);
}

static uint32_t memoryAddressFromTransmitBuffer(void) {
	return ((transmitBuffer[0] << 24) + (transmitBuffer[1] << 16) + (transmitBuffer[2] << 8) + (transmitBuffer[3]));
}
	
static RECEIVED_DATA_TYPE receiveData(uint16_t receivedDataLenght, uint8_t flags) {
	uint16_t i = 0;
	RECEIVED_DATA_TYPE dataReceived = NO_RECEIVED_DATA_TYPE;	
	static uint8_t receiveState = INIT_RECEIVE_STATE;	

	switch (receiveState) {
		case INIT_RECEIVE_STATE:
			expectedDataLength = receivedDataLenght;
			rxFrameTimeout = RX_TIMEOUT_MS;
			receiveState = WAIT_FOR_DATA_RECEIVE_STATE;
			rxDataReady = False;
			HAL_UART_Receive_IT(uartHandle, &rxCharacter, 1);
			break;
		case WAIT_FOR_DATA_RECEIVE_STATE:
			if (rxDataReady == True) {
				rxDataReady	= False;				
				dataReceived = VALID_RECEIVED_DATA_TYPE;
				if (flags & INIT_CRC_FLAG) {
					receivedDataCRC = CRC_INIT_VALUE;
				}
				if (flags & CALCULATE_CRC_FLAG) {
					uint16_t receivedCRC = ((uint16_t)(transmitBuffer[expectedDataLength - 2]) << 8) + transmitBuffer[expectedDataLength - 1];
					for (i=0; i<(expectedDataLength - CRC_LENGTH); i++) {
						receivedDataCRC = crcCalculation(receivedDataCRC, transmitBuffer[i]);
					}								
					if (receivedDataCRC != receivedCRC) {
						dataReceived = INVALID_RECEIVED_DATA_TYPE;
					}
				} else {				
						for (i=0; i<expectedDataLength; i++) {
							receivedDataCRC = crcCalculation(receivedDataCRC, transmitBuffer[i]);
						}
				}
				receiveState = INIT_RECEIVE_STATE;
				rxPointer = 0;			
			} else if (!rxFrameTimeout) {
				dataReceived = TIMEOUT_RECEIVED_DATA_TYPE;
				receiveState = INIT_RECEIVE_STATE;
				rxPointer = 0;
			}
			break;
		default:
			break;
	}	
	return dataReceived;
}

static void handleProtocolError(RECEIVED_DATA_TYPE receivedDataType, uint8_t sendNack) {
	switch (receivedDataType) {
		case INVALID_RECEIVED_DATA_TYPE:
			if (sendNack) {
				protocolState = WAIT_FOR_COMMAND_PROTOCOL_STATE;
				transmitBuffer[0] = NACK;				
				sendData(transmitBuffer, 1);				
			}
			break;
		case TIMEOUT_RECEIVED_DATA_TYPE:
			protocolState = WAIT_FOR_COMMAND_PROTOCOL_STATE;
			break;
		default:
			break;
	} 
}

void bootloaderInit(UART_HandleTypeDef *huart, DISABLE_HARDWARE_FUNC_PTR disableHardwareCallbackFunction) {
	uartHandle = huart;	
	disableHardwareCallback = disableHardwareCallbackFunction;
	validApplicationTag = (uint32_t*)(FLASH_APP_END_ADDRESS - sizeof(uint32_t) + 1);
	if (((*bootloaderTag) != JUMP_FROM_APPLICATION_TAG_VALUE) && (*validApplicationTag == VALID_FIRMWARE_VALUE)) {
		*bootloaderTag = 0;
		disableHardwareCallback();
		flash_jump_to_app();
	}
}

void bootloaderHandler(void) {
	RECEIVED_DATA_TYPE receiveResult = NO_RECEIVED_DATA_TYPE;
	uint8_t confirmError = True;
	uint8_t acknowledge = NACK;
	switch (protocolState) {
		case WAIT_FOR_COMMAND_PROTOCOL_STATE:
			receiveResult = receiveData(COMMAND_LENGHT + CRC_LENGTH, INIT_CRC_FLAG | CALCULATE_CRC_FLAG);
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {
				switch (transmitBuffer[0]) {
					case WRITE_BOOTLOADER_COMMAND:
						sendConfirmation(ACK);
						protocolState = WRITE_COMMAND_RECEIVE_ADDRESS_PROTOCOL_STATE;
						break;
					case READ_BOOTLOADER_COMMAND:
						sendConfirmation(ACK);
						protocolState = READ_COMMAND_RECEIVE_ADDRESS_PROTOCOL_STATE;					
						break;
					case GETID_BOOTLOADER_COMMAND:
						sendConfirmation(ACK);
						executeCommand(GETID_BOOTLOADER_COMMAND, transmitBuffer, NULL, NULL);
						sendData(transmitBuffer, 3);
						break;
					case GET_BOOTLOADER_COMMAND:
						sendConfirmation(ACK);
						executeCommand(GET_BOOTLOADER_COMMAND, transmitBuffer, NULL, NULL);
						sendData(transmitBuffer, 10);
						break;
					case EXTERASE_BOOTLOADER_COMMAND:
						sendConfirmation(ACK);
						protocolState = ERASE_COMMAND_RECEIVE_MODE_PROTOCOL_STATE;
						break;
					case GO_BOOTLOADER_COMMAND:
						sendConfirmation(ACK);
						protocolState = GO_COMMAND_RECEIVE_ADDRESS_PROTOCOL_STATE;					
						break;
					case RANDOMIZE_BOOTLOADER_COMMAND:
						sendConfirmation(ACK);
						executeCommand(RANDOMIZE_BOOTLOADER_COMMAND, transmitBuffer, randomValueSeed, NULL);
						sendData(transmitBuffer, ENCRYPTED_RANDOM_VALUE_LENGTH);	
						protocolState = RANDOMIZE_COMMAND_RECEIVE_ENCRYPTED_RANDOM_VALUE_STATE;
						break;						
					default:
						sendConfirmation(NACK);
						break;
				}
			}
			break;
		case WRITE_COMMAND_RECEIVE_ADDRESS_PROTOCOL_STATE:			
			receiveResult = receiveData(COMMAND_ADDRESS_LENGTH + CRC_LENGTH, INIT_CRC_FLAG | CALCULATE_CRC_FLAG);
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {
				memoryAddress = memoryAddressFromTransmitBuffer();
				protocolState = WRITE_COMMAND_RECEIVE_LENGTH_PROTOCOL_STATE;
				sendConfirmation(ACK);				
			}
			break;	
		case WRITE_COMMAND_RECEIVE_LENGTH_PROTOCOL_STATE:
			receiveResult = receiveData(COMMAND_LENGTH_LENGHT, INIT_CRC_FLAG);
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {
				segmentDataLength = transmitBuffer[0] + 1;
				protocolState = WRITE_COMMAND_RECEIVE_DATA_PROTOCOL_STATE;
			}
			confirmError = False;
			break;
		case WRITE_COMMAND_RECEIVE_DATA_PROTOCOL_STATE:
			receiveResult = receiveData(segmentDataLength + CRC_LENGTH, CALCULATE_CRC_FLAG);		
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {
				executeCommand(WRITE_BOOTLOADER_COMMAND, transmitBuffer, memoryAddress, segmentDataLength);
				sendConfirmation(ACK);
				protocolState = WAIT_FOR_COMMAND_PROTOCOL_STATE;													
			}
			break;		
		case READ_COMMAND_RECEIVE_ADDRESS_PROTOCOL_STATE:
			receiveResult = receiveData(COMMAND_ADDRESS_LENGTH + CRC_LENGTH, INIT_CRC_FLAG | CALCULATE_CRC_FLAG);
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {
				memoryAddress = memoryAddressFromTransmitBuffer();
				sendConfirmation(ACK);
				protocolState = READ_COMMAND_RECEIVE_LENGTH_PROTOCOL_STATE;									
			}
			break;
		case READ_COMMAND_RECEIVE_LENGTH_PROTOCOL_STATE:
			receiveResult = receiveData(COMMAND_LENGTH_LENGHT + CRC_LENGTH, INIT_CRC_FLAG | CALCULATE_CRC_FLAG);
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {
				segmentDataLength = transmitBuffer[0] + 1;
				executeCommand(READ_BOOTLOADER_COMMAND, transmitBuffer, memoryAddress, segmentDataLength);
				sendConfirmation(ACK);
				sendData(transmitBuffer, segmentDataLength);
				protocolState = WAIT_FOR_COMMAND_PROTOCOL_STATE;
			}
			confirmError = False;			
			break;
		case ERASE_COMMAND_RECEIVE_MODE_PROTOCOL_STATE:	
			receiveResult = receiveData(EXTERASE_MODE_LENGTH, INIT_CRC_FLAG);		
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {
				externalEraseMode = (uint16_t)(transmitBuffer[0] << 8) + transmitBuffer[1];
				if (externalEraseMode < 0xFFF0) {
					protocolState = ERASE_COMMAND_RECEIVE_PAGES_NUMBERS;
				} else {
					protocolState = ERASE_COMMAND_RECEIVE_CRC;
				}
			}
			confirmError = False;
			break;
		case ERASE_COMMAND_RECEIVE_PAGES_NUMBERS:
			receiveResult = receiveData(EXTERASE_PAGE_LENGTH, NULL);		
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {
				externalErasePageNumber = (uint16_t)(transmitBuffer[0] << 8) + transmitBuffer[1];
				protocolState = ERASE_COMMAND_RECEIVE_CRC;
			}
			confirmError = False;
			break;
		case ERASE_COMMAND_RECEIVE_CRC:
			receiveResult = receiveData(CRC_LENGTH, CALCULATE_CRC_FLAG);
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {				
					transmitBuffer[0] = (uint8_t)(externalEraseMode >> 8);
					transmitBuffer[1] = (uint8_t)(externalEraseMode & 0x00FF);			
					transmitBuffer[2] = (uint8_t)(externalErasePageNumber >> 8);
					transmitBuffer[3] = (uint8_t)(externalErasePageNumber & 0x00FF);
					if (executeCommand(EXTERASE_BOOTLOADER_COMMAND, transmitBuffer, NULL, NULL)) {
						acknowledge = ACK;
					}
					sendConfirmation(acknowledge);					
					protocolState = WAIT_FOR_COMMAND_PROTOCOL_STATE;					
			}
			confirmError = False;			
			break;
		case GO_COMMAND_RECEIVE_ADDRESS_PROTOCOL_STATE:
			receiveResult = receiveData(COMMAND_ADDRESS_LENGTH + CRC_LENGTH, INIT_CRC_FLAG | CALCULATE_CRC_FLAG);
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {			
				if (*validApplicationTag == VALID_FIRMWARE_VALUE) {
					acknowledge = ACK;
				}
				memoryAddress = memoryAddressFromTransmitBuffer();
				sendConfirmation(acknowledge);								
				executeCommand(GO_BOOTLOADER_COMMAND, transmitBuffer, memoryAddress, NULL);
				protocolState = WAIT_FOR_COMMAND_PROTOCOL_STATE;											
			}
			break;		
		case RANDOMIZE_COMMAND_RECEIVE_ENCRYPTED_RANDOM_VALUE_STATE:
			receiveResult = receiveData(ENCRYPTED_RANDOM_VALUE_LENGTH + CRC_LENGTH, INIT_CRC_FLAG | CALCULATE_CRC_FLAG);
			if (receiveResult == VALID_RECEIVED_DATA_TYPE) {	
					if (executeCommand(AUTHORIZE_HOST_COMMAND, transmitBuffer, NULL, ENCRYPTED_RANDOM_VALUE_LENGTH)) {
						acknowledge = ACK;
					}					
					sendConfirmation(acknowledge);								
					protocolState = WAIT_FOR_COMMAND_PROTOCOL_STATE;											
				}	
			break;
		default:
			break;
	}	
	if (receiveResult != VALID_RECEIVED_DATA_TYPE) {
		handleProtocolError(receiveResult, confirmError);
	}	
	if (!bootloaderExitTimeout) {
		executeCommand(GO_BOOTLOADER_COMMAND, transmitBuffer, memoryAddress, NULL);
	}
}

void bootloaderTimer(void) {
	if (rxFrameTimeout > 0) rxFrameTimeout--;
	if (bootloaderExitTimeout > 0) bootloaderExitTimeout--;
	randomValueSeed++;
}

void bootloaderRxCompletedCallback(void) {
	transmitBuffer[rxPointer] =	rxCharacter;
	if (++rxPointer > RX_BUFFER_SIZE)  {
		rxPointer = 0;
	}		
	if (rxPointer < expectedDataLength) {
		HAL_UART_Receive_IT(uartHandle, &rxCharacter, 1);
	} else {
		rxDataReady = True;
	}
	bootloaderExitTimeout = BOOTLOADER_TIMEOUT_MS;
	rxFrameTimeout = RX_TIMEOUT_MS;	
}

void bootloaderTxCompletedCallback(void) {	
	TRANSMIT_DRIVER_OFF;
	txDataInProgress = False;
}
