/*
 * A library that needs libt_same.so (DT_NEEDED), built twice like it: as the
 * app's libt_user.so and as the runtime's libt_ruser.so. Each says which
 * libt_same.so it was given, and whether a name that only the other kind's
 * exports was bound for it (a weak import: zero when nobody of its own kind
 * has it).
 */
int t_same_which(void);
int t_same_runtime_only(void) __attribute__((weak));
int t_same_app_only(void) __attribute__((weak));

#ifdef RUNTIME
int t_ruser_which(void) { return 200 + t_same_which(); }
int t_ruser_stray(void) { return t_same_app_only ? t_same_app_only() : -1; }
#else
int t_user_which(void) { return 100 + t_same_which(); }
int t_user_stray(void) { return t_same_runtime_only ? t_same_runtime_only() : -1; }
#endif
