#include "python_slash_command.h"

#include "structmember.h"

#include <slash/slash.h>
#include <slash/completer.h>

#include "../pycsh.h"
#include <pycsh/utils.h>


/* The main_thread_state is mostly needed by apm.c. But we define it here,
    so it's also visible when not compiling as APM. */
__attribute__((weak)) 
PyThreadState *main_thread_state = NULL;

int SlashCommand_func(struct slash *slash, void *context);

/**
 * @brief Check if this slash command is wrapped by a PythonSlashCommandObject.
 * 
 * @return borrowed reference to the wrapping PythonSlashCommandObject if wrapped, otherwise NULL.
 */
PythonSlashCommandObject *python_wraps_slash_command(const struct slash_command * command) {
    /* Every `PythonSlashCommand->command` requires `->context`.
        So if it doesn't have that, it isn't one of ours. */
    if (command == NULL  || command->context == NULL || command->func_ctx != SlashCommand_func)
        return NULL;  /* This slash command is not wrapped by PythonSlashCommandObject */
    return (PythonSlashCommandObject *)((char *)command - offsetof(PythonSlashCommandObject, command_heap));
}

/**
 * @brief Parse positional and named slash arguments, supporting short options if enabled.
 *
 * @param self PythonSlashCommandObject containing the Python function and short_opts flag
 * @param slash Slash context
 * @param args_out Output tuple for positional args
 * @param kwargs_out Output dict for keyword args
 * @return int 0 on success, -1 on error
 */
