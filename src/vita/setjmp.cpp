// setjmp / longjmp that save only the core registers (r4-r11, sp and lr: 40 bytes).
//
// The C library in current VitaSDK toolchains also saves the VFP registers d8-d15, 104 bytes
// in all, but the prebuilt libraries VivoTube links (libpng, libvita2d, FreeType, pthreads) were
// compiled when jmp_buf was 92 bytes. Their setjmp calls then wrote 12 bytes past their buffers:
// over libpng's error-handling pointers (a crash on every PNG load on a real Vita, where d14/d15
// hold leftover values; Vita3K starts them at zero), over FreeType's cmap validator state, and
// over libpng's own stack frames. These definitions take precedence over the C library's, so
// every caller gets a jump buffer write that fits, as the libraries were built to expect. Like
// the older toolchains, longjmp doesn't restore d8-d15; the libraries only longjmp on error
// paths that don't keep floating-point values across setjmp.

extern "C" {

__attribute__((naked, used)) int setjmp(void* /*env*/) {
    asm volatile(
        "mov ip, sp\n\t"
        "stmia r0!, {r4-r11, ip, lr}\n\t"
        "movs r0, #0\n\t"
        "bx lr\n\t");
}

__attribute__((naked, used)) void longjmp(void* /*env*/, int /*value*/) {
    asm volatile(
        "ldmia r0!, {r4-r11, ip, lr}\n\t"
        "mov sp, ip\n\t"
        "movs r0, r1\n\t"  // setjmp returns the value, or 1 for 0
        "it eq\n\t"
        "moveq r0, #1\n\t"
        "bx lr\n\t");
}

}  // extern "C"
