#include "speaker.h"
/*
 * Console Speaker Emulation
 * =============================================================================
 */

#include <sys/ioctl.h>
#ifdef __linux__
#include <sys/kd.h>
#endif


void console_speaker_on(void *gp, unsigned ms, unsigned short period)
{
	ioctl((int)(uintptr_t)gp, KDMKTONE,
		(unsigned) ((ms & 0xffff) << 16) | (period & 0xffff));
}

void console_speaker_off(void *gp)
{
	ioctl((int)(uintptr_t)gp, KDMKTONE, 0);
}
