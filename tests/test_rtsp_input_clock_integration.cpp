/*
 * Copyright (c) 2016-present The ZLMediaKit project authors.
 * SPDX-License-Identifier: MIT
 */

#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "Common/config.h"
#include "Player/PlayerProxy.h"
#include "Poller/EventPoller.h"
#include "Rtcp/RtcpContext.h"
#include "Rtsp/RtspPlayerImp.h"

using namespace mediakit;
using namespace toolkit;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::string sdp(bool aac = false, const std::string &session_range = "", const std::string &audio_range = "") {
    return "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=input clock integration\r\nt=0 0\r\n"
        + session_range
        + "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
          "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=Z2QAH6zZQFAFuhAAAAMAEAAAAwDxgxHg,aM4G4g==\r\n"
          "a=control:trackID=0\r\n"
        + (aac ? "m=audio 0 RTP/AVP 97\r\na=rtpmap:97 MPEG4-GENERIC/64000/1\r\n"
                 "a=fmtp:97 streamtype=5;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1108\r\n"
               : "m=audio 0 RTP/AVP 0\r\na=rtpmap:0 PCMU/8000/1\r\n")
        + audio_range + "a=control:trackID=1\r\n";
}

// Transport only is replaced. Responses traverse the production RTSP handlers;
// RTP and SR traverse onRtpPacket -> receiver/sorter -> player -> demuxer/decoder.
// No private-member macro, real socket, media sleeps, or algorithm clock hook.
class CapturingPlayer : public RtspPlayerImp {
public:
    explicit CapturingPlayer(const EventPoller::Ptr &poller) : RtspPlayerImp(poller) {
        (*this)[Client::kWaitTrackReady] = false;
    }

    ssize_t send(Buffer::Ptr buffer) override {
        requests.emplace_back(buffer->data(), buffer->size());
        return buffer->size();
    }

    void describe(const std::string &description) {
        teardown();
        onConnect(SockException());
        respond("Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, GET_PARAMETER\r\n");
        respond("Content-Base: rtsp://clock.example/live\r\nContent-Type: application/sdp\r\n", description);
        require(requests.back().find("SETUP ") == 0, "DESCRIBE did not enter real SETUP handler");
        require(getTracks(false).size() == 2, "SDP decoder tracks missing");
    }

    void finishHandshake() {
        respond("Session: fixture\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n");
        respond("Session: fixture\r\nTransport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n");
        require(requests.back().find("PLAY ") == 0, "SETUP did not enter real PLAY handler");
        require(getInputClockMode() == 0, "effective mode became active before PLAY success");
        respond("Session: fixture\r\n");
    }

    void captureFrames() {
        for (const auto &track : getTracks(false)) {
            track->addDelegate([this](const Frame::Ptr &frame) {
                frames[frame->getTrackType()].emplace_back(Frame::getCacheAbleFrame(frame));
                return true;
            });
        }
    }

    void input(const RtpPacket::Ptr &packet) {
        onRtpPacket(packet->data(), packet->size());
    }

    void senderReport(unsigned track, uint32_t ssrc, uint32_t raw, uint32_t rate, uint64_t ntp_ms) {
        RtcpContextForSend sender;
        sender.onRtp(1, raw, ntp_ms, rate, 160);
        auto sr = sender.createRtcpSR(ssrc);
        std::string wire { '$', char(track * 2 + 1), char(sr->size() >> 8), char(sr->size() & 255) };
        wire.append(sr->data(), sr->size());
        onRtpPacket(wire.data(), wire.size());
    }

    void disconnect() { onError(SockException(Err_eof, "fixture disconnect")); }

    std::array<std::vector<RtpPacket::Ptr>, 2> packets;
    std::array<std::vector<Frame::Ptr>, 2> frames;
    std::vector<std::string> requests;

protected:
    void onRtpSorted(RtpPacket::Ptr packet, int index) override {
        std::string wire(packet->data() + 4, packet->size() - 4);
        RtspPlayer::onRtpSorted(packet, index);
        require(wire == std::string(packet->data() + 4, packet->size() - 4),
            "input clock changed an RTP header or payload byte");
        packets[index].emplace_back(std::move(packet));
    }

private:
    void respond(const std::string &headers, const std::string &body = "") {
        std::string response = "RTSP/1.0 200 OK\r\nCSeq: 1\r\n" + headers
            + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        Parser parser;
        parser.parse(response.data(), response.size());
        onWholeRtspPacket(parser);
    }
};

