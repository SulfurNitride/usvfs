/*
Userspace Virtual Filesystem

Copyright (C) 2015 Sebastian Herbord. All rights reserved.

This file is part of usvfs.

usvfs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

usvfs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with usvfs. If not, see <http://www.gnu.org/licenses/>.
*/
#include "hookcontext.h"
#include "exceptionex.h"
#include "hookcallcontext.h"
#include "loghelpers.h"
#include "profiling.h"
#include "usvfs.h"
#include <shared_memory.h>
#include <sharedparameters.h>
#include <usvfsparameters.h>
#include <winapi.h>

namespace bi = boost::interprocess;
using usvfs::shared::SharedMemoryT;
using usvfs::shared::VoidAllocatorT;

using namespace usvfs;
namespace ush = usvfs::shared;

namespace
{
bool sharedContextLockEnabled()
{
  static const bool enabled = []() {
    wchar_t value[16]{};
    const DWORD length = ::GetEnvironmentVariableW(L"FLUORINE_USVFS_SHARED_CONTEXT",
                                                   value, ARRAYSIZE(value));
    return length > 0 && length < ARRAYSIZE(value) && _wcsicmp(value, L"0") != 0 &&
           _wcsicmp(value, L"false") != 0 && _wcsicmp(value, L"off") != 0;
  }();
  return enabled;
}

const char* mappingTreeName(MappingTree tree)
{
  switch (tree) {
  case MappingTree::Redirection:
    return "redirection";
  case MappingTree::Inverse:
    return "inverse";
  default:
    return "unknown";
  }
}

const char* mappingMutationName(MappingMutation mutation)
{
  switch (mutation) {
  case MappingMutation::Clear:
    return "clear";
  case MappingMutation::AddFile:
    return "add_file";
  case MappingMutation::AddDirectory:
    return "add_directory";
  case MappingMutation::Remove:
    return "remove";
  default:
    return "unknown";
  }
}
}  // namespace

HookContext* HookContext::s_Instance = nullptr;

void printBuffer(const char* buffer, size_t size)
{
  static const int bufferSize = 16 * 3;
  char temp[bufferSize + 1];
  temp[bufferSize] = '\0';

  for (size_t i = 0; i < size; ++i) {
    size_t offset = i % 16;
    _snprintf(&temp[offset * 3], 3, "%02x ", (unsigned char)buffer[i]);
    if (offset == 15) {
      spdlog::get("hooks")->info("{0:x} - {1}", i - offset, temp);
    }
  }

  spdlog::get("hooks")->info(temp);
}

HookContext::HookContext(const usvfsParameters& params, HMODULE module)
    : m_ConfigurationSHM(bi::open_or_create, params.instanceName, 64 * 1024),
      m_Parameters(retrieveParameters(params)),
      m_MappingMutex(params.instanceName, sharedContextLockEnabled()),
      m_InitialMappingReadLock(&m_MappingMutex, sharedContextLockEnabled()),
      m_MappingsPublishedLocally(m_Parameters->mappingsPublished()),
      m_Tree(m_Parameters->currentSHMName(),
             4 * 1024 * 1024,  // 4 MiB empirically covers most small setups without
                               // need to resize
             [this](shared::TreeMutation mutation) {
               observeMappingMutation(MappingTree::Redirection, mutation);
             }),
      m_InverseTree(
          m_Parameters->currentInverseSHMName(),
          128 * 1024,  // 128 KiB should cover reverse tree for even larger setups
          [this](shared::TreeMutation mutation) {
            observeMappingMutation(MappingTree::Inverse, mutation);
          }),
      m_DLLModule(module)
{
  if (s_Instance != nullptr) {
    throw std::runtime_error("singleton duplicate instantiation (HookContext)");
  }

  const auto userCount = m_Parameters->userConnected();

  spdlog::get("usvfs")->debug("context current shm: {0} (now {1} connections)",
                              m_Parameters->currentSHMName(), userCount);

  s_Instance = this;

  if (profiling::enabled()) {
    profiling::reset();
  }

  if (m_Tree.get() == nullptr) {
    USVFS_THROW_EXCEPTION(usage_error()
                          << ex_msg("shm not found") << ex_msg(params.instanceName));
  }

  m_InitialMappingReadLock.release();
}

