/****************************************************************
 *								*
 * Copyright (c) 2001-2023 Fidelity National Information	*
 * Services, Inc. and/or its subsidiaries. All rights reserved.	*
 *								*
 * Copyright (c) 2017-2020 YottaDB LLC and/or its subsidiaries. *
 * All rights reserved.						*
 *								*
 *	This source code contains the intellectual property	*
 *	of its copyright holder(s), and is made available	*
 *	under a license.  If you do not know the terms of	*
 *	the license, please stop and do not read further.	*
 *								*
 ****************************************************************/

#ifndef GTMDBGLVL_H_INCLUDED
#define GTMDBGLVL_H_INCLUDED

/* Define GT.M debug levels. These values can be added together to turn on multiple
   features at the same time. Note that the cumulative value specified in the logical
   or environment variable must currently be specified in decimal. */
#define	GDL_None		0x00000000	/* (000) No debugging is happening today */
#define	GDL_Simple		0x00000001	/* (001) Regular assert checking, no special checks */
#define	GDL_SmStats		0x00000002	/* (002) Print usage statistics at end of process */
#define	GDL_SmTrace		0x00000004	/* (004) Trace each malloc/free (output to stderr) */
#define	GDL_SmDumpTrace		0x00000008	/* (008) Dump malloc/free trace information on exit */
#define	GDL_SmAllocVerf		0x00000010	/* (016) Perform verification of allocated storage chain for each call */
#define	GDL_SmFreeVerf		0x00000020	/* (032) Perform simple verification of free storage chain for each call */
#define	GDL_SmBackfill		0x00000040	/* (064) Backfill unused storage (cause exceptions if released storage is used */
#define	GDL_SmChkAllocBackfill	0x00000080	/* (128) Verify backfilled storage in GDL_AllocVerf while verifying \
						   each individual queue entry */
#define	GDL_SmChkFreeBackfill	0x00000100	/* (256) Verify backfilled storage in GDL_FreeVerf while verifying \
						   each individual queue entry */
#define	GDL_SmStorHog		0x00000200	/* (512) Each piece of storage allocated is allocated in an element twice \
						   the desired size to provide glorious amounts of backfill for \
						   overrun checking. */
#define GDL_DumpOnStackOFlow	0x00000400	/* (1024) When get a stack overflow or out-of-memory error, generate a core */
#define GDL_ZSHOWDumpOnSignal	0x00000800	/* (2048) Don't supress YDB_FATAL file creation when get a signal */
#define GDL_PrintIndCacheStats	0x00001000	/* (4096) Print indirect cacheing stats */
#define GDL_PrintCacheStats	0x00002000	/* (8192) Print stats on $Piece and UTF8 cacheing (debug only) */
#define GDL_DebugCompiler	0x00004000	/* (16384) Turn on compiler debugging */
#define GDL_SmDump		0x00008000	/* (32768) Do full blown storage dump -- only useful in debug mode */
#define GDL_PrintEntryPoints	0x00010000	/* (65536) Print address of entry points when they are loaded/resolved */
#define GDL_PrintSockIntStats	0x00020000	/* (131072) Print Socket interrupt stats on exit */
#define GDL_SmInitAlloc		0x00040000	/* (262144) Initialize all storage allocated or deallocated with 0xdeadbeef */
#define GDL_PrintPipeIntStats	0x00080000	/* (524288) Print Pipe/Fifo(rm) interrupt stats on exit */
#define GDL_IgnoreAvailSpace	0x00100000	/* (1048576) Allow gdsfilext/mu_cre_file (UNIX) to ignore available space */
#define GDL_PrintPMAPStats	0x00200000	/* (2097152) Print process memory map on exit (using pmap or procmap utility) */
#define GDL_AllowLargeMemcpy	0x00400000	/* (4194304) Bypass the 1GB sanity check in gtm_memcpy_validate_and_execute() */
						/* ... */
#define GDL_UnconditionalEpoch	0x40000000	/* (1073741824) Write an EPOCH for each update. replaces UNCONDITIONAL_EPOCH */
#define GDL_UseSystemMalloc	0x80000000	/* (2147483648) Use the system's malloc(), disabling all the above GDL_Sm options */

#define GDL_SmAllMallocDebug	(GDL_Simple | GDL_SmStats | GDL_SmTrace | GDL_SmDumpTrace | GDL_SmAllocVerf			\
					| GDL_SmFreeVerf | GDL_SmBackfill | GDL_SmChkAllocBackfill | GDL_SmChkFreeBackfill	\
					| GDL_SmStorHog | GDL_SmDump | GDL_SmInitAlloc)


/* From sr_port/wcs_flu.h: Use GDL_UnconditionalEpoch to write EPOCH records for EVERY update.
 * This feature (when coupled with replication can turn out to be very useful to debug integ errors. Given that
 * an integ error occurred, through a sequence of binary search like rollbacks, we can find out exactly what
 * transaction number the error occurred so we can have a copy of the db for the prior transaction and compare
 * it with the db after the bad transaction and see exactly what changed. This was very useful in figuring out
 * the cause of the DBKEYORD error as part of enabling clues for TP (C9905-001119).
 */

#endif
