/*
 * Imports nothing provides (tools/build-guest.sh builds this twice; mg_tests.c, test_unresolved_imports): the loader names
 * each in the library's line as it loads, once, however many relocations ask for it.
 *
 * libt_unres.so: two functions, one of them asked for by its PLT slot and by a pointer in a table too, and a variable asked
 * for through the GOT and by a pointer in the table.
 * libt_unres_many.so (MANY): forty functions of long names, more than the line has room for: it names the first and says
 * how many more.
 */
#ifndef MANY
extern int mg_absent_one(int x);
extern int mg_absent_two(void);
extern int mg_absent_data;

/* (Pointers in data: R_ARM_ABS32 against the same names as the calls' and the GOT's relocations.) */
void *const t_unres_table[2] = { (void *)&mg_absent_two, (void *)&mg_absent_data };

int t_unres_call_one(int x) { return mg_absent_one(x) + 7; }
int t_unres_call_two(void) { return mg_absent_two() + 9; }
int *t_unres_data(void) { return &mg_absent_data; }
void *const *t_unres_pointers(void) { return t_unres_table; }
#else
#define ABSENT(n)                                                                     \
    extern int mg_absent_function_whose_name_is_rather_long_number_##n(void);        \
    int t_unres_many_##n(void) { return mg_absent_function_whose_name_is_rather_long_number_##n(); }
ABSENT(0) ABSENT(1) ABSENT(2) ABSENT(3) ABSENT(4) ABSENT(5) ABSENT(6) ABSENT(7) ABSENT(8) ABSENT(9)
ABSENT(10) ABSENT(11) ABSENT(12) ABSENT(13) ABSENT(14) ABSENT(15) ABSENT(16) ABSENT(17) ABSENT(18) ABSENT(19)
ABSENT(20) ABSENT(21) ABSENT(22) ABSENT(23) ABSENT(24) ABSENT(25) ABSENT(26) ABSENT(27) ABSENT(28) ABSENT(29)
ABSENT(30) ABSENT(31) ABSENT(32) ABSENT(33) ABSENT(34) ABSENT(35) ABSENT(36) ABSENT(37) ABSENT(38) ABSENT(39)
#endif
