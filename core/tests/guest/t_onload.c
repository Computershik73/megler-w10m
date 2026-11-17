/*
 * Two guest libraries that stand in for Microsoft OneDrive's libcore.so and the libQt.so it needs, for the Dalvik
 * runtime's test of which JNI_OnLoad a library loaded by Java gets (core/tests/mg_dvm_tests_jni.c, test_onload_of_needed):
 *
 *   libt_onload_dep.so (-DDEP)  has the JNI_OnLoad, which keeps the JavaVM it is given, as libQt.so's does;
 *   libt_onload.so              needs that one, has no JNI_OnLoad of its own and reads the JavaVM kept there, as
 *                               libcore.so does.
 *
 * Java loads the second alone. ART finds JNI_OnLoad with dlsym(handle), which looks in the library and then in what it
 * needs, so the first's is run (core/src/dvm/mg_dvm_jvm.c, load_native).
 */
#ifdef DEP
void *t_onload_dep_vm;                 /* the JavaVM its JNI_OnLoad was given last */
int t_onload_dep_calls;                /* how many times its JNI_OnLoad has run */

int JNI_OnLoad(void *vm, void *reserved)
{
    (void)reserved;
    t_onload_dep_vm = vm;
    t_onload_dep_calls++;
    return 0x10006;                    /* JNI_VERSION_1_6 */
}
#else
extern void *t_onload_dep_vm;
extern int t_onload_dep_calls;

void *t_onload_vm(void) { return t_onload_dep_vm; }
int t_onload_calls(void) { return t_onload_dep_calls; }
#endif
