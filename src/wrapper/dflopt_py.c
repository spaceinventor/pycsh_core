/*
 * dflopt_py.c
 *
 * Wrappers for lib/slash/src/dflopt.c
 *
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "dflopt_py.h"

#include <pycsh/pycsh.h>
#include <apm/csh_api.h>


// TODO Kevin: These differ from the newest version of slash/csh

PyObject * pycsh_slash_node(PyObject * self, PyObject * args, PyObject * kwds) {

	PyObject * node = NULL;
	int verbose = pycsh_dfl_verbose;

	static char *kwlist[] = {"node", "verbose", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|Oi", kwlist, &node, &verbose)) {
		return NULL;  // TypeError is thrown
	}

	if (node == NULL) {  // No argument passed
		if (verbose >= 2) {
			printf("Default node = %d\n", pycsh_dfl_node);
		}
	} else if (PyLong_Check(node)) {

		pycsh_dfl_node = PyLong_AsUnsignedLong(node);
		if (verbose >= 1) {
			printf("Set default node to %d\n", pycsh_dfl_node);
		}
	} else {
		PyErr_SetString(PyExc_TypeError, "'node' argument must be an int");
		return NULL;
	}

	return Py_BuildValue("i", pycsh_dfl_node);
}

PyObject * pycsh_slash_timeout(PyObject * self, PyObject * args, PyObject * kwds) {

	int timeout = -1;
	int verbose = pycsh_dfl_verbose;

	static char *kwlist[] = {"timeout", "verbose", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|ii:timeout", kwlist, &timeout, &verbose)) {
		return NULL;  // TypeError is thrown
	}

	if (timeout == -1) {
		if (verbose >= 2) {
			printf("Default timeout = %d\n", pycsh_dfl_timeout);
		}
	} else {
		pycsh_dfl_timeout = timeout;
		if (verbose >= 1) {
			printf("Set default timeout to %d\n", pycsh_dfl_timeout);
		}
	}

	return Py_BuildValue("i", pycsh_dfl_timeout);
}

PyObject * pycsh_slash_verbose(PyObject * self, PyObject * args) {

	int verbose = INT_MIN;

	if (!PyArg_ParseTuple(args, "|i", &verbose)) {
		return NULL;  // TypeError is thrown
	}

	if (verbose == INT_MIN && pycsh_dfl_verbose >= 0) {
		printf("Default verbose = %d\n", pycsh_dfl_verbose);
	} else {
		pycsh_dfl_verbose = verbose;
		if (pycsh_dfl_verbose >= 0) {
			printf("Set default verbosity to %d\n", pycsh_dfl_verbose);
		}
	}

	return Py_BuildValue("i", pycsh_dfl_verbose);
}
