/*
 * parameter.c
 *
 * Contains the Parameter base class.
 *
 *  Created on: Apr 28, 2022
 *      Author: Kevin Wallentin Carlsen
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <pycsh/parameter.h>

#include "structmember.h"

#include <param/param.h>

#include <pycsh/pycsh.h>
#include <pycsh/utils.h>

#include "valueproxy.h"


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
		(void)set_res;
		/* Just sanity check that we can get the item, so it doesn't later fail in `Parameter_dealloc()` */
		assert(PyDict_GetItem((PyObject*)param_callback_dict, key) != NULL);
		assert(!PyErr_Occurred());

		assert(self);
		Py_DECREF(self);  // param_callback_dict should hold a weak reference to self

		#if 0
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
		#endif
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



static PyObject * Parameter_get_py_type(ParameterObject *self, void *closure) {

	return (PyObject *)Py_NewRef(self->type);
}

static PyObject * Parameter_gettype_deprecated(ParameterObject *self, void *closure) {

	if (PyErr_WarnEx(PyExc_DeprecationWarning, "Use `.py_type` instead", 2) < 0) {
		return NULL;
	}

	return Parameter_get_py_type(self, closure);
}

static PyObject * Parameter_get_c_type(ParameterObject *self, void *closure) {

	return Py_BuildValue("i", self->param->type);
}

#ifdef OLD_PARAM_API_ERROR

static PyObject * Parameter_get_oldvalue(ParameterObject *self, void *closure) {
	PyErr_SetString(PyExc_AttributeError, "`Parameter.remote_value` and `Parameter.cached_value` have been changed to: `.get_value()`, `.set_value()`, `.get_value_array()` and `.set_value_array()`.");
	return NULL;
}

static int Parameter_set_oldvalue(ParameterObject *self, PyObject *value, void *closure) {
	PyErr_SetString(PyExc_AttributeError, "`Parameter.remote_value` and `Parameter.cached_value` have been changed to: `.get_value()`, `.set_value()`, `.get_value_array()` and `.set_value_array()`.");
	return -1;
}

#endif  /* OLD_PARAM_API_ERROR */


static PyObject * Parameter_get_valueproxy(ParameterObject *self, void *closure) {
	/* Default to remote, user can override by calling the ValueProxy, i.e: `.value_index(remote=False)[0]` */
	return pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
}

static PyObject * Parameter_get_valueproxy_cached(ParameterObject *self, void *closure) {

	if (PyErr_WarnEx(PyExc_DeprecationWarning, "`Parameter.remote_value` and `Parameter.cached_value` have been changed to `Parameter.value` `ValueProxy` property", 2) < 0) {
		return NULL;
	}

	/* Default to remote, user can override by calling the ValueProxy, i.e: `.value_index(remote=False)[0]` */
	ValueProxyObject * const value_proxy = (ValueProxyObject*)pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
	if (!value_proxy) {
		return NULL;
	}
	value_proxy->remote = false;
	return (PyObject*)value_proxy;  /* Already new reference */
}

static int Parameter_set_valueproxy(ParameterObject *self, PyObject *value, void *closure) {

	ValueProxyObject * const value_proxy = (ValueProxyObject*)pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
	if (!value_proxy) {
		return -1;
	}
	return ValueProxy_ass_subscript(value_proxy, Py_None, value);
}

static int Parameter_set_valueproxy_cached(ParameterObject *self, PyObject *value, void *closure) {

	if (PyErr_WarnEx(PyExc_DeprecationWarning, "`Parameter.remote_value` and `Parameter.cached_value` have been changed to `Parameter.value` `ValueProxy` property", 2) < 0) {
		return -1;
	}

	ValueProxyObject * const value_proxy = (ValueProxyObject*)pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
	if (!value_proxy) {
		return -1;
	}
	value_proxy->remote = false;
	return ValueProxy_ass_subscript(value_proxy, Py_None, value);
}


