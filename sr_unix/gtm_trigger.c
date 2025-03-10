/****************************************************************
 *								*
 * Copyright (c) 2010-2022 Fidelity National Information	*
 * Services, Inc. and/or its subsidiaries. All rights reserved.	*
 *								*
 * Copyright (c) 2017-2025 YottaDB LLC and/or its subsidiaries. *
 * All rights reserved.						*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_stdlib.h"
#include "gtm_string.h"
#include "gtm_limits.h"
#include "gtm_stdio.h"
#include "gtm_unistd.h"

#include <errno.h>

#include "cmd_qlf.h"
#include "compiler.h"
#include "error.h"
#include "stack_frame.h"
#include "lv_val.h"
#include "mv_stent.h"
#include "gdsroot.h"
#include "gdsblk.h"
#include "gtm_facility.h"
#include "fileinfo.h"
#include "gdsbt.h"
#include "gdsfhead.h"
#include "filestruct.h"
#include "gdscc.h"
#include "gdskill.h"
#include "jnl.h"
#include "gv_trigger.h"
#include "gtm_trigger.h"
#include "trigger.h"
#include "op.h"
#include "gtmio.h"
#include "stringpool.h"
#include "alias.h"
#include "urx.h"
#include "zbreak.h"
#include "gtm_text_alloc.h"
#include "buddy_list.h"		/* needed for tp.h */
#include "tp.h"
#include "tp_frame.h"
#include "gvname_info.h"
#include "op_merge.h"
#include "golevel.h"
#include "flush_jmp.h"
#include "dollar_zlevel.h"
#include "gtmimagename.h"
#include "wbox_test_init.h"
#include "have_crit.h"
#include "srcline.h"
#include "zshow.h"
#include "zwrite.h"
#include "zr_unlink_rtn.h"
#include "gtmci.h"		/* for "gtm_levl_ret_code" prototype */

#ifdef GTM_TRIGGER
#define EMBED_SOURCE_PARM	" -EMBED_SOURCE "
#define ERROR_CAUSING_JUNK	"XX XX XX XX"
#define NEWLINE			"\n"
#define OBJECT_PARM		" -OBJECT="
#define OBJECT_FTYPE		DOTOBJ
#define NAMEOFRTN_PARM		" -NAMEOFRTN="
#define S_CUTOFF 		7
#define GTM_TRIGGER_SOURCE_NAME	"GTM Trigger"
#define MAX_MKSTEMP_RETRIES	100

GBLREF	boolean_t		run_time;
GBLREF	mv_stent		*mv_chain;
GBLREF	stack_frame		*frame_pointer;
GBLREF	uint4			trigger_name_cntr;
GBLREF	int			dollar_truth;
GBLREF	mstr			extnam_str;
GBLREF	mval			dollar_zsource;
GBLREF	unsigned char		*stackbase, *stacktop, *msp, *stackwarn;
GBLREF	symval			*curr_symval;
GBLREF	int4			gtm_trigger_depth;
GBLREF	int4			tstart_trigger_depth;
GBLREF	mstr			*dollar_ztname;
GBLREF	mval			*dollar_ztdata;
GBLREF	mval			*dollar_ztdelim;
GBLREF	mval			*dollar_ztoldval;
GBLREF	mval			*dollar_ztriggerop;
GBLREF	mval			*dollar_ztupdate;
GBLREF	mval			*dollar_ztvalue;
GBLREF	boolean_t		*ztvalue_changed_ptr;
GBLREF	rtn_tabent		*rtn_names, *rtn_names_end;
GBLREF	boolean_t		ztrap_explicit_null;
GBLREF	int			mumps_status;
GBLREF	tp_frame		*tp_pointer;
GBLREF	boolean_t		skip_dbtriggers;		/* see gbldefs.c for description of this global */
GBLREF  uint4			dollar_tlevel;
GBLREF	symval			*trigr_symval_list;
GBLREF	trans_num		local_tn;
GBLREF	int			merge_args;
GBLREF	zwr_hash_table		*zwrhtab;
#ifdef DEBUG
GBLREF	ch_ret_type		(*ch_at_trigger_init)();
GBLREF	boolean_t		donot_INVOKE_MUMTSTART;
/* For debugging purposes - since a stack unroll does not let us see past the current GTM invocation, knowing
 * what these parms are can be the determining factor in debugging an issue -- knowing what gtm_trigger() is
 * attempting. For that reason, these values are also saved/restored.
 */
GBLREF	gv_trigger_t		*gtm_trigdsc_last;
GBLREF	gtm_trigger_parms	*gtm_trigprm_last;
GBLREF	mval			dollar_ztwormhole;
#endif

LITREF	mval			literal_null;
LITREF	char			alphanumeric_table[];
LITREF	int			alphanumeric_table_len;

STATICDEF int4			gtm_trigger_comp_prev_run_time;

error_def(ERR_ASSERT);
error_def(ERR_FILENOTFND);
error_def(ERR_GTMASSERT);
error_def(ERR_GTMASSERT2);
error_def(ERR_GTMCHECK);
error_def(ERR_LABELUNKNOWN);
error_def(ERR_MAXTRIGNEST);
error_def(ERR_MEMORY);
error_def(ERR_OUTOFSPACE);
error_def(ERR_REPEATERROR);
error_def(ERR_STACKCRIT);
error_def(ERR_STACKOFLOW);
error_def(ERR_SYSCALL);
error_def(ERR_TEXT);
error_def(ERR_TPRETRY);
error_def(ERR_TRIGCOMPFAIL);
error_def(ERR_TRIGNAMEUNIQ);
error_def(ERR_TRIGTLVLCHNG);

/* Macro to re-initialize a symval block that was on a previously-used free chain */
#define REINIT_SYMVAL_BLK(svb, prev)									\
{													\
	symval	*ptr;											\
	lv_blk	*lvbp;											\
	lv_val	*lv_base, *lv_free;									\
													\
	ptr = svb;											\
	assert(NULL == ptr->xnew_var_list);								\
	assert(NULL == ptr->xnew_ref_list);								\
	reinitialize_hashtab_mname(&ptr->h_symtab);							\
	ptr->lv_flist = NULL;										\
	ptr->tp_save_all = 0;										\
	ptr->alias_activity = FALSE;									\
	ptr->last_tab = (prev);										\
	ptr->symvlvl = prev->symvlvl + 1;								\
	ptr->stack_level = dollar_zlevel() - 1;								\
	/* The lv_blk chain can remain as is but need to reinit each block so no elements are "used" */	\
	for (lvbp = ptr->lv_first_block; lvbp; lvbp = lvbp->next)					\
	{	/* Likely only one of these blocks (some few lvvals) but loop in case.. */		\
		lv_base = (lv_val *)LV_BLK_GET_BASE(lvbp);						\
		lv_free = LV_BLK_GET_FREE(lvbp, lv_base);						\
		clrlen = INTCAST((char *)lv_free - (char *)lv_base);					\
		if (0 != clrlen)									\
		{											\
			memset(lv_base, '\0', clrlen);							\
			lvbp->numUsed = 0;								\
		}											\
	}												\
}

/* All other platforms use this much faster direct return */
STATICFNDEF int gtm_trigger_invoke(void);

