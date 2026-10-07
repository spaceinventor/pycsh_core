/*
 * param_py.c
 *
 * Wrappers for lib/param/src/param/param_slash.c
 *
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <apm/environment.h>

#include "pycshconfig.h"

#include <pycsh/pycsh.h>
#include <pycsh/utils.h>


static void _add_string_to_dict(const char *name, void *ctx) {

    PyObject *dict = (PyObject *)ctx;

    PyObject *key AUTO_DECREF = PyUnicode_FromString(name);
    if (key == NULL) {
        return;
    }

    PyObject *value AUTO_DECREF = PyUnicode_FromString(csh_getvar(name));
    if (value == NULL) {
        return;
    }

    PyDict_SetItem(dict, key, value);

}

PyObject * pycsh_var_show(PyObject * self, PyObject * args) {
	(void)self;
	(void)args;

	PyObject *dict = PyDict_New();
	if (dict == NULL) {
		return NULL;
	}

	csh_foreach_var(_add_string_to_dict, dict);

	return dict;
}

PyObject * pycsh_var_get(PyObject * self, PyObject * args, PyObject * kwds) {
	(void)self;

	char * name;  // Raw argument object/type passed. Identify its type when needed.

	static char *kwlist[] = {"name", NULL};
	
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &name)) {
		return NULL;  // TypeError is thrown
	}

	const char * const value = csh_getvar(name);

	if (value == NULL) {  // Did not find a match.
        PyErr_Format(PyExc_KeyError, "Variable '%s' not found", name);
        return NULL;
    }

	return PyUnicode_FromString(value);
}

PyObject * pycsh_var_set(PyObject * self, PyObject * args, PyObject * kwds) {
	(void)self;

	char * name;
    PyObject * value_obj;  /* We will stringify whatever the user passes. Easiest way to also handle ints and such. */

	static char *kwlist[] = {"name", "value", NULL};
	
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "sO", kwlist, &name, &value_obj)) {
		return NULL;  // TypeError is thrown
	}

    PyObject * const str_value_obj AUTO_DECREF = PyObject_Str(value_obj);
    if (str_value_obj == NULL) {
        return NULL;  // TypeError is thrown
    }

    const char * const value = PyUnicode_AsUTF8(str_value_obj);
    if (value == NULL) {
        return NULL;  // TypeError is thrown
    }

    if (csh_putvar(name, value)) {
        PyErr_Format(PyExc_RuntimeError, "Failed to set variable '%s' to value '%s'", name, value);
        return NULL;
    }

	Py_RETURN_NONE;
}

PyObject * pycsh_var_unset(PyObject * self, PyObject * args, PyObject * kwds) {
	(void)self;

	char * name;  // Raw argument object/type passed. Identify its type when needed.

	static char *kwlist[] = {"name", NULL};
	
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "s", kwlist, &name)) {
		return NULL;  // TypeError is thrown
	}

	const int result = csh_delvar(name);

	if (result) {  // Did not find a match.
        PyErr_Format(PyExc_KeyError, "Variable '%s' not found", name);
        return NULL;
    }

	Py_RETURN_NONE;
}
