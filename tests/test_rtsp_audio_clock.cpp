/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Common/config.h"
#include "Extension/Factory.h"
#include "Rtsp/RtpReceiver.h"
#include "Rtsp/RtspMuxer.h"
#include "ext-codec/AACRtp.h"
#include "ext-codec/G711Rtp.h"

using namespace mediakit;
using namespace toolkit;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

FrameImp::Ptr makeFrame(CodecId codec, int index, uint64_t stamp, const std::string &payload) {
    auto frame = FrameImp::create();
    frame->_codec_id = codec;
    frame->setIndex(index);
    frame->_dts = frame->_pts = stamp;
    frame->_buffer.assign(payload.data(), payload.size());
    return frame;
}

// A sender report is built from precisely these two fields by
// RtcpContextForSend: RTP.ts and RtpPacket::ntp_stamp. No sockets, sleeps or
// artificial system clock are needed to test their relative progression.
void testG711DoesNotAdvanceVideoSenderClock(bool audio_first) {
    const std::string input_sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=audio clock regression\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z2QAH6zZQFAFuhAAAAMAEAAAAwDxgxHg,aM4G4g==\r\n"
        "a=control:trackID=0\r\n"
        // Deliberately reproduce the camera's nonstandard static PT mapping.
        "m=audio 0 RTP/AVP 0\r\n"
        "a=rtpmap:0 PCMU/16000/1\r\n"
        "a=control:trackID=1\r\n";
    SdpParser parser(input_sdp);
    auto video = Factory::getTrackBySdp(parser.getTrack(TrackVideo));
    auto audio = Factory::getTrackBySdp(parser.getTrack(TrackAudio));
    require(video && audio && video->ready() && audio->ready(), "failed to create ready SDP tracks");
    video->setIndex(audio_first ? 11 : 2);
    audio->setIndex(audio_first ? 2 : 11);

    RtspMuxer muxer;
    require(muxer.addTrack(audio_first ? audio : video), "failed to add first track");
    require(muxer.addTrack(audio_first ? video : audio), "failed to add second track");
    std::vector<RtpPacket::Ptr> video_packets;
    std::vector<RtpPacket::Ptr> audio_packets;
    muxer.getRtpRing()->setDelegate(std::make_shared<RingDelegateHelper>(
        [&](RtpPacket::Ptr packet, bool) {
            (packet->type == TrackVideo ? video_packets : audio_packets).emplace_back(std::move(packet));
        }));

    // Each 40 ms interval contains one 25 fps video frame and 640 PCMU
    // samples at 16 kHz. The legacy 8 kHz G711 Track emits four 20 ms packets
    // here, so the next interval starts with a backwards audio timestamp.
    // Stamp rejects that rollback and repeatedly syncs the video NTP clock
    // forward, while the actual video RTP timestamp still advances normally.
    constexpr uint64_t first_stamp = 1000000;
    constexpr size_t frame_count = 500;
    const std::string video_payload("\x41\x80\x01\x02", 4);
    const std::string audio_payload(640, static_cast<char>(0xff));
    for (size_t i = 0; i < frame_count; ++i) {
        auto stamp = first_stamp + i * 40;
        auto video_frame = makeFrame(CodecH264, video->getIndex(), stamp, video_payload);
        auto audio_frame = makeFrame(CodecG711U, audio->getIndex(), stamp, audio_payload);
        muxer.inputFrame(audio_first ? audio_frame : video_frame);
        muxer.inputFrame(audio_first ? video_frame : audio_frame);
    }
    muxer.flush();
    require(video_packets.size() == frame_count, "unexpected video packet count");
    require(!audio_packets.empty(), "G711 encoder produced no packets");

    const auto video_rtp_ms = static_cast<uint32_t>(video_packets.back()->getStamp()
        - video_packets.front()->getStamp()) / 90.0;
    const auto video_ntp_ms = static_cast<int64_t>(video_packets.back()->ntp_stamp)
        - static_cast<int64_t>(video_packets.front()->ntp_stamp);
    size_t payload_bytes = 0;
    size_t audio_rollbacks = 0;
    for (size_t i = 0; i < audio_packets.size(); ++i) {
        payload_bytes += audio_packets[i]->getPayloadSize();
        if (i && static_cast<int32_t>(audio_packets[i]->getStamp() - audio_packets[i - 1]->getStamp()) <= 0) {
            ++audio_rollbacks;
        }
    }
    std::cout << "G711 clock (audio_first=" << audio_first << "): video RTP=" << video_rtp_ms
              << " ms, video SR NTP=" << video_ntp_ms << " ms, audio packets=" << audio_packets.size()
              << ", audio clock=" << audio_packets.front()->sample_rate
              << ", audio non-increasing timestamps=" << audio_rollbacks << std::endl;

    // Check the externally visible clock corruption first, so the unchanged
    // implementation demonstrates the same 1x RTP / 2x SR failure as the IPC.
    require(std::abs(video_ntp_ms - video_rtp_ms) <= 40,
        "G711 repacketization advanced the video sender-report clock independently of video RTP");
    require(audio_rollbacks == 0, "G711 packets repeat or roll back their sample timeline");
    require(payload_bytes == frame_count * audio_payload.size(), "G711 samples were lost or duplicated");
    require(audio_packets.front()->sample_rate == 16000, "G711 RTP clock discarded the SDP sample rate");
    auto audio_out = SdpParser(muxer.getSdp()).getTrack(TrackAudio);
    require(audio_out && audio_out->_pt >= 96 && audio_out->_samplerate == 16000 && audio_out->_channel == 1,
        "nonstandard input G711 must be remapped to dynamic PT with matching SDP clock/channel");
    for (const auto &packet : audio_packets) {
        require(packet->getHeader()->pt == audio_out->_pt, "RTP PT differs from advertised SDP PT");
        require(packet->size() - RtpPacket::kRtpTcpHeaderSize <= 600, "G711 RTP exceeds the configured MTU");
    }
    const auto audio_span_ms = static_cast<uint32_t>(audio_packets.back()->getStamp()
        - audio_packets.front()->getStamp()) / 16.0 + audio_packets.back()->getPayloadSize() / 16.0;
    require(std::abs(audio_span_ms - frame_count * 40.0) <= 1,
        "G711 output sample duration differs from the 20-second input duration");
}

