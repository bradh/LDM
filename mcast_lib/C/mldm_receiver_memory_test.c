/**
 * Copyright 2014 University Corporation for Atmospheric Research. All rights
 * reserved. See the the file COPYRIGHT in the top-level source-directory for
 * licensing conditions.
 *
 *   @file: mldm_receiver_memory_test.c
 * @author: Steven R. Emmerson
 *
 * This file performs a unit-test of the mldm_receiver_memory module.
 */

#include "config.h"

#include "globals_stub.h"
#include "inetutil.h"
#include "ldm.h"
#include "log.h"
#include "mldm_receiver_memory.h"

#include <errno.h>
#include <libgen.h>
#include <opmock.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef __BASE_FILE__
    #define __BASE_FILE__ "BASE_FILE_REPLACEMENT" // needed due to OpMock
#endif

static const char* const     MCAST_FEEDTYPE = ANY;
static const char* const     HOSTNAME = "hostname";
static const unsigned short  PORT = 38800;
static ServiceAddr*          SERVICE_ADDR;
static const char*           CWD;

static void init()
{
    static bool initialized;

    if (!initialized) {
        char buf[265];
        int status = sa_new(&SERVICE_ADDR, HOSTNAME, PORT);

        OP_ASSERT_TRUE(0 == status);
        OP_ASSERT_TRUE(SERVICE_ADDR != NULL);
        CWD = getcwd(buf, sizeof(buf));
        OP_ASSERT_TRUE(CWD != NULL);
        initialized = true;
    }
}

static void openMsm(
    McastSessionMemory** msm)
{
    getLdmLogDir_ExpectAndReturn(CWD);
    *msm = msm_open(SERVICE_ADDR, MCAST_FEEDTYPE);
    log_log(LOG_ERR);
    OP_ASSERT_TRUE(*msm != NULL);
}

static void test_missed_mcast_files()
{
    McastSessionMemory* msm;
    int                 status;
    VcmtpProdIndex      iProd;

    openMsm(&msm);
    msm_clearAllMissedFiles(msm);

    status = msm_getAnyMissedFileNoWait(msm, &iProd);
    log_log(LOG_ERR);
    OP_ASSERT_FALSE(status);

    status = msm_addMissedFile(msm, 1);
    OP_ASSERT_TRUE(status);
    status = msm_addMissedFile(msm, 2);
    OP_ASSERT_TRUE(status);
    status = msm_addMissedFile(msm, 3);
    OP_ASSERT_TRUE(status);

    status = msm_peekMissedFileNoWait(msm, &iProd);
    OP_ASSERT_TRUE(status);
    OP_ASSERT_TRUE(iProd == 1);

    status = msm_addRequestedFile(msm, iProd);
    OP_ASSERT_TRUE(status);

    status = msm_removeMissedFileNoWait(msm, &iProd);
    OP_ASSERT_TRUE(status);
    OP_ASSERT_TRUE(iProd == 1);

    status = msm_removeMissedFileNoWait(msm, &iProd);
    OP_ASSERT_TRUE(status);
    OP_ASSERT_TRUE(iProd == 2);

    status = msm_addRequestedFile(msm, iProd);
    OP_ASSERT_TRUE(status);

    status = msm_removeRequestedFileNoWait(msm, &iProd);
    OP_ASSERT_TRUE(status);
    OP_ASSERT_TRUE(iProd == 1);

    msm_close(msm);
    log_log(LOG_ERR);
    OP_ASSERT_TRUE(status);

    openMsm(&msm);

    status = msm_getAnyMissedFileNoWait(msm, &iProd);
    log_log(LOG_ERR);
    OP_ASSERT_TRUE(status);
    OP_ASSERT_TRUE(iProd = 2);

    status = msm_getAnyMissedFileNoWait(msm, &iProd);
    log_log(LOG_ERR);
    OP_ASSERT_TRUE(status);
    OP_ASSERT_TRUE(iProd = 3);

    status = msm_getAnyMissedFileNoWait(msm, &iProd);
    log_log(LOG_ERR);
    OP_ASSERT_FALSE(status);

    msm_close(msm);

    OP_VERIFY();
}

static void test_last_mcast_prod()
{
    McastSessionMemory* msm;
    int                 status;

    getLdmLogDir_ExpectAndReturn(CWD);
    OP_ASSERT_TRUE(msm_delete(SERVICE_ADDR, MCAST_FEEDTYPE));

    openMsm(&msm);

    signaturet sig1;
    status = msm_getLastMcastProd(msm, sig1);
    log_log(LOG_ERR);
    OP_ASSERT_FALSE(status);

    signaturet sig2;
    (void)memset(&sig2, 1, sizeof(sig2));
    status = msm_setLastMcastProd(msm, sig2);
    log_log(LOG_ERR);
    OP_ASSERT_TRUE(status);
    status = msm_getLastMcastProd(msm, sig1);
    log_log(LOG_ERR);
    OP_ASSERT_TRUE(status);
    OP_ASSERT_TRUE(memcmp(&sig1, &sig2, sizeof(sig1)) == 0);

    status = msm_close(msm);
    log_log(LOG_ERR);
    OP_ASSERT_TRUE(status);

    // Verify the data in the new file

    openMsm(&msm);

    status = msm_getLastMcastProd(msm, sig1);
    log_log(LOG_ERR);
    OP_ASSERT_TRUE(status);
    OP_ASSERT_TRUE(memcmp(&sig1, &sig2, sizeof(signaturet)) == 0);

    status = msm_close(msm);
    log_log(LOG_ERR);
    OP_ASSERT_TRUE(status);

    OP_VERIFY();
}

static void test_msm_open()
{
    McastSessionMemory* msm;

    openMsm(&msm);
    OP_ASSERT_TRUE(msm != NULL);

    OP_ASSERT_TRUE(msm_close(msm));
    log_log(LOG_ERR);

    OP_VERIFY();
}

int main(
    int		argc,
    char**	argv)
{
    (void) openulog(basename(argv[0]), LOG_NOTIME | LOG_IDENT, LOG_LDM, "-");
    (void) setulogmask(LOG_UPTO(LOG_INFO));
    opmock_test_suite_reset();
    opmock_register_test(test_msm_open, "test_msm_open");
    opmock_register_test(test_last_mcast_prod, "test_last_mcast_prod");
    opmock_register_test(test_missed_mcast_files, "test_missed_mcast_files");
    init();
    opmock_test_suite_run();
    return opmock_get_number_of_errors();
}
