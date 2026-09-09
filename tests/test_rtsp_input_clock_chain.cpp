/*
 * Copyright (c) 2016-present The ZLMediaKit project authors.
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "Common/config.h"
#include "Extension/Factory.h"
#include "Rtsp/RtspInputClock.h"
#include "Rtsp/RtspMuxer.h"
#include "ext-codec/AACRtp.h"
#include "ext-codec/G711Rtp.h"

using namespace mediakit;
using namespace toolkit;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) throw std::runtime_error(message);
}

struct ErrorRange {
    double minimum = 1e20, maximum = -1e20, last = 0;
    double observe_after_ms = 300000;
    size_t observed = 0;
    void input(double media_ms, double error_ms) {
        last = error_ms;
        if (media_ms >= observe_after_ms) {
            minimum = std::min(minimum, error_ms);
            maximum = std::max(maximum, error_ms);
            ++observed;
        }
    }
    void check(const std::string &stage) const {
        require(observed && minimum >= -10 && maximum <= 10 && maximum - minimum <= 10,
            stage + " accumulated phase: min=" + std::to_string(minimum)
                + " max=" + std::to_string(maximum) + " ms");
    }
};

std::string accessUnit(uint64_t index) {
    std::string au(12, '\0');
    au[0] = 0x21; // Never imitate ADTS; subsequent bytes identify this exact AU.
    for (unsigned j = 0; j != 8; ++j) au[j + 1] = char(index >> (j * 8));
    au[9] = 0x31; au[10] = 0x52; au[11] = 0x73;
    return au;
}

void runChain(CodecId codec, uint32_t rate, int ppm, unsigned aac_units, int seconds, bool restart = false) {
    const bool aac = codec == CodecAAC;
    const double ratio = 1.0 + ppm / 1000000.0;
    const uint64_t wall_ms = 1788880000000ULL;
    const uint32_t samples_per_packet = aac ? 1024 * aac_units : rate / 25;
    const uint32_t starts[] = { 0xffff0000u, 0xffff8000u };
    const uint32_t ssrcs[] = { 0x13572468u, 0x24681357u };
    const uint32_t rates[] = { 90000, rate };
    const std::string fmtp = "streamtype=5;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config="
        + std::string(rate == 16000 ? "1408" : "1108");
    const std::string description =
        "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=input clock chain\r\nt=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z2QAH6zZQFAFuhAAAAMAEAAAAwDxgxHg,aM4G4g==\r\n"
        "a=control:trackID=0\r\nm=audio 0 RTP/AVP 97\r\na=rtpmap:97 "
        + std::string(aac ? "MPEG4-GENERIC" : codec == CodecG711A ? "PCMA" : "PCMU")
        + "/" + std::to_string(rate) + "/1\r\n"
        + (aac ? "a=fmtp:97 " + fmtp + "\r\n" : "") + "a=control:trackID=1\r\n";
    SdpParser sdp(description);
    auto video_track = Factory::getTrackBySdp(sdp.getTrack(TrackVideo));
    auto audio_track = Factory::getTrackBySdp(sdp.getTrack(TrackAudio));
    require(video_track && audio_track && video_track->ready() && audio_track->ready(), "chain SDP tracks unavailable");
    video_track->setIndex(0); audio_track->setIndex(1);
    RtspMuxer muxer;
    require(muxer.addTrack(video_track) && muxer.addTrack(audio_track), "chain muxer rejected tracks");
    auto decoder = Factory::getRtpDecoderByCodecId(codec);
    require(bool(decoder), "chain audio decoder unavailable");
    decoder->setAudioInfo(rate, 1);
    if (aac) {
        Any option;
        option.set<std::string>(fmtp);
        decoder->setOpt(RtpCodec::RTP_DECODER_AAC_LIVE_FMTP, option);
    }

    RtspInputClock clock;
    // Valid pre-RTP reports establish the shared initial phase; UTC itself is
    // eight hours away. All following progression comes from real sample counts.
    for (unsigned track = 0; track != 2; ++track)
        clock.inputSr(track, ssrcs[track], starts[track], int64_t(wall_ms + 28800000) * 1000, 0);
    RtpInfo audio_source(ssrcs[1], 4096, rate, 97, 2, 1);
    std::array<ErrorRange, 2> mapped_errors, mux_errors;
    ErrorRange decoded_errors;
    if (restart) {
        // The input clock confirms three progressing packets. The existing
        // G711 NTP observer can then require two seconds to re-anchor; verify
        // all subsequent frames, while keeping every transient sample below.
        decoded_errors.observe_after_ms = 4000;
        for (auto &error : mapped_errors) error.observe_after_ms = 4000;
        for (auto &error : mux_errors) error.observe_after_ms = 4000;
    }
    uint64_t input_samples = 0, decoded_samples = 0, output_samples = 0, decoded_frames = 0;
    uint64_t video_inputs = 0, video_outputs = 0, audio_outputs = 0, sample_origin = 0;
    uint64_t mux_origin = 0;
    uint32_t output_rtp_origin = 0;
    bool have_mux_origin = false;
    unsigned decoder_boundaries = 0;
    double last_decoder_boundary_ms = 0;

    const auto audioTimeMs = [&](uint64_t samples) { return samples * 1000.0 / (rate * ratio); };
    muxer.getRtpRing()->setDelegate(std::make_shared<RingDelegateHelper>([&](RtpPacket::Ptr packet, bool) {
        if (!have_mux_origin) { have_mux_origin = true; mux_origin = packet->ntp_stamp; }
        const unsigned track = packet->type == TrackVideo ? 0 : 1;
        const double media_ms = track ? audioTimeMs(output_samples) : video_outputs * 40.0;
        mux_errors[track].input(media_ms, int64_t(packet->ntp_stamp) - int64_t(mux_origin) - media_ms);
        if (!track) {
            require(packet->sample_rate == 90000, "video output clock changed");
            ++video_outputs;
            return;
        }
        require(packet->sample_rate == rate, "audio output clock changed");
        const auto payload = packet->getPayload();
        const auto bytes = size_t(packet->getPayloadSize());
        if (aac) {
            const auto expected = accessUnit(output_samples / 1024);
            require(bytes == expected.size() + 4 && payload[0] == 0 && payload[1] == 16
                    && size_t((payload[2] << 5) | (payload[3] >> 3)) == expected.size()
                    && std::string(reinterpret_cast<const char *>(payload) + 4, bytes - 4) == expected,
                "AAC mux lost, duplicated or reordered an access unit");
            output_samples += 1024;
        } else {
            if (!audio_outputs) output_rtp_origin = packet->getStamp();
            require(restart || packet->getStamp() == uint32_t(output_rtp_origin + output_samples),
                "G711 output RTP lost exact sample continuity");
            for (size_t j = 0; j < bytes; ++j) {
                if (payload[j] != uint8_t(output_samples + j))
                    throw std::runtime_error("G711 mux changed sample bytes or order");
            }
            output_samples += bytes;
        }
        ++audio_outputs;
    }));
    audio_track->addDelegate([&](const Frame::Ptr &frame) {
        // The site modify_stamp=0 path does not wrap these frames in FrameStamp.
        return muxer.inputFrame(Frame::getCacheAbleFrame(frame));
    });
    decoder->addDelegate([&](const Frame::Ptr &frame) {
        auto cached = Frame::getCacheAbleFrame(frame);
        const double media_ms = audioTimeMs(decoded_samples);
        decoded_errors.input(media_ms, int64_t(cached->pts()) - int64_t(wall_ms) - media_ms);
        const size_t samples = aac ? 1024 : cached->size() - cached->prefixSize();
        if (!aac) {
            auto exact = dynamic_cast<const G711RtpFrame *>(cached.get());
            require(exact && (restart || !exact->discontinuity) && exact->sample_rate == int(rate) && exact->channels == 1,
                "G711 frequency correction lost exact metadata or created a discontinuity");
            if (exact->discontinuity) {
                ++decoder_boundaries;
                last_decoder_boundary_ms = media_ms;
                sample_origin = exact->sample_stamp - decoded_samples;
            } else if (!decoded_frames) {
                sample_origin = exact->sample_stamp;
            }
            require(exact->sample_stamp == sample_origin + decoded_samples, "G711 decoder inserted a sample gap");
        }
        decoded_samples += samples;
        ++decoded_frames;
        cached->setIndex(1);
        return audio_track->inputFrame(cached);
    });

    std::array<uint64_t, 2> next_packet { 0, 0 };
    const auto mediaUs = [&](unsigned track, uint64_t packet) {
        return track ? audioTimeMs(packet * samples_per_packet) * 1000 : packet * 40000.0;
    };
    const auto arrivalUs = [&](unsigned track, uint64_t packet) {
        if (restart) return int64_t(std::llround(mediaUs(track, packet)));
        const uint64_t batch_size = track ? 3 : 2;
        const auto batch = packet / batch_size;
        uint32_t random = uint32_t(batch + track * 997) * 1664525u + 1013904223u;
        const auto jitter = batch ? random % 6001 : 0;
        return int64_t(std::llround(mediaUs(track, batch * batch_size + batch_size - 1))) + jitter;
    };
    while (true) {
        const auto video_arrival = arrivalUs(0, next_packet[0]);
        const auto audio_arrival = arrivalUs(1, next_packet[1]);
        const unsigned track = audio_arrival < video_arrival ? 1 : 0;
        const auto arrival = track ? audio_arrival : video_arrival;
        if (arrival > int64_t(seconds) * 1000000) break;
        const auto index = next_packet[track]++;
        const double media_us = mediaUs(track, index);
        // Raw RTP advances exactly by payload samples, never by ppm-scaled
        // pseudo-samples. The oscillator error changes physical arrival time.
        const auto restart_packet = uint64_t(rate) / samples_per_packet + 1;
        const auto raw_index = restart && track && index >= restart_packet ? index - restart_packet : index;
        const auto raw = starts[track] + uint32_t(raw_index * (track ? samples_per_packet : 3600));
        if (index && index % 125 == 0)
            clock.inputSr(track, ssrcs[track], raw, int64_t(wall_ms + 28800000) * 1000
                + int64_t(std::llround(media_us)), arrival);
        const auto mapped = clock.inputRtp(track, ssrcs[track], raw, rates[track], arrival,
            int64_t(wall_ms) * 1000 + arrival, track == 1, uint16_t(index));
        mapped_errors[track].input(media_us / 1000, int64_t(mapped) - int64_t(wall_ms) - media_us / 1000);
        if (!track) {
            auto frame = FrameImp::create();
            frame->_codec_id = CodecH264;
            frame->_dts = frame->_pts = mapped;
            frame->_buffer.assign("\x41\x80\x01\x02", 4);
            frame->setIndex(0);
            muxer.inputFrame(frame);
            require(video_outputs == ++video_inputs, "complete video frame was buffered or lost");
            continue;
        }
        std::string payload;
        if (aac) {
            payload.push_back(char((aac_units * 16) >> 8));
            payload.push_back(char(aac_units * 16));
            for (unsigned j = 0; j != aac_units; ++j) {
                payload.push_back(0); payload.push_back(12 << 3);
            }
            for (unsigned j = 0; j != aac_units; ++j) payload += accessUnit(input_samples / 1024 + j);
        } else {
            payload.resize(samples_per_packet);
            for (size_t j = 0; j < payload.size(); ++j) payload[j] = char(input_samples + j);
        }
        const auto before = decoded_frames;
        decoder->inputRtp(audio_source.makeRtpWithStamp(TrackAudio, payload.data(), payload.size(), true, mapped, raw), false);
        input_samples += samples_per_packet;
        require(decoded_frames - before == (aac ? aac_units : 1)
                && decoded_samples == input_samples && output_samples == input_samples,
            "complete audio packet was buffered, dropped, duplicated or padded");
    }
    muxer.flush();
    require(clock.getRtpResets() == unsigned(restart) && clock.hasInitialSr(0)
            && clock.hasInitialSr(1) == !restart, "chain did not preserve or recover its input clock epoch");
    if (restart && !aac) {
        require(decoder_boundaries == 2 && last_decoder_boundary_ms <= 3500,
            "G711 raw reset/NTP recovery did not settle within the existing two-second protection");
    }
    require(output_samples == input_samples && video_outputs == video_inputs, "chain flush changed sample/frame conservation");
    std::cout << "input clock chain codec=" << (aac ? "AAC" : codec == CodecG711A ? "PCMA" : "PCMU")
              << " rate=" << rate << " ppm=" << ppm << " seconds=" << seconds << " samples=" << output_samples
              << " small_reset=" << restart << " last_decoder_boundary_ms=" << last_decoder_boundary_ms
              << " decoded error=[" << decoded_errors.minimum << ',' << decoded_errors.maximum
              << "] mux audio error=[" << mux_errors[1].minimum << ',' << mux_errors[1].maximum
              << "] mux video error=[" << mux_errors[0].minimum << ',' << mux_errors[0].maximum << "] ms\n";
    mapped_errors[0].check("input video"); mapped_errors[1].check("input audio");
    decoded_errors.check("decoded audio"); mux_errors[0].check("mux video"); mux_errors[1].check("mux audio");
    require(mux_errors[1].maximum - mux_errors[0].minimum <= 10
            && mux_errors[0].maximum - mux_errors[1].minimum <= 10,
        "audio decoder/mux reintroduced more than 10 ms cross-track drift");
}
} // namespace

int main() {
    try {
        mINI::Instance()[Rtp::kAudioMtuSize] = 600;
        mINI::Instance()[Rtp::kVideoMtuSize] = 1400;
        // Exercise the existing no-wait video packetizer mode; the clock must
        // not add a new packet hold to either codec path.
        mINI::Instance()[Rtp::kLowLatency] = 1;
        mINI::Instance()[RtpProxy::kRtpG711DurMs] = 20;
        for (int ppm : { -100, 100 }) {
            runChain(CodecG711A, 16000, ppm, 0, 7200);
            runChain(CodecAAC, 64000, ppm, 3, 7200);
            runChain(CodecG711U, 8000, ppm, 0, 1800);
        }
        runChain(CodecG711A, 16000, 0, 0, 8, true);
        runChain(CodecG711U, 8000, 0, 0, 8, true);
        runChain(CodecAAC, 16000, 0, 1, 8, true);
        std::cout << "RTSP input clock decoder/mux chain tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "RTSP input clock chain failure: " << error.what() << std::endl;
        return 1;
    }
}
