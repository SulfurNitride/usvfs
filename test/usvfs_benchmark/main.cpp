#include <usvfs.h>
#include <windows_sane.h>

#include <test_helpers.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <string_view>
#include <thread>

namespace fs = std::filesystem;

namespace
{
struct Options
{
  fs::path root;
  fs::path output;
  std::size_t files{100000};
  std::size_t directories{4096};
  std::size_t layers{8};
  std::size_t iterations{3};
  std::size_t threads{1};
  std::uint64_t seed{0x555356465342454EULL};
  bool generate{false};
  bool worker{false};
};

std::wstring quoted(const fs::path& value)
{
  return L"\"" + value.wstring() + L"\"";
}

std::wstring bucketName(std::size_t index, std::size_t directories)
{
  wchar_t value[32]{};
  swprintf_s(value, L"bucket_%05zu", index % directories);
  return value;
}

std::wstring assetName(std::size_t index)
{
  wchar_t value[32]{};
  swprintf_s(value, L"asset_%09zu.dat", index);
  return value;
}

std::wstring collisionName(std::size_t index)
{
  wchar_t value[40]{};
  swprintf_s(value, L"collision_%09zu.dat", index);
  return value;
}

fs::path relativeAsset(std::size_t index, std::size_t directories)
{
  return fs::path(bucketName(index, directories)) / assetName(index);
}

fs::path relativeCollision(std::size_t index, std::size_t directories)
{
  return fs::path(bucketName(index, directories)) / collisionName(index);
}

fs::path layerPath(const Options& options, std::size_t layer)
{
  wchar_t value[32]{};
  swprintf_s(value, L"layer_%03zu", layer);
  return options.root / L"layers" / value;
}

std::uint64_t ticks()
{
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return static_cast<std::uint64_t>(value.QuadPart);
}

std::uint64_t frequency()
{
  LARGE_INTEGER value{};
  QueryPerformanceFrequency(&value);
  return static_cast<std::uint64_t>(value.QuadPart);
}

double milliseconds(std::uint64_t start, std::uint64_t end)
{
  return static_cast<double>(end - start) * 1000.0 / static_cast<double>(frequency());
}

void writeResult(std::ofstream& output, std::string_view operation,
                 std::size_t operations, std::size_t threads, double elapsedMs,
                 std::size_t errors)
{
  output << "{\"format\":1,\"operation\":\"" << operation
         << "\",\"operations\":" << operations << ",\"threads\":" << threads
         << ",\"elapsed_ms\":" << elapsedMs << ",\"ops_per_second\":"
         << (elapsedMs == 0.0 ? 0.0 : operations * 1000.0 / elapsedMs)
         << ",\"errors\":" << errors << "}\n";
  output.flush();
}

void createFile(const fs::path& path, unsigned char layer)
{
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("failed to create corpus file");
  }
  stream.put(static_cast<char>(layer));
}

void generateCorpus(const Options& options)
{
  const fs::path marker = options.root / L".usvfs-benchmark-corpus";
  if (fs::exists(options.root)) {
    if (!fs::exists(marker)) {
      throw std::runtime_error(
          "refusing to use an existing directory without the corpus marker");
    }
    std::wifstream existing(marker);
    std::wstring configuration;
    std::getline(existing, configuration);
    const auto expected =
        std::to_wstring(options.files) + L" " + std::to_wstring(options.directories) +
        L" " + std::to_wstring(options.layers) + L" " + std::to_wstring(options.seed);
    if (configuration != expected) {
      throw std::runtime_error(
          "existing corpus parameters differ; choose a new corpus directory");
    }
    std::cout << "Corpus already exists; reusing it.\n";
    return;
  }

  fs::create_directories(options.root / L"mount");
  const std::size_t collisionCount = std::max<std::size_t>(1, options.files / 10);
  for (std::size_t index = 0; index < options.files; ++index) {
    const std::size_t layer = index % options.layers;
    createFile(layerPath(options, layer) / relativeAsset(index, options.directories),
               static_cast<unsigned char>(layer));
  }
  for (std::size_t index = 0; index < collisionCount; ++index) {
    for (std::size_t layer = 0; layer < options.layers; ++layer) {
      createFile(layerPath(options, layer) /
                     relativeCollision(index, options.directories),
                 static_cast<unsigned char>(layer));
    }
  }

  std::wofstream output(marker);
  output << options.files << L" " << options.directories << L" " << options.layers
         << L" " << options.seed << L"\n";
}

std::size_t readFirstByte(const fs::path& path, unsigned char* value)
{
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return 1;
  }
  DWORD read        = 0;
  const BOOL result = ReadFile(handle, value, 1, &read, nullptr);
  CloseHandle(handle);
  return result && read == 1 ? 0 : 1;
}

