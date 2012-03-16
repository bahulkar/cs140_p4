#ifndef __LIB_DEBUG_H
#define __LIB_DEBUG_H

// #include <list.h>
#include "lib/kernel/list.h"

/* GCC lets us add "attributes" to functions, function
   parameters, etc. to indicate their properties.
   See the GCC manual for details. */
#define UNUSED __attribute__ ((unused))
#define NO_RETURN __attribute__ ((noreturn))
#define NO_INLINE __attribute__ ((noinline))
#define PRINTF_FORMAT(FMT, FIRST) __attribute__ ((format (printf, FMT, FIRST)))

/* Halts the OS, printing the source file name, line number, and
   function name, plus a user-specific message. */
#define PANIC(...) debug_panic (__FILE__, __LINE__, __func__, __VA_ARGS__)

void debug_panic (const char *file, int line, const char *function,
                  const char *message, ...) PRINTF_FORMAT (4, 5) NO_RETURN;
void debug_backtrace (void);
void debug_backtrace_all (void);
void debug_validate_list (struct list *list);
void debug_print_list (struct list *list);

#endif



/* This is outside the header guard so that debug.h may be
   included multiple times with different settings of NDEBUG. */
#undef ASSERT
#undef NOT_REACHED

#ifndef NDEBUG
#define ASSERT(CONDITION)                                       \
        if (CONDITION) { } else {                               \
                PANIC ("assertion `%s' failed.", #CONDITION);   \
        }
#define NOT_REACHED() PANIC ("executed an unreachable statement");
#define DEBUG(fmt, ...) 	\
 	printf("%s/%s:%d "fmt, __FILE__, __FUNCTION__, __LINE__, __VA_ARGS__);

#else
#define ASSERT(CONDITION) ((void) 0)
#define NOT_REACHED() for (;;)
#define DEBUG(fmt, ...) {}
#endif /* lib/debug.h */