class CapturingProxy : public PlayerProxy {
public:
    explicit CapturingProxy(const EventPoller::Ptr &poller)
        : PlayerProxy(MediaTuple { DEFAULT_VHOST, "clock-test", "proxy", "" }, ProtocolOption(), 0, poller) {}
    void attach(const PlayerBase::Ptr &player) {
        _delegate = player;
        player->setOnPlayResult(result);
        player->setOnShutdown(shutdown);
    }
    void setOnPlayResult(const Event &callback) override {
        result = callback;
        MediaPlayer::setOnPlayResult(callback);
    }
    void setOnShutdown(const Event &callback) override {
        shutdown = callback;
        MediaPlayer::setOnShutdown(callback);
    }
    // Install the production lifecycle callbacks through play(). Unsupported
    // schema fails synchronously before any socket can be created; retry=0.
    void armLifecycle() { PlayerProxy::play("fixture-invalid://local"); }
    Event result;
    Event shutdown;
};

void testEligibility(const EventPoller::Ptr &poller) {
    struct Case { const char *session; const char *audio; bool live; };
    const Case cases[] = {
        { "", "", true },
        { "a=range:npt=now-\r\n", "", true },
        { "a=range:npt=0.000-\r\n", "", true },
        { "a=range:npt=0-120\r\n", "", false },
        { "a=range:npt=2-\r\n", "", false },
        { "a=range:clock=20260909T000000Z-20260909T010000Z\r\n", "", false },
        { "", "a=range:npt=0-60\r\n", false },
        { "a=range:npt=now-\r\n", "a=range:invalid\r\n", false }
    };
    for (const auto &item : cases) {
        auto player = std::make_shared<CapturingPlayer>(poller);
        (*player)[Client::kRtspInputClock] = 1;
        RtspDemuxer demuxer;
        demuxer.loadSdp(sdp(false, item.session, item.audio));
        require(demuxer.isLive() == item.live, "session/track range live qualification changed");
        player->describe(sdp(false, item.session, item.audio));
        require(player->getInputClockMode() == 0, "request was confused with effective mode");
        player->finishHandshake();
        require(player->getInputClockMode() == int(item.live), "player ignored live/VOD eligibility");
        player->teardown();
    }
    for (int requested : { 0, 1, 2 }) {
        auto player = std::make_shared<CapturingPlayer>(poller);
        if (requested) (*player)[Client::kRtspInputClock] = requested;
        player->describe(sdp());
        player->finishHandshake();
        require(player->getInputClockMode() == (requested == 1), "default-off or explicit-one contract changed");
        player->teardown();
    }
    auto player = std::make_shared<CapturingPlayer>(poller);
    (*player)[Client::kRtspInputClock] = 1;
    (*player)[Client::kBenchmarkMode] = 1;
    player->describe(sdp());
    player->finishHandshake();
    require(player->getInputClockMode() == 0, "benchmark input claimed clock normalization");
    player->teardown();
}

void testProxyModeAndDirectPath(const EventPoller::Ptr &poller) {
    auto player = std::make_shared<CapturingPlayer>(poller);
    auto proxy = std::make_shared<CapturingProxy>(poller);
    (*proxy)[Client::kRtspInputClock] = 1;
    (*player)[Client::kRtspInputClock] = 1;
    proxy->armLifecycle();
    proxy->attach(player);
    require(proxy->getInputClockRequested() == 1, "proxy did not snapshot its requested mode");
    require(proxy->getInputClockMode() == 0, "proxy request reported active before handshake");
    player->describe(sdp());
    player->finishHandshake();
    // The socket-free fixture has no play timeout timer, so the RTSP response
    // follows onResume. Deliver the installed production result callback after
    // the real handlers have established the delegate's effective mode.
    proxy->result(SockException());
    require(proxy->getInputClockMode() == 1, "proxy did not expose delegate effective mode before first SR");
    const auto sent = player->requests.size();
    player->seekTo(uint32_t(1));
    player->speed(2.0f);
    player->pause(true);
    require(player->requests.size() == sent, "protected live input accepted PAUSE");
    player->pause(false);
    require(player->requests.size() == sent, "protected live input accepted seek, speed or resume PLAY");
    player->disconnect();
    require(proxy->getInputClockMode() == 0, "proxy kept effective mode after disconnect");
    require((*proxy)[Client::kRtspInputClock].as<int>() == 1, "disconnect erased requested mode");
    player->describe(sdp());
    player->finishHandshake();
    proxy->result(SockException());
    require(proxy->getInputClockMode() == 1, "reconnect did not requalify the input clock");
    proxy->teardown();
    require(proxy->getInputClockMode() == 0, "teardown retained active mode");

    player->setMediaSource(std::make_shared<RtspMediaSource>(
        MediaTuple { DEFAULT_VHOST, "clock-test", "direct", "" }));
    player->describe(sdp());
    player->finishHandshake();
    proxy->result(SockException());
    require(proxy->getInputClockMode() == 0, "direct RTP proxy claimed repacketized input clock");
    proxy->teardown();

    auto legacy = std::make_shared<CapturingPlayer>(poller);
    legacy->describe(sdp());
    legacy->finishHandshake();
    legacy->pause(true);
    require(legacy->requests.back().find("PAUSE ") == 0, "mode=0 lost its PAUSE request");
    legacy->pause(false);
    require(legacy->requests.back().find("PLAY ") == 0
        && legacy->requests.back().find("Range:") != std::string::npos,
        "mode=0 lost its legacy resume PLAY/Range request");
    legacy->teardown();
}

