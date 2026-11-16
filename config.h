/* See LICENSE file for copyright and license details. */
#include <X11/XF86keysym.h>

/* appearance */
static const unsigned int gappx     = 8;        /* gaps between windows */
static const unsigned int borderpx  = 2;        /* border pixel of windows */
static const unsigned int snap      = 32;       /* snap pixel */
static const int showbar            = 1;        /* 0 means no bar */
static const int topbar             = 1;        /* 0 means bottom bar */
static const char *fonts[]          = {
	"Ubuntu Mono:size=13:antialias=true:autohint=true",
	"FontAwesome:size=12:antialias=true:autohint=true",
	"DejaVu Sans Mono:size=12:antialias=true:autohint=true",
	"monospace:size=12"
};
static const char dmenufont[]       = "Ubuntu Mono:size=13:antialias=true:autohint=true";
static const char col_gray1[]       = "#1e1e2e";
static const char col_gray2[]       = "#45475a";
static const char col_gray3[]       = "#cdd6f4";
static const char col_gray4[]       = "#cdd6f4";
static const char col_cyan[]        = "#585b70";
static const char col_border[]      = "#89b4fa";
static const char col_sky[]         = "#89dceb";
static const char col_peach[]       = "#fab387";
static const char col_yellow[]      = "#f9e2af";
static const char col_teal[]        = "#94e2d5";
static const char col_green[]       = "#a6e3a1";
static const char col_red[]         = "#f38ba8";
static const char col_lavender[]    = "#b4befe";
static const char col_rosewater[]   = "#f5e0dc";
static const char *colors[][3]      = {
	/*                   fg             bg         border   */
	[SchemeNorm]     = { col_gray3,     col_gray1, col_gray2  },
	[SchemeSel]      = { col_gray4,     col_cyan,  col_border },
	[SchemeTitle]    = { col_border,    col_gray1, col_border },
	[SchemeMem]      = { col_sky,       col_gray1, col_gray2  },
	[SchemeCpu]      = { col_peach,     col_gray1, col_gray2  },
	[SchemeVol]      = { col_yellow,    col_gray1, col_gray2  },
	[SchemeNet]      = { col_teal,      col_gray1, col_gray2  },
	[SchemeBatOk]    = { col_green,     col_gray1, col_gray2  },
	[SchemeBatWarn]  = { col_yellow,    col_gray1, col_gray2  },
	[SchemeBatCrit]  = { col_red,       col_gray1, col_gray2  },
	[SchemeDate]     = { col_lavender,  col_gray1, col_gray2  },
	[SchemeTime]     = { col_rosewater, col_gray1, col_gray2  },
};

