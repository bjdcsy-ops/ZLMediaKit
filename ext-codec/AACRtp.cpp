/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "AACRtp.h"

namespace mediakit{

bool AACRtpEncoder::inputFrame(const Frame::Ptr &frame) {
    auto ptr = (char *)frame->data() + frame->prefixSize();
    auto size = frame->size() - frame->prefixSize();
    auto remain_size = size;
    auto max_size = getRtpInfo().getMaxSize() - 4;
    while (remain_size > 0) {
        if (remain_size <= max_size) {
            outputRtp(ptr, remain_size, size, true, frame->dts());
            break;
        }
        outputRtp(ptr, max_size, size, false, frame->dts());
        ptr += max_size;
        remain_size -= max_size;
    }
    return true;
}

void AACRtpEncoder::outputRtp(const char *data, size_t len, size_t total_len, bool mark, uint64_t stamp) {
    auto rtp = getRtpInfo().makeRtp(TrackAudio, nullptr, len + 4, mark, stamp);
    auto payload = rtp->data() + RtpPacket::kRtpTcpHeaderSize + RtpPacket::kRtpHeaderSize;
    payload[0] = 0;
    payload[1] = 16;
    payload[2] = ((total_len) >> 5) & 0xFF;
    payload[3] = ((total_len & 0x1F) << 3) & 0xFF;
    memcpy(payload + 4, data, len);
    RtpCodec::inputRtp(std::move(rtp), false);
}

/////////////////////////////////////////////////////////////////////////////////////

AACRtpDecoder::AACRtpDecoder() {
    obtainFrame();
}

void AACRtpDecoder::obtainFrame() {
    // 从缓存池重新申请对象，防止覆盖已经写入环形缓存的对象  [AUTO-TRANSLATED:f85fe201]
    // Re-apply the object from the cache pool to prevent overwriting the object that has been written to the ring buffer
    _frame = FrameImp::create();
    _frame->_codec_id = CodecAAC;
}

bool AACRtpDecoder::inputRtp(const RtpPacket::Ptr &rtp, bool key_pos) {
    auto seq = rtp->getSeq();
    auto rtp_stamp = rtp->getStamp();
    auto ssrc = rtp->getSSRC();
    auto same_source = _have_packet && ssrc == _last_ssrc;
    auto same_stamp = same_source && rtp_stamp == _last_rtp_stamp;
    if (same_stamp && static_cast<int16_t>(seq - _last_seq) <= 0) {
        // Protect this AU from duplicate/late fragments only. The upstream
        // RTP sorter owns stream-level recovery, including same-SSRC sequence
        // resets; a new timestamp must not be rejected by another seq gate.
        return false;
    }
    if (!same_stamp) {
        // An incomplete AU belongs to its original RTP timestamp/SSRC only.
        if (_fragment_size) {
            obtainFrame();
        }
        _fragment_size = 0;
        _drop_fragment = false;
    } else if (seq != static_cast<uint16_t>(_last_seq + 1)) {
        obtainFrame();
        _fragment_size = 0;
        _drop_fragment = true;
    }
    _have_packet = true;
    _last_seq = seq;
    _last_rtp_stamp = rtp_stamp;
    _last_ssrc = ssrc;
    if (_drop_fragment) {
        return false;
    }
    auto discard_fragment = [&]() {
        obtainFrame();
        _fragment_size = 0;
        _drop_fragment = true;
        return false;
    };

    auto payload_size = rtp->getPayloadSize();
    if (payload_size < 2) {
        // AU-Header-Length至少占用两个字节  [AUTO-TRANSLATED:2267e6ac]
        // AU-Header-Length occupies at least two bytes
        return discard_fragment();
    }

    auto stamp = rtp->getStampMS();
    // rtp数据开始部分  [AUTO-TRANSLATED:f22ebdb9]
    // Start of rtp data
    auto ptr = rtp->getPayload();
    // rtp数据末尾  [AUTO-TRANSLATED:ee108f2b]
    // End of rtp data
    auto end = ptr + payload_size;
    // 首2字节表示Au-Header的个数，单位bit，所以除以16得到Au-Header个数  [AUTO-TRANSLATED:c7175051]
    // The first 2 bytes represent the number of Au-Headers, in bits, so divide by 16 to get the number of Au-Headers
    auto au_header_bits = (ptr[0] << 8) | ptr[1];
    auto au_header_count = au_header_bits >> 4;
    if (!au_header_count || (au_header_bits & 15)) {
        // 问题issue: https://github.com/ZLMediaKit/ZLMediaKit/issues/1869  [AUTO-TRANSLATED:14be1ff8]
        // Issue: https://github.com/ZLMediaKit/ZLMediaKit/issues/1869
        WarnL << "invalid aac rtp au_header_count";
        return discard_fragment();
    }
    auto au_headers_size = static_cast<size_t>(au_header_count) * 2;
    if (static_cast<size_t>(payload_size) - 2 < au_headers_size) {
        // AU-Header数据不完整  [AUTO-TRANSLATED:830a2785]
        // Incomplete AU-Header data
        return discard_fragment();
    }
    // 记录au_header起始指针  [AUTO-TRANSLATED:b9083b72]
    // Record the starting pointer of au_header
    auto au_header_ptr = ptr + 2;
    ptr = au_header_ptr + au_headers_size;

    if (au_header_count > 1 && !rtp->sample_rate) {
        return discard_fragment();
    }

    uint64_t au_offset = 0;
    for (auto i = 0u; i < (size_t)au_header_count; ++i) {
        // 高13位是AU字节长度，低3位是AU-index或AU-index-delta。
        // AAC-hbr: 13-bit AU-size followed by a 3-bit AU-index/index-delta.
        auto header = (au_header_ptr[0] << 8) | au_header_ptr[1];
        auto size = static_cast<size_t>(header >> 3);
        auto index = header & 7;
        if (i) {
            au_offset += index + 1;
        }
        au_header_ptr += 2;
        if (!size) {
            return discard_fragment();
        }
        auto remaining = static_cast<size_t>(end - ptr);
        if (_fragment_size) {
            if (au_header_count != 1 || size != _fragment_size || index != _fragment_index
                || remaining > size - _frame->size()) {
                return discard_fragment();
            }
            _frame->_buffer.append(reinterpret_cast<const char *>(ptr), remaining);
            if (_frame->size() == size) {
                // Keep the first fragment's NTP mapping even if an SR arrived
                // between fragments of this same RTP timestamp.
                _fragment_size = 0;
                flushData();
            } else if (rtp->getHeader()->mark) {
                return discard_fragment();
            }
            return false;
        }

        // The first AU belongs to this packet, not the previous RTP packet.
        // Bundled AAC-hbr AUs follow the existing AACTrack 1024-sample model
        // (AAC-LC with frameLengthFlag=0). 960-sample/HE-AAC bundle timing
        // requires ASC/constantDuration support, not packet-arrival inference.
        _frame->_dts = stamp + (au_offset ? au_offset * 1024 * 1000 / rtp->sample_rate : 0);
        if (remaining < size) {
            // Fragmentation carries one AU per RTP packet. A terminal packet
            // without all bytes is truncated and cannot seed the next AU.
            if (au_header_count != 1 || !remaining || rtp->getHeader()->mark) {
                return discard_fragment();
            }
            _frame->_buffer.append(reinterpret_cast<const char *>(ptr), remaining);
            _fragment_size = size;
            _fragment_index = index;
            return false;
        } else {
            _frame->_buffer.append(reinterpret_cast<const char *>(ptr), size);
            ptr += size;
            flushData();
        }
    }
    return false;
}

void AACRtpDecoder::flushData() {
    auto ptr = reinterpret_cast<const uint8_t *>(_frame->data());
    if (_frame->size() > ADTS_HEADER_LEN && ptr[0] == 0xFF && (ptr[1] & 0xF0) == 0xF0) {
        // adts头打入了rtp包，不符合规范，兼容EasyPusher的bug  [AUTO-TRANSLATED:203a5ee9]
        // The adts header is inserted into the rtp packet, which is not compliant with the specification, compatible with the bug of EasyPusher
        _frame->_prefix_size = ADTS_HEADER_LEN;
    }
    RtpCodec::inputFrame(_frame);
    obtainFrame();
}

}//namespace mediakit
