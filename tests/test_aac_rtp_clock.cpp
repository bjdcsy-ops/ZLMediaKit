/*
 * Copyright (c) 2016-present The ZLMediaKit project authors.
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Extension/Factory.h"
#include "Rtcp/RtcpContext.h"
#include "Rtsp/RtspMuxer.h"
#include "Rtsp/RtspDemuxer.h"
#include "ext-codec/AACRtp.h"

using namespace mediakit;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string liveFmtp(const std::string &config) {
    const char *hex = "0123456789abcdef";
    std::string result = "streamtype=5;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=";
    for (unsigned char ch : config) {
        result += hex[ch >> 4];
        result += hex[ch & 15];
    }
    return result;
}

void configureClock(AACRtpDecoder &decoder, const std::string &fmtp) {
    toolkit::Any option;
    option.set<std::string>(fmtp);
    decoder.setOpt(RtpCodec::RTP_DECODER_AAC_LIVE_FMTP, option);
}

std::string aacPayload(const std::string &au) {
    std::string payload("\0\20", 2);
    payload.push_back(static_cast<char>(au.size() >> 5));
    payload.push_back(static_cast<char>(au.size() << 3));
    return payload + au;
}

void requirePayload(const RtpPacket::Ptr &packet, const std::string &au) {
    const auto size = packet->getPayloadSize();
    const auto payload = packet->getPayload();
    require(size == au.size() + 4, "AAC payload size changed");
    require(payload[0] == 0 && payload[1] == 16
            && static_cast<size_t>((payload[2] << 5) | (payload[3] >> 3)) == au.size(),
        "AAC output AU header is invalid");
    require(std::string(reinterpret_cast<const char *>(payload) + 4, size - 4) == au,
        "AAC output payload bytes changed");
}

void requireSenderReport(const RtpPacket::Ptr &packet) {
    RtcpContextForSend sender;
    sender.onRtp(packet->getSeq(), packet->getStamp(), packet->ntp_stamp, packet->sample_rate, packet->size());
    auto buffer = sender.createRtcpSR(packet->getSSRC());
    auto reports = RtcpHeader::loadFromBytes(buffer->data(), buffer->size());
    require(reports.size() == 1 && reports.front()->pt == static_cast<uint8_t>(RtcpType::RTCP_SR),
        "could not build AAC sender report");
    auto sr = static_cast<RtcpSR *>(reports.front());
    require(sr->rtpts == packet->getStamp() && sr->getNtpUnixStampMS() != 0
            && std::abs(int64_t(sr->getNtpUnixStampMS()) - int64_t(packet->ntp_stamp)) <= 1,
        "sender report changed the output RTP/NTP clock pair");
}

// Exercise the real decoder -> AACTrack (ADTS/cache) -> RtspMuxer path.
// The output clock uses ordinary Frame DTS; source RTP has an independent origin.
struct Pipeline {
    Pipeline(uint32_t rate, const std::string &config, float duration = 0)
        : source(0x12345678, 4096, rate, 97, 2, 1), track(std::make_shared<AACTrack>(config)),
          muxer(std::make_shared<TitleSdp>(duration)) {
        track->setIndex(1);
        require(track->getAudioSampleRate() == static_cast<int>(rate), "invalid AAC test config");
        require(muxer.addTrack(track), "failed to add AAC track");
        muxer.getRtpRing()->setDelegate(std::make_shared<RingDelegateHelper>(
            [&](RtpPacket::Ptr packet, bool) {
                (packet->type == TrackAudio ? packets : video_packets).emplace_back(std::move(packet));
            }));
        track->addDelegate([&](const Frame::Ptr &frame) {
            auto cached = Frame::getCacheAbleFrame(frame);
            frames.emplace_back(cached);
            return muxer.inputFrame(cached);
        });
        clock_fmtp = duration ? "" : liveFmtp(config);
        attach(decoder);
    }

    void attach(AACRtpDecoder &input) {
        configureClock(input, clock_fmtp);
        input.addDelegate([&](const Frame::Ptr &frame) {
            auto cached = Frame::getCacheAbleFrame(frame);
            cached->setIndex(track->getIndex());
            return track->inputFrame(cached);
        });
    }

    void input(uint32_t raw_stamp, uint64_t dts, const std::string &au) {
        auto payload = aacPayload(au);
        decoder.inputRtp(source.makeRtpWithStamp(TrackAudio, payload.data(), payload.size(), true, dts, raw_stamp));
    }

    void addVideo() {
        SdpParser sdp(
            "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=AAC VOD regression\r\nt=0 0\r\n"
            "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z2QAH6zZQFAFuhAAAAMAEAAAAwDxgxHg,aM4G4g==\r\n"
            "a=control:trackID=0\r\n");
        auto video = Factory::getTrackBySdp(sdp.getTrack(TrackVideo));
        require(video && video->ready(), "failed to create ready video track");
        video->setIndex(0);
        require(muxer.addTrack(video), "failed to add video track");
    }

    void inputVideo(uint64_t dts) {
        auto frame = FrameImp::create();
        frame->_codec_id = CodecH264;
        frame->_dts = frame->_pts = dts;
        frame->_buffer.assign("\x41\x80\x01\x02", 4);
        frame->setIndex(0);
        muxer.inputFrame(frame);
    }

    RtpInfo source;
    AACRtpDecoder decoder;
    AACTrack::Ptr track;
    RtspMuxer muxer;
    std::vector<Frame::Ptr> frames;
    std::vector<RtpPacket::Ptr> packets;
    std::vector<RtpPacket::Ptr> video_packets;
    std::string clock_fmtp;
};

void testDtsQuantization(uint32_t rate, const std::string &config) {
    Pipeline pipeline(rate, config);
    constexpr uint64_t start_ms = 1000000;
    constexpr size_t count = 2000;
    bool saw_quantized_step = false;
    for (size_t i = 0; i < count; ++i) {
        const auto samples = uint64_t(i) * 1024;
        const auto dts = start_ms + samples * 1000 / rate;
        const auto au = "au-" + std::to_string(i);
        // The source wraps independently of the output's DTS-derived origin.
        pipeline.input(0xffff0000u + static_cast<uint32_t>(samples), dts, au);
        require(pipeline.frames.size() == i + 1 && pipeline.packets.size() == i + 1,
            "a complete AAC AU was delayed, dropped or duplicated");
        const auto &frame = pipeline.frames.back();
        const auto &packet = pipeline.packets.back();
        require(frame->dts() == dts && frame->prefixSize() == ADTS_HEADER_LEN,
            "AACTrack changed DTS or failed to add ADTS");
        require(packet->getStamp() == static_cast<uint32_t>(dts * rate / 1000),
            "AAC output no longer follows ordinary Frame DTS");
        requirePayload(packet, au);
        const auto output_samples = uint32_t(packet->getStamp() - pipeline.packets.front()->getStamp());
        require(samples >= output_samples && samples - output_samples <= (rate + 999) / 1000,
            "AAC millisecond quantization accumulated beyond one millisecond");
        if (i) {
            const auto step = uint32_t(packet->getStamp() - pipeline.packets[i - 1]->getStamp());
            require(step > 0, "AAC quantization caused a non-increasing timestamp");
            saw_quantized_step |= step != 1024;
        }
    }
    require(saw_quantized_step == (rate != 16000), "test did not exercise expected millisecond quantization");
    const auto elapsed_ms = uint64_t(count - 1) * 1024 * 1000 / rate;
    require(pipeline.packets.back()->ntp_stamp - pipeline.packets.front()->ntp_stamp == elapsed_ms,
        "AAC sender clock drifted from the Frame DTS interval");
    requireSenderReport(pipeline.packets.back());
}

void testRetainedMuxerReconnect(bool reuse_ssrc) {
    Pipeline pipeline(16000, std::string("\x14\x08", 2));
    pipeline.input(160000000, 1000, "old-source");

    // Same track/encoder/muxer, fresh input decoder: PlayerProxy can retain
    // these output objects when a compatible source reconnects.
    AACRtpDecoder replacement_decoder;
    pipeline.attach(replacement_decoder);
    RtpInfo replacement(reuse_ssrc ? 0x12345678 : 0x87654321, 4096, 16000, 97, 2, 1);
    auto payload = aacPayload("new-source");
    replacement_decoder.inputRtp(replacement.makeRtpWithStamp(
        TrackAudio, payload.data(), payload.size(), true, 1128, 3000000000u));

    require(pipeline.packets.size() == 2, "AAC reconnect lost or delayed an AU");
    const auto &first = pipeline.packets[0];
    const auto &second = pipeline.packets[1];
    require(first->getSSRC() == second->getSSRC() && uint16_t(second->getSeq() - first->getSeq()) == 1,
        "test did not retain the output SSRC and sequence");
    require(first->getStamp() == 16000 && second->getStamp() == 18048
            && second->ntp_stamp - first->ntp_stamp == 128,
        "new source RTP origin contaminated the retained output clock");
    requirePayload(first, "old-source");
    requirePayload(second, "new-source");
    requireSenderReport(second);
}

void testVodUsesDtsPosition() {
    // The second value is an epoch timestamp for which legacy 16kHz/90kHz
    // modulo conversion both yield 1000ms. Avoid mistaking the pre-existing
    // VOD epoch/wrap limitation for the source-raw-origin regression.
    for (auto dts : { uint64_t(1000), uint64_t(833) * (uint64_t(1) << 31) + 1000 }) {
        Pipeline pipeline(16000, std::string("\x14\x08", 2), 60);
        pipeline.addVideo();
        pipeline.input(160000000, dts, "vod-au");
        pipeline.inputVideo(dts);
        pipeline.muxer.flush();
        require(pipeline.packets.size() == 1 && pipeline.video_packets.size() == 1,
            "VOD test did not produce both audio and video");
        const auto &audio = pipeline.packets.front();
        const auto &video = pipeline.video_packets.front();
        // RtspSession also uses getStampMS(false) for PLAY Range/NPT.
        require(audio->getStampMS(false) == 1000 && video->getStampMS(false) == 1000,
            "VOD RTP position inherited the source's random AAC clock origin");
        require(audio->ntp_stamp == video->ntp_stamp,
            "equal-DTS VOD tracks no longer share the same sender-report time");
        requirePayload(audio, "vod-au");
        requireSenderReport(audio);
    }
}

void testFirstZeroTimestampHasSenderClock() {
    Pipeline pipeline(16000, std::string("\x14\x08", 2));
    // A nonzero millisecond DTS that wraps to RTP zero at 16kHz. Keep this
    // separate from the pre-existing DeltaStamp DTS-zero sentinel behavior.
    constexpr uint64_t first_dts = uint64_t(1) << 28;
    pipeline.input(0, first_dts, "first-au");
    require(pipeline.packets.size() == 1, "first zero-timestamp AAC AU was not emitted");
    const auto first = pipeline.packets.front();
    require(first->getStamp() == 0 && first->ntp_stamp != 0,
        "legal first RTP timestamp zero skipped muxer NTP initialization");
    requireSenderReport(first);
    pipeline.input(1024, first_dts + 64, "second-au");
    require(pipeline.packets.size() == 2 && pipeline.packets.back()->getStamp() == 1024
            && pipeline.packets.back()->ntp_stamp - first->ntp_stamp == 64,
        "muxer initialized one AU late after a zero RTP timestamp");
}

void testAdtsCompatibility() {
    Pipeline pipeline(16000, std::string("\x14\x08", 2));
    const std::string adts("\xff\xf1\x60\x40\x01\x3f\xfc\x11\x22", 9);
    pipeline.input(123, 1000, adts);
    pipeline.input(0xfffffc00u, 1064, adts + adts);
    require(pipeline.frames.size() == 3 && pipeline.packets.size() == 3,
        "ADTS compatibility path did not split two complete AUs");
    for (size_t i = 0; i < pipeline.packets.size(); ++i) {
        require(pipeline.frames[i]->dts() == 1000 + i * 64
                && pipeline.packets[i]->getStamp() == 16000 + i * 1024,
            "ADTS splitting changed the ordinary DTS contract");
        requirePayload(pipeline.packets[i], std::string("\x11\x22", 2));
    }
}

void testOrdinaryFrameAndStampOverride() {
    Pipeline pipeline(16000, std::string("\x14\x08", 2));
    auto ordinary = FrameImp::create();
    ordinary->_codec_id = CodecAAC;
    ordinary->_buffer.assign("plain-au");
    ordinary->_dts = 1200;
    ordinary->setIndex(1);
    pipeline.track->inputFrame(ordinary);
    require(pipeline.packets.size() == 1 && pipeline.packets.back()->getStamp() == 19200,
        "ordinary AAC Frame no longer uses DTS");

    auto overridden = std::make_shared<FrameStamp>(pipeline.frames.front());
    overridden->setStamp(5000, 5000);
    pipeline.muxer.inputFrame(Frame::getCacheAbleFrame(overridden));
    require(pipeline.packets.size() == 2 && pipeline.packets.back()->getStamp() == 80000,
        "AAC output bypassed explicit FrameStamp");
    require(pipeline.frames.front()->dts() == 1200, "FrameStamp mutated the cached source frame");
    requirePayload(pipeline.packets.back(), "plain-au");
}

struct DecoderCapture {
    explicit DecoderCapture(uint32_t rate = 64000, const std::string &fmtp = liveFmtp(std::string("\x11\x08", 2)))
        : source(0x12345678, 8192, rate, 97, 2, 1) {
        configureClock(decoder, fmtp);
        decoder.addDelegate([&](const Frame::Ptr &frame) {
            stamps.emplace_back(frame->dts());
            last_payload.assign(frame->data(), frame->size());
            return true;
        });
    }

    void packet(uint16_t seq, uint32_t stamp, uint64_t ntp, const std::string &payload, bool marker = true,
                uint32_t ssrc = 0x12345678, uint32_t rate = 0) {
        auto rtp = source.makeRtpWithStamp(TrackAudio, payload.data(), payload.size(), marker, ntp, stamp);
        rtp->getHeader()->seq = htons(seq);
        rtp->getHeader()->ssrc = htonl(ssrc);
        if (rate) {
            rtp->sample_rate = rate;
        }
        decoder.inputRtp(rtp);
    }

    void au(uint16_t seq, uint32_t stamp, uint64_t ntp, const std::string &payload = "audio",
            uint32_t ssrc = 0x12345678) {
        packet(seq, stamp, ntp, aacPayload(payload), true, ssrc);
        require(last_payload == payload, "AAC normalization changed AU payload");
    }

    RtpInfo source;
    AACRtpDecoder decoder;
    std::vector<uint64_t> stamps;
    std::string last_payload;
};

std::string aggregate(const std::vector<std::pair<std::string, uint8_t>> &aus) {
    auto bits = aus.size() * 16;
    std::string result;
    result += char(bits >> 8);
    result += char(bits);
    for (const auto &au : aus) {
        auto header = (au.first.size() << 3) | au.second;
        result += char(header >> 8);
        result += char(header);
    }
    for (const auto &au : aus) {
        result += au.first;
    }
    return result;
}

void testBatchQualification() {
    const auto valid = liveFmtp(std::string("\x11\x08", 2));
    auto changed = [&](const std::string &from, const std::string &to) {
        auto fmtp = valid;
        fmtp.replace(fmtp.find(from), from.size(), to);
        return fmtp;
    };
    const std::vector<std::string> rejected = {
        "", liveFmtp(std::string("\x11\x0c", 2)), // 960 samples
        liveFmtp(std::string("\x2b\x92\x08", 3)), // HE-AAC
        liveFmtp(std::string("\x11\x08\x56\xe5\x00", 5)), // extended LC ASC
        changed("mode=AAC-hbr", "mode=AAC-lbr"), changed("sizelength=13", "sizelength=12"),
        changed("indexlength=3", "indexlength=0"), changed("indexdeltalength=3", "indexdeltalength=0"),
        valid + ";constantduration=960", valid + ";ctsdeltalength=1", valid + ";maxdisplacement=1",
        changed("config=1108", "config=zzzz"), changed("config=1108", "config=1788"),
        valid + ";mode=AAC-lbr" // Conflicting duplicate declarations remain unqualified.
    };
    for (const auto &fmtp : rejected) {
        DecoderCapture capture(64000, fmtp);
        capture.au(0, 1000, 1000);
        capture.au(1, 1000, 1000);
        require(capture.stamps == std::vector<uint64_t>({ 1000, 1000 }),
            "unknown/unsupported AAC configuration was normalized: " + fmtp);
    }
    DecoderCapture mismatch;
    mismatch.packet(0, 0, 1000, aacPayload("a"), true, 0x12345678, 48000);
    mismatch.packet(1, 0, 1000, aacPayload("b"), true, 0x12345678, 48000);
    require(mismatch.stamps == std::vector<uint64_t>({ 1000, 1000 }), "ASC/RTP rate mismatch was normalized");
    mismatch.au(2, 0, 1000);
    mismatch.au(3, 0, 1000);
    mismatch.packet(4, 0, 1000, aacPayload("mismatch"), true, 0x12345678, 48000);
    mismatch.au(5, 0, 1000);
    require(mismatch.stamps[3] == 1016 && mismatch.stamps[4] == 1000 && mismatch.stamps[5] == 1000,
        "mid-batch rate mismatch retained the old sample axis");
}

void testBatchActivationAndSr() {
    DecoderCapture capture;
    capture.au(0, 0, 1000);
    capture.au(1, 2560, 1040); // Forward this normal positive step unchanged.
    capture.au(2, 2560, 1040, std::string(300, 'b'));
    capture.au(3, 2560, 1040, "c");
    capture.au(4, 5120, 1080);
    require(capture.stamps == std::vector<uint64_t>({ 1000, 1040, 1056, 1072, 1088 }),
        "batch activation moved behind an emitted AU or reanchored at the next batch");
    // A source SR mapping change must remain visible: the AAC fix only removes
    // raw RTP phase and must not silently mask the separate SR problem.
    capture.au(5, 5120, 800);
    require(capture.stamps.back() == 824, "AAC batch correction swallowed an SR/NTP mapping change");

    Pipeline pipeline(64000, std::string("\x11\x08", 2));
    for (uint32_t i = 0; i != 6; ++i) {
        auto raw = i < 3 ? 0 : 2560;
        pipeline.input(raw, 1000 + raw / 64, "batch-au-" + std::to_string(i));
        require(pipeline.packets.size() == i + 1 && pipeline.frames.size() == i + 1,
            "a complete batch AU waited for the next RTP packet");
        require(pipeline.frames.back()->dts() == 1000 + i * 16
                && pipeline.packets.back()->getStamp() == 64000 + i * 1024,
            "AAC Track/muxer changed the normalized sample progression");
        requirePayload(pipeline.packets.back(), "batch-au-" + std::to_string(i));
    }
    requireSenderReport(pipeline.packets.back());
}

void testBatchAggregateAndFragments() {
    DecoderCapture capture;
    capture.au(0, 0, 1000);
    capture.au(1, 0, 1000);
    capture.packet(2, 1024, 1016, aggregate({ { "first", 0 }, { "after-gap", 2 } }));
    require(capture.stamps == std::vector<uint64_t>({ 1000, 1016, 1032, 1080 }),
        "AAC normalization discarded an aggregate AU-index gap");
    require(capture.last_payload == "after-gap", "aggregated AU payload changed");

    DecoderCapture ordinary;
    ordinary.packet(0, 0, 1000, aggregate({ { "first", 0 }, { "second", 0 } }));
    ordinary.au(1, 1024, 1016);
    require(ordinary.stamps == std::vector<uint64_t>({ 1000, 1016, 1032 }),
        "aggregate completion could not seed a later cross-packet batch");

    DecoderCapture fragments;
    fragments.au(0, 0, 1000);
    auto full = aacPayload("fragmented");
    fragments.packet(1, 0, 1000, full.substr(0, 7), false);
    require(fragments.stamps.size() == 1, "AAC first fragment advanced the complete-AU clock");
    fragments.packet(2, 0, 7000, full.substr(0, 4) + full.substr(7));
    require(fragments.stamps == std::vector<uint64_t>({ 1000, 1016 })
            && fragments.last_payload == "fragmented", "AAC fragment completion lost its first-fragment mapping");
    fragments.au(3, 0, 7000);
    require(fragments.stamps.back() == 7032, "later complete AU failed to retain the changed SR mapping");
}

void testBatchLossResetAndWrap() {
    DecoderCapture capture;
    capture.au(65534, 0xfffffc00u, 1000);
    capture.au(65535, 0xfffffc00u, 1000);
    capture.au(0, 0, 1016);
    require(capture.stamps == std::vector<uint64_t>({ 1000, 1016, 1032 }), "AAC sequence/RTP wrap reset a valid batch");
    capture.au(0, 0, 1016); // Duplicate: payload guard still sees the last complete AU.
    require(capture.stamps.size() == 3, "duplicate RTP advanced the batch clock");
    capture.au(2, 4096, 1080); // A missing packet may carry an unknown number of AUs.
    require(capture.stamps.back() == 1080, "AAC loss compressed the source gap");
    capture.au(3, 4096, 1080);
    capture.au(4, 900000, 15000, "new-ssrc", 0x87654321);
    require(capture.stamps.back() == 15000, "AAC SSRC change retained the old sample origin");
    capture.au(5, 900000, 15000, "new-ssrc-2", 0x87654321);
    capture.au(20, 10, 300, "same-ssrc-restart", 0x87654321);
    require(capture.stamps.back() == 300, "same-SSRC restart was rejected or forced onto the old timeline");

    DecoderCapture truncated;
    truncated.au(0, 0, 1000);
    auto full = aacPayload("fragmented");
    truncated.packet(1, 0, 1000, full.substr(0, 7), false);
    truncated.packet(3, 0, 1000, full.substr(0, 4) + full.substr(7));
    require(truncated.stamps.size() == 1, "a missing AAC fragment emitted a partial AU");
    truncated.au(4, 4096, 1064);
    require(truncated.stamps.back() == 1064, "AAC missing-fragment recovery retained batch correction");

    DecoderCapture reconfigured;
    reconfigured.au(0, 0, 1000);
    reconfigured.packet(1, 0, 1000, full.substr(0, 7), false);
    configureClock(reconfigured.decoder, liveFmtp(std::string("\x11\x08", 2)));
    reconfigured.packet(2, 0, 1000, full.substr(0, 4) + full.substr(7));
    require(reconfigured.stamps.size() == 1, "AAC SDP reconfiguration retained an old partial AU");
    reconfigured.au(3, 4096, 1064);
    reconfigured.au(4, 4096, 1064);
    require(reconfigured.stamps.back() == 1080, "AAC configuration reset prevented a fresh valid batch");
    configureClock(reconfigured.decoder, liveFmtp(std::string("\x11\x0c", 2)));
    reconfigured.au(5, 4096, 1064);
    require(reconfigured.stamps.back() == 1064, "AAC format change retained a qualified 1024-sample clock");
}

void testBatchBoundsAndAdts() {
    DecoderCapture frozen;
    for (unsigned i = 0; i != 10; ++i) {
        frozen.au(i, 0, 1000);
    }
    require(frozen.stamps[5] == 1080 && frozen.stamps[6] == 1000 && frozen.stamps[9] == 1000,
        "AAC frozen raw clock was normalized beyond 80 ms or repeatedly reanchored");
    frozen.au(10, 64000, 2000);
    frozen.au(11, 64000, 2000);
    require(frozen.stamps.back() == 2016, "AAC clock did not recover after the source timestamp advanced");

    DecoderCapture jump;
    jump.au(0, 0, 1000);
    jump.au(1, 0, 1000);
    jump.au(2, 640000, 11000);
    require(jump.stamps.back() == 11000, "AAC real positive discontinuity was compressed");
    jump.au(3, 640000, 11000);
    jump.au(4, 10000, 1156);
    require(jump.stamps.back() == 1156, "AAC real backward discontinuity was hidden");

    DecoderCapture adts;
    const std::string one("\xff\xf1\x60\x40\x01\x3f\xfc\x11\x22", 9);
    adts.au(0, 0, 1000, one);
    adts.au(1, 0, 1000, one + one);
    require(adts.stamps == std::vector<uint64_t>({ 1000, 1000 }), "ADTS multi-frame RTP was treated as one 1024-sample AU");
}

void testDemuxerRangeAndOriginalAsc() {
    const auto fmtp = liveFmtp(std::string("\x11\x08", 2));
    auto sdp = [](const std::string &session_range, const std::string &media_range, const std::string &original_fmtp) {
        return "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=AAC range test\r\nt=0 0\r\n"
            + session_range + "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 MPEG4-GENERIC/64000/1\r\n"
            + "a=fmtp:97 " + original_fmtp + "\r\na=control:trackID=1\r\n" + media_range;
    };
    auto check = [&](const std::string &session_range, const std::string &media_range,
                     const std::string &original_fmtp, bool normalize) {
        RtspDemuxer demuxer;
        demuxer.loadSdp(sdp(session_range, media_range, original_fmtp));
        auto tracks = demuxer.getTracks(false);
        require(tracks.size() == 1, "AAC range test failed to build its audio track");
        std::vector<uint64_t> stamps;
        tracks.front()->addDelegate([&](const Frame::Ptr &frame) {
            stamps.emplace_back(frame->dts());
            return true;
        });
        RtpInfo source(0x12345678, 4096, 64000, 97, 2, 1);
        auto payload = aacPayload("range-au");
        for (auto dts : { uint64_t(1000), uint64_t(1000), uint64_t(400) }) {
            demuxer.inputRtp(source.makeRtpWithStamp(TrackAudio, payload.data(), payload.size(), true, dts, 0));
        }
        require(stamps.size() == 3 && stamps[1] == (normalize ? 1016 : 1000)
                && stamps[2] == (normalize ? 432 : 400),
            "AAC original ASC or VOD Range gate changed timestamp/seek semantics");
    };
    check("", "", fmtp, true);
    check("a=range:npt=now-\r\n", "", fmtp, true);
    check("a=range:npt=0.000-\r\n", "", fmtp, true);
    check("a=range:npt=0-60\r\n", "", fmtp, false);
    check("a=range:npt=5-\r\n", "", fmtp, false);
    check("a=range:clock=20260908T000000Z-\r\n", "", fmtp, false);
    check("a=range:npt=now-\r\n", "a=range:npt=0-60\r\n", fmtp, false);
    check("a=range:unknown=0-\r\n", "", fmtp, false);
    check("a=range:npt=now-\r\n", "", liveFmtp(std::string("\x11\x08\x56\xe5\x00", 5)), false);
    check("", "", " Mode = aac-HBR ; SizeLength = 13 ; IndexLength = 3 ; IndexDeltaLength = 3 ; config=1108;", true);

    RtspDemuxer reused;
    reused.loadSdp(sdp("a=range:npt=now-\r\n", "", fmtp));
    std::vector<uint64_t> stamps;
    reused.getTracks(false).front()->addDelegate([&](const Frame::Ptr &frame) {
        stamps.emplace_back(frame->dts());
        return true;
    });
    RtpInfo source(0x12345678, 4096, 64000, 97, 2, 1);
    auto payload = aacPayload("reload-au");
    for (int i = 0; i != 2; ++i) {
        reused.inputRtp(source.makeRtpWithStamp(TrackAudio, payload.data(), payload.size(), true, 1000, 0));
    }
    require(stamps.back() == 1016, "SDP reload test did not activate batch normalization");
    reused.loadSdp(sdp("a=range:npt=0-60\r\n", "", fmtp));
    for (int i = 0; i != 2; ++i) {
        reused.inputRtp(source.makeRtpWithStamp(TrackAudio, payload.data(), payload.size(), true, 400, 0));
    }
    require(stamps == std::vector<uint64_t>({ 1000, 1016, 400, 400 }),
        "live-to-VOD SDP reload retained normalization across a seek boundary");
}

void testCapturedBatchAllJoinPositions() {
    struct Row { uint16_t seq; uint32_t stamp; size_t size; };
    std::vector<Row> windows[2];
    auto source = std::string(__FILE__);
    std::ifstream fixture(source.substr(0, source.find_last_of("/\\") + 1) + "fixtures/aac_64k_batch_metadata.txt");
    require(fixture.good(), "cannot open AAC batch metadata fixture");
    for (std::string line; std::getline(fixture, line);) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream input(line);
        unsigned window, seq;
        Row row;
        require(bool(input >> window >> seq >> row.stamp >> row.size) && window < 2 && seq <= 65535,
            "invalid AAC batch fixture row");
        row.seq = static_cast<uint16_t>(seq);
        windows[window].emplace_back(row);
    }
    size_t inputs = 0;
    for (const auto &rows : windows) {
        require(rows.size() == 2626, "AAC captured window lost complete AUs");
        for (size_t join = 0; join < rows.size(); ++join) {
            DecoderCapture capture;
            bool active = false;
            for (size_t i = join; i < rows.size(); ++i) {
                const auto &row = rows[i];
                std::string payload(row.size, char('a' + i % 23));
                capture.au(row.seq, row.stamp, 1000000 + uint32_t(row.stamp - rows[join].stamp) / 64, payload);
                require(capture.stamps.size() == i - join + 1, "AAC replay delayed, duplicated or lost an AU");
                if (i != join) {
                    active |= row.stamp == rows[i - 1].stamp;
                    auto step = int64_t(capture.stamps.back()) - int64_t(capture.stamps[capture.stamps.size() - 2]);
                    require(step > 0 && (!active || step == 16), "AAC captured batch failed at an input join position");
                }
                ++inputs;
            }
        }
    }
    require(inputs == 6898502, "AAC replay did not exercise all 5252 join positions");
}

void testVideoOnlyVodRange() {
    const std::string session = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=Video VOD range\r\nt=0 0\r\n";
    const std::string audio = "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 MPEG4-GENERIC/64000/1\r\na=fmtp:97 "
        + liveFmtp(std::string("\x11\x08", 2)) + "\r\na=control:trackID=1\r\n";
    const std::string video = "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z2QAH6zZQFAFuhAAAAMAEAAAAwDxgxHg,aM4G4g==\r\n"
        "a=control:trackID=0\r\na=range:npt=0-60\r\n";
    for (bool reload : { false, true }) {
        for (bool video_first : { false, true }) {
            auto vod_sdp = session + (video_first ? video + audio : audio + video);
            RtspDemuxer demuxer;
            demuxer.loadSdp(reload ? session + "a=range:npt=now-\r\n" + audio : vod_sdp);
            auto tracks = demuxer.getTracks(false);
            auto track = std::find_if(tracks.begin(), tracks.end(), [](const Track::Ptr &item) {
                return item->getTrackType() == TrackAudio;
            });
            require(track != tracks.end(), "video-only VOD range test lost its audio track");
            std::vector<uint64_t> stamps;
            (*track)->addDelegate([&](const Frame::Ptr &frame) {
                stamps.emplace_back(frame->dts());
                return true;
            });
            RtpInfo source(0x12345678, 4096, 64000, 97, 2, 1);
            auto payload = aacPayload("video-range-au");
            auto input_pair = [&](uint64_t dts) {
                for (int i = 0; i != 2; ++i) {
                    demuxer.inputRtp(source.makeRtpWithStamp(TrackAudio, payload.data(), payload.size(), true, dts, 0));
                }
            };
            input_pair(1000);
            if (reload) {
                require(stamps.back() == 1016, "video-range reload test did not first activate the live sample clock");
                demuxer.loadSdp(vod_sdp);
                input_pair(400);
                require(stamps == std::vector<uint64_t>({ 1000, 1016, 400, 400 }),
                    "video-only VOD range failed to clear a previously active AAC sample clock");
            } else {
                require(stamps == std::vector<uint64_t>({ 1000, 1000 }),
                    "video-only VOD range incorrectly qualified the audio track as live");
            }
        }
    }
}

} // namespace

int main() {
    try {
        testDtsQuantization(16000, std::string("\x14\x08", 2));
        testDtsQuantization(44100, std::string("\x12\x08", 2));
        testDtsQuantization(48000, std::string("\x11\x88", 2));
        testRetainedMuxerReconnect(false);
        testRetainedMuxerReconnect(true);
        testVodUsesDtsPosition();
        testFirstZeroTimestampHasSenderClock();
        testAdtsCompatibility();
        testOrdinaryFrameAndStampOverride();
        testBatchQualification();
        testBatchActivationAndSr();
        testBatchAggregateAndFragments();
        testBatchLossResetAndWrap();
        testBatchBoundsAndAdts();
        testDemuxerRangeAndOriginalAsc();
        testVideoOnlyVodRange();
        testCapturedBatchAllJoinPositions();
        std::cout << "AAC RTP compatibility and batch clock tests passed (5252 captured join positions)" << std::endl;
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
}
