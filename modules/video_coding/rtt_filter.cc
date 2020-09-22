/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/rtt_filter.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "modules/video_coding/internal_defines.h"

namespace webrtc {

VCMRttFilter::VCMRttFilter()
    : _filtFactMax(35),
      _jumpStdDevs(2.5),
      _driftStdDevs(3.5),
      //kMaxDriftJumpCount=5
      _detectThreshold(kMaxDriftJumpCount) {
  Reset();
}

VCMRttFilter& VCMRttFilter::operator=(const VCMRttFilter& rhs) {
  if (this != &rhs) {
    _gotNonZeroUpdate = rhs._gotNonZeroUpdate;
    _avgRtt = rhs._avgRtt;
    _varRtt = rhs._varRtt;
    _maxRtt = rhs._maxRtt;
    _filtFactCount = rhs._filtFactCount;
    _jumpCount = rhs._jumpCount;
    _driftCount = rhs._driftCount;
    memcpy(_jumpBuf, rhs._jumpBuf, sizeof(_jumpBuf));
    memcpy(_driftBuf, rhs._driftBuf, sizeof(_driftBuf));
  }
  return *this;
}

void VCMRttFilter::Reset() {
  _gotNonZeroUpdate = false;
  _avgRtt = 0;
  _varRtt = 0;
  _maxRtt = 0;
  _filtFactCount = 1;
  _jumpCount = 0;
  _driftCount = 0;
  memset(_jumpBuf, 0, sizeof(_jumpBuf));
  memset(_driftBuf, 0, sizeof(_driftBuf));
}

void VCMRttFilter::Update(int64_t rttMs) {
  if (!_gotNonZeroUpdate) {
    if (rttMs == 0) {
      return;
    }
    _gotNonZeroUpdate = true;
  }

  // Sanity check
  if (rttMs > 3000) {
    rttMs = 3000;
  }

  double filtFactor = 0;
  //算比例
  if (_filtFactCount > 1) {
    filtFactor = static_cast<double>(_filtFactCount - 1) / _filtFactCount;
  }
  //累加统计
  _filtFactCount++;
  
  if (_filtFactCount > _filtFactMax) {
    // This prevents filtFactor from going above
    // (_filtFactMax - 1) / _filtFactMax,
    // e.g., _filtFactMax = 50 => filtFactor = 49/50 = 0.98
    _filtFactCount = _filtFactMax;
  }
  double oldAvg = _avgRtt;
  double oldVar = _varRtt;
  //按比例估计新的
  _avgRtt = filtFactor * _avgRtt + (1 - filtFactor) * rttMs;
  _varRtt = filtFactor * _varRtt +
            (1 - filtFactor) * (rttMs - _avgRtt) * (rttMs - _avgRtt);
  _maxRtt = VCM_MAX(rttMs, _maxRtt);
  if (!JumpDetection(rttMs) || !DriftDetection(rttMs)) {
    // In some cases we don't want to update the statistics
    _avgRtt = oldAvg;
    _varRtt = oldVar;
  }
}

//检测跳动，平均值_avgRtt与现在的值rttMs的比较，大于2.5倍中误差
//返回true表示正常，false表示有突变
bool VCMRttFilter::JumpDetection(int64_t rttMs) {
  double diffFromAvg = _avgRtt - rttMs;
  //_jumpStdDevs=2.5
  if (fabs(diffFromAvg) > _jumpStdDevs * sqrt(_varRtt)) {
    int diffSign = (diffFromAvg >= 0) ? 1 : -1;
    int jumpCountSign = (_jumpCount >= 0) ? 1 : -1;
    if (diffSign != jumpCountSign) {
      // Since the signs differ the samples currently
      // in the buffer is useless as they represent a
      // jump in a different direction.
      //由于符号不同，缓冲区中当前的样本是无用的，因为它们代表了一个不同方向的跳跃。
      _jumpCount = 0;
    }
    if (abs(_jumpCount) < kMaxDriftJumpCount) {
      // Update the buffer used for the short time
      // statistics.
      // The sign of the diff is used for updating the counter since
      // we want to use the same buffer for keeping track of when
      // the RTT jumps down and up.
      //更新用于短时间统计的缓冲区。
      //diff的符号用于更新计数器，因为我们希望使用相同的缓冲区来跟踪RTT何时上下跳转。
      _jumpBuf[abs(_jumpCount)] = rttMs;
      _jumpCount += diffSign;
    }
	//_detectThreshold=kMaxDriftJumpCount=5
    if (abs(_jumpCount) >= _detectThreshold) {
      // Detected an RTT jump
      //检测到RTT跳转
      ShortRttFilter(_jumpBuf, abs(_jumpCount));
      _filtFactCount = _detectThreshold + 1;
      _jumpCount = 0;
    } else {
      return false;
    }
  } else {
    _jumpCount = 0;
  }
  return true;
}

//漂移探测
bool VCMRttFilter::DriftDetection(int64_t rttMs) {
  //_driftStdDevs=3.5
  if (_maxRtt - _avgRtt > _driftStdDevs * sqrt(_varRtt)) {
    if (_driftCount < kMaxDriftJumpCount) {
      // Update the buffer used for the short time
      // statistics.
      //更新用于短时间统计的缓冲区。
      _driftBuf[_driftCount] = rttMs;
      _driftCount++;
    }
    if (_driftCount >= _detectThreshold) {
      // Detected an RTT drift
      //检测到RTT漂移
      ShortRttFilter(_driftBuf, _driftCount);
      _filtFactCount = _detectThreshold + 1;
      _driftCount = 0;
    }
  } else {
    _driftCount = 0;
  }
  return true;
}

//更新最大值_maxRtt和平均值_avgRtt
void VCMRttFilter::ShortRttFilter(int64_t* buf, uint32_t length) {
  if (length == 0) {
    return;
  }
  _maxRtt = 0;
  _avgRtt = 0;
  for (uint32_t i = 0; i < length; i++) {
    if (buf[i] > _maxRtt) {
      _maxRtt = buf[i];
    }
    _avgRtt += buf[i];
  }
  _avgRtt = _avgRtt / static_cast<double>(length);
}

//对外返回结果
int64_t VCMRttFilter::RttMs() const {
  return static_cast<int64_t>(_maxRtt + 0.5);
}
}  // namespace webrtc
