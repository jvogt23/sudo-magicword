//#pragma once

#include "magic_word.h"

static sudo_conv_t magic_word_conv;
static sudo_printf_t magic_word_printf;

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
) {
    magic_word_conv = conversation;
    magic_word_printf = sudo_plugin_printf;

    magic_word_printf(SUDO_CONV_INFO_MSG, "Starting the magicword plugin\n");
    return 1;
}

void magic_word_close(int status_type, int status) {
    magic_word_printf(SUDO_CONV_INFO_MSG, "exiting\n");
    return;
}

int magic_word_accept(
    const char *plugin_name,
    unsigned int plugin_type,
    char * const command_info[],
    char * const run_argv[],
    char * const run_envp[],
    const char **errstr
) {
    magic_word_printf(SUDO_CONV_INFO_MSG, "accept thrown\n");
    return 1;
}

int magic_word_reject(
    const char *plugin_name,
    unsigned int plugin_type,
    const char *audit_msg,
    char * const command_info[],
    const char **errstr
) {
    magic_word_printf(SUDO_CONV_INFO_MSG, "reject thrown\n");
    return 1;
}

int magic_word_error(
    const char *plugin_name,
    unsigned int plugin_type,
    const char *audit_msg,
    char * const command_info[],
    const char **errstr
) {
    magic_word_printf(SUDO_CONV_INFO_MSG, "someone did an oopsie and error was called\n");
    return 1;
}

int show_version(int verbose) {
    magic_word_printf(SUDO_CONV_INFO_MSG, "version 42069 idk\n");
    return 1;
}

sudo_dso_public struct audit_plugin magic_word_audit = {
    SUDO_AUDIT_PLUGIN,
    SUDO_API_VERSION,
    magic_word_open,
    magic_word_close,
    magic_word_accept,
    magic_word_reject,
    magic_word_error,
    show_version,
    NULL, /* register_hooks */
    NULL, /* deregister_hooks */
    NULL /* event_alloc() filled in by sudo */
};