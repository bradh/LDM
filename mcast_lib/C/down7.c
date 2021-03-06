/**
 * Copyright 2014 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 *   @file: down7.c
 * @author: Steven R. Emmerson
 *
 * This file implements a downstream LDM-7, which executes on its own threads
 * to
 *     - Subscribe to a data-stream from an upstream LDM-7;
 *     - Receive multicast data-products;
 *     - Request data-products that were missed by the multicast receiver, and
 *     - Receive those requested data-products.
 */

#include "config.h"

#include "down7.h"
#include "executor.h"
#include "prod_index_queue.h"
#include "globals.h"
#include "inetutil.h"
#include "ldm.h"
#include "ldmprint.h"
#include "log.h"
#include "mcast.h"
#include "mldm_receiver.h"
#include "mldm_receiver_memory.h"
#include "pq.h"
#include "rpc/rpc.h"
#include "rpcutil.h"
#include "timestamp.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>
#include <unistd.h>

/**
 * Key for getting the pointer to a downstream LDM-7 that's associated with a
 * thread:
 */
static pthread_key_t  down7Key;
/**
 * Lockout for initializing `down7Key`:
 */
static pthread_once_t down7KeyControl = PTHREAD_ONCE_INIT;

/**
 * The data structure of a downstream LDM-7:
 */
struct Down7 {
    pqueue*               pq;            ///< pointer to the product-queue
    ServiceAddr*          servAddr;      ///< socket address of remote LDM-7
    feedtypet             feedtype;      ///< feed-expression of multicast group
    CLIENT*               clnt;          ///< client-side RPC handle
    McastInfo*            mcastInfo;     ///< information on multicast group
    Mlr*                  mlr;           ///< multicast LDM receiver
    McastSessionMemory*   msm;           ///< persistent multicast session memory
    /** Starts other threads and waits on them */
    pthread_t             receiveThread; ///< thread for receiving products
    pthread_t             requestThread; ///< thread for requesting products
    pthread_t             mcastThread;   ///< thread for multicast reception
    pthread_mutex_t       waitMutex;     ///< mutex for waiting
    pthread_cond_t        waitCond;      ///< condition-variable for waiting
    /** Synchronizes multiple-thread access to client-side RPC handle */
    pthread_mutex_t       clntMutex;
    int                   exitStatus;    ///< status of first exited task
    int                   sock;          ///< socket with remote LDM-7
    volatile sig_atomic_t taskExited;    ///< whether a task exited
    bool                  mcastWorking;  ///< product received via multicast?
    /**
     * Signature of the first data-product received by the associated multicast
     * LDM receiver during the current session.
     */
    signaturet            firstMcast;
    /**
     * Signature of the last data-product received by the associated multicast
     * LDM receiver during the previous session.
     */
    signaturet            prevLastMcast;
    /**
     * Whether or not `prevLastMcast` is set:
     */
    bool                  prevLastMcastSet;
};

/**
 * Locks the wait-lock of a downstream LDM-7 for exclusive access.
 *
 * @param[in] down7  Pointer to the downstream LDM-7 to have its wait-lock
 *                   locked.
 */
static void
lockWait(
    Down7* const down7)
{
    (void)pthread_mutex_lock(&down7->waitMutex);
}

/**
 * Unlocks the wait-lock of a downstream LDM-7.
 *
 * @param[in] down7  Pointer to the downstream LDM-7 to have its wait-lock
 *                   unlocked.
 */
static void
unlockWait(
    Down7* const down7)
{
    (void)pthread_mutex_unlock(&down7->waitMutex);
}

/**
 * Locks the client-side handle of a downstream LDM-7 for exclusive access.
 *
 * @param[in] down7    Pointer to the downstream LDM-7 to have its client-side
 *                     handle locked.
 * @retval    0        Success.
 * @retval    EDEADLK  A deadlock condition was detected or the current thread
 *                     already owns the mutex.
 */
static int
lockClient(
    Down7* const down7)
{
    return pthread_mutex_lock(&down7->clntMutex);
}

/**
 * Unlocks the client-side handle of a downstream LDM-7.
 *
 * @param[in] down7  Pointer to the downstream LDM-7 to have its client-side
 *                   handle unlocked.
 */
static void
unlockClient(
    Down7* const down7)
{
    (void)pthread_mutex_unlock(&down7->clntMutex);
}

/**
 * Performs common exit actions for a task of a downstream LDM-7:
 *   -# Logs outstanding error messages if the downstream LDM-7 wasn't shutdown;
 *   -# Frees log-message resources of the current thread;
 *   -# Sets the status of the first task to exit a downstream LDM-7;
 *   -# Sets the task-exited flag of the downstream LDM-7; and
 *   -# Signals the wait condition-variable.
 *
 * @param[in] arg     Pointer to the downstream LDM-7.
 * @param[in] status  Return status of the exiting task.
 */
static void
taskExit(
    Down7* const down7,
    int const    status)
{
    /*
     * Finish with logging.
     */
    done ? log_clear() : log_log(LOG_ERR);      // because end of thread
    log_free();

    /*
     * Inform the managing thread.
     */
    lockWait(down7);
    if (down7->exitStatus < 0)
        down7->exitStatus = status;
    down7->taskExited = 1;
    pthread_cond_signal(&down7->waitCond);
    unlockWait(down7);
}

/**
 * Returns a socket that's connected to an Internet server via TCP.
 *
 * @param[in]  servAddr       Pointer to the address of the server.
 * @param[out] sock           Pointer to the socket to be set. The client should
 *                            call \c close(*sock) when it's no longer needed.
 * @param[out] sockAddr       Pointer to the socket address object to be set.
 * @retval     0              Success. \c *sock and \c *sockAddr are set.
 * @retval     LDM7_INVAL     Invalid port number or host identifier. \c
 *                            log_start() called.
 * @retval     LDM7_IPV6      IPv6 not supported. \c log_start() called.
 * @retval     LDM7_REFUSED   Remote LDM-7 refused connection. \c log_start()
 *                            called.
 * @retval     LDM7_TIMEDOUT  Connection attempt timed-out. \c log_start()
 *                            called.
 * @retval     LDM7_SYSTEM    System error. \c log_start() called.
 */
