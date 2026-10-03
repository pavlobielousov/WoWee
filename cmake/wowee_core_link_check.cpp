// The program behind wowee_core_link_check (cmake/wowee_core.cmake, VITA-47): it does nothing. It
// exists so that every object of wowee_core is linked into an executable, which fails with
// "undefined reference" for any symbol the core needs that it does not itself define.
int main() { return 0; }
