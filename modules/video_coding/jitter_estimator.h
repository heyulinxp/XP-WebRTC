/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_JITTER_ESTIMATOR_H_
#define MODULES_VIDEO_CODING_JITTER_ESTIMATOR_H_

#include "modules/video_coding/rtt_filter.h"
#include "rtc_base/rolling_accumulator.h"

namespace webrtc {

class Clock;

class VCMJitterEstimator {
 public:
  explicit VCMJitterEstimator(Clock* clock);
  virtual ~VCMJitterEstimator();
  VCMJitterEstimator& operator=(const VCMJitterEstimator& rhs);

  // Resets the estimate to the initial state.
  //将估计重置为初始状态。
  void Reset();

  // Updates the jitter estimate with the new data.
  //
  // Input:
  //          - frameDelay      : Delay-delta calculated by UTILDelayEstimate in
  //                              milliseconds.
  //          - frameSize       : Frame size of the current frame.
  //          - incompleteFrame : Flags if the frame is used to update the
  //                              estimate before it was complete.
  //                              Default is false.
  //用新数据更新抖动估计值。
  //输入：
  //-frameDelay：UTILDelayEstimate计算的延迟增量（毫秒）。
  //-帧大小：当前帧的帧大小。
  //-incompleteFrame：标记该帧是否在完成之前用于更新估计。默认值为false。
  void UpdateEstimate(int64_t frameDelayMS,
                      uint32_t frameSizeBytes,
                      bool incompleteFrame = false);

  // Returns the current jitter estimate in milliseconds and adds an RTT
  // dependent term in cases of retransmission.
  //  Input:
  //          - rttMultiplier  : RTT param multiplier (when applicable).
  //
  // Return value              : Jitter estimate in milliseconds.
  //返回当前抖动估计值（毫秒），并在重新传输的情况下添加一个RTT相关项。
  //输入：
  //-RTT乘法器：RTT参数乘数（如适用）。
  //返回值：抖动估计值（毫秒）。
  virtual int GetJitterEstimate(double rttMultiplier,
                                absl::optional<double> rttMultAddCapMs);

  // Updates the nack counter.
  void FrameNacked();

  // Updates the RTT filter.
  //
  // Input:
  //          - rttMs          : RTT in ms.
  void UpdateRtt(int64_t rttMs);

  // A constant describing the delay from the jitter buffer to the delay on the
  // receiving side which is not accounted for by the jitter buffer nor the
  // decoding delay estimate.
  //一个常数，描述从抖动缓冲器到接收端的延迟，该延迟不被抖动缓冲器或解码延迟估计所考虑。
  static const uint32_t OPERATING_SYSTEM_JITTER = 10;

 protected:
  // These are protected for better testing possibilities.
  //信道传输速率的倒数？斜率theta[0]和网络排队延迟theta[1]
  double _theta[2];  // Estimated line parameters (slope, offset)
  double _varNoise;  // Variance of the time-deviation from the line

 private:
  // Updates the Kalman filter for the line describing the frame size dependent
  // jitter.
  //
  // Input:
  //          - frameDelayMS    : Delay-delta calculated by UTILDelayEstimate in
  //                              milliseconds.
  //          - deltaFSBytes    : Frame size delta, i.e. frame size at time T
  //                            : minus frame size at time T-1.
  //更新描述帧大小相关抖动的行的Kalman滤波器。
  //输入：
  //-frameDelayMS：UTILDelayEstimate以毫秒为单位计算的延迟增量。
  //-deltaFSBytes：帧大小delta，即时间T的帧大小减去时间T-1的帧大小。
  void KalmanEstimateChannel(int64_t frameDelayMS, int32_t deltaFSBytes);

  // Updates the random jitter estimate, i.e. the variance of the time
  // deviations from the line given by the Kalman filter.
  //
  // Input:
  //          - d_dT              : The deviation from the kalman estimate.
  //          - incompleteFrame   : True if the frame used to update the
  //                                estimate with was incomplete.
  //更新随机抖动估计值，即与Kalman滤波器给定的直线的时间偏差的方差。
  //输入：
  //-d_dT：与kalman估计值的偏差。
  //-不完整帧：如果用于更新估算的帧不完整，则为True。
  void EstimateRandomJitter(double d_dT, bool incompleteFrame);

  double NoiseThreshold() const;

  // Calculates the current jitter estimate.
  //
  // Return value                 : The current jitter estimate in milliseconds.
  //计算当前抖动估计值
  double CalculateEstimate();

  // Post process the calculated estimate.
  //后处理计算的估计值。
  void PostProcessEstimate();

  // Calculates the difference in delay between a sample and the expected delay
  // estimated by the Kalman filter.
  //
  // Input:
  //          - frameDelayMS    : Delay-delta calculated by UTILDelayEstimate in
  //                              milliseconds.
  //          - deltaFS         : Frame size delta, i.e. frame size at time
  //                              T minus frame size at time T-1.
  //
  // Return value               : The difference in milliseconds.
  //计算样本与卡尔曼滤波器估计的期望延迟之间的延迟差。
  //输入：
  //-frameDelayMS：UtilLayestimate以毫秒为单位计算的延迟增量。
  //-deltaFS：帧大小delta，即T时刻的帧大小减去T-1时刻的帧大小。
  //返回值：以毫秒为单位的差值。
  double DeviationFromExpectedDelay(int64_t frameDelayMS,
                                    int32_t deltaFSBytes) const;

  double GetFrameRate() const;

  // Constants, filter parameters.
  const double _phi;
  const double _psi;
  const uint32_t _alphaCountMax;
  const double _thetaLow;
  const uint32_t _nackLimit;
  const int32_t _numStdDevDelayOutlier;
  const int32_t _numStdDevFrameSizeOutlier;
  const double _noiseStdDevs;
  const double _noiseStdDevOffset;

  double _thetaCov[2][2];  // Estimate covariance
  double _Qcov[2][2];      // Process noise covariance
  double _avgFrameSize;    // Average frame size
  double _varFrameSize;    // Frame size variance
  double _maxFrameSize;    // Largest frame size received (descending
                           // with a factor _psi)
  uint32_t _fsSum;
  uint32_t _fsCount;

  int64_t _lastUpdateT;
  double _prevEstimate;     // The previously returned jitter estimate
  uint32_t _prevFrameSize;  // Frame size of the previous frame
  double _avgNoise;         // Average of the random jitter
  uint32_t _alphaCount;
  double _filterJitterEstimate;  // The filtered sum of jitter estimates

  uint32_t _startupCount;

  int64_t
      _latestNackTimestamp;  // Timestamp in ms when the latest nack was seen
  uint32_t _nackCount;       // Keeps track of the number of nacks received,
                             // but never goes above _nackLimit
  VCMRttFilter _rttFilter;

  rtc::RollingAccumulator<uint64_t> fps_counter_;
  const double time_deviation_upper_bound_;
  const bool enable_reduced_delay_;
  Clock* clock_;
};

}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_JITTER_ESTIMATOR_H_