static int
getSocket(
    const ServiceAddr* const restrict       servAddr,
    int* const restrict                     sock,
    struct sockaddr_storage* const restrict sockAddr)
{
    struct sockaddr_storage addr;
    socklen_t               sockLen;
    int                     status = sa_getInetSockAddr(servAddr, false, &addr,
            &sockLen);

    if (status == 0) {
        const int         useIPv6 = addr.ss_family == AF_INET6;
        const char* const addrFamilyId = useIPv6 ? "IPv6" : "IPv4";
        const int         fd = socket(addr.ss_family, SOCK_STREAM, IPPROTO_TCP);

        if (fd == -1) {
            LOG_SERROR1("Couldn't create %s TCP socket", addrFamilyId);
            status = (useIPv6 && errno == EAFNOSUPPORT)
                    ? LDM7_IPV6
                    : LDM7_SYSTEM;
        }
        else {
            if (connect(fd, (struct sockaddr*)&addr, sockLen)) {
                LOG_SERROR3("Couldn't connect %s TCP socket to \"%s\", port %hu",
                        addrFamilyId, sa_getInetId(servAddr),
                        sa_getPort(servAddr));
                (void)close(fd);
                status = (errno == ETIMEDOUT)
                        ? LDM7_TIMEDOUT
                        : (errno == ECONNREFUSED)
                          ? LDM7_REFUSED
                          : LDM7_SYSTEM;
            }
            else {
                *sock = fd;
                *sockAddr = addr;
            }
        } /* "fd" is open */
    } /* "addr" is set */

    return status;
}

/**
 * Returns a client-side RPC handle to a remote LDM-7.
 *
 * @param[out] client         Address of pointer to client-side handle. The
 *                            client should call \c clnt_destroy(*client) when
 *                            it is no longer needed.
 * @param[in]  servAddr       Pointer to the address of the server.
 * @param[out] socket         Pointer to the socket to be set. The client should
 *                            call \c close(*socket) when it's no longer needed.
 * @retval     0              Success. \c *client and \c *sock are set.
 * @retval     LDM7_INVAL     Invalid port number or host identifier. \c
 *                            log_start() called.
 * @retval     LDM7_REFUSED   Remote LDM-7 refused connection. \c log_start()
 *                            called.
 * @retval     LDM7_RPC       RPC error. \c log_start() called.
 * @retval     LDM7_SYSTEM    System error. \c log_start() called.
 * @retval     LDM7_TIMEDOUT  Connection attempt timed-out. \c log_start() called.
 * @retval     LDM7_UNAUTH    Not authorized. \c log_start() called.
 */
static int
newClient(
    CLIENT** const restrict           client,
    const ServiceAddr* const restrict servAddr,
    int* const restrict               socket)
{
    int                     sock;
    struct sockaddr_storage sockAddr;
    int                     status = getSocket(servAddr, &sock, &sockAddr);

    if (status == 0) {
        /*
         * "clnttcp_create()" expects a pointer to a "struct sockaddr_in", but
         * a pointer to a "struct sockaddr_in6" object may be used if the socket
         * value is non-negative and the port field of the socket address
         * structure is non-zero. Both conditions are true at this point.
         */
        CLIENT* const clnt = clnttcp_create((struct sockaddr_in*)&sockAddr,
                LDMPROG, SEVEN, &sock, 0, 0);

        if (clnt == NULL) {
            LOG_SERROR3("Couldn't create RPC client for host \"%s\", "
                    "port %hu: %s", sa_getInetId(servAddr),
                    sa_getPort(servAddr), clnt_spcreateerror(""));
            (void)close(sock);
            status = clntStatusToLdm7Status(rpc_createerr.cf_stat);
        }
        else {
            *client = clnt;
            *socket = sock;
        }
    } /* "sock" allocated */

    return status;
}

#if 0
/**
 * Tests the connection to an upstream LDM-7 by sending a no-op message to it.
 *
 * @param[in] down7     Pointer to the downstream LDM-7.
 * @retval    0         Success. The connection is still good.
 * @retval    LDM7_RPC  RPC error. `log_start()` called.
 */
static int
testConnection(
    Down7* const down7)
{
    int status;

    (void)lockClient(down7);
    test_connection_7(NULL, down7->clnt);

    if (clnt_stat(down7->clnt) == RPC_TIMEDOUT) {
        /*
         * "test_connection_7()" uses asynchronous message-passing, so the
         * status will always be RPC_TIMEDOUT unless an error occurs.
         */
        unlockClient(down7);
        status = 0;
    }
    else {
	LOG_START1("test_connection_7() failure: %s", clnt_errmsg(down7->clnt));
        unlockClient(down7);
        status = LDM7_RPC;
    }

    return status;
}
#endif

/**
 * Runs an RPC-based server. Doesn't return until an error occurs.
 *
 * @param[in] xprt           Pointer to the RPC service transport.
 * @param[in] timeout        Timeout interval or NULL for indefinite timeout.
 * @retval    LDM7_RPC       Error in RPC layer. `log_start()` called.
 * @retval    LDM7_SYSTEM    System error. `log_start()` called.
 * @retval    LDM7_TIMEDOUT  Time-out occurred.
 */
