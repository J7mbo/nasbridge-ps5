/* NASBridge - What libnfs asks of the services and protocols databases.
 * Copyright (C) 2026 NASBridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * config.h points libnfs's getservbyport and getprotobyname here: the PS5
 * has neither database. libnfs asks whether a low port is a well-known
 * service before it tries to bind to it (none is, here), and the number of
 * the "tcp" protocol before it sets TCP options.
 */

#include "config.h"

#include <netinet/in.h>
#include <string.h>

struct servent *nfs_getservbyport(int port, const char *proto)
{
    (void)port;
    (void)proto;
    return NULL;
}

struct protoent *nfs_getprotobyname(const char *name)
{
    static char tcp_name[] = "tcp";
    static char *aliases[] = {NULL};
    static struct protoent tcp = {tcp_name, aliases, IPPROTO_TCP};
    return name != NULL && strcmp(name, "tcp") == 0 ? &tcp : NULL;
}
