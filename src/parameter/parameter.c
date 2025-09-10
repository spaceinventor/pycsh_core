/*
 * parameter.c
 *
 * Contains the Parameter base class.
 *
 *  Created on: Apr 28, 2022
 *      Author: Kevin Wallentin Carlsen
 */

#include <pycsh/parameter.h>

#include "structmember.h"

#include <param/param.h>

#include <pycsh/pycsh.h>
#include "parameterarray.h"
#include <pycsh/utils.h>

/* Maps param_t to its corresponding PythonParameter for use by C callbacks. */
PyDictObject * param_callback_dict = NULL;

/* 1 for success. Compares the wrapped param_t for parameters, otherwise 0. Assumes self to be a ParameterObject. */
static int Parameter_equal(PyObject *self, PyObject *other) {
	if (PyObject_TypeCheck(other, &ParameterType) && (memcmp(&(((ParameterObject *)other)->param), &(((ParameterObject *)self)->param), sizeof(param_t)) == 0))
		return 1;
	return 0;
}

static PyObject * Parameter_richcompare(PyObject *self, PyObject *other, int op) {

	PyObject *result = Py_NotImplemented;

	switch (op) {
		// case Py_LT:
		// 	break;
		// case Py_LE:
		// 	break;
		case Py_EQ:
			result = (Parameter_equal(self, other)) ? Py_True : Py_False;
			break;
		case Py_NE:
			result = (Parameter_equal(self, other)) ? Py_False : Py_True;
			break;
		// case Py_GT:
		// 	break;
		// case Py_GE:
		// 	break;
	}

    Py_XINCREF(result);
    return result;
}

static PyObject * Parameter_str(ParameterObject *self) {
	char buf[100];
	sprintf(buf, "[id:%i|node:%i] %s | %s", self->param->id, *self->param->node, self->param->name, self->type->tp_name);
	return Py_BuildValue("s", buf);
}

static PyTypeObject * get_arrayparameter_subclass(PyTypeObject *type) {

	// Get the __subclasses__ method
    PyObject *subclasses_method AUTO_DECREF = PyObject_GetAttrString((PyObject *)type, "__subclasses__");
    if (subclasses_method == NULL) {
        return NULL;
    }

	// NOTE: .__subclasses__() is not recursive, but this is currently not an issue with ParameterArray and PythonArrayParameter

    // Call the __subclasses__ method
    PyObject *subclasses_list AUTO_DECREF = PyObject_CallObject(subclasses_method, NULL);
    if (subclasses_list == NULL) {
        return NULL;
    }

    // Ensure the result is a list
    if (!PyList_Check(subclasses_list)) {
        PyErr_SetString(PyExc_TypeError, "__subclasses__ did not return a list");
        return NULL;
    }

    // Iterate over the list of subclasses
    Py_ssize_t num_subclasses = PyList_Size(subclasses_list);
    for (Py_ssize_t i = 0; i < num_subclasses; i++) {
        PyObject *subclass = PyList_GetItem(subclasses_list, i);  // Borrowed reference
        if (subclass == NULL) {
            return NULL;
        }

		int is_subclass = PyObject_IsSubclass(subclass, (PyObject*)&ParameterArrayType);
        if (is_subclass < 0) {
			return NULL;
		}
		
		PyErr_Clear();
		if (is_subclass) {
			return (PyTypeObject*)subclass;
		}
    }

	PyErr_Format(PyExc_TypeError, "Failed to find ArrayParameter variant of class %s", type->tp_name);
	return NULL;
}

