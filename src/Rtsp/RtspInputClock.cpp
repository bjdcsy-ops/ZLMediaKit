/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 * SPDX-License-Identifier: MIT
 */

#include "RtspInputClock.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mediakit {
namespace {
constexpr double kUsPerSecond = 1000000;
constexpr double kMaxRateError = 0.001;
constexpr double kInitialCorrectionRate = 0.001;
constexpr double kMaxInitialCorrectionUs = 50000;
constexpr double kMaxLearningUs = 300 * kUsPerSecond;
constexpr double kSrStepUs = 250000; // Above one quantized 1024-sample AAC AU, including 8 kHz.
constexpr int64_t kPendingLifetimeUs = 10000000;
constexpr double kRtpResetUs = 3000000;
}

void RtspInputClock::reset() {
    *this = RtspInputClock();
}

double RtspInputClock::mapped(const Track &track, uint32_t rtp) {
    return track.media_us + int64_t(int32_t(rtp - track.rtp)) * kUsPerSecond / track.rate * track.scale;
}

void RtspInputClock::observeArrival(Track &track, int64_t arrival_us) {
    Track::Sample observation;
    observation.rtp_us = track.ticks * kUsPerSecond / track.rate;
    observation.arrival_us = arrival_us;
    const auto bucket = arrival_us / 5000000;
    if (track.bucket != bucket) {
        if (track.bucket >= 0) {
            track.samples[track.next_sample] = track.minimum;
            track.next_sample = (track.next_sample + 1) % track.samples.size();
            track.sample_count = std::min<unsigned>(track.sample_count + 1, track.samples.size());
            estimateRate(track);
        }
        track.bucket = bucket;
        track.minimum = observation;
    } else if (double(arrival_us - track.minimum.arrival_us) < observation.rtp_us - track.minimum.rtp_us) {
        track.minimum = observation;
    }
}

void RtspInputClock::estimateRate(Track &track) {
    if (track.sample_count < 8) {
        return;
    }
    const auto &origin = track.samples[0];
    std::array<unsigned, 64> selected;
    for (unsigned i = 0; i < track.sample_count; ++i) {
        selected[i] = i;
    }
    double scale = 1, mean_x = 0, mean_y = 0;
    const auto fit = [&](unsigned count) {
        mean_x = mean_y = 0;
        double min_x = track.samples[selected[0]].rtp_us - origin.rtp_us, max_x = min_x;
        for (unsigned i = 0; i < count; ++i) {
            const auto &sample = track.samples[selected[i]];
            const auto x = sample.rtp_us - origin.rtp_us;
            mean_x += x;
            mean_y += double(sample.arrival_us - origin.arrival_us);
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
        }
        mean_x /= count;
        mean_y /= count;
        double variance = 0, covariance = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto &sample = track.samples[selected[i]];
            const auto x = sample.rtp_us - origin.rtp_us - mean_x;
            const auto y = double(sample.arrival_us - origin.arrival_us) - mean_y;
            variance += x * x;
            covariance += x * y;
        }
        if (variance <= 0) {
            scale = 1;
            return false;
        }
        scale = covariance / variance;
        double residual_squared = 0;
        for (unsigned i = 0; i < count; ++i) {
            const auto &sample = track.samples[selected[i]];
            const auto x = sample.rtp_us - origin.rtp_us - mean_x;
            const auto y = double(sample.arrival_us - origin.arrival_us) - mean_y;
            const auto residual = y - scale * x;
            residual_squared += residual * residual;
        }
        const auto uncertainty = 3 * std::sqrt(residual_squared / (count - 2) / variance);
        return max_x - min_x >= 30 * kUsPerSecond && std::isfinite(scale)
            && std::abs(scale - 1) <= kMaxRateError && uncertainty <= 0.000025;
    };
    if (!fit(track.sample_count)) {
        // Changing network queue levels must not keep a cold session forever
        // at nominal rate. Fit the low-delay observations again, without
        // integrating the queue step or discarding every previous sample.
        const auto residual = [&](unsigned index) {
            const auto &sample = track.samples[index];
            return double(sample.arrival_us - origin.arrival_us) - mean_y
                - scale * (sample.rtp_us - origin.rtp_us - mean_x);
        };
        std::sort(selected.begin(), selected.begin() + track.sample_count,
                  [&](unsigned left, unsigned right) { return residual(left) < residual(right); });
        auto count = std::max(8u, track.sample_count / 4);
        const auto threshold = residual(selected[count - 1]);
        while (count < track.sample_count && residual(selected[count]) <= threshold) {
            ++count;
        }
        if (!fit(count)) {
            return;
        }
    }
    // Only slope is applied, continuously at the last media frontier. Neither
    // fitted intercept nor SR UTC can create a phase correction/debt.
    if (!track.rate_established) {
        track.rate_established = true;
        const auto learning_us = track.ticks * kUsPerSecond / track.rate;
        if (learning_us <= kMaxLearningUs) {
            // Once only: account for the frequency error accumulated while
            // observations were insufficient. Bound both age and total amount;
            // later rate estimates and SRs cannot repeatedly rewrite history.
            track.initial_correction_us = std::max(-kMaxInitialCorrectionUs,
                std::min(learning_us * (scale - 1), kMaxInitialCorrectionUs));
        }
    }
    track.scale = scale;
}

