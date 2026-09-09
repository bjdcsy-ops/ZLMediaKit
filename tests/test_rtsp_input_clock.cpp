/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include "Rtsp/RtspInputClock.h"

using namespace mediakit;

namespace {
constexpr int64_t wall = 1788880000000000;
constexpr uint32_t video_ssrc = 17;
constexpr uint32_t audio_ssrc = 23;

void require(bool ok, const std::string &message) {
    if (!ok) {
        throw std::runtime_error(message);
    }
}

void testSteps(bool audio_first, int delay, int64_t initial_offset) {
    RtspInputClock clock;
    const uint32_t starts[] = {0xffff0000u, 0xffff8000u};
    const uint32_t rates[] = {90000, 64000};
    const uint32_t sources[] = {video_ssrc, audio_ssrc};
    for (int i = 0; i <= 15000; ++i) { // 300 seconds of metadata, no sleeps.
        const int64_t us = i * 20000LL;
        const int64_t offset = i < 1500 ? 0 : i < 3000 ? 1000000 : i < 4500 ? 2000000
            : i < 6000 ? -1000000 : i < 7500 ? -130000000 : i < 9000 ? 130000000 : 0;
        for (int n = 0; n < 2; ++n) {
            const unsigned track = audio_first ? 1 - n : n;
            const uint32_t rtp = starts[track] + uint64_t(us) * rates[track] / 1000000;
            if (i >= delay && i % 250 == (track ? 5 : 0)) {
                clock.inputSr(track, sources[track], rtp, wall + us + initial_offset + offset, us);
            }
            // Deliberately move the receiver's wall clock: only its first value
            // is an epoch, so this cannot alter elapsed media time.
            const auto result = clock.inputRtp(track, sources[track], rtp, rates[track], us,
                wall + us + (i > 5000 ? 129000000 : 0), track == 1, uint16_t(i));
            require(std::abs(int64_t(result) - (wall + us) / 1000) <= 1,
                "SR step or late first SR moved media track " + std::to_string(track));
        }
    }
    require(clock.getRtpResets() == 0, "SR-only injection reset RTP epoch");
    require(clock.getSrSteps() >= 8, "large SR steps were not observed");
}

void testEarlySrPhaseAndLateBootstrap() {
    RtspInputClock clock;
    // Audio and video clocks have unrelated RTP origins. Both SRs describe the
    // same instant, but audio media arrives 40 ms later with a 20 ms phase.
    clock.inputSr(0, 17, 100000, wall + 28800000000LL, 0);
    clock.inputSr(1, 23, 200000, wall + 28800000000LL, 0);
    auto video = clock.inputRtp(0, 17, 100000, 90000, 1000, wall + 1000);
    auto audio = clock.inputRtp(1, 23, 200160, 8000, 41000, wall + 41000);
    require(int64_t(audio) - int64_t(video) == 20, "early SR lost cross-track phase");
    require(clock.hasInitialSr(0) && clock.hasInitialSr(1), "early SR bootstrap not identified");
    clock.reset();
    clock.inputRtp(0, 17, 0, 90000, 0, wall);
    clock.inputSr(0, 17, 90000, wall + 28801000000LL, 1000000);
    auto output = clock.inputRtp(0, 17, 90000, 90000, 1000000, wall + 1000000);
    require(output == uint64_t(wall / 1000 + 1000), "late SR changed bootstrap epoch");
    require(!clock.hasInitialSr(0), "late SR claimed exact initial phase");
}

void testInconsistentEarlySr(bool audio_first, unsigned changed_track, int64_t offset) {
    RtspInputClock clock;
    const uint32_t starts[] = {100000, 200000};
    const uint32_t rates[] = {90000, 64000};
    const uint32_t sources[] = {video_ssrc, audio_ssrc};
    for (unsigned track = 0; track < 2; ++track) {
        clock.inputSr(track, sources[track], starts[track], wall + (track == changed_track ? offset : 0), 0);
    }
    // Both reports arrive together and describe their track's first media.
    // A one-track UTC step must not become a permanent cross-track phase.
    for (int64_t us = 0; us <= 2000000; us += 20000) {
        for (unsigned n = 0; n < 2; ++n) {
            const unsigned track = audio_first ? 1 - n : n;
            const auto raw = starts[track] + uint32_t(us * rates[track] / 1000000);
            const auto output = clock.inputRtp(track, sources[track], raw, rates[track], us, wall + us);
            require(std::abs(int64_t(output) - (wall + us) / 1000) <= 1,
                "inconsistent early SR created a permanent media phase");
        }
    }
    const unsigned first = audio_first ? 1 : 0;
    require(clock.hasInitialSr(first), "first track lost its initial SR reference");
    require(!clock.hasInitialSr(1 - first), "inconsistent second-track SR claimed an exact bootstrap");
    require(clock.getRtpResets() == 0, "inconsistent early SR reset RTP");
}

void testSeparatedEarlySr(bool audio_first, int64_t gap_us, int64_t utc_offset) {
    RtspInputClock clock;
    const uint32_t starts[] = {100000, 200000};
    const uint32_t rates[] = {90000, 64000};
    const uint32_t sources[] = {video_ssrc, audio_ssrc};
    const unsigned first = audio_first ? 1 : 0;
    const unsigned second = 1 - first;
    clock.inputSr(first, sources[first], starts[first], wall + utc_offset, 0);
    clock.inputRtp(first, sources[first], starts[first], rates[first], 0, wall);
    // The next track's SR is genuinely several seconds newer. Compare elapsed
    // sender and receiver time, rather than rejecting the absolute NTP gap.
    clock.inputSr(second, sources[second], starts[second], wall + utc_offset + gap_us, gap_us);
    const auto media_us = gap_us + 20000;
    const auto first_raw = starts[first] + uint32_t(media_us * rates[first] / 1000000);
    const auto first_output = clock.inputRtp(first, sources[first], first_raw, rates[first], media_us, wall + media_us);
    const auto second_raw = starts[second] + rates[second] / 50;
    const auto second_output = clock.inputRtp(second, sources[second], second_raw, rates[second],
        gap_us + 40000, wall + gap_us + 40000);
    require(first_output == uint64_t((wall + media_us) / 1000) && second_output == first_output,
        "normal inter-track SR interval lost the source media phase");
    require(clock.hasInitialSr(first) && clock.hasInitialSr(second), "compatible delayed SR was rejected");
    require(clock.getRtpResets() == 0, "normal inter-track SR interval reset RTP");
}

void testGapsResetAndBFrames() {
    RtspInputClock clock;
    auto output = [&](uint32_t ssrc, uint32_t raw, int64_t us) {
        return clock.inputRtp(0, ssrc, raw, 90000, us, wall + us);
    };
    const auto first = output(17, 0xffffff00, 0);
    auto wrap = uint32_t(0xffffff00 + 9000u);
    require(output(17, wrap, 100000) == first + 100, "32-bit RTP wrap changed span");
    require(output(17, wrap - 3600, 110000) == first + 60, "B-frame presentation order clamped");
    require(output(17, wrap, 115000) == first + 100, "same timestamp changed on another fragment");
    require(output(17, wrap + 279000, 3200000) == first + 3200, "real 3.1 second gap was erased");
    require(clock.getRtpResets() == 0, "true RTP gap was misclassified as restart");
    auto next = output(29, 7000, 4000000);
    require(next == first + 4000 && clock.getRtpResets() == 1, "SSRC restart kept the old epoch");
    clock.inputSr(0, 17, 7000, wall + 900000000, 4000000);
    require(output(29, 7900, 4010000) == next + 10, "old SSRC SR polluted current track");
}

void testSmallAudioReset(bool batched, bool sequence_restart) {
    RtspInputClock clock;
    const uint32_t origin = 0xffffe000u;
    const int reset_packet = 26;
    const int confirmation = reset_packet + (batched ? 6 : 2);
    for (int packet = 0; packet <= 200; ++packet) {
        const int64_t us = packet * 40000LL;
        const auto video = clock.inputRtp(0, video_ssrc, packet * 3600, 90000, us, wall + us);
        const auto after_reset = packet - reset_packet;
        const auto source_packet = after_reset < 0 ? packet : batched ? after_reset / 3 * 3 : after_reset;
        const auto sequence = uint16_t(sequence_restart && after_reset >= 0 ? 65534 + after_reset : 65510 + packet);
        const auto raw = origin + source_packet * 320;
        const auto audio = clock.inputRtp(1, audio_ssrc, raw, 8000, us, wall + us, true, sequence);
        require(clock.getRtpResets() == uint64_t(packet >= confirmation),
            "small audio reset was not confirmed by exactly three progressing timestamps");
        if (packet >= confirmation) {
            const auto batch_phase_ms = batched ? after_reset % 3 * 40 : 0;
            require(std::abs(int64_t(audio) - int64_t(video) + batch_phase_ms) <= 1,
                "confirmed same-SSRC audio reset left a permanent cross-track phase");
        }
    }
}

void testAudioResetProbation() {
    for (bool audio : { false, true }) {
        RtspInputClock clock;
        uint16_t sequence = 100;
        const auto input = [&](uint32_t raw, int64_t us) {
            return clock.inputRtp(0, audio_ssrc, raw, 8000, us, wall + us, audio, sequence++);
        };
        input(0, 0);
        input(8000, 1000000);
        // A historical packet followed by the current axis must erase probation.
        input(0, 1040000);
        require(input(8640, 1080000) == uint64_t(wall / 1000 + 1080), "old packet changed the forward axis");
        input(320, 1120000);
        require(input(9280, 1160000) == uint64_t(wall / 1000 + 1160), "cancelled audio probation leaked into the next probe");
        require(clock.getRtpResets() == 0, "single historical packets confirmed an audio restart");
        // A real new axis wraps its sequence while being confirmed. Video keeps
        // presentation reordering semantics even for this same metadata input.
        sequence = 65534;
        input(0, 1200000);
        input(320, 1240000);
        const auto result = input(640, 1280000);
        require(clock.getRtpResets() == uint64_t(audio), "audio probation affected video or missed sequence wrap");
        require(result == uint64_t(wall / 1000 + (audio ? 1280 : 80)), "wrong media phase after probation");
    }
    RtspInputClock repeated;
    repeated.inputRtp(0, audio_ssrc, 0, 8000, 0, wall, true, 0);
    repeated.inputRtp(0, audio_ssrc, 8000, 8000, 1000000, wall + 1000000, true, 1);
    for (int i = 0; i != 10; ++i) {
        repeated.inputRtp(0, audio_ssrc, 0, 8000, 1040000 + i * 20000, wall, true, 2 + i);
    }
    require(repeated.getRtpResets() == 0, "shared timestamp packets counted as progressing restart evidence");
    RtspInputClock batching;
    for (int packet = 0; packet != 200; ++packet) {
        const int64_t us = packet * 20000LL;
        batching.inputRtp(0, audio_ssrc, 0xfffffe00u + (packet / 4 * 4) * 160,
            8000, us, wall + us, true, uint16_t(65530 + packet));
    }
    require(batching.getRtpResets() == 0, "normal G711 shared timestamps or RTP/sequence wrap triggered recovery");
}

void testFrequencyWithSrQuantization(int ppm, bool quantized, int sr_interval = 250) {
    RtspInputClock clock;
    const auto ratio = 1.0 + ppm / 1000000.0;
    int64_t largest_error = 0;
    int64_t first_error = 0, last_error = 0;
    for (int i = 0; i <= 90000; ++i) { // Thirty minutes, AAC clock at 64 kHz.
        const int64_t us = i * 20000LL;
        const auto raw = uint32_t(std::llround(us * 0.064 * ratio));
        if (i % sr_interval == 0) {
            // SR RTP is quantized to an AAC sample frame while NTP is not.
            // This reproduces 16 ms-scale phase noise without removing drift.
            const auto sr_rtp = quantized ? raw / 1024 * 1024 : raw;
            clock.inputSr(0, 23, sr_rtp, wall + us, us);
        }
        const auto result = clock.inputRtp(0, 23, raw, 64000, us, wall + us);
        const auto error = int64_t(result) - (wall + us) / 1000;
        if (i == 45000) {
            first_error = error;
        }
        if (i >= 45000) {
            largest_error = std::max(largest_error, int64_t(std::abs(error)));
        }
        last_error = error;
    }
    std::cout << "frequency ppm=" << ppm << " quantized=" << quantized << " SR interval=" << sr_interval * 20 << " ms"
              << " late max error=" << largest_error << " ms, trend=" << last_error - first_error << " ms\n";
    require(std::abs(last_error - first_error) <= 5, "normal sampling frequency drift accumulated");
    require(largest_error <= 10, "SR quantization created excessive phase error");
}

void testSmallSrSteps(int64_t step_us, bool repeated) {
    RtspInputClock reference, stepped;
    for (int64_t us = 0; us <= 1800000000LL; us += 20000) {
        const auto raw = uint32_t(std::llround(us * 0.064 * 1.0001));
        if (us % 5000000 == 0) {
            const auto count = us < 600000000 ? 0 : repeated ? 1 + (us - 600000000) / 60000000 : 1;
            stepped.inputSr(0, audio_ssrc, raw, wall + us + count * step_us, us);
        }
        const auto expected = reference.inputRtp(0, audio_ssrc, raw, 64000, us, wall + us);
        const auto actual = stepped.inputRtp(0, audio_ssrc, raw, 64000, us, wall + us);
        require(actual == expected, "small or repeated SR steps changed the continuous clock");
    }
    require(stepped.getRtpResets() == 0, "small SR steps reset the RTP epoch");
}

void testBatchedArrivalFrequency(int ppm, uint32_t seed) {
    RtspInputClock clock;
    uint32_t random = seed;
    int64_t last_arrival = 0, first = 0, worst = 0, at_fifteen_minutes = 0, final_error = 0;
    for (int64_t us = 0; us <= 7200000000LL; us += 20000) { // Two hours, metadata only.
        // Explicit xorshift32 keeps the fixture identical on libc++ and libstdc++.
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        const int64_t batch = (us + 199999) / 200000 * 200000;
        // Deliver 200 ms batches with up to 40 ms extra jitter, retaining TCP order.
        const auto arrival = std::max(last_arrival, batch + random % 40001);
        last_arrival = arrival;
        const auto raw = uint32_t(std::llround(us * 0.064 * (1.0 + ppm / 1000000.0)));
        const auto output = clock.inputRtp(0, audio_ssrc, raw, 64000, arrival, wall + arrival,
            true, uint16_t(us / 20000));
        if (!us) {
            first = output;
        }
        final_error = int64_t(output) - first - us / 1000;
        if (us == 900000000) {
            at_fifteen_minutes = final_error;
        }
        if (us >= 900000000) {
            worst = std::max(worst, int64_t(std::abs(final_error)));
        }
    }
    std::cout << "batched arrival seed=" << seed << " ppm=" << ppm << " late max=" << worst
              << " ms, change from 15 min to 2 h=" << final_error - at_fifteen_minutes << " ms\n";
    require(worst <= 10, "bounded batching jitter created excessive phase error");
    require(std::abs(final_error - at_fifteen_minutes) <= 5, "bounded batching jitter caused ongoing drift");
    require(clock.getRtpResets() == 0, "bounded batching jitter reset the RTP epoch");
}

void testLateFrequencyLearning() {
    RtspInputClock clock;
    int64_t last_error = 0;
    for (int64_t us = 0; us <= 900000000; us += 20000) {
        // Eight completed observations are unavailable before 360 s. RTP still
        // advances across the gaps, so this is one epoch, not repeated restarts.
        if (us < 360000000 && us % 45000000) {
            continue;
        }
        const auto raw = uint32_t(std::llround(us * 0.064 * 1.0001));
        const auto output = clock.inputRtp(0, audio_ssrc, raw, 64000, us, wall + us);
        last_error = int64_t(output) - (wall + us) / 1000;
    }
    require(clock.getRtpResets() == 0, "sparse advancing RTP was treated as a restart");
    require(std::abs(last_error - 36) <= 1, "late rate estimate retroactively corrected history older than 300 s");
}

void testCorrectionWithRepeatedPackets() {
    RtspInputClock reference, probed;
    uint32_t previous = 100000;
    for (int64_t us = 0; us <= 180000000; us += 20000) {
        const auto raw = 100000 + uint32_t(std::llround(us * 0.09 * 1.0001));
        const auto expected = reference.inputRtp(0, video_ssrc, raw, 90000, us, wall + us);
        const auto actual = probed.inputRtp(0, video_ssrc, raw, 90000, us, wall + us);
        require(actual == expected, "extra RTP fragments or B frames consumed initial correction");
        if (us) {
            const auto older = probed.inputRtp(0, video_ssrc, previous, 90000, us + 1, wall + us + 1);
            require(older <= actual && actual - older <= 22, "B-frame presentation timestamp was clamped");
        }
        for (int n = 0; n < 3; ++n) {
            require(probed.inputRtp(0, video_ssrc, raw, 90000, us + n + 2, wall + us + n + 2) == actual,
                "same RTP fragment changed timestamp");
        }
        if (us && us % 5000000 == 0) {
            const auto offset = us % 10000000 ? 28800000000LL : -28800000000LL;
            probed.inputSr(0, video_ssrc, raw, wall + us + offset, us + 5);
            require(probed.inputRtp(0, video_ssrc, raw, 90000, us + 6, wall + us + 6) == actual,
                "SR changed an already emitted timestamp");
        }
        previous = raw;
    }
    require(probed.getRtpResets() == 0, "repeated RTP metadata reset the epoch");
}

void testEarlySrWithHistoricalVideo(int64_t utc_offset) {
    RtspInputClock clock;
    // Both reports describe current media. Only video replays a four-second GOP.
    clock.inputSr(0, video_ssrc, 360000, wall + utc_offset, 0);
    clock.inputSr(1, audio_ssrc, 256000, wall + utc_offset, 0);
    const auto first = clock.inputRtp(0, video_ssrc, 0, 90000, 1000, wall + 1000);
    auto video = first;
    for (int ms = 40; ms <= 4000; ms += 40) {
        const auto arrival = 1000 + ms * 25; // Deliver four seconds in 100 ms.
        video = clock.inputRtp(0, video_ssrc, ms * 90, 90000, arrival, wall + arrival);
    }
    const auto audio = clock.inputRtp(1, audio_ssrc, 256000, 64000, 102000, wall + 102000);
    require(first == uint64_t(wall / 1000 - 4000), "early SR lost historical GOP phase");
    require(video == uint64_t(wall / 1000) && audio == video, "early SR misaligned current audio and video");
    require(clock.hasInitialSr(0) && clock.hasInitialSr(1), "historical media bootstrap was not identified");
    require(clock.getRtpResets() == 0, "historical GOP delivery reset the RTP epoch");
}

void testLaterFrequencyChange(int ppm) {
    RtspInputClock clock;
    int64_t settled_error = 0, last_error = 0;
    for (int64_t us = 0; us <= 1800000000LL; us += 20000) {
        const auto changed_us = std::max<int64_t>(0, us - 600000000);
        const auto source_us = us + changed_us * ppm / 1000000.0;
        const auto raw = uint32_t(std::llround(source_us * 0.064));
        const auto output = clock.inputRtp(0, audio_ssrc, raw, 64000, us, wall + us);
        last_error = int64_t(output) - (wall + us) / 1000;
        if (us == 599980000) {
            require(last_error == 0, "nominal media moved before the frequency change");
        }
        if (us == 1000000000) {
            settled_error = last_error;
        }
    }
    // A real frequency change has finite estimator response time (currently
    // about 16–17 ms phase). This is a boundary test, not a <=10 ms acceptance.
    std::cout << "later frequency change ppm=" << ppm << " retained phase=" << last_error << " ms\n";
    require(last_error * ppm >= 0 && std::abs(last_error) <= 20, "later rate estimate reapplied startup correction");
    require(std::abs(last_error - settled_error) <= 1, "stable frequency continued drifting after convergence");
}
}

