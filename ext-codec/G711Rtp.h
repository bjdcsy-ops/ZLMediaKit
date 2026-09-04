/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#ifndef ZLMEDIAKIT_G711RTP_H
#define ZLMEDIAKIT_G711RTP_H

#include <array>
#include "Rtsp/RtpCodec.h"
#include "Extension/Frame.h"
#include "Extension/CommonRtp.h"

namespace mediakit {

// Cacheable RTP-originated G711 data keeps its sample clock separate from the
// receiver's millisecond NTP mapping. Deliberate FrameStamp overrides use the
// encoder's ordinary PTS path instead of bypassing modify_stamp.
class G711RtpFrame : public FrameImp {
public:
    uint64_t sample_stamp = 0;
    uint64_t ntp_stamp_us = 0;
    int sample_rate = 0;
    int channels = 0;
    bool discontinuity = false;
};

// Every G711 payload is independently decodable; do not wait for a subsequent timestamp.
class G711RtpDecoder : public RtpCodec {
public:
    explicit G711RtpDecoder(CodecId codec) : _codec(codec) {}
    bool inputRtp(const RtpPacket::Ptr &rtp, bool key_pos = false) override;
    void setAudioInfo(int sample_rate, int channels) override;

private:
    CodecId _codec;
    int _sample_rate = 0;
    int _channels = 1;
    bool _have_clock = false;
    uint16_t _seq = 0;
    uint32_t _ssrc = 0;
    uint32_t _last_raw_stamp = 0;
    uint32_t _expected_raw_stamp = 0;
    uint64_t _next_sample_stamp = 0;
    RtpPacket::Ptr _restart_packet;
    bool _restart_confirmed = false;
    struct RecentPacket {
        uint16_t seq;
        uint32_t stamp;
        uint32_t samples;
    };
    // Metadata only: exact matching also works for variable-size G711 packets.
    std::array<RecentPacket, 100> _recent_packets;
    size_t _recent_count = 0;
    size_t _recent_pos = 0;
    uint64_t _ntp_anchor_sample = 0;
    int64_t _ntp_anchor_us = 0;
    uint64_t _clock_error_start = 0;
    int _clock_error_sign = 0;
    uint64_t _slew_remainder = 0;
};

/**
 * G711 rtp编码类
 * G711 rtp encoding class
 
 * [AUTO-TRANSLATED:92aa6cf3]
 */
class G711RtpEncoder : public RtpCodec {
public:
    using Ptr = std::shared_ptr<G711RtpEncoder>;

    /**
     * 构造函数
     * @param sample_rate 音频采样率
     * @param channels 通道数
     * @param sample_bit 音频采样位数
     * Constructor
     * @param sample_rate audio sample rate
     * @param channels Number of channels
     * @param sample_bit audio sample bits

     * [AUTO-TRANSLATED:dbbd593e]
     */
    G711RtpEncoder(int sample_rate = 8000, int channels = 1, int sample_bit = 16);

    /**
     * 输入帧数据并编码成rtp
     * Input frame data and encode it into rtp
     
     
     * [AUTO-TRANSLATED:02bc9009]
     */
    bool inputFrame(const Frame::Ptr &frame) override;

    void setOpt(int opt, const toolkit::Any &param) override;
    void setAudioInfo(int sample_rate, int channels) override;
    void flush() override;

private:
    size_t packetSamples();
    void emitSamples(size_t samples);

    int _channels;
    int _sample_rate;

    uint32_t _pkt_dur_ms = 20;
    bool _have_stamp = false;
    bool _exact_stamp = false;
    uint64_t _buffer_stamp = 0;
    uint64_t _input_end_stamp = 0;
    uint64_t _ntp_sample_stamp = 0;
    uint64_t _ntp_stamp_us = 0;
    toolkit::BufferLikeString _buffer;
};

}//namespace mediakit
#endif //ZLMEDIAKIT_G711RTP_H
