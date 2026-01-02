#include "vmem.h"

// It is recommended to always define PY_SSIZE_T_CLEAN before including Python.h
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "structmember.h"

#include <vmem/vmem.h>

#include <pycsh/pycsh.h>
#include <pycsh/utils.h>

#include <assert.h>

/* `Dict[Vmem]` of cached instances. Used to help keep track of when `vmem->name` should be freed, as we should only do it for Python Vmem's. */
PyDictObject * vmem_dict;

csp_packet_t * pycsh_vmem_client_list_get(int node, int timeout, int version) {

    csp_conn_t * conn = csp_connect(CSP_PRIO_HIGH, node, VMEM_PORT_SERVER, timeout, CSP_O_CRC32);
    if (conn == NULL)
        return NULL;

    csp_packet_t * packet = csp_buffer_get(sizeof(vmem_request_t));
    if (packet == NULL)
        return NULL;

    vmem_request_t * request = (void *) packet->data;
    request->version = version;
    request->type = VMEM_SERVER_LIST;
    packet->length = sizeof(vmem_request_t);

    csp_send(conn, packet);

    csp_packet_t *resp = NULL;

    if (version == 3) {
        /* Allocate the maximum packet length to hold the response for the caller */
        resp = csp_buffer_get(CSP_BUFFER_SIZE);
        if (!resp) {
            printf("Could not allocate CSP buffer for VMEM response.\n");
            csp_close(conn);
            return NULL;
        }

        resp->length = 0;
        /* Keep receiving until we got everything or we got a timeout */
        while ((packet = csp_read(conn, timeout)) != NULL) {
            if (packet->data[0] & 0b01000000) {
                /* First packet */
                resp->length = 0;
            }

            /* Collect the response in the response packet */
            memcpy(&resp->data[resp->length], &packet->data[1], (packet->length - 1));
            resp->length += (packet->length - 1);

            if (packet->data[0] & 0b10000000) {
                /* Last packet, break the loop */
                csp_buffer_free(packet);
                break;
            }

            csp_buffer_free(packet);
        }

        if (packet == NULL) {
            printf("No response to VMEM list request\n");
        }
    } else {
        /* Wait for response */
        packet = csp_read(conn, timeout);
        if (packet == NULL) {
            printf("No response to VMEM list request\n");
        }
        resp = packet;
    }

    csp_close(conn);

    return resp;
}

static PyObject * Vmem_str(VmemObject *self) {
    const vmem_t * const vmem = self->vmem;
    return PyUnicode_FromFormat(
        " %2u: %-16.16s 0x%016"PRIX64" - %"PRIu64" typ %u\r\n",
        self->vmem_id, vmem->name, be64toh(vmem->vaddr), be64toh(vmem->size), vmem->type
    );
}