void testProxyStatusCallbacks(const EventPoller::Ptr &poller) {
    auto proxy = std::make_shared<CapturingProxy>(poller);
    (*proxy)[Client::kRtspInputClock] = 1;
    require(proxy->getStatus() == 1, "new proxy is not connecting");
    proxy->armLifecycle();
    require(proxy->getStatus() == 1, "failed start reported playing");
    require(proxy->getInputClockRequested() == 1 && proxy->getInputClockMode() == 0,
        "failed start did not retain requested mode with effective mode disabled");
    auto player = std::make_shared<CapturingPlayer>(poller);
    (*player)[Client::kRtspInputClock] = 1;
    player->describe(sdp());
    player->finishHandshake();
    proxy->attach(player);
    proxy->setPlayCallbackOnce([&](const SockException &error) {
        require(!error && proxy->getStatus() == 0 && proxy->getInputClockMode() == 1,
            "play callback observed stale status/effective mode");
    });
    proxy->result(SockException());
    require(proxy->getStatus() == 0, "successful callback did not mark proxy playing");
    player->disconnect();
    require(proxy->getStatus() == 1 && proxy->getInputClockMode() == 0,
        "offline proxy retained playing status or active mode");
    require((*proxy)[Client::kRtspInputClock].as<int>() == 1, "offline proxy lost request");
    player->describe(sdp());
    player->finishHandshake();
    proxy->result(SockException());
    require(proxy->getStatus() == 0 && proxy->getInputClockMode() == 1,
        "reconnected proxy failed to restore status and effective mode");
    proxy->setPlayCallbackOnce([&](const SockException &error) {
        require(error && proxy->getStatus() == 1, "failure callback observed stale playing status");
    });
    proxy->result(SockException(Err_timeout, "fixture failed start"));
    proxy->teardown();
    mINI updated;
    updated[Client::kRtspInputClock] = 0;
    proxy->update("fixture-invalid://local", updated);
    require(proxy->getInputClockRequested() == 0 && proxy->getInputClockMode() == 0,
        "proxy option update retained a stale request snapshot");
}

