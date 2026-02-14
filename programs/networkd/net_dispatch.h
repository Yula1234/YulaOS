#ifndef YOS_NETD_DISPATCH_H
#define YOS_NETD_DISPATCH_H

#include "net_hash_map.h"
#include "net_core.h"
#include "net_proto.h"

#include <stdint.h>

namespace netd {

namespace detail {

template <typename K, typename EntryT, uint32_t SmallCap>
class DispatchTable {
public:
    DispatchTable() : m_map(), m_small() {
    }

    explicit DispatchTable(Arena& arena) : m_map(arena), m_small() {
    }

    DispatchTable(const DispatchTable&) = delete;
    DispatchTable& operator=(const DispatchTable&) = delete;

    void bind(Arena& arena) {
        m_map.bind(arena);
    }

    bool reserve(uint32_t n) {
        if (n <= SmallCap) {
            return true;
        }

        if (!m_map.reserve(n)) {
            return false;
        }

        promote_to_map();
        return true;
    }

    bool put(K key, const EntryT& e) {
        Slot* existing = find_small(key);
        if (existing) {
            existing->entry = e;
            return true;
        }

        if (m_map.capacity() != 0) {
            return m_map.put((uint32_t)key, e);
        }

        if (m_small.size() < SmallCap) {
            Slot s{};
            s.key = key;
            s.entry = e;
            return m_small.push_back(s);
        }

        if (!m_map.reserve(SmallCap * 2u)) {
            return false;
        }

        promote_to_map();
        return m_map.put((uint32_t)key, e);
    }

    bool get(K key, EntryT& out) const {
        if (m_map.capacity() == 0) {
            const Slot* s = find_small_const(key);
            if (!s) {
                return false;
            }

            out = s->entry;
            return true;
        }

        return m_map.get((uint32_t)key, out);
    }

private:
    struct Slot {
        K key;
        EntryT entry;
    };

    Slot* find_small(K key) {
        for (uint32_t i = 0; i < m_small.size(); i++) {
            if (m_small[i].key == key) {
                return &m_small[i];
            }
        }

        return nullptr;
    }

    const Slot* find_small_const(K key) const {
        for (uint32_t i = 0; i < m_small.size(); i++) {
            const Slot& s = m_small[i];
            if (s.key == key) {
                return &s;
            }
        }

        return nullptr;
    }

    void promote_to_map() {
        if (m_map.capacity() == 0) {
            return;
        }

        while (m_small.size() != 0u) {
            const Slot s = m_small[m_small.size() - 1u];
            (void)m_map.put((uint32_t)s.key, s.entry);
            m_small.erase_unordered(m_small.size() - 1u);
        }
    }

    HashMap<uint32_t, EntryT> m_map;
    StaticVec<Slot, SmallCap> m_small;
};

}

class EthertypeDispatch {
public:
    using HandlerFn = void (*)(void* ctx, const uint8_t* frame, uint32_t len, uint32_t now_ms);

    static constexpr uint32_t SmallCap = 8u;

    EthertypeDispatch() : m_tab() {
    }

    explicit EthertypeDispatch(Arena& arena) : m_tab(arena) {
    }

    EthertypeDispatch(const EthertypeDispatch&) = delete;
    EthertypeDispatch& operator=(const EthertypeDispatch&) = delete;

    void bind(Arena& arena) {
        m_tab.bind(arena);
    }

    bool reserve(uint32_t n) {
        return m_tab.reserve(n);
    }

    bool add(uint16_t ethertype, void* ctx, HandlerFn fn) {
        Entry e{};
        e.ctx = ctx;
        e.fn = fn;

        return m_tab.put(ethertype, e);
    }

    bool dispatch(uint16_t ethertype, const uint8_t* frame, uint32_t len, uint32_t now_ms) const {
        Entry e{};
        if (!m_tab.get(ethertype, e)) {
            return false;
        }

        if (!e.fn) {
            return false;
        }

        e.fn(e.ctx, frame, len, now_ms);
        return true;
    }

private:
    struct Entry {
        void* ctx;
        HandlerFn fn;
    };

    detail::DispatchTable<uint16_t, Entry, SmallCap> m_tab;
};

class IpProtoDispatch {
public:
    using HandlerFn = bool (*)(void* ctx, const EthHdr* eth, const Ipv4Hdr* ip, const uint8_t* payload, uint32_t payload_len, uint32_t now_ms);

    static constexpr uint32_t SmallCap = 8u;

    IpProtoDispatch() : m_tab() {
    }

    explicit IpProtoDispatch(Arena& arena) : m_tab(arena) {
    }

    IpProtoDispatch(const IpProtoDispatch&) = delete;
    IpProtoDispatch& operator=(const IpProtoDispatch&) = delete;

    void bind(Arena& arena) {
        m_tab.bind(arena);
    }

    bool reserve(uint32_t n) {
        return m_tab.reserve(n);
    }

    bool add(uint8_t proto, void* ctx, HandlerFn fn) {
        Entry e{};
        e.ctx = ctx;
        e.fn = fn;

        return m_tab.put(proto, e);
    }

    bool dispatch(uint8_t proto, const EthHdr* eth, const Ipv4Hdr* ip, const uint8_t* payload, uint32_t payload_len, uint32_t now_ms) const {
        Entry e{};
        if (!m_tab.get(proto, e)) {
            return false;
        }

        if (!e.fn) {
            return false;
        }

        return e.fn(e.ctx, eth, ip, payload, payload_len, now_ms);
    }

private:
    struct Entry {
        void* ctx;
        HandlerFn fn;
    };

    detail::DispatchTable<uint8_t, Entry, SmallCap> m_tab;
};

class IpcMsgDispatch {
public:
    using HandlerFn = bool (*)(void* handler_ctx, void* call_ctx, uint16_t type, uint32_t seq, const uint8_t* payload, uint32_t len, uint32_t now_ms);

    static constexpr uint32_t SmallCap = 8u;

    IpcMsgDispatch() : m_tab() {
    }

    explicit IpcMsgDispatch(Arena& arena) : m_tab(arena) {
    }

    IpcMsgDispatch(const IpcMsgDispatch&) = delete;
    IpcMsgDispatch& operator=(const IpcMsgDispatch&) = delete;

    void bind(Arena& arena) {
        m_tab.bind(arena);
    }

    bool reserve(uint32_t n) {
        return m_tab.reserve(n);
    }

    bool add(uint16_t type, void* ctx, HandlerFn fn) {
        Entry e{};
        e.ctx = ctx;
        e.fn = fn;

        return m_tab.put(type, e);
    }

    bool dispatch(uint16_t type, void* call_ctx, uint32_t seq, const uint8_t* payload, uint32_t len, uint32_t now_ms) const {
        Entry e{};
        if (!m_tab.get(type, e)) {
            return false;
        }

        if (!e.fn) {
            return false;
        }

        return e.fn(e.ctx, call_ctx, type, seq, payload, len, now_ms);
    }

private:
    struct Entry {
        void* ctx;
        HandlerFn fn;
    };

    detail::DispatchTable<uint16_t, Entry, SmallCap> m_tab;
};

}

#endif
