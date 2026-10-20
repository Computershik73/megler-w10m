/*
 * A program a real compiler made: core/tests/dvm/d8/src/d8t/Main.java through
 * javac and d8 (tools/dvm-d8-fixture.sh), kept as d8t.dex. The same class files
 * were run on the JVM, and expected.txt is what it printed: one line for each
 * test, "name=result". Each test is called here and must return that text.
 *
 * It is the one place where the expected answers come from Java's own
 * implementation: the loops, switches, inner classes, enums, lambdas,
 * try-with-resources and string concatenation are as d8 writes them, not as
 * someone wrote them in smali.
 */
#include <stdio.h>

#include "../src/dvm/mg_dvm_internal.h"
#include "mg_dvm_tests.h"

void test_d8(const char *fixtures)
{
    char *path = mg_sprintf("%s/expected.txt", fixtures), *dex = mg_sprintf("%s/d8t.dex", fixtures);
    size_t size = 0;
    char *expected = (char *)mg_file_read_all(path, &size), *line, *next;
    mg_dvm_stats before, after;
    int tests = 0;
    say("a program compiled by javac and d8, against what the JVM printed");
    CHECK(expected != NULL);
    CHECK(mg_dvm_add_dex_file(dex));
    free(path);
    free(dex);
    if (!expected) return;
    mg_dvm_get_stats(&before);
    for (line = expected; *line; line = next) {
        char *eq, *end = strchr(line, '\n'), stack[1200];
        mg_jvalue r;
        next = end ? end + 1 : line + strlen(line);
        if (end) *end = 0;
        if (end && end > line && end[-1] == '\r') end[-1] = 0;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        tests++;
        r.j = 0;
        if (!mg_dvm_call_static("d8t/Main", line, "()Ljava/lang/String;", NULL, &r)) {
            /* Where it threw, which says more than that the text was not what was expected. */
            mg_dvm_pending_stack(stack, sizeof(stack));
            say("  %s threw %s\n%s", line, thrown(), stack);
            r.l = NULL;
        }
        g_checks++;
        if (strcmp(text_of(r.l), eq + 1) != 0) {
            g_failed++;
            say("  FAIL d8t.Main.%s():\n    got      %s\n    expected %s", line, text_of(r.l), eq + 1);
        }
    }
    free(expected);
    CHECK_INT(tests, 10);
    mg_dvm_get_stats(&after);
    say("  %d tests; they linked %u classes and made %u objects", tests, (unsigned)(after.classes_linked - before.classes_linked),
        (unsigned)(after.objects - before.objects));
}
