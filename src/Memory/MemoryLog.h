#ifndef _MEMORY_LOG_H_
#define _MEMORY_LOG_H_

void MemoryDebugLog(const char* fmt, ...);
void MemoryDebugLogSetEnabled(bool enabled);
void MemoryDebugLogEnableFromArgs(int argc, char* argv[]);
bool MemoryDebugLogIsEnabled();

#endif
