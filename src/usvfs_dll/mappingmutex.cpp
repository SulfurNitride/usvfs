#include "mappingmutex.h"

#include <logging.h>
#include <windows_sane.h>

#include <cstdint>
#include <format>
#include <stdexcept>

namespace usvfs
{

namespace
{
  std::uint64_t hashInstanceName(const char* instanceName)
  {
    constexpr std::uint64_t Offset = 14695981039346656037ull;
    constexpr std::uint64_t Prime  = 1099511628211ull;

    std::uint64_t result = Offset;
    for (const auto* current = reinterpret_cast<const unsigned char*>(instanceName);
         *current != 0; ++current) {
      result ^= *current;
      result *= Prime;
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
}

void InterprocessMappingMutex::lockShared()
{
  if (!m_Enabled) {
    return;
  }

  acquire(m_Gate, "writer gate");
  const std::size_t stripe = ::GetCurrentThreadId() % m_Stripes.size();
  try {
    acquire(m_Stripes[stripe], "reader stripe");
  } catch (...) {
    release(m_Gate, "writer gate");
    throw;
  }
  release(m_Gate, "writer gate");
}

void InterprocessMappingMutex::unlockShared()
{
  if (!m_Enabled) {
    return;
  }

  const std::size_t stripe = ::GetCurrentThreadId() % m_Stripes.size();
  release(m_Stripes[stripe], "reader stripe");
}

void InterprocessMappingMutex::lockExclusive()
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
  } catch (...) {
    while (acquired > 0) {
      release(m_Stripes[--acquired], "reader stripe");
    }
    release(m_Gate, "writer gate");
    throw;
  }
  release(m_Gate, "writer gate");
}

void InterprocessMappingMutex::unlockExclusive()
{
  if (!m_Enabled) {
    return;
  }

  for (std::size_t i = m_Stripes.size(); i > 0; --i) {
    release(m_Stripes[i - 1], "reader stripe");
  }
}

std::wstring InterprocessMappingMutex::objectName(const char* instanceName,
                                                  const wchar_t* kind,
                                                  std::size_t stripe)
{
  return std::format(L"fluorine-usvfs-mapping-{:016x}-{}-{}-{}",
                     hashInstanceName(instanceName), sizeof(void*) * 8, kind, stripe);
}

void InterprocessMappingMutex::acquire(HANDLE handle, const char* kind)
{
  const DWORD result = ::WaitForSingleObject(handle, INFINITE);
  if (result == WAIT_OBJECT_0) {
    return;
  }
  if (result == WAIT_ABANDONED) {
    if (auto logger = spdlog::get("usvfs")) {
      logger->warn("recovered abandoned mapping {}", kind);
    }
    return;
  }
  throw std::runtime_error("failed to acquire interprocess mapping mutex");
}

void InterprocessMappingMutex::release(HANDLE handle, const char* kind) noexcept
{
  if (::ReleaseMutex(handle) == FALSE) {
    if (auto logger = spdlog::get("usvfs")) {
      logger->error("failed to release mapping {}", kind);
    }
  }
}

}  // namespace usvfs
