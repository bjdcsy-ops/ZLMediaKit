/*
 * Copyright (c) 2016-present The ZLMediaKit project authors.
 * SPDX-License-Identifier: MIT
 */
#include <algorithm>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Extension/Factory.h"
#include "Rtcp/RtcpContext.h"
#include "Rtsp/RtspMuxer.h"
#include "ext-codec/G711Rtp.h"

using namespace mediakit;
using namespace toolkit;

static void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}

static FrameImp::Ptr frame(bool audio, int index, uint64_t pts, uint64_t dts) {
    auto out = FrameImp::create();
    out->_codec_id = audio ? CodecG711U : CodecH264;
    out->setIndex(index);
    out->_pts = pts;
    out->_dts = dts;
    out->_buffer = audio ? std::string(160, '\xff') : std::string("\x41\x80\x11\x22", 4);
    return out;
}

struct Capture {
    RtspMuxer mux;
    Track::Ptr video, audio;
    std::vector<RtpPacket::Ptr> packets;
    explicit Capture(bool audio_added_first = false, bool vod = false)
        : mux(vod ? std::make_shared<TitleSdp>(60) : nullptr) {
        const std::string sdp =
            "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=clock test\r\nt=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z2QAH6zZQFAFuhAAAAMAEAAAAwDxgxHg,aM4G4g==\r\n"
            "a=control:trackID=0\r\nm=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000/1\r\na=control:trackID=1\r\n";
        SdpParser parser(sdp);
        video = Factory::getTrackBySdp(parser.getTrack(TrackVideo));
        audio = Factory::getTrackBySdp(parser.getTrack(TrackAudio));
        require(video && audio && video->ready() && audio->ready(), "SDP tracks are not ready");
        video->setIndex(audio_added_first ? 11 : 2);
        audio->setIndex(audio_added_first ? 2 : 11);
        addTracks(audio_added_first);
        mux.getRtpRing()->setDelegate(std::make_shared<RingDelegateHelper>([this](RtpPacket::Ptr packet, bool) {
            RtcpContextForSend sender;
            sender.onRtp(packet->getSeq(), packet->getStamp(), packet->ntp_stamp, packet->sample_rate,
                         packet->size() - RtpPacket::kRtpTcpHeaderSize);
            auto buffer = sender.createRtcpSR(packet->getSSRC());
            auto sr = reinterpret_cast<const RtcpSR *>(buffer->data());
            auto ntp = uint64_t(ntohl(sr->ntpmsw) - 0x83AA7E80) * 1000
                + uint64_t(ntohl(sr->ntplsw)) * 1000 / (uint64_t(1) << 32);
            require(ntohl(sr->rtpts) == packet->getStamp(), "SR rewrote RTP stamp");
            require(std::abs(int64_t(ntp) - int64_t(packet->ntp_stamp)) <= 1, "SR differs from paired NTP metadata");
            packets.push_back(std::move(packet));
        }));
    }
    void addTracks(bool audio_first = false) {
        require(mux.addTrack(audio_first ? audio : video), "first addTrack failed");
        require(mux.addTrack(audio_first ? video : audio), "second addTrack failed");
    }
    RtpPacket::Ptr push(const Frame::Ptr &value) {
        auto count = packets.size();
        require(mux.inputFrame(value), "inputFrame failed");
        require(packets.size() == count + 1, "fixture expected one immediate RTP packet");
        return packets.back();
    }
    RtpPacket::Ptr push(bool is_audio, uint64_t stamp) {
        return push(frame(is_audio, (is_audio ? audio : video)->getIndex(), stamp, stamp));
    }
};

static void track_order(bool add_audio_first, bool arrive_audio_first) {
    Capture c(add_audio_first);
    auto first = c.push(arrive_audio_first, arrive_audio_first ? 1080 : 1000);
    auto second = c.push(!arrive_audio_first, arrive_audio_first ? 1000 : 1080);
    int64_t first_input = arrive_audio_first ? 1080 : 1000;
    int64_t second_input = arrive_audio_first ? 1000 : 1080;
    require(int64_t(second->ntp_stamp) - int64_t(first->ntp_stamp) == second_input - first_input,
            "independent track origin changed AV difference");
    require(first->getStamp() == uint32_t(first_input * first->sample_rate / 1000), "first RTP timestamp changed");
    require(second->getStamp() == uint32_t(second_input * second->sample_rate / 1000), "second RTP timestamp changed");
}