bool RtspInputClock::compatibleReport(const Report &report) const {
    // Check SR epoch consistency separately from the age of a cached GOP.
    // Two first reports straddling a camera clock step cannot establish a
    // trustworthy cross-track phase, even when both arrived very recently.
    return !_have_sr_origin || std::abs(double(report.ntp_us - _sr_origin_ntp_us)
        - double(report.arrival_us - _sr_origin_arrival_us)) < kSrStepUs;
}

void RtspInputClock::setReportOrigin(const Report &report, double media_us) {
    _have_sr_origin = true;
    _sr_offset_us = media_us - report.ntp_us;
    _sr_origin_ntp_us = report.ntp_us;
    _sr_origin_arrival_us = report.arrival_us;
}

void RtspInputClock::anchorReport(Track &track, const Report &report) {
    track.last = report;
    track.pending = Report();
    if (!_have_sr_origin) {
        setReportOrigin(report, mapped(track, report.rtp));
    }
}

uint64_t RtspInputClock::inputRtp(unsigned index, uint32_t ssrc, uint32_t rtp, uint32_t rate,
                                  int64_t arrival_us, int64_t wall_us, bool audio,
                                  uint16_t sequence) {
    if (!rate) {
        throw std::invalid_argument("RTSP input clock requires an RTP clock rate");
    }
    auto &track = _tracks.at(index);
    if (!_started) {
        _started = true;
        _arrival_origin_us = arrival_us;
        _wall_origin_us = wall_us;
    }
    // Keep arithmetic relative to the session. Repeatedly adding fractional
    // samples to a Unix epoch stored as double would itself accumulate error.
    const auto arrival_media_us = double(arrival_us - _arrival_origin_us);
    auto delta = int64_t(int32_t(rtp - track.rtp));
    const auto elapsed = track.started ? delta * kUsPerSecond / track.rate : 0;
    // A real pause with advancing RTP is a gap, not a reset. Small negative
    // PTS differences (B frames/batched audio) do not rewind forward state.
    auto reset = track.started && (ssrc != track.ssrc || rate != track.rate
        || (delta && std::abs(elapsed - (arrival_us - track.arrival_us)) > kRtpResetUs));
    if (audio && track.started && !reset) {
        auto &restart = track.restart;
        if (restart.valid) {
            const auto advance = int32_t(rtp - restart.previous_rtp);
            const auto source_us = int64_t(int32_t(rtp - restart.first_rtp)) * kUsPerSecond / rate;
            const auto received_us = arrival_us - restart.arrival_us;
            if (uint16_t(sequence - restart.sequence) != 1 || advance < 0 || received_us < 0
                || received_us > kRtpResetUs || std::abs(source_us - received_us) >= kSrStepUs) {
                restart.valid = false;
            } else {
                restart.sequence = sequence;
                restart.previous_rtp = rtp;
                // Repeated timestamps can contain distinct G711 samples, but
                // are not independent evidence of a progressing new RTP axis.
                if (advance > 0 && ++restart.progressing == 3) {
                    reset = true;
                }
            }
        }
        if (!restart.valid && elapsed < -kSrStepUs) {
            // The existing sorter may resume a same-SSRC sender without an
            // explicit restart event. Confirm a small backwards audio reset
            // across three advancing, sequence-contiguous packets. A single
            // historical packet followed by the old axis cancels this probe.
            // Only metadata is retained; even probation media is sent now.
            restart.valid = true;
            restart.sequence = sequence;
            restart.first_rtp = restart.previous_rtp = rtp;
            restart.arrival_us = arrival_us;
            restart.progressing = 1;
        }
    }
    if (!track.started || reset) {
        const auto pending = track.pending;
        if (reset) {
            ++_rtp_resets;
        }
        track = Track();
        track.started = true;
        track.ssrc = ssrc;
        track.rate = rate;
        track.rtp = rtp;
        track.arrival_us = arrival_us;
        track.origin_us = arrival_media_us;
        track.media_us = track.origin_us;
        if (pending.valid && pending.ssrc == ssrc && arrival_us >= pending.arrival_us
            && arrival_us - pending.arrival_us <= kPendingLifetimeUs) {
            const auto source_us = double(pending.ntp_us)
                + int64_t(int32_t(rtp - pending.rtp)) * kUsPerSecond / rate;
            const auto compatible = compatibleReport(pending);
            if (!_have_sr_origin) {
                setReportOrigin(pending, double(pending.arrival_us - _arrival_origin_us));
            }
            const auto mapped = source_us + _sr_offset_us;
            // An early, compatible SR can establish cross-track phase before
            // any media from this track has escaped. Late SR cannot rewrite it.
            if (compatible && std::abs(mapped - arrival_media_us) <= kPendingLifetimeUs) {
                track.origin_us = mapped;
                track.media_us = mapped;
                track.initial_sr = true;
            }
            anchorReport(track, pending);
        }
        observeArrival(track, arrival_us);
        return uint64_t(std::max<int64_t>(0, _wall_origin_us + int64_t(std::floor(track.origin_us))) / 1000);
    }

    if (delta > 0) {
        const auto budget = elapsed * kInitialCorrectionRate;
        const auto correction = std::max(-budget, std::min(track.initial_correction_us, budget));
        track.initial_correction_us -= correction;
        track.media_us += elapsed * track.scale + correction;
        track.ticks += delta;
        track.rtp = rtp;
        track.arrival_us = arrival_us;
        observeArrival(track, arrival_us);
    }
    return uint64_t(std::max<int64_t>(0, _wall_origin_us + int64_t(std::floor(mapped(track, rtp)))) / 1000);
}