void HookContext::remove(const char* instanceName)
{
  bi::shared_memory_object::remove(instanceName);
}

HookContext::~HookContext()
{
  emitMappingPublicationSummary();
  profiling::emitSummary();
  spdlog::get("usvfs")->info("releasing hook context");

  s_Instance           = nullptr;
  const auto userCount = m_Parameters->userDisconnected();

  if (userCount == 0) {
    spdlog::get("usvfs")->info("removing tree {}", m_Parameters->instanceName());
    bi::shared_memory_object::remove(m_Parameters->instanceName().c_str());
  } else {
    spdlog::get("usvfs")->info("{} users left", userCount);
  }
}

void HookContext::publishMappings() const noexcept
{
  try {
    const bool firstPublication = m_Parameters->publishMappings();
    m_MappingsPublishedLocally.store(true, std::memory_order_release);
    if (firstPublication) {
      spdlog::get("usvfs")->info(
          "[tree-publication] boundary published by process {}; mutations remain "
          "enabled",
          ::GetCurrentProcessId());
    }
  } catch (const std::exception& e) {
    spdlog::get("usvfs")->error("[tree-publication] failed to publish boundary: {}",
                                e.what());
  }
}

void HookContext::recordMappingRemoval(MappingTree tree) noexcept
{
  recordMappingMutation(tree, MappingMutation::Remove);
}

void HookContext::observeMappingMutation(MappingTree tree,
                                         shared::TreeMutation mutation) noexcept
{
  switch (mutation) {
  case shared::TreeMutation::Clear:
    recordMappingMutation(tree, MappingMutation::Clear);
    break;
  case shared::TreeMutation::AddFile:
    recordMappingMutation(tree, MappingMutation::AddFile);
    break;
  case shared::TreeMutation::AddDirectory:
    recordMappingMutation(tree, MappingMutation::AddDirectory);
    break;
  }
}

void HookContext::recordMappingMutation(MappingTree tree,
                                        MappingMutation mutation) noexcept
{
  if (!m_MappingsPublishedLocally.load(std::memory_order_acquire)) {
    return;
  }

  try {
    const auto mutationCount = m_Parameters->recordMappingMutation(tree, mutation);
    if (mutationCount == 1) {
      spdlog::get("usvfs")->warn(
          "[tree-publication] first post-publication mutation in process {}: "
          "tree={} operation={}",
          ::GetCurrentProcessId(), mappingTreeName(tree),
          mappingMutationName(mutation));
    }
  } catch (const std::exception& e) {
    spdlog::get("usvfs")->error("[tree-publication] failed to record mutation: {}",
                                e.what());
  }
}

void HookContext::emitMappingPublicationSummary() const noexcept
{
  try {
    const auto stats = m_Parameters->mappingPublicationStats();
    spdlog::get("usvfs")->info(
        "[tree-publication] summary published={} publish_calls={} "
        "post_publish_mutations={} redirection={} inverse={} clear={} add_file={} "
        "add_directory={} remove={}",
        stats.published, stats.publishCalls, stats.postPublishMutations,
        stats.byTree.at(static_cast<std::size_t>(MappingTree::Redirection)),
        stats.byTree.at(static_cast<std::size_t>(MappingTree::Inverse)),
        stats.byMutation.at(static_cast<std::size_t>(MappingMutation::Clear)),
        stats.byMutation.at(static_cast<std::size_t>(MappingMutation::AddFile)),
        stats.byMutation.at(static_cast<std::size_t>(MappingMutation::AddDirectory)),
        stats.byMutation.at(static_cast<std::size_t>(MappingMutation::Remove)));
  } catch (const std::exception& e) {
    spdlog::get("usvfs")->error("[tree-publication] failed to emit summary: {}",
                                e.what());
  }
}

