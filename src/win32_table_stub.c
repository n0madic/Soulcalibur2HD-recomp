/*
 * Windows host only. The toolkit's runtime (lv2_register.c, a debug backtrace) refers to the lifted code's
 * function_table by name. A monolith build links the real table; the host loads the game as a module and keeps the
 * table it finds there in g_game_function_table (main.cpp), so these empty ones only satisfy the linker.
 * Keep the layout in step with func_entry in the lifter's header preamble.
 */
struct stub_func_entry {
    unsigned long long addr;
    void*              func;
    const char*        name;
};

const struct stub_func_entry function_table[1] = { { 0, 0, 0 } };
const unsigned long long     function_table_count = 0;
