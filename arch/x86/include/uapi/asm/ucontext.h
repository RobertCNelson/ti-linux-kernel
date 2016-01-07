#ifndef _ASM_X86_UCONTEXT_H
#define _ASM_X86_UCONTEXT_H

/*
 * indicates the presence of extended state
 * information in the memory layout pointed
 * by the fpstate pointer in the ucontext's
 * sigcontext struct (uc_mcontext).
 */
#define UC_FP_XSTATE	0x1

#ifdef __x86_64__
/*
 * UC_SIGCONTEXT_SS will be set when delivering 64-bit or x32 signals on
 * kernels that save SS in the sigcontext.  All kernels that set
 * UC_SIGCONTEXT_SS will correctly restore at least the low 32 bits of esp
 * regardless of SS (i.e. they implement espfix).
 *
 * Kernels that set UC_SIGCONTEXT_SS will also set UC_STRICT_RESTORE_SS
 * when delivering a signal that came from 64-bit code.
 *
 * Sigreturn modifies its behavior depending on the
 * UC_STRICT_RESTORE_SS flag.  If UC_STRICT_RESTORE_SS is set, or if
 * the CS value in the signal context does not refer to a 64-bit
 * code segment, then the SS value in the signal context is restored
 * verbatim.  If UC_STRICT_RESTORE_SS is not set, the CS value in
 * the signal context refers to a 64-bit code segment, and the
 * signal context's SS value is invalid, then SS it will be replaced
 * with a flat 32-bit selector.

 * This behavior serves two purposes.  It ensures that older programs
 * that are unaware of the signal context's SS slot and either construct
 * a signal context from scratch or that catch signals from segmented
 * contexts and change CS to a 64-bit selector won't crash due to a bad
 * SS value.  It also ensures that signal handlers that do not modify
 * the signal context at all return back to the exact CS and SS state
 * that they came from.
 */
#define UC_SIGCONTEXT_SS	0x2
#define UC_STRICT_RESTORE_SS	0x4
#endif

#include <asm-generic/ucontext.h>

#endif /* _ASM_X86_UCONTEXT_H */