static int
run_svc(
    SVCXPRT* const                        xprt,
    const struct timespec* const restrict timeout)
{
    #define SOCK (xprt->xp_sock)

    for (;;) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(SOCK, &fds);

        /*
         * `pselect()` is used because it doesn't modify its timeout argument
         * whereas `select()` does.
         */
        const int status = pselect(SOCK+1, &fds, 0, 0, timeout, NULL);
        if (0 == status) {
            LOG_START1("pselect() on socket %d timed-out", SOCK);
            return LDM7_TIMEDOUT;
        }
        if (0 > status) {
            LOG_SERROR1("pselect() error on socket %d", SOCK);
            return LDM7_SYSTEM;
        }

        /*
         * The socket is ready for reading.
         */
        svc_getreqsock(SOCK); /* Process RPC message. Calls ldmprog_7() */

        if (FD_ISSET(SOCK, &svc_fdset))
            continue;

        /*
         * The RPC layer closed the socket and destroyed the associated
         * SVCXPRT structure.
         */
         LOG_START0("RPC layer closed connection");
         return LDM7_RPC;
    }

    return 0; // Eclipse IDE wants to see a return
}

/**
 * Runs the RPC-based data-product receiving service of a downstream LDM-7.
 * Executes until an unrecoverable error occurs.
 *
 * @param[in] down7          Pointer to the downstream LDM-7.
 * @param[in] xprt           Pointer to the server-side transport object.
 * @retval    LDM7_RPC       An RPC error occurred. `log_start()` called.
 * @retval    LDM7_SYSTEM    System error. `log_start()` called.
 */
static int
run_down7_svc(
    Down7* const restrict   down7,
    SVCXPRT* const restrict xprt)
{
    /*
     * The RPC-based server doesn't know its associated downstream LDM-7;
     * therefore, a thread-specific pointer to the downstream LDM-7 is set to
     * provide context to the server.
     */
    int status = pthread_setspecific(down7Key, down7);

    if (status) {
        LOG_ERRNUM0(status,
                "Couldn't set thread-specific pointer to downstream LDM-7");
        status = LDM7_SYSTEM;
    }
    else {
        for (;;) {
            status = run_svc(xprt, NULL); // indefinite timeout

#if 0
            /*
             * If `run_svc()` is given an indefinite timeout then the following
             * if-statement is superfluous.
             */
            if (status == LDM7_TIMEDOUT && (status = testConnection(down7)) == 0)
                continue; // connection is still good
#endif

            LOG_ADD0("Connection to upstream LDM-7 is broken");
            break;
        }
    } // thread-specific pointer to downstream LDM-7 is set

    return status;
}

/**
 * Requests a data-product that was missed by the multicast LDM receiver.
 *
 * @param[in] down7       Pointer to the downstream LDM-7.
 * @param[in] prodId      LDM product-ID of missed data-product.
 * @retval    0           Success. A data-product was requested.
 * @retval    LDM7_RPC    RPC error. `log_start()` called.
 */
static int
requestProduct(
    Down7* const         down7,
    VcmtpProdIndex const iProd)
{
    int    status;
#   define CLNT (down7->clnt)

    (void)lockClient(down7);
    (void)request_product_7((VcmtpProdIndex*)&iProd, CLNT); // asynchronous send

    if (clnt_stat(CLNT) != RPC_TIMEDOUT) {
        /*
         * The status will always be RPC_TIMEDOUT unless an error occurs
         * because `request_product_7()` uses asynchronous message-passing.
         */
        LOG_START1("request_product_7() failure: %s", clnt_errmsg(CLNT));
        status = LDM7_RPC;
    }
    else {
        status = 0;
    }

    unlockClient(down7);

    return status;
}

/**
 * Requests the backlog of data-products from the previous session. The backlog
 * comprises all products since the last product received by the associated
 * multicast LDM receiver from the previous session (or the time-offset if
 * that product isn't found) to the first product received by the associated
 * multicast LDM receiver of this session (or the current time if that product
 * isn't found). Called by `pthread_create()`.
 *
 * NB: If the current session ends before all backlog products have been
 * received, then the backlog products that weren't received will never be
 * received.
 *
 * This function blocks until the client-side handle is available.
 *
 * @param[in] arg       Pointer to downstream LDM-7.
 * @retval    0         Success.
 * @retval    LDM7_RPC  Error in RPC layer. `log_log()` called.
 */
static void*
requestSessionBacklog(
    void* const restrict arg)
{
    Down7* const down7 = (Down7*)arg;
    BacklogSpec  spec;
    int          status;

    if (down7->prevLastMcastSet)
        (void)memcpy(spec.after, down7->prevLastMcast, sizeof(signaturet));
    spec.afterIsSet = down7->prevLastMcastSet;
    (void)memcpy(spec.before, down7->firstMcast, sizeof(signaturet));
    spec.timeOffset = getTimeOffset();

    (void)lockClient(down7);
    (void)request_backlog_7(&spec, down7->clnt);
    if (clnt_stat(down7->clnt) != RPC_TIMEDOUT) {
        /*
         * The status will always be RPC_TIMEDOUT unless an error occurs
         * because `request_backlog_7()` uses asynchronous message-passing.
         */
        LOG_ADD1("request_backlog_7() failure: %s", clnt_errmsg(down7->clnt));
        status = LDM7_RPC;
    }
    else {
        status = 0;
    }
    (void)unlockClient(down7);

    log_log(LOG_ERR); // because end of thread

    return (void*)status;
}

/**
 * Requests from the associated upstream LDM-7, the next product in a downstream
 * LDM-7's missed-but-not-requested queue. Blocks until the queue has a product,
 * or the queue is shut down, or an error occurs.
 *
 * @param[in] down7          Pointer to the downstream LDM-7.
 * @retval    0              Success.
 * @retval    LDM7_SHUTDOWN  The missed-but-not-requested queue has been shut
 *                           down.
 * @retval    LDM7_SYSTEM    System error. `log_start()` called.
 * @retval    LDM7_RPC       Error in RPC layer. `log_start()` called.
 */
