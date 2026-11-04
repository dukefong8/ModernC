/* Test-only: whether this platform can fork a child to observe a crash, trap or
 * sanitizer report from the suite. Included by each test source that does so. */
#ifndef TEST_FORK_H_
#define TEST_FORK_H_

#if defined(__unix__) || defined(__APPLE__) || defined(__COSMOPOLITAN__)
#include <sys/wait.h>
#include <unistd.h>
#define TEST_HAVE_FORK 1
#endif

#endif
