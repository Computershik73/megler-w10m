/*
 * POSIX regular expressions -- regcomp, regexec, regerror, regfree -- as bionic has them: basic and extended, with
 * REG_ICASE, REG_NOSUB, REG_NEWLINE and NetBSD's REG_NOSPEC and REG_PEND, and regexec's REG_NOTBOL, REG_NOTEOL and
 * REG_STARTEND. bionic's regex_t and regmatch_t are NetBSD's: { int re_magic; size_t re_nsub; const char *re_endp;
 * struct re_guts *re_g; } and two offsets of 32 bits.
 *
 * The pattern is parsed into a tree and matched by backtracking. A repeat of one byte's worth (a literal, '.', a bracket
 * expression) runs as a loop and backs off a byte at a time, with no call per byte, so a long subject costs no stack.
 * Of the matches at the leftmost place the longest is taken, as POSIX has it, by going on after each one found, within a
 * budget of steps past which the longest so far stands; the subexpressions are where they matched in it. GNU's \w \W
 * \s \S \b \B \< \> are there too, and in a basic expression \+ \? \|.
 *
 * Bytes, not characters: a '.' is one byte of UTF-8. Zalo's libraries import regcomp, regexec and regfree (libzalosqlite,
 * libzmedia, libzjxl), and an import not provided is a stub that ends the app when called.
 */
#include "mg_bionic.h"

#include <ctype.h>

enum {
    G_REG_EXTENDED = 1, G_REG_ICASE = 2, G_REG_NOSUB = 4, G_REG_NEWLINE = 8, G_REG_NOSPEC = 16, G_REG_PEND = 32
};
enum { G_REG_NOTBOL = 1, G_REG_NOTEOL = 2, G_REG_STARTEND = 4 };
enum {
    G_REG_NOMATCH = 1, G_REG_BADPAT, G_REG_ECOLLATE, G_REG_ECTYPE, G_REG_EESCAPE, G_REG_ESUBREG, G_REG_EBRACK, G_REG_EPAREN,
    G_REG_EBRACE, G_REG_BADBR, G_REG_ERANGE, G_REG_ESPACE, G_REG_BADRPT, G_REG_EMPTY, G_REG_ASSERT, G_REG_INVARG
};

typedef struct g_regex {
    int re_magic;
    size_t re_nsub;
    const char *re_endp;
    void *re_g;
} g_regex;

/* regoff_t is an off_t in bionic: a long, 32 bits in 32-bit Android. */
typedef struct g_regmatch {
    goff rm_so, rm_eo;
} g_regmatch;

#define RE_MAGIC 0xf265
#define RE_STEPS 4000000u      /* steps of one regexec, past which the longest match so far stands */
#define RE_STACK 65536u        /* bytes of the caller's stack, past which a path is given up: an app's thread may have 160 KB */

enum { N_CHAR, N_ANY, N_SET, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_WORDBEG, N_WORDEND, N_GROUP, N_GEND, N_CAT, N_ALT, N_REP, N_BACKREF, N_EMPTY };

typedef struct rnode {
    int type;
    int c;                     /* N_CHAR: the byte; N_GROUP, N_GEND, N_BACKREF: the subexpression's number */
    int min, max;              /* N_REP: max -1 for no bound */
    u8 set[32];                /* N_SET: a bit per byte */
    struct rnode *a, *b;       /* N_CAT, N_ALT: the two sides; N_GROUP, N_REP: what is inside; N_GROUP: its N_GEND */
} rnode;

typedef struct rprog {
    rnode *nodes;
    int count, cap;
    rnode *root;
    int nsub, flags;
    int first;                 /* the byte every match starts with, or -1 */
    bool anchored;             /* every match starts at the subject's start */
} rprog;

/* ------------------------------------------------------------------ parsing */

typedef struct rparse {
    rprog *p;
    const char *s, *end;
    int err;
    int nsub;
    bool closed[10];           /* which subexpressions have ended, for back references to them */
    bool ext, icase, newline, nospec;
} rparse;

