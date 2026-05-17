#include <string.h>
#include "MemBlock.h"
#include "LinAlloc.h"
#include "Memory.h"
#include "MemoryLog.h"
#include "../Platform.h"
#include "../App.h"

#ifdef __DOS__
#include <dos.h>
#include "../DOS/EMS.h"
#include "../DOS/XMS.h"

EMSManager ems;
XMSManager xms;
#endif

static void LogInvalidMemBlockHandle(const char* operation, MemBlockHandle* handle)
{
	const unsigned char* raw = (const unsigned char*)handle;
	MemoryDebugLog("MEMBLOCK invalid-handle op=%s self=%p raw=%02x %02x %02x %02x %02x type=%u conv=%p swap=%ld emsPage=%u emsOff=%u xmsPtr=%lu xmsLen=%u",
		operation, handle,
		(unsigned)raw[0], (unsigned)raw[1], (unsigned)raw[2], (unsigned)raw[3], (unsigned)raw[4],
		(unsigned)handle->type, handle->conventionalPointer, handle->swapFilePosition,
		(unsigned)handle->emsPage, (unsigned)handle->emsPageOffset,
		(unsigned long)handle->xmsPointer, (unsigned)handle->xmsLength);
}

void* MemBlockHandle::GetPtr()
{
	switch ((MemBlockHandle::Type)type)
	{
	case MemBlockHandle::Unallocated:
		return nullptr;
	case MemBlockHandle::Conventional:
		return conventionalPointer;
	case MemBlockHandle::DiskSwap:
	{
		return MemoryManager::pageBlockAllocator.AccessSwap(*this);
	}
#ifdef __DOS__
	case MemBlockHandle::EMS:
	{
		return ems.MapBlock(*this);
	}
	case MemBlockHandle::XMS:
	{
		return xms.MapBlock(*this);
	}
#endif
	default:
		LogInvalidMemBlockHandle("get", this);
		Platform::FatalError("Invalid pointer type: %u\n", (unsigned)type);
		return nullptr;
	}
}

void MemBlockHandle::Commit()
{
	switch ((MemBlockHandle::Type)type)
	{
	case MemBlockHandle::DiskSwap:
		MemoryManager::pageBlockAllocator.CommitSwap(*this);
		break;
#ifdef __DOS__
	case MemBlockHandle::XMS:
		xms.Commit(*this);
		break;
#endif
	case MemBlockHandle::Unallocated:
	case MemBlockHandle::Conventional:
#ifdef __DOS__
	case MemBlockHandle::EMS:
#endif
		break;
	default:
		LogInvalidMemBlockHandle("commit", this);
		Platform::FatalError("Invalid pointer type: %u\n", (unsigned)type);
		break;
	}
}

MemBlockAllocator::MemBlockAllocator()
	: swapFile(nullptr)
	, swapFileLength(0)
	, swapBuffer(nullptr)
	, lastSwapRead(-1)
	, maxSwapSize(0)
	, totalAllocated(0)
{
}

void MemBlockAllocator::Init()
{
	MemoryDebugLog("MEMBLOCK init useSwap=%d useEMS=%d useXMS=%d", App::config.useSwap, App::config.useEMS, App::config.useXMS);
	if (App::config.useSwap)
	{
		swapFile = fopen("Microweb.swp", "wb+");

		if (!swapFile)
		{
			Platform::FatalError("Could not open Microweb.swp swap file!");
		}
	}

	if (swapFile)
	{
		swapBuffer = malloc(MAX_SWAP_ALLOCATION);
		lastSwapRead = -1;
		swapFileLength = 0;
		maxSwapSize = MAX_SWAP_SIZE;
		MemoryDebugLog("SWAP init buffer=%p maxAlloc=%u maxSize=%ld", swapBuffer, (unsigned)MAX_SWAP_ALLOCATION, maxSwapSize);
	}

#ifdef __DOS__
	if (App::config.useEMS)
	{
		ems.Init();
	}
	if (App::config.useXMS)
	{
		xms.Init();
	}
#endif
}

void MemBlockAllocator::Shutdown()
{
	MemoryDebugLog("MEMBLOCK shutdown total=%ld swap=%ld", totalAllocated, swapFileLength);
#ifdef __DOS__
	ems.Shutdown();
	xms.Shutdown();
#endif

	if (swapFile)
	{
		fclose(swapFile);
		swapFile = NULL;
	}
}

MemBlockHandle MemBlockAllocator::AllocString(const char* inString)
{
	MemBlockHandle result = Allocate((uint16_t)strlen(inString) + 1);
	if (result.IsAllocated())
	{
		strcpy(result.Get<char*>(), inString);
		result.Commit();
	}
	return result;
}