/* gtm_trigger - saves (some of) current environment, sets up new environment and drives a trigger.
 *
 * Triggers are one of two places where compiled M code is driven while the C stack is not at a constant level.
 * The other place that does this is call-ins. Because this M code invocation needs to be separate from other
 * running code, a new running environment is setup with its own base frame to prevent random unwinding back
 * into earlier levels. All returns from the invoked generated code come back through gtm_trigger_invoke() with
 * the exception of error handling looking for a handler or not having an error "handled" (clearing $ECODE) can
 * just keep unwinding until all trigger levels are gone.
 *
 * Trigger names:
 *
 * Triggers have a base name set by MUPIP TRIGGER in the TRIGNAME hasht entry which is read by gv_trigger.c and
 * passed to us. If it collides with an existing trigger name, we add some suffixing to it (up to two chars)
 * and create it with that name.
 *
 * Trigger compilation:
 *
 * - When a trigger is presented to us for the first time, it needs to be compiled. We do this by writing it out
 *   using a system generated unique name to a temp file and compiling it with the -NAMEOFRTN parameter which
 *   sets the name of the routine different than the unique random object name.
 * - The file is then linked in and its address recorded so the compilation only happens once.
 *
 * Trigger M stack format:
 *
 * - First created frame is a "base frame" (created by base_frame). This frame is set up to return to us
 *   (the caller) and has no backchain (old_frame_pointer is null). It also has the type SFT_TRIGR | SFT_COUNT
 *   so it is a counted frame (it is important to be counted so the mv_stents we create don't try to backup to
 *   a previous counted frame.
 * - The second created frame is for the trigger being executed. We fill in the stack_frame from the trigger
 *   description and then let it rip by calling dm_start(). When the trigger returns through the base frame
 *   which calls gtm_levl_ret_code and pulls the return address of our call to dm_start off the stack and
 *   unwinds the appropriate saved regs, it returns back to us.
 *
 * Error handling in a trigger frame:
 *
 * - $ETRAP only. $ZTRAP is forbidden. Standard rules apply.
 * - Error handling does not return to the trigger base frame but unwinds the base frame doing a rollback if
 *   necessary.
 */

CONDITION_HANDLER(gtm_trigger_complink_ch)
{	/* Condition handler for trigger compilation and link - be noisy but don't error out. Note that compilations do
	 * have their own handler but other errors are still possible. The primary use of this handler is (1) to remove
	 * the mv_stent we created and (2) most importantly to turn off the trigger_compile_and_link flag.
	 */
	START_CH(TRUE);
	TREF(trigger_compile_and_link) = FALSE;
	run_time = gtm_trigger_comp_prev_run_time;
	if (((unsigned char *)mv_chain == msp) && (MVST_MSAV == mv_chain->mv_st_type)
	    && (&dollar_zsource == mv_chain->mv_st_cont.mvs_msav.addr))
	{	/* Top mv_stent is one we pushed on there - get rid of it */
		dollar_zsource = mv_chain->mv_st_cont.mvs_msav.v;
		POP_MV_STENT();
	}
	if (DUMPABLE)
		/* Treat fatal errors thusly for a ch that may give better diagnostics */
		NEXTCH;
	if (ERR_TRIGCOMPFAIL != SIGNAL)
	{
		/* Echo error message if not general trigger compile failure message (which gtm_trigger outputs anyway */
		PRN_ERROR;
	}
	if ((SUCCESS == SEVERITY) || (INFO == SEVERITY))
	{	/* Just keep going for non-error issues */
		CONTINUE;
	}
	UNWIND(NULL, NULL);
}

CONDITION_HANDLER(gtm_trigger_ch)
{	/* Condition handler for trigger execution - This handler is pushed on first for a given trigger level, then
	 * mdb_condition_handler is pushed on so will appear multiple times as trigger depth increases. There is
	 * always an mdb_condition_handler behind us for an earlier trigger level and we let it handle severe
	 * errors for us as it gives better diagnostics (e.g. YDB_FATAL_ERROR dumps) in addition to the file core dump.
	 */
	START_CH(TRUE);	/* Note: "prev_intrpt_state" variable is defined/declared inside START_CH macro */
	DBGTRIGR((stderr, "gtm_trigger_ch: Failsafe condition cond handler entered with SIGNAL = %d\n", SIGNAL));
	if (DUMPABLE)
		/* Treat fatal errors thusly */
		NEXTCH;
	if ((SUCCESS == SEVERITY) || (INFO == SEVERITY))
	{	/* Just keep going for non-error issues */
		CONTINUE;
	}
	mumps_status = SIGNAL;
	/* We are about to no longer have a trigger stack frame and thus re-enter trigger no-mans-land */
	DEFER_INTERRUPTS(INTRPT_IN_TRIGGER_NOMANS_LAND, prev_intrpt_state);
	assert(INTRPT_OK_TO_INTERRUPT == prev_intrpt_state); /* relied upon by ENABLE_INTERRUPTS in "gtm_trigger_invoke" */
	gtm_trigger_depth--;	/* Bypassing gtm_trigger_invoke() so do maint on depth indicator */
	assert(0 <= gtm_trigger_depth);
	/* Return back to gtm_trigger with error code */
	UNWIND(NULL, NULL);
}

STATICFNDEF int gtm_trigger_invoke(void)
{	/* Invoke trigger M routine. Separate so error returns to gtm_trigger with proper retcode */
	int		rc;
	intrpt_state_t	prev_intrpt_state;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	ESTABLISH_RET(gtm_trigger_ch, mumps_status);
	gtm_trigger_depth++;
	DBGTRIGR((stderr, "gtm_trigger: Dispatching trigger at depth %d\n", gtm_trigger_depth));
	assert(0 < gtm_trigger_depth);
	assert(GTM_TRIGGER_DEPTH_MAX >= gtm_trigger_depth);
	/* Allow interrupts to occur while the trigger is running.
	 * Normally we would have the new state stored in "prev_intrpt_state" but that is not possible here because
	 * the corresponding DEFER_INTERRUPTS happened in "gtm_trigger" or a different call to "gtm_trigger_invoke"
	 * (in both cases, a different function) so we have an assert there that the previous state was INTRPT_OK_TO_INTERRUPT
	 * and use that instead of prev_intrpt_state here.
	 */
	ENABLE_INTERRUPTS(INTRPT_IN_TRIGGER_NOMANS_LAND, INTRPT_OK_TO_INTERRUPT);
	rc = dm_start();
	/* Now that we no longer have a trigger stack frame, we are back in trigger no-mans-land */
	DEFER_INTERRUPTS(INTRPT_IN_TRIGGER_NOMANS_LAND, prev_intrpt_state);
	assert(INTRPT_OK_TO_INTERRUPT == prev_intrpt_state); /* relied upon by ENABLE_INTERRUPTS in "gtm_trigger_invoke" above */
	gtm_trigger_depth--;
	DBGTRIGR((stderr, "gtm_trigger: Trigger returns with rc %d\n", rc));
	REVERT;
	assert(frame_pointer->type & SFT_TRIGR);
	assert(0 <= gtm_trigger_depth);
	CHECKHIGHBOUND(ctxt);
	CHECKLOWBOUND(ctxt);
	CHECKHIGHBOUND(active_ch);
	CHECKLOWBOUND(active_ch);
	return rc;
}

