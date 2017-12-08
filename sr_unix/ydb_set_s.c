/****************************************************************
 *								*
 * Copyright (c) 2017 YottaDB LLC. and/or its subsidiaries.	*
 * All rights reserved.						*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include "gtm_string.h"
#include <stdarg.h>

#include "lv_val.h"
#include "hashtab_mname.h"
#include "callg.h"
#include "op.h"
#include "error.h"
#include "nametabtyp.h"
#include "compiler.h"
#include "namelook.h"
#include "stringpool.h"
#include "libyottadb_int.h"
#include "libydberrors.h"

GBLREF 	boolean_t	gtm_startup_active;
GBLREF	symval		*curr_symval;

/* Routine to set local, global and ISV values
 *
 * Parameters:
 *   value   - Value to be set into local/global/ISV
 *   count   - Count of subscripts (if any else 0)
 *   varname - Gives name of local, global or ISV variable
 *   subscrN - a list of 0 or more ydb_buffer_t subscripts follows varname in the parm list
 */
int ydb_set_s(ydb_buffer_t *value, int count, ydb_buffer_t *varname, ...)
{
	char		chr;
	ydb_set_types	set_type;
	int		set_svn_index, i;
	lv_val		*lvvalp, *dst_lv;
	mval		set_value, gvname, plist_mvals[YDB_MAX_SUBS + 1];
	boolean_t	error_encountered;
	gparam_list	plist;
	mname_entry	var_mname;
	ht_ent_mname	*tabent;
	ydb_buffer_t	*subval;
	va_list		var;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	/* Verify entry conditions, make sure YDB CI environment is up, ESTABLISH error handler, etc */
	LIBYOTTADB_INIT(LYDB_RTN_SET);
	/* Do some validation */
	VALIDATE_VARNAME(varname, set_type, set_svn_index);
	if (0 > count)
		rts_error_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_VARNAMEINVALID);
	if (YDB_MAX_SUBS < count)
		rts_error_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_MAXNRSUBSCRIPTS);
	VALIDATE_VALUE(value);			/* Value must exist for SET */
	/* Separate actions depending on the type of SET being done */
	VAR_START(var, varname);
	switch(set_type)
	{
		case LYDB_SET_LOCAL:
			/* Set the given local variable with the given value.
			 *
			 * Note need to rebuffer EVERYTHING - the varname, subscripts and of course value are all potentially
			 * copied via saving name and address.
			 */
			FIND_BASE_VAR(varname, &var_mname, tabent, lvvalp);	/* Locate the base var lv_val */
			if (0 == count)
			{	/* We have a var with NO subscripts - simple case - just copy value pointers in and rebuffer */
				SET_LVVAL_VALUE_FROM_BUFFER(lvvalp, value);
				s2pool(&(lvvalp->v.str));		/* Rebuffer in stringpool for protection */
				break;
			}
			/* We have some subscripts - load the varname and the subscripts into our array for callg so we can drive
			 * op_putindx() to locate the mval associated with those subscripts that need to be set.
			 */
			plist.arg[0] = lvvalp;	/* First arg is lv_val of the base var */
			COPY_PARMS_TO_CALLG_BUFFER(count, TRUE);	/* Set up plist for callg invocation of op_putindx */
			dst_lv = (lv_val *)callg((callgfnptr)op_putindx, &plist);	/* Drive op_putindx to locate/create node */
			SET_LVVAL_VALUE_FROM_BUFFER(dst_lv, value);	/* Set value into located/created node */
			s2pool(&(dst_lv->v.str));			/* Rebuffer in stringpool for protection */
			RECORD_MSTR_FOR_GC(&(dst_lv->v.str));
			break;
		case LYDB_SET_GLOBAL:
			/* Set the given global variable with the given value. We do this by:
			 *   1. Drive op_gvname() with the global name and all subscripts to setup the key we need to global node.
			 *   2. Drive op_gvput() to place value into target global node
			 * Note no need to rebuffer the inputs here as they won't live in the stringpool once set.
			 */
			gvname.mvtype = MV_STR;
			gvname.str.addr = varname->buf_addr + 1;	/* Point past '^' to var name */
			gvname.str.len = varname->len_used - 1;
			plist.arg[0] = &gvname;
			COPY_PARMS_TO_CALLG_BUFFER(count, FALSE);	/* Set up plist for callg invocation of op_putindx */
			callg((callgfnptr)op_gvname, &plist);		/* Drive op_gvname() to create key */
			SET_LVVAL_VALUE_FROM_BUFFER(&set_value, value);	/* Put value to set into mval for op_gvput() */
			op_gvput(&set_value);				/* Save the global value */
			break;
		case LYDB_SET_ISV:
			/* Set the given ISV (subscripts not currently supported) with the given value.
			 *
			 * Note need to rebuffer the input value as the addr/length are directly copied in many cases.
			 */
			SET_LVVAL_VALUE_FROM_BUFFER(&set_value, value);	/* Setup mval with target value */
			s2pool(&set_value.str);				/* Rebuffer in stringpool for protection */
			RECORD_MSTR_FOR_GC(&set_value.str);
			op_svput(set_svn_index, &set_value);
			break;
		default:
			va_end(var);
			assertpro(FALSE);
	}
	va_end(var);
	TREF(sapi_mstrs_for_gc_indx) = 0;		/* These mstrs are no longer protected */
	REVERT;
	return YDB_OK;
}
