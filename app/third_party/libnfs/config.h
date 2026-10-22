/* NASBridge - libnfs's build settings, in place of the config.h its CMake
 * build would generate. Copyright (C) 2026 NASBridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libnfs is built from its own sources (pinned in the Dockerfile) as part of
 * the app and the copy helper, for the PS5 and, for the tests, for this
 * computer. Every libnfs file includes this first (the build defines
 * HAVE_CONFIG_H). No threads of its own (NASBridge gives each thread its own
 * context), no Kerberos and no TLS: NFSv3 with the client's user and group IDs.
 *
 * libsmb2 has a config.h of its own, found through the include path, so the
 * app's build puts a copy of this one beside every libnfs source and header
 * that includes it: a quoted include looks in the file's own folder first.
 */

#ifndef NASBRIDGE_LIBNFS_CONFIG_H
#define NASBRIDGE_LIBNFS_CONFIG_H

#define HAVE_ARPA_INET_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_NET_IF_H 1
#define HAVE_POLL_H 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_STATVFS_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1
#define STDC_HEADERS 1
#ifdef __FreeBSD__ /* the PS5 */
#define HAVE_SOCKADDR_LEN 1
#define HAVE_SYS_FILIO_H 1
#endif
#ifdef __linux__ /* the tests */
#define HAVE_SYS_SYSMACROS_H 1
#endif

/* What libnfs's own build passes on the command line. */
#define _U_ __attribute__((unused))
/* The UI kit builds C as -std=c11, where typeof is spelled __typeof__. */
#define typeof __typeof__

/* Names can't be looked up from a native PS5 app, so libnfs, like libsmb2,
 * uses NASBridge's small IPv4-only getaddrinfo (libsmb2/smb2_compat.c).
 * There's no services or protocols database on the PS5 either:
 * nfs_compat.c answers the two questions libnfs asks of them. */
#define getaddrinfo smb2_getaddrinfo
#define freeaddrinfo smb2_freeaddrinfo
#define getservbyport nfs_getservbyport
#define getprotobyname nfs_getprotobyname

/* libnfs reads the clock with Linux's CLOCK_MONOTONIC_COARSE. */
#include <time.h>
#ifndef CLOCK_MONOTONIC_COARSE
#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC
#endif

#include <netdb.h>
#ifdef __cplusplus
extern "C" {
#endif
struct servent *nfs_getservbyport(int port, const char *proto);
struct protoent *nfs_getprotobyname(const char *name);
struct addrinfo;
int smb2_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                     struct addrinfo **res);
void smb2_freeaddrinfo(struct addrinfo *res);
#ifdef __cplusplus
}
#endif

#endif
