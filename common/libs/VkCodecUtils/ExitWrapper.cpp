#include "ExitWrapper.h"
#include <cstdio>
#include <cstdlib>

void safe_exit(int status, const char* function_name, int line_number, const char* description) {
    printf("Exiting from %s:%d - %s (status: %d)\n", function_name, line_number, description, status);
    exit(status);
}
