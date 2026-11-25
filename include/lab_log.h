#pragma once
#include <switch.h>
#include <stdbool.h>

bool labLogOpen(void);
void labLogClose(void);
void labLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
void labLogResult(const char *operation, Result rc);
void labLogBytes(const char *name, const void *buffer, size_t size);

