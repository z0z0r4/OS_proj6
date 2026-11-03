// A dynamically linked program: libxv6.so (printf, ...) and libgreet.so
// are found and bound by ldso at startup.
#include "kernel/types.h"
#include "user/user.h"

extern int greet_calls;
extern const char **greet_names;
void greet(const char *);
int greet_sum(int);

static void (*greeter)(const char *) = greet;   // a function pointer in the executable

int
main(int argc, char *argv[])
{
  greet("dynamic world");
  greeter(greet_names[1]);
  printf("calls=%d sum=%d argc=%d\n", greet_calls, greet_sum(10), argc);
  if(greet_calls != 2 || greet_sum(10) != 55)
    exit(1);
  printf("DYNHELLO_OK\n");
  return 0;
}