static rnode *rnew(rparse *ps, int type)
{
    rnode *n;
    if (ps->err) return NULL;
    if (ps->p->count == ps->p->cap) {
        ps->err = G_REG_ESPACE;
        return NULL;
    }
    n = &ps->p->nodes[ps->p->count++];
    memset(n, 0, sizeof(*n));
    n->type = type;
    n->max = -1;
    return n;
}

static rnode *rcat(rparse *ps, rnode *a, rnode *b)
{
    rnode *n;
    if (!a) return b;
    if (!b) return a;
    n = rnew(ps, N_CAT);
    if (n) {
        n->a = a;
        n->b = b;
    }
    return n;
}

static void set_add(u8 *set, int c) { set[(u8)c >> 3] |= (u8)(1u << ((u8)c & 7)); }
static bool set_has(const u8 *set, int c) { return (set[(u8)c >> 3] >> ((u8)c & 7)) & 1; }

static void set_add_case(const rparse *ps, u8 *set, int c)
{
    set_add(set, c);
    if (ps->icase && c < 128) {
        set_add(set, tolower(c));
        set_add(set, toupper(c));
    }
}

static bool class_add(u8 *set, const char *name, size_t len)
{
    static const char *const names[] = { "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper", "xdigit" };
    int k, c;
    for (k = 0; k < (int)MG_COUNT(names); k++)
        if (strlen(names[k]) == len && !memcmp(names[k], name, len)) break;
    if (k == (int)MG_COUNT(names)) return false;
    for (c = 0; c < 128; c++) {
        bool in;
        switch (k) {
        case 0: in = isalnum(c) != 0; break;
        case 1: in = isalpha(c) != 0; break;
        case 2: in = c == ' ' || c == '\t'; break;
        case 3: in = iscntrl(c) != 0; break;
        case 4: in = isdigit(c) != 0; break;
        case 5: in = isgraph(c) != 0; break;
        case 6: in = islower(c) != 0; break;
        case 7: in = isprint(c) != 0; break;
        case 8: in = ispunct(c) != 0; break;
        case 9: in = isspace(c) != 0; break;
        case 10: in = isupper(c) != 0; break;
        default: in = isxdigit(c) != 0; break;
        }
        if (in) set_add(set, c);
    }
    return true;
}

static void set_invert(u8 *set)
{
    int k;
    for (k = 0; k < 32; k++) set[k] = (u8)~set[k];
}

/* A bracket expression's one element -- a byte, [=c=] or [.c.] -- at ps->s: the byte, or -1 with an error. */
static int bracket_element(rparse *ps)
{
    if (ps->s + 1 < ps->end && ps->s[0] == '[' && (ps->s[1] == '=' || ps->s[1] == '.')) {
        char kind = ps->s[1];
        const char *from = ps->s + 2, *at = from;
        while (at + 1 < ps->end && !(at[0] == kind && at[1] == ']')) at++;
        if (at + 1 >= ps->end) {
            ps->err = G_REG_EBRACK;
            return -1;
        }
        if (at - from != 1) {
            ps->err = G_REG_ECOLLATE;    /* (no multi-byte collating elements here) */
            return -1;
        }
        ps->s = at + 2;
        return (u8)*from;
    }
    return (u8)*ps->s++;
}

