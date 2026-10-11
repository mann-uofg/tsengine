#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_util.h"
#include "tsengine/engine.h"

// Crash tests. A child process writes batches to a database and reports each batch on a pipe
// once Write() has returned for it. The child is then killed, either by SIGKILL at a random
// moment or by exiting at a chosen point inside the engine. The parent reopens the database
// and checks that:
//
//   - every batch the child reported is there, with the right values;
//   - nothing is there that no writer produced;
//   - every batch is there for all of its series or for none of them;
//   - no temporary or unlisted files are left behind.
//
// Killing a process does not lose data the kernel already has, so these tests prove that the
// engine recovers from dying at any point. They do not prove it survives power loss.

namespace tsengine {
namespace {

constexpr size_t kSeriesCount = 4;
constexpr int64_t kTimestampStep = 10;

// Exit codes of the child process.
constexpr int kHookExit = 41;
constexpr int kOpenFailed = 42;
constexpr int kWriteFailed = 43;

// Small limits, so that a few hundred batches go through many flushes, several partitions
// and several compactions.
EngineOptions CrashOptions() {
  EngineOptions options;
  options.partition_duration_ns = 500;
  options.memtable_flush_points = 40;
  options.compaction_min_files = 3;
  return options;
}

std::string SeriesKey(size_t i) { return "crash,series=" + std::to_string(i) + " value"; }

// The value batch `seq` writes to series `i`. Knowing it lets the parent check every point.
double ExpectedValue(uint64_t seq, size_t i) { return static_cast<double>(seq * 1000 + i); }

struct WriterJob {
  std::string dir;
  EngineOptions options;
  // Writes batches first, first + 1, ... first + count - 1.
  uint64_t first = 0;
  uint64_t count = 0;
};

// Exits the process, as if it had been killed, the nth time the engine reaches a point.
struct ExitAtPoint {
  std::string point;
  int nth = 1;
  int hits = 0;

  void operator()(std::string_view reached) {
    if (reached == point && ++hits == nth) {
      _exit(kHookExit);
    }
  }
};

// Body of the child process. Never returns, and never runs destructors: like a killed
// process, it leaves the database exactly as it was at that instant.
[[noreturn]] void RunWriter(const WriterJob& job, int ack_fd) {
  std::unique_ptr<Engine> engine;
  if (!Engine::Open(job.dir, job.options, &engine).ok()) {
    _exit(kOpenFailed);
  }
  std::vector<Sample> batch(kSeriesCount);
  for (size_t i = 0; i < kSeriesCount; ++i) {
    if (!engine->GetOrCreateSeries(SeriesKey(i), &batch[i].series_id).ok()) {
      _exit(kWriteFailed);
    }
  }
  for (uint64_t seq = job.first; seq < job.first + job.count; ++seq) {
    for (size_t i = 0; i < kSeriesCount; ++i) {
      batch[i].timestamp_ns = static_cast<int64_t>(seq) * kTimestampStep;
      batch[i].value = ExpectedValue(seq, i);
    }
    if (!engine->Write(batch).ok()) {
      _exit(kWriteFailed);
    }
    // Write() has returned: from here on the batch counts as acknowledged.
    if (::write(ack_fd, &seq, sizeof(seq)) != static_cast<ssize_t>(sizeof(seq))) {
      _exit(kWriteFailed);
    }
    // Compact on a schedule as well, so the compaction crash points are always reached.
    if (seq % 25 == 24 && !engine->Compact().ok()) {
      _exit(kWriteFailed);
    }
  }
  _exit(0);
}

struct ChildResult {
  bool started = false;
  bool killed = false;
  int exit_code = -1;
  std::vector<uint64_t> acked;
};

// Runs a writer in a child process and waits for it to end. If kill_after_ms is not negative
// the child is sent SIGKILL after that many milliseconds.
ChildResult RunChild(const WriterJob& job, int kill_after_ms) {
  ChildResult result;
  int fds[2];
  if (::pipe(fds) != 0) {
    return result;
  }
  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fds[0]);
    ::close(fds[1]);
    return result;
  }
  if (pid == 0) {
    ::close(fds[0]);
    RunWriter(job, fds[1]);
  }
  result.started = true;
  ::close(fds[1]);
  if (kill_after_ms >= 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(kill_after_ms));
    ::kill(pid, SIGKILL);
  }
  int status = 0;
  ::waitpid(pid, &status, 0);
  result.killed = WIFSIGNALED(status);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  }
  // The child is gone, so its end of the pipe is closed and this reads to the end.
  uint64_t seq = 0;
  while (::read(fds[0], &seq, sizeof(seq)) == static_cast<ssize_t>(sizeof(seq))) {
    result.acked.push_back(seq);
  }
  ::close(fds[0]);
  return result;
}

size_t CountFiles(const std::string& dir) {
  size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      ++count;
    }
  }
  return count;
}

