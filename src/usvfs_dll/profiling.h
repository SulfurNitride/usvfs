#pragma once

#include "semaphore.h"

namespace usvfs::profiling
{

bool enabled();
void reset();

LONGLONG beginLockWait();
void lockAcquired(LONGLONG waitStarted, BenaphoreWaitKind waitKind, bool write,
                  const char* source);
void lockReleased();

void directoryQuery(bool extendedApi, ULONG informationClass, ULONG bufferLength,
                    bool singleEntry, bool restartScan, const wchar_t* fileName,
                    USHORT fileNameLength, bool firstSearch,
                    size_t virtualFilesRemaining, LONG result);

void emitSummary();

}  // namespace usvfs::profiling