int pycsh_parse_slash_args(PythonSlashCommandObject *self, const struct slash *slash, PyObject **args_out, PyObject **kwargs_out) {
    PyObject *py_func = self->py_slash_func;
    const bool short_opts = self->short_opts;

    // Build short option map and boolean map if needed
    char param_short_map[256] = {0};
    PyObject *param_name_map[256] = {0};
    char param_short_is_bool[256] = {0};
    if (short_opts) {
        PyObject *func_code AUTO_DECREF = PyObject_GetAttrString(py_func, "__code__");
        PyObject *co_varnames AUTO_DECREF = PyObject_GetAttrString(func_code, "co_varnames");
        PyObject *co_argcount_obj AUTO_DECREF = PyObject_GetAttrString(func_code, "co_argcount");
        PyObject *annotations AUTO_DECREF = PyObject_GetAttrString(py_func, "__annotations__");
        if (!co_varnames || !PyTuple_Check(co_varnames) || !co_argcount_obj || !PyLong_Check(co_argcount_obj)) {
            PyErr_SetString(PyExc_TypeError, "Unable to inspect function parameters for short opts");
            return -1;
        }
        Py_ssize_t co_argcount = PyLong_AsSsize_t(co_argcount_obj);
        for (Py_ssize_t i = 0; i < co_argcount; ++i) {
            PyObject *name_obj = PyTuple_GetItem(co_varnames, i); // borrowed
            if (!name_obj || !PyUnicode_Check(name_obj)) continue;
            const char *name = PyUnicode_AsUTF8(name_obj);
            if (!name || !name[0]) continue;
            unsigned char first = (unsigned char)name[0];
            param_short_map[first] = 1;
            param_name_map[first] = name_obj;
            // Generic type inference: if no type-hint, defer to default value's type
            int param_type = 0; // 0=unknown, 1=bool, 2=int, 3=float, 4=str
            PyObject *hint = NULL;
            if (annotations && PyDict_Check(annotations)) {
                hint = PyDict_GetItem(annotations, name_obj); // borrowed
                if (hint && PyType_Check(hint)) {
                    if (PyType_IsSubtype((PyTypeObject*)hint, &PyBool_Type)) {
                        param_type = 1;
                    } else if (PyType_IsSubtype((PyTypeObject*)hint, &PyLong_Type)) {
                        param_type = 2;
                    } else if (PyType_IsSubtype((PyTypeObject*)hint, &PyFloat_Type)) {
                        param_type = 3;
                    } else if (PyType_IsSubtype((PyTypeObject*)hint, &PyUnicode_Type)) {
                        param_type = 4;
                    }
                }
            }
            if (param_type == 0) {
                // If no type-hint, check if default value exists and use its type
                PyObject *defaults AUTO_DECREF = PyObject_GetAttrString(py_func, "__defaults__");
                if (defaults && PyTuple_Check(defaults)) {
                    Py_ssize_t num_defaults = PyTuple_Size(defaults);
                    Py_ssize_t default_idx = i - (co_argcount - num_defaults);
                    if (default_idx >= 0 && default_idx < num_defaults) {
                        PyObject *default_val = PyTuple_GetItem(defaults, default_idx); // borrowed
                        if (default_val == Py_True || default_val == Py_False) {
                            param_type = 1;
                        } else if (PyLong_Check(default_val)) {
                            param_type = 2;
                        } else if (PyFloat_Check(default_val)) {
                            param_type = 3;
                        } else if (PyUnicode_Check(default_val)) {
                            param_type = 4;
                        }
                    }
                }
            }
            param_short_is_bool[first] = (param_type == 1);
            // Optionally, you could add param_short_type[first] = param_type for future use
        }
    }

    PyObject* args_tuple AUTO_DECREF = PyTuple_New(slash->argc < 0 ? 0 : slash->argc);
    PyObject* kwargs_dict AUTO_DECREF = PyDict_New();
    if (args_tuple == NULL || kwargs_dict == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory for args list or kwargs dictionary");
        return -1;
    }
    int parsed_positional_args = 0;
    for (int i = 1; i < slash->argc; ++i) {
        const char* arg = slash->argv[i];
        if (short_opts && strncmp(arg, "-", 1) == 0 && strncmp(arg, "--", 2) != 0 && strlen(arg) >= 2) {
            // Single dash, short option
            unsigned char first = (unsigned char)arg[1];
            if (param_short_map[first] && param_name_map[first]) {
                // For boolean short opts, treat as flag only (no value allowed)
                if (param_short_is_bool[first]) {
                    printf("Processing short option: %s\n", arg);
                    const char *key_str = PyUnicode_AsUTF8(param_name_map[first]);
                    if (!key_str) {
                        PyErr_SetString(PyExc_RuntimeError, "Failed to extract key string for short option");
                        return -1;
                    }
                    PyObject *py_key AUTO_DECREF = PyUnicode_FromString(key_str);
                    PyObject *py_value = Py_True;
                    if (!py_key || !py_value) {
                        PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory for short option key or value");
                        return -1;
                    }
                    if (PyDict_SetItem(kwargs_dict, py_key, py_value) != 0) {
                        PyErr_SetString(PyExc_RuntimeError, "Failed to add short option to kwargs");
                        return -1;
                    }
                    // Do NOT consume the next argument for boolean short opts
                    continue;
                } else {
                    // Non-bool short opts: allow value
                    const char *eq = strchr(arg, '=');
                    const char *value = NULL;
                    if (eq) {
                        value = eq + 1;
                    } else if (i + 1 < slash->argc) {
                        value = slash->argv[++i];
                    } else {
                        value = "1"; // treat as flag if no value
                    }
                    const char *key_str = PyUnicode_AsUTF8(param_name_map[first]);
                    if (!key_str) {
                        PyErr_SetString(PyExc_RuntimeError, "Failed to extract key string for short option");
                        return -1;
                    }
                    PyObject *py_key AUTO_DECREF = PyUnicode_FromString(key_str);
                    PyObject *py_value AUTO_DECREF = PyUnicode_FromString(value);
                    if (!py_key || !py_value) {
                        PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory for short option key or value");
                        return -1;
                    }
                    if (PyDict_SetItem(kwargs_dict, py_key, py_value) != 0) {
                        PyErr_SetString(PyExc_RuntimeError, "Failed to add short option to kwargs");
                        return -1;
                    }
                    continue;
                }
            }
        }
        if (strncmp(arg, "--", 2) != 0) {
            /* Token is not a keyword argument, simply add it as a positional argument to *args_out */
            PyObject* py_arg = PyUnicode_FromString(arg);
            if (py_arg == NULL) {
                PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory for positional argument");
                return -1;
            }
            assert(parsed_positional_args < slash->argc);
            PyTuple_SET_ITEM(args_tuple, parsed_positional_args++, py_arg);
            continue;  /* Skip processing for keyword arguments */
        }

        /* Token is a keyword argument, now try to split it into key and value */
        char* equal_sign = strchr(arg, '=');
        if (equal_sign == NULL) {
            PyErr_SetString(PyExc_ValueError, "Invalid format for keyword argument");
            return -1;
        }
        *equal_sign = '\0';  /* Null-terminate the key */
        const char* key = arg + 2;  /* Skip "--" */
        const char* value = equal_sign + 1;
        PyObject* py_key AUTO_DECREF = PyUnicode_FromString(key);
        PyObject* py_value AUTO_DECREF = PyUnicode_FromString(value);
        if (py_key == NULL || py_value == NULL) {
            PyErr_SetString(PyExc_MemoryError, "Failed to allocate memory for key or value");
            return -1;
        }

        /* TODO Kevin: Test what happens if the key is already in the dictionary here.
            i.e, the same keyword-argument being passed multiple times:
            mycommand --key=value --key=value --key=value2 */

        /* Add the key-value pair to the kwargs_out dictionary */
        if (PyDict_SetItem(kwargs_dict, py_key, py_value) != 0) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to add key-value pair to kwargs dictionary");
            return -1;
        }

    }

    /* Resize tuple to actual length, which should only ever shrink it from a maximal value of slash->argc */
    if (_PyTuple_Resize(&args_tuple, parsed_positional_args) < 0) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to resize tuple for positional arguments");
        return -1;
    }

    /* Assign the args and kwargs variables */
    *args_out = args_tuple;
    *kwargs_out = kwargs_dict;

    /* Don't let AUTO_DECREF decrement *args and **kwargs, now that they are new references.
        This could just as well have been done with Py_INCREF() */
    args_tuple = NULL;
    kwargs_dict = NULL;

    return 0;
}