static void Vmem_dealloc(VmemObject *self) {

    vmem_t * list_vmem = NULL;

    for (vmem_iter_t *iter = vmem_next(NULL); iter != NULL; iter = vmem_next(iter)) {
        if (vmem_from_iter(iter) == self->vmem) {
            list_vmem = self->vmem;
        }
    }

    assert(!list_vmem && "`Vmem_dealloc` should not be called while `self->vmem` is in the linked list, as we store `vmem->name` in `self->name`");

    {   /* Remove ourselves from the callback/lookup dictionary */
        PyObject *key AUTO_DECREF = PyLong_FromVoidPtr(self->vmem);
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

    const PyTypeObject * const baseclass = pycsh_get_base_dealloc_class((PyObject*)self);
    baseclass->tp_dealloc((PyObject*)self);
}


/**
 * @brief Check that the read func is `Callable[[Vmem, int, int], bytes]` (i.e, `self, addr, length`),
 *  as specified by "void (*read)(struct vmem_s * vmem, uint64_t addr, void * dataout, uint32_t len);"
 * 
 * Currently also checks type-hints (if specified).
 * Additional optional arguments are also allowed,
 *  as these can be disregarded by the caller.
 * 
 * @param callback function to check
 * @param raise_exc Whether to set exception message when returning false.
 * @return true for success
 */
bool is_valid_read_func(PyObject * const func, bool raise_exc) {

    if (func == NULL || func == Py_None) {
        return true;
    }

    // Suppress the incompatible pointer type warning when AUTO_DECREF is used on subclasses of PyObject*
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"

    // Get the __code__ attribute of the function, and check that it is a PyCodeObject
    // TODO Kevin: Hopefully it's safe to assume that PyObject_GetAttrString() won't mutate callback
    PyCodeObject *func_code AUTO_DECREF = (PyCodeObject*)PyObject_GetAttrString((PyObject*)func, "__code__");
    if (!func_code || !PyCode_Check(func_code)) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided read function must be callable");
        return false;
    }

    int accepted_pos_args = pycsh_get_num_accepted_pos_args(func, raise_exc);
    if (accepted_pos_args < 3) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided read function must accept at least 3 positional arguments");
        return false;
    }

    // Check for too many required arguments
    int num_non_default_pos_args = pycsh_get_num_required_args(func, raise_exc);
    if (num_non_default_pos_args > 3) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided read function must not require more than 3 positional arguments");
        return false;
    }

    // Get the __annotations__ attribute of the function
    // TODO Kevin: Hopefully it's safe to assume that PyObject_GetAttrString() won't mutate callback
    PyDictObject *func_annotations AUTO_DECREF = (PyDictObject *)PyObject_GetAttrString((PyObject*)func, "__annotations__");

    // Re-enable the warning
    #pragma GCC diagnostic pop

    assert(PyDict_Check(func_annotations));
    if (!func_annotations) {
        return true;  // It's okay to not specify type-hints for the callback.
    }

    // Get the parameters annotation
    // PyCode_GetVarnames() exists and should be exposed, but it doesn't appear to be in any visible header.
    PyObject *param_names AUTO_DECREF = PyObject_GetAttrString((PyObject*)func_code, "co_varnames");// PyCode_GetVarnames(func_code);
    if (!param_names) {
        return true;  // Function parameters have not been annotated, this is probably okay.
    }

    // Check if it's a tuple
    if (!PyTuple_Check(param_names)) {
        // TODO Kevin: Not sure what exception to set here.
        if (raise_exc)
            PyErr_Format(PyExc_TypeError, "param_names type \"%s\" %p", param_names->ob_type->tp_name, param_names);
        return false;  // Not sure when this fails, but it's probably bad if it does.
    }

    PyObject *typing_module_name AUTO_DECREF = PyUnicode_FromString("typing");
    if (!typing_module_name) {
        return false;
    }

    PyObject *typing_module AUTO_DECREF = PyImport_Import(typing_module_name);
    if (!typing_module) {
        if (raise_exc)
            PyErr_SetString(PyExc_ImportError, "Failed to import typing module");
        return false;
    }

#if 1
    PyObject *get_type_hints AUTO_DECREF = PyObject_GetAttrString(typing_module, "get_type_hints");
    if (!get_type_hints) {
        if (raise_exc)
            PyErr_SetString(PyExc_ImportError, "Failed to get 'get_type_hints()' function");
        return false;
    }
    assert(PyCallable_Check(get_type_hints));


    PyObject *type_hint_dict AUTO_DECREF = PyObject_CallFunctionObjArgs(get_type_hints, func, NULL);

#else

    PyObject *get_type_hints_name AUTO_DECREF = PyUnicode_FromString("get_type_hints");
    if (!get_type_hints_name) {
        return false;
    }

    PyObject *type_hint_dict AUTO_DECREF = PyObject_CallMethodObjArgs(typing_module, get_type_hints_name, callback, NULL);
    if (!type_hint_dict) {
        if (raise_exc)
            PyErr_SetString(PyExc_ImportError, "Failed to get type hints of callback");
        return false;
    }
