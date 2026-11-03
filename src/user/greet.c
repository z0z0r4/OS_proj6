// libgreet.so: a small shared library that exercises data relocations
// (a pointer table, an exported variable), a constructor and calls into
// libxv6.so.
#include "kernel/types.h"
#include "user/user.h"

int greet_calls;                          // exported variable
static int ctor_ran;

__attribute__((constructor)) static void
greet_init(void)
{
  ctor_ran = 1;
}

static const char *names[] = { "alpha", "beta", "gamma" };
const char **greet_names = names;         // pointers that need R_*_RELATIVE

void
greet(const char *who)
{
  greet_calls++;
  printf("hello, %s (constructor ran: %d)\n", who, ctor_ran);
}

int
greet_sum(int n)
{
  int s = 0;
  for(int i = 1; i <= n; i++)
    s += i;
  return s;
}
