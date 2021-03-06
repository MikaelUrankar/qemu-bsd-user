/*
 *  powerpc cpu init and loop
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _TARGET_ARCH_CPU_H_
#define _TARGET_ARCH_CPU_H_

#include "target_arch.h"

#if defined(TARGET_PPC64) && !defined(TARGET_ABI32)
#define TARGET_DEFAULT_CPU_MODEL "ppc64"
#else
#define TARGET_DEFAULT_CPU_MODEL "ppc"
#endif

#define TARGET_CPU_RESET(cpu)	cpu_reset(cpu)

static inline void target_cpu_set_tls(CPUPPCState *env, target_ulong newtls)
{
#if defined(TARGET_PPC64)
    /* The kernel checks TIF_32BIT here; we don't support loading 32-bit
       binaries on PPC64 yet. */
    env->gpr[13] = newtls + 0x7010;
#else
    env->gpr[2] = newtls + 0x7008;
#endif
}

static inline void target_cpu_init(CPUPPCState *env,
        struct target_pt_regs *regs)
{
    int i;

#ifdef TARGET_PPC64
	env->msr |= (target_ulong)1 << MSR_SF;
#endif
    for (i = 0; i < 32; i++) {
        env->gpr[i] = regs->gpr[i];
    }
    env->nip = regs->nip;
}

static int do_store_exclusive(CPUPPCState *env)
{
    target_ulong addr;
    target_ulong page_addr;
    target_ulong val, val2 __attribute__((unused));
    int flags;
    int segv = 0;

    addr = env->reserve_ea;
    page_addr = addr & TARGET_PAGE_MASK;
    start_exclusive();
    mmap_lock();
    flags = page_get_flags(page_addr);
    if ((flags & PAGE_READ) == 0) {
        segv = 1;
    } else {
        int reg = env->reserve_info & 0x1f;
        int size = (env->reserve_info >> 5) & 0xf;
        int stored = 0;

        if (addr == env->reserve_addr) {
            switch (size) {
            case 1: segv = get_user_u8(val, addr); break;
            case 2: segv = get_user_u16(val, addr); break;
            case 4: segv = get_user_u32(val, addr); break;
#if defined(TARGET_PPC64)
            case 8: segv = get_user_u64(val, addr); break;
            case 16: {
                segv = get_user_u64(val, addr);
                if (!segv) {
                    segv = get_user_u64(val2, addr + 8);
                }
                break;
            }
#endif
            default: abort();
            }
            if (!segv && val == env->reserve_val) {
                val = env->gpr[reg];
                switch (size) {
                case 1: segv = put_user_u8(val, addr); break;
                case 2: segv = put_user_u16(val, addr); break;
                case 4: segv = put_user_u32(val, addr); break;
#if defined(TARGET_PPC64)
                case 8: segv = put_user_u64(val, addr); break;
                case 16: {
                    if (val2 == env->reserve_val2) {
                        segv = put_user_u64(val, addr);
                        if (!segv) {
                            segv = put_user_u64(val2, addr + 8);
                        }
                    }
                    break;
                }
#endif
                default: abort();
                }
                if (!segv) {
                    stored = 1;
                }
            }
        }
        env->crf[0] = (stored << 1) | xer_so;
        env->reserve_addr = (target_ulong)-1;
    }
    if (!segv) {
        env->nip += 4;
    }
    mmap_unlock();
    end_exclusive();
    return segv;
}

