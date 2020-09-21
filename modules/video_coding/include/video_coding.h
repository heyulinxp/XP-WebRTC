/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef MODULES_VIDEO_CODING_INCLUDE_VIDEO_CODING_H_
#define MODULES_VIDEO_CODING_INCLUDE_VIDEO_CODING_H_

#include "api/video/video_frame.h"
#include "api/video_codecs/video_codec.h"
#include "modules/include/module.h"
#include "modules/rtp_rtcp/source/rtp_video_header.h"
#include "modules/video_coding/include/video_coding_defines.h"

namespace webrtc {

class Clock;
class EncodedImageCallback;
class VideoDecoder;
class VideoEncoder;
struct CodecSpecificInfo;

class VideoCodingModule : public Module {
 public:
  // DEPRECATED.
  static VideoCodingModule* Create(Clock* clock);

  /*
   *   Receiver
   */

  // Register possible receive codecs, can be called multiple times for
  // different codecs.
  // The module will automatically switch between registered codecs depending on
  // the
  // payload type of incoming frames. The actual decoder will be created when
  // needed.
  //
  // Input:
  //      - payload_type      : RTP payload type
  //      - receiveCodec      : Settings for the codec to be registered.
  //      - numberOfCores     : Number of CPU cores that the decoder is allowed
  //      to use.
  //
  // Return value      : VCM_OK, on success.
  //                     < 0,    on error.
  //注册可能的接收编解码器，可以为不同的编解码器多次调用。
  //该模块将根据传入帧的有效载荷类型在已注册的编解码器之间自动切换。
  //实际的解码器将在需要时创建。
  //输入：
  //-有效载荷类型：RTP有效载荷类型
  //-receiveCodec：要注册的编解码器的设置。
  //-numberOfCores：解码器允许的CPU核数使用。
  //返回值：
  //VCM_OK，成功时。
  //<0，出错时。
  virtual int32_t RegisterReceiveCodec(uint8_t payload_type,
                                       const VideoCodec* receiveCodec,
                                       int32_t numberOfCores) = 0;

  // Register an external decoder object.
  //
  // Input:
  //      - externalDecoder : Decoder object to be used for decoding frames.
  //      - payloadType     : The payload type which this decoder is bound to.
  //注册外部解码器对象。
  //输入：
  //-externalDecoder：用于解码帧的解码器对象。
  //-payloadType：此解码器绑定到的有效负载类型。
  virtual void RegisterExternalDecoder(VideoDecoder* externalDecoder,
                                       uint8_t payloadType) = 0;

  // Register a receive callback. Will be called whenever there is a new frame
  // ready
  // for rendering.
  //
  // Input:
  //      - receiveCallback        : The callback object to be used by the
  //      module when a
  //                                 frame is ready for rendering.
  //                                 De-register with a NULL pointer.
  //
  // Return value      : VCM_OK, on success.
  //                     < 0,    on error.
  //注册接收回调。将在有新帧准备渲染时调用。
  //输入：
  //-receiveCallback：当帧准备好呈现时，模块要使用的回调对象。用空指针取消注册。
  //返回值：
  //VCM_OK，成功时。
  //<0，出错时。
  virtual int32_t RegisterReceiveCallback(
      VCMReceiveCallback* receiveCallback) = 0;

  // Register a frame type request callback. This callback will be called when
  // the
  // module needs to request specific frame types from the send side.
  //
  // Input:
  //      - frameTypeCallback      : The callback object to be used by the
  //      module when
  //                                 requesting a specific type of frame from
  //                                 the send side.
  //                                 De-register with a NULL pointer.
  //
  // Return value      : VCM_OK, on success.
  //                     < 0,    on error.
  //注册帧类型请求回调。当模块需要从发送端请求特定帧类型时，将调用此回调。
  //输入：
  //-frameTypeCallback：模块在从发送端请求特定类型的帧时使用的回调对象。用空指针取消注册。
  //返回值：
  //VCM_OK，成功时。
  //<0，出错时。
  virtual int32_t RegisterFrameTypeCallback(
      VCMFrameTypeCallback* frameTypeCallback) = 0;

  // Registers a callback which is called whenever the receive side of the VCM
  // encounters holes in the packet sequence and needs packets to be
  // retransmitted.
  //
  // Input:
  //              - callback      : The callback to be registered in the VCM.
  //
  // Return value     : VCM_OK,     on success.
  //                    <0,         on error.
  //注册一个回调，每当VCM的接收端遇到数据包序列中的漏洞并需要重新传输数据包时，就会调用该回调。
  //输入：
  //-回调：要在VCM中注册的回调。
  //返回值：
  //VCM_OK，成功时。
  //<0，出错时。
  virtual int32_t RegisterPacketRequestCallback(
      VCMPacketRequestCallback* callback) = 0;

  // Waits for the next frame in the jitter buffer to become complete
  // (waits no longer than maxWaitTimeMs), then passes it to the decoder for
  // decoding.
  // Should be called as often as possible to get the most out of the decoder.
  //
  // Return value      : VCM_OK, on success.
  //                     < 0,    on error.
  //等待抖动缓冲区中的下一帧完成（等待时间不超过maxWaitTimeMs），然后将其传递给解码器进行解码。
  //应尽可能频繁地调用，以最大限度地利用解码器。
  //返回值：
  //VCM_OK，成功时。
  //<0，出错时。
  virtual int32_t Decode(uint16_t maxWaitTimeMs = 200) = 0;

  // Insert a parsed packet into the receiver side of the module. Will be placed
  // in the
  // jitter buffer waiting for the frame to become complete. Returns as soon as
  // the packet
  // has been placed in the jitter buffer.
  //
  // Input:
  //      - incomingPayload      : Payload of the packet.
  //      - payloadLength        : Length of the payload.
  //      - rtp_header           : The parsed RTP header.
  //      - video_header         : The relevant extensions and payload header.
  //
  // Return value      : VCM_OK, on success.
  //                     < 0,    on error.
  //将解析后的数据包插入模块的接收方。将被放入抖动缓冲区等待帧完成。一旦包被放入抖动缓冲区，就返回。
  //输入：
  //-incomingPayload：包的有效负载。
  //-payloadLength：有效载荷的长度。
  //-rtp_header：解析的rtp头。
  //-video_header：相关扩展和有效负载头。
  //返回值：
  //VCM_OK，成功时。
  //<0，出错时。
  virtual int32_t IncomingPacket(const uint8_t* incomingPayload,
                                 size_t payloadLength,
                                 const RTPHeader& rtp_header,
                                 const RTPVideoHeader& video_header) = 0;

  // Sets the maximum number of sequence numbers that we are allowed to NACK
  // and the oldest sequence number that we will consider to NACK. If a
  // sequence number older than |max_packet_age_to_nack| is missing
  // a key frame will be requested. A key frame will also be requested if the
  // time of incomplete or non-continuous frames in the jitter buffer is above
  // |max_incomplete_time_ms|.
  //设置允许NACK的最大序列号数目和我们将考虑NACK的最旧序列号。
  //如果序列号大于| max_packet_age_to_nack |丢失，将请求一个关键帧。
  //如果抖动缓冲区中的不完整或非连续帧的时间超过| max_incomplete_time|，也将请求关键帧。
  virtual void SetNackSettings(size_t max_nack_list_size,
                               int max_packet_age_to_nack,
                               int max_incomplete_time_ms) = 0;
};

}  // namespace webrtc

#endif  // MODULES_VIDEO_CODING_INCLUDE_VIDEO_CODING_H_