void testSrDecoderChain(const EventPoller::Ptr &poller, bool enabled, bool aac, bool late_sr = false) {
    auto player = std::make_shared<CapturingPlayer>(poller);
    (*player)[Client::kRtspInputClock] = enabled ? 1 : 0;
    player->describe(sdp(aac));
    player->finishHandshake();
    player->captureFrames();
    const uint32_t rates[] = { 90000, aac ? 64000u : 8000u };
    const uint32_t starts[] = { 0xffff0000u, 0xffff8000u };
    const uint32_t ssrcs[] = { 0x13572468u, 0x24681357u };
    const uint32_t step_ms = aac ? 16 : 20;
    const uint64_t sr_origin = 1788880000000ULL + 8 * 60 * 60 * 1000ULL;
    RtpInfo video(ssrcs[0], 4096, rates[0], 96, 0, 0);
    RtpInfo audio(ssrcs[1], 4096, rates[1], aac ? 97 : 0, 2, 1);
    std::string expected_audio;
    for (unsigned i = 0; i != 350; ++i) {
        const int64_t offset = i < 100 ? 0 : i < 200 ? 1000 : i < 300 ? 130000 : -1000;
        for (unsigned track = 0; track != 2; ++track) {
            uint32_t raw = starts[track] + uint64_t(i) * step_ms * rates[track] / 1000;
            if (i % 25 == 0 && (!late_sr || i >= 25)) {
                player->senderReport(track, ssrcs[track], raw, rates[track], sr_origin + i * step_ms + offset);
            }
            std::string au(aac ? 12 : 160, char(i & 255));
            if (aac) au[0] = 0x21; // Raw AAC fixture must not accidentally imitate an ADTS sync word.
            std::string payload = track == 0 ? std::string("\x41\x80\x01\x02", 4) : au;
            if (track && aac) {
                payload = std::string("\0\20\0\140", 4) + au; // 16-bit AU header, 12-byte access unit.
            }
            auto packet = (track ? audio : video).makeRtpWithStamp(
                track ? TrackAudio : TrackVideo, payload.data(), payload.size(), true, 0, raw);
            auto before = player->frames[track].size();
            player->input(packet);
            require(player->packets[track].size() == i + 1, "ordered RTP was delayed or dropped");
            require(player->frames[track].size() == before + 1, "complete RTP did not immediately reach decoder delegate");
            if (track) expected_audio += au;
            const auto &mapped = player->packets[track].back();
            require(mapped->getStamp() == raw && mapped->getSSRC() == ssrcs[track], "wire RTP identity changed");
            const auto span = int64_t(mapped->ntp_stamp) - int64_t(player->packets[track].front()->ntp_stamp);
            // Native mapping has RTCP millisecond truncation plus its existing
            // floating-point RTP-to-microsecond rounding; do not redefine it here.
            require(std::abs(span - (int64_t(i) * step_ms + (enabled ? 0 : offset))) <= (enabled ? 1 : 2),
                "SR-to-sorted-RTP mismatch: track=" + std::to_string(track) + ", packet=" + std::to_string(i)
                    + ", span=" + std::to_string(span) + ", expected="
                    + std::to_string(int64_t(i) * step_ms + (enabled ? 0 : offset)));
            if (enabled) {
                const auto frame_span = int64_t(player->frames[track].back()->pts())
                    - int64_t(player->frames[track].front()->pts());
                require(std::abs(frame_span - int64_t(i) * step_ms) <= 1,
                    "decoder lost normalized media clock after SR jump");
            }
        }
    }
    std::string actual_audio;
    for (const auto &frame : player->frames[TrackAudio]) {
        actual_audio.append(frame->data() + frame->prefixSize(), frame->size() - frame->prefixSize());
    }
    require(actual_audio == expected_audio, "G711/AAC decoder lost or duplicated payload bytes");
    player->teardown();
}

