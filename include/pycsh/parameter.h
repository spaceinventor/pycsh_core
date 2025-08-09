/*
 * parameter.h
 *
 * Contains the Parameter base class.
 *
 *  Created on: Apr 28, 2022
 *      Author: Kevin Wallentin Carlsen
 */

#pragma once

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <param/param.h>

typedef enum {
	PY_PARAM_FREE_NO,  /* Borrowed param_t, don't free at all. */
	PY_PARAM_FREE_LIST_DESTROY,  /* Call `param_list_destroy(self->param)`, indicating that we fully own the parameter. */
	PY_PARAM_FREE_PARAM_T,  /* `free(self->param)`, but not deeper buffers (Advanced users only) */
} py_param_free_e;

typedef struct {
    PyObject_HEAD
    /* Type-specific fields go here. */
	PyTypeObject *type;  // Best Python representation of the parameter type, i.e 'int' for uint32.

	param_t * param;

	int host;
	int timeout;
	int retries;  // TODO Kevin: The 'retries' code was implemented rather hastily, consider refactoring of removing it. 
	int paramver;

	py_param_free_e free_in_dealloc;
} ParameterObject;

extern PyTypeObject ParameterType;

/**
 * @brief Takes a param_t and creates and or returns the wrapping ParameterObject.
 * 
 * @param type 
 * @param param param_t to find the ParameterObject for.
 * @param callback 
 * @param host 
 * @param timeout 
 * @param retries 
 * @param paramver 
 * @param free_in_dealloc If true, we will free self->param in our deallocator. 
 * @return ParameterObject* The wrapping ParameterObject.
 */
PyObject * pycsh_Parameter_from_param(PyTypeObject *type, param_t * param, const PyObject * callback, int host, int timeout, int retries, int paramver, py_param_free_e free_in_dealloc);
