/******************************************************************************
* Copyright (C) 2018, Divideon.
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*
* This library is also available under a commercial license.
* Please visit https://xvc.io/license/ for more information.
******************************************************************************/

#include "xvc_dec_lib/thread_decoder.h"

#include <algorithm>
#include <utility>

namespace xvc {

ThreadDecoder::ThreadDecoder(int num_threads) {
  if (num_threads < 0) {
    num_threads = std::thread::hardware_concurrency();
  }
  // Need at least one thread to work
  num_threads = std::max(1, num_threads);
  while (num_threads > static_cast<int>(worker_threads_.size())) {
    worker_threads_.emplace_back([this] {
      WorkerMain();
    });
  }
}

ThreadDecoder::~ThreadDecoder() {
  StopAll();
}

void ThreadDecoder::StopAll() {
  std::unique_lock<std::mutex> lock(global_mutex_);
  running_ = false;
  wait_work_cond_.notify_all();  // wakeup all
  lock.unlock();
  for (auto &thread : worker_threads_) {
    thread.join();
  }
  worker_threads_.clear();
}

void ThreadDecoder::DecodeAsync(
  std::shared_ptr<SegmentHeader> &&segment_header,
  std::shared_ptr<SegmentHeader> &&prev_segment_header,
  std::shared_ptr<PictureDecoder> &&pic_dec,
  std::vector<std::shared_ptr<const PictureDecoder>> &&deps,
  std::unique_ptr<std::vector<uint8_t>> &&nal, size_t nal_offset) {
  // Prepare work for thread
  WorkItem work;
  work.pic_dec = std::move(pic_dec);
  work.inter_dependencies = std::move(deps);
  work.segment_header = std::move(segment_header);
  work.prev_segment_header = std::move(prev_segment_header);
  work.nal_offset = nal_offset;
  work.nal = std::move(nal);

  // Signal one worker thread to begin processing
  std::unique_lock<std::mutex> lock(global_mutex_);
  pending_work_.push_back(std::move(work));
  jobs_in_flight_++;
  wait_work_cond_.notify_one();
}

void ThreadDecoder::WaitForPicture(const std::shared_ptr<PictureDecoder> &pic,
                                   PictureDecodedCallback callback) {
  while (pic->GetOutputStatus() != OutputStatus::kHasNotBeenOutput) {
    WaitOne(callback);
  }
}

void ThreadDecoder::WaitOne(PictureDecodedCallback callback) {
  std::unique_lock<std::mutex> lock(global_mutex_);
  work_done_cond_.wait(lock, [this] { return !finished_work_.empty(); });
  WorkItem work = std::move(finished_work_.front());
  finished_work_.pop_front();
  jobs_in_flight_--;
  // Note! Callback invoked while lock is being held
  callback(work.pic_dec, work.success, work.inter_dependencies);
}

void ThreadDecoder::WaitAll(PictureDecodedCallback callback) {
  std::unique_lock<std::mutex> lock(global_mutex_);
  while (jobs_in_flight_ > 0) {
    work_done_cond_.wait(lock, [this] { return !finished_work_.empty(); });
    WorkItem work = std::move(finished_work_.front());
    finished_work_.pop_front();
    jobs_in_flight_--;
    // Note! Callback invoked while lock is held
    callback(work.pic_dec, work.success, work.inter_dependencies);
  }
}

void ThreadDecoder::WorkerMain() {
  std::unique_lock<std::mutex> lock(global_mutex_);
  while (true) {
    ThreadDecoder::WorkItem work;
    // Find one picture that can be decoded now
    wait_work_cond_.wait(lock, [this, &work] {
      if (!running_) {
        return true;
      }
      if (pending_work_.empty()) {
        return false;
      }
      // Verify all dependencies are satisfied before taking work
      auto it = pending_work_.begin();
      for (; it != pending_work_.end(); ++it) {
        bool valid = true;
        for (auto &dependency : it->inter_dependencies) {
          if (dependency->GetOutputStatus() == OutputStatus::kProcessing) {
            valid = false;
            break;
          }
        }
        if (!valid) {
          continue;
        }
        work = std::move(*it);
        pending_work_.erase(it);
        return true;
      }
      return false;
    });
    if (!running_) {
      break;
    }
    lock.unlock();

    // Load restriction flags for current thread unles already done
    thread_local SegmentNum restriction_soc = static_cast<SegmentNum>(-1);
    if (restriction_soc != work.segment_header->soc) {
      Restrictions::GetRW() = work.segment_header->restrictions;
      restriction_soc = work.segment_header->soc;
    }

    // Decode picture
    BitReader bit_reader(&(*work.nal)[0] + work.nal_offset,
                         work.nal->size() - work.nal_offset);
    work.success = work.pic_dec->Decode(*work.segment_header,
                                        *work.prev_segment_header, &bit_reader,
                                        false);
    work.pic_dec->SetOutputStatus(OutputStatus::kPostProcessing);

    // Notify all workers that a dependency might be ready
    lock.lock();
    wait_work_cond_.notify_all();
    lock.unlock();

    // Verify checksum and prepare output picture
    work.success &=
      work.pic_dec->Postprocess(*work.segment_header, &bit_reader);
    work.pic_dec->SetOutputStatus(OutputStatus::kFinishedProcessing);

    // Notify main thread picture that picture is fully decoded
    lock.lock();
    // TODO(PH) some fields are not needed anymore (like nal)
    finished_work_.push_back(std::move(work));
    work_done_cond_.notify_all();
  }
}

}   // namespace xvc