// Opens the database as the next process would and checks it against what the writers
// reported as acknowledged.
void VerifyRecovered(const std::string& dir, const std::set<uint64_t>& acked) {
  std::unique_ptr<Engine> engine;
  const Status s = Engine::Open(dir, CrashOptions(), &engine);
  ASSERT_TRUE(s.ok()) << s.ToString();

  std::set<uint64_t> in_first_series;
  for (size_t i = 0; i < kSeriesCount; ++i) {
    uint64_t id = 0;
    ASSERT_TRUE(engine->GetOrCreateSeries(SeriesKey(i), &id).ok());
    Points points;
    ASSERT_TRUE(engine->Query(id, TimeRange(), &points).ok());

    // Every point must be one a writer produced, with the value it wrote.
    std::set<uint64_t> present;
    for (size_t j = 0; j < points.size(); ++j) {
      ASSERT_EQ(points.timestamps[j] % kTimestampStep, 0);
      const auto seq = static_cast<uint64_t>(points.timestamps[j] / kTimestampStep);
      ASSERT_EQ(points.values[j], ExpectedValue(seq, i)) << "batch " << seq;
      present.insert(seq);
    }
    // No acknowledged batch may be missing.
    for (const uint64_t seq : acked) {
      ASSERT_EQ(present.count(seq), 1U) << "lost acknowledged batch " << seq << ", series " << i;
    }
    // A batch is a single WAL record, so it is recovered for all of its series or for none.
    if (i == 0) {
      in_first_series = present;
    }
    ASSERT_TRUE(present == in_first_series) << "series " << i << " holds a partial batch";
  }
  EXPECT_EQ(CountFiles(dir + "/blocks"), engine->Files().size());
}

std::vector<std::string> CrashPoints() {
  std::vector<std::string> points;
  points.push_back("write:wal_appended");
  points.push_back("flush:blocks_written");
  points.push_back("flush:wal_rotated");
  points.push_back("flush:manifest_saved");
  points.push_back("compact:output_written");
  points.push_back("compact:manifest_saved");
  return points;
}

class CrashTest : public TempDirTest {};

// kill -9 at random moments, over and over, on the same database.
TEST_F(CrashTest, SurvivesBeingKilledAtRandomMoments) {
  std::mt19937 rng(20261010);
  std::set<uint64_t> acked;
  WriterJob job;
  job.dir = Path("db");
  job.options = CrashOptions();
  job.count = 400;
  for (int round = 0; round < 25; ++round) {
    const ChildResult result = RunChild(job, static_cast<int>(rng() % 60) + 1);
    ASSERT_TRUE(result.started);
    ASSERT_TRUE(result.killed || result.exit_code == 0) << "writer exited " << result.exit_code;
    acked.insert(result.acked.begin(), result.acked.end());
    ASSERT_NO_FATAL_FAILURE(VerifyRecovered(job.dir, acked));
    // Carry on from the batch after the last acknowledged one. That batch may or may not
    // have been written already; writing it again stores the same points.
    if (!acked.empty()) {
      job.first = *acked.rbegin() + 1;
    }
  }

  // One last run that is allowed to finish, so the test always ends with real data behind
  // it however early the kills above happened to land.
  job.count = 100;
  const ChildResult last = RunChild(job, -1);
  ASSERT_EQ(last.exit_code, 0);
  EXPECT_EQ(last.acked.size(), 100U);
  acked.insert(last.acked.begin(), last.acked.end());
  ASSERT_NO_FATAL_FAILURE(VerifyRecovered(job.dir, acked));
  EXPECT_GE(acked.size(), 100U);
}

// Dies exactly between the steps of a write, a flush and a compaction, on the first, second
// and third time each step is reached.
TEST_F(CrashTest, RecoversFromACrashAtEveryStepOfFlushAndCompaction) {
  std::set<uint64_t> acked;
  WriterJob job;
  job.dir = Path("db");
  job.count = 400;
  int crashes = 0;
  for (const std::string& point : CrashPoints()) {
    for (const int nth : {1, 2, 3}) {
      ExitAtPoint hook;
      hook.point = point;
      hook.nth = nth;
      job.options = CrashOptions();
      job.options.sync_writes = false;
      job.options.test_hook = hook;

      const ChildResult result = RunChild(job, -1);
      ASSERT_TRUE(result.started);
      ASSERT_FALSE(result.killed);
      const bool ended_as_planned = result.exit_code == kHookExit || result.exit_code == 0;
      ASSERT_TRUE(ended_as_planned) << "writer exited " << result.exit_code << " at " << point;
      if (result.exit_code == kHookExit) {
        ++crashes;
      }
      acked.insert(result.acked.begin(), result.acked.end());
      ASSERT_NO_FATAL_FAILURE(VerifyRecovered(job.dir, acked)) << point << " #" << nth;
      if (!acked.empty()) {
        job.first = *acked.rbegin() + 1;
      }
    }
  }
  // 400 batches reach every point at least three times, so every run must end in its hook.
  EXPECT_EQ(crashes, 18);
}

}  // namespace
}  // namespace tsengine