/* A bracket expression, ps->s just past its '['. */
static rnode *parse_bracket(rparse *ps)
{
    rnode *n = rnew(ps, N_SET);
    bool negate = false, first = true;
    int k;
    if (!n) return NULL;
    if (ps->s < ps->end && *ps->s == '^') {
        negate = true;
        ps->s++;
    }
    for (;;) {
        int lo;
        if (ps->s >= ps->end) {
            ps->err = G_REG_EBRACK;
            return NULL;
        }
        if (*ps->s == ']' && !first) {
            ps->s++;
            break;
        }
        first = false;
        if (ps->s + 1 < ps->end && ps->s[0] == '[' && ps->s[1] == ':') {
            const char *from = ps->s + 2, *at = from;
            while (at + 1 < ps->end && !(at[0] == ':' && at[1] == ']')) at++;
            if (at + 1 >= ps->end) {
                ps->err = G_REG_EBRACK;
                return NULL;
            }
            if (!class_add(n->set, from, (size_t)(at - from))) {
                ps->err = G_REG_ECTYPE;
                return NULL;
            }
            if (ps->icase)
                for (k = 'a'; k <= 'z'; k++)
                    if (set_has(n->set, k) || set_has(n->set, k - 32)) {
                        set_add(n->set, k);
                        set_add(n->set, k - 32);
                    }
            ps->s = at + 2;
            continue;
        }
        lo = bracket_element(ps);
        if (lo < 0) return NULL;
        /* A range, unless the '-' is the last thing before the ']'. */
        if (ps->s + 1 < ps->end && ps->s[0] == '-' && ps->s[1] != ']') {
            int hi;
            ps->s++;
            hi = bracket_element(ps);
            if (hi < 0) return NULL;
            if (hi < lo) {
                ps->err = G_REG_ERANGE;
                return NULL;
            }
            for (k = lo; k <= hi; k++) set_add_case(ps, n->set, k);
        } else {
            set_add_case(ps, n->set, lo);
        }
    }
    if (negate) {
        set_invert(n->set);
        if (ps->newline) n->set['\n' >> 3] &= (u8)~(1u << ('\n' & 7));
    }
    return n;
}

static rnode *literal(rparse *ps, int c)
{
    rnode *n;
    if (ps->icase && c < 128 && tolower(c) != toupper(c)) {
        n = rnew(ps, N_SET);
        if (n) set_add_case(ps, n->set, c);
        return n;
    }
    n = rnew(ps, N_CHAR);
    if (n) n->c = c;
    return n;
}

/* An interval's {m}, {m,} or {m,n}, ps->s just past its '{' (or "\{"). False, with an error, for one that is not. */
static bool parse_interval(rparse *ps, int *min, int *max)
{
    int a = 0, b;
    bool digits = false;
    while (ps->s < ps->end && isdigit((u8)*ps->s)) {
        a = a * 10 + (*ps->s++ - '0');
        digits = true;
        if (a > 255) {
            ps->err = G_REG_BADBR;
            return false;
        }
    }
    if (!digits) {
        ps->err = G_REG_BADBR;
        return false;
    }
    b = a;
    if (ps->s < ps->end && *ps->s == ',') {
        ps->s++;
        if (ps->s < ps->end && isdigit((u8)*ps->s)) {
            b = 0;
            while (ps->s < ps->end && isdigit((u8)*ps->s)) {
                b = b * 10 + (*ps->s++ - '0');
                if (b > 255) {
                    ps->err = G_REG_BADBR;
                    return false;
                }
            }
        } else {
            b = -1;
        }
    }
    if (ps->ext ? (ps->s >= ps->end || *ps->s != '}') : (ps->s + 1 >= ps->end || ps->s[0] != '\\' || ps->s[1] != '}')) {
        ps->err = G_REG_EBRACE;
        return false;
    }
    ps->s += ps->ext ? 1 : 2;
    if (b != -1 && b < a) {
        ps->err = G_REG_BADBR;
        return false;
    }
    *min = a;
    *max = b;
    return true;
}

/* Whether the next thing ends a branch: the pattern's end, '|' or ')' (extended), \| or \) (basic). */
static bool branch_ends(const rparse *ps)
{
    if (ps->s >= ps->end) return true;
    if (ps->nospec) return false;
    if (ps->ext) return *ps->s == '|' || *ps->s == ')';
    return ps->s + 1 < ps->end && ps->s[0] == '\\' && (ps->s[1] == '|' || ps->s[1] == ')');
}

static rnode *parse_alt(rparse *ps, int depth);

