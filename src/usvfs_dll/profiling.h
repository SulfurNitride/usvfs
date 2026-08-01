#pragma once

#include "semaphore.h"

namespace usvfs::profiling
{

bool enabled();
void reset();

LONGLONG beginLockWait();
LONGLONG beginOperation();
void lockAcquired(LONGLONG waitStarted, BenaphoreWaitKind waitKind, bool write,
                  const char* source);
void lockReleased();

void directoryQuery(bool extendedApi, ULONG informationClass, ULONG bufferLength,
                    bool singleEntry, bool restartScan, const wchar_t* fileName,
                    USHORT fileNameLength, bool firstSearch,
                    size_t virtualFilesRemaining, LONG result);
void parentDirectoryOpen(LONGLONG started, bool success);
void backingDirectoryQuery(LONGLONG started, bool virtualQuery, LONG result);
unsigned long long hashPath(const wchar_t* path);
void treeLookup(unsigned long long pathHash, bool found);
void attributeLookup(unsigned long long pathHash, bool missing);

void emitSummary();

}  // namespace usvfs::profiling
