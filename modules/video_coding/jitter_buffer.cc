/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "modules/video_coding/jitter_buffer.h"

#include <assert.h>

#include <algorithm>
#include <limits>
#include <utility>

#include "modules/video_coding/frame_buffer.h"
#include "modules/video_coding/include/video_coding.h"
#include "modules/video_coding/inter_frame_delay.h"
#include "modules/video_coding/internal_defines.h"
#include "modules/video_coding/jitter_buffer_common.h"
#include "modules/video_coding/jitter_estimator.h"
#include "modules/video_coding/packet.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "system_wrappers/include/clock.h"

namespace webrtc {
// Use this rtt if no value has been reported.
//Round-Trip Time
static const int64_t kDefaultRtt = 200;

//注意VCMFrameBuffer和FrameBuffer的区分
typedef std::pair<uint32_t, VCMFrameBuffer*> FrameListPair;

//是否是关键帧
bool IsKeyFrame(FrameListPair pair) {
  return pair.second->FrameType() == VideoFrameType::kVideoFrameKey;
}

//是否有非空状态
bool HasNonEmptyState(FrameListPair pair) {
  return pair.second->GetState() != kStateEmpty;
}

//在列表开始处添加帧。rbegin是末尾吧？？
void FrameList::InsertFrame(VCMFrameBuffer* frame) {
  insert(rbegin().base(), FrameListPair(frame->Timestamp(), frame));
}

//查找该时间戳的帧，并清空数据
VCMFrameBuffer* FrameList::PopFrame(uint32_t timestamp) {
  FrameList::iterator it = find(timestamp);
  if (it == end())
    return NULL;
  VCMFrameBuffer* frame = it->second;
  erase(it);
  return frame;
}

VCMFrameBuffer* FrameList::Front() const {
  return begin()->second;
}

VCMFrameBuffer* FrameList::Back() const {
  return rbegin()->second;
}

//清理帧，直到遇到关键帧，返回丢弃掉的帧数量，key_frame_it也返回下一个关键帧的位置
int FrameList::RecycleFramesUntilKeyFrame(FrameList::iterator* key_frame_it,
                                          UnorderedFrameList* free_frames) {
  int drop_count = 0;
  FrameList::iterator it = begin();
  while (!empty()) {
    // Throw at least one frame.
    //一定会丢弃第一帧，直到丢弃了一帧关键帧/或者列表空了为止
    it->second->Reset();
    free_frames->push_back(it->second);
    //erase函数：使用删除之前的迭代器定位下一个元素，STL建议的使用方式
    erase(it++);
    ++drop_count;
    if (it != end() &&
        it->second->FrameType() == VideoFrameType::kVideoFrameKey) {
      *key_frame_it = it;
      return drop_count;
    }
  }
  *key_frame_it = end();
  return drop_count;
}

//清理队列中所有旧的或者空的帧。
void FrameList::CleanUpOldOrEmptyFrames(VCMDecodingState* decoding_state,
                                        UnorderedFrameList* free_frames) {
  //如果列表非空，遍历所有的帧
  while (!empty()) {
    //取首帧
    VCMFrameBuffer* oldest_frame = Front();
    bool remove_frame = false;
    if (oldest_frame->GetState() == kStateEmpty && size() > 1) {
      // This frame is empty, try to update the last decoded state and drop it
      // if successful.
      //此帧为空，请尝试更新上次解码的状态，如果成功，请将其删除。
      remove_frame = decoding_state->UpdateEmptyFrame(oldest_frame);
    } else {
      remove_frame = decoding_state->IsOldFrame(oldest_frame);
    }
	//清空一帧，放入free_frames
    if (!remove_frame) {
      break;
    }
    free_frames->push_back(oldest_frame);
    erase(begin());
  }
}

//重置队列中所有的帧，放回到freeframes中。
void FrameList::Reset(UnorderedFrameList* free_frames) {
  while (!empty()) {
    begin()->second->Reset();
    free_frames->push_back(begin()->second);
    erase(begin());
  }
}

VCMJitterBuffer::VCMJitterBuffer(Clock* clock,
                                 std::unique_ptr<EventWrapper> event)
    : clock_(clock),
      running_(false),
      frame_event_(std::move(event)),
      max_number_of_frames_(kStartNumberOfFrames),
      free_frames_(),
      decodable_frames_(),
      incomplete_frames_(),
      last_decoded_state_(),
      first_packet_since_reset_(true),
      num_consecutive_old_packets_(0),
      num_packets_(0),
      num_duplicated_packets_(0),
      jitter_estimate_(clock),
      inter_frame_delay_(clock_->TimeInMilliseconds()),
      missing_sequence_numbers_(SequenceNumberLessThan()),
      latest_received_sequence_number_(0),
      max_nack_list_size_(0),
      max_packet_age_to_nack_(0),
      max_incomplete_time_ms_(0),
      average_packets_per_frame_(0.0f),
      frame_counter_(0) {
  for (int i = 0; i < kStartNumberOfFrames; i++)
    free_frames_.push_back(new VCMFrameBuffer());
}

VCMJitterBuffer::~VCMJitterBuffer() {
  Stop();
  //清空所有列表
  for (UnorderedFrameList::iterator it = free_frames_.begin();
       it != free_frames_.end(); ++it) {
    delete *it;
  }
  for (FrameList::iterator it = incomplete_frames_.begin();
       it != incomplete_frames_.end(); ++it) {
    delete it->second;
  }
  for (FrameList::iterator it = decodable_frames_.begin();
       it != decodable_frames_.end(); ++it) {
    delete it->second;
  }
}

void VCMJitterBuffer::Start() {
  MutexLock lock(&mutex_);
  running_ = true;

  num_consecutive_old_packets_ = 0;
  num_packets_ = 0;
  num_duplicated_packets_ = 0;

  // Start in a non-signaled state.
  waiting_for_completion_.frame_size = 0;
  waiting_for_completion_.timestamp = 0;
  waiting_for_completion_.latest_packet_time = -1;
  first_packet_since_reset_ = true;
  last_decoded_state_.Reset();

  //清空两个列表，放入free_frames_
  decodable_frames_.Reset(&free_frames_);
  incomplete_frames_.Reset(&free_frames_);
}

void VCMJitterBuffer::Stop() {
  MutexLock lock(&mutex_);
  running_ = false;
  last_decoded_state_.Reset();

  // Make sure we wake up any threads waiting on these events.
  frame_event_->Set();
}

bool VCMJitterBuffer::Running() const {
  MutexLock lock(&mutex_);
  return running_;
}

void VCMJitterBuffer::Flush() {
  MutexLock lock(&mutex_);
  decodable_frames_.Reset(&free_frames_);
  incomplete_frames_.Reset(&free_frames_);
  last_decoded_state_.Reset();  // TODO(mikhal): sync reset.
  num_consecutive_old_packets_ = 0;
  // Also reset the jitter and delay estimates
  jitter_estimate_.Reset();
  inter_frame_delay_.Reset(clock_->TimeInMilliseconds());
  waiting_for_completion_.frame_size = 0;
  waiting_for_completion_.timestamp = 0;
  waiting_for_completion_.latest_packet_time = -1;
  first_packet_since_reset_ = true;
  missing_sequence_numbers_.clear();
}

int VCMJitterBuffer::num_packets() const {
  MutexLock lock(&mutex_);
  return num_packets_;
}

int VCMJitterBuffer::num_duplicated_packets() const {
  MutexLock lock(&mutex_);
  return num_duplicated_packets_;
}

// Returns immediately or a |max_wait_time_ms| ms event hang waiting for a
// complete frame, |max_wait_time_ms| decided by caller.
//获取下一帧可以decode的帧，最长等待时长max_wait_time_ms
VCMEncodedFrame* VCMJitterBuffer::NextCompleteFrame(uint32_t max_wait_time_ms) {
  MutexLock lock(&mutex_);
  if (!running_) {
    return nullptr;
  }
  CleanUpOldOrEmptyFrames();

  if (decodable_frames_.empty() ||
      decodable_frames_.Front()->GetState() != kStateComplete) {
    //如果decodableframes为空，或者它第一帧不是complete状态，那么进行等待
    const int64_t end_wait_time_ms =
        clock_->TimeInMilliseconds() + max_wait_time_ms;
    int64_t wait_time_ms = max_wait_time_ms;
    while (wait_time_ms > 0) {
      mutex_.Unlock();
	  //进行等待
      const EventTypeWrapper ret =
          frame_event_->Wait(static_cast<uint32_t>(wait_time_ms));
      mutex_.Lock();
      if (ret == kEventSignaled) {
        // Are we shutting down the jitter buffer?
        if (!running_) {
          return nullptr;
        }
        // Finding oldest frame ready for decoder.
        CleanUpOldOrEmptyFrames();
        if (decodable_frames_.empty() ||
            decodable_frames_.Front()->GetState() != kStateComplete) {
          //计算剩余等待时间
          wait_time_ms = end_wait_time_ms - clock_->TimeInMilliseconds();
        } else {
          break;
        }
      } else {
        break;
      }
    }
  }
  //返回decodable_frames_的首帧
  if (decodable_frames_.empty() ||
      decodable_frames_.Front()->GetState() != kStateComplete) {
    return nullptr;
  }
  return decodable_frames_.Front();
}

// Extract frame corresponding to input timestamp.
// Frame will be set to a decoding state.
//提取与输入时间戳对应的帧。帧将被设置为解码状态。
VCMEncodedFrame* VCMJitterBuffer::ExtractAndSetDecode(uint32_t timestamp) {
  MutexLock lock(&mutex_);
  if (!running_) {
    return NULL;
  }
  // Extract the frame with the desired timestamp.
  //先在decode里取一帧
  VCMFrameBuffer* frame = decodable_frames_.PopFrame(timestamp);
  bool continuous = true;
  if (!frame) {
    //decodable中没有，在incomplete里拿
    frame = incomplete_frames_.PopFrame(timestamp);
    if (frame)
      //incomplete中的frame要和last_decode_state中ContinuousFrame合成
      continuous = last_decoded_state_.ContinuousFrame(frame);
    else
      return NULL;
  }
  // Frame pulled out from jitter buffer, update the jitter estimate.
  //nack数量大于0就要重传？
  const bool retransmitted = (frame->GetNackCount() > 0);
  if (retransmitted) {
    jitter_estimate_.FrameNacked();
  } else if (frame->size() > 0) {
    // Ignore retransmitted and empty frames.
    //忽略重传的和空的帧
    if (waiting_for_completion_.latest_packet_time >= 0) {
      UpdateJitterEstimate(waiting_for_completion_, true);
    }
    if (frame->GetState() == kStateComplete) {
      //这一帧完整了
      UpdateJitterEstimate(*frame, false);
    } else {
      // Wait for this one to get complete.
      //这一帧不完整
      waiting_for_completion_.frame_size = frame->size();
      waiting_for_completion_.latest_packet_time = frame->LatestPacketTimeMs();
      waiting_for_completion_.timestamp = frame->Timestamp();
    }
  }

  // The state must be changed to decoding before cleaning up zero sized
  // frames to avoid empty frames being cleaned up and then given to the
  // decoder. Propagates the missing_frame bit.
  //通知解码器
  frame->PrepareForDecode(continuous);

  // We have a frame - update the last decoded state and nack list.
  last_decoded_state_.SetState(frame);
  DropPacketsFromNackList(last_decoded_state_.sequence_num());

  if ((*frame).IsSessionComplete())
    UpdateAveragePacketsPerFrame(frame->NumPackets());

  return frame;
}

// Release frame when done with decoding. Should never be used to release
// frames from within the jitter buffer.
//解码完后释放帧
void VCMJitterBuffer::ReleaseFrame(VCMEncodedFrame* frame) {
  RTC_CHECK(frame != nullptr);
  MutexLock lock(&mutex_);
  VCMFrameBuffer* frame_buffer = static_cast<VCMFrameBuffer*>(frame);
  RecycleFrameBuffer(frame_buffer);
}

// Gets frame to use for this timestamp. If no match, get empty frame.
//根据packet里的包，找buffer，返回找到的帧的状态VCMFrameBufferEnum
//网上总结：
//从decodable_frames_取出一帧用于解码
//更新jitterestimate
//当前帧状态更新
//当前解码状态更新
//丢掉nacklist无用的包
//输出：frame，frame_list，结果VCMFrameBufferEnum
VCMFrameBufferEnum VCMJitterBuffer::GetFrame(const VCMPacket& packet,
                                             VCMFrameBuffer** frame,
                                             FrameList** frame_list) {
  *frame = incomplete_frames_.PopFrame(packet.timestamp);
  if (*frame != NULL) {
    *frame_list = &incomplete_frames_;
    return kNoError;
  }
  *frame = decodable_frames_.PopFrame(packet.timestamp);
  if (*frame != NULL) {
    *frame_list = &decodable_frames_;
    return kNoError;
  }

  *frame_list = NULL;
  // No match, return empty frame.
  //获取空帧
  *frame = GetEmptyFrame();
  //获取不到空帧
  if (*frame == NULL) {
    // No free frame! Try to reclaim some...
    RTC_LOG(LS_WARNING) << "Unable to get empty frame; Recycling.";
	//找不到空间了，回收一下帧，直到碰到关键帧
    bool found_key_frame = RecycleFramesUntilKeyFrame();
    *frame = GetEmptyFrame();
    RTC_CHECK(*frame);
    if (!found_key_frame) {
      //还是没空间，直接释放一帧好了
      RecycleFrameBuffer(*frame);
      return kFlushIndicator;
    }
  }
  (*frame)->Reset();
  return kNoError;
}

//最新的packet时间, 同时返回是否重传retransmitted
int64_t VCMJitterBuffer::LastPacketTime(const VCMEncodedFrame* frame,
                                        bool* retransmitted) const {
  assert(retransmitted);
  MutexLock lock(&mutex_);
  const VCMFrameBuffer* frame_buffer =
      static_cast<const VCMFrameBuffer*>(frame);
  //Nack数大于0就重传了？
  *retransmitted = (frame_buffer->GetNackCount() > 0);
  return frame_buffer->LatestPacketTimeMs();
}

//插入packet
//1. 按时间戳进行排序
//2. 更新包的依赖状态，例如p帧依赖于前面的i帧，依赖帧都ok更新continue标志
//3. 保存包接收时间等状态
VCMFrameBufferEnum VCMJitterBuffer::InsertPacket(const VCMPacket& packet,
                                                 bool* retransmitted) {
  MutexLock lock(&mutex_);

  ++num_packets_;
  // Does this packet belong to an old frame?
  //这个包是否属于一个旧的帧？
  //利用packet里的时间戳和last_decoded_state_里的时间戳去比
  if (last_decoded_state_.IsOldPacket(&packet)) {
    // Account only for media packets.
    //有数据时，num_consecutive_old_packets++，只统计media packets？
    if (packet.sizeBytes > 0) {
      num_consecutive_old_packets_++;
    }
    // Update last decoded sequence number if the packet arrived late and
    // belongs to a frame with a timestamp equal to the last decoded
    // timestamp.
    //这个packet来晚了,如果包延迟到达并且属于时间戳等于最后解码时间戳的帧，则更新最后解码序列号。
    last_decoded_state_.UpdateOldPacket(&packet);
	//丢弃数据包，根据最新的seq_num
    DropPacketsFromNackList(last_decoded_state_.sequence_num());

    // Also see if this old packet made more incomplete frames continuous.
    //还要看看这个旧包是否使更多不完整的帧连续。
    FindAndInsertContinuousFramesWithState(last_decoded_state_);

    if (num_consecutive_old_packets_ > kMaxConsecutiveOldPackets) {
      RTC_LOG(LS_WARNING)
          << num_consecutive_old_packets_
          << " consecutive old packets received. Flushing the jitter buffer.";
      Flush();
      return kFlushIndicator;
    }
    return kOldPacket;
  }

  num_consecutive_old_packets_ = 0;

  VCMFrameBuffer* frame;
  FrameList* frame_list;
  const VCMFrameBufferEnum error = GetFrame(packet, &frame, &frame_list);
  if (error != kNoError)
    return error;

  int64_t now_ms = clock_->TimeInMilliseconds();
  // We are keeping track of the first and latest seq numbers, and
  // the number of wraps to be able to calculate how many packets we expect.
  //我们跟踪第一个和最新的序列号，以及packet的数量，以便能够计算出我们期望的数据包数量。
  if (first_packet_since_reset_) {
    // Now it's time to start estimating jitter
    // reset the delay estimate.
    //reset后的第一个packet,重置inter_frame_delay_
    inter_frame_delay_.Reset(now_ms);
  }

  // Empty packets may bias the jitter estimate (lacking size component),
  // therefore don't let empty packet trigger the following updates:
  //空白包不参与jitter estimate的估算
  if (packet.video_header.frame_type != VideoFrameType::kEmptyFrame) {
  	//packet非空，处理timestamp相等的和相差在2s以内的情况
    if (waiting_for_completion_.timestamp == packet.timestamp) {
      // This can get bad if we have a lot of duplicate packets,
      // we will then count some packet multiple times.
      waiting_for_completion_.frame_size += packet.sizeBytes;
      waiting_for_completion_.latest_packet_time = now_ms;
    } else if (waiting_for_completion_.latest_packet_time >= 0 &&
               waiting_for_completion_.latest_packet_time + 2000 <= now_ms) {
      // A packet should never be more than two seconds late
      UpdateJitterEstimate(waiting_for_completion_, true);
      waiting_for_completion_.latest_packet_time = -1;
      waiting_for_completion_.frame_size = 0;
      waiting_for_completion_.timestamp = 0;
    }
  }

  VCMFrameBufferStateEnum previous_state = frame->GetState();
  // Insert packet.
  FrameData frame_data;
  frame_data.rtt_ms = kDefaultRtt;
  frame_data.rolling_average_packets_per_frame = average_packets_per_frame_;
  //frame是GetFrame获取得到的一个frame
  VCMFrameBufferEnum buffer_state =
      frame->InsertPacket(packet, now_ms, frame_data);

  if (buffer_state > 0) {
    if (first_packet_since_reset_) {
      latest_received_sequence_number_ = packet.seqNum;
      first_packet_since_reset_ = false;
    } else {
      if (IsPacketRetransmitted(packet)) {
        frame->IncrementNackCount();
      }
      if (!UpdateNackList(packet.seqNum) &&
          packet.video_header.frame_type != VideoFrameType::kVideoFrameKey) {
        buffer_state = kFlushIndicator;
      }

      latest_received_sequence_number_ =
          LatestSequenceNumber(latest_received_sequence_number_, packet.seqNum);
    }
  }

  // Is the frame already in the decodable list?
  bool continuous = IsContinuous(*frame);
  switch (buffer_state) {
    case kGeneralError:
    case kTimeStampError:
    case kSizeError: {
      RecycleFrameBuffer(frame);
      break;
    }
    case kCompleteSession: {
      if (previous_state != kStateComplete) {
        if (continuous) {
          // Signal that we have a complete session.
          //通知说有一个完整的session了
          frame_event_->Set();
        }
      }
      //是否需要重传
      *retransmitted = (frame->GetNackCount() > 0);
      //根据是否连续判断加入decodable还是incomplete
      if (continuous) {
        decodable_frames_.InsertFrame(frame);
        FindAndInsertContinuousFrames(*frame);
      } else {
        incomplete_frames_.InsertFrame(frame);
      }
      break;
    }
    case kIncomplete: {
      if (frame->GetState() == kStateEmpty &&
          last_decoded_state_.UpdateEmptyFrame(frame)) {
        //未完整状态，空帧然后能让buffer进行清空操作
        RecycleFrameBuffer(frame);
        return kNoError;
      } else {
        incomplete_frames_.InsertFrame(frame);
      }
      break;
    }
    case kNoError:
    case kOutOfBoundsPacket:
    case kDuplicatePacket: {
      // Put back the frame where it came from.
      if (frame_list != NULL) {
        frame_list->InsertFrame(frame);
      } else {
        RecycleFrameBuffer(frame);
      }
      ++num_duplicated_packets_;
      break;
    }
    case kFlushIndicator:
      RecycleFrameBuffer(frame);
      return kFlushIndicator;
    default:
      assert(false);
  }
  return buffer_state;
}

//判断该帧是否是完整的或者连续的
bool VCMJitterBuffer::IsContinuousInState(
    const VCMFrameBuffer& frame,
    const VCMDecodingState& decoding_state) const {
  // Is this frame complete and continuous?
  return (frame.GetState() == kStateComplete) &&
         decoding_state.ContinuousFrame(&frame);
}

//判断是否是连续的frame
bool VCMJitterBuffer::IsContinuous(const VCMFrameBuffer& frame) const {
  if (IsContinuousInState(frame, last_decoded_state_)) {
    return true;
  }
  VCMDecodingState decoding_state;
  decoding_state.CopyFrom(last_decoded_state_);
  for (FrameList::const_iterator it = decodable_frames_.begin();
       it != decodable_frames_.end(); ++it) {
    VCMFrameBuffer* decodable_frame = it->second;
    if (IsNewerTimestamp(decodable_frame->Timestamp(), frame.Timestamp())) {
      //时间比frame晚的decodable_frame就忽略了
      break;
    }
	//循环遍历,选择一个满足条件的decodable_frame,setState,然后如果判断state连续则返回true
	//setState也只是修改本地的变量,不影响类变量
    decoding_state.SetState(decodable_frame);
    if (IsContinuousInState(frame, decoding_state)) {
      return true;
    }
  }
  return false;
}

//连续帧,找到并插入其应当所在的位置
void VCMJitterBuffer::FindAndInsertContinuousFrames(
    const VCMFrameBuffer& new_frame) {
  //都是利用复制的decoding_state来操作的?
  VCMDecodingState decoding_state;
  decoding_state.CopyFrom(last_decoded_state_);
  decoding_state.SetState(&new_frame);
  FindAndInsertContinuousFramesWithState(decoding_state);
}

//找到并插入连续帧之间
void VCMJitterBuffer::FindAndInsertContinuousFramesWithState(
    const VCMDecodingState& original_decoded_state) {
  // Copy original_decoded_state so we can move the state forward with each
  // decodable frame we find.
  //复制备份
  VCMDecodingState decoding_state;
  decoding_state.CopyFrom(original_decoded_state);

  // When temporal layers are available, we search for a complete or decodable
  // frame until we hit one of the following:
  // 1. Continuous base or sync layer.
  // 2. The end of the list was reached.
  //当时间层可用时，我们会搜索一个完整的或可解码的帧，直到找到以下其中一个：
  //1. 连续基本层或同步层。
  //2. 已到达列表的末尾。
  for (FrameList::iterator it = incomplete_frames_.begin();
       it != incomplete_frames_.end();) {
    VCMFrameBuffer* frame = it->second;
    if (IsNewerTimestamp(original_decoded_state.time_stamp(),
                         frame->Timestamp())) {
      //timestamp较小的continue跳过
      ++it;
      continue;
    }
    if (IsContinuousInState(*frame, decoding_state)) {
      //可以解码了
      decodable_frames_.InsertFrame(frame);
      incomplete_frames_.erase(it++);
      decoding_state.SetState(frame);
    } else if (frame->TemporalId() <= 0) {
      break;
    } else {
      ++it;
    }
  }
}

//调用jitter_estimate的估计jitterMs的方法
uint32_t VCMJitterBuffer::EstimatedJitterMs() {
  MutexLock lock(&mutex_);
  const double rtt_mult = 1.0f;
  return jitter_estimate_.GetJitterEstimate(rtt_mult, absl::nullopt);
}

//设置nack参数
void VCMJitterBuffer::SetNackSettings(size_t max_nack_list_size,
                                      int max_packet_age_to_nack,
                                      int max_incomplete_time_ms) {
  MutexLock lock(&mutex_);
  assert(max_packet_age_to_nack >= 0);
  assert(max_incomplete_time_ms_ >= 0);
  max_nack_list_size_ = max_nack_list_size;
  max_packet_age_to_nack_ = max_packet_age_to_nack;
  max_incomplete_time_ms_ = max_incomplete_time_ms;
}

//返回不连续或者不完整的帧时长
int VCMJitterBuffer::NonContinuousOrIncompleteDuration() {
  if (incomplete_frames_.empty()) {
    return 0;
  }
  //incomplete列表里首项的时间作为开始时间，如果decodable非空，改用decodable的最后一项的时间
  uint32_t start_timestamp = incomplete_frames_.Front()->Timestamp();
  if (!decodable_frames_.empty()) {
    start_timestamp = decodable_frames_.Back()->Timestamp();
  }
  return incomplete_frames_.Back()->Timestamp() - start_timestamp;
}

//估计low sequence number
uint16_t VCMJitterBuffer::EstimatedLowSequenceNumber(
    const VCMFrameBuffer& frame) const {
  assert(frame.GetLowSeqNum() >= 0);
  if (frame.HaveFirstPacket())
    return frame.GetLowSeqNum();

  // This estimate is not accurate if more than one packet with lower sequence
  // number is lost.
  //如果丢失多个序列号较低的包，则此估计不准确。
  return frame.GetLowSeqNum() - 1;
}

//获取nack列表,返回值是nack列表,同时更新bool值request_key_frame
std::vector<uint16_t> VCMJitterBuffer::GetNackList(bool* request_key_frame) {
  MutexLock lock(&mutex_);
  *request_key_frame = false;
  if (last_decoded_state_.in_initial_state()) {
  	//初始化时
    VCMFrameBuffer* next_frame = NextFrame();
	//判断首个frame是不是key frame
    const bool first_frame_is_key =
        next_frame &&
        next_frame->FrameType() == VideoFrameType::kVideoFrameKey &&
        next_frame->HaveFirstPacket();
    if (!first_frame_is_key) {
      //如果首个frame不是key frame,那么从decodable和incomplete队列中查找非空的frame
      bool have_non_empty_frame =
          decodable_frames_.end() != find_if(decodable_frames_.begin(),
                                             decodable_frames_.end(),
                                             HasNonEmptyState);
      if (!have_non_empty_frame) {
        have_non_empty_frame =
            incomplete_frames_.end() != find_if(incomplete_frames_.begin(),
                                                incomplete_frames_.end(),
                                                HasNonEmptyState);
      }
      bool found_key_frame = RecycleFramesUntilKeyFrame();
      if (!found_key_frame) {
	  	//如果没碰到关键帧,但是有非空frame,说明需要请求关键帧,request_key_frame
        *request_key_frame = have_non_empty_frame;
        return std::vector<uint16_t>();
      }
    }
  }
  if (TooLargeNackList()) {
    *request_key_frame = !HandleTooLargeNackList();
  }
  //max_incomplete_time_ms_这个参数只在SetNackSettings的时候赋值
  if (max_incomplete_time_ms_ > 0) {
    int non_continuous_incomplete_duration =
        NonContinuousOrIncompleteDuration();
    if (non_continuous_incomplete_duration > 90 * max_incomplete_time_ms_) {
      RTC_LOG_F(LS_WARNING) << "Too long non-decodable duration: "
                            << non_continuous_incomplete_duration << " > "
                            << 90 * max_incomplete_time_ms_;
	  //反向查找看是否有关键帧
      FrameList::reverse_iterator rit = find_if(
          incomplete_frames_.rbegin(), incomplete_frames_.rend(), IsKeyFrame);
      if (rit == incomplete_frames_.rend()) {
        // Request a key frame if we don't have one already.
        //如果没有关键帧
        *request_key_frame = true;
        return std::vector<uint16_t>();
      } else {
        // Skip to the last key frame. If it's incomplete we will start
        // NACKing it.
        // Note that the estimated low sequence number is correct for VP8
        // streams because only the first packet of a key frame is marked.
        //跳到最后一个关键帧。如果它不完整，我们就开始处理它。
        //注意，对于VP8流，估计的低序列号是正确的，因为只标记关键帧的第一个分组
        last_decoded_state_.Reset();
        DropPacketsFromNackList(EstimatedLowSequenceNumber(*rit->second));
      }
    }
  }
  std::vector<uint16_t> nack_list(missing_sequence_numbers_.begin(),
                                  missing_sequence_numbers_.end());
  return nack_list;
}

//下一帧，先从decodable中取，没有的话从incomplete中取，再没有的话返回NULL
VCMFrameBuffer* VCMJitterBuffer::NextFrame() const {
  if (!decodable_frames_.empty())
    return decodable_frames_.Front();
  if (!incomplete_frames_.empty())
    return incomplete_frames_.Front();
  return NULL;
}

//更新nack列表,返回值true表示正常,false表示异常,会导致buffer_state = kFlushIndicator
bool VCMJitterBuffer::UpdateNackList(uint16_t sequence_number) {
  // Make sure we don't add packets which are already too old to be decoded.
  //确保我们不要添加已经太旧无法解码的数据包。
  if (!last_decoded_state_.in_initial_state()) {
  	//更新seqNum
    latest_received_sequence_number_ = LatestSequenceNumber(
        latest_received_sequence_number_, last_decoded_state_.sequence_num());
  }
  if (IsNewerSequenceNumber(sequence_number,
                            latest_received_sequence_number_)) {
    //如果latest_received_sequence_number_ < sequence_number，说明seqNum有跳过的情况
    // Push any missing sequence numbers to the NACK list.
    //把缺失的seqNum补充到missing列表中
    for (uint16_t i = latest_received_sequence_number_ + 1;
         IsNewerSequenceNumber(sequence_number, i); ++i) {
      missing_sequence_numbers_.insert(missing_sequence_numbers_.end(), i);
    }
    if (TooLargeNackList() && !HandleTooLargeNackList()) {
      RTC_LOG(LS_WARNING) << "Requesting key frame due to too large NACK list.";
      return false;
    }
    if (MissingTooOldPacket(sequence_number) &&
        !HandleTooOldPackets(sequence_number)) {
      RTC_LOG(LS_WARNING)
          << "Requesting key frame due to missing too old packets";
      return false;
    }
  } else {
    //如果latest_received_sequence_number_ > sequence_number，说明正常
    missing_sequence_numbers_.erase(sequence_number);
  }
  return true;
}

//判断是否nack列表过长
bool VCMJitterBuffer::TooLargeNackList() const {
  return missing_sequence_numbers_.size() > max_nack_list_size_;
}

//处理过长的nack列表,返回是否能够找到关键帧
bool VCMJitterBuffer::HandleTooLargeNackList() {
  // Recycle frames until the NACK list is small enough. It is likely cheaper to
  // request a key frame than to retransmit this many missing packets.
  //请求一个关键帧可能比重传这么多丢失的packet更好
  RTC_LOG_F(LS_WARNING) << "NACK list has grown too large: "
                        << missing_sequence_numbers_.size() << " > "
                        << max_nack_list_size_;
  bool key_frame_found = false;
  while (TooLargeNackList()) {
    key_frame_found = RecycleFramesUntilKeyFrame();
  }
  return key_frame_found;
}

//是否丢失太老的packet
bool VCMJitterBuffer::MissingTooOldPacket(
    uint16_t latest_sequence_number) const {
  if (missing_sequence_numbers_.empty()) {
    return false;
  }
  const uint16_t age_of_oldest_missing_packet =
      latest_sequence_number - *missing_sequence_numbers_.begin();
  // Recycle frames if the NACK list contains too old sequence numbers as
  // the packets may have already been dropped by the sender.
  //如果NACK列表包含太旧的序列号，则回收帧，因为数据包可能已经被发送方丢弃。
  return age_of_oldest_missing_packet > max_packet_age_to_nack_;
}

//处理太老的packet,返回是否能够找到关键帧
bool VCMJitterBuffer::HandleTooOldPackets(uint16_t latest_sequence_number) {
  bool key_frame_found = false;
  const uint16_t age_of_oldest_missing_packet =
      latest_sequence_number - *missing_sequence_numbers_.begin();
  RTC_LOG_F(LS_WARNING) << "NACK list contains too old sequence numbers: "
                        << age_of_oldest_missing_packet << " > "
                        << max_packet_age_to_nack_;
  while (MissingTooOldPacket(latest_sequence_number)) {
    key_frame_found = RecycleFramesUntilKeyFrame();
  }
  return key_frame_found;
}

//根据序列号，从Nack列表中丢弃掉包
//initial_state的时候需要调用
void VCMJitterBuffer::DropPacketsFromNackList(
    uint16_t last_decoded_sequence_number) {
  // Erase all sequence numbers from the NACK list which we won't need any
  // longer.
  missing_sequence_numbers_.erase(
      missing_sequence_numbers_.begin(),
      missing_sequence_numbers_.upper_bound(last_decoded_sequence_number));
}

//获取空帧对象，如果没有空间了，需要尝试增大空间
VCMFrameBuffer* VCMJitterBuffer::GetEmptyFrame() {
  if (free_frames_.empty()) {
    if (!TryToIncreaseJitterBufferSize()) {
      return NULL;
    }
  }
  VCMFrameBuffer* frame = free_frames_.front();
  free_frames_.pop_front();
  return frame;
}

//尝试增大空间，最大kMaxNumberOfFrames=300
bool VCMJitterBuffer::TryToIncreaseJitterBufferSize() {
  if (max_number_of_frames_ >= kMaxNumberOfFrames)
    return false;
  free_frames_.push_back(new VCMFrameBuffer());
  ++max_number_of_frames_;
  return true;
}

// Recycle oldest frames up to a key frame, used if jitter buffer is completely
// full.
//释放帧空间，直到碰到关键帧,返回值表示是否碰到关键帧
bool VCMJitterBuffer::RecycleFramesUntilKeyFrame() {
  // First release incomplete frames, and only release decodable frames if there
  // are no incomplete ones.
  //首先释放不完整的帧，然后只当没有不完整帧时才释放decodable的帧
  FrameList::iterator key_frame_it;
  bool key_frame_found = false;
  int dropped_frames = 0;
  dropped_frames += incomplete_frames_.RecycleFramesUntilKeyFrame(
      &key_frame_it, &free_frames_);
  key_frame_found = key_frame_it != incomplete_frames_.end();
  if (dropped_frames == 0) {
    //没有不完整帧时才释放decodable的帧
    dropped_frames += decodable_frames_.RecycleFramesUntilKeyFrame(
        &key_frame_it, &free_frames_);
    key_frame_found = key_frame_it != decodable_frames_.end();
  }
  if (key_frame_found) {
    RTC_LOG(LS_INFO) << "Found key frame while dropping frames.";
    // Reset last decoded state to make sure the next frame decoded is a key
    // frame, and start NACKing from here.
    //重设last_decoded_state_，确保下一帧是关键帧，然后从这里开始NACKing
    last_decoded_state_.Reset();
    DropPacketsFromNackList(EstimatedLowSequenceNumber(*key_frame_it->second));
  } else if (decodable_frames_.empty()) {
    // All frames dropped. Reset the decoding state and clear missing sequence
    // numbers as we're starting fresh.
    //所有帧都掉了。重新设置解码状态并清除丢失的序列号。
    last_decoded_state_.Reset();
    missing_sequence_numbers_.clear();
  }
  return key_frame_found;
}

//更新每一帧的平均packet数量
void VCMJitterBuffer::UpdateAveragePacketsPerFrame(int current_number_packets) {
  if (frame_counter_ > kFastConvergeThreshold) {
    //数量多时，正常收敛
    average_packets_per_frame_ =
        average_packets_per_frame_ * (1 - kNormalConvergeMultiplier) +
        current_number_packets * kNormalConvergeMultiplier;
  } else if (frame_counter_ > 0) {
    //数量少时，快速收敛
    average_packets_per_frame_ =
        average_packets_per_frame_ * (1 - kFastConvergeMultiplier) +
        current_number_packets * kFastConvergeMultiplier;
    frame_counter_++;
  } else {
    //初始化
    average_packets_per_frame_ = current_number_packets;
    frame_counter_++;
  }
}

// Must be called under the critical section |mutex_|.
//删除旧的或者空的帧
void VCMJitterBuffer::CleanUpOldOrEmptyFrames() {
  decodable_frames_.CleanUpOldOrEmptyFrames(&last_decoded_state_,
                                            &free_frames_);
  incomplete_frames_.CleanUpOldOrEmptyFrames(&last_decoded_state_,
                                             &free_frames_);
  if (!last_decoded_state_.in_initial_state()) {
    DropPacketsFromNackList(last_decoded_state_.sequence_num());
  }
}

// Must be called from within |mutex_|.
//是否是重传的包？看序号是否是missing队列里的,如果是missing队列里的则说明是重传的包
bool VCMJitterBuffer::IsPacketRetransmitted(const VCMPacket& packet) const {
  return missing_sequence_numbers_.find(packet.seqNum) !=
         missing_sequence_numbers_.end();
}

// Must be called under the critical section |mutex_|. Should never be
// called with retransmitted frames, they must be filtered out before this
// function is called.
//必须在关键部分|互斥体|下调用。不应使用重新传输的帧调用，必须在调用此函数之前过滤掉这些帧。
void VCMJitterBuffer::UpdateJitterEstimate(const VCMJitterSample& sample,
                                           bool incomplete_frame) {
  if (sample.latest_packet_time == -1) {
    return;
  }
  UpdateJitterEstimate(sample.latest_packet_time, sample.timestamp,
                       sample.frame_size, incomplete_frame);
}

// Must be called under the critical section mutex_. Should never be
// called with retransmitted frames, they must be filtered out before this
// function is called.
//必须在关键部分|互斥体|下调用。不应使用重新传输的帧调用，必须在调用此函数之前过滤掉这些帧。
void VCMJitterBuffer::UpdateJitterEstimate(const VCMFrameBuffer& frame,
                                           bool incomplete_frame) {
  if (frame.LatestPacketTimeMs() == -1) {
    return;
  }
  // No retransmitted frames should be a part of the jitter
  // estimate.
  UpdateJitterEstimate(frame.LatestPacketTimeMs(), frame.Timestamp(),
                       frame.size(), incomplete_frame);
}

// Must be called under the critical section |mutex_|. Should never be
// called with retransmitted frames, they must be filtered out before this
// function is called.
//卡尔曼滤波更新估计的卡顿时间jitter estimate
void VCMJitterBuffer::UpdateJitterEstimate(int64_t latest_packet_time_ms,
                                           uint32_t timestamp,
                                           unsigned int frame_size,
                                           bool incomplete_frame) {
  if (latest_packet_time_ms == -1) {
    return;
  }
  int64_t frame_delay;
  //返回值表明没有重排序?
  bool not_reordered = inter_frame_delay_.CalculateDelay(
      timestamp, &frame_delay, latest_packet_time_ms);
  // Filter out frames which have been reordered in time by the network
  //重排序的就不进if,不参与估计jitter_estimate_了
  if (not_reordered) {
    // Update the jitter estimate with the new samples
    jitter_estimate_.UpdateEstimate(frame_delay, frame_size, incomplete_frame);
  }
}

//释放一帧，放回freeframes
void VCMJitterBuffer::RecycleFrameBuffer(VCMFrameBuffer* frame) {
  frame->Reset();
  free_frames_.push_back(frame);
}

}  // namespace webrtc
