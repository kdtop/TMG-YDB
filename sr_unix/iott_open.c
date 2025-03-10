/****************************************************************
 *								*
 * Copyright (c) 2001-2023 Fidelity National Information	*
 * Services, Inc. and/or its subsidiaries. All rights reserved.	*
 *								*
 * Copyright (c) 2018-2025 YottaDB LLC and/or its subsidiaries.	*
 * All rights reserved.						*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#include "mdef.h"

#include <errno.h>

#include "gtm_fcntl.h"
#include "gtm_string.h"
#include "gtm_stdlib.h"
#include "gtm_termios.h"

#include "io.h"
#include "iottdef.h"
#include "io_params.h"
#include "trmdef.h"
#include "gtmio.h"
#include "iott_setterm.h"
#include "stringpool.h"
#include "getcaps.h"
#include "gtm_isanlp.h"
#include "gtm_conv.h"
#include "gtmimagename.h"
#include "error.h"
#include "op.h"
#include "indir_enum.h"
#include "ydb_getenv.h"

GBLREF int		COLUMNS, GTM_LINES, AUTO_RIGHT_MARGIN;
GBLREF uint4		ydb_principal_editing_defaults;
GBLREF io_pair		io_std_device;
GBLREF	boolean_t	gtm_utf8_mode;
LITREF unsigned char	io_params_size[];

error_def(ERR_BADCHSET);
error_def(ERR_NOTERMENTRY);
error_def(ERR_NOTERMENV);
error_def(ERR_NOTERMINFODB);
error_def(ERR_TCGETATTR);
error_def(ERR_ZINTRECURSEIO);

short iott_open(io_log_name *dev_name, mval *pp, int fd, mval *mspace, uint8 timeout)
{
	unsigned char	ch;
	d_tt_struct	*tt_ptr;
	io_desc		*ioptr;
	int		status, chset_index;
	int		save_errno;
	int		p_offset;
	mstr		chset_mstr;
	gtm_chset_t	temp_chset, old_ichset;
	boolean_t	empt = FALSE;
	boolean_t	ch_set;
	boolean_t	initializing = FALSE;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	ioptr = dev_name->iod;
	ESTABLISH_RET_GTMIO_CH(&ioptr->pair, -1, ch_set);
	if (ioptr->state == dev_never_opened)
	{
		//Large block change.  Putting state into separate structures, which are not a pointers....
		//NOTE: d_tt_struct has been changed such that additional memory does not need to be allocated for ttio_struct's
		dev_name->iod->dev_sp = (void *)malloc(SIZEOF(d_tt_struct));
		memset(dev_name->iod->dev_sp, 0, SIZEOF(d_tt_struct)); // NOTE: by filling structure initially with 0's, this effects setting all booleans to FALSE. E.g. tt_ptr->io_state.canonical etc.
		tt_ptr = (d_tt_struct *)dev_name->iod->dev_sp;
		tt_ptr->in_buf_sz = TTDEF_BUF_SZ;
		tt_ptr->enbld_outofbands.x = 0;
		tt_ptr->ttybuff = (char *)malloc(IOTT_BUFF_LEN);
		ioptr->ichset = ioptr->ochset = gtm_utf8_mode ? CHSET_UTF8 : CHSET_M;	/* default */

		//Initialize ydb IO state based on current state of TTY IO subsystem.
		//loads TTY IO into tt_ptr->initial_io_state -- will later be used to restore terminal to initial state when exiting program
		iott_TTY_to_state(tt_ptr, &(tt_ptr->initial_io_state));

		initializing = TRUE;  //set up direct mode io state at the end of this function, after terminators etc initialized.

		//Start setup of device, starting io_state from initial_io_state
		//Will be further modified below.
		tt_ptr->io_state = tt_ptr->initial_io_state;
		//end mod -------------
	}
	tt_ptr = (d_tt_struct *)dev_name->iod->dev_sp;
	if (tt_ptr->mupintr)
		RTS_ERROR_CSA_ABT(NULL, VARLSTCNT(1) ERR_ZINTRECURSEIO);
	p_offset = 0;
	old_ichset = ioptr->ichset;
	iott_open_params_to_state(ioptr, pp, &(tt_ptr->io_state));  //centralizing param management to allow alternate IO states
	if (ioptr->state != dev_open)
	{
		int	status;
		char	*env_term;

		assert(fd >= 0);
		tt_ptr->fildes = fd;
		//NOTE: Moved the setting of TTY attribs to below.
		//         In iott_compile_state_and_set_tty_and_ydb_echo(), below, tt_ptr->io_state.ttio_struct will be properly set up.
		status = getcaps(tt_ptr->fildes);
		if (1 != status)
		{
			if (status == 0)
			{
				env_term = ydb_getenv(YDBENVINDX_GENERIC_TERM, NULL_SUFFIX, NULL_IS_YDB_ENV_MATCH);
				if (!env_term)
				{
					rts_error_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_NOTERMENV);
					env_term = "unknown";
				}
				rts_error_csa(CSA_ARG(NULL) VARLSTCNT(4) ERR_NOTERMENTRY, 2, LEN_AND_STR(env_term));
			} else
				rts_error_csa(CSA_ARG(NULL) VARLSTCNT(1) ERR_NOTERMINFODB);
		}
		ioptr->width = COLUMNS;
		ioptr->length = GTM_LINES;
		ioptr->wrap = (0 == AUTO_RIGHT_MARGIN) ? FALSE : TRUE; /* defensive programming; till we are absolutely, positively
									* certain that there are no uses of wrap == TRUE */
		tt_ptr->tbuffp = tt_ptr->ttybuff;	/* Buffer is now empty */
		tt_ptr->io_state.discard_lf = FALSE;
		if (!io_std_device.in || io_std_device.in == ioptr->pair.in)	/* io_std_device.in not set yet in io_init */
		{	/* $PRINCIPAL */
			tt_ptr->ext_cap = ydb_principal_editing_defaults;
			ioptr->ichset = ioptr->ochset = gtm_utf8_mode ? CHSET_UTF8 : CHSET_M;	/* default */
		} else
			tt_ptr->ext_cap = 0;
		if (empt)
			tt_ptr->ext_cap |= TT_EMPTERM;
		/* Set terminal mask on the terminal not open, if default_term or if CHSET changes */
		if (tt_ptr->io_state.default_mask_term || (old_ichset != ioptr->ichset))
		{
			iott_set_mask_term_conditional(ioptr, &(tt_ptr->io_state.mask_term), (CHSET_M != ioptr->ichset), TRUE);
		}
		ioptr->state = dev_open;
	} else
	{
		/* Set terminal mask on the already open terminal, if CHSET changes */
		if (old_ichset != ioptr->ichset)
		{
			iott_set_mask_term_conditional(ioptr, &(tt_ptr->io_state.mask_term), (CHSET_M != ioptr->ichset), TRUE);
		}
	}
	//Compile ydb state and send this to TTY IO subsystem.  8  1 will be default time and minimum read; can be changed later.
	iott_compile_state_and_set_tty_and_ydb_echo(ioptr, 8, 1, handle_set_tty_err_mode_3);
	if (initializing)
	{
		//complete setup for initial io state.
		iott_set_mask_term_conditional(ioptr, &(tt_ptr->initial_io_state.mask_term), (CHSET_M != ioptr->ichset), TRUE);

		//setup an IO state for use when interacting with user at console in direct mode.
		tt_ptr->direct_mode_io_state = tt_ptr->io_state;
		tt_ptr->direct_mode_io_state.canonical = FALSE;
		tt_ptr->direct_mode_io_state.devparam_echo = TRUE;
		iott_compile_ttio_struct(ioptr,  &(tt_ptr->direct_mode_io_state), 8, 1);   //compiles IO state into direct_mode_io_state.ttio_struct
	}

	REVERT_GTMIO_CH(&ioptr->pair, ch_set);
	return TRUE;
}

