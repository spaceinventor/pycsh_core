
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <pycsh/pycsh.h>

#include <stdio.h>
#include <stdlib.h>
#include <csp/csp.h>
#include <csp/csp_cmp.h>
#include <vmem/vmem_server.h>
#include <param/param_queue.h>
#include <param/param_client.h>
#include <slash/slash.h>
#include <slash/optparse.h>
#include <apm/csh_api.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>

static vmem_list_t stdbuf_get_base(int node, int timeout) {
    vmem_list_t ret = {};

    csp_conn_t * conn = csp_connect(CSP_PRIO_HIGH, node, VMEM_PORT_SERVER, timeout, CSP_O_CRC32);
    if (conn == NULL)
        return ret;

    csp_packet_t * packet = csp_buffer_get(sizeof(vmem_request_t));
    vmem_request_t * request = (void *) packet->data;
    request->version = 1;
    request->type = VMEM_SERVER_LIST;
    packet->length = sizeof(vmem_request_t);

    csp_send(conn, packet);

    /* Wait for response */
    packet = csp_read(conn, timeout);
    if (packet == NULL) {
        printf("No response\n");
        csp_close(conn);
        return ret;
    }

    for (vmem_list_t * vmem = (void *) packet->data; (intptr_t) vmem < (intptr_t) packet->data + packet->length; vmem++) {
        //printf(" %u: %-5.5s 0x%08X - %u typ %u\r\n", vmem->vmem_id, vmem->name, (unsigned int) be32toh(vmem->vaddr), (unsigned int) be32toh(vmem->size), vmem->type);
        if (strncmp(vmem->name, "stdbu", 5) == 0) {
            ret.vmem_id = vmem->vmem_id;
            ret.type = vmem->type;
            memcpy(ret.name, vmem->name, 5);
            ret.vaddr = be32toh(vmem->vaddr);
            ret.size = be32toh(vmem->size);
        }
    }

    csp_buffer_free(packet);
    csp_close(conn);

    return ret;
}

static int stdbuf_get(uint16_t node, uint32_t base, int from, int to, int timeout) {

    int len = to - from;
    if (len > 200)
        len = 200;

    struct csp_cmp_message message;
    message.peek.addr = htobe32(base + from);
    message.peek.len = len;

    if (csp_cmp_peek(node, timeout, &message) != CSP_ERR_NONE) {
        return 0;
    }

    int ignore __attribute__((unused)) = write(fileno(stdout), message.peek.data, len);

    return len;

}

#if 0
static int stdbuf_v1_py(struct slash *slash, unsigned int node, unsigned int timeout, unsigned int paramversion) {

    /* Pull buffer */
    char pull_buf[25];
    param_queue_t pull_q = {
        .buffer = pull_buf,
        .buffer_size = 50,
        .type = PARAM_QUEUE_TYPE_GET,
        .used = 0,
        .version = paramversion,
    };

    vmem_list_t vmem = stdbuf_get_base(node, 1000);

    if (vmem.size == 0 || vmem.vaddr == 0) {
        printf("Could not find stdbuffer on node %u\n", node);
        return SLASH_EINVAL;
    }

    param_t * stdbuf_in = param_list_find_id(node, 28);
    if (stdbuf_in == NULL) {
        stdbuf_in = param_list_create_remote(28, node, PARAM_TYPE_UINT16, PM_DEBUG, 0, "stdbuf_in", NULL, NULL, -1);
        param_list_add(stdbuf_in);
    }

    param_t * stdbuf_out = param_list_find_id(node, 29);
    if (stdbuf_out == NULL) {
        stdbuf_out = param_list_create_remote(29, node, PARAM_TYPE_UINT16, PM_DEBUG, 0, "stdbuf_out", NULL, NULL, -1);
        param_list_add(stdbuf_out);
    }

    param_queue_add(&pull_q, stdbuf_in, 0, NULL);
    param_queue_add(&pull_q, stdbuf_out, 0, NULL);

    printf("Monitoring stdbuf on node %u, base %x, size %u\n", node, vmem.vaddr, vmem.size);

    while(1) {

        param_pull_queue(&pull_q, CSP_PRIO_HIGH, 0, node, 100);
        int in = param_get_uint16(stdbuf_in);
        int out = param_get_uint16(stdbuf_out);

        int got = 0;
        if (out < in) {
            //printf("Get from %u to %u\n", out, in);
            got = stdbuf_get(node, vmem.vaddr, out, in, 100);
        } else if (out > in) {
            //printf("Get from %u to %u\n", out, vmem.size);
            got = stdbuf_get(node, vmem.vaddr, out, vmem.size, 100);
        }

        uint16_t out_push = out + got;
        out_push %= vmem.size;
        param_push_single(stdbuf_out, 0, CSP_PRIO_NORM, &out_push, 0, node, 100, paramversion, false);

        if (got > 0) {
            continue;
        }

        /* Delay (press enter to exit) */
        if (slash_wait_interruptible(slash, 100) != 0) {
            break;
        }

    };

    return SLASH_SUCCESS;
}
#endif

