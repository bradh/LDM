/**
 * Copyright (C) 2013 University Corporation for Atmospheric Research.
 * All Rights Reserved.
 * <p>
 * See file COPYRIGHT in the top-level source directory for copying and
 * redistribution conditions.
 */

/*LINTLIBRARY*/

#include <config.h>

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>             /* UINT_MAX */
#include <netdb.h>
#include <rpc/rpc.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <regex.h>
#include <unistd.h>

#include "abbr.h"
#include "ldm_config_file.h"
#include "autoshift.h"
#include "down6.h"
#include "error.h"
#include "feedTime.h"
#include "remote.h"
#include "globals.h"            /* global "pq"; defined in ldmd.c */
#include "inetutil.h"
#include "ldm5_clnt.h"
#include "ldmfork.h"
#include "ldmprint.h"
#include "log.h"
#include "pattern.h"
#include "peer_info.h"
#include "pq.h"
#include "priv.h"
#include "prod_class.h"
#include "prod_info.h"
#include "registry.h"
#include "RegularExpressions.h"
#include "requester6.h"
#include "rpcutil.h"
#include "savedInfo.h"
#include "timestamp.h"
#include "ulog.h"
#include "log.h"
#include "UpFilter.h"
#include "md5.h"


/******************************************************************************
 * String/Unsigned Structure Module
 ******************************************************************************/

/**
 * Immutable string and unsigned integer structure:
 */
typedef struct {
    const char* string;
    unsigned    integer;
} SUS;


/**
 * Returns a new string/unsigned object.
 *
 * @param string        [in] The string. Client may free upon return.
 * @param integer       [in] Integer value.
 * @retval NULL         Failure. log_add() called.
 * @return              Pointer to the new String/Unsigned object.
 */
static const SUS*
sus_new(
        const char*     string,
        const unsigned  integer)
{
    char*   str = strdup(string);

    if (str == NULL) {
        LOG_SERROR1("Couldn't duplicate string \"%s\"", string);
    }
    else {
        SUS*    sus = malloc(sizeof(SUS));

        if (sus == NULL) {
            LOG_SERROR0("Couldn't allocate new string/unsigned object");
        }
        else {
            sus->string = str;
            sus->integer = integer;

            return sus;
        }

        free(str);
    } /* "str" allocated */

    return NULL;
}


/**
 * Clones a string/unsigned object.
 *
 * @param sus           [in] The object to be cloned.
 * @retval NULL         Failure. log_add() called.
 * @return              Pointer to the clone.
 */
static const SUS*
sus_clone(
        const SUS* const sus)
{
    const SUS*   clone = sus_new(sus->string, sus->integer);

    if (clone == NULL)
        LOG_ADD0("Couldn't clone string/unsigned object");

    return clone;
}


/**
 * Frees a string/unsigned object.
 *
 * @param sus           [in/out] The object to be freed.
 */
static void
sus_free(
        const SUS* const sus)
{
    free((void*)sus->string); /* cast away "const" */
    free((void*)sus); /* cast away "const" */
}


/**
 * Indicates if two string/unsigned objects are equal.
 *
 * @param sus1          [in] Pointer to the first string/unsigned object.
 * @param sus2          [in] Pointer to the second string/unsigned object.
 * @retval 0            The objects are not equal.
 * @retval 1            The objects are equal.
 */
static int
sus_equal(
        const SUS* const sus1,
        const SUS* const sus2)
{
    return (strcmp(sus1->string, sus2->string) == 0)
            && (sus1->integer == sus2->integer);
}


/**
 * Returns the string of a string/unsigned object.
 *
 * @param sus       [in] The string/unsigned object.
 * @return          Pointer to the string.
 */
static const char*
sus_getString(
        const SUS* const    sus)
{
    return sus->string;
}


/**
 * Returns the integer of a string/unsigned object.
 *
 * @param sus       [in] The string/unsigned object.
 * @return          The integer.
 */
static unsigned
sus_getUnsigned(
        const SUS* const    sus)
{
    return sus->integer;
}


/******************************************************************************
 * Server Information Module
 *****************************************************************************/

typedef SUS ServerInfo;


/**
 * Returns a new server-information object.
 *
 * @param hostId        [in] Host identifier. Client may free upon return.
 * @param port          [in] Port number.
 * @retval NULL         Failure. log_add() called.
 * @return              Pointer to the new server-information object.
 */
static const ServerInfo*
serverInfo_new(
        const char* const   hostId,
        const unsigned      port)
{
    return sus_new(hostId, port);
}


/**
 * Clones server information.
 *
 * @param server    [in] The server information to be cloned. Client
 *                  may free upon return.
 * @retval NULL     Failure. log_add() called.
 * @return          Pointer to the clone.
 */
static const ServerInfo*
serverInfo_clone(
        const ServerInfo* const server)
{
    return sus_clone(server);
}


/**
 * Frees a server-information object.
 *
 * @param server        [in/out] Pointer to the server-information object.
 */
static void
serverInfo_free(
        const ServerInfo* const   server)
{
    sus_free((ServerInfo*)server); /* cast away "const" */
}


/**
 * Indicates if two server-information objects are equal.
 *
 * @param server1       [in] Pointer to the first server-information object.
 * @param server2       [in] Pointer to the second server-information object.
 * @retval 0            The objects are not equal.
 * @retval 1            The objects are equal.
 */
static int
serverInfo_equal(
        const ServerInfo* const server1,
        const ServerInfo* const server2)
{
    return sus_equal(server1, server2);
}


/**
 * Returns the host identifier of a server-information object.
 *
 * @param server        [in] The server-information object.
 * @return              The host identifier.
 */
static const char*
serverInfo_getHostId(
        const ServerInfo* const server)
{
    return sus_getString(server);
}


/**
 * Returns the port number of a server-information object.
 *
 * @param server        [in] The server-information object.
 * @return              The port number.
 */
static unsigned
serverInfo_getPort(
        const ServerInfo* const server)
{
    return sus_getUnsigned(server);
}

/******************************************************************************
 * Product and Product-Queue Module
 ******************************************************************************/

/*
 * Ensures that the "from" time isn't too long ago.
 *
 * Arguments:
 *      from            Pointer to "from" time to be vetted.
 *      backoff         Maximum "backoff" time interval in seconds.
 */
static void
vetFromTime(
    timestampt* const   from,
    const int           backoff)
{
    timestampt                  defaultFrom;

    (void)set_timestamp(&defaultFrom);
    defaultFrom.tv_sec -= backoff;

    if (tvCmp(defaultFrom, *from, >))
        *from = defaultFrom;
}


/*
 * Extracts the metadata of a data-product.  Called by pq_sequence() in
 * restoreFromQueue().
 *
 * Arguments:
 *      infop           Pointer to data-product metadata.
 *      datap           Pointer to data-product.
 *      xprod           Pointer to XDR-encoded data-product.
 *      len             Length of XDR-encoded data-product.
 *      arg             Pointer completely-allocated prod_info structure for
 *                      copied data-product metadata.
 * Returns:
 *      PQ_END          Always.
 */
/*ARGUSED*/
static int
getInfo(
    const prod_info* const      infop,
    const void* const           datap,
    void* const                 xprod,
    const size_t                len,
    void* const                 arg)
{
    (void)pi_copy((prod_info*)arg, infop);

    return PQ_END;                      /* use first matching data-product */
}


/*
 * Returns the product-information of the last data-product in the
 * product-queue that matches the product-class.
 *
 * Calls exitIfDone() at potential termination points.
 *
 * Arguments:
 *      pq              Pointer to the product-queue structure.
 *      prodClass       Pointer to the request product-class.
 *      info            Pointer to product-information created by pi_new() or
 *                      pi_clone().
 * Returns:
 *      -1              Error.  log_*() called.
 *      0               Success.  "info" is set.
 *      1               No matching product-information exists.
 */
static int
getQueueProdInfo(
    pqueue* const               pq,
    const prod_class_t* const     prodClass,
    prod_info* const            info)
{
    int     status = -1;                /* error */

    assert(pq != NULL);
    assert(prodClass != NULL);
    assert(info != NULL);

    for (pq_cset(pq, &TS_ENDT);
        !(status = pq_sequence(pq, TV_LT, prodClass, getInfo, info));)
    {
        timestampt  cursor;

        (void)exitIfDone(0);

        pq_ctimestamp(pq, &cursor);

        if (d_diff_timestamp(&prodClass->from, &cursor) > interval)
            break;                      /* gone too far back */
    }

    if (status && PQ_END != status) {
        log_start("getQueueProdInfo(): %s", pq_strerror(pq, status));
    }
    else {
        status =
            0 == status || tvIsNone(info->arrival)
                ? 1
                : 0;
    }

    return status;
}


#if 0
/*
 * Initialize the "savedInfo" module with the product-information of the last
 * data-product in the product-queue that matches the product-class.  If no
 * such data-product exists in the product-queue, then a subsequent
 * savedInfo_get() call will return NULL.
 *
 * Calls exitIfDone() at potential termination points.
 *
 * Arguments:
 *      pq              Pointer to the product-queue structure.
 *      prodClass       Pointer to the product-class structure against which the
 *                      data-products will be matched.
 * Returns:
 *      NULL    Success.  savedInfo_get() will return the product-information
 *              of the last, matching data-product in the product-queue or NULL
 *              if no such data-product was found.
 *      else    Failure.
 */
static ErrorObj*
restoreFromQueue(
    pqueue* const               pq,
    const prod_class_t* const     prodClass)
{
    ErrorObj*           errObj = NULL;  /* success */
    prod_info* const    info = pi_new();

    if (NULL == info) {
        errObj = ERR_NEW1(errno, NULL,
            "Couldn't allocate new product-information structure: %s",
            strerror(errno));
    }
    else {
        int     errCode;

        for (pq_cset(pq, &TS_ENDT);
            !(errCode = pq_sequence(pq, TV_LT, prodClass, getInfo, info));)
        {
            timestampt  cursor;

            (void)exitIfDone(0);

            pq_ctimestamp(pq, &cursor);

            if (d_diff_timestamp(&prodClass->from, &cursor) > interval)
                break;
        }

        if (errCode && PQ_END != errCode) {
            errObj =
                ERR_NEW1(errCode, NULL,
                    "Couldn't get last matching data-product in product-queue: "
                        "%s",
                    pq_strerror(pq, errCode));
        }
        else {
            if (0 == errCode || tvIsNone(info->arrival)) {
                err_log_and_free(
                    ERR_NEW(errCode, NULL,
                        "No matching data-product in product-queue"),
                    ERR_INFO);
                (void)savedInfo_set(NULL);
            }
            else {
                if (errCode = savedInfo_set(info))
                    errObj = ERR_NEW1(errCode, NULL,
                        "Couldn't save product-information: %s",
                        strerror(errCode));
            }
        }

        pi_free(info);
    }                                   /* "info" allocated */

    return errObj;
}
#endif


