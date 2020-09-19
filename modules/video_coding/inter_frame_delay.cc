/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/inter_frame_delay.h"

namespace webrtc {

VCMInterFrameDelay::VCMInterFrameDelay(int64_t currentWallClock) {
  Reset(currentWallClock);
}

// Resets the delay estimate.
void VCMInterFrameDelay::Reset(int64_t currentWallClock) {
  _zeroWallClock = currentWallClock;
  _wrapArounds = 0;
  _prevWallClock = 0;
  _prevTimestamp = 0;
  _dTS = 0;
}

// Calculates the delay of a frame with the given timestamp.
// This method is called when the frame is complete.
//计算具有给定时间戳的帧的延迟。此方法在帧完成时调用。
//返回值true表示没有重排序,false表示有重排序
bool VCMInterFrameDelay::CalculateDelay(uint32_t timestamp,
                                        int64_t* delay,
                                        int64_t currentWallClock) {
  if (_prevWallClock == 0) {
    // First set of data, initialization, wait for next frame.
    //初始化
    _prevWallClock = currentWallClock;
    _prevTimestamp = timestamp;
    *delay = 0;
    return true;
  }

  int32_t prevWrapArounds = _wrapArounds;
  CheckForWrapArounds(timestamp);

  // This will be -1 for backward wrap arounds and +1 for forward wrap arounds.
  int32_t wrapAroundsSincePrev = _wrapArounds - prevWrapArounds;

  // Account for reordering in jitter variance estimate in the future?
  // Note that this also captures incomplete frames which are grabbed for
  // decoding after a later frame has been complete, i.e. real packet losses.
  //考虑到未来抖动方差估计的重新排序？
  //注意，这也捕获了在下一帧完成后被抓取用于解码的不完整帧，即实际的分组丢失。
  if ((wrapAroundsSincePrev == 0 && timestamp < _prevTimestamp) ||
      wrapAroundsSincePrev < 0) {
    //wrapAroundsSincePrev < 0, _wrapArounds < prevWrapArounds,后退了,表明有重排序?
    *delay = 0;
    return false;
  }

  // Compute the compensated timestamp difference and convert it to ms and round
  // it to closest integer.
  //计算补偿后的时间戳差并将其转换为ms并四舍五入到最接近的整数。
  _dTS = static_cast<int64_t>(
      (timestamp + wrapAroundsSincePrev * (static_cast<int64_t>(1) << 32) -
       _prevTimestamp) /
          90.0 +
      0.5);

  // frameDelay is the difference of dT and dTS -- i.e. the difference of the
  // wall clock time difference and the timestamp difference between two
  // following frames.
  //帧延迟是dT和dTS的差，即墙时钟时差和后两帧之间的时间戳差。
  *delay = static_cast<int64_t>(currentWallClock - _prevWallClock - _dTS);

  _prevTimestamp = timestamp;
  _prevWallClock = currentWallClock;

  return true;
}

// Investigates if the timestamp clock has overflowed since the last timestamp
// and keeps track of the number of wrap arounds since reset.
//控制RTP时间戳计数器在当前帧和以前接收到的帧之间是否有环绕。
void VCMInterFrameDelay::CheckForWrapArounds(uint32_t timestamp) {
  if (timestamp < _prevTimestamp) {
    // This difference will probably be less than -2^31 if we have had a wrap
    // around (e.g. timestamp = 1, _prevTimestamp = 2^32 - 1). Since it is cast
    // to a int32_t, it should be positive.
    //timestamp = 1, _prevTimestamp = 2^32 - 1, timestamp - _prevTimestamp>0??
    if (static_cast<int32_t>(timestamp - _prevTimestamp) > 0) {
      // Forward wrap around.
      _wrapArounds++;
    }
    // This difference will probably be less than -2^31 if we have had a
    // backward wrap around. Since it is cast to a int32_t, it should be
    // positive.
  } else if (static_cast<int32_t>(_prevTimestamp - timestamp) > 0) {
    // Backward wrap around.
    _wrapArounds--;
  }
}
}  // namespace webrtc