MemBlockHandle MemBlockAllocator::Allocate(uint16_t size)
{
	MemBlockHandle result;

	// Use EMS if available
#ifdef __DOS__
	if (ems.IsAvailable())
	{
		result = ems.Allocate(size);
		if (result.IsAllocated())
		{
			totalAllocated += size;
			MemoryDebugLog("MEMBLOCK alloc EMS size=%u page=%u off=%u total=%ld emsUsed=%ld/%ld",
				(unsigned)size, (unsigned)result.emsPage, (unsigned)result.emsPageOffset,
				totalAllocated, ems.TotalUsed(), ems.TotalAllocated());
			return result;
		}
	}

	if (xms.IsAvailable())
	{
		result = xms.Allocate(size);
		if (result.IsAllocated())
		{
			totalAllocated += size;
			MemoryDebugLog("MEMBLOCK alloc XMS size=%u ptr=%lu len=%u total=%ld xmsUsed=%ld/%ld",
				(unsigned)size, (unsigned long)result.xmsPointer, (unsigned)result.xmsLength,
				totalAllocated, xms.TotalUsed(), xms.TotalAllocated());
			return result;
		}
	}
#endif
	
	long conventionalMemoryAvailable = MemoryManager::GetConventionalMemoryAvailableKB() * 1024L;
	conventionalMemoryAvailable += MemoryManager::pageAllocator.TotalAllocated() - MemoryManager::pageAllocator.TotalUsed();

	if (swapFile && conventionalMemoryAvailable < 16 * 1024)	// If we have less than 16K of conventional memory available, fall back to disk
	{
		uint16_t sizeNeededForSwap = size + sizeof(uint16_t);

		if (sizeNeededForSwap <= MAX_SWAP_ALLOCATION && swapFileLength + sizeNeededForSwap < maxSwapSize)
		{
			result.swapFilePosition = swapFileLength;
			fseek(swapFile, swapFileLength, SEEK_SET);

			fwrite(&size, sizeof(uint16_t), 1, swapFile);

			uint8_t empty = 0xaa;
			size_t bytesLeft = size;
			while (bytesLeft > 0)
			{
				if(!fwrite(&empty, 1, 1, swapFile))
				{
					// Out of disk space?
					result.type = MemBlockHandle::Unallocated;
					return result;
				}
				bytesLeft--;
			}

			swapFileLength += sizeNeededForSwap;
			result.type = MemBlockHandle::DiskSwap;
			totalAllocated += sizeNeededForSwap;
			MemoryDebugLog("MEMBLOCK alloc SWAP size=%u stored=%u pos=%ld total=%ld swap=%ld",
				(unsigned)size, (unsigned)sizeNeededForSwap, result.swapFilePosition,
				totalAllocated, swapFileLength);
			return result;
		}
	}

	//if(0)
	{
		result.conventionalPointer = MemoryManager::pageAllocator.Allocate(size);
		if (result.conventionalPointer)
		{
			result.type = MemBlockHandle::Conventional;
			totalAllocated += size;
			MemoryDebugLog("MEMBLOCK alloc CONV size=%u ptr=%p total=%ld pageUsed=%ld/%ld dosFree=%ldK",
				(unsigned)size, result.conventionalPointer, totalAllocated,
				MemoryManager::pageAllocator.TotalUsed(), MemoryManager::pageAllocator.TotalAllocated(),
				MemoryManager::GetConventionalMemoryAvailableKB());
		}
		else
		{
			MemoryDebugLog("MEMBLOCK alloc FAIL size=%u total=%ld pageUsed=%ld/%ld dosFree=%ldK",
				(unsigned)size, totalAllocated,
				MemoryManager::pageAllocator.TotalUsed(), MemoryManager::pageAllocator.TotalAllocated(),
				MemoryManager::GetConventionalMemoryAvailableKB());
		}
	}

	return result;
}

void* MemBlockAllocator::AccessSwap(MemBlockHandle& handle)
{
	if (swapFile && lastSwapRead != handle.swapFilePosition)
	{
		fseek(swapFile, handle.swapFilePosition, SEEK_SET);
		uint16_t allocatedSize = 0;
		fread(&allocatedSize, sizeof(uint16_t), 1, swapFile);
		fread(swapBuffer, 1, allocatedSize, swapFile);
		lastSwapRead = handle.swapFilePosition;
	}

	return swapBuffer;
}

void MemBlockAllocator::CommitSwap(MemBlockHandle& handle)
{
	if (swapFile)
	{
		fseek(swapFile, handle.swapFilePosition, SEEK_SET);
		uint16_t allocatedSize = 0;
		fread(&allocatedSize, sizeof(uint16_t), 1, swapFile);
		fseek(swapFile, handle.swapFilePosition + sizeof(uint16_t), SEEK_SET);
		fwrite(swapBuffer, 1, allocatedSize, swapFile);
	}
}

void MemBlockAllocator::Reset()
{
	MemoryDebugLog("MEMBLOCK reset total=%ld swap=%ld pageUsed=%ld/%ld",
		totalAllocated, swapFileLength,
		MemoryManager::pageAllocator.TotalUsed(), MemoryManager::pageAllocator.TotalAllocated());
	swapFileLength = 0;
	lastSwapRead = -1;
	totalAllocated = 0;

#ifdef __DOS__
	ems.Reset();
	xms.Reset();
#endif
}
