/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "RtspMuxer.h"
#include "Common/config.h"
#include "Extension/Factory.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

void RtspMuxer::onRtp(RtpPacket::Ptr in, bool is_key) {
    if (_live) {
        if (!_ntp_stamp_initialized) {
            _media_stamp_start = in->ntp_stamp;
            _ntp_stamp_start = getCurrentMillisecond(true);
            _ntp_stamp_initialized = true;
        }

        // FrameStamp exposes signed PTS through uint64_t, including negatives
        // around the initial DTS origin. Use the signed modular difference so
        // crossing zero keeps one common offset without signed overflow.
        const auto delta = in->ntp_stamp - _media_stamp_start;
        if (delta <= INT64_MAX) {
            CHECK(delta <= UINT64_MAX - _ntp_stamp_start, "RTSP NTP timestamp overflow");
            in->ntp_stamp = _ntp_stamp_start + delta;
        } else {
            const auto backward = UINT64_MAX - delta + 1;
            CHECK(backward <= _ntp_stamp_start, "RTSP NTP timestamp underflow");
            in->ntp_stamp = _ntp_stamp_start - backward;
        }
    } else {
        // 点播情况下设置ntp时间戳为rtp时间戳加基准ntp时间戳  [AUTO-TRANSLATED:b9f77de4]
        // In on-demand scenarios, set the NTP timestamp to the RTP timestamp plus the base NTP timestamp
        in->ntp_stamp = _ntp_stamp_start + (in->getStamp() * uint64_t(1000) / in->sample_rate);
    }
    _rtpRing->write(std::move(in), is_key);
}

RtspMuxer::RtspMuxer(const TitleSdp::Ptr &title) {
    if (!title) {
        _sdp = std::make_shared<TitleSdp>()->getSdp();
    } else {
        _live = title->getDuration() == 0;
        _sdp = title->getSdp();
    }
    _rtpRing = std::make_shared<RtpRing::RingType>();
    _rtpInterceptor = std::make_shared<RtpRing::RingType>();
    _rtpInterceptor->setDelegate(std::make_shared<RingDelegateHelper>([this](RtpPacket::Ptr in, bool is_key) {
        onRtp(std::move(in), is_key);
    }));

    _ntp_stamp_start = getCurrentMillisecond(true);
}

bool RtspMuxer::addTrack(const Track::Ptr &track) {
    if (_track_existed[track->getTrackType()]) {
        // rtsp不支持多个同类型track  [AUTO-TRANSLATED:87262d86]
        // RTSP does not support multiple tracks of the same type
        WarnL << "Already add a track kind of: " << track->getTrackTypeStr() << ", ignore track: " << track->getCodecName();
        return false;
    }
    if (!track->ready()) {
        WarnL << track->getCodecName() << " unready!";
        return false;
    }

    auto &ref = _tracks[track->getIndex()];
    auto &encoder = ref.encoder;
    CHECK(!encoder);

    auto pt = RtpPayload::getPayloadType(*track);
    // payload type 96以后则为动态pt  [AUTO-TRANSLATED:812ac0a2]
    // Payload type 96 and above is dynamic PT
    Sdp::Ptr sdp = track->getSdp(pt == -1 ? 96 + _index : pt);
    if (!sdp) {
        WarnL << "Unsupported codec: " << track->getCodecName();
        return false;
    }

    encoder = Factory::getRtpEncoderByCodecId(track->getCodecId(), sdp->getPayloadType());
    if (!encoder) {
        return false;
    }
    if (track->getTrackType() == TrackAudio) {
        auto &audio = static_cast<const AudioTrack &>(*track);
        encoder->setAudioInfo(audio.getAudioSampleRate(), audio.getAudioChannel());
    }

    // 标记已经存在该类型track  [AUTO-TRANSLATED:ed79ebb5]
    // Mark that a track of this type already exists
    _track_existed[track->getTrackType()] = true;

    {
        static atomic<uint32_t> s_ssrc(0);
        uint32_t ssrc = s_ssrc++;
        if (!ssrc) {
            // ssrc不能为0  [AUTO-TRANSLATED:312a1b47]
            // SSRC cannot be 0
            ssrc = s_ssrc++;
        }
        if (track->getTrackType() == TrackVideo) {
            // 视频的ssrc是偶数，方便调试  [AUTO-TRANSLATED:c22cd03f]
            // The video SSRC is even for debugging convenience
            ssrc = 2 * ssrc;
        } else {
            // 音频ssrc是奇数  [AUTO-TRANSLATED:50688636]
            // The audio SSRC is odd
            ssrc = 2 * ssrc + 1;
        }
        GET_CONFIG(uint32_t, audio_mtu, Rtp::kAudioMtuSize);
        GET_CONFIG(uint32_t, video_mtu, Rtp::kVideoMtuSize);
        auto mtu = track->getTrackType() == TrackVideo ? video_mtu : audio_mtu;
        encoder->setRtpInfo(ssrc, mtu, sdp->getSampleRate(), sdp->getPayloadType(), 2 * track->getTrackType(), track->getIndex());
    }

    // 设置rtp输出环形缓存  [AUTO-TRANSLATED:5ac7e24a]
    // Set the RTP output circular buffer
    encoder->setRtpRing(_rtpInterceptor);

    auto str = sdp->getSdp();
    str += "a=control:trackID=";
    str += std::to_string(_index);
    str += "\r\n";

    // 添加其sdp  [AUTO-TRANSLATED:80958925]
    // Add its SDP
    _sdp.append(str);
    ++_index;
    return true;
}

bool RtspMuxer::inputFrame(const Frame::Ptr &frame) {
    auto &encoder = _tracks[frame->getIndex()].encoder;
    return encoder ? encoder->inputFrame(frame) : false;
}

void RtspMuxer::flush() {
    for (auto &pr : _tracks) {
        if (pr.second.encoder) {
            pr.second.encoder->flush();
        }
    }
}

string RtspMuxer::getSdp() {
    return _sdp;
}

RtpRing::RingType::Ptr RtspMuxer::getRtpRing() const {
    return _rtpRing;
}

void RtspMuxer::resetTracks() {
    _sdp.clear();
    _tracks.clear();
    _ntp_stamp_initialized = false;
    _media_stamp_start = 0;
    CLEAR_ARR(_track_existed);
}

} /* namespace mediakit */
