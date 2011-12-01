//
//  esc_serial.c
//  OpenPilotOSX
//
//  Created by James Cotton on 12/1/11.
//  Copyright 2011 OpenPilot. All rights reserved.
//

#include <stdio.h>

#include "pios.h"
#include "esc.h"
#include "esc_fsm.h"
#include "esc_settings.h"
#include "fifo_buffer.h"

#define ESC_SYNC_BYTE 0x85

#define MAX_SERIAL_BUFFER_SIZE 128

static int32_t esc_serial_process();

//! The set of commands the ESC understands
enum esc_serial_command {
	ESC_COMMAND_SET_CONFIG = 0x01,
	ESC_COMMAND_GET_CONFIG = 0x02,
	ESC_COMMAND_SAVE_CONFIG = 0x03,
	ESC_COMMAND_ENABLE_SERIAL_LOGGING = 0x04,
	ESC_COMMAND_DISABLE_SERIAL_LOGGING = 0x05,
	ESC_COMMAND_REBOOT_BL = 0x06,
	ESC_COMMAND_ENABLE_SERIAL_CONTROL = 0x07,
	ESC_COMMAND_DISABLE_SERIAL_CONTROL = 0x08,
	ESC_COMMAND_SET_SPEED = 0x09,
	ESC_COMMAND_LAST = 0x10,
};

//! The size of the data packets
uint8_t esc_command_data_size[ESC_COMMAND_LAST] = {
	[ESC_COMMAND_SET_CONFIG] = sizeof(struct esc_config) + 0, // No CRC yet
	[ESC_COMMAND_GET_CONFIG] = 0,
	[ESC_COMMAND_SAVE_CONFIG] = 0,
	[ESC_COMMAND_ENABLE_SERIAL_LOGGING] = 0,
	[ESC_COMMAND_DISABLE_SERIAL_LOGGING] = 0,
	[ESC_COMMAND_REBOOT_BL] = 0,
	[ESC_COMMAND_DISABLE_SERIAL_CONTROL] = 0,
	[ESC_COMMAND_SET_SPEED] = 2,
};

//! States for the ESC parsers
enum esc_serial_parser_state {
	ESC_SERIAL_WAIT_SYNC,
	ESC_SERIAL_WAIT_FOR_COMMAND,
	ESC_SERIAL_WAIT_FOR_DATA,
	ESC_SERIAL_WAIT_FOR_PROCESS
};

static struct esc_serial_state {
	enum esc_serial_parser_state state;
	enum esc_serial_command command;
	uint8_t buffer[MAX_SERIAL_BUFFER_SIZE];
	uint32_t buffer_pointer;
	int32_t crc;
} esc_serial_state;

/**
 * Initialize the esc serial parser
 */
int32_t esc_serial_init()
{
	esc_serial_state.buffer_pointer = 0;
	esc_serial_state.state = ESC_SERIAL_WAIT_SYNC;
	return 0;
}

/**
 * Parse a character from the comms
 * @param[in] c if less than zero thrown out
 * @return 0 if success, -1 if failure
 */
int32_t esc_serial_parse(int32_t c)
{
	if (c < 0 || c > 0xff)
		return -1;
	
	switch (esc_serial_state.state) {
		case ESC_SERIAL_WAIT_SYNC:
			if (c == ESC_SYNC_BYTE) {
				esc_serial_state.state = ESC_SERIAL_WAIT_FOR_COMMAND;
				esc_serial_state.buffer_pointer = 0;
			}
			break;
		case ESC_SERIAL_WAIT_FOR_COMMAND:
			if (c == 0 || c >= ESC_COMMAND_LAST) {
				esc_serial_state.state = ESC_SERIAL_WAIT_SYNC;
				return 0;
			}			
			esc_serial_state.command = c;
			if (esc_command_data_size[esc_serial_state.command] == 0) {
				esc_serial_state.state = ESC_SERIAL_WAIT_FOR_PROCESS;
				return esc_serial_process();
			}
			esc_serial_state.state = ESC_SERIAL_WAIT_FOR_DATA;
			break;
		case ESC_SERIAL_WAIT_FOR_DATA:
			esc_serial_state.buffer[esc_serial_state.buffer_pointer] = c;
			esc_serial_state.buffer_pointer++;
			if (esc_serial_state.buffer_pointer >= esc_command_data_size[esc_serial_state.state]) {
				esc_serial_state.state = ESC_SERIAL_WAIT_FOR_PROCESS;
				return esc_serial_process();
			}
			break;
		default:
			PIOS_DEBUG_Assert(0);
	}
	
	return 0;
}

extern struct esc_config config;
extern uint8_t esc_logging;

/**
 * Process a command once it is parsed
 */
static int32_t esc_serial_process()
{
	int32_t retval = -1;
	
	switch(esc_serial_state.command) {
		case ESC_COMMAND_SET_CONFIG:
			memcpy((uint8_t *) &config, (uint8_t *) esc_serial_state.buffer, sizeof(config));
			break;
		case ESC_COMMAND_GET_CONFIG:
			// TODO: Send esc configuration
			PIOS_COM_SendBuffer(PIOS_COM_DEBUG, (uint8_t *) &config, sizeof(config));
			retval = -1;
			break;
		case ESC_COMMAND_SAVE_CONFIG:
			esc_settings_save(&config);
			retval = 0;
			break;
		case ESC_COMMAND_ENABLE_SERIAL_LOGGING:
			esc_logging = 1;
			retval = 0;
			break;
		case ESC_COMMAND_DISABLE_SERIAL_LOGGING:
			esc_logging = 0;
			retval = 0;
			break;
		case ESC_COMMAND_REBOOT_BL:
			retval = -1;
			break;
		case ESC_COMMAND_ENABLE_SERIAL_CONTROL:
			retval = -1;
			break;
		case ESC_COMMAND_DISABLE_SERIAL_CONTROL:
			retval = -1;
			break;
		case ESC_COMMAND_SET_SPEED:
		{
			int16_t new_speed = esc_serial_state.buffer[0] | (esc_serial_state.buffer[1] << 8);
			if(new_speed >= 0 || new_speed < 10000) {
//				esc_data->speed_setpoint = new_speed;
				retval = 0;
			} else 
				retval = -1;
		}
			break;
		default:
			PIOS_DEBUG_Assert(0);
	}

	esc_serial_init();
	
	return retval;
}