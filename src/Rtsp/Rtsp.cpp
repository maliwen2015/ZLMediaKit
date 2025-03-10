﻿/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <cstdlib>
#include <cinttypes>
#include <random>
#include "Rtsp.h"
#include "Network/Socket.h"
#include "Common/Parser.h"
#include "Common/config.h"
#include "Extension/Track.h"
#include "Extension/Factory.h"

using namespace std;
using namespace toolkit;

namespace mediakit {

int RtpPayload::getClockRate(int pt) {
    switch (pt) {
#define XX(name, type, value, clock_rate, channel, codec_id)                                                                                          \
    case value: return clock_rate;
        RTP_PT_MAP(XX)
#undef XX
        default: return 90000;
    }
}

int RtpPayload::getClockRateByCodec(CodecId codec) {
#define XX(name, type, value, clock_rate, channel, codec_id) { codec_id, clock_rate },
    static map<CodecId, int> s_map = { RTP_PT_MAP(XX) };
#undef XX
    auto it = s_map.find(codec);
    if (it == s_map.end()) {
        WarnL << "Unsupported codec: " << getCodecName(codec);
        return 90000;
    }
    return it->second;
}

int RtpPayload::getPayloadType(const Track &track) {
#define XX(name, type, value, clock_rate, channel, codec_id) { codec_id, info { clock_rate, channel, value } },
    struct info {
        int clock_rate;
        int channels;
        int pt;
    };
    static map<CodecId, info> s_map = { RTP_PT_MAP(XX) };
#undef XX
    auto it = s_map.find(track.getCodecId());
    if (it == s_map.end()) {
        return -1;
    }
    if (track.getTrackType() == TrackAudio) {
        if (static_cast<const AudioTrack &>(track).getAudioSampleRate() != it->second.clock_rate
            || static_cast<const AudioTrack &>(track).getAudioChannel() != it->second.channels) {
            return -1;
        }
    }
    return it->second.pt;
}

TrackType RtpPayload::getTrackType(int pt) {
    switch (pt) {
#define XX(name, type, value, clock_rate, channel, codec_id)                                                                                          \
    case value: return type;
        RTP_PT_MAP(XX)
#undef XX
        default: return TrackInvalid;
    }
}

int RtpPayload::getAudioChannel(int pt) {
    switch (pt) {
#define XX(name, type, value, clock_rate, channel, codec_id)                                                                                          \
    case value: return channel;
        RTP_PT_MAP(XX)
#undef XX
        default: return 1;
    }
}

const char *RtpPayload::getName(int pt) {
    switch (pt) {
#define XX(name, type, value, clock_rate, channel, codec_id)                                                                                          \
    case value: return #name;
        RTP_PT_MAP(XX)
#undef XX
        default: return "unknown payload type";
    }
}

CodecId RtpPayload::getCodecId(int pt) {
    switch (pt) {
#define XX(name, type, value, clock_rate, channel, codec_id)                                                                                          \
    case value: return codec_id;
        RTP_PT_MAP(XX)
#undef XX
        default: return CodecInvalid;
    }
}

static void getAttrSdp(const multimap<string, string> &attr, _StrPrinter &printer) {
    const map<string, string>::value_type *ptr = nullptr;
    for (auto &pr : attr) {
        if (pr.first == "control") {
            ptr = &pr;
            continue;
        }
        if (pr.second.empty()) {
            printer << "a=" << pr.first << "\r\n";
        } else {
            printer << "a=" << pr.first << ":" << pr.second << "\r\n";
        }
    }
    if (ptr) {
        printer << "a=" << ptr->first << ":" << ptr->second << "\r\n";
    }
}

string SdpTrack::getName() const {
    return RtpPayload::getName(_pt);
}

string SdpTrack::getControlUrl(const string &base_url) const {
    if (_control.find("://") != string::npos) {
        // 以rtsp://开头  [AUTO-TRANSLATED:293b3f8c]
        // Starts with rtsp://
        return _control;
    }
    return base_url + "/" + _control;
}

string SdpTrack::toString(uint16_t port) const {
    _StrPrinter _printer;
    switch (_type) {
        case TrackTitle: {
            TitleSdp title(_duration);
            _printer << title.getSdp();
            break;
        }
        case TrackAudio:
        case TrackVideo: {
            if (_type == TrackAudio) {
                _printer << "m=audio " << port << " RTP/AVP " << _pt << "\r\n";
            } else {
                _printer << "m=video " << port << " RTP/AVP " << _pt << "\r\n";
            }
            if (!_b.empty()) {
                _printer << "b=" << _b << "\r\n";
            }
            getAttrSdp(_attr, _printer);
            break;
        }
        default: break;
    }
    return std::move(_printer);
}

static TrackType toTrackType(const string &str) {
    if (str == "") {
        return TrackTitle;
    }

    if (str == "video") {
        return TrackVideo;
    }

    if (str == "audio") {
        return TrackAudio;
    }

    return TrackInvalid;
}

void SdpParser::load(const string &sdp) {
    {
        _track_vec.clear();
        SdpTrack::Ptr track = std::make_shared<SdpTrack>();
        track->_type = TrackTitle;
        _track_vec.emplace_back(track);

        auto lines = split(sdp, "\n");
        for (auto &line : lines) {
            trim(line);
            if (line.size() < 2 || line[1] != '=') {
                continue;
            }
            char opt = line[0];
            string opt_val = line.substr(2);
            switch (opt) {
                case 't':
                    track->_t = opt_val;
                    break;
                case 'b':
                    track->_b = opt_val;
                    break;
                case 'm': {
                    track = std::make_shared<SdpTrack>();
                    int pt, port, port_count;
                    char rtp[16] = {0}, type[16];
                    if (4 == sscanf(opt_val.data(), " %15[^ ] %d %15[^ ] %d", type, &port, rtp, &pt) ||
                        5 == sscanf(opt_val.data(), " %15[^ ] %d/%d %15[^ ] %d", type, &port, &port_count, rtp, &pt)) {
                        track->_pt = pt;
                        track->_samplerate = RtpPayload::getClockRate(pt);
                        track->_channel = RtpPayload::getAudioChannel(pt);
                        track->_type = toTrackType(type);
                        track->_port = port;
                        _track_vec.emplace_back(track);
                    }
                    break;
                }
                case 'a': {
                    string attr = findSubString(opt_val.data(), nullptr, ":");
                    if (attr.empty()) {
                        track->_attr.emplace(opt_val, "");
                    } else {
                        track->_attr.emplace(attr, findSubString(opt_val.data(), ":", nullptr));
                    }
                    break;
                }
                default: track->_other[opt] = opt_val; break;
            }
        }
    }

    for (auto &track_ptr : _track_vec) {
        auto &track = *track_ptr;
        auto it = track._attr.find("range");
        if (it != track._attr.end()) {
            char name[16] = { 0 }, start[16] = { 0 }, end[16] = { 0 };
            int ret = sscanf(it->second.data(), "%15[^=]=%15[^-]-%15s", name, start, end);
            if (3 == ret || 2 == ret) {
                if (strcmp(start, "now") == 0) {
                    strcpy(start, "0");
                }
                track._start = (float)atof(start);
                track._end = (float)atof(end);
                track._duration = track._end - track._start;
            }
        }

        for (it = track._attr.find("rtpmap"); it != track._attr.end() && it->first == "rtpmap";) {
            auto &rtpmap = it->second;
            int pt, samplerate, channel;
            char codec[16] = { 0 };

            sscanf(rtpmap.data(), "%d", &pt);
            if (track._pt != pt && track._pt != 0xff) {
                // pt不匹配  [AUTO-TRANSLATED:ce7abb0a]
                // pt mismatch
                it = track._attr.erase(it);
                continue;
            }
            if (4 == sscanf(rtpmap.data(), "%d %15[^/]/%d/%d", &pt, codec, &samplerate, &channel)) {
                track._codec = codec;
                track._samplerate = samplerate;
                track._channel = channel;
            } else if (3 == sscanf(rtpmap.data(), "%d %15[^/]/%d", &pt, codec, &samplerate)) {
                track._pt = pt;
                track._codec = codec;
                track._samplerate = samplerate;
            }
            ++it;
        }

        for (it = track._attr.find("fmtp"); it != track._attr.end() && it->first == "fmtp";) {
            auto &fmtp = it->second;
            int pt;
            sscanf(fmtp.data(), "%d", &pt);
            if (track._pt != pt && track._pt != 0xff) {
                // pt不匹配  [AUTO-TRANSLATED:ce7abb0a]
                // pt mismatch
                it = track._attr.erase(it);
                continue;
            }
            track._fmtp = findSubString(fmtp.data(), " ", nullptr);
            ++it;
        }

        it = track._attr.find("control");
        if (it != track._attr.end()) {
            track._control = it->second;
        }

        if (!track._samplerate && track._type == TrackVideo) {
            // 未设置视频采样率时，赋值为90000  [AUTO-TRANSLATED:416c4f0f]
            // If video sampling rate is not set, assign it to 90000
            track._samplerate = 90000;
        } else if (!track._samplerate && track._type == TrackAudio) {
            // some rtsp sdp no sample rate but has fmt config to parser get sample rate
            auto t = Factory::getTrackBySdp(track_ptr);
            if (t) {
                track._samplerate = std::static_pointer_cast<AudioTrack>(t)->getAudioSampleRate();
            }
        }
    }
}

bool SdpParser::available() const {
    return getTrack(TrackAudio) || getTrack(TrackVideo);
}

SdpTrack::Ptr SdpParser::getTrack(TrackType type) const {
    for (auto &track : _track_vec) {
        if (track->_type == type) {
            return track;
        }
    }
    return nullptr;
}

vector<SdpTrack::Ptr> SdpParser::getAvailableTrack() const {
    vector<SdpTrack::Ptr> ret;
    bool audio_added = false;
    bool video_added = false;
    for (auto &track : _track_vec) {
        if (track->_type == TrackAudio) {
            if (!audio_added) {
                ret.emplace_back(track);
                audio_added = true;
            }
            continue;
        }

        if (track->_type == TrackVideo) {
            if (!video_added) {
                ret.emplace_back(track);
                video_added = true;
            }
            continue;
        }
    }
    return ret;
}

string SdpParser::toString() const {
    string title, audio, video;
    for (auto &track : _track_vec) {
        switch (track->_type) {
            case TrackTitle: {
                title = track->toString();
                break;
            }
            case TrackVideo: {
                video = track->toString();
                break;
            }
            case TrackAudio: {
                audio = track->toString();
                break;
            }
            default: break;
        }
    }
    return title + video + audio;
}

std::string SdpParser::getControlUrl(const std::string &url) const {
    auto title_track = getTrack(TrackTitle);
    if (title_track && title_track->_control.find("://") != string::npos) {
        // 以rtsp://开头  [AUTO-TRANSLATED:293b3f8c]
        // Starts with rtsp://
        return title_track->_control;
    }
    return url;
}

template <int type>
class PortManager : public std::enable_shared_from_this<PortManager<type>> {
public:
    PortManager() {
        static auto func = [](const string &str, int index) {
            uint16_t port[] = { 30000, 35000 };
            sscanf(str.data(), "%" SCNu16 "-%" SCNu16, port, port + 1);
            return port[index];
        };
        GET_CONFIG_FUNC(uint16_t, s_min_port, RtpProxy::kPortRange, [](const string &str) { return func(str, 0); });
        GET_CONFIG_FUNC(uint16_t, s_max_port, RtpProxy::kPortRange, [](const string &str) { return func(str, 1); });
        assert(s_max_port >= s_min_port + 36 - 1);
        setRange((s_min_port + 1) / 2, s_max_port / 2);
    }