static char     statePath[_POSIX_PATH_MAX];


/*
 * Returns the product-information of the last, successfully-received
 * data-product for a given data-request from the previous session.
 *
 * Arguments:
 *      upId            Identifier of upstream LDM host.
 *      port            Port number of upstream LDM server.
 *      prodClass       Requested product-class.
 *      info            Pointer to product-information structure created by
 *                      pi_new() or pi_clone();
 * Returns:
 *      -1              Error.  log_*() called.
 *      0               Success.  "*info" is set.
 *      1               No product-information available for the given request.
 */
static int
getPreviousProdInfo(
    const char* const           upId,
    const unsigned              port,
    const prod_class_t* const   prodClass,
    prod_info* const            info)

{
    int         status = -1;            /* failure */
    MD5_CTX*    context = new_MD5_CTX();

    if (context == NULL) {
        log_errno();
        log_add("getPreviousProdInfo(): Couldn't allocate MD5 structure");
    }
    else {
        /*
         * Create a filename based on a hash of the request.
         */
        unsigned char           hash[16];
        const prod_spec*        prodSpec = prodClass->psa.psa_val;
        FILE*                   file;

        MD5Update(context, (unsigned char*)upId, strlen(upId));
        MD5Update(context, (unsigned char*)&port, sizeof(port));

        for (; prodSpec < prodClass->psa.psa_val + prodClass->psa.psa_len;
                prodSpec++) {
            const feedtypet     feedtype = prodSpec->feedtype;

            if (feedtype != NONE) {
                const char* const       pattern = prodSpec->pattern;

                MD5Update(context, (unsigned char*)&feedtype,
                    sizeof(feedtypet));

                if (pattern != NULL) {
                    MD5Update(context, (unsigned char*)pattern,
                        strlen(pattern));
                }
            }
        }

        MD5Final(hash, context);
        (void)snprintf(statePath, sizeof(statePath)-1, ".%s.info",
            s_signaturet(NULL, 0, hash));

        file = fopen(statePath, "r");

        if (file == NULL) {
            if (errno == ENOENT) {
                unotice("Previous product-information file \"%s\" "
                    "doesn't exist", statePath);
                status = 1;
            }
            else {
                log_errno();
                log_add("getPreviousProdInfo(): Couldn't open \"%s\"",
                    statePath);
            }
        }
        else {
            /*
             * The file is open.  Read in the information on the last,
             * successfully-received product.
             */
            int         c;

            /*
             * Skip any comments.
             */
            while ((c = fgetc(file)) == '#')
                (void)fscanf(file, "%*[^\n]\n");

            if (ferror(file)) {
                log_errno();
                log_add("getPreviousProdInfo(): Couldn't skip comments in "
                    "\"%s\"", statePath);
            }
            else {
                (void)ungetc(c, file);

                if (pi_scan(info, file) < 0) {
                    log_add("getPreviousProdInfo(): "
                        "Couldn't scan product-information in \"%s\"",
                        statePath);
                }
                else {
                    status = 0;
                }
            }                           /* comments skipped */

            (void)fclose(file);
        }                               /* "file" open */

        free_MD5_CTX(context);
    }                                   /* "context" allocated */

    return status;
}


/*
 * Initializes the "savedInfo" module.
 *
 * Arguments:
 *      upId            Identifier of upstream LDM host.
 *      port            Port number of upstream LDM server.
 *      pqPath          Pathname of the product-queue.
 *      prodClass       Requested product-class.
 * Returns:
 *      -1              Error.  "log_*()" called.
 *      0               Success.
 */
static int
initSavedInfo(
    const char* const           upId,
    const unsigned              port,
    const char* const           pqPath,
    const prod_class_t* const   prodClass)
{
    int         status;
    prod_info*  info = pi_new();

    if (info == NULL) {
        log_errno();
        log_add("initSavedInfo(): "
            "Couldn't allocate product-information structure");
    }
    else {
        /*
         * Try getting product-information from the previous session.
         */
        status = getPreviousProdInfo(upId, port, prodClass, info);

        if (status == 1) {
            /*
             * There's no product-information from the previous session.
             * Try getting product-information from the most recent data-
             * product in the product-queue that matches the product-class.
             */
            pqueue*     pq;

            status = pq_open(pqPath, PQ_READONLY, &pq);

            if (status) {
                log_start("initSavedInfo(): Couldn't open product-queue "
                    "\"%s\" for reading: %s", pqPath, pq_strerror(pq, status));

                status = -1;
            }
            else {
                status = getQueueProdInfo(pq, prodClass, info);

                if (status == 1) {
                    pi_free(info);
                    info = NULL;
                    status = 0;
                }                       /* no matching data-product in queue */

                (void)pq_close(pq);
            }                           /* "pq" open */
        }                               /* no previous product-information */

        if (status == 0) {
            if (savedInfo_set(info)) {
                log_errno();
                log_add("initSavedInfo(): Couldn't set product-information");

                status = -1;
            }
        }

        pi_free(info);                  /* NULL safe */
    }                                   /* "info" allocated */

    return status;
}



/******************************************************************************
 * Requester (i.e., downstream LDM) Module
 ******************************************************************************/

struct requester {
        pid_t                   pid;
        const char*             source;
        unsigned                port;
        prod_class_t*           clssp;
        struct requester*       next;
        int                     isPrimary;
};
typedef struct requester Requester;


/**
 * Executes a requester.
 * <p>
 * This function calls exit(): it does not return.
 *
 * @param source        [in] Pointer to the name of the upstream host.
 * @param port          [in] The port on which to connect.
 * @param clssp         [in] Pointer to the class of desired data-products.  The
 *                      caller may free upon return.
 * @param isPrimary     [in] Whether or not the initial transfer-mode should be
 *                      primary (uses HEREIS) or not (uses COMINGSOON/BLKDATA).
 * @param serverCount   [in] The number of servers to which the same request
 *                      will be made.
 */
static void
requester_exec(
    const char*         source,
    const unsigned      port,
    prod_class_t*       clssp,
    int                 isPrimary,
    const unsigned      serverCount)
{
    int                 errCode = 0;    /* success */
    /*
     * Maximum acceptable silence, in seconds, from upstream LDM before
     * taking action.  NOTE: Generally smaller than ldmd.c's
     * "inactive_timeo".  TODO: Make configurable.
     */
    unsigned int        maxSilence = 2 * interval;
    int                 backoffTime =
        toffset == TOFFSET_NONE
            ? max_latency
            : toffset;

    set_abbr_ident(source, NULL);
    str_setremote(source);

    /*
     * Set the "from" time in the data-class to the default value.
     */
    vetFromTime(&clssp->from, backoffTime);

    unotice("Starting Up(%s): %s:%u %s", PACKAGE_VERSION, source, port,
        s_prod_class(NULL, 0, clssp));

    (void)as_setLdmCount(serverCount);

    /*
     * Initialize the "savedInfo" module with the product-information
     * of the last, successfully-received data-product.
     *
     * NB: Potentially lengthy and CPU-intensive.
     */
    if (initSavedInfo(source, port, getQueuePath(), clssp) != 0) {
        log_add("prog_requester(): "
            "Couldn't initialize saved product-information module");
        log_log(LOG_ERR);

        errCode = EXIT_FAILURE;
    }
    else {
        (void)exitIfDone(0);

        /*
         * Open the product-queue for writing.  It will be closed by
         * cleanup() at process termination.
         */
        errCode = pq_open(getQueuePath(), PQ_DEFAULT, &pq);
        if (errCode) {
            err_log_and_free(
                ERR_NEW2(errCode, NULL,
                    "Couldn't open product-queue \"%s\" for writing: %s",
                    getQueuePath(), pq_strerror(pq, errCode)),
                ERR_FAILURE);

            errCode = EXIT_FAILURE;
        }
        else while (!errCode && exitIfDone(0)) {
            int doSleep = 1; /* default */

            /*
             * Ensure that the "from" time in the data-class isn't too
             * long ago.
             */
            vetFromTime(&clssp->from, backoffTime);

            savedInfo_reset();

            /*
             * Try LDM version 6. Potentially lengthy operation.
             */
            ErrorObj* errObj = req6_new(source, port, clssp, maxSilence, getQueuePath(),
                pq, isPrimary);
            exitIfDone(0);

            if (!errObj) {
                /*
                 * NB: If the selection-criteria is modified at this point by
                 * taking into account the most-recently received data-product
                 * by any *other* downstream LDM processes (e.g. by calling
                 * getQueueProdInfo()), then bad things could happen.  For
                 * example, imagine getting NEXRAD Level-II data from both the
                 * Eastern and Western HQs (same data but disjoint sets).
                 */

                if (as_shouldSwitch()) {
                    isPrimary = !isPrimary;
                    doSleep = 0; /* reconnect immediately */

                    LOG_ADD1("Switching data-product transfer-mode to %s",
                                isPrimary ? "primary" : "alternate");
                    log_log(LOG_NOTICE);
                }
            } /* req6_new() success */
            else {
                int feedCode = err_code(errObj);

                if (feedCode != REQ6_BAD_VERSION) {
                    int             logLevel = LOG_ERR; /* default */
                    enum err_level  errLevel = ERR_ERROR; /* default */

                    if (feedCode == REQ6_UNKNOWN_HOST ||
                            feedCode == REQ6_NO_CONNECT) {
                        logLevel = LOG_WARNING;
                        errLevel = ERR_WARNING;
                    }
                    else if (feedCode == REQ6_NOT_ALLOWED) {
                        errObj = ERR_NEW(0, errObj,
                            "Request not allowed. Does it overlap with another?");
                    }
                    else if (feedCode == REQ6_BAD_PATTERN ||
                            feedCode == REQ6_BAD_RECLASS) {
                    }
                    else if (feedCode == REQ6_DISCONNECT) {
                        logLevel = LOG_NOTICE;
                        errLevel = ERR_NOTICE;
                    }
                    else if (feedCode == REQ6_TIMED_OUT) {
                        logLevel = LOG_NOTICE;
                        errLevel = ERR_NOTICE;
                        doSleep = 0; /* reconnect immediately */
                    }
                    else if (feedCode == REQ6_SYSTEM_ERROR) {
                        errObj = ERR_NEW(0, errObj,
                            "Terminating due to system failure");
                        errCode = EXIT_FAILURE; /* terminate */
                    }
                    else {
                        errObj = ERR_NEW1(0, errObj,
                            "Unexpected req6_new() return: %d", feedCode);
                        errCode = EXIT_FAILURE; /* terminate */
                    }

                    log_log(logLevel);
                    err_log(errObj, errLevel);
                } /* don't need to try version 5 of the LDM */
                else {
                    /*
                     * Try LDM version 5.
                     */
                    log_log(LOG_NOTICE);
                    err_log(errObj, ERR_NOTICE);
                    free_remote_clss();

                    if (set_remote_class(clssp)) {
                        log_log(LOG_ERR);
                        errCode = EXIT_FAILURE;
                    }
                    else {
                        int             feedCode;
                        peer_info*      remote = get_remote();

                        feedCode = forn5(FEEDME, source, &remote->clssp,
                            rpctimeo, inactive_timeo, ldmprog_5);
                        exitIfDone(0);

                        udebug("forn5(...) = %d", feedCode);

                        if (feedCode == ECONNABORTED) {
                            unotice("Connection aborted");
                        }
                        else if (feedCode == ECONNRESET) {
                            unotice("Connection closed by upstream LDM");
                        }
                        else if (feedCode == ETIMEDOUT) {
                            unotice("Connection timed-out");
                            doSleep = 0; /* reconnect immediately */
                        }
                        else if (feedCode == ECONNREFUSED) {
                            unotice("Connection refused");
                        }
                        else if (feedCode != 0){
                            uerror("Unexpected forn5() return: %d", feedCode);

                            errCode = EXIT_FAILURE; /* terminate */
                        }
                    } /* remote product-class set */
                } /* LDM-6 protocol not supported */

                log_clear();
                err_free(errObj);
            } /* req6_new() error; "errObj" allocated */

            if (!errCode) {
#if 0
                if (savedInfo_wasSet()) {
                    savedInfo_reset();
                    sleepAmount = 0; /* got data => reconnect immediately */
                }
#endif

                if (doSleep) {
                    /*
                     * Pause before reconnecting.
                     */
                    const unsigned  sleepAmount = 2*interval;

                    uinfo("Sleeping %u seconds before retrying...", sleepAmount);
                    (void)sleep(sleepAmount);
                    exitIfDone(0);

                    /*
                     * Close any connection to the network host database so
                     * that any name resolution starts from scratch. This
                     * allows DNS updates to affect a running downstream LDM.
                     */
                    endhostent();
                }
            }                           /* no error and not done */
        }                               /* connection loop */
    }                                   /* savedInfo module initialized */

    exit(errCode);
}


