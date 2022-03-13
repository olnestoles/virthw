#ifndef UTILS_H
#define UTILS_H

#include<stdio.h>

static inline void error(const char *s) {fprintf(stderr, "%s\n", s); exit(-1);}
#ifdef __ANDROID__
#include <android/log.h>

#define LOG_TAG "VIRT_HW"
#define I( ... ) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define D( ... ) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,__VA_ARGS__)

#else 

#define I printf
#define D printf

#endif

#endif /* UTILS_H */