SharedParameters* HookContext::retrieveParameters(const usvfsParameters& params)
{
  std::pair<SharedParameters*, SharedMemoryT::size_type> res =
      m_ConfigurationSHM.find<SharedParameters>("parameters");

  if (res.first == nullptr) {
    // not configured yet
    spdlog::get("usvfs")->info("create config in {}", ::GetCurrentProcessId());

    res.first = m_ConfigurationSHM.construct<SharedParameters>("parameters")(
        params, VoidAllocatorT(m_ConfigurationSHM.get_segment_manager()));

    if (res.first == nullptr) {
      USVFS_THROW_EXCEPTION(bi::bad_alloc());
    }
  } else {
    spdlog::get("usvfs")->info("access existing config in {}", ::GetCurrentProcessId());
  }

  spdlog::get("usvfs")->info("{} processes", res.first->registeredProcessCount());

  return res.first;
}

HookContext::ConstPtr HookContext::readAccess(const char* source)
{
  BOOST_ASSERT(s_Instance != nullptr);

  const auto waitStarted = profiling::beginLockWait();
  BenaphoreWaitKind waitKind;
  if (sharedContextLockEnabled()) {
    const bool outermost = !s_Instance->m_SharedMutex.heldByCurrentThread();
    if (outermost) {
      s_Instance->m_MappingMutex.lockShared();
    }

    try {
      waitKind = s_Instance->m_SharedMutex.lockShared();
      if (outermost) {
        s_Instance->m_Tree.refresh();
        s_Instance->m_InverseTree.refresh();
      }
    } catch (...) {
      if (s_Instance->m_SharedMutex.heldByCurrentThread()) {
        s_Instance->m_SharedMutex.unlockShared();
      }
      if (outermost) {
        s_Instance->m_MappingMutex.unlockShared();
      }
      throw;
    }
  } else {
    waitKind = s_Instance->m_Mutex.wait(200);
  }
  profiling::lockAcquired(waitStarted, waitKind, false, source);
  return ConstPtr(s_Instance, unlockShared);
}

HookContext::Ptr HookContext::writeAccess(const char* source)
{
  BOOST_ASSERT(s_Instance != nullptr);

  const auto waitStarted = profiling::beginLockWait();
  BenaphoreWaitKind waitKind;
  if (sharedContextLockEnabled()) {
    const bool outermost = !s_Instance->m_SharedMutex.heldByCurrentThread();
    if (outermost) {
      s_Instance->m_MappingMutex.lockExclusive();
    }

    try {
      waitKind = s_Instance->m_SharedMutex.lockExclusive();
    } catch (...) {
      if (outermost) {
        s_Instance->m_MappingMutex.unlockExclusive();
      }
      throw;
    }
  } else {
    waitKind = s_Instance->m_Mutex.wait(200);
  }
  profiling::lockAcquired(waitStarted, waitKind, true, source);
  return Ptr(s_Instance, unlock);
}

void HookContext::setDebugParameters(LogLevel level, CrashDumpsType dumpType,
                                     const std::string& dumpPath,
                                     std::chrono::milliseconds delayProcess)
{
  m_Parameters->setDebugParameters(level, dumpType, dumpPath, delayProcess);
}

void HookContext::updateParameters() const
{
  m_Parameters->setSHMNames(m_Tree.shmName(), m_InverseTree.shmName());
}

usvfsParameters HookContext::callParameters() const
{
  updateParameters();
  return m_Parameters->makeLocal();
}

std::wstring HookContext::dllPath() const
{
  std::wstring path = winapi::wide::getModuleFileName(m_DLLModule);
  return boost::filesystem::path(path).parent_path().make_preferred().wstring();
}

void HookContext::registerProcess(DWORD pid)
{
  m_Parameters->registerProcess(pid);
}

void HookContext::unregisterCurrentProcess()
{
  m_Parameters->unregisterProcess(::GetCurrentProcessId());
}

std::vector<DWORD> HookContext::registeredProcesses() const
{
  return m_Parameters->registeredProcesses();
}

void HookContext::blacklistExecutable(const std::wstring& wexe)
{
  const auto exe = shared::string_cast<std::string>(wexe, shared::CodePage::UTF8);

  spdlog::get("usvfs")->debug("blacklisting '{}'", exe);
  m_Parameters->blacklistExecutable(exe);
}

void HookContext::clearExecutableBlacklist()
{
  spdlog::get("usvfs")->debug("clearing blacklist");
  m_Parameters->clearExecutableBlacklist();
}