void iott_TTY_to_state(d_tt_struct * tt_ptr, ttio_state* an_io_state_ptr)
//Purpose: Set ydb IO state based on state of TTY IO subsystem, esp when first opening TTY IO device.
//
//  Why needed? Because if the TTY IO initially has echo (+) mode but no devparam specifies state of echo,
//  then ydb would leave state in default value (all 0's --> false).  Thus ydb state would not be
//  synchronized with actual TTY IO subsystem.  So will set ydb state from TTY IO first, then later
//  let ydb change anything wanted based on devparams.
//
//  Update: The code has been extended so that echo mode or noecho mode is specified as an initialization
//	    devparam, but will leave this code in because the principle of starting with the state of the
//	    TTY system still holds.
//
{
	int		status;
	struct termios	cur_IO_state;
	int		save_errno;

	status = tcgetattr(tt_ptr->fildes, &cur_IO_state);
	if (0 != status)
	{
		save_errno = errno;
		if (gtm_isanlp(tt_ptr->fildes) == 0)
			RTS_ERROR_CSA_ABT(NULL, VARLSTCNT(4) ERR_TCGETATTR, 1, tt_ptr->fildes, save_errno);
	}
	iott_tio_struct_to_state(an_io_state_ptr, &cur_IO_state);
}

void iott_tio_struct_to_state(ttio_state* an_io_state_ptr, struct termios * ttio_struct_ptr)
//Purpose: Set ydb IO state based on value of a ttio_struct_ptr
{
	an_io_state_ptr->ttio_struct = *ttio_struct_ptr;

	an_io_state_ptr->ttsync         = BIT_FLAG_IS_ON(IXON,   ttio_struct_ptr->c_iflag); //note "i" flag e.g. "I"
	an_io_state_ptr->hostsync       = BIT_FLAG_IS_ON(IXOFF,  ttio_struct_ptr->c_iflag); //note "i" flag e.g. "I"
	an_io_state_ptr->canonical      = BIT_FLAG_IS_ON(ICANON, ttio_struct_ptr->c_lflag); //note "l" flag e.g. "L"  <-- different
	an_io_state_ptr->devparam_echo  = BIT_FLAG_IS_ON(ECHO,   ttio_struct_ptr->c_lflag); //note "l" flag e.g. "L"  <-- different

	an_io_state_ptr->default_mask_term = TRUE;
}

