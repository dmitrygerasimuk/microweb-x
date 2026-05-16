#ifndef _MEMORY_LOG_H_
#define _MEMORY_LOG_H_

#ifndef MEMORY_DEBUG_LOG
#define MEMORY_DEBUG_LOG 0
#endif

#if MEMORY_DEBUG_LOG
void MemoryDebugLog(const char* fmt, ...);
#endif

#endif
