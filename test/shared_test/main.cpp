#include <boost/interprocess/containers/string.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/predef.h>
#include <gtest/gtest.h>
#include <loghelpers.h>
#include <shared_memory.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <wildcard.h>
#include <windows_sane.h>

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <sstream>
#include <thread>
#include <vector>

#define PRIVATE public
#include <directory_tree.h>
#undef PRIVATE

#include <tree_container.h>

using namespace usvfs::shared;

using namespace boost::interprocess;

static const char g_SHMName[] = "treetest_shm";
static const char TreeName[]  = "treetest_tree";

typedef DirectoryTree<int> TreeType;
typedef TreeContainer<TreeType> ContainerType;

typedef boost::container::scoped_allocator_adaptor<
    boost::interprocess::allocator<void, SegmentManagerT>>
    VoidAllocator;
typedef VoidAllocator::rebind<char>::other CharAllocator;
typedef basic_string<char, std::char_traits<char>, CharAllocator> SHMString;
typedef DirectoryTree<SHMString> ComplexTreeType;
typedef TreeContainer<ComplexTreeType> ComplexContainerType;

template <>
struct usvfs::shared::SHMDataCreator<int, int>
{
  static int create(int source, const VoidAllocatorT&) { return source; }
};

template <>
inline int usvfs::shared::createDataEmpty<int>(const typename VoidAllocatorT&)
{
  return 0;
}

template <>
inline void usvfs::shared::dataAssign<int>(int& destination, const int& source)
{
  destination = source;
}

template <>
inline SHMString
usvfs::shared::createDataEmpty<SHMString>(const typename VoidAllocatorT& allocator)
{
  return SHMString("", allocator);
}

template <>
inline void usvfs::shared::dataAssign<SHMString>(SHMString& destination,
                                                 const SHMString& source)
{
  destination.assign(source.c_str());
}

static std::shared_ptr<spdlog::logger> logger()
{
  std::shared_ptr<spdlog::logger> result = spdlog::get("test");
  if (result.get() == nullptr) {
    result = spdlog::stdout_logger_mt("test");
  }
  return result;
}

TEST(WildcardTest, MatchWildcard)
{
  EXPECT_TRUE(wildcard::Match(TEXT("abc"), TEXT("a*")));
  EXPECT_TRUE(wildcard::Match(TEXT("abc"), TEXT("a*c")));
  EXPECT_TRUE(wildcard::Match(TEXT("abc"), TEXT("a?c")));
  EXPECT_TRUE(wildcard::Match(TEXT("abc"), TEXT("abc")));
  EXPECT_TRUE(wildcard::Match(TEXT("abc"), TEXT("abc*")));

  EXPECT_TRUE(wildcard::Match("abc", "*.*"));
  EXPECT_TRUE(wildcard::Match("abc.def", "*"));
  EXPECT_TRUE(wildcard::Match(TEXT("abc"), TEXT("*.*")));
  EXPECT_TRUE(wildcard::Match(TEXT("abc.def"), TEXT("*")));

  EXPECT_NE(nullptr, wildcard::PartialMatch("abc", "*.*"));
  EXPECT_NE(nullptr, wildcard::PartialMatch("abc.def", "*"));
  EXPECT_EQ('\0', *wildcard::PartialMatch("abc", "*.*"));
  EXPECT_EQ('\0', *wildcard::PartialMatch("abc.def", "*"));

  EXPECT_FALSE(wildcard::Match(TEXT("abc"), TEXT("b*")));
}

TEST(CallLoggerTest, DisabledFastPathTracksRuntimeState)
{
  std::ostringstream output;
  auto sink        = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
  auto hooksLogger = std::make_shared<spdlog::logger>("hooks", sink);
  hooksLogger->set_level(spdlog::level::debug);
  hooksLogger->set_pattern("%v");
  spdlog::register_logger(hooksLogger);

  usvfs::log::CallLogger::setEnabled(false);
  {
    usvfs::log::CallLogger("scope::disabled").addParam("value", 1);
  }
  hooksLogger->flush();
  EXPECT_TRUE(output.str().empty());

  usvfs::log::CallLogger::setEnabled(true);
  {
    usvfs::log::CallLogger("scope::enabled").addParam("value", 2);
  }
  hooksLogger->flush();
  EXPECT_NE(std::string::npos, output.str().find("enabled[value=2]"));

  usvfs::log::CallLogger::setEnabled(false);
  spdlog::drop("hooks");
}

TEST(DirectoryTreeTest, SimpleTreeInit)
{
  EXPECT_NO_THROW({
    ContainerType tree(g_SHMName, 4096);
    TreeType::NodePtrT p = tree.addFile(R"(C:\temp\test.txt)", 42, false);
    EXPECT_NE(TreeType::NodePtrT(), p);
  });
}

TEST(DirectoryTreeTest, FindNode)
{
  shared_memory_object::remove(g_SHMName);
  ContainerType tree(g_SHMName, 64 * 1024);
  EXPECT_NE(nullptr, tree.addFile(R"(C:\temp\bla)", 0x42, 0, false));

  EXPECT_NE(nullptr, tree->findNode(R"(C:\temp)").get());
  EXPECT_EQ(nullptr, tree->findNode(R"(C:\temp\bla\blubb)").get());
}

