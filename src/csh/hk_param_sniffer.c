/*
 * hk_param_sniffer.c
 *
 *  Created on: Mar 2, 2022
 *      Author: Troels
 */

#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <param/param_server.h>
#include <param/param_queue.h>
#include <param/param_serializer.h>
#include <mpack/mpack.h>
#include <csp/csp.h>
#include <csp/csp_crc32.h>

#include <slash/slash.h>
#include <slash/optparse.h>
#include <apm/csh_api.h>
#include <hk/hk.h>

#include "param_sniffer.h"
#include <hk/hk.h>

bool hk_param_sniffer(csp_packet_t * packet) {

	if (packet->id.sport != 13) {
		return false;
	}

	if (param_sniffer_crc(packet) < 0) {
		return false;
	}

	/* Protocol has a header size of 5, and RDP adds 5 bytes to the end of the packet if activated */
	size_t header_size = 5;
	size_t data_len = packet->length - header_size - ((packet->id.flags & CSP_FRDP) ? 5 : 0);
	param_queue_t queue;
	param_queue_init(&queue, &packet->data[header_size], data_len, data_len, PARAM_QUEUE_TYPE_SET, 2);
	queue.last_node = packet->id.src;

	mpack_reader_t reader;
	mpack_reader_init_data(&reader, queue.buffer, queue.used);
	static bool epoch_notfound_warning = false; // Only print this warning once
	while (reader.data < reader.end) {
		int id, node, offset = -1;
		csp_timestamp_t timestamp = { .tv_sec = 0, .tv_nsec = 0 };
		param_deserialize_id(&reader, &id, &node, &timestamp, &offset, &queue);
		if (node == 0) {
			node = packet->id.src;
		}
		const param_t * param = param_list_find_id(node, id);
		if (param) {
			*param->timestamp = timestamp;
			if (param->timestamp->tv_sec == 0) {
				printf("HK: Param timestamp is missing for %u:%s, logging is aborted\n", *(param->node), param->name);
				break;
			}

			time_t local_epoch = -1;
			/* Only use local epoch if not receiving a UTC timestamp. 1577836800: Jan 1st 2020 */
			if (param->timestamp->tv_sec < 1577836800) {
				if(false == hk_sync_epoch(&reader, node, packet->id.src, param, &timestamp, &local_epoch)) {
					if(!epoch_notfound_warning) {
						printf("HK: No local epoch found for node %u, skipping %u %u %u\n", packet->id.src, *param->node, param->id, param->timestamp->tv_sec);
						epoch_notfound_warning = true;
					}
					mpack_discard(&reader);
					continue;
				}

				param->timestamp->tv_sec += local_epoch;
			}
			param_sniffer_log(NULL, &queue, param, offset, &reader, param->timestamp);
		} else {
			printf("HK: Found unknown param node %d id %d\n", node, id);
			mpack_discard(&reader);
			continue;
		}
	}
	return true;
}
