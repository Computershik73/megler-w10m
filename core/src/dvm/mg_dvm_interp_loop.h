/*
 * The interpreter's loop, included twice by mg_dvm_interp.c: as mg_dvm_interpret, which runs code, and as
 * mg_dvm_interpret_watched, which also does what every instruction must while anything watches them (slow_step: the trace
 * at level 3, the step limit, the profile). DVM_LOOP is the function's name, DVM_WATCHED which of the two it is.
 *
 * The plain loop does not look at g_dvm.slow for every instruction, only where code can go on for ever: at a jump back
 * and at a call (SAFEPOINT). When what it finds there is not what it runs as, it hands the frame on top to the other
 * loop (DVM_SWITCH), which goes on from that frame's pc; interpret() in mg_dvm_interp.c runs the one that is wanted until
 * the frame entered from C returns or throws. Nothing is in flight at those places: no result of a call waits for its
 * move-result, no exception for its move-exception.
 */
int DVM_LOOP(mg_dvm_thread *t, mg_dvm_frame *f, mg_jvalue *ret);
int DVM_LOOP(mg_dvm_thread *t, mg_dvm_frame *f, mg_jvalue *ret)
{
    mg_dvm_method *m = f->m;
    const u16 *insns = m->insns;
    mg_dvm_reg *regs = f->regs;
    mg_dvm_dex *dex = m->cls->dex;
    u32 pc = f->pc;                    /* 0 for a frame entered from C; where to go on when the other loop handed over */
    mg_jvalue result;
    mg_jobj *caught = NULL;
    /* What an invoke instruction decoded, for the code they all share. */
    int inv_kind = 0;
    u32 inv_idx = 0, inv_count = 0;
    const mg_dvm_reg *inv_argv = NULL;
    mg_dvm_reg inv_five[5];
    u16 w;
    /* Instructions executed, given to the thread at each native call and at the way out (mg_dvm_thread_costs). */
    u32 executed = 0;
    result.j = 0;
    t->running = m;

    for (;;) {
        w = insns[pc];
        executed++;
#if DVM_WATCHED
        if (!slow_step(t, f, pc)) goto exception;
#endif
        switch (w & 0xFF) {
        case 0x00:                                               /* nop */
            if (w == 0x0100 || w == 0x0200 || w == 0x0300) {
                /* Data, which no instruction should fall into; stepped over whole rather than run. */
                u32 n = mg_dvm_insn_units(insns, m->ninsns, pc);
                if (!n) THROW("java/lang/VerifyError", "damaged data in %s.%s at %u", m->cls->name, m->name, (unsigned)pc);
                NEXT(n);
            }
            NEXT(1);

        /* moves: a whole register either way, so the plain and the object forms are one */
        case 0x01: case 0x07: regs[A4] = regs[B4]; NEXT(1);
        case 0x02: case 0x08: regs[AA] = regs[FETCH(1)]; NEXT(2);
        case 0x03: case 0x09: regs[FETCH(1)] = regs[FETCH(2)]; NEXT(3);
        case 0x04: WJ(A4, RJ(B4)); NEXT(1);
        case 0x05: WJ(AA, RJ(FETCH(1))); NEXT(2);
        case 0x06: WJ(FETCH(1), RJ(FETCH(2))); NEXT(3);
        case 0x0a: WI(AA, result.i); NEXT(1);
        case 0x0b: WJ(AA, result.j); NEXT(1);
        case 0x0c: WL(AA, result.l); NEXT(1);
        case 0x0d:
            WL(AA, caught);
            caught = NULL;
            NEXT(1);

        case 0x0e:
            result.j = 0;
            goto do_return;
        case 0x0f:
            result.j = 0;
            result.i = RI(AA);
            goto do_return;
        case 0x10:
            result.j = RJ(AA);
            goto do_return;
        case 0x11: {
            mg_jobj *o = RL(AA);
            result.j = 0;
            result.l = o;
            goto do_return;
        }

        case 0x12: WI(A4, (i32)(B4 ^ 8) - 8); NEXT(1);           /* const/4: the nibble sign-extended */
        case 0x13: WI(AA, (i32)(i16)FETCH(1)); NEXT(2);
        case 0x14: WI(AA, FETCH(1) | (u32)FETCH(2) << 16); NEXT(3);
        case 0x15: WI(AA, (u32)FETCH(1) << 16); NEXT(2);
        case 0x16: WJ(AA, (i64)(i16)FETCH(1)); NEXT(2);
        case 0x17: WJ(AA, (i64)(i32)(FETCH(1) | (u32)FETCH(2) << 16)); NEXT(3);
        case 0x18: WJ(AA, (u64)FETCH(1) | (u64)FETCH(2) << 16 | (u64)FETCH(3) << 32 | (u64)FETCH(4) << 48); NEXT(5);
        case 0x19: WJ(AA, (u64)FETCH(1) << 48); NEXT(2);
        case 0x1a: case 0x1b: {                                  /* const-string, const-string/jumbo */
            bool jumbo = (w & 0xFF) == 0x1b;
            u32 idx = jumbo ? (FETCH(1) | (u32)FETCH(2) << 16) : FETCH(1);
            mg_jobj *s = idx < dex->nstr ? dex->scache[idx] : NULL;
            if (!s) {
                f->pc = pc;
                s = mg_dvm_const_string(dex, idx);
                if (!s) goto exception;
            }
            WL(AA, s);
            NEXT(jumbo ? 3 : 2);
        }
        case 0x1c: {                                             /* const-class */
            mg_dvm_class *c;
            mg_jobj *mirror;
            RESOLVE_TYPE(c, FETCH(1));
            f->pc = pc;
            mirror = mg_dvm_mirror(c);
            if (!mirror) goto exception;
            WL(AA, mirror);
            NEXT(2);
        }
        case 0x1d: {                                             /* monitor-enter */
            mg_jobj *o = RL(AA);
            if (!o) THROW("java/lang/NullPointerException", "monitor-enter on null");
            mg_dvm_monitor_enter(o);
            NEXT(1);
        }
        case 0x1e: {                                             /* monitor-exit */
            mg_jobj *o = RL(AA);
            if (!o) THROW("java/lang/NullPointerException", "monitor-exit on null");
            if (!mg_dvm_monitor_exit(o)) THROW("java/lang/IllegalMonitorStateException", "monitor-exit on an object this thread has not locked");
            NEXT(1);
        }
        case 0x1f: {                                             /* check-cast: null is of every class, and its class need not exist */
            mg_jobj *o = RL(AA);
            if (o) {
                mg_dvm_class *c;
                RESOLVE_TYPE(c, FETCH(1));
                if (mg_dvm_class_of(o) != c && !mg_dvm_assignable(mg_dvm_class_of(o), c))
                    THROW("java/lang/ClassCastException", "%s cannot be cast to %s", mg_dvm_class_of(o)->name, c->name);
            }
            NEXT(2);
        }
        case 0x20: {                                             /* instance-of */
            mg_jobj *o = RL(B4);
            if (o) {
                mg_dvm_class *c;
                RESOLVE_TYPE(c, FETCH(1));
                WI(A4, mg_dvm_class_of(o) == c || mg_dvm_assignable(mg_dvm_class_of(o), c));
            } else {
                WI(A4, 0);
            }
            NEXT(2);
        }
        case 0x21: {                                             /* array-length */
            mg_jobj *a = RL(B4);
            if (!a) THROW("java/lang/NullPointerException", "Attempt to get length of null array");
            WI(A4, mg_dvm_array_length(a));
            NEXT(1);
        }
        case 0x22: {                                             /* new-instance */
            mg_dvm_class *c;
            mg_jobj *o;
            RESOLVE_TYPE(c, FETCH(1));
            f->pc = pc;
            if (c->state != DVM_CS_INITIALIZED && !mg_dvm_ensure_init(c)) goto exception;
            o = mg_dvm_alloc_object(c);
            if (!o) goto exception;
            WL(AA, o);
            NEXT(2);
        }
        case 0x23: {                                             /* new-array */
            mg_dvm_class *c;
            mg_jobj *a;
            RESOLVE_TYPE(c, FETCH(1));
            if (c->name[0] != '[') THROW("java/lang/VerifyError", "new-array of %s", c->name);
            f->pc = pc;
            a = mg_dvm_alloc_array(c, RI(B4));
            if (!a) goto exception;
            WL(A4, a);
            NEXT(2);
        }
        case 0x24: case 0x25: {                                  /* filled-new-array(/range) */
            bool range = (w & 0xFF) == 0x25;
            u32 count = range ? AA : B4, i;
            u16 list = FETCH(2);
            mg_dvm_class *c;
            mg_jobj *a;
            RESOLVE_TYPE(c, FETCH(1));
            /* Arrays of int and of references only, which is all Android's own runtime fills this way. */
            if (c->name[0] != '[' || (c->elem != 'I' && c->elem != 'L')) THROW("java/lang/InternalError", "filled-new-array of %s", c->name);
            f->pc = pc;
            a = mg_dvm_alloc_array(c, (i32)count);
            if (!a) goto exception;
            for (i = 0; i < count; i++) {
                u32 r = range ? (u32)list + i : (i < 4 ? ((u32)list >> (4 * i)) & 0xF : A4);
                if (c->elem == 'L') a->u.oarr.v[i] = RL(r);
                else ((i32 *)a->u.arr.data)[i] = RI(r);
            }
            result.j = 0;
            result.l = a;
            NEXT(3);
        }
        case 0x26: {                                             /* fill-array-data */
            mg_jobj *a = RL(AA);
            const u16 *p = payload_at(m, pc, (i32)(FETCH(1) | (u32)FETCH(2) << 16), 0x0300);
            u32 width, n;
            if (!a) THROW("java/lang/NullPointerException", "fill-array-data on a null array");
            if (!p) THROW("java/lang/VerifyError", "fill-array-data without its data in %s.%s", m->cls->name, m->name);
            width = p[1];
            n = p[2] | (u32)p[3] << 16;
            if (a->kind != MG_J_PRIM_ARRAY || a->u.arr.esz != width)
                THROW("java/lang/InternalError", "fill-array-data of width %u on a %s", (unsigned)width, mg_dvm_class_of(a)->name);
            if (n > a->u.arr.len)
                THROW("java/lang/ArrayIndexOutOfBoundsException", "fill-array-data: %u elements into an array of %u", (unsigned)n, (unsigned)a->u.arr.len);
            memcpy(a->u.arr.data, p + 4, (size_t)width * n);
            NEXT(3);
        }
        case 0x27: {                                             /* throw */
            mg_jobj *e = RL(AA);
            f->pc = pc;
            if (!e) mg_dvm_throw("java/lang/NullPointerException", "throw with null exception");
            else t->pending = e;
            /* With the trace on, every exception bytecode throws is a line, with where: one that is caught and dropped is otherwise never seen. */
            if (e && g_dvm.trace >= 1) mg_info("dvm: throw %s in %s.%s", mg_dvm_class_of(e)->name, f->m->cls->name, f->m->name);
            goto exception;
        }
        case 0x28: JUMP((i8)(w >> 8));
        case 0x29: JUMP((i16)FETCH(1));
        case 0x2a: JUMP((i32)(FETCH(1) | (u32)FETCH(2) << 16));
        case 0x2b: {                                             /* packed-switch: consecutive keys from the first */
            const u16 *p = payload_at(m, pc, (i32)(FETCH(1) | (u32)FETCH(2) << 16), 0x0100);
            i64 i;
            if (!p) THROW("java/lang/VerifyError", "packed-switch without its table in %s.%s", m->cls->name, m->name);
            i = (i64)RI(AA) - (i32)(p[2] | (u32)p[3] << 16);
            if (i >= 0 && i < p[1]) {
                const u16 *target = p + 4 + 2 * i;
                JUMP((i32)(target[0] | (u32)target[1] << 16));
            }
            NEXT(3);
        }
        case 0x2c: {                                             /* sparse-switch: sorted keys, then their targets */
            const u16 *p = payload_at(m, pc, (i32)(FETCH(1) | (u32)FETCH(2) << 16), 0x0200);
            i32 v = RI(AA), lo = 0, hi;
            if (!p) THROW("java/lang/VerifyError", "sparse-switch without its table in %s.%s", m->cls->name, m->name);
            hi = (i32)p[1] - 1;
            while (lo <= hi) {
                i32 mid = (lo + hi) / 2;
                i32 key = (i32)(p[2 + 2 * mid] | (u32)p[3 + 2 * mid] << 16);
                if (key < v) lo = mid + 1;
                else if (key > v) hi = mid - 1;
                else {
                    const u16 *target = p + 2 + 2 * (i32)p[1] + 2 * mid;
                    lo = (i32)(target[0] | (u32)target[1] << 16);
                    hi = -2;
                    break;
                }
            }
            if (hi == -2) JUMP(lo);
            NEXT(3);
        }
        case 0x2d: case 0x2e: {                                  /* cmpl-float, cmpg-float: what a NaN compares as is the difference */
            float a = RF(FETCH(1) & 0xFF), b = RF(FETCH(1) >> 8);
            WI(AA, a < b ? -1 : a > b ? 1 : a == b ? 0 : ((w & 0xFF) == 0x2d ? -1 : 1));
            NEXT(2);
        }
        case 0x2f: case 0x30: {
            double a = RD(FETCH(1) & 0xFF), b = RD(FETCH(1) >> 8);
            WI(AA, a < b ? -1 : a > b ? 1 : a == b ? 0 : ((w & 0xFF) == 0x2f ? -1 : 1));
            NEXT(2);
        }
        case 0x31: {
            i64 a = RJ(FETCH(1) & 0xFF), b = RJ(FETCH(1) >> 8);
            WI(AA, a < b ? -1 : a > b ? 1 : 0);
            NEXT(2);
        }
        /* Ints are kept zero-extended and references whole, so equality is the whole register either way. */
        IF2(0x32, regs[A4] == regs[B4])
        IF2(0x33, regs[A4] != regs[B4])
        IF2(0x34, RI(A4) < RI(B4))
        IF2(0x35, RI(A4) >= RI(B4))
        IF2(0x36, RI(A4) > RI(B4))
        IF2(0x37, RI(A4) <= RI(B4))
        IF2(0x38, regs[AA] == 0)
        IF2(0x39, regs[AA] != 0)
        IF2(0x3a, RI(AA) < 0)
        IF2(0x3b, RI(AA) >= 0)
        IF2(0x3c, RI(AA) > 0)
        IF2(0x3d, RI(AA) <= 0)

        /* aget, aput */
        case 0x44: { mg_jobj *a; i32 i; ARRAY(a, i, 4); WI(AA, ((i32 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x45: { mg_jobj *a; i32 i; ARRAY(a, i, 8); WJ(AA, ((i64 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x46: { mg_jobj *a; i32 i; ARRAY(a, i, 0); WL(AA, a->u.oarr.v[i]); NEXT(2); }
        case 0x47: { mg_jobj *a; i32 i; ARRAY(a, i, 1); WI(AA, ((u8 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x48: { mg_jobj *a; i32 i; ARRAY(a, i, 1); WI(AA, (i32)((i8 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x49: { mg_jobj *a; i32 i; ARRAY(a, i, 2); WI(AA, ((u16 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x4a: { mg_jobj *a; i32 i; ARRAY(a, i, 2); WI(AA, (i32)((i16 *)a->u.arr.data)[i]); NEXT(2); }
        case 0x4b: { mg_jobj *a; i32 i; ARRAY(a, i, 4); ((i32 *)a->u.arr.data)[i] = RI(AA); NEXT(2); }
        case 0x4c: { mg_jobj *a; i32 i; ARRAY(a, i, 8); ((i64 *)a->u.arr.data)[i] = RJ(AA); NEXT(2); }
        case 0x4d: {
            mg_jobj *a, *v = RL(AA);
            i32 i;
            ARRAY(a, i, 0);
            if (v && !mg_dvm_is_instance(v, mg_dvm_class_of(a)->component))
                THROW("java/lang/ArrayStoreException", "%s cannot be stored in an array of type %s", mg_dvm_class_of(v)->name, mg_dvm_class_of(a)->name);
            a->u.oarr.v[i] = v;
            NEXT(2);
        }
        case 0x4e: { mg_jobj *a; i32 i; ARRAY(a, i, 1); ((u8 *)a->u.arr.data)[i] = (u8)regs[AA]; NEXT(2); }
        case 0x4f: { mg_jobj *a; i32 i; ARRAY(a, i, 1); ((i8 *)a->u.arr.data)[i] = (i8)regs[AA]; NEXT(2); }
        case 0x50: { mg_jobj *a; i32 i; ARRAY(a, i, 2); ((u16 *)a->u.arr.data)[i] = (u16)regs[AA]; NEXT(2); }
        case 0x51: { mg_jobj *a; i32 i; ARRAY(a, i, 2); ((i16 *)a->u.arr.data)[i] = (i16)regs[AA]; NEXT(2); }

        /* iget, iput */
        case 0x52: { mg_jvalue *s; IFIELD(s); WI(A4, s->i); NEXT(2); }
        case 0x53: { mg_jvalue *s; IFIELD(s); WJ(A4, s->j); NEXT(2); }
        case 0x54: { mg_jvalue *s; IFIELD(s); WL(A4, s->l); NEXT(2); }
        case 0x55: { mg_jvalue *s; IFIELD(s); WI(A4, s->z); NEXT(2); }
        case 0x56: { mg_jvalue *s; IFIELD(s); WI(A4, (i32)s->b); NEXT(2); }
        case 0x57: { mg_jvalue *s; IFIELD(s); WI(A4, s->c); NEXT(2); }
        case 0x58: { mg_jvalue *s; IFIELD(s); WI(A4, (i32)s->s); NEXT(2); }
        case 0x59: { mg_jvalue *s; IFIELD(s); s->i = RI(A4); NEXT(2); }
        case 0x5a: { mg_jvalue *s; IFIELD(s); s->j = RJ(A4); NEXT(2); }
        case 0x5b: { mg_jvalue *s; IFIELD(s); s->l = RL(A4); NEXT(2); }
        case 0x5c: { mg_jvalue *s; IFIELD(s); s->z = (u8)regs[A4]; NEXT(2); }
        case 0x5d: { mg_jvalue *s; IFIELD(s); s->b = (i8)regs[A4]; NEXT(2); }
        case 0x5e: { mg_jvalue *s; IFIELD(s); s->c = (u16)regs[A4]; NEXT(2); }
        case 0x5f: { mg_jvalue *s; IFIELD(s); s->s = (i16)regs[A4]; NEXT(2); }

        /* sget, sput */
        case 0x60: { mg_jvalue *s; SFIELD(s); WI(AA, s->i); NEXT(2); }
        case 0x61: { mg_jvalue *s; SFIELD(s); WJ(AA, s->j); NEXT(2); }
        case 0x62: { mg_jvalue *s; SFIELD(s); WL(AA, s->l); NEXT(2); }
        case 0x63: { mg_jvalue *s; SFIELD(s); WI(AA, s->z); NEXT(2); }
        case 0x64: { mg_jvalue *s; SFIELD(s); WI(AA, (i32)s->b); NEXT(2); }
        case 0x65: { mg_jvalue *s; SFIELD(s); WI(AA, s->c); NEXT(2); }
        case 0x66: { mg_jvalue *s; SFIELD(s); WI(AA, (i32)s->s); NEXT(2); }
        case 0x67: { mg_jvalue *s; SFIELD(s); s->i = RI(AA); NEXT(2); }
        case 0x68: { mg_jvalue *s; SFIELD(s); s->j = RJ(AA); NEXT(2); }
        case 0x69: { mg_jvalue *s; SFIELD(s); s->l = RL(AA); NEXT(2); }
        case 0x6a: { mg_jvalue *s; SFIELD(s); s->z = (u8)regs[AA]; NEXT(2); }
        case 0x6b: { mg_jvalue *s; SFIELD(s); s->b = (i8)regs[AA]; NEXT(2); }
        case 0x6c: { mg_jvalue *s; SFIELD(s); s->c = (u16)regs[AA]; NEXT(2); }
        case 0x6d: { mg_jvalue *s; SFIELD(s); s->s = (i16)regs[AA]; NEXT(2); }

        case 0x6e: case 0x6f: case 0x70: case 0x71: case 0x72: {  /* invoke-kind {vC, vD, vE, vF, vG} */
            u16 list = FETCH(2);
            inv_kind = (w & 0xFF) - 0x6e;
            inv_idx = FETCH(1);
            inv_count = B4 > 5 ? 5 : B4;
            /* Only the registers the count names: the nibbles beyond it may name registers the method does not have. */
            switch (inv_count) {
            case 5: inv_five[4] = regs[A4]; /* fall through */
            case 4: inv_five[3] = regs[list >> 12]; /* fall through */
            case 3: inv_five[2] = regs[(list >> 8) & 0xF]; /* fall through */
            case 2: inv_five[1] = regs[(list >> 4) & 0xF]; /* fall through */
            case 1: inv_five[0] = regs[list & 0xF]; /* fall through */
            default: break;
            }
            inv_argv = inv_five;
            goto do_invoke;
        }
        case 0x74: case 0x75: case 0x76: case 0x77: case 0x78:    /* invoke-kind/range {vCCCC .. vNNNN} */
            inv_kind = (w & 0xFF) - 0x74;
            inv_idx = FETCH(1);
            inv_count = AA;
            inv_argv = regs + FETCH(2);
            goto do_invoke;

        UNARY(0x7b, WI, 0u - (u32)RI(B4))
        UNARY(0x7c, WI, ~RI(B4))
        UNARY(0x7d, WJ, (u64)0 - (u64)RJ(B4))
        UNARY(0x7e, WJ, ~RJ(B4))
        UNARY(0x7f, WF, -RF(B4))
        UNARY(0x80, WD, -RD(B4))
        UNARY(0x81, WJ, (i64)RI(B4))
        UNARY(0x82, WF, (float)RI(B4))
        UNARY(0x83, WD, (double)RI(B4))
        UNARY(0x84, WI, (u32)(u64)RJ(B4))
        UNARY(0x85, WF, (float)RJ(B4))
        UNARY(0x86, WD, (double)RJ(B4))
        UNARY(0x87, WI, float_to_int(RF(B4)))
        UNARY(0x88, WJ, double_to_long((double)RF(B4)))
        UNARY(0x89, WD, (double)RF(B4))
        UNARY(0x8a, WI, double_to_int(RD(B4)))
        UNARY(0x8b, WJ, double_to_long(RD(B4)))
        UNARY(0x8c, WF, (float)RD(B4))
        UNARY(0x8d, WI, (i32)(i8)RI(B4))
        UNARY(0x8e, WI, (u16)RI(B4))
        UNARY(0x8f, WI, (i32)(i16)RI(B4))

        BIN(0x90, i32, RI, WI, (u32)a + (u32)b)
        BIN(0x91, i32, RI, WI, (u32)a - (u32)b)
        BIN(0x92, i32, RI, WI, (u32)a * (u32)b)
        DIVIDE(0x93, i32, RI, WI, div_int(a, b))
        DIVIDE(0x94, i32, RI, WI, rem_int(a, b))
        BIN(0x95, i32, RI, WI, a & b)
        BIN(0x96, i32, RI, WI, a | b)
        BIN(0x97, i32, RI, WI, a ^ b)
        BIN(0x98, i32, RI, WI, (u32)a << (b & 31))
        BIN(0x99, i32, RI, WI, shr_int(a, (u32)b & 31))
        BIN(0x9a, i32, RI, WI, (u32)a >> (b & 31))
        BIN(0x9b, i64, RJ, WJ, (u64)a + (u64)b)
        BIN(0x9c, i64, RJ, WJ, (u64)a - (u64)b)
        BIN(0x9d, i64, RJ, WJ, (u64)a * (u64)b)
        DIVIDE(0x9e, i64, RJ, WJ, div_long(a, b))
        DIVIDE(0x9f, i64, RJ, WJ, rem_long(a, b))
        BIN(0xa0, i64, RJ, WJ, a & b)
        BIN(0xa1, i64, RJ, WJ, a | b)
        BIN(0xa2, i64, RJ, WJ, a ^ b)
        SHIFT_LONG(0xa3, (u64)a << n)
        SHIFT_LONG(0xa4, shr_long(a, n))
        SHIFT_LONG(0xa5, (u64)a >> n)
        BIN(0xa6, float, RF, WF, a + b)
        BIN(0xa7, float, RF, WF, a - b)
        BIN(0xa8, float, RF, WF, a * b)
        BIN(0xa9, float, RF, WF, a / b)
        BIN(0xaa, float, RF, WF, fmodf(a, b))
        BIN(0xab, double, RD, WD, a + b)
        BIN(0xac, double, RD, WD, a - b)
        BIN(0xad, double, RD, WD, a * b)
        BIN(0xae, double, RD, WD, a / b)
        BIN(0xaf, double, RD, WD, fmod(a, b))

        case 0xd0: case 0xd1: case 0xd2: case 0xd3: case 0xd4: case 0xd5: case 0xd6: case 0xd7:     /* int with a 16-bit literal */
        case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:     /* int with an 8-bit literal */
        case 0xe0: case 0xe1: case 0xe2: {
            u32 op = w & 0xFF, dst, src;
            i32 lit, a, v = 0;
            if (op <= 0xd7) {
                dst = A4;
                src = B4;
                lit = (i16)FETCH(1);
                op -= 0xd0;
            } else {
                dst = AA;
                src = FETCH(1) & 0xFF;
                lit = (i8)(FETCH(1) >> 8);
                op -= 0xd8;
            }
            a = RI(src);
            switch (op) {
            case 0: v = (i32)((u32)a + (u32)lit); break;
            case 1: v = (i32)((u32)lit - (u32)a); break;         /* rsub: the literal minus the register */
            case 2: v = (i32)((u32)a * (u32)lit); break;
            case 3: case 4:
                if (!lit) THROW("java/lang/ArithmeticException", "divide by zero");
                v = op == 3 ? div_int(a, lit) : rem_int(a, lit);
                break;
            case 5: v = a & lit; break;
            case 6: v = a | lit; break;
            case 7: v = a ^ lit; break;
            case 8: v = (i32)((u32)a << (lit & 31)); break;
            case 9: v = shr_int(a, (u32)lit & 31); break;
            default: v = (i32)((u32)a >> (lit & 31)); break;
            }
            WI(dst, v);
            NEXT(2);
        }

        case 0xfa: case 0xfb:                                    /* invoke-polymorphic(/range) */
            mg_note_once("dvm: invoke-polymorphic is not implemented (met in %s.%s)", m->cls->name, m->name);
            THROW("java/lang/UnsupportedOperationException", "invoke-polymorphic (a MethodHandle or VarHandle call) is not implemented, in %s.%s", m->cls->name, m->name);
        case 0xfc: case 0xfd:                                    /* invoke-custom(/range) */
            mg_note_once("dvm: invoke-custom is not implemented (met in %s.%s)", m->cls->name, m->name);
            THROW("java/lang/UnsupportedOperationException", "invoke-custom (a call site bound at run time) is not implemented, in %s.%s", m->cls->name, m->name);
        case 0xfe: case 0xff:                                    /* const-method-handle, const-method-type */
            THROW("java/lang/UnsupportedOperationException", "%s is not implemented, in %s.%s", mg_dvm_insn_name(w), m->cls->name, m->name);
        default:
            THROW("java/lang/VerifyError", "unused opcode %#x in %s.%s at %u", (unsigned)(w & 0xFF), m->cls->name, m->name, (unsigned)pc);
        }

    do_invoke: {
        mg_dvm_method *ref, *target;
        mg_jobj *self = NULL;
        mg_dvm_frame *nf;
        u32 base, n;
        /* A place where the other loop may take over: the invoke is decoded again there, nothing having changed yet. */
        SAFEPOINT(pc);
        f->pc = pc;
        ref = inv_idx < dex->nmeth ? dex->mcache[inv_idx] : NULL;
        if (!ref || ((ref->flags & DVM_ACC_STATIC) != 0) != (inv_kind == DVM_INVOKE_STATIC)) {
            ref = mg_dvm_resolve_method(dex, inv_idx, inv_kind);
            if (!ref) goto exception;
        }
        target = ref;
        if (inv_kind != DVM_INVOKE_STATIC) {
            self = inv_count ? (mg_jobj *)inv_argv[0] : NULL;
            if (!self) {
                mg_dvm_throw("java/lang/NullPointerException", "Attempt to invoke %s method '%s.%s%s' on a null object reference",
                             inv_kind == DVM_INVOKE_INTERFACE ? "interface" : inv_kind == DVM_INVOKE_DIRECT ? "direct" : "virtual", ref->cls->name, ref->name,
                             ref->sig);
                goto exception;
            }
        }
        switch (inv_kind) {
        case DVM_INVOKE_VIRTUAL: case DVM_INVOKE_INTERFACE: {
            mg_dvm_class *rc = mg_dvm_class_of(self);
            if (!(ref->cls->flags & DVM_ACC_INTERFACE) && ref->vidx < 0) break;      /* a private or final method named as if virtual: itself */
            if (!(ref->cls->flags & DVM_ACC_INTERFACE) && ref->vidx < rc->nvtab) {
                target = rc->vtab[ref->vidx];
            } else {
                /* Through an interface, or a method an abstract class only inherits from one: by name on the object's class, remembered there. */
                u32 slot = (u32)(((mg_addr)ref->name >> 3) ^ ((mg_addr)ref->sig >> 5)) % DVM_ICACHE;
                target = rc->icache[slot];
                if (!target || target->name != ref->name || target->sig != ref->sig) {
                    target = mg_dvm_find_virtual(rc, ref->name, ref->sig);
                    if (!target) {
                        mg_dvm_throw("java/lang/IncompatibleClassChangeError", "%s does not implement %s.%s%s", rc->name, ref->cls->name, ref->name, ref->sig);
                        goto exception;
                    }
                    rc->icache[slot] = target;
                }
            }
            break;
        }
        case DVM_INVOKE_SUPER: {
            mg_dvm_class *sc = m->cls->super;
            if (ref->cls->flags & DVM_ACC_INTERFACE) break;      /* Interface.super.method(): that interface's own */
            if (sc && ref->vidx >= 0 && ref->vidx < sc->nvtab) target = sc->vtab[ref->vidx];
            else target = sc ? mg_dvm_find_virtual(sc, ref->name, ref->sig) : NULL;
            if (!target) {
                mg_dvm_throw("java/lang/NoSuchMethodError", "%s.%s%s above %s", ref->cls->name, ref->name, ref->sig, m->cls->name);
                goto exception;
            }
            break;
        }
        case DVM_INVOKE_STATIC:
            if (target->cls->state != DVM_CS_INITIALIZED && !mg_dvm_ensure_init(target->cls)) goto exception;
            break;
        default:                                                 /* direct */
            if (self->kind == MG_J_STRING && ref->cls == g_dvm.c_string && ref->name[0] == '<') {
                /*
                 * new String(...): a string's length is fixed when it is made, so the object new-instance made cannot become
                 * the result. The constructor's result replaces it in every register that holds it, as it does in ART.
                 */
                mg_jvalue args[8];
                mg_jobj *s;
                u32 i;
                if (ref->nparams > MG_COUNT(args)) {
                    mg_dvm_throw("java/lang/InternalError", "String%s", ref->sig);
                    goto exception;
                }
                marshal(ref, inv_argv, args);
                if (!mg_dvm_string_init(ref->sig, args, &s)) goto exception;
                for (i = 0; i < m->regs; i++)
                    if (regs[i] == (mg_dvm_reg)self) regs[i] = (mg_dvm_reg)s;
                result.j = 0;
                NEXT(3);
            }
            break;
        }
        /* A framework method that is its signature alone: its first call said (before a trivial body is done in place). */
        if (target->stub) mg_dvm_stub_called(m, target);
        /* One the interpreter does itself, when its arguments allow (not while every instruction is watched: the trace, the profile). */
        if (target->intrinsic && !g_dvm.slow && intrinsic(t, target->intrinsic, inv_argv, &result)) NEXT(3);
        /* A method whose body is trivial, done in place; counted as the instructions it would have executed. */
        if (target->shape != SHAPE_REGULAR && target->insns && !g_dvm.slow) {
            u32 shape;
            if (target->shape == SHAPE_UNKNOWN) classify(target);
            shape = target->shape;
            if (SHAPE_OF(shape) != SHAPE_REGULAR && trivial(target, shape, inv_argv, &result)) {
                executed += target->ninsns == 1 ? 1 : 2;
                NEXT(3);
            }
        }
        if (!target->insns) {
            if (!(target->flags & DVM_ACC_NATIVE)) {
                mg_dvm_throw("java/lang/AbstractMethodError", "%s.%s%s", target->cls->name, target->name, target->sig);
                goto exception;
            }
            t->executed += executed;
            executed = 0;
            if (!invoke_native(t, target, self, inv_argv, NULL, &result)) goto exception;
            NEXT(3);
        }
        if (g_dvm.trace >= 2) mg_info("dvm: %*s-> %s.%s%s", t->depth, "", target->cls->name, target->name, target->sig);
        nf = push_frame(t, target, target->regs);
        if (!nf) goto exception;
        if (g_call_trace) call_trace_enter(t, target, "", inv_argv, inv_count);
        /*
         * The arguments are the callee's last registers, in the order and the widths the caller's hold them; the others
         * start at zero. (Loops, not memset and memcpy: a call has a few registers, and the C library's calls cost more.)
         */
        base = (u32)target->regs - target->ins;
        n = inv_count < target->ins ? inv_count : target->ins;
        {
            mg_dvm_reg *to = nf->regs;
            u32 k;
            for (k = 0; k < base; k++) to[k] = 0;
            for (k = 0; k < n; k++) to[base + k] = inv_argv[k];
            for (k = base + n; k < target->regs; k++) to[k] = 0;
        }
        if (target->flags & DVM_ACC_SYNCHRONIZED) {
            nf->sync = (target->flags & DVM_ACC_STATIC) ? mg_dvm_mirror(target->cls) : self;
            if (nf->sync) mg_dvm_monitor_enter(nf->sync);
        }
        f = nf;
        LOAD_FRAME();
        pc = 0;
        /* Its compiled code, when its start is an entry: run until it returns or hands the frame back. */
        JIT_BACK(0)
        continue;
    }

    do_return:
        if (g_call_trace) call_trace_return(t, f->m, &result);
        if (f->sync) mg_dvm_monitor_exit(f->sync);
        if (f->entry) {
            *ret = result;
            pop_frame(t);
            t->executed += executed;
            return DVM_RETURNED;
        }
        pop_frame(t);
        f = t->top;
        LOAD_FRAME();
        pc = f->pc + 3;                                          /* past the invoke, which is three code units in every form */
        continue;

    exception: {
        mg_jobj *e = t->pending;
        u32 target_pc = 0;
        if (!e) {
            mg_dvm_throw("java/lang/InternalError", "an exception with nothing pending, in %s.%s", m->cls->name, m->name);
            e = t->pending;
        }
        for (;;) {
            if (find_handler(t, m, pc, e, &target_pc)) {
                t->pending = NULL;
                caught = e;
                pc = target_pc;
                break;
            }
            if (g_call_trace) call_trace_unwind(t, f->m, e);
            if (f->sync) mg_dvm_monitor_exit(f->sync);
            if (f->entry) {
                pop_frame(t);
                t->executed += executed;
                return DVM_THREW;
            }
            pop_frame(t);
            f = t->top;
            LOAD_FRAME();
            pc = f->pc;
        }
        continue;
    }
    }

    switch_loop:
        /* The other loop goes on from f->pc of the frame on top (interpret). */
        t->executed += executed;
        return DVM_SWITCH;
}
