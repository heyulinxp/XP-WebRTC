/*
 *  Copyright (c) 2017 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#ifndef CALL_RTP_DEMUXER_H_
#define CALL_RTP_DEMUXER_H_

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace webrtc {

class RtpPacketReceived;
class RtpPacketSinkInterface;

// This struct describes the criteria that will be used to match packets to a
// specific sink.
struct RtpDemuxerCriteria {
  RtpDemuxerCriteria();
  ~RtpDemuxerCriteria();

  // If not the empty string, will match packets with this MID.
  std::string mid;

  // If not the empty string, will match packets with this as their RTP stream
  // ID or repaired RTP stream ID.
  // Note that if both MID and RSID are specified, this will only match packets
  // that have both specified (either through RTP header extensions, SSRC
  // latching or RTCP).
  std::string rsid;

  // Will match packets with any of these SSRCs.
  std::set<uint32_t> ssrcs;

  // Will match packets with any of these payload types.
  std::set<uint8_t> payload_types;

  // Return string representation of demux criteria to facilitate logging
  std::string ToString() const;
};

// This class represents the RTP demuxing, for a single RTP session (i.e., one
// SSRC space, see RFC 7656). It isn't thread aware, leaving responsibility of
// multithreading issues to the user of this class.
// The demuxing algorithm follows the sketch given in the BUNDLE draft:
// https://tools.ietf.org/html/draft-ietf-mmusic-sdp-bundle-negotiation-38#section-10.2
// with modifications to support RTP stream IDs also.
//
// When a packet is received, the RtpDemuxer will route according to the
// following rules:
// 1. If the packet contains the MID header extension, and no sink has been
//    added with that MID as a criteria, the packet is not routed.
// 2. If the packet has the MID header extension, but no RSID or RRID extension,
//    and the MID is bound to a sink, then bind its SSRC to the same sink and
//    forward the packet to that sink. Note that rebinding to the same sink is
//    not an error. (Later packets with that SSRC would therefore be forwarded
//    to the same sink, whether they have the MID header extension or not.)
// 3. If the packet has the MID header extension and either the RSID or RRID
//    extension, and the MID, RSID (or RRID) pair is bound to a sink, then bind
//    its SSRC to the same sink and forward the packet to that sink. Later
//    packets with that SSRC will be forwarded to the same sink.
// 4. If the packet has the RSID or RRID header extension, but no MID extension,
//    and the RSID or RRID is bound to an RSID sink, then bind its SSRC to the
//    same sink and forward the packet to that sink. Later packets with that
//    SSRC will be forwarded to the same sink.
// 5. If the packet's SSRC is bound to an SSRC through a previous call to
//    AddSink, then forward the packet to that sink. Note that the RtpDemuxer
//    will not verify the payload type even if included in the sink's criteria.
//    The sink is expected to do the check in its handler.
// 6. If the packet's payload type is bound to exactly one payload type sink
//    through an earlier call to AddSink, then forward the packet to that sink.
// 7. Otherwise, the packet is not routed.
//
// In summary, the routing algorithm will always try to first match MID and RSID
// (including through SSRC binding), match SSRC directly as needed, and use
// payload types only if all else fails.
//此类表示单个RTP会话（即一个SSRC空间，见rfc7656）的RTP解组。它没有线程意识，将多线程问题的责任留给了这个类的用户。
//解组算法遵循捆绑草案中给出的草图：
//https://tools.ietf.org/html/draft-ietf-mmusic-sdp-bundle-negotiation-38\section-10.2款
//也支持RTP流id。
//
//当接收到数据包时，RtpDemuxer将根据以下规则进行路由：
//1.如果数据包包含MID报头扩展，并且没有使用该MID作为标准添加接收器，则不会路由该数据包。
//2.如果数据包有MID报头扩展，但没有RSID或RRID扩展，并且MID绑定到接收器，则将其SSRC绑定到同一个接收器，然后将数据包转发到该接收器。请注意，重新绑定到同一个接收器不是错误。（因此，具有该SSRC的后续数据包将被转发到同一个接收器，无论它们是否具有中间报头扩展）
//3.如果数据包有MID报头扩展和RSID或RRID扩展，并且MID、RSID（或RRID）对绑定到接收器，则将其SSRC绑定到同一接收器，然后将数据包转发到该接收器。随后，带有该SSRC的数据包将被转发到同一个接收器。
//4.如果数据包具有RSID或RRID报头扩展，但没有中间扩展，并且RSID或RRID绑定到RSID接收器，则将其SSRC绑定到同一接收器，并将数据包转发到该接收器。随后，带有该SSRC的数据包将被转发到同一个接收器。
//5.如果包的SSRC通过之前对AddSink的调用绑定到SSRC，则将包转发到该sink。注意，RtpDemuxer不会验证有效负载类型，即使它包含在sink的条件中。接收器应该在其处理程序中执行检查。
//6.如果包的有效负载类型通过先前对AddSink的调用绑定到一个负载类型接收器，则将数据包转发到该接收器。
//7.否则，数据包不会被路由。
//
//总之，路由算法将始终尝试首先匹配MID和RSID（包括通过SSRC绑定），根据需要直接匹配SSRC，并且只有在所有其他方法都失败时才使用有效负载类型。
class RtpDemuxer {
 public:
  // Maximum number of unique SSRC bindings allowed. This limit is to prevent
  // memory overuse attacks due to a malicious peer sending many packets with
  // different SSRCs.
  static constexpr int kMaxSsrcBindings = 1000;

  // Returns a string that contains all the attributes of the given packet
  // relevant for demuxing.
  static std::string DescribePacket(const RtpPacketReceived& packet);

  RtpDemuxer();
  ~RtpDemuxer();

  RtpDemuxer(const RtpDemuxer&) = delete;
  void operator=(const RtpDemuxer&) = delete;

  // Registers a sink that will be notified when RTP packets match its given
  // criteria according to the algorithm described in the class description.
  // Returns true if the sink was successfully added.
  // Returns false in the following situations:
  // - Only MID is specified and the MID is already registered.
  // - Only RSID is specified and the RSID is already registered.
  // - Both MID and RSID is specified and the (MID, RSID) pair is already
  //   registered.
  // - Any of the criteria SSRCs are already registered.
  // If false is returned, no changes are made to the demuxer state.
  bool AddSink(const RtpDemuxerCriteria& criteria,
               RtpPacketSinkInterface* sink);

  // Registers a sink. Multiple SSRCs may be mapped to the same sink, but
  // each SSRC may only be mapped to one sink. The return value reports
  // whether the association has been recorded or rejected. Rejection may occur
  // if the SSRC has already been associated with a sink. The previously added
  // sink is *not* forgotten.
  bool AddSink(uint32_t ssrc, RtpPacketSinkInterface* sink);

  // Registers a sink's association to an RSID. Only one sink may be associated
  // with a given RSID. Null pointer is not allowed.
  void AddSink(const std::string& rsid, RtpPacketSinkInterface* sink);

  // Removes a sink. Return value reports if anything was actually removed.
  // Null pointer is not allowed.
  bool RemoveSink(const RtpPacketSinkInterface* sink);

  // Demuxes the given packet and forwards it to the chosen sink. Returns true
  // if the packet was forwarded and false if the packet was dropped.
  bool OnRtpPacket(const RtpPacketReceived& packet);

  // Configure whether to look at the MID header extension when demuxing
  // incoming RTP packets. By default this is enabled.
  void set_use_mid(bool use_mid) { use_mid_ = use_mid; }

 private:
  // Returns true if adding a sink with the given criteria would cause conflicts
  // with the existing criteria and should be rejected.
  bool CriteriaWouldConflict(const RtpDemuxerCriteria& criteria) const;

  // Runs the demux algorithm on the given packet and returns the sink that
  // should receive the packet.
  // Will record any SSRC<->ID associations along the way.
  // If the packet should be dropped, this method returns null.
  RtpPacketSinkInterface* ResolveSink(const RtpPacketReceived& packet);

  // Used by the ResolveSink algorithm.
  RtpPacketSinkInterface* ResolveSinkByMid(const std::string& mid,
                                           uint32_t ssrc);
  RtpPacketSinkInterface* ResolveSinkByMidRsid(const std::string& mid,
                                               const std::string& rsid,
                                               uint32_t ssrc);
  RtpPacketSinkInterface* ResolveSinkByRsid(const std::string& rsid,
                                            uint32_t ssrc);
  RtpPacketSinkInterface* ResolveSinkByPayloadType(uint8_t payload_type,
                                                   uint32_t ssrc);

  // Regenerate the known_mids_ set from information in the sink_by_mid_ and
  // sink_by_mid_and_rsid_ maps.
  void RefreshKnownMids();

  // Map each sink by its component attributes to facilitate quick lookups.
  // Payload Type mapping is a multimap because if two sinks register for the
  // same payload type, both AddSinks succeed but we must know not to demux on
  // that attribute since it is ambiguous.
  // Note: Mappings are only modified by AddSink/RemoveSink (except for
  // SSRC mapping which receives all MID, payload type, or RSID to SSRC bindings
  // discovered when demuxing packets).
  std::map<std::string, RtpPacketSinkInterface*> sink_by_mid_;
  std::map<uint32_t, RtpPacketSinkInterface*> sink_by_ssrc_;
  std::multimap<uint8_t, RtpPacketSinkInterface*> sinks_by_pt_;
  std::map<std::pair<std::string, std::string>, RtpPacketSinkInterface*>
      sink_by_mid_and_rsid_;
  std::map<std::string, RtpPacketSinkInterface*> sink_by_rsid_;

  // Tracks all the MIDs that have been identified in added criteria. Used to
  // determine if a packet should be dropped right away because the MID is
  // unknown.
  std::set<std::string> known_mids_;

  // Records learned mappings of MID --> SSRC and RSID --> SSRC as packets are
  // received.
  // This is stored separately from the sink mappings because if a sink is
  // removed we want to still remember these associations.
  std::map<uint32_t, std::string> mid_by_ssrc_;
  std::map<uint32_t, std::string> rsid_by_ssrc_;

  // Adds a binding from the SSRC to the given sink.
  void AddSsrcSinkBinding(uint32_t ssrc, RtpPacketSinkInterface* sink);

  bool use_mid_ = true;
};

}  // namespace webrtc

#endif  // CALL_RTP_DEMUXER_H_