/* A subexpression, ps->s just past its "(" (or "\("). */
static rnode *parse_group(rparse *ps, int depth)
{
    int idx = ++ps->nsub;
    rnode *g = rnew(ps, N_GROUP), *gend = rnew(ps, N_GEND), *inner;
    if (!g || !gend) return NULL;
    g->c = gend->c = idx;
    if (ps->ext ? (ps->s < ps->end && *ps->s == ')') : (ps->s + 1 < ps->end && ps->s[0] == '\\' && ps->s[1] == ')'))
        inner = rnew(ps, N_EMPTY);
    else
        inner = parse_alt(ps, depth + 1);
    if (ps->err) return NULL;
    if (ps->ext ? (ps->s >= ps->end || *ps->s != ')') : (ps->s + 1 >= ps->end || ps->s[0] != '\\' || ps->s[1] != ')')) {
        ps->err = G_REG_EPAREN;
        return NULL;
    }
    ps->s += ps->ext ? 1 : 2;
    g->a = inner;
    g->b = gend;
    if (idx < 10) ps->closed[idx] = true;
    return g;
}

static rnode *parse_escape(rparse *ps, int depth)
{
    int c;
    rnode *n;
    if (ps->s + 1 >= ps->end) {
        ps->err = G_REG_EESCAPE;
        return NULL;
    }
    c = (u8)ps->s[1];
    ps->s += 2;
    if (!ps->ext && c == '(') return parse_group(ps, depth);
    if (c >= '1' && c <= '9') {
        if (!ps->closed[c - '0']) {
            ps->err = G_REG_ESUBREG;
            return NULL;
        }
        n = rnew(ps, N_BACKREF);
        if (n) n->c = c - '0';
        return n;
    }
    switch (c) {
    case 'w': case 'W': case 's': case 'S': {
        int k;
        n = rnew(ps, N_SET);
        if (!n) return NULL;
        if (c == 'w' || c == 'W') {
            for (k = 0; k < 128; k++)
                if (isalnum(k) || k == '_') set_add(n->set, k);
        } else {
            class_add(n->set, "space", 5);
        }
        if (c == 'W' || c == 'S') set_invert(n->set);
        return n;
    }
    case 'b': return rnew(ps, N_WORDB);
    case 'B': return rnew(ps, N_NWORDB);
    case '<': return rnew(ps, N_WORDBEG);
    case '>': return rnew(ps, N_WORDEND);
    case '`': return rnew(ps, N_BOL);
    case '\'': return rnew(ps, N_EOL);
    default: return literal(ps, c);
    }
}

static rnode *parse_atom(rparse *ps, int depth, bool at_start)
{
    int c = (u8)*ps->s;
    if (ps->nospec) {
        ps->s++;
        return literal(ps, c);
    }
    if (c == '\\') return parse_escape(ps, depth);
    ps->s++;
    switch (c) {
    case '.': return rnew(ps, N_ANY);
    case '[': return parse_bracket(ps);
    case '(':
        if (ps->ext) return parse_group(ps, depth);
        return literal(ps, c);
    case '^':
        /* Extended: an anchor anywhere. Basic: at a branch's start (of the RE, after \( or \|), else a literal. */
        return ps->ext || at_start ? rnew(ps, N_BOL) : literal(ps, c);
    case '$':
        return ps->ext || branch_ends(ps) ? rnew(ps, N_EOL) : literal(ps, c);
    default:
        /* '*', '+', '?' and '{' with nothing before them: literals, as most libraries take them. */
        return literal(ps, c);
    }
}

