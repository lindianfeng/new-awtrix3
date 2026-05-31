#include "awtrix_notifications.h"

AwtrixNotificationManager& AwtrixNotificationManager::get()
{
    static AwtrixNotificationManager instance;
    return instance;
}

void AwtrixNotificationManager::enqueue(const AwtrixNotification& notification)
{
    portENTER_CRITICAL(&m_mux);
    m_queue.push_back(notification);
    portEXIT_CRITICAL(&m_mux);
}

void AwtrixNotificationManager::replaceHead(const AwtrixNotification& notification)
{
    portENTER_CRITICAL(&m_mux);
    if (m_queue.empty()) m_queue.push_back(notification);
    else m_queue[0] = notification;
    portEXIT_CRITICAL(&m_mux);
}

void AwtrixNotificationManager::dismiss()
{
    portENTER_CRITICAL(&m_mux);
    if (!m_queue.empty()) m_queue.erase(m_queue.begin());
    portEXIT_CRITICAL(&m_mux);
}

bool AwtrixNotificationManager::empty() const
{
    portENTER_CRITICAL(&m_mux);
    const bool e = m_queue.empty();
    portEXIT_CRITICAL(&m_mux);
    return e;
}

std::vector<AwtrixNotification>& AwtrixNotificationManager::queue()
{
    /* Caller MUST hold Lock(mux()) — see header. */
    return m_queue;
}

const std::vector<AwtrixNotification>& AwtrixNotificationManager::queue() const
{
    return m_queue;
}

portMUX_TYPE* AwtrixNotificationManager::mux()
{
    return &m_mux;
}