#endif

    // TODO Kevin: Perhaps issue warnings for type-hint errors, instead of errors.
    {   // Checking first parameter type-hint

        // co_varnames may be too short for our index, if the signature has *args, but that's okay.
        if (PyTuple_Size(param_names)-1 <= 0) {
            return true;
        }

        PyObject *param_name = PyTuple_GetItem(param_names, 0);
        if (!param_name) {
            if (raise_exc)
                PyErr_SetString(PyExc_IndexError, "Could not get first parameter name");
            return false;
        }

        PyObject *param_annotation = PyDict_GetItem(type_hint_dict, param_name);
        if (param_annotation != NULL && param_annotation != Py_None) {
            if (!PyType_Check(param_annotation)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "First parameter annotation is %s, which is not a type", param_annotation->ob_type->tp_name);
                return false;
            }
            if (!PyObject_IsSubclass(param_annotation, (PyObject *)&VmemType)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "First function parameter should be type-hinted as `self: Vmem`. (not %s)", param_annotation->ob_type->tp_name);
                return false;
            }
        }
    }

    {   // Checking second parameter type-hint

        // co_varnames may be too short for our index, if the signature has *args, but that's okay.
        if (PyTuple_Size(param_names)-1 <= 1) {
            return true;
        }

        PyObject *param_name = PyTuple_GetItem(param_names, 1);
        if (!param_name) {
            if (raise_exc)
                PyErr_SetString(PyExc_IndexError, "Could not get second parameter name");
            return false;
        }

        PyObject *param_annotation = PyDict_GetItem(type_hint_dict, param_name);
        if (param_annotation != NULL && param_annotation != Py_None) {
            if (!PyType_Check(param_annotation)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "Second parameter annotation is %s, which is not a type", param_annotation->ob_type->tp_name);
                return false;
            }
            if (!PyObject_IsSubclass(param_annotation, (PyObject *)&PyLong_Type)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "Second callback parameter should be type-hinted as `addr: int`. (not %s)", param_annotation->ob_type->tp_name);
                return false;
            }
        }
    }

    {   // Checking third parameter type-hint

        // co_varnames may be too short for our index, if the signature has *args, but that's okay.
        if (PyTuple_Size(param_names)-1 <= 2) {
            return true;
        }

        PyObject *param_name = PyTuple_GetItem(param_names, 2);
        if (!param_name) {
            if (raise_exc)
                PyErr_SetString(PyExc_IndexError, "Could not get second parameter name");
            return false;
        }

        PyObject *param_annotation = PyDict_GetItem(type_hint_dict, param_name);
        if (param_annotation != NULL && param_annotation != Py_None) {
            if (!PyType_Check(param_annotation)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "Second parameter annotation is %s, which is not a type", param_annotation->ob_type->tp_name);
                return false;
            }
            if (!PyObject_IsSubclass(param_annotation, (PyObject *)&PyLong_Type)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "Second callback parameter should be type-hinted as `length: int`. (not %s)", param_annotation->ob_type->tp_name);
                return false;
            }
        }
    }

    return true;
}


/**
 * @brief Check that the write func is `Callable[[Vmem, int, bytes], None]` (i.e, `self, addr, data`),
 *  as specified by "void (*write)(struct vmem_s * vmem, uint64_t addr, const void * datain, uint32_t len);"
 * 
 * Currently also checks type-hints (if specified).
 * Additional optional arguments are also allowed,
 *  as these can be disregarded by the caller.
 * 
 * @param callback function to check
 * @param raise_exc Whether to set exception message when returning false.
 * @return true for success
 */
