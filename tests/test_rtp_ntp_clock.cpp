/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "Extension/CommonRtp.h"
#include "Extension/Factory.h"
#include "Rtcp/RtcpContext.h"
#include "Rtsp/RtpReceiver.h"
#include "Rtsp/RtspMuxer.h"
#include "ext-codec/AAC.h"
#include "ext-codec/AACRtp.h"
#include "ext-codec/H265Rtp.h"

using namespace mediakit;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class ReceiverClock {
public:
    explicit ReceiverClock(TrackType type, int rate)
        : info(type == TrackVideo ? 11 : 12, 65535, rate, type == TrackVideo ? 96 : 97, 0, type),
          type(type), rate(rate) {
        receiver.setOnSorted([this](RtpPacket::Ptr packet) {
            packets.emplace_back(packet);
            if (on_packet) {
                on_packet(packet);
            }
        });
    }

    void input(uint16_t seq, uint32_t raw_stamp, size_t bytes = 16, bool marker = true) {
        // Deterministic synthetic bytes: the fixture contains no captured media.
        std::string payload(bytes, '\0');
        for (size_t i = 0; i < bytes; ++i) {
            payload[i] = static_cast<char>((uint64_t(seq) + i) % 251);
        }
        inputPayload(seq, raw_stamp, payload, marker);
    }

    void inputPayload(uint16_t seq, uint32_t raw_stamp, const std::string &payload, bool marker, uint32_t ssrc = 0) {
        auto packet = info.makeRtpWithStamp(type, payload.data(), payload.size(), marker, 0, raw_stamp);
        packet->getHeader()->seq = htons(seq);
        if (ssrc) {
            packet->getHeader()->ssrc = htonl(ssrc);
        }
        auto before = packets.size();
        receiver.inputRtp(type, rate,
            reinterpret_cast<uint8_t *>(packet->data()) + RtpPacket::kRtpTcpHeaderSize,
            packet->size() - RtpPacket::kRtpTcpHeaderSize);
        require(packets.size() == before + 1, "contiguous clock replay packet was buffered or lost");
    }

    RtpTrackImp receiver;
    RtpInfo info;
    TrackType type;
    int rate;
    std::vector<RtpPacket::Ptr> packets;
    std::function<void(const RtpPacket::Ptr &)> on_packet;
};

void testConsistentSenderReportsKeepMediaIntervals() {
    for (int rate : { 16000, 90000 }) {
        ReceiverClock clock(rate == 90000 ? TrackVideo : TrackAudio, rate);
        constexpr uint32_t raw_start = 123456;
        constexpr uint64_t ntp_start = 1000000;
        for (unsigned i = 0; i < 200; ++i) {
            auto raw = raw_start + i * (rate * 40 / 1000);
            if (i % 25 == 0) {
                clock.receiver.setNtpStamp(raw, ntp_start + i * 40);
            }
            clock.input(i, raw);
            auto stamp = clock.packets.back()->ntp_stamp;
            require(std::abs(int64_t(stamp) - int64_t(ntp_start + i * 40)) <= 1,
                    "consistent SRs changed the media clock origin or progression");
        }
    }
}

void testSenderReportsRemainAuthoritative() {
    for (int rate : { 16000, 90000 }) {
        ReceiverClock clock(rate == 90000 ? TrackVideo : TrackAudio, rate);
        const auto interval = uint32_t(rate * 40 / 1000);
        clock.receiver.setNtpStamp(123456, 1000000);
        clock.input(1000, 123456);
        // A caller supplying a new SR must retain upstream's immediate mapping
        // contract, including actual wall-clock calibration in either direction.
        clock.receiver.setNtpStamp(123456 + interval, 1000540);
        clock.input(1001, 123456 + interval);
        require(clock.packets.back()->ntp_stamp == 1000540, "positive SR calibration was delayed");
        clock.receiver.setNtpStamp(123456 + 2 * interval, 1000080);
        clock.input(1002, 123456 + 2 * interval);
        require(clock.packets.back()->ntp_stamp == 1000080, "negative SR calibration was delayed");
    }
}

void testLateFirstSenderReportAndClear() {
    ReceiverClock clock(TrackVideo, 90000);
    clock.input(1000, 123456); // The local bootstrap value is not deterministic.
    clock.receiver.setNtpStamp(127056, 1000000);
    clock.input(1001, 127056);
    require(clock.packets.back()->ntp_stamp == 1000000, "late first SR could not calibrate the clock");
    clock.receiver.clear();
    clock.receiver.setNtpStamp(90000, 2000000);
    clock.inputPayload(20, 90000, std::string(16, 'x'), true, 22);
    require(clock.receiver.getSSRC() == 22 && clock.packets.back()->ntp_stamp == 2000000,
            "clear followed by a new source and SR retained the old mapping");
}