struct TestVisitor
{
  TreeType::NodePtrT lastNode;
  bool flag40{false};

  void operator()(const TreeType::NodePtrT& node)
  {
    lastNode = node;
    flag40   = node->hasFlag(0x40);
    logger()->debug("{0} - {1}", lastNode->name(), flag40);
    // BOOST_LOG_SEV(globalLogger::get(), LogLevel::Debug) << lastNode->name() << " - "
    // << flag40;
  }
};

TEST(DirectoryTreeTest, VisitPath)
{
  shared_memory_object::remove(g_SHMName);
  ContainerType tree(g_SHMName, 64 * 1024);
  EXPECT_NE(nullptr, tree.addFile(R"(C:\temp\bla)", 1, 0x40, false));

  TestVisitor visitor;

  tree->visitPath(R"(C:\temp\bla\blubb)",
                  TreeType::VisitorFunction([&](const TreeType::NodePtrT& node) {
                    visitor(node);
                  }));
  EXPECT_TRUE(visitor.flag40);
  EXPECT_EQ("bla", visitor.lastNode->name());
}

TEST(DirectoryTreeTest, WildCardFind)
{
  shared_memory_object::remove(g_SHMName);
  EXPECT_NO_THROW({
    ContainerType tree(g_SHMName, 64 * 1024);

    EXPECT_NE(nullptr, tree.addFile(R"(C:\temp)", 1, FLAG_DIRECTORY, false));
    EXPECT_NE(nullptr, tree.addFile(R"(C:\temp\abc)", 1, 0, false));
    EXPECT_NE(nullptr, tree.addFile(R"(C:\temp\abd)", 2, 0, false));
    EXPECT_NE(nullptr, tree.addFile(R"(C:\temp\ace)", 3, 0, false));

    EXPECT_EQ(3, tree->find(R"(C:\temp\*)").size());
    EXPECT_NE(nullptr, tree->node("C:"));
    EXPECT_EQ(1, tree->node("C:")->find("*").size());
    EXPECT_NE(nullptr, tree->node("C:")->node("temp"));
    EXPECT_EQ(3, tree->node("C:")->node("temp")->find("*").size());  // alternative
    // search on the top-level
    EXPECT_EQ(1, tree->find("*").size());
    // * should work
    EXPECT_EQ(2, tree->find(R"(C:\temp\ab*)").size());
    // * does not match directory separators
    EXPECT_EQ(0, tree->find("*ab*").size());
    // matches only the directory itself
    EXPECT_EQ(1, tree->find(R"(C:\temp*)").size());
  });
}

TEST(DirectoryTreeTest, SHMAllocation)
{
  EXPECT_NO_THROW({
    ContainerType create(g_SHMName, 64 * 1024);

    {  // creation
      create.addFile(R"(C:\temp\abc)", 1, false);
      create.addFile(R"(C:\temp\abd)", 2, false);
      create.addFile(R"(C:\temp\ace)", 3, false);
    }

    {  // access
      ContainerType access(g_SHMName, 64 * 1024);
      EXPECT_NE(nullptr, access.get());
      std::vector<TreeType::NodePtrT> res = access->find(R"(C:\temp\*)");
      EXPECT_EQ(3, res.size());  // matches the three files
      EXPECT_EQ(access->m_Self.lock().get(), access->node("C:")->parent().get());
    }
  });
}

TEST(DirectoryTreeTest, SHMAllocationError)
{
  EXPECT_NO_THROW({
    try {
      ContainerType tree(g_SHMName, 4096);
      int c = 0;
      for (char i = 'a'; i <= 'z'; ++i) {
        for (char j = 'a'; j <= 'z'; ++j) {
          std::string name = std::string(R"(C:\temp\)") + i + j;
          tree.addFile(name, ++c, false);
        }
      }

      EXPECT_EQ(1, tree->node("C:")->node("temp")->node("aa", MissingThrow)->data());
      EXPECT_EQ(26, tree->node("C:")->node("temp")->node("az", MissingThrow)->data());
    } catch (const std::exception& e) {
      logger()->error("{0}", e.what());
      // BOOST_LOG_SEV(globalLogger::get(), LogLevel::Error) << e.what();
      throw;
    }
  });
}

TEST(DirectoryTreeTest, SHMAllocationErrorComplex)
{
  EXPECT_NO_THROW({
    try {
      ComplexContainerType tree(g_SHMName, 4096);
      SHMString str = tree.create("gaga");
      for (char i = 'a'; i <= 'z'; ++i) {
        for (char j = 'a'; j <= 'z'; ++j) {
          std::string name = std::string(R"(C:\temp\)") + i + j;
          tree.addFile(name, str, false);
        }
      }
      EXPECT_STREQ(
          str.c_str(),
          tree->node("C:")->node("temp")->node("aa", MissingThrow)->data().c_str());
      EXPECT_STREQ(
          str.c_str(),
          tree->node("C:")->node("temp")->node("az", MissingThrow)->data().c_str());
    } catch (const std::exception& e) {
      logger()->error("{}", e.what());
      throw;
    }
  });
}

