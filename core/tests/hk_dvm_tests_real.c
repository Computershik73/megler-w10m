/*
 * A real app's DEX: the Java side of a Unity game, written by a real compiler,
 * with no android.* and little of java.* to stand on.
 *
 * Three things are asked of it. That every class, method and instruction of
 * the file can be read (which needs nothing but the file). That its classes
 * link as far as their ancestors exist, and say what was missing where they
 * do not. And that its code runs: a few methods whose results are known, and
 * then everything that can be called with no objects to hand, to see that no
 * bytecode a real compiler wrote takes the interpreter anywhere but to a
 * result or to a Java exception.
 */
#include <stdio.h>

#include "../src/dvm/hk_dvm_internal.h"
#include "../include/meglet.h"
#include "hk_dvm_tests.h"

typedef struct tally {
    char what[96];
    int count;
} tally;

static void count_in(tally *t, int cap, const char *what)
{
    int i;
    for (i = 0; i < cap && t[i].count; i++)
        if (!strcmp(t[i].what, what)) {
            t[i].count++;
            return;
        }
    if (i < cap) {
        hk_strlcpy(t[i].what, what, sizeof(t[i].what));
        t[i].count = 1;
    }
}

static void say_tally(const char *indent, tally *t, int cap, int most)
{
    int shown, i, best;
    for (shown = 0; shown < most; shown++) {
        best = -1;
        for (i = 0; i < cap && t[i].count; i++)
            if (t[i].count > 0 && (best < 0 || t[i].count > t[best].count)) best = i;
        if (best < 0) break;
        say("%s%4d  %s", indent, t[best].count, t[best].what);
        t[best].count = -t[best].count;
    }
}

/* Every class definition of one file, read with nothing linked: its members, and every instruction of every method. */
static void read_everything(hk_dvm_dex *d, u32 *fields, u32 *methods, u32 *with_code, u32 *instructions, u32 *bad)
{
    const u8 *end = d->b + d->size;
    u32 i;
    for (i = 0; i < d->ncls; i++) {
        const u8 *def = d->b + d->cls_off + 32 * i, *p;
        u32 data_off = hk_dvm_rd32(def + 24), n[4], k, g;
        if (!data_off) continue;
        if (data_off >= d->size) {
            (*bad)++;
            continue;
        }
        p = d->b + data_off;
        for (k = 0; k < 4; k++) n[k] = hk_dvm_uleb(&p);
        for (g = 0; g < 4; g++) {
            for (k = 0; k < n[g] && p < end; k++) {
                u32 code_off;
                hk_dvm_uleb(&p);
                hk_dvm_uleb(&p);
                if (g < 2) {
                    (*fields)++;
                    continue;
                }
                (*methods)++;
                code_off = hk_dvm_uleb(&p);
                if (code_off) {
                    const u8 *ci = d->b + code_off;
                    const u16 *insns = (const u16 *)(const void *)(ci + 16);
                    u32 ninsns, pc = 0;
                    if ((u64)code_off + 16 > d->size) {
                        (*bad)++;
                        continue;
                    }
                    ninsns = hk_dvm_rd32(ci + 12);
                    (*with_code)++;
                    /* Instruction after instruction must land exactly on the end of the method. */
                    while (pc < ninsns) {
                        u32 units = hk_dvm_insn_units(insns, ninsns, pc);
                        if (!units) break;
                        if (!((insns[pc] & 0xFF) == 0 && insns[pc] != 0)) (*instructions)++;
                        pc += units;
                    }
                    if (pc != ninsns) (*bad)++;
                }
            }
        }
    }
}

/* A value of zero for each parameter: only methods that take no objects are called this way. */
static bool only_primitives(const hk_dvm_method *m)
{
    const char *s;
    for (s = m->shorty + 1; *s; s++)
        if (*s == 'L') return false;
    return true;
}

static void run_one(hk_dvm_method *m, hk_jobj *self, int *returned, int *threw, tally *why, int cap)
{
    hk_jvalue zeros[64], r;
    char text[300], *colon;
    if (m->nparams > HK_COUNT(zeros)) return;
    memset(zeros, 0, sizeof(zeros));
    if (hk_dvm_invoke(m, self, zeros, &r)) {
        (*returned)++;
        return;
    }
    (*threw)++;
    if (!hk_dvm_describe_pending(text, sizeof(text))) hk_strlcpy(text, "(nothing pending)", sizeof(text));
    hk_dvm_clear_pending();
    /* Grouped by the kind of exception; where the exception is that something is not there, by what. */
    colon = strchr(text, ':');
    if (colon && strncmp(text, "java.lang.NoClassDefFoundError", 30) != 0 && strncmp(text, "java.lang.NoSuch", 16) != 0 && strncmp(text, "java.lang.Unsatisfied", 21) != 0)
        *colon = 0;
    else if (colon && strchr(colon + 2, ' ')) *strchr(colon + 2, ' ') = 0;
    count_in(why, cap, text);
}