void testSenderReportReenablesNtp() {
    ReceiverClock clock(TrackAudio, 16000);
    clock.receiver.setNtpStamp(0, 0);
    clock.input(1000, 16000);
    require(clock.packets.back()->ntp_stamp == 1000, "disabled NTP did not use the raw RTP clock");
    clock.receiver.setNtpStamp(16320, 1000000);
    clock.input(1001, 16320);
    require(clock.packets.back()->ntp_stamp == 1000000, "valid SR did not reenable NTP mapping");
}

void testConfirmedSequenceRestartUsesItsSenderReport() {
    ReceiverClock clock(TrackVideo, 90000);
    clock.receiver.setNtpStamp(900000, 1000000);
    clock.input(20000, 900000);
    clock.input(20001, 903600);
    // A same-SSRC raw reset of only 1040 ms must not permanently suppress a
    // truthful new SR. The existing sorter confirms the new sequence domain
    // with its default limits; do not add another restart gate in this test.
    clock.receiver.setNtpStamp(810000, 1000080);
    const std::string payload(16, 'x');
    for (unsigned i = 0; i < 259; ++i) {
        auto packet = clock.info.makeRtpWithStamp(TrackVideo, payload.data(), payload.size(), true, 0, 810000 + i * 3600);
        packet->getHeader()->seq = htons(1000 + i);
        clock.receiver.inputRtp(TrackVideo, 90000,
            reinterpret_cast<uint8_t *>(packet->data()) + RtpPacket::kRtpTcpHeaderSize,
            packet->size() - RtpPacket::kRtpTcpHeaderSize);
    }
    require(clock.packets.size() == 261, "confirmed sequence restart lost buffered RTP packets");
    for (size_t i = 2; i < clock.packets.size(); ++i) {
        const auto &packet = clock.packets[i];
        const auto expected = uint64_t(1000080 + (i - 2) * 40);
        require(packet->getSeq() == i - 2 + 1000 && std::abs(int64_t(packet->ntp_stamp) - int64_t(expected)) <= 1,
                "confirmed restart ignored its valid SR: seq=" + std::to_string(packet->getSeq())
                    + ", actual_ms=" + std::to_string(packet->ntp_stamp) + ", expected_ms=" + std::to_string(expected));
    }
}

void testRestartReportBeforeAndAfterFirstPacket() {
    for (bool report_first : { false, true }) {
        ReceiverClock clock(TrackVideo, 90000);
        clock.receiver.setNtpStamp(900000, 1000000);
        clock.input(1000, 900000);
        clock.input(1001, 903600);
        if (report_first) {
            clock.receiver.setNtpStamp(90000, 1000080);
        }
        clock.input(1002, 90000);
        clock.receiver.setNtpStamp(93600, 1000120);
        clock.input(1003, 93600);
        require(clock.packets.back()->ntp_stamp == 1000120, "fresh SR could not recover a raw-clock restart");
        if (report_first) {
            require(clock.packets[2]->ntp_stamp == 1000080, "SR preceding the first reset packet was discarded");
        }
    }
}

void testPresentationReorderingAndSequenceWrap() {
    ReceiverClock clock(TrackVideo, 90000);
    clock.receiver.setNtpStamp(900000, 1000000);
    const std::vector<unsigned> offsets { 0, 80, 40, 120, 160 };
    for (size_t i = 0; i < offsets.size(); ++i) {
        clock.input(1000 + i, 900000 + offsets[i] * 90);
        require(std::abs(int64_t(clock.packets.back()->ntp_stamp) - int64_t(1000000 + offsets[i])) <= 1,
                "presentation-order RTP was treated as a sender restart");
    }
    ReceiverClock wrapped(TrackAudio, 16000);
    constexpr uint32_t origin = 16000;
    wrapped.receiver.setNtpStamp(origin, 1000000);
    for (unsigned i = 0; i < 100; ++i) {
        wrapped.input(uint16_t(65500 + i), uint32_t(origin + i * 320));
        require(std::abs(int64_t(wrapped.packets.back()->ntp_stamp) - int64_t(1000000 + i * 20)) <= 1,
                "RTP sequence wrap broke ordinary audio progression");
    }
}

