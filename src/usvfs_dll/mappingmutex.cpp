#include "mappingmutex.h"

#include <logging.h>
#include <windows_sane.h>

#include <format>
#include <stdexcept>

namespace usvfs
{

namespace
{
  std::wstring encodeInstanceName(const char* instanceName)
  {
    constexpr wchar_t Hex[] = L"0123456789abcdef";

    std::wstring result;
    result.reserve(128);
    for (const auto* current = reinterpret_cast<const unsigned char*>(instanceName);
         *current != 0; ++current) {
      result.push_back(Hex[*current >> 4]);
      result.push_back(Hex[*current & 0x0f]);
    }
    return result;
  }
}  // namespace

InterprocessMappingMutex::InterprocessMappingMutex(const char* instanceName,
                                                   bool enabled)
    : m_Enabled(enabled)
{
  if (!m_Enabled) {
    return;
  }

  try {
    m_Gate = ::CreateMutexW(nullptr, FALSE, objectName(instanceName, L"gate").c_str());
    if (m_Gate == nullptr) {
      throw std::runtime_error("failed to create mapping writer gate");
    }

    m_WriteInProgress = ::CreateEventW(
        nullptr, TRUE, FALSE, objectName(instanceName, L"write-in-progress").c_str());
    if (m_WriteInProgress == nullptr) {
      throw std::runtime_error("failed to create mapping write-state event");
    }

    for (std::size_t i = 0; i < m_Stripes.size(); ++i) {
      m_Stripes[i] = ::CreateMutexW(nullptr, FALSE,
                                    objectName(instanceName, L"stripe", i).c_str());
      if (m_Stripes[i] == nullptr) {
        throw std::runtime_error("failed to create mapping reader stripe");
      }
    }
  } catch (...) {
    for (HANDLE stripe : m_Stripes) {
      if (stripe != nullptr) {
        ::CloseHandle(stripe);
      }
    }
    if (m_Gate != nullptr) {
      ::CloseHandle(m_Gate);
    }
    if (m_WriteInProgress != nullptr) {
      ::CloseHandle(m_WriteInProgress);
    }
    throw;
  }
}

InterprocessMappingMutex::~InterprocessMappingMutex()
{
  for (HANDLE stripe : m_Stripes) {
    if (stripe != nullptr) {
      ::CloseHandle(stripe);
    }
  }
  if (m_Gate != nullptr) {
    ::CloseHandle(m_Gate);
  }
  if (m_WriteInProgress != nullptr) {
    ::CloseHandle(m_WriteInProgress);
  }
}

void InterprocessMappingMutex::lockShared() const
{
  if (!m_Enabled) {
    return;
  }

  acquire(m_Gate, "writer gate");
  const std::size_t stripe = ::GetCurrentThreadId() % m_Stripes.size();
  bool stripeAcquired      = false;
  try {
    acquire(m_Stripes[stripe], "reader stripe");
    stripeAcquired = true;
    ensureWriteStateIsClean();
  } catch (...) {
    if (stripeAcquired) {
      release(m_Stripes[stripe], "reader stripe");
    }
    release(m_Gate, "writer gate");
    throw;
  }
  release(m_Gate, "writer gate");
}

void InterprocessMappingMutex::unlockShared() const
{
  if (!m_Enabled) {
    return;
  }

  const std::size_t stripe = ::GetCurrentThreadId() % m_Stripes.size();
  release(m_Stripes[stripe], "reader stripe");
}

void InterprocessMappingMutex::lockExclusive() const
{
  if (!m_Enabled) {
    return;
  }

  acquire(m_Gate, "writer gate");
  std::size_t acquired = 0;
  try {
    for (; acquired < m_Stripes.size(); ++acquired) {
      acquire(m_Stripes[acquired], "reader stripe");
    }
    ensureWriteStateIsClean();
    if (::SetEvent(m_WriteInProgress) == FALSE) {
      throw std::runtime_error("failed to mark mapping write in progress");
    }
  } catch (...) {
    while (acquired > 0) {
      release(m_Stripes[--acquired], "reader stripe");
    }
    release(m_Gate, "writer gate");
    throw;
  }
  release(m_Gate, "writer gate");
}

void InterprocessMappingMutex::unlockExclusive(bool commit) const noexcept
{
  if (!m_Enabled) {
    return;
  }

  if (commit) {
    if (::ResetEvent(m_WriteInProgress) == FALSE) {
      try {
        if (auto logger = spdlog::get("usvfs")) {
          logger->error("failed to commit interprocess mapping write state");
        }
      } catch (...) {
      }
    }
  } else {
    try {
      if (auto logger = spdlog::get("usvfs")) {
        logger->critical("mapping write failed; refusing further access to this "
                         "mapping instance");
      }
    } catch (...) {
    }
  }

  for (std::size_t i = m_Stripes.size(); i > 0; --i) {
    release(m_Stripes[i - 1], "reader stripe");
  }
}

void InterprocessMappingMutex::ensureWriteStateIsClean() const
{
  const DWORD state = ::WaitForSingleObject(m_WriteInProgress, 0);
  if (state == WAIT_TIMEOUT) {
    return;
  }
  if (state == WAIT_OBJECT_0) {
    throw std::runtime_error("mapping instance was left in an incomplete write state");
  }
  throw std::runtime_error("failed to inspect interprocess mapping write state");
}

std::wstring InterprocessMappingMutex::objectName(const char* instanceName,
                                                  const wchar_t* kind,
                                                  std::size_t stripe)
{
  return std::format(L"fluorine-usvfs-mapping-{}-{}-{}-{}",
                     encodeInstanceName(instanceName), sizeof(void*) * 8, kind, stripe);
}

void InterprocessMappingMutex::acquire(HANDLE handle, const char* kind)
{
  const DWORD result = ::WaitForSingleObject(handle, INFINITE);
  if (result == WAIT_OBJECT_0) {
    return;
  }
  if (result == WAIT_ABANDONED) {
    try {
      if (auto logger = spdlog::get("usvfs")) {
        logger->warn("recovered abandoned mapping {}", kind);
      }
    } catch (...) {
      // Recovery already owns the mutex; diagnostics must not leak that ownership.
    }
    return;
  }
  throw std::runtime_error("failed to acquire interprocess mapping mutex");
}

void InterprocessMappingMutex::release(HANDLE handle, const char* kind) noexcept
{
  if (::ReleaseMutex(handle) == FALSE) {
    try {
      if (auto logger = spdlog::get("usvfs")) {
        logger->error("failed to release mapping {}", kind);
      }
    } catch (...) {
      // Release paths are noexcept and must remain safe during stack unwinding.
    }
  }
}

}  // namespace usvfs