void testG711BounceDoesNotAdvanceVideoSenderClock(bool audio_first) {
    const std::string input_sdp =
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=paired G711 bounce regression\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 97\r\n"
        "a=rtpmap:97 H264/90000\r\n"
        "a=fmtp:97 packetization-mode=1;sprop-parameter-sets=Z2QAH6zZQFAFuhAAAAMAEAAAAwDxgxHg,aM4G4g==\r\n"
        "a=control:trackID=0\r\n"
        "m=audio 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 PCMA/32000/1\r\n"
        "a=control:trackID=1\r\n";
    SdpParser parser(input_sdp);
    auto video = Factory::getTrackBySdp(parser.getTrack(TrackVideo));
    auto audio = Factory::getTrackBySdp(parser.getTrack(TrackAudio));
    require(video && audio && video->ready() && audio->ready(), "failed to create bounce SDP tracks");
    video->setIndex(audio_first ? 1 : 0);
    audio->setIndex(audio_first ? 0 : 1);
    RtspMuxer muxer;
    require(muxer.addTrack(audio_first ? audio : video), "failed to add first bounce track");
    require(muxer.addTrack(audio_first ? video : audio), "failed to add second bounce track");
    std::vector<RtpPacket::Ptr> video_packets;
    std::vector<RtpPacket::Ptr> audio_packets;
    muxer.getRtpRing()->setDelegate(std::make_shared<RingDelegateHelper>(
        [&](RtpPacket::Ptr packet, bool) {
            (packet->type == TrackVideo ? video_packets : audio_packets).emplace_back(std::move(packet));
        }));

    constexpr int rate = 32000;
    constexpr uint64_t first_stamp = 1000000;
    constexpr uint32_t raw_start = 0x10000000;
    constexpr size_t samples_per_frame = 1280;
    constexpr size_t frame_count = 1501; // 60 seconds between the first and last video frames.
    size_t decoded_frames = 0;
    auto decoder = Factory::getRtpDecoderByCodecId(CodecG711A);
    require(bool(decoder), "failed to create G711 bounce decoder");
    decoder->setAudioInfo(rate, 1);
    decoder->addDelegate([&](const Frame::Ptr &input) {
        auto cached = Frame::getCacheAbleFrame(input);
        auto exact = dynamic_cast<const G711RtpFrame *>(cached.get());
        require(exact && exact->sample_rate == rate && exact->channels == 1,
            "G711 decoder lost cacheable sample/NTP metadata before the muxer");
        require(!exact->discontinuity, "bounded bounce or small SR adjustment became a discontinuity");
        require(exact->sample_stamp == first_stamp * rate / 1000 + decoded_frames * samples_per_frame,
            "G711 decoder turned timestamp bounce into a sample gap or overlap");
        ++decoded_frames;
        // Assign the media pipeline's track index explicitly; the receiver's
        // packet track_index is not audio routing metadata.
        cached->setIndex(audio->getIndex());
        return muxer.inputFrame(cached);
    });
    RtpTrackImp receiver;
    receiver.setOnSorted([&](RtpPacket::Ptr packet) { decoder->inputRtp(packet, false); });
    RtpInfo info(0x12345678, 4096, rate, 96, 2, audio->getIndex());
    const std::string video_payload("\x41\x80\x01\x02", 4);
    const int sr_offsets_ms[] = { 0, 3, -3 };
    std::string expected_audio;
    expected_audio.reserve(frame_count * samples_per_frame);
    for (size_t i = 0; i < frame_count; ++i) {
        const auto stamp = first_stamp + i * 40;
        const auto natural_raw = raw_start + static_cast<uint32_t>(i * samples_per_frame);
        if (i % 125 == 0) {
            // Exercise the real SR -> NtpStamp -> decoder mapping, not an
            // already-smoothed Frame PTS. Video remains a stable 25 fps clock.
            receiver.setNtpStamp(natural_raw, static_cast<int64_t>(stamp) + sr_offsets_ms[(i / 125) % 3]);
        }
        // Exercise three-, four- and five-packet phase holds, including a
        // direct +10 -> -10 ms transition and its reverse. These are offsets
        // from the original cumulative sample axis, not adjacent-packet
        // deltas; no payload sample is missing at either phase transition.
        const auto hold_packets = 3 + (i / 25) % 3;
        const auto phase = i % 25;
        const int first_phase_ms = i / 25 % 2 ? -10 : 10;
        const int bounce_ms = phase >= 5 && phase < 5 + hold_packets ? first_phase_ms
            : phase >= 5 + hold_packets && phase < 5 + 2 * hold_packets ? -first_phase_ms : 0;
        const auto raw_stamp = static_cast<uint32_t>(static_cast<int64_t>(natural_raw) + bounce_ms * (rate / 1000));
        std::string payload(samples_per_frame, '\0');
        for (size_t j = 0; j < payload.size(); ++j) {
            payload[j] = static_cast<char>((i * samples_per_frame + j) & 255);
        }
        expected_audio.append(payload);
        auto packet = info.makeRtpWithStamp(TrackAudio, payload.data(), payload.size(), true, 0, raw_stamp);
        auto video_frame = makeFrame(CodecH264, video->getIndex(), stamp, video_payload);
        if (!audio_first) {
            muxer.inputFrame(video_frame);
        }
        receiver.inputRtp(TrackAudio, rate,
            reinterpret_cast<uint8_t *>(packet->data()) + RtpPacket::kRtpTcpHeaderSize,
            packet->size() - RtpPacket::kRtpTcpHeaderSize);
        require(decoded_frames == i + 1, "G711 bounce path delayed or lost a complete input frame");
        if (audio_first) {
            muxer.inputFrame(video_frame);
        }
    }
    muxer.flush();
    require(video_packets.size() == frame_count, "bounce path lost or duplicated video frames");
    require(!audio_packets.empty(), "bounce path produced no G711 RTP");
    auto audio_sdp = SdpParser(muxer.getSdp()).getTrack(TrackAudio);
    require(audio_sdp && audio_sdp->_pt >= 96 && audio_sdp->_samplerate == rate && audio_sdp->_channel == 1,
        "bounce path changed the advertised PCMA sample clock or channel count");
    std::string actual_audio;
    uint64_t next_sample = first_stamp * rate / 1000;
    for (const auto &packet : audio_packets) {
        require(packet->getStamp() == static_cast<uint32_t>(next_sample),
            "G711 output sample intervals overlap or skip after timestamp bounce");
        require(packet->sample_rate == rate && packet->getHeader()->pt == audio_sdp->_pt,
            "G711 output RTP clock or PT differs from its SDP");
        require(packet->size() - RtpPacket::kRtpTcpHeaderSize <= 600,
            "G711 bounce output exceeds the configured MTU");
        actual_audio.append(reinterpret_cast<const char *>(packet->getPayload()), packet->getPayloadSize());
        next_sample += packet->getPayloadSize();
    }
    require(actual_audio == expected_audio, "G711 bounce lost, duplicated or reordered sample bytes");

    int64_t max_video_error_ms = 0;
    for (size_t i = 1; i < video_packets.size(); ++i) {
        const auto &packet = video_packets[i];
        const auto &previous = video_packets[i - 1];
        require(static_cast<uint32_t>(packet->getStamp() - previous->getStamp()) == 3600,
            "G711 bounce changed a 25 fps video RTP increment");
        const auto ntp_step = static_cast<int64_t>(packet->ntp_stamp) - static_cast<int64_t>(previous->ntp_stamp);
        require(std::abs(ntp_step - 40) <= 5, "G711 bounce caused a video sender-clock step");
        const auto ntp_span = static_cast<int64_t>(packet->ntp_stamp)
            - static_cast<int64_t>(video_packets.front()->ntp_stamp);
        const auto error = std::abs(ntp_span - static_cast<int64_t>(i * 40));
        max_video_error_ms = std::max(max_video_error_ms, error);
        require(error <= 5, "G711 bounce accumulated error in the shared video sender clock");
        if (i % 125 == 0) {
            const auto &last_report = video_packets[i - 125];
            const auto ntp_interval = static_cast<int64_t>(packet->ntp_stamp)
                - static_cast<int64_t>(last_report->ntp_stamp);
            const auto rtp_interval = static_cast<uint32_t>(packet->getStamp() - last_report->getStamp()) / 90;
            require(std::abs(ntp_interval - static_cast<int64_t>(rtp_interval)) <= 5,
                "adjacent video SR NTP/RTP intervals diverged after G711 bounce");
        }
    }
    std::cout << "G711 paired bounce (audio_first=" << audio_first
              << ", phase holds=3/4/5 packets, direct +/- transitions): video=60000 ms, audio samples="
              << actual_audio.size() << ", max video SR error=" << max_video_error_ms << " ms" << std::endl;
}

