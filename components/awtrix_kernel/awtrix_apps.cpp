#include "awtrix_apps.h"

#include <utility>

AwtrixAppRegistry& AwtrixAppRegistry::get()
{
    static AwtrixAppRegistry instance;
    return instance;
}

void AwtrixAppRegistry::clearApps()
{
    m_apps.clear();
}

void AwtrixAppRegistry::addApp(const std::string& name, AppCallback callback)
{
    m_apps.push_back({name, std::move(callback)});
}

void AwtrixAppRegistry::replaceApps(std::vector<std::pair<std::string, AppCallback>> apps)
{
    m_apps = std::move(apps);
}

void AwtrixAppRegistry::eraseApp(const std::string& name)
{
    for (auto it = m_apps.begin(); it != m_apps.end();)
    {
        if (it->first == name) it = m_apps.erase(it);
        else ++it;
    }
}

int AwtrixAppRegistry::findAppIndex(const std::string& name) const
{
    for (size_t i = 0; i < m_apps.size(); ++i)
    {
        if (m_apps[i].first == name) return (int)i;
    }
    return -1;
}

std::string AwtrixAppRegistry::appNameAt(size_t index) const
{
    return index < m_apps.size() ? m_apps[index].first : std::string();
}

void AwtrixAppRegistry::upsertCustomApp(const std::string& name, CustomApp app)
{
    m_customApps[name] = std::move(app);
}

void AwtrixAppRegistry::eraseCustomApp(const std::string& name)
{
    m_customApps.erase(name);
}

CustomApp* AwtrixAppRegistry::findCustomApp(const std::string& name)
{
    auto it = m_customApps.find(name);
    return it == m_customApps.end() ? nullptr : &it->second;
}

const CustomApp* AwtrixAppRegistry::findCustomApp(const std::string& name) const
{
    auto it = m_customApps.find(name);
    return it == m_customApps.end() ? nullptr : &it->second;
}