BOOL HookContext::executableBlacklisted(LPCWSTR wapp, LPCWSTR wcmd) const
{
  std::string app;
  if (wapp) {
    app = ush::string_cast<std::string>(wapp, ush::CodePage::UTF8);
  }

  std::string cmd;
  if (wcmd) {
    cmd = ush::string_cast<std::string>(wcmd, ush::CodePage::UTF8);
  }

  return m_Parameters->executableBlacklisted(app, cmd);
}

void usvfs::HookContext::addSkipFileSuffix(const std::wstring& fileSuffix)
{
  const auto fsuffix =
      shared::string_cast<std::string>(fileSuffix, shared::CodePage::UTF8);

  if (fsuffix.empty()) {
    return;
  }

  spdlog::get("usvfs")->debug("added skip file suffix '{}'", fsuffix);
  m_Parameters->addSkipFileSuffix(fsuffix);
}

void usvfs::HookContext::clearSkipFileSuffixes()
{
  spdlog::get("usvfs")->debug("clearing skip file suffixes");
  m_Parameters->clearSkipFileSuffixes();
}

std::vector<std::string> usvfs::HookContext::skipFileSuffixes() const
{
  return m_Parameters->skipFileSuffixes();
}

void usvfs::HookContext::addSkipDirectory(const std::wstring& directory)
{
  const auto dir = shared::string_cast<std::string>(directory, shared::CodePage::UTF8);

  if (dir.empty()) {
    return;
  }

  spdlog::get("usvfs")->debug("added skip directory '{}'", dir);
  m_Parameters->addSkipDirectory(dir);
}

void usvfs::HookContext::clearSkipDirectories()
{
  spdlog::get("usvfs")->debug("clearing skip directories");
  m_Parameters->clearSkipDirectories();
}

std::vector<std::string> usvfs::HookContext::skipDirectories() const
{
  return m_Parameters->skipDirectories();
}

void HookContext::forceLoadLibrary(const std::wstring& wprocess,
                                   const std::wstring& wpath)
{
  const auto process =
      shared::string_cast<std::string>(wprocess, shared::CodePage::UTF8);

  const auto path = shared::string_cast<std::string>(wpath, shared::CodePage::UTF8);

  spdlog::get("usvfs")->debug("adding forced library '{}' for process '{}'", path,
                              process);

  m_Parameters->addForcedLibrary(process, path);
}

void HookContext::clearLibraryForceLoads()
{
  spdlog::get("usvfs")->debug("clearing forced libraries");
  m_Parameters->clearForcedLibraries();
}

std::vector<std::wstring>
HookContext::librariesToForceLoad(const std::wstring& processName)
{
  const auto v = m_Parameters->forcedLibraries(
      shared::string_cast<std::string>(processName, shared::CodePage::UTF8));

  std::vector<std::wstring> wv;
  for (const auto& s : v) {
    wv.push_back(shared::string_cast<std::wstring>(s, shared::CodePage::UTF8));
  }

  return wv;
}

void HookContext::registerDelayed(std::future<int> delayed)
{
  m_Futures.push_back(std::move(delayed));
}

std::vector<std::future<int>>& HookContext::delayed()
{
  return m_Futures;
}

void HookContext::unlock(HookContext* instance)
{
  profiling::lockReleased();
  if (sharedContextLockEnabled()) {
    if (instance->m_SharedMutex.unlockExclusive()) {
      instance->m_MappingMutex.unlockExclusive();
    }
  } else {
    instance->m_Mutex.signal();
  }
}

void HookContext::unlockShared(const HookContext* instance)
{
  profiling::lockReleased();
  if (sharedContextLockEnabled()) {
    if (instance->m_SharedMutex.unlockShared()) {
      instance->m_MappingMutex.unlockShared();
    }
  } else {
    instance->m_Mutex.signal();
  }
}

// deprecated
//
extern "C" DLLEXPORT HookContext* __cdecl
CreateHookContext(const USVFSParameters& oldParams, HMODULE module)
{
  const usvfsParameters p(oldParams);
  return usvfsCreateHookContext(p, module);
}

extern "C" DLLEXPORT usvfs::HookContext* WINAPI
usvfsCreateHookContext(const usvfsParameters& params, HMODULE module)
{
  return new HookContext(params, module);
}
