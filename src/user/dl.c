// dlopen() and friends for programs linked against libxv6.so.  The
// implementation lives in the dynamic linker (ldso.c), which fills in
// __dl_hooks when the program starts.
#include "kernel/types.h"
#include "user/user.h"

struct dl_hooks {
  void *(*open)(const char *);
  void *(*sym)(void *, const char *);
  int (*close)(void *);
  const char *(*error)(void);
};

struct dl_hooks __dl_hooks;

void *
dlopen(const char *path)
{
  return __dl_hooks.open ? __dl_hooks.open(path) : 0;
}

void *
dlsym(void *handle, const char *name)
{
  return __dl_hooks.sym ? __dl_hooks.sym(handle, name) : 0;
}

int
dlclose(void *handle)
{
  return __dl_hooks.close ? __dl_hooks.close(handle) : -1;
}

const char *
dlerror(void)
{
  return __dl_hooks.error ? __dl_hooks.error() : "not dynamically linked";
}
