/****************************************************************
 *								*
 * Copyright (c) 2001-2018 Fidelity National Information	*
 * Services, Inc. and/or its subsidiaries. All rights reserved.	*
 *								*
 * Copyright (c) 2018-2024 YottaDB LLC and/or its subsidiaries.	*
 * All rights reserved.						*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_ipc.h"
#include "gtm_unistd.h"
#include "gtm_string.h"
#include "gtm_stdlib.h"
#include "gtm_fcntl.h"
#include "gtm_stdio.h"
#include "gtm_stat.h"
#include "gtm_signal.h" /* for kill(), SIGTERM, SIGQUIT */

#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/time.h>
#include <errno.h>

#include "gtm_sem.h"
#include "gdsroot.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsblk.h"
#include "gdsfhead.h"
#include "filestruct.h"
#include "gtm_c_stack_trace.h"
#include "eintr_wrapper_semop.h"
#include "eintr_wrappers.h"
#include "mu_rndwn_file.h"
#include "error.h"
#include "io.h"
#include "gt_timer.h"
#include "iosp.h"
#include "gtmio.h"
#include "gtmimagename.h"
#include "do_semop.h"
#include "ipcrmid.h"
#include "gtmmsg.h"
#include "util.h"
#include "repl_sem.h"
#include "jnl.h"
#include "repl_msg.h"
#include "gtmsource.h"
#include "gtmrecv.h"
#include "gtm_semutils.h"
#include "ftok_sems.h"
#include "wbox_test_init.h"

GBLREF	gd_region		*ftok_sem_reg;

error_def(ERR_CRITSEMFAIL);
error_def(ERR_FTOKERR);
error_def(ERR_MAXSEMGETRETRY);
error_def(ERR_SEMWT2LONG);

#define RETURN_SUCCESS(REG)												\
{															\
	ftok_sem_reg = REG;												\
	udi->grabbed_ftok_sem = TRUE;											\
	return TRUE;													\
}