#if 0
// Source: https://chat.openai.com
// Function to print or return the signature of a provided Python function
char* print_function_signature(PyObject* function, bool only_print) {
    assert(function != NULL);

    if (!PyCallable_Check(function)) {
        PyErr_SetString(PyExc_TypeError, "Argument is not callable");
        return NULL;
    }

    PyObject* inspect_module AUTO_DECREF = PyImport_ImportModule("inspect");
    if (!inspect_module) {
        return NULL;
    }

    PyObject* signature_func AUTO_DECREF = PyObject_GetAttrString(inspect_module, "signature");
    if (!signature_func) {
        return NULL;
    }

    PyObject* signature AUTO_DECREF = PyObject_CallFunctionObjArgs(signature_func, function, NULL);
    if (!signature) {
        return NULL;
    }

    PyObject* str_signature AUTO_DECREF = PyObject_Str(signature);
    if (!str_signature) {
        return NULL;
    }

    PyObject* qualname_attr AUTO_DECREF = PyObject_GetAttrString(function, "__qualname__");
    PyObject* name_attr AUTO_DECREF = PyObject_GetAttrString(function, "__name__");

    const char *qualname = qualname_attr ? PyUnicode_AsUTF8(qualname_attr) : NULL;
    const char *name = name_attr ? PyUnicode_AsUTF8(name_attr) : NULL;
    const char *func_name = qualname ? qualname : (name ? name : "Unknown");

    const char *signature_cstr = PyUnicode_AsUTF8(str_signature);
    if (!signature_cstr) {
        return NULL;
    }

    if (only_print) {
        printf("def %s%s\n", func_name, signature_cstr);
        return NULL;
    }

    ssize_t signature_len = strlen("def ") + strlen(func_name) + strlen(signature_cstr) + 1;  // +1 for NULL terminator
    char * signature_buf = malloc(signature_len);
    if (!signature_buf) {
        return NULL;
    }

    int cx = snprintf(signature_buf, signature_len, "def %s%s", func_name, signature_cstr);
    if (cx < 0) {
        free(signature_buf);
        return NULL;
     }
 
    signature_buf[signature_len-1] = '\0';
    return signature_buf;
 }
 