void testRtpTimestampWrapPrecision() {
    for (int rate : { 8000, 16000, 44100, 48000, 90000 }) {
        ReceiverClock wrapped(rate == 90000 ? TrackVideo : TrackAudio, rate);
        constexpr uint32_t origin = 0xffffff00U;
        wrapped.receiver.setNtpStamp(origin, 1000000);
        int64_t largest_error_ms = 0;
        for (unsigned i = 0; i < 100; ++i) {
            const auto raw = uint32_t(origin + i * (rate / 50));
            wrapped.input(uint16_t(65500 + i), raw);
            const auto expected = uint64_t(1000000 + i * 20);
            const auto error = int64_t(wrapped.packets.back()->ntp_stamp) - int64_t(expected);
            largest_error_ms = std::max(largest_error_ms, std::abs(error));
            require(std::abs(error) <= 1, "RTP wrap changed the 20 ms media span at rate " + std::to_string(rate));
        }
        // Upstream converted the almost-2^32 difference to float first: at
        // 16 kHz the ARM64 baseline retained a +12 ms error after the wrap.
        std::cout << "METRIC RTP timestamp wrap rate=" << rate
                  << " packets=100 maximum_error_ms=" << largest_error_ms << std::endl;
    }
}

void testRtpTimestampWrapReordering() {
    for (uint32_t rate : { 8000, 16000, 44100, 48000, 90000 }) {
        const uint32_t origin = uint32_t(0U - rate / 100); // 10 ms before the wrap.
        NtpStamp clock;
        clock.setNtpStamp(origin, 1000000);
        require(clock.getNtpStamp(uint32_t(origin + rate / 50), rate) == 1000020,
                "forward wrap lost one RTP cycle tick");
        require(clock.getNtpStamp(origin, rate) == 1000000,
                "late pre-wrap packet acquired a rounding offset");
        require(clock.getNtpStamp(uint32_t(origin + 2 * (rate / 50)), rate) == 1000040,
                "late pre-wrap packet moved the advancing clock anchor");
    }
}

void testRtpTimestampAnomalyBoundaries() {
    // Preserve the existing 3-second anomaly threshold and 60-second wrap
    // neighborhood; this patch only changes the arithmetic of accepted wraps.
    NtpStamp clock;
    clock.setNtpStamp(160000, 1000000);
    require(std::abs(int64_t(clock.getNtpStamp(160000 + 2800 * 16, 16000)) - 1002800) <= 1,
            "ordinary forward span below the anomaly threshold changed");
    clock.setNtpStamp(160000, 1000000);
    require(clock.getNtpStamp(160000 + 3200 * 16, 16000) == 1000000,
            "unexplained forward span above the anomaly threshold was accepted");
    clock.setNtpStamp(160000, 1000000);
    require(std::abs(int64_t(clock.getNtpStamp(160000 - 2800 * 16, 16000)) - 997200) <= 1,
            "ordinary backward presentation span changed");
    clock.setNtpStamp(160000, 1000000);
    require(clock.getNtpStamp(160000 - 3200 * 16, 16000) == 1000000,
            "unexplained backward span above the anomaly threshold was accepted");
    clock.setNtpStamp(0xffffff00U, 1000000);
    require(clock.getNtpStamp(960000, 16000) == 1000000,
            "wrap neighborhood was widened past its existing strict boundary");
}

void testNewSsrcKeepsAnAlreadyReceivedSenderReport() {
    ReceiverClock clock(TrackVideo, 90000);
    clock.receiver.setNtpStamp(123456, 1000000);
    clock.input(1000, 123456);
    // A WebRTC RID can retain its existing RtpTrack while the SSRC changes.
    // Its SR callback uses this upstream two-argument API. Keep sequence
    // numbers contiguous to isolate the mapping from restart-buffer behavior.
    std::this_thread::sleep_for(std::chrono::milliseconds(3100));
    clock.receiver.setNtpStamp(90000, 5000000);
    clock.inputPayload(1001, 90000, std::string(16, 'x'), true, 22);
    clock.inputPayload(1002, 93600, std::string(16, 'x'), true, 22);
    require(clock.receiver.getSSRC() == 22 && clock.packets[1]->ntp_stamp == 5000000
                && clock.packets[2]->ntp_stamp == 5000040,
            "accepted new SSRC discarded its already-received SR and bootstrapped the local clock");
}

struct Metadata {
    char kind;
    uint64_t relative_us;
    char track;
    uint16_t seq;
    uint32_t raw_stamp;
    size_t bytes;
    unsigned marker;
    size_t au_size;
    uint64_t ntp_ms;
};

