#pragma once
#include <windows.h>

// Fault guard for reading reverse-engineered engine structures. LLVM-MinGW i686 has no working
// __try, so a vectored handler redirects a faulting guarded call to a resume stub, which then
// leaves through __builtin_longjmp outside the exception dispatcher.
namespace vegas::guard {
struct State {
    void* jump[5];
    volatile bool active;
};
inline thread_local State tls{};
[[noreturn]] __attribute__((noinline)) inline void resume() { __builtin_longjmp(tls.jump, 1); }
inline LONG CALLBACK handler(EXCEPTION_POINTERS* info) {
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (tls.active && (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
            code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION ||
            code == EXCEPTION_DATATYPE_MISALIGNMENT || code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED)) {
        tls.active = false;
        info->ContextRecord->Eip = reinterpret_cast<DWORD>(&resume);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
inline void* registered = nullptr;
inline void install() {
    if (!registered) registered = AddVectoredExceptionHandler(1, handler);
}
// Before the module holding the handler is unloaded.
inline void uninstall() {
    if (registered) RemoveVectoredExceptionHandler(registered);
    registered = nullptr;
}
// Runs fn; false if it faulted. Not reentrant: a guarded call must not nest another.
template<class F> __attribute__((noinline)) bool run(F&& fn) {
    if (tls.active) { fn(); return true; }
    if (__builtin_setjmp(tls.jump)) return false;
    tls.active = true;
    __asm__ volatile("" ::: "memory");
    fn();
    __asm__ volatile("" ::: "memory");
    tls.active = false;
    return true;
}
} // namespace vegas::guard
