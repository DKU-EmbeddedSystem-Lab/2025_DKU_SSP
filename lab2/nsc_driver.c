//////////////////////////////////////////////////////////////////////////////////
// fmc_driver.c for Cosmos+ OpenSSD
// Copyright (c) 2016 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//				  Kibin Park <kbpark@enc.hanyang.ac.kr>
//				  Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// This file is part of Cosmos+ OpenSSD.
//
// Cosmos+ OpenSSD is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Cosmos+ OpenSSD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Cosmos+ OpenSSD; see the file COPYING.
// If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Company: ENC Lab. <http://enc.hanyang.ac.kr>
// Engineer: Kibin Park <kbpark@enc.hanyang.ac.kr>
//
// Project Name: Cosmos+ OpenSSD
// Design Name: Cosmos+ Firmware
// Module Name: NAND Storage Controller Driver
// File Name: nsc_driver.c
//
// Version: v1.1.0
//
// Description:
//   - low level driver for NAND storage controller
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.1.0
//   - V2FReadPageTransferAsync needs additional input (rowAddress)
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////

#include "nsc_driver.h"
#include "sim_backend.h" /* jy */

unsigned int __attribute__((optimize("O0"))) V2FIsControllerBusy(V2FMCRegisters* dev)
{
	volatile unsigned int channelBusy = *((volatile unsigned int*)&(dev->channelBusy));

	return channelBusy;
}

void __attribute__((optimize("O0"))) V2FResetSync(V2FMCRegisters* dev, int way)
{
	*((volatile unsigned int*)&(dev->waySelection)) = way;
	*((volatile unsigned int*)&(dev->cmdSelect)) = V2FCommand_Reset;
#if 1 //jy
	SchedulingNand();
#endif
	while (V2FIsControllerBusy(dev));
}

void __attribute__((optimize("O0"))) V2FSetFeaturesSync(V2FMCRegisters* dev, int way, unsigned int feature0x02, unsigned int feature0x10, unsigned int feature0x01, unsigned int payLoadAddr)
{
#if 0 //jy
	unsigned int* payload = (unsigned int*)payLoadAddr;
#else
	unsigned int* payload = Addr2Mem(unsigned int, payLoadAddr);
#endif
	payload[0] = feature0x02;
	payload[1] = feature0x10;
	payload[2] = feature0x01;
	*((volatile unsigned int*)&(dev->waySelection)) = way;
	*((volatile unsigned int*)&(dev->userData)) = (unsigned int)payload;
	*((volatile unsigned int*)&(dev->cmdSelect)) = V2FCommand_SetFeatures;
#if 1 //jy
	SchedulingNand();
#endif
	while (V2FIsControllerBusy(dev));
}

void __attribute__((optimize("O0"))) V2FGetFeaturesSync(V2FMCRegisters* dev, int way, unsigned int* feature0x01, unsigned int* feature0x02, unsigned int* feature0x10, unsigned int* feature0x30)
{
	volatile unsigned int buffer[4] = {0};
	volatile unsigned int completion = 0;
	*((volatile unsigned int*)&(dev->waySelection)) = way;
	*((volatile unsigned int*)&(dev->userData)) = (unsigned int)buffer;
#if 1 //jy
	union addr addr;
   	addr.addr = (void *)&completion;
	*((volatile unsigned int*)&(dev->completionAddress)) = addr.low;
	*((volatile unsigned int*)&(dev->errorCountAddress)) = addr.high;
#else
	*((volatile unsigned int*)&(dev->completionAddress)) = (unsigned int)&completion;
#endif
	*((volatile unsigned int*)&(dev->cmdSelect)) = V2FCommand_GetFeatures;
#if 1 //jy
	SchedulingNand();
#endif
	while (V2FIsControllerBusy(dev));
	while (!(completion & 1));
	*feature0x01 = buffer[0];
	*feature0x02 = buffer[1];
	*feature0x10 = buffer[2];
	*feature0x30 = buffer[3];
}

void __attribute__((optimize("O0"))) V2FReadPageTriggerAsync(V2FMCRegisters* dev, int way, unsigned int rowAddress)
{
	*((volatile unsigned int*)&(dev->waySelection)) = way;
	*((volatile unsigned int*)&(dev->rowAddress)) = rowAddress;
	*((volatile unsigned int*)&(dev->cmdSelect)) = V2FCommand_ReadPageTrigger;
#if 1 //jy
	SchedulingNand();
#endif
}

