// Copyright 2026, Nick Kennedy
// SPDX-License-Identifier: BSL-1.0
#include <stdio.h>
extern void vrrt_init(const char *);
extern void *vrrt_dlsym(const char *);
int main(void) { vrrt_init(""); if(!vrrt_dlsym("VRClientCoreFactory") || vrrt_dlsym("not_an_export")) return 1; puts("PASS native xrizer factory forwarding; unknown symbol rejected"); }
