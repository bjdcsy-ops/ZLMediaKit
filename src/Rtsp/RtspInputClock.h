/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <array>
#include <cstdint>

namespace mediakit {

// A live RTSP session's media clock. It consumes metadata after the existing
// RTP sorter; it never owns packets, waits for another track, or reads a clock.
// Explicit clock arguments also make SR/arrival ordering reproducible in tests.
class RtspInputClock {
public:
    void reset();
    uint64_t inputRtp(unsigned track, uint32_t ssrc, uint32_t rtp, uint32_t rate,
                      int64_t arrival_us, int64_t wall_us, bool audio = false,
                      uint16_t sequence = 0);
    void inputSr(unsigned track, uint32_t ssrc, uint32_t rtp, int64_t ntp_us,
                 int64_t arrival_us);

    uint64_t getSrSteps() const { return _sr_steps; }
    uint64_t getRtpResets() const { return _rtp_resets; }
    bool hasInitialSr(unsigned track) const { return _tracks.at(track).initial_sr; }

private:
    struct Report {
        bool valid = false;
        uint32_t ssrc = 0;
        uint32_t rtp = 0;
        int64_t ntp_us = 0;
        int64_t arrival_us = 0;
    };

    struct Track {
        bool started = false;
        bool initial_sr = false;
        uint32_t ssrc = 0;
        uint32_t rate = 0;
        uint32_t rtp = 0;
        int64_t ticks = 0;
        int64_t arrival_us = 0;
        double origin_us = 0;
        double media_us = 0;
        double scale = 1;
        bool rate_established = false;
        double initial_correction_us = 0;
        Report last;
        Report pending;
        struct Restart {
            bool valid = false;
            uint16_t sequence = 0;
            uint32_t first_rtp = 0;
            uint32_t previous_rtp = 0;
            int64_t arrival_us = 0;
            unsigned progressing = 0;
        } restart;
        struct Sample {
            double rtp_us = 0;
            int64_t arrival_us = 0;
        };
        // One minimum-delay observation per completed five-second interval.
        // Only metadata is retained; media is delivered immediately.
        std::array<Sample, 64> samples;
        unsigned sample_count = 0;
        unsigned next_sample = 0;
        int64_t bucket = -1;
        Sample minimum;
    };

    static double mapped(const Track &track, uint32_t rtp);
    static void observeArrival(Track &track, int64_t arrival_us);
    static void estimateRate(Track &track);
    bool compatibleReport(const Report &report) const;
    void setReportOrigin(const Report &report, double media_us);
    void anchorReport(Track &track, const Report &report);

    std::array<Track, 2> _tracks;
    bool _started = false;
    bool _have_sr_origin = false;
    int64_t _arrival_origin_us = 0;
    int64_t _wall_origin_us = 0;
    double _sr_offset_us = 0;
    int64_t _sr_origin_ntp_us = 0;
    int64_t _sr_origin_arrival_us = 0;
    uint64_t _sr_steps = 0;
    uint64_t _rtp_resets = 0;
};

} // namespace mediakit
