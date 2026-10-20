// NASBridge - tests: wait for the fake NAS (tests/fake_nas.py) to listen,
// rather than sleeping a fixed time after starting it.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// True once something accepts connections on 127.0.0.1:port (up to 10 s).
inline bool wait_for_port(int port)
{
    for (int i = 0; i < 400; ++i)
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<unsigned short>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool up =
            ::connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0;
        ::close(fd);
        if (up)
            return true;
        timespec pause{0, 25 * 1000000L};
        nanosleep(&pause, nullptr);
    }
    return false;
}