bool is_valid_write_func(PyObject * const func, bool raise_exc) {

    if (func == NULL || func == Py_None) {
        return true;
    }

    // Suppress the incompatible pointer type warning when AUTO_DECREF is used on subclasses of PyObject*
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"

    // Get the __code__ attribute of the function, and check that it is a PyCodeObject
    // TODO Kevin: Hopefully it's safe to assume that PyObject_GetAttrString() won't mutate callback
    PyCodeObject *func_code AUTO_DECREF = (PyCodeObject*)PyObject_GetAttrString((PyObject*)func, "__code__");
    if (!func_code || !PyCode_Check(func_code)) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided write function must be callable");
        return false;
    }

    int accepted_pos_args = pycsh_get_num_accepted_pos_args(func, raise_exc);
    if (accepted_pos_args < 3) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided write function must accept at least 3 positional arguments");
        return false;
    }

    // Check for too many required arguments
    int num_non_default_pos_args = pycsh_get_num_required_args(func, raise_exc);
    if (num_non_default_pos_args > 3) {
        if (raise_exc)
            PyErr_SetString(PyExc_TypeError, "Provided write function must not require more than 3 positional arguments");
        return false;
    }

    // Get the __annotations__ attribute of the function
    // TODO Kevin: Hopefully it's safe to assume that PyObject_GetAttrString() won't mutate callback
    PyDictObject *func_annotations AUTO_DECREF = (PyDictObject *)PyObject_GetAttrString((PyObject*)func, "__annotations__");

    // Re-enable the warning
    #pragma GCC diagnostic pop

    assert(PyDict_Check(func_annotations));
    if (!func_annotations) {
        return true;  // It's okay to not specify type-hints for the callback.
    }

    // Get the parameters annotation
    // PyCode_GetVarnames() exists and should be exposed, but it doesn't appear to be in any visible header.
    PyObject *param_names AUTO_DECREF = PyObject_GetAttrString((PyObject*)func_code, "co_varnames");// PyCode_GetVarnames(func_code);
    if (!param_names) {
        return true;  // Function parameters have not been annotated, this is probably okay.
    }

    // Check if it's a tuple
    if (!PyTuple_Check(param_names)) {
        // TODO Kevin: Not sure what exception to set here.
        if (raise_exc)
            PyErr_Format(PyExc_TypeError, "param_names type \"%s\" %p", param_names->ob_type->tp_name, param_names);
        return false;  // Not sure when this fails, but it's probably bad if it does.
    }

    PyObject *typing_module_name AUTO_DECREF = PyUnicode_FromString("typing");
    if (!typing_module_name) {
        return false;
    }

    PyObject *typing_module AUTO_DECREF = PyImport_Import(typing_module_name);
    if (!typing_module) {
        if (raise_exc)
            PyErr_SetString(PyExc_ImportError, "Failed to import typing module");
        return false;
    }

#if 1
    PyObject *get_type_hints AUTO_DECREF = PyObject_GetAttrString(typing_module, "get_type_hints");
    if (!get_type_hints) {
        if (raise_exc)
            PyErr_SetString(PyExc_ImportError, "Failed to get 'get_type_hints()' function");
        return false;
    }
    assert(PyCallable_Check(get_type_hints));


    PyObject *type_hint_dict AUTO_DECREF = PyObject_CallFunctionObjArgs(get_type_hints, func, NULL);

#else

    PyObject *get_type_hints_name AUTO_DECREF = PyUnicode_FromString("get_type_hints");
    if (!get_type_hints_name) {
        return false;
    }

    PyObject *type_hint_dict AUTO_DECREF = PyObject_CallMethodObjArgs(typing_module, get_type_hints_name, callback, NULL);
    if (!type_hint_dict) {
        if (raise_exc)
            PyErr_SetString(PyExc_ImportError, "Failed to get type hints of callback");
        return false;
    }