/**
 * Spawns a requester.
 *
 * @param hostId        [in] Pointer to identifier of upstream host.
 * @param port          [in] The port on which to connect.
 * @param clssp         [in] Pointer to desired class of products.  Caller may free
 *                      upon return.
 * @param isPrimary     [in] Whether or not the data-product exchange-mode should be
 *                      primary (i.e., use HEREIS) or alternate (i.e., use
 *                      COMINGSOON/BLKDATA).
 * @param serverCount   [in] The number of servers to which the same request will be
 *                      made.
 * @retval 0            Success.
 * @retval -1           Failure.  errno is set.  "log_log()" called.
 */
static pid_t
requester_spawn(
    const char*         hostId,
    const unsigned      port,
    prod_class_t*       clssp,
    const int           isPrimary,
    const unsigned      serverCount)
{
        pid_t pid = ldmfork();
        if(pid == -1)
        {
                log_add("Couldn't fork downstream LDM");
                log_log(LOG_ERR);
                return -1;
        }

        if(pid == 0)
        {
                endpriv();
                requester_exec(hostId, port, clssp, isPrimary, serverCount);
                /*NOTREACHED*/
        }

        /* else, parent */
        return pid;
}


/**
 * Returns a new requester object.
 *
 * @param server        [in] Pointer to remote location to which to connect.
 *                      Caller may free on return.  Must not be NULL.
 * @param clssp         [in] Pointer to desired class of data-products.  Caller
 *                      may free upon return.  Must not be NULL.
 * @param isPrimary     [in] Whether or not the data-product exchange-mode
 *                      should be primary (i.e., use HEREIS) or alternate
 *                      (i.e., use COMINGSOON/BLKDATA).
 * @param serverCount   [in] The number of servers to which the same request will
 *                      be made.
 * @retval NULL         Failure.  errno is set.
 * @return              Pointer to initialized requester structure.  The
 *                      associated requester is executing.
 */
static Requester*
requester_new(
    const ServerInfo*   server,
    prod_class_t*       clssp,
    const int           isPrimary,
    const unsigned      serverCount)
{
    Requester*  reqstrp = (Requester*)malloc(sizeof(Requester));

    assert(server != NULL);
    assert(clssp != NULL);

    if (reqstrp != NULL) {
        int     error = 0;              /* success */

        (void)memset(reqstrp, 0, sizeof(Requester));

        reqstrp->clssp = NULL;
        reqstrp->pid = -1;
        reqstrp->isPrimary = isPrimary;
        reqstrp->source = strdup(serverInfo_getHostId(server));

        if (reqstrp->source == NULL) {
            error = 1;
        }
        else {
            reqstrp->port = serverInfo_getPort(server);
            reqstrp->clssp = clssp;
            reqstrp->pid =
                requester_spawn(reqstrp->source, reqstrp->port, reqstrp->clssp,
                    isPrimary, serverCount);
        }                               /* "reqstrp->source" allocated */

        if (error) {
            free(reqstrp);
            reqstrp = NULL;
        }
    }                                   /* "reqstrp" allocated */

    return reqstrp;
}


static Requester *requesters;

/*
 * Creates a new requester and adds it to the list of requesters.  The new
 * requester is executing
 *
 * Arguments:
 *      server          Pointer to information on the server to which to
 *                      connect.  Caller may free on return.  Must not be NULL.
 *      clssp           Pointer to desired class of data-products.  Caller may
 *                      free upon return.  Must not be NULL.
 *      isPrimary       Whether or not the data-product exchange-mode should be
 *                      primary (i.e., use HEREIS) or alternate (i.e., use
 *                      COMINGSOON/BLKDATA).
 *      serverCount     The number of servers to which the same request will be
 *                      made.
 * Returns:
 *      0               Success.
 *      else            <errno.h> error-code.
 */
static int
requester_add(
    const ServerInfo*   server,
    prod_class_t*       clssp,
    const int           isPrimary,
    unsigned            serverCount)
{
    int         error = 0;              /* success */
    Requester*  reqstrp =
        requester_new(server, clssp, isPrimary, serverCount);

    if (reqstrp == NULL) {
        error = errno;
    }
    else {
        if (requesters == NULL) {
            requesters = reqstrp;
        }
        else {
            Requester*  ep = requesters;

            while(ep->next != NULL)
                ep = ep->next;

            ep->next = reqstrp;
        }
    }                                   /* "reqstrp" allocated */

    return error;
}


/******************************************************************************
 * Subscription Module
 *****************************************************************************/

typedef struct {
    const char* pattern;
    feedtypet   feedtype;
} Subscription;

/**
 * Returns a new subscription.
 *
 * @param feedtype  [in] Feedtype.
 * @param pattern   [in] Pattern. The client may free upon return.
 * @retval NULL     Failure. log_add() called.
 * @return          Pointer to the new subscription.
 */
static Subscription*
sub_new(
        const feedtypet     feedtype,
        const char* const   pattern)
{
    char* const pat = strdup(pattern);

    if (pat == NULL) {
        LOG_SERROR1("Couldn't duplicate string \"%s\"", pattern);
    }
    else {
        Subscription*   sub = malloc(sizeof(Subscription));

        if (sub == NULL) {
            LOG_SERROR0("Couldn't allocate new subscription object");
        }
        else {
            sub->feedtype = feedtype;
            sub->pattern = pat;

            return sub;
        } /* "sub" allocated */

        free(pat);
    } /* "pat" allocated */

    return NULL;
}


/**
 * Frees a subscription.
 *
 * @param sub       [in/out] Pointer to the subscription.
 */
static void
sub_free(
        Subscription* const   sub)
{
    free((void*)sub->pattern); /* cast away "const" */
    free(sub);
}


/**
 * Returns the formatted encoding of a subscription.
 *
 * @param buf       [in/out] The buffer into which to format the subscription.
 * @param size      [in] The size of the buffer in bytes.
 * @param sub       [in] Pointer to the subscription to be formatted.
 */
static int
sub_toString_r(
        char* const                 buf,
        const size_t                size,
        const Subscription* const   sub)
{
    return snprintf(buf, size, "(%s, \"%s\")", s_feedtypet(sub->feedtype),
            sub->pattern);
}


/**
 * Returns the formatted encoding of a subscription.
 *
 * @param sub       [in] Pointer to the subscription to be formatted.
 * @return          Pointer to a static buffer containing the formatted
 *                  encoding.
 */
static const char*
sub_toString(
        const Subscription* const   sub)
{
    static char buf[1024];

    (void)sub_toString_r(buf, sizeof(buf), sub);

    return buf;
}


/**
 * Clones a subscription.
 *
 * @param sub       [in] The subscription to be cloned. Client may free upon
 *                  return.
 * @retval NULL     Failure. log_add() called.
 * @return          Pointer to the clone.
 */
static Subscription*
sub_clone(
        const Subscription* const   sub)
{
    Subscription* const   clone = sub_new(sub->feedtype, sub->pattern);

    if (clone == NULL)
        LOG_ADD1("Couldn't clone subscription %s", sub_toString(sub));

    return clone;
}


/**
 * Returns the feedtype of a subscription.
 *
 * @param sub       [in] The subscription.
 * @return          The feedtype of the subscription.
 */
static feedtypet
sub_getFeedtype(
        const Subscription* const   sub)
{
    return sub->feedtype;
}


/**
 * Returns the pattern of a subscription.
 *
 * @param sub       [in] The subscription.
 * @return          The pattern of the subscription.
 */
static const char*
sub_getPattern(
        const Subscription* const   sub)
{
    return sub->pattern;
}


/**
 * Removes one subscription from another.
 *
 * @param target        [in/out] The subscription to be potentially modified.
 * @param remove        [in] The subscription to be removed.
 * @retval 0            The target subscription was not modified.
 * @retval 1            The target subscription was modified.
 */
static int
sub_remove(
        Subscription* const         target,
        const Subscription* const   remove)
{
    if ((strcmp(target->pattern, remove->pattern) == 0) &&
            (target->feedtype & remove->feedtype)) {
        target->feedtype &= ~remove->feedtype;

        return 1;
    }

    return 0;
}


/**
 * Indicates whether or not a subscription specifies nothing.
 *
 * @param sub       [in] The subscription to be checked.
 * @retval 0        The subscription is not empty.
 * @retval 1        The subscription is empty.
 */
