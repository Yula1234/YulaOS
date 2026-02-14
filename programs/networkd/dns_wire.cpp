#include "dns_wire.h"

#include "net_proto.h"

#include <yula.h>

namespace netd::dns_wire {

namespace {

class BufWriter {
public:
    BufWriter(uint8_t* out, uint32_t cap) : m_out(out), m_cap(cap), m_pos(0u) {
    }

    bool put_u8(uint8_t v) {
        if ((m_pos + 1u) > m_cap) {
            return false;
        }

        m_out[m_pos] = v;
        m_pos++;
        return true;
    }

    bool put_u16_be(uint16_t v) {
        if ((m_pos + 2u) > m_cap) {
            return false;
        }

        m_out[m_pos + 0u] = (uint8_t)(v >> 8);
        m_out[m_pos + 1u] = (uint8_t)(v & 0xFFu);
        m_pos += 2u;
        return true;
    }

    bool put_bytes(const void* src, uint32_t len) {
        if (!src && len != 0u) {
            return false;
        }

        if ((m_pos + len) > m_cap) {
            return false;
        }

        if (len != 0u) {
            memcpy(m_out + m_pos, src, len);
            m_pos += len;
        }

        return true;
    }

    uint32_t pos() const {
        return m_pos;
    }

private:
    uint8_t* m_out;
    uint32_t m_cap;
    uint32_t m_pos;
};

static bool encode_dns_name(const char* name, uint8_t name_len, BufWriter& w) {
    if (!name || name_len == 0u) {
        return false;
    }

    uint32_t label_start = 0u;
    uint32_t label_len = 0u;
    bool wrote_any = false;

    for (uint32_t i = 0; i < name_len; i++) {
        const char c = name[i];

        if (c == '.') {
            if (!wrote_any || label_len == 0u || label_len > 63u) {
                return false;
            }

            if (!w.put_u8((uint8_t)label_len)) {
                return false;
            }

            if (!w.put_bytes(name + label_start, label_len)) {
                return false;
            }

            label_start = i + 1u;
            label_len = 0u;
            continue;
        }

        wrote_any = true;
        label_len++;

        if (label_len > 63u) {
            return false;
        }
    }

    if (!wrote_any || label_len == 0u || label_len > 63u) {
        return false;
    }

    if (!w.put_u8((uint8_t)label_len)) {
        return false;
    }

    if (!w.put_bytes(name + label_start, label_len)) {
        return false;
    }

    if (!w.put_u8(0u)) {
        return false;
    }

    return true;
}

static bool skip_dns_name(const uint8_t* pkt, uint32_t len, uint32_t& off) {
    if (!pkt) {
        return false;
    }

    uint32_t jumps = 0;

    for (;;) {
        if (off >= len) {
            return false;
        }

        const uint8_t c = pkt[off];

        if (c == 0u) {
            off++;
            return true;
        }

        if ((c & 0xC0u) == 0xC0u) {
            if ((off + 1u) >= len) {
                return false;
            }

            off += 2u;
            return true;
        }

        const uint32_t l = (uint32_t)c;
        off++;

        if ((off + l) > len) {
            return false;
        }

        off += l;

        jumps++;
        if (jumps > 255u) {
            return false;
        }
    }
}

}

bool build_dns_a_query(
    uint16_t txid,
    const char* name,
    uint8_t name_len,
    uint8_t* out,
    uint32_t out_cap,
    uint32_t& out_len
) {
    out_len = 0;

    if (!name || !out || out_cap < 32u) {
        return false;
    }

    if (name_len == 0u || name_len > 127u) {
        return false;
    }

    const uint16_t flags = 0x0100u;
    const uint16_t qd = 1u;
    const uint16_t zero = 0;

    BufWriter w(out, out_cap);

    if (!w.put_u16_be(txid)) {
        return false;
    }

    if (!w.put_u16_be(flags)) {
        return false;
    }

    if (!w.put_u16_be(qd)) {
        return false;
    }

    if (!w.put_u16_be(zero)) {
        return false;
    }

    if (!w.put_u16_be(zero)) {
        return false;
    }

    if (!w.put_u16_be(zero)) {
        return false;
    }

    if (!encode_dns_name(name, name_len, w)) {
        return false;
    }

    const uint16_t qtype = 1u;
    const uint16_t qclass = 1u;

    if (!w.put_u16_be(qtype)) {
        return false;
    }

    if (!w.put_u16_be(qclass)) {
        return false;
    }

    out_len = w.pos();
    return true;
}

bool parse_dns_a_response(uint16_t txid, const uint8_t* pkt, uint32_t len, uint32_t& out_ip_be) {
    out_ip_be = 0;

    if (!pkt || len < 12u) {
        return false;
    }

    const uint16_t id = (uint16_t)pkt[0] << 8 | (uint16_t)pkt[1];
    if (id != txid) {
        return false;
    }

    const uint16_t flags = (uint16_t)pkt[2] << 8 | (uint16_t)pkt[3];

    const uint16_t qd = (uint16_t)pkt[4] << 8 | (uint16_t)pkt[5];
    const uint16_t an = (uint16_t)pkt[6] << 8 | (uint16_t)pkt[7];

    const uint16_t rcode = flags & 0x000Fu;
    const bool is_resp = (flags & 0x8000u) != 0u;

    if (!is_resp || rcode != 0u || qd == 0u || an == 0u) {
        return false;
    }

    uint32_t off = 12u;

    for (uint32_t i = 0; i < (uint32_t)qd; i++) {
        if (!skip_dns_name(pkt, len, off)) {
            return false;
        }

        if ((off + 4u) > len) {
            return false;
        }

        off += 4u;
    }

    for (uint32_t i = 0; i < (uint32_t)an; i++) {
        if (!skip_dns_name(pkt, len, off)) {
            return false;
        }

        if ((off + 10u) > len) {
            return false;
        }

        const uint16_t type = (uint16_t)pkt[off + 0u] << 8 | (uint16_t)pkt[off + 1u];
        const uint16_t klass = (uint16_t)pkt[off + 2u] << 8 | (uint16_t)pkt[off + 3u];
        const uint16_t rdlen = (uint16_t)pkt[off + 8u] << 8 | (uint16_t)pkt[off + 9u];

        off += 10u;

        if ((off + rdlen) > len) {
            return false;
        }

        if (type == 1u && klass == 1u && rdlen == 4u) {
            const uint32_t a = (uint32_t)pkt[off + 0u];
            const uint32_t b = (uint32_t)pkt[off + 1u];
            const uint32_t c = (uint32_t)pkt[off + 2u];
            const uint32_t d = (uint32_t)pkt[off + 3u];

            const uint32_t ip = (a << 24) | (b << 16) | (c << 8) | d;
            out_ip_be = htonl(ip);
            return true;
        }

        off += rdlen;
    }

    return false;
}

}