// Function to get or print the signature and docstring of a provided Python function in .pyi format
char* print_function_signature_w_docstr(PyObject* function, int only_print) {
    char* signature CLEANUP_STR = print_function_signature(function, false);
    if (!signature) {
        return NULL;
    }

    assert(!PyErr_Occurred());
    PyObject* doc_attr AUTO_DECREF = PyObject_GetAttrString(function, "__doc__");
    PyErr_Clear();  // We don't want an exception if __doc__ doesn't exist, we can simply use an empty string.
    const char *docstr = (doc_attr != NULL && doc_attr != Py_None) ? PyUnicode_AsUTF8(doc_attr) : "";
    
    if (!docstr) {
        return NULL;
    }

    if (only_print) {
        printf("%s:", signature);
        if (docstr && *docstr) {
            printf("    \"\"\"%s\"\"\"\n", docstr);
        }
        return NULL;
    }

    ssize_t signature_len = strlen(signature);
    ssize_t docstr_len = docstr ? strlen(docstr) : 0;
    ssize_t total_len = signature_len + (docstr_len ? (docstr_len + 16) : 0) + 1; // +14 for indentation and quote marks and null terminator
    char *result_buf = malloc(total_len);
    if (!result_buf) {
        return NULL;
    }

    strcpy(result_buf, signature);
    if (docstr_len) {
        //result_buf[signature_len-1] = '\0';  // Remove newline ...
        strcat(result_buf, ":\n");           // so we can insert a colon before it
        strcat(result_buf, "    \"\"\"");
        strcat(result_buf, docstr);
        strcat(result_buf, "\"\"\"");
    }

    return result_buf;
}
#else
static char *format_python_func_help(PyObject *func, int only_print, bool short_opts) {
    PyObject *inspect AUTO_DECREF = PyImport_ImportModule("inspect");
    if (!inspect) {
        return NULL;
    }

    PyObject *getsig AUTO_DECREF = PyObject_GetAttrString(inspect, "signature");
    PyObject *signature AUTO_DECREF = PyObject_CallFunctionObjArgs(getsig, func, NULL);
    if (!signature) {
        return NULL;
    }

    PyObject *parameters AUTO_DECREF = PyObject_GetAttrString(signature, "parameters");

    PyObject *typing AUTO_DECREF = PyImport_ImportModule("typing");
    PyObject *get_type_hints AUTO_DECREF = PyObject_GetAttrString(typing, "get_type_hints");
    PyObject *type_hints AUTO_DECREF = PyObject_CallFunctionObjArgs(get_type_hints, func, NULL);

    PyObject *doc AUTO_DECREF = PyObject_GetAttrString(func, "__doc__");
    PyObject *keys AUTO_DECREF = PyMapping_Keys(parameters);
    Py_ssize_t num_args = PyList_Size(keys);

    PyObject *output AUTO_DECREF = PyUnicode_FromString("Usage: ");
    PyObject *func_name AUTO_DECREF = PyObject_GetAttrString(func, "__name__");
    if (func_name) {
        PyUnicode_Append(&output, func_name);
    } else {
        PyUnicode_Append(&output, PyUnicode_FromString("<function>"));
    }

    PyUnicode_Append(&output, PyUnicode_FromString(" [OPTIONS...]"));

    for (Py_ssize_t i = 0; i < num_args; ++i) {
        PyObject *key = PyList_GetItem(keys, i);  // borrowed
        const char *argname = PyUnicode_AsUTF8(key);
        PyUnicode_Append(&output, PyUnicode_FromFormat(" [%s]", argname));
    }

    PyUnicode_Append(&output, PyUnicode_FromString("\n"));

    // Improved docstring formatting: avoid redundant newlines, trim leading/trailing whitespace
    if (doc && PyUnicode_Check(doc)) {
        Py_ssize_t doc_len = PyUnicode_GET_LENGTH(doc);
        // Remove leading/trailing whitespace and newlines
        PyObject *doc_stripped AUTO_DECREF = PyObject_CallMethod(doc, "strip", NULL);
        if (doc_stripped && PyUnicode_GET_LENGTH(doc_stripped) > 0) {
            PyUnicode_Append(&output, PyUnicode_FromString("\n"));
            PyUnicode_Append(&output, doc_stripped);
            PyUnicode_Append(&output, PyUnicode_FromString("\n"));
        }
    }

    PyUnicode_Append(&output, PyUnicode_FromString("\n"));

    for (Py_ssize_t i = 0; i < num_args; ++i) {
        PyObject *key = PyList_GetItem(keys, i);  // borrowed
        PyObject *param AUTO_DECREF = PyObject_GetItem(parameters, key);
        const char *argname = PyUnicode_AsUTF8(key);
        PyObject *default_obj AUTO_DECREF = PyObject_GetAttrString(param, "default");
        PyObject *empty AUTO_DECREF = PyObject_GetAttrString(inspect, "_empty");
        int has_default = 0;
        if (default_obj && empty) {
            int cmp = PyObject_RichCompareBool(default_obj, empty, Py_NE);
            if (cmp < 0) return NULL;  // Error
            has_default = cmp;
        }

        PyObject *hint = type_hints ? PyDict_GetItem(type_hints, key) : NULL;
        PyObject *hint_str_obj AUTO_DECREF = hint ? PyObject_Str(hint) : NULL;
        const char *hint_str = hint_str_obj ? PyUnicode_AsUTF8(hint_str_obj) : NULL;

        PyObject *arg_str AUTO_DECREF = PyUnicode_FromString("  ");

        /* `PyType_Check(hint)` is required to prevent segmentation faults
            for special typing objects such as `collections.abc.Container[int, pycsh.Ident]` */
        if (hint && PyType_Check(hint) && PyType_IsSubtype((PyTypeObject*)hint, &PyBool_Type)) {

            if (short_opts) {
                PyUnicode_Append(&arg_str, PyUnicode_FromFormat("-%c, ", argname[0]));
            }

            PyUnicode_Append(&arg_str,
                PyUnicode_FromFormat("--%s ", argname));

            char space_pad[28] = {0};
            for (ssize_t i = 0; i < 28-PyUnicode_GET_LENGTH(arg_str); i++) {
                space_pad[i] = ' ';
            }

            if (strnlen(space_pad, 28) > 0) {
                PyUnicode_Append(&arg_str, PyUnicode_FromFormat("%s", space_pad));
            }

            PyUnicode_Append(&arg_str,
                PyUnicode_FromFormat("(flag: bool)"));

        } else {

            char * c_str_type = "STR";
            if (hint && PyType_Check(hint) && PyType_IsSubtype((PyTypeObject*)hint, &PyLong_Type)) {
                c_str_type = "NUM";
            }

            if (short_opts) {
                PyUnicode_Append(&arg_str, PyUnicode_FromFormat("-%c, ", argname[0]));
            }

            PyUnicode_Append(&arg_str, PyUnicode_FromFormat("--%s=%s ", argname, c_str_type));

            if (!arg_str) {
                PyErr_NoMemory();
                return NULL;
            }

            char space_pad[28] = {0};
            for (ssize_t i = 0; i < 28-PyUnicode_GET_LENGTH(arg_str); i++) {
                space_pad[i] = ' ';
            }

            if (strnlen(space_pad, 28) > 0) {
                PyUnicode_Append(&arg_str, PyUnicode_FromFormat("%s", space_pad));
            }

            if (hint_str)
                PyUnicode_Append(&arg_str, PyUnicode_FromFormat("type: %s", hint_str));

            if (has_default) {
                PyObject *default_repr AUTO_DECREF = PyObject_Repr(default_obj);
                if (default_repr) {
                    PyUnicode_Append(&arg_str, PyUnicode_FromFormat(" (default = %U)", default_repr));
                }
            }

        }

        PyUnicode_Append(&arg_str, PyUnicode_FromString("\n"));
        PyUnicode_Append(&output, arg_str);
    }

    if (only_print) {
        const char *text = PyUnicode_AsUTF8(output);
        if (text) {
            PySys_WriteStdout("%s\n", text);
        }
        return NULL;
    }

    Py_ssize_t size;
    const char *utf8 = PyUnicode_AsUTF8AndSize(output, &size);
    if (!utf8) {
        return NULL;
    }

    char *result = malloc(size + 1);
    if (!result) {
        return NULL;
    }

    memcpy(result, utf8, size);
    result[size] = '\0';
    return result;  // caller must free()
}
#endif

typedef enum {
    TYPECAST_SUCCESS = 0,
    TYPECAST_UNSPECIFIED_EXCEPTION = -1,
    TYPECAST_NO_ANNOTATION = -2,
    TYPECAST_INVALID_ARG = -3,
    TYPECAST_INVALID_KWARG = -4,
} typecast_result_t;

/**
 * @brief Typecast a value to the hinted type if possible.
 * 
 * @param hint The type hint to cast to.
 * @param value The value to be typecasted.
 * @param python_func The Python function for error reporting.
 * @param param_name The name of the parameter being typecasted, used for error messages.
 * @return PyObject* New reference to the typecasted value, or NULL on error.
 */
