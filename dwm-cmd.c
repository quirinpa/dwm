/* See LICENSE file for copyright and license details.
 * dwm-cmd.c: Decoupled handler for _DWM_CMD ClientMessage opcodes.
 */

#include "dwm-cmd.h"
#include "dwm-types.h"
#include "dwm.h"

/* Prototype declared in dwm.c to dispatch _DWM_CMD */
int dwm_dispatch_cmd_opcode(long cmd, long arg1, long arg2);

int
dwm_handle_cmd_message(XClientMessageEvent *cme)
{
	if (!cme)
		return 0;
	return dwm_dispatch_cmd_opcode(cme->data.l[0], cme->data.l[1], cme->data.l[2]);
}