int gtm_trigger_complink(gv_trigger_t *trigdsc, boolean_t dolink)
{
	char		rtnname[YDB_PATH_MAX], rtnname_template[YDB_PATH_MAX];
	char		objname[YDB_PATH_MAX];
	char		zcomp_parms[(YDB_PATH_MAX * 2) + SIZEOF(mident_fixed) + SIZEOF(OBJECT_PARM) + SIZEOF(NAMEOFRTN_PARM)
				    + SIZEOF(EMBED_SOURCE_PARM)];
	mstr		save_zsource;
	int		rtnfd, rc, lenobjname, len, retry, save_errno, urc;
	char		*error_desc, *mident_suffix_p1, *mident_suffix_p2, *mident_suffix_top, *namesub1, *namesub2;
	char		*zcomp_parms_ptr, *zcomp_parms_top;
	mval		zlfile, zcompprm;
	int		unlink_ret;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	DBGTRIGR_ONLY(memcpy(rtnname, trigdsc->rtn_desc.rt_name.addr, trigdsc->rtn_desc.rt_name.len));
	DBGTRIGR_ONLY(rtnname[trigdsc->rtn_desc.rt_name.len] = 0);
	DBGTRIGR((stderr, "gtm_trigger_complink: (Re)compiling trigger %s\n", rtnname));
	ESTABLISH_RET(gtm_trigger_complink_ch, ((0 == error_condition) ? TREF(dollar_zcstatus) : error_condition));
	 /* Verify there are 2 available chars for uniqueness */
	assert((MAX_MIDENT_LEN - TRIGGER_NAME_RESERVED_SPACE) >= (trigdsc->rtn_desc.rt_name.len));
	assert(NULL == trigdsc->rtn_desc.rt_adr);
	gtm_trigger_comp_prev_run_time = run_time;
	run_time = TRUE;	/* Required by compiler */
	/* Verify the routine name set by MUPIP TRIGGER and read by gvtr_db_read_hasht() is not in use */
	if (NULL != find_rtn_hdr(&trigdsc->rtn_desc.rt_name))
	{	/* Ooops .. need name to be more unique.. */
		namesub1 = trigdsc->rtn_desc.rt_name.addr + trigdsc->rtn_desc.rt_name.len++;
		mident_suffix_top = (char *)alphanumeric_table + alphanumeric_table_len;
		/* Phase 1. See if any single character can add uniqueness */
		for (mident_suffix_p1 = (char *)alphanumeric_table; mident_suffix_p1 < mident_suffix_top; mident_suffix_p1++)
		{
			*namesub1 = *mident_suffix_p1;
			if (NULL == find_rtn_hdr(&trigdsc->rtn_desc.rt_name))
				break;
		}
		if (mident_suffix_p1 == mident_suffix_top)
		{	/* Phase 2. Phase 1 could not find uniqueness .. Find it with 2 char variations */
			namesub2 = trigdsc->rtn_desc.rt_name.addr + trigdsc->rtn_desc.rt_name.len++;
			for (mident_suffix_p1 = (char *)alphanumeric_table; mident_suffix_p1 < mident_suffix_top;
			     mident_suffix_p1++)
			{	/* First char loop */
				for (mident_suffix_p2 = (char *)alphanumeric_table; mident_suffix_p2 < mident_suffix_top;
				     mident_suffix_p2++)
				{	/* 2nd char loop */
					*namesub1 = *mident_suffix_p1;
					*namesub2 = *mident_suffix_p2;
					if (NULL == find_rtn_hdr(&trigdsc->rtn_desc.rt_name))
					{
						mident_suffix_p1 = mident_suffix_top + 1;	/* Break out of both loops */
						break;
					}
				}
			}
			if (mident_suffix_p1 == mident_suffix_top)
			{	/* Phase 3: Punt */
				assert(WBTEST_HELPOUT_TRIGNAMEUNIQ == ydb_white_box_test_case_number);
				RTS_ERROR_CSA_ABT(cs_addrs, VARLSTCNT(5) ERR_TRIGNAMEUNIQ, 3,
						trigdsc->rtn_desc.rt_name.len - 2, trigdsc->rtn_desc.rt_name.addr,
						((alphanumeric_table_len + 1) * alphanumeric_table_len) + 1);
			}
		}
	}
	/* Write trigger execute string out to temporary file and compile it */
	assert(MAX_XECUTE_LEN >= trigdsc->xecute_str.str.len);
	assert(GTM_PATH_MAX >= ((TREF(gtm_tmpdir)).len + SIZEOF("/trgtmpXXXXXX") + 1));
	rc = SNPRINTF(rtnname_template, GTM_PATH_MAX, "%.*s/trgtmpXXXXXX", (TREF(gtm_tmpdir)).len, (TREF(gtm_tmpdir)).addr);
	assert(0 < rc);					/* Note rc is return code aka length - we expect a non-zero length */
	PRO_ONLY(UNUSED(rc));
	/* The mkstemp() routine is known to bogus-fail for no apparent reason at all especially on AIX 6.1. In the event
	 * this shortcoming plagues other platforms as well, we add a low-cost retry wrapper.
	 */
	retry = MAX_MKSTEMP_RETRIES;
	do
	{
		strcpy(rtnname, rtnname_template);
		MKSTEMP(rtnname, rtnfd);
	} while ((-1 == rtnfd) && (EEXIST == errno) && (0 < --retry));
	if (-1 == rtnfd)
	{
		save_errno = errno;
		error_desc = STRERROR(save_errno);
		RTS_ERROR_CSA_ABT(cs_addrs, VARLSTCNT(8) ERR_FILENOTFND, 2, RTS_ERROR_TEXT(rtnname), ERR_TEXT, 2,
			LEN_AND_STR(error_desc));
	}
	assert(0 < rtnfd);	/* Verify file descriptor */
#	ifdef GEN_TRIGCOMPFAIL_ERROR
	{	/* Used ONLY to generate an error in a trigger compile by adding some junk in a previous line */
		DOWRITERC(rtnfd, ERROR_CAUSING_JUNK, strlen(ERROR_CAUSING_JUNK), rc); /* BYPASSOK */
		if (0 != rc)
		{
			urc = UNLINK(rtnname);
			assert(0 == urc);
			RTS_ERROR_CSA_ABT(cs_addrs, VARLSTCNT(8) ERR_SYSCALL, 5, RTS_ERROR_LITERAL("write()"), CALLFROM, rc);
		}
	}
#	endif
	DOWRITERC(rtnfd, trigdsc->xecute_str.str.addr, trigdsc->xecute_str.str.len, rc);
	if (0 != rc)
	{
		urc = UNLINK(rtnname);
		assert(0 == urc);
		PRO_ONLY(UNUSED(urc));
		RTS_ERROR_CSA_ABT(cs_addrs, VARLSTCNT(8) ERR_SYSCALL, 5, RTS_ERROR_LITERAL("write()"), CALLFROM, rc);
	}
	if (NULL == memchr(trigdsc->xecute_str.str.addr, '\n', trigdsc->xecute_str.str.len))
	{
		DOWRITERC(rtnfd, NEWLINE, strlen(NEWLINE), rc);			/* BYPASSOK */
		if (0 != rc)
		{
			urc = UNLINK(rtnname);
			assert(0 == urc);
			PRO_ONLY(UNUSED(urc));
			RTS_ERROR_CSA_ABT(cs_addrs, VARLSTCNT(8) ERR_SYSCALL, 5, RTS_ERROR_LITERAL("write()"), CALLFROM, rc);
		}
	}
	CLOSEFILE(rtnfd, rc);
	if (0 != rc)
	{
		urc = UNLINK(rtnname);
		assert(0 == urc);
		PRO_ONLY(UNUSED(urc));
		RTS_ERROR_CSA_ABT(cs_addrs, VARLSTCNT(8) ERR_SYSCALL, 5, RTS_ERROR_LITERAL("close()"), CALLFROM, rc);
	}
	assert(MAX_MIDENT_LEN > trigdsc->rtn_desc.rt_name.len);
	zcomp_parms_ptr = zcomp_parms;
	zcomp_parms_top = zcomp_parms + SIZEOF(zcomp_parms);
	/* rt_name is not null terminated so start the compilation string like this */
	MEMCPY_LIT(zcomp_parms_ptr, NAMEOFRTN_PARM);
	zcomp_parms_ptr += STRLEN(NAMEOFRTN_PARM);
	memcpy(zcomp_parms_ptr, trigdsc->rtn_desc.rt_name.addr, trigdsc->rtn_desc.rt_name.len);
	zcomp_parms_ptr += trigdsc->rtn_desc.rt_name.len;
	len = INTCAST(zcomp_parms_ptr - zcomp_parms);
	/* Copy the rtnname to become object name while appending the null terminated OBJ file ext string */
	lenobjname = SNPRINTF(objname, GTM_PATH_MAX, "%s" OBJECT_FTYPE, rtnname);
	/* Append the remaining parameters to the compile string */
	len += SNPRINTF(zcomp_parms_ptr, zcomp_parms_top - zcomp_parms_ptr,
				" -OBJECT=%s -EMBED_SOURCE %s", objname, rtnname);
	if (SIZEOF(zcomp_parms) <= len)	/* overflow */
	{
		urc = UNLINK(rtnname);
		assert(0 == urc);
		PRO_ONLY(UNUSED(urc));
		assert(FALSE);
		rts_error_csa(CSA_ARG(cs_addrs) VARLSTCNT(5) ERR_TEXT, 2, RTS_ERROR_LITERAL("Compilation string too long"));
	}
	zcompprm.mvtype = MV_STR;
	zcompprm.str.addr = zcomp_parms;
	zcompprm.str.len = len;
	/* Backup dollar_zsource so trigger doesn't show */
	PUSH_MV_STENT(MVST_MSAV);
	mv_chain->mv_st_cont.mvs_msav.v = dollar_zsource;
	mv_chain->mv_st_cont.mvs_msav.addr = &dollar_zsource;
	TREF(trigger_compile_and_link) = TRUE;	/* Set flag so compiler knows this is a special trigger compile */
	op_zcompile(&zcompprm, TRUE);		/* Compile but don't use $ZCOMPILE qualifiers */
	TREF(trigger_compile_and_link) = FALSE;	/* compile_source_file() establishes handler so always returns */
	if (0 != TREF(dollar_zcstatus))
	{	/* Someone err'd.. */
		run_time = gtm_trigger_comp_prev_run_time;
		REVERT;
		DEBUG_ONLY(send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(5) ERR_TEXT, 2,
					RTS_ERROR_LITERAL("TRIGCOMPFAIL"), TREF(dollar_zcstatus)));
		unlink_ret = UNLINK(objname);	/* Delete the object file first since rtnname is the unique key */
#		ifdef DEBUG
		if (-1 == unlink_ret)
			send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(12) ERR_SYSCALL, 5,
						RTS_ERROR_LITERAL("unlink(objname)"), CALLFROM,
						ERR_TEXT, 2, LEN_AND_STR(objname), errno);