//static FILE * log_f = 0;
//static char log_name[100] = {0};

/**
    This implementation is not optimized at all.
        We are very likely copying the immutable string buffer far too many times,
        just to not deviate from the CSH implementation.
 */
static PyObject * stdbuf_v2_py(unsigned int node, unsigned int timeout) {

    PyObject * stdbuf_reply_str = PyUnicode_New(0, 0);

    if(!stdbuf_reply_str) {
        return PyErr_NoMemory();
    }

    csp_conn_t * conn = csp_connect(CSP_PRIO_HIGH, node, 15, 0, CSP_O_CRC32);
    if (conn == NULL) {
        PyErr_Format(PyExc_ConnectionError, "No response (node=%d) (timeout=%d)", node, timeout);
        return NULL;
    }

    csp_packet_t * packet = csp_buffer_get(1);
    if (packet == NULL) {
        csp_close(conn);
        PyErr_Format(PyExc_ConnectionError, "No response (node=%d) (timeout=%d)", node, timeout);
        return NULL;
    }
    packet->data[0] = 0xAA;
    packet->length = 1;
    csp_send(conn, packet);

    #define LF '\n' /* ASCII 10d */
    #define CR '\r' /* ASCII 13d */
    while ((packet = csp_read(conn, timeout))) {
        int again = packet->data[0];
#ifdef PYCSH_STDBUF_W_CTRL_CHARS
        PyUnicode_AppendAndDel(&stdbuf_reply_str, PyUnicode_FromStringAndSize((char*)&packet->data[1], packet->length - 1));
        if (again == 0 && packet->data[packet->length - 1] != LF) {
            /* If this was the last packet and there was no LF we will add one here to flush the stdout file */
            PyUnicode_AppendAndDel(&stdbuf_reply_str, PyUnicode_FromString("\n"));
        }
#else
        int i = 1;
        while (i < packet->length) {
            if (isprint(packet->data[i])) {
                PyUnicode_AppendAndDel(&stdbuf_reply_str, PyUnicode_FromStringAndSize((char*)&packet->data[i], 1));
                i++;
            } else if ((packet->data[i] == CR) || (packet->data[i] == LF)) {
                PyUnicode_AppendAndDel(&stdbuf_reply_str, PyUnicode_FromStringAndSize("\n", 1));
                while ((i < packet->length) && ((packet->data[i] == CR) || (packet->data[i] == LF))) {
                    i++;
                }
            } else  {
                // Other control character
                PyUnicode_AppendAndDel(&stdbuf_reply_str, PyUnicode_FromFormat("0x%02x", packet->data[i]));
                i++;
            }
        }
#endif
        csp_buffer_free(packet);

        if (again == 0) {
            break;
        }
    }

    csp_close(conn);

    return stdbuf_reply_str;
}

PyObject * pycsh_slash_stdbuf(PyObject * self, PyObject * args, PyObject * kwds) {
	(void)self;

	CSP_INIT_CHECK()

	unsigned int node = pycsh_dfl_node;
	unsigned int timeout = pycsh_dfl_timeout;
    unsigned int version = 2;
    //unsigned int paramversion = 2;

    //char * logfile = NULL;
    //optparse_add_string(parser, 'f', "logfile", "STRING", &logfile, "Log file name");

	static char *kwlist[] = {"node", "timeout", "version", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "|IIII:stdbuf", kwlist, &node, &timeout, &version)) {
		return NULL;
	}

    int result;
    if (version == 2) {
        return stdbuf_v2_py(node, timeout);
    } else if (version == 1) {
        PyErr_SetString(PyExc_NotImplementedError, "`pycsh.stdbuf()` currently only supports version 2");
        return NULL;
        //return stdbuf_v1_py(slash, node, timeout, paramversion);
    }

    return PyErr_Format(PyExc_ValueError, "`pycsh.stdbuf(..., version=...,)` can only be 1 or 2, not %d", version);
}
