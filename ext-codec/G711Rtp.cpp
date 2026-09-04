#include "G711Rtp.h"
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace mediakit {

namespace {

constexpr uint32_t kMaxBounceMs = 20;
constexpr int64_t kNtpDeadbandUs = 1000;
constexpr uint64_t kNtpSlewUsPerSecond = 1000;
constexpr int64_t kNtpDiscontinuityUs = 250000;
constexpr unsigned kNtpDiscontinuitySeconds = 2;

// Divide before multiplying so an epoch-based sample stamp cannot overflow.
int64_t samplesToUs(int64_t samples, int rate) {
    return samples / rate * 1000000 + samples % rate * 1000000 / rate;
}

void checkAudioInfo(int sample_rate, int channels) {
    if (sample_rate <= 0 || channels <= 0 || channels > std::numeric_limits<uint16_t>::max()
        || uint64_t(sample_rate) * channels > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("Invalid G711 RTP audio parameters");
    }
}

} // namespace

void G711RtpDecoder::setAudioInfo(int sample_rate, int channels) {
    checkAudioInfo(sample_rate, channels);
    if (_have_clock && (_sample_rate != sample_rate || _channels != channels)) {
        WarnL << "G711 RTP discontinuity: audio parameters changed, seq=" << _seq << ", SSRC=" << _ssrc;
        _have_clock = false;
        _restart_confirmed = true;
        _restart_packet.reset();
        _recent_count = _recent_pos = 0;
    }
    _sample_rate = sample_rate;
    _channels = channels;
}

bool G711RtpDecoder::inputRtp(const RtpPacket::Ptr &rtp, bool) {
    auto size = rtp->getPayloadSize();
    if (size <= 0 || size % _channels || !rtp->sample_rate) {
        return false;
    }
    if (_sample_rate != int(rtp->sample_rate)) {
        setAudioInfo(rtp->sample_rate, _channels);
    }
    const auto samples = uint64_t(size / _channels);
    const auto bounce_limit = std::min<uint64_t>(uint64_t(_sample_rate) * kMaxBounceMs / 1000, samples / 2);
    const auto seq = rtp->getSeq();
    const auto ssrc = rtp->getSSRC();
    const auto raw_stamp = rtp->getStamp();
    auto seq_delta = int16_t(seq - _seq);
    auto raw_delta = int64_t(int32_t(raw_stamp - _expected_raw_stamp));
    bool restart = _restart_confirmed || (_have_clock && ssrc != _ssrc);
    _restart_confirmed = false;
    // A reset such as 60000 -> 1000 has a positive signed sequence delta.
    // Forward packet loss cannot explain a backwards sample clock beyond the
    // phase bound; confirm that case before applying its old/new RTP offset.
    const auto backward_clock_gap = seq_delta > 1 && raw_delta < -int64_t(bounce_limit);
    if (_have_clock && !restart && (seq_delta <= 0 || backward_clock_gap)) {
        // Normal input is already RTP-sorted. Reject duplicate/late packets,
        // but confirm a same-SSRC sequence restart after two progressing packets
        // rather than rejecting a rebooted sender forever. Only this abnormal
        // branch holds one probation packet; confirmed restarts lose no samples.
        if (!seq_delta && raw_stamp == _last_raw_stamp) {
            return false;
        }
        // Two historical late packets must not confirm a new sender. Match
        // actual recent packet metadata, never extrapolate variable payloads.
        // A reboot replaying the same seq/stamp/sample interval is deliberately
        // indistinguishable from old media until it exits this bounded window.
        if (-int(seq_delta) <= int(_recent_packets.size())) {
            for (size_t i = 0; i < _recent_count; ++i) {
                const auto &old = _recent_packets[i];
                if (old.seq == seq && old.stamp == raw_stamp && old.samples == samples) {
                    return false;
                }
            }
        }
        if (_restart_packet && _restart_packet->getSSRC() == ssrc
            && uint16_t(seq - _restart_packet->getSeq()) == 1) {
            const auto previous_samples = uint64_t(_restart_packet->getPayloadSize() / _channels);
            const auto previous_limit = std::min<uint64_t>(uint64_t(_sample_rate) * kMaxBounceMs / 1000, previous_samples / 2);
            const auto pair_delta = int64_t(int32_t(raw_stamp - uint32_t(_restart_packet->getStamp() + previous_samples)));
            // Before a new sample axis is anchored, the pair error is the
            // difference of two bounded phases, so allow both packet limits.
            // Sequence progression, not strict raw stamp growth, proves order.
            if (uint64_t(std::abs(pair_delta)) <= previous_limit + bounce_limit) {
                auto first = std::move(_restart_packet);
                _restart_confirmed = true;
                inputRtp(first, false);
                return inputRtp(rtp, false);
            }
        }
        _restart_packet = rtp;
        return false;
    }
    _restart_packet.reset();

    // Bound phase error against the cumulative sample axis, not by packet count.
    // A small fixed offset cannot be distinguished from a longer excursion when
    // sequence coverage and samples are complete. Normalize both without moving
    // the expected axis; accumulated drift still crosses this amplitude bound.
    bool discontinuity = restart || (_have_clock && seq_delta != 1);
    if (_have_clock && !discontinuity && raw_delta) {
        discontinuity = uint64_t(std::abs(raw_delta)) > bounce_limit;
    }

    int64_t ntp_us = rtp->getStampMS() * 1000;
    if (!_have_clock || discontinuity) {
        if (_have_clock) {
            WarnL << "G711 RTP discontinuity: delta=" << raw_delta << " samples, seq=" << seq
                  << " (previous=" << _seq << "), SSRC=" << ssrc << " (previous=" << _ssrc << ")";
        }
        if (!_have_clock || restart) {
            _next_sample_stamp = rtp->getStampMS() * uint64_t(_sample_rate) / 1000;
            _recent_count = _recent_pos = 0;
        } else {
            _next_sample_stamp = std::max<int64_t>(0, int64_t(_next_sample_stamp) + raw_delta);
        }
        _ntp_anchor_sample = _next_sample_stamp;
        _ntp_anchor_us = ntp_us;
        _slew_remainder = 0;
        _clock_error_sign = 0;
        _expected_raw_stamp = raw_stamp;
    } else {
        // Remove the raw RTP phase excursion before observing the SR/NTP clock;
        // otherwise a corrected +/-10 ms bounce would reappear as clock drift.
        auto observed_us = ntp_us - samplesToUs(raw_delta, _sample_rate);
        ntp_us = _ntp_anchor_us + samplesToUs(int64_t(_next_sample_stamp) - int64_t(_ntp_anchor_sample), _sample_rate);
        auto error_us = observed_us - ntp_us;
        if (std::abs(error_us) > kNtpDiscontinuityUs) {
            auto sign = error_us < 0 ? -1 : 1;
            if (sign != _clock_error_sign) {
                _clock_error_start = _next_sample_stamp;
                _clock_error_sign = sign;
            }
        } else {
            _clock_error_sign = 0;
        }
        if (_clock_error_sign && _next_sample_stamp - _clock_error_start
                >= uint64_t(_sample_rate) * kNtpDiscontinuitySeconds) {
            // A persistent large SR change (including the first real SR after a
            // local-clock bootstrap) is an explicit NTP boundary, not lost audio.
            WarnL << "G711 clock discontinuity: delta=" << error_us << " us, seq=" << seq << ", SSRC=" << ssrc;
            _ntp_anchor_us += error_us;
            ntp_us = observed_us;
            _clock_error_sign = 0;
            discontinuity = true;
        } else if (std::abs(error_us) > kNtpDeadbandUs) {
            // 1 ms observation deadband covers NTP/Frame quantization; bounded
            // 1 ms/second slew follows drift without writing SR jumps into RTP.
            auto budget = samples * kNtpSlewUsPerSecond + _slew_remainder;
            auto step = std::min<int64_t>(std::abs(error_us) - kNtpDeadbandUs, budget / _sample_rate);
            _slew_remainder = budget % _sample_rate;
            step = error_us < 0 ? -step : step;
            _ntp_anchor_us += step;
            ntp_us += step;
        } else {
            _slew_remainder = 0;
        }
    }

    auto frame = FrameImp::create<G711RtpFrame>();
    frame->_codec_id = _codec;
    frame->sample_stamp = _next_sample_stamp;
    frame->ntp_stamp_us = std::max<int64_t>(0, ntp_us);
    frame->sample_rate = _sample_rate;
    frame->channels = _channels;
    frame->discontinuity = discontinuity;
    frame->_dts = frame->ntp_stamp_us / 1000;
    frame->_buffer.assign(reinterpret_cast<const char *>(rtp->getPayload()), size);
    _have_clock = true;
    _seq = seq;
    _ssrc = ssrc;
    _last_raw_stamp = raw_stamp;
    _recent_packets[_recent_pos] = { seq, raw_stamp, uint32_t(samples) };
    _recent_pos = (_recent_pos + 1) % _recent_packets.size();
    _recent_count = std::min(_recent_count + 1, _recent_packets.size());
    _next_sample_stamp += samples;
    _expected_raw_stamp += samples;
    RtpCodec::inputFrame(frame);
    return false;
}

G711RtpEncoder::G711RtpEncoder(int sample_rate, int channels, int sample_bit) {
    setAudioInfo(sample_rate, channels);
}

void G711RtpEncoder::setAudioInfo(int sample_rate, int channels) {
    checkAudioInfo(sample_rate, channels);
    if (_have_stamp && (_sample_rate != sample_rate || _channels != channels)) {
        throw std::logic_error("Cannot change active G711 RTP audio parameters");
    }
    _sample_rate = sample_rate;
    _channels = channels;
}

void G711RtpEncoder::setOpt(int opt, const toolkit::Any &param) {
    if (opt == RTP_ENCODER_PKT_DUR_MS) {
        if (param.is<uint32_t>()) {
            auto dur = param.get<uint32_t>();
            if (dur < 20 || dur > 180) {
                WarnL << "set g711 rtp encoder  duration ms failed for " << dur;
                return;
            }
            // 向上 20ms 取整  [AUTO-TRANSLATED:b8a9e39e]
            // Round up to the nearest 20ms
            _pkt_dur_ms = (dur + 19) / 20 * 20;
        }
    }
}

size_t G711RtpEncoder::packetSamples() {
    auto max_payload = std::min<size_t>(getRtpInfo().getMaxSize(),
                                       std::numeric_limits<uint16_t>::max() - RtpPacket::kRtpHeaderSize);
    auto max_samples = max_payload / _channels;
    auto duration_samples = uint64_t(_pkt_dur_ms) * _sample_rate / 1000;
    if (!max_samples || !duration_samples) {
        throw std::invalid_argument("G711 RTP MTU or packet duration cannot contain a sample");
    }
    return std::min<uint64_t>(max_samples, duration_samples);
}

void G711RtpEncoder::emitSamples(size_t samples) {
    auto bytes = samples * _channels;
    auto ntp_ms = _exact_stamp
        ? uint64_t(std::max<int64_t>(0, int64_t(_ntp_stamp_us)
              + samplesToUs(int64_t(_buffer_stamp) - int64_t(_ntp_sample_stamp), _sample_rate))) / 1000
        : _buffer_stamp * 1000 / _sample_rate;
    auto packet = getRtpInfo().makeRtpWithStamp(TrackAudio, _buffer.data(), bytes, false,
                                              ntp_ms, uint32_t(_buffer_stamp));
    RtpCodec::inputRtp(packet, false);
    _buffer_stamp += samples;
    _buffer.erase(0, bytes);
}

bool G711RtpEncoder::inputFrame(const Frame::Ptr &frame) {
    if (frame->prefixSize() > frame->size()) {
        return false;
    }
    auto size = frame->size() - frame->prefixSize();
    if (!size || size % _channels) {
        WarnL << "Invalid G711 payload sample alignment";
        return false;
    }
    auto packet_samples = packetSamples();
    auto exact = dynamic_cast<const G711RtpFrame *>(frame.get());
    if (exact && (exact->sample_rate != _sample_rate || exact->channels != _channels)) {
        WarnL << "G711 RTP frame audio parameters do not match encoder";
        return false;
    }
    auto input_stamp = exact ? exact->sample_stamp : frame->pts() * uint64_t(_sample_rate) / 1000;
    auto delta = input_stamp > _input_end_stamp ? input_stamp - _input_end_stamp : _input_end_stamp - input_stamp;
    // Frame timestamps have millisecond precision. Preserve sub-ms sample offsets, not real discontinuities.
    if (!_have_stamp || bool(exact) != _exact_stamp
        || (exact ? exact->discontinuity || delta : delta > (uint64_t(_sample_rate) + 999) / 1000)) {
        if (_have_stamp) {
            if (!exact) {
                WarnL << "G711 frame discontinuity: delta=" << delta << " samples";
            }
            flush();
        }
        _buffer_stamp = input_stamp;
        _input_end_stamp = input_stamp;
        _have_stamp = true;
    }
    _exact_stamp = bool(exact);
    if (exact) {
        _ntp_sample_stamp = input_stamp;
        _ntp_stamp_us = exact->ntp_stamp_us;
    }
    _input_end_stamp += size / _channels;
    _buffer.append(frame->data() + frame->prefixSize(), size);
    while (_buffer.size() / _channels >= packet_samples) {
        emitSamples(packet_samples);
    }
    return true;
}

void G711RtpEncoder::flush() {
    if (_buffer.empty()) {
        return;
    }
    auto packet_samples = packetSamples();
    while (!_buffer.empty()) {
        emitSamples(std::min(packet_samples, _buffer.size() / _channels));
    }
}

} // namespace mediakit
