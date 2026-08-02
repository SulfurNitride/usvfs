#pragma once

#include <array>
#include <string>
#include <windows_sane.h>

namespace usvfs
{

/**
 * @brief Crash-recoverable interprocess reader/writer boundary for mapping trees.
 *
 * Readers briefly pass through a named writer gate and then own one named mutex
 * stripe. Writers keep the gate while acquiring every stripe. Windows abandons an
 * owned mutex when its thread or process exits, so abandoned readers can be
 *
 * recovered instead of leaving a shared reader count permanently stranded. A
 *
 * manual-reset write-state event keeps an interrupted writer fail-closed rather
 * than
 * exposing a tree that may have been left mid-mutation.
 */
class InterprocessMappingMutex
{
public:
  static constexpr std::size_t StripeCount = 64;

  InterprocessMappingMutex(const char* instanceName, bool enabled);
  ~InterprocessMappingMutex();

  InterprocessMappingMutex(const InterprocessMappingMutex&)            = delete;
  InterprocessMappingMutex& operator=(const InterprocessMappingMutex&) = delete;

  void lockShared() const;
  void unlockShared() const;
  void lockExclusive() const;
  void unlockExclusive(bool commit = true) const noexcept;

private:
  static std::wstring objectName(const char* instanceName, const wchar_t* kind,
                                 std::size_t stripe = 0);
  static void acquire(HANDLE handle, const char* kind);
  static void release(HANDLE handle, const char* kind) noexcept;
  void ensureWriteStateIsClean() const;

private:
  bool m_Enabled;
  HANDLE m_Gate{nullptr};
  HANDLE m_WriteInProgress{nullptr};
  std::array<HANDLE, StripeCount> m_Stripes{};
};

}  // namespace usvfs
