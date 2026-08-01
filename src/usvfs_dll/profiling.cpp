#include "profiling.h"

#include <array>

namespace usvfs::profiling
{
namespace
{
  constexpr size_t SourceSlotCount       = 128;
  constexpr size_t InformationClassCount = 128;

  struct SourceStats
  {
    std::atomic<const char*> source{nullptr};
    std::atomic<unsigned long long> acquisitions{0};
    std::atomic<unsigned long long> contended{0};
    std::atomic<unsigned long long> waitTicks{0};
    std::atomic<unsigned long long> maxWaitTicks{0};
  };

  struct Counters
  {
    std::atomic<unsigned long long> lockAcquisitions{0};
    std::atomic<unsigned long long> lockReads{0};
    std::atomic<unsigned long long> lockWrites{0};
    std::atomic<unsigned long long> lockRecursive{0};
    std::atomic<unsigned long long> lockContended{0};
    std::atomic<unsigned long long> lockWaitTicks{0};
    std::atomic<unsigned long long> lockMaxWaitTicks{0};
    std::atomic<unsigned long long> lockHoldTicks{0};
    std::atomic<unsigned long long> lockMaxHoldTicks{0};
    std::atomic<unsigned long long> lockMaxDepth{0};

    std::atomic<unsigned long long> directoryQueries{0};
    std::atomic<unsigned long long> directoryLegacy{0};
    std::atomic<unsigned long long> directoryExtended{0};
    std::atomic<unsigned long long> directorySingle{0};
    std::atomic<unsigned long long> directoryRestart{0};
    std::atomic<unsigned long long> directoryNullPattern{0};
    std::atomic<unsigned long long> directoryExactPattern{0};
    std::atomic<unsigned long long> directoryWildcardPattern{0};
    std::atomic<unsigned long long> directoryFirstSearch{0};
    std::atomic<unsigned long long> directoryVirtualRemaining{0};
    std::atomic<unsigned long long> directorySuccess{0};
    std::atomic<unsigned long long> directoryNoMoreFiles{0};
    std::atomic<unsigned long long> directoryNoSuchFile{0};
    std::atomic<unsigned long long> directoryOtherStatus{0};
    std::atomic<unsigned long long> parentDirectoryOpens{0};
    std::atomic<unsigned long long> parentDirectoryOpenFailures{0};
    std::atomic<unsigned long long> parentDirectoryOpenTicks{0};
    std::atomic<unsigned long long> parentDirectoryOpenMaxTicks{0};
    std::atomic<unsigned long long> regularBackingQueries{0};
    std::atomic<unsigned long long> regularBackingQueryTicks{0};
    std::atomic<unsigned long long> regularBackingQueryMaxTicks{0};
    std::atomic<unsigned long long> virtualBackingQueries{0};
    std::atomic<unsigned long long> virtualBackingQueryTicks{0};
    std::atomic<unsigned long long> virtualBackingQueryMaxTicks{0};
    std::atomic<unsigned long long> backingQuerySuccess{0};
    std::atomic<unsigned long long> backingQueryNoMoreFiles{0};
    std::atomic<unsigned long long> backingQueryOtherStatus{0};
    std::array<std::atomic<unsigned long long>, 6> directoryBufferBuckets{};
    std::array<std::atomic<unsigned long long>, InformationClassCount>
        directoryInformationClasses{};
    std::array<SourceStats, SourceSlotCount> sources{};
  };

  Counters g_Counters;

  struct HeldLock
  {
    LONGLONG acquiredAt;
  };

  thread_local std::array<HeldLock, 64> g_HeldLocks{};
  thread_local size_t g_HeldLockDepth = 0;

  void updateMax(std::atomic<unsigned long long>& target, unsigned long long candidate)
  {
    auto observed = target.load(std::memory_order_relaxed);
    while (
        observed < candidate &&
        !target.compare_exchange_weak(observed, candidate, std::memory_order_relaxed)) {
    }
  }

  LONGLONG nowTicks()
  {
    LARGE_INTEGER value{};
    ::QueryPerformanceCounter(&value);
    return value.QuadPart;
  }

