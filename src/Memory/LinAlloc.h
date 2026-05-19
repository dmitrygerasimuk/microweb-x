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

#define LINEAR_ALLOC_DEFAULT_CHUNK_SIZE (16 * 1024)
#define LINEAR_ALLOC_MIN_CHUNK_SIZE (1024)

class LinearAllocator : public Allocator
{
	struct Chunk
	{
		Chunk* next;
		unsigned long dataSize;
	};

public:
	enum AllocationError
	{
		Error_None,
		Error_AllocationTooLarge,
		Error_OutOfMemory
	};

	LinearAllocator()
		: firstChunk(NULL), currentChunk(NULL), allocOffset(0), chunkAllocationSize(LINEAR_ALLOC_DEFAULT_CHUNK_SIZE), numAllocatedChunks(0), totalBytesAllocated(0), totalBytesUsed(0), errorFlag(Error_None)
	{
	}

	~LinearAllocator()
	{
		for (Chunk* ptr = firstChunk; ptr;)
		{
			Chunk* next = ptr->next;
			FreeChunk(ptr);
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
		if (numBytes > ChunkDataSize(chunkAllocationSize))
		{
			errorFlag = Error_AllocationTooLarge;
			MemoryDebugLog("LINALLOC fail-too-large size=%lu chunkData=%u", (unsigned long)numBytes, (unsigned)ChunkDataSize(chunkAllocationSize));
			return NULL;
		}

		if (!currentChunk && !AllocateFirstChunk())
		{
			errorFlag = Error_OutOfMemory;
			MemoryDebugLog("LINALLOC fail-no-current size=%lu chunks=%ld used=%ld", (unsigned long)numBytes, numAllocatedChunks, totalBytesUsed);
			return nullptr;
		}

		uint8_t* result = ChunkData(currentChunk) + allocOffset;

		if (allocOffset + numBytes > currentChunk->dataSize)
		{
			// Need to allocate from the next chunk

			if (!currentChunk->next)
			{
				currentChunk->next = AllocateChunk(chunkAllocationSize);

				if (!currentChunk->next)
				{
					errorFlag = Error_OutOfMemory;
					MemoryDebugLog("LINALLOC fail-new-chunk size=%lu chunks=%ld used=%ld chunk=%u", (unsigned long)numBytes, numAllocatedChunks, totalBytesUsed, (unsigned)chunkAllocationSize);
					return NULL;
				}
				numAllocatedChunks++;
				totalBytesAllocated += ChunkAllocationSize(currentChunk->next);
				MemoryDebugLog("LINALLOC new-chunk chunks=%ld used=%ld request=%lu chunk=%u", numAllocatedChunks, totalBytesUsed, (unsigned long)numBytes, (unsigned)chunkAllocationSize);
			}

			currentChunk = currentChunk->next;
			allocOffset = 0;
			result = ChunkData(currentChunk);
		}

		totalBytesUsed += (long) numBytes;
		allocOffset += numBytes;
		return NormalizeFarPointer(result);
	}

	void SetChunkSize(size_t numBytes)
	{
		chunkAllocationSize = NormalizeChunkSize(numBytes);
	}

	size_t GetChunkSize() { return chunkAllocationSize; }
	static size_t DefaultChunkSize() { return LINEAR_ALLOC_DEFAULT_CHUNK_SIZE; }

	long TotalAllocated() { return totalBytesAllocated; }
	long TotalUsed() { return totalBytesUsed; }
	AllocationError GetError() { return errorFlag; }

private:
	static size_t NormalizeChunkSize(size_t numBytes)
	{
		if (numBytes < LINEAR_ALLOC_MIN_CHUNK_SIZE)
		{
			numBytes = LINEAR_ALLOC_MIN_CHUNK_SIZE;
		}
		if (numBytes > LINEAR_ALLOC_DEFAULT_CHUNK_SIZE)
		{
			numBytes = LINEAR_ALLOC_DEFAULT_CHUNK_SIZE;
		}
		return numBytes;
	}

	static long ChunkAllocationSize(Chunk* chunk)
	{
		return sizeof(Chunk) + (long)chunk->dataSize;
	}

	static size_t ChunkDataSize(size_t allocationSize)
	{
		return NormalizeChunkSize(allocationSize) - sizeof(Chunk);
	}

	static uint8_t* ChunkData(Chunk* chunk)
	{
		return (uint8_t*)(chunk + 1);
	}

	static Chunk* AllocateChunk(size_t allocationSize)
	{
		allocationSize = NormalizeChunkSize(allocationSize);
		uint8_t* memory = new uint8_t[allocationSize];
		if (!memory)
		{
			return NULL;
		}

		Chunk* chunk = (Chunk*)memory;
		chunk->next = NULL;
		chunk->dataSize = ChunkDataSize(allocationSize);
		return chunk;
	}

	static void FreeChunk(Chunk* chunk)
	{
		delete[] (uint8_t*)chunk;
	}

	bool AllocateFirstChunk()
	{
		currentChunk = firstChunk = AllocateChunk(chunkAllocationSize);
		if (!firstChunk)
		{
			return false;
		}

		numAllocatedChunks = 1;
		totalBytesAllocated = ChunkAllocationSize(firstChunk);
		MemoryDebugLog("LINALLOC init first=%p chunk=%u", firstChunk, (unsigned)chunkAllocationSize);
		return true;
	}

	Chunk* firstChunk;
	Chunk* currentChunk;
	size_t allocOffset;
	size_t chunkAllocationSize;

	long numAllocatedChunks;
	long totalBytesAllocated;
	long totalBytesUsed;		// Bytes actually used for data
	AllocationError errorFlag;
};

#endif