std::vector<uint8_t> aacPayload(const std::vector<std::string> &units) {
    std::vector<uint8_t> payload;
    auto header_bits = units.size() * 16;
    payload.push_back(static_cast<uint8_t>(header_bits >> 8));
    payload.push_back(static_cast<uint8_t>(header_bits));
    for (const auto &unit : units) {
        auto header = unit.size() << 3;
        payload.push_back(static_cast<uint8_t>(header >> 8));
        payload.push_back(static_cast<uint8_t>(header));
    }
    for (const auto &unit : units) {
        payload.insert(payload.end(), unit.begin(), unit.end());
    }
    return payload;
}

void testAacSingleAuUsesCurrentPacketTimestamp() {
    AACRtpDecoder decoder;
    std::vector<uint64_t> stamps;
    decoder.addDelegate([&](const Frame::Ptr &frame) {
        stamps.push_back(frame->dts());
        return true;
    });
    RtpInfo info(0x12345678, 600, 16000, 97, 2, 1);
    const auto payload = aacPayload({ "au" });
    const std::vector<uint64_t> expected { 1000, 1064, 1128, 1192 };
    for (auto stamp : expected) {
        decoder.inputRtp(info.makeRtp(TrackAudio, payload.data(), payload.size(), true, stamp));
    }
    std::cout << "AAC single-AU DTS:";
    for (auto stamp : stamps) {
        std::cout << ' ' << stamp;
    }
    std::cout << std::endl;
    require(stamps == expected, "AAC single-AU packet was timestamped with the previous packet's DTS");
}

