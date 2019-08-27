/* BEGIN_COMMON_COPYRIGHT_HEADER
 *
 * clight: C daemon utility to automagically adjust screen backlight to match ambient brightness.
 * https://github.com/FedeDP/Clight/tree/master/clight
 *
 * Copyright (C) 2019  Federico Di Pierro <nierro92@gmail.com>
 *
 * This file is part of clight.
 * clight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * END_COMMON_COPYRIGHT_HEADER */

#include <opts.h>
#include <log.h>
#include <glob.h>
#ifndef NDEBUG
    #include <assert.h>
#endif
#include <module/modules_easy.h>

static void init(int argc, char *argv[]);
static void init_state(void);
static void init_topics(void);
static void sigsegv_handler(int signum);
static void check_clightd_version(void);
static void init_user_mod_path(enum CONFIG file, char *filename);
static void load_user_modules(enum CONFIG file);

state_t state = {0};
conf_t conf = {0};
const char *topics[MSGS_SIZE] = { 0 };

/* Every module needs these; let's init them before any module */
void modules_pre_start(void) {
    state.display = getenv("DISPLAY");
    state.wl_display = getenv("WAYLAND_DISPLAY");
    state.xauthority = getenv("XAUTHORITY");
} 

int main(int argc, char *argv[]) {
    state.quit = setjmp(state.quit_buf);
    if (!state.quit) {
        init(argc, argv);
        if (conf.no_backlight && conf.no_dimmer && conf.no_dpms && conf.no_gamma) {
            WARN("No functional module running. Leaving...\n");
        } else {
            modules_loop();
        }
    }
    close_log();
    return state.quit == NORM_QUIT ? EXIT_SUCCESS : EXIT_FAILURE;
}

/*
 * First of all loads options from both global and
 * local config file, and from cmdline options.
 * Then init needed modules.
 */
static void init(int argc, char *argv[]) {
    /* 
     * When receiving segfault signal,
     * call our sigsegv handler that just logs
     * a debug message before dying
     */
    signal(SIGSEGV, sigsegv_handler);
        
    open_log();
    /* We want any issue while parsing config to be logged */
    init_opts(argc, argv);
    log_conf();
    
    /* We want any error while checking Clightd required version to be logged AFTER conf logging */
    check_clightd_version();
    
    init_state();
    init_topics();
    
    /* 
     * Load user custom modules after opening log (thus this information is logged).
     * Note that local (ie: placed in $HOME) modules have higher priority,
     * thus one can override a global module (placed in /usr/share/clight/modules.d/)
     * by creating a module with same name in $HOME.
     * 
     * Clight internal modules cannot be overriden.
     */
    load_user_modules(LOCAL);
    load_user_modules(GLOBAL);
}

static void init_state(void) {
    strncpy(state.version, VERSION, sizeof(state.version));
    memcpy(&state.current_loc, &conf.loc, sizeof(loc_t));
    if (!conf.no_gamma) {
        /* Initial value -> undefined; if GAMMA is disabled instead assume DAY */
        state.time = -1;
    } else {
        state.time = DAY;
    }
    
    /* 
     * Initial state -> undefined; UPower will set this as soon as it is available, 
     * or to ON_AC if UPower is not available 
     */
    state.ac_state = -1;
}