static PyObject * typecast_to_hinted_type(PyObject *hint, PyObject *value, PyObject *python_func, PyObject *param_name) {
    if (!hint || !PyType_Check(hint)) {
        return Py_NewRef(value);  // No type hint, return original value
    }

    // Handle type casting based on the type hint
    if (hint == (PyObject*)&PyLong_Type && PyUnicode_Check(value)) {
        return PyLong_FromUnicodeObject(value, 10);
    } else if (hint == (PyObject*)&PyFloat_Type && PyUnicode_Check(value)) {
        return PyFloat_FromString(value);
    } else if (hint == (PyObject*)&PyBool_Type) {
        if (value == Py_True || value == Py_False) {
            return Py_NewRef(value);
        } else if (PyUnicode_Check(value)) {
            PyObject *lowered AUTO_DECREF = PyObject_CallMethod(value, "lower", NULL);
            if (!lowered) {
                PyObject * python_func_name AUTO_DECREF = PyObject_GetAttrString(python_func, "__qualname__");
                assert(python_func_name);
                PyErr_Format(PyExc_ValueError, "Failed to create lowered version of '%s' for boolean argument '%s' for function '%s()'. Use either \"True\"/\"False\"", PyUnicode_AsUTF8(lowered), PyUnicode_AsUTF8(param_name), PyUnicode_AsUTF8(python_func_name));
            }
            if (lowered && PyUnicode_CompareWithASCIIString(lowered, "true") == 0) {
                return Py_NewRef(Py_True);
            } else if (lowered && PyUnicode_CompareWithASCIIString(lowered, "false") == 0) {
                return Py_NewRef(Py_False);
            } else if (lowered && PyUnicode_CompareWithASCIIString(lowered, "1") == 0) {
                return Py_NewRef(Py_True);
            } else if (lowered && PyUnicode_CompareWithASCIIString(lowered, "0") == 0) {
                return Py_NewRef(Py_False);
            } else {
                PyObject * python_func_name AUTO_DECREF = PyObject_GetAttrString(python_func, "__qualname__");
                assert(python_func_name);
                PyErr_Format(PyExc_ValueError, "Invalid value '%s' for boolean argument '%s' for function '%s()'. Use either \"True\"/\"False\"", PyUnicode_AsUTF8(lowered), PyUnicode_AsUTF8(param_name), PyUnicode_AsUTF8(python_func_name));
                return NULL;
            }
        }
    }

    return Py_NewRef(value);  /* Unsupported type-hint, original value will have to do. */
}

/**
 * @brief Inspect type-hints of the provided PyObject *func, and convert the arguments in py_args and py_kwargs (in-place) to the their corresponding type-hinted type.
 * 
 * Source: https://chatgpt.com
 * 
 * @param func Python function to call with py_args and py_kwargs.
 * @param py_args New tuple (without other references) that we can still modify in place.
 * @param py_kwargs Dictionary of arguments to modify in-place.
 * @return int 0 for success
 */
static typecast_result_t SlashCommand_typecast_args(PyObject *python_func, PyObject *py_args, PyObject *py_kwargs) {
    
    PyObject *annotations AUTO_DECREF = PyObject_GetAttrString(python_func, "__annotations__");
    if (!annotations || !PyDict_Check(annotations)) {
        // Handle error, no annotations found
        // PyErr_SetString(PyExc_ValueError, "Failed to find annotations for function");
        fprintf(stderr, "Failed to find annotations for Python function, no typehint argument conversion will be made");
        return TYPECAST_NO_ANNOTATION;  // Or other appropriate error handling
    }

    {   /* handle *args */

        // Get the parameter names from the function's code object
        PyObject *code AUTO_DECREF = PyObject_GetAttrString(python_func, "__code__");
        if (!code) {
            assert(PyErr_Occurred());  // PyObject_GetAttrString() has probably raised an exception.
            return TYPECAST_UNSPECIFIED_EXCEPTION;
        }

        PyObject *varnames AUTO_DECREF = PyObject_GetAttrString(code, "co_varnames");
        if (!varnames || !PyTuple_Check(varnames)) {
            assert(PyErr_Occurred());  // PyObject_GetAttrString() has probably raised an exception.
            return TYPECAST_UNSPECIFIED_EXCEPTION;
        }

        Py_ssize_t num_args = PyTuple_Size(py_args);
        for (Py_ssize_t i = 0; i < num_args; i++) {
            PyObject *arg = PyTuple_GetItem(py_args, i);  // Borrowed reference
            
            // Get the corresponding parameter name for this positional argument
            PyObject *param_name = PyTuple_GetItem(varnames, i);  // Borrowed reference
            assert(param_name && PyUnicode_Check(param_name));

            PyObject *hint = PyDict_GetItem(annotations, param_name);  // Borrowed reference
            PyObject *new_arg = typecast_to_hinted_type(hint, arg, python_func, param_name);  // No Py_DecRef(), as PyTuple_SetItem() will steal the reference.
            
            if (!new_arg) {
                assert(PyErr_Occurred());
                return TYPECAST_INVALID_ARG;
            }
            
            if (new_arg) {
                // Replace the item in the tuple, stealing the reference of new_arg
                PyTuple_SetItem(py_args, i, new_arg);
            }
        }
    }

    {   /* Handle **kwargs */
        PyObject *key = NULL, *value = NULL;
        Py_ssize_t pos = 0;

        while (PyDict_Next(py_kwargs, &pos, &key, &value)) {
            PyObject *hint = PyDict_GetItem(annotations, key);  // Borrowed reference
            assert(!PyErr_Occurred());

            if (!hint) {
                continue;
            }

            assert(key && PyUnicode_Check(key));
            PyObject *new_kwarg AUTO_DECREF = typecast_to_hinted_type(hint, value, python_func, key);

            if (!new_kwarg) {
                assert(PyErr_Occurred());
                return TYPECAST_INVALID_KWARG;
            }
            
            if (new_kwarg) {
                // Replace the value in the dictionary
                PyDict_SetItem(py_kwargs, key, new_kwarg);
                new_kwarg = NULL;  // Set to NULL so that AUTO_DECREF doesn't decrement it
            }
        }
    }

    assert(!PyErr_Occurred());
    return TYPECAST_SUCCESS;
}

