/*
 *   Copyright 2014, University Corporation for Atmospheric Research
 *   See file ../COPYRIGHT for copying and redistribution conditions.
 */

/* 
 * Miscellaneous functions to make dealing with Internet addresses easier.
 */

#ifndef _INETUTIL_H_
#define _INETUTIL_H_

#include "config.h"

#include "error.h"
#include "ldm.h"
#include "rpc/types.h"
#include "rpc/xdr.h"

#include <stdbool.h>
#include <netinet/in.h>

#ifdef IPPROTO_IP /* we included netinet/in.h, so struct sockaddr_in is */
extern const char*    hostbyaddr(
    const struct sockaddr_in* const paddr);
extern int            addrbyhost(
    const char* const               id,
    struct sockaddr_in* const       paddr);
extern ErrorObj*      hostHasIpAddress(
    const char* const	            hostname,
    const in_addr_t	            targetAddr,
    int* const		            hasAddress);
extern char*          s_sockaddr_in(struct sockaddr_in *paddr);
extern int            gethostaddr_in(struct sockaddr_in *paddr);
#endif
extern int            getservport(const char *servicename, const char *proto);
extern char*          ghostname(void);
extern int            usopen(const char *name);
extern int            udpopen(const char *hostname, const char *servicename);
extern int            isMe(const char *remote);
extern int            local_sockaddr_in(struct sockaddr_in* addr);
extern int            sockbind(const char *type, unsigned short port);
/**
 * Returns the IPv4 dotted-decimal form of an Internet identifier.
 *
 * @param[in]  inetId  The Internet identifier. May be a name or a formatted
 *                     IPv4 address in dotted-decimal format.
 * @param[out] out     The corresponding form of the Internet identifier in
 *                     dotted-decimal format. It's the caller's responsibility
 *                     to ensure that the buffer can hold at least
 *                     `INET_ADDRSTRLEN` (from `<netinet/in.h>`) bytes.
 * @retval     0       Success. `out` is set.
 * @retval     EAGAIN  A necessary resource is temporarily unavailable.
 *                     `log_start()` called.
 * @retval     EINVAL  The identifier cannot be converted to an IPv4
 *                     dotted-decimal format. `log_start()` called.
 * @retval     ENOENT  No IPv4 address corresponds to the given Internet
 *                     identifier.
 * @retval     ENOMEM  Out-of-memory. `log_start()` called.
 * @retval     ENOSYS  A non-recoverable error occurred when attempting to
 *                     resolve the identifier. `log_start()` called.
 */
int
getDottedDecimal(
    const char* const    inetId,
    char* const restrict out);

#if WANT_MULTICAST
/**
 * Returns a new service address.
 *
 * @param[in] addr    Address of the service. May be hostname or formatted IP
 *                    address. Client may free upon return.
 * @param[in] port    Port number of the service.
 * @retval    NULL    Failure. \c errno will be ENOMEM.
 * @return            Pointer to a new service address object corresponding to
 *                    the input.
 */
extern ServiceAddr*   sa_new(const char* const addr, const unsigned short port);
extern void           sa_free(ServiceAddr* const sa);
/**
 * Copies a service address.
 *
 * @param[out] dest   The destination.
 * @param[in]  src    The source. The caller may free.
 * @retval     true   Success. `*dest` is set.
 * @retval     false  Failure. `log_start()` called.
 */
extern bool           sa_copy(
    ServiceAddr* const restrict       dest,
    const ServiceAddr* const restrict src);
extern ServiceAddr*   sa_clone(const ServiceAddr* const sa);
extern const char*    sa_getInetId(const ServiceAddr* const sa);
extern unsigned short sa_getPort(const ServiceAddr* const sa);
extern int            sa_snprint(const ServiceAddr* restrict sa,
                          char* restrict buf, size_t len);
/**
 * Returns the formatted representation of a service address.
 *
 * This function is thread-safe.
 *
 * @param[in]  sa    Pointer to the service address.
 * @retval     NULL  Failure. `log_add()` called.
 * @return           Pointer to the formatted representation. The caller should
 *                   free when it's no longer needed.
 */
extern char*          sa_format(const ServiceAddr* const sa);
/**
 * Parses a formatted Internet service address. An Internet service address has
 * the general form `id:port`, where `id` is the Internet identifier (either a
 * name, a formatted IPv4 address, or a formatted IPv6 address enclosed in
 * square brackets) and `port` is the port number.
 *
 * @param[out] serviceAddr  Internet service address. Caller should call
 *                          `sa_free(*sa)` when it's no longer needed.
 * @param[in]  spec         String containing the specification.
 * @retval     0            Success. `*sa` is set.
 * @retval     EINVAL       Invalid specification. `log_start()` called.
 * @retval     ENOMEM       Out of memory. `log_start()` called.
 */
int
sa_parse(
    ServiceAddr** const restrict serviceAddr,
    const char* restrict         spec);
/**
 * Returns the Internet socket address that corresponds to a service address.
 *
 * @param[in]  serviceAddr   The service address.
 * @param[in]  serverSide    Whether or not the returned socket address should be
 *                           suitable for a server's `bind()` operation.
 * @param[out] inetSockAddr  The corresponding Internet socket address. The
 *                           socket type will be `SOCK_STREAM` and the protocol
 *                           will be `IPPROTO_TCP`.
 * @param[out] sockLen       The size of the returned socket address in bytes.
 *                           Suitable for use in a `bind()` or `connect()` call.
 * @retval     0             Success.
 * @retval     EAGAIN        A necessary resource is temporarily unavailable.
 *                           `log_start()` called.
 * @retval     EINVAL        Invalid port number. `log_start()` called.
 * @retval     ENOENT        The service address doesn't resolve into an IP
 *                           address.
 * @retval     ENOMEM        Out-of-memory. `log_start()` called.
 * @retval     ENOSYS        A non-recoverable error occurred when attempting to
 *                           resolve the name. `log_start()` called.
 */
int
sa_getInetSockAddr(
    const ServiceAddr* const       servAddr,
    const bool                     serverSide,
    struct sockaddr_storage* const inetSockAddr,
    socklen_t* const               sockLen);

/**
 * Compares two service address objects. Returns a value less than, equal to, or
 * greater than zero as the first object is considered less than, equal to, or
 * greater than the second object, respectively. Service addresses are
 * considered equal if their Internet identifiers and port numbers are equal.
 *
 * @param[in] sa1  First service address object.
 * @param[in] sa2  Second service address object.
 * @retval    -1   First object is less than second.
 * @retval     0   Objects are equal.
 * @retval    +1   First object is greater than second.
 */
int
sa_compare(
    const ServiceAddr* const sa1,
    const ServiceAddr* const sa2);
#endif // WANT_MULTICAST

#endif /* !_INETUTIL_H_ */
