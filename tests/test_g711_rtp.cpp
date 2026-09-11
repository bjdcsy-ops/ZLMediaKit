#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Extension/Factory.h"
#include "Common/config.h"
#include "Rtsp/RtspMuxer.h"
#include "Rtsp/RtpReceiver.h"
#include "Rtp/RawEncoder.h"
#include "ext-codec/G711.h"
#include "ext-codec/G711Rtp.h"

using namespace mediakit;

namespace {

void require(bool value, const std::string &message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

FrameImp::Ptr frame(CodecId codec, size_t bytes, uint64_t pts, size_t offset = 0) {
    auto result = FrameImp::create();
    result->_codec_id = codec;
    result->_dts = pts;
    for (size_t i = 0; i < bytes; ++i) {
        result->_buffer.push_back(static_cast<char>((offset + i) % 251));
    }
    return result;
}

struct Packets {
    std::vector<RtpPacket::Ptr> packets;
    RtpRing::RingType::Ptr ring = std::make_shared<RtpRing::RingType>();

    Packets() {
        ring->setDelegate(std::make_shared<RingDelegateHelper>([this](RtpPacket::Ptr packet, bool) {
            packets.emplace_back(std::move(packet));
        }));
    }

    void check(const std::string &expected, uint32_t start, int channels, uint8_t pt, size_t max_payload) const {
        std::string actual;
        auto stamp = start;
        for (auto &packet : packets) {
            auto size = packet->getPayloadSize();
            require(size > 0 && static_cast<size_t>(size) <= max_payload, "payload violates MTU");
            require(size % channels == 0, "packet splits an interleaved sample");
            require(packet->getHeader()->pt == pt, "SDP and RTP payload types differ");
            require(packet->getStamp() == stamp, "sample timestamp overlaps, skips, or rounds");
            actual.append(reinterpret_cast<const char *>(packet->getPayload()), size);
            stamp += size / channels;
        }
        require(actual == expected, "audio payload bytes lost, duplicated, or reordered");
    }

    void checkPayload(const std::string &expected, int channels = 1) const {
        std::string actual;
        for (auto &packet : packets) {
            auto size = packet->getPayloadSize();
            require(size > 0 && size <= 588 && size % channels == 0, "invalid G711 output packet alignment/MTU");
            actual.append(reinterpret_cast<const char *>(packet->getPayload()), size);
        }
        require(actual == expected, "G711 boundary handling lost, duplicated, or reordered samples");
    }
};

std::shared_ptr<AudioTrack> parseTrack(CodecId codec, int pt, int rate, int channels, bool rtpmap = true) {
    auto sdp = std::string("v=0\r\nm=audio 0 RTP/AVP ") + std::to_string(pt) + "\r\n";
    if (rtpmap) {
        sdp += "a=rtpmap:" + std::to_string(pt) + " " + getCodecName(codec) + "/"
             + std::to_string(rate) + "/" + std::to_string(channels) + "\r\n";
    }
    SdpParser parser;
    parser.load(sdp);
    return std::dynamic_pointer_cast<AudioTrack>(Factory::getTrackBySdp(parser.getTrack(TrackAudio)));
}

void testTrackAndSdp(CodecId codec, int input_pt, int rate, int channels) {
    auto track = parseTrack(codec, input_pt, rate, channels);
    require(track && track->getAudioSampleRate() == rate, "G711 sample rate was discarded");
    require(track->getAudioChannel() == channels, "G711 channel count was discarded");
    require(track->getAudioSampleBit() == 16, "PCM sample depth must remain 16 bits");
    RtspMuxer muxer;
    require(muxer.addTrack(track), "failed to add G711 track");
    SdpParser parser;
    parser.load(muxer.getSdp());
    auto audio = parser.getTrack(TrackAudio);
    require(audio && audio->_samplerate == rate && audio->_channel == channels, "SDP lost G711 audio parameters");
    bool standard = rate == 8000 && channels == 1;
    require(standard ? audio->_pt == (codec == CodecG711U ? 0 : 8) : audio->_pt >= 96,
            "static G711 PT is only valid for 8000 Hz mono");
    if (!standard) {
        require(muxer.getSdp().find(std::string(getCodecName(codec)) + "/" + std::to_string(rate)
                   + "/" + std::to_string(channels)) != std::string::npos, "dynamic SDP is missing rtpmap");
    }
    Packets captured;
    muxer.getRtpRing()->setDelegate(std::make_shared<RingDelegateHelper>([&](RtpPacket::Ptr packet, bool) {
        captured.packets.emplace_back(std::move(packet));
    }));
    auto input = frame(codec, rate * channels * 40 / 1000, 7);
    muxer.inputFrame(input);
    muxer.flush();
    captured.check(std::string(input->data(), input->size()), uint64_t(7) * rate / 1000, channels, audio->_pt, 588);
}

void testImmediateDecode() {
    for (auto codec : { CodecG711A, CodecG711U }) {
        auto decoder = Factory::getRtpDecoderByCodecId(codec);
        std::vector<Frame::Ptr> frames;
        decoder->addDelegate([&](const Frame::Ptr &input) {
            frames.emplace_back(Frame::getCacheAbleFrame(input));
            return true;
        });
        RtpInfo info(17, 1500, 16000, 0, 0, 0);
        auto input = frame(codec, 640, 7);
        auto packet = info.makeRtp(TrackAudio, input->data(), input->size(), false, 7);
        decoder->inputRtp(packet, false);
        require(frames.size() == 1, "G711 decoder waits for the next packet");
        require(frames[0]->pts() == 7 && frames[0]->size() == 640, "decoded G711 timing or payload changed");
        decoder->flush();
        require(frames.size() == 1, "flush duplicates the last G711 packet");
    }
}

void testDecoderIgnoresReceiverTrackIndex() {
    auto decoder = Factory::getRtpDecoderByCodecId(CodecG711U);
    std::vector<Frame::Ptr> frames;
    decoder->addDelegate([&](const Frame::Ptr &input) {
        frames.emplace_back(Frame::getCacheAbleFrame(input));
        return true;
    });
    RtpInfo info(17, 1500, 16000, 0, 0, 0);
    auto input = frame(CodecG711U, 640, 7);
    // RtpReceiver does not assign track_index; stale output-side values must not route audio into video.
    for (auto stale_index : { 0, 37, -1 }) {
        auto packet = info.makeRtp(TrackAudio, input->data(), input->size(), false, 7);
        packet->track_index = stale_index;
        decoder->inputRtp(packet, false);
        require(!frames.empty() && frames.back()->getIndex() == TrackAudio,
                "receiver-side packet track_index corrupted the decoded audio track");
    }
    require(frames.size() == 3, "receiver-index regression lost G711 packets");
}

void testPacketization(int rate, int channels, size_t mtu) {
    G711RtpEncoder encoder(rate, channels);
    encoder.setRtpInfo(17, mtu, rate, 96);
    Packets captured;
    encoder.setRtpRing(captured.ring);
    std::string expected;
    uint64_t pts = 7;
    for (auto duration : { 10, 30, 10, 20, 5 }) {
        auto input = frame(CodecG711U, rate * channels * duration / 1000, pts, expected.size());
        expected.append(input->data(), input->size());
        encoder.inputFrame(input);
        pts += duration;
    }
    encoder.flush();
    auto count = captured.packets.size();
    encoder.flush();
    require(captured.packets.size() == count, "flush must be idempotent");
    captured.check(expected, uint64_t(7) * rate / 1000, channels, 96, mtu - 12);
}

void testStaticFallback() {
    for (auto codec : { CodecG711A, CodecG711U }) {
        auto track = parseTrack(codec, codec == CodecG711U ? 0 : 8, 8000, 1, false);
        require(track && track->getAudioSampleRate() == 8000 && track->getAudioChannel() == 1,
                "static PT without rtpmap no longer defaults to 8000 Hz mono");
    }
}

uint32_t littleEndian(const char *ptr, size_t size) {
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i) {
        value |= uint32_t(static_cast<unsigned char>(ptr[i])) << (8 * i);
    }
    return value;
}

void testExtraData() {
    auto track = Factory::getTrackByCodecId(CodecG711U, 16000, 2, 16);
    auto audio = std::dynamic_pointer_cast<AudioTrack>(track);
    require(audio && audio->getAudioSampleRate() == 16000 && audio->getAudioChannel() == 2,
            "non-SDP track creation lost audio parameters");
    auto extra = track->getExtraData();
    require(extra && extra->size() >= 18, "missing G711 WAVEFORMATEX");
    require(littleEndian(extra->data() + 2, 2) == 2 && littleEndian(extra->data() + 4, 4) == 16000,
            "WAVEFORMATEX lost rate or channels");
    require(littleEndian(extra->data() + 8, 4) == 32000 && littleEndian(extra->data() + 12, 2) == 2,
            "WAVEFORMATEX byte rate or block alignment is wrong");
    require(littleEndian(extra->data() + 14, 2) == 8, "encoded G711 samples must remain one byte");
    auto restored = Factory::getTrackByCodecId(CodecG711A, 8000, 1, 16);
    restored->setExtraData(reinterpret_cast<const uint8_t *>(extra->data()), extra->size());
    auto restored_audio = std::dynamic_pointer_cast<AudioTrack>(restored);
    require(restored_audio->getCodecId() == CodecG711U && restored_audio->getAudioSampleRate() == 16000
                && restored_audio->getAudioChannel() == 2 && restored_audio->getAudioSampleBit() == 16,
            "WAVE metadata round-trip lost G711 audio parameters");
}

void testBoundaries() {
    require(!parseTrack(CodecG711U, 0, 0, 1), "invalid SDP zero sample rate was accepted");
    require(!parseTrack(CodecG711U, 0, 16000, 0), "invalid SDP zero channel count was accepted");
    require(!parseTrack(CodecG711U, 0, 16000, 65536), "invalid SDP channel count was accepted");
    bool rejected = false;
    try {
        G711Track invalid(CodecG711U, -1, 1, 16);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "negative sample rate was accepted");
    G711RtpEncoder encoder(16000, 2, 8);
    encoder.setRtpInfo(17, 600, 16000, 96);
    Packets captured;
    encoder.setRtpRing(captured.ring);
    require(!encoder.inputFrame(frame(CodecG711U, 3, 7)), "partial interleaved sample was accepted");
    auto valid = frame(CodecG711U, 640, 7);
    encoder.inputFrame(valid);
    encoder.flush();
    captured.check(std::string(valid->data(), valid->size()), 112, 2, 96, 588);
    rejected = false;
    try {
        G711RtpEncoder tiny(8000, 2);
        tiny.setRtpInfo(17, 13, 8000, 96);
        tiny.inputFrame(frame(CodecG711U, 320, 0));
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "MTU unable to contain an interleaved sample was accepted");
}

void testDiscontinuity() {
    G711RtpEncoder encoder(16000, 1);
    encoder.setRtpInfo(17, 600, 16000, 96);
    Packets captured;
    encoder.setRtpRing(captured.ring);
    auto first = frame(CodecG711U, 160, 7);
    auto second = frame(CodecG711U, 160, 107, 160);
    encoder.inputFrame(first);
    encoder.inputFrame(second);
    encoder.flush();
    require(captured.packets.size() == 2, "discontinuity merged separate audio periods");
    require(captured.packets[0]->getStamp() == 112 && captured.packets[1]->getStamp() == 1712,
            "real input discontinuity was silently re-numbered");
    require(captured.packets[0]->getPayloadSize() == 160 && captured.packets[1]->getPayloadSize() == 160,
            "discontinuity lost audio bytes");
}

void testBackwardDiscontinuity() {
    G711RtpEncoder encoder(16000, 1);
    encoder.setRtpInfo(17, 600, 16000, 96);
    Packets captured;
    encoder.setRtpRing(captured.ring);
    auto first = frame(CodecG711U, 160, 107);
    auto second = frame(CodecG711U, 160, 7, 160);
    encoder.inputFrame(first);
    encoder.inputFrame(second);
    encoder.flush();
    encoder.flush();
    require(captured.packets.size() == 2, "backward discontinuity lost or duplicated partial packets");
    require(captured.packets[0]->getStamp() == 1712 && captured.packets[1]->getStamp() == 112,
            "backward input discontinuity underflowed or was silently renumbered");
    std::string payload;
    for (auto &packet : captured.packets) {
        payload.append(reinterpret_cast<const char *>(packet->getPayload()), packet->getPayloadSize());
    }
    require(payload == std::string(first->data(), first->size()) + std::string(second->data(), second->size()),
            "backward discontinuity changed audio bytes");
}

void testRtpWrap() {
    G711RtpEncoder encoder(16000, 1);
    encoder.setRtpInfo(17, 600, 16000, 96);
    Packets captured;
    encoder.setRtpRing(captured.ring);
    const uint64_t pts = 268435450;
    auto input = frame(CodecG711U, 640, pts);
    encoder.inputFrame(input);
    encoder.flush();
    captured.check(std::string(input->data(), input->size()), uint32_t(pts * 16), 1, 96, 588);
    require(captured.packets.size() == 2 && captured.packets[0]->getStamp() > captured.packets[1]->getStamp(),
            "test did not cross the 32-bit RTP timestamp boundary");
    require(captured.packets[1]->ntp_stamp == captured.packets[0]->ntp_stamp + 20,
            "RTP wrap also wrapped the extended millisecond timeline");
}

void testLongSampleClock() {
    G711RtpEncoder encoder(44100, 2);
    encoder.setRtpInfo(17, 115, 44100, 96);
    Packets captured;
    encoder.setRtpRing(captured.ring);
    std::string expected;
    for (uint64_t i = 0; i < 1000; ++i) {
        // Fractional-millisecond packet duration: Frame PTS rounds down, sample clock must not drift.
        auto input = frame(CodecG711U, 202, (i * 101 * 1000) / 44100, expected.size());
        expected.append(input->data(), input->size());
        encoder.inputFrame(input);
    }
    encoder.flush();
    captured.check(expected, 0, 2, 96, 103);
}

// Drive the real decoder -> encoder boundary: the decoder receives both raw
// RTP sample time and receiver-mapped NTP time. Synthetic payloads contain no
// captured audio and make byte loss/duplication/reordering observable.
struct G711Pipeline {
    explicit G711Pipeline(int rate, int channels, CodecId codec = CodecG711A)
        : decoder(Factory::getRtpDecoderByCodecId(codec)), encoder(rate, channels),
          info(17, 4096, rate, 96, 2, TrackAudio), rate(rate), channels(channels), codec(codec) {
        decoder->setAudioInfo(rate, channels);
        encoder.setRtpInfo(31, 600, rate, 96, 2, TrackAudio);
        encoder.setRtpRing(captured.ring);
        decoder->addDelegate([this](const Frame::Ptr &input) {
            frames.emplace_back(Frame::getCacheAbleFrame(input));
            return encoder.inputFrame(wrap ? wrap(input) : input);
        });
    }