/* Create a Python Parameter object from a param_t pointer directly. */
PyObject * pycsh_Parameter_from_param(PyTypeObject *type, param_t * param, const PyObject * callback, int host, int timeout, int retries, int paramver, py_param_free_e free_in_dealloc) {
	if (param == NULL) {
 		return NULL;
	}
	// This parameter is already wrapped by a ParameterObject, which we may return instead.
	ParameterObject * existing_parameter;
	if ((existing_parameter = Parameter_wraps_param(param)) != NULL) {
		/* TODO Kevin: How should we handle when: host, timeout, retries and paramver are different for the existing parameter? */
		return (PyObject*)Py_NewRef(existing_parameter);
	}

	if (param->array_size <= 1 && type == &ParameterArrayType) {
		PyErr_SetString(PyExc_TypeError, 
			"Attempted to create a ParameterArray instance, for a non array parameter.");
		return NULL;
	} else if (param->array_size > 1) {  		   // If the parameter is an array.
		type = get_arrayparameter_subclass(type);  // We create a ParameterArray instance instead.
		if (type == NULL) {
			return NULL;
		}
		// If you listen really carefully here, you can hear OOP idealists, screaming in agony.
		// On a more serious note, I'm amazed that this even works at all.
	}

	ParameterObject *self = (ParameterObject *)type->tp_alloc(type, 0);

	if (self == NULL) {
		return NULL;
	}

	{   /* Add ourselves to the callback/lookup dictionary */
		PyObject *key AUTO_DECREF = PyLong_FromVoidPtr(param);
		assert(key != NULL);
		assert(!PyErr_Occurred());
		assert(PyDict_GetItem((PyObject*)param_callback_dict, key) == NULL);
		int set_res = PyDict_SetItem((PyObject*)param_callback_dict, key, (PyObject*)self);
		assert(set_res == 0);  // Allows the param_t callback to find the corresponding ParameterObject.
		assert(PyDict_GetItem((PyObject*)param_callback_dict, key) != NULL);
		assert(!PyErr_Occurred());

		assert(self);
		assert(self->ob_base.ob_type);
		/* The parameter linked list should maintain an eternal reference to Parameter() instances, and subclasses thereof (with the exception of PythonParameter() and its subclasses).
			This check should ensure that: Parameter("name") is Parameter("name") == True.
			This check doesn't apply to PythonParameter()'s, because its reference is maintained by .keep_alive */
		int is_pythonparameter = PyObject_IsSubclass((PyObject*)(type), (PyObject*)&PythonParameterType);
        if (is_pythonparameter < 0) {
			assert(false);
			return NULL;
		}

		if (is_pythonparameter) {
			Py_DECREF(self);  // param_callback_dict should hold a weak reference to self
		}
	}

	self->host = (host != INT_MIN) ? host : *param->node;
	self->param = param;
	self->timeout = timeout;
	self->retries = retries;
	self->paramver = paramver;
	self->free_in_dealloc = free_in_dealloc;

	self->type = (PyTypeObject *)pycsh_util_get_type((PyObject *)self, NULL);

    return (PyObject *) self;
}

/* May perform black magic and return a ParameterArray instead of the specified type. */
static PyObject * Parameter_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {

	static char *kwlist[] = {"param_identifier", "node", "host", "paramver", "timeout", "retries", NULL};

	PyObject * param_identifier;  // Raw argument object/type passed. Identify its type when needed.
	int node = pycsh_dfl_node;
	int host = INT_MIN;
	int paramver = 2;
	int timeout = pycsh_dfl_timeout;
	int retries = 1;

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|iiiii", kwlist, &param_identifier, &node, &host, &paramver, &timeout, &retries)) {
		return NULL;  // TypeError is thrown
	}

	param_t * param = _pycsh_util_find_param_t(param_identifier, node);

	if (param == NULL)  // Did not find a match.
		return NULL;  // Raises TypeError or ValueError.

    return pycsh_Parameter_from_param(type, param, NULL, host, timeout, retries, paramver, PY_PARAM_FREE_NO);
}

static PyObject * Parameter_get_name(ParameterObject *self, void *closure) {
	return Py_BuildValue("s", self->param->name);
}

static PyObject * Parameter_get_unit(ParameterObject *self, void *closure) {
	return Py_BuildValue("s", self->param->unit);
}

static PyObject * Parameter_get_docstr(ParameterObject *self, void *closure) {
	return Py_BuildValue("s", self->param->docstr);
}

