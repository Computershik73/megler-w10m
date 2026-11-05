/*
 * A second guest library that needs the first (DT_NEEDED libt_basic.so): its
 * calls into it go through this library's PLT, whose entries the linker wrote
 * in ARM mode and the loader rewrote (arm/mg_patch.c) on armeabi-v7a, and which
 * are the ABI's own elsewhere.
 */
int t_add(int a, int b);
int *t_counter_ptr(void);
const char *t_hello(void);
double t_fmul(double a, float b);
extern int t_exported_value;
__SIZE_TYPE__ strlen(const char *);

int d_add_twice(int a, int b) { return t_add(t_add(a, b), b); }
int d_bump(void) { return ++*t_counter_ptr(); }
int d_hello_len(void) { return (int)strlen(t_hello()); }
double d_fmul(double a, float b) { return t_fmul(a, b) + 1.0; }
int d_value(void) { return t_exported_value; }
int (*d_add_ptr(void))(int, int) { return t_add; }

/* Many calls through the PLT, for the probe that looks for a thread interrupted on an entry. */
int d_spin(int n)
{
    int i, v = 0;
    for (i = 0; i < n; i++) v = t_add(v, 1);
    return v;
}
