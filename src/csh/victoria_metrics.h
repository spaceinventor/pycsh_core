/*
 * victoria_metrics.h
 *
 *  Created on: May 8, 2023
 *      Author: Edvard
 */
#pragma once

#include <pthread.h>
#include <param/param.h>

extern pthread_t vm_push_thread;

void vm_add(char * metric_line);
void vm_add_param(param_t * param);
void * vm_push(void * arg);