static rnode *parse_branch(rparse *ps, int depth)
{
    rnode *seq = NULL;
    bool at_start = true;
    while (!ps->err && !branch_ends(ps)) {
        rnode *atom = parse_atom(ps, depth, at_start);
        if (ps->err) return NULL;
        /* Its repetitions. */
        for (;;) {
            int min, max = -1;
            if (ps->s >= ps->end || ps->nospec) break;
            if (*ps->s == '*') {
                ps->s++;
                min = 0;
            } else if (ps->ext && *ps->s == '+') {
                ps->s++;
                min = 1;
            } else if (ps->ext && *ps->s == '?') {
                ps->s++;
                min = 0;
                max = 1;
            } else if (ps->ext && *ps->s == '{' && ps->s + 1 < ps->end && isdigit((u8)ps->s[1])) {
                ps->s++;
                if (!parse_interval(ps, &min, &max)) return NULL;
            } else if (!ps->ext && *ps->s == '\\' && ps->s + 1 < ps->end && (ps->s[1] == '+' || ps->s[1] == '?')) {
                min = ps->s[1] == '+' ? 1 : 0;
                max = ps->s[1] == '?' ? 1 : -1;
                ps->s += 2;
            } else if (!ps->ext && *ps->s == '\\' && ps->s + 1 < ps->end && ps->s[1] == '{') {
                ps->s += 2;
                if (!parse_interval(ps, &min, &max)) return NULL;
            } else {
                break;
            }
            /* (A repeated anchor is the anchor.) */
            if (atom->type == N_BOL || atom->type == N_EOL || atom->type == N_WORDB || atom->type == N_NWORDB) continue;
            {
                rnode *r = rnew(ps, N_REP);
                if (!r) return NULL;
                r->a = atom;
                r->min = min;
                r->max = max;
                atom = r;
            }
        }
        seq = rcat(ps, seq, atom);
        at_start = false;
    }
    if (!seq) seq = rnew(ps, N_EMPTY);
    return seq;
}

static rnode *parse_alt(rparse *ps, int depth)
{
    rnode *left;
    if (depth > 200) {
        ps->err = G_REG_ESPACE;
        return NULL;
    }
    left = parse_branch(ps, depth);
    while (!ps->err && !ps->nospec && ps->s < ps->end &&
           (ps->ext ? *ps->s == '|' : ps->s + 1 < ps->end && ps->s[0] == '\\' && ps->s[1] == '|')) {
        rnode *alt, *right;
        ps->s += ps->ext ? 1 : 2;
        right = parse_branch(ps, depth);
        alt = rnew(ps, N_ALT);
        if (!alt) return NULL;
        alt->a = left;
        alt->b = right;
        left = alt;
    }
    return left;
}

/* The byte every match starts with, where the expression says so: -1 where it does not (anything that may match nothing first). */
static int first_byte(const rnode *n)
{
    for (;;) {
        if (!n) return -1;
        switch (n->type) {
        case N_CHAR: return n->c;
        case N_CAT: case N_GROUP: n = n->a; continue;
        case N_REP:
            if (n->min == 0) return -1;
            n = n->a;
            continue;
        default: return -1;
        }
    }
}

static bool anchored_at_bol(const rnode *n)
{
    for (;;) {
        if (!n) return false;
        if (n->type == N_BOL) return true;
        if (n->type == N_CAT || n->type == N_GROUP) n = n->a;
        else return false;
    }
}

static int g_regcomp(g_regex *preg, const char *pattern, int cflags)
{
    rparse ps;
    rprog *p;
    const char *end;
    size_t len;
    if (!preg || !pattern) return G_REG_INVARG;
    end = (cflags & G_REG_PEND) ? preg->re_endp : pattern + strlen(pattern);
    if (!end || end < pattern) return G_REG_INVARG;
    len = (size_t)(end - pattern);
    p = (rprog *)mg_alloc(sizeof(*p));
    memset(p, 0, sizeof(*p));
    /* At most four nodes a byte: a subexpression, its end, the empty one in it and the sequence it is a part of. */
    p->cap = (int)(len * 5 + 16);
    p->nodes = (rnode *)mg_alloc(sizeof(rnode) * (size_t)p->cap);
    p->flags = cflags;
    memset(&ps, 0, sizeof(ps));
    ps.p = p;
    ps.s = pattern;
    ps.end = end;
    ps.ext = (cflags & G_REG_EXTENDED) != 0;
    ps.icase = (cflags & G_REG_ICASE) != 0;
    ps.newline = (cflags & G_REG_NEWLINE) != 0;
    ps.nospec = (cflags & G_REG_NOSPEC) != 0;
    p->root = parse_alt(&ps, 0);
    if (!ps.err && ps.s < ps.end) ps.err = G_REG_EPAREN;   /* a ')' with no '(' */
    if (ps.err || !p->root) {
        free(p->nodes);
        free(p);
        return ps.err ? ps.err : G_REG_BADPAT;
    }
    p->nsub = ps.nsub;
    p->first = first_byte(p->root);
    p->anchored = !ps.newline && anchored_at_bol(p->root);
    preg->re_magic = RE_MAGIC;
    preg->re_nsub = (size_t)ps.nsub;
    preg->re_g = p;
    return 0;
}

