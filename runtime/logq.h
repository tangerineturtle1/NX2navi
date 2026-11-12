// Log writes on a thread of their own (logq.c). main.c owns the log's destinations and hands every line here.
#pragma once
#include <stddef.h>

typedef void (*LogqSink)(const char *p, size_t n);   // writes the bytes to the log's destinations; may block

void logq_init(LogqSink sink);              // before the first logq_write; lines are written by the thread that logs them
int logq_async(void);                       // from now on the log thread writes them; 0 when it could not be started
void logq_write(const char *p, size_t n);   // queues the bytes, or writes them when the queue is off or has no room
void logq_sync(void);                       // writes what is queued, on the calling thread, and turns the queue off