#		endif
		PRO_ONLY(UNUSED(unlink_ret));
		unlink_ret = UNLINK(rtnname);	/* Delete the source file */
#		ifdef DEBUG
		if (-1 == unlink_ret)
			send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(12) ERR_SYSCALL, 5,
						RTS_ERROR_LITERAL("unlink(rtnname)"), CALLFROM,
						ERR_TEXT, 2, LEN_AND_STR(rtnname), errno);
#		endif
		PRO_ONLY(UNUSED(unlink_ret));
		return ERR_TRIGCOMPFAIL;
	}
	if (dolink)
	{	/* Link is optional as MUPIP TRIGGER doesn't need link */
		zlfile.mvtype = MV_STR;
		zlfile.str.addr = objname;
		zlfile.str.len = lenobjname;
		/* Specifying literal_null for a second arg (as opposed to NULL or 0) allows us to specify
		 * linking the object file (no compilation or looking for source). The 2nd arg is parms for
		 * recompilation and is non-null in an explicit zlink which we need to emulate.
		 */
#		ifdef GEN_TRIGLINKFAIL_ERROR
		UNLINK(objname);				/* Delete object before it can be used */
#		endif
		TREF(trigger_compile_and_link) = TRUE;		/* Overload flag so we know it is a trigger link */
		op_zlink(&zlfile, (mval *)&literal_null);	/* Need cast due to "extern const" attributes */
		TREF(trigger_compile_and_link) = FALSE;		/* If doesn't return, condition handler will clear */
		/* No return here if link fails for some reason */
		trigdsc->rtn_desc.rt_adr = find_rtn_hdr(&trigdsc->rtn_desc.rt_name);
		/* Verify can find routine we just put there. Catastrophic if not */
		assertpro(NULL != trigdsc->rtn_desc.rt_adr);
		/* Replace the randomly generated source name with the constant "GTM Trigger" */
		trigdsc->rtn_desc.rt_adr->src_full_name.addr = GTM_TRIGGER_SOURCE_NAME;
		trigdsc->rtn_desc.rt_adr->src_full_name.len = STRLEN(GTM_TRIGGER_SOURCE_NAME);
		trigdsc->rtn_desc.rt_adr->trigr_handle = trigdsc;       /* Back pointer to trig def */
		/* Release trigger source field since it was compiled with -embed_source */
		assert(0 < trigdsc->xecute_str.str.len);
		free(trigdsc->xecute_str.str.addr);
		trigdsc->xecute_str.str.len = 0;
		trigdsc->xecute_str.str.addr = NULL;
	}
	if (MVST_MSAV == mv_chain->mv_st_type && &dollar_zsource == mv_chain->mv_st_cont.mvs_msav.addr)
	{       /* Top mv_stent is one we pushed on there - restore dollar_zsource and get rid of it */
		dollar_zsource = mv_chain->mv_st_cont.mvs_msav.v;
		POP_MV_STENT();
	} else
		assert(FALSE); 	/* This mv_stent should be the one we just pushed */
	/* Remove temporary files created */
	unlink_ret = UNLINK(objname);	/* Delete the object file first since rtnname is the unique key */
#	ifdef DEBUG
	if (-1 == unlink_ret)
		send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(12) ERR_SYSCALL, 5,
					RTS_ERROR_LITERAL("unlink(objname)"), CALLFROM,
					ERR_TEXT, 2, LEN_AND_STR(objname), errno);
#	endif
	PRO_ONLY(UNUSED(unlink_ret));
	unlink_ret = UNLINK(rtnname);	/* Delete the source file */
#	ifdef DEBUG
	if (-1 == unlink_ret)
		send_msg_csa(CSA_ARG(cs_addrs) VARLSTCNT(12) ERR_SYSCALL, 5,
					RTS_ERROR_LITERAL("unlink(rtnname)"), CALLFROM,
					ERR_TEXT, 2, LEN_AND_STR(rtnname), errno);
#	endif
	PRO_ONLY(UNUSED(unlink_ret));
	run_time = gtm_trigger_comp_prev_run_time;
	REVERT;
	return 0;
}

