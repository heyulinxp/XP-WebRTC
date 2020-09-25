/*
 *  Copyright (c) 2013 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#ifndef CALL_CALL_H_
#define CALL_CALL_H_

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "api/adaptation/resource.h"
#include "api/media_types.h"
#include "call/audio_receive_stream.h"
#include "call/audio_send_stream.h"
#include "call/call_config.h"
#include "call/flexfec_receive_stream.h"
#include "call/packet_receiver.h"
#include "call/rtp_transport_controller_send_interface.h"
#include "call/video_receive_stream.h"
#include "call/video_send_stream.h"
#include "modules/utility/include/process_thread.h"
#include "rtc_base/copy_on_write_buffer.h"
#include "rtc_base/network/sent_packet.h"
#include "rtc_base/network_route.h"
#include "rtc_base/ref_count.h"

namespace webrtc {

// A restricted way to share the module process thread across multiple instances
// of Call that are constructed on the same worker thread (which is what the
// peer connection factory guarantees).
// SharedModuleThread supports a callback that is issued when only one reference
// remains, which is used to indicate to the original owner that the thread may
// be discarded.
//在同一个工作线程上构造的多个调用实例之间共享模块进程线程的一种受限方法（对等连接工厂保证这样做）。
//SharedModuleThread支持在只剩下一个引用时发出的回调，用于向原始所有者指示线程可能被丢弃。
class SharedModuleThread : public rtc::RefCountInterface {
 protected:
  SharedModuleThread(std::unique_ptr<ProcessThread> process_thread,
                     std::function<void()> on_one_ref_remaining);
  friend class rtc::scoped_refptr<SharedModuleThread>;
  ~SharedModuleThread() override;

 public:
  // Allows injection of an externally created process thread.
  //允许注入外部创建的进程线程。
  static rtc::scoped_refptr<SharedModuleThread> Create(
      std::unique_ptr<ProcessThread> process_thread,
      std::function<void()> on_one_ref_remaining);

  void EnsureStarted();

  ProcessThread* process_thread();

 private:
  void AddRef() const override;
  rtc::RefCountReleaseStatus Release() const override;

  class Impl;
  mutable std::unique_ptr<Impl> impl_;
};

// A Call instance can contain several send and/or receive streams. All streams
// are assumed to have the same remote endpoint and will share bitrate estimates
// etc.
//一个调用实例可以包含多个发送和/或接收流。假设所有流都具有相同的远程端点，
//并且将共享比特率估计值等。
class Call {
 public:
  using Config = CallConfig;

  struct Stats {
    std::string ToString(int64_t time_ms) const;

    int send_bandwidth_bps = 0;       // Estimated available send bandwidth.
    int max_padding_bitrate_bps = 0;  // Cumulative configured max padding.
    int recv_bandwidth_bps = 0;       // Estimated available receive bandwidth.
    int64_t pacer_delay_ms = 0;
    int64_t rtt_ms = -1;
  };

  static Call* Create(const Call::Config& config);
  static Call* Create(const Call::Config& config,
                      rtc::scoped_refptr<SharedModuleThread> call_thread);
  static Call* Create(const Call::Config& config,
                      Clock* clock,
                      rtc::scoped_refptr<SharedModuleThread> call_thread,
                      std::unique_ptr<ProcessThread> pacer_thread);

  virtual AudioSendStream* CreateAudioSendStream(
      const AudioSendStream::Config& config) = 0;

  virtual void DestroyAudioSendStream(AudioSendStream* send_stream) = 0;

  virtual AudioReceiveStream* CreateAudioReceiveStream(
      const AudioReceiveStream::Config& config) = 0;
  
  virtual void DestroyAudioReceiveStream(
      AudioReceiveStream* receive_stream) = 0;

  virtual VideoSendStream* CreateVideoSendStream(
      VideoSendStream::Config config,
      VideoEncoderConfig encoder_config) = 0;
  
  virtual VideoSendStream* CreateVideoSendStream(
      VideoSendStream::Config config,
      VideoEncoderConfig encoder_config,
      std::unique_ptr<FecController> fec_controller);
  
  virtual void DestroyVideoSendStream(VideoSendStream* send_stream) = 0;

  virtual VideoReceiveStream* CreateVideoReceiveStream(
      VideoReceiveStream::Config configuration) = 0;
  
  virtual void DestroyVideoReceiveStream(
      VideoReceiveStream* receive_stream) = 0;

  // In order for a created VideoReceiveStream to be aware that it is
  // protected by a FlexfecReceiveStream, the latter should be created before
  // the former.
  //为了使已创建的VideoReceiveStream知道它受FlexfecReceiveStream保护，
  //应在FlexfecReceiveStream之前创建后者。
  virtual FlexfecReceiveStream* CreateFlexfecReceiveStream(
      const FlexfecReceiveStream::Config& config) = 0;
  
  virtual void DestroyFlexfecReceiveStream(
      FlexfecReceiveStream* receive_stream) = 0;

  // When a resource is overused, the Call will try to reduce the load on the
  // sysem, for example by reducing the resolution or frame rate of encoded
  // streams.
  //当资源被过度使用时，Call将尝试减少系统的负载，例如通过降低编码流的分辨率或帧速率。
  virtual void AddAdaptationResource(rtc::scoped_refptr<Resource> resource) = 0;

  // All received RTP and RTCP packets for the call should be inserted to this
  // PacketReceiver. The PacketReceiver pointer is valid as long as the
  // Call instance exists.
  //所有接收到的用于Call的RTP和RTCP数据包都应插入到此PacketReceiver。
  //只要Call实例存在，PacketReceiver指针就有效。
  virtual PacketReceiver* Receiver() = 0;

  // This is used to access the transport controller send instance owned by
  // Call. The send transport controller is currently owned by Call for legacy
  // reasons. (for instance  variants of call tests are built on this assumtion)
  // TODO(srte): Move ownership of transport controller send out of Call and
  // remove this method interface.
  //这用于访问Call拥有的传输控制器send实例。由于遗留原因，发送传输控制器当前由Call拥有。
  //（例如，调用测试的变体是基于此假设构建的）
  //TODO（srte）：将传输控制器的所有权移出调用并删除此方法接口。
  virtual RtpTransportControllerSendInterface* GetTransportControllerSend() = 0;

  // Returns the call statistics, such as estimated send and receive bandwidth,
  // pacing delay, etc.
  //返回呼叫统计信息，例如估计的发送和接收带宽、速度调整延迟等。
  virtual Stats GetStats() const = 0;

  // TODO(skvlad): When the unbundled case with multiple streams for the same
  // media type going over different networks is supported, track the state
  // for each stream separately. Right now it's global per media type.
  //TODO（skvlad）：当支持同一媒体类型的多个流通过不同网络的未绑定情况时，
  //请分别跟踪每个流的状态。现在它是全局的每种媒体类型。
  virtual void SignalChannelNetworkState(MediaType media,
                                         NetworkState state) = 0;

  virtual void OnAudioTransportOverheadChanged(
      int transport_overhead_per_packet) = 0;

  virtual void OnSentPacket(const rtc::SentPacket& sent_packet) = 0;

  virtual void SetClientBitratePreferences(
      const BitrateSettings& preferences) = 0;

  virtual const WebRtcKeyValueConfig& trials() const = 0;

  virtual ~Call() {}
};

}  // namespace webrtc

#endif  // CALL_CALL_H_