static inline int // inlined because only called by one small function
makeRequest(
    Down7* const down7)
{
    int            status;
    VcmtpProdIndex iProd;

    /*
     * The semantics and order of the following actions are necessary to
     * preserve the meaning of the two queues and to ensure that all missed
     * data-products are received following a restart.
     */
    if (msm_peekMissedFileWait(down7->msm, &iProd)) {
        udebug("The queue of missed data-products has been shutdown");
        status = LDM7_SHUTDOWN;
    }
    else {
        if (msm_addRequestedFile(down7->msm, iProd)) {
            LOG_ADD0("Couldn't add VCMTP product-index to requested-queue");
            status = LDM7_SYSTEM;
        }
        else {
            /* The queue can't be empty */
            (void)msm_removeMissedFileNoWait(down7->msm, &iProd);

            if ((status = requestProduct(down7, iProd)) != 0)
                LOG_ADD0("Couldn't request missed data-product");
        } // product-index added to requested-but-not-received queue
    } // have a peeked-at product-index from the missed-but-not-requested queue

    return status;
}

/**
 * Requests data-products that were missed by the multicast LDM receiver.
 * Entries from the missed-but-not-requested queue are removed and converted
 * into requests for missed data-products, which are asynchronously sent to the
 * remote LDM-7. Blocks until the request-queue is shut down or an unrecoverable
 * error occurs. Called by `pthread_create()`.
 *
 * @param[in] arg            Pointer to the downstream LDM-7 object.
 * @retval    LDM7_RPC       Error in RPC layer. \c log_start() called.
 * @retval    LDM7_SHUTDOWN  The request-queue was shut down.
 * @retval    LDM7_SYSTEM    System error. `log_start()` called.
 */
static void*
startRequester(
    void* const arg)
{
    Down7* const down7 = (Down7*)arg;
    int          status;

    do {
        status = makeRequest(down7);
    } while (status == 0);

    taskExit(down7, status);

    return (void*)status;
}

/**
 * Cleanly stops the executing task of a downstream LDM-7 that's requesting
 * data-products that were missed by the multicast LDM receiver by shutting down
 * the queue of missed products and shutting down the socket to the remote LDM-7
 * for writing.
 *
 * @param[in] down7  Pointer to the downstream LDM-7 whose requesting task is
 *                   to be stopped.
 */
static void
stopRequester(
    Down7* const down7)
{
    msm_shutDownMissedFiles(down7->msm);
    (void)shutdown(down7->sock, SHUT_WR);
}

/**
 * Receives unicast data-products from the associated upstream LDM-7 -- either
 * because they were missed by the multicast LDM receiver or because they are
 * part of the backlog. Called by `pthread_create()`.
 *
 * NB: When this function returns, the TCP socket will have been closed.
 *
 * @param[in] arg            Pointer to the downstream LDM-7.
 * @retval    0              Success.
 * @retval    LDM7_RPC       RPC error. \c log_start() called.
 * @retval    LDM7_TIMEDOUT  A timeout occurred. `log_start()` called.
 * @retval    LDM7_SYSTEM    System error. `log_log()` called.
 */
static void*
startUnicastProductReceiver(
    void* const arg)
{
    Down7* const       down7 = (Down7*)arg;
    ServiceAddr* const servAddr = down7->servAddr;
    SVCXPRT*           xprt = svcfd_create(down7->sock, 0, MAX_RPC_BUF_NEEDED);
    int                status;
    char               buf[256];

    if (xprt == NULL) {
        (void)sa_snprint(servAddr, buf, sizeof(buf));
        LOG_ADD1("Couldn't create RPC service for receiving data-products from "
                "upstream LDM-7 at \"%s\"", buf);
        status = LDM7_RPC;
    }
    else {
        if (!svc_register(xprt, LDMPROG, SEVEN, ldmprog_7, 0)) {
            (void)sa_snprint(servAddr, buf, sizeof(buf));
            LOG_ADD1("Couldn't register RPC service for receiving "
                    "data-products from upstream LDM-7 at \"%s\"", buf);
            status = LDM7_RPC;
        }
        else {
            status = run_down7_svc(down7, xprt); // indefinite execution
        }

        /*
         * The following closes the server socket in `xprt`, which is also the
         * downstream LDM-7's client socket.
         */
        svc_destroy(xprt);
    } // "xprt" allocated

    taskExit(down7, status);

    return (void*)status;
}

/**
 * Cleanly stops the task of a downstream LDM-7 that's receiving unicast
 * data-products from an upstream LDM-7 by shutting down the socket to the
 * remote LDM-7 for reading.
 *
 * @param[in] down7  Pointer to the downstream LDM-7 whose receiving task is to
 *                   be stopped.
 */
static void
stopUnicastProductReceiver(
    Down7* const down7)
{
    (void)shutdown(down7->sock, SHUT_RD);
}

/**
 * Receives data-products via multicast. Blocks until the multicast LDM receiver
 * is stopped. Called by `pthread_create()`.
 *
 * @param[in] arg            Pointer to the downstream LDM-7.
 * @retval    LDM7_SHUTDOWN  The multicast LDM receiver was stopped.
 * @retval    LDM7_SYSTEM    System error. \c log_start() called.
 */
static void*
startMulticastProductReceiver(
    void* const arg)
{
    Down7* const down7 = (Down7*)arg;
    int          status;
    Mlr* const   mlr = mlr_new(pq, down7->mcastInfo, down7);

    if (mlr == NULL) {
        LOG_ADD0("Couldn't create a new multicast LDM receiver");
        status = LDM7_SYSTEM;
    }
    else {
        down7->mlr = mlr;
        status = mlr_start(down7->mlr);
    }

    taskExit(down7, status);

    return (void*)status;
}

/**
 * Terminates all tasks of a downstream LDM-7. Undefined behavior results if
 * called from a signal handler.
 *
 * @param[in] down7  Pointer to the downstream LDM-7.
 * @return           The status of the first task to exit.
 */
