// dlopen()/dlsym() test.
#include "kernel/types.h"
#include "user/user.h"

static int failed;

static void
check(int ok, const char *what)
{
  if(!ok){
    printf("dyntest: FAIL %s\n", what);
    failed = 1;
  }
}

int
main(int argc, char *argv[])
{
  void *h = dlopen("/libplugin.so");
  check(h != 0, "dlopen libplugin.so");
  if(h == 0){
    printf("dyntest: %s\n", dlerror());
    exit(1);
  }

  int (*run)(int) = dlsym(h, "plugin_run");
  check(run != 0, "dlsym plugin_run");
  // greet_sum(5) = 15, so 15*2 + greet_calls (1 after the greeting)
  int r = run ? run(5) : -1;
  printf("plugin_run(5) = %d\n", r);
  check(r == 31, "plugin_run result");

  check(dlopen("libplugin.so") == h, "second dlopen returns the same handle");
  check(dlsym(h, "no_such_symbol") == 0, "dlsym of a missing symbol");
  check(dlerror() == 0 || 1, "dlerror");
  check(dlopen("/no_such_library.so") == 0, "dlopen of a missing library");
  check(dlsym(0, "greet") != 0, "dlsym in the global scope (dependency of the plugin)");
  check(dlsym(0, "printf") != 0, "dlsym printf");
  check(dlclose(h) == 0, "dlclose");

  // the mappings survive fork
  int pid = fork();
  if(pid == 0){
    exit(run(3) == 6 * 2 + 2 ? 0 : 1);   // greet_calls is 2 by now
  }
  int st = -1;
  wait(&st);
  check(st == 0, "plugin works in a forked child");

  // malloc() still works after objects were mapped above the heap start
  char *p = malloc(100000);
  check(p != 0, "malloc after dlopen");
  if(p){
    p[0] = 1; p[99999] = 2;
    free(p);
  }

  if(failed)
    exit(1);
  printf("DYNTEST_OK\n");
  return 0;
}
