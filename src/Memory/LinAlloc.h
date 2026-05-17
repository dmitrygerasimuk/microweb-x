//
// Copyright (C) 2021 James Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//

#ifndef _LINALLOC_H_
#define _LINALLOC_H_

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include "Alloc.h"
#include "MemoryLog.h"
#pragma warning(disable:4996)

// 16K chunk size including next chunk pointer
#define CHUNK_DATA_SIZE (16 * 1024 - sizeof(struct Chunk*))

class LinearAllocator : public Allocator
{
	struct Chunk
	{
		Chunk() : next(NULL) {}
		uint8_t data[CHUNK_DATA_SIZE];
		Chunk* next;
	};

public:
	enum AllocationError
	{
		Error_None,
		Error_AllocationTooLarge,
		Error_OutOfMemory
	};

	LinearAllocator() : allocOffset(0), numAllocatedChunks(1), totalBytesUsed(0), errorFlag(Error_None)
	{
		currentChunk = firstChunk = new Chunk();
		MemoryDebugLog("LINALLOC init first=%p chunk=%u", firstChunk, (unsigned)sizeof(Chunk));
	}

	~LinearAllocator()
	{
		for (Chunk* ptr = firstChunk; ptr;)
		{
			Chunk* next = ptr->next;
			delete ptr;
			ptr = next;
		}
		MemoryDebugLog("LINALLOC shutdown chunks=%ld used=%ld", numAllocatedChunks, totalBytesUsed);
	}

	void Reset()
	{
		MemoryDebugLog("LINALLOC reset chunks=%ld used=%ld offset=%u", numAllocatedChunks, totalBytesUsed, (unsigned)allocOffset);
		currentChunk = firstChunk;
		allocOffset = 0;
		totalBytesUsed = 0;
		errorFlag = Error_None;
	}

	virtual void* Allocate(size_t numBytes)
	{
		if (numBytes >= CHUNK_DATA_SIZE)
		{
			errorFlag = Error_AllocationTooLarge;
			MemoryDebugLog("LINALLOC fail-too-large size=%lu chunkData=%u", (unsigned long)numBytes, (unsigned)CHUNK_DATA_SIZE);
			return NULL;
		}

		if (!currentChunk)
		{
			errorFlag = Error_OutOfMemory;
			MemoryDebugLog("LINALLOC fail-no-current size=%lu chunks=%ld used=%ld", (unsigned long)numBytes, numAllocatedChunks, totalBytesUsed);
			return nullptr;
		}

		uint8_t* result = &currentChunk->data[allocOffset];

		if (allocOffset + numBytes > CHUNK_DATA_SIZE)
		{
			// Need to allocate from the next chunk

			if (!currentChunk->next)
			{
				currentChunk->next = new Chunk();

				if (!currentChunk->next)
				{
					errorFlag = Error_OutOfMemory;
					MemoryDebugLog("LINALLOC fail-new-chunk size=%lu chunks=%ld used=%ld", (unsigned long)numBytes, numAllocatedChunks, totalBytesUsed);
					return NULL;
				}
				numAllocatedChunks++;
				MemoryDebugLog("LINALLOC new-chunk chunks=%ld used=%ld request=%lu", numAllocatedChunks, totalBytesUsed, (unsigned long)numBytes);
			}

			currentChunk = currentChunk->next;
			allocOffset = 0;
			result = &currentChunk->data[allocOffset];
		}

		totalBytesUsed += (long) numBytes;
		allocOffset += numBytes;
		return NormalizeFarPointer(result);
	}

	long TotalAllocated() { return numAllocatedChunks * sizeof(Chunk); }
	long TotalUsed() { return totalBytesUsed; }
	AllocationError GetError() { return errorFlag; }

private:

	Chunk* firstChunk;
	Chunk* currentChunk;
	size_t allocOffset;

	long numAllocatedChunks;
	long totalBytesUsed;		// Bytes actually used for data
	AllocationError errorFlag;
};

#endif
