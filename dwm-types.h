#ifndef DWM_TYPES_H
#define DWM_TYPES_H

/**
 * @file dwm-types.h
 * @brief Shared types for dwm libxylem (XY) plugins.
 *
 * Follows the libxylem two-header rule:
 * - Implementers of dwm XY hooks include "dwm-types.h" and <ttypt/xy-mod.h>.
 * - Callers (including dwm.c) include "dwm.h".
 */

#include <stddef.h>
#include <stdint.h>

/**
 * @brief _DWM_CMD ClientMessage opcodes.
 */
typedef enum {
	DWM_CMD_RELOAD           = 1,
	DWM_CMD_VIEW             = 2,
	DWM_CMD_TAG              = 3,
	DWM_CMD_TOGGLEFLOATING   = 4,
	DWM_CMD_TOGGLEFULLSCREEN = 5,
	DWM_CMD_ZOOM             = 6,
	DWM_CMD_FOCUSSTACK       = 7,
	DWM_CMD_KILLCLIENT       = 8,
	DWM_CMD_SETLAYOUT        = 9,
	DWM_CMD_SETGAPS          = 10,
	DWM_CMD_TOGGLEBAR        = 11,
	DWM_CMD_FOCUS_WIN        = 12,
	DWM_CMD_TAG_WIN          = 13,
	DWM_CMD_PULL_WIN         = 14,
	DWM_CMD_CLOSE_WIN        = 15,
	DWM_CMD_XY_MOD           = 16  /* arg1: 1=load, 2=unload, 3=reload (module name in _DWM_MOD_CMD root prop) */
} dwm_cmd_op_t;

typedef enum {
	DWM_XY_MOD_LOAD   = 1,
	DWM_XY_MOD_UNLOAD = 2,
	DWM_XY_MOD_RELOAD = 3
} dwm_xy_mod_action_t;

#endif /* DWM_TYPES_H */
