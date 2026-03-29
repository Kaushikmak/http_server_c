#ifndef LOGGER_H
#define LOGGER_H

#include <pthread.h>

#define LOG_INFO  1
#define LOG_ERROR 2
#define LOG_DEBUG 3

void log_init();
void log_destroy();


void log_msg(int level, const char *msg);

#endif