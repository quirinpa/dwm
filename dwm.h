#ifndef DWM_H
#define DWM_H

/**
 * @file dwm.h
 * @brief libxylem (XY) hook declarations emitted by the dwm window manager.
 */

#include "dwm-types.h"
#include <ttypt/xy.h>

/**
 * @brief Fired after dwm completes X11 setup and loads initial Xylem modules.
 */
XY_DECL(int, dwm_xy_on_startup, int, screen_num);

/**
 * @brief Fired whenever client focus changes in dwm.
 */
XY_DECL(int, dwm_xy_on_focus, unsigned long, win_id, const char *, wm_class, const char *, title);

/**
 * @brief Fired when a new X11 client window is managed by dwm.
 */
XY_DECL(int, dwm_xy_on_manage, unsigned long, win_id, const char *, wm_class, const char *, title);

/**
 * @brief Fired when an X11 client window is unmanaged/closed in dwm.
 */
XY_DECL(int, dwm_xy_on_unmanage, unsigned long, win_id);

/**
 * @brief Fired on custom _DWM_CMD ClientMessage opcodes.
 *
 * Modules can return 1 if they handled the command, 0 otherwise.
 */
XY_DECL(int, dwm_xy_on_cmd, long, cmd, long, arg1, long, arg2);

#endif /* DWM_H */
