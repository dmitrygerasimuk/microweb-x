#include "EMS.h"
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Memory/MemoryLog.h"

#define EMS_INTERRUPT_NUMBER 0x67

void EMSManager::Init()
{
    union REGS inregs, outregs;
    struct SREGS sregs;

    // Check for EMS driver
    inregs.h.ah = 0x35;
    inregs.h.al = EMS_INTERRUPT_NUMBER;
    int86x(0x21, &inregs, &outregs, &sregs);
    char* emm = (char*) MK_FP(sregs.es, 0xa);

    if(memcmp(emm, "EMMXXXX0", 8))
    {
        // No EMS present
#if MEMORY_DEBUG_LOG
        MemoryDebugLog("EMS init unavailable");
#endif
        return;
    }

    // Check for EMS version 4
    inregs.h.ah = 0x46;
    int86(EMS_INTERRUPT_NUMBER, &inregs, &outregs);
    if ((outregs.h.al & 0xf0) < 0x40) {
        // Incorrect version
#if MEMORY_DEBUG_LOG
        MemoryDebugLog("EMS init bad-version raw=%u", (unsigned)outregs.h.al);
#endif
        return;
    }

    // Get the page address
    inregs.h.ah = 0x41;
    int86(EMS_INTERRUPT_NUMBER, &inregs, &outregs);
    pageAddressSegment = outregs.x.bx;

    // Get the number of unallocated pages
    inregs.h.ah = 0x42;
    int86(EMS_INTERRUPT_NUMBER, &inregs, &outregs);
    int numAvailablePages = outregs.x.bx;

    // Allocate the pages
    inregs.h.ah = 0x43;
    inregs.x.bx = numAvailablePages;
    int86(EMS_INTERRUPT_NUMBER, &inregs, &outregs);

    if (outregs.h.ah)
    {
        // Allocation failed
#if MEMORY_DEBUG_LOG
        MemoryDebugLog("EMS init alloc-fail pages=%d error=%u", numAvailablePages, (unsigned)outregs.h.ah);
#endif
        return;
    }

    numAllocatedPages = numAvailablePages;
    allocationHandle = outregs.x.dx;

    allocationPageIndex = 0;
    allocationPageUsed = 0;

    for (int n = 0; n < NUM_MAPPABLE_PAGES; n++)
    {
        mappedPages[n] = 0xffff;
    }
    nextPageToMap = 0;

    isAvailable = true;
#if MEMORY_DEBUG_LOG
    MemoryDebugLog("EMS init ok pages=%d handle=%u frame=%04x", numAllocatedPages, (unsigned)allocationHandle, (unsigned)pageAddressSegment);
#endif
}

void EMSManager::Reset()
{
#if MEMORY_DEBUG_LOG
    MemoryDebugLog("EMS reset used=%ld/%ld page=%d off=%u", TotalUsed(), TotalAllocated(), allocationPageIndex, (unsigned)allocationPageUsed);
#endif
    allocationPageIndex = 0;
    allocationPageUsed = 0;
}

void EMSManager::Shutdown()
{
    if (isAvailable)
    {
#if MEMORY_DEBUG_LOG
        MemoryDebugLog("EMS shutdown/free handle=%u used=%ld/%ld", (unsigned)allocationHandle, TotalUsed(), TotalAllocated());
#endif
        union REGS inregs, outregs;

        // Free allocated pages
        inregs.h.ah = 0x45;
        inregs.x.dx = allocationHandle;
        int86(EMS_INTERRUPT_NUMBER, &inregs, &outregs);
#if MEMORY_DEBUG_LOG
        MemoryDebugLog("EMS shutdown/free done handle=%u error=%u", (unsigned)allocationHandle, (unsigned)outregs.h.ah);
#endif

        isAvailable = false;
        numAllocatedPages = 0;
        allocationHandle = 0;
        allocationPageIndex = 0;
        allocationPageUsed = 0;
    }
}

MemBlockHandle EMSManager::Allocate(size_t size)
{
    MemBlockHandle result;

    if (allocationPageIndex < numAllocatedPages)
    {
        if (size + allocationPageUsed > EMS_PAGE_SIZE)
        {
            allocationPageIndex++;
            allocationPageUsed = 0;
        }

        if (allocationPageIndex < numAllocatedPages)
        {
            result.emsPage = allocationPageIndex;
            result.emsPageOffset = allocationPageUsed;
            result.type = MemBlockHandle::EMS;
            allocationPageUsed += size;
        }
    }

    return result;
}

void* EMSManager::MapBlock(MemBlockHandle& handle)
{
    if (isAvailable && handle.type == MemBlockHandle::EMS)
    {
        // Check if this page is already mapped first
        for (int n = 0; n < NUM_MAPPABLE_PAGES; n++)
        {
            if (mappedPages[n] == handle.emsPage)
            {
                return MK_FP(pageAddressSegment + n * EMS_PAGE_SEGMENT_SPACING, handle.emsPageOffset);
            }
        }

        int mappedPageIndex = nextPageToMap;

        nextPageToMap++;
        if (nextPageToMap >= NUM_MAPPABLE_PAGES)
            nextPageToMap = 0;

        {
            union REGS inregs, outregs;
            inregs.h.ah = 0x44;
            inregs.h.al = mappedPageIndex;
            inregs.x.bx = handle.emsPage;
            inregs.x.dx = allocationHandle;
            int86(EMS_INTERRUPT_NUMBER, &inregs, &outregs);
            mappedPages[mappedPageIndex] = handle.emsPage;
        }

        return MK_FP(pageAddressSegment + mappedPageIndex * EMS_PAGE_SEGMENT_SPACING, handle.emsPageOffset);
    }

    return nullptr;
}