static void zero_origin() {
    Capture c;
    auto zero = c.push(false, 0);
    auto audio_zero = c.push(true, 0);
    require(zero->ntp_stamp == audio_zero->ntp_stamp, "zero input was treated as uninitialized per-track origin");
    auto repeated_zero = c.push(false, 0);
    require(repeated_zero->ntp_stamp == zero->ntp_stamp, "repeated zero changed the common origin");
    auto v80 = c.push(false, 80);
    require(v80->ntp_stamp - zero->ntp_stamp == 80, "video PTS advance from zero changed");
}

static void backwards_pts() {
    Capture c;
    c.push(false, 1000);
    auto v80 = c.push(frame(false, c.video->getIndex(), 1080, 1040));
    auto v40 = c.push(frame(false, c.video->getIndex(), 1040, 1080));
    require(int64_t(v40->ntp_stamp) - int64_t(v80->ntp_stamp) == -40, "legal B-frame NTP correspondence was clamped");
    require(v40->getStamp() == 93600, "B-frame RTP timestamp was rewritten");
}

static void audio_backlog() {
    Capture c;
    const uint64_t epoch = 1788860000000ULL;
    auto first = c.push(true, epoch);
    for (unsigned i = 1; i < 40; ++i) c.push(true, epoch + i * 20);
    auto video = c.push(false, epoch + 104);
    require(video->ntp_stamp - first->ntp_stamp == 104, "audio-first backlog forced unrelated video synchronization");
}

static void epoch_gap() {
    Capture c;
    const uint64_t epoch = 1788860000000ULL;
    auto first = c.push(false, epoch);
    auto minute = c.push(false, epoch + 60000);
    require(minute->ntp_stamp - first->ntp_stamp == 60000, "long source stop was clamped/reanchored");
    auto last = c.push(false, epoch + 28800000); // Eight-hour source offset is representable.
    require(last->ntp_stamp - first->ntp_stamp == 28800000, "long stop or eight-hour source offset was clamped/reanchored");
}

static void exact_g711_samples() {
    Capture c;
    const uint64_t initial_samples = uint64_t(UINT32_MAX) - 79;
    for (unsigned i = 0; i < 3; ++i) {
        auto value = std::make_shared<G711RtpFrame>();
        value->_codec_id = CodecG711U;
        value->setIndex(c.audio->getIndex());
        value->_pts = value->_dts = 1000 + i * 20;
        value->_buffer = std::string(160, '\x55');
        value->sample_stamp = initial_samples + i * 160;
        value->ntp_stamp_us = (1000 + i * 20) * 1000;
        value->sample_rate = 8000;
        value->channels = 1;
        auto packet = c.push(value);
        require(packet->getStamp() == uint32_t(initial_samples + i * 160), "exact G711 sample axis or wrap changed");
        require(packet->getPayloadSize() == 160 && std::string(reinterpret_cast<const char *>(packet->getPayload()), 160) == std::string(160, '\x55'), "G711 payload changed");
        require(packet->ntp_stamp - c.packets.front()->ntp_stamp == i * 20, "G711 sample/NTP relationship changed");
    }
}

static void reset_generation() {
    Capture live;
    live.push(false, 1234567);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    const auto before = getCurrentMillisecond(true);
    live.mux.resetTracks();
    live.addTracks(true);
    auto new_generation = live.push(true, 0);
    require(new_generation->ntp_stamp >= before && new_generation->ntp_stamp <= getCurrentMillisecond(true),
            "reset reused the preceding generation wall/media origin");

}

static void vod_backwards() {
    Capture vod(false, true);
    auto old_position = vod.push(false, 1000);
    auto seek_back = vod.push(false, 500);
    require(int64_t(seek_back->ntp_stamp) - int64_t(old_position->ntp_stamp) == -500, "VOD backward seek changed");
    vod.mux.resetTracks();
    vod.addTracks();
    auto zero_position = vod.push(false, 0);
    require(old_position->ntp_stamp - zero_position->ntp_stamp == 1000, "VOD reset branch changed");
}

static void configured_frame_stamp(int mode) {
    Capture c;
    Stamp upstream;
    int64_t common_offset = 0;
    for (unsigned i = 0; i < 3; ++i) {
        Frame::Ptr input = frame(false, c.video->getIndex(), i == 2 ? 10000 : 5000 + i * 64, i == 2 ? 10000 : 5000 + i * 64);
        if (mode) input = std::make_shared<FrameStamp>(input, upstream, mode);
        auto actual_policy_pts = input->pts();
        auto out = c.push(input);
        if (!i) common_offset = int64_t(out->ntp_stamp) - int64_t(actual_policy_pts);
        require(int64_t(out->ntp_stamp) - int64_t(actual_policy_pts) == common_offset, "RtspMuxer overrode upstream modify_stamp policy");
        require(out->getStamp() == uint32_t(actual_policy_pts * 90), "configured RTP output changed");
        if (mode == ProtocolOption::kModifyStampRelative && i == 2)
            require(actual_policy_pts == 128, "upstream relative gap correction did not execute");
    }
}