static int
terminateTasks(
    Down7* const down7)
{
    mlr_stop(down7->mlr);
    stopRequester(down7);
    stopUnicastProductReceiver(down7);

    (void)pthread_join(down7->mcastThread, NULL);
    (void)pthread_join(down7->requestThread, NULL);
    (void)pthread_join(down7->receiveThread, NULL);

    return down7->exitStatus;
}

/**
 * Starts the concurrent tasks of a downstream LDM-7.
 *
 * @param[in] down7        Pointer to the downstream LDM-7.
 * @retval    0            Success.
 * @retval    LDM7_SYSTEM  Error. `log_start()` called. All tasks were
 *                         terminated.
 */
static int
startTasks(
    Down7* const    down7)
{
    int       status;

    if ((status = pthread_create(&down7->receiveThread, NULL,
            startUnicastProductReceiver, down7)) != 0) {
        LOG_ERRNUM0(status, "Couldn't start task that receives data-products "
                "that were missed by the multicast LDM receiver task");
        status = LDM7_SYSTEM;
    }
    else if ((status = pthread_create(&down7->requestThread, NULL,
            startRequester, down7)) != 0) {
        LOG_ERRNUM0(status, "Couldn't start task that requests data-products "
                "that were missed by the multicast LDM receiver task");
        status = LDM7_SYSTEM;
    }
    else if ((status = pthread_create(&down7->mcastThread, NULL,
            startMulticastProductReceiver, down7)) != 0) {
        LOG_ERRNUM0(status, "Couldn't start multicast LDM receiver task");
        status = LDM7_SYSTEM;
    }
    else {
        status = 0;
    }

    if (status)
        terminateTasks(down7);

    return status;
}

/**
 * Waits for all tasks of a downstream LDM-7 to complete. Blocks until one task
 * terminates or the downstream LDM-7 is shut down, then terminates all remaining
 * tasks and returns.
 *
 * @param[in] down7          Pointer to the downstream LDM-7.
 * @retval    LDM7_SHUTDOWN  The downstream LDM-7 was shut down.
 * @return                   The status of the first task to exit.
 */
static int
waitOnTasks(
    Down7* const down7)
{
    int status;

    lockWait(down7);
    while (!done && !down7->taskExited)
        pthread_cond_wait(&down7->waitCond, &down7->waitMutex);
    unlockWait(down7);

    status = terminateTasks(down7);

    return done ? LDM7_SHUTDOWN : status;
}

/**
 * Receives data for a downstream LDM-7. Blocks until the LDM-7 is shut down or
 * an unrecoverable error occurs.
 *
 * @param[in] down7            Pointer to the downstream LDM-7.
 * @retval    LDM7_SYSTEM      System error. \c log_start() called.
 * @retval    LDM7_SHUTDOWN    The downstream LDM-7 was shut down.
 * @retval    LDM7_RPC         RPC error. \c log_start() called.
 * @retval    LDM7_TIMEDOUT    A timeout occurred. `log_start()` called.
 */
static int
execute(
    Down7* const down7)
{
    int status = startTasks(down7);

    if (status) {
        LOG_ADD0("Couldn't start downstream LDM-7 tasks");
    }
    else {
        status = waitOnTasks(down7);
    }

    return status;
}

/**
 * Subscribes a downstream LDM-7 to a multicast group and receives the data.
 * Blocks until the LDM-7 is shut down or an error occurs.
 *
 * @param[in] down7          Pointer to the downstream LDM-7.
 * @retval    LDM7_SHUTDOWN  LDM-7 was shut down.
 * @retval    LDM7_TIMEDOUT  Timeout occurred. \c log_start() called.
 * @retval    LDM7_RPC       RPC failure (including interrupt). \c
 *                           log_start() called.
 * @retval    LDM7_INVAL     Invalid multicast group name.
 * @retval    LDM7_UNAUTH    Not authorized to receive multicast group.
 * @retval    LDM7_SYSTEM    System error. \c log_start() called.
 */
static int
subscribeAndExecute(
    Down7* const down7)
{
    int                status;
    SubscriptionReply* reply;

    (void)lockClient(down7);
    reply = subscribe_7(&down7->feedtype, down7->clnt);

    if (reply == NULL) {
	LOG_START1("subscribe_7() failure: %s", clnt_errmsg(down7->clnt));
	status = clntStatusToLdm7Status(clnt_stat(down7->clnt));
        unlockClient(down7);
    }
    else {
        unlockClient(down7);

        if (reply->status == 0) {
            /*
             * NB: The simple assignment to "down7->mcastInfo" works because
             * the right-hand-side won't be freed until after "execute()".
             * Otherwise, something like "mcastInfo_clone()" would have to be
             * created and used.
             */
            down7->mcastInfo = &reply->SubscriptionReply_u.mgi;
            status = execute(down7);
        }
        xdr_free(xdr_SubscriptionReply, (char*)reply);
    } /* `reply` allocated */

    return status;
}

/**
 * Creates the client-side handle and executes the downstream LDM-7.
 *
 * @param[in] down7          Pointer to the downstream LDM-7 to be executed.
 * @retval    LDM7_SHUTDOWN  LDM-7 was shut down.
 * @retval    LDM7_INVAL     Invalid port number or host identifier. \c
 *                           log_start() called.
 * @retval    LDM7_REFUSED   Remote LDM-7 refused connection. \c log_start()
 *                           called.
 * @retval    LDM7_TIMEDOUT  Connection attempt timed-out. \c log_start() called.
 * @retval    LDM7_SYSTEM    System error. \c log_start() called.
 * @retval    LDM7_RPC       RPC failure (including interrupt). \c log_start()
 *                           called.
 * @retval    LDM7_INVAL     Invalid multicast group name. `log_start()` called.
 * @retval    LDM7_UNAUTH    Not authorized to receive multicast group.
 *                           `log_start()` called.
 */
