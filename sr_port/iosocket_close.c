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

/* iosocket_close.c - close a socket connection
 *  Parameters-
 *  iod -- I/O descriptor for the current device.
 *
 *  pp -- mval that carries the device parameters
 *
 */
#include "mdef.h"

#include "gtm_string.h"
#include "gtm_iconv.h"
#include "gtm_stat.h"
#include "gtmio.h"
#include "gtm_stdio.h"
#include "gtm_unistd.h"
#include "gtm_socket.h"
#include "gtm_inet.h"

#include "copy.h"
#include "io_params.h"
#include "io.h"
#include "gt_timer.h"
#include "iosocketdef.h"
#include "stringpool.h"
#include "eintr_wrappers.h"
#ifdef GTM_TLS
#include "gtm_tls.h"
#endif
#include "error.h"
#include "op.h"
#include "indir_enum.h"

GBLREF	io_desc		*active_device;
GBLREF	int		process_exiting;
GBLREF	boolean_t	gtm_pipe_child;

LITREF unsigned char		io_params_size[];

error_def(ERR_CLOSEFAIL);
error_def(ERR_FILEOPENFAIL);
error_def(ERR_SOCKNOTFND);
error_def(ERR_SYSCALL);
error_def(ERR_TLSIOERROR);

#define	SOCKET_DELETE_FALSE	FALSE
#define	SOCKET_DELETE_TRUE	TRUE

void iosocket_close_range(d_socket_struct *dsocketptr, int start, int end, boolean_t socket_delete);

void iosocket_close(io_desc *iod, mval *pp)
{
	boolean_t	socket_specified = FALSE;
	unsigned char	ch;
	int		handle_len;
	d_socket_struct	*dsocketptr;
	char		sock_handle[MAX_HANDLE_LEN], *errp;
	int4		start, end, index;
	int		p_offset = 0;
	boolean_t	socket_destroy = FALSE;
	boolean_t	ch_set;
	DCL_THREADGBL_ACCESS;

	SETUP_THREADGBL_ACCESS;
	assert(iod->type == gtmsocket);
	dsocketptr = (d_socket_struct *)iod->dev_sp;
	assert(dsocketptr->iod == iod);
	ESTABLISH_GTMIO_CH(&iod->pair, ch_set);

	while (iop_eol != (ch = *(pp->str.addr + p_offset++)))
	{
		switch (ch)
		{
		case iop_exception:
			DEF_EXCEPTION(pp, p_offset, iod);
			break;
		case iop_socket:
			handle_len = (int)(unsigned char)*(pp->str.addr + p_offset);
			memcpy(sock_handle, (char *)(pp->str.addr + p_offset + 1), handle_len);
			socket_specified = TRUE;
			break;
		case iop_ipchset:
#			if defined(KEEP_zOS_EBCDIC)
			if ((iconv_t)0 != iod->input_conv_cd)
			{
				ICONV_CLOSE_CD(iod->input_conv_cd);
			}
			SET_CODE_SET(iod->in_code_set, (char *)(pp->str.addr + p_offset + 1));
			if (DEFAULT_CODE_SET != iod->in_code_set)
				ICONV_OPEN_CD(iod->input_conv_cd, INSIDE_CH_SET, (char *)(pp->str.addr + p_offset + 1));
#			endif
			break;
		case iop_opchset:
#			if defined(KEEP_zOS_EBCDIC)
			if ((iconv_t)0 != iod->output_conv_cd)
			{
				ICONV_CLOSE_CD(iod->output_conv_cd);
			}
			SET_CODE_SET(iod->out_code_set, (char *)(pp->str.addr + p_offset + 1));
			if (DEFAULT_CODE_SET != iod->out_code_set)
				ICONV_OPEN_CD(iod->output_conv_cd, (char *)(pp->str.addr + p_offset + 1), INSIDE_CH_SET);
#			endif
			break;
		case iop_destroy:
			socket_destroy = TRUE;
			break;
		case iop_nodestroy:
			socket_destroy = FALSE;
			break;
		case iop_delete:
		default:
			break;
		}
		UPDATE_P_OFFSET(p_offset, ch, pp);	/* updates "p_offset" using "ch" and "pp" */
	}
	if (socket_specified)
	{
		if (0 > (index = iosocket_handle(sock_handle, &handle_len, FALSE, dsocketptr)))
		{
			rts_error_csa(CSA_ARG(NULL) VARLSTCNT(4) ERR_SOCKNOTFND, 2, handle_len, sock_handle);
			return;
		}
		start = end = index;
	}
	else
	{
		start = dsocketptr->n_socket - 1;
		end = 0;
	}
	iosocket_close_range(dsocketptr, start, end, SOCKET_DELETE_TRUE);
	/* "forget" the UTF-16 CHSET variant if no sockets are connected */
	if (0 >= dsocketptr->n_socket)
	{
		dsocketptr->ichset_utf16_variant = iod->ichset = 0;
		dsocketptr->ochset_utf16_variant = iod->ochset = 0;
	}
	if (!socket_specified)
	{
		iod->state = dev_closed;
		if (socket_destroy)
		{
			active_device = 0;
			iosocket_destroy(iod);
		}
	}
	REVERT_GTMIO_CH(&iod->pair, ch_set);
}

