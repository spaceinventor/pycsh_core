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

	/* TODO Kevin: Disallow or Unit test what happens when a user changes
		the callback of an existing non-static (C) Parameter. */
	PyObject *callback;
} ParameterObject;


extern PyObject * PyExc_ParamCallbackError;
extern PyObject * PyExc_InvalidParameterTypeError;

extern PyDictObject * param_callback_dict;

extern PyMethodDef Parameter_class_methods[2];

/**
 * @brief Shared callback for all param_t's wrapped by a Parameter instance,
 * 	that must call a PyObject* callback function.
 */
void Parameter_callback(param_t * param, int offset);


// Source: https://chat.openai.com
/**
 * @brief Check that the callback accepts exactly one Parameter and one integer,
 *  as specified by "void (*callback)(struct param_s * param, int offset)"
 * 
 * Currently also checks type-hints (if specified).
 * 
 * @param callback function to check
 * @param raise_exc Whether to set exception message when returning false.
 * @return true for success
 */
bool is_valid_callback(const PyObject *callback, bool raise_exc);


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

ParameterObject * Parameter_create_new(PyTypeObject *type, uint16_t id, param_type_e param_type, uint32_t mask, char * name, char * unit, char * docstr, int array_size, const PyObject * callback, int host, int timeout, int retries, int paramver);
