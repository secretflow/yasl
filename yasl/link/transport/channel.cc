// Copyright 2019 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "yasl/link/transport/channel.h"

#include "spdlog/spdlog.h"

#include "yasl/base/byte_container_view.h"
#include "yasl/base/exception.h"

namespace yasl::link {

// use acsii control code inside ack/fin msg key.
// avoid conflict to normal msg key.
static const std::string kAckKey{'A', 'C', 'K', '\x01', '\x00'};
static const std::string kFinKey{'F', 'I', 'N', '\x01', '\x00'};

class ChunkedMessage {
 public:
  explicit ChunkedMessage(size_t num_chunks)
      : num_chunks_(num_chunks), message_size_(0) {}

  void AddChunk(size_t index, ByteContainerView data) {
    std::unique_lock lock(mutex_);
    chunks_.emplace(index, data);
    message_size_ += data.size();
  }

  size_t NumChunks() const { return num_chunks_; }

  size_t NumFilled() const { return chunks_.size(); }

  bool IsFullyFilled() const { return chunks_.size() == num_chunks_; }

  Buffer Reassemble() {
    Buffer out(message_size_);
    size_t bytes_written = 0;
    for (auto& itr : chunks_) {
      std::memcpy(out.data<char>() + bytes_written, itr.second.data(),
                  itr.second.size());
      bytes_written += itr.second.size();
    }
    message_size_ = 0;
    chunks_.clear();
    return out;
  }

 protected:
  const size_t num_chunks_;