void test_real_apk(const char *apk)
{
    static tally missing[256], threw_why[256], init_why[64];
    u32 first = hk_dvm_defined_count(), total, i, fields = 0, methods = 0, with_code = 0, instructions = 0, bad = 0, checksum = 0;
    int added, linked = 0, failed = 0, inited = 0, init_failed = 0, returned = 0, threw = 0, made = 0;
    hk_dvm_dex *seen[16], *d = NULL;
    int nseen = 0, k;
    hk_stat st;
    hk_dvm_stats stats;
    hk_jvalue v;
    hk_jobj *o;

    if (!hk_path_stat(apk, &st, NULL)) {
        say("(%s is not there: the real-world DEX tests are skipped)", apk);
        return;
    }
    say("a real app's DEX (%s)", apk);
    {
        /* And one thing of its manifest, while the file is at hand: how its launcher activity asks to be held. */
        char *report = meglet_apk_report(apk);
        const char *o = report ? strstr(report, "\"orientation\":\"") : NULL;
        char word[24] = "";
        if (o) sscanf(o + 15, "%23[a-z]", word);
        say("  its launcher activity asks to be held: %s", word[0] ? word : "(the report does not say)");
        CHECK(!strcmp(word, "landscape") || !strcmp(word, "portrait") || !strcmp(word, "unspecified"));
        meglet_free(report);
    }
    added = hk_dvm_add_apk(apk);
    CHECK(added >= 1);
    total = hk_dvm_defined_count();
    CHECK(total > first);

    /* 1. Read every class, method and instruction of each file the APK brought. */
    for (i = first; i < total; i++) {
        hk_dvm_defined_name(i, &d);
        for (k = 0; k < nseen; k++)
            if (seen[k] == d) break;
        if (k == nseen && nseen < (int)HK_COUNT(seen)) seen[nseen++] = d;
    }
    for (k = 0; k < nseen; k++) {
        read_everything(seen[k], &fields, &methods, &with_code, &instructions, &bad);
        if (k == 0) checksum = hk_dvm_rd32(seen[k]->b + 8);
    }
    say("  read: %d DEX file(s), %u classes, %u fields, %u methods, %u with code, %u instructions; %u could not be read", added, (unsigned)(total - first),
        (unsigned)fields, (unsigned)methods, (unsigned)with_code, (unsigned)instructions, (unsigned)bad);
    CHECK_INT(bad, 0);
    CHECK(instructions > 0);
    if (checksum == 0xbbe48d80u) {
        /* The APK these tests were written against: what tools/dexasm.py, which shares no code with this, counts in the same file. */
        CHECK_INT(total - first, 106);
        CHECK_INT(fields, 358);
        CHECK_INT(methods, 614);
        CHECK_INT(with_code, 554);
        CHECK_INT(instructions, 7232);
    }

    /* 2. Link every class. One that cannot be linked must say which class was missing. */
    for (i = first; i < total; i++) {
        const char *name = hk_dvm_defined_name(i, NULL);
        if (hk_dvm_find_class(name)) linked++;
        else {
            char text[400], *sp;
            failed++;
            if (!hk_dvm_describe_pending(text, sizeof(text))) hk_strlcpy(text, "(nothing pending)", sizeof(text));
            hk_dvm_clear_pending();
            /* "java.lang.NoClassDefFoundError: android/app/Activity (needed by ...)": the missing class is what is counted. */
            sp = strstr(text, " (needed by");
            if (sp) *sp = 0;
            count_in(missing, (int)HK_COUNT(missing), text);
        }
    }
    say("  linked: %d of %u classes; %d not, for want of:", linked, (unsigned)(total - first), failed);
    say_tally("    ", missing, (int)HK_COUNT(missing), 12);
    CHECK_INT(linked + failed, total - first);
    CHECK(linked > 0);
    for (k = 0; k < (int)HK_COUNT(missing) && missing[k].count; k++) {
        /* Nothing but classes that are not there: no other kind of failure is expected of a well-formed file. */
        if (strncmp(missing[k].what, "java.lang.NoClassDefFoundError: ", 32) != 0) {
            g_failed++;
            say("  FAIL: a class did not link for another reason: %s", missing[k].what);
        }
    }
    g_checks++;

    /* 3a. Methods whose results are known. (Only where the APK has them: they are Unity's.) */
    if (hk_dvm_class_exists("com/unity3d/player/g$a") && hk_dvm_class_exists("com/unity3d/player/i")) {
        hk_jobj *a, *b;
        /* An "enum" the obfuscator turned into an int[]: its static initialiser fills {1, 2, 3}, a() returns a clone. */
        a = call("com/unity3d/player/g$a", "a", "()[I").l;
        b = call("com/unity3d/player/g$a", "a", "()[I").l;
        CHECK_STR(thrown(), "");
        CHECK(a && b && a != b);
        if (a && b) {
            CHECK_STR(hk_dvm_class_name(a), "[I");
            CHECK_INT(a->u.arr.len, 3);
            CHECK(((i32 *)a->u.arr.data)[0] == 1 && ((i32 *)a->u.arr.data)[1] == 2 && ((i32 *)a->u.arr.data)[2] == 3);
            CHECK(!memcmp(a->u.arr.data, b->u.arr.data, 12));
        }
        /* A class of flags: static setters and a getter, a constructor, instance fields. */
        CHECK_INT(call("com/unity3d/player/i", "c", "()Z").z, 0);
        call("com/unity3d/player/i", "a", "()V");
        CHECK_INT(call("com/unity3d/player/i", "c", "()Z").z, 1);
        o = hk_dvm_new("com/unity3d/player/i", "()V", NULL);
        CHECK(o != NULL);
        if (o) {
            CHECK_INT(vcall(o, "f", "()Z").z, 0);                /* a && b && !d && !c, and b is still false */
            A[0].z = 1;
            vcall(o, "a", "(Z)V");                               /* b = true */
            CHECK_INT(vcall(o, "f", "()Z").z, 0);                /* d is true from the constructor */
            A[0].z = 0;
            vcall(o, "b", "(Z)V");                               /* d = false */
            CHECK_INT(vcall(o, "f", "()Z").z, 1);
            CHECK_INT(vcall(o, "d", "()Z").z, 0);
            /* toString() is invoke-super to Object's, a native: "com.unity3d.player.i@<hash>" */
            CHECK(!strncmp(text_of(vcall(o, "toString", "()Ljava/lang/String;").l), "com.unity3d.player.i@", 21));
        }
        call("com/unity3d/player/i", "b", "()V");
        CHECK_INT(call("com/unity3d/player/i", "c", "()Z").z, 0);
        CHECK_STR(thrown(), "");
    }
    if (hk_dvm_class_exists("com/unity3d/player/ReflectionHelper$a")) {
        /* A hash key of a Class and two Strings: its constructor works the hash out with lit16 and lit8 arithmetic and three virtual calls. */
        hk_jobj *cls = hk_dvm_class_object("java/lang/String"), *name = hk_dvm_new_string("name"), *sig = hk_dvm_new_string("(I)V"), *one, *two, *three;
        u32 h1 = (u32)vcall(cls, "hashCode", "()I").i, h2 = (u32)vcall(name, "hashCode", "()I").i, h3 = (u32)vcall(sig, "hashCode", "()I").i;
        i32 want = (i32)(((h1 + 527u) * 31u + h2) * 31u + h3);
        A[0].l = cls;
        A[1].l = hk_dvm_new_string("name");
        A[2].l = hk_dvm_new_string("(I)V");
        one = hk_dvm_new("com/unity3d/player/ReflectionHelper$a", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;)V", A);
        two = hk_dvm_new("com/unity3d/player/ReflectionHelper$a", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;)V", A);
        A[2].l = hk_dvm_new_string("(J)V");
        three = hk_dvm_new("com/unity3d/player/ReflectionHelper$a", "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/String;)V", A);
        memset(A, 0, sizeof(A));
        CHECK(one && two && three);
        if (one && two && three) {
            CHECK_INT(vcall(one, "hashCode", "()I").i, want);
            A[0].l = two;
            CHECK_INT(vcall(one, "equals", "(Ljava/lang/Object;)Z").z, 1);
            A[0].l = three;
            CHECK_INT(vcall(one, "equals", "(Ljava/lang/Object;)Z").z, 0);
            A[0].l = cls;
            CHECK_INT(vcall(one, "equals", "(Ljava/lang/Object;)Z").z, 0);
        }
        CHECK_STR(thrown(), "");
    }
    if (hk_dvm_class_exists("com/TeamCherry/HollowKnight/BuildConfig")) {
        /* Static fields whose values are in the file itself, not set by code. */
        CHECK(hk_dvm_get_static("com/TeamCherry/HollowKnight/BuildConfig", "APPLICATION_ID", &v));
        CHECK_STR(text_of(v.l), "com.TeamCherry.HollowKnight");
        CHECK(hk_dvm_get_static("com/TeamCherry/HollowKnight/BuildConfig", "DEBUG", &v));
        CHECK_INT(v.z, 0);
        CHECK(hk_dvm_get_static("com/TeamCherry/HollowKnight/BuildConfig", "VERSION_CODE", &v));
        CHECK(v.i > 0);
        k = v.i;
        CHECK(hk_dvm_get_static("com/TeamCherry/HollowKnight/BuildConfig", "VERSION_NAME", &v));
        say("  BuildConfig: VERSION_CODE %d, VERSION_NAME \"%s\"", k, text_of(v.l));
    }

    /*
     * 3b. Everything that can be called with nothing to hand: each linked class is initialised, each static method that takes
     * no objects is called with zeros, and so is each such method of an object made with a constructor that takes nothing.
     * A limit on instructions ends what does not end by itself. Most of it throws, for want of android.*; none of it may do
     * anything else.
     */
    hk_dvm_set_step_limit(200000);
    for (i = first; i < total; i++) {
        const char *name = hk_dvm_defined_name(i, NULL);
        hk_dvm_class *c = hk_dvm_class_exists(name) ? hk_dvm_find_class(name) : NULL;
        hk_dvm_method *ctor;
        int j;
        if (!c) {
            hk_dvm_clear_pending();
            continue;
        }
        if (!hk_dvm_ensure_init(c)) {
            char text[300], *bar;
            init_failed++;
            if (!hk_dvm_describe_pending(text, sizeof(text))) hk_strlcpy(text, "(nothing pending)", sizeof(text));
            hk_dvm_clear_pending();
            bar = strstr(text, " | caused by ");
            count_in(init_why, (int)HK_COUNT(init_why), bar ? bar + 13 : text);
            continue;
        }
        inited++;
        for (j = 0; j < c->ndirect; j++) {
            hk_dvm_method *m = &c->direct[j];
            if ((m->flags & DVM_ACC_STATIC) && m->name[0] != '<' && m->insns && only_primitives(m)) run_one(m, NULL, &returned, &threw, threw_why, (int)HK_COUNT(threw_why));
        }
        ctor = hk_dvm_find_method(c, "<init>", "()V", false);
        if (!ctor || (c->flags & (DVM_ACC_ABSTRACT | DVM_ACC_INTERFACE))) continue;
        o = hk_dvm_alloc_object(c);
        if (!o || !hk_dvm_invoke(ctor, o, NULL, NULL)) {
            hk_dvm_clear_pending();
            continue;
        }
        made++;
        for (j = 0; j < c->nvirt; j++) {
            hk_dvm_method *m = &c->virt[j];
            if (m->insns && only_primitives(m)) run_one(m, o, &returned, &threw, threw_why, (int)HK_COUNT(threw_why));
        }
    }
    hk_dvm_set_step_limit(0);
    say("  ran: %d classes initialised (%d could not be), %d objects made; %d methods called with zeros: %d returned, %d threw:", inited, init_failed, made,
        returned + threw, returned, threw);
    say_tally("    ", threw_why, (int)HK_COUNT(threw_why), 10);
    if (init_failed) {
        say("  the static initialisers that failed threw:");
        say_tally("    ", init_why, (int)HK_COUNT(init_why), 10);
    }
    CHECK(inited > 0);
    CHECK(returned > 0);
    hk_dvm_get_stats(&stats);
    say("  in all: %u classes linked, %u initialised, %u objects (%u KB), %u strings interned", (unsigned)stats.classes_linked,
        (unsigned)stats.classes_initialised, (unsigned)stats.objects, (unsigned)(stats.object_bytes >> 10), (unsigned)stats.strings_interned);
}