static void configured_negative_pts(int mode, bool audio_first) {
    Capture c;
    Stamp upstream;
    int64_t ignored_dts, ignored_pts;
    // Establish a known DTS origin without a wall-clock sleep. The first
    // FrameStamp keeps this DTS and applies its negative composition offset.
    upstream.revise(5000, 5000, ignored_dts, ignored_pts);
    if (audio_first) c.push(true, 0);
    RtpPacket::Ptr previous;
    int64_t previous_pts = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const uint64_t dts = 5000 + i * 40;
        auto input = std::make_shared<FrameStamp>(frame(false, c.video->getIndex(),
                                                      i == 2 ? dts + 40 : dts - 40, dts), upstream, mode);
        const auto signed_pts = int64_t(input->pts());
        if (!i) require(signed_pts == -40, "fixture did not produce actual negative FrameStamp PTS");
        if (i == 2) require(signed_pts >= 40, "fixture did not cross into positive PTS");
        auto packet = c.push(input);
        if (previous) {
            require(int64_t(packet->ntp_stamp) - int64_t(previous->ntp_stamp) == signed_pts - previous_pts,
                    "configured negative PTS lost the common NTP offset across zero");
        } else {
            // Existing unsigned encoder arithmetic is deliberately unchanged;
            // this is not the mathematically signed -3600 RTP tick value.
            require(packet->getStamp() == 1271306719U, "pre-existing negative-PTS RTP header changed");
            if (audio_first) require(int64_t(packet->ntp_stamp) - int64_t(c.packets.front()->ntp_stamp) == -40,
                                     "negative video PTS lost the audio-first common origin");
        }
        previous = packet;
        previous_pts = signed_pts;
    }
}

static void signed_frame_cross_zero(bool start_negative) {
    Capture c;
    const int64_t values[3] = {start_negative ? -40 : 0, start_negative ? 0 : -40, 40};
    RtpPacket::Ptr previous;
    int64_t previous_pts = 0;
    for (auto pts : values) {
        auto input = std::make_shared<FrameStamp>(frame(false, c.video->getIndex(), 1000, 1000));
        input->setStamp(0, pts);
        auto packet = c.push(input);
        if (previous) require(int64_t(packet->ntp_stamp) - int64_t(previous->ntp_stamp) == pts - previous_pts,
                              "signed FrameStamp PTS crossing zero changed NTP delta");
        previous = packet;
        previous_pts = pts;
    }
}

int main() {
    mINI::Instance()[Rtp::kLowLatency] = 1;
    mINI::Instance()[Rtp::kAudioMtuSize] = 600;
    unsigned failures = 0, count = 0;
    auto run = [&](const char *name, const std::function<void()> &test) {
        ++count;
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception &error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    };
    run("video add/arrival first", [] { track_order(false, false); });
    run("video add/audio arrival first", [] { track_order(false, true); });
    run("audio add/video arrival first", [] { track_order(true, false); });
    run("audio add/arrival first", [] { track_order(true, true); });
    run("zero and repeated zero origin", zero_origin);
    run("legal B-frame PTS", backwards_pts);
    run("audio-first backlog", audio_backlog);
    run("long gap and eight-hour offset", epoch_gap);
    run("exact G711 sample axis and wrap", exact_g711_samples);
    run("reset live generation", reset_generation);
    run("VOD backward seek and reset", vod_backwards);
    run("modify_stamp 0", [] { configured_frame_stamp(0); });
    run("modify_stamp 1 actual FrameStamp", [] { configured_frame_stamp(1); });
    run("modify_stamp 2 actual FrameStamp", [] { configured_frame_stamp(2); });
    run("modify_stamp 1 negative PTS video first", [] { configured_negative_pts(1, false); });
    run("modify_stamp 1 negative PTS audio first", [] { configured_negative_pts(1, true); });
    run("modify_stamp 2 negative PTS video first", [] { configured_negative_pts(2, false); });
    run("modify_stamp 2 negative PTS audio first", [] { configured_negative_pts(2, true); });
    run("signed FrameStamp negative through zero", [] { signed_frame_cross_zero(true); });
    run("signed FrameStamp zero then negative", [] { signed_frame_cross_zero(false); });
    std::cout << count << " public inputFrame cases; failures=" << failures << '\n';
    return failures ? 1 : 0;
}