std::map<std::string, std::vector<Metadata>> readMetadata() {
    auto source = std::string(__FILE__);
    auto path = source.substr(0, source.find_last_of("/\\") + 1) + "fixtures/rtp_sr_reanchor_metadata.txt";
    std::ifstream fixture(path);
    require(fixture.good(), "cannot open RTP/SR reanchor metadata fixture");
    std::map<std::string, std::vector<Metadata>> cases;
    std::string line;
    while (std::getline(fixture, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream row(line);
        std::string name;
        Metadata event;
        require(bool(row >> name >> event.kind >> event.relative_us >> event.track >> event.seq >> event.raw_stamp
                         >> event.bytes >> event.marker >> event.au_size >> event.ntp_ms),
                "invalid RTP/SR reanchor metadata row");
        require((event.kind == 'R' || event.kind == 'S') && (event.track == 'V' || event.track == 'A'),
                "invalid RTP/SR reanchor metadata event or track type");
        auto &events = cases[name];
        require(events.empty() || event.relative_us >= events.back().relative_us,
                "RTP/SR reanchor capture event order changed");
        events.emplace_back(event);
    }
    require(cases.size() == 4 && cases.at("publisher_video_sr_step").size() == 494
                && cases.at("publisher_aac_sr_step").size() == 318
                && cases.at("publisher_aac_sr_plateau").size() == 171
                && cases.at("source_av_sr_progress").size() == 602,
            "RTP/SR replay no longer covers all captured clock transitions");
    return cases;
}

struct ClockMetrics {
    size_t intervals = 0;
    size_t violations = 0;
    size_t backwards = 0;
    size_t collapsed = 0;
    double minimum = 0;
    double maximum = 0;
    double largest_error = 0;
    double expected_span = 0;
    double actual_span = 0;
    std::string worst;

    void add(double expected_ms, double actual_ms, uint16_t seq, double tolerance_ms = 1) {
        if (!intervals || actual_ms < minimum) {
            minimum = actual_ms;
        }
        if (!intervals || actual_ms > maximum) {
            maximum = actual_ms;
        }
        ++intervals;
        backwards += expected_ms >= 0 && actual_ms < 0;
        collapsed += expected_ms > 0 && actual_ms == 0;
        expected_span += expected_ms;
        actual_span += actual_ms;
        auto error = std::abs(actual_ms - expected_ms);
        violations += expected_ms == 0 ? actual_ms != 0 : error > tolerance_ms;
        if (error > largest_error) {
            largest_error = error;
            worst = "seq=" + std::to_string(seq) + ", expected_step_ms=" + std::to_string(expected_ms)
                + ", actual_step_ms=" + std::to_string(actual_ms);
        }
    }

    void report(const std::string &name) const {
        std::cout << "METRIC " << name << " intervals=" << intervals << " violations=" << violations
                  << " backwards=" << backwards << " collapsed=" << collapsed
                  << " min_step_ms=" << minimum << " max_step_ms=" << maximum
                  << " phase_change_ms=" << actual_span - expected_span;
        if (!worst.empty()) {
            std::cout << " worst={" << worst << '}';
        }
        std::cout << std::endl;
    }
};

void testCapturedReceiverClock(const std::string &name, const std::vector<Metadata> &events) {
    ReceiverClock video(TrackVideo, 90000);
    ReceiverClock audio(TrackAudio, 16000);
    ClockMetrics video_metrics, audio_metrics;
    bool video_seeded = false;
    bool audio_seeded = false;
    for (const auto &event : events) {
        auto &clock = event.track == 'V' ? video : audio;
        auto &seeded = event.track == 'V' ? video_seeded : audio_seeded;
        if (event.kind == 'S') {
            clock.receiver.setNtpStamp(event.raw_stamp, event.ntp_ms);
            seeded = true;
            continue;
        }
        require(seeded, "captured replay attempted an uncontrolled local-clock bootstrap");
        clock.input(event.seq, event.raw_stamp, event.bytes, event.marker);
        if (clock.packets.size() >= 2) {
            const auto &current = clock.packets.back();
            const auto &previous = clock.packets[clock.packets.size() - 2];
            auto &metrics = event.track == 'V' ? video_metrics : audio_metrics;
            metrics.add(int32_t(current->getStamp() - previous->getStamp()) * 1000.0 / clock.rate,
                        int64_t(current->ntp_stamp) - int64_t(previous->ntp_stamp), event.seq);
        }
    }
    require(video_seeded && audio_seeded && !video.packets.empty() && !audio.packets.empty(),
            "captured clock replay did not cover both media tracks");
    video_metrics.report(name + " receiver/video");
    audio_metrics.report(name + " receiver/audio");
    require(!video_metrics.violations && !audio_metrics.violations,
            "SR reanchor acceptance is unmet; see complete receiver interval metrics above");
}

void checkGeneratedSenderReports(const std::vector<RtpPacket::Ptr> &packets, ClockMetrics &metrics) {
    require(!packets.empty(), "muxer emitted no RTP for sender-report validation");
    RtcpContextForSend sender;
    uint64_t bytes = 0;
    uint32_t previous_rtp = 0;
    uint64_t previous_ntp = 0;
    size_t reports = 0;
    for (size_t i = 0; i < packets.size(); ++i) {
        const auto &packet = packets[i];
        const auto wire_bytes = packet->size() - RtpPacket::kRtpTcpHeaderSize;
        bytes += wire_bytes;
        sender.onRtp(packet->getSeq(), packet->getStamp(), packet->ntp_stamp, packet->sample_rate, wire_bytes);
        if (reports && i + 1 != packets.size()
            && uint32_t(packet->getStamp() - previous_rtp) < packet->sample_rate) {
            continue;
        }
        auto buffer = sender.createRtcpSR(packet->getSSRC());
        auto report = reinterpret_cast<const RtcpSR *>(buffer->data());
        const auto rtp_stamp = ntohl(report->rtpts);
        const auto ntp_stamp = uint64_t(ntohl(report->ntpmsw) - 0x83AA7E80) * 1000
            + uint64_t(ntohl(report->ntplsw)) * 1000 / (uint64_t(1) << 32);
        require(rtp_stamp == packet->getStamp() && ntohl(report->packet_count) == i + 1
                    && ntohl(report->octet_count) == bytes,
                "generated SR did not describe the actual emitted RTP and counters");
        require(std::abs(int64_t(ntp_stamp) - int64_t(packet->ntp_stamp)) <= 1,
                "generated SR NTP does not match the emitted packet clock");
        if (reports) {
            metrics.add(int32_t(rtp_stamp - previous_rtp) * 1000.0 / packet->sample_rate,
                        int64_t(ntp_stamp) - int64_t(previous_ntp), packet->getSeq(), 2);
        }
        previous_rtp = rtp_stamp;
        previous_ntp = ntp_stamp;
        ++reports;
    }
    require(reports >= 2, "muxer replay did not produce adjacent sender reports");
}

void testCapturedDecoderTrackMuxer(const std::string &name, const std::vector<Metadata> &events) {
    const bool aac = std::any_of(events.begin(), events.end(), [](const Metadata &event) {
        return event.track == 'A' && event.kind == 'R' && event.au_size != 0;
    });
    // Fixed HEVC test parameters from media-server's sdp-h265.c fixture;
    // synthesized FU payloads below contain no captured picture content.
    const std::string sdp =
        "v=0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H265/90000\r\n"
        "a=fmtp:96 sprop-vps=QAEMAf//AWAAAAMAsAAAAwAAAwBdFcCQ;"
        "sprop-sps=QgEBAWAAAAMAsAAAAwAAAwBdoAWiAFAWIFe5FlRA;sprop-pps=RAHALLwUyQ==\r\n";
    auto video_track = Factory::getTrackBySdp(SdpParser(sdp).getTrack(TrackVideo));
    Track::Ptr audio_track = aac ? std::static_pointer_cast<Track>(std::make_shared<AACTrack>(std::string("\x14\x08", 2)))
        : Factory::getTrackByCodecId(CodecG711A, 16000, 1, 16);
    require(video_track && video_track->ready() && audio_track && audio_track->ready(),
            "failed to construct ready clock-replay audio/video tracks");
    video_track->setIndex(0);
    audio_track->setIndex(1);
    RtspMuxer muxer;
    require(muxer.addTrack(video_track) && muxer.addTrack(audio_track), "failed to add clock-replay tracks to muxer");
    auto video_decoder = Factory::getRtpDecoderByCodecId(CodecH265);
    auto audio_decoder = Factory::getRtpDecoderByCodecId(aac ? CodecAAC : CodecG711A);
    audio_decoder->setAudioInfo(16000, 1);
    ReceiverClock video(TrackVideo, 90000);
    ReceiverClock audio(TrackAudio, 16000);
    std::vector<Frame::Ptr> video_frames;
    std::vector<Frame::Ptr> audio_frames;
    std::vector<Frame::Ptr> audio_track_frames;
    std::vector<RtpPacket::Ptr> video_packets;
    std::vector<RtpPacket::Ptr> audio_packets;
    muxer.getRtpRing()->setDelegate(std::make_shared<RingDelegateHelper>([&](RtpPacket::Ptr packet, bool) {
        (packet->type == TrackVideo ? video_packets : audio_packets).emplace_back(std::move(packet));
    }));
    video_track->addDelegate([&](const Frame::Ptr &frame) { return muxer.inputFrame(frame); });
    audio_track->addDelegate([&](const Frame::Ptr &frame) {
        audio_track_frames.emplace_back(Frame::getCacheAbleFrame(frame));
        return muxer.inputFrame(frame);
    });
    video_decoder->addDelegate([&](const Frame::Ptr &frame) {
        auto cached = Frame::getCacheAbleFrame(frame);
        cached->setIndex(0);
        video_frames.emplace_back(cached);
        return video_track->inputFrame(cached);
    });
    audio_decoder->addDelegate([&](const Frame::Ptr &frame) {
        auto cached = Frame::getCacheAbleFrame(frame);
        cached->setIndex(1);
        audio_frames.emplace_back(cached);
        return audio_track->inputFrame(cached);
    });
    video.on_packet = [&](const RtpPacket::Ptr &packet) { video_decoder->inputRtp(packet, false); };
    audio.on_packet = [&](const RtpPacket::Ptr &packet) { audio_decoder->inputRtp(packet, false); };
    bool have_video = false;
    uint32_t last_video_raw = 0;
    std::string current_video;
    std::vector<std::string> expected_video;
    std::vector<Metadata> video_frame_events;
    std::string expected_audio;
    std::vector<uint32_t> expected_audio_stamps;
    const Metadata *video_seed = nullptr;
    const Metadata *audio_seed = nullptr;
    for (const auto &event : events) {
        if (event.kind == 'S') {
            auto &seed = event.track == 'V' ? video_seed : audio_seed;
            if (!seed) {
                seed = &event;
            }
        }
    }
    require(video_seed && audio_seed, "full clock replay is missing an initial SR for one track");
    const auto common_start_us = std::max(video_seed->relative_us, audio_seed->relative_us);
    // Captured initial SRs can arrive more than two seconds apart. Seed both
    // clocks, then start media at their common window boundary: this test
    // covers steady dual-track playback, not an artificial single-track muxer
    // startup. The receiver-only replay retains every event in capture order,
    // and the separate late-first-SR test covers the bootstrap boundary.
    video.receiver.setNtpStamp(video_seed->raw_stamp, video_seed->ntp_ms);
    audio.receiver.setNtpStamp(audio_seed->raw_stamp, audio_seed->ntp_ms);
    for (const auto &event : events) {
        if (event.relative_us < common_start_us || &event == video_seed || &event == audio_seed) {
            continue;
        }
        auto &clock = event.track == 'V' ? video : audio;
        if (event.kind == 'S') {
            clock.receiver.setNtpStamp(event.raw_stamp, event.ntp_ms);
            continue;
        }
        std::string payload(event.bytes, '\0');
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(4 + (uint64_t(event.seq) + i) % 247);
        }
        if (event.track == 'V') {
            const auto start = !have_video || event.raw_stamp != last_video_raw;
            require(payload.size() >= 4, "HEVC fixture packet cannot contain a synthetic FU");
            payload[0] = 49 << 1;
            payload[1] = 1;
            payload[2] = 1 | (start ? 0x80 : 0) | (event.marker ? 0x40 : 0);
            if (start) {
                current_video.assign("\x02\x01", 2);
                payload[3] = static_cast<char>(0x80); // first_slice_segment_in_pic_flag
            }
            current_video.append(payload.data() + 3, payload.size() - 3);
            if (event.marker) {
                expected_video.emplace_back(current_video);
                video_frame_events.emplace_back(event);
            }
            have_video = true;
            last_video_raw = event.raw_stamp;
        } else if (aac) {
            require(event.au_size + 4 == payload.size(), "AAC fixture no longer contains one complete AU per packet");
            payload[0] = 0;
            payload[1] = 16;
            payload[2] = static_cast<char>(event.au_size >> 5);
            payload[3] = static_cast<char>((event.au_size & 31) << 3);
            expected_audio.append(payload.data() + 4, payload.size() - 4);
            expected_audio_stamps.emplace_back(event.raw_stamp);
        } else {
            expected_audio.append(payload);
        }
        clock.inputPayload(event.seq, event.raw_stamp, payload, event.marker);
    }
    video_decoder->flush();
    audio_decoder->flush();
    muxer.flush();
    require(video_frames.size() == expected_video.size(), "HEVC decoder lost or duplicated complete synthetic FUs");
    for (size_t i = 0; i < video_frames.size(); ++i) {
        const auto &frame = video_frames[i];
        require(std::string(frame->data() + frame->prefixSize(), frame->size() - frame->prefixSize()) == expected_video[i],
                "HEVC receiver/decoder altered the synthetic NAL payload");
    }
    require(!audio_frames.empty() && audio_frames.size() == audio_track_frames.size(),
            "audio track lost decoded frames during metadata/ADTS mapping");
    auto downstream_video = Factory::getRtpDecoderByCodecId(CodecH265);
    std::vector<Frame::Ptr> roundtrip_video;
    downstream_video->addDelegate([&](const Frame::Ptr &frame) {
        roundtrip_video.emplace_back(Frame::getCacheAbleFrame(frame));
        return true;
    });
    std::vector<uint32_t> emitted_video_stamps;
    for (const auto &packet : video_packets) {
        downstream_video->inputRtp(packet, false);
        if (packet->getHeader()->mark) {
            emitted_video_stamps.emplace_back(packet->getStamp());
        }
    }
    downstream_video->flush();
    require(roundtrip_video.size() == video_frames.size(), "HEVC track/muxer lost complete media timestamps");
    require(emitted_video_stamps.size() == video_frame_events.size(), "video RTP markers do not cover complete synthetic frames");
    ClockMetrics video_metrics, audio_metrics, video_ntp_metrics, audio_ntp_metrics;
    ClockMetrics video_sr_metrics, audio_sr_metrics;
    for (size_t i = 0; i < roundtrip_video.size(); ++i) {
        const auto &frame = roundtrip_video[i];
        require(std::string(frame->data() + frame->prefixSize(), frame->size() - frame->prefixSize()) == expected_video[i],
                "HEVC repacketization lost or reordered synthetic payload bytes");
        if (i) {
            const auto expected_ms = int32_t(video_frame_events[i].raw_stamp - video_frame_events[i - 1].raw_stamp) / 90.0;
            video_metrics.add(expected_ms, int32_t(emitted_video_stamps[i] - emitted_video_stamps[i - 1]) / 90.0,
                              video_frame_events[i].seq);
            video_ntp_metrics.add(expected_ms, int64_t(frame->pts()) - int64_t(roundtrip_video[i - 1]->pts()),
                                  video_frame_events[i].seq);
        }
    }
    std::string actual_audio;
    uint64_t emitted_samples = 0;
    for (size_t i = 0; i < audio_packets.size(); ++i) {
        const auto &packet = audio_packets[i];
        if (aac) {
            require(i < expected_audio_stamps.size(), "AAC muxer emitted an unexpected extra AU");
            actual_audio.append(reinterpret_cast<const char *>(packet->getPayload()) + 4, packet->getPayloadSize() - 4);
        } else {
            require(packet->getStamp() == uint32_t(audio_packets.front()->getStamp() + emitted_samples),
                    "source G711 repacketization lost its continuous normalized sample axis");
            actual_audio.append(reinterpret_cast<const char *>(packet->getPayload()), packet->getPayloadSize());
            emitted_samples += packet->getPayloadSize();
        }
        if (i) {
            const auto &previous = audio_packets[i - 1];
            const auto expected_ms = aac ? int32_t(expected_audio_stamps[i] - expected_audio_stamps[i - 1]) / 16.0
                : previous->getPayloadSize() / 16.0;
            audio_metrics.add(expected_ms, int32_t(packet->getStamp() - previous->getStamp()) / 16.0, packet->getSeq());
            audio_ntp_metrics.add(expected_ms, int64_t(packet->ntp_stamp) - int64_t(previous->ntp_stamp), packet->getSeq());
        }
    }
    require(actual_audio == expected_audio, "audio decoder/track/muxer lost, duplicated or reordered payload bytes");
    if (aac) {
        require(audio_packets.size() == expected_audio_stamps.size(), "AAC clock replay lost or duplicated complete AUs");
        for (const auto &frame : audio_track_frames) {
            require(frame->prefixSize() == 7, "AAC full-pipeline clock replay bypassed real ADTS-header insertion");
        }
    }
    checkGeneratedSenderReports(video_packets, video_sr_metrics);
    checkGeneratedSenderReports(audio_packets, audio_sr_metrics);
    video_metrics.report(name + " muxer/video RTP");
    audio_metrics.report(name + " muxer/audio RTP");
    video_ntp_metrics.report(name + " muxer/video NTP");
    audio_ntp_metrics.report(name + " muxer/audio NTP");
    video_sr_metrics.report(name + " generated/video SR");
    audio_sr_metrics.report(name + " generated/audio SR");
    require(!video_metrics.violations && !audio_metrics.violations && !video_ntp_metrics.violations
                && !audio_ntp_metrics.violations && !video_sr_metrics.violations && !audio_sr_metrics.violations,
            "SR reanchor acceptance is unmet; payloads were conserved, but emitted media/SR intervals differ from the source");
}