static int
sub_isEmpty(
        const Subscription* const   sub)
{
    return sub->feedtype == NONE;
}


/**
 * Indicates if two subscriptions are equal.
 *
 * @param sub1      [in] The first subscription.
 * @param sub2      [in] The second subscription.
 * @retval 0        The subscriptions are not equal.
 * @retval 1        The subscriptions are equal.
 */
static int
sub_equal(
        const Subscription* const   sub1,
        const Subscription* const   sub2)
{
    return (sub1->feedtype == sub2->feedtype)
            && (strcmp(sub1->pattern, sub2->pattern) == 0);
}


/******************************************************************************
 * Request Module
 *
 * A request contains a subscription. A request is also a member of a
 * linked-list.
 *****************************************************************************/

typedef struct request {
    struct request*     next;
    Subscription*       subscription;
} Request;

/**
 * Returns a new, uninitialized request object. The request is not a member of
 * any list.
 *
 * @retval NULL     Failure. log_add() called.
 * @return          A new, uninitialized request.
 */
static Request* req_alloc()
{
    static size_t   nbytes = sizeof(Request);
    Request*        req = malloc(nbytes);

    if (req == NULL) {
        LOG_ADD1("Couldn't allocate %lu bytes for a new request object", nbytes);
    }
    else {
        req->subscription = NULL;
        req->next = NULL;
    }

    return req;
}

/**
 * Initializes a request.
 *
 * @param req           [in/out] Pointer to the request to be initialized.
 * @param sub           [in] Pointer to the subscription. Copied.
 * @param next          [in] Pointer to the next request in the linked-list or
 *                      NULL.
 * @retval 0            Success.
 * @retval -1           Failure. log_add() called.
 */
static int req_init(
    Request* const              req,
    const Subscription* const   sub,
    Request* const              next)
{
    Subscription*   subClone = sub_clone(sub);

    if (subClone != NULL) {
        req->subscription = subClone;
        req->next = next;

        return 0;
    } /* "subClone" allocated */

    return -1;
}

/**
 * Frees a request.
 *
 * @param req       [in] Pointer to the request to be freed or NULL.
 */
static void req_free(
        Request* const  req)
{
    if (req != NULL) {
        if (req->subscription != NULL) {
            sub_free(req->subscription);
            req->subscription = NULL;
        }

        req->next = NULL;

        free(req);
    }
}

/**
 * Creates a new request object.
 *
 * @param sub           [in] Pointer to the subscription. Copied.
 * @param next          [in] Pointer to the next request in the linked-list or
 *                      NULL.
 * @retval 0            Success.
 * @retval -1           Failure. log_add() called.
 */
static Request* req_new(
    const Subscription* const   sub,
    Request* const              next)
{
    Request*    request = req_alloc();

    if (request != NULL) {
        if (req_init(request, sub, next)) {
            req_free(request);
            request = NULL;
        }
    }

    return request;
}

/**
 * Returns the subscription of a request.
 *
 * @param request       [in] Pointer to the request.
 * @return              The request's subscription.
 */
static const Subscription* req_getSubscription(
        const Request* const    request)
{
    return request->subscription;
}

/**
 * Returns the next request in the linked-list.
 *
 * @param request       [in] Pointer to the request just before the request to
 *                      be returned.
 * @retval NULL         No more requests.
 * @return              Pointer to the next request.
 */
static Request* req_getNext(
        Request* const  request)
{
    return request->next;
}

/******************************************************************************
 * Server-Information Entry Module
 *****************************************************************************/

struct serverEntry {
    struct serverEntry* next;
    const ServerInfo*   serverInfo;
    Request*            requests;
};
typedef struct serverEntry    ServerEntry;

/**
 * Returns a new server-entry.
 *
 * @param server    [in] Information on the server. Client may free upon
 *                  return.
 * @retval NULL     Failure. log_add() called.
 * @return          Pointer to the new server-entry.
 */
static ServerEntry*
serverEntry_new(
        const ServerInfo* const server)
{
    const ServerInfo* const clone = serverInfo_clone(server);

    if (clone != NULL) {
        ServerEntry*  entry = malloc(sizeof(ServerEntry));

        if (entry == NULL) {
            LOG_SERROR0("Couldn't allocate new server-entry object");
        }
        else {
            entry->serverInfo = clone;
            entry->next = NULL;
            entry->requests = NULL;

            return entry;
        } /* "entry" allocated */

        serverInfo_free(clone);
    } /* "clone" allocated */

    return NULL;
}


/**
 * Frees a server-information entry.
 *
 * @param entry     [in/out] Pointer to the server-information entry.
 */
static void
serverEntry_free(
        ServerEntry* const    entry)
{
    struct request*  request;

    for (request = entry->requests; request != NULL;) {
        Request*    next = req_getNext(request);

        req_free(request);

        request = next;
    }

    serverInfo_free(entry->serverInfo);
    free(entry);
}


/**
 * Returns the server-information of a server-entry.
 *
 * @param serverEntry       [in] The server-entry.
 * @return                  The server-information. Client shall not free.
 */
static const ServerInfo*
serverEntry_getServerInfo(
        const ServerEntry* const    serverEntry)
{
    return serverEntry->serverInfo;
}


/**
 * Reduces a subscription by the subscriptions in a server-entry. log_add() is
 * called for every overlap in subscriptions.
 *
 * @param entry         [in] The server-entry.
 * @param sub           [in/out] The subscription to be reduced.
 * @retval 0            Success.
 * @retval -1           Failure. log_add() called.
 */
static int
serverEntry_reduceSub(
        const ServerEntry* const    entry,
        Subscription* const         sub)
{
    Subscription*   origSub = sub_clone(sub);

    if (origSub != NULL) {
        struct request* req;

        for (req = entry->requests; req != NULL; req = req_getNext(req)) {
            const Subscription* const   entrySub = req_getSubscription(req);

            if (sub_remove(sub, entrySub)) {
                char    buf[1024];

                (void)sub_toString_r(buf, sizeof(buf), origSub);
                LOG_ADD2("Subscription %s overlaps subscription %s",
                        buf, sub_toString(entrySub));
            }
        }

        sub_free(origSub);

        return 0;
    } /* "origSub" allocated */

    return -1;
}


/**
 * Adds to a server-entry.
 *
 * @param entry         [in/out] Pointer to the server-entry to be added to.
 * @param sub           [in/out] Pointer to the subscription to be added. The
 *                      subscription will be reduced by overlapping previous
 *                      subscriptions to the same server. The result might be
 *                      an empty specification, in which case the subscription
 *                      is ignored (i.e., not added). The client may free upon
 *                      return.
 * @retval 0            Success. log_add() is called if the subscription was
 *                      reduced.
 * @retval -1           Failure. log_add() called.
 */
static int
serverEntry_add(
        ServerEntry* const  entry,
        Subscription* const sub)
{
    int     status = -1; /* failure */

    if (serverEntry_reduceSub(entry, sub)) {
        LOG_ADD0("Couldn't reduce subscription by previous subscriptions");
    }
    else {
        if (sub_isEmpty(sub)) {
            status = 0;
        }
        else {
            Request*    request = req_new(sub, entry->requests);

            if (request != NULL) {
                entry->requests = request;
                status = 0;
            }
        }
    } /* subscription successfully reduced */

    return status;
}


/******************************************************************************
 * Set of Server-Informations Module
 *****************************************************************************/

static ServerEntry*         serverEntries = NULL;

/**
 * Adds server-information to the set of server-informations if it isn't already
 * present. Returns the corresponding entry.
 *
 * @param server        [in] Pointer to the server-information. Client may free
 *                      upon return.
 * @retval NULL         Failure. log_add() called.
 * @return              Pointer to the corresponding entry.
 */
static ServerEntry*
servers_addIfAbsent(
        const ServerInfo* const server)
{
    ServerEntry*  entry;

    for (entry = serverEntries; entry != NULL; entry = entry->next)
        if (serverInfo_equal(server, entry->serverInfo))
            return entry;

    entry = serverEntry_new(server);

    if (entry == NULL) {
        LOG_ADD0("Couldn't create new server-entry");
        return NULL;
    }

    entry->next = serverEntries;
    serverEntries = entry;

    return entry;
}


/**
 * Frees the set of server-informations.
 */
static void
servers_free(void)
{
    ServerEntry*    entry = serverEntries;

    while (entry != NULL) {
        ServerEntry*    next = entry->next;

        serverEntry_free(entry);

        entry = next;
    }

    serverEntries = NULL;
}

/******************************************************************************
 * Host-Set Module
 ******************************************************************************/

static int
host_set_match(const peer_info *rmtip, const host_set *hsp)
{
        if(rmtip == NULL || hsp == NULL)
                return 0;
        if(rmtip->astr == hsp->cp || rmtip->name == hsp->cp)
                return 1;
        switch (hsp->type) {
        case HS_NAME:
                if(strcasecmp(rmtip->name, hsp->cp) == 0)
                        return 1;
                break;
        case HS_DOTTED_QUAD:
                if(strcmp(rmtip->astr, hsp->cp) == 0)
                        return 1;
                break;
        case HS_REGEXP:
                if(regexec(&hsp->rgx, rmtip->astr, 0, NULL, 0) == 0)
                        return 1;
                /* else */
                if(regexec(&hsp->rgx, rmtip->name, 0, NULL, 0) == 0)
                        return 1;
                break;
        }
        return 0;
}


static int
contains(const host_set *hsp, const char *name, const char *dotAddr)
{
    int contains;

    if (hsp->type == HS_NAME) {
        contains = strcasecmp(name, hsp->cp) == 0;
    }
    else if (hsp->type == HS_DOTTED_QUAD) {
        contains = strcmp(dotAddr, hsp->cp) == 0;
    }
    else if (hsp->type == HS_REGEXP) {
        contains =
            regexec(&hsp->rgx, dotAddr, 0, NULL, 0) == 0 ||
            regexec(&hsp->rgx, name, 0, NULL, 0) == 0;
    }
    else {
        contains = 0;
    }

    return contains;
}


/******************************************************************************
 * Subscription-Entry Module
 ******************************************************************************/

struct subEntry {
    struct subEntry*        next;
    Subscription*           subscription;
    const ServerInfo**      servers;
    unsigned                serverCount;
};
typedef struct subEntry SubEntry;

/**
 * Returns a new subscription-entry.
 *
 * @param sub       [in] The subscription to be in the entry. Client may free
 *                  upon return.
 * @retval NULL     Failure. log_add() called.
 * @return          Pointer to the new entry.
 */