    RtpPacket::Ptr packet(uint16_t seq, uint32_t raw_stamp, uint64_t ntp_ms,
                         size_t bytes, size_t offset, uint32_t ssrc = 17) {
        auto input = frame(codec, bytes, ntp_ms, offset);
        auto packet = info.makeRtpWithStamp(TrackAudio, input->data(), bytes, true, ntp_ms, raw_stamp);
        packet->getHeader()->seq = htons(seq);
        packet->getHeader()->ssrc = htonl(ssrc);
        return packet;
    }

    void input(uint16_t seq, uint32_t raw_stamp, uint64_t ntp_ms, size_t bytes) {
        auto packet = this->packet(seq, raw_stamp, ntp_ms, bytes, expected.size());
        expected.append(reinterpret_cast<const char *>(packet->getPayload()), bytes);
        auto before = frames.size();
        decoder->inputRtp(packet, false);
        require(frames.size() == before + 1, "G711 jitter handling buffered a complete input Frame");
    }

    void checkContinuous(uint32_t expected_start) {
        encoder.flush();
        captured.check(expected, expected_start, channels, 96, 588);
    }

    RtpCodec::Ptr decoder;
    G711RtpEncoder encoder;
    RtpInfo info;
    int rate;
    int channels;
    CodecId codec;
    Packets captured;
    std::vector<Frame::Ptr> frames;
    std::string expected;
    std::function<Frame::Ptr(const Frame::Ptr &)> wrap;
};

const G711RtpFrame &exactFrame(const Frame::Ptr &frame) {
    auto exact = dynamic_cast<const G711RtpFrame *>(frame.get());
    require(exact && exact->cacheAble(), "decoder lost its cacheable G711 sample-clock metadata");
    return *exact;
}

void testRawTimestampBounce(int rate, int channels) {
    for (auto codec : { CodecG711A, CodecG711U }) {
        for (unsigned hold_packets : { 1U, 2U, 3U, 4U, 5U, 25U, 250U }) {
            for (int direction : { -1, 1 }) {
                G711Pipeline pipeline(rate, channels, codec);
                for (unsigned i = 0; i < hold_packets + 2; ++i) {
                    auto natural_ms = int64_t(1000 + i * 40);
                    auto ntp_ms = uint64_t(natural_ms + (i && i <= hold_packets ? direction * 10 : 0));
                    pipeline.input(static_cast<uint16_t>(i), uint32_t(ntp_ms * rate / 1000), ntp_ms,
                                   rate * channels * 40 / 1000);
                    auto &exact = exactFrame(pipeline.frames.back());
                    require(!exact.discontinuity, "bounded RTP phase plateau or return created a discontinuity");
                    require(exact.sample_stamp == uint64_t(natural_ms * rate / 1000),
                            "bounded RTP phase plateau accumulated sample-clock drift");
                }
                pipeline.checkContinuous(rate);
            }
        }
    }
}

void testTransientSenderReportReanchor() {
    G711Pipeline pipeline(32000, 1);
    for (unsigned i = 0; i < 80; ++i) {
        auto ntp_ms = uint64_t(1000 + i * 40 + (i == 25 ? 10 : i == 50 ? -10 : 0));
        pipeline.input(static_cast<uint16_t>(i), 32000 + i * 1280, ntp_ms, 1280);
    }
    pipeline.checkContinuous(32000);
}

void testDecoderDuplicateAndLatePackets() {
    G711Pipeline pipeline(32000, 1);
    auto first = pipeline.packet(10, 32000, 1000, 1280, 0);
    pipeline.expected.assign(reinterpret_cast<const char *>(first->getPayload()), 1280);
    pipeline.decoder->inputRtp(first, false);
    pipeline.decoder->inputRtp(first, false);
    require(pipeline.frames.size() == 1, "duplicate G711 RTP was emitted as new samples");
    pipeline.input(11, 33280, 1040, 1280);
    pipeline.input(12, 34560, 1080, 1280);
    pipeline.decoder->inputRtp(first, false);
    require(pipeline.frames.size() == 3, "late G711 RTP restarted or duplicated the sample timeline");
    pipeline.input(13, 35840, 1120, 1280);
    require(pipeline.frames.size() == 4, "normal input did not discard a lone late probation packet");
    pipeline.checkContinuous(32000);
}

void testConsecutiveHistoricalPacketsAreNotRestart() {
    for (unsigned delayed_by : { 3U, 99U }) {
        G711Pipeline pipeline(32000, 1);
        std::vector<RtpPacket::Ptr> history;
        uint32_t raw_stamp = 32000;
        for (unsigned i = 0; i <= 100; ++i) {
            // Variable payload prevents inferring old timestamps from the most
            // recent packet's duration: history must match actual metadata.
            auto bytes = 320 * (1 + i % 4);
            auto packet = pipeline.packet(1000 + i, raw_stamp, raw_stamp / 32,
                                          bytes, pipeline.expected.size());
            pipeline.expected.append(reinterpret_cast<const char *>(packet->getPayload()), bytes);
            history.emplace_back(packet);
            pipeline.decoder->inputRtp(packet, false);
            require(pipeline.frames.size() == i + 1, "normal variable-size RTP input was buffered");
            raw_stamp += bytes;
        }
        auto before = pipeline.frames.size();
        pipeline.decoder->inputRtp(history[100 - delayed_by], false);
        pipeline.decoder->inputRtp(history[101 - delayed_by], false);
        require(pipeline.frames.size() == before,
                "two consecutive historical RTP packets were misclassified as a source restart");
        pipeline.input(1101, raw_stamp, raw_stamp / 32, 1280);
        pipeline.checkContinuous(32000);
    }
}

void testDecoderSequenceGapPreservesMissingTime() {
    G711Pipeline pipeline(32000, 1);
    pipeline.input(10, 32000, 1000, 1280);
    pipeline.input(12, 34560, 1080, 1280); // One missing 40 ms input packet.
    pipeline.encoder.flush();
    pipeline.captured.checkPayload(pipeline.expected);
    size_t gaps = 0;
    for (size_t i = 1; i < pipeline.captured.packets.size(); ++i) {
        auto &prev = pipeline.captured.packets[i - 1];
        auto &next = pipeline.captured.packets[i];
        auto error = static_cast<int32_t>(next->getStamp() - prev->getStamp()) - prev->getPayloadSize();
        require(error == 0 || error == 1280, "sequence gap produced a wrong RTP discontinuity");
        gaps += error != 0;
    }
    require(gaps == 1, "sequence loss was silently converted to continuous samples");
}

void testDecoderSsrcAndSequenceRestart() {
    for (bool change_ssrc : { false, true }) {
        G711Pipeline pipeline(32000, 1);
        pipeline.input(20000, 32000, 1000, 1280);
        auto restart = pipeline.packet(1000, 160000, 5000, 1280, pipeline.expected.size(), change_ssrc ? 18 : 17);
        pipeline.expected.append(reinterpret_cast<const char *>(restart->getPayload()), 1280);
        pipeline.decoder->inputRtp(restart, false);
        require(pipeline.frames.size() == (change_ssrc ? 2 : 1),
                "G711 restart bypassed or exceeded its one-packet probation");
        if (!change_ssrc) {
            auto second = pipeline.packet(1001, 161280, 5040, 1280, pipeline.expected.size());
            pipeline.expected.append(reinterpret_cast<const char *>(second->getPayload()), 1280);
            pipeline.decoder->inputRtp(second, false);
            require(pipeline.frames.size() == 3, "confirmed same-SSRC restart lost its probation packet");
        }
        require(pipeline.frames[1]->pts() >= 4999 && pipeline.frames[1]->pts() <= 5001,
                "G711 restart was absorbed indefinitely as sample jitter");
        pipeline.encoder.flush();
        pipeline.captured.checkPayload(pipeline.expected);
    }
}

void testReceiverSameSsrcRestartWithBoundedPhase(int rate, int channels, const std::vector<int> &phases_ms) {
    for (auto codec : { CodecG711A, CodecG711U }) {
        G711Pipeline pipeline(rate, channels, codec);
        RtpTrackImp receiver;
        // Keep receiver timestamp mapping deterministic without bypassing its
        // real sequence sorter or the decoder's same-SSRC restart probation.
        receiver.setNtpStamp(0, 0);
        std::vector<size_t> decoded_counts;
        receiver.setOnSorted([&](RtpPacket::Ptr packet) {
            pipeline.decoder->inputRtp(packet, false);
            decoded_counts.emplace_back(pipeline.frames.size());
        });
        const auto samples = uint32_t(rate * 40 / 1000);
        const auto bytes = samples * channels;
        auto input = [&](uint16_t seq, uint32_t raw_stamp) {
            auto packet = pipeline.packet(seq, raw_stamp, raw_stamp * uint64_t(1000) / rate,
                                          bytes, pipeline.expected.size());
            pipeline.expected.append(reinterpret_cast<const char *>(packet->getPayload()), bytes);
            receiver.inputRtp(TrackAudio, rate,
                reinterpret_cast<uint8_t *>(packet->data()) + RtpPacket::kRtpTcpHeaderSize,
                packet->size() - RtpPacket::kRtpTcpHeaderSize);
        };
        input(20000, rate);
        constexpr unsigned restart_packets = 300;
        for (unsigned i = 0; i < restart_packets; ++i) {
            // Both 0/+/-10 ms alternation and opposite +/-10 ms phases are
            // allowed. The latter puts probation at the 20 ms phase bound.
            auto raw_ms = int64_t(5000 + i * 40) + phases_ms[i % phases_ms.size()];
            input(static_cast<uint16_t>(1000 + i), uint32_t(raw_ms * rate / 1000));
        }
        receiver.flush();
        pipeline.decoder->flush();
        pipeline.encoder.flush();
        require(decoded_counts.size() == restart_packets + 1,
                "real RTP sorter lost same-SSRC restart packets");
        require(pipeline.frames.size() == decoded_counts.size(),
                "same-SSRC bounded-phase restart lost audio: receiver delivered "
                    + std::to_string(decoded_counts.size()) + ", decoder emitted "
                    + std::to_string(pipeline.frames.size()));
        require(decoded_counts[0] == 1 && decoded_counts[1] == 1,
                "same-SSRC restart bypassed its one-packet probation");
        for (size_t i = 2; i < decoded_counts.size(); ++i) {
            require(decoded_counts[i] == i + 1,
                    "same-SSRC restart did not restore its first probation packet promptly");
        }
        const auto restart_stamp = uint64_t(5000 + phases_ms.front()) * rate / 1000;
        for (size_t i = 0; i < pipeline.frames.size(); ++i) {
            auto &exact = exactFrame(pipeline.frames[i]);
            require(exact.discontinuity == (i == 1),
                    "same-SSRC restart must create exactly one explicit boundary despite subsequent bounded phase");
            require(exact.sample_stamp == (i ? restart_stamp + (i - 1) * samples : uint64_t(rate)),
                    "bounded phase after restart moved the cumulative sample axis");
            require(exact.sample_rate == rate && exact.channels == channels && exact.size() == bytes,
                    "same-SSRC restart changed sample rate, channel count, or frame samples");
        }
        pipeline.captured.checkPayload(pipeline.expected, channels);
        size_t emitted_bytes = 0;
        for (auto &packet : pipeline.captured.packets) {
            bool before_restart = emitted_bytes < bytes;
            auto expected_stamp = before_restart ? rate + emitted_bytes / channels
                : restart_stamp + (emitted_bytes - bytes) / channels;
            require(packet->getStamp() == uint32_t(expected_stamp),
                    "restart repacketization lost, duplicated, or shifted sample time");
            require(!before_restart || emitted_bytes + packet->getPayloadSize() <= bytes,
                    "restart repacketization merged samples across the explicit boundary");
            emitted_bytes += packet->getPayloadSize();
        }
    }
}

void testReceiverSameSsrcBackwardClockRestart(uint16_t previous_seq, int rate, int channels) {
    for (auto codec : { CodecG711A, CodecG711U }) {
        G711Pipeline pipeline(rate, channels, codec);
        RtpTrackImp receiver;
        constexpr uint64_t first_ntp_ms = 1700000000000ULL;
        constexpr uint32_t old_raw_stamp = 500000000;
        const auto samples = uint32_t(rate * 40 / 1000);
        const auto bytes = samples * channels;
        receiver.setNtpStamp(old_raw_stamp, first_ntp_ms);
        std::vector<size_t> decoded_counts;
        receiver.setOnSorted([&](RtpPacket::Ptr packet) {
            pipeline.decoder->inputRtp(packet, false);
            decoded_counts.emplace_back(pipeline.frames.size());
        });
        auto input = [&](uint16_t seq, uint32_t raw_stamp) {
            auto packet = pipeline.packet(seq, raw_stamp, 0, bytes, pipeline.expected.size());
            pipeline.expected.append(reinterpret_cast<const char *>(packet->getPayload()), bytes);
            receiver.inputRtp(TrackAudio, rate,
                reinterpret_cast<uint8_t *>(packet->data()) + RtpPacket::kRtpTcpHeaderSize,
                packet->size() - RtpPacket::kRtpTcpHeaderSize);
        };
        input(previous_seq, old_raw_stamp);
        constexpr unsigned restart_packets = 300;
        for (unsigned i = 0; i < restart_packets; ++i) {
            const int phase_ms = i % 3 == 1 ? 10 : i % 3 == 2 ? -10 : 0;
            input(static_cast<uint16_t>(1000 + i), uint32_t((5000 + i * 40 + phase_ms) * (rate / 1000)));
        }
        receiver.flush();
        pipeline.encoder.flush();
        require(decoded_counts.size() == restart_packets + 1 && pipeline.frames.size() == decoded_counts.size(),
                "same-SSRC backward-clock restart lost sorted packets or decoded samples");
        const auto start = first_ntp_ms * rate / 1000;
        require(exactFrame(pipeline.frames[1]).sample_stamp == start,
                "same-SSRC restart copied the old raw RTP clock difference into the output sample clock");
        require(decoded_counts[0] == 1 && decoded_counts[1] == 1,
                "backward-clock restart bypassed the one-packet probation");
        for (size_t i = 1; i < pipeline.frames.size(); ++i) {
            const auto &exact = exactFrame(pipeline.frames[i]);
            require(exact.discontinuity == (i == 1) && exact.sample_stamp == start + (i - 1) * samples,
                    "backward-clock restart did not continue from its NTP sample anchor");
            require(i == 1 || decoded_counts[i] == i + 1,
                    "backward-clock restart did not release its probation packet promptly");
        }
        pipeline.captured.checkPayload(pipeline.expected, channels);
        size_t emitted_bytes = 0;
        for (const auto &packet : pipeline.captured.packets) {
            const auto segment_bytes = emitted_bytes < bytes ? emitted_bytes : emitted_bytes - bytes;
            require(packet->getStamp() == uint32_t(start + segment_bytes / channels),
                    "repacketized RTP retained the sender's pre-restart clock offset");
            require(emitted_bytes >= bytes || emitted_bytes + packet->getPayloadSize() <= bytes,
                    "repacketization merged samples across the restart boundary");
            emitted_bytes += packet->getPayloadSize();
        }
    }
}

void testForwardSequenceGapAcrossWrapPreservesMissingTime() {
    for (auto previous_seq : { 60000, 65534 }) {
        for (auto old_raw_stamp : { 32000U, 0xfffff000U }) {
            G711Pipeline pipeline(32000, 1);
            const uint16_t next_seq = previous_seq == 60000 ? 1000 : 1;
            const auto elapsed_samples = uint32_t(uint16_t(next_seq - previous_seq)) * 1280;
            pipeline.input(previous_seq, old_raw_stamp, 1000000, 1280);
            // A separate SR correction must not replace the actual missing
            // RTP duration with the receiver's NTP/sample origin.
            pipeline.input(next_seq, old_raw_stamp + elapsed_samples, 1000000 + elapsed_samples / 32 + 75, 1280);
            const auto &exact = exactFrame(pipeline.frames.back());
            require(exact.discontinuity && exact.sample_stamp == 32000000ULL + elapsed_samples,
                    "forward loss across sequence/RTP wrap was reanchored as a restart");
            pipeline.encoder.flush();
            pipeline.captured.checkPayload(pipeline.expected);
        }
    }
}

void testForwardSequenceRestartProbationGuards() {
    for (auto second_seq : { 1000, 1001, 1002 }) {
        G711Pipeline pipeline(32000, 1);
        pipeline.input(60000, 500000000, 1000000, 1280);
        pipeline.decoder->inputRtp(pipeline.packet(1000, 160000, 1000000, 1280, 1280), false);
        // A duplicate, a missing sequence, or a pair one sample outside the
        // phase bound must not commit an unconfirmed clock restart.
        const auto raw_stamp = 161280U + (second_seq == 1001 ? 1281 : 0);
        pipeline.decoder->inputRtp(pipeline.packet(second_seq, raw_stamp, 1000040, 1280, 2560), false);
        require(pipeline.frames.size() == 1, "invalid positive-sequence probation pair confirmed a restart");
        pipeline.input(60001, 500001280, 1000040, 1280);
        pipeline.checkContinuous(32000000);
    }
}

void testSameSsrcRestartProbationPhaseBounds() {
    for (auto rate : { 16000, 32000 }) {
        for (auto channels : { 1, 2 }) {
            for (auto first_duration_ms : { 10, 40 }) {
                for (int direction : { -1, 1 }) {
                    for (int excess_samples : { 0, 1 }) {
                        G711Pipeline pipeline(rate, channels);
                        auto samples = rate * 40 / 1000;
                        auto first_samples = rate * first_duration_ms / 1000;
                        auto pair_limit = rate * (first_duration_ms / 2 + 20) / 1000;
                        pipeline.input(20000, rate, 1000, samples * channels);
                        auto first = pipeline.packet(1000, 5 * rate, 5000, first_samples * channels,
                                                     pipeline.expected.size());
                        // Pair phase is the difference between two bounded
                        // observations, not the phase of either packet alone.
                        // -40 ms gives equal timestamps; 10/40 ms payloads can
                        // legitimately step backward at their -25 ms bound.
                        auto second_stamp = 5 * rate + first_samples + direction * (pair_limit + excess_samples);
                        auto second = pipeline.packet(1001, second_stamp, uint64_t(second_stamp) * 1000 / rate,
                                                      samples * channels, pipeline.expected.size() + first->getPayloadSize());
                        pipeline.decoder->inputRtp(first, false);
                        require(pipeline.frames.size() == 1, "phase-bound restart bypassed its probation packet");
                        pipeline.decoder->inputRtp(second, false);
                        if (excess_samples) {
                            require(pipeline.frames.size() == 1,
                                    "restart accepted a pair phase error one sample outside its bound");
                            pipeline.input(20001, rate + samples, 1040, samples * channels);
                            pipeline.checkContinuous(rate);
                        } else {
                            require(pipeline.frames.size() == 3,
                                    "restart rejected its inclusive pair phase bound or lost the first probation packet");
                            pipeline.expected.append(reinterpret_cast<const char *>(first->getPayload()), first->getPayloadSize());
                            pipeline.expected.append(reinterpret_cast<const char *>(second->getPayload()), second->getPayloadSize());
                            pipeline.encoder.flush();
                            pipeline.captured.checkPayload(pipeline.expected, channels);
                        }
                    }
                }
            }
        }
    }
}

void testSameSsrcRestartProbationSequenceGuards() {
    for (auto sequence_step : { 0, 2 }) {
        G711Pipeline pipeline(16000, 1);
        pipeline.input(20000, 16000, 1000, 640);
        auto first = pipeline.packet(1000, 80000, 5000, 640, pipeline.expected.size());
        auto second = pipeline.packet(1000 + sequence_step, 80640, 5040, 640, pipeline.expected.size() + 640);
        pipeline.decoder->inputRtp(first, false);
        pipeline.decoder->inputRtp(second, false);
        require(pipeline.frames.size() == 1, "duplicate or gapped probation sequence confirmed a restart");
        pipeline.input(20001, 16640, 1040, 640);
        pipeline.checkContinuous(16000);
    }
    G711Pipeline pipeline(16000, 1);
    pipeline.input(20000, 16000, 1000, 640);
    auto first = pipeline.packet(1000, 80000, 5000, 640, pipeline.expected.size());
    pipeline.decoder->inputRtp(first, false);
    require(pipeline.frames.size() == 1, "lone restart candidate was emitted before confirmation");
    pipeline.input(20001, 16640, 1040, 640);
    auto second = pipeline.packet(1001, 80640, 5040, 640, pipeline.expected.size());
    pipeline.decoder->inputRtp(second, false);
    require(pipeline.frames.size() == 2, "normal old-stream input failed to discard a stale restart candidate");
    pipeline.input(20002, 17280, 1080, 640);
    pipeline.checkContinuous(16000);
}

void testPersistentBoundedRawPhaseDoesNotAccumulate() {
    for (auto rate : { 16000, 32000 }) {
        for (auto codec : { CodecG711A, CodecG711U }) {
            for (int direction : { -1, 1 }) {
                G711Pipeline pipeline(rate, 1, codec);
                for (unsigned i = 0; i <= 1001; ++i) {
                    auto natural_ms = int64_t(1000 + i * 40);
                    auto raw_ms = natural_ms + (i && i <= 1000 ? direction * 10 : 0);
                    pipeline.input(i, uint32_t(raw_ms * rate / 1000), raw_ms, rate * 40 / 1000);
                    auto &exact = exactFrame(pipeline.frames.back());
                    require(exact.sample_rate == rate && exact.channels == 1, "exact frame lost audio parameters");
                    require(!exact.discontinuity, "persistent bounded phase or its return was treated as a gap");
                    require(exact.sample_stamp == uint64_t(natural_ms * rate / 1000),
                            "persistent bounded phase accumulated sample-clock drift");
                }
                pipeline.checkContinuous(rate);
            }
        }
    }
}

void testOppositeRawPhasePlateausRemainBounded() {
    for (auto codec : { CodecG711A, CodecG711U }) {
        for (int direction : { -1, 1 }) {
            G711Pipeline pipeline(16000, 1, codec);
            for (unsigned i = 0; i <= 2001; ++i) {
                auto natural_ms = int64_t(1000 + i * 40);
                auto phase_ms = !i || i == 2001 ? 0 : i <= 1000 ? direction * 10 : -direction * 10;
                auto raw_ms = natural_ms + phase_ms;
                pipeline.input(i, uint32_t(raw_ms * 16), raw_ms, 640);
                auto &exact = exactFrame(pipeline.frames.back());
                require(!exact.discontinuity, "opposite bounded plateaus required an artificial return to zero");
                require(exact.sample_stamp == uint64_t(natural_ms * 16),
                        "opposite bounded plateaus moved the cumulative sample origin");
            }
            pipeline.checkContinuous(16000);
        }
    }
}

void testGradualRawDriftCrossesAbsolutePhaseBound() {
    for (auto rate : { 16000, 32000 }) {
        auto samples = uint32_t(rate * 40 / 1000);
        // Equal 40 ms packets now allow phase strictly below one packet.
        auto limit = uint32_t(rate * 40 / 1000 - 1);
        for (int direction : { -1, 1 }) {
            G711Pipeline pipeline(rate, 1);
            for (unsigned i = 0; i <= 2 * (limit + 1); ++i) {
                auto natural_stamp = uint64_t(rate) + uint64_t(i) * samples;
                auto raw_stamp = int64_t(natural_stamp) + direction * int64_t(i);
                pipeline.input(i, uint32_t(raw_stamp), raw_stamp * 1000 / rate, samples);
                auto &exact = exactFrame(pipeline.frames.back());
                auto crossings = i / (limit + 1);
                auto expected = int64_t(natural_stamp) + direction * int64_t(crossings * (limit + 1));
                bool boundary = i && i % (limit + 1) == 0;
                require(exact.discontinuity == boundary,
                        "gradual RTP drift was bounded by a packet count or silently normalized forever");
                require(exact.sample_stamp == uint64_t(expected),
                        "gradual RTP drift was measured against the preceding packet instead of cumulative samples");
            }
            pipeline.encoder.flush();
            pipeline.captured.checkPayload(pipeline.expected);
        }
    }
}

void testVariablePayloadUsesCurrentPhaseMagnitudeBound() {
    for (int channels : { 1, 2 }) {
        for (int direction : { -1, 1 }) {
            G711Pipeline pipeline(16000, channels);
            const unsigned durations_ms[] = { 40, 80, 20, 40, 20, 10, 40, 20 };
            uint64_t natural_stamp = 16000;
            for (unsigned i = 0; i < sizeof(durations_ms) / sizeof(durations_ms[0]); ++i) {
                auto raw_stamp = int64_t(natural_stamp) + (i ? direction * 160 : 0);
                auto samples = durations_ms[i] * 16;
                pipeline.input(i, uint32_t(raw_stamp), raw_stamp / 16, samples * channels);
                auto &exact = exactFrame(pipeline.frames.back());
                // Ten ms fits half of a 20 ms packet but exceeds half of a
                // 10 ms packet. The tightened bound applies immediately.
                require(exact.discontinuity == (i == 5),
                        "variable payload did not use the current packet's half-duration bound");
                auto expected = int64_t(natural_stamp) + (i >= 5 ? direction * 160 : 0);
                require(exact.sample_stamp == uint64_t(expected),
                        "variable payload advanced the sample clock using bytes rather than channel samples");
                natural_stamp += samples;
            }
            pipeline.encoder.flush();
            pipeline.captured.checkPayload(pipeline.expected, channels);
        }
    }
}

void testSequenceGapOverridesBoundedRawPhase() {
    for (int direction : { -1, 0, 1 }) {
        G711Pipeline pipeline(16000, 1);
        pipeline.input(10, 16000, 1000, 640);
        auto raw_stamp = 16640 + direction * 160;
        pipeline.input(12, raw_stamp, raw_stamp / 16, 640);
        require(exactFrame(pipeline.frames.back()).discontinuity,
                "complete-sequence requirement was bypassed for a small or zero raw phase error");
        require(exactFrame(pipeline.frames.back()).sample_stamp == uint64_t(raw_stamp),
                "sequence gap hid the sender's actual phase or invented a missing duration");
        pipeline.input(13, raw_stamp + 640, raw_stamp / 16 + 40, 640);
        require(!exactFrame(pipeline.frames.back()).discontinuity,
                "normal input after a sequence gap repeatedly reset the sample clock");
        require(exactFrame(pipeline.frames.back()).sample_stamp == uint64_t(raw_stamp + 640),
                "sample timeline after a sequence gap did not continue from its explicit boundary");
        pipeline.encoder.flush();
        pipeline.captured.checkPayload(pipeline.expected);
    }
}

void testRawPhaseCorrectionMagnitudeBoundary() {
    for (auto duration_ms : { 10, 40, 80 }) {
        auto limit_ms = std::min(20, duration_ms / 2);
        for (int direction : { -1, 1 }) {
            for (auto error_ms : { limit_ms, limit_ms + 1, 1000 }) {
                G711Pipeline pipeline(32000, 1);
                auto bytes = 32 * duration_ms;
                pipeline.input(1, 320000, 10000, bytes);
                auto natural_ms = 10000 + duration_ms;
                auto raw_ms = natural_ms + direction * error_ms;
                pipeline.input(2, uint32_t(raw_ms * 32), raw_ms, bytes);
                auto &exact = exactFrame(pipeline.frames.back());
                // The fixed 40/80 ms pairs additionally accept 21 ms phase;
                // short packets retain their original half-duration bound.
                bool boundary = duration_ms == 10 ? error_ms > limit_ms : error_ms >= 40;
                require(exact.discontinuity == boundary, "raw correction exceeded its magnitude bound");
                require(exact.sample_stamp == uint64_t((boundary ? raw_ms : natural_ms) * 32),
                        "raw correction magnitude boundary changed the wrong sample phase");
                pipeline.encoder.flush();
                pipeline.captured.checkPayload(pipeline.expected);
            }
        }
    }
}

void testLongPacketPhaseBoundaries() {
    for (auto codec : { CodecG711A, CodecG711U }) {
        for (auto rate : { 8000, 16000, 32000, 48000, 64000 }) {
            for (auto channels : { 1, 2 }) {
                for (auto duration_ms : { 40, 80 }) {
                    const auto samples = rate * duration_ms / 1000;
                    const auto cap = rate * 40 / 1000;
                    struct Case {
                        int phase;
                        bool boundary;
                    };
                    const Case cases[] = {
                        { rate * 30 / 1000, false }, { -rate * 30 / 1000, false },
                        { cap - 1, false }, { 1 - cap, false },
                        { cap, true }, { cap + 1, true },
                        // At 40 ms, exactly -40 ms is a shared timestamp and
                        // retains the existing 80 ms batch allowance.
                        { -cap, duration_ms != 40 }, { -cap - 1, true },
                    };
                    for (const auto &item : cases) {
                        G711Pipeline pipeline(rate, channels, codec);
                        pipeline.input(1, rate, 1000, samples * channels);
                        const auto raw = rate + samples + item.phase;
                        pipeline.input(2, raw, uint64_t(raw) * 1000 / rate, samples * channels);
                        const auto &exact = exactFrame(pipeline.frames.back());
                        const auto expected = rate + samples + (item.boundary ? item.phase : 0);
                        require(exact.discontinuity == item.boundary && exact.sample_stamp == uint64_t(expected),
                                "long-packet strict phase boundary changed: rate=" + std::to_string(rate)
                                    + ", channels=" + std::to_string(channels)
                                    + ", duration_ms=" + std::to_string(duration_ms)
                                    + ", phase_samples=" + std::to_string(item.phase));
                        if (!item.boundary) {
                            require(exact.ntp_stamp_us == uint64_t(1000 + duration_ms) * 1000,
                                    "long-packet phase leaked into normalized NTP");
                            pipeline.checkContinuous(rate);
                        } else {
                            pipeline.encoder.flush();
                            pipeline.captured.checkPayload(pipeline.expected, channels);
                        }
                    }
                }
            }
        }
    }
}

void testLongPacketAllowanceKeepsOtherBoundaries() {
    struct Case {
        unsigned first_ms, second_ms, phase_ms;
        bool boundary;
    };
    const Case cases[] = {
        { 10, 10, 6, true }, { 20, 20, 11, true },
        { 40, 10, 30, true }, { 40, 80, 30, true }, { 80, 40, 30, true },
        { 10, 80, 10, false }, { 20, 40, 10, false },
    };
    for (auto codec : { CodecG711A, CodecG711U }) {
        for (const auto &item : cases) {
            G711Pipeline pipeline(16000, 2, codec);
            pipeline.input(1, 16000, 1000, item.first_ms * 16 * 2);
            const auto raw_ms = 1000 + item.first_ms + item.phase_ms;
            pipeline.input(2, raw_ms * 16, raw_ms, item.second_ms * 16 * 2);
            const auto &exact = exactFrame(pipeline.frames.back());
            require(exact.discontinuity == item.boundary,
                    "long-packet allowance changed a short or variable-payload boundary");
            pipeline.encoder.flush();
            pipeline.captured.checkPayload(pipeline.expected, 2);
        }
        for (bool change_ssrc : { false, true }) {
            G711Pipeline pipeline(16000, 1, codec);
            pipeline.input(1, 16000, 1000, 640);
            auto packet = pipeline.packet(change_ssrc ? 2 : 3, 17120, 1070, 640,
                                          pipeline.expected.size(), change_ssrc ? 18 : 17);
            pipeline.expected.append(reinterpret_cast<const char *>(packet->getPayload()), 640);
            pipeline.decoder->inputRtp(packet, false);
            require(pipeline.frames.size() == 2 && exactFrame(pipeline.frames.back()).discontinuity
                        && exactFrame(pipeline.frames.back()).sample_stamp == 17120,
                    "long-packet allowance hid a sequence gap or SSRC change");
            pipeline.encoder.flush();
            pipeline.captured.checkPayload(pipeline.expected);
        }
        G711Pipeline frozen(16000, 1, codec);
        for (unsigned i = 0; i < 3; ++i) {
            frozen.input(i, 16000, 1000, 640);
            require(exactFrame(frozen.frames.back()).discontinuity == (i == 2),
                    "long packets escaped the shared-timestamp duration bound");
        }
        frozen.encoder.flush();
        frozen.captured.checkPayload(frozen.expected);
    }
}

void testPersistentLongPacketPhase() {
    // Metadata cannot distinguish a bounded plateau from a real short pause:
    // the accepted policy normalizes both, including their return to zero.
    for (int phase_ms : { -39, -30, -21, 21, 30, 39 }) {
        G711Pipeline pipeline(16000, 1);
        for (unsigned i = 0; i < 102; ++i) {
            const auto natural_ms = 1000 + i * 40;
            const auto raw_ms = int64_t(natural_ms) + (i && i <= 100 ? phase_ms : 0);
            pipeline.input(i, uint32_t(raw_ms * 16), raw_ms, 640);
            const auto &exact = exactFrame(pipeline.frames.back());
            require(!exact.discontinuity && exact.sample_stamp == uint64_t(natural_ms) * 16,
                    "long-packet phase plateau or return moved the cumulative sample axis");
        }
        pipeline.checkContinuous(16000);
    }
}

void testReturnAfterBoundedRawPhase() {
    G711Pipeline pipeline(32000, 1);
    const int offsets_ms[] = { 0, 10, 10, 0, -10, -10, 0 };
    for (unsigned i = 0; i < 70; ++i) {
        auto raw_ms = int64_t(1000 + i * 40) + offsets_ms[i % 7];
        pipeline.input(i, uint32_t(raw_ms * 32), raw_ms, 1280);
        require(!exactFrame(pipeline.frames.back()).discontinuity,
                "return after bounded raw phase created a discontinuity");
    }
    pipeline.checkContinuous(32000);
}

void testDecoderRtpAndSequenceWrap() {
    G711Pipeline pipeline(32000, 1);
    for (unsigned i = 0; i < 5; ++i) {
        pipeline.input(static_cast<uint16_t>(65534 + i), uint32_t(0xffffff00U + i * 1280U),
                       1000 + i * 40, 1280);
    }
    pipeline.checkContinuous(32000);
}

void testCacheAndTimestampWrappers() {
    for (int phase_ms : { 10, 30 }) {
        for (int kind = 0; kind < 3; ++kind) {
            G711Pipeline pipeline(32000, 1);
            pipeline.wrap = [=](const Frame::Ptr &input) -> Frame::Ptr {
                if (kind == 0) {
                    auto cached = Frame::getCacheAbleFrame(input);
                    require(cached.get() == input.get(), "cacheable G711 frame lost exact sample metadata");
                    return cached;
                }
                if (kind == 1) {
                    return std::make_shared<FrameCacheAble>(input);
                }
                auto stamped = std::make_shared<FrameStamp>(input);
                stamped->setStamp(input->dts() + 2000, input->pts() + 2000);
                return stamped;
            };
            for (unsigned i = 0; i < 40; ++i) {
                auto ms = uint64_t(1000 + i * 40 + (i == 5 ? phase_ms : i == 20 ? -phase_ms : 0));
                pipeline.input(i, uint32_t(ms * 32), ms, 1280);
            }
            pipeline.encoder.flush();
            pipeline.captured.checkPayload(pipeline.expected);
            require(pipeline.captured.packets.front()->getStamp() == uint32_t((kind == 2 ? 3000 : 1000) * 32),
                    "G711 metadata bypassed an explicit FrameStamp override");
            for (size_t i = 1; i < pipeline.captured.packets.size(); ++i) {
                require(static_cast<int32_t>(pipeline.captured.packets[i]->getStamp()
                    - pipeline.captured.packets[i - 1]->getStamp()) > 0, "frame wrapper reintroduced G711 RTP rollback");
            }
        }
    }
}

void testSenderClockSlewIsBoundedAndNotIgnored() {
    G711Pipeline pipeline(32000, 1);
    for (unsigned i = 0; i < 1000; ++i) {
        auto natural_ms = uint64_t(1000 + i * 40);
        pipeline.input(static_cast<uint16_t>(i), 32000 + i * 1280,
                       natural_ms + (i >= 10 ? 50 : 0), 1280);
        auto corrected = static_cast<int64_t>(pipeline.frames.back()->pts()) - static_cast<int64_t>(natural_ms);
        require(corrected >= 0 && corrected <= static_cast<int64_t>(i * 40 / 1000 + 2),
                "G711 sender clock moved faster than the bounded 1 ms/s slew");
    }
    auto corrected = static_cast<int64_t>(pipeline.frames.back()->pts()) - (1000 + 999 * 40);
    require(corrected >= 30 && corrected <= 42,
            "persistent small sender-clock error was ignored instead of slowly tracked");
    pipeline.checkContinuous(32000);
}

void testPersistentLargeSenderClockReanchorsWithinBound() {
    for (int direction : { -1, 1 }) {
        G711Pipeline pipeline(32000, 1);
        bool reanchored = false;
        for (unsigned i = 0; i < 100; ++i) {
            auto natural_ms = int64_t(1000 + i * 40);
            pipeline.input(static_cast<uint16_t>(i), 32000 + i * 1280,
                           natural_ms + (i >= 10 ? direction * 300 : 0), 1280);
            auto correction = static_cast<int64_t>(pipeline.frames.back()->pts()) - natural_ms;
            if (std::abs(correction) > 250) {
                require(i >= 60, "large sender-clock outlier reanchored before the persistence window");
                reanchored = true;
            }
            if (i >= 75) {
                require(reanchored, "persistent large sender-clock change was hidden without a bounded reanchor");
            }
        }
        pipeline.encoder.flush();
        pipeline.captured.checkPayload(pipeline.expected);
    }
}

void testCapturedRawRtpAndSrReplay() {
    auto source = std::string(__FILE__);
    auto path = source.substr(0, source.find_last_of("/\\") + 1) + "fixtures/g711_32k_sr_metadata.txt";
    std::ifstream fixture(path);
    require(fixture.good(), "cannot open G711 metadata-only replay fixture");
    G711Pipeline pipeline(32000, 1);
    RtpTrackImp receiver;
    receiver.setOnSorted([&](RtpPacket::Ptr packet) { pipeline.decoder->inputRtp(packet, false); });
    std::string line;
    size_t inputs = 0;
    size_t reports = 0;
    while (std::getline(fixture, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream row(line);
        char kind;
        uint64_t relative_us, seq, raw_stamp, size, ntp_ms;
        require(bool(row >> kind >> relative_us >> seq >> raw_stamp >> size >> ntp_ms),
                "invalid G711 replay metadata row");
        if (kind == 'S') {
            receiver.setNtpStamp(static_cast<uint32_t>(raw_stamp), ntp_ms);
            ++reports;
        } else {
            auto packet = pipeline.packet(static_cast<uint16_t>(seq), static_cast<uint32_t>(raw_stamp),
                                          0, size, pipeline.expected.size());
            pipeline.expected.append(reinterpret_cast<const char *>(packet->getPayload()), size);
            auto before = pipeline.frames.size();
            receiver.inputRtp(TrackAudio, 32000,
                reinterpret_cast<uint8_t *>(packet->data()) + RtpPacket::kRtpTcpHeaderSize,
                packet->size() - RtpPacket::kRtpTcpHeaderSize);
            require(pipeline.frames.size() == before + 1,
                    "metadata replay delayed or lost a contiguous G711 packet");
            ++inputs;
        }
    }
    require(inputs == 749 && reports == 6, "G711 metadata replay did not cover the captured 30-second window");
    require(!pipeline.captured.packets.empty(), "G711 replay emitted no RTP");
    // The first SR chooses the absolute origin. Every following G711 packet
    // must still cover its synthetic samples exactly once, without overlap.
    auto start = pipeline.captured.packets.front()->getStamp();
    pipeline.checkContinuous(start);
}

void testSharedBatchFormatsAndWrap() {
    for (auto codec : { CodecG711A, CodecG711U }) {
        for (auto rate : { 8000, 16000, 32000, 44100, 48000 }) {
            for (auto channels : { 1, 2 }) {
                for (bool wrap : { false, true }) {
                    G711Pipeline pipeline(rate, channels, codec);
                    const uint32_t raw_start = wrap ? 0xffffff00U : 123456U;
                    const uint64_t ntp_start = wrap ? uint64_t(0xffffffffU) * 1000 / rate - 10 : 1000;
                    const uint64_t sample_start = ntp_start * rate / 1000;
                    const std::vector<std::vector<unsigned>> groups = { { 10, 20, 10 }, { 20, 10 }, { 10, 10, 30 } };
                    uint64_t samples = 0;
                    uint16_t seq = 65530;
                    for (unsigned group = 0; group < 60; ++group) {
                        const auto group_samples = samples;
                        for (auto duration_ms : groups[group % groups.size()]) {
                            auto bytes = uint64_t(duration_ms) * rate / 1000 * channels;
                            pipeline.input(seq++, uint32_t(raw_start + group_samples),
                                           ntp_start + group_samples * 1000 / rate, bytes);
                            const auto &exact = exactFrame(pipeline.frames.back());
                            require(!exact.discontinuity && exact.sample_stamp == sample_start + samples,
                                    "shared variable-size batch changed its cumulative sample clock");
                            require(exact.ntp_stamp_us == ntp_start * 1000 + samples * 1000000 / rate,
                                    "shared variable-size batch changed its normalized NTP clock");
                            require(exact.getCodecId() == codec && exact.sample_rate == rate
                                        && exact.channels == channels && exact.size() == bytes,
                                    "shared batch changed its G711 format or sample count");
                            samples += bytes / channels;
                        }
                    }
                    pipeline.checkContinuous(uint32_t(sample_start));
                    uint64_t emitted = 0;
                    for (const auto &packet : pipeline.captured.packets) {
                        require(packet->ntp_stamp == ntp_start + emitted * 1000 / rate,
                                "shared batch RTP and NTP clocks diverged during repacketization");
                        emitted += packet->getPayloadSize() / channels;
                    }
                    require(emitted == samples, "shared batch repacketization changed total sample duration");
                    require(!wrap || pipeline.captured.packets.back()->getStamp()
                                < pipeline.captured.packets.front()->getStamp(),
                            "shared batch wrap regression did not cross the output RTP boundary");
                }
            }
        }
    }
}

void testSharedBatchSequenceAndSsrcBoundaries() {
    for (int boundary : { 0, 1, 2 }) {
        G711Pipeline pipeline(16000, 1);
        pipeline.input(10, 16000, 1000, 256);
        pipeline.input(11, 16000, 1000, 256);
        // A gap inside the same batch must be emitted as a real boundary,
        // rather than mistaken for restart probation because raw time is held.
        uint16_t seq = boundary == 2 ? 12 : 13;
        uint32_t raw = boundary == 0 ? 16000 : boundary == 1 ? 16768 : 80000;
        uint32_t ssrc = boundary == 2 ? 18 : 17;
        auto input = [&](uint32_t stamp, bool discontinuity) {
            auto packet = pipeline.packet(seq++, stamp, stamp / 16, 256, pipeline.expected.size(), ssrc);
            pipeline.expected.append(reinterpret_cast<const char *>(packet->getPayload()), packet->getPayloadSize());
            auto before = pipeline.frames.size();
            pipeline.decoder->inputRtp(packet, false);
            require(pipeline.frames.size() == before + 1, "shared-batch sequence/SSRC boundary lost or delayed audio");
            require(exactFrame(pipeline.frames.back()).discontinuity == discontinuity,
                    "shared-batch sequence/SSRC boundary did not reset the compatibility lease");
        };
        input(raw, true);
        require(exactFrame(pipeline.frames.back()).sample_stamp == raw,
                "shared-batch sequence/SSRC boundary did not preserve the new sample origin");
        raw += 256 + 160;
        input(raw, true); // Ten ms exceeds the ordinary half-packet limit of eight ms.
        input(raw, false); // New shared-timestamp evidence can enable compatibility again.
        input(raw + 512, false);
        pipeline.encoder.flush();
        pipeline.captured.checkPayload(pipeline.expected);
    }
}

void testSharedBatchSameSsrcRestart() {
    for (uint16_t old_seq : { 20000, 60000 }) {
        G711Pipeline pipeline(16000, 1);
        pipeline.input(old_seq, 160000, 10000, 256);
        pipeline.input(old_seq + 1, 160000, 10000, 256);
        // 60001 -> 1000 has a positive signed sequence delta. Its backward
        // raw clock exceeds the batch bound and must still enter probation.
        for (unsigned i = 0; i < 2; ++i) {
            const auto bytes = 256;
            auto packet = pipeline.packet(1000 + i, 80000, 5000, bytes, pipeline.expected.size());
            pipeline.expected.append(reinterpret_cast<const char *>(packet->getPayload()), bytes);
            pipeline.decoder->inputRtp(packet, false);
            require(pipeline.frames.size() == (i ? 4 : 2),
                    "shared-timestamp restart bypassed probation or lost its first packet");
        }
        require(exactFrame(pipeline.frames[2]).discontinuity && !exactFrame(pipeline.frames[3]).discontinuity,
                "shared-timestamp restart did not create exactly one boundary");
        require(exactFrame(pipeline.frames[2]).sample_stamp == 80000
                    && exactFrame(pipeline.frames[3]).sample_stamp == 80256,
                "shared-timestamp restart did not normalize its confirmed sample sequence");
        pipeline.input(1002, 80000, 5000, 768); // Variable payload reaches exactly 80 ms in the group.
        pipeline.input(1003, 81280, 5080, 256);
        require(!exactFrame(pipeline.frames[4]).discontinuity && !exactFrame(pipeline.frames[5]).discontinuity
                    && exactFrame(pipeline.frames[4]).sample_stamp == 80512
                    && exactFrame(pipeline.frames[5]).sample_stamp == 81280,
                "confirmed shared-timestamp restart did not continue on its new sample axis");
        pipeline.encoder.flush();
        pipeline.captured.checkPayload(pipeline.expected);
    }
}

void testSharedBatchFrozenClockIsBounded() {
    G711Pipeline pipeline(16000, 1);
    unsigned since_boundary = 0;
    for (unsigned i = 0; i < 40; ++i) {
        pipeline.input(i, 16000, 1000, 256);
        const auto boundary = exactFrame(pipeline.frames.back()).discontinuity;
        require(i >= 5 || !boundary, "shared timestamp was rejected before its inclusive 80 ms duration bound");
        require(i != 5 || boundary, "shared timestamp containing more than 80 ms was normalized indefinitely");
        since_boundary = boundary ? 1 : since_boundary + 1;
        require(since_boundary <= 5, "permanently frozen G711 timestamp escaped its bounded duration");
    }
    pipeline.encoder.flush();
    pipeline.captured.checkPayload(pipeline.expected);

    G711Pipeline variable(16000, 2);
    const unsigned durations[] = { 10, 20, 50, 1 };
    for (unsigned i = 0; i < 4; ++i) {
        variable.input(i, 16000, 1000, durations[i] * 16 * 2);
        require(exactFrame(variable.frames.back()).discontinuity == (i == 3),
                "shared timestamp duration bound counted packets/bytes instead of channel samples");
    }
    variable.encoder.flush();
    variable.captured.checkPayload(variable.expected, 2);

    // A 10 ms packet followed by an 80 ms packet sharing its timestamp is
    // already accepted by the ordinary 20 ms phase bound. Exceeding the batch
    // duration limit must retain that existing behavior and all 90 ms of audio.
    G711Pipeline legacy(16000, 1);
    legacy.input(0, 16000, 1000, 160);
    legacy.input(1, 16000, 1000, 1280);
    legacy.input(2, 17440, 1090, 256);
    const uint64_t stamps[] = { 16000, 16160, 17440 };
    for (size_t i = 0; i < legacy.frames.size(); ++i) {
        const auto &exact = exactFrame(legacy.frames[i]);
        require(!exact.discontinuity && exact.sample_stamp == stamps[i]
                    && exact.ntp_stamp_us == stamps[i] * 1000000 / 16000,
                "overlong shared-timestamp group broke an input accepted by the ordinary phase bound");
    }
    legacy.checkContinuous(16000);
}

void testSharedBatchCumulativeDriftBound() {
    for (int direction : { -1, 1 }) {
        G711Pipeline pipeline(16000, 1);
        bool crossed = false;
        uint16_t seq = 0;
        uint32_t last_raw = 0;
        for (unsigned group = 0; group < 12 && !crossed; ++group) {
            const auto raw_ms = int64_t(1000 + group * 32) + direction * int64_t(group * 8);
            for (unsigned packet = 0; packet < 2; ++packet) {
                last_raw = uint32_t(raw_ms * 16);
                const auto phase = direction * int64_t(group * 128) - int64_t(packet * 256);
                crossed = std::abs(phase) > 1280;
                pipeline.input(seq++, last_raw, raw_ms, 256);
                const auto &exact = exactFrame(pipeline.frames.back());
                require(exact.discontinuity == crossed,
                        "shared G711 batches reanchored each group or exceeded the absolute 80 ms phase bound");
                require(exact.sample_stamp == (crossed ? last_raw : uint64_t(16000 + (seq - 1) * 256)),
                        "shared G711 batch drift moved the sample axis before its explicit boundary");
                if (crossed) {
                    break;
                }
            }
        }
        require(crossed, "shared batch drift test did not cross its cumulative phase bound");
        last_raw += 256 + 160;
        pipeline.input(seq, last_raw, last_raw / 16, 256);
        require(exactFrame(pipeline.frames.back()).discontinuity,
                "shared batch phase discontinuity did not restore the ordinary half-packet bound");
        pipeline.encoder.flush();
        pipeline.captured.checkPayload(pipeline.expected);
    }
}

void testSharedBatchLeaseExpiresBySampleDuration() {
    for (const auto &durations : { std::vector<unsigned> { 16, 16, 16, 16, 16 },
                                   std::vector<unsigned> { 48, 8, 8, 16 } }) {
        G711Pipeline pipeline(16000, 1);
        pipeline.input(0, 16000, 1000, 256);
        pipeline.input(1, 16000, 1000, 256);
        uint64_t elapsed_ms = 32;
        uint16_t seq = 2;
        for (auto duration_ms : durations) {
            // The last shared packet starts at 16 ms; the 80 ms lease ends
            // at sample time 96 ms, independent of subsequent packet lengths.
            const bool boundary = elapsed_ms >= 96;
            const auto raw_ms = 1000 + elapsed_ms + 10;
            pipeline.input(seq++, raw_ms * 16, raw_ms, duration_ms * 16);
            const auto &exact = exactFrame(pipeline.frames.back());
            require(exact.discontinuity == boundary,
                    "shared batch lease expired at packet end or remained active after 80 ms of samples");
            require(exact.sample_stamp == (1000 + elapsed_ms + (boundary ? 10 : 0)) * 16,
                    "shared batch lease changed the sample origin before expiry");
            elapsed_ms += duration_ms;
        }
        require(exactFrame(pipeline.frames.back()).discontinuity,
                "shared batch lease regression never reached its ordinary input boundary");
        pipeline.encoder.flush();
        pipeline.captured.checkPayload(pipeline.expected);
    }
}

void testCapturedBatchTimestampReplay() {
    struct Metadata {
        uint64_t relative_us;
        uint16_t seq;
        uint32_t raw_stamp;
        size_t bytes;
    };
    auto source = std::string(__FILE__);
    auto path = source.substr(0, source.find_last_of("/\\") + 1) + "fixtures/g711_16k_batch_metadata.txt";
    std::ifstream fixture(path);
    require(fixture.good(), "cannot open G711 batch timestamp metadata fixture");
    std::vector<Metadata> packets;
    std::string line;
    while (std::getline(fixture, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream row(line);
        Metadata packet;
        require(bool(row >> packet.relative_us >> packet.seq >> packet.raw_stamp >> packet.bytes),
                "invalid G711 batch timestamp metadata row");
        require(packet.bytes == 256, "captured 16 kHz G711 packet no longer contains 16 ms of samples");
        if (!packets.empty()) {
            require(uint16_t(packet.seq - packets.back().seq) == 1,
                    "G711 batch capture does not have continuous sequence coverage");
            require(packet.relative_us >= packets.back().relative_us,
                    "G711 batch capture receive order was changed");
        }
        packets.emplace_back(packet);
    }
    require(packets.size() == 940 && packets.front().relative_us == 0
                && packets.back().relative_us == 14998683,
            "G711 batch replay did not cover the captured 15-second window");
    require(packets[0].raw_stamp == packets[1].raw_stamp && packets[1].raw_stamp == packets[2].raw_stamp
                && packets[3].raw_stamp != packets[2].raw_stamp,
            "G711 batch fixture no longer starts with three packets sharing one timestamp");

    // Replay from every possible join point, including every 30/40/50 ms
    // group and its tail. A join at the last packet of a batch has no shared
    // timestamp evidence yet: its next timestamp jump must remain a boundary.
    // Once a repeated timestamp is observed, sample time must stay continuous.
    // Receive times document the capture order; replay deliberately does not
    // sleep or feed wall time into the media clock. There are no captured SRs:
    // this explicit mapping fixes the NTP origin for a deterministic receiver.
    for (size_t first = 0; first < packets.size(); ++first) {
        constexpr int rate = 16000;
        constexpr uint64_t first_ntp_ms = 1000000;
        constexpr uint64_t first_sample = first_ntp_ms * rate / 1000;
        const bool initial_boundary = first + 1 < packets.size()
            && packets[first + 1].raw_stamp != packets[first].raw_stamp;
        const auto boundary_shift = initial_boundary
            ? uint64_t(uint32_t(packets[first + 1].raw_stamp - packets[first].raw_stamp)) - packets[first].bytes
            : 0;
        const auto context = " at capture offset " + std::to_string(first);
        G711Pipeline pipeline(rate, 1);
        RtpTrackImp receiver;
        receiver.setNtpStamp(packets[first].raw_stamp, first_ntp_ms);
        size_t sorted = 0;
        uint64_t decoded_samples = 0;
        uint64_t segment_ntp_us = first_ntp_ms * 1000;
        uint64_t segment_samples = 0;
        receiver.setOnSorted([&](RtpPacket::Ptr packet) {
            auto before = pipeline.frames.size();
            pipeline.decoder->inputRtp(packet, false);
            require(pipeline.frames.size() == before + 1,
                    "G711 batch normalization buffered or lost a sorted input packet" + context);
            const auto &exact = exactFrame(pipeline.frames.back());
            const auto elapsed_samples = decoded_samples + (sorted ? boundary_shift : 0);
            require(exact.discontinuity == (initial_boundary && sorted == 1),
                    "G711 batch replay did not preserve exactly its unevidenced initial boundary" + context);
            require(exact.sample_stamp == first_sample + elapsed_samples,
                    "captured G711 batch timestamp moved the segment sample axis" + context);
            if (initial_boundary && sorted == 1) {
                // A real boundary adopts the receiver's millisecond mapping,
                // while its sample origin keeps the raw gap.
                segment_ntp_us = packet->getStampMS() * 1000;
                segment_samples = decoded_samples;
            }
            const auto expected_ntp_us = segment_ntp_us + (decoded_samples - segment_samples) * 1000000 / rate;
            require(exact.ntp_stamp_us == expected_ntp_us
                        && exact.pts() == exact.ntp_stamp_us / 1000,
                    "captured G711 batch phase leaked into the normalized NTP clock" + context
                        + ", seq=" + std::to_string(packet->getSeq())
                        + ", actual_us=" + std::to_string(exact.ntp_stamp_us)
                        + ", expected_us=" + std::to_string(expected_ntp_us)
                        + ", receiver_ms=" + std::to_string(packet->getStampMS()));
            require(exact.sample_rate == rate && exact.channels == 1
                        && exact.size() == size_t(packet->getPayloadSize()),
                    "G711 batch normalization changed audio parameters or frame sample count");
            decoded_samples += exact.size();
            ++sorted;
        });
        for (size_t i = first; i < packets.size(); ++i) {
            const auto &metadata = packets[i];
            auto packet = pipeline.packet(metadata.seq, metadata.raw_stamp, 0,
                                          metadata.bytes, pipeline.expected.size());
            pipeline.expected.append(reinterpret_cast<const char *>(packet->getPayload()), metadata.bytes);
            receiver.inputRtp(TrackAudio, rate,
                reinterpret_cast<uint8_t *>(packet->data()) + RtpPacket::kRtpTcpHeaderSize,
                packet->size() - RtpPacket::kRtpTcpHeaderSize);
            require(sorted == i - first + 1, "G711 batch receiver delayed a contiguous input packet");
        }
        receiver.flush();
        pipeline.decoder->flush();
        require(sorted == packets.size() - first && pipeline.frames.size() == sorted,
                "G711 batch replay flush lost or duplicated decoded frames");
        pipeline.encoder.flush();
        pipeline.captured.checkPayload(pipeline.expected);
        uint64_t emitted_samples = 0;
        for (const auto &packet : pipeline.captured.packets) {
            const bool before_boundary = initial_boundary && emitted_samples < packets[first].bytes;
            const auto elapsed_samples = emitted_samples + (before_boundary ? 0 : boundary_shift);
            require(packet->getStamp() == uint32_t(first_sample + elapsed_samples),
                    "repacketized G711 batch lost continuous RTP sample time within a segment" + context);
            const auto expected_ntp_ms = before_boundary ? first_ntp_ms + emitted_samples * 1000 / rate
                : (segment_ntp_us + (emitted_samples - segment_samples) * 1000000 / rate) / 1000;
            require(packet->ntp_stamp == expected_ntp_ms,
                    "repacketized G711 batch NTP clock does not match its RTP sample clock" + context);
            require(!before_boundary || emitted_samples + packet->getPayloadSize() <= packets[first].bytes,
                    "repacketized G711 batch merged samples across its initial boundary" + context);
            emitted_samples += packet->getPayloadSize();
        }
        require(emitted_samples == decoded_samples && emitted_samples == pipeline.expected.size(),
                "G711 batch replay did not preserve the complete captured sample duration");
    }
}

// Four windows contain original RTP metadata but deliberately exclude recorded
// SRs: this is a raw-phase regression with an explicit synthetic NTP mapping,
// not validation of the captured SR/NTP path or source connection attribution.
struct G711LongPhasePacket {
    uint64_t relative_us;
    uint16_t seq;
    uint32_t raw_stamp;
    size_t bytes;
};

struct G711LongPhaseCapture {
    std::string name;
    CodecId codec;
    size_t declared_packets;
    std::vector<G711LongPhasePacket> packets;
};

const std::vector<G711LongPhaseCapture> &longPhaseCaptures() {
    static const auto captures = [] {
        auto source = std::string(__FILE__);
        auto path = source.substr(0, source.find_last_of("/\\") + 1)
            + "fixtures/g711_16k_long_packet_phase_metadata.txt";
        std::ifstream fixture(path);
        require(fixture.good(), "cannot open G711 long-packet phase fixture");
        std::vector<G711LongPhaseCapture> result;
        std::string line;
        while (std::getline(fixture, line)) {
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::istringstream row(line);
            char kind;
            require(bool(row >> kind), "invalid G711 long-packet fixture row");
            if (kind == 'W') {
                G711LongPhaseCapture capture;
                std::string codec;
                int rate, channels;
                require(bool(row >> capture.name >> codec >> rate >> channels >> capture.declared_packets)
                            && (codec == "PCMA" || codec == "PCMU") && rate == 16000 && channels == 1,
                        "invalid G711 long-packet fixture window");
                capture.codec = codec == "PCMA" ? CodecG711A : CodecG711U;
                result.emplace_back(std::move(capture));
                continue;
            }
            require(kind == 'R' && !result.empty(), "G711 long-packet fixture packet lacks its window");
            G711LongPhasePacket packet;
            require(bool(row >> packet.relative_us >> packet.seq >> packet.raw_stamp >> packet.bytes)
                        && packet.bytes == 640,
                    "invalid G711 long-packet fixture packet");
            auto &packets = result.back().packets;
            if (packets.empty()) {
                require(packet.relative_us == 0, "G711 long-packet fixture receipt origin changed");
            } else {
                require(uint16_t(packet.seq - packets.back().seq) == 1
                            && int32_t(packet.raw_stamp - packets.back().raw_stamp) > 0
                            && packet.relative_us >= packets.back().relative_us,
                        "G711 long-packet fixture lost continuous sequence/positive timestamp order");
            }
            packets.emplace_back(packet);
        }
        const std::string names[] = { "h264-pcma", "h264-pcmu", "h265-pcma", "h265-pcmu" };
        const size_t counts[] = { 1051, 1050, 1051, 1050 };
        require(result.size() == 4, "G711 long-packet fixture does not cover all four captures");
        for (size_t i = 0; i < result.size(); ++i) {
            require(result[i].name == names[i] && result[i].declared_packets == counts[i]
                        && result[i].packets.size() == counts[i],
                    "G711 long-packet capture identity or duration changed");
        }
        return result;
    }();
    return captures;
}

void testCapturedLongPacketPhaseReplay(size_t window) {
    const auto &capture = longPhaseCaptures().at(window);
    const auto &packets = capture.packets;
    constexpr int rate = 16000;
    constexpr uint64_t first_ntp_ms = 1000000;
    constexpr uint64_t first_sample = first_ntp_ms * rate / 1000;
    // Every captured phase lies within a 30 ms diameter. Replaying all joins
    // includes origins at 0/+10/+30 ms and proves correction is relative to the
    // actual first accepted sample, not a favorable fixed phase in the fixture.
    // All 4202 join positions total 2209202 real receiver inputs across windows.
    for (size_t first = 0; first < packets.size(); ++first) {
        const auto context = " in " + capture.name + " at join " + std::to_string(first);
        G711Pipeline pipeline(rate, 1, capture.codec);
        RtpTrackImp receiver;
        receiver.setNtpStamp(packets[first].raw_stamp, first_ntp_ms);
        size_t sorted = 0;
        uint64_t decoded_samples = 0;
        receiver.setOnSorted([&](RtpPacket::Ptr packet) {
            const auto before = pipeline.frames.size();
            pipeline.decoder->inputRtp(packet, false);
            require(pipeline.frames.size() == before + 1,
                    "long-packet phase handling delayed or lost a sorted frame" + context);
            const auto &exact = exactFrame(pipeline.frames.back());
            const auto expected_sample = first_sample + decoded_samples;
            const auto expected_ntp_us = first_ntp_ms * 1000 + decoded_samples * 1000000 / rate;
            if (exact.discontinuity || exact.sample_stamp != expected_sample) {
                throw std::runtime_error("long-packet raw phase moved the cumulative sample clock" + context
                    + ", seq=" + std::to_string(packet->getSeq())
                    + ", boundary=" + std::to_string(exact.discontinuity)
                    + ", actual_sample=" + std::to_string(exact.sample_stamp)
                    + ", expected_sample=" + std::to_string(expected_sample));
            }
            if (exact.ntp_stamp_us != expected_ntp_us || exact.pts() != expected_ntp_us / 1000) {
                throw std::runtime_error("long-packet raw phase leaked into the synthetic NTP clock" + context
                    + ", seq=" + std::to_string(packet->getSeq())
                    + ", actual_us=" + std::to_string(exact.ntp_stamp_us)
                    + ", expected_us=" + std::to_string(expected_ntp_us)
                    + ", receiver_ms=" + std::to_string(packet->getStampMS()));
            }
            require(exact.sample_rate == rate && exact.channels == 1 && exact.size() == 640
                        && exact.getCodecId() == capture.codec,
                    "long-packet phase replay changed codec, format or decoded sample count" + context);
            decoded_samples += exact.size();
            ++sorted;
        });
        for (size_t i = first; i < packets.size(); ++i) {
            const auto &metadata = packets[i];
            auto packet = pipeline.packet(metadata.seq, metadata.raw_stamp, 0,
                                          metadata.bytes, pipeline.expected.size());
            pipeline.expected.append(reinterpret_cast<const char *>(packet->getPayload()), metadata.bytes);
            receiver.inputRtp(TrackAudio, rate,
                reinterpret_cast<uint8_t *>(packet->data()) + RtpPacket::kRtpTcpHeaderSize,
                packet->size() - RtpPacket::kRtpTcpHeaderSize);
            require(sorted == i - first + 1, "long-packet receiver buffered a contiguous packet" + context);
            require(pipeline.captured.packets.size() == 2 * sorted,
                    "a complete 40 ms input did not immediately emit both 20 ms RTP packets" + context);
        }
        receiver.flush();
        pipeline.decoder->flush();
        pipeline.encoder.flush();
        require(sorted == packets.size() - first && pipeline.frames.size() == sorted
                    && pipeline.captured.packets.size() == 2 * sorted,
                "long-packet phase replay flush lost or duplicated media" + context);
        pipeline.captured.check(pipeline.expected, uint32_t(first_sample), 1, 96, 588);
        uint64_t emitted_samples = 0;
        for (const auto &packet : pipeline.captured.packets) {
            require(packet->getPayloadSize() == 320
                        && packet->getStamp() == uint32_t(first_sample + emitted_samples)
                        && packet->ntp_stamp == first_ntp_ms + emitted_samples * 1000 / rate,
                    "long-packet repacketization changed its exact RTP/NTP sample axis" + context);
            emitted_samples += packet->getPayloadSize();
        }
        require(emitted_samples == decoded_samples && emitted_samples == pipeline.expected.size(),
                "long-packet phase replay did not conserve its complete sample duration" + context);
    }
}


#if defined(ENABLE_RTPPROXY)
class RawPackets : public RawEncoderImp {
public:
    explicit RawPackets(uint8_t pt) : RawEncoderImp(17, pt, true) {}
    std::vector<RtpPacket::Ptr> packets;

private:
    void onRTP(toolkit::Buffer::Ptr buffer, bool) override {
        auto packet = std::dynamic_pointer_cast<RtpPacket>(buffer);
        require(packet != nullptr, "raw encoder did not emit RTP");
        packets.emplace_back(std::move(packet));
    }
};

void testRawPayloadType() {
    for (auto codec : { CodecG711A, CodecG711U }) {
        auto pt = codec == CodecG711U ? 0 : 8;
        for (auto rate : { 8000, 16000 }) {
            for (auto channels : { 1, 2 }) {
                auto track = Factory::getTrackByCodecId(codec, rate, channels, 16);
                bool compatible = rate == 8000 && channels == 1;
                RawPackets raw(pt);
                require(raw.addTrack(track) == compatible, "raw static PT accepted incompatible G711 parameters");
                auto input = frame(codec, rate * channels * 20 / 1000, 7);
                raw.inputFrame(input);
                raw.flush();
                if (compatible) {
                    require(raw.packets.size() == 1 && raw.packets[0]->getHeader()->pt == pt
                                && raw.packets[0]->getStamp() == 56,
                            "compatible raw static PT was changed or flush lost residual audio");
                } else {
                    require(raw.packets.empty(), "rejected raw track emitted RTP");
                }
                RawPackets wrong_codec(pt == 0 ? 8 : 0);
                require(!wrong_codec.addTrack(track), "raw G711 codec/PT mismatch was accepted");
                RawPackets dynamic(110);
                require(dynamic.addTrack(track), "raw dynamic G711 PT was rejected");
                dynamic.inputFrame(input);
                dynamic.flush();
                require(!dynamic.packets.empty() && dynamic.packets[0]->getHeader()->pt == 110,
                        "negotiated dynamic PT was changed");
                Packets captured;
                captured.packets = dynamic.packets;
                captured.check(std::string(input->data(), input->size()), uint64_t(7) * rate / 1000, channels, 110, 588);
            }
        }
    }
}

void testRawPacketDurationMtu() {
    struct RestoreDuration {
        std::string previous = toolkit::mINI::Instance()[RtpProxy::kRtpG711DurMs];
        ~RestoreDuration() { toolkit::mINI::Instance()[RtpProxy::kRtpG711DurMs] = previous; }
    } restore;
    toolkit::mINI::Instance()[RtpProxy::kRtpG711DurMs] = 100;
    RawPackets raw(110);
    auto track = Factory::getTrackByCodecId(CodecG711U, 16000, 2, 16);
    require(raw.addTrack(track), "100 ms raw G711 track was rejected");
    auto input = frame(CodecG711U, 3200, 7);
    raw.inputFrame(input);
    raw.flush();
    require(raw.packets.size() == 6, "100 ms raw packetization ignored the 600-byte MTU");
    Packets captured;
    captured.packets = raw.packets;
    captured.check(std::string(input->data(), input->size()), 112, 2, 110, 588);
}
#endif

} // namespace

int main() {
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
    run("static fallback", testStaticFallback);
    run("immediate decoder", testImmediateDecode);
    run("receiver track index is not audio routing metadata", testDecoderIgnoresReceiverTrackIndex);
    run("WAVE metadata", testExtraData);
    run("invalid parameters and encoded sample width", testBoundaries);
    run("preserve real input discontinuity", testDiscontinuity);
    run("partial packet before backward discontinuity", testBackwardDiscontinuity);
    run("RTP timestamp wrap", testRtpWrap);
    run("long fractional sample clock", testLongSampleClock);
    for (auto rate : { 8000, 16000, 32000, 48000, 64000 }) {
        for (auto channels : { 1, 2 }) {
            run("raw RTP timestamp bounce " + std::to_string(rate) + "/" + std::to_string(channels),
                [=] { testRawTimestampBounce(rate, channels); });
        }
    }
    run("transient SR reanchor does not reset sample clock", testTransientSenderReportReanchor);
    run("captured raw RTP and SR receiver replay", testCapturedRawRtpAndSrReplay);
    run("captured shared batch timestamp receiver replay", testCapturedBatchTimestampReplay);
    for (size_t window = 0; window < 4; ++window) {
        run("captured G711 long-packet phase " + std::to_string(window),
            [=] { testCapturedLongPacketPhaseReplay(window); });
    }
    run("shared G711 batch formats, variable payloads and wrap", testSharedBatchFormatsAndWrap);
    run("shared G711 batch sequence and SSRC boundaries", testSharedBatchSequenceAndSsrcBoundaries);
    run("shared G711 batch same-SSRC restart", testSharedBatchSameSsrcRestart);
    run("shared G711 frozen timestamp duration bound", testSharedBatchFrozenClockIsBounded);
    run("shared G711 cumulative phase bound", testSharedBatchCumulativeDriftBound);
    run("shared G711 lease expires by sample duration", testSharedBatchLeaseExpiresBySampleDuration);
    run("G711 duplicate and late packets", testDecoderDuplicateAndLatePackets);
    run("G711 consecutive historical packets are not restart", testConsecutiveHistoricalPacketsAreNotRestart);
    run("G711 sequence gap preserves missing time", testDecoderSequenceGapPreservesMissingTime);
    run("G711 SSRC and same-SSRC sequence restart", testDecoderSsrcAndSequenceRestart);
    for (auto rate : { 16000, 32000 }) {
        for (auto channels : { 1, 2 }) {
            auto format = std::to_string(rate) + "/" + std::to_string(channels);
            run("G711 receiver same-SSRC restart without jitter " + format,
                [=] { testReceiverSameSsrcRestartWithBoundedPhase(rate, channels, { 0 }); });
            for (int direction : { -1, 1 }) {
                auto phase = std::to_string(direction * 10);
                run("G711 receiver same-SSRC restart 0/" + phase + " ms " + format,
                    [=] { testReceiverSameSsrcRestartWithBoundedPhase(rate, channels, { 0, direction * 10 }); });
                run("G711 receiver same-SSRC restart opposite " + phase + " ms " + format,
                    [=] { testReceiverSameSsrcRestartWithBoundedPhase(rate, channels, { direction * 10, -direction * 10 }); });
            }
        }
    }
    run("G711 same-SSRC restart probation phase bounds", testSameSsrcRestartProbationPhaseBounds);
    for (auto previous_seq : { 20000, 33768, 33769, 60000 }) {
        for (auto channels : { 1, 2 }) {
            run("G711 receiver backward-clock restart " + std::to_string(previous_seq) + "->1000/" + std::to_string(channels),
                [=] { testReceiverSameSsrcBackwardClockRestart(previous_seq, 32000, channels); });
        }
    }
    run("G711 forward loss across sequence and RTP wrap", testForwardSequenceGapAcrossWrapPreservesMissingTime);
    run("G711 positive-sequence restart probation guards", testForwardSequenceRestartProbationGuards);
    run("G711 same-SSRC restart probation sequence guards", testSameSsrcRestartProbationSequenceGuards);
    run("G711 persistent bounded raw phase does not accumulate", testPersistentBoundedRawPhaseDoesNotAccumulate);
    run("G711 opposite bounded raw phase plateaus", testOppositeRawPhasePlateausRemainBounded);
    run("G711 gradual raw drift crosses absolute phase bound", testGradualRawDriftCrossesAbsolutePhaseBound);
    run("G711 variable payload tightens phase magnitude bound", testVariablePayloadUsesCurrentPhaseMagnitudeBound);
    run("G711 sequence gap overrides bounded raw phase", testSequenceGapOverridesBoundedRawPhase);
    run("G711 raw phase correction magnitude boundaries", testRawPhaseCorrectionMagnitudeBoundary);
    run("G711 strict long-packet phase boundaries", testLongPacketPhaseBoundaries);
    run("G711 long-packet allowance retains other boundaries", testLongPacketAllowanceKeepsOtherBoundaries);
    run("G711 persistent long-packet phase and short-pause ambiguity", testPersistentLongPacketPhase);
    run("G711 return after bounded raw phase", testReturnAfterBoundedRawPhase);
    run("G711 decoder RTP and sequence wrap", testDecoderRtpAndSequenceWrap);
    run("G711 cache and timestamp wrapper compatibility", testCacheAndTimestampWrappers);
    run("G711 sender-clock slew is bounded but tracks drift", testSenderClockSlewIsBoundedAndNotIgnored);
    run("G711 persistent large sender-clock reanchor is bounded", testPersistentLargeSenderClockReanchorsWithinBound);
#if defined(ENABLE_RTPPROXY)
    run("raw explicit static and dynamic payload types", testRawPayloadType);
    run("raw 100 ms packet duration obeys MTU", testRawPacketDurationMtu);
#endif
    for (auto codec : { CodecG711A, CodecG711U }) {
        auto pt = codec == CodecG711U ? 0 : 8;
        for (auto rate : { 8000, 16000 }) {
            for (auto channels : { 1, 2 }) {
                run(std::string(getCodecName(codec)) + "/" + std::to_string(rate) + "/" + std::to_string(channels),
                    [=] { testTrackAndSdp(codec, pt, rate, channels); });
            }
        }
    }
    run("dynamic input", [] { testTrackAndSdp(CodecG711U, 110, 16000, 1); });
    run("variable packet sizes and flush", [] { testPacketization(16000, 1, 600); });
    run("small MTU exact sample offsets", [] { testPacketization(16000, 1, 113); });
    run("stereo small MTU exact sample offsets", [] { testPacketization(48000, 2, 115); });
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