void iosocket_close_range(d_socket_struct *dsocketptr, int start, int end, boolean_t socket_delete)
{
	int4		ii,jj;
	int		rc, save_fd = FD_INVALID, save_rc = 0, save_errno = 0;
	ssize_t		status;
	intrpt_state_t	prev_intrpt_state;
	socket_struct	*socketptr;
	struct stat	statbuf, fstatbuf;
	char		*path;
	int		res;
	int		null_fd = 0;
	DEBUG_ONLY(int	local_process_exiting);

	DEBUG_ONLY(local_process_exiting = process_exiting);	/* record in case changes while here */
	for (ii = start; ii >= end; ii--)
	{
		/* defer interrupts each iteration */
		DEFER_INTERRUPTS(INTRPT_IN_SOCKET_CLOSE, prev_intrpt_state);
		socketptr = dsocketptr->socket[ii];
		if ((NULL != socketptr) && (FD_INVALID != socketptr->sd))
		{
			if (!gtm_pipe_child)
			{	/* Don't reap if in a child process creating a new job or pipe device */
				if ((socket_local == socketptr->protocol) && socketptr->passive && socket_delete)
				{	/* only delete if passive/listening and caller wants us to delete this socket */
					assertpro(socketptr->local.sa);
					path = ((struct sockaddr_un *)(socketptr->local.sa))->sun_path;
					FSTAT_FILE(socketptr->sd, &fstatbuf, res);
					if (-1 == res)
					{
						ENABLE_INTERRUPTS(INTRPT_IN_SOCKET_CLOSE, prev_intrpt_state);
						RTS_ERROR_CSA_ABT(NULL, VARLSTCNT(8) ERR_SYSCALL, 5,
							LEN_AND_LIT("fstat during socket delete"), CALLFROM, errno);
					}
					STAT_FILE(path, &statbuf, res);
					if (-1 == res)
					{
						ENABLE_INTERRUPTS(INTRPT_IN_SOCKET_CLOSE, prev_intrpt_state);
						RTS_ERROR_CSA_ABT(NULL, VARLSTCNT(8) ERR_SYSCALL, 5,
							LEN_AND_LIT("stat during socket delete"), CALLFROM, errno);
					}
					if (UNLINK(path) == -1)
					{
						ENABLE_INTERRUPTS(INTRPT_IN_SOCKET_CLOSE, prev_intrpt_state);
						RTS_ERROR_CSA_ABT(NULL, VARLSTCNT(8) ERR_SYSCALL, 5,
							LEN_AND_LIT("unlink during socket delete"), CALLFROM, errno);
					}
				}
				/* below is similar to iosocket_flush but socketptr may not be current socket */
				if (socketptr->obuffer_timer_set)
				{
					cancel_timer((TID)socketptr);
					socketptr->obuffer_timer_set = FALSE;
				}
				status = 1;		/* OK value */
				if ((0 < socketptr->obuffer_length) && (0 == socketptr->obuffer_errno))
				{
					socketptr->obuffer_output_active = TRUE;
					status = iosocket_output_buffer(socketptr);
					socketptr->obuffer_output_active = FALSE;
				}
				if ((0 < socketptr->obuffer_size) && ((0 >= status) || (0 != socketptr->obuffer_errno)))
					iosocket_buffer_error(socketptr);	/* pre-existing error or error flushing buffer */
#				ifdef GTM_TLS
				if (socketptr->tlsenabled)
				{
					gtm_tls_session_close((gtm_tls_socket_t **)&socketptr->tlssocket);
					socketptr->tlsenabled = FALSE;
				}
#				endif
			}
			CLOSE(socketptr->sd, rc);
			if (-1 == rc)
			{
				save_rc = rc;
				save_fd = socketptr->sd;
				save_errno = errno;
			} else if (!process_exiting && (3 > socketptr->sd))
			{
				OPENFILE(DEVNULL, O_RDWR, null_fd);
				if (-1 == null_fd)
				{
					save_errno = errno;
				}
				assert(socketptr->sd == null_fd);
			}
			socketptr->sd = FD_INVALID;
			assert(local_process_exiting == process_exiting);
			if (!process_exiting)
				SOCKET_FREE(socketptr);		/* skip if on the way out */
		}
		if (dsocketptr->current_socket >= ii)
			dsocketptr->current_socket--;
		for (jj = ii + 1; jj <= (dsocketptr->n_socket - 1); jj++)
			dsocketptr->socket[jj - 1] = dsocketptr->socket[jj];
		dsocketptr->n_socket--;
		ENABLE_INTERRUPTS(INTRPT_IN_SOCKET_CLOSE, prev_intrpt_state);
	}
	if (0 != save_rc)
	{
		RTS_ERROR_CSA_ABT(NULL, VARLSTCNT(4) ERR_CLOSEFAIL, 1, save_fd, save_errno);
	} else if (-1 == null_fd)
	{
		RTS_ERROR_CSA_ABT(NULL, VARLSTCNT(6) ERR_FILEOPENFAIL, 2, LEN_AND_LIT(DEVNULL), save_errno, 0);
	}
}

void iosocket_close_one(d_socket_struct *dsocketptr, int index)
{
	/* This function is called to pass sockets to another process or to the JOB command (as stdin/stdout/stderr).
	 * In either case, if the socket is a unix domain socket and is still in the listening state (i.e. not a connected
	 * socket), we should NOT delete the socket file (that is used to establish a connection). It will be deleted by
	 * the process that takes ownership of this listening socket.
	 */
	iosocket_close_range(dsocketptr, index, index, SOCKET_DELETE_FALSE);
}
