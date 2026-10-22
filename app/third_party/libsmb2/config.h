/* NASBridge - libsmb2's build settings, in place of the config.h its CMake
 * build would generate. Copyright (C) 2026 NASBridge contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * libsmb2 is built from its own sources (pinned in the Dockerfile) as part of
 * the app, for the PS5 and, for the tests, for this computer. Every libsmb2
 * file includes this first (the build defines HAVE_CONFIG_H).
 */

#ifndef NASBRIDGE_LIBSMB2_CONFIG_H
#define NASBRIDGE_LIBSMB2_CONFIG_H

#define HAVE_ARPA_INET_H 1
#define HAVE_FCNTL_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_NETDB_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_NETINET_TCP_H 1
#define HAVE_POLL_H 1
#define HAVE_SOCKADDR_STORAGE 1
#define HAVE_LINGER 1
#define HAVE_STDINT_H 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_ERRNO_H 1
#define STDC_HEADERS 1
#ifdef __FreeBSD__ /* the PS5 */
#define HAVE_SOCKADDR_LEN 1
#endif

#define PACKAGE_VERSION "6.2"

/* What libsmb2's own build passes on the command line. */
#define _U_ __attribute__((unused))
/* The UI kit builds C as -std=c11, where typeof is spelled __typeof__. */
#define typeof __typeof__

/* TCP_NODELAY's level, so libsmb2 needn't look up "tcp" (no protocol
 * database on the PS5). */
#ifndef SOL_TCP
#define SOL_TCP 6
#endif

/* Names can't be looked up from a native PS5 app (see http.hpp), and
 * getaddrinfo lives in a module the app doesn't load. libsmb2 is always given
 * an IPv4 address, so it uses NASBridge's own small getaddrinfo
 * (smb2_compat.c), on the PS5 and on this computer alike. */
#define getaddrinfo smb2_getaddrinfo
#define freeaddrinfo smb2_freeaddrinfo

#endif
