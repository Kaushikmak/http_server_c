#include "logger.h"
#include <stdio.h>
#include <time.h>

static pthread_mutex_t log_lock;

void log_init(){
    pthread_mutex_init(&log_lock, NULL);
}

void log_destroy(){
    pthread_mutex_destroy(&log_lock);
}

void log_msg(int level, const char *msg){
    pthread_mutex_lock(&log_lock);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    char timebuf[64];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", t);

    const char *level_str;
    if(level == LOG_INFO) level_str = "INFO";
    else if(level == LOG_ERROR) level_str = "ERROR";
    else level_str = "DEBUG";

    printf("[%s] [%s] %s\n", timebuf, level_str, msg);

    pthread_mutex_unlock(&log_lock);
}