static SubEntry*
subEntry_new(
        const Subscription* const   sub)
{
    Subscription*   subClone = sub_clone(sub);

    if (subClone != NULL) {
        SubEntry*   entry = malloc(sizeof(SubEntry));

        if (entry == NULL) {
            LOG_SERROR0("Couldn't allocate new subscription-entry");
        }
        else {
            entry->next = NULL;
            entry->subscription = subClone;
            entry->servers = NULL;
            entry->serverCount = 0;

            return entry;
        } /* "entry" allocated */

        sub_free(subClone);
    } /* "subClone" allocated */

    return NULL;
}


/**
 * Adds server information to a subscription entry.
 *
 * @param entry         [in] The subscription entry.
 * @param server        [in] The server information. Client may free upon
 *                      return.
 * @retval 0            Success.
 * @retval -1           Failure. log_add() called.
 */
static int
subEntry_add(
    SubEntry* const         entry,
    const ServerInfo* const server)
{
    const ServerInfo** const    servers = (const ServerInfo**)realloc(
            entry->servers,
            (size_t)((entry->serverCount+1)*sizeof(ServerInfo*)));

    if (NULL == servers) {
        LOG_SERROR0("Couldn't allocate new server-information array");
    }
    else {
        const ServerInfo* const clone = serverInfo_clone(server);

        if (clone != NULL) {
            servers[entry->serverCount] = clone;
            entry->servers = servers;
            entry->serverCount++;

            return 0;
        } /* "clone" allocated */

        free(servers);
    } /* "servers" allocated */

    return -1;
}


/**
 * Starts a downstream LDM for each server of a subscription entry.
 *
 * @param entry     [in] The subscription entry.
 * @retval 0        Success.
 * @return          System error code. log_add() called.
 */
static int
subEntry_startRequester(
        SubEntry* const entry)
{
    int         status = 0; /* success */
    unsigned    serverIndex;

    for (serverIndex = 0; serverIndex < entry->serverCount; serverIndex++) {
        prod_class_t*       clssp;
        const ServerInfo*  requestServer = entry->servers[serverIndex];

        clssp = new_prod_class(1);

        if (clssp == NULL) {
            status = errno;
        }
        else {
            prod_spec*  sp;

            clssp->from = TS_ZERO; /* prog_requester() adjusts */
            clssp->to = TS_ENDT;

            sp = clssp->psa.psa_val;
            sp->feedtype = sub_getFeedtype(entry->subscription);
            sp->pattern = strdup(sub_getPattern(entry->subscription));

            if (sp->pattern == NULL) {
                LOG_SERROR1("Couldn't duplicate pattern \"%s\"",
                        sub_getPattern(entry->subscription));
                status = errno;
            }
            else {
                (void)re_vetSpec(sp->pattern);

                if (regcomp(&sp->rgx, sp->pattern,
                        REG_EXTENDED|REG_NOSUB) != 0) {
                    LOG_ADD1("Couldn't compile pattern \"%s\"", sp->pattern);
                    status = EINVAL;
                }
                else {
                    status = requester_add(requestServer, clssp,
                            serverIndex == 0, entry->serverCount);
                }
            } /* "sp->pattern" allocated */

            free_prod_class(clssp);

            if (status)
                break;
        } /* "clssp" allocated */
    } /* server-information loop */

    return status;
}


/**
 * Frees a subscription entry.
 *
 * @param entry     [in/out] The subscription entry to be freed.
 */
static void
subEntry_free(
        SubEntry* const entry)
{
    int i;

    for (i = 0; i < entry->serverCount; i++)
        serverInfo_free(entry->servers[i]);

    free(entry->servers);
    sub_free(entry->subscription);
    free(entry);
}

/******************************************************************************
 * Set of Subscriptions Module
 ******************************************************************************/

/**
 * The set of subscriptions. A queue is used rather than a stack in order to
 * maintain temporal ordering.
 */
static SubEntry*   subsHead = NULL;
static SubEntry*   subsTail = NULL;


/**
 * Adds a subscription to the subscriptions table if it isn't already present.
 * Returns the corresponding entry.
 *
 * @param sub           [in] Pointer to the subscription. Client may free upon
 *                      return.
 * @retval NULL         Failure. log_add() called.
 * @return              Pointer to the corresponding entry.
 */
static SubEntry*
subs_addIfAbsent(
        const Subscription* const   sub)
{
    SubEntry*  entry;

    for (entry = subsHead; entry != NULL; entry = entry->next)
        if (sub_equal(sub, entry->subscription))
            return entry;

    entry = subEntry_new(sub);

    if (NULL == entry) {
        LOG_ADD0("Couldn't create new subscription-entry");
        return NULL;
    }

    if (NULL == subsHead) {
        subsHead = entry;
    }
    else {
        subsTail->next = entry;
    }

    subsTail = entry;

    return entry;
}


/**
 * Frees the set of subscriptions.
 */
static void
subs_free(void)
{
    SubEntry* entry = subsHead;

    while (entry != NULL) {
        SubEntry*  nextEntry = entry->next;

        subEntry_free(entry);

        entry = nextEntry;
    }

    subsHead = NULL;
    subsTail = NULL;
}


/**
 * Starts all downstream LDM-s necessary to satisfy the set of subscriptions.
 *
 * @retval 0        Success.
 * @return          System error code. log_add() called.
 */
static int
subs_startRequesters(void)
{
    int status = ENOERR;

    SubEntry*   entry;

    for (entry = subsHead; entry != NULL; entry = entry->next) {
        if ((status = subEntry_startRequester(entry)))
            break;
    }
#if 0   /* DEBUG */
    {
        char buf[1984];
        Requester *reqstrp;

        for (reqstrp = requesters; reqstrp != NULL; reqstrp = reqstrp->next)
        {
            unotice("%s: %s",
                 reqstrp->source,
                 s_prod_class(buf, sizeof(buf), reqstrp->clssp));
        }
    }
#endif

    return status;
}

/**
 * Adds a subscription request. Helper function.
 *
 * @param sub           [in] Subscription to be added. Client may free upon
 *                      return.
 * @param serverEntry   [in/out] Server entry to which to add the subscription.
 * @return 0            Success.
 * @return -1           Failure. log_add() called.
 */
static int
addRequest(
        Subscription* const sub,
        ServerEntry* const  serverEntry)
{
    int             status = -1; /* failure */
    Subscription*   origSub = sub_clone(sub);

    if (origSub != NULL) {
        if (serverEntry_add(serverEntry, sub)) {
            LOG_ADD0("Couldn't add subscription to server entry");
        }
        else if (sub_isEmpty(sub)) {
            LOG_ADD1("Ignoring subscription %s because it "
                    "duplicates previous subscriptions or specifies nothing",
                    sub_toString(origSub));
            log_log(LOG_WARNING);
            status = 0;
        }
        else {
            SubEntry*  subEntry;

            if (!sub_equal(origSub, sub)) {
                char    buf[1024];

                (void)sub_toString_r(buf, sizeof(buf), origSub);

                LOG_ADD2("Subscription %s reduced to %s by previous "
                        "subscriptions", buf, sub_toString(sub));
                log_log(LOG_WARNING);
            }

            subEntry = subs_addIfAbsent(sub);

            if (subEntry == NULL) {
                LOG_ADD0("Couldn't get subscription entry");
            }
            else {
                if (subEntry_add(subEntry,
                        serverEntry_getServerInfo(serverEntry))) {
                    LOG_ADD0("Couldn't add server information to subscription "
                            "entry");
                }
                else {
                    status = 0; /* success */
                }
            } /* got subscription entry */
        } /* non-empty reduced subscription added to server-entry */

        sub_free(origSub);
    } /* "origSub" allocated */

    return status;
}


/******************************************************************************
 * ACCEPT Entries Module
 *****************************************************************************/

struct acceptEntry {
    feedtypet           ft;
    char*               pattern;
    regex_t*            rgx;
    host_set*           hsp;
    struct acceptEntry* next;
    int                 isPrimary;
};
typedef struct acceptEntry AcceptEntry;

static AcceptEntry*     acceptEntries;


/**
 * Returns a new accept-entry.
 *
 * @param ft            [in] The feedtype.
 * @param pattern       [in] Pointer to allocated memory containing the pattern for
 *                      matching the data-product identifier.  Upon successful
 *                      return, the client shall abandon responsibility for
 *                      freeing.
 * @param rgxp          [in] Pointer to allocated memory containing the
 *                      regular-expression for matching the data-product
 *                      identifier.  Upon successful return, the client shall
 *                      abandon responsibility for calling "regfree(rgxp)" and
 *                      "free(rgxp)".
 * @param hsp           [in] Pointer to allocated memory containing the allowed set
 *                      of hosts.  Upon successful return, the client shall
 *                      abandon responsibility for calling
 *                      "free_host_set(hsp)".
 * @param isPrimary     [in] Whether or not the initial data-product exchange-mode is
 *                      primary (i.e., uses HEREIS) or alternate (i.e., uses
 *                      COMINGSOON/BLKDATA).
 * @retval NULL         Failure.  errno is set.
 * @return              Success.
 */
static AcceptEntry *
acceptEntry_new(
    feedtypet   ft,
    char*       pattern,
    regex_t*    rgxp,
    host_set*   hsp,
    int         isPrimary)
{
    AcceptEntry*       ap = (AcceptEntry *)malloc(sizeof(AcceptEntry));

    if (NULL != ap) {
        ap->ft = ft;
        ap->pattern = pattern;

        if (rgxp != NULL)
            ap->rgx = rgxp;

        ap->hsp = hsp;
        ap->isPrimary = isPrimary;
        ap->next = NULL;
    }

    return ap;
}


/**
 * Frees an accept-entry.
 *
 * @param entry     [in/out] Pointer to the accept-entry. May be NULL.
 */
static void
acceptEntry_free(
    AcceptEntry* const  entry)
{
    if (NULL != entry) {
        if (NULL != entry->pattern) {
            free(entry->pattern);

            if (NULL != entry->rgx) {
                regfree(entry->rgx);
                free(entry->rgx);
            }
        }

        lcf_freeHostSet(entry->hsp);
        free(entry);
    }
}

/**
 * Frees the set of ACCEPT entries.
 */
static void
acceptEntries_free(void)
{
    AcceptEntry*       entry = acceptEntries;

    while (NULL != entry) {
        AcceptEntry*   next = entry->next;
        acceptEntry_free(entry);
        entry = next;
    }

    acceptEntries = NULL;
}


