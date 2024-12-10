/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "velox/runner/LocalRunner.h"
#include "velox/common/time/Timer.h"

#include "velox/connectors/hive/HiveConnectorSplit.h"

namespace facebook::velox::runner {
namespace {
std::shared_ptr<exec::RemoteConnectorSplit> remoteSplit(
    const std::string& taskId) {
  return std::make_shared<exec::RemoteConnectorSplit>(taskId);
}
} // namespace

RowVectorPtr LocalRunner::next() {
  if (!cursor_) {
    start();
  }
  bool hasNext = cursor_->moveNext();
  if (!hasNext) {
    state_ = State::kFinished;
    return nullptr;
  }
  return cursor_->current();
}

void LocalRunner::start() {
  VELOX_CHECK_EQ(state_, State::kInitialized);
  auto lastStage = makeStages();
  params_.planNode = plan_->fragments().back().fragment.planNode;
  auto cursor = exec::test::TaskCursor::create(params_);
  stages_.push_back({cursor->task()});
  // Add table scan splits to the final gathere stage.
  for (auto& scan : fragments_.back().scans) {
    auto source = splitSourceFactory_->splitSourceForScan(*scan);
    for (;;) {
      auto split = source->next(0);
      if (!split.hasConnectorSplit()) {
        break;
      }
      cursor->task()->addSplit(scan->id(), std::move(split));
    }
    cursor->task()->noMoreSplits(scan->id());
  }
  // If the plan only has the final gather stage, there are no shuffles between
  // the last
  // and previous stages to set up.
  if (!lastStage.empty()) {
    const auto finalStageConsumer =
        fragments_.back().inputStages[0].consumerNodeId;
    for (auto& remote : lastStage) {
      cursor->task()->addSplit(finalStageConsumer, exec::Split(remote));
    }
    cursor->task()->noMoreSplits(finalStageConsumer);
  }
  {
    std::lock_guard<std::mutex> l(mutex_);
    if (!error_) {
      cursor_ = std::move(cursor);
      state_ = State::kRunning;
    }
  }
  if (!cursor_) {
    // The cursor was not set because previous fragments had an error.
    abort();
    std::rethrow_exception(error_);
  }
}

void LocalRunner::abort() {
  // If called without previous error, we set the error to be cancellation.
  if (!error_) {
    try {
      state_ = State::kCancelled;
      VELOX_FAIL("Query cancelled");
    } catch (const std::exception& e) {
      error_ = std::current_exception();
    }
  }
  VELOX_CHECK(state_ != State::kInitialized);
  // Setting errors is thred safe. The stages do not change after
  // initialization.
  for (auto& stage : stages_) {
    for (auto& task : stage) {
      task->setError(error_);
    }
  }
  if (cursor_) {
    cursor_->setError(error_);
  }
}

void LocalRunner::waitForCompletion(int32_t maxWaitUs) {
  VELOX_CHECK_NE(state_, State::kInitialized);
  std::vector<ContinueFuture> futures;
  {
    std::lock_guard<std::mutex> l(mutex_);
    for (auto& stage : stages_) {
      for (auto& task : stage) {
        futures.push_back(task->taskDeletionFuture());
      }
      stage.clear();
    }
  }
  auto startTime = getCurrentTimeMicro();
  for (auto& future : futures) {
    auto& executor = folly::QueuedImmediateExecutor::instance();
    if (getCurrentTimeMicro() - startTime > maxWaitUs) {
      VELOX_FAIL("LocalRunner did not finish within {} us", maxWaitUs);
    }
    std::move(future)
        .within(std::chrono::microseconds(maxWaitUs))
        .via(&executor)
        .wait();
  }
}

std::vector<std::shared_ptr<exec::RemoteConnectorSplit>>
LocalRunner::makeStages() {
  std::unordered_map<std::string, int32_t> stageMap;
  auto sharedRunner = shared_from_this();
  auto onError = [self = sharedRunner, this](std::exception_ptr error) {
    {
      std::lock_guard<std::mutex> l(mutex_);
      if (error_) {
        return;
      }
      state_ = State::kError;
      error_ = error;
    }
    if (cursor_) {
      abort();
    }
  };

  for (auto fragmentIndex = 0; fragmentIndex < fragments_.size() - 1;
       ++fragmentIndex) {
    auto& fragment = fragments_[fragmentIndex];
    stageMap[fragment.taskPrefix] = stages_.size();
    stages_.emplace_back();
    for (auto i = 0; i < fragment.width; ++i) {
      exec::Consumer consumer = nullptr;
      auto task = exec::Task::create(
          fmt::format(
              "local://{}/{}.{}",
              params_.queryCtx->queryId(),
              fragment.taskPrefix,
              i),
          fragment.fragment,
          i,
          params_.queryCtx,
          exec::Task::ExecutionMode::kParallel,
          consumer,
          0,
          onError);
      stages_.back().push_back(task);
      if (fragment.numBroadcastDestinations) {
        // TODO: Add support for Arbitrary partition type.
        task->updateOutputBuffers(fragment.numBroadcastDestinations, true);
      }
      task->start(options_.numDrivers);
    }
  }

  for (auto fragmentIndex = 0; fragmentIndex < fragments_.size() - 1;
       ++fragmentIndex) {
    auto& fragment = fragments_[fragmentIndex];
    for (auto& scan : fragment.scans) {
      auto source = splitSourceFactory_->splitSourceForScan(*scan);
      bool allDone = false;
      do {
        for (auto i = 0; i < stages_[fragmentIndex].size(); ++i) {
          auto split = source->next(i);
          if (!split.hasConnectorSplit()) {
            allDone = true;
            break;
          }
          stages_[fragmentIndex][i]->addSplit(scan->id(), std::move(split));
        }
      } while (!allDone);
    }
    for (auto& scan : fragment.scans) {
      for (auto i = 0; i < stages_[fragmentIndex].size(); ++i) {
        stages_[fragmentIndex][i]->noMoreSplits(scan->id());
      }
    }

    for (auto& input : fragment.inputStages) {
      const auto sourceStage = stageMap[input.producerTaskPrefix];
      std::vector<std::shared_ptr<exec::RemoteConnectorSplit>> sourceSplits;
      for (auto i = 0; i < stages_[sourceStage].size(); ++i) {
        sourceSplits.push_back(remoteSplit(stages_[sourceStage][i]->taskId()));
      }
      for (auto& task : stages_[fragmentIndex]) {
        for (auto& remote : sourceSplits) {
          task->addSplit(input.consumerNodeId, exec::Split(remote));
        }
        task->noMoreSplits(input.consumerNodeId);
      }
    }
  }
  if (stages_.empty()) {
    return {};
  }
  std::vector<std::shared_ptr<exec::RemoteConnectorSplit>> lastStage;
  for (auto& task : stages_.back()) {
    lastStage.push_back(remoteSplit(task->taskId()));
  }
  return lastStage;
}

exec::Split LocalSplitSource::next(int32_t /*worker*/) {
  if (currentFile_ >= static_cast<int32_t>(table_->files().size())) {
    return exec::Split();
  }

  if (currentSplit_ >= fileSplits_.size()) {
    fileSplits_.clear();
    ++currentFile_;
    if (currentFile_ >= table_->files().size()) {
      return exec::Split();
    }

    currentSplit_ = 0;
    auto filePath = table_->files()[currentFile_];
    const auto fileSize = fs::file_size(filePath);
    // Take the upper bound.
    const int splitSize = std::ceil((fileSize) / splitsPerFile_);
    for (int i = 0; i < splitsPerFile_; ++i) {
      fileSplits_.push_back(
          connector::hive::HiveConnectorSplitBuilder(filePath)
              .connectorId(table_->schema()->connector()->connectorId())
              .fileFormat(table_->format())
              .start(i * splitSize)
              .length(splitSize)
              .build());
    }
  }
  return exec::Split(std::move(fileSplits_[currentSplit_++]));
}

std::unique_ptr<SplitSource> LocalSplitSourceFactory::splitSourceForScan(
    const core::TableScanNode& tableScan) {
  auto* tableHandle = dynamic_cast<const connector::hive::HiveTableHandle*>(
      tableScan.tableHandle().get());
  VELOX_CHECK_NOT_NULL(tableHandle);
  auto* table = reinterpret_cast<LocalTable*>(
      schema_->findTable(tableHandle->tableName()));

  return std::make_unique<LocalSplitSource>(table, splitsPerFile_);
}

std::vector<exec::TaskStats> LocalRunner::stats() const {
  std::vector<exec::TaskStats> result;
  std::lock_guard<std::mutex> l(mutex_);
  for (auto i = 0; i < stages_.size(); ++i) {
    auto& tasks = stages_[i];
    VELOX_CHECK(!tasks.empty());
    auto stats = tasks[0]->taskStats();
    for (auto j = 1; j < tasks.size(); ++j) {
      auto moreStats = tasks[j]->taskStats();
      for (auto pipeline = 0; pipeline < stats.pipelineStats.size();
           ++pipeline) {
        for (auto op = 0;
             op < stats.pipelineStats[pipeline].operatorStats.size();
             ++op) {
          stats.pipelineStats[pipeline].operatorStats[op].add(
              moreStats.pipelineStats[pipeline].operatorStats[op]);
        }
      }
    }
    result.push_back(std::move(stats));
  }
  return result;
}

} // namespace facebook::velox::runner