void testAacMultipleAusAdvanceBySampleCount() {
    // Current AAC-hbr compatibility scope: AAC-LC, 1024 samples per AU.
    AACRtpDecoder decoder;
    std::vector<uint64_t> stamps;
    decoder.addDelegate([&](const Frame::Ptr &frame) {
        stamps.push_back(frame->dts());
        return true;
    });
    RtpInfo info(0x12345678, 600, 16000, 97, 2, 1);
    const auto payload = aacPayload({ "a0", "a1", "a2" });
    decoder.inputRtp(info.makeRtp(TrackAudio, payload.data(), payload.size(), true, 1000));
    decoder.inputRtp(info.makeRtp(TrackAudio, payload.data(), payload.size(), true, 1192));
    require(stamps == std::vector<uint64_t>({ 1000, 1064, 1128, 1192, 1256, 1320 }),
        "AAC bundled AUs must start at the current RTP timestamp and advance 1024 samples each");
}

void testAacFragmentedAuRetainsItsTimestamp() {
    AACRtpDecoder decoder;
    std::vector<uint64_t> stamps;
    decoder.addDelegate([&](const Frame::Ptr &frame) {
        stamps.push_back(frame->dts());
        return true;
    });
    RtpInfo info(0x12345678, 600, 16000, 97, 2, 1);
    // Each fragment declares the complete four-byte AU length in its header.
    const std::vector<uint8_t> first { 0, 16, 0, 32, 'a', 'b' };
    const std::vector<uint8_t> last { 0, 16, 0, 32, 'c', 'd' };
    for (uint64_t stamp : { 1000U, 1064U }) {
        auto count = stamps.size();
        decoder.inputRtp(info.makeRtp(TrackAudio, first.data(), first.size(), false, stamp));
        require(stamps.size() == count, "AAC decoder emitted an incomplete AU");
        decoder.inputRtp(info.makeRtp(TrackAudio, last.data(), last.size(), true, stamp));
    }
    require(stamps == std::vector<uint64_t>({ 1000, 1064 }), "fragmented AAC AU lost its packet timestamp");
}

