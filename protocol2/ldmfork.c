/*
 *   This file contains the function for fork(2)ing in the LDM context.
 *
 *   See file ../COPYRIGHT for copying and redistribution conditions.
 */

#include <config.h>

#include <sys/types.h>
#include <unistd.h>

#include "ldmfork.h"
#include <log.h>
#include <registry.h>

/*
 * Forks the current process in the context of the LDM.  Does whatever's
 * necessary before and after the fork to ensure correct behavior.  Terminates
 * the child process if the fork() was successful but an error occurs.
 *
 * Returns:
 *      0               Success.  The calling process is the child.
 *      -1              Failure.  "log_start()" called.
 *      else            PID of child process.  The calling process is the       
 *                      parent.
 */
pid_t ldmfork(void)
{
    pid_t       pid;

    if (reg_close()) {
        pid = -1;
    }
    else {
        pid = fork();

        if (0 == pid) {
            /* Child process */
        }
        else if (-1 == pid) {
            /* System error */
            LOG_SERROR0("Couldn't fork a child process");
        }
    }

    return pid;
}