#endif

    // TODO Kevin: Perhaps issue warnings for type-hint errors, instead of errors.
    {   // Checking first parameter type-hint

        // co_varnames may be too short for our index, if the signature has *args, but that's okay.
        if (PyTuple_Size(param_names)-1 <= 0) {
            return true;
        }

        PyObject *param_name = PyTuple_GetItem(param_names, 0);
        if (!param_name) {
            if (raise_exc)
                PyErr_SetString(PyExc_IndexError, "Could not get first parameter name");
            return false;
        }

        PyObject *param_annotation = PyDict_GetItem(type_hint_dict, param_name);
        if (param_annotation != NULL && param_annotation != Py_None) {
            if (!PyType_Check(param_annotation)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "First parameter annotation is %s, which is not a type", param_annotation->ob_type->tp_name);
                return false;
            }
            if (!PyObject_IsSubclass(param_annotation, (PyObject *)&VmemType)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "First callback parameter should be type-hinted as `self: Vmem`. (not %s)", param_annotation->ob_type->tp_name);
                return false;
            }
        }
    }

    {   // Checking second parameter type-hint

        // co_varnames may be too short for our index, if the signature has *args, but that's okay.
        if (PyTuple_Size(param_names)-1 <= 1) {
            return true;
        }

        PyObject *param_name = PyTuple_GetItem(param_names, 1);
        if (!param_name) {
            if (raise_exc)
                PyErr_SetString(PyExc_IndexError, "Could not get second parameter name");
            return false;
        }

        PyObject *param_annotation = PyDict_GetItem(type_hint_dict, param_name);
        if (param_annotation != NULL && param_annotation != Py_None) {
            if (!PyType_Check(param_annotation)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "Second parameter annotation is %s, which is not a type", param_annotation->ob_type->tp_name);
                return false;
            }
            if (!PyObject_IsSubclass(param_annotation, (PyObject *)&PyLong_Type)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "Second callback parameter should be type-hinted as `addr: int`. (not %s)", param_annotation->ob_type->tp_name);
                return false;
            }
        }
    }

    {   // Checking third parameter type-hint

        // co_varnames may be too short for our index, if the signature has *args, but that's okay.
        if (PyTuple_Size(param_names)-1 <= 2) {
            return true;
        }

        PyObject *param_name = PyTuple_GetItem(param_names, 2);
        if (!param_name) {
            if (raise_exc)
                PyErr_SetString(PyExc_IndexError, "Could not get second parameter name");
            return false;
        }

        PyObject *param_annotation = PyDict_GetItem(type_hint_dict, param_name);
        if (param_annotation != NULL && param_annotation != Py_None) {
            if (!PyType_Check(param_annotation)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "Second parameter annotation is %s, which is not a type", param_annotation->ob_type->tp_name);
                return false;
            }
            if (!PyObject_IsSubclass(param_annotation, (PyObject *)&PyBytes_Type)) {
                if (raise_exc)
                    PyErr_Format(PyExc_TypeError, "Second function parameter should be type-hinted as `datain: bytes`. (not %s)", param_annotation->ob_type->tp_name);
                return false;
            }
        }
    }

    return true;
}


static void _py_vmem_read(vmem_t * vmem, uint64_t addr, void * dataout, uint32_t len) {
	//memcpy(dataout, ((vmem_file_driver_t *) vmem->driver)->physaddr + addr, len);
    PyGILState_STATE CLEANUP_GIL gstate = PyGILState_Ensure();

    VmemObject * self = (VmemObject*)vmem->driver;

    assert(is_valid_read_func(self->py_read, true));

    PyObject * args AUTO_DECREF = Py_BuildValue("OkI", self, addr, len);
    if (!args) {
        return;
    }

    assert(!PyErr_Occurred());
    PyObject * value AUTO_DECREF = PyObject_CallObject(self->py_read, args);
    if (PyErr_Occurred()) {
        return;
    }

    PyObject * PyBytes_cast AUTO_DECREF = PyBytes_FromObject(value);

    if (!PyBytes_cast) {
        PyErr_SetString(PyExc_TypeError, "Python VMEM `read()` method should return a bytes object");
        return;
    }

    char *data;
    Py_ssize_t size;
    if (PyBytes_AsStringAndSize(value, &data, &size) < 0) {
        return;  // error already set
    }

    if (size != len) {
        PyErr_Format(PyExc_ValueError, "Python VMEM `read()` method returned incorrect number of bytes (got: %ld, expected: %d)", size, len);
        return;
    }

    memcpy(dataout, data, size);
}

static void _py_vmem_write(vmem_t * vmem, uint64_t addr, const void * datain, uint32_t len) {
	//memcpy(((vmem_file_driver_t *) vmem->driver)->physaddr + addr, datain, len);
    PyGILState_STATE CLEANUP_GIL gstate = PyGILState_Ensure();

    VmemObject * self = (VmemObject*)vmem->driver;

    assert(is_valid_write_func(self->py_write, true));

    PyObject * datain_bytes AUTO_DECREF = PyBytes_FromStringAndSize((const char *)datain, (Py_ssize_t)len);

    PyObject * args AUTO_DECREF = Py_BuildValue("OkO", self, addr, datain_bytes);
    if (!args) {
        return;
    }

    PyObject * value AUTO_DECREF = PyObject_CallObject(self->py_write, args);

}