  SourceStats* sourceStats(const char* source)
  {
    if (source == nullptr) {
      return nullptr;
    }
    const auto value   = reinterpret_cast<uintptr_t>(source);
    const size_t start = (value >> 4) % SourceSlotCount;
    for (size_t offset = 0; offset < SourceSlotCount; ++offset) {
      SourceStats& slot    = g_Counters.sources[(start + offset) % SourceSlotCount];
      const char* observed = slot.source.load(std::memory_order_relaxed);
      if (observed == source) {
        return &slot;
      }
      if (observed == nullptr && slot.source.compare_exchange_strong(
                                     observed, source, std::memory_order_relaxed)) {
        return &slot;
      }
    }
    return nullptr;
  }

  size_t bufferBucket(ULONG length)
  {
    if (length <= 64)
      return 0;
    if (length <= 256)
      return 1;
    if (length <= 1024)
      return 2;
    if (length <= 4096)
      return 3;
    if (length <= 16384)
      return 4;
    return 5;
  }

  enum class PatternKind
  {
    Null,
    Exact,
    Wildcard
  };

  PatternKind patternKind(const wchar_t* fileName, USHORT fileNameLength)
  {
    if (fileName == nullptr || fileNameLength == 0) {
      return PatternKind::Null;
    }
    const size_t characterCount = fileNameLength / sizeof(wchar_t);
    for (size_t i = 0; i < characterCount; ++i) {
      if (fileName[i] == L'*' || fileName[i] == L'?') {
        return PatternKind::Wildcard;
      }
    }
    return PatternKind::Exact;
  }