static void g_regfree(g_regex *preg)
{
    rprog *p;
    if (!preg || preg->re_magic != RE_MAGIC) return;
    p = (rprog *)preg->re_g;
    if (p) {
        free(p->nodes);
        free(p);
    }
    preg->re_magic = 0;
    preg->re_g = NULL;
}

/* ------------------------------------------------------------------ matching */

/*
 * What is left to match after a node, as a list: the node next (K_NODE), or the rest of a repetition (K_REP: one more
 * of its iterations, or what follows it), with how many it has had and where the latest began.
 */
enum { K_NODE, K_REP };
typedef struct rcont {
    int kind;
    const rnode *n;
    const struct rcont *next;
    int count, at;
} rcont;

typedef struct rmatch {
    const rprog *p;
    const u8 *s;
    int from, len;             /* where the subject begins and ends, from s */
    int eflags;
    bool newline, longest;
    i32 *caps, *best;          /* two a subexpression and two for the whole: as they are now, and in the longest match yet */
    int best_end;
    u32 steps;
    mg_addr sp0;               /* the stack where regexec began */
    bool stop;
} rmatch;

static bool is_word(int c) { return c == '_' || (c < 128 && isalnum(c)); }

static bool step(rmatch *m, const rnode *n, int i, const rcont *k);
static bool rep_more(rmatch *m, const rnode *n, int count, int at, int i, const rcont *k);

/* What follows: the continuation's next, or the end of a match. */
static bool cont(rmatch *m, int i, const rcont *k)
{
    if (k) return k->kind == K_NODE ? step(m, k->n, i, k->next) : rep_more(m, k->n, k->count, k->at, i, k->next);
    /*
     * A match. The longest is kept; the search goes on for a longer one (it is the leftmost longest POSIX wants) unless
     * none can be -- this one reaches the subject's end -- or any match will do (REG_NOSUB, no room for submatches).
     */
    if (i > m->best_end) {
        m->best_end = i;
        if (m->best) memcpy(m->best, m->caps, sizeof(i32) * 2 * (size_t)(m->p->nsub + 1));
    }
    if (!m->longest || i == m->len) m->stop = true;
    return m->stop;
}

static bool one_byte(const rmatch *m, const rnode *n, int c)
{
    switch (n->type) {
    case N_CHAR: return c == n->c;
    case N_ANY: return !(m->newline && c == '\n');
    case N_SET: return set_has(n->set, c);
    default: return false;
    }
}

/* One more iteration of the repetition `n` after `count` of them, the latest of which began at `at` and ended at i; or what follows. */
static bool rep_more(rmatch *m, const rnode *n, int count, int at, int i, const rcont *k)
{
    /* An iteration that matched nothing ends the repetition: another would match nothing again, for ever. */
    if (count > 0 && i == at) return cont(m, i, k);
    if (n->max < 0 || count < n->max) {
        rcont c;
        c.kind = K_REP;
        c.n = n;
        c.next = k;
        c.count = count + 1;
        c.at = i;
        if (step(m, n->a, i, &c)) return true;
    }
    return count >= n->min && cont(m, i, k);
}