int main() {
    try {
        for (bool audio_first : {false, true}) {
            for (int delay : {0, 250, 1500}) {
                for (int64_t offset : {0LL, 28800000000LL, -28800000000LL}) {
                    testSteps(audio_first, delay, offset);
                }
            }
        }
        testEarlySrPhaseAndLateBootstrap();
        for (bool audio_first : {false, true}) {
            for (unsigned changed_track : {0u, 1u}) {
                for (int64_t offset : {-28800000000LL, -130000000LL, -1000000LL,
                                      1000000LL, 130000000LL, 28800000000LL}) {
                    testInconsistentEarlySr(audio_first, changed_track, offset);
                }
            }
            for (int64_t gap : {3000000LL, 5000000LL, 9000000LL}) {
                for (int64_t offset : {0LL, 28800000000LL, -28800000000LL}) {
                    testSeparatedEarlySr(audio_first, gap, offset);
                }
            }
        }
        testGapsResetAndBFrames();
        for (bool batched : { false, true }) {
            for (bool sequence_restart : { false, true }) {
                testSmallAudioReset(batched, sequence_restart);
            }
        }
        testAudioResetProbation();
        testLateFrequencyLearning();
        testCorrectionWithRepeatedPackets();
        for (int64_t offset : {0LL, 28800000000LL, -28800000000LL}) {
            testEarlySrWithHistoricalVideo(offset);
        }
        for (int64_t step : {-100000LL, -32000LL, -16000LL, 16000LL, 32000LL, 100000LL}) {
            testSmallSrSteps(step, false);
            testSmallSrSteps(step, true);
        }
        // A 2.5 s SR period previously turned AAC quantization into a ratchet,
        // even at zero frequency error; the existing 5 s fixture missed it.
        testFrequencyWithSrQuantization(0, true, 125);
        for (int ppm : {-100, 100}) {
            testFrequencyWithSrQuantization(ppm, false);
            testFrequencyWithSrQuantization(ppm, true);
            testLaterFrequencyChange(ppm);
        }
        for (uint32_t seed : {1u, 17u, 43u, 101u}) {
            for (int ppm : {-100, 0, 100}) {
                testBatchedArrivalFrequency(ppm, seed);
            }
        }
        std::cout << "RTSP input clock tests passed\n";
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