  template <class T>
  void clear(std::atomic<T>& value)
  {
    value.store(0, std::memory_order_relaxed);
  }
}  // namespace

bool enabled()
{
  static const bool result = []() {
    wchar_t value[16]{};
    const DWORD length =
        ::GetEnvironmentVariableW(L"FLUORINE_USVFS_PROFILE", value, ARRAYSIZE(value));
    if (length == 0 || length >= ARRAYSIZE(value))
      return false;
    return _wcsicmp(value, L"0") != 0 && _wcsicmp(value, L"false") != 0 &&
           _wcsicmp(value, L"off") != 0;
  }();
  return result;
}

void reset()
{
  clear(g_Counters.lockAcquisitions);
  clear(g_Counters.lockReads);
  clear(g_Counters.lockWrites);
  clear(g_Counters.lockRecursive);
  clear(g_Counters.lockContended);
  clear(g_Counters.lockWaitTicks);
  clear(g_Counters.lockMaxWaitTicks);
  clear(g_Counters.lockHoldTicks);
  clear(g_Counters.lockMaxHoldTicks);
  clear(g_Counters.lockMaxDepth);
  clear(g_Counters.directoryQueries);
  clear(g_Counters.directoryLegacy);
  clear(g_Counters.directoryExtended);
  clear(g_Counters.directorySingle);
  clear(g_Counters.directoryRestart);
  clear(g_Counters.directoryNullPattern);
  clear(g_Counters.directoryExactPattern);
  clear(g_Counters.directoryWildcardPattern);
  clear(g_Counters.directoryFirstSearch);
  clear(g_Counters.directoryVirtualRemaining);
  clear(g_Counters.directorySuccess);
  clear(g_Counters.directoryNoMoreFiles);
  clear(g_Counters.directoryNoSuchFile);
  clear(g_Counters.directoryOtherStatus);
  clear(g_Counters.parentDirectoryOpens);
  clear(g_Counters.parentDirectoryOpenFailures);
  clear(g_Counters.parentDirectoryOpenTicks);
  clear(g_Counters.parentDirectoryOpenMaxTicks);
  clear(g_Counters.regularBackingQueries);
  clear(g_Counters.regularBackingQueryTicks);
  clear(g_Counters.regularBackingQueryMaxTicks);
  clear(g_Counters.virtualBackingQueries);
  clear(g_Counters.virtualBackingQueryTicks);
  clear(g_Counters.virtualBackingQueryMaxTicks);
  clear(g_Counters.backingQuerySuccess);
  clear(g_Counters.backingQueryNoMoreFiles);
  clear(g_Counters.backingQueryOtherStatus);
  for (auto& value : g_Counters.directoryBufferBuckets)
    clear(value);
  for (auto& value : g_Counters.directoryInformationClasses)
    clear(value);
  for (auto& value : g_Counters.sources) {
    value.source.store(nullptr, std::memory_order_relaxed);
    clear(value.acquisitions);
    clear(value.contended);
    clear(value.waitTicks);
    clear(value.maxWaitTicks);
  }
  g_HeldLockDepth = 0;
}

LONGLONG beginLockWait()
{
  return enabled() ? nowTicks() : 0;
}

LONGLONG beginOperation()
{
  return enabled() ? nowTicks() : 0;
}

void lockAcquired(LONGLONG waitStarted, BenaphoreWaitKind waitKind, bool write,
                  const char* source)
{
  if (!enabled())
    return;
  const LONGLONG acquiredAt = nowTicks();
  const auto wait           = static_cast<unsigned long long>(acquiredAt - waitStarted);
  g_Counters.lockAcquisitions.fetch_add(1, std::memory_order_relaxed);
  (write ? g_Counters.lockWrites : g_Counters.lockReads)
      .fetch_add(1, std::memory_order_relaxed);
  if (waitKind == BenaphoreWaitKind::Recursive) {
    g_Counters.lockRecursive.fetch_add(1, std::memory_order_relaxed);
  } else if (waitKind == BenaphoreWaitKind::Contended) {
    g_Counters.lockContended.fetch_add(1, std::memory_order_relaxed);
  }
  g_Counters.lockWaitTicks.fetch_add(wait, std::memory_order_relaxed);
  updateMax(g_Counters.lockMaxWaitTicks, wait);

  if (SourceStats* stats = sourceStats(source)) {
    stats->acquisitions.fetch_add(1, std::memory_order_relaxed);
    if (waitKind == BenaphoreWaitKind::Contended)
      stats->contended.fetch_add(1, std::memory_order_relaxed);
    stats->waitTicks.fetch_add(wait, std::memory_order_relaxed);
    updateMax(stats->maxWaitTicks, wait);
  }
  if (g_HeldLockDepth < g_HeldLocks.size()) {
    g_HeldLocks[g_HeldLockDepth] = {acquiredAt};
  }
  ++g_HeldLockDepth;
  updateMax(g_Counters.lockMaxDepth, static_cast<unsigned long long>(g_HeldLockDepth));
}

void lockReleased()
{
  if (!enabled() || g_HeldLockDepth == 0)
    return;
  --g_HeldLockDepth;
  if (g_HeldLockDepth >= g_HeldLocks.size())
    return;
  const auto hold = static_cast<unsigned long long>(
      nowTicks() - g_HeldLocks[g_HeldLockDepth].acquiredAt);
  g_Counters.lockHoldTicks.fetch_add(hold, std::memory_order_relaxed);
  updateMax(g_Counters.lockMaxHoldTicks, hold);
}

void directoryQuery(bool extendedApi, ULONG informationClass, ULONG bufferLength,
                    bool singleEntry, bool restartScan, const wchar_t* fileName,
                    USHORT fileNameLength, bool firstSearch,
                    size_t virtualFilesRemaining, LONG result)
{
  if (!enabled())
    return;
  g_Counters.directoryQueries.fetch_add(1, std::memory_order_relaxed);
  (extendedApi ? g_Counters.directoryExtended : g_Counters.directoryLegacy)
      .fetch_add(1, std::memory_order_relaxed);
  if (singleEntry)
    g_Counters.directorySingle.fetch_add(1, std::memory_order_relaxed);
  if (restartScan)
    g_Counters.directoryRestart.fetch_add(1, std::memory_order_relaxed);
  if (firstSearch)
    g_Counters.directoryFirstSearch.fetch_add(1, std::memory_order_relaxed);
  if (virtualFilesRemaining > 0)
    g_Counters.directoryVirtualRemaining.fetch_add(1, std::memory_order_relaxed);

  switch (patternKind(fileName, fileNameLength)) {
  case PatternKind::Null:
    g_Counters.directoryNullPattern.fetch_add(1, std::memory_order_relaxed);
    break;
  case PatternKind::Exact:
    g_Counters.directoryExactPattern.fetch_add(1, std::memory_order_relaxed);
    break;
  case PatternKind::Wildcard:
    g_Counters.directoryWildcardPattern.fetch_add(1, std::memory_order_relaxed);
    break;
  }
  g_Counters.directoryBufferBuckets[bufferBucket(bufferLength)].fetch_add(
      1, std::memory_order_relaxed);
  if (informationClass < InformationClassCount) {
    g_Counters.directoryInformationClasses[informationClass].fetch_add(
        1, std::memory_order_relaxed);
  }

  const auto status = static_cast<unsigned long>(result);
  if (status == 0x00000000UL)
    g_Counters.directorySuccess.fetch_add(1, std::memory_order_relaxed);
  else if (status == 0x80000006UL)
    g_Counters.directoryNoMoreFiles.fetch_add(1, std::memory_order_relaxed);
  else if (status == 0xC000000FUL)
    g_Counters.directoryNoSuchFile.fetch_add(1, std::memory_order_relaxed);
  else
    g_Counters.directoryOtherStatus.fetch_add(1, std::memory_order_relaxed);
}

void parentDirectoryOpen(LONGLONG started, bool success)
{
  if (!enabled())
    return;
  const auto elapsed = static_cast<unsigned long long>(nowTicks() - started);
  g_Counters.parentDirectoryOpens.fetch_add(1, std::memory_order_relaxed);
  if (!success)
    g_Counters.parentDirectoryOpenFailures.fetch_add(1, std::memory_order_relaxed);
  g_Counters.parentDirectoryOpenTicks.fetch_add(elapsed, std::memory_order_relaxed);
  updateMax(g_Counters.parentDirectoryOpenMaxTicks, elapsed);
}

void backingDirectoryQuery(LONGLONG started, bool virtualQuery, LONG result)
{
  if (!enabled())
    return;
  const auto elapsed = static_cast<unsigned long long>(nowTicks() - started);
  if (virtualQuery) {
    g_Counters.virtualBackingQueries.fetch_add(1, std::memory_order_relaxed);
    g_Counters.virtualBackingQueryTicks.fetch_add(elapsed, std::memory_order_relaxed);
    updateMax(g_Counters.virtualBackingQueryMaxTicks, elapsed);
  } else {
    g_Counters.regularBackingQueries.fetch_add(1, std::memory_order_relaxed);
    g_Counters.regularBackingQueryTicks.fetch_add(elapsed, std::memory_order_relaxed);
    updateMax(g_Counters.regularBackingQueryMaxTicks, elapsed);
  }
  const auto status = static_cast<unsigned long>(result);
  if (status == 0x00000000UL)
    g_Counters.backingQuerySuccess.fetch_add(1, std::memory_order_relaxed);
  else if (status == 0x80000006UL)
    g_Counters.backingQueryNoMoreFiles.fetch_add(1, std::memory_order_relaxed);
  else
    g_Counters.backingQueryOtherStatus.fetch_add(1, std::memory_order_relaxed);
}

void emitSummary()
{
  if (!enabled())
    return;
  LARGE_INTEGER frequency{};
  ::QueryPerformanceFrequency(&frequency);
  auto logger = spdlog::get("usvfs");
  if (!logger)
    return;

  logger->info(
      "[profile] format=1 kind=context_lock pid={} qpc_frequency={} acquisitions={} "
      "reads={} writes={} recursive={} contended={} wait_ticks={} max_wait_ticks={} "
      "hold_ticks={} max_hold_ticks={} max_depth={}",
      ::GetCurrentProcessId(), frequency.QuadPart, g_Counters.lockAcquisitions.load(),
      g_Counters.lockReads.load(), g_Counters.lockWrites.load(),
      g_Counters.lockRecursive.load(), g_Counters.lockContended.load(),
      g_Counters.lockWaitTicks.load(), g_Counters.lockMaxWaitTicks.load(),
      g_Counters.lockHoldTicks.load(), g_Counters.lockMaxHoldTicks.load(),
      g_Counters.lockMaxDepth.load());

  logger->info(
      "[profile] format=1 kind=directory_query pid={} total={} legacy={} ex={} "
      "single={} restart={} pattern_null={} pattern_exact={} pattern_wildcard={} "
      "first_search={} virtual_remaining={} success={} no_more={} no_such={} "
      "other_status={} buffer_le_64={} buffer_le_256={} buffer_le_1024={} "
      "buffer_le_4096={} buffer_le_16384={} buffer_gt_16384={}",
      ::GetCurrentProcessId(), g_Counters.directoryQueries.load(),
      g_Counters.directoryLegacy.load(), g_Counters.directoryExtended.load(),
      g_Counters.directorySingle.load(), g_Counters.directoryRestart.load(),
      g_Counters.directoryNullPattern.load(), g_Counters.directoryExactPattern.load(),
      g_Counters.directoryWildcardPattern.load(),
      g_Counters.directoryFirstSearch.load(),
      g_Counters.directoryVirtualRemaining.load(), g_Counters.directorySuccess.load(),
      g_Counters.directoryNoMoreFiles.load(), g_Counters.directoryNoSuchFile.load(),
      g_Counters.directoryOtherStatus.load(),
      g_Counters.directoryBufferBuckets[0].load(),
      g_Counters.directoryBufferBuckets[1].load(),
      g_Counters.directoryBufferBuckets[2].load(),
      g_Counters.directoryBufferBuckets[3].load(),
      g_Counters.directoryBufferBuckets[4].load(),
      g_Counters.directoryBufferBuckets[5].load());

  logger->info("[profile] format=1 kind=directory_work pid={} qpc_frequency={} "
               "parent_opens={} parent_open_failures={} parent_open_ticks={} "
               "parent_open_max_ticks={} regular_queries={} regular_query_ticks={} "
               "regular_query_max_ticks={} virtual_queries={} virtual_query_ticks={} "
               "virtual_query_max_ticks={} backing_success={} backing_no_more={} "
               "backing_other_status={}",
               ::GetCurrentProcessId(), frequency.QuadPart,
               g_Counters.parentDirectoryOpens.load(),
               g_Counters.parentDirectoryOpenFailures.load(),
               g_Counters.parentDirectoryOpenTicks.load(),
               g_Counters.parentDirectoryOpenMaxTicks.load(),
               g_Counters.regularBackingQueries.load(),
               g_Counters.regularBackingQueryTicks.load(),
               g_Counters.regularBackingQueryMaxTicks.load(),
               g_Counters.virtualBackingQueries.load(),
               g_Counters.virtualBackingQueryTicks.load(),
               g_Counters.virtualBackingQueryMaxTicks.load(),
               g_Counters.backingQuerySuccess.load(),
               g_Counters.backingQueryNoMoreFiles.load(),
               g_Counters.backingQueryOtherStatus.load());

  for (size_t i = 0; i < InformationClassCount; ++i) {
    const auto count = g_Counters.directoryInformationClasses[i].load();
    if (count != 0) {
      logger->info("[profile] format=1 kind=directory_information_class pid={} "
                   "class={} count={}",
                   ::GetCurrentProcessId(), i, count);
    }
  }
  for (const auto& source : g_Counters.sources) {
    const char* name        = source.source.load();
    const auto acquisitions = source.acquisitions.load();
    if (name != nullptr && acquisitions != 0) {
      logger->info("[profile] format=1 kind=context_lock_source pid={} source={} "
                   "acquisitions={} contended={} wait_ticks={} max_wait_ticks={}",
                   ::GetCurrentProcessId(), name, acquisitions, source.contended.load(),
                   source.waitTicks.load(), source.maxWaitTicks.load());
    }
  }
}

}  // namespace usvfs::profiling
