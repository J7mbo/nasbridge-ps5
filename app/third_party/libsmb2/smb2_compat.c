/* NASBridge - The address lookup libsmb2 uses: IPv4 addresses only.
 * Copyright (C) 2026 NASBridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * config.h points libsmb2's getaddrinfo and freeaddrinfo here. A name that
 * isn't an IPv4 address fails, as NASBridge's WebDAV client does.
 */

#include "config.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

int smb2_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                     struct addrinfo **res)
{
    (void)hints;
    *res = NULL;
    struct sockaddr_in *sin = calloc(1, sizeof(*sin));
    struct addrinfo *info = calloc(1, sizeof(*info));
    if (sin == NULL || info == NULL)
    {
        free(sin);
        free(info);
        return EAI_MEMORY;
    }
    if (node == NULL || inet_pton(AF_INET, node, &sin->sin_addr) != 1)
    {
        free(sin);
        free(info);
        return EAI_NONAME;
    }
#ifdef HAVE_SOCKADDR_LEN
    sin->sin_len = sizeof(*sin);
#endif
    sin->sin_family = AF_INET;
    sin->sin_port = htons(service != NULL ? (unsigned short)atoi(service) : 0);
    info->ai_family = AF_INET;
    info->ai_socktype = SOCK_STREAM;
    info->ai_protocol = IPPROTO_TCP;
    info->ai_addrlen = sizeof(*sin);
    info->ai_addr = (struct sockaddr *)sin;
    *res = info;
    return 0;
}

void smb2_freeaddrinfo(struct addrinfo *res)
{
    while (res != NULL)
    {
        struct addrinfo *next = res->ai_next;
        free(res->ai_addr);
        free(res);
        res = next;
    }
}
