#include <debug.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Prints the call stack, that is, a list of addresses, one in
   each of the functions we are nested within.  gdb or addr2line
   may be applied to kernel.o to translate these into file names,
   line numbers, and function names.  */
void
debug_backtrace (void) 
{
  static bool explained;
  void **frame;
  
  printf ("Call stack: %p", __builtin_return_address (0));
  for (frame = __builtin_frame_address (1);
       (uintptr_t) frame >= 0x1000 && frame[0] != NULL;
       frame = frame[0]) 
    printf (" %p", frame[1]);
  printf (".\n");

  if (!explained) 
    {
      explained = true;
      printf ("The `backtrace' program can make call stacks useful.\n"
              "Read \"Backtraces\" in the \"Debugging Tools\" chapter\n"
              "of the Pintos documentation for more information.\n");
    }
}

void
debug_validate_list (struct list *list)
{
	if (list != NULL) {
		struct list_elem *e = &list->head;

		ASSERT (e->prev == NULL);
		while (e->next) {
			ASSERT (e->next->prev == e);
			e = e->next;
		}

		ASSERT (e->next == NULL);
	}
	else {
		printf("Error: Invalid list");
	}
}

void
debug_print_list (struct list *list)
{

	if (list != NULL) {
		struct list_elem *e = &list->head;

		ASSERT (e->prev == NULL);
		while (e->next) {
			printf("[%x {p=%x;n=%x}] -> ", (int)e, (int)e->prev, (int)e->next);
			ASSERT (e->next->prev == e);
			e = e->next;
		}

		printf("[%x {p=%x;n=%x}]\n", (int)e, (int)e->prev, (int)e->next);
		ASSERT (e->next == NULL);
	}
	else {
		printf("Error: Invalid list");
	}
}