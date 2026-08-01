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
                    bool singleEntry, bool restartScan,
                    const UNICODE_STRING* fileName, bool firstSearch,
                    size_t virtualFilesRemaining, LONG result);

void emitSummary();

}