static inline void target_cpu_loop(CPUPPCState *env)
{
    CPUState *cs = CPU(ppc_env_get_cpu(env));
    target_siginfo_t info;
    int trapnr;
    target_ulong ret;

    for(;;) {
        cpu_exec_start(cs);
        trapnr = cpu_exec(cs);
        cpu_exec_end(cs);
	process_queued_cpu_work(cs);

        switch(trapnr) {
        case POWERPC_EXCP_NONE:
            /* Just go on */
            break;
        case POWERPC_EXCP_CRITICAL: /* Critical input                        */
            cpu_abort(cs, "Critical interrupt while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_MCHECK:   /* Machine check exception               */
            cpu_abort(cs, "Machine check exception while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_DSI:      /* Data storage exception                */
            fprintf(stderr, "Invalid data memory access: 0x" TARGET_FMT_lx "\n",
                      env->spr[SPR_DAR]);
            /* XXX: check this. Seems bugged */
            if (env->error_code & 0x40000000) {
                info.si_signo = TARGET_SIGSEGV;
                info.si_errno = 0;
                info.si_code = TARGET_SEGV_MAPERR;
            } else if (env->error_code & 0x04000000) {
                info.si_signo = TARGET_SIGILL;
                info.si_errno = 0;
                info.si_code = TARGET_ILL_ILLADR;
            } else if (env->error_code & 0x08000000) {
                info.si_signo = TARGET_SIGSEGV;
                info.si_errno = 0;
                info.si_code = TARGET_SEGV_ACCERR;
            } else {
                /* Let's send a regular segfault... */
                fprintf(stderr, "Invalid segfault errno (%02x)\n",
                          env->error_code);
                info.si_signo = TARGET_SIGSEGV;
                info.si_errno = 0;
                info.si_code = TARGET_SEGV_MAPERR;
            }
            info.si_addr = env->nip;
            queue_signal(env, info.si_signo, &info);
            break;
        case POWERPC_EXCP_ISI:      /* Instruction storage exception         */
            fprintf(stderr, "Invalid instruction fetch: 0x\n" TARGET_FMT_lx
                      "\n", env->spr[SPR_SRR0]);
            /* XXX: check this */
            switch (env->error_code & 0xFF000000) {
            case 0x40000000:
                info.si_signo = TARGET_SIGSEGV;
                info.si_errno = 0;
                info.si_code = TARGET_SEGV_MAPERR;
                break;
            case 0x10000000:
            case 0x08000000:
                info.si_signo = TARGET_SIGSEGV;
                info.si_errno = 0;
                info.si_code = TARGET_SEGV_ACCERR;
                break;
            default:
                /* Let's send a regular segfault... */
                fprintf(stderr, "Invalid segfault errno (%02x)\n",
                          env->error_code);
                info.si_signo = TARGET_SIGSEGV;
                info.si_errno = 0;
                info.si_code = TARGET_SEGV_MAPERR;
                break;
            }
            info.si_addr = env->nip - 4;
            queue_signal(env, info.si_signo, &info);
            break;
        case POWERPC_EXCP_EXTERNAL: /* External input                        */
            cpu_abort(cs, "External interrupt while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_ALIGN:    /* Alignment exception                   */
            fprintf(stderr, "Unaligned memory access\n");
            /* XXX: check this */
            info.si_signo = TARGET_SIGBUS;
            info.si_errno = 0;
            info.si_code = TARGET_BUS_ADRALN;
            info.si_addr = env->nip - 4;
            queue_signal(env, info.si_signo, &info);
            break;
        case POWERPC_EXCP_PROGRAM:  /* Program exception                     */
            /* XXX: check this */
            switch (env->error_code & ~0xF) {
            case POWERPC_EXCP_FP:
                fprintf(stderr, "Floating point program exception\n");
                info.si_signo = TARGET_SIGFPE;
                info.si_errno = 0;
                switch (env->error_code & 0xF) {
                case POWERPC_EXCP_FP_OX:
                    info.si_code = TARGET_FPE_FLTOVF;
                    break;
                case POWERPC_EXCP_FP_UX:
                    info.si_code = TARGET_FPE_FLTUND;
                    break;
                case POWERPC_EXCP_FP_ZX:
                case POWERPC_EXCP_FP_VXZDZ:
                    info.si_code = TARGET_FPE_FLTDIV;
                    break;
                case POWERPC_EXCP_FP_XX:
                    info.si_code = TARGET_FPE_FLTRES;
                    break;
                case POWERPC_EXCP_FP_VXSOFT:
                    info.si_code = TARGET_FPE_FLTINV;
                    break;
                case POWERPC_EXCP_FP_VXSNAN:
                case POWERPC_EXCP_FP_VXISI:
                case POWERPC_EXCP_FP_VXIDI:
                case POWERPC_EXCP_FP_VXIMZ:
                case POWERPC_EXCP_FP_VXVC:
                case POWERPC_EXCP_FP_VXSQRT:
                case POWERPC_EXCP_FP_VXCVI:
                    info.si_code = TARGET_FPE_FLTSUB;
                    break;
                default:
                    fprintf(stderr, "Unknown floating point exception (%02x)\n",
                              env->error_code);
                    break;
                }
                break;
            case POWERPC_EXCP_INVAL:
                fprintf(stderr, "Invalid instruction\n");
                info.si_signo = TARGET_SIGILL;
                info.si_errno = 0;
                switch (env->error_code & 0xF) {
                case POWERPC_EXCP_INVAL_INVAL:
                    info.si_code = TARGET_ILL_ILLOPC;
                    break;
                case POWERPC_EXCP_INVAL_LSWX:
                    info.si_code = TARGET_ILL_ILLOPN;
                    break;
                case POWERPC_EXCP_INVAL_SPR:
                    info.si_code = TARGET_ILL_PRVREG;
                    break;
                case POWERPC_EXCP_INVAL_FP:
                    info.si_code = TARGET_ILL_COPROC;
                    break;
                default:
                    fprintf(stderr, "Unknown invalid operation (%02x)\n",
                              env->error_code & 0xF);
                    info.si_code = TARGET_ILL_ILLADR;
                    break;
                }
                break;
            case POWERPC_EXCP_PRIV:
                fprintf(stderr, "Privilege violation\n");
                info.si_signo = TARGET_SIGILL;
                info.si_errno = 0;
                switch (env->error_code & 0xF) {
                case POWERPC_EXCP_PRIV_OPC:
                    info.si_code = TARGET_ILL_PRVOPC;
                    break;
                case POWERPC_EXCP_PRIV_REG:
                    info.si_code = TARGET_ILL_PRVREG;
                    break;
                default:
                    fprintf(stderr, "Unknown privilege violation (%02x)\n",
                              env->error_code & 0xF);
                    info.si_code = TARGET_ILL_PRVOPC;
                    break;
                }
                break;
            case POWERPC_EXCP_TRAP:
                cpu_abort(cs, "Tried to call a TRAP\n");
                break;
            default:
                /* Should not happen ! */
                cpu_abort(cs, "Unknown program exception (%02x)\n",
                          env->error_code);
                break;
            }
            info.si_addr = env->nip - 4;
            queue_signal(env, info.si_signo, &info);
            break;
        case POWERPC_EXCP_FPU:      /* Floating-point unavailable exception  */
            env->msr |= (1 << MSR_FP);
            env->nip -= 4;
#if 0
            fprintf(stderr, "No floating point allowed\n");
            info.si_signo = TARGET_SIGILL;
            info.si_errno = 0;
            info.si_code = TARGET_ILL_COPROC;
            info.si_addr = env->nip - 4;
            queue_signal(env, info.si_signo, &info);
#endif
            break;
        case POWERPC_EXCP_SYSCALL:  /* System call exception                 */
            cpu_abort(cs, "Syscall exception while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_APU:      /* Auxiliary processor unavailable       */
            fprintf(stderr, "No APU instruction allowed\n");
            info.si_signo = TARGET_SIGILL;
            info.si_errno = 0;
            info.si_code = TARGET_ILL_COPROC;
            info.si_addr = env->nip - 4;
            queue_signal(env, info.si_signo, &info);
            break;
        case POWERPC_EXCP_DECR:     /* Decrementer exception                 */
            cpu_abort(cs, "Decrementer interrupt while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_FIT:      /* Fixed-interval timer interrupt        */
            cpu_abort(cs, "Fix interval timer interrupt while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_WDT:      /* Watchdog timer interrupt              */
            cpu_abort(cs, "Watchdog timer interrupt while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_DTLB:     /* Data TLB error                        */
            cpu_abort(cs, "Data TLB exception while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_ITLB:     /* Instruction TLB error                 */
            cpu_abort(cs, "Instruction TLB exception while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_SPEU:     /* SPE/embedded floating-point unavail.  */
            fprintf(stderr, "No SPE/floating-point instruction allowed\n");
            info.si_signo = TARGET_SIGILL;
            info.si_errno = 0;
            info.si_code = TARGET_ILL_COPROC;
            info.si_addr = env->nip - 4;
            queue_signal(env, info.si_signo, &info);
            break;
        case POWERPC_EXCP_EFPDI:    /* Embedded floating-point data IRQ      */
            cpu_abort(cs, "Embedded floating-point data IRQ not handled\n");
            break;
        case POWERPC_EXCP_EFPRI:    /* Embedded floating-point round IRQ     */
            cpu_abort(cs, "Embedded floating-point round IRQ not handled\n");
            break;
        case POWERPC_EXCP_EPERFM:   /* Embedded performance monitor IRQ      */
            cpu_abort(cs, "Performance monitor exception not handled\n");
            break;
        case POWERPC_EXCP_DOORI:    /* Embedded doorbell interrupt           */
            cpu_abort(cs, "Doorbell interrupt while in user mode. "
                       "Aborting\n");
            break;
        case POWERPC_EXCP_DOORCI:   /* Embedded doorbell critical interrupt  */
            cpu_abort(cs, "Doorbell critical interrupt while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_RESET:    /* System reset exception                */
            cpu_abort(cs, "Reset interrupt while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_DSEG:     /* Data segment exception                */
            cpu_abort(cs, "Data segment exception while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_ISEG:     /* Instruction segment exception         */
            cpu_abort(cs, "Instruction segment exception "
                      "while in user mode. Aborting\n");
            break;
        /* PowerPC 64 with hypervisor mode support */
        case POWERPC_EXCP_HDECR:    /* Hypervisor decrementer exception      */
            cpu_abort(cs, "Hypervisor decrementer interrupt "
                      "while in user mode. Aborting\n");
            break;
        case POWERPC_EXCP_TRACE:    /* Trace exception                       */
            /* Nothing to do:
             * we use this exception to emulate step-by-step execution mode.
             */
            break;
        /* PowerPC 64 with hypervisor mode support */
        case POWERPC_EXCP_HDSI:     /* Hypervisor data storage exception     */
            cpu_abort(cs, "Hypervisor data storage exception "
                      "while in user mode. Aborting\n");
            break;
        case POWERPC_EXCP_HISI:     /* Hypervisor instruction storage excp   */
            cpu_abort(cs, "Hypervisor instruction storage exception "
                      "while in user mode. Aborting\n");
            break;
        case POWERPC_EXCP_HDSEG:    /* Hypervisor data segment exception     */
            cpu_abort(cs, "Hypervisor data segment exception "
                      "while in user mode. Aborting\n");
            break;
        case POWERPC_EXCP_HISEG:    /* Hypervisor instruction segment excp   */
            cpu_abort(cs, "Hypervisor instruction segment exception "
                      "while in user mode. Aborting\n");
            break;
        case POWERPC_EXCP_VPU:      /* Vector unavailable exception          */
            env->msr |= (1 << MSR_VR);
            env->nip -= 4;
#if 0
            fprintf(stderr, "No Altivec instructions allowed\n");
            info.si_signo = TARGET_SIGILL;
            info.si_errno = 0;
            info.si_code = TARGET_ILL_COPROC;
            info.si_addr = env->nip - 4;
            queue_signal(env, info.si_signo, &info);
#endif
            break;
        case POWERPC_EXCP_PIT:      /* Programmable interval timer IRQ       */
            cpu_abort(cs, "Programmable interval timer interrupt "
                      "while in user mode. Aborting\n");
            break;
        case POWERPC_EXCP_IO:       /* IO error exception                    */
            cpu_abort(cs, "IO error exception while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_RUNM:     /* Run mode exception                    */
            cpu_abort(cs, "Run mode exception while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_EMUL:     /* Emulation trap exception              */
            cpu_abort(cs, "Emulation trap exception not handled\n");
            break;
        case POWERPC_EXCP_IFTLB:    /* Instruction fetch TLB error           */
            cpu_abort(cs, "Instruction fetch TLB exception "
                      "while in user-mode. Aborting");
            break;
        case POWERPC_EXCP_DLTLB:    /* Data load TLB miss                    */
            cpu_abort(cs, "Data load TLB exception while in user-mode. "
                      "Aborting");
            break;
        case POWERPC_EXCP_DSTLB:    /* Data store TLB miss                   */
            cpu_abort(cs, "Data store TLB exception while in user-mode. "
                      "Aborting");
            break;
        case POWERPC_EXCP_FPA:      /* Floating-point assist exception       */
            cpu_abort(cs, "Floating-point assist exception not handled\n");
            break;
        case POWERPC_EXCP_IABR:     /* Instruction address breakpoint        */
            cpu_abort(cs, "Instruction address breakpoint exception "
                      "not handled\n");
            break;
        case POWERPC_EXCP_SMI:      /* System management interrupt           */
            cpu_abort(cs, "System management interrupt while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_THERM:    /* Thermal interrupt                     */
            cpu_abort(cs, "Thermal interrupt interrupt while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_PERFM:   /* Embedded performance monitor IRQ      */
            cpu_abort(cs, "Performance monitor exception not handled\n");
            break;
        case POWERPC_EXCP_VPUA:     /* Vector assist exception               */
            cpu_abort(cs, "Vector assist exception not handled\n");
            break;
        case POWERPC_EXCP_SOFTP:    /* Soft patch exception                  */
            cpu_abort(cs, "Soft patch exception not handled\n");
            break;
        case POWERPC_EXCP_MAINT:    /* Maintenance exception                 */
            cpu_abort(cs, "Maintenance exception while in user mode. "
                      "Aborting\n");
            break;
        case POWERPC_EXCP_STOP:     /* stop translation                      */
            /* We did invalidate the instruction cache. Go on */
            break;
        case POWERPC_EXCP_BRANCH:   /* branch instruction:                   */
            /* We just stopped because of a branch. Go on */
            break;
        case POWERPC_EXCP_SYSCALL_USER:
            /* system call in user-mode emulation */
            /* WARNING:
             * PPC ABI uses overflow flag in cr0 to signal an error
             * in syscalls.
             */
            env->crf[0] &= ~0x1;
            ret = do_freebsd_syscall(env, env->gpr[0], env->gpr[3], env->gpr[4],
                             env->gpr[5], env->gpr[6], env->gpr[7],
                             env->gpr[8], env->gpr[9], env->gpr[10]);
            if (ret == (target_ulong)(-TARGET_QEMU_ESIGRETURN)) {
                /* Returning from a successful sigreturn syscall.
                   Avoid corrupting register state.  */
                break;
            } else if (ret == (target_ulong)(-TARGET_ERESTART)) {
                env->nip -= 4;
				break;
			}
            if (ret > (target_ulong)(-515)) {
                env->crf[0] |= 0x1;
                ret = -ret;
            }
            env->gpr[3] = ret;
            break;
        case POWERPC_EXCP_STCX:
            if (do_store_exclusive(env)) {
                info.si_signo = TARGET_SIGSEGV;
                info.si_errno = 0;
                info.si_code = TARGET_SEGV_MAPERR;
				info.si_addr = env->nip;
                queue_signal(env, info.si_signo, &info);
            }
            break;
        case EXCP_DEBUG:
            {
                int sig;

                sig = gdb_handlesig(cs, TARGET_SIGTRAP);
                if (sig) {
                    info.si_signo = sig;
                    info.si_errno = 0;
                    info.si_code = TARGET_TRAP_BRKPT;
                    queue_signal(env, info.si_signo, &info);
                  }
            }
            break;
        case EXCP_ATOMIC:
            cpu_exec_step_atomic(cs);
            break;
        case EXCP_INTERRUPT:
            /* just indicate that signals should be handled asap */
            break;
        default:
            cpu_abort(cs, "Unknown exception 0x%d. Aborting\n", trapnr);
            break;
        }
        process_pending_signals(env);
    }
}

static inline void target_cpu_clone_regs(CPUPPCState *env, target_ulong newsp)
{
    if (newsp)
        env->gpr[1] = newsp;
    env->gpr[3] = 0;
}

static inline void target_cpu_reset(CPUArchState *cpu)
{
    cpu_reset(ENV_GET_CPU(cpu));
}

#endif /* ! _TARGET_ARCH_CPU_H_ */