TEST(DirectoryTreeTest, ConcurrentReadViewsSerializeOutdatedReassignment)
{
  static const char shmName[] = "treetest_concurrent_read_view";
  shared_memory_object::remove(shmName);

  ContainerType writer(shmName, 4096);
  ContainerType reader(shmName, 4096);
  const std::string readerInitialName = reader.shmName();
  std::string finalPath;

  for (int i = 0; i < 10000 && writer.shmName() == readerInitialName; ++i) {
    finalPath = std::format(R"(C:\refresh\file_{:05}.txt)", i);
    writer.addFile(finalPath, i, 0, false);
  }

  ASSERT_NE(readerInitialName, writer.shmName());

  constexpr int ThreadCount = 8;
  std::atomic<bool> start{false};
  std::atomic<int> failures{0};
  std::vector<std::thread> readers;
  readers.reserve(ThreadCount);

  for (int i = 0; i < ThreadCount; ++i) {
    readers.emplace_back([&]() {
      while (!start.load(std::memory_order_acquire))
        std::this_thread::yield();

      auto view = reader.readView();
      if (!view->findNode(finalPath).get())
        failures.fetch_add(1, std::memory_order_relaxed);
    });
  }

  start.store(true, std::memory_order_release);
  for (auto& thread : readers)
    thread.join();

  EXPECT_EQ(0, failures.load());
  EXPECT_EQ(writer.shmName(), reader.shmName());
}

TEST(DirectoryTreeTest, RefreshMovesRawAccessToCurrentGeneration)
{
  static const char shmName[] = "treetest_explicit_refresh";
  shared_memory_object::remove(shmName);

  ContainerType writer(shmName, 4096);
  ContainerType reader(shmName, 4096);
  const std::string initialName = reader.shmName();
  std::string finalPath;

  for (int i = 0; i < 10000 && writer.shmName() == initialName; ++i) {
    finalPath = std::format(R"(C:\refresh\explicit_{:05}.txt)", i);
    writer.addFile(finalPath, i, 0, false);
  }

  ASSERT_NE(initialName, writer.shmName());
  reader.refresh();

  EXPECT_EQ(writer.shmName(), reader.shmName());
  EXPECT_NE(nullptr, reader.get()->findNode(finalPath).get());
}

TEST(DirectoryTreeTest, ReadViewPinsLocalAssignmentAgainstWriter)
{
  using namespace std::chrono_literals;

  static const char shmName[] = "treetest_pinned_read_view";
  shared_memory_object::remove(shmName);

  ContainerType tree(shmName, 64 * 1024);
  tree.addFile(R"(C:\pinned\before.txt)", 1, 0, false);

  std::promise<void> started;
  std::promise<void> finished;
  auto startedFuture  = started.get_future();
  auto finishedFuture = finished.get_future();
  std::thread writer;

  {
    auto view = tree.readView();
    ASSERT_NE(nullptr, view->findNode(R"(C:\pinned\before.txt)").get());

    writer = std::thread([&]() {
      started.set_value();
      tree.addFile(R"(C:\pinned\after.txt)", 2, 0, false);
      finished.set_value();
    });

    startedFuture.wait();
    EXPECT_EQ(std::future_status::timeout, finishedFuture.wait_for(50ms));
  }

  EXPECT_EQ(std::future_status::ready, finishedFuture.wait_for(2s));
  writer.join();

  auto view = tree.readView();
  EXPECT_NE(nullptr, view->findNode(R"(C:\pinned\after.txt)").get());
}

TEST(DirectoryTreeTest, MutationObserverReportsSuccessfulContainerChanges)
{
  static const char shmName[] = "treetest_mutation_observer";
  shared_memory_object::remove(shmName);

  std::array<int, 3> counts{};
  ContainerType tree(shmName, 64 * 1024, [&](usvfs::shared::TreeMutation mutation) {
    ++counts.at(static_cast<std::size_t>(mutation));
  });

  EXPECT_NE(nullptr, tree.addFile(R"(C:\observed\file.txt)", 1, 0, false));
  EXPECT_EQ(nullptr, tree.addFile(R"(C:\observed\file.txt)", 2, 0, false));
  EXPECT_NE(nullptr, tree.addDirectory(R"(C:\observed\directory)", 3, 0, false));
  tree.clear();

  EXPECT_EQ(1, counts.at(static_cast<std::size_t>(usvfs::shared::TreeMutation::Clear)));
  EXPECT_EQ(1,
            counts.at(static_cast<std::size_t>(usvfs::shared::TreeMutation::AddFile)));
  EXPECT_EQ(1, counts.at(static_cast<std::size_t>(
                   usvfs::shared::TreeMutation::AddDirectory)));
}

int main(int argc, char** argv)
{
  auto logger = spdlog::stdout_logger_mt("usvfs");
  logger->set_level(spdlog::level::warn);
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
