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
  DWORD tid = ::GetCurrentThreadId();
  BenaphoreWaitKind result = BenaphoreWaitKind::Uncontended;

  if (::_InterlockedIncrement(&m_Counter) > 1) {
    if (tid != m_OwnerId) {
      result = BenaphoreWaitKind::Contended;
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