// A successful baseline must exercise the actual decoder/Track/muxer chain.
// This deliberately synthetic control keeps captured packet shapes and order,
// but replaces every later SR by a consistent source-clock observation. It is
// not the field capture and must never stand in for --verify-sr-reanchor.
void testConsistentFullPipeline() {
    auto events = readMetadata().at("publisher_aac_sr_step");
    std::map<char, Metadata> anchors;
    for (auto &event : events) {
        if (event.kind != 'S') {
            continue;
        }
        auto anchor = anchors.emplace(event.track, event).first->second;
        const auto rate = event.track == 'V' ? 90000 : 16000;
        event.ntp_ms = anchor.ntp_ms + int64_t(int32_t(event.raw_stamp - anchor.raw_stamp)) * 1000 / rate;
    }
    testCapturedDecoderTrackMuxer("synthetic consistent-SR control", events);
}

} // namespace

int main(int argc, char **argv) {
    const bool verify_sr = argc == 2 && std::string(argv[1]) == "--verify-sr-reanchor";
    const bool verify_wrap = argc == 2 && std::string(argv[1]) == "--verify-rtp-wrap";
    if (argc > 1 && !verify_sr && !verify_wrap) {
        std::cout << "Usage: " << argv[0] << " [--verify-sr-reanchor | --verify-rtp-wrap]\n"
                  << "Default: upstream clock compatibility and a synthetic consistent-SR full pipeline.\n"
                  << "--verify-sr-reanchor: unchanged field RTP/SR replay through receiver and real codecs/muxer;\n"
                  << "returns failure while the separately tracked SR reanchor issue remains unresolved.\n"
                  << "--verify-rtp-wrap: strict timestamp-wrap precision and boundary checks.\n";
        return argc == 2 && std::string(argv[1]) == "--help" ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    unsigned failures = 0;
    auto run = [&](const std::string &name, const std::function<void()> &test) {
        try {
            test();
            std::cout << "PASS " << name << std::endl;
        } catch (const std::exception &ex) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << ex.what() << std::endl;
        }
    };
    if (verify_sr) {
        std::cout << "Explicit SR reanchor acceptance; excluded from the default baseline.\n"
                  << "This command is expected to fail on the minimal G711/AAC candidate.\n";
        try {
            for (const auto &entry : readMetadata()) {
                run(entry.first + " receiver", [&] { testCapturedReceiverClock(entry.first, entry.second); });
                run(entry.first + " decoder/track/muxer/SR", [&] { testCapturedDecoderTrackMuxer(entry.first, entry.second); });
            }
        } catch (const std::exception &ex) {
            ++failures;
            std::cerr << "FAIL field fixture: " << ex.what() << std::endl;
        }
    } else if (verify_wrap) {
        std::cout << "Explicit RTP timestamp wrap precision acceptance." << std::endl;
        run("RTP timestamp wrap retains its exact media span", testRtpTimestampWrapPrecision);
        run("late pre-wrap packet retains its original time", testRtpTimestampWrapReordering);
        run("timestamp anomaly thresholds remain unchanged", testRtpTimestampAnomalyBoundaries);
    } else {
        run("consistent SR media progression", testConsistentSenderReportsKeepMediaIntervals);
        run("later SR remains authoritative", testSenderReportsRemainAuthoritative);
        run("late first SR and clear with a new source", testLateFirstSenderReportAndClear);
        run("valid SR reenables disabled NTP", testSenderReportReenablesNtp);
        run("confirmed same-SSRC sequence restart uses its SR", testConfirmedSequenceRestartUsesItsSenderReport);
        run("raw-clock restart accepts SR before or after RTP", testRestartReportBeforeAndAfterFirstPacket);
        run("presentation reordering and RTP sequence wrap", testPresentationReorderingAndSequenceWrap);
        run("new SSRC retains its preceding SR through the upstream API", testNewSsrcKeepsAnAlreadyReceivedSenderReport);
        run("synthetic consistent-SR decoder/track/muxer", testConsistentFullPipeline);
        std::cout << "SR reanchor acceptance was NOT run; use --verify-sr-reanchor for the unchanged field capture." << std::endl;
        run("RTP timestamp wrap retains its exact media span", testRtpTimestampWrapPrecision);
        run("late pre-wrap packet retains its original time", testRtpTimestampWrapReordering);
        run("timestamp anomaly thresholds remain unchanged", testRtpTimestampAnomalyBoundaries);
    }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
