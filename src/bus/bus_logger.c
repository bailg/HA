// bus_logger.c — Consumer for SHM audit queue: drains entries to a file
//
// Opens the named shared-memory queue (created by the bus) in read-only mode,
// writes every entry as a 1 KB line to the output file, and sleeps briefly
// when the queue is empty.  The output grows unboundedly — intended for the
// ~1 billion writes/day scenario.
//
// Usage:  bin/bus_logger                (default: ./device_audit.log)
//         bin/bus_logger /path/to/file
//
// Signal handling:
//   SIGINT / SIGTERM — drain remaining entries and exit cleanly.
#define _POSIX_C_SOURCE 199309L

#include "common/shm_queue.h"
#include "common/logging.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>

// ========================================================================
// Static globals
// ========================================================================

static volatile int g_running = 1;   // set to 0 on SIGINT / SIGTERM
static ShmQueue *g_queue = NULL;     // shared-memory queue handle
static FILE *g_output = NULL;        // output file handle
static int g_consumer_id = -1;       // registered consumer id

// ========================================================================
// Signal handling
// ========================================================================

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static void setup_logger_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}

// ========================================================================
// Main
// ========================================================================

int main(int argc, char *argv[]) {
    const char *output_path = (argc > 1) ? argv[1] : "./device_audit.log";

    setup_logger_signals();

    g_queue = shm_queue_open(SHM_QUEUE_NAME, 0);
    if (!g_queue) {
        LOG_FATAL("Cannot open SHM queue %s — is the bus running?", SHM_QUEUE_NAME);
    }
    g_consumer_id = shm_queue_register(g_queue);
    if (g_consumer_id < 0) {
        LOG_FATAL("Cannot register SHM queue consumer");
    }

    g_output = fopen(output_path, "ab");
    if (!g_output) {
        LOG_FATAL("Cannot open output file %s: %s", output_path, strerror(errno));
    }
    setbuf(g_output, NULL);   // unbuffered for crash safety
    LOG_INFO("bus_logger started, writing to %s", output_path);

    uint8_t buf[SHM_QUEUE_ENTRY_SIZE];
    while (g_running) {
        size_t n = shm_queue_dequeue(g_queue, buf, sizeof(buf), g_consumer_id);
        if (n > 0) {
            fwrite(buf, 1, SHM_QUEUE_ENTRY_SIZE, g_output);
            fputc('\n', g_output);
        } else {
            // Queue empty — avoid busy-looping
            poll(NULL, 0, 10);  // 10 ms
        }
    }

    // Drain remaining entries on graceful shutdown
    LOG_INFO("bus_logger shutting down, draining remaining entries...");
    for (;;) {
        size_t n = shm_queue_dequeue(g_queue, buf, sizeof(buf), g_consumer_id);
        if (n == 0) break;
        fwrite(buf, 1, SHM_QUEUE_ENTRY_SIZE, g_output);
        fputc('\n', g_output);
    }
    LOG_INFO("bus_logger drain complete, exiting.");

    fclose(g_output);
    shm_queue_close(g_queue, SHM_QUEUE_NAME);
    return 0;
}