/**
 * Adds an accept-entry to the set of accept-entries.
 *
 * @param app           [in/out] Address of the pointer to the access-control list.  May
 *                      not be NULL.
 * @param ft            [in] The feedtype.
 * @param pattern       [in] Pointer to allocated memory containing the pattern
 *                      for matching the data-product identifier.  The client
 *                      shall not free.
 * @param rgxp          [in] Pointer to allocated memory containing the
 *                      regular-expression for matching the data-product
 *                      identifier.  The client shall not free.
 * @param hsp           [in] Pointer to allocated memory containing the allowed set
 *                      of hosts.  The client shall not free.
 * @param isPrimary     [in] Whether or not the initial data-product exchange-mode is
 *                      primary (i.e., uses HEREIS) or alternate (i.e., uses
 *                      COMINGSOON/BLKDATA).
 * @retval 0            Success
 * @return              System error code.
 */
static int
acceptEntries_add(
    AcceptEntry** app,
    feedtypet     ft,
    char*         pattern,
    regex_t*      rgxp,
    host_set*     hsp,
    int           isPrimary)
{
        AcceptEntry *ap = acceptEntry_new(ft, pattern, rgxp, hsp, isPrimary);
        if(ap == NULL)
                return errno;

        if(*app == NULL)
        {
                *app = ap;
        }
        else
        {
                AcceptEntry *ep = *app;
                while(ep->next != NULL)
                    ep = ep->next;
                ep->next = ap;
        }
        return ENOERR;
}


/******************************************************************************
 * ALLOW Entries Module
 *****************************************************************************/

typedef struct AllowEntry {
    struct AllowEntry*          next;
    host_set*                   hsp;
    Pattern*                    okPattern;
    Pattern*                    notPattern;
    feedtypet                   ft;
} AllowEntry;
static AllowEntry*          allowEntryHead = NULL;
static AllowEntry*          allowEntryTail = NULL;


/*
 * Frees the resources of the ALLOW entries.
 */
static void
allowEntries_free(void)
{
    AllowEntry* entry = allowEntryHead;

    while (entry != NULL) {
        AllowEntry*     nextEntry = entry->next;

        pat_free(entry->okPattern);
        pat_free(entry->notPattern);
        lcf_freeHostSet(entry->hsp);
        free(entry);

        entry = nextEntry;
    }

    allowEntryHead = NULL;
    allowEntryTail = NULL;
}


/******************************************************************************
 * EXEC Action Module
 ******************************************************************************/

struct process {
    pid_t           pid;
    wordexp_t       wrdexp;
    struct process* next;
};
typedef struct process  Process ;

static Process*         processes;


static void
proc_free(Process *proc)
{
    if (NULL != proc) {
        wordfree(&proc->wrdexp);

        if(proc->pid > 0) {
          /*EMPTY*/
          /* TODO */ ;
        }

        free(proc);
    }
}


static int
close_rest(int bottom)
{
        static long open_max = 0; /* number of descriptors */
        int ii;
        if(!open_max)
        {
#ifdef _SC_OPEN_MAX
                open_max = sysconf(_SC_OPEN_MAX);
#else
                open_max = 32 ; /* punt */
#endif
        }
        for(ii = bottom; (long)ii < open_max ; ii++)
                (void)close(ii) ;
        return ii ;
}


static int
proc_exec(Process *proc)
{
        assert(proc->pid == -1);
        assert(proc->wrdexp.we_wordv[proc->wrdexp.we_wordc] == NULL);

        proc->pid = ldmfork() ;
        if(proc->pid == -1)
        {       /* failure */
                log_log(LOG_ERR);
                return -1;
        }
        /* else */

        if(proc->pid == 0)
        {       /* child */
                const unsigned  ulogOptions = ulog_get_options();
                const char*     ulogIdent = getulogident();
                const unsigned  ulogFacility = getulogfacility();
                const char*     ulogPath = getulogpath();

                /* restore signals */
                {
                        struct sigaction sigact;

                        (void)sigemptyset(&sigact.sa_mask);
                        sigact.sa_flags = 0;
                        sigact.sa_handler = SIG_DFL;

                        (void) sigaction(SIGPIPE, &sigact, NULL);
                        (void) sigaction(SIGHUP, &sigact, NULL);
                        (void) sigaction(SIGUSR1, &sigact, NULL);
                        (void) sigaction(SIGUSR2, &sigact, NULL);
                        (void) sigaction(SIGCHLD, &sigact, NULL);
                        (void) sigaction(SIGALRM, &sigact, NULL);
                        (void) sigaction(SIGINT, &sigact, NULL);
                        (void) sigaction(SIGTERM, &sigact, NULL);
                }

                /* Set up fd 0,1 */
                (void)close(0);
                {
                        int fd = open("/dev/null", O_RDONLY);
                        if(fd > 0)
                        {
                                (void) dup2(fd, 0);
                                (void) close(fd);
                        }
                }
                (void)close(1);
                {
                        int fd = open("/dev/console", O_WRONLY);
                        if(fd < 0)
                                fd = open("/dev/null", O_WRONLY);
                        if(fd > 1)
                        {
                                (void) dup2(fd, 1);
                                (void) close(fd);
                        }
                }
                if(logfname == NULL)
                {
                        (void)close(2);
                        {
                                int fd = open("/dev/console", O_WRONLY);
                                if(fd < 0)
                                        fd = open("/dev/null", O_WRONLY);
                                if(fd > 2)
                                {
                                        (void) dup2(fd, 2);
                                        (void) close(fd);
                                }
                        }
                } /* else, the logFd is stderr, and that's ok */
                close_rest(3);
                endpriv();
                (void) execvp(proc->wrdexp.we_wordv[0], proc->wrdexp.we_wordv);
                openulog(ulogIdent, ulogOptions, ulogFacility, ulogPath);
                serror("execvp: %s", proc->wrdexp.we_wordv[0]) ;
                _exit(127) ;
        }
        /* else, parent */

        return (int)proc->pid;
}


/**
 * Returns a new process-information object.
 *
 * @param wrdexpp       [in] List of command-line words.
 * @retval NULL         Failure. Error-message logged.
 * @return              Pointer to the new process-information.
 */
static Process *
proc_new(
    wordexp_t*  wrdexpp)
{
    Process *proc = (Process*)malloc(sizeof(Process));

    if (proc != NULL) {
        proc->wrdexp = *wrdexpp;
        proc->pid = -1;
        proc->next = NULL;

        if (proc_exec(proc) < 0) {
            proc_free(proc);
            proc = NULL;
        }
    }

    return proc;
}

/******************************************************************************
 * Public Interface:
 ******************************************************************************/

/**
 * Whether or not a top-level LDM server needs to run based on the entries in
 * the LDM configuration-file. The top-level LDM server only needs to exist if
 * the configuration-file contains ALLOW or ACCEPT entries.
 */
static bool serverNeeded = false;

/**
 * Adds an EXEC entry and executes the command as a child process.
 *
 * @param words     [in] Command-line words. Client may free upon return.
 * @retval 0        Success.
 * @return          System error code.
 */
int
lcf_addExec(wordexp_t *wrdexpp)
{
        Process *process = proc_new(wrdexpp);
        if(process == NULL)
                return errno;

        if(processes == NULL)
        {
                processes = process;
        }
        else
        {
                Process *ep = processes;
                while (ep->next != NULL)
                    ep = ep->next;
                ep->next = process;
        }
        return ENOERR;
}


/**
 * Frees an EXEC entry.
 *
 * @param pid       [in] The process identifier of the child process whose
 *                  entry is to be freed.
 */
void
lcf_freeExec(
    const pid_t pid)
{
    Process*      entry = processes;
    Process**     prev = &processes;

    for (; entry != NULL; prev = &entry->next, entry = entry->next) {
        if (entry->pid == pid) {
            *prev = entry->next;
            proc_free(entry);
            break;
        }
    }
}


/**
 * Returns the command-line of an EXEC entry.
 *
 * @param pid       [in] The process identifier of the child process.
 * @param buf       [in] The buffer into which to write the command-line.
 * @param size      [in] The size of "buf".
 * @retval -2       The child process wasn't found.
 * @retval -1       Write error.  See errno.  Error-message(s) written via
 *                  log_*().
 * @return          The number of characters written into "buf" excluding any
 *                  terminating NUL.  If the number of characters equals "size",
 *                  then no terminating NUL was written.
 */
int
lcf_getCommandLine(
    const pid_t         pid,
    char* const         buf,
    size_t              size)
{
    int                 status;
    const Process*        ep;

    for (ep = processes; ep != NULL; ep = ep->next) {
        if (ep->pid == pid)
            break;
    }

    if (ep == NULL) {
        status = -2;
    }
    else {
        const wordexp_t*        wrdexpp = &ep->wrdexp;
        char*                   cp = buf;
        size_t                  i;

        status = 0;

        for (i = 0; i < wrdexpp->we_wordc; i++) {
            int n = snprintf(cp, size, i == 0 ? "%s" : " %s",
                wrdexpp->we_wordv[i]);

            if (n >= 0 && n <= size) {
                cp += n;
                size -= n;
            }
            else {
                log_start("%s", strerror(errno));
                log_add("Couldn't write command-line word %lu", (unsigned)i);

                status = -1;
                break;
            }
        }

        if (status != -1)
            status = cp - buf;
    }

    return status;
}


/**
 * Adds a REQUEST entry.
 *
 * @param feedtype      [in] Feedtype.
 * @param pattern       [in] Pattern. Client may free upon return.
 * @param hostId        [in] Host identifier. Client may free upon return.
 * @param port          [in] Port number.
 * @retval 0            Success.
 * @retval -1           System error. log_add() called.
 */
int
lcf_addRequest(
    const feedtypet     feedtype,
    const char* const   pattern,
    const char* const   hostId,
    const unsigned      port)
{
    int                 status = -1; /* failure */
    const ServerInfo*   server = serverInfo_new(hostId, port);

    if (server == NULL) {
        LOG_ADD0("Couldn't create new server-information object");
    }
    else {
        ServerEntry*  serverEntry = servers_addIfAbsent(server);

        if (serverEntry == NULL) {
            LOG_ADD0("Couldn't get server entry");
        }
        else {
            Subscription*   sub = sub_new(feedtype, pattern);

            if (sub == NULL) {
                LOG_ADD0("Couldn't create new subscription object");
            }
            else {
                status = addRequest(sub, serverEntry);

                sub_free(sub);
            } /* "sub" allocated */
        } /* got server-information entry */

        serverInfo_free(server);
    } /* "server" allocated */

    return status;
}

/**
 * Returns a new specification of a set of hosts.
 *
 * @param cp    Pointer to host(s) specification.  Caller must not free on
 *              return if and only if call is successful and "type" is
 *              HS_REGEXP.
 * @param rgxp  Pointer to regular-expression structure.  Caller may free
 *              on return but must not call regfree() if and only if call is
 *              successful and "type" is HS_REGEXP.
 * @retval NULL Out of memory. No error-message logged or started.
 */