static PyObject * Parameter_get_id(ParameterObject *self, void *closure) {
	return Py_BuildValue("H", self->param->id);
}

static PyObject * Parameter_get_node(ParameterObject *self, void *closure) {
	return Py_BuildValue("H", *self->param->node);
}

static PyObject * Parameter_get_storage_type(ParameterObject *self, void *closure) {
	return Py_BuildValue("H", self->param->vmem->type);
}

/* This will change self->param to be one by the same name at the specified node. */
static int Parameter_set_node(ParameterObject *self, PyObject *value, void *closure) {

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the node attribute");
        return -1;
    }

	if(!PyLong_Check(value)) {
		PyErr_SetString(PyExc_TypeError,
                        "The node attribute must be set to an int");
        return -1;
	}

	uint16_t node;

	// This is pretty stupid, but seems to be the easiest way to convert a long to short using Python.
	PyObject * value_tuple AUTO_DECREF = PyTuple_Pack(1, value);
	if (!PyArg_ParseTuple(value_tuple, "H", &node)) {
		return -1;
	}

	param_t * param = param_list_find_id(node, self->param->id);

	if (param == NULL)  // Did not find a match.
		return -1;  // Raises either TypeError or ValueError.

	self->param = param;

	return 0;
}

static PyObject * Parameter_get_host(ParameterObject *self, void *closure) {
	if (self->host != INT_MIN)
		return Py_BuildValue("i", self->host);
	Py_RETURN_NONE;
}

static int Parameter_set_host(ParameterObject *self, PyObject *value, void *closure) {

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the host attribute");
        return -1;
    }

	if (value == Py_None) {  // Set the host to INT_MIN, which we use as no host.
		self->host = INT_MIN;
		return 0;
	}

	if(!PyLong_Check(value)) {
		PyErr_SetString(PyExc_TypeError,
                        "The host attribute must be set to an int or None");
        return -1;
	}

	int host = _PyLong_AsInt(value);

	if (PyErr_Occurred())
		return -1;  // 'Reraise' the current exception.

	self->host = host;

	return 0;
}

static PyObject * Parameter_gettype(ParameterObject *self, void *closure) {
	Py_INCREF(self->type);
	return (PyObject *)self->type;
}

#ifdef OLD_PARAM_API_ERROR

static PyObject * Parameter_get_oldvalue(ParameterObject *self, void *closure) {
	PyErr_SetString(PyExc_AttributeError, "`Parameter.remote_value` and `Parameter.remote_value` has been changed to: `.get_value()`, `.set_value()`, `.get_value_array()` and `.set_value_array()`.");
	return NULL;
}

static int Parameter_set_oldvalue(ParameterObject *self, PyObject *value, void *closure) {
	PyErr_SetString(PyExc_AttributeError, "`Parameter.remote_value` and `Parameter.remote_value` has been changed to: `.get_value()`, `.set_value()`, `.get_value_array()` and `.set_value_array()`.");
	return -1;
}

#else  /* OLD_PARAM_API_ERROR */

static PyObject * _Parameter_get_value(ParameterObject *self, int remote) {

	if (PyErr_WarnEx(PyExc_DeprecationWarning, "`Parameter.remote_value` and `Parameter.remote_value` has been changed to: `.get_value()`, `.set_value()`, `.get_value_array()` and `.set_value_array()`", 2) < 0) {
		return NULL;
	}

	if (self->param->array_size > 1 && self->param->type != PARAM_TYPE_STRING)
		return _pycsh_util_get_array(self->param, remote, self->host, self->timeout, self->retries, self->paramver, pycsh_dfl_verbose);
	return _pycsh_util_get_single(self->param, INT_MIN, remote, self->host, self->timeout, self->retries, self->paramver, pycsh_dfl_verbose);
}