int runWorker(const Options& options)
{
  std::ofstream output(options.output, std::ios::app);
  if (!output) {
    std::cerr << "Cannot open benchmark output.\n";
    return 2;
  }

  const fs::path mount             = options.root / L"mount";
  const std::size_t collisionCount = std::max<std::size_t>(1, options.files / 10);
  std::atomic<std::size_t> errors{0};

  auto measure = [&](std::string_view name, std::size_t count, auto&& operation) {
    errors.store(0);
    const auto start = ticks();
    operation(errors);
    const auto end = ticks();
    writeResult(output, name, count, 1, milliseconds(start, end), errors.load());
  };

  for (std::size_t pass = 0; pass < options.iterations; ++pass) {
    measure(pass == 0 ? "attributes_existing_cold" : "attributes_existing_warm",
            options.files, [&](auto& failures) {
              for (std::size_t index = 0; index < options.files; ++index) {
                if (GetFileAttributesW(
                        (mount / relativeAsset(index, options.directories)).c_str()) ==
                    INVALID_FILE_ATTRIBUTES) {
                  ++failures;
                }
              }
            });

    measure(pass == 0 ? "attributes_missing_cold" : "attributes_missing_warm",
            options.files, [&](auto& failures) {
              for (std::size_t index = 0; index < options.files; ++index) {
                const fs::path path = mount / bucketName(index, options.directories) /
                                      (L"missing_" + std::to_wstring(index) + L".dat");
                if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                  ++failures;
                }
              }
            });

    measure(pass == 0 ? "open_existing_cold" : "open_existing_warm", options.files,
            [&](auto& failures) {
              for (std::size_t index = 0; index < options.files; ++index) {
                unsigned char value = 0;
                failures += readFirstByte(
                    mount / relativeAsset(index, options.directories), &value);
                if (value != static_cast<unsigned char>(index % options.layers)) {
                  ++failures;
                }
              }
            });

    measure(pass == 0 ? "find_exact_cold" : "find_exact_warm", collisionCount,
            [&](auto& failures) {
              for (std::size_t index = 0; index < collisionCount; ++index) {
                WIN32_FIND_DATAW data{};
                HANDLE find = FindFirstFileW(
                    (mount / relativeCollision(index, options.directories)).c_str(),
                    &data);
                if (find == INVALID_HANDLE_VALUE) {
                  ++failures;
                } else {
                  FindClose(find);
                  unsigned char value = 0;
                  failures += readFirstByte(
                      mount / relativeCollision(index, options.directories), &value);
                  if (value != static_cast<unsigned char>(options.layers - 1)) {
                    ++failures;
                  }
                }
              }
            });

    measure(pass == 0 ? "enumerate_directories_cold" : "enumerate_directories_warm",
            options.directories, [&](auto& failures) {
              for (std::size_t index = 0; index < options.directories; ++index) {
                WIN32_FIND_DATAW data{};
                HANDLE find = FindFirstFileW(
                    (mount / bucketName(index, options.directories) / L"*").c_str(),
                    &data);
                if (find == INVALID_HANDLE_VALUE) {
                  ++failures;
                  continue;
                }
                while (FindNextFileW(find, &data)) {
                }
                if (GetLastError() != ERROR_NO_MORE_FILES) {
                  ++failures;
                }
                FindClose(find);
              }
            });
  }

  errors.store(0);
  const std::size_t mixedOperations = options.files * options.iterations;
  const auto start                  = ticks();
  std::vector<std::thread> workers;
  for (std::size_t threadIndex = 0; threadIndex < options.threads; ++threadIndex) {
    workers.emplace_back([&, threadIndex]() {
      std::mt19937_64 random(options.seed + threadIndex);
      for (std::size_t operation = threadIndex; operation < mixedOperations;
           operation += options.threads) {
        const std::size_t index = random() % options.files;
        const bool missing      = (operation & 3) == 0;
        fs::path path           = mount / relativeAsset(index, options.directories);
        if (missing) {
          path.replace_filename(L"missing_" + std::to_wstring(index) + L".dat");
        }
        const bool found = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (found == missing) {
          ++errors;
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  const auto end = ticks();
  writeResult(output, "mixed_concurrent", mixedOperations, options.threads,
              milliseconds(start, end), errors.load());
  return errors.load() == 0 ? 0 : 3;
}

fs::path executablePath()
{
  std::wstring buffer(32768, L'\0');
  const DWORD size =
      GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  buffer.resize(size);
  return buffer;
}

int runController(const Options& options)
{
  const auto dll =
      test::path_of_usvfs_lib(test::platform_dependant_executable("usvfs", "dll"));
  test::ScopedLoadLibrary loadDll(dll.c_str());
  if (!loadDll) {
    std::wcerr << L"Could not load " << dll << L" (error " << GetLastError() << L").\n";
    return 4;
  }

  auto parameters = std::unique_ptr<usvfsParameters, decltype(&usvfsFreeParameters)>(
      usvfsCreateParameters(), &usvfsFreeParameters);
  const std::string instance =
      "usvfs_benchmark_" + std::to_string(GetCurrentProcessId());
  usvfsSetInstanceName(parameters.get(), instance.c_str());
  usvfsSetDebugMode(parameters.get(), false);
  usvfsSetLogLevel(parameters.get(), LogLevel::Info);
  usvfsSetCrashDumpType(parameters.get(), CrashDumpsType::None);
  usvfsSetCrashDumpPath(parameters.get(), "");
  usvfsInitLogging(false);
  if (!usvfsCreateVFS(parameters.get())) {
    std::cerr << "Could not create VFS.\n";
    return 5;
  }

  std::ofstream output(options.output, std::ios::trunc);
  output << "{\"format\":1,\"kind\":\"configuration\",\"files\":" << options.files
         << ",\"directories\":" << options.directories
         << ",\"layers\":" << options.layers << ",\"iterations\":" << options.iterations
         << ",\"threads\":" << options.threads << ",\"seed\":" << options.seed << "}\n";
  const auto mapStart      = ticks();
  std::size_t mappedLayers = 0;
  for (std::size_t layer = 0; layer < options.layers; ++layer) {
    if (usvfsVirtualLinkDirectoryStatic(layerPath(options, layer).c_str(),
                                        (options.root / L"mount").c_str(),
                                        LINKFLAG_RECURSIVE)) {
      ++mappedLayers;
    }
  }
  const auto mapEnd = ticks();
  writeResult(output, "mapping_build", options.files, 1, milliseconds(mapStart, mapEnd),
              options.layers - mappedLayers);
  output.close();

  std::wstring command = quoted(executablePath()) + L" --worker --root " +
                         quoted(options.root) + L" --output " + quoted(options.output) +
                         L" --files " + std::to_wstring(options.files) +
                         L" --directories " + std::to_wstring(options.directories) +
                         L" --layers " + std::to_wstring(options.layers) +
                         L" --iterations " + std::to_wstring(options.iterations) +
                         L" --threads " + std::to_wstring(options.threads) +
                         L" --seed " + std::to_wstring(options.seed);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!usvfsCreateProcessHooked(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                                nullptr, options.root.c_str(), &startup, &process)) {
    std::cerr << "Could not launch hooked benchmark worker.\n";
    usvfsDisconnectVFS();
    return 6;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 99;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  usvfsDisconnectVFS();

  fs::path profilePath = options.output;
  profilePath += L".usvfs.log";
  std::ofstream profile(profilePath);
  std::string message(4096, '\0');
  while (usvfsGetLogMessages(message.data(), message.size(), false)) {
    profile << message.c_str() << '\n';
  }
  return static_cast<int>(exitCode);
}

Options parse(int argc, wchar_t** argv)
{
  Options options;
  auto value = [&](int& index) -> std::wstring {
    if (++index >= argc)
      throw std::runtime_error("missing option value");
    return argv[index];
  };
  for (int index = 1; index < argc; ++index) {
    const std::wstring_view argument(argv[index]);
    if (argument == L"--root")
      options.root = value(index);
    else if (argument == L"--output")
      options.output = value(index);
    else if (argument == L"--files")
      options.files = std::stoull(value(index));
    else if (argument == L"--directories")
      options.directories = std::stoull(value(index));
    else if (argument == L"--layers")
      options.layers = std::stoull(value(index));
    else if (argument == L"--iterations")
      options.iterations = std::stoull(value(index));
    else if (argument == L"--threads")
      options.threads = std::stoull(value(index));
    else if (argument == L"--seed")
      options.seed = std::stoull(value(index));
    else if (argument == L"--generate")
      options.generate = true;
    else if (argument == L"--worker")
      options.worker = true;
    else
      throw std::runtime_error("unknown option");
  }
  return options;
}
}  // namespace

int wmain(int argc, wchar_t** argv)
{
  if (argc == 1) {
    std::cout << "usvfs_benchmark is opt-in; see test/usvfs_benchmark/README.md\n";
    return 0;
  }
  try {
    Options options = parse(argc, argv);
    if (options.root.empty() || options.output.empty() || options.files == 0 ||
        options.directories == 0 || options.layers == 0 || options.layers > 255 ||
        options.iterations == 0 || options.threads == 0) {
      throw std::runtime_error("invalid or missing benchmark parameters");
    }
    if (options.generate)
      generateCorpus(options);
    if (!fs::exists(options.root / L".usvfs-benchmark-corpus")) {
      throw std::runtime_error("corpus marker is missing; use --generate first");
    }
    return options.worker ? runWorker(options) : runController(options);
  } catch (const std::exception& error) {
    std::cerr << "usvfs_benchmark: " << error.what() << '\n';
    return 2;
  }
}
