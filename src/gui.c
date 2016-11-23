/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license in the file COPYING
 * or http://www.opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file COPYING.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2016 Saso Kiselkov. All rights reserved.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include <XPLMDisplay.h>
#include <XPLMGraphics.h>
#include <XPLMProcessing.h>
#include <XPLMMenus.h>
#include <XPLMUtilities.h>
#include <XPWidgets.h>
#include <XPStandardWidgets.h>

#include "assert.h"
#include "dbg_gui.h"
#include "init_msg.h"
#include "list.h"
#include "nd_alert.h"
#include "xraas2.h"
#include "xraas_cfg.h"

#include "gui.h"
#include "gui_tooltips.h"

#define	XRAAS_MENU_NAME		"X-RAAS"
#define	CONFIG_GUI_CMD_NAME	"X-RAAS configuration..."
#define	DBG_GUI_TOGGLE_CMD_NAME	"Toggle debug overlay"
#define	RAAS_RESET_CMD_NAME	"Reset"
#define	RECREATE_CACHE_CMD_NAME	"Recreate data cache"
#if	IBM
#define	NEWLINE "\r\n"
#else	/* !IBM */
#define	NEWLINE "\n"
#endif	/* !IBM */

#define	COPYRIGHT1	"Copyright 2016 Saso Kiselkov. All rights reserved."
#define	COPYRIGHT2	"X-RAAS is open-source software. See COPYING for " \
			"more information."
#define	TOOLTIP_HINT	"Hint: hover your mouse cursor over any knob to " \
			"show a short description of what it does."

enum {
	LAYOUT_START_X =	10,
	LAYOUT_START_Y =	40,

	BUTTON_HEIGHT =		20,
	BUTTON_WIDTH =		200,

	TEXT_FIELD_WIDTH =	270,
	TEXT_FIELD_HEIGHT =	20,
	WINDOW_MARGIN =		10,

	TOOLTIP_LINE_HEIGHT =	13,
	TOOLTIP_WINDOW_OFFSET =	5,

	COLUMN_X =		300,
	COLUMN_Y =		BUTTON_HEIGHT * NUM_MONITORS + WINDOW_MARGIN,
	BUTTON_CHIN_Y =		120,
	MAIN_WINDOW_WIDTH =	3 * COLUMN_X + 4 * WINDOW_MARGIN,
	MAIN_WINDOW_HEIGHT =	COLUMN_Y + BUTTON_CHIN_Y
};

#define	TOOLTIP_INTVAL		0.1
#define	TOOLTIP_DISPLAY_DELAY	SEC2USEC(1)

typedef struct {
	int		x, y, w, h;
	const char	**lines;
	list_node_t	node;
} tooltip_t;

typedef struct {
	XPWidgetID	window;
	list_t		tooltips;
	list_node_t	node;
} tooltip_set_t;

enum {
	CONFIG_GUI_CMD,
	DBG_GUI_TOGGLE_CMD,
	RAAS_RESET_CMD,
	RECREATE_CACHE_CMD
};

static int plugins_menu_item;
static XPLMMenuID root_menu;
static int dbg_gui_menu_item;

static XPWidgetID main_win;
static list_t main_win_scrollbar_cbs;

static struct {
	XPWidgetID	enabled;
	XPWidgetID	allow_helos;
	XPWidgetID	startup_notify;
	XPWidgetID	use_imperial;
	XPWidgetID	us_runway_numbers;
	XPWidgetID	monitors[NUM_MONITORS];
	XPWidgetID	disable_ext_view;
	XPWidgetID	override_electrical;
	XPWidgetID	override_replay;
	XPWidgetID	speak_units;
	XPWidgetID	use_tts;
	XPWidgetID	voice_female;
	XPWidgetID	say_deep_landing;
	XPWidgetID	nd_alerts_enabled;
	XPWidgetID	nd_alert_overlay_enabled;
	XPWidgetID	nd_alert_overlay_force;
	XPWidgetID	openal_shared;

	XPWidgetID	save_acf_conf;
	XPWidgetID	save_glob_conf;

	XPWidgetID	reset_acf_conf;
	XPWidgetID	reset_glob_conf;
} buttons;

static struct {
	XPWidgetID	min_engines;
	XPWidgetID	min_mtow;
	XPWidgetID	min_takeoff_dist;
	XPWidgetID	min_landing_dist;
	XPWidgetID	min_rotation_dist;
	XPWidgetID	min_rotation_angle;
	XPWidgetID	stop_dist_cutoff;
	XPWidgetID	on_rwy_warn_initial;
	XPWidgetID	on_rwy_warn_repeat;
	XPWidgetID	on_rwy_warn_max_n;
	XPWidgetID	long_land_lim_abs;
	XPWidgetID	nd_alert_timeout;
	XPWidgetID	nd_alert_overlay_font;
	XPWidgetID	nd_alert_overlay_font_size;

	XPWidgetID	status_msg;
} text_fields;

static struct {
	XPWidgetID	long_land_lim_fract;
	XPWidgetID	voice_volume;
	XPWidgetID	min_landing_flap;
	XPWidgetID	min_takeoff_flap;
	XPWidgetID	max_takeoff_flap;
	XPWidgetID	gpa_limit_max;
	XPWidgetID	gpa_limit_mult;
	XPWidgetID	nd_alert_filter;
} scrollbars;

typedef struct {
	XPWidgetID	scrollbar;
	XPWidgetID	numeric_caption;
	double		display_multiplier;
	const char	*suffix;
	void		(*formatter)(int value, char buf[32]);
	list_node_t	node;
} scrollbar_cb_t;

static XPLMCommandRef
    toggle_cfg_gui_cmd = NULL,
    toggle_dbg_gui_cmd = NULL,
    recreate_cache_cmd = NULL,
    raas_reset_cmd = NULL;

static list_t tooltip_sets;
static tooltip_t *cur_tt = NULL;
static XPWidgetID cur_tt_win = NULL;
static int last_mouse_x, last_mouse_y;
static uint64_t mouse_moved_time;

