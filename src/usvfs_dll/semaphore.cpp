#include "semaphore.h"
#include "exceptionex.h"

RecursiveBenaphore::RecursiveBenaphore() : m_Counter(0), m_OwnerId(0UL), m_Recursion(0)
{
  // The counter represents owners plus waiters. The first owner enters without
  // waiting; therefore the semaphore must begin non-signaled and be released
  // only when that owner leaves while a waiter exists.
  m_Semaphore = ::CreateSemaphore(nullptr, 0, 1, nullptr);
}

RecursiveBenaphore::~RecursiveBenaphore()
{
  ::CloseHandle(m_Semaphore);
}

BenaphoreWaitKind RecursiveBenaphore::wait(DWORD timeout)
{
  DWORD tid                = ::GetCurrentThreadId();
  BenaphoreWaitKind result = BenaphoreWaitKind::Uncontended;

  if (::_InterlockedIncrement(&m_Counter) > 1) {
    if (tid != m_OwnerId) {
      result    = BenaphoreWaitKind::Contended;
      int tries = 3;
      while (::WaitForSingleObject(m_Semaphore, timeout) != WAIT_OBJECT_0) {
        HANDLE owner = ::OpenThread(SYNCHRONIZE, FALSE, m_OwnerId);
        ON_BLOCK_EXIT([owner]() {
          ::CloseHandle(owner);
        });
        if ((tries <= 0) || (::WaitForSingleObject(owner, 0) == WAIT_OBJECT_0)) {
          // owner has quit without releasing the semaphore!
          m_Recursion = 0;
          spdlog::get("usvfs")->error("thread {} never released the mutex", m_OwnerId);
          break;
        } else {
          --tries;
        }
      }
    } else {
      result = BenaphoreWaitKind::Recursive;
    }
  }
  m_OwnerId = tid;
  ++m_Recursion;
  return result;
}

void RecursiveBenaphore::signal()
{
  if (m_Recursion == 0) {
    return;
  }
  // no validation the signaling thread is the one owning the lock
  DWORD recursion = --m_Recursion;
  if (recursion == 0) {
    m_OwnerId = 0;
  }
  DWORD result = ::_InterlockedDecrement(&m_Counter);
  if (result > 0) {
    if (recursion == 0) {
      ::ReleaseSemaphore(m_Semaphore, 1, nullptr);
    }
  }
}

thread_local RecursiveSharedMutex* RecursiveSharedMutex::s_CurrentLock = nullptr;
thread_local unsigned int RecursiveSharedMutex::s_SharedDepth          = 0;
thread_local unsigned int RecursiveSharedMutex::s_ExclusiveDepth       = 0;

RecursiveSharedMutex::RecursiveSharedMutex()
{
  ::InitializeSRWLock(&m_Lock);
}

BenaphoreWaitKind RecursiveSharedMutex::lockShared()
{
  if (s_CurrentLock == this) {
    BOOST_ASSERT(s_SharedDepth > 0 || s_ExclusiveDepth > 0);
    ++s_SharedDepth;
    return BenaphoreWaitKind::Recursive;
  }
  if (s_CurrentLock != nullptr)
    throw std::logic_error("nested recursive shared mutexes are unsupported");

  BenaphoreWaitKind result = BenaphoreWaitKind::Uncontended;
  if (!::TryAcquireSRWLockShared(&m_Lock)) {
    result = BenaphoreWaitKind::Contended;
    ::AcquireSRWLockShared(&m_Lock);
  }
  s_CurrentLock = this;
  s_SharedDepth = 1;
  return result;
}

BenaphoreWaitKind RecursiveSharedMutex::lockExclusive()
{
  if (s_CurrentLock == this) {
    if (s_ExclusiveDepth == 0)
      throw std::logic_error("recursive shared mutex upgrade is unsupported");
    ++s_ExclusiveDepth;
    return BenaphoreWaitKind::Recursive;
  }
  if (s_CurrentLock != nullptr)
    throw std::logic_error("nested recursive shared mutexes are unsupported");

  BenaphoreWaitKind result = BenaphoreWaitKind::Uncontended;
  if (!::TryAcquireSRWLockExclusive(&m_Lock)) {
    result = BenaphoreWaitKind::Contended;
    ::AcquireSRWLockExclusive(&m_Lock);
  }
  s_CurrentLock    = this;
  s_ExclusiveDepth = 1;
  m_ExclusiveOwner.store(::GetCurrentThreadId(), std::memory_order_release);
  return result;
}

bool RecursiveSharedMutex::unlockShared()
{
  BOOST_ASSERT(s_CurrentLock == this && s_SharedDepth > 0);
  if (--s_SharedDepth != 0)
    return false;
  if (s_ExclusiveDepth != 0)
    return false;

  s_CurrentLock = nullptr;
  ::ReleaseSRWLockShared(&m_Lock);
  return true;
}

bool RecursiveSharedMutex::unlockExclusive()
{
  BOOST_ASSERT(s_CurrentLock == this && s_ExclusiveDepth > 0);
  BOOST_ASSERT(m_ExclusiveOwner.load(std::memory_order_acquire) ==
               ::GetCurrentThreadId());
  if (--s_ExclusiveDepth != 0)
    return false;

  BOOST_ASSERT(s_SharedDepth == 0);
  m_ExclusiveOwner.store(0, std::memory_order_release);
  s_CurrentLock = nullptr;
  ::ReleaseSRWLockExclusive(&m_Lock);
  return true;
}

bool RecursiveSharedMutex::heldByCurrentThread() const
{
  return s_CurrentLock == this;
}