static int
createClientAndExecute(
    Down7* const down7)
{
    int status = newClient(&down7->clnt, down7->servAddr, &down7->sock);

    if (status == 0) {
        status = subscribeAndExecute(down7);

        clnt_destroy(down7->clnt); // won't close externally-created socket
        (void)close(down7->sock); // likely closed by server-side receiver
    } // "down7->clnt" and "down7->sock" allocated

    return status;
}

/**
 * Executes a downstream LDM-7. Blocks until the LDM-7 is shut down or an
 * error occurs.
 *
 * @param[in] down7          Pointer to the downstream LDM-7 to be executed.
 * @retval    LDM7_SHUTDOWN  LDM-7 was shut down.
 * @retval    LDM7_INVAL     Invalid port number or host identifier. \c
 *                           log_start() called.
 * @retval    LDM7_REFUSED   Remote LDM-7 refused connection. \c log_start()
 *                           called.
 * @retval    LDM7_TIMEDOUT  Connection attempt timed-out. \c log_start() called.
 * @retval    LDM7_SYSTEM    System error. \c log_start() called.
 * @retval    LDM7_RPC       RPC failure (including interrupt). \c log_start()
 *                           called.
 * @retval    LDM7_INVAL     Invalid multicast group name. `log_start()` called.
 * @retval    LDM7_UNAUTH    Not authorized to receive multicast group.
 *                           `log_start()` called.
 */
static int
runDown7Once(
    Down7* const down7)
{
    int status;

    down7->msm = msm_open(down7->servAddr, down7->feedtype);

    if (down7->msm == NULL) {
        LOG_ADD0("Couldn't open multicast session memory");
        status = LDM7_SYSTEM;
    }
    else {
        down7->prevLastMcastSet = msm_getLastMcastProd(down7->msm,
                down7->prevLastMcast);
        status = createClientAndExecute(down7);

        if (!msm_close(down7->msm)) {
            LOG_ADD0("Couldn't close multicast session memory");
            status = LDM7_SYSTEM;
        }
    } // `down7->msm` open

    return status;
}

/**
 * Waits a short time. Blocks until the time period is up or the downstream
 * LDM-7 is shut down. Returns immediately if the LDM-7 is shut down.
 *
 * @param[in] down7          Pointer to the downstream LDM-7.
 * @retval    0              Timeout occurred.
 * @retval    LDM7_SHUTDOWN  The downstream LDM-7 was shut down.
 */
static int
nap(
    Down7* const down7)
{
    struct timespec absTime;

    absTime.tv_sec = time(NULL) + 60; // a time in the future
    absTime.tv_nsec = 0;

    lockWait(down7);
    while (!done) {
        if (pthread_cond_timedwait(&down7->waitCond, &down7->waitMutex,
                &absTime) == ETIMEDOUT)
            break;
    }
    unlockWait(down7);

    return done ? LDM7_SHUTDOWN : 0;
}

/**
 * Inserts a data-product into the product-queue and then unlocks the
 * product-queue. Logs directly.
 *
 * @param[in] pq          Pointer to the product-queue. Must be locked.
 * @param[in] prod        Pointer to the data-product to be inserted into the product-
 *                        queue.
 * @retval    0           Success. `uinfo()` called.
 * @retval    EINVAL      Invalid argument. `uerror()` called.
 * @retval    PQUEUE_DUP  Product already exists in the queue. `uinfo()` called.
 * @retval    PQUEUE_BIG  Product is too large to insert in the queue. `uwarn()`
 *                        called.
 */
static int
insertAndUnlock(
    pqueue* const restrict  pq,
    product* const restrict prod)
{
    int status = pq_insert(pq, prod);

    (void)pq_unlock(pq);

    if (status) {
        if (status == EINVAL) {
            uerror("Invalid argument");
        }
        else {
            char buf[256];

            (void)s_prod_info(buf, sizeof(buf), &prod->info, ulogIsDebug());

            if (0 == status) {
                uinfo("%s", buf);
            }
            else if (status == PQUEUE_DUP) {
                uinfo("Duplicate data-product: %s", buf);
            }
            else {
                uwarn("Product too big for queue: %s", buf);
            }
        }
    }

    return status;
}

/**
 * Processes a data-product from a remote LDM-7 by attempting to add the
 * data-product to the product-queue. The data-product should have been
 * previously requested from the remote LDM-7.
 *
 * @param[in] pq           Pointer to the product-queue.
 * @param[in] prod         Pointer to the data-product.
 * @retval    0            Success.
 * @retval    LDM7_SYSTEM  System error. `log_start()` called.
 */
static int
deliver_product(
    pqueue* const restrict  pq,
    product* const restrict prod)
{
    int status = pq_lock(pq);

    if (status) {
        LOG_ERRNUM0(status, "Couldn't lock product-queue");
        status = LDM7_SYSTEM;
    }
    else {
        status = insertAndUnlock(pq, prod);

        if (status) {
            status = (status == EINVAL)
                    ? LDM7_SYSTEM
                    : 0; // either too big or duplicate data-product
        }
    } // product-queue is locked

    return status;
}

/**
 * Ensures that the thread-specific data-key for the downstream LDM pointer
 * exists.
 */
static void
createDown7Key(void)
{
    int status = pthread_key_create(&down7Key, NULL);

    if (status) {
        LOG_ERRNUM0(status,
                "Couldn't create thread-specific data-key for downstream LDM-7");
    }
}

/**
 * Handles failure of delivery of a data-product by logging the fact and
 * destroying the server-side RPC transport.
 *
 * @param[in] msg    The log message.
 * @param[in] info   The product metadata.
 * @param[in] rqstp  The service request.
 */
static void
deliveryFailure(
    const char* restrict            msg,
    const prod_info* const restrict info,
    struct svc_req* const restrict  rqstp)
{
    char buf[256];