static PyObject * Parameter_is_vmem(ParameterObject *self, void *closure) {
	// I believe this is the most appropriate way to check for vmem parameters.
	PyObject * result = self->param->vmem == NULL ? Py_False : Py_True;
	return Py_NewRef(result);
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
		assert(!PyErr_Occurred());
        if (PyDict_GetItem((PyObject*)param_callback_dict, key) != NULL) {
			/* `param_callback_dict` currently holds a weak reference to `self`.
				`PyDict_DelItem()` will `Py_DECREF()`,
				which should not cause `self->ob_base.ob_refcnt` to go negative. */
			Py_INCREF(key);
            if (PyDict_DelItem((PyObject*)param_callback_dict, key) != 0) {
				/* Restore reference count if removal failed. */
				Py_DECREF(key);
			}
        }
    }

	assert(self->param);
	const param_t * const list_param = param_list_find_id(*self->param->node, self->param->id);
	if (list_param == NULL || list_param != self->param) {
		/* Our parameter is not in the list, we should free it. */
		param_list_destroy(self->param);
	}

	#if 0
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
	#endif

	PyTypeObject *baseclass = pycsh_get_base_dealloc_class(&ParameterType);
    baseclass->tp_dealloc((PyObject*)self);
}

static PyObject * Parameter_GetItem(ParameterObject *self, PyObject *item) {

	#pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
	ValueProxyObject * const value_proxy AUTO_DECREF = pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
	#pragma GCC diagnostic pop
	if (!value_proxy) {
		return NULL;
	}

	return ValueProxy_subscript(value_proxy, item);
}

static int Parameter_SetItem(ParameterObject *self, PyObject* key, PyObject* value) {

	if (value == NULL) {
        PyErr_SetString(PyExc_TypeError, "Cannot delete parameter array indexes.");
        return -1;
    }

	#pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
	ValueProxyObject * const value_proxy AUTO_DECREF = pycsh_ValueProxy_from_Parameter(&ValueProxyType, self);
	#pragma GCC diagnostic pop
	if (!value_proxy) {
		return -1;
	}

	return ValueProxy_ass_subscript(value_proxy, key, value);
}

static Py_ssize_t ParameterArray_length(ParameterObject *self) {
	// We currently raise an exception when getting len() of non-array type parameters.
	// This stops PyCharm (Perhaps other IDE's) from showing their length as 0. ¯\_(ツ)_/¯
	return self->param->array_size;
}

static PyMappingMethods ParameterArray_as_mapping = {
    (lenfunc)ParameterArray_length,
    (binaryfunc)Parameter_GetItem,
    (objobjargproc)Parameter_SetItem
};

/* 
The Python binding 'Parameter' class exposes most of its attributes through getters, 
as only its 'value', 'host' and 'node' are mutable, and even those are through setters.
*/
static PyGetSetDef Parameter_getsetters[] = {

#if 1  // param_t getsetters
	{"name", (getter)Parameter_get_name, NULL,
     PyDoc_STR("Returns the name of the wrapped param_t C struct."), NULL},
    {"unit", (getter)Parameter_get_unit, NULL,
     PyDoc_STR("The unit of the wrapped param_t c struct as a string or None."), NULL},
	{"docstr", (getter)Parameter_get_docstr, NULL,
     PyDoc_STR("The help-text of the wrapped param_t c struct as a string or None."), NULL},
	{"id", (getter)Parameter_get_id, NULL,
     PyDoc_STR("id of the parameter"), NULL},
	{"type", (getter)Parameter_gettype_deprecated, NULL,
     PyDoc_STR("type of the parameter"), NULL},
	{"py_type", (getter)Parameter_get_py_type, NULL,
     PyDoc_STR("type of the parameter"), NULL},
	{"c_type", (getter)Parameter_get_c_type, NULL,
     PyDoc_STR("type of the parameter"), NULL},
	{"mask", (getter)Parameter_getmask, NULL,
     PyDoc_STR("mask of the parameter"), NULL},
	{"timestamp", (getter)Parameter_gettimestamp, NULL,
     PyDoc_STR("timestamp of the parameter"), NULL},
	{"node", (getter)Parameter_get_node, (setter)Parameter_set_node,
     PyDoc_STR("node of the parameter"), NULL},
#endif

#if 1  // Parameter getsetters
	{"host", (getter)Parameter_get_host, (setter)Parameter_set_host,
     PyDoc_STR("host of the parameter"), NULL},
#ifdef OLD_PARAM_API_ERROR
	{"remote_value", (getter)Parameter_get_oldvalue, (setter)Parameter_set_oldvalue,
     PyDoc_STR("get/set the remote (and cached) value of the parameter"), NULL},
	{"cached_value", (getter)Parameter_get_oldvalue, (setter)Parameter_set_oldvalue,
     PyDoc_STR("get/set the cached value of the parameter"), NULL},
#else  /* OLD_PARAM_API_ERROR */
	{"remote_value", (getter)Parameter_get_valueproxy, (setter)Parameter_set_valueproxy,
     PyDoc_STR("get/set the remote (and cached) value of the parameter"), NULL},
	{"cached_value", (getter)Parameter_get_valueproxy_cached, (setter)Parameter_set_valueproxy_cached,
     PyDoc_STR("get/set the cached value of the parameter"), NULL},
#endif  /* OLD_PARAM_API_ERROR */

	{"value", (getter)Parameter_get_valueproxy, (setter)Parameter_set_valueproxy,
     PyDoc_STR("get/set the remote/cached value of the parameter"), NULL},

	{"is_vmem", (getter)Parameter_is_vmem, NULL,
     PyDoc_STR("whether the parameter is a vmem parameter"), NULL},
	{"storage_type", (getter)Parameter_get_storage_type, NULL,
     PyDoc_STR("storage type of the parameter"), NULL},
	{"timeout", (getter)Parameter_get_timeout, (setter)Parameter_set_timeout,
     PyDoc_STR("timeout of the parameter"), NULL},
	{"retries", (getter)Parameter_get_retries, (setter)Parameter_set_retries,
     PyDoc_STR("available retries of the parameter"), NULL},
#endif
    {NULL, NULL, NULL, NULL}  /* Sentinel */
};

