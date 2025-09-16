
#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <param/param.h>
#include <pycsh/parameter.h>


typedef struct {
    PyObject_HEAD
    /* Type-specific fields go here. */

	param_t * param;

	int host;
	int timeout;
	int retries;  // TODO Kevin: The 'retries' code was implemented rather hastily, consider refactoring of removing it. 
	int paramver;
	bool remote : 1;
	int verbose;

	/* Cached Python value of the parameter,
		will be NULL before we query it. */
	PyObject * value;

	/* TODO Kevin: Can't really decide if we should have indexes here */
	//PyObject * indexes;

} ValueProxyObject;

extern PyTypeObject ValueProxyType;

/* Create a ValueProxy object from a Parameter instance directly. */
PyObject * pycsh_ValueProxy_from_Parameter(PyTypeObject *type, ParameterObject * param, bool remote);