    LOG_ADD2("%s: %s", msg, s_prod_info(buf, sizeof(buf), info, ulogIsDebug()));
    log_log(LOG_ERR);
    (void)svcerr_systemerr(rqstp->rq_xprt);
    svc_destroy(rqstp->rq_xprt);
}

/**
 * Returns a new downstream LDM-7.
 *
 * @param[in] servAddr    Pointer to the address of the server from which to
 *                        obtain multicast information, backlog products, and
 *                        products missed by the VCMTP layer. Caller may free
 *                        upon return.
 * @param[in] feedtype    Feedtype of multicast group to receive.
 * @param[in] pqPathname  Pathname of the product-queue.
 * @retval    NULL        Failure. \c log_start() called.
 * @return                Pointer to the new downstream LDM-7.
 */
static Down7*
down7_new(
    const ServiceAddr* const restrict servAddr,
    const feedtypet                   feedtype,
    const char* const restrict        pqPathname)
{
    Down7* const down7 = LOG_MALLOC(sizeof(Down7), "downstream LDM-7");
    int          status;

    if (down7 == NULL)
        goto return_NULL;

    if ((down7->servAddr = sa_clone(servAddr)) == NULL) {
        char buf[256];

        (void)sa_snprint(servAddr, buf, sizeof(buf));
        LOG_ADD1("Couldn't clone server address \"%s\"", buf);
        goto free_down7;
    }

    if ((status = pthread_cond_init(&down7->waitCond, NULL)) != 0) {
        LOG_ERRNUM0(status,
                "Couldn't initialize condition-variable for waiting");
        goto free_servAddr;
    }

    if ((status = pthread_mutex_init(&down7->waitMutex, NULL)) != 0) {
        LOG_ERRNUM0(status, "Couldn't initialize mutex for waiting");
        goto free_waitCond;
    }

    if ((status = pthread_mutex_init(&down7->clntMutex, NULL)) != 0) {
        LOG_ERRNUM0(status, "Couldn't initialize mutex for client-side handle");
        goto free_waitMutex;
    }

    if ((status = pq_open(pqPathname, 0, &down7->pq))) {
        LOG_ADD1("Couldn't open product-queue \"%s\"", pqPathname);
        goto free_clntMutex;
    }

    if ((status = pthread_once(&down7KeyControl, createDown7Key)) != 0) {
        goto close_pq;
    }

    down7->feedtype = feedtype;
    down7->pq = pq;
    down7->clnt = NULL;
    down7->sock = -1;
    down7->mcastInfo = NULL;
    down7->taskExited = 0;
    down7->exitStatus = -1;
    down7->mlr = NULL;
    down7->mcastWorking = false;
    (void)memset(down7->firstMcast, 0, sizeof(signaturet));
    (void)memset(down7->prevLastMcast, 0, sizeof(signaturet));

    return down7;

close_pq:
    (void)pq_close(down7->pq);
free_clntMutex:
    pthread_mutex_destroy(&down7->clntMutex);
free_waitMutex:
    pthread_mutex_destroy(&down7->waitMutex);
free_waitCond:
    pthread_cond_destroy(&down7->waitCond);
free_servAddr:
    sa_free(down7->servAddr);
free_down7:
    free(down7);
return_NULL:
    return NULL;
}

/**
 * Frees a downstream LDM-7.
 *
 * @param[in] down7  Pointer to the downstream LDM-7 object to be freed or NULL.
 */
static void
down7_free(
    Down7* const down7)
{
    if (down7) {
        (void)pthread_mutex_destroy(&down7->clntMutex);
        (void)pthread_mutex_destroy(&down7->waitMutex);
        (void)pthread_cond_destroy(&down7->waitCond);
        (void)pq_close(down7->pq);
        sa_free(down7->servAddr);
        free(down7);
    }
}

/**
 * Executes a downstream LDM-7. Doesn't return until process termination is
 * externally requested or an unrecoverable error occurs.
 *
 * @param[in] arg            Pointer to downstream LDM-7 object.
 * @retval    LDM7_SHUTDOWN  LDM-7 was shut down.
 * @retval    LDM7_SYSTEM    System error. `log_log()` called.
 */
static Ldm7Status
down7_execute(
        void* const arg)
{
    int          status;
    Down7* const down7 = arg;

    do {
        status = runDown7Once(down7);

        if (LDM7_SYSTEM == status)
            break;

        log_log(LOG_NOTICE); // might log nothing
        status = nap(down7); // returns immediately if shut down
    } while (LDM7_SHUTDOWN != status);

    return status;
}

/******************************************************************************
 * Public API:
 ******************************************************************************/

/**
 * Runs a downstream LDM-7. Blocks until the process is externally terminated or
 * a non-recoverable error occurs.
 *
 * @param[in] servAddr       Pointer to the address of the server from which to
 *                           obtain multicast information, backlog products, and
 *                           products missed by the VCMTP layer. Caller may free
 *                           upon return.
 * @param[in] feedtype       Feedtype of multicast group to receive.
 * @param[in] pqPathname     Pathname of the product-queue.
 * @retval    LDM7_SHUTDOWN  Process termination requested.
 * @retval    LDM7_SYSTEM    System error occurred. `log_start()` called.
 */
Ldm7Status
down7_run(
    const ServiceAddr* const restrict servAddr,
    const feedtypet                   feedtype,
    const char* const restrict        pqPathname)
{
    /*
     * A child process is not forked because it is assumed that the current
     * process is a child process of the top-level server.
     */
    unotice("Down-7 starting up: servAddr=%s, feedtype=%s, pq=%s",
            sa_format(servAddr), s_feedtypet(feedtype), pqPathname);

    int    status;
    Down7* down7 = down7_new(servAddr, feedtype, pqPathname);

    if (NULL == down7) {
        status = LDM7_SYSTEM;
    }
    else {
        status = down7_execute(down7);
        down7_free(down7);
    } // `down7` allocated

    return status;
}