host_set *
lcf_newHostSet(enum host_set_type type, const char *cp, const regex_t *rgxp)
{
        host_set *hsp = (host_set *)malloc(sizeof(host_set));
        if(hsp == NULL)
                return NULL;

        hsp->type = type;
        hsp->cp = NULL;
        (void)memset(&hsp->rgx, 0, sizeof(regex_t));

        if(cp != NULL)
        {
                switch (type) {
                case HS_NAME:
                case HS_DOTTED_QUAD:
                        hsp->cp = strdup(cp);
                        if(hsp->cp == NULL)
                                goto unwind_alloc;
                        break;
                case HS_REGEXP:
                        /* private copies already allocate in the lexer */
                        hsp->cp = cp;
                        hsp->rgx = *rgxp;
                        break;
                }
        }

        return hsp;

unwind_alloc:
        free(hsp);
        return NULL;
}

/**
 * Frees a specification of a set of hosts.
 *
 * @param hsp       [in/out] The specification of a set of hosts.
 */
void
lcf_freeHostSet(host_set *hsp)
{
        if(hsp == NULL)
                return;
        if(hsp->type == HS_REGEXP)
                regfree(&hsp->rgx); /* cast away const */
        if(hsp->cp != NULL)
                free((void *)hsp->cp); /* cast away const */
        free(hsp);
}


/**
 * Adds an ALLOW entry.
 *
 * @param ft            [in] The feedtype.
 * @param hostSet       [in] Pointer to allocated set of allowed downstream hosts.
 *                      Upon successful return, the client shall abandon
 *                      responsibility for calling "free_host_set(hostSet)".
 * @param okEre         [in] Pointer to the ERE that data-product identifiers
 *                      must match.  Caller may free upon return.
 * @param notEre        [in] Pointer to the ERE that data-product identifiers
 *                      must not match or NULL if such matching should be
 *                      disabled.  Caller may free upon return.
 * @retval NULL         Success.
 * @return              Failure error object.
 */
ErrorObj*
lcf_addAllow(
    const feedtypet             ft,
    host_set* const             hostSet,
    const char* const           okEre,
    const char* const           notEre)
{
    ErrorObj*           errObj = NULL;  /* success */
    AllowEntry* const   entry = (AllowEntry*)malloc(sizeof(AllowEntry));

    if (NULL == entry) {
        errObj = ERR_NEW1(0, NULL, "Couldn't allocate new allow-entry",
            strerror(errno));
    }
    else {
        Pattern*        okPattern;

        errObj = pat_new(&okPattern, okEre, 0);

        if (errObj) {
            errObj = ERR_NEW(0, errObj, "Couldn't create OK-pattern");
        }
        else {
            Pattern*    notPattern;

            if (NULL == notEre) {
                notPattern = NULL;
            }
            else {
                if ((errObj = pat_new(&notPattern, notEre, 0)))
                    errObj = ERR_NEW(0, errObj, "Couldn't create not-pattern");
            }
            
            if (!errObj) {
                entry->hsp = hostSet;
                entry->okPattern = okPattern;
                entry->notPattern = notPattern;
                entry->ft = ft;
                entry->next = NULL;

                if (NULL == allowEntryHead) {
                    allowEntryHead = entry;
                    allowEntryTail = entry;
                }
                else {
                    allowEntryTail->next = entry;
                    allowEntryTail = entry;
                }

                serverNeeded = true;
            }

            if (errObj)
                pat_free(okPattern);
        }                               /* "okPattern" allocated */
        
        if (errObj)
            free(entry);
    }                                   /* "entry" allocated */

    return errObj;
}


/**
 * Returns the class of products that a host is allowed to receive based on the
 * host and the feed-types of products that it wants to receive.  The pointer
 * to the product-class structure will reference allocated space on success;
 * otherwise, it won't be modified.  The returned product-class may be the
 * empty set.  The client is responsible for invoking
 * free_prod_class(prod_class_t*) on a non-NULL product-class structure when it
 * is no longer needed.
 *
 * @param name          [in] Pointer to the name of the host.
 * @param addr          [in] Pointer to the IP address of the host.
 * @param want          [in] Pointer to the class of products that the host wants.
 * @param intersect     [out] Pointer to a pointer to the intersection of the
 *                      wanted product class and the allowed product class.
 *                      References allocated space on success; otherwise won't
 *                      be modified.  Referenced product-class may be empty.
 *                      On success, the caller should eventually invoke
 *                      free_prod_class(*intersect).
 * @retval 0            Success.
 * @retval EINVAL       The regular expression pattern of a
 *                      product-specification couldn't be compiled.
 * @retval ENOMEM       Out-of-memory.
 */
int
lcf_reduceToAllowed(
    const char*           name,
    const struct in_addr* addr,
    const prod_class_t*   want,
    prod_class_t ** const intersect)
{
    int              error = 0;          /* default success */
    size_t           nhits = 0;          /* number of matching ACL entries */
#   define           MAXHITS 128         /* TODO */
    feedtypet        feedType[MAXHITS];  /* matching feed-types */
    prod_class_t*    inter;              /* want and allow intersection */

    /*
     * Find the number of matching entries in the ACL and save their
     * feed-types.
     */
    if (allowEntryHead == NULL || want->psa.psa_len == 0) {
        /*
         * If there's no access-control-list (ACL) or the host wants nothing,
         * then the intersection is the empty set.
         */
        uwarn("%s:%d: no ACL or empty request", __FILE__, __LINE__);
        nhits = 0;
    }
    else {
        AllowEntry*     entry;                  /* ACL entry */
        char            dotAddr[DOTTEDQUADLEN]; /* dotted-quad IP address */

        (void)strcpy(dotAddr, inet_ntoa(*addr));

        for(entry = allowEntryHead; entry != NULL; entry = entry->next) {
            if (contains(entry->hsp, name, dotAddr)) {
                feedType[nhits++] = entry->ft;

                if (nhits >= MAXHITS) {
                    uerror("%s:%d: nhits (%u) >= MAXHITS (%d)",
                        __FILE__, __LINE__, nhits, MAXHITS);
                    break;
                }
            }
        }
    }

    /*
     * Allocate a product-class for the intersection.
     */
    inter = new_prod_class(nhits == 0 ? 0 : want->psa.psa_len);

    if(inter == NULL) {
        error = ENOMEM;
    }
    else if (nhits != 0) {
        error = cp_prod_class(inter, want, 0);

        if (error == 0) {
            /*
             * Compute the intersection.
             */
            int       ii;

            for (ii = 0; ii < inter->psa.psa_len; ii++) {
                feedtypet ft;
                size_t    jj;
                char      s1[255], s2[255], s3[255];

                sprint_feedtypet(s1, sizeof(s1),
                    inter->psa.psa_val[ii].feedtype);

                for (jj = 0; jj < nhits; jj++) {
                    sprint_feedtypet(s2, sizeof(s2), feedType[jj]);

                    ft = inter->psa.psa_val[ii].feedtype &
                        feedType[jj];

                    sprint_feedtypet(s3, sizeof(s3), ft);

                    if (ft) {
                        udebug("hit %s = %s & %s", s3, s1, s2);

                        inter->psa.psa_val[ii].feedtype = ft;

                        break; /* first match priority */
                    }
                }

                if (ft == NONE) {
                    udebug("miss %s", s1);

                    inter->psa.psa_val[ii].feedtype = NONE;
                }
            }

            clss_scrunch(inter);
        }

        if (error)
            (void)free_prod_class(inter);
    }

    if (!error)
        *intersect = inter;

    return error;
}


/**
 * Indicates if it's OK to feed or notify a given host a given class of
 * data-products.
 *
 * @param *rmtip        [in] Information on the remote host.  rmtip->clssp will
 *                      be set to the intersection unless there's an
 *                      error, or there are no matching host entries in the
 *                      ACL, or the intersection is the empty set, in which
 *                      case it will be unmodified.
 * @param *want         [in] The product-class that the host wants.
 * @retval 0            if successful.
 * @retval ENOMEM       if out-of-memory.
 * @retval EINVAL       if a regular expression of a product specification
 *                      couldn't be compiled.
 */
int
lcf_okToFeedOrNotify(peer_info *rmtip, prod_class_t *want)
{
    /*
     * The logic of this function seems very odd to me -- but I kept it the same
     * as Glenn's original forn_acl_ck().  SRE 2002-11-04
     */

    int         error;

    if (allowEntryHead == NULL || want == NULL || want->psa.psa_len == 0) {
        error = ENOERR;
    }
    else {
        prod_class_t *inter;  /* intersection of want and allowed */

        error =
            lcf_reduceToAllowed(rmtip->name, &rmtip->addr, want, &inter);

        if (!error) {
            if (inter->psa.psa_len == 0) {
                (void)free_prod_class(inter);
            }
            else {
                rmtip->clssp = inter;
            }
        }
    }

    return error;
}


/**
 * Returns the product-class appropriate for filtering data-products on the
 * upstream LDM before sending them to the downstream LDM.
 *
 * @param name          [in] Pointer to the name of the downstream host.
 * @param addr          [in] Pointer to the IP address of the downstream host.
 * @param want          [in] Pointer to the class of products that the downstream
 *                      host wants.
 * @param filter        [out] Pointer to a pointer to the upstream filter.
 *                      *filter is set on and only on success.  Caller
 *                      should call upFilter_free(*filter). *filter is set to
 *                      NULL if and only if no data-products should be sent to
 *                      the downstream LDM.
 * @retval NULL         Success.
 * @return              Failure error object.
 */
ErrorObj*
lcf_getUpstreamFilter(
    const char*                 name,
    const struct in_addr*       addr, 
    const prod_class_t*         want,
    UpFilter** const            upFilter)
{
    ErrorObj*   errObj;
    UpFilter*   filt;
    
    if ((errObj = upFilter_new(&filt))) {
        errObj = ERR_NEW(0, errObj, "Couldn't get new upstream filter");
    }
    else {
        int             i;
        char            dotAddr[DOTTEDQUADLEN];
        AllowEntry*     entry;

        (void)strcpy(dotAddr, inet_ntoa(*addr));

        for (i = 0; i < want->psa.psa_len; ++i) {
            for (entry = allowEntryHead; entry != NULL; entry = entry->next) {
                feedtypet       feedtype =
                    entry->ft & want->psa.psa_val[i].feedtype;

                if (feedtype && contains(entry->hsp, name, dotAddr)) {
                    if ((errObj = upFilter_addComponent(filt, feedtype,
                        entry->okPattern, entry->notPattern))) {

                        errObj = ERR_NEW2(0, errObj,
                            "Couldn't add upstream filter component for server "
                                "%s [%s]",
                            name, dotAddr);
                    }

                    break;              /* first match controls */
                }                       /* feedtype & server-information match */
            }                           /* ACL entry loop */
        }                               /* wanted product-specification loop */

        if (errObj) {
            upFilter_free(filt);
        }
        else {
            if (upFilter_getComponentCount(filt) > 0) {
                *upFilter = filt;
            }
            else {
                upFilter_free(filt);
                *upFilter = NULL;
            }
        }
    }                                   /* "filt" allocated */

    return errObj;
}


