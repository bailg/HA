#pragma once
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3
#define LOG_LEVEL_FATAL 4

#ifndef CURRENT_LOG_LEVEL
#define CURRENT_LOG_LEVEL LOG_LEVEL_INFO
#endif

#define LOG_PRINT(level, fmt, ...) do { \
    if (level >= CURRENT_LOG_LEVEL) { \
        struct timeval tv; \
        gettimeofday(&tv, NULL); \
        struct tm *tm_info = localtime(&tv.tv_sec); \
        char time_buf[64]; \
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info); \
        fprintf(stderr, "[%s.%03ld] [%s:%d] [%s] " fmt "\n", \
                time_buf, tv.tv_usec / 1000, \
                __FILE__, __LINE__, \
                #level, ##__VA_ARGS__); \
        /* 【关键修复】在重定向到文件时，强制刷新缓冲区，防止自动化测试因为全缓冲拿不到最新日志 */ \
        fflush(stderr); \
    } \
} while(0)

#define LOG_DEBUG(fmt, ...)  LOG_PRINT(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)   LOG_PRINT(LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)   LOG_PRINT(LOG_LEVEL_WARN, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)  LOG_PRINT(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...)  do { LOG_PRINT(LOG_LEVEL_FATAL, fmt, ##__VA_ARGS__); exit(1); } while(0)

/* CHECK 宏 - 错误处理断言 */
#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        LOG_ERROR("Check failed: %s, %s", #cond, msg); \
        return ERR_GENERIC; \
    } \
} while(0)
