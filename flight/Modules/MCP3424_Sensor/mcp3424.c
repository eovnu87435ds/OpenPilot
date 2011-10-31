/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{ 
 * @addtogroup ET_EGT_Sensor EagleTree EGT Sensor Module
 * @brief Read ET EGT temperature sensors @ref ETEGTSensor "ETEGTSensor UAV Object"
 * @{ 
 *
 * @file       et_egt_sensor.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Reads dual thermocouple temperature sensors via EagleTree EGT expander
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/**
 * Output object: mcp3424sensor
 *
 * This module will periodically update the value of the mcp3424sensor UAVobject.
 *
 */

#include "openpilot.h"
#include "mcp3424.h"
#include "mcp3424sensor.h"	// UAVobject that will be updated by the module
#include "mcp3424settings.h" // UAVobject used to modify module settings

// Private constants
#define STACK_SIZE_BYTES 600
#define TASK_PRIORITY (tskIDLE_PRIORITY+1)
#define UPDATE_PERIOD 500
static double_t vPerC = 0.0000403; //volts per celcius for K-type thermocouple
static double_t MCP3424_REFVOLTAGE = 2.048; //internal reference voltage for MCP3424 IC

// Private types
#define MCP9804_I2C_ADDRESS 0x1F //Cold junction temperature sensor
#define MCP3424_I2C_ADDRESS 0x68 //Four channel ADC sensor

// Private variables
static xTaskHandle taskHandle;

// down sampling variables
#define MCP3424_ds_size    4
static int32_t MCP3424_ds_temp1 = 0;
static int32_t MCP3424_ds_temp2 = 0;
static int MCP3424_ds_count = 0;

// Private functions
static void MCP3424Task(void *parameters);

/**
* Start the module, called on startup
*/
int32_t MCP3424Start()
{
	// Start main task
	xTaskCreate(MCP3424Task, (signed char *)"MCP3424", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &taskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_MCP3424, taskHandle);
	return 0;
}

/**
* Initialise the module, called on startup
*/
int32_t MCP3424Initialize()
{
	MCP3424sensorInitialize(); //Initialise the UAVObject used for transferring data to GCS
	MCP3424SettingsInitialize();

	// init down-sampling data
	MCP3424_ds_temp1 = 0;
	MCP3424_ds_temp2 = 0;
	MCP3424_ds_count = 0;

	return 0;
}

MODULE_INITCALL(MCP3424Initialize, MCP3424Start)

/**
* Read the cold junction temperature via I2C
* Returns true if successful and false if not.
*/
static bool Read_Cold_Junction_Temp(double_t* pColdTemp)
{
	uint8_t setToReadAmbientTempRegister = 0x05;
	uint8_t coldBuff[2] = {0};

	const struct pios_i2c_txn txn_list_1[] = {
		{
		 .addr = MCP9804_I2C_ADDRESS,// & 0xFE, //Bit 0 must be 0 to write
		 .rw = PIOS_I2C_TXN_WRITE,
		 .len = 1,
		 .buf = &setToReadAmbientTempRegister, //Read ambient temperature register
		 },
		{
		 .addr = MCP9804_I2C_ADDRESS,// | 0x01, //Bit 0 must be 1 to read
		 .rw = PIOS_I2C_TXN_READ,
		 .len = 2,
		 .buf = coldBuff,
		 },

	};

	if( PIOS_I2C_Transfer(PIOS_I2C_MAIN_ADAPTER, txn_list_1, NELEMENTS(txn_list_1))) {
		//Convert the temperature data
		//First Check flag bits
		if((coldBuff[0] & 0x80) == 0x80) { //TA ≥ TCRIT
		}

		if((coldBuff[0] & 0x40) == 0x40) { //TA > TUPPER
		}

		if((coldBuff[0] & 0x20) == 0x20) { //TA < TLOWER
		}

		coldBuff[0] = coldBuff[0] & 0x1F; //Clear flag bits

		double_t coldMSB = coldBuff[0];
		double_t coldLSB = coldBuff[1];
		if((coldBuff[0] & 0x10) == 0x10) { //TA < 0°C
			coldBuff[0] = coldBuff[0] & 0x0F; //Clear SIGN

			*pColdTemp = 256 - (coldMSB * 16 + coldLSB / 16.0); //2's complement
		}
		else //TA  ≥ 0°C
			*pColdTemp = coldMSB * 16 + coldLSB / 16.0;

		return true;
	}
	else
		return false;
}

static uint8_t getGain(uint8_t x) {
	switch(x) {
	case(0):
		return 1;
		break;
	case(1):
		return 2;
		break;
	case(2):
		return 4;
		break;
	case(3):
		return 8;
		break;
	default:
		return -1;
	}
}

static uint8_t getResolution(uint8_t x) {
	switch(x) {
	case(0):
		return 12;
		break;
	case(1):
		return 14;
		break;
	case(2):
		return 16;
		break;
	case(3):
		return 18;
		break;
	default:
		return -1;
	}
}

static bool MCP3424SetConfig(uint8_t channel, uint8_t* dataNumBytes, uint8_t* pResolution, uint8_t* pGain, uint8_t* pConfigByte) {

	uint8_t gain = 0;
	uint8_t resolution = 0;
	uint8_t PGAgain = 0;
	uint8_t sampleRate = 0;

	uint8_t conversionModeBit = 0; //1=Continuous, 0=One Shot
	uint8_t channelBits = 0; //0 = Channel 1

	//UAVObject settings data
	MCP3424SettingsData mcp3424settings;

	//get any updated settings
	MCP3424SettingsGet(&mcp3424settings);

	//update config parameters
	switch(channel) {
	case(1):
		gain = getGain(mcp3424settings.Channel1Gain);
		resolution = getResolution(mcp3424settings.Channel1Resolution);
		break;
	case(2):
		gain = getGain(mcp3424settings.Channel2Gain);
		resolution = getResolution(mcp3424settings.Channel2Resolution);
		break;
	case(3):
		gain = getGain(mcp3424settings.Channel3Gain);
		resolution = getResolution(mcp3424settings.Channel3Resolution);
		break;
	case(4):
		gain = getGain(mcp3424settings.Channel4Gain);
		resolution = getResolution(mcp3424settings.Channel4Resolution);
		break;
	default:
		gain = -1;
		resolution = -1;
	}

	*pResolution = resolution;
	*pGain = gain;

	channelBits = channel - 1; //zero based

	switch(gain) {
	case('8'):
		PGAgain = 0x03;
		break;
	case('4'):
		PGAgain = 0x02;
		break;
	case('2'):
		PGAgain = 0x01;
		break;
	case('1'):
		PGAgain = 0x00;
		break;
	default:
		PGAgain = 0x00;
	}

	switch(resolution) {
	case(18):
		sampleRate = 0x03; //3.75 sps (18 bits), 3 bytes of data
		break;
	case(16):
		sampleRate = 0x02; //2 bytes of data,
		break;
	case(14):
		sampleRate = 0x01; //2 bytes of data
		break;
	case(12):
		sampleRate = 0x00; //240 SPS (12 bits), 2 bytes of data
		break;
	default:
		sampleRate = 0x00;
	}

	uint8_t config = PGAgain;
	config = config | (sampleRate << 2);
	config = config | (conversionModeBit << 4);
	config = config | (channelBits << 5);
	config = config | (1 << 7); //write a 1 here to initiate a new conversion in One-shot mode
	*pConfigByte = config;

	//the resolution setting affects the number of data bytes returned during a read
	if(resolution == 18)
		*dataNumBytes = 3;
	else
		*dataNumBytes = 2;

	//Set mcp3424 config register via i2c
	const struct pios_i2c_txn txn_list_1[] = {
		{
		 .addr = MCP3424_I2C_ADDRESS,
		 .rw = PIOS_I2C_TXN_WRITE,
		 .len = 1,
		 .buf = &config,
		 },
	};

	return PIOS_I2C_Transfer(PIOS_I2C_MAIN_ADAPTER, txn_list_1, NELEMENTS(txn_list_1));
}

static void decipherI2Cresponse(uint8_t* pBufferTemp, uint8_t* pBuffer, uint8_t* pNumDataBytes, uint8_t* pResolution, uint32_t* pCounts){
	int8_t sign = 0; //+ve

	if((*pNumDataBytes) == 3) {
		memcpy(pBuffer, pBufferTemp, 4);

		pBuffer[0] = pBuffer[0] & 0x01; //remove the repeated MSB and sign bit
	}
	else { //numDataBytes == 2.
		pBuffer[0] = 0;              //set upper data byte to zero
		pBuffer[1] = pBufferTemp[0];  //middle data byte
		pBuffer[2] = pBufferTemp[1];  //lower data byte
		pBuffer[3] = pBufferTemp[2];  //config byte

		if((*pResolution) == 14)
			pBuffer[1] = pBuffer[1] & 0x1F; //remove the repeated MSB and sign bit
		else if(*pResolution == 12)
			pBuffer[1] = pBuffer[1] & 0x07; //remove the repeated MSB and sign bit
	}

	sign = pBufferTemp[0];

	*pCounts = ((pBuffer[0] << 16) | (pBuffer[1] << 8) | pBuffer[2]);

	//Convert to 2's complement
	if(sign < 0) {
		int32_t largestNumber = ((1 << (*pResolution - 1)) - 1);
		*pCounts = *pCounts - largestNumber;
	}

	pBuffer[4] = *pCounts;
	pBuffer[5] = *pResolution;
}

static int32_t GetDelay(uint8_t* pResolution)
{
	double_t d = 0;
	switch(*pResolution) {
	case(12):
		d = 1000/240.0; //240 samples per second
		break;
	case(14):
		d = 1000/60.0;  //60 sps
		break;
	case(16):
		d = 1000/15.0;  //15 sps
		break;
	case(18):
		d = 1000/3.75;  //3.75 sps
		break;
	default:
		d = 500;
	}
	return (int32_t)d + 50; //Add extra 50ms time
}

/*
 * Read CylinderHeadTemp using a K-type thermocouple connected to Channel 1 of the MCP3424 IC
 * on the I2C bus
 */

static bool ReadCylinderHeadTemp(uint8_t* pBuffer, double_t* pTemperature, uint8_t* pConfigByte)
{
	uint8_t channel = 1;
	uint8_t numDataBytes = 3;
	uint8_t bufferTemp[4] = {0};  //buffer to store return data from sensor
	uint8_t resolution, gain = 0;
	uint32_t counts = 0;

	bool success = MCP3424SetConfig(channel, &numDataBytes, &resolution, &gain, pConfigByte);

	if(!success)
		return false;

	//wait long enough for conversion to happen after setting config
	vTaskDelay(GetDelay(&resolution) / portTICK_RATE_MS);

	const struct pios_i2c_txn txn_list_1[] = {
		{
		 .addr = MCP3424_I2C_ADDRESS,
		 .rw = PIOS_I2C_TXN_READ,
		 .len = 4, //Upper, Middle, Lower data bytes and config byte returned for 18 bit mode
		 .buf = bufferTemp,
		 },
	};

	//Read data bytes
	if(PIOS_I2C_Transfer(PIOS_I2C_MAIN_ADAPTER, txn_list_1, NELEMENTS(txn_list_1))) {

		decipherI2Cresponse(bufferTemp, pBuffer, &numDataBytes, &resolution, &counts);

		//Assume K-type thermocouple is connected to channel 1
		//do conversion here
		double_t LSB = 2 * MCP3424_REFVOLTAGE / (1 << resolution);
		*pTemperature = (int32_t)counts * LSB / vPerC / gain;

		return true;
	}
	else
		return false;
}

/*
 * Read Exhaust Gas Temp using a K-type thermocouple connected to Channel 2 of the MCP3424 IC
 * on the I2C bus
 */

static bool ReadExhaustGasTemp(uint8_t* pBuffer, double_t* pTemperature, uint8_t* pConfigByte)
{
	uint8_t channel = 2;
	uint8_t numDataBytes = 3;
	uint8_t bufferTemp[4] = {0};  //buffer to store return data from sensor
	uint8_t resolution, gain = 0;
	uint32_t counts = 0;

	bool success = MCP3424SetConfig(channel, &numDataBytes, &resolution, &gain, pConfigByte);

	if(!success)
		return false;

	//wait long enough for conversion to happen after setting config
	vTaskDelay(GetDelay(&resolution) / portTICK_RATE_MS);

	const struct pios_i2c_txn txn_list_1[] = {
		{
		 .addr = MCP3424_I2C_ADDRESS,
		 .rw = PIOS_I2C_TXN_READ,
		 .len = 4, //Upper, Middle, Lower data bytes and config byte returned for 18 bit mode
		 .buf = bufferTemp,
		 },
	};

	//Read data bytes
	if(PIOS_I2C_Transfer(PIOS_I2C_MAIN_ADAPTER, txn_list_1, NELEMENTS(txn_list_1))) {

		decipherI2Cresponse(bufferTemp, pBuffer, &numDataBytes, &resolution, &counts);

		//Assume K-type thermocouple is connected to channel 1
		//do conversion here
		double_t LSB = 2 * MCP3424_REFVOLTAGE / (1 << resolution);
		*pTemperature = (int32_t)counts * LSB / vPerC / gain;

		return true;
	}
	else
		return false;
}

/*
 * Read the ignition battery voltage via the AttoPilot voltage and current sensor board connected to Channel 3
 * of the MCP3424 IC on the I2C bus
 */
static bool ReadIgnitionBatteryVoltage(uint8_t* pBuffer, double_t* pVoltage, uint8_t* pConfigByte, uint32_t* pRawCounts)
{
	uint8_t channel = 3;
	uint8_t numDataBytes = 3;
	uint8_t bufferTemp[4] = {0};  //buffer to store return data from sensor
	uint8_t resolution, gain = 0;
	uint32_t counts = 0;

	bool success = MCP3424SetConfig(channel, &numDataBytes, &resolution, &gain, pConfigByte);

	if(!success)
		return false;

	//wait long enough for conversion to happen after setting config
	vTaskDelay(GetDelay(&resolution) / portTICK_RATE_MS);

	const struct pios_i2c_txn txn_list_1[] = {
		{
		 .addr = MCP3424_I2C_ADDRESS,
		 .rw = PIOS_I2C_TXN_READ,
		 .len = 4, //Upper, Middle, Lower data bytes and config byte returned for 18 bit mode
		 .buf = bufferTemp,
		 },
	};

	//Read data bytes
	if(PIOS_I2C_Transfer(PIOS_I2C_MAIN_ADAPTER, txn_list_1, NELEMENTS(txn_list_1))) {

		decipherI2Cresponse(bufferTemp, pBuffer, &numDataBytes, &resolution, &counts);

		pBuffer[6] = gain;

		//Assume Attopilot voltage and current sensor is being used.
		// Specifically, the full scale voltage is 51.8V = 3.3V
		// Full scale current is 90A = 3.3V
		double_t attoPilotVscale = 1 / 0.06369; //From data sheet

		double_t LSB = 2 * MCP3424_REFVOLTAGE / (1 << resolution);
		*pVoltage = (int32_t)counts * LSB / gain * attoPilotVscale;

		*pRawCounts = counts;

		return true;
	}
	else
		return false;
}

/*
 * Read the ignition battery current via the AttoPilot voltage and current sensor board connected to Channel 4
 * of the MCP3424 IC on the I2C bus
 */
static bool ReadIgnitionBatteryCurrent(uint8_t* pBuffer, double_t* pCurrent, uint8_t* pConfigByte)
{
	uint8_t channel = 4;
	uint8_t numDataBytes = 3;
	uint8_t bufferTemp[4] = {0};  //buffer to store return data from sensor
	uint8_t resolution, gain = 0;
	uint32_t counts = 0;

	bool success = MCP3424SetConfig(channel, &numDataBytes, &resolution, &gain, pConfigByte);

	if(!success)
		return false;

	//wait long enough for conversion to happen after setting config
	vTaskDelay(GetDelay(&resolution) / portTICK_RATE_MS);

	const struct pios_i2c_txn txn_list_1[] = {
		{
		 .addr = MCP3424_I2C_ADDRESS,
		 .rw = PIOS_I2C_TXN_READ,
		 .len = 4, //Upper, Middle, Lower data bytes and config byte returned for 18 bit mode
		 .buf = bufferTemp,
		 },
	};

	//Read data bytes
	if(PIOS_I2C_Transfer(PIOS_I2C_MAIN_ADAPTER, txn_list_1, NELEMENTS(txn_list_1))) {

		decipherI2Cresponse(bufferTemp, pBuffer, &numDataBytes, &resolution, &counts);

		//Assume 90A Attopilot voltage and current sensor is being used.
		// Full scale current is 90A = 3.3V
		double_t attoPilotIscale = 1 / 0.0366; //From data sheet

		double_t LSB = 2 * MCP3424_REFVOLTAGE / (1 << resolution);
		*pCurrent = (int32_t)counts * LSB / gain * attoPilotIscale;

		return true;
	}
	else
		return false;
}


/**
 * Module thread, should not return.
 * Channel1 = cylinderHeadTemperature
 * Channel2 = exhaustGasTemperature
 * Channel3 = ignitionBatteryVoltage
 * Channel4 = ignitionBatteryAmps
 */
static void MCP3424Task(void *parameters)
{
	bool bParamSet = false;

	uint8_t buf[8] = {0};
	uint8_t configByte = 0;
	uint32_t rawCounts = 0;

	portTickType lastSysTime;

	//UAVObject data structure
	MCP3424sensorData d1;

	double_t cylinderHeadTemp, exhaustGasTemp, batteryVoltage, batteryCurrent = 0;

	//down sample variables
//	double_t chan1_ds_size = 3;
//	double_t chan1_ds_temp = 0;
//	double_t chan1_ds_count = 0;

	double_t coldTemp = 0;

	portTickType energyTimeTickCount = 0; //delta time for calculating battery energy consumption

	bool bMCP3424readSuccess = false;

	// Main task loop
	lastSysTime = xTaskGetTickCount();

	while(1) {

		/*
		 * Read cold junction temp from separate MCP9804 IC via I2C
		 */
		bool b2 = Read_Cold_Junction_Temp(&coldTemp);

		/*
		 * Read channel 1
		 */
		bMCP3424readSuccess = ReadCylinderHeadTemp(buf, &cylinderHeadTemp, &configByte);

		if(bMCP3424readSuccess && b2) {
			PIOS_LED_Off(LED2);

			//down sample
//			chan1_ds_temp += channel1;
//			if (++chan1_ds_count >= chan1_ds_size)
//			{
//				chan1_ds_count = 0;
//
//				// Get average of last 10 samples
//				channel1 = chan1_ds_temp / chan1_ds_size;
//				chan1_ds_temp = 0;
//			}

			cylinderHeadTemp += coldTemp; //thermocouple reads temperature relative to cold junction

			d1.CylinderHeadTemp = cylinderHeadTemp; //calculated temperature
			d1.ColdJunction = coldTemp;
		}
		else {
			PIOS_LED_On(LED2);
			d1.buf0 = 99;
			d1.buf1 = 99;
			d1.buf2 = 99;
			d1.buf4 = 111;

			//configuration may be wrong so set flag to set parameters again
			bParamSet = false;
		}

		/*
		 * Read channel 2
		 */
		//Read thermocouple connected to channel 1 of MCP3424 IC via I2C
		ReadExhaustGasTemp(buf, &exhaustGasTemp, &configByte);
		exhaustGasTemp += coldTemp; //thermocouple reads temperature relative to cold junction
		d1.ExhaustGasTemp = exhaustGasTemp;

		/*
		 * Read channel 3
		 */
		ReadIgnitionBatteryVoltage(buf, &batteryVoltage, &configByte, &rawCounts);
		d1.BatteryVoltage = batteryVoltage;
//		d1.buf0 = buf[0]; //data1
//		d1.buf1 = buf[1]; //data2
//		d1.buf2 = buf[2]; //data3
//		d1.buf3 = buf[3]; //data4
		//d1.CylinderHeadTemp = cylinderHeadTemp; //calculated temperature
		//d1.ColdJunction = coldTemp;
//		d1.buf4 = buf[4]; //raw counts
//		d1.buf5 = buf[5];
//		d1.buf6 = buf[6];
//		d1.buf6 = configByte;
//		d1.buf7 = rawCounts;

		/*
		 * Read channel 4
		 */
		ReadIgnitionBatteryCurrent(buf, &batteryCurrent, &configByte);
		d1.BatteryAmps = batteryCurrent;

		portTickType x = xTaskGetTickCount();
		portTickType deltaT = (x - energyTimeTickCount) * portTICK_RATE_MS; //Conversion to milliseconds
		energyTimeTickCount = x;

		float_t mAh = 0.0;
		MCP3424sensorIgnitionBattery_mAhGet(&mAh); //This allows some other module or the GCS to reset it to zero

		d1.IgnitionBattery_mAh = mAh + (deltaT * batteryCurrent / 3600.0); //Conversion to mAh

		/*
		 * Update UAVObject data
		 */
		MCP3424sensorSet(&d1);

		// Delay until it is time to read the next sample
		vTaskDelayUntil(&lastSysTime, UPDATE_PERIOD / portTICK_RATE_MS);
	}
}



/**
  * @}
 * @}
 */
