#pragma once

#include <oxwald/net/transport.hpp>

namespace ox::net::detail {

// Byte/packet counters with a smoothed bytes-per-second estimate updated on poll.
struct PeerCounters {
    u64 bytesSent = 0, bytesReceived = 0, packetsSent = 0, packetsReceived = 0;
    u64 windowSent = 0, windowReceived = 0;
    f64 windowStart = -1.0;
    f32 sendRate = 0.f, receiveRate = 0.f;

    void onSend(usize bytes) {
        bytesSent += bytes;
        windowSent += bytes;
        ++packetsSent;
    }
    void onReceive(usize bytes) {
        bytesReceived += bytes;
        windowReceived += bytes;
        ++packetsReceived;
    }
    void update(f64 now) {
        if (windowStart < 0.0) {
            windowStart = now;
            return;
        }
        const f64 dt = now - windowStart;
        if (dt < 0.25) {
            return;
        }
        constexpr f32 kAlpha = 0.5f;
        sendRate += kAlpha * (static_cast<f32>(windowSent / dt) - sendRate);
        receiveRate += kAlpha * (static_cast<f32>(windowReceived / dt) - receiveRate);
        windowSent = windowReceived = 0;
        windowStart = now;
    }
    void fill(PeerStats& s) const {
        s.bytesSent = bytesSent;
        s.bytesReceived = bytesReceived;
        s.packetsSent = packetsSent;
        s.packetsReceived = packetsReceived;
        s.sendBandwidth = sendRate;
        s.receiveBandwidth = receiveRate;
    }
};

} // namespace ox::net::detail