void iott_open_params_to_state(io_desc* io_ptr, mval* devparms, ttio_state* io_state_ptr)
//NOTE: This establishes the IO state into ydb data structures, based on USE device parameters.
//      It does NOT write anything out to the TTY IO subsystem.
{
	d_tt_struct* 		tt_ptr;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	tt_ptr = (d_tt_struct *)io_ptr->dev_sp;
	if (tt_ptr->mupintr)
		RTS_ERROR_CSA_ABT(NULL, VARLSTCNT(1) ERR_ZINTRECURSEIO);
	iott_common_params_to_state(io_ptr, io_state_ptr, devparms, iott_is_valid_open_param);
}

boolean_t iott_is_valid_open_param(io_params_type aparam)   //is aparam valid for OPEN command?
{
	boolean_t	result;
	result = (	(aparam == iop_exception) 	||
			(aparam == iop_echo) 		||
			(aparam == iop_noecho) 		||
			(aparam == iop_canonical) 	||
			(aparam == iop_nocanonical)	||
			(aparam == iop_empterm)		||
			(aparam == iop_noempterm)	||
			(aparam == iop_m)		||
			(aparam == iop_utf8)		||
			(aparam == iop_ipchset)		||
			(aparam == iop_opchset)		||
			(aparam == iop_chset) 		||
			//---- below are sometimes added to devparams in io_init, but apparently not processed.
			(aparam == iop_newversion)	||
			(aparam == iop_stream)		||
			(aparam == iop_nl)		||
			(aparam == iop_shared)		||
			(aparam == iop_readonly)		);

	return result;
}