ATTR_MALLOC(Vmem_dealloc, 1)
static PyObject * Vmem_new(PyTypeObject *cls, PyObject * args, PyObject * kwds) {

    uint64_t vaddr; static_assert(sizeof(unsigned long) == sizeof(vaddr));
    uint64_t size; static_assert(sizeof(unsigned long) == sizeof(size));
    //uint8_t vmem_id;
    uint8_t type; static_assert(sizeof(unsigned char) == sizeof(type));
    //char name[16+1];
    const char *name;
    PyObject *py_read;
    PyObject *py_write;

    static char *kwlist[] = {"vaddr", "size", /*"vmem_id",*/ "type", "name", "read", "write", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "kkBsOO", kwlist, &vaddr, &size, /*&vmem_id,*/ &type, &name, &py_read, &py_write)) {
        return NULL;  // TypeError is thrown
    }

    if (strnlen(name, 16+1) >= 16) {  /* Fit NULL byte */
        /* NOTE: Technically speaking, NameError is not the right exception here.
            But come on, it's even in the name. :) */
        PyErr_Format(PyExc_NameError, "VMEM name '%s' cannot exceed 16 characters", name);
        return NULL;
    }

    vmem_t * overlap_vmem = vmem_vaddr_to_vmem(vaddr);
    if (overlap_vmem) {
        PyErr_Format(PyExc_BufferError, "vaddr %lu overlaps with vmem %s", vaddr, overlap_vmem->name);
        return NULL;
    }

    if (!is_valid_read_func(py_read, true)) {
        return NULL;
    }

    if (!is_valid_write_func(py_write, true)) {
        return NULL;
    }

    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wincompatible-pointer-types"
    VmemObject * AUTO_DECREF self = (VmemObject *)cls->tp_alloc(cls, 0);
    #pragma GCC diagnostic pop

    if (self == NULL) {
		return NULL;
	}

    vmem_t *vmem = calloc(sizeof(vmem_t), 1);
    if (!vmem) {
        return PyErr_NoMemory();
    }

    strncpy(self->name, name, 16);
    vmem->vaddr = vaddr;
    vmem->size = size;
    vmem->type = type;
    vmem->name = name;
    vmem->ack_with_pull = true;  /* TODO Kevin: Should the user control this, or should we set it to `false` if no `read` method is specified? */

    /* TODO Kevin: Check parameters of user-specified `read()` and `write()` methods. */
    vmem->read = _py_vmem_read;
    vmem->write = _py_vmem_write;
    vmem->driver = self;

    /* TODO Kevin: `vmem_add()` currently always succeeds,
        and since it's `void` we have to trust that it will continue to do so. */
    vmem_add(vmem, vmem);

    {
        vmem_t * list_vmem = NULL;

        for (vmem_iter_t *iter = vmem_next(NULL); iter != NULL; iter = vmem_next(iter)) {
            if (vmem_from_iter(iter) == vmem) {
                list_vmem = vmem;
            }
        }

        assert(list_vmem && "`self->vmem` not in list after `vmem_add()`");
    }

    self->vmem = vmem;
    self->py_read = py_read;
    self->py_write = py_write;

    {   /* Add ourselves to the cache dictionary */
		PyObject *key AUTO_DECREF = PyLong_FromVoidPtr(vmem);
		assert(key != NULL);
		assert(!PyErr_Occurred());
		assert(PyDict_GetItem((PyObject*)vmem_dict, key) == NULL);
		int set_res = PyDict_SetItem((PyObject*)vmem_dict, key, (PyObject*)self);
		assert(set_res == 0);
		(void)set_res;
		/* Just sanity check that we can get the item, so it doesn't later fail in `Vmem_dealloc()` */
		assert(PyDict_GetItem((PyObject*)vmem_dict, key) != NULL);
		assert(!PyErr_Occurred());

        //Py_INCREF(self);  /* `self->vmem->name` is stored on `self`, which prevents us from freeing it while `self->vmem` is in the linked list. */
		//Py_DECREF(self);  // param_callback_dict should hold a weak reference to self

	}

    return (PyObject*)Py_NewRef(self);
}

