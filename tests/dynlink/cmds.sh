# Dynamic linking: ldso, libxv6.so, shared libraries and dlopen().
echo DYN_BEGIN

# load-time linking against libxv6.so and libgreet.so
dynhello
# dlopen / dlsym, a dependency pulled in by dlopen, fork afterwards
dyntest

# repeated exec / exit of dynamic programs must not leak memory or pages
dynhello
dynhello
dynhello
dynhello
dynhello
dynhello
dynhello
dynhello
dynhello
dynhello
dyntest

# statically linked programs are unaffected
echo STATIC_STILL_WORKS
hello

# a missing dependency is reported and does not hurt the system
rm /libgreet.so
dynhello
dyntest
echo STILL_ALIVE
echo DYN_DONE