/**
 * Adds an ACCEPT entry.
 *
 * @param ft            [in] Feedtype.
 * @param pattern       [in] Pointer to allocated memory containing extended
 *                      regular-expression for matching the product-identifier.
 *                      The client shall not free.
 * @param rgxp          [in] Pointer to allocated memory containing the
 *                      regular-expression structure for matching
 *                      product-identifiers.  The client shall not free.
 * @param hsp           [in] Pointer to allocated memory containing the host-set.
 *                      The client shall not free.
 * @param isPrimary     [in] Whether or not the initial data-product exchange-mode
 *                      is primary (i.e., uses HEREIS) or alternate (i.e., uses
 *                      COMINGSOON/BLKDATA).
 * @retval 0            Success
 * @retval !0           <errno.h> error-code
 */
int
lcf_addAccept(
    feedtypet     ft,
    char*         pattern,
    regex_t*      rgxp,
    host_set*     hsp,
    int           isPrimary)
{
    int status = acceptEntries_add(&acceptEntries, ft, pattern, rgxp, hsp,
            isPrimary);

    if (0 == status)
        serverNeeded = true;

    return status;
}


/**
 * Checks the LDM configuration-file for ACCEPT entries that are relevant to a
 * given remote host.
 *
 * @param rmtip         [in/out] Information on the remote host. May be
 *                      modified.
 * @param offerd        [in] The product-class that the remote host is offering
 *                      to send.
 * @retval 0            if successful.
 * @retval ENOMEM       if out-of-memory.
 */
int
lcf_isHiyaAllowed(peer_info *rmtip, prod_class_t *offerd)
{
    int         error;

    if ((acceptEntries == NULL) || offerd == NULL || offerd->psa.psa_len == 0) {
        /*
         * I don't understand why, if there are no REQUEST or ACCEPT entries in
         * the ACL, that the acceptable product-class wouldn't become the empty
         * set.  SRE 2002-11-07
         */
        error = ENOERR;
    }
    else {
        prod_class_t*   prodClass;
        int             isPrimary;

        error = lcf_reduceToAcceptable(rmtip->name, inet_ntoa(rmtip->addr),
            offerd, &prodClass, &isPrimary);

        if (!error) {
            /*
             * I don't understand why an empty product-class set isn't a valid
             * value for rmtip->clssp.  SRE 2002-11-07
             */
            if (prodClass->psa.psa_len == 0) {
                free_prod_class(prodClass);
            }
            else {
                rmtip->clssp = prodClass;
            }
        }
    }

    return error;
}


/**
 * Determines the set of acceptable products given the upstream host and the
 * offered set of products.
 *
 * @param name          [in] Pointer to name of host.
 * @param addr          [in] Pointer to Internet address of host.
 * @param dotAddr       [in] Pointer to the dotted-quad form of the IP address
 *                      of the host.
 * @param offered       [in] Pointer to the class of offered products.
 * @param accept        [out] Address of pointer to set of acceptable products.
 *                      On success, the pointer will be set to reference
 *                      allocated space; otherwise, it won't be modified.
 *                      Acceptable product set may be empty. The client should
 *                      call "free_prod_class(prod_class_t*)" when the
 *                      product-class is no longer needed.  In general, the
 *                      class of acceptable products will be a subset of
 *                      *offered.
 * @param isPrimary     [in] Pointer to flag indicating whether or not the
 *                      data-product exchange-mode should be primary (i.e., use
 *                      HEREIS) or alternate (i.e., use COMINGSOON/BLKDATA).
 * @retval 0            Success.
 * @retval ENOMEM       Failure.  Out-of-memory.
 */
int
lcf_reduceToAcceptable(
    const char*         name,
    const char*         dotAddr, 
    prod_class_t*       offerd,
    prod_class_t**      accept,
    int*                isPrimary)
{
    int                 error = 0;
    prod_class_t*       prodClass;
    u_int               nhits = 0;
    AcceptEntry*               hits[MAXHITS];

    if (NULL != acceptEntries) {
        AcceptEntry       *ap;

        /*
         * Find ACCEPT entries with matching identifiers.
         */
        for (ap = acceptEntries; ap != NULL; ap = ap->next) {
            if (contains(ap->hsp, name, dotAddr)) {
                hits[nhits++] = ap;

                if (nhits >= MAXHITS) {
                        uerror("nhits (%u) >= MAXHITS (%d)",
                                nhits, MAXHITS);
                        break;
                }
            }
        }                               /* ACCEPT entries loop */
    }                                   /* ACCEPT list exists */

    prodClass = new_prod_class(nhits);  /* nhits may be 0 */

    if (prodClass == NULL) {
        error = ENOMEM;
    }
    else {
        int       ii;

        prodClass->from = offerd->from;
        prodClass->to = offerd->to;

        /*
         * Iterate over ACCEPT entries with matching identifiers.
         */
        for (ii = 0; ii < nhits; ii++) {
            int         jj;
            feedtypet   fi = NONE;
            char        s1[255];

            /*
             * Find first offered feedtype/pattern element whose feedtype
             * intersects the ACCEPT entry's.
             */
            for (jj = 0; jj < offerd->psa.psa_len; jj++) {
                fi = offerd->psa.psa_val[jj].feedtype & hits[ii]->ft;
                if (fi)
                    break;
            }

            prodClass->psa.psa_val[ii].feedtype = fi;

            if (ulogIsDebug())
                (void)sprint_feedtypet(s1, sizeof(s1), hits[ii]->ft);

            if (NONE == fi) {
                udebug("miss %s", s1);
            }
            else {
                if (ulogIsDebug()) {
                    char s2[255], s3[255];

                    (void)sprint_feedtypet(s2, sizeof(s2),
                        offerd->psa.psa_val[jj].feedtype);
                    (void)sprint_feedtypet(s3, sizeof(s3), fi);
                    udebug("hit %s = %s & %s", s3, s1, s2);
                    udebug("    %s was %s", hits[ii]->pattern,
                         offerd->psa.psa_val[jj].pattern);
                }

                prodClass->psa.psa_val[ii].pattern =
                    strdup((char *)hits[ii]->pattern);

                if (prodClass->psa.psa_val[ii].pattern == NULL)
                    error = ENOMEM;
            }                           /* offered feedtype intersects ACCEPT */

            if (error)
                break;
        }                               /* matching ACCEPT entries loop */

        if (error) {
            free_prod_class(prodClass);
            prodClass = NULL;
        }
        else {
            prodClass->psa.psa_len = (u_int)ii;

            clss_scrunch(prodClass);
            clss_regcomp(prodClass);
        }
    }                                   /* "prodClass" allocated */

    if (!error) {
        *accept = prodClass;
        *isPrimary = 1;                 /* always use primary mode for HIYA-s */
    }

    return error;
}

/**
 * Starts the necessary downstream LDM-s.
 *
 * @param ldmPort       [in] Ignored.
 * @retval 0            Success.
 * @return              System error code. log_add() called.
 */
int
lcf_startRequesters(
    unsigned    ldmPort)
{
    return subs_startRequesters();
}


/**
 * Indicates if a given host is allowed to connect in any fashion. First line
 * of (weak) defense.
 * <p>
 * Of course, a serious threat would spoof the IP address or name service.
 *
 * @retval 0        Iff the host is not allowed to connect.
 */
int
lcf_isHostOk(const peer_info *rmtip)
{
        /* TODO: Does this need to be more efficient ? */
        AcceptEntry*    acceptEntry;
        AllowEntry*     allowEntry;

        for (allowEntry = allowEntryHead;
            allowEntry != NULL;
            allowEntry = allowEntry->next)
        {
                if(host_set_match(rmtip, allowEntry->hsp))
                        return 1;
        }
        for (acceptEntry = acceptEntries;
            acceptEntry != NULL;
            acceptEntry = acceptEntry->next)
        {
                if(host_set_match(rmtip, acceptEntry->hsp))
                        return 1;
        }
        return 0;
}

/**
 * Indicates whether or not a top-level LDM server is needed based on the
 * entries of the LDM configuration-file.
 *
 * @retval false  Server is not needed.
 * @retval true   Server is needed.
 */
bool
lcf_isServerNeeded(void)
{
    return serverNeeded;
}

/**
 * Frees this module's resources. Idempotent.
 */
void
lcf_free(void)
{
    servers_free();
    subs_free();
    allowEntries_free();
    acceptEntries_free();
    serverNeeded = false;
}


/**
 * Saves information on the last, successfully-received product under a key
 * that comprises the relevant components of the data-request.
 */
void
lcf_savePreviousProdInfo(void)
{
    const prod_info*    info = savedInfo_get();

    if (info != NULL && statePath[0] != 0) {
        FILE*   file;
        char    tmpStatePath[_POSIX_PATH_MAX];

        (void)strcpy(tmpStatePath, statePath);
        (void)strcat(tmpStatePath, ".tmp");

        file = fopen(tmpStatePath, "w");

        if (file == NULL) {
            serror("savePreviousProdInfo(): Couldn't open \"%s\" for writing",
                tmpStatePath);
        }
        else {
            if (fputs(
"# The following is the product-information of the last,\n"
"# successfully-received data-product.  Do not modify it unless\n"
"# you know exactly what you're doing!\n", file) < 0) {
                log_errno();
                log_add("savePreviousProdInfo(): "
                    "Couldn't write comment to \"%s\"", tmpStatePath);
            }
            else {
                if (pi_print(info, file) < 0 || fputc('\n', file) == EOF) {
                    log_add("Couldn't write product-information to \"%s\"",
                        tmpStatePath);
                }
                else {
                    if (fclose(file) != 0) {
                        serror("savePreviousProdInfo(): Error closing \"%s\"",
                            tmpStatePath);
                    }
                    else if (rename(tmpStatePath, statePath) == -1) {
                        serror("savePreviousProdInfo(): Couldn't rename "
                            "\"%s\" to \"%s\"",
                            tmpStatePath, statePath);
                    }

                    file = NULL;
                }                       /* product-information written */
            }                           /* comment written */

            if (file != NULL) {
                (void)fclose(file);
                (void)unlink(tmpStatePath);
            }
        }                               /* "file" open */
    }                                   /* info != NULL && statePath[0] */
}