static int _Parameter_set_value(ParameterObject *self, PyObject *value, int remote) {

	if (PyErr_WarnEx(PyExc_DeprecationWarning, "`Parameter.remote_value` and `Parameter.remote_value` has been changed to: `.get_value()`, `.set_value()`, `.get_value_array()` and `.set_value_array()`", 2) < 0) {
		return -2;
	}

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the value attribute");
        return -1;
    }

	if (self->param->array_size > 1 && self->param->type != PARAM_TYPE_STRING && self->param->type != PARAM_TYPE_DATA)  // Is array parameter
		return _pycsh_util_set_array(self->param, value, self->host, self->timeout, self->retries, self->paramver, pycsh_dfl_verbose);
	return _pycsh_util_set_single(self->param, value, INT_MIN, self->host, self->timeout, self->retries, self->paramver, remote, pycsh_dfl_verbose);  // Normal parameter
}

static PyObject * Parameter_get_remote_value(ParameterObject *self, void *closure) {
	return _Parameter_get_value(self, 1);
}

static PyObject * Parameter_get_cached_value(ParameterObject *self, void *closure) {
	return _Parameter_get_value(self, 0);
}

static int Parameter_set_remote_value(ParameterObject *self, PyObject *value, void *closure) {
	return _Parameter_set_value(self, value, 1);
}

static int Parameter_set_cached_value(ParameterObject *self, PyObject *value, void *closure) {
	return _Parameter_set_value(self, value, 0);
}

#endif  /* OLD_PARAM_API_ERROR */


PyObject * Parameter_get_value(ParameterObject * self, PyObject * args, PyObject * kwds) {
	unsigned int index = INT_MIN;
    int remote = true;
	int verbose = pycsh_dfl_verbose;

    static char *kwlist[] = {"index", "remote", "verbose", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|Ipi:get_value", kwlist, &index, &remote, &verbose)) {
        return NULL;
    }

	param_t *param = self->param;

	return _pycsh_util_get_single(param, index, remote, self->host, self->timeout, self->retries, self->paramver, verbose);
}

PyObject * Parameter_set_value(ParameterObject * self, PyObject * args, PyObject * kwds) {
	PyObject * value;
	unsigned int index = 0;
    int remote = true;
	int verbose = pycsh_dfl_verbose;

    static char *kwlist[] = {"value", "index", "remote", "verbose", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|Ipi:set_value", kwlist, &value, &index, &remote, &verbose)) {
        return NULL;
    }

	param_t *param = self->param;

	if (_pycsh_util_set_single(param, value, index, self->host, self->timeout, self->retries, self->paramver, remote, verbose) != 0) {
		return NULL;
	}

	Py_RETURN_NONE;
}

PyObject * Parameter_get_value_array(ParameterObject * self, PyObject * args, PyObject * kwds) {

	PyObject * indexes = NULL;
    int remote = true;
	int verbose = pycsh_dfl_verbose;

    static char *kwlist[] = {"indexes", "remote", "verbose", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|Opi:get_value_array", kwlist, &indexes, &remote, &verbose)) {
        return NULL;
    }

	param_t * param = self->param;

	return _pycsh_util_get_array_indexes(param, indexes, remote, self->host, self->timeout, self->retries, self->paramver, verbose);
}

PyObject * Parameter_set_value_array(ParameterObject * self, PyObject * args, PyObject * kwds) {

	PyObject * values;
	PyObject * indexes = NULL;
    int remote = true;
	int verbose = pycsh_dfl_verbose;

    static char *kwlist[] = {"values", "indexes", "remote", "verbose", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O|Opii:set_value_array", kwlist, &values, &indexes, &remote, &verbose)) {
        return NULL;
    }

	param_t * param = self->param;

	return _pycsh_util_set_array_indexes(param, values, indexes, remote, self->host, self->timeout, self->retries, self->paramver, verbose);
}



static PyObject * Parameter_is_vmem(ParameterObject *self, void *closure) {
	// I believe this is the most appropriate way to check for vmem parameters.
	PyObject * result = self->param->vmem == NULL ? Py_False : Py_True;
	Py_INCREF(result);
	return result;
}