int gtm_trigger(gv_trigger_t *trigdsc, gtm_trigger_parms *trigprm)
{
	mval			*lvvalue;
	lnr_tabent		*lbl_offset_p;
	uchar_ptr_t		transfer_addr;
	lv_val			*lvval;
	mname_entry		*mne_p;
	uint4			*indx_p;
	ht_ent_mname		*tabent;
	boolean_t		added;
	int			clrlen, rc, i, unwinds;
	mval			**lvvalarray;
	mv_stent		*mv_st_ent;
	symval			*new_symval;
	uint4			dollar_tlevel_start;
	stack_frame		*fp;
	intrpt_state_t		prev_intrpt_state;
#	ifdef DEBUG
	condition_handler	*tmpctxt;
#	endif
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	assert(!skip_dbtriggers);	/* should not come here if triggers are not supposed to be invoked */
	assert(trigdsc);
	assert(trigprm);
	assert((NULL != trigdsc->rtn_desc.rt_adr) || ((MV_STR & trigdsc->xecute_str.mvtype)
						      && (0 != trigdsc->xecute_str.str.len)
						      && (NULL != trigdsc->xecute_str.str.addr)));
	assert(dollar_tlevel);
	/* Determine if trigger needs to be compiled */
	if (NULL == trigdsc->rtn_desc.rt_adr)
	{	/* No routine hdr addr exists. Need to do compile */
		if (0 != gtm_trigger_complink(trigdsc, TRUE))
		{
			PRN_ERROR;	/* Leave record of what error caused the compilation failure if any */
			RTS_ERROR_CSA_ABT(cs_addrs, VARLSTCNT(4) ERR_TRIGCOMPFAIL, 2,
				trigdsc->rtn_desc.rt_name.len - 1, trigdsc->rtn_desc.rt_name.addr);
		}
	}
	assert(trigdsc->rtn_desc.rt_adr);
	assert(trigdsc->rtn_desc.rt_adr == CURRENT_RHEAD_ADR(trigdsc->rtn_desc.rt_adr));
	/* Setup trigger environment stack frame(s) for execution */
	if (!(frame_pointer->type & SFT_TRIGR))
	{	/* Create new trigger base frame first that back-stops stack unrolling and return to us */
		if (GTM_TRIGGER_DEPTH_MAX < (gtm_trigger_depth + 1))	/* Verify we won't nest too deep */
			RTS_ERROR_CSA_ABT(cs_addrs, VARLSTCNT(3) ERR_MAXTRIGNEST, 1, GTM_TRIGGER_DEPTH_MAX);
		DBGTRIGR((stderr, "gtm_trigger: Invoking new trigger at frame_pointer 0x"lvaddr"  ctxt value: 0x"lvaddr"\n",
			  frame_pointer, ctxt));
		/* Protect against interrupts while we have only a trigger base frame on the stack */
		DEFER_INTERRUPTS(INTRPT_IN_TRIGGER_NOMANS_LAND, prev_intrpt_state);
		assert(INTRPT_OK_TO_INTERRUPT == prev_intrpt_state); /* relied upon by ENABLE_INTERRUPTS in "gtm_trigger_invoke" */
		/* The current frame invoked a trigger. We cannot return to it for a TP restart or other reason unless
		 * either the total operation (including trigger) succeeds and we unwind normally or unless the mpc is reset
		 * (like what happens in various error or restart conditions) because right now it returns to where a database
		 * command (KILL, SET or ZTRIGGER) was entered. Set flag in the frame to prevent MUM_TSTART unless the frame gets
		 * reset.
		 */
		frame_pointer->flags |= SFF_NORET_VIA_MUMTSTART;	/* Do not return to this frame via MUM_TSTART */
		DBGTRIGR((stderr, "gtm_trigger: Setting SFF_NORET_VIA_MUMTSTART in frame 0x"lvaddr"\n", frame_pointer));
		base_frame(trigdsc->rtn_desc.rt_adr);
		/* Finish base frame initialization - reset mpc/context to return to us without unwinding base frame */
		frame_pointer->type |= SFT_TRIGR;
		frame_pointer->mpc = CODE_ADDRESS(gtm_levl_ret_code);
		frame_pointer->ctxt = GTM_CONTEXT(gtm_levl_ret_code);
		/* This base stack frame is also where we save environmental info for all triggers invoked at this stack level.
		 * Subsequent triggers fired at this level in this trigger invocation need only reinitialize a few things but
		 * can avoid "the big save".
		 */
		if (NULL == trigr_symval_list)
		{	/* No available symvals for use with this trigger, create one */
			symbinit();	/* Initialize a symbol table the trigger will use */
			curr_symval->trigr_symval = TRUE;	/* Mark as trigger symval so will be saved not decommissioned */
		} else
		{	/* Trigger symval is available for reuse */
			new_symval = trigr_symval_list;
			assert(new_symval->trigr_symval);
			trigr_symval_list = new_symval->last_tab;		/* dequeue new curr_symval from list */
			REINIT_SYMVAL_BLK(new_symval, curr_symval);
			curr_symval = new_symval;
			PUSH_MV_STENT(MVST_STAB);
			mv_chain->mv_st_cont.mvs_stab = new_symval;		/* So unw_mv_ent() can requeue it for later use */
		}
		/* Push our trigger environment save mv_stent onto the chain */
		PUSH_MV_STENT(MVST_TRIGR);
		mv_st_ent = mv_chain;
		/* Initialize the mv_stent elements processed by stp_gcol which can be called by either op_gvsavtarg() or
		 * by the extnam saving code below. This initialization keeps stp_gcol - should it be called - from attempting
		 * to process unset fields filled with garbage in them as valid mstr address/length pairs.
		 */
		mv_st_ent->mv_st_cont.mvs_trigr.savtarg.str.len = 0;
		mv_st_ent->mv_st_cont.mvs_trigr.savextref.len = 0;
		mv_st_ent->mv_st_cont.mvs_trigr.dollar_etrap_save.str.len = 0;
		mv_st_ent->mv_st_cont.mvs_trigr.dollar_ztrap_save.str.len = 0;
		mv_st_ent->mv_st_cont.mvs_trigr.saved_dollar_truth = dollar_truth;
		op_gvsavtarg(&mv_st_ent->mv_st_cont.mvs_trigr.savtarg);
		if (extnam_str.len)
		{
			ENSURE_STP_FREE_SPACE(extnam_str.len);
			mv_st_ent->mv_st_cont.mvs_trigr.savextref.addr = (char *)stringpool.free;
			memcpy(mv_st_ent->mv_st_cont.mvs_trigr.savextref.addr, extnam_str.addr, extnam_str.len);
			stringpool.free += extnam_str.len;
			assert(stringpool.free <= stringpool.top);
		}
		mv_st_ent->mv_st_cont.mvs_trigr.savextref.len = extnam_str.len;
		mv_st_ent->mv_st_cont.mvs_trigr.ztname_save = dollar_ztname;
		mv_st_ent->mv_st_cont.mvs_trigr.ztdata_save = dollar_ztdata;
		mv_st_ent->mv_st_cont.mvs_trigr.ztdelim_save = dollar_ztdelim;
		mv_st_ent->mv_st_cont.mvs_trigr.ztoldval_save = dollar_ztoldval;
		mv_st_ent->mv_st_cont.mvs_trigr.ztriggerop_save = dollar_ztriggerop;
		mv_st_ent->mv_st_cont.mvs_trigr.ztupdate_save = dollar_ztupdate;
		mv_st_ent->mv_st_cont.mvs_trigr.ztvalue_save = dollar_ztvalue;
		mv_st_ent->mv_st_cont.mvs_trigr.ztvalue_changed_ptr = ztvalue_changed_ptr;
#		ifdef DEBUG
		/* In a debug process, these fields give clues of what trigger we are working on */
		mv_st_ent->mv_st_cont.mvs_trigr.gtm_trigdsc_last_save = trigdsc;
		mv_st_ent->mv_st_cont.mvs_trigr.gtm_trigprm_last_save = trigprm;
#		endif
		/* If this is a spanning node or spanning region update, a spanning node/region condition handler may be ahead.
		 * However, the handler just behind it should be mdb_condition_handler or ch_at_trigger_init or ydb_simpleapi_ch.
		 */
		assert(((0 == gtm_trigger_depth)
				&& ((&ydb_simpleapi_ch == ctxt->ch)
					|| (ch_at_trigger_init == ctxt->ch)
					|| (((ch_at_trigger_init == (ctxt - 1)->ch) || (&ydb_simpleapi_ch == (ctxt - 1)->ch))
							&& ((&gvcst_put_ch == ctxt->ch) || (&gvcst_kill_ch == ctxt->ch)
								|| (&gvcst_spr_kill_ch == ctxt->ch)))))
			|| ((0 < gtm_trigger_depth)
				&& (((&mdb_condition_handler == ctxt->ch)
					|| (((&mdb_condition_handler == (ctxt - 1)->ch) || (&ydb_simpleapi_ch == (ctxt - 1)->ch))
						&& ((&gvcst_put_ch == ctxt->ch) || (&gvcst_kill_ch == ctxt->ch)
							|| (&gvcst_spr_kill_ch == ctxt->ch)))))));
		mv_st_ent->mv_st_cont.mvs_trigr.ctxt_save = ctxt;
		mv_st_ent->mv_st_cont.mvs_trigr.gtm_trigger_depth_save = gtm_trigger_depth;
		if (0 == gtm_trigger_depth)
		{	/* Only back up $*trap settings when initiating the first trigger level */
			mv_st_ent->mv_st_cont.mvs_trigr.dollar_etrap_save = TREF(dollar_etrap);
			mv_st_ent->mv_st_cont.mvs_trigr.dollar_ztrap_save = TREF(dollar_ztrap);
			mv_st_ent->mv_st_cont.mvs_trigr.ztrap_explicit_null_save = ztrap_explicit_null;
			(TREF(dollar_ztrap)).str.len = 0;
			ztrap_explicit_null = FALSE;
			if (NULL != (TREF(ydb_trigger_etrap)).str.addr)
				/* An etrap was defined for the trigger environment - Else existing $etrap persists */
				TREF(dollar_etrap) = TREF(ydb_trigger_etrap);
		}
		mv_st_ent->mv_st_cont.mvs_trigr.mumps_status_save = mumps_status;
		mv_st_ent->mv_st_cont.mvs_trigr.run_time_save = run_time;
		/* See if a MERGE launched the trigger. If yes, save some state so ZWRITE, ZSHOW and/or MERGE can be
		 * run in the trigger we dispatch. */
		PUSH_MVST_MRGZWRSV_IF_NEEDED;
		run_time = TRUE;	/* Previous value saved just above restored when frame pops */
	} else
	{	/* Trigger base frame exists so reinitialize the symbol table for new trigger invocation */
		REINIT_SYMVAL_BLK(curr_symval, curr_symval->last_tab);
		/* Locate the MVST_TRIGR mv_stent containing the backed up values. Some of those values need
		 * to be restored so the 2nd trigger has the same environment as the previous trigger at this level
		 */
		for (mv_st_ent = mv_chain;
		     (NULL != mv_st_ent) && (MVST_TRIGR != mv_st_ent->mv_st_type);
		     mv_st_ent = (mv_stent *)(mv_st_ent->mv_st_next + (char *)mv_st_ent))
			;
		assert(NULL != mv_st_ent);
		assert((char *)mv_st_ent < (char *)frame_pointer); /* Ensure mv_stent associated this trigger frame */
		/* Reinit backed up values from the trigger environment backup */
		dollar_truth = mv_st_ent->mv_st_cont.mvs_trigr.saved_dollar_truth;
		op_gvrectarg(&mv_st_ent->mv_st_cont.mvs_trigr.savtarg);
		extnam_str.len = mv_st_ent->mv_st_cont.mvs_trigr.savextref.len;
		if (extnam_str.len)
		{
			assert(0 < extnam_str.len);
			/* It should be safe to free extnam_str.addr, which always looks to come from malloc(), and explicitly
			   and visibly malloc to the correct len here */
#ifdef STATIC_ANALYSIS
			free(extnam_str.addr);
			extnam_str.addr = (char *) malloc(extnam_str.len);
#endif
			assert(extnam_str.addr);
			assert(mv_st_ent->mv_st_cont.mvs_trigr.savextref.addr);
			memcpy(extnam_str.addr, mv_st_ent->mv_st_cont.mvs_trigr.savextref.addr, extnam_str.len);
		}
		assert(run_time);
		/* Note we do not reset the handlers for parallel triggers - set one time only when enter first level
		 * trigger. After that, whatever happens in trigger world, stays in trigger world.
		 */
	}
	mumps_status = 0;			/* Reset later by dm_start - this is just clearing any setting that
						 * is irrelevant in the trigger frame.
						 */
	assert(frame_pointer->type & SFT_TRIGR);
#	ifdef DEBUG
	gtm_trigdsc_last = trigdsc;
	gtm_trigprm_last = trigprm;
#	endif
	/* Set new value of trigger ISVs. Previous values already saved in trigger base frame */
	dollar_ztname = &trigdsc->rtn_desc.rt_name;
	dollar_ztdata = (mval *)trigprm->ztdata_new;
	dollar_ztdelim = (mval *)trigprm->ztdelim_new;
	dollar_ztoldval = trigprm->ztoldval_new;
	dollar_ztriggerop = (mval *)trigprm->ztriggerop_new;
	dollar_ztupdate = trigprm->ztupdate_new;
	dollar_ztvalue = trigprm->ztvalue_new;
	ztvalue_changed_ptr = &trigprm->ztvalue_changed;
	/* Set values associated with trigger into symbol table */
	lvvalarray = trigprm->lvvalarray;
	for (i = 0, mne_p = trigdsc->lvnamearray, indx_p = trigdsc->lvindexarray;
	     i < trigdsc->numlvsubs; ++i, ++mne_p, ++indx_p)
	{	/* Once thru for each subscript we are to set */
		lvvalue = lvvalarray[*indx_p];			/* Locate mval that contains value */
		assert(NULL != lvvalue);
		assert(MV_DEFINED(lvvalue));			/* No sense in defining the undefined */
		lvval = lv_getslot(curr_symval);		/* Allocate an lvval to put into symbol table */
		LVVAL_INIT(lvval, curr_symval);
		lvval->v = *lvvalue;				/* Copy mval into lvval */
		added = add_hashtab_mname_symval(&curr_symval->h_symtab, mne_p, lvval, &tabent);
		assert(added);
		PRO_ONLY(UNUSED(added));
		assert(NULL != tabent);
	}
	/* While the routine header is available in trigdsc, we also need the <null> label address associated with
	 *  the first (and only) line of code.
	 */
	lbl_offset_p = LNRTAB_ADR(trigdsc->rtn_desc.rt_adr);
	transfer_addr = (uchar_ptr_t)LINE_NUMBER_ADDR(trigdsc->rtn_desc.rt_adr, lbl_offset_p);
	/* Create new stack frame for invoked trigger in same fashion as gtm_init_env() creates its 2ndary frame */
#	ifdef HAS_LITERAL_SECT
	new_stack_frame(trigdsc->rtn_desc.rt_adr, (unsigned char *)LINKAGE_ADR(trigdsc->rtn_desc.rt_adr), transfer_addr);
#	else
	/* Any platform that does not follow pv-based linkage model either
	 *	(1) uses the following calculation to determine the context pointer value, or
	 *	(2) doesn't need a context pointer
	 */
	new_stack_frame(trigdsc->rtn_desc.rt_adr, PTEXT_ADR(trigdsc->rtn_desc.rt_adr), transfer_addr);
#	endif
	/* Earlier, we set our `stack_level` to the *current* $STACK.
	   But we just called `new_stack_frame` above, which increments the stack level. Bump the level by one. */
	curr_symval->stack_level++;
	dollar_tlevel_start = dollar_tlevel;
	assert(gv_target->gd_csa == cs_addrs);
	gv_target->trig_local_tn = local_tn;			/* Record trigger being driven for this global */
	/* Invoke trigger generated code */
	rc = gtm_trigger_invoke();
	if (1 == rc)
	{	/* Normal return code (from dm_start). Check if TP has been unwound or not */
		assert(dollar_tlevel <= dollar_tlevel_start);	/* Bigger would be quite the surprise */
		if (dollar_tlevel < dollar_tlevel_start)
		{	/* Our TP level was unwound during the trigger so throw an error */
			DBGTRIGR((stderr, "gtm_trigger: $TLEVEL less than at start - throwing TRIGTLVLCHNG\n"));
			gtm_trigger_fini(TRUE, FALSE);	/* dump this trigger level */
			RTS_ERROR_CSA_ABT(cs_addrs, VARLSTCNT(4) ERR_TRIGTLVLCHNG, 2, trigdsc->rtn_desc.rt_name.len,
				trigdsc->rtn_desc.rt_name.addr);
		}
		rc = 0;			/* Be polite and return 0 for the (hopefully common) success case */
	} else if (ERR_TPRETRY == rc)
	{	/* We are restarting the entire transaction. There are two possibilities here:
		 * 1) This is a nested trigger level in which case we need to unwind further or
		 *    the outer trigger level was created by M code. If either is true, just
		 *    rethrow the TPRETRY error.
		 * 2) This is the outer trigger and the call to op_tstart() was done by our caller.
		 *    In this case, we just return to our caller with a code signifying they need
		 *    to restart the implied transaction.
		 */
		assert(dollar_tlevel && (tstart_trigger_depth <= gtm_trigger_depth));
		if ((tstart_trigger_depth < gtm_trigger_depth) || !tp_pointer->implicit_tstart || !tp_pointer->implicit_trigger)
		{	/* Unwind a trigger level to restart level or to next trigger boundary */
			gtm_trigger_fini(FALSE, FALSE);	/* Get rid of this trigger level - we won't be returning */
			DBGTRIGR((stderr, "gtm_trigger: dm_start returned rethrow code - rethrowing ERR_TPRETRY\n"));
#			ifdef DEBUG
			if ((&mdb_condition_handler == ch_at_trigger_init) && (&ydb_simpleapi_ch != ctxt->ch))
			{
				assert(&mdb_condition_handler == ctxt->ch);
				if (tp_pointer->implicit_tstart)
				{
					assert(&chnd[1] < ctxt);
					/* The bottommost mdb_condition handler better not be catching this restart
					 * if we did an implicit tstart. mdb_condition_handler will try to unwind further,
					 * and the process will inadvertently exit. Assert below that there is one more
					 * mdb_condition_handler behind the currently established handler (which we already
					 * asserted above that it is "mdb_condition_handler"). If not, there should be at least
					 * one "ydb_simpleapi_ch" behind the currently established handler. In this case, the
					 * caller who established this handler (e.g. "ydb_set_s", "ydb_get_s", "ydb_tp_s" etc.)
					 * knows how to handle a ERR_TPRETRY error code.
					 */
					for (tmpctxt = ctxt - 1; tmpctxt >= &chnd[0]; tmpctxt--)
					{
						if ((&mdb_condition_handler == tmpctxt->ch) || (&ydb_simpleapi_ch == tmpctxt->ch))
							break;
					}
					assert(tmpctxt >= &chnd[0]);
				}
			}
#			endif
			INVOKE_RESTART;
		} else
		{	/* It is possible we are restarting a transaction that never got around to creating a base
			 * frame yet the implicit TStart was done. So if there is no trigger base frame, do not
			 * run gtm_trigger_fini() but instead do the one piece of cleanup it does that we still need.
			 */
			assert(donot_INVOKE_MUMTSTART);
			if (SFT_TRIGR & frame_pointer->type)
			{	/* Normal case when TP restart unwinding back to implicit beginning */
				gtm_trigger_fini(FALSE, FALSE);
				DBGTRIGR((stderr, "gtm_trigger: dm_start returned rethrow code - returning to gvcst_<caller>\n"));
			} else
			{       /* Unusual case of trigger that died in no-mans-land before trigger base frame established.
				 * Remove the "do not return to me" flag only on non-error unwinds */
				assert(tp_pointer->implicit_tstart);
				assert(SFF_NORET_VIA_MUMTSTART & frame_pointer->flags);
				frame_pointer->flags &= SFF_NORET_VIA_MUMTSTART_OFF;
				DBGTRIGR((stderr, "gtm_trigger: turning off SFF_NORET_VIA_MUMTSTART (1) in frame 0x"lvaddr"\n",
					  frame_pointer));
				DBGTRIGR((stderr, "gtm_trigger: unwinding no-base-frame trigger for TP restart\n"));
			}
		}
		/* Fall out and return ERR_TPRETRY to caller */
	} else
	{	/* We should never get a return code of 0. This would be out-of-design and a signal that something
		 * is quite broken. We cannot "rethrow" outside the trigger because it was not initially an error so
		 * mdb_condition_handler would have no record of it (rethrown errors must have originally occurred in
		 * or to be RE-thrown) and assert fail at best.
		 */
		assertpro(0 != rc);
		/* We have an unexpected return code due to some error during execution of the trigger that tripped
		 * gtm_trigger's safety handler (i.e. an error occurred in mdb_condition_handler() established by
		 * dm_start(). Since we are going to unwind the trigger frame and rethrow the error, we also have
		 * to unwind all the stack frames on top of the trigger frame. Figure out how many frames that is,
		 * unwind them all plus the trigger base frame before rethrowing the error.
		 */
		for (unwinds = 0, fp = frame_pointer; (NULL != fp) && !(SFT_TRIGR & fp->type); fp = fp->old_frame_pointer)
			unwinds++;
		assert((NULL != fp) && (SFT_TRIGR & fp->type));
		GOFRAMES(unwinds, TRUE, FALSE);
		assert((NULL != frame_pointer) && !(SFT_TRIGR & frame_pointer->type));
		DBGTRIGR((stderr, "gtm_trigger: Unsupported return code (%d) - unwound %d frames and now rethrowing error\n",
			  rc, unwinds));
		RTS_ERROR_CSA_ABT(cs_addrs, VARLSTCNT(1) ERR_REPEATERROR);
	}
	return rc;
}