/* tagging */
static const char *tags[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9" };

static const Rule rules[] = {
	/* xprop(1):
	 *	WM_CLASS(STRING) = instance, class
	 *	WM_NAME(STRING) = title
	 */
	/* class                  instance    title       tags mask     isfloating   monitor */
	{ "Gimp",                 NULL,       NULL,       0,            1,           -1 },
	{ "Firefox",              NULL,       NULL,       0,            0,           -1 },
	{ "Nm-connection-editor", NULL,       NULL,       0,            1,           -1 },
	{ "Pavucontrol",          NULL,       NULL,       0,            1,           -1 },
	{ "Blueman-manager",      NULL,       NULL,       0,            1,           -1 },
	{ "gnome-control-center", NULL,       NULL,       0,            1,           -1 },
	{ "Arandr",               NULL,       NULL,       0,            1,           -1 },
	{ "Lxappearance",         NULL,       NULL,       0,            1,           -1 },
	{ "Dwm-help",             NULL,       NULL,       0,            1,           -1 },
	{ "dwm-float",            NULL,       NULL,       0,            1,           -1 },
	{ NULL,                   NULL,       "dwm-help", 0,            1,           -1 },
	{ NULL,                   NULL,       "nmtui",    0,            1,           -1 },
	{ NULL,                   NULL,       "alsamixer",0,            1,           -1 },
};

/* layout(s) */
static const float mfact     = 0.55; /* factor of master area size [0.05..0.95] */
static const int nmaster     = 1;    /* number of clients in master area */
static const int resizehints = 0;    /* 1 means respect size hints in tiled resizals */
static const int lockfullscreen = 1; /* 1 will force focus on the fullscreen window */
static const int refreshrate = 120;  /* refresh rate (per second) for client move/resize */

static const Layout layouts[] = {
	/* symbol     arrange function */
	{ "◫",        tile },    /* first entry is default */
	{ "◰",        NULL },    /* no layout function means floating behavior */
	{ "▣",        monocle },
};

/* key definitions */
#define MODKEY Mod4Mask
#define TAGKEYS(KEY,TAG) \
	{ MODKEY,                       KEY,      view,           {.ui = 1 << TAG} }, \
	{ MODKEY|ControlMask,           KEY,      toggleview,     {.ui = 1 << TAG} }, \
	{ MODKEY|ShiftMask,             KEY,      tag,            {.ui = 1 << TAG} }, \
	{ MODKEY|ControlMask|ShiftMask, KEY,      toggletag,      {.ui = 1 << TAG} },

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

/* commands */
static char dmenumon[8]; /* component of dmenucmd, manipulated in spawn() */
static const char *dmenucmd[]  = { "dmenu_run", "-m", dmenumon, "-fn", dmenufont, "-nb", col_gray1, "-nf", col_gray3, "-sb", col_cyan, "-sf", col_gray4, NULL };
static const char *termcmd[]   = { "ghostty", NULL };
static const char *lockcmd[]   = { "slock", NULL };
static const char *netcmd[]    = { "/home/quirinpa/.local/bin/dwm-net", "wifi", NULL };
static const char *sysmenucmd[]= { "/home/quirinpa/.local/bin/dwm-net", NULL };
static const char *pwrmenucmd[]= { "/home/quirinpa/.local/bin/dwm-ctl", "power-menu", NULL };
static const char *volupcmd[]  = { "/home/quirinpa/.local/bin/dwm-ctl", "vol-up", NULL };
static const char *voldncmd[]  = { "/home/quirinpa/.local/bin/dwm-ctl", "vol-down", NULL };
static const char *volmutecmd[]= { "/home/quirinpa/.local/bin/dwm-ctl", "vol-mute", NULL };
static const char *micmutecmd[]= { "/home/quirinpa/.local/bin/dwm-ctl", "mic-mute", NULL };
static const char *briupcmd[]  = { "/home/quirinpa/.local/bin/dwm-ctl", "bri-up", NULL };
static const char *bridncmd[]  = { "/home/quirinpa/.local/bin/dwm-ctl", "bri-down", NULL };
static const char *shotfull[]  = { "/home/quirinpa/.local/bin/dwm-shot", "full", NULL };
static const char *shotarea[]  = { "/home/quirinpa/.local/bin/dwm-shot", "select", NULL };
static const char *shotwin[]   = { "/home/quirinpa/.local/bin/dwm-shot", "window", NULL };
static const char *helpcmd[]   = { "/home/quirinpa/.local/bin/dwm-help", NULL };

static const Key keys[] = {
	/* modifier                     key        function        argument */
	{ MODKEY,                       XK_p,      spawn,          {.v = dmenucmd } },
	{ MODKEY|ShiftMask,             XK_Return, spawn,          {.v = termcmd } },
	{ MODKEY,                       XK_F1,     spawn,          {.v = helpcmd } },
	{ MODKEY,                       XK_slash,  spawn,          {.v = helpcmd } },
	{ MODKEY,                       XK_n,      spawn,          {.v = netcmd } },
	{ MODKEY,                       XK_s,      spawn,          {.v = sysmenucmd } },
	{ MODKEY|ShiftMask,             XK_l,      spawn,          {.v = lockcmd } },
	{ MODKEY,                       XK_Escape, spawn,          {.v = pwrmenucmd } },
	{ 0,                            XK_Print,  spawn,          {.v = shotfull } },
	{ ShiftMask,                    XK_Print,  spawn,          {.v = shotarea } },
	{ MODKEY,                       XK_Print,  spawn,          {.v = shotwin } },
	{ MODKEY|ShiftMask,             XK_s,      spawn,          {.v = shotarea } },
	{ 0,              XF86XK_AudioRaiseVolume, spawn,          {.v = volupcmd } },
	{ 0,              XF86XK_AudioLowerVolume, spawn,          {.v = voldncmd } },
	{ 0,              XF86XK_AudioMute,        spawn,          {.v = volmutecmd } },
	{ 0,              XF86XK_AudioMicMute,     spawn,          {.v = micmutecmd } },
	{ 0,              XF86XK_MonBrightnessUp,  spawn,          {.v = briupcmd } },
	{ 0,              XF86XK_MonBrightnessDown,spawn,          {.v = bridncmd } },
	{ MODKEY,                       XK_F12,    spawn,          {.v = volupcmd } },
	{ MODKEY,                       XK_F11,    spawn,          {.v = voldncmd } },
	{ MODKEY,                       XK_F10,    spawn,          {.v = volmutecmd } },
	{ MODKEY,                       XK_F6,     spawn,          {.v = briupcmd } },
	{ MODKEY,                       XK_F5,     spawn,          {.v = bridncmd } },
	{ MODKEY,                       XK_b,      togglebar,      {0} },
	{ MODKEY,                       XK_j,      focusstack,     {.i = +1 } },
	{ MODKEY,                       XK_k,      focusstack,     {.i = -1 } },
	{ MODKEY|ShiftMask,             XK_j,      movestack,      {.i = +1 } },
	{ MODKEY|ShiftMask,             XK_k,      movestack,      {.i = -1 } },
	{ MODKEY,                       XK_i,      incnmaster,     {.i = +1 } },
	{ MODKEY,                       XK_d,      incnmaster,     {.i = -1 } },
	{ MODKEY,                       XK_h,      setmfact,       {.f = -0.05} },
	{ MODKEY,                       XK_l,      setmfact,       {.f = +0.05} },
	{ MODKEY,                       XK_Return, zoom,           {0} },
	{ MODKEY,                       XK_Tab,    view,           {0} },
	{ MODKEY|ShiftMask,             XK_c,      killclient,     {0} },
	{ MODKEY,                       XK_t,      setlayout,      {.v = &layouts[0]} },
	{ MODKEY,                       XK_f,      setlayout,      {.v = &layouts[1]} },
	{ MODKEY|ShiftMask,             XK_f,      togglefullscreen,{0} },
	{ MODKEY,                       XK_m,      setlayout,      {.v = &layouts[2]} },
	{ MODKEY,                       XK_space,  setlayout,      {0} },
	{ MODKEY|ShiftMask,             XK_space,  togglefloating, {0} },
	{ MODKEY,                       XK_0,      view,           {.ui = ~0 } },
	{ MODKEY|ShiftMask,             XK_0,      tag,            {.ui = ~0 } },
	{ MODKEY,                       XK_comma,  focusmon,       {.i = -1 } },
	{ MODKEY,                       XK_period, focusmon,       {.i = +1 } },
	{ MODKEY|ShiftMask,             XK_comma,  tagmon,         {.i = -1 } },
	{ MODKEY|ShiftMask,             XK_period, tagmon,         {.i = +1 } },
	TAGKEYS(                        XK_1,                      0)
	TAGKEYS(                        XK_2,                      1)
	TAGKEYS(                        XK_3,                      2)
	TAGKEYS(                        XK_4,                      3)
	TAGKEYS(                        XK_5,                      4)
	TAGKEYS(                        XK_6,                      5)
	TAGKEYS(                        XK_7,                      6)
	TAGKEYS(                        XK_8,                      7)
	TAGKEYS(                        XK_9,                      8)
	{ MODKEY,                       XK_minus,  setgaps,        {.i = -2 } },
	{ MODKEY,                       XK_equal,  setgaps,        {.i = +2 } },
	{ MODKEY|ShiftMask,             XK_equal,  setgaps,        {.i = 0  } },
	{ MODKEY|ShiftMask,             XK_r,      quit,           {.i = 1} },
	{ MODKEY|ShiftMask,             XK_q,      quit,           {0} },
};

/* button definitions */
/* click can be ClkTagBar, ClkLtSymbol, ClkStatusText, ClkWinTitle, ClkClientWin, or ClkRootWin */
static const Button buttons[] = {
	/* click                event mask      button          function        argument */
	{ ClkLtSymbol,          0,              Button1,        setlayout,      {0} },
	{ ClkLtSymbol,          0,              Button3,        setlayout,      {.v = &layouts[2]} },
	{ ClkWinTitle,          0,              Button2,        zoom,           {0} },
	{ ClkStatusText,        0,              Button1,        statusclick,    {.i = 1 } },
	{ ClkStatusText,        0,              Button2,        statusclick,    {.i = 2 } },
	{ ClkStatusText,        0,              Button3,        statusclick,    {.i = 3 } },
	{ ClkStatusText,        0,              Button4,        statusclick,    {.i = 4 } },
	{ ClkStatusText,        0,              Button5,        statusclick,    {.i = 5 } },
	{ ClkClientWin,         MODKEY,         Button1,        movemouse,      {0} },
	{ ClkClientWin,         MODKEY,         Button2,        togglefloating, {0} },
	{ ClkClientWin,         MODKEY,         Button3,        resizemouse,    {0} },
	{ ClkTagBar,            0,              Button1,        view,           {0} },
	{ ClkTagBar,            0,              Button3,        toggleview,     {0} },
	{ ClkTagBar,            MODKEY,         Button1,        tag,            {0} },
	{ ClkTagBar,            MODKEY,         Button3,        toggletag,      {0} },
};
