/*
 * pythonparameter.h
 *
 * Contains the PythonParameter Parameter subclass.
 *
 */

#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <slash/slash.h>

#include "slash_command.h"


extern PyThreadState* main_thread_state;

typedef struct {
    SlashCommandObject slash_command_object;
    struct slash_command command_heap;  // The implementation of slash allows us control where to store our slash_command
    PyObject *py_slash_func;
    // TODO Kevin: We should also expose completer to be implemented by the user
	int keep_alive: 1;  // For the sake of reference counting, keep_alive should only be changed by SlashCommand_set_keep_alive()

    /* NOTE: This should currently only be set by the constructor. Changing during object lifetime skips desired error checks. */
    /* Whether to generate short opts for the parameters of the provided Python function.
        Taking the following signature as an example:
        `def function(option: str) -> None`
        short_opts allows `option` to also be filled by `-o`, otherwise only --option is allowed.
        short_opts also enables exceptions for multiple parameters starting with the same (case-sensitive) letter:
        `def function(option: str, option2: str) -> None` <-- exception
        `def function(option: str, Option2: str) -> None` <-- permitted, as `option2` becomes `-O` */
    bool short_opts;
} PythonSlashCommandObject;

extern PyTypeObject PythonSlashCommandType;

PythonSlashCommandObject *python_wraps_slash_command(const struct slash_command * command);