void RtspInputClock::inputSr(unsigned index, uint32_t ssrc, uint32_t rtp, int64_t ntp_us,
                             int64_t arrival_us) {
    auto &track = _tracks.at(index);
    if (ntp_us <= 0) {
        return;
    }
    Report report;
    report.valid = true;
    report.ssrc = ssrc;
    report.rtp = rtp;
    report.ntp_us = ntp_us;
    report.arrival_us = arrival_us;
    if (!track.started) {
        track.pending = report; // At most one pre-RTP SR per negotiated track.
        return;
    }
    if (ssrc != track.ssrc || std::abs(int64_t(int32_t(rtp - track.rtp)) * kUsPerSecond / track.rate)
        > kPendingLifetimeUs) {
        return;
    }
    if (!track.last.valid) {
        anchorReport(track, report);
        return;
    }
    if (arrival_us <= track.last.arrival_us || int32_t(rtp - track.last.rtp) <= 0) {
        return; // Duplicate or delayed SR, not a negative wall-clock step.
    }
    const auto elapsed = int64_t(int32_t(rtp - track.last.rtp)) * kUsPerSecond / track.rate;
    if (std::abs(double(ntp_us - track.last.ntp_us) - elapsed * track.scale) >= kSrStepUs) {
        // This counter is diagnostic only: source UTC changes must not alter
        // the continuous receiver clock, its rate, or its observation window.
        ++_sr_steps;
        anchorReport(track, report);
        return;
    }
    track.last = report;
}

} // namespace mediakit
