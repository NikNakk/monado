// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief macOS XPC execution-context propagation for custom worker threads.
 * @ingroup aux_os
 */

#include "os_macos_xpc_context.h"

#include <Block.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

static pthread_mutex_t g_context_mutex = PTHREAD_MUTEX_INITIALIZER;
static dispatch_block_t g_context_block = NULL;
static uint32_t g_context_refcount = 0;
static uint64_t g_context_generation = 0;
static uint64_t g_last_logged_generation = 0;

static __thread os_macos_xpc_context_func_t g_tls_func = NULL;
static __thread void *g_tls_ptr = NULL;

static void
context_trampoline(void)
{
	if (g_tls_func != NULL) {
		g_tls_func(g_tls_ptr);
	}
}

void
os_macos_xpc_context_acquire_current(void)
{
	/*
	 * DISPATCH_BLOCK_ASSIGN_CURRENT captures properties of the current IPC
	 * request. When invoked directly later, libdispatch applies those
	 * properties to the calling thread for the duration of the block body.
	 */
	dispatch_block_t block = dispatch_block_create(DISPATCH_BLOCK_ASSIGN_CURRENT, ^{
		context_trampoline();
	});
	if (block == NULL) {
		fprintf(stderr, "XR_XPC_CONTEXT capture failed\n");
		fflush(stderr);
		return;
	}

	dispatch_block_t old = NULL;
	uint64_t generation = 0;
	uint32_t refcount = 0;

	pthread_mutex_lock(&g_context_mutex);
	old = g_context_block;
	g_context_block = block;
	g_context_refcount++;
	g_context_generation++;
	generation = g_context_generation;
	refcount = g_context_refcount;
	pthread_mutex_unlock(&g_context_mutex);

	if (old != NULL) {
		Block_release(old);
	}

	fprintf(stderr,
	        "XR_XPC_CONTEXT captured generation=%llu refs=%u\n",
	        (unsigned long long)generation,
	        refcount);
	fflush(stderr);
}

void
os_macos_xpc_context_release(void)
{
	dispatch_block_t released = NULL;
	uint32_t refcount = 0;

	pthread_mutex_lock(&g_context_mutex);
	if (g_context_refcount > 0) {
		g_context_refcount--;
	}
	refcount = g_context_refcount;
	if (g_context_refcount == 0 && g_context_block != NULL) {
		released = g_context_block;
		g_context_block = NULL;
	}
	pthread_mutex_unlock(&g_context_mutex);

	if (released != NULL) {
		Block_release(released);
	}

	fprintf(stderr, "XR_XPC_CONTEXT release refs=%u\n", refcount);
	fflush(stderr);
}

void
os_macos_xpc_context_run(os_macos_xpc_context_func_t func, void *ptr)
{
	if (func == NULL) {
		return;
	}

	dispatch_block_t block = NULL;
	uint64_t generation = 0;
	bool log_application = false;

	pthread_mutex_lock(&g_context_mutex);
	if (g_context_block != NULL) {
		block = Block_copy(g_context_block);
		generation = g_context_generation;
		if (g_last_logged_generation != generation) {
			g_last_logged_generation = generation;
			log_application = true;
		}
	}
	pthread_mutex_unlock(&g_context_mutex);

	if (block == NULL) {
		func(ptr);
		return;
	}

	if (log_application) {
		uint64_t thread_id = 0;
		(void)pthread_threadid_np(NULL, &thread_id);
		fprintf(stderr,
		        "XR_XPC_CONTEXT applied generation=%llu thread_id=%llu\n",
		        (unsigned long long)generation,
		        (unsigned long long)thread_id);
		fflush(stderr);
	}

	os_macos_xpc_context_func_t previous_func = g_tls_func;
	void *previous_ptr = g_tls_ptr;
	g_tls_func = func;
	g_tls_ptr = ptr;

	block();

	g_tls_func = previous_func;
	g_tls_ptr = previous_ptr;
	Block_release(block);
}
