#ifndef DWM_CMD_H
#define DWM_CMD_H

#include <X11/Xlib.h>

/**
 * @brief Handles _DWM_CMD ClientMessage opcodes dispatched to root window.
 * Returns 1 if handled (caller should return from clientmessage), 0 otherwise.
 */
int dwm_handle_cmd_message(XClientMessageEvent *cme);

#endif /* DWM_CMD_H */
