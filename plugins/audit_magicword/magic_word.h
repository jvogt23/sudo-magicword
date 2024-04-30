/**
 * MagicWord - Plugin for sudo developed by James Vogt.
 * Version 1.0
 * The purpose of this plugin is to emulate the famous
 * "Uh uh uh! you didn't say the magic word!" scene from the original
 * Jurassic Park. This program checks for errors in
 * the completion of a sudo command, checks what
 * type of error occurred, and if the error may have been
 * caused by an incorrect password, it will run the same
 * routine seen in the original Jurassic Park movie.
 * 
 * This isn't legal advice, but personally I don't care
 * if you decide to change this program or anything like that.
 * Just leave my name on it and add yours or something.
*/

//#pragma once
#include <config.h>

#include <sys/wait.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pathnames.h>
#include <sudo_compat.h>
#include <sudo_conf.h>
#include <sudo_debug.h>
#include <sudo_dso.h>
#include <sudo_fatal.h>
#include <sudo_gettext.h>
#include <sudo_json.h>
#include <sudo_plugin.h>
#include <sudo_util.h>
#ifdef HAVE_BSM_AUDIT
# include <bsm_audit.h>
#endif
#ifdef HAVE_LINUX_AUDIT
# include <linux_audit.h>
#endif
#ifdef HAVE_SOLARIS_AUDIT
# include <solaris_audit.h>
#endif

extern sudo_dso_public struct audit_plugin magic_word_audit;

int magic_word_open(
    unsigned int version,
    sudo_conv_t conversation,
    sudo_printf_t sudo_plugin_printf,
    char * const settings[],
    char * const user_info,
    int submit_optind,
    char * const submit_argv[],
    char * const submit_envp[],
    char * const plugin_options[],
    const char **errstr
);

void magic_word_close(int status_type, int status);

int magic_word_accept(
    const char *plugin_name,
    unsigned int plugin_type,
    char * const command_info[],
    char * const run_argv[],
    char * const run_envp[],
    const char **errstr
);

int magic_word_reject(
    const char *plugin_name,
    unsigned int plugin_type,
    const char *audit_msg,
    char * const command_info[],
    const char **errstr
);

int magic_word_error(
    const char *plugin_name,
    unsigned int plugin_type,
    const char *audit_msg,
    char * const command_info[],
    const char **errstr
);

int show_version(int verbose);
