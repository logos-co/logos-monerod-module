#include "tor_follow.h"

#include <algorithm>

namespace monerod {

void TorFollow::started(const std::string& inUse, Clock::time_point now, bool automatic) {
    m_inUse = inUse;
    m_pending.clear();
    if (automatic) { m_lastAuto = now; m_hadAuto = true; }
}

void TorFollow::stopped() {
    m_inUse.clear();
    m_pending.clear();
}

bool TorFollow::observe(const std::string& torSocks, Clock::time_point now) {
    const std::string before = m_pending;
    if (m_inUse.empty() || torSocks.empty() || torSocks == m_inUse) m_pending.clear();
    // The same new port seen again keeps its debounce; a different one starts it over.
    else if (torSocks != m_pending) { m_pending = torSocks; m_seenAt = now; }
    return m_pending != before;
}

Clock::time_point TorFollow::dueAt() const {
    if (m_pending.empty()) return Clock::time_point::max();
    const Clock::time_point settled = m_seenAt + kFollowDebounce;
    return m_hadAuto ? std::max(settled, m_lastAuto + kFollowMinGap) : settled;
}

std::string TorFollow::due(Clock::time_point now) const {
    return !m_pending.empty() && now >= dueAt() ? m_pending : std::string();
}

}  // namespace monerod