static bool step_in(rmatch *m, const rnode *n, int i, const rcont *k)
{
    switch (n->type) {
    case N_CHAR: case N_ANY: case N_SET:
        return i < m->len && one_byte(m, n, m->s[i]) && cont(m, i + 1, k);
    case N_EMPTY:
        return cont(m, i, k);
    case N_BOL:
        return ((i == m->from && !(m->eflags & G_REG_NOTBOL)) || (m->newline && i > m->from && m->s[i - 1] == '\n')) && cont(m, i, k);
    case N_EOL:
        return ((i == m->len && !(m->eflags & G_REG_NOTEOL)) || (m->newline && i < m->len && m->s[i] == '\n')) && cont(m, i, k);
    case N_WORDB: case N_NWORDB: case N_WORDBEG: case N_WORDEND: {
        bool before = i > m->from && is_word(m->s[i - 1]), after = i < m->len && is_word(m->s[i]), ok;
        if (n->type == N_WORDB) ok = before != after;
        else if (n->type == N_NWORDB) ok = before == after;
        else if (n->type == N_WORDBEG) ok = !before && after;
        else ok = before && !after;
        return ok && cont(m, i, k);
    }
    case N_CAT: {
        rcont c;
        c.kind = K_NODE;
        c.n = n->b;
        c.next = k;
        c.count = c.at = 0;
        return step(m, n->a, i, &c);
    }
    case N_ALT:
        return step(m, n->a, i, k) || step(m, n->b, i, k);
    case N_GROUP: {
        i32 old_s = m->caps[2 * n->c], old_e = m->caps[2 * n->c + 1];
        rcont c;
        c.kind = K_NODE;
        c.n = n->b;            /* its N_GEND */
        c.next = k;
        c.count = c.at = 0;
        m->caps[2 * n->c] = i;
        if (step(m, n->a, i, &c)) return true;
        m->caps[2 * n->c] = old_s;
        m->caps[2 * n->c + 1] = old_e;
        return false;
    }
    case N_GEND: {
        i32 old_e = m->caps[2 * n->c + 1];
        m->caps[2 * n->c + 1] = i;
        if (cont(m, i, k)) return true;
        m->caps[2 * n->c + 1] = old_e;
        return false;
    }
    case N_BACKREF: {
        i32 s = m->caps[2 * n->c], e = m->caps[2 * n->c + 1];
        int len, j;
        if (s < 0 || e < s) return false;
        len = e - s;
        if (i + len > m->len) return false;
        for (j = 0; j < len; j++) {
            int a = m->s[s + j], b = m->s[i + j];
            if (a != b && !((m->p->flags & G_REG_ICASE) && a < 128 && b < 128 && tolower(a) == tolower(b))) return false;
        }
        return cont(m, i + len, k);
    }
    case N_REP: {
        const rnode *x = n->a;
        if (x->type == N_CHAR || x->type == N_ANY || x->type == N_SET) {
            /* One byte's worth: as many as there are, up to the most, then backing off one at a time. */
            int most = 0, cnt;
            while (i + most < m->len && (n->max < 0 || most < n->max) && one_byte(m, x, m->s[i + most])) most++;
            for (cnt = most; cnt >= n->min; cnt--)
                if (cont(m, i + cnt, k)) return true;
            return false;
        }
        return rep_more(m, n, 0, -1, i, k);
    }
    default:
        return false;
    }
}

static bool step(rmatch *m, const rnode *n, int i, const rcont *k)
{
    volatile char here = 0;
    if (m->stop) return true;
    if (++m->steps > RE_STEPS) {
        /* Past the budget: the longest match so far stands, or there is none. */
        m->stop = true;
        return true;
    }
    /* (The stack grows down. A path deeper than this is given up, as one that does not match.) */
    if (m->sp0 - (mg_addr)&here > RE_STACK) return false;
    return step_in(m, n, i, k);
}