boolean_t ftok_sem_get_common(gd_region *reg, boolean_t incr_cnt, int project_id, boolean_t immediate, boolean_t *stacktrace_time,
				boolean_t *timedout, semwait_status_t *retstat, boolean_t *bypass, boolean_t *ftok_counter_halted)
{
	int			status = SS_NORMAL, save_errno;
	int			ftok_sopcnt, sem_pid;
	uint4			lcnt, loopcnt;
	unix_db_info		*udi;
	union semun		semarg;
	sgmnt_addrs             *csa;
	node_local_ptr_t        cnl;
	boolean_t		shared_mem_available, sem_known_removed;
	int4			lcl_ftok_ops_index;
	key_t			ftokid;
	struct sembuf		ftok_sop[3];
	char			*msgstr;

	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	udi = FILE_INFO(reg);
	assert(!udi->grabbed_ftok_sem && !udi->grabbed_access_sem);
	assert(NULL == ftok_sem_reg);
	if (-1 == (udi->key = FTOK(udi->fn, project_id)))
		RETURN_SEMWAIT_FAILURE(retstat, errno, op_ftok, 0, ERR_FTOKERR, 0);
	/* First try is always IPC_NOWAIT */
	SET_YDB_SOP_ARRAY(ftok_sop, ftok_sopcnt, incr_cnt, (SEM_UNDO | IPC_NOWAIT));
	/* The following loop deals with the possibility that the semaphores can be deleted by someone else AFTER a successful
	 * semget but BEFORE semop locks it, in which case we should retry.
	 */
	*ftok_counter_halted = FALSE;
	sem_known_removed = FALSE;
	for (lcnt = 0; MAX_SEMGET_RETRIES > lcnt; lcnt++)
	{	/* Try to find an existing sem if we haven't already discovered that there isn't one. */
		if (!sem_known_removed
			&& (INVALID_SEMID == (ftokid = udi->ftok_semid = semget(udi->key, FTOK_SEM_PER_ID, RWDALL))))
		{
			save_errno = errno;
			if (ENOENT == save_errno)
				sem_known_removed = TRUE;
			else
				RETURN_SEMWAIT_FAILURE(retstat, save_errno, op_semget, 0, ERR_CRITSEMFAIL, 0);
		}
		/* If we found there is no sem, create and initialize one. */
		if (sem_known_removed)
		{
			/* Note assignment in next line */
			ftokid = udi->ftok_semid = semget(udi->key, FTOK_SEM_PER_ID, RWDALL | IPC_CREAT | IPC_EXCL);
			if (INVALID_SEMID == ftokid)
			{
				if (EEXIST == errno)
				{	/* If it now exists (was created by another process since our previous semget() call)
					 * then cycle back around so we pick it up without trying to recreate it.
					 */
					sem_known_removed = FALSE;
					continue;
				}
				save_errno = errno;
				RETURN_SEMWAIT_FAILURE(retstat, save_errno, op_semget, 0, ERR_CRITSEMFAIL, 0);
			}
			sem_known_removed = FALSE;
			udi->ftok_sem_created = TRUE;
			SET_GTM_ID_SEM(ftokid, status); /* Set 3rd semaphore's value to GTM_ID = 43 */
			if (-1 == status)
			{
				save_errno = errno;
				if (SEM_REMOVED(save_errno))
				{	/* start afresh for next iteration of for loop with new semid and initial operations */
					*ftok_counter_halted = FALSE;
					SET_YDB_SOP_ARRAY(ftok_sop, ftok_sopcnt, incr_cnt, (SEM_UNDO | IPC_NOWAIT));
					sem_known_removed = TRUE;
					continue;
				}
				RETURN_SEMWAIT_FAILURE(retstat, save_errno, op_semctl, 0, ERR_CRITSEMFAIL, 0);
			}
		}
		/* First try is always non-blocking */
		SEMOP(ftokid, ftok_sop, ftok_sopcnt, status, NO_WAIT);
		if (-1 != status)
		{
			udi->counter_ftok_incremented = (FTOK_SOPCNT_NO_INCR_COUNTER != ftok_sopcnt);
			/* Input parameter *bypass could be OK_TO_BYPASS_FALSE or OK_TO_BYPASS_TRUE (for "do_blocking_semop" call).
			 * But if we are returning without going that path, reset "*bypass" to reflect no bypass happened.
			 */
			*bypass = FALSE;
			RETURN_SUCCESS(reg);
		}
		save_errno = errno;
		assert(EINTR != save_errno);
		if (ERANGE == save_errno)
		{	/* We have no access to file header to check so just assume qdbrundown is set in the file header.
			 * If it turns out to be FALSE, after we read the file header, we will issue an error
			 */
			assert(!*ftok_counter_halted);
			*ftok_counter_halted = TRUE;
			ftok_sopcnt = FTOK_SOPCNT_NO_INCR_COUNTER; /* Ignore increment operation */
			lcnt--; /* Do not count this attempt */
			continue;
		}
		if (immediate)
			RETURN_SEMWAIT_FAILURE(retstat, save_errno, op_semop, 0, ERR_CRITSEMFAIL, 0);
		if (EAGAIN == save_errno)
		{	/* someone else is holding it */
			if (NO_SEMWAIT_ON_EAGAIN == TREF(dbinit_max_delta_secs))
			{
				sem_pid = semctl(ftokid, DB_CONTROL_SEM, GETPID);
				if (-1 != sem_pid)
					RETURN_SEMWAIT_FAILURE(retstat, 0, op_invalid_sem_syscall, ERR_SEMWT2LONG, 0, sem_pid);
				save_errno = errno; /* fall-through */
			} else if (do_blocking_semop(ftokid, gtm_ftok_sem, stacktrace_time, timedout, retstat, reg, bypass,
						     ftok_counter_halted, incr_cnt))
			{	/* ftok_counter_halted and bypass set by "do_blocking_semop" */
				udi->counter_ftok_incremented = incr_cnt && !(*ftok_counter_halted);
				if (*bypass)
					return TRUE;
				else
					RETURN_SUCCESS(reg);
			} else if (!SEM_REMOVED(retstat->save_errno))
				return FALSE; /* retstat will already have the necessary error information */
			else
				save_errno = retstat->save_errno;
			/* At this point "save_errno" is guaranteed to hold the errno detail across all "if/else" blocks above */
		}
		if (SEM_REMOVED(save_errno))
		{	/* start afresh for next iteration of for loop with new semid and ftok_sopcnt */
			*ftok_counter_halted = FALSE;
			SET_YDB_SOP_ARRAY(ftok_sop, ftok_sopcnt, incr_cnt, (SEM_UNDO | IPC_NOWAIT));
			sem_known_removed = TRUE;
			continue;
		}
		assert(EINTR != save_errno);
		RETURN_SEMWAIT_FAILURE(retstat, save_errno, op_semctl_or_semop, 0, ERR_CRITSEMFAIL, 0);
	}
	assert(FALSE);
	RETURN_SEMWAIT_FAILURE(retstat, 0, op_invalid_sem_syscall, 0, ERR_MAXSEMGETRETRY, 0);
}