  std::mutex mutex_;
  // chunk index to value.
  std::map<size_t, Buffer> chunks_;
  size_t message_size_;
};

Buffer ChannelBase::Recv(const std::string& key) {
  YASL_ENFORCE(key != kAckKey && key != kFinKey,
               "For developer: pls use another key for normal message.");

  Buffer value;
  {
    std::unique_lock lock(msg_mutex_);
    const auto& duration = std::chrono::milliseconds(recv_timeout_ms_);
    if (!msg_db_cond_.wait_for(lock, duration, [&] {
          auto itr = this->msg_db_.find(key);
          if (itr == this->msg_db_.end()) {
            return false;
          } else {
            value = std::move(itr->second);
            this->msg_db_.erase(itr);
            return true;
          }
        })) {
      YASL_THROW_IO_ERROR("Get data timeout, key={}", key);
    }
  }
  SendAsyncImpl(kAckKey, ByteContainerView{});

  return value;
}

template <typename T>
void ChannelBase::OnNormalMessage(const std::string& key, T&& v) {
  received_msg_count_++;
  if (!waiting_finish_) {
    if (!msg_db_.emplace(key, std::forward<T>(v)).second) {
      SendAsyncImpl(kAckKey, ByteContainerView{});
      SPDLOG_WARN("Duplicate key {}", key);
    }
  } else {
    SendAsyncImpl(kAckKey, ByteContainerView{});
    SPDLOG_WARN("Asymmetric logic exist, auto ack key {}", key);
  }
  msg_db_cond_.notify_all();
}

void ChannelBase::OnMessage(const std::string& key, ByteContainerView value) {
  std::unique_lock lock(msg_mutex_);
  if (key == kAckKey) {
    ack_msg_count_++;
    ack_fin_cond_.notify_all();
  } else if (key == kFinKey) {
    YASL_ENFORCE(value.size() == sizeof(size_t));
    if (!received_fin_) {
      received_fin_ = true;
      std::memcpy(&peer_sent_msg_count_, value.data(), sizeof(size_t));
      ack_fin_cond_.notify_all();
    }
  } else {
    OnNormalMessage(key, value);
  }
}

void ChannelBase::OnChunkedMessage(const std::string& key,
                                   ByteContainerView value, size_t chunk_idx,
                                   size_t num_chunks) {
  YASL_ENFORCE(key != kAckKey && key != kFinKey,
               "For developer: pls use another key for normal message.");
  if (chunk_idx >= num_chunks) {
    YASL_THROW_LOGIC_ERROR("invalid chunk info, index={}, size={}", chunk_idx,
                           num_chunks);
  }

  std::shared_ptr<ChunkedMessage> data;
  {
    std::unique_lock lock(chunked_values_mutex_);
    auto itr = chunked_values_.find(key);
    if (itr == chunked_values_.end()) {
      itr = chunked_values_
                .emplace(key, std::make_shared<ChunkedMessage>(num_chunks))
                .first;
    }
    data = itr->second;
  }

  data->AddChunk(chunk_idx, value);
  {
    bool should_reassemble = false;
    if (data->IsFullyFilled()) {
      // two threads may arrive here at same time.
      std::unique_lock lock(chunked_values_mutex_);
      auto const& itr = chunked_values_.find(key);
      if (itr == chunked_values_.end()) {
        // this data block is handled by another chunk, just return.
        return;
      }

      chunked_values_.erase(key);

      // only one thread do the reassemble
      should_reassemble = true;
    }

    if (should_reassemble) {
      // notify new value arrived.
      auto reassembled_data = data->Reassemble();
      std::unique_lock lock(msg_mutex_);
      OnNormalMessage(key, std::move(reassembled_data));
    }
  }
}

void ChannelBase::SetRecvTimeout(uint32_t recv_timeout_ms) {
  recv_timeout_ms_ = recv_timeout_ms;
}

uint32_t ChannelBase::GetRecvTimeout() const { return recv_timeout_ms_; }

void ChannelBase::SendAsync(const std::string& key, ByteContainerView value) {
  YASL_ENFORCE(key != kAckKey && key != kFinKey,
               "For developer: pls use another key for normal message.");
  SendAsyncImpl(key, value);
  ThrottleWindowWait(sent_msg_count_.fetch_add(1) + 1);
}

void ChannelBase::SendAsync(const std::string& key, Buffer&& value) {
  YASL_ENFORCE(key != kAckKey && key != kFinKey,
               "For developer: pls use another key for normal message.");
  SendAsyncImpl(key, value);
  ThrottleWindowWait(sent_msg_count_.fetch_add(1) + 1);
}

void ChannelBase::Send(const std::string& key, ByteContainerView value) {
  YASL_ENFORCE(key != kAckKey && key != kFinKey,
               "For developer: pls use another key for normal message.");
  SendImpl(key, value);
  ThrottleWindowWait(sent_msg_count_.fetch_add(1) + 1);
}

// all sender thread wait on it's send order.
void ChannelBase::ThrottleWindowWait(size_t wait_count) {
  if (throttle_window_size_ == 0) {
    return;
  }
  std::unique_lock<std::mutex> lock(msg_mutex_);
  const auto& duration = std::chrono::milliseconds(recv_timeout_ms_);
  if (!ack_fin_cond_.wait_for(lock, duration, [&] {
        return (throttle_window_size_ == 0) ||
               (ack_msg_count_ + throttle_window_size_ > wait_count);
      })) {
    YASL_THROW_IO_ERROR("Throttle window wait timeout");
  }
}

void ChannelBase::WaitForFinAndFlyingMsg() {
  size_t sent_msg_count = sent_msg_count_;
  SendAsyncImpl(
      kFinKey, ByteContainerView{reinterpret_cast<const char*>(&sent_msg_count),
                                 sizeof(size_t)});
  {
    std::unique_lock<std::mutex> lock(msg_mutex_);
    ack_fin_cond_.wait(lock, [&] { return received_fin_; });
  }
  {
    std::unique_lock<std::mutex> lock(msg_mutex_);
    msg_db_cond_.wait(
        lock, [&] { return received_msg_count_ >= peer_sent_msg_count_; });
    if (received_msg_count_ > peer_sent_msg_count_) {
      // brpc will reply msg if connection is break (not timeout!), may cause
      // duplicate msg. e.g. alice's gateway pod is migrated before revice bob's
      // responce. in this rare case we may revice one msg more than once.
      // received msg count will greater then expected count.
      SPDLOG_WARN("duplicated msg exist during running");
    }
  }
}

void ChannelBase::StopReceivingAndAckUnreadMsgs() {
  std::unique_lock<std::mutex> lock(msg_mutex_);
  waiting_finish_ = true;
  for (auto& msg : msg_db_) {
    SPDLOG_WARN("Asymmetric logic exist, clear unread key {}", msg.first);
    SendAsyncImpl(kAckKey, ByteContainerView{});
  }
  msg_db_.clear();
}

void ChannelBase::WaitForFlyingAck() {
  std::unique_lock<std::mutex> lock(msg_mutex_);
  ack_fin_cond_.wait(lock, [&] { return ack_msg_count_ >= sent_msg_count_; });
  if (ack_msg_count_ > sent_msg_count_) {
    // brpc will reply msg if connection is break (not timeout!), may cause
    // duplicate msg. e.g. alice's gateway pod is migrated before revice bob's
    // responce. in this rare case we may revice one msg more than once.
    // received msg count will greater then expected count.
    SPDLOG_WARN("duplicated msg exist during running");
  }
}

void ChannelBase::WaitLinkTaskFinish() {
  // 4 steps to total stop link.
  // send ack for msg exist in msg_db_ that unread by up layer logic.
  // stop OnMessage & auto ack all normal msg from now on.
  StopReceivingAndAckUnreadMsgs();
  // wait for last fin msg contain peer's send msg count.
  // then check if received count is equal to peer's send count.
  // we can not close server port if peer still sending msg
  // or peer's gateway will throw 504 error.
  WaitForFinAndFlyingMsg();
  // make sure all Async send is finished.
  WaitAsyncSendToFinish();
  // at least, wait for all ack msg.
  WaitForFlyingAck();
  // after all, we can safely close server port and exit.
}

void ReceiverLoopBase::AddListener(size_t rank,
                                   std::shared_ptr<IChannel> listener) {
  auto ret = listeners_.emplace(rank, std::move(listener));
  if (!ret.second) {
    YASL_THROW_LOGIC_ERROR("duplicated listener for rank={}", rank);
  }
}

}  // namespace yasl::link