void __attribute__((optimize("O0"))) V2FReadPageTransferAsync(V2FMCRegisters* dev, int way, void* pageDataBuffer, void* spareDataBuffer, unsigned int* errorInformation, unsigned int* completion, unsigned int rowAddress)
{
	*((volatile unsigned int*)&(dev->waySelection)) = way;
	*((volatile unsigned int*)&(dev->dataAddress)) = (unsigned int)pageDataBuffer;
	*((volatile unsigned int*)&(dev->spareAddress)) = (unsigned int)spareDataBuffer;
#if 1 //jy
	union addr addr;
   	addr.addr = completion;
	*((volatile unsigned int*)&(dev->completionAddress)) = addr.low;
	*((volatile unsigned int*)&(dev->errorCountAddress)) = addr.high;
#else
	*((volatile unsigned int*)&(dev->errorCountAddress)) = (unsigned int)errorInformation;
	*((volatile unsigned int*)&(dev->completionAddress)) = (unsigned int)completion;
#endif
	*((volatile unsigned int*)&(dev->rowAddress)) = rowAddress;
	*completion = 0;
	*((volatile unsigned int*)&(dev->cmdSelect)) = V2FCommand_ReadPageTransfer;
#if 1 //jy
	SchedulingNand();
#endif
}

void __attribute__((optimize("O0"))) V2FReadPageTransferRawAsync(V2FMCRegisters* dev, int way, void* pageDataBuffer, unsigned int* completion)
{
	*((volatile unsigned int*)&(dev->waySelection)) = way;
	*((volatile unsigned int*)&(dev->dataAddress)) = (unsigned int)pageDataBuffer;
#if 1 //jy
	union addr addr;
   	addr.addr = completion;
	*((volatile unsigned int*)&(dev->completionAddress)) = addr.low;
	*((volatile unsigned int*)&(dev->errorCountAddress)) = addr.high;
#else
	*((volatile unsigned int*)&(dev->completionAddress)) = (unsigned int)completion;
#endif
	*completion = 0;
	*((volatile unsigned int*)&(dev->cmdSelect)) = V2FCommand_ReadPageTransferRaw;
#if 1 //jy
	SchedulingNand();
#endif
}


void __attribute__((optimize("O0"))) V2FProgramPageAsync(V2FMCRegisters* dev, int way, unsigned int rowAddress, void* pageDataBuffer, void* spareDataBuffer)
{
	*((volatile unsigned int*)&(dev->waySelection)) = way;
	*((volatile unsigned int*)&(dev->rowAddress)) = rowAddress;
	*((volatile unsigned int*)&(dev->dataAddress)) = (unsigned int)pageDataBuffer;
	*((volatile unsigned int*)&(dev->spareAddress)) = (unsigned int)spareDataBuffer;
	*((volatile unsigned int*)&(dev->cmdSelect)) = V2FCommand_ProgramPage;
#if 1 //jy
	SchedulingNand();
#endif
}

void __attribute__((optimize("O0"))) V2FEraseBlockAsync(V2FMCRegisters* dev, int way, unsigned int rowAddress)
{
	*((volatile unsigned int*)&(dev->waySelection)) = way;
	*((volatile unsigned int*)&(dev->rowAddress)) = rowAddress;
	*((volatile unsigned int*)&(dev->cmdSelect)) = V2FCommand_BlockErase;
#if 1 //jy
	SchedulingNand();
#endif
}

void __attribute__((optimize("O0"))) V2FStatusCheckAsync(V2FMCRegisters* dev, int way, unsigned int* statusReport)
{
	*((volatile unsigned int*)&(dev->waySelection)) = way;
#if 1 //jy
	union addr addr;
   	addr.addr = statusReport;
	*((volatile unsigned int*)&(dev->completionAddress)) = addr.low;
	*((volatile unsigned int*)&(dev->errorCountAddress)) = addr.high;
#else
	*((volatile unsigned int*)&(dev->completionAddress)) = (unsigned int)statusReport;
#endif
	*statusReport = 0;
	*((volatile unsigned int*)&(dev->cmdSelect)) = V2FCommand_StatusCheck;
#if 1 //jy
	SchedulingNand();
#endif
}

unsigned int __attribute__((optimize("O0"))) V2FStatusCheckSync(V2FMCRegisters* dev, int way)
{
	volatile unsigned int status;
	V2FStatusCheckAsync(dev, way, (unsigned int*)&status);
	while (!(status & 1));
	return (status >> 1);
}

unsigned int __attribute__((optimize("O0"))) V2FReadyBusyAsync(V2FMCRegisters* dev)
{
	volatile unsigned int readyBusy = dev->readyBusy;

	return readyBusy;
}


