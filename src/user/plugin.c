// libplugin.so: loaded with dlopen(); it needs libgreet.so, which then
// gets loaded as its dependency.
#include "kernel/types.h"
#include "user/user.h"

extern int greet_calls;
void greet(const char *);
int greet_sum(int);

int
plugin_run(int n)
{
  greet("plugin");
  return greet_sum(n) * 2 + greet_calls;
}