    static PortManager &Instance() {
        static auto instance = std::make_shared<PortManager>();
        return *instance;
    }

    void makeSockPair(std::pair<Socket::Ptr, Socket::Ptr> &pair, const string &local_ip, bool re_use_port, bool is_udp) {
        auto sock_pair = getPortPair();
        if (!sock_pair) {
            throw runtime_error("none reserved port in pool");
        }
        makeSockPair_l(sock_pair, pair, local_ip, re_use_port, is_udp);

        // 确保udp和tcp模式都能打开  [AUTO-TRANSLATED:dcb46232]
        // Ensure both udp and tcp modes are open
        auto new_pair = std::make_pair(Socket::createSocket(), Socket::createSocket());
        makeSockPair_l(sock_pair, new_pair, local_ip, re_use_port, !is_udp);
    }

    void makeSockPair_l(const std::shared_ptr<uint16_t> &sock_pair, std::pair<Socket::Ptr, Socket::Ptr> &pair, const string &local_ip, bool re_use_port, bool is_udp) {
        auto &sock0 = pair.first;
        auto &sock1 = pair.second;
        if (is_udp) {
            if (!sock0->bindUdpSock(2 * *sock_pair, local_ip.data(), re_use_port)) {
                // 分配端口失败  [AUTO-TRANSLATED:59ecd25d]
                // Port allocation failed
                throw runtime_error("open udp socket[0] failed");
            }

            if (!sock1->bindUdpSock(2 * *sock_pair + 1, local_ip.data(), re_use_port)) {
                // 分配端口失败  [AUTO-TRANSLATED:59ecd25d]
                // Port allocation failed
                throw runtime_error("open udp socket[1] failed");
            }

            auto on_cycle = [sock_pair](Socket::Ptr &, std::shared_ptr<void> &) {};
            // udp socket没onAccept事件，设置该回调，目的是为了在销毁socket时，回收对象  [AUTO-TRANSLATED:ee91256f]
            // UDP socket has no onAccept event, set this callback to recycle the object when destroying the socket
            sock0->setOnAccept(on_cycle);
            sock1->setOnAccept(on_cycle);
        } else {
            if (!sock0->listen(2 * *sock_pair, local_ip.data())) {
                // 分配端口失败  [AUTO-TRANSLATED:59ecd25d]
                // Port allocation failed
                throw runtime_error("listen tcp socket[0] failed");
            }

            if (!sock1->listen(2 * *sock_pair + 1, local_ip.data())) {
                // 分配端口失败  [AUTO-TRANSLATED:59ecd25d]
                // Port allocation failed
                throw runtime_error("listen tcp socket[1] failed");
            }

            auto on_cycle = [sock_pair](const Buffer::Ptr &buf, struct sockaddr *addr, int addr_len) {};
            // udp socket没onAccept事件，设置该回调，目的是为了在销毁socket时，回收对象  [AUTO-TRANSLATED:ee91256f]
            // UDP socket has no onAccept event, set this callback to recycle the object when destroying the socket
            sock0->setOnRead(on_cycle);
            sock1->setOnRead(on_cycle);
        }
    }

private:
    void setRange(uint16_t start_pos, uint16_t end_pos) {
        std::mt19937 rng(std::random_device {}());
        lock_guard<recursive_mutex> lck(_pool_mtx);
        auto it = _port_pair_pool.begin();
        while (start_pos < end_pos) {
            // 随机端口排序，防止重启后导致分配的端口重复  [AUTO-TRANSLATED:b622db1c]
            // Randomly sort ports to prevent duplicate port allocation after restart
            _port_pair_pool.insert(it, start_pos++);
            it = _port_pair_pool.begin() + (rng() % (1 + _port_pair_pool.size()));
        }
    }

