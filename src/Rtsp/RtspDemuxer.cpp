/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <cctype>
#include <algorithm>
#include <cstdlib>
#include "RtpCodec.h"
#include "RtspDemuxer.h"
#include "Util/base64.h"
#include "Extension/Factory.h"

using namespace std;

namespace mediakit {

static bool hasLiveRange(const SdpTrack::Ptr &track) {
    if (!track) {
        return true;
    }
    auto range = track->_attr.equal_range("range");
    for (auto it = range.first; it != range.second; ++it) {
        auto value = toolkit::trim(std::string(it->second));
        if (value == "npt=now-") {
            continue;
        }
        if (value.compare(0, 4, "npt=") || value.empty() || value.back() != '-') {
            return false;
        }
        auto start = value.substr(4, value.size() - 5);
        char *end = nullptr;
        auto position = std::strtod(start.c_str(), &end);
        if (start.empty() || *end || position != 0) {
            return false;
        }
    }
    // Missing Range follows the existing live/unknown convention. A server
    // that omits all VOD metadata cannot be identified here as seekable.
    return true;
}

void RtspDemuxer::loadSdp(const string &sdp) {
    loadSdp(SdpParser(sdp));
}

bool RtspDemuxer::isLiveSdp(const SdpParser &parser) {
    auto tracks = parser.getAvailableTrack();
    return hasLiveRange(parser.getTrack(TrackTitle))
        && std::all_of(tracks.begin(), tracks.end(), hasLiveRange);
}

void RtspDemuxer::loadSdp(const SdpParser &attr) {
    auto titleTrack = attr.getTrack(TrackTitle);
    auto tracks = attr.getAvailableTrack();
    const auto live_range = isLiveSdp(attr);
    _live = live_range;
    for (auto &track : tracks) {
        switch (track->_type) {
            case TrackVideo: {
                makeVideoTrack(track);
            }
                break;
            case TrackAudio: {
                makeAudioTrack(track);
                if (_audio_rtp_decoder && _audio_track->getCodecId() == CodecAAC) {
                    toolkit::Any fmtp;
                    // loadSdp may reuse this decoder. Explicit VOD/unknown
                    // ranges must clear a previous live-clock qualification.
                    fmtp.set<std::string>(live_range ? track->_fmtp : "");
                    _audio_rtp_decoder->setOpt(RtpCodec::RTP_DECODER_AAC_LIVE_FMTP, fmtp);
                }
            }
                break;
            default:
                break;
        }
    }
    // rtsp能通过sdp立即知道有多少个track  [AUTO-TRANSLATED:66a4c8d3]
    // rtsp can immediately know how many tracks there are through sdp
    addTrackCompleted();

    if (titleTrack) {
        _duration = titleTrack->_duration;
    }
}

float RtspDemuxer::getDuration() const {
    return _duration;
}

bool RtspDemuxer::inputRtp(const RtpPacket::Ptr &rtp) {
    switch (rtp->type) {
        case TrackVideo: {
            if (_video_rtp_decoder) {
                return _video_rtp_decoder->inputRtp(rtp, true);
            }
            return false;
        }
        case TrackAudio: {
            if (_audio_rtp_decoder) {
                _audio_rtp_decoder->inputRtp(rtp, false);
                return false;
            }
            return false;
        }
        default: return false;
    }
}

static void setBitRate(const SdpTrack::Ptr &sdp, const Track::Ptr &track) {
    if (!sdp->_b.empty()) {
        int data_rate = 0;
        sscanf(sdp->_b.data(), "AS:%d", &data_rate);
        if (data_rate) {
            track->setBitRate(data_rate * 1024);
        }
    }
}

void RtspDemuxer::makeAudioTrack(const SdpTrack::Ptr &audio) {
    if (_audio_rtp_decoder) {
        return;
    }
    // 生成Track对象  [AUTO-TRANSLATED:c2f2ac3b]
    // Generate Track object
    _audio_track = dynamic_pointer_cast<AudioTrack>(Factory::getTrackBySdp(audio));
    if (!_audio_track) {
        return;
    }
    setBitRate(audio, _audio_track);
    // 生成RtpCodec对象以便解码rtp  [AUTO-TRANSLATED:889376fd]
    // Generate RtpCodec object to decode rtp
    _audio_rtp_decoder = Factory::getRtpDecoderByCodecId(_audio_track->getCodecId());
    if (!_audio_rtp_decoder) {
        // 找不到相应的rtp解码器，该track无效  [AUTO-TRANSLATED:1c8c5eab]
        // Cannot find the corresponding rtp decoder, the track is invalid
        _audio_track.reset();
        return;
    }
    // 设置rtp解码器代理，生成的frame写入该Track  [AUTO-TRANSLATED:b2a2362e]
    // Set the rtp decoder proxy, the generated frame is written to this Track
    _audio_rtp_decoder->setAudioInfo(_audio_track->getAudioSampleRate(), _audio_track->getAudioChannel());
    _audio_rtp_decoder->addDelegate(_audio_track);
    addTrack(_audio_track);
}

void RtspDemuxer::makeVideoTrack(const SdpTrack::Ptr &video) {
    if (_video_rtp_decoder) {
        return;
    }
    // 生成Track对象  [AUTO-TRANSLATED:c2f2ac3b]
    // Generate Track object
    _video_track = dynamic_pointer_cast<VideoTrack>(Factory::getTrackBySdp(video));
    if (!_video_track) {
        return;
    }
    setBitRate(video, _video_track);
    // 生成RtpCodec对象以便解码rtp  [AUTO-TRANSLATED:889376fd]
    // Generate RtpCodec object to decode rtp
    _video_rtp_decoder = Factory::getRtpDecoderByCodecId(_video_track->getCodecId());
    if (!_video_rtp_decoder) {
        // 找不到相应的rtp解码器，该track无效  [AUTO-TRANSLATED:1c8c5eab]
        // Cannot find the corresponding rtp decoder, the track is invalid
        _video_track.reset();
        return;
    }
    // 设置rtp解码器代理，生成的frame写入该Track  [AUTO-TRANSLATED:b2a2362e]
    // Set the rtp decoder proxy, the generated frame is written to this Track
    _video_rtp_decoder->addDelegate(_video_track);
    addTrack(_video_track);
}

} /* namespace mediakit */