/**
 * @brief Shared callback for all slash_commands wrapped by a Slash object instance.
 */
int SlashCommand_func(struct slash *slash, void *context) {

    /* Re-acquire GIL */
    PyEval_RestoreThread(main_thread_state);
    PyThreadState * state __attribute__((cleanup(state_release_GIL))) = main_thread_state;

    PyGILState_STATE CLEANUP_GIL gstate = PyGILState_Ensure();  // TODO Kevin: Do we ever need to take the GIL here, we don't have a CSP thread that can run slash commands
    if(PyErr_Occurred()) {  // Callback may raise an exception. But we don't want to override an existing one.
        PyErr_Print();  // Provide context by printing before we self-destruct
        assert(false);
    }

    assert(context != NULL);
    PythonSlashCommandObject *self = (PythonSlashCommandObject *)context;
    struct slash_command *command = &self->command_heap;

    PyObject *python_func = self->py_slash_func;

    /* It probably doesn't make sense to have a slash command without a corresponding function,
        but we will allow it for now. */
    if (python_func == NULL || python_func == Py_None) {
        return SLASH_ENOENT;
    }

    assert(PyCallable_Check(python_func));

    // TODO Kevin: Support printing help with "help <command>", would probably require print_function_signature() to return char*
    // Check if we should just print help.
    for (int i = 1; i < slash->argc; i++) {
        const char *arg = slash->argv[i];
        if (strncmp(arg, "-h", 3) == 0 || strncmp(arg, "--help", 7) == 0) {

            if (command->args != NULL) {
#if 0
                void slash_command_usage(struct slash *slash, struct slash_command *command);
                slash_command_usage(slash, command);
#else
                printf("%s\n", command->args+1);  // +1 to skip newline
#endif
            }

            return SLASH_SUCCESS;
        }
    }

    /* Create the arguments. */
    PyObject *py_args AUTO_DECREF = NULL;
    PyObject *py_kwargs AUTO_DECREF = NULL;
    if (pycsh_parse_slash_args(self, slash, &py_args, &py_kwargs) != 0) {
        PyErr_Print();
        return SLASH_EINVAL;
    }

    /* Convert to type-hinted types */
    if (SlashCommand_typecast_args(python_func, py_args, py_kwargs) && PyErr_Occurred()) {
        PyErr_Print();
        return SLASH_EINVAL;
    }

    
    /* Call the user provided Python function */
    PyObject * value AUTO_DECREF = PyObject_Call(python_func, py_args, py_kwargs);

    if (PyErr_Occurred()) {
        PyErr_Print();
        return SLASH_EINVAL;
    }

#if 0  // It's probably best to just let any potential error propagate normally
    if (PyErr_Occurred()) {
        /* It may not be clear to the user, that the exception came from the callback,
            we therefore chain unto the existing exception, for better clarity. */
        /* _PyErr_FormatFromCause() seems simpler than PyException_SetCause() and PyException_SetContext() */
        // TODO Kevin: It seems exceptions raised in the CSP thread are ignored.
        _PyErr_FormatFromCause(PyExc_ParamCallbackError, "Error calling Python callback");
    }
#endif
    return SLASH_SUCCESS;//?
}

/**
 * @brief Check that the slash function is callable.
 * 
 * Currently also checks type-hints (if specified).
 * 
 * @param function function to check
 * @param raise_exc Whether to set exception message when returning false.
 * @param short_opts If true, disallows multiple function parameters to start with the same case-sensitive letter.
 * @return true for success
 */
static bool is_valid_slash_func(const PyObject *function, bool raise_exc, bool short_opts) {

    /*We currently allow both NULL and Py_None,
            as those are valid to have on PythonSlashCommandObject */
    if (function == NULL || function == Py_None)
        return true;

    // Get the __code__ attribute of the function, and check that it is a PyCodeObject
    // TODO Kevin: Hopefully it's safe to assume that PyObject_GetAttrString() won't mutate function
    PyObject *func_code AUTO_DECREF = PyObject_GetAttrString((PyObject*)function, "__code__");
    if (!func_code || !PyCode_Check((PyCodeObject *)func_code)) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided function must be callable");
        return false;
    }

    if (!short_opts) {
        return true;
    }

    // Parse function parameters for duplicates of first letter (case-sensitive)
    PyObject *co_varnames AUTO_DECREF = PyObject_GetAttrString(func_code, "co_varnames");
    PyObject *co_argcount_obj AUTO_DECREF = PyObject_GetAttrString(func_code, "co_argcount");
    if (!co_varnames || !PyTuple_Check(co_varnames) || !co_argcount_obj || !PyLong_Check(co_argcount_obj)) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Unable to inspect function parameters");
        return false;
    }
    Py_ssize_t co_argcount = PyLong_AsSsize_t(co_argcount_obj);
    // Use a simple array of 256 (ASCII) for seen first letters
    char seen[256] = {0};
    for (Py_ssize_t i = 0; i < co_argcount; ++i) {
        PyObject *name_obj = PyTuple_GetItem(co_varnames, i); // borrowed
        if (!name_obj || !PyUnicode_Check(name_obj)) {
            continue;
        }
        const char *name = PyUnicode_AsUTF8(name_obj);
        if (!name || !name[0]) {
            continue;
        }
        unsigned char first = (unsigned char)name[0];
        if (seen[first]) {
            if (raise_exc) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Multiple function parameters start with the same letter: '%c'", name[0]);
                PyErr_SetString(PyExc_ValueError, msg);
            }
            return false;
        }
        seen[first] = 1;
    }
    return true;
}