static PyObject * Parameter_list_add(ParameterObject *self, PyObject *args, PyObject *kwds) {

	int return_self = true;

	static const char * const kwlist[] = {"return_self", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|p", kwlist, &return_self)) {
		return NULL;  // TypeError is thrown
	}

	const param_t * const list_param = param_list_find_id(*self->param->node, self->param->id);
	
	if (list_param == self->param) {
		/* Existing parameter is ourself.
			So there is no reason to assign the list parameter to what it already is. */
		/* 5 is a unique return value (albeit obtuse) (unlikely to be used by `param_list_add()` in the future),
			which indicates that we did not update `self` in the list. */
		return return_self ? Py_NewRef(self) : Py_BuildValue("i", 5);
	}


	/* res==1=="existing parameter updated" */
	const int res = param_list_add(self->param);

	
	if (res!=1) {
		/* Did not update existing parameter. We either added a new parameter, or error. */
		return return_self ? Py_NewRef(self) : Py_BuildValue("i", res);
	}

	/* NOTE Kevin: Be very careful about `destroy=true` here.
		it `assert()`s that we never create multiple `ParameterObject`s pointing/reusing the same `self->param`.
		We can ensure this by keeping a weakref dict of existing `ParameterObject` instances.
		(weakref dict should use `&param_t` as key, which should it to return both new parameters we've created, and wrappers for existing parameters). */
	/* TODO Kevin: Test correct handling of `param_heap_t` here.  */
	param_list_remove_specific(self->param, 0, true);

	/* NOTE: This assignment has big implications of state. */
	self->param = list_param;

	return return_self ? Py_NewRef(self) : Py_BuildValue("i", res);
}
static PyObject * Parameter_list_forget(ParameterObject *self, PyObject *args, PyObject *kwds) {
	int verbose = pycsh_dfl_verbose;

	static const char * const kwlist[] = {"verbose", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|i", kwlist, &verbose)) {
		return NULL;  // TypeError is thrown
	}

	/* `param_list_destroy()` will be called by `Parameter_dealloc()` */
	param_list_remove_specific(self->param, verbose, false);

	Py_RETURN_NONE;
}
static PyMethodDef Parameter_methods[] = {
    {"list_add", (PyCFunction)Parameter_list_add, METH_VARARGS | METH_KEYWORDS, PyDoc_STR("Add `self` the global parameter list. Exposes it to other CSP nodes on the network, "\
		"And allows it to be found in `pycsh.list()`")},
    {"list_forget", (PyCFunction)Parameter_list_forget, METH_VARARGS | METH_KEYWORDS, PyDoc_STR("Remove this parameter from the global parameter list. Hiding it from other CSP nodes on the network. "\
		"Also removes it from `pycsh.list()`")},
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
	.tp_as_mapping = &ParameterArray_as_mapping,
	.tp_methods = Parameter_methods,
	.tp_str = (reprfunc)Parameter_str,
	.tp_richcompare = (richcmpfunc)Parameter_richcompare,
	.tp_hash = (hashfunc)Parameter_hash,
};
