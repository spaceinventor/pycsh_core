/*
 * spaceboot_py.c
 *
 * Wrappers for csh/src/spaceboot_slash.c
 *
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <pycsh/pycsh.h>

#include "spaceboot_py.h"

#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <param/param.h>
#include <pycsh/utils.h>
#include <param/param_list.h>
#include <param/param_client.h>

#include <vmem/vmem.h>
#include <vmem/vmem_client.h>
#include <vmem/vmem_server.h>
#include <vmem/vmem_ram.h>

#include <csp/csp.h>
#include <csp/csp_cmp.h>
#include <csp/csp_crc32.h>

#include <apm/csh_api.h>
#include <slash/dflopt.h>

/* Custom exceptions */
PyObject * PyExc_ProgramDiffError;

static int ping(int node) {

	struct csp_cmp_message message = {};
	if (csp_cmp_ident(node, 3000, &message) != CSP_ERR_NONE) {
		printf("Cannot ping system\n");
		return -1;
	}
	printf("  | %s\n  | %s\n  | %s\n  | %s %s\n", message.ident.hostname, message.ident.model, message.ident.revision, message.ident.date, message.ident.time);
	return 0;
}

static int reset_to_flash(int node, int flash, int times, int type) {

	param_t * boot_img[4];
	/* Setup remote parameters */
	boot_img[0] = param_list_create_remote(21, node, PARAM_TYPE_UINT8, PM_CONF, 0, "boot_img0", NULL, NULL, -1);
	boot_img[1] = param_list_create_remote(20, node, PARAM_TYPE_UINT8, PM_CONF, 0, "boot_img1", NULL, NULL, -1);
	boot_img[2] = param_list_create_remote(22, node, PARAM_TYPE_UINT8, PM_CONF, 0, "boot_img2", NULL, NULL, -1);
	boot_img[3] = param_list_create_remote(23, node, PARAM_TYPE_UINT8, PM_CONF, 0, "boot_img3", NULL, NULL, -1);

	printf("  Switching to flash %d\n", flash);
	printf("  Will run this image %d times\n", times);

	char queue_buf[50];
	param_queue_t queue;
	param_queue_init(&queue, queue_buf, 50, 0, PARAM_QUEUE_TYPE_SET, 2);

	uint8_t zero = 0;
	param_queue_add(&queue, boot_img[0], 0, &zero);
	param_queue_add(&queue, boot_img[1], 0, &zero);
	if (type == 1) {
		param_queue_add(&queue, boot_img[2], 0, &zero);
		param_queue_add(&queue, boot_img[3], 0, &zero);
	}
	param_queue_add(&queue, boot_img[flash], 0, &times);
	param_push_queue(&queue, 1, 0, node, 1000, 0, false);

	printf("  Rebooting");
	csp_reboot(node);
	int step = 25;
	int ms = 1000;
	while (ms > 0) {
		printf(".");
		fflush(stdout);
		usleep(step * 1000);
		ms -= step;
	}
	printf("\n");

	for (int i = 0; i < 4; i++)
		param_list_destroy(boot_img[i]);

	return ping(node);
}

PyObject * slash_csp_switch(PyObject * self, PyObject * args, PyObject * kwds) {

	CSP_INIT_CHECK()

    unsigned int slot;
	unsigned int node = pycsh_dfl_node;
	unsigned int times = 1;

    static char *kwlist[] = {"slot", "node", "times", NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "I|II:switch", kwlist, &slot, &node, &times))
		return NULL;  // TypeError is thrown

	int type = 0;
	if (slot >= 2)
		type = 1;

	if (reset_to_flash(node, slot, times, type) != 0) {
        PyErr_SetString(PyExc_ConnectionError, "Cannot ping system");
        return NULL;
    }

	Py_RETURN_NONE;
}