    std::shared_ptr<uint16_t> getPortPair() {
        lock_guard<recursive_mutex> lck(_pool_mtx);
        if (_port_pair_pool.empty()) {
            return nullptr;
        }
        auto pos = _port_pair_pool.front();
        _port_pair_pool.pop_front();
        InfoL << "got port from pool:" << 2 * pos << "-" << 2 * pos + 1;

        weak_ptr<PortManager> weak_self = this->shared_from_this();
        std::shared_ptr<uint16_t> ret(new uint16_t(pos), [weak_self, pos](uint16_t *ptr) {
            delete ptr;
            auto strong_self = weak_self.lock();
            if (!strong_self) {
                return;
            }
            InfoL << "return port to pool:" << 2 * pos << "-" << 2 * pos + 1;
            // 回收端口号  [AUTO-TRANSLATED:646a5284]
            // Recycle port number
            lock_guard<recursive_mutex> lck(strong_self->_pool_mtx);
            strong_self->_port_pair_pool.emplace_back(pos);
        });
        return ret;
    }

private:
    recursive_mutex _pool_mtx;
    deque<uint16_t> _port_pair_pool;
};

void makeSockPair(std::pair<Socket::Ptr, Socket::Ptr> &pair, const string &local_ip, bool re_use_port, bool is_udp) {
    int try_count = 0;
    while (true) {
        try {
            // udp和tcp端口池使用相同算法和范围分配，但是互不相干  [AUTO-TRANSLATED:86200c72]
            // UDP and TCP port pools use the same algorithm and range for allocation, but are independent of each other
            if (is_udp) {
                PortManager<0>::Instance().makeSockPair(pair, local_ip, re_use_port, is_udp);
            } else {
                PortManager<1>::Instance().makeSockPair(pair, local_ip, re_use_port, is_udp);
            }
            break;
        } catch (exception &ex) {
            if (++try_count == 3) {
                throw;
            }
            WarnL << "open socket failed:" << ex.what() << ", retry: " << try_count;
        }
    }
}

string printSSRC(uint32_t ui32Ssrc) {
    char tmp[9] = { 0 };
    ui32Ssrc = htonl(ui32Ssrc);
    uint8_t *pSsrc = (uint8_t *)&ui32Ssrc;
    for (int i = 0; i < 4; i++) {
        sprintf(tmp + 2 * i, "%02X", pSsrc[i]);
    }
    return tmp;
}

bool getSSRC(const char *data, size_t data_len, uint32_t &ssrc) {
    if (data_len < 12) {
        return false;
    }
    uint32_t *ssrc_ptr = (uint32_t *)(data + 8);
    ssrc = ntohl(*ssrc_ptr);
    return true;
}

bool isRtp(const char *buf, size_t size) {
    if (size < 2) {
        return false;
    }
    RtpHeader *header = (RtpHeader *)buf;
    return ((header->pt < 64) || (header->pt >= 96)) && header->version == RtpPacket::kRtpVersion;
}

bool isRtcp(const char *buf, size_t size) {
    if (size < 2) {
        return false;
    }
    RtpHeader *header = (RtpHeader *)buf;
    return ((header->pt >= 64) && (header->pt < 96));
}

Buffer::Ptr makeRtpOverTcpPrefix(uint16_t size, uint8_t interleaved) {
    auto rtp_tcp = BufferRaw::create();
    rtp_tcp->setCapacity(RtpPacket::kRtpTcpHeaderSize);
    rtp_tcp->setSize(RtpPacket::kRtpTcpHeaderSize);
    auto ptr = rtp_tcp->data();
    ptr[0] = '$';
    ptr[1] = interleaved;
    ptr[2] = (size >> 8) & 0xFF;
    ptr[3] = size & 0xFF;
    return rtp_tcp;
}

#define AV_RB16(x) ((((const uint8_t *)(x))[0] << 8) | ((const uint8_t *)(x))[1])

size_t RtpHeader::getCsrcSize() const {
    // 每个csrc占用4字节  [AUTO-TRANSLATED:6237ca37]
    // Each csrc occupies 4 bytes
    return csrc << 2;
}

uint8_t *RtpHeader::getCsrcData() {
    if (!csrc) {
        return nullptr;
    }
    return &payload;
}

size_t RtpHeader::getExtSize() const {
    // rtp有ext  [AUTO-TRANSLATED:d5d9af4f]
    // RTP has ext
    if (!ext) {
        return 0;
    }
    auto ext_ptr = &payload + getCsrcSize();
    // uint16_t reserved = AV_RB16(ext_ptr);
    // 每个ext占用4字节  [AUTO-TRANSLATED:93e9b453]
    // Each ext occupies 4 bytes
    return AV_RB16(ext_ptr + 2) << 2;
}

uint16_t RtpHeader::getExtReserved() const {
    // rtp有ext  [AUTO-TRANSLATED:d5d9af4f]
    // RTP has ext
    if (!ext) {
        return 0;
    }
    auto ext_ptr = &payload + getCsrcSize();
    return AV_RB16(ext_ptr);
}

uint8_t *RtpHeader::getExtData() {
    if (!ext) {
        return nullptr;
    }
    auto ext_ptr = &payload + getCsrcSize();
    // 多出的4个字节分别为reserved、ext_len  [AUTO-TRANSLATED:070138f4]
    // The extra 4 bytes are reserved, ext_len
    return ext_ptr + 4;
}

size_t RtpHeader::getPayloadOffset() const {
    // 有ext时，还需要忽略reserved、ext_len 4个字节  [AUTO-TRANSLATED:3e222997]
    // When there is ext, you also need to ignore the reserved, ext_len 4 bytes
    return getCsrcSize() + (ext ? (4 + getExtSize()) : 0);
}

uint8_t *RtpHeader::getPayloadData() {
    return &payload + getPayloadOffset();
}

size_t RtpHeader::getPaddingSize(size_t rtp_size) const {
    if (!padding) {
        return 0;
    }
    auto end = (uint8_t *)this + rtp_size - 1;
    return *end;
}

ssize_t RtpHeader::getPayloadSize(size_t rtp_size) const {
    auto invalid_size = getPayloadOffset() + getPaddingSize(rtp_size);
    return (ssize_t)rtp_size - invalid_size - RtpPacket::kRtpHeaderSize;
}

string RtpHeader::dumpString(size_t rtp_size) const {
    _StrPrinter printer;
    printer << "version:" << (int)version << "\r\n";
    printer << "padding:" << getPaddingSize(rtp_size) << "\r\n";
    printer << "ext:" << getExtSize() << "\r\n";
    printer << "csrc:" << getCsrcSize() << "\r\n";
    printer << "mark:" << (int)mark << "\r\n";
    printer << "pt:" << (int)pt << "\r\n";
    printer << "seq:" << ntohs(seq) << "\r\n";
    printer << "stamp:" << ntohl(stamp) << "\r\n";
    printer << "ssrc:" << ntohl(ssrc) << "\r\n";
    printer << "rtp size:" << rtp_size << "\r\n";
    printer << "payload offset:" << getPayloadOffset() << "\r\n";
    printer << "payload size:" << getPayloadSize(rtp_size) << "\r\n";
    return std::move(printer);
}

///////////////////////////////////////////////////////////////////////

RtpHeader *RtpPacket::getHeader() {
    // 需除去rtcp over tcp 4个字节长度  [AUTO-TRANSLATED:936f6f5b]
    // Need to remove the rtcp over tcp 4 byte length
    return (RtpHeader *)(data() + RtpPacket::kRtpTcpHeaderSize);
}

const RtpHeader *RtpPacket::getHeader() const {
    return (RtpHeader *)(data() + RtpPacket::kRtpTcpHeaderSize);
}

string RtpPacket::dumpString() const {
    return ((RtpPacket *)this)->getHeader()->dumpString(size() - RtpPacket::kRtpTcpHeaderSize);
}

uint16_t RtpPacket::getSeq() const {
    return ntohs(getHeader()->seq);
}

uint32_t RtpPacket::getStamp() const {
    return ntohl(getHeader()->stamp);
}

uint64_t RtpPacket::getStampMS(bool ntp) const {
    return ntp ? ntp_stamp : getStamp() * uint64_t(1000) / sample_rate;
}

uint32_t RtpPacket::getSSRC() const {
    return ntohl(getHeader()->ssrc);
}

uint8_t *RtpPacket::getPayload() {
    return getHeader()->getPayloadData();
}

size_t RtpPacket::getPayloadSize() const {
    // 需除去rtcp over tcp 4个字节长度  [AUTO-TRANSLATED:936f6f5b]
    // Need to remove the rtcp over tcp 4 byte length
    return getHeader()->getPayloadSize(size() - kRtpTcpHeaderSize);
}

RtpPacket::Ptr RtpPacket::create() {
#if 0
    static ResourcePool<RtpPacket> packet_pool;
    static onceToken token([]() {
        packet_pool.setSize(1024);
    });
    auto ret = packet_pool.obtain2();
    ret->setSize(0);
    return ret;
#else
    return Ptr(new RtpPacket);
#endif
}

/**
 * 构造title类型sdp
 * @param dur_sec rtsp点播时长，0代表直播，单位秒
 * @param header 自定义sdp描述
 * @param version sdp版本
 * Construct title type sdp
 * @param dur_sec rtsp on-demand duration, 0 represents live broadcast, unit is seconds
 * @param header Custom sdp description
 * @param version sdp version
 
 * [AUTO-TRANSLATED:a548fc69]
 */

TitleSdp::TitleSdp(float dur_sec, const std::map<std::string, std::string> &header, int version)
    : Sdp(0, 0) {
    _printer << "v=" << version << "\r\n";

    if (!header.empty()) {
        for (auto &pr : header) {
            _printer << pr.first << "=" << pr.second << "\r\n";
        }
    } else {
        _printer << "o=- 0 0 IN IP4 0.0.0.0\r\n";
        _printer << "s=Streamed by " << kServerName << "\r\n";
        _printer << "c=IN IP4 0.0.0.0\r\n";
        _printer << "t=0 0\r\n";
    }

    if (dur_sec <= 0) {
        // 直播  [AUTO-TRANSLATED:079c0cbc]
        // Live broadcast
        _printer << "a=range:npt=now-\r\n";
    } else {
        // 点播  [AUTO-TRANSLATED:f0b0f74a]
        // On-demand
        _dur_sec = dur_sec;
        _printer << "a=range:npt=0-" << dur_sec << "\r\n";
    }
    _printer << "a=control:*\r\n";
}

DefaultSdp::DefaultSdp(int payload_type, const Track &track)
    : Sdp(track.getTrackType() == TrackVideo ? 9000 : static_cast<const AudioTrack &>(track).getAudioSampleRate(), payload_type) {
    _printer << "m=" << track.getTrackTypeStr() << " 0 RTP/AVP " << payload_type << "\r\n";
    auto bitrate = track.getBitRate() >> 10;
    if (bitrate) {
        _printer << "b=AS:" << bitrate << "\r\n";
    }
    if (payload_type < 96) {
        return;
    }
    _printer << "a=rtpmap:" << payload_type << " " << track.getCodecName() << "/" << getSampleRate();
    if (track.getTrackType() == TrackAudio) {
        _printer << "/" << static_cast<const AudioTrack &>(track).getAudioChannel();
    }
    _printer << "\r\n";
}

} // namespace mediakit

namespace toolkit {
StatisticImp(mediakit::RtpPacket);
}