struct AacCapture {
    AACRtpDecoder decoder;
    RtpInfo info { 0x12345678, 600, 16000, 97, 2, 1 };
    std::vector<Frame::Ptr> frames;

    AacCapture() {
        decoder.addDelegate([&](const Frame::Ptr &frame) {
            frames.push_back(frame);
            return true;
        });
    }

    RtpPacket::Ptr packet(const std::vector<uint8_t> &payload, uint64_t stamp, bool mark) {
        return info.makeRtp(TrackAudio, payload.data(), payload.size(), mark, stamp);
    }
};

void testAacFragmentLossAndLatePackets() {
    AacCapture capture;
    const std::vector<uint8_t> first { 0, 16, 0, 48, 'a', 'b' };
    const std::vector<uint8_t> middle { 0, 16, 0, 48, 'c', 'd' };
    const std::vector<uint8_t> last { 0, 16, 0, 48, 'e', 'f' };
    capture.decoder.inputRtp(capture.packet(first, 1000, false));
    auto late = capture.packet(middle, 1000, false);
    capture.decoder.inputRtp(capture.packet(last, 1000, true));
    capture.decoder.inputRtp(late);
    require(capture.frames.empty(), "AAC fragment gap/late packet emitted a damaged AU");
    capture.decoder.inputRtp(capture.packet(aacPayload({ "ok" }), 1064, true));
    require(capture.frames.size() == 1 && capture.frames[0]->toString() == "ok"
            && capture.frames[0]->dts() == 1064,
        "AAC fragment loss contaminated the next AU");
}

void testAacDuplicateFragmentsAndSequenceWrap() {
    AacCapture capture;
    const std::vector<uint8_t> first { 0, 16, 0, 32, 'a', 'b' };
    const std::vector<uint8_t> last { 0, 16, 0, 32, 'c', 'd' };
    auto start = capture.packet(first, 1000, false);
    auto finish = capture.packet(last, 1000, true);
    start->getHeader()->seq = htons(65535);
    finish->getHeader()->seq = htons(0);
    // Simulate an SR remapping the NTP value between two pieces of one AU.
    finish->ntp_stamp = 6000;
    capture.decoder.inputRtp(start);
    capture.decoder.inputRtp(start);
    capture.decoder.inputRtp(finish);
    capture.decoder.inputRtp(finish);
    require(capture.frames.size() == 1 && capture.frames[0]->toString() == "abcd"
            && capture.frames[0]->dts() == 1000,
        "AAC duplicate/wrapped fragments changed samples or the first-fragment timestamp");
}

void testAacNewTimestampAcceptsSequenceReset() {
    AacCapture capture;
    const auto payload = aacPayload({ "ok" });
    auto before = capture.packet(payload, 1000, true);
    auto reset = capture.packet(payload, 1064, true);
    auto after = capture.packet(payload, 1128, true);
    before->getHeader()->seq = htons(20000);
    reset->getHeader()->seq = htons(1000);
    after->getHeader()->seq = htons(1001);
    capture.decoder.inputRtp(before);
    capture.decoder.inputRtp(reset);
    capture.decoder.inputRtp(after);
    require(capture.frames.size() == 3 && capture.frames[1]->dts() == 1064
            && capture.frames[2]->dts() == 1128,
        "AAC decoder rejected the RTP sorter's same-SSRC sequence-reset recovery");

    AacCapture interrupted;
    const std::vector<uint8_t> fragment { 0, 16, 0, 32, 'a', 'b' };
    auto partial = interrupted.packet(fragment, 1000, false);
    auto complete = interrupted.packet(payload, 1064, true);
    partial->getHeader()->seq = htons(20000);
    complete->getHeader()->seq = htons(1000);
    interrupted.decoder.inputRtp(partial);
    interrupted.decoder.inputRtp(complete);
    require(interrupted.frames.size() == 1 && interrupted.frames[0]->toString() == "ok"
            && interrupted.frames[0]->dts() == 1064,
        "sequence reset retained an incomplete AAC AU from the old timestamp");
}

void testAacFragmentCannotCrossTimestampOrSsrc() {
    const std::vector<uint8_t> first { 0, 16, 0, 32, 'a', 'b' };
    const auto complete = aacPayload({ "ok" });
    AacCapture changed_stamp;
    changed_stamp.decoder.inputRtp(changed_stamp.packet(first, 1000, false));
    changed_stamp.decoder.inputRtp(changed_stamp.packet(complete, 1064, true));
    require(changed_stamp.frames.size() == 1 && changed_stamp.frames[0]->toString() == "ok",
        "incomplete AAC payload crossed its RTP timestamp");

    AacCapture changed_ssrc;
    changed_ssrc.decoder.inputRtp(changed_ssrc.packet(first, 1000, false));
    RtpInfo other(0x87654321, 600, 16000, 97, 2, 1);
    changed_ssrc.decoder.inputRtp(other.makeRtp(TrackAudio, complete.data(), complete.size(), true, 1000));
    require(changed_ssrc.frames.size() == 1 && changed_ssrc.frames[0]->toString() == "ok",
        "incomplete AAC payload crossed its SSRC");
}

void testAacMalformedFragmentCannotContaminateNextAu() {
    AacCapture capture;
    const std::vector<uint8_t> first { 0, 16, 0, 32, 'a', 'b' };
    const std::vector<uint8_t> wrong_size { 0, 16, 0, 48, 'c', 'd' };
    capture.decoder.inputRtp(capture.packet(first, 1000, false));
    capture.decoder.inputRtp(capture.packet(wrong_size, 1000, true));
    require(capture.frames.empty(), "inconsistent AAC fragment length emitted an AU");
    capture.decoder.inputRtp(capture.packet(aacPayload({ "ok" }), 1064, true));
    require(capture.frames.size() == 1 && capture.frames[0]->toString() == "ok",
        "malformed AAC fragment contaminated the next AU");

    const std::vector<uint8_t> truncated { 0, 16, 0, 32, 'x' };
    capture.decoder.inputRtp(capture.packet(truncated, 1128, true));
    require(capture.frames.size() == 1, "truncated terminal AAC fragment emitted an AU");
    capture.decoder.inputRtp(capture.packet(aacPayload({ "next" }), 1192, true));
    require(capture.frames.size() == 2 && capture.frames[1]->toString() == "next",
        "truncated AAC packet contaminated the next AU");
}