/* Unwind a trigger level, pop all the associated mv_stents and dispense with the base frame.
 * Note that this unwind restores the condition handler stack pointer and correct gtm_trigger_depth value in
 * order to maintain these values properly in the event of a major unwind. This routine is THE routine to use to unwind
 * trigger base frames in all cases due to the cleanups it takes care of.
 */
void gtm_trigger_fini(boolean_t forced_unwind, boolean_t fromzgoto)
{
	intrpt_state_t		prev_intrpt_state;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	/* Would normally be an assert but potential frame stack damage so severe and resulting debug difficulty that we
	 * assertpro() instead.
	 */
	assertpro(frame_pointer->type & SFT_TRIGR);
	DEFER_INTERRUPTS(INTRPT_IN_FRAME_POINTER_NULL, prev_intrpt_state);
	TREF(trig_forced_unwind) = forced_unwind;	/* used by "op_unwind" */
	/* Unwind the trigger base frame */
	op_unwind();
	/* Note: "frame_pointer" can be NULL at this point hence the need for the surrounding DEFER_INTERRUPTS/ENABLE_INTERRUPTS */
	assert(FALSE == TREF(trig_forced_unwind));	/* should have been reset by "op_unwind" */
	/* restore frame_pointer stored at msp (see base_frame.c) */
        frame_pointer = *(stack_frame**)msp;
	assert(NULL != frame_pointer);
	ENABLE_INTERRUPTS(INTRPT_IN_FRAME_POINTER_NULL, prev_intrpt_state);
	msp += SIZEOF(stack_frame *);           /* Remove frame save pointer from stack */
	/* Remove the "do not return to me". Note this flag may have already been turned off by an earlier tp_restart if
	 * this is not an implicit_tstart situation. Note we must ALWAYS turn this flag back off here because we're the
	 * one who set it because we could be in a state where a simpleapi call to set a variable drove a trigger. If that
	 * trigger gets an error and we don't turn this flag off, it causes error_return() to assert fail.
	 */
	assert(forced_unwind || !tp_pointer->implicit_tstart || (SFF_NORET_VIA_MUMTSTART & frame_pointer->flags));
	frame_pointer->flags &= SFF_NORET_VIA_MUMTSTART_OFF;
	DBGTRIGR((stderr, "gtm_trigger_fini: turning off SFF_NORET_VIA_MUMTSTART(2) in frame 0x"lvaddr"\n", frame_pointer));
	if (forced_unwind)
	{	/* Error unwind, make sure certain cleanups are done */
#		ifdef DEBUG
		assert(!dollar_tlevel || (tstart_trigger_depth <= gtm_trigger_depth));
		if (tstart_trigger_depth == gtm_trigger_depth) /* Unwinding gvcst_put() so get rid of flag it potentially set */
			donot_INVOKE_MUMTSTART = FALSE;
#		endif
		/* Now that op_unwind has been done, it is possible "lvzwrite_block" got restored to pre-trigger state
		 * (if it was saved off in PUSH_MVST_MRGZWRSV call done at triggerland entry). If so, given this is a
		 * forced unwind of triggerland, clear any context corresponding to MERGE/ZSHOW that caused us to enter
		 * triggerland in the first place.
		 */
		NULLIFY_MERGE_ZWRITE_CONTEXT;
		if (tp_pointer)
		{	/* This TP transaction can never be allowed to commit if this is the first trigger
			 * (see comment in tp_frame.h against "cannot_commit" field for details).
			 */
			if ((0 == gtm_trigger_depth) && !fromzgoto)
			{
				DBGTRIGR((stderr, "gtm_trigger: cannot_commit flag set to TRUE\n"))
				tp_pointer->cannot_commit = TRUE;
			}
			/* We just unrolled the implicitly started TSTART so unroll what it did. The only exception is if
			 * this TP frame was created by a call from "ydb_tp_s". In that case, it will do the rollback itself.
			 */
			if ((tp_pointer->fp == frame_pointer) && tp_pointer->implicit_tstart && !tp_pointer->ydb_tp_s_tstart)
				OP_TROLLBACK(-1);
		}
	}
	DBGTRIGR((stderr, "gtm_trigger: Unwound to trigger invoking frame: frame_pointer 0x"lvaddr"  ctxt value: 0x"lvaddr"\n",
		  frame_pointer, ctxt));
	/* Re-allow interruptions now that our base frame is gone.
	 * Normally we would have the new state stored in "prev_intrpt_state" but that is not possible here because
	 * the corresponding DEFER_INTERRUPTS happened in "gtm_trigger" or "gtm_trigger_invoke"
	 * (in both cases, a different function) so we have an assert there that the previous state was INTRPT_OK_TO_INTERRUPT
	 * and use that instead of prev_intrpt_state here.
	 */
	if (forced_unwind)
	{	/* Since we are being force-unwound, we don't know the state of things except that it it should be either
		 * the state we set it to or the ok-to-interrupt state. Assert that and if we are changing the state,
		 * be sure to run the deferred handler.
		 */
		assert((INTRPT_IN_TRIGGER_NOMANS_LAND == intrpt_ok_state) || (INTRPT_OK_TO_INTERRUPT == intrpt_ok_state));
		ENABLE_INTERRUPTS(intrpt_ok_state, INTRPT_OK_TO_INTERRUPT);
	} else
	{	/* Normal unwind should be ok with this macro */
		ENABLE_INTERRUPTS(INTRPT_IN_TRIGGER_NOMANS_LAND, INTRPT_OK_TO_INTERRUPT);
	}
}

