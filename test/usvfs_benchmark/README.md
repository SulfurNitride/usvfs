# USVFS generated loose-file benchmark

`usvfs_benchmark` creates a deterministic, opt-in loose-file corpus and runs a
worker inside USVFS. It is not a replacement for a real game benchmark. Its
purpose is to replay fixed operations quickly after game profiling identifies a
hot request shape.

Place the benchmark executable and its matching architecture/configuration
`usvfs` DLL in the same directory. Unlike the repository's test runners, the
standalone benchmark does not depend on the build-tree `test/bin` and `lib`
layout.

The corpus has configurable logical-file, directory and layer counts. Ten per
cent of the logical file count is additionally written to every layer under the
same virtual name, providing deterministic priority collisions. Every file
contains its source-layer byte so the worker can verify the selected mapping.

Example from a Windows shell, or under Wine using Windows paths:

```text
set FLUORINE_USVFS_PROFILE=1
usvfs_benchmark_x64.exe --generate ^
  --root Z:\tmp\usvfs-corpus-100k ^
  --output Z:\tmp\usvfs-corpus-100k.jsonl ^
  --files 100000 --directories 4096 --layers 8 ^
  --iterations 3 --threads 8 --seed 6148352776335410510
```

Use a new root when changing corpus parameters. Generation refuses to touch an
existing directory unless it contains the benchmark marker with exactly the
requested configuration; the tool never recursively deletes a corpus.

JSON Lines output records mapping construction, existing/missing attribute
lookups, one-byte opens, exact-name searches, full directory enumeration and a
mixed concurrent workload. The adjacent `.usvfs.log` contains profiler summary
records when the profiling environment variable is enabled.

The `_cold` labels mean the first pass in that hooked worker and `_warm` means
an immediate repeat. They do not claim a physically cold kernel page cache; the
tool never drops global caches. Directory-walk correctness checks the complete
unique merged name set and exact collision winners, not just successful API
return codes.

For comparisons, generate once, alternate DLL A/B/A/B/A/B against the same
corpus, and report median, range and median absolute deviation. Run 100K first;
1M is supported but consumes substantially more inodes and setup time.