static const char *monitor_names[NUM_MONITORS] = {
    "Approaching runway on ground",	/* APCH_RWY_ON_GND_MON */
    "Approaching runway in air",	/* APCH_RWY_IN_AIR_MON */
    "Approaching short runway in air",	/* APCH_RWY_IN_AIR_SHORT_MON */
    "On runway lined up",		/* ON_RWY_LINEUP_MON */
    "On short runway lined up",		/* ON_RWY_LINEUP_SHORT_MON */
    "On runway lined up flaps",		/* ON_RWY_FLAP_MON */
    "Short runway takeoff",		/* ON_RWY_TKOFF_SHORT_MON */
    "On runway extended holding",	/* ON_RWY_HOLDING_MON */
    "Taxiway takeoff",			/* TWY_TKOFF_MON */
    "Distance remaining on landing",	/* DIST_RMNG_LAND_MON */
    "Distance remaining on RTO",	/* DIST_RMNG_RTO_MON */
    "Taxiway landing",			/* TWY_LAND_MON */
    "Approaching runway end",		/* RWY_END_MON */
    "Too high approach (upper gate)",	/* APCH_TOO_HIGH_UPPER_MON */
    "Too high approach (lower gate)",	/* APCH_TOO_HIGH_LOWER_MON */
    "Too fast approach (upper gate)",	/* APCH_TOO_FAST_UPPER_MON */
    "Too fast approach (lower gate)",	/* APCH_TOO_FAST_LOWER_MON */
    "Landing flaps (upper gate)",	/* APCH_FLAPS_UPPER_MON */
    "Landing flaps (lower gate)",	/* APCH_FLAPS_LOWER_MON */
    "Unstable approach",		/* APCH_UNSTABLE_MON */
    "QNE altimeter setting",		/* ALTM_QNE_MON */
    "QNH altimeter setting",		/* ALTM_QNH_MON */
    "QFE altimeter setting",		/* ALTM_QFE_MON */
    "Long landing",			/* LONG_LAND_MON */
    "Late rotation"			/* LATE_ROTATION_MON */
};

static void
nd_alert2str(int level, char buf[32])
{
	switch (level) {
	case ND_ALERT_ROUTINE:
		my_strlcpy(buf, "ALL", 32);
		break;
	case ND_ALERT_NONROUTINE:
		my_strlcpy(buf, "NON-R", 32);
		break;
	default:
		ASSERT(level == ND_ALERT_CAUTION);
		my_strlcpy(buf, "CAUT", 32);
		break;
	}
}