static PyObject * Parameter_get_timeout(ParameterObject *self, void *closure) {
	return Py_BuildValue("i", self->timeout);
}

static int Parameter_set_timeout(ParameterObject *self, PyObject *value, void *closure) {

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the timeout attribute");
        return -1;
    }

	if (value == Py_None) {
		self->timeout = pycsh_dfl_timeout;
		return 0;
	}

	if(!PyLong_Check(value)) {
		PyErr_SetString(PyExc_TypeError,
                        "The timeout attribute must be set to an int or None");
        return -1;
	}

	int timeout = _PyLong_AsInt(value);

	if (PyErr_Occurred())
		return -1;  // 'Reraise' the current exception.

	self->timeout = timeout;

	return 0;
}

static PyObject * Parameter_getmask(ParameterObject *self, void *closure) {
	return Py_BuildValue("I", self->param->mask);
}

static PyObject * Parameter_gettimestamp(ParameterObject *self, void *closure) {
	/* TODO Kevin: Convert to float with `->tv_nsec` as decimals */
	return Py_BuildValue("I", self->param->timestamp->tv_sec);
}

static PyObject * Parameter_get_retries(ParameterObject *self, void *closure) {
	return Py_BuildValue("i", self->retries);
}

#if PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 13
#define  _PyLong_AsInt PyLong_AsInt
#endif

static int Parameter_set_retries(ParameterObject *self, PyObject *value, void *closure) {

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete the retries attribute");
        return -1;
    }

	if (value == Py_None) {
		self->retries = 1;
		return 0;
	}

	if(!PyLong_Check(value)) {
		PyErr_SetString(PyExc_TypeError,
                        "The retries attribute must be set to an int or None");
        return -1;
	}

	int retries = _PyLong_AsInt(value);

	if (PyErr_Occurred())
		return -1;  // 'Reraise' the current exception.

	self->retries = retries;

	return 0;
}

static long Parameter_hash(ParameterObject *self) {
	/* Use the ID of the parameter as the hash, as it is assumed unique. */
    return self->param->id;
}

static void Parameter_dealloc(ParameterObject *self) {

	{   /* Remove ourselves from the callback/lookup dictionary */
        PyObject *key AUTO_DECREF = PyLong_FromVoidPtr(self->param);
        assert(key != NULL);
        if (PyDict_GetItem((PyObject*)param_callback_dict, key) != NULL) {
            PyDict_DelItem((PyObject*)param_callback_dict, key);
        }
    }

	if (self->free_in_dealloc == PY_PARAM_FREE_PARAM_T) {
		free(self->param);
	} else if (self->free_in_dealloc == PY_PARAM_FREE_LIST_DESTROY) {
		/* With `param_list_destroy(self->param);` users of `self->free_in_dealloc` must take we to not resuse param_t buffer fields with param in list. */
		param_list_destroy(self->param);
	} else if (param_list_find_id(*self->param->node, self->param->id) != self->param && self->param != NULL) {
		/* Somehow we hold a reference to a parameter that is not in the list,
			this should only be possible if it was "list forget"en, after we wrapped it.
			It should therefore follow that we are now responsible for its memory (<-- TODO Kevin: Is this true? It has probably already been freed).
			We must therefore free() it, now that we are being deallocated.
			We check that (self->param != NULL), just in case we allow that to raise exceptions in the future. */
		param_list_destroy(self->param);
	} 

	PyTypeObject *baseclass = pycsh_get_base_dealloc_class(&ParameterType);
    baseclass->tp_dealloc((PyObject*)self);
}

