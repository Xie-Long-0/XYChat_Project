#pragma once

#include <QMutex>
#include <QMutexLocker>
#include <QHash>
#include <QSet>

// M11C C2 加固：连接级在线注册表。
//
// `sessions.last_active_at` 的 120 秒阈值只反映"最近发过请求"——客户端连着
// 但空闲（不发消息、不刷新列表）就会被判为离线。这里记录的是"当前仍有已认证
// 连接"，由 handler 在登录成功/登出/断开时同步维护。
//
// handler 各在独立线程运行，注册表被跨线程读写，故整体加锁；临界区只做集合
// 增删与遍历，不在持锁期间做 I/O。sessionId 全局唯一且删除幂等，登出与断线
// 两条清理路径重复调用无副作用。
class OnlineRegistry
{
public:
    void addSession(qint64 userId, qint64 sessionId)
    {
        if (userId <= 0 || sessionId <= 0) {
            return;
        }
        QMutexLocker locker(&m_mutex);
        m_sessionsByUser[userId].insert(sessionId);
    }

    void removeSession(qint64 userId, qint64 sessionId)
    {
        if (userId <= 0 || sessionId <= 0) {
            return;
        }
        QMutexLocker locker(&m_mutex);
        auto it = m_sessionsByUser.find(userId);
        if (it == m_sessionsByUser.end()) {
            return;
        }
        it->remove(sessionId);
        if (it->isEmpty()) {
            m_sessionsByUser.erase(it);
        }
    }

    bool isOnline(qint64 userId) const
    {
        if (userId <= 0) {
            return false;
        }
        QMutexLocker locker(&m_mutex);
        const auto it = m_sessionsByUser.constFind(userId);
        return it != m_sessionsByUser.constEnd() && !it->isEmpty();
    }

    QSet<qint64> onlineUserIds() const
    {
        QMutexLocker locker(&m_mutex);
        QSet<qint64> ids;
        for (auto it = m_sessionsByUser.constBegin(); it != m_sessionsByUser.constEnd(); ++it) {
            ids.insert(it.key());
        }
        return ids;
    }

    int onlineUserCount() const
    {
        QMutexLocker locker(&m_mutex);
        return m_sessionsByUser.size();
    }

private:
    mutable QMutex m_mutex;
    // userId -> 该账号当前已认证的 sessionId 集合（多设备/多连接）
    QHash<qint64, QSet<qint64>> m_sessionsByUser;
};