void testMixedEarlySrEpochs(const EventPoller::Ptr &poller, bool aac, bool audio_first, int64_t audio_offset_ms) {
    auto player = std::make_shared<CapturingPlayer>(poller);
    (*player)[Client::kRtspInputClock] = 1;
    player->describe(sdp(aac));
    player->finishHandshake();
    player->captureFrames();
    const uint32_t rates[] = { 90000, aac ? 64000u : 8000u };
    const uint32_t starts[] = { 0xffff0000u, 0xffff8000u };
    const uint32_t ssrcs[] = { 0x13572468u, 0x24681357u };
    const uint32_t step_ms = aac ? 16 : 20;
    const int64_t sr_origin = 1788880000000LL + 8 * 60 * 60 * 1000LL;
    RtpInfo video(ssrcs[0], 4096, rates[0], 96, 0, 0);
    RtpInfo audio(ssrcs[1], 4096, rates[1], aac ? 97 : 0, 2, 1);

    // Both tracks have a pre-RTP SR, but they straddle a source UTC step.
    // The second track must fall back to arrival, in either startup order.
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned n = 0; n != 2; ++n) {
        const unsigned track = audio_first ? 1 - n : n;
        player->senderReport(track, ssrcs[track], starts[track], rates[track],
            sr_origin + (track ? audio_offset_ms : 0));
    }
    int64_t startup_bound_ms = 0;
    std::string expected_audio;
    for (unsigned i = 0; i != 32; ++i) {
        for (unsigned n = 0; n != 2; ++n) {
            const unsigned track = audio_first ? 1 - n : n;
            const uint32_t raw = starts[track] + uint64_t(i) * step_ms * rates[track] / 1000;
            if (i == 16) {
                // A later consistent pair must not move either emitted axis.
                player->senderReport(track, ssrcs[track], raw, rates[track], sr_origin + i * step_ms);
            }
            std::string au(aac ? 12 : 160, char(i & 255));
            if (aac) au[0] = 0x21;
            std::string payload = track ? au : std::string("\x41\x80\x01\x02", 4);
            if (track && aac) payload = std::string("\0\20\0\140", 4) + au;
            auto packet = (track ? audio : video).makeRtpWithStamp(
                track ? TrackAudio : TrackVideo, payload.data(), payload.size(), true, 0, raw);
            player->input(packet);
            require(player->packets[track].size() == i + 1 && player->frames[track].size() == i + 1,
                "mixed early SR delayed a complete RTP packet or decoder frame");
            if (track) expected_audio += au;
            const auto span = int64_t(player->packets[track].back()->ntp_stamp)
                - int64_t(player->packets[track].front()->ntp_stamp);
            const auto frame_span = int64_t(player->frames[track].back()->pts())
                - int64_t(player->frames[track].front()->pts());
            require(std::abs(span - int64_t(i) * step_ms) <= 1
                    && std::abs(frame_span - int64_t(i) * step_ms) <= 1,
                "recovered SR changed a mixed-bootstrap RTP or decoder axis");
        }
        if (!i) {
            startup_bound_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - begin).count() + 2;
            require(startup_bound_ms < 250, "fixture startup scheduling cannot distinguish a one-second SR step");
        }
        const auto rtp_skew = int64_t(player->packets[1].back()->ntp_stamp)
            - int64_t(player->packets[0].back()->ntp_stamp);
        const auto frame_skew = int64_t(player->frames[1].back()->pts())
            - int64_t(player->frames[0].back()->pts());
        require(std::abs(rtp_skew) <= startup_bound_ms && std::abs(frame_skew) <= startup_bound_ms,
            "mixed early SR left a cross-track phase step: RTP=" + std::to_string(rtp_skew)
                + ", frame=" + std::to_string(frame_skew));
    }
    std::string actual_audio;
    for (const auto &frame : player->frames[TrackAudio]) {
        actual_audio.append(frame->data() + frame->prefixSize(), frame->size() - frame->prefixSize());
    }
    require(actual_audio == expected_audio, "mixed early SR lost or duplicated G711/AAC payload bytes");
    player->teardown();
}

} // namespace

int main() {
    try {
        EventPollerPool::setPoolSize(1);
        auto poller = EventPollerPool::Instance().getPoller();
        std::vector<std::string> failures;
        poller->sync([&] {
            auto run = [&](const std::string &name, const std::function<void()> &test) {
                try {
                    test();
                } catch (const std::exception &error) {
                    failures.emplace_back(name + ": " + error.what());
                }
            };
            run("eligibility", [&] { testEligibility(poller); });
            run("proxy/direct mode", [&] { testProxyModeAndDirectPath(poller); });
            run("proxy lifecycle status", [&] { testProxyStatusCallbacks(poller); });
            for (bool enabled : { false, true }) {
                for (bool aac : { false, true }) {
                    run(std::string(aac ? "AAC" : "G711") + (enabled ? " protected" : " native"),
                        [&] { testSrDecoderChain(poller, enabled, aac); });
                    if (enabled) {
                        run(std::string(aac ? "AAC" : "G711") + " late first SR",
                            [&] { testSrDecoderChain(poller, true, aac, true); });
                        for (bool audio_first : { false, true }) {
                            for (int64_t offset : { -1000, 1000 }) {
                                run(std::string(aac ? "AAC" : "G711") + " mixed early SR " + std::to_string(offset)
                                        + (audio_first ? " audio first" : " video first"),
                                    [&] { testMixedEarlySrEpochs(poller, aac, audio_first, offset); });
                            }
                        }
                    }
                }
            }
        });
        for (const auto &failure : failures) std::cerr << failure << std::endl;
        require(failures.empty(), "one or more integration cases failed");
        std::cout << "RTSP input clock integration tests passed" << std::endl;
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "RTSP input clock integration failure: " << error.what() << std::endl;
        return 1;
    }
}
