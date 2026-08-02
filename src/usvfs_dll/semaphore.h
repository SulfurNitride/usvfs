#pragma once

#include <atomic>
#include <stdexcept>

// based on code by Jeff Preshing

// this is a synchronization class that prefers
// undefined behaviour over deadlock. It's utterly broken
// and needs to be replaced in time.

enum class BenaphoreWaitKind
{
  Uncontended,
  Recursive,
  Contended
};

class RecursiveBenaphore
{

public:
  RecursiveBenaphore();
  ~RecursiveBenaphore();

  // wait on the semaphore. after timeout this will check if the current owner
  // thread is still alive and steal the semaphore if it isn't. Otherwise this
  // will continue to wait.
  BenaphoreWaitKind wait(DWORD timeout = INFINITE);
  void signal();

private:
  LONG m_Counter;
  DWORD m_OwnerId;
  int m_Recursion;
  HANDLE m_Semaphore;
};

// Recursive reader/writer lock for HookContext's read-mostly redirection tree.
// A writer may enter a read section recursively. Upgrading a held shared lock
// to exclusive is deliberately unsupported and asserted in debug builds.
class RecursiveSharedMutex
{
public:
  RecursiveSharedMutex();

  BenaphoreWaitKind lockShared();
  BenaphoreWaitKind lockExclusive();
  bool unlockShared();
  bool unlockExclusive();
  bool heldByCurrentThread() const;

private:
  SRWLOCK m_Lock;
  std::atomic<DWORD> m_ExclusiveOwner{0};

  static thread_local RecursiveSharedMutex* s_CurrentLock;
  static thread_local unsigned int s_SharedDepth;
  static thread_local unsigned int s_ExclusiveDepth;
};
