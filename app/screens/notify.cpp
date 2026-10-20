// NASBridge - PS5 system notifications.
// Copyright (C) 2026 NASBridge contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "concepts/notify.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(__PROSPERO__)
extern "C" {
// libkernel's request layout, as the installer (and other homebrew) use it.
struct NotifyRequest
{
    char reserved[45];
    char message[3075];
};
// Weak: a function the console's library doesn't have is left at address 0,
// and calling that crashes the app (seen on 13.60 with a ShellCoreUtil one).
__attribute__((weak)) int sceKernelSendNotificationRequest(int, NotifyRequest *, std::size_t, int);
}
#endif

namespace hui::concepts
{

namespace
{

std::mutex g_lock;
std::function<void(const std::string &)> g_hook;

} // namespace

void set_notify_hook(std::function<void(const std::string &)> hook)
{
    std::lock_guard<std::mutex> hold(g_lock);
    g_hook = std::move(hook);
}

void ps5_notify(const std::string &text)
{
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_hook)
        {
            g_hook(text);
            return;
        }
    }
#if defined(__PROSPERO__)
    int (*volatile send)(int, NotifyRequest *, std::size_t, int) = &sceKernelSendNotificationRequest;
    if (send == nullptr)
        return;
    NotifyRequest request;
    std::memset(&request, 0, sizeof(request));
    std::snprintf(request.message, sizeof(request.message), "%s", text.c_str());
    send(0, &request, sizeof(request), 0);
#endif
}

} // namespace hui::concepts