void testAacBundleIndicesAndFractionalClock() {
    AacCapture indexed;
    auto payload = aacPayload({ "a0", "a1" });
    payload[3] |= 5; // The first AU index does not offset the packet's RTP timestamp.
    payload[5] |= 2; // AU-index-delta=2: two AUs are skipped before this one.
    indexed.decoder.inputRtp(indexed.packet(payload, 0, true));
    require(indexed.frames.size() == 2 && indexed.frames[0]->dts() == 0 && indexed.frames[1]->dts() == 192,
        "AAC bundle ignored AU-index-delta or zero packet timestamp");

    AacCapture fractional;
    RtpInfo info(0x12345678, 600, 44100, 97, 2, 1);
    const auto bundled = aacPayload(std::vector<std::string>(12, "au"));
    fractional.decoder.inputRtp(info.makeRtp(TrackAudio, bundled.data(), bundled.size(), true, 1000));
    require(fractional.frames.size() == 12, "unexpected 44.1 kHz AAC bundle size");
    for (size_t i = 0; i < fractional.frames.size(); ++i) {
        require(fractional.frames[i]->dts() == 1000 + i * 1024 * 1000 / 44100,
            "AAC bundle accumulated rounded millisecond durations");
    }
}

void testAacEasyPusherAdtsCompatibility() {
    AacCapture capture;
    const std::string adts("\xff\xf1\x60\x40\x01\x3f\xfc\x11\x22", 9);
    capture.decoder.inputRtp(capture.packet(aacPayload({ adts }), 1000, true));
    require(capture.frames.size() == 1 && capture.frames[0]->toString() == adts
            && capture.frames[0]->prefixSize() == ADTS_HEADER_LEN && capture.frames[0]->dts() == 1000,
        "AAC RTP decoder lost the existing EasyPusher ADTS compatibility");
}

} // namespace

int main(int argc, char **argv) {
    mINI::Instance()[Rtp::kLowLatency] = 1;
    mINI::Instance()[Rtp::kAudioMtuSize] = 600;
    const std::string group = argc > 1 ? argv[1] : "all";
    if (group != "all" && group != "g711" && group != "aac") {
        std::cerr << "usage: test_rtsp_audio_clock [all|g711|aac]" << std::endl;
        return 2;
    }
    int failures = 0;
    auto run = [&](const std::string &name, const std::function<void()> &test) {
        try {
            test();
            std::cout << "PASS " << name << std::endl;
        } catch (const std::exception &ex) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << ex.what() << std::endl;
        }
    };
    if (group != "aac") {
        run("G711/video sender clock, video first", [] { testG711DoesNotAdvanceVideoSenderClock(false); });
        run("G711/video sender clock, audio first", [] { testG711DoesNotAdvanceVideoSenderClock(true); });
        run("G711 bounce/video sender clock, video first", [] { testG711BounceDoesNotAdvanceVideoSenderClock(false); });
        run("G711 bounce/video sender clock, audio first", [] { testG711BounceDoesNotAdvanceVideoSenderClock(true); });
    }
    if (group != "g711") {
        run("AAC single AU timestamp", testAacSingleAuUsesCurrentPacketTimestamp);
        run("AAC bundled AU timestamps", testAacMultipleAusAdvanceBySampleCount);
        run("AAC fragmented AU timestamp", testAacFragmentedAuRetainsItsTimestamp);
        run("AAC fragment loss/late packets", testAacFragmentLossAndLatePackets);
        run("AAC duplicate fragments/sequence wrap", testAacDuplicateFragmentsAndSequenceWrap);
        run("AAC same-SSRC sequence reset", testAacNewTimestampAcceptsSequenceReset);
        run("AAC fragment timestamp/SSRC isolation", testAacFragmentCannotCrossTimestampOrSsrc);
        run("AAC malformed fragment recovery", testAacMalformedFragmentCannotContaminateNextAu);
        run("AAC bundle indices/fractional clock", testAacBundleIndicesAndFractionalClock);
        run("AAC EasyPusher ADTS compatibility", testAacEasyPusherAdtsCompatibility);
    }
    return failures ? 1 : 0;
}