static int g_regexec(const g_regex *preg, const char *string, size_t nmatch, g_regmatch *pmatch, int eflags)
{
    const rprog *p;
    rmatch m;
    i32 small[2][2 * 11], *caps, *best;
    volatile char here = 0;
    int start, from = 0, to, nsub, k;
    if (!preg || preg->re_magic != RE_MAGIC || !preg->re_g || !string) return G_REG_BADPAT;
    p = (const rprog *)preg->re_g;
    if (p->flags & G_REG_NOSUB) nmatch = 0;
    if (eflags & G_REG_STARTEND) {
        if (!pmatch || pmatch[0].rm_so < 0 || pmatch[0].rm_eo < pmatch[0].rm_so) return G_REG_INVARG;
        from = pmatch[0].rm_so;
        to = pmatch[0].rm_eo;
    } else {
        size_t n = strlen(string);
        to = n > 0x7FFFFFFF ? 0x7FFFFFFF : (int)n;
    }
    nsub = p->nsub;
    if (nsub < 11) {
        caps = small[0];
        best = small[1];
    } else {
        caps = (i32 *)mg_alloc(sizeof(i32) * 4 * (size_t)(nsub + 1));
        best = caps + 2 * (nsub + 1);
    }
    memset(&m, 0, sizeof(m));
    m.p = p;
    m.s = (const u8 *)string;
    m.from = from;
    m.len = to;
    m.eflags = eflags;
    m.newline = (p->flags & G_REG_NEWLINE) != 0;
    m.longest = nmatch > 0;
    m.caps = caps;
    m.best = best;
    m.best_end = -1;
    m.sp0 = (mg_addr)&here;
    for (start = from; start <= to; start++) {
        if (p->anchored && start != from) break;
        if (p->first >= 0) {
            const u8 *at = (const u8 *)memchr(m.s + start, p->first, (size_t)(to - start));
            if (!at) break;
            start = (int)(at - m.s);
        }
        for (k = 0; k < 2 * (nsub + 1); k++) caps[k] = -1;
        caps[0] = start;
        m.stop = false;
        step(&m, p->root, start, NULL);
        if (m.best_end >= 0) break;
        if (m.steps > RE_STEPS) break;
    }
    if (m.best_end < 0) {
        if (nsub >= 11) free(caps);
        return G_REG_NOMATCH;
    }
    if (nmatch > 0 && pmatch) {
        for (k = 0; k < (int)nmatch; k++) {
            if (k <= nsub && best[2 * k] >= 0 && best[2 * k + 1] >= best[2 * k]) {
                pmatch[k].rm_so = best[2 * k];
                pmatch[k].rm_eo = best[2 * k + 1];
            } else {
                pmatch[k].rm_so = pmatch[k].rm_eo = -1;
            }
        }
        pmatch[0].rm_so = best[0];
        pmatch[0].rm_eo = m.best_end;
    }
    if (nsub >= 11) free(caps);
    return 0;
}

static size_t g_regerror(int errcode, const g_regex *preg, char *errbuf, size_t size)
{
    static const char *const messages[] = {
        "success", "regexec() failed to match", "invalid regular expression", "invalid collating element", "invalid character class",
        "trailing backslash (\\)", "invalid backreference number", "brackets ([ ]) not balanced", "parentheses not balanced",
        "braces not balanced", "invalid repetition count(s)", "invalid character range", "out of memory",
        "repetition-operator operand invalid", "empty (sub)expression", "\"can't happen\" -- you found a bug",
        "invalid argument to regex routine"
    };
    const char *text = errcode >= 0 && errcode < (int)MG_COUNT(messages) ? messages[errcode] : "unknown regexp error";
    size_t n = strlen(text) + 1;
    (void)preg;
    if (errbuf && size) {
        size_t c = n < size ? n : size;
        memcpy(errbuf, text, c - 1);
        errbuf[c - 1] = 0;
    }
    return n;
}

const mg_sym mg_tab_regex[] = {
    MG_WRAP("regcomp", g_regcomp), MG_WRAP("regexec", g_regexec), MG_WRAP("regerror", g_regerror), MG_WRAP("regfree", g_regfree),
    MG_END
};