/* 
The Python binding 'Parameter' class exposes most of its attributes through getters, 
as only its 'value', 'host' and 'node' are mutable, and even those are through setters.
*/
static PyGetSetDef Parameter_getsetters[] = {

#if 1  // param_t getsetters
	{"name", (getter)Parameter_get_name, NULL,
     "Returns the name of the wrapped param_t C struct.", NULL},
    {"unit", (getter)Parameter_get_unit, NULL,
     "The unit of the wrapped param_t c struct as a string or None.", NULL},
	{"docstr", (getter)Parameter_get_docstr, NULL,
     "The help-text of the wrapped param_t c struct as a string or None.", NULL},
	{"id", (getter)Parameter_get_id, NULL,
     "id of the parameter", NULL},
	{"type", (getter)Parameter_gettype, NULL,
     "type of the parameter", NULL},
	{"mask", (getter)Parameter_getmask, NULL,
     "mask of the parameter", NULL},
	{"timestamp", (getter)Parameter_gettimestamp, NULL,
     "timestamp of the parameter", NULL},
	{"node", (getter)Parameter_get_node, (setter)Parameter_set_node,
     "node of the parameter", NULL},
#endif

#if 1  // Parameter getsetters
	{"host", (getter)Parameter_get_host, (setter)Parameter_set_host,
     "host of the parameter", NULL},
#ifdef OLD_PARAM_API_ERROR
	{"remote_value", (getter)Parameter_get_oldvalue, (setter)Parameter_set_oldvalue,
     "get/set the remote (and cached) value of the parameter", NULL},
	{"cached_value", (getter)Parameter_get_oldvalue, (setter)Parameter_set_oldvalue,
     "get/set the cached value of the parameter", NULL},
#else  /* OLD_PARAM_API_ERROR */
	{"remote_value", (getter)Parameter_get_remote_value, (setter)Parameter_set_remote_value,
     "get/set the remote (and cached) value of the parameter", NULL},
	{"cached_value", (getter)Parameter_get_cached_value, (setter)Parameter_set_cached_value,
     "get/set the cached value of the parameter", NULL},
#endif  /* OLD_PARAM_API_ERROR */
	{"is_vmem", (getter)Parameter_is_vmem, NULL,
     "whether the parameter is a vmem parameter", NULL},
	{"storage_type", (getter)Parameter_get_storage_type, NULL,
     "storage type of the parameter", NULL},
	{"timeout", (getter)Parameter_get_timeout, (setter)Parameter_set_timeout,
     "timeout of the parameter", NULL},
	{"retries", (getter)Parameter_get_retries, (setter)Parameter_set_retries,
     "available retries of the parameter", NULL},
#endif
    {NULL, NULL, NULL, NULL}  /* Sentinel */
};

static PyMethodDef Parameter_methods[] = {
    {"get_value", (PyCFunction)Parameter_get_value, METH_VARARGS | METH_KEYWORDS, PyDoc_STR("Returns the value of a single index, so the result will not be iterable (with the exception of string parameters, "\
		"which always returns the whole string, ignoring the `index` argument).")},
    {"set_value", (PyCFunction)Parameter_set_value, METH_VARARGS | METH_KEYWORDS, PyDoc_STR("Sets the value of the parameter.")},
    {"get_value_array", (PyCFunction)Parameter_get_value_array, METH_VARARGS | METH_KEYWORDS, PyDoc_STR("Always return an iterable from the specified sequence. By default return the whole parameter. "\
        "Examples for the following parameter `set index_array [0 1 2 3 4 5 6 7]`:")},
    {"set_value_array", (PyCFunction)Parameter_set_value_array, METH_VARARGS | METH_KEYWORDS, PyDoc_STR("Sets the local cached value of the parameter.")},
    {NULL, NULL, 0, NULL}
};

PyTypeObject ParameterType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pycsh.Parameter",
    .tp_doc = "Wrapper utility class for libparam parameters.",
    .tp_basicsize = sizeof(ParameterObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = Parameter_new,
    .tp_dealloc = (destructor)Parameter_dealloc,
	.tp_getset = Parameter_getsetters,
	// .tp_members = Parameter_members,
	.tp_methods = Parameter_methods,
	.tp_str = (reprfunc)Parameter_str,
	.tp_richcompare = (richcmpfunc)Parameter_richcompare,
	.tp_hash = (hashfunc)Parameter_hash,
};