static vmem_list_t vmem_list_find(int node, int timeout, char * name, int namelen) {
	vmem_list_t ret = {};

	csp_conn_t * conn = csp_connect(CSP_PRIO_HIGH, node, VMEM_PORT_SERVER, timeout, CSP_O_CRC32);
	if (conn == NULL)
		return ret;

	csp_packet_t * packet = csp_buffer_get(sizeof(vmem_request_t));
	vmem_request_t * request = (void *)packet->data;
	request->version = 1;
	request->type = VMEM_SERVER_LIST;
	packet->length = sizeof(vmem_request_t);

	csp_send(conn, packet);

	/* Wait for response */
	packet = csp_read(conn, timeout);
	if (packet == NULL) {
		fprintf(stderr, "No response\n");
		csp_close(conn);
		return ret;
	}

	for (vmem_list_t * vmem = (void *)packet->data; (intptr_t)vmem < (intptr_t)packet->data + packet->length; vmem++) {
		// printf(" %u: %-5.5s 0x%08X - %u typ %u\r\n", vmem->vmem_id, vmem->name, (unsigned int) be32toh(vmem->vaddr), (unsigned int) be32toh(vmem->size), vmem->type);
		if (strncmp(vmem->name, name, namelen) == 0) {
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

static int image_get(char * filename, char ** data, int * len) {

	/* Open file */
	FILE * fd = fopen(filename, "r");
	if (fd == NULL) {
		printf("  Cannot find file: %s\n", filename);
		return -1;
	}

	/* Read size */
	struct stat file_stat;
	fstat(fd->_fileno, &file_stat);

	/* Copy to memory:
	 * Note we ignore the memory leak because the application will terminate immediately after using the data */
	*data = malloc(file_stat.st_size);
	*len = fread(*data, 1, file_stat.st_size, fd);
	fclose(fd);

	return 0;
}

#if 0
static void upload(int node, int address, char * data, int len) {

	unsigned int timeout = 10000;
	printf("  Upload %u bytes to node %u addr 0x%x\n", len, node, address);
	vmem_upload(node, timeout, address, data, len, 1);
	printf("  Waiting for flash driver to flush\n");
	usleep(100000);
}
#endif


#define BIN_PATH_MAX_ENTRIES 10
#define BIN_PATH_MAX_SIZE 256

typedef struct bin_file_ident_s {
	bool valid;
	char hostname[BIN_PATH_MAX_ENTRIES+1];
	char model[BIN_PATH_MAX_ENTRIES+1];
	char version_string[BIN_PATH_MAX_ENTRIES+1];
	uint32_t stext;
} bin_file_ident_t;


struct bin_info_t {
	uint32_t addr_min;
	uint32_t addr_max;
	unsigned count;
	char entries[BIN_PATH_MAX_ENTRIES][BIN_PATH_MAX_SIZE];
};
struct bin_info_t bin_info __attribute__((weak));

// Binary file byte offset of entry point address.
// C21: 4, E70: 2C4
static const uint32_t entry_offsets[] = { 4, 0x2c4 };

static bool is_valid_binary(const char * path, struct bin_info_t * binf, bin_file_ident_t * binf_ident)
{
	binf_ident->valid = false;

	/* 1. does the file have the .bin extention */
	int len = strlen(path);
	if ((len <= 4) || (strcmp(&(path[len-4]), ".bin") != 0)) {
		return false;
	}

	/* 2. read all the data from the file */
	char * data;
	if (image_get((char*)path, &data, &len) < 0) {
		return false;
	}

	/* 3. detect if there is a IDENT structure at the end */
	bool ident_found = false;
	char *ident_str[3] = { &binf_ident->hostname[0], &binf_ident->model[0], &binf_ident->version_string[0] };
	int32_t idx = -4;
	if (!memcmp(&data[len + idx], "\xC0\xDE\xBA\xD0", 4)) {
		idx -= 4;

		/* 3.1. grab the stext address (start of text) */
		binf_ident->stext = ((uint32_t *)(&data[len + idx]))[0];

		/* 3.2. verify that the entry lies within the vmem area to be programmed */
		if ((binf->addr_min <= binf_ident->stext) && (binf->addr_max >= binf_ident->stext) && ((binf->addr_min + len) <= binf->addr_max)) {
			/* 3.2.1. scan for an other magic marker */
			char *ident_begin = NULL;
			do {
				idx--;
				if (!memcmp(&data[len + idx], "\xBA\xD0\xFA\xCE", 4)) {
					ident_begin = &data[len + idx + 4];
					break;
				}
			} while (idx >= -256);
			/* 3.2.2. if we found the beginning, we can extract IDENT strings */
			char *ident_iter;
			uint8_t ident_id;
			for (ident_iter = ident_begin, ident_id = 0; ident_iter && (ident_iter < &data[len - 4] && ident_id < 3); ident_id++) {
				if (ident_iter) {
					strncpy(ident_str[ident_id], ident_iter, 32);
				}
				ident_iter += (strlen(ident_iter) + 1);
				ident_found = true;
				binf_ident->valid = true;
			}
		} else {
			/* We found the magic marker and the entry point address, but it did not match the area */
			free(data);
			return false;
		}
	}

	if (!ident_found) {
		/* 4. analyze the "magic position" for a valid value - might be the Reset_Handler address in the vector table */
		if (binf->addr_min + len <= binf->addr_max) {
			uint32_t addr = 0;
			for (size_t i = 0; i < sizeof(entry_offsets)/sizeof(uint32_t); i++) {
				addr = *((uint32_t *) &data[entry_offsets[i]]);
				if ((binf->addr_min <= addr) && (addr <= binf->addr_max)) {
					free(data);
					return true;
				}
			}
		}
	}

	free(data);
	return ident_found;
}

static int upload_and_verify(int node, int address, char * data, int len) {

	unsigned int timeout = 10000;
	printf("  Upload %u bytes to node %u addr 0x%x\n", len, node, address);
	vmem_upload(node, timeout, address, data, len, 1);

	char * datain = malloc(len);
	vmem_download(node, timeout, address, len, datain, 1, 1);

	for (int i = 0; i < len; i++) {
		if (datain[i] == data[i])
			continue;
		printf("Diff at %x: %hhx != %hhx\n", address + i, data[i], datain[i]);
		free(datain);
		return -1;
	}

	free(datain);
	return 0;
}

unsigned int rdp_tmp_window __attribute__((weak));
unsigned int rdp_tmp_conn_timeout __attribute__((weak));
unsigned int rdp_tmp_packet_timeout __attribute__((weak));
unsigned int rdp_tmp_delayed_acks __attribute__((weak));
unsigned int rdp_tmp_ack_timeout __attribute__((weak));
unsigned int rdp_tmp_ack_count __attribute__((weak));

unsigned int rdp_dfl_window __attribute__((weak));
unsigned int rdp_dfl_conn_timeout __attribute__((weak));
unsigned int rdp_dfl_packet_timeout __attribute__((weak));
unsigned int rdp_dfl_ack_timeout __attribute__((weak));
unsigned int rdp_dfl_ack_count __attribute__((weak));
unsigned int rdp_dfl_delayed_acks __attribute__((weak));

 __attribute__((weak)) void rdp_opt_set(void) {

	csp_rdp_set_opt(rdp_tmp_window, rdp_tmp_conn_timeout, rdp_tmp_packet_timeout, rdp_tmp_delayed_acks, rdp_tmp_ack_timeout, rdp_tmp_ack_count);

	printf("Using RDP options window: %u, conn_timeout: %u, packet_timeout: %u, ack_timeout: %u, ack_count: %u\n",
        rdp_tmp_window, rdp_tmp_conn_timeout, rdp_tmp_packet_timeout, rdp_tmp_ack_timeout, rdp_tmp_ack_count);
}

 __attribute__((weak)) void rdp_opt_reset(void) {

	csp_rdp_set_opt(rdp_dfl_window, rdp_dfl_conn_timeout, rdp_dfl_packet_timeout, rdp_dfl_delayed_acks, rdp_dfl_ack_timeout, rdp_dfl_ack_count);
}

static void _auto_reset_rdp(void ** stuff) {
	rdp_opt_reset();
}

#define RDP_KWARGS "window", "conn_timeout", "packet_timeout", "delayed_acks", "ack_timeout", "ack_count"
#define RDP_OPTS &rdp_tmp_window, &rdp_tmp_conn_timeout, &rdp_tmp_packet_timeout, &rdp_tmp_delayed_acks, &rdp_tmp_ack_timeout, &rdp_tmp_ack_count
#define RDP_TYPESTR "IIIIII"

PyObject * pycsh_csh_program(PyObject * self, PyObject * args, PyObject * kwds) {

	CSP_INIT_CHECK()

    unsigned int slot;
	char * filename = NULL;
	unsigned int node = pycsh_dfl_node;

	int do_crc32 = false;

	/* RDPOPT - Keyword-only */
	rdp_tmp_window = rdp_dfl_window;
	rdp_tmp_conn_timeout = rdp_dfl_conn_timeout;
	rdp_tmp_packet_timeout = rdp_dfl_packet_timeout;
	rdp_tmp_delayed_acks = rdp_dfl_delayed_acks;
	rdp_tmp_ack_timeout = rdp_dfl_ack_timeout;
	rdp_tmp_ack_count = rdp_dfl_ack_count;

    static char *kwlist[] = {"slot", "filename", "node", "do_crc32", RDP_KWARGS, NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "Is|Ip$"RDP_TYPESTR":program", kwlist, &slot, &filename, &node, &do_crc32, RDP_OPTS))
		return NULL;  // TypeError is thrown

	/* Temporarily set RDP options */
	rdp_opt_set();
	void * rdp_cleanup __attribute__((cleanup(_auto_reset_rdp))) = NULL;

	char vmem_name[5];
	snprintf(vmem_name, 5, "fl%u", slot);

	printf("  Requesting VMEM name: %s...\n", vmem_name);

	vmem_list_t vmem = vmem_list_find(node, 5000, vmem_name, strlen(vmem_name));
	if (vmem.size == 0) {
		PyErr_SetString(PyExc_ConnectionError, "Failed to find vmem on subsystem\n");
		return NULL;
	} else {
		printf("  Found vmem\n");
		printf("    Base address: 0x%x\n", vmem.vaddr);
		printf("    Size: %u\n", vmem.size);
	}

    assert(filename != NULL);
    strncpy(bin_info.entries[0], filename, BIN_PATH_MAX_SIZE-1);
	bin_info.addr_min = vmem.vaddr;
	bin_info.addr_max = (vmem.vaddr + vmem.size) - 1;
    bin_info.count = 0;

	char * path = bin_info.entries[0];

	bin_file_ident_t binf_ident;
	if (!is_valid_binary(path, &bin_info, &binf_ident)) {
		PyErr_Format(PyExc_LookupError, "%s is not a valid firmware for %s on node %d", path, vmem.name, node);
		return NULL;
	}

    printf("\033[31m\n");
    printf("ABOUT TO PROGRAM: %s\n", path);
    printf("\033[0m\n");
    if (ping(node) < 0) {
        PyErr_Format(PyExc_ConnectionError, "No Response from node %d", node);
		return NULL;
	}
    printf("\n");

	char * data CLEANUP_STR = NULL;
	int len;
	if (image_get(path, &data, &len) < 0) {
        PyErr_SetString(PyExc_IOError, "Failed to open file");
		return NULL;
	}

	if (do_crc32) {
		uint32_t crc;
		crc = csp_crc32_memory((const uint8_t *)data, len);
		printf("  File CRC32: 0x%08"PRIX32"\n", crc);
		printf("  Upload %u bytes to node %u addr 0x%"PRIX32"\n", len, node, vmem.vaddr);
		vmem_upload(node, 10000, vmem.vaddr, data, len, 1);
		uint32_t crc_node;
		int res = vmem_client_calc_crc32(node, 10000, vmem.vaddr, len, &crc_node, 1);
		if (res < 0) {
			printf("\033[31m\n");
			printf("  Communication failure: %"PRId32"\n", res);
			printf("\033[0m\n");
			PyErr_Format(PyExc_ConnectionError, "No response from node %d", node);
			return NULL;
		}

		if (crc_node != crc) {
			printf("\033[31m\n");
			printf("  Failure: %"PRIX32" != %"PRIX32"\n", crc, crc_node);
			printf("\033[0m\n");
			PyErr_Format(PyExc_ProgramDiffError, "CRC32 mismatch: %"PRIX32" != %"PRIX32"", crc, crc_node);
			return NULL;
		}

		printf("\033[32m\n");
		printf("  Success\n");
		printf("\033[0m\n");
		Py_RETURN_NONE;
	}

	if (upload_and_verify(node, vmem.vaddr, data, len) != 0) {
		PyErr_SetString(PyExc_ProgramDiffError, "Diff during download (upload/download mismatch)");
		return NULL;
	}

	Py_RETURN_NONE;
}

PyObject * slash_sps(PyObject * self, PyObject * args, PyObject * kwds) {

	CSP_INIT_CHECK()

    unsigned int from;
    unsigned int to;
    char * filename = NULL;
	unsigned int node = pycsh_dfl_node;

	/* RDPOPT - Keyword-only */
	rdp_tmp_window = rdp_dfl_window;
	rdp_tmp_conn_timeout = rdp_dfl_conn_timeout;
	rdp_tmp_packet_timeout = rdp_dfl_packet_timeout;
	rdp_tmp_delayed_acks = rdp_dfl_delayed_acks;
	rdp_tmp_ack_timeout = rdp_dfl_ack_timeout;
	rdp_tmp_ack_count = rdp_dfl_ack_count;

    static char *kwlist[] = {"from_", "to", "filename", "node", RDP_KWARGS, NULL};

	if (!PyArg_ParseTupleAndKeywords(args, kwds, "IIs|I$"RDP_TYPESTR":sps", kwlist, &from, &to, &filename, &node, RDP_OPTS))
		return NULL;  // TypeError is thrown

	/* Temporarily set RDP options */
	rdp_opt_set();
	void * rdp_cleanup __attribute__((cleanup(_auto_reset_rdp))) = NULL;

	int type = 0;
	if (from >= 2)
		type = 1;
	if (to >= 2)
		type = 1;

	reset_to_flash(node, from, 1, type);

	char vmem_name[5];
	snprintf(vmem_name, 5, "fl%u", to);
	printf("  Requesting VMEM name: %s...\n", vmem_name);

	vmem_list_t vmem = vmem_list_find(node, 5000, vmem_name, strlen(vmem_name));
	if (vmem.size == 0) {
		PyErr_SetString(PyExc_ConnectionError, "Failed to find vmem on subsystem\n");
		return NULL;
	} else {
		printf("  Found vmem\n");
		printf("    Base address: 0x%x\n", vmem.vaddr);
		printf("    Size: %u\n", vmem.size);
	}

	assert(filename != NULL);
    strncpy(bin_info.entries[0], filename, BIN_PATH_MAX_SIZE-1);
	bin_info.addr_min = vmem.vaddr;
	bin_info.addr_max = (vmem.vaddr + vmem.size) - 1;
    bin_info.count = 0;

	char * path = bin_info.entries[0];

	bin_file_ident_t binf_ident;
	if (!is_valid_binary(path, &bin_info, &binf_ident)) {
		PyErr_Format(PyExc_LookupError, "%s is not a valid firmware for %s on node %d", path, vmem.name, node);
		return NULL;
	}

    printf("\033[31m\n");
    printf("ABOUT TO PROGRAM: %s\n", path);
    printf("\033[0m\n");
    if (ping(node) < 0) {
		PyErr_SetString(PyExc_ConnectionError, "Cannot ping system");
        return NULL;
	}
    printf("\n");

	char * data;
	int len;
	if (image_get(path, &data, &len) < 0) {
		PyErr_SetString(PyExc_IOError, "Failed to open file");
		return NULL;
	}

	int result = upload_and_verify(node, vmem.vaddr, data, len);
	if (result != 0) {
        PyErr_SetString(PyExc_ProgramDiffError, "Diff during download (upload/download mismatch)");
        return NULL;
	}

    if (reset_to_flash(node, to, 1, type)) {
        PyErr_SetString(PyExc_ConnectionError, "Cannot ping system");
        return NULL;
    }

    Py_RETURN_NONE;
}
