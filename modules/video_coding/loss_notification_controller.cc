/*
 *  Copyright (c) 2019 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/loss_notification_controller.h"

#include <stdint.h>

#include "api/array_view.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/numerics/sequence_number_util.h"

namespace webrtc {
namespace {
// Keep a container's size no higher than |max_allowed_size|, by paring its size
// down to |target_size| whenever it has more than |max_allowed_size| elements.
//当数量超过max_allowed_size时，删除一些直到target_size
template <typename Container>
void PareDown(Container* container,
              size_t max_allowed_size,
              size_t target_size) {
  if (container->size() > max_allowed_size) {
    const size_t entries_to_delete = container->size() - target_size;
    auto erase_to = container->begin();
    std::advance(erase_to, entries_to_delete);
    container->erase(container->begin(), erase_to);
    RTC_DCHECK_EQ(container->size(), target_size);
  }
}
}  // namespace

LossNotificationController::LossNotificationController(
    KeyFrameRequestSender* key_frame_request_sender,
    LossNotificationSender* loss_notification_sender)
    : key_frame_request_sender_(key_frame_request_sender),
      loss_notification_sender_(loss_notification_sender),
      current_frame_potentially_decodable_(true) {
  RTC_DCHECK(key_frame_request_sender_);
  RTC_DCHECK(loss_notification_sender_);
}

LossNotificationController::~LossNotificationController() = default;

//收到RTP包
void LossNotificationController::OnReceivedPacket(
    uint16_t rtp_seq_num,
    const LossNotificationController::FrameDetails* frame) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  // Ignore repeated or reordered packets.
  // TODO(bugs.webrtc.org/10336): Handle packet reordering.
  //忽略重复或重新排序的数据包
  if (last_received_seq_num_ &&
      !AheadOf(rtp_seq_num, *last_received_seq_num_)) {
    return;
  }

  //丢掉一些过时信息
  DiscardOldInformation();  // Prevent memory overconsumption.
  //是否有seqNum跳跃
  const bool seq_num_gap =
      last_received_seq_num_ &&
      rtp_seq_num != static_cast<uint16_t>(*last_received_seq_num_ + 1u);

  last_received_seq_num_ = rtp_seq_num;

  // |frame| is not nullptr iff the packet is the first packet in the frame.
  //如果packet是frame里的第一个packet，那么frame不是nullptr
  if (frame != nullptr) {
    // Ignore repeated or reordered frames.
    // TODO(bugs.webrtc.org/10336): Handle frame reordering.
    //到达的包是乱序的，序号在last_received_frame_id_之前
    if (last_received_frame_id_.has_value() &&
        frame->frame_id <= last_received_frame_id_.value()) {
      RTC_LOG(LS_WARNING) << "Repeated or reordered frame ID ("
                          << frame->frame_id << ").";
      return;
    }

    last_received_frame_id_ = frame->frame_id;

    if (frame->is_keyframe) {
      //是关键帧
      // Subsequent frames may not rely on frames before the key frame.
      // Note that upon receiving a key frame, we do not issue a loss
      // notification on RTP sequence number gap, unless that gap spanned
      // the key frame itself. This is because any loss which occurred before
      // the key frame is no longer relevant.
      //后续帧不能依赖于关键帧之前的帧。
      //请注意，在接收到关键帧时，我们不会对RTP序列号间隙发出丢失通知，
      //除非该间隙跨越了关键帧本身。这是因为在关键帧之前发生的任何损失都不再相关。
      decodable_frame_ids_.clear();
      current_frame_potentially_decodable_ = true;
    } else {
      //不是关键帧
      const bool all_dependencies_decodable =
          AllDependenciesDecodable(frame->frame_dependencies);
	  //当前的帧是否可能decode？
      current_frame_potentially_decodable_ = all_dependencies_decodable;
	  //如果有seqNum的gap，或者不能被解码，则handle loss
      if (seq_num_gap || !current_frame_potentially_decodable_) {
        HandleLoss(rtp_seq_num, current_frame_potentially_decodable_);
      }
    }
  } else if (seq_num_gap || !current_frame_potentially_decodable_) {
    current_frame_potentially_decodable_ = false;
    // We allow sending multiple loss notifications for a single frame
    // even if only one of its packets is lost. We do this because the bigger
    // the frame, the more likely it is to be non-discardable, and therefore
    // the more robust we wish to be to loss of the feedback messages.
    //我们允许为一个帧发送多个丢失通知，即使其中只有一个数据包丢失。
    //我们这样做是因为帧越大，不可丢弃的可能性越大，因此我们希望丢失反馈消息的鲁棒性就越高。
    HandleLoss(rtp_seq_num, false);
  }
}

//一个帧是由先前接收到的数据包组装而成的。（即使帧由单个包组成，也应调用。）
void LossNotificationController::OnAssembledFrame(
    uint16_t first_seq_num,
    int64_t frame_id,
    bool discardable,
    rtc::ArrayView<const int64_t> frame_dependencies) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  DiscardOldInformation();  // Prevent memory overconsumption.

  if (discardable) {
    return;
  }

  if (!AllDependenciesDecodable(frame_dependencies)) {
    return;
  }

  last_decodable_non_discardable_.emplace(first_seq_num);
  const auto it = decodable_frame_ids_.insert(frame_id);
  RTC_DCHECK(it.second);
}

//丢弃掉不用的信息
void LossNotificationController::DiscardOldInformation() {
  constexpr size_t kExpectedKeyFrameIntervalFrames = 3000;
  constexpr size_t kMaxSize = 2 * kExpectedKeyFrameIntervalFrames;
  constexpr size_t kTargetSize = kExpectedKeyFrameIntervalFrames;
  PareDown(&decodable_frame_ids_, kMaxSize, kTargetSize);
}

//判断当前的帧是否可能被解码
bool LossNotificationController::AllDependenciesDecodable(
    rtc::ArrayView<const int64_t> frame_dependencies) const {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  // Due to packet reordering, frame buffering and asynchronous decoders, it is
  // infeasible to make reliable conclusions on the decodability of a frame
  // immediately when it arrives. We use the following assumptions:
  // * Intra frames are decodable.
  // * Inter frames are decodable if all of their references were decodable.
  // One possibility that is ignored, is that the packet may be corrupt.
  //由于包重排序、帧缓冲和异步解码器的存在，在帧到达时不可能立即得出可靠的可解性结论。
  //我们使用以下假设：
  //*帧内帧是可解码的。
  //*如果帧间的所有引用都是可解码的，则帧间是可解码的。
  //一种被忽略的可能性是，数据包可能已损坏。
  for (int64_t ref_frame_id : frame_dependencies) {
  	//寻找依赖项，看能否解码
    const auto ref_frame_it = decodable_frame_ids_.find(ref_frame_id);
    if (ref_frame_it == decodable_frame_ids_.end()) {
      // Reference frame not decodable.
      //有一个依赖项（参考帧）不能解码，则不能解码
      return false;
    }
  }
  //否则是有可能解码的
  return true;
}

void LossNotificationController::HandleLoss(uint16_t last_received_seq_num,
                                            bool decodability_flag) {
  RTC_DCHECK_RUN_ON(&sequence_checker_);

  if (last_decodable_non_discardable_) {
  	//如果上一个解码的没有被丢弃？
    RTC_DCHECK(AheadOf(last_received_seq_num,
                       last_decodable_non_discardable_->first_seq_num));
    loss_notification_sender_->SendLossNotification(
        last_decodable_non_discardable_->first_seq_num, last_received_seq_num,
        decodability_flag, /*buffering_allowed=*/true);
  } else {
  	//申请关键帧
    key_frame_request_sender_->RequestKeyFrame();
  }
}
}  //  namespace webrtc