static char *
gen_config(void)
{
	char *conf_text = NULL;
	size_t conf_sz = 0;
	char buf[512];

	append_format(&conf_text, &conf_sz,
	    "-- This configuration file was automatically generated using the\n"
	    "-- X-RAAS configuration GUI." NEWLINE NEWLINE);

#define	GEN_BOOL_CONF(widget) \
	append_format(&conf_text, &conf_sz, "%s = %s" NEWLINE, #widget, \
	    XPGetWidgetProperty(buttons.widget, xpProperty_ButtonState, \
	    NULL) ? "true" : "false")

	GEN_BOOL_CONF(enabled);
	GEN_BOOL_CONF(allow_helos);
	GEN_BOOL_CONF(startup_notify);
	GEN_BOOL_CONF(use_imperial);
	GEN_BOOL_CONF(us_runway_numbers);
	for (int i = 0; i < NUM_MONITORS; i++)
		append_format(&conf_text, &conf_sz, "%s = %s" NEWLINE,
		    monitor_conf_keys[i],
		    XPGetWidgetProperty(buttons.monitors[i],
		    xpProperty_ButtonState, NULL) ? "true" : "false");
	GEN_BOOL_CONF(disable_ext_view);
	GEN_BOOL_CONF(override_electrical);
	GEN_BOOL_CONF(override_replay);
	GEN_BOOL_CONF(speak_units);
#if	!LIN
	GEN_BOOL_CONF(use_tts);
#endif	/* !LIN */
	GEN_BOOL_CONF(voice_female);
	GEN_BOOL_CONF(say_deep_landing);
	GEN_BOOL_CONF(nd_alerts_enabled);
	GEN_BOOL_CONF(nd_alert_overlay_enabled);
	GEN_BOOL_CONF(nd_alert_overlay_force);
	GEN_BOOL_CONF(openal_shared);

#undef	GEN_BOOL_CONF

#define	GEN_TEXT_CONF(text_field) \
	do { \
		char buf[32]; \
		XPGetWidgetDescriptor(text_fields.text_field, buf, \
		    sizeof (buf) - 1); \
		if (strlen(buf) != 0 && strcmp(buf, "default") != 0) \
			append_format(&conf_text, &conf_sz, "%s = %s" NEWLINE, \
			    #text_field, buf);\
	} while (0)

	GEN_TEXT_CONF(min_engines);
	GEN_TEXT_CONF(min_mtow);
	GEN_TEXT_CONF(min_takeoff_dist);
	GEN_TEXT_CONF(min_landing_dist);
	GEN_TEXT_CONF(min_rotation_dist);
	GEN_TEXT_CONF(min_rotation_angle);
	GEN_TEXT_CONF(stop_dist_cutoff);
	GEN_TEXT_CONF(on_rwy_warn_initial);
	GEN_TEXT_CONF(on_rwy_warn_repeat);
	GEN_TEXT_CONF(on_rwy_warn_max_n);
	GEN_TEXT_CONF(long_land_lim_abs);
	GEN_TEXT_CONF(nd_alert_timeout);

	XPGetWidgetDescriptor(text_fields.nd_alert_overlay_font, buf,
	    sizeof (buf) - 1);
	if (strlen(buf) != 0 && strcmp(buf, "default") != 0 &&
	    strcmp(buf, ND_alert_overlay_default_font) != 0)
		append_format(&conf_text, &conf_sz,
		    "nd_alert_overlay_font = %s" NEWLINE, buf);
	GEN_TEXT_CONF(nd_alert_overlay_font_size);

#undef	GEN_TEXT_CONF

#define	GEN_FRACT_CONF(scrollbar, multiplier) \
	do { \
		double value = XPGetWidgetProperty(scrollbars.scrollbar, \
		    xpProperty_ScrollBarSliderPosition, NULL) * multiplier; \
		append_format(&conf_text, &conf_sz, "%s = %g" NEWLINE, \
		    #scrollbar, value); \
	} while (0)

	GEN_FRACT_CONF(long_land_lim_fract, 0.01);
	GEN_FRACT_CONF(voice_volume, 0.01);
	GEN_FRACT_CONF(min_landing_flap, 0.01);
	GEN_FRACT_CONF(min_takeoff_flap, 0.01);
	GEN_FRACT_CONF(max_takeoff_flap, 0.01);
	GEN_FRACT_CONF(gpa_limit_max, 0.1);
	GEN_FRACT_CONF(gpa_limit_mult, 0.1);
	GEN_FRACT_CONF(nd_alert_filter, 1);

#undef	GEN_FRACT_CONF

	return (conf_text);
}

static void
save_config(bool_t acf_config)
{
	char *config;
	char *filename = mkpathname(acf_config ? xraas_acf_dirpath :
	    xraas_plugindir, "X-RAAS.cfg", NULL);
	FILE *fp = fopen(filename, "w");

	if (fp == NULL) {
		char buf[256];
		int err = errno;

		snprintf(buf, sizeof (buf), "Error writing configuration "
		    "file: %s", strerror(err));
		XPSetWidgetDescriptor(text_fields.status_msg, buf);
		logMsg("Error writing configuration file %s: %s", filename,
		    strerror(err));
		free(filename);
		return;
	}

	config = gen_config();
	fputs(config, fp);
	fclose(fp);
	free(config);
	free(filename);

	xraas_fini();
	xraas_init();
	gui_update();

	XPSetWidgetDescriptor(text_fields.status_msg, acf_config ?
	    "Saved aircraft configuration" : "Saved global configuration");
}

static void
reset_config(bool_t acf_config)
{
	char *filename = mkpathname(acf_config ? xraas_acf_dirpath :
	    xraas_plugindir, "X-RAAS.cfg", NULL);

	if (remove_file(filename, B_TRUE)) {
		xraas_fini();
		xraas_init();
		gui_update();
		XPSetWidgetDescriptor(text_fields.status_msg,
		    acf_config ? "Aircraft configuration reset successful" :
		    "Global configuration reset successful");
	} else {
		XPSetWidgetDescriptor(text_fields.status_msg,
		    acf_config ? "Error resetting aircraft configuration, "
		    "see Log.txt for details." :
		    "Error resetting aircraft configuration, see Log.txt "
		    "for details.");
	}
	free(filename);
}

static XPWidgetID
create_widget_rel(int x, int y, bool_t y_from_bottom, int width, int height,
    int visible, const char *descr, int root, XPWidgetID container,
    XPWidgetClass cls)
{
	int wleft = 0, wtop = 0, wright = 0, wbottom = 0;
	int bottom, right;

	if (container != NULL)
		XPGetWidgetGeometry(container, &wleft, &wtop, &wright,
		    &wbottom);
	else
		XPLMGetScreenSize(&wright, &wtop);

	x += wleft;
	if (!y_from_bottom) {
		y = wtop - y;
		bottom = y - height;
	} else {
		bottom = y;
		y = y + height;
	}
	right = x + width;

	return (XPCreateWidget(x, y, right, bottom, visible, descr, root,
	    container, cls));
}

static void
tooltip_new(tooltip_set_t *tts, int x, int y, int w, int h, const char **lines)
{
	ASSERT(lines[0] != NULL);

	tooltip_t *tt = malloc(sizeof (*tt));
	tt->x = x;
	tt->y = y;
	tt->w = w;
	tt->h = h;
	tt->lines = lines;
	list_insert_tail(&tts->tooltips, tt);
}

static tooltip_set_t *
tooltip_set_new(XPWidgetID window)
{
	tooltip_set_t *tts = malloc(sizeof (*tts));
	tts->window = window;
	list_create(&tts->tooltips, sizeof (tooltip_t),
	    offsetof(tooltip_t, node));
	list_insert_tail(&tooltip_sets, tts);
	return (tts);
}

static void
destroy_cur_tt(void)
{
	ASSERT(cur_tt_win != NULL);
	ASSERT(cur_tt != NULL);
	XPDestroyWidget(cur_tt_win, 1);
	cur_tt_win = NULL;
	cur_tt = NULL;
}

static void
tooltip_set_destroy(tooltip_set_t *tts)
{
	tooltip_t *tt;
	while ((tt = list_head(&tts->tooltips)) != NULL) {
		if (cur_tt == tt)
			destroy_cur_tt();
		list_remove(&tts->tooltips, tt);
		free(tt);
	}
	list_destroy(&tts->tooltips);
	list_remove(&tooltip_sets, tts);
	free(tts);
}

static void
set_cur_tt(tooltip_t *tt, int mouse_x, int mouse_y)
{
	int width = 2 * WINDOW_MARGIN;
	int height = 2 * WINDOW_MARGIN;

	ASSERT(cur_tt == NULL);
	ASSERT(cur_tt_win == NULL);

	for (int i = 0; tt->lines[i] != NULL; i++) {
		const char *line = tt->lines[i];
		width = MAX(XPLMMeasureString(xplmFont_Proportional, line,
		    strlen(line)) + 2 * WINDOW_MARGIN, width);
		height += TOOLTIP_LINE_HEIGHT;
	}

	cur_tt = tt;
	cur_tt_win = create_widget_rel(mouse_x + TOOLTIP_WINDOW_OFFSET,
	    mouse_y - height - TOOLTIP_WINDOW_OFFSET, B_TRUE, width, height,
	    0, "", 1, NULL, xpWidgetClass_MainWindow);
	XPSetWidgetProperty(cur_tt_win, xpProperty_MainWindowType,
	    xpMainWindowStyle_Translucent);

	for (int i = 0, y = WINDOW_MARGIN; tt->lines[i] != NULL; i++,
	    y += TOOLTIP_LINE_HEIGHT) {
		const char *line = tt->lines[i];
		XPWidgetID line_caption;

		line_caption = create_widget_rel(WINDOW_MARGIN, y, B_FALSE,
		    width - 2 * WINDOW_MARGIN, TOOLTIP_LINE_HEIGHT, 1,
		    line, 0, cur_tt_win, xpWidgetClass_Caption);
		XPSetWidgetProperty(line_caption, xpProperty_CaptionLit, 1);
	}

	XPShowWidget(cur_tt_win);
}

static float
tooltip_floop_cb(float elapsed_since_last_call, float elapsed_since_last_floop,
    int counter, void *refcon)
{
	int mouse_x, mouse_y;
	long long now = microclock();
	tooltip_t *hit_tt = NULL;

	UNUSED(elapsed_since_last_call);
	UNUSED(elapsed_since_last_floop);
	UNUSED(counter);
	UNUSED(refcon);

	XPLMGetMouseLocation(&mouse_x, &mouse_y);

	if (last_mouse_x != mouse_x || last_mouse_y != mouse_y) {
		last_mouse_x = mouse_x;
		last_mouse_y = mouse_y;
		mouse_moved_time = now;
		if (cur_tt != NULL)
			destroy_cur_tt();
		return (TOOLTIP_INTVAL);
	}

	if (now - mouse_moved_time < TOOLTIP_DISPLAY_DELAY || cur_tt != NULL)
		return (TOOLTIP_INTVAL);

	for (tooltip_set_t *tts = list_head(&tooltip_sets); tts != NULL;
	    tts = list_next(&tooltip_sets, tts)) {
		int wleft, wtop, wright, wbottom;

		XPGetWidgetGeometry(tts->window, &wleft, &wtop, &wright,
		    &wbottom);
		if (!XPIsWidgetVisible(tts->window) ||
		    mouse_x < wleft || mouse_x > wright ||
		    mouse_y < wbottom || mouse_y > wtop)
			continue;
		for (tooltip_t *tt = list_head(&tts->tooltips); tt != NULL;
		    tt = list_next(&tts->tooltips, tt)) {
			int x1 = wleft + tt->x, x2 = wleft + tt->x + tt->w,
			    y1 = wtop - tt->y - tt->h, y2 = wtop - tt->y;

			if (mouse_x >= x1 && mouse_x <= x2 &&
			    mouse_y >= y1 && mouse_y <= y2) {
				hit_tt = tt;
				goto out;
			}
		}
	}

out:
	if (hit_tt != NULL)
		set_cur_tt(hit_tt, mouse_x, mouse_y);

	return (TOOLTIP_INTVAL);
}

static void
tooltip_init(void)
{
	list_create(&tooltip_sets, sizeof (tooltip_set_t),
	    offsetof(tooltip_set_t, node));

	XPLMRegisterFlightLoopCallback(tooltip_floop_cb, TOOLTIP_INTVAL, NULL);
}

static void
tooltip_fini(void)
{
	tooltip_set_t *tts;
	while ((tts = list_head(&tooltip_sets)) != NULL)
		tooltip_set_destroy(tts);
	list_destroy(&tooltip_sets);

	XPLMUnregisterFlightLoopCallback(tooltip_floop_cb, NULL);
}

static void
menu_cb(void *menu, void *item)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-to-int-cast"
	int cmd = (int)item;
#pragma GCC diagnostic pop

	UNUSED(menu);
	switch (cmd) {
	case CONFIG_GUI_CMD:
		XPShowWidget(main_win);
		break;
	case DBG_GUI_TOGGLE_CMD:
		ASSERT(xraas_inited);
		if (!dbg_gui_inited)
			dbg_gui_init();
		else
			dbg_gui_fini();
		XPLMCheckMenuItem(root_menu, dbg_gui_menu_item,
		    dbg_gui_inited ? xplm_Menu_Checked : xplm_Menu_Unchecked);
		break;
	case RECREATE_CACHE_CMD: {
		char *cachedir = mkpathname(xraas_xpprefsdir, "X-RAAS.cache",
		    NULL);
		if (!remove_directory(cachedir)) {
			log_init_msg(B_TRUE, INIT_ERR_MSG_TIMEOUT, NULL, NULL,
			    "Cannot remove existing data cache. See Log.txt "
			    "for more information.");
			free(cachedir);
			break;
		}
		free(cachedir);
		/*FALLTHROUGH*/
	}
	case RAAS_RESET_CMD:
		xraas_fini();
		xraas_init();
		gui_update();
		break;
	}
}

static void
create_menu(void)
{
	plugins_menu_item = XPLMAppendMenuItem(XPLMFindPluginsMenu(),
	    XRAAS_MENU_NAME, NULL, 1);
	root_menu = XPLMCreateMenu(XRAAS_MENU_NAME, XPLMFindPluginsMenu(),
	    plugins_menu_item, menu_cb, NULL);
	XPLMAppendMenuItem(root_menu, CONFIG_GUI_CMD_NAME,
	    (void *)CONFIG_GUI_CMD, 1);
	dbg_gui_menu_item = XPLMAppendMenuItem(root_menu,
	    DBG_GUI_TOGGLE_CMD_NAME, (void *)DBG_GUI_TOGGLE_CMD, 1);
	XPLMAppendMenuItem(root_menu, RAAS_RESET_CMD_NAME,
	    (void *)RAAS_RESET_CMD, 1);
	XPLMAppendMenuItem(root_menu, RECREATE_CACHE_CMD_NAME,
	    (void *)RECREATE_CACHE_CMD, 1);
}

static void
destroy_menu(void)
{
	XPLMDestroyMenu(root_menu);
	XPLMRemoveMenuItem(XPLMFindPluginsMenu(), plugins_menu_item);
}

static int
main_window_cb(XPWidgetMessage msg, XPWidgetID widget, intptr_t param1,
    intptr_t param2)
{
	UNUSED(param1);
	UNUSED(param2);

	if (msg == xpMessage_CloseButtonPushed && widget == main_win) {
		XPHideWidget(main_win);
		return (1);
	} else if (msg == xpMsg_ScrollBarSliderPositionChanged) {
		XPWidgetID scrollbar = (XPWidgetID)param1;

		for (scrollbar_cb_t *scb = list_head(&main_win_scrollbar_cbs);
		    scb != NULL;
		    scb = list_next(&main_win_scrollbar_cbs, scb)) {
			char buf[32];

			if (scrollbar != scb->scrollbar)
				continue;
			if (scb->formatter != NULL) {
				scb->formatter(XPGetWidgetProperty(scrollbar,
				    xpProperty_ScrollBarSliderPosition, NULL),
				    buf);
			} else {
				double val= XPGetWidgetProperty(scrollbar,
				    xpProperty_ScrollBarSliderPosition, NULL) *
				    scb->display_multiplier;
				if (scb->suffix != NULL)
					snprintf(buf, sizeof (buf), "%g %s",
					    val, scb->suffix);
				else
					snprintf(buf, sizeof (buf), "%g", val);
			}
			XPSetWidgetDescriptor(scb->numeric_caption, buf);
			break;
		}
	} else if (msg == xpMsg_PushButtonPressed) {
		XPWidgetID btn = (XPWidgetID)param1;

		if (btn == buttons.save_acf_conf)
			save_config(B_TRUE);
		else if (btn == buttons.save_glob_conf)
			save_config(B_FALSE);
		else if (btn == buttons.reset_acf_conf)
			reset_config(B_TRUE);
		else if (btn == buttons.reset_glob_conf)
			reset_config(B_FALSE);
		else
			assert(0);
	}

	return (0);
}

static XPWidgetID
layout_text_field(XPWidgetID window, tooltip_set_t *tts, int x, int y,
    const char *label, int max_chars, const char *units, const char **tooltip,
    bool_t wide)
{
	XPWidgetID widget;
	double cap_width = wide ? 0.5 : 0.75;

	(void) create_widget_rel(x, y, B_FALSE, TEXT_FIELD_WIDTH * cap_width,
	    TEXT_FIELD_HEIGHT - 5, 1, label, 0, window, xpWidgetClass_Caption);
	if (units != NULL)
		(void) create_widget_rel(x + TEXT_FIELD_WIDTH * 0.95, y,
		    B_FALSE, TEXT_FIELD_WIDTH * 0.05, TEXT_FIELD_HEIGHT - 5, 1,
		    units, 0, window, xpWidgetClass_Caption);
	widget = create_widget_rel(x + TEXT_FIELD_WIDTH * cap_width, y + 2,
	    B_FALSE, TEXT_FIELD_WIDTH * (0.95 - cap_width),
	    TEXT_FIELD_HEIGHT - 5, 1, "", 0, window, xpWidgetClass_TextField);
	XPSetWidgetProperty(widget, xpProperty_TextFieldType, xpTextEntryField);
	XPSetWidgetProperty(widget, xpProperty_Enabled, 1);
	if (max_chars != 0)
		XPSetWidgetProperty(widget, xpProperty_MaxCharacters,
		    max_chars);
	if (tooltip != NULL)
		tooltip_new(tts, x, y, TEXT_FIELD_WIDTH, TEXT_FIELD_HEIGHT,
		    tooltip);

	return (widget);
}

static XPWidgetID
layout_scroll_control(XPWidgetID window, tooltip_set_t *tts,
    list_t *cbs_list, int x, int y, const char *label, int minval,
    int maxval, int pagestep, bool_t slider, double display_multiplier,
    const char *suffix, void (*formatter)(int val, char buf[32]),
    const char **tooltip)
{
	XPWidgetID widget;
	XPWidgetID caption;
	scrollbar_cb_t *scb = malloc(sizeof (*scb));
	char buf[32];

	(void) create_widget_rel(x, y, B_FALSE, TEXT_FIELD_WIDTH * 0.6,
	    TEXT_FIELD_HEIGHT - 5, 1, label, 0, window, xpWidgetClass_Caption);

	widget = create_widget_rel(x + TEXT_FIELD_WIDTH * 0.6, y + 3, B_FALSE,
	    TEXT_FIELD_WIDTH * 0.35, TEXT_FIELD_HEIGHT - 5, 1, "", 0,
	    window, xpWidgetClass_ScrollBar);
	XPSetWidgetProperty(widget, xpProperty_ScrollBarType,
	    slider ? xpScrollBarTypeSlider : xpScrollBarTypeScrollBar);
	XPSetWidgetProperty(widget, xpProperty_Enabled, 1);
	XPSetWidgetProperty(widget, xpProperty_ScrollBarMin, minval);
	XPSetWidgetProperty(widget, xpProperty_ScrollBarMax, maxval);
	XPSetWidgetProperty(widget, xpProperty_ScrollBarPageAmount, pagestep);

	snprintf(buf, sizeof (buf), "%d", minval);
	caption = create_widget_rel(x + TEXT_FIELD_WIDTH * 0.95, y, B_FALSE,
	    TEXT_FIELD_WIDTH * 0.05, TEXT_FIELD_HEIGHT - 5, 1, buf, 0,
	    window, xpWidgetClass_Caption);

	scb->scrollbar = widget;
	scb->display_multiplier = display_multiplier;
	scb->numeric_caption = caption;
	scb->suffix = suffix;
	scb->formatter = formatter;
	list_insert_tail(cbs_list, scb);

	if (tooltip != NULL)
		tooltip_new(tts, x, y, TEXT_FIELD_WIDTH, TEXT_FIELD_HEIGHT,
		    tooltip);

	return (widget);
}

static void
create_main_window(void)
{
	int x, y;
	tooltip_set_t *tts;

	main_win = create_widget_rel(100, 100, B_FALSE, MAIN_WINDOW_WIDTH,
	    MAIN_WINDOW_HEIGHT, 0, "X-RAAS Configuration", 1, NULL,
	    xpWidgetClass_MainWindow);
	XPSetWidgetProperty(main_win, xpProperty_MainWindowHasCloseBoxes, 1);
	XPAddWidgetCallback(main_win, main_window_cb);
	list_create(&main_win_scrollbar_cbs, sizeof (scrollbar_cb_t),
	    offsetof(scrollbar_cb_t, node));

	tts = tooltip_set_new(main_win);

	(void) create_widget_rel(LAYOUT_START_X + WINDOW_MARGIN,
	    LAYOUT_START_Y - 15, B_FALSE, COLUMN_X, 12, 1, "Global settings",
	    0, main_win, xpWidgetClass_Caption);
	(void) create_widget_rel(LAYOUT_START_X, LAYOUT_START_Y, B_FALSE,
	    COLUMN_X, COLUMN_Y, 1, "", 0, main_win, xpWidgetClass_SubWindow);

	(void) create_widget_rel(LAYOUT_START_X + WINDOW_MARGIN +
	    COLUMN_X + WINDOW_MARGIN,
	    LAYOUT_START_Y - 15, B_FALSE, COLUMN_X, 12, 1, "Monitor settings",
	    0, main_win, xpWidgetClass_Caption);
	(void) create_widget_rel(LAYOUT_START_X + COLUMN_X + WINDOW_MARGIN,
	    LAYOUT_START_Y, B_FALSE, COLUMN_X, COLUMN_Y, 1, "",
	    0, main_win, xpWidgetClass_SubWindow);

	(void) create_widget_rel(LAYOUT_START_X + WINDOW_MARGIN +
	    2 * (COLUMN_X + WINDOW_MARGIN),
	    LAYOUT_START_Y - 15, B_FALSE, COLUMN_X, 12, 1,
	    "Customize parameters", 0, main_win, xpWidgetClass_Caption);
	(void) create_widget_rel(LAYOUT_START_X +
	    2 * (COLUMN_X + WINDOW_MARGIN),
	    LAYOUT_START_Y, B_FALSE, COLUMN_X, COLUMN_Y, 1, "",
	    0, main_win, xpWidgetClass_SubWindow);

#define	LAYOUT_BUTTON(var, text, type, behavior, tooltip) \
	do { \
		buttons.var = create_widget_rel(x, y + 2, B_FALSE, 20, \
		    BUTTON_HEIGHT - 5, 1, "", 0, main_win, \
		    xpWidgetClass_Button); \
		XPSetWidgetProperty(buttons.var, xpProperty_ButtonType, \
		    xpRadioButton); \
		XPSetWidgetProperty(buttons.var, xpProperty_ButtonBehavior, \
		    xpButtonBehavior ## behavior); \
		(void) create_widget_rel(x + 20, y, B_FALSE, \
		    BUTTON_WIDTH - 20, BUTTON_HEIGHT - 5, 1, text, 0, \
		    main_win, xpWidgetClass_Caption); \
		tooltip_new(tts, x, y, BUTTON_WIDTH, BUTTON_HEIGHT, tooltip); \
		y += BUTTON_HEIGHT; \
	} while (0)

	x = LAYOUT_START_X;
	y = LAYOUT_START_Y + WINDOW_MARGIN / 2;
	LAYOUT_BUTTON(enabled, "Enabled",
	    PushButton, CheckBox, enabled_tooltip);
	LAYOUT_BUTTON(use_imperial, "Call out distances in feet",
	    PushButton, CheckBox, use_imperial_tooltip);
	LAYOUT_BUTTON(us_runway_numbers, "US runway numbers",
	    PushButton, CheckBox, us_runway_numbers_tooltip);
	LAYOUT_BUTTON(voice_female, "Voice gender female",
	    PushButton, CheckBox, voice_female_tooltip);
	LAYOUT_BUTTON(speak_units, "Speak units",
	    PushButton, CheckBox, speak_units_tooltip);
	LAYOUT_BUTTON(say_deep_landing, "Say 'DEEP LANDING'",
	    PushButton, CheckBox, say_deep_landing_tooltip);
	LAYOUT_BUTTON(allow_helos, "Start up in helicopters",
	    PushButton, CheckBox, allow_helos_tooltip);
	LAYOUT_BUTTON(startup_notify, "Show startup notification",
	    PushButton, CheckBox, startup_notify_tooltip);
	LAYOUT_BUTTON(disable_ext_view, "Silence in external views",
	    PushButton, CheckBox, disable_ext_view_tooltip);
	LAYOUT_BUTTON(nd_alerts_enabled, "Visual alerts",
	    PushButton, CheckBox, nd_alerts_enabled_tooltip);
	LAYOUT_BUTTON(nd_alert_overlay_enabled, "Visual alert overlay",
	    PushButton, CheckBox, nd_alert_overlay_enabled_tooltip);
	LAYOUT_BUTTON(nd_alert_overlay_force,
	    "Always show visual alerts using overlay",
	    PushButton, CheckBox, nd_alert_overlay_force_tooltip);
	LAYOUT_BUTTON(override_electrical, "Override electrical check",
	    PushButton, CheckBox, override_electrical_tooltip);
	LAYOUT_BUTTON(override_replay, "Show in replay mode",
	    PushButton, CheckBox, override_replay_tooltip);
	LAYOUT_BUTTON(use_tts, "Use Text-To-Speech",
	    PushButton, CheckBox, use_tts_tooltip);
#if	LIN
	XPSetWidgetProperty(buttons.use_tts, xpProperty_Enabled, 0);
#endif	/* !LIN */
	LAYOUT_BUTTON(openal_shared, "Shared audio driver context",
	    PushButton, CheckBox, openal_shared_tooltip);

	x = LAYOUT_START_X + COLUMN_X + WINDOW_MARGIN;
	y = LAYOUT_START_Y + WINDOW_MARGIN / 2;

	for (int i = 0; i < NUM_MONITORS; i++)
		LAYOUT_BUTTON(monitors[i], monitor_names[i], PushButton,
		    CheckBox, monitor_tooltips[i]);

#undef	LAYOUT_BUTTON

	x = LAYOUT_START_X + 2 * (COLUMN_X + WINDOW_MARGIN);
	y = LAYOUT_START_Y + WINDOW_MARGIN / 2;
	scrollbars.voice_volume = layout_scroll_control(main_win, tts,
	    &main_win_scrollbar_cbs, x, y, "Audio volume", 0, 100, 10,
	    B_FALSE, 1.0, "%", NULL, voice_volume_tooltip);
	y += TEXT_FIELD_HEIGHT;
	text_fields.min_engines = layout_text_field(main_win, tts, x, y,
	    "Minimum number of engines", 1, NULL, min_engines_tooltip, B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	text_fields.min_mtow = layout_text_field(main_win, tts, x, y,
	    "Minimum MTOW", 6, "kg", min_mtow_tooltip, B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	text_fields.min_takeoff_dist = layout_text_field(main_win, tts, x, y,
	    "Minimum takeoff distance", 4, "m", min_takeoff_dist_tooltip,
	    B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	text_fields.min_landing_dist = layout_text_field(main_win, tts, x, y,
	    "Minimum landing distance", 4, "m", min_landing_dist_tooltip,
	    B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	text_fields.min_rotation_dist = layout_text_field(main_win, tts, x, y,
	    "Minimum rotation distance", 4, "m", min_rotation_dist_tooltip,
	    B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	text_fields.min_rotation_angle = layout_text_field(main_win, tts, x, y,
	    "Minimum rotation angle", 4, "deg", min_rotation_angle_tooltip,
	    B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	text_fields.stop_dist_cutoff = layout_text_field(main_win, tts, x, y,
	    "Runway remaining cutoff length", 4, "m", stop_dist_cutoff_tooltip,
	    B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	text_fields.on_rwy_warn_initial = layout_text_field(main_win, tts, x, y,
	    "Runway extended holding (initial)", 3, "sec",
	    on_rwy_warn_initial_tooltip, B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	text_fields.on_rwy_warn_repeat = layout_text_field(main_win, tts, x, y,
	    "Runway extended holding (repeat)", 3, "sec",
	    on_rwy_warn_repeat_tooltip, B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	text_fields.on_rwy_warn_max_n = layout_text_field(main_win, tts, x, y,
	    "Runway extended holding maximum", 2, NULL,
	    on_rwy_warn_max_n_tooltip, B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	scrollbars.gpa_limit_mult = layout_scroll_control(main_win, tts,
	    &main_win_scrollbar_cbs, x, y, "GPA limit multiplier", 0, 100, 10,
	    B_FALSE, 0.1, NULL, NULL, gpa_limit_mult_tooltip);
	y += TEXT_FIELD_HEIGHT;
	scrollbars.gpa_limit_max = layout_scroll_control(main_win, tts,
	    &main_win_scrollbar_cbs, x, y, "GPA limit maximum", 0, 100, 10,
	    B_FALSE, 0.1, "deg", NULL, gpa_limit_max_tooltip);
	y += TEXT_FIELD_HEIGHT;
	text_fields.long_land_lim_abs = layout_text_field(main_win, tts, x, y,
	    "Long landing absolute limit", 4, "m", long_land_lim_abs_tooltip,
	    B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	scrollbars.long_land_lim_fract = layout_scroll_control(main_win, tts,
	    &main_win_scrollbar_cbs, x, y, "Long landing limit fraction", 0,
	    100, 10, B_FALSE, 0.01, NULL, NULL, long_land_lim_fract_tooltip);
	y += TEXT_FIELD_HEIGHT;
	scrollbars.min_landing_flap = layout_scroll_control(main_win, tts,
	    &main_win_scrollbar_cbs, x, y, "Minimum landing flaps", 0, 100,
	    10, B_FALSE, 0.01, NULL, NULL, min_landing_flap_tooltip);
	y += TEXT_FIELD_HEIGHT;
	scrollbars.min_takeoff_flap = layout_scroll_control(main_win, tts,
	    &main_win_scrollbar_cbs, x, y, "Minimum takeoff flaps", 0, 100,
	    10, B_FALSE, 0.01, NULL, NULL, min_takeoff_flap_tooltip);
	y += TEXT_FIELD_HEIGHT;
	scrollbars.max_takeoff_flap = layout_scroll_control(main_win, tts,
	    &main_win_scrollbar_cbs, x, y, "Maximum takeoff flaps", 0, 100,
	    10, B_FALSE, 0.01, NULL, NULL, max_takeoff_flap_tooltip);
	y += TEXT_FIELD_HEIGHT;
	text_fields.nd_alert_timeout = layout_text_field(main_win, tts, x, y,
	    "Visual alert timeout", 3, "sec", nd_alert_timeout_tooltip,
	    B_FALSE);
	y += TEXT_FIELD_HEIGHT;
	scrollbars.nd_alert_filter = layout_scroll_control(main_win, tts,
	    &main_win_scrollbar_cbs, x, y, "Visual alert filter", 0, 2, 1,
	    B_FALSE, 1, NULL, nd_alert2str, nd_alert_filter_tooltip);
	y += TEXT_FIELD_HEIGHT;
	text_fields.nd_alert_overlay_font = layout_text_field(main_win, tts,
	    x, y, "Visual alert overlay font", 500, NULL,
	    nd_alert_overlay_font_tooltip, B_TRUE);
	y += TEXT_FIELD_HEIGHT;
	text_fields.nd_alert_overlay_font_size = layout_text_field(main_win,
	    tts, x, y, "Visual alert overlay font size", 3, NULL,
	    nd_alert_overlay_font_size_tooltip, B_FALSE);
	y += TEXT_FIELD_HEIGHT;

#define	LAYOUT_PUSH_BUTTON(var, x, y, w, h, label, tooltip) \
	do { \
		buttons.var = create_widget_rel(x, y, B_FALSE, w, h, 1, \
		    label, 0, main_win, xpWidgetClass_Button); \
		tooltip_new(tts, x, y, w, h, tooltip); \
	} while (0)

	LAYOUT_PUSH_BUTTON(save_acf_conf, WINDOW_MARGIN, MAIN_WINDOW_HEIGHT -
	    70, BUTTON_WIDTH, 18, "SAVE aircraft configuration",
	    save_acf_tooltip);
	LAYOUT_PUSH_BUTTON(save_glob_conf, WINDOW_MARGIN, MAIN_WINDOW_HEIGHT -
	    50, BUTTON_WIDTH, 18, "SAVE global configuration",
	    save_glob_tooltip);

	LAYOUT_PUSH_BUTTON(reset_acf_conf, MAIN_WINDOW_WIDTH - BUTTON_WIDTH -
	    WINDOW_MARGIN, MAIN_WINDOW_HEIGHT - 70, BUTTON_WIDTH, 18,
	    "RESET aircraft configuration", reset_acf_tooltip);
	LAYOUT_PUSH_BUTTON(reset_glob_conf, MAIN_WINDOW_WIDTH - BUTTON_WIDTH -
	    WINDOW_MARGIN, MAIN_WINDOW_HEIGHT - 50, BUTTON_WIDTH, 18,
	    "RESET global configuration", reset_glob_tooltip);

#undef	LAYOUT_PUSH_BUTTON

	create_widget_rel(2 * WINDOW_MARGIN + BUTTON_WIDTH,
	    MAIN_WINDOW_HEIGHT - 75, B_FALSE,
	    MAIN_WINDOW_WIDTH - 2 * BUTTON_WIDTH - 4 * WINDOW_MARGIN,
	    TEXT_FIELD_HEIGHT, 1, COPYRIGHT1, 0, main_win,
	    xpWidgetClass_Caption);
	create_widget_rel(2 * WINDOW_MARGIN + BUTTON_WIDTH,
	    MAIN_WINDOW_HEIGHT - 62, B_FALSE,
	    MAIN_WINDOW_WIDTH - 2 * BUTTON_WIDTH - 4 * WINDOW_MARGIN,
	    TEXT_FIELD_HEIGHT, 1, COPYRIGHT2, 0, main_win,
	    xpWidgetClass_Caption);
	create_widget_rel(2 * WINDOW_MARGIN + BUTTON_WIDTH,
	    MAIN_WINDOW_HEIGHT - 49, B_FALSE,
	    MAIN_WINDOW_WIDTH - 2 * BUTTON_WIDTH - 4 * WINDOW_MARGIN,
	    TEXT_FIELD_HEIGHT, 1, TOOLTIP_HINT, 0, main_win,
	    xpWidgetClass_Caption);

	text_fields.status_msg = create_widget_rel(WINDOW_MARGIN,
	    MAIN_WINDOW_HEIGHT - 27, B_FALSE, MAIN_WINDOW_WIDTH -
	    2 * WINDOW_MARGIN, 18, 1, "", 0, main_win, xpWidgetClass_Caption);
}

static void
destroy_main_window(void)
{
	scrollbar_cb_t *scb;
	while ((scb = list_head(&main_win_scrollbar_cbs)) != NULL) {
		list_remove(&main_win_scrollbar_cbs, scb);
		free(scb);
	}
	XPDestroyWidget(main_win, 1);
}

static void
update_main_window(void)
{
#define	UPDATE_BUTTON_STATE(button) \
	XPSetWidgetProperty(buttons.button, xpProperty_ButtonState, \
	    xraas_state->button)

	UPDATE_BUTTON_STATE(enabled);
	UPDATE_BUTTON_STATE(allow_helos);
	UPDATE_BUTTON_STATE(startup_notify);
	UPDATE_BUTTON_STATE(use_imperial);
	UPDATE_BUTTON_STATE(us_runway_numbers);
	for (int i = 0; i < NUM_MONITORS; i++)
		XPSetWidgetProperty(buttons.monitors[i],
		    xpProperty_ButtonState, xraas_state->monitors[i]);
	UPDATE_BUTTON_STATE(disable_ext_view);
	UPDATE_BUTTON_STATE(override_electrical);
	UPDATE_BUTTON_STATE(override_replay);
	UPDATE_BUTTON_STATE(speak_units);
	UPDATE_BUTTON_STATE(use_tts);
	UPDATE_BUTTON_STATE(voice_female);
	UPDATE_BUTTON_STATE(say_deep_landing);
	UPDATE_BUTTON_STATE(nd_alerts_enabled);
	UPDATE_BUTTON_STATE(nd_alert_overlay_enabled);
	UPDATE_BUTTON_STATE(nd_alert_overlay_force);
	UPDATE_BUTTON_STATE(openal_shared);

#undef	UPDATE_BUTTON_STATE

#define	UPDATE_TEXT_FIELD(field, fmt) \
	do { \
		char buf[32]; \
		snprintf(buf, sizeof (buf), fmt, xraas_state->field); \
		XPSetWidgetDescriptor(text_fields.field, buf); \
	} while (0)

	UPDATE_TEXT_FIELD(min_engines, "%d");
	UPDATE_TEXT_FIELD(min_mtow, "%d");
	UPDATE_TEXT_FIELD(min_takeoff_dist, "%d");
	UPDATE_TEXT_FIELD(min_landing_dist, "%d");
	UPDATE_TEXT_FIELD(min_rotation_dist, "%d");
	UPDATE_TEXT_FIELD(min_rotation_angle, "%g");
	UPDATE_TEXT_FIELD(stop_dist_cutoff, "%d");
	UPDATE_TEXT_FIELD(on_rwy_warn_initial, "%d");
	UPDATE_TEXT_FIELD(on_rwy_warn_repeat, "%d");
	UPDATE_TEXT_FIELD(on_rwy_warn_max_n, "%d");
	UPDATE_TEXT_FIELD(long_land_lim_abs, "%d");
	UPDATE_TEXT_FIELD(nd_alert_timeout, "%d");
	if (strcmp(xraas_state->nd_alert_overlay_font,
	    ND_alert_overlay_default_font) == 0)
		XPSetWidgetDescriptor(text_fields.nd_alert_overlay_font,
		    "default");
	else
		XPSetWidgetDescriptor(text_fields.nd_alert_overlay_font,
		    xraas_state->nd_alert_overlay_font);
	UPDATE_TEXT_FIELD(nd_alert_overlay_font_size, "%d");

#undef	UPDATE_TEXT_FIELD

#define	UPDATE_SCROLLBAR(field, multiplier) \
	do { \
		XPSetWidgetProperty(scrollbars.field, \
		    xpProperty_ScrollBarSliderPosition, \
		    xraas_state->field * multiplier); \
		main_window_cb(xpMsg_ScrollBarSliderPositionChanged, \
		    main_win, (intptr_t)scrollbars.field, 0); \
	} while (0)

	UPDATE_SCROLLBAR(gpa_limit_mult, 10);
	UPDATE_SCROLLBAR(gpa_limit_max, 10);
	UPDATE_SCROLLBAR(long_land_lim_fract, 100);
	UPDATE_SCROLLBAR(voice_volume, 100);
	UPDATE_SCROLLBAR(min_landing_flap, 100);
	UPDATE_SCROLLBAR(min_takeoff_flap, 100);
	UPDATE_SCROLLBAR(max_takeoff_flap, 100);
	UPDATE_SCROLLBAR(nd_alert_filter, 1);

#undef	UPDATE_SCROLLBAR
}

static int
command_cb(XPLMCommandRef cmd, XPLMCommandPhase phase, void *refcon)
{
	UNUSED(cmd);
	if (phase == xplm_CommandBegin) {
		if (cmd == toggle_cfg_gui_cmd) {
			if (!XPIsWidgetVisible(main_win))
				XPShowWidget(main_win);
			else
				XPHideWidget(main_win);
		} else
			menu_cb(NULL, refcon);
	}
	return (1);
}

static void
register_commands(void)
{
	toggle_cfg_gui_cmd = XPLMCreateCommand("xraas/toggle_config_gui",
	    "Shows/hides the X-RAAS configuration window");
	XPLMRegisterCommandHandler(toggle_cfg_gui_cmd, command_cb, 0,
	    (void *)CONFIG_GUI_CMD);
	toggle_dbg_gui_cmd = XPLMCreateCommand("xraas/toggle_debug_gui",
	    "Toggles the X-RAAS debug overlay");
	XPLMRegisterCommandHandler(toggle_dbg_gui_cmd, command_cb, 0,
	    (void *)DBG_GUI_TOGGLE_CMD);
	recreate_cache_cmd = XPLMCreateCommand("xraas/recreate_data_cache",
	    "Tells X-RAAS to recreate its data cache from the current AIRAC "
	    "database");
	XPLMRegisterCommandHandler(recreate_cache_cmd, command_cb, 0,
	    (void *)RECREATE_CACHE_CMD);
	raas_reset_cmd = XPLMCreateCommand("xraas/reset",
	    "Resets X-RAAS as if it had been power-cycled");
	XPLMRegisterCommandHandler(raas_reset_cmd, command_cb, 0,
	    (void *)RAAS_RESET_CMD);
}

static void
unregister_commands(void)
{
	XPLMUnregisterCommandHandler(toggle_cfg_gui_cmd, command_cb, 0,
	    (void *)CONFIG_GUI_CMD);
	XPLMUnregisterCommandHandler(toggle_dbg_gui_cmd, command_cb, 0,
	    (void *)DBG_GUI_TOGGLE_CMD);
	XPLMUnregisterCommandHandler(recreate_cache_cmd, command_cb, 0,
	    (void *)RECREATE_CACHE_CMD);
	XPLMUnregisterCommandHandler(raas_reset_cmd, command_cb, 0,
	    (void *)RAAS_RESET_CMD);
}

void
gui_init(void)
{
	tooltip_init();
	create_menu();
	create_main_window();
	register_commands();
}

void
gui_fini(void)
{
	destroy_menu();
	destroy_main_window();
	tooltip_fini();
	unregister_commands();
}

void
gui_update(void)
{
	XPLMEnableMenuItem(root_menu, dbg_gui_menu_item, xraas_inited);
	XPLMCheckMenuItem(root_menu, dbg_gui_menu_item,
	    xraas_state->debug_graphical ? xplm_Menu_Checked :
	    xplm_Menu_Unchecked);
	update_main_window();
}