static void init_topics(void) {
    /* BACKLIGHT */
    topics[AMBIENT_BR_UPD] = "AmbientBr";
    topics[BL_UPD] = "BlPct";
    topics[KBD_BL_UPD] = "KbdPct";
    
    /* DIMMER/DPMS */
    topics[DISPLAY_UPD] = "DisplayState";
    
    /* GAMMA */
    topics[TIME_UPD] = "Time";
    topics[IN_EVENT_UPD] = "InEvent";
    topics[SUNRISE_UPD] = "Sunrise";
    topics[SUNSET_UPD] = "Sunset";
    topics[TEMP_UPD] = "Temp";
    
    /* INHIBIT */
    topics[INHIBIT_UPD] = "Inhibited";
    
    /* INTERFACE/Requests */
    topics[DIMMER_TO_REQ] = "ReqDimmerTo";
    topics[DPMS_TO_REQ] = "ReqDpmsTo";
    topics[SCR_TO_REQ] = "ReqScrTo";
    topics[BL_TO_REQ] = "ReqBlTo";
    topics[DISPLAY_REQ] = "ReqDisplay";
    topics[TEMP_REQ] = "ReqTemp";
    topics[CAPTURE_REQ] = "ReqCapture";
    topics[CURVE_REQ] = "ReqCurve";
    topics[NO_AUTOCALIB_REQ] = "ReqAutocalib";
    topics[CONTRIB_REQ] = "ReqContrib";
    topics[LOCATION_REQ] = "ReqLocation";
    topics[SUNRISE_REQ] = "ReqSunrise";
    topics[SUNSET_REQ] = "ReqSunset";
    topics[INHIBIT_REQ] = "ReqInhibit";
    topics[BL_REQ] = "ReqBl";
    topics[UPOWER_REQ] = "ReqAcState";
    topics[KBD_BL_REQ] = "ReqKbdBl";
    
    /* LOCATION */
    topics[LOCATION_UPD] = "Location";

    /* SCREEN */
    topics[SCR_BL_UPD] = "ScreenComp";

    /* UPOWER */
    topics[UPOWER_UPD] = "AcState";
    
#ifndef NDEBUG
    /* Runtime check that any topic has been inited; useful in devel */
    for (int i = 0; i < MSGS_SIZE; i++) {
        assert(strlen(topics[i]));
    }
#endif
}

/*
 * If received a sigsegv, log a message, destroy lock then
 * set sigsegv signal handler to default (SIG_DFL),
 * and send again the signal to the process.
 */
static void sigsegv_handler(int signum) {
    WARN("Received sigsegv signal. Aborting.\n");
    close_log();
    signal(signum, SIG_DFL);
    raise(signum);
}

static void check_clightd_version(void) {
    SYSBUS_ARG(vers_args, CLIGHTD_SERVICE, "/org/clightd/clightd", "org.clightd.clightd", "Version");
    
    int r = get_property(&vers_args, "s", state.clightd_version, sizeof(state.clightd_version));
    if (r < 0 || !strlen(state.clightd_version)) {
        ERROR("No clightd found. Clightd is a mandatory dep.\n");
    } else {
        int maj_val = atoi(state.clightd_version);
        int min_val = atoi(strchr(state.clightd_version, '.') + 1);
        if (maj_val < MINIMUM_CLIGHTD_VERSION_MAJ || (maj_val == MINIMUM_CLIGHTD_VERSION_MAJ && min_val < MINIMUM_CLIGHTD_VERSION_MIN)) {
            ERROR("Clightd must be updated. Required version: %d.%d.\n", MINIMUM_CLIGHTD_VERSION_MAJ, MINIMUM_CLIGHTD_VERSION_MIN);
        } else {
            INFO("Clightd found, version: %s.\n", state.clightd_version);
        }
    }
}

static void init_user_mod_path(enum CONFIG file, char *filename) {
    switch (file) {
        case LOCAL:
            if (getenv("XDG_DATA_HOME")) {
                snprintf(filename, PATH_MAX, "%s/clight/modules.d/*", getenv("XDG_DATA_HOME"));
            } else {
                snprintf(filename, PATH_MAX, "%s/.local/share/clight/modules.d/*", getpwuid(getuid())->pw_dir);
            }
            break;
        case GLOBAL:
            snprintf(filename, PATH_MAX, "%s/modules.d/*", DATADIR);
            break;
        default:
            break;
    }    
}

static void load_user_modules(enum CONFIG file) {
    char modules_path[PATH_MAX + 1];
    init_user_mod_path(file, modules_path);
    
    glob_t gl = {0};
    if (glob(modules_path, GLOB_NOSORT | GLOB_ERR, NULL, &gl) == 0) {
        for (int i = 0; i < gl.gl_pathc; i++) {
            if (m_load(gl.gl_pathv[i]) == MOD_OK) {
                INFO("'%s' loaded.\n", gl.gl_pathv[i]);
            } else {
                WARN("'%s' failed to load.\n", gl.gl_pathv[i]);
            }
        }
        globfree(&gl);
    }
}
