/* 	MS-DOS API translator for DOSEMU's DPMI Server
 *
 * DANG_BEGIN_MODULE msdos.h
 *
 * REMARK
 * MS-DOS API translator allows DPMI programs to call DOS service directly
 * in protected mode.
 *
 * /REMARK
 * DANG_END_MODULE
 *
 * First Attempted by Dong Liu,  dliu@rice.njit.edu
 *
 */

#ifndef __MSDOS_H__
#define __MSDOS_H__

void _msdos_reset(void);
const char *_msdos_describe_selector(unsigned short sel);

#endif
