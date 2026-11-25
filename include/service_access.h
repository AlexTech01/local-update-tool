#pragma once
#include <switch.h>

/* Uses HEN's SM proxy without sending SM Initialize/Detach. */
Result labGetService(Service *sm_proxy, const char *name, Service *out);