/**
 * Queues a data-product that that was missed by the multicast LDM receiver.
 * This function is called by the multicast LDM receiver; therefore, it must
 * return immediately so that the multicast LDM receiver can continue.
 *
 * @param[in] down7   Pointer to the downstream LDM-7.
 * @param[in] iProd   Index of the missed VCMTP product.
 */
void
down7_missedProduct(
    Down7* const         down7,
    const VcmtpProdIndex iProd)
{
    /*
     * Cancellation of the operation of the missed-but-not-requested queue is
     * ignored because nothing can be done about it at this point and no harm
     * should result.
     */
    udebug("Down-7 missed product: %lu", (unsigned long)iProd);
    (void)msm_addMissedFile(down7->msm, iProd);
}

/**
 * Tracks the last data-product to be successfully received by the multicast
 * LDM receiver associated with a downstream LDM-7. This function is called by
 * the multicast LDM receiver; therefore, it must return immediately so that the
 * multicast LDM receiver can continue.
 *
 * The first time this function is called for a given downstream LDM-7, it
 * starts a detached thread that requests the backlog of data-products that
 * were missed due to the passage of time from the end of the previous session
 * to the reception of the first multicast data-product.
 *
 * @param[in] down7  Pointer to the downstream LDM-7.
 * @param[in] last   Pointer to the metadata of the last data-product to be
 *                   successfully received by the associated multicast
 *                   LDM receiver. Caller may free when it's no longer needed.
 */
void
down7_lastReceived(
    Down7* const restrict           down7,
    const prod_info* const restrict last)
{
    msm_setLastMcastProd(down7->msm, last->signature);

    if (!down7->mcastWorking) {
        down7->mcastWorking = true;
        (void)memcpy(down7->firstMcast, last->signature, sizeof(signaturet));

        pthread_t backlogThread;
        int status = pthread_create(&backlogThread, NULL, requestSessionBacklog,
                down7);

        if (status) {
            LOG_ERRNUM0(status, "Couldn't create backlog-requesting thread");
            log_log(LOG_ERR);
        }
        else {
            (void)pthread_detach(backlogThread);
        }
    }
}

/**
 * Processes a missed data-product from a remote LDM-7 by attempting to add the
 * data-product to the product-queue. The data-product should have been
 * previously requested from the remote LDM-7 because it was missed by the
 * multicast LDM receiver. Destroys the server-side RPC transport if the
 * data-product isn't expected or can't be inserted into the product-queue. Does
 * not reply. Called by the RPC dispatcher `ldmprog_7()`.
 *
 * @param[in] missedProd  Pointer to the missed data-product.
 * @param[in] rqstp       Pointer to the RPC service-request.
 * @retval    NULL        Always.
 */
void*
deliver_missed_product_7_svc(
    MissedProduct* const restrict  missedProd,
    struct svc_req* const restrict rqstp)
{
    prod_info* const info = &missedProd->prod.info;
    Down7*           down7 = pthread_getspecific(down7Key);
    VcmtpProdIndex   iProd;

    if (!msm_peekRequestedFileNoWait(down7->msm, &iProd) ||
            iProd != missedProd->iProd) {
        deliveryFailure("Unexpected product received", info, rqstp);
    }
    else {
        // The queue can't be empty
        (void)msm_removeRequestedFileNoWait(down7->msm, &iProd);

        if (deliver_product(down7->pq, &missedProd->prod) != 0)
            deliveryFailure("Couldn't insert missed product", info, rqstp);
    }

    return NULL; // causes RPC dispatcher to not reply
}

/**
 * Accepts notification from the upstream LDM-7 that a requested data-product
 * doesn't exist. Called by the RPC dispatch routine `ldmprog_7()`.
 *
 * @param[in] iProd   Index of the data-product.
 * @param[in] rqstp   Pointer to the RPC service-request.
 */
void*
no_such_product_7_svc(
    VcmtpProdIndex* const iProd,
    struct svc_req* const rqstp)
{
    uwarn("Upstream LDM-7 says requested product doesn't exist: %lu",
            (unsigned long)iProd);

    return NULL ; /* don't reply */
}

/**
 * Processes a backlog data-product from a remote LDM-7 by attempting to add the
 * data-product to the product-queue. The data-product should have been
 * previously requested from the remote LDM-7 because it was missed during the
 * previous session. Destroys the server-side RPC transport if the data-product
 * can't be inserted into the product-queue. Does not reply. Called by the RPC
 * dispatcher `ldmprog_7()`.
 *
 * @param[in] prod        Pointer to the backlog data-product.
 * @param[in] rqstp       Pointer to the RPC service-request.
 * @retval    NULL        Always.
 */
void*
deliver_backlog_product_7_svc(
    product* const restrict        prod,
    struct svc_req* const restrict rqstp)
{
    Down7* down7 = pthread_getspecific(down7Key);

    if (deliver_product(down7->pq, prod))
        deliveryFailure("Couldn't insert backlog product", &prod->info, rqstp);

    return NULL; // causes RPC dispatcher to not reply
}

/**
 * Accepts notification that the downstream LDM-7 associated with the current
 * thread has received all backlog data-products from its upstream LDM-7. From
 * now on, the current process may be terminated for a time period that is less
 * than the minimum residence time of the upstream LDM-7's product-queue without
 * loss of data. Called by the RPC dispatcher `ldmprog_7()`.
 *
 * @param[in] rqstp  Pointer to the RPC server-request.
 */
void*
end_backlog_7_svc(
    void* restrict                 noArg,
    struct svc_req* const restrict rqstp)
{
    char   saStr[512];
    Down7* down7 = pthread_getspecific(down7Key);

    unotice("All backlog data-products received: feedtype=%s, server=%s",
            s_feedtypet(down7->feedtype),
            sa_snprint(down7->servAddr, saStr, sizeof(saStr)));

    return NULL; // causes RPC dispatcher to not reply
}
