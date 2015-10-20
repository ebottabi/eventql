/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#include "stx/logging.h"
#include "zbase/mapreduce/MapReduceScheduler.h"

using namespace stx;

namespace zbase {

MapReduceScheduler::MapReduceScheduler(
    const MapReduceShardList& shards,
    thread::ThreadPool* tpool,
    size_t max_concurrent_tasks /* = kDefaultMaxConcurrentTasks */) :
    shards_(shards),
    shard_status_(shards_.size(), MapReduceShardStatus::PENDING),
    shard_results_(shards_.size()),
    tpool_(tpool),
    max_concurrent_tasks_(max_concurrent_tasks),
    done_(false),
    num_shards_running_(0),
    num_shards_completed_(0) {}

void MapReduceScheduler::execute() {
  std::unique_lock<std::mutex> lk(mutex_);

  for (;;) {
    logDebug(
        "z1.mapreduce",
        "Running job; progress=$0/$1 ($2 runnning)",
        num_shards_completed_,
        shards_.size(),
        num_shards_running_);

    if (done_) {
      break;
    }

    if (startJobs() > 0) {
      continue;
    }

    cv_.wait(lk);
  }
}

size_t MapReduceScheduler::startJobs() {
  if (num_shards_running_ >= max_concurrent_tasks_) {
    return 0;
  }

  if (num_shards_completed_ + num_shards_running_ >= shards_.size()) {
    return 0;
  }

  size_t num_started = 0;
  for (size_t i = 0; i < shards_.size(); ++i) {
    if (shard_status_[i] != MapReduceShardStatus::PENDING) {
      continue;
    }

    bool ready = true;
    for (auto dep : shards_[i]->dependencies) {
      if (shard_status_[dep] != MapReduceShardStatus::COMPLETED) {
        ready = false;
      }
    }

    if (!ready) {
      continue;
    }

    ++num_shards_running_;
    ++num_started;
    shard_status_[i] = MapReduceShardStatus::RUNNING;
    auto shard = shards_[i];

    tpool_->run([this, i, shard] {
      bool error = false;

      try {
        shard->task->execute(shard, this);
      } catch (const StandardException& e) {
        error = true;
      }

      {
        std::unique_lock<std::mutex> lk(mutex_);
        shard_status_[i] = error
            ? MapReduceShardStatus::ERROR
            : MapReduceShardStatus::COMPLETED;

        --num_shards_running_;
        if (++num_shards_completed_ == shards_.size()) {
          done_ = true;
        }

        lk.unlock();
        cv_.notify_all();
      }
    });

    if (num_shards_running_ >= max_concurrent_tasks_) {
      break;
    }
  }

  return num_started;
}

} // namespace zbase