int PythonSlashCommand_set_func(PythonSlashCommandObject *self, PyObject *value, void *closure) {

    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the function attribute");
        return -1;
    }

    if (!is_valid_slash_func(value, true, self->short_opts)) {
        return -1;
    }

    if (value == self->py_slash_func)
        return 0;  // No work to do

    /* Changing the callback to None. */
    if (value == Py_None) {
        if (self->py_slash_func != Py_None) {
            /* We should not arrive here when the old value is Py_None, 
                but prevent Py_DECREF() on at all cost. */
            Py_XDECREF(self->py_slash_func);
        }
        self->py_slash_func = Py_None;
        return 0;
    }

    /* We now know that 'value' is a new callable. */

    /* When replacing a previous callable. */
    if (self->py_slash_func != Py_None) {
        Py_XDECREF(self->py_slash_func);
    }

    Py_INCREF(value);
    self->py_slash_func = value;

    return 0;
}

static PyObject * PythonSlashCommand_get_keep_alive(PythonSlashCommandObject *self, void *closure) {
    return self->keep_alive ? Py_True : Py_False;
}

static int PythonSlashCommand_set_keep_alive(PythonSlashCommandObject *self, PyObject *value, void *closure) {

    if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the keep_alive attribute");
        return -1;
    }

    if (value != Py_True && value != Py_False) {
        PyErr_SetString(PyExc_TypeError, "keep_alive must be True or False");
        return -1;
    }

    if (self->keep_alive && value == Py_False) {
        self->keep_alive = 0;
        Py_DECREF(self);
    } else if (!self->keep_alive && value == Py_True) {
        self->keep_alive = 1;
        Py_INCREF(self);
    }

    return 0;
}

static void PythonSlashCommand_on_remove_hook(struct slash_command *command) {
    
    assert(command != NULL);

    /* We are being removed from the command list.
        Tell Python that it no longer holds a reference to us. */
    PythonSlashCommandObject *py_slash_command = python_wraps_slash_command(command);
    assert(py_slash_command != NULL);
    printf("Removing Python slash command '%s'\n", command->name);
    if (py_slash_command != NULL) {
        PythonSlashCommand_set_keep_alive(py_slash_command, Py_False, NULL);
    }
}

/* Internal API for creating a new PythonSlashCommandObject. */
static PythonSlashCommandObject * SlashCommand_create_new(PyTypeObject *type, char * name, const char * args, const PyObject * py_slash_func, bool short_opts) {

/* NOTE: Overriding an existing PythonSlashCommand here will most likely cause a memory leak. SLIST_REMOVE() will not Py_DECREF() */
#if 0  /* It's okay if a command with this name already exists, Overriding it is an intended feature. */
    if (slash_list_find_name(name)) {
        PyErr_Format(PyExc_ValueError, "Command with name \"%s\" already exists", name);
        return NULL;
    }
#endif

    if (!is_valid_slash_func(py_slash_func, true, short_opts)) {
        return NULL;  // Exception message set by is_valid_slash_func();
    }

    // TODO Kevin: We almost certainly have to strcpy() 'name' and 'args'
    PythonSlashCommandObject *self = (PythonSlashCommandObject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        /* This is likely a memory allocation error, in which case we expect .tp_alloc() to have raised an exception. */
        return NULL;
    }

    self->keep_alive = 1;
    self->short_opts = short_opts;
    Py_INCREF(self);  // Slash command list now holds a reference to this PythonSlashCommandObject
    /* NOTE: If .keep_alive defaults to False, then we should remove this Py_INCREF() */

    /* NULL callback becomes None on a SlashCommandObject instance */
    if (py_slash_func == NULL)
        py_slash_func = Py_None;

    if (PythonSlashCommand_set_func(self, (PyObject *)py_slash_func, NULL) == -1) {
        Py_DECREF(self);
        return NULL;
    }

    if (args) {
        args = safe_strdup(args);
    } else {
        char *docstr_wo_newline CLEANUP_STR = format_python_func_help((PyObject*)py_slash_func, false, short_opts);
        //char *docstr_wo_newline CLEANUP_STR = print_function_signature_w_docstr((PyObject*)py_slash_func, false);

        if (docstr_wo_newline == NULL) {
            Py_DECREF(self);
            return NULL;
        }

        // Prepend newline to make "help <command>" look better
        char *docstr_w_newline = malloc(strlen(docstr_wo_newline)+2);
        docstr_w_newline[0] = '\n';
        strcpy(docstr_w_newline+1, docstr_wo_newline);

        args = docstr_w_newline;
    }

    const struct slash_command temp_command = {
        .args = args,
        .name = safe_strdup(name),
        .func_ctx = SlashCommand_func,
        .context = (void *)self,  // We will use this to find the PythonSlashCommandObject instance
        .completer = slash_path_completer,  // TODO Kevin: It should probably be possible for the user to change the completer.
        //.on_remove_hook = PythonSlashCommand_on_remove_hook,
    };

    /* NOTE: This memcpy() seems to be the best way to deal with the fact that self->command->func is const  */
    memcpy(&self->command_heap, &temp_command, sizeof(struct slash_command));
    self->slash_command_object.command = &self->command_heap;

    struct slash_command * existing = slash_list_find_name(self->command_heap.name);

    int res = slash_list_add(&self->command_heap);
    if (res < 0) {
        fprintf(stderr, "Failed to add slash command \"%s\" while loading APM (return status: %d)\n", self->command_heap.name, res);
        Py_DECREF(self);
        return NULL;
    } else if (res > 0) {
        printf("Slash command '%s' is overriding an existing command\n", self->command_heap.name);
        
    }

    return self;
}
#if 0 // TODO Kevin: (Causes seg fault when paramteres are dealocated) Find out what went wrong here. After outcommenting this function we can exit without any hang ups or seg faults.
__attribute__((destructor(151))) static void remove_python_slash_commands(void) {
    struct slash_command * cmd;
    slash_list_iterator i = {};
    while ((cmd = slash_list_iterate(&i)) != NULL) {
        PythonSlashCommandObject *py_slash_command = python_wraps_slash_command(cmd);
        if (py_slash_command != NULL) {
            PythonSlashCommand_set_keep_alive(py_slash_command, Py_False, NULL);
            // TODO Kevin: Remove from slash list here?
        }
    }
}
#endif
#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 13
#define  _Py_IsFinalizing Py_IsFinalizing
#endif

