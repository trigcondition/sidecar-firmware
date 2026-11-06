// shim_dwc/assert.h - test_gadget_ep0: an assert that names its line and aborts (Circle's would halt)
#pragma once
#include <cstdio>
#include <cstdlib>
#define assert(e) ((e) ? (void) 0 : (fprintf (stderr, "%s:%d: assert %s\n", __FILE__, __LINE__, #e), abort ()))