static PyObject * Vmem_find(PyTypeObject *type, PyObject *args, PyObject *kwds) {

    CSP_INIT_CHECK()

    unsigned int node = pycsh_dfl_node;
    unsigned int timeout = pycsh_dfl_timeout;
    unsigned int version = 2;
    int verbose = pycsh_dfl_verbose;

    static char *kwlist[] = {"node", "timeout", "version", "verbose", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|IIIi", kwlist, &node, &timeout, &version, &verbose))
        return NULL;  // Raises TypeError.

    if (verbose >= 2) {
        printf("Requesting vmem list from node %u timeout %u version %d\n", node, timeout, version);
    }
    
    csp_packet_t * packet = NULL;
    Py_BEGIN_ALLOW_THREADS;
        packet = pycsh_vmem_client_list_get(node, timeout, version);
    Py_END_ALLOW_THREADS;
    if (packet == NULL) {
        PyErr_Format(PyExc_ConnectionError, "No response (node=%d, timeout=%d)", node, timeout);
        return NULL;
    }

    size_t vmem_idx = 0;
    size_t vmem_count = 0;
    size_t namelen = 0;  // Length from  vmem_server.h
    if (version == 3) {
        vmem_count = packet->length / sizeof(vmem_list3_t);
        namelen = 16+1;
    } else if (version == 2) {
        vmem_count = packet->length / sizeof(vmem_list2_t);
        namelen = 5;
    } else {
        vmem_count = packet->length / sizeof(vmem_list_t);
        namelen = 5;
    }

    /* AUTO_DECREF used for exception handling, Py_NewRef() returned otherwise. */
    PyObject * vmem_tuple AUTO_DECREF = PyTuple_New(vmem_count);
    if (!vmem_tuple) {
        return NULL;  // Let's just assume that Python has set some a MemoryError exception here
    }

    if (version == 3) {
        for (vmem_list3_t * vmem = (void *) packet->data; (intptr_t) vmem < (intptr_t) packet->data + packet->length; vmem++) {

            VmemObject *self = (VmemObject *)type->tp_alloc(type, 0);
            if (self == NULL) {
                /* This is likely a memory allocation error, in which case we expect .tp_alloc() to have raised an exception. */
                return NULL;
            }

            self->vmem_id = vmem->vmem_id,
            self->vmem->vaddr = be64toh(vmem->vaddr),
            self->vmem->size = be64toh(vmem->size),
            self->vmem->type = vmem->type,
            strncpy(self->name, vmem->name, namelen);
            self->vmem->name = self->name;
            
            if (verbose >= 1) {
                printf(" %2u: %-16.16s 0x%016"PRIX64" - %"PRIu64" typ %u\r\n", vmem->vmem_id, vmem->name, be64toh(vmem->vaddr), be64toh(vmem->size), vmem->type);
            }
            
            PyTuple_SET_ITEM(vmem_tuple, vmem_idx++, (PyObject*)self);
        }
    } else if (version == 2) {
        for (vmem_list2_t * vmem = (void *) packet->data; (intptr_t) vmem < (intptr_t) packet->data + packet->length; vmem++) {

            VmemObject *self = (VmemObject *)type->tp_alloc(type, 0);
            if (self == NULL) {
                /* This is likely a memory allocation error, in which case we expect .tp_alloc() to have raised an exception. */
                return NULL;
            }

            self->vmem_id = vmem->vmem_id,
            self->vmem->vaddr = be64toh(vmem->vaddr),
            self->vmem->size = be32toh(vmem->size),
            self->vmem->type = vmem->type,
            strncpy(self->name, vmem->name, namelen);
            self->vmem->name = self->name;
            
            if (verbose >= 1) {
                printf(" %2u: %-5.5s 0x%016"PRIX64" - %"PRIu32" typ %u\r\n", vmem->vmem_id, vmem->name, be64toh(vmem->vaddr), be32toh(vmem->size), vmem->type);
            }
            
            PyTuple_SET_ITEM(vmem_tuple, vmem_idx++, (PyObject*)self);
        }
    } else {
        for (vmem_list_t * vmem = (void *) packet->data; (intptr_t) vmem < (intptr_t) packet->data + packet->length; vmem++) {

            VmemObject *self = (VmemObject *)type->tp_alloc(type, 0);
            if (self == NULL) {
                /* This is likely a memory allocation error, in which case we expect .tp_alloc() to have raised an exception. */
                return NULL;
            }

            self->vmem_id = vmem->vmem_id,
            self->vmem->vaddr = be32toh(vmem->vaddr),
            self->vmem->size = be32toh(vmem->size),
            self->vmem->type = vmem->type,
            strncpy(self->name, vmem->name, namelen);
            self->vmem->name = self->name;
            
            if (verbose >= 1) {
                printf(" %2u: %-5.5s 0x%08"PRIX32" - %"PRIu32" typ %u\r\n", vmem->vmem_id, vmem->name, be32toh(vmem->vaddr), be32toh(vmem->size), vmem->type);
            }
            
            PyTuple_SET_ITEM(vmem_tuple, vmem_idx++, (PyObject*)self);
        }

    }

    return Py_NewRef(vmem_tuple);
}

static PyMemberDef Vmem_members[] = {
    // {"vaddr", T_LONG, offsetof(VmemObject, vmem.vaddr), READONLY, "Starting address of the VMEM area. Used for upload and download I think"},
    // {"size", T_LONG, offsetof(VmemObject, vmem.size), READONLY, "Size of the VMEM area in bytes"},
    {"vmem_id", T_BYTE, offsetof(VmemObject, vmem_id), READONLY, "ID of the VMEM area, used for certain commands"},
    // {"type", T_BYTE, offsetof(VmemObject, vmem.type), READONLY, "int type of the VMEM area"},
    {"name", T_STRING_INPLACE, offsetof(VmemObject, name), READONLY, "Name of the VMEM area"},
    {NULL}  /* Sentinel */
};

static PyObject *Vmem_get_vaddr(VmemObject *self, void *closure) {
    return Py_BuildValue("k", self->vmem->vaddr);
}

static PyObject *Vmem_get_size(VmemObject *self, void *closure) {
    return Py_BuildValue("k", self->vmem->size);
}

#if 0
static PyObject *Vmem_get_vmem_id(VmemObject *self, void *closure) {
    return Py_BuildValue("", self->vmem_id);
}
#endif

static PyObject *Vmem_get_type(VmemObject *self, void *closure) {
    return Py_BuildValue("B", self->vmem->type);
}

// static PyObject *Vmem_get_name(VmemObject *self, void *closure) {
//     return Py_BuildValue("s", self->vmem->name);
// }


static PyGetSetDef Vmem_getsetters[] = {
    {"vaddr", (getter)Vmem_get_vaddr, NULL,
        PyDoc_STR("Starting address of the VMEM area. Used for upload and download I think")},
    {"size", (getter)Vmem_get_size, NULL,
        PyDoc_STR("Size of the VMEM area in bytes")},
    // {"vmem_id", (getter)Vmem_get_vmem_id, NULL,
    //     PyDoc_STR("ID of the VMEM area, used for certain commands")},
    {"type", (getter)Vmem_get_type, NULL,
        PyDoc_STR("int type of the VMEM area")},
    // {"name", (getter)Vmem_get_name, NULL,
    //     PyDoc_STR("Name of the VMEM area")},
    {NULL, NULL, NULL, NULL}  /* Sentinel */
};

PyMethodDef Vmem_class_methods[2] = {
    {"new", (PyCFunction)Vmem_new, METH_VARARGS | METH_KEYWORDS | METH_CLASS, "Create an entirely new VMEM, instead of just wrapping an existing one"},
    {"find", (PyCFunction)Vmem_find, METH_VARARGS | METH_KEYWORDS | METH_CLASS, "Find an existing VMEM in the global linked list, using a VMEM identifier."},
};

PyTypeObject VmemType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "pycsh.Vmem",
    .tp_doc = "Convenient wrapper class for 'vmem' replies.",
    .tp_basicsize = sizeof(VmemObject),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    .tp_new = Vmem_find,
    .tp_dealloc = (destructor)Vmem_dealloc,
    .tp_members = Vmem_members,
    .tp_getset = Vmem_getsetters,
    .tp_str = (reprfunc)Vmem_str,
};