/* Routine to eliminate the zlinked trigger code for a given trigger about to be deleted. Operations performed
 * differ depending on platform type (shared binary or not).
 */
void gtm_trigger_cleanup(gv_trigger_t *trigdsc)
{
	rtn_tabent	*mid;
	mident		*rtnname;
	rhdtyp		*rtnhdr;
	int		size;
	stack_frame	*fp;
	intrpt_state_t  prev_intrpt_state;

	/* TODO: We don't expect the trigger source to exist now gtm_trigger cleans it up ASAP. Remove it after a few releases */
	assert (0 == trigdsc->xecute_str.str.len);
	/* First thing to do is release trigger source field if it exists */
	if (0 < trigdsc->xecute_str.str.len)
	{
		free(trigdsc->xecute_str.str.addr);
		trigdsc->xecute_str.str.len = 0;
		trigdsc->xecute_str.str.addr = NULL;
	}
	/* Next thing to do is find the routine header in the rtn_names list so we can remove it. */
	rtnname = &trigdsc->rtn_desc.rt_name;
	rtnhdr = trigdsc->rtn_desc.rt_adr;
	/* Only one possible version of a trigger routine */
	assert(USHBIN_ONLY(NULL) NON_USHBIN_ONLY(rtnhdr) == OLD_RHEAD_ADR(CURRENT_RHEAD_ADR(rtnhdr)));
	/* Verify trigger routine we want to remove is not currently active. If it is, we need to assert fail.
	 * Triggers are not like regular routines since they should only ever be referenced from the stack during a
	 * transaction. Likewise, we should only ever load the triggers as the first action in that transaction.
	 */
#	ifdef DEBUG
	for (fp = frame_pointer; NULL != fp; fp = fp->old_frame_pointer)
	{
		SKIP_BASE_FRAMES(fp, (SFT_CI | SFT_TRIGR));	/* Can update fp if fp is a call-in or trigger base frame */
		assert(fp->rvector != rtnhdr);
	}
#	endif
	/* Locate the routine in the routine table while all the pieces are available. Then remove from routine table
	 * after the routine is unlinked.
	 */
	assertpro(find_rtn_tabent(&mid, rtnname));		/* Routine should be found (sets "mid" with found entry) */
	assert(rtnhdr == mid->rt_adr);
	/* Free all storage allocated on behalf of this trigger routine. Do this before removing from routine table since
	 * some of the activities called during unlink look for the routine so it must be found.
	 */
	DEFER_INTERRUPTS(INTRPT_IN_RTN_CLEANUP, prev_intrpt_state);
	zr_unlink_rtn(rtnhdr, TRUE);
	/* Remove the routine from the rtn_table */
	size = INTCAST((char *)rtn_names_end - (char *)mid);
	if (0 < size)
		memmove((char *)mid, (char *)(mid + 1), size);	/* Remove this routine name from sorted table */
	rtn_names_end--;
	ENABLE_INTERRUPTS(INTRPT_IN_RTN_CLEANUP, prev_intrpt_state);
}
#endif /* GTM_TRIGGER */
