#pragma once

#include <cstdio>
#include <cstdlib>

void safe_exit(int status, const char* function_name, int line_number, const char* description);

#define SAFE_EXIT(status, description) safe_exit(status, __FUNCTION__, __LINE__, description)
