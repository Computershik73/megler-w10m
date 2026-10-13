/*
 * One file name, two libraries (tools/build-guest.sh builds this twice): the
 * app's libt_same.so and, in the folder that stands for the Android root's,
 * the runtime's libt_same.so. The linker must give each asker the one of its
 * own kind (ld/hk_ld.h, "The runtime's libraries").
 */
#ifndef WHICH
#define WHICH 1
#endif

int t_same_which(void) { return WHICH; }

#if WHICH == 2
/* Only the runtime's has this: an app's library that imports the name must not be given it. */
int t_same_runtime_only(void) { return 22; }
#else
int t_same_app_only(void) { return 11; }
#endif
