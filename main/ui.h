#pragma once
#include <stdbool.h>

#define EREADER_VERSION "1.0.0"

/* Runs the user interface forever. woke = returning from deep sleep. */
void ui_run(bool woke);