static void PythonSlashCommand_dealloc(PythonSlashCommandObject *self) {

    /* Calling Py_XDECREF() while Python is finalizing, seems to cause a segfault (due to the GIL not being held)
        (->tp_free() seems fine however), So that is why we check for _Py_IsFinalizing(). */
    if (!_Py_IsFinalizing() && self->py_slash_func != NULL && self->py_slash_func != Py_None) {
        Py_XDECREF(self->py_slash_func);
        self->py_slash_func = NULL;
    }

    struct slash_command * existing = slash_list_find_name(self->command_heap.name);
    //PythonSlashCommandObject *py_slash_command = python_wraps_slash_command(existing);

    /* This check will fail if a new slash command is added to the list, before we are garbage collected.
        This is actually quite likely to happen if the same Python 'APM' is loaded multiple times. */
    if (existing == &self->command_heap) {
        slash_list_remove(((SlashCommandObject*)self)->command);
    }

    free(self->command_heap.name);
    free((char*)self->command_heap.args);
    //self->command_heap.name = NULL;
    //((char*)self->command_heap.args) = NULL;
    // Get the type of 'self' in case the user has subclassed 'SlashCommand'.
    // Py_TYPE(self)->tp_free((PyObject *) self);
    PyTypeObject *baseclass = pycsh_get_base_dealloc_class(&PythonSlashCommandType);
    baseclass->tp_dealloc((PyObject*)self);
}

static PyObject * PythonSlashCommand_new(PyTypeObject *type, PyObject * args, PyObject * kwds) {

    char * name;
    PyObject * function;
    char * slash_args = NULL;
    int short_opts = 1;  // Using bool here is apparently not supported

    static char *kwlist[] = {"name", "function", "args", "short_opts", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO|zp", kwlist, &name, &function, &slash_args, &short_opts))
        return NULL;  // TypeError is thrown

    PythonSlashCommandObject * python_slash_command = SlashCommand_create_new(type, name, slash_args, function, short_opts);
    if (python_slash_command == NULL) {
        // Assume exception message to be set by SlashCommand_create_new()
        /* physaddr should be freed in dealloc() */
        return NULL;
    }

    /* return should steal the reference created by SlashCommand_create_new() */
    return (PyObject *)python_slash_command;
}

static PyObject * PythonSlashCommand_get_function(PythonSlashCommandObject *self, void *closure) {
    return Py_NewRef(self->py_slash_func);
}

static PyObject * PythonSlashCommand_call(PythonSlashCommandObject *self, PyObject *args, PyObject *kwds) {
    assert(self->py_slash_func);
    assert(PyCallable_Check(self->py_slash_func));
    /* Call the user provided Python function. Return whatever it returns, and let errors propagate normally. */
    return PyObject_Call(self->py_slash_func, args, kwds);
}

static PyGetSetDef PythonSlashCommand_getsetters[] = {
    {"keep_alive", (getter)PythonSlashCommand_get_keep_alive, (setter)PythonSlashCommand_set_keep_alive,
     "Whether the slash command should remain in the command list, when all Python references are lost", NULL},
    {"function", (getter)PythonSlashCommand_get_function, (setter)PythonSlashCommand_set_func,
     "function invoked by the slash command", NULL},
    // TODO Kevin: Maybe PythonSlashCommandObject should have getsetters for 'name' and 'args'
    {NULL, NULL, NULL, NULL}  /* Sentinel */
};

PyTypeObject PythonSlashCommandType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pycsh.PythonSlashCommand",
    .tp_doc = "Slash command created in Python.",
    .tp_basicsize = sizeof(PythonSlashCommandType),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = PythonSlashCommand_new,
    .tp_dealloc = (destructor)PythonSlashCommand_dealloc,
    .tp_getset = PythonSlashCommand_getsetters,
    // .tp_str = (reprfunc)SlashCommand_str,
    // .tp_richcompare = (richcmpfunc)SlashCommand_richcompare,
    .tp_base = &SlashCommandType,
    .tp_call = (PyCFunctionWithKeywords)PythonSlashCommand_call,
};
