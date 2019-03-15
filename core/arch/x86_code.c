/* **********************************************************
 * Copyright (c) 2013-2019 Google, Inc.  All rights reserved.
 * Copyright (c) 2001-2010 VMware, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* Copyright (c) 2003-2007 Determina Corp. */
/* Copyright (c) 2001-2003 Massachusetts Institute of Technology */
/* Copyright (c) 2001 Hewlett-Packard Company */

/*
 * x86_code.c - auxiliary C routines to assembly routines in x86.asm
 */
#include "../globals.h"
#include "../fragment.h"
#include "../dispatch.h"
#include "../monitor.h"
#include "arch.h"
#if defined(UNIX) && defined(X86)
#include <immintrin.h>
#endif

/* Helper routine for the x86.asm PUSH_DR_MCONTEXT, to fill in the xmm0-5 values
 * (or all for linux) (or ymm) only if necessary.
 */
void
get_xmm_vals(priv_mcontext_t *mc)
{
#ifdef X86
    if (preserve_xmm_caller_saved()) {
        ASSERT(proc_has_feature(FEATURE_SSE));
        if (YMM_ENABLED())
            get_ymm_caller_saved(&mc->ymm[0]);
        else
            get_xmm_caller_saved(&mc->ymm[0]);
    }
#elif defined(ARM)
    /* FIXME i#1551: no xmm but SIMD regs on ARM */
    ASSERT_NOT_REACHED();
#endif
}

/* Just calls dynamo_thread_under_dynamo.  We used to initialize dcontext here,
 * but that would end up initializing it twice.
 */
static void
thread_starting(dcontext_t *dcontext)
{
    ASSERT(dcontext->initialized);
    dynamo_thread_under_dynamo(dcontext);
}

/* Initializes a dcontext with the supplied state and calls d_r_dispatch */
void
dynamo_start(priv_mcontext_t *mc)
{
    priv_mcontext_t *mcontext;
    dcontext_t *dcontext = get_thread_private_dcontext();
    if (dcontext == NULL) {
        /* This may be an initialized thread that is currently native (which results
         * in a NULL dcontext via i#2089).
         */
        os_thread_re_take_over();
        dcontext = get_thread_private_dcontext();
    }
    if (dcontext == NULL) {
        /* If dr_app_start is called from a different thread than the one
         * that called dr_app_setup, we'll need to initialize this thread here.
         */
        dcontext = os_thread_take_over_secondary(mc);
        ASSERT(dcontext != NULL);
        ASSERT(dr_api_entry);
    }

    /* Signal other threads for take over.
     * This routine now calls dynamo_thread_under_dynamo() for this thread as well.
     */
    dynamorio_take_over_threads(dcontext);

    /* Set return address */
    mc->pc = canonicalize_pc_target(dcontext, mc->pc);
    dcontext->next_tag = mc->pc;
    ASSERT(dcontext->next_tag != NULL);

    /* transfer exec state to mcontext */
    mcontext = get_mcontext(dcontext);
    *mcontext = *mc;

    /* clear pc */
    mcontext->pc = 0;

    DOLOG(2, LOG_TOP, {
        byte *cur_esp;
        GET_STACK_PTR(cur_esp);
        LOG(THREAD, LOG_TOP, 2,
            "%s: next_tag=" PFX ", cur xsp=" PFX ", mc->xsp=" PFX "\n", __FUNCTION__,
            dcontext->next_tag, cur_esp, mc->xsp);
    });

    /* Swap stacks so d_r_dispatch is invoked outside the application. */
    call_switch_stack(dcontext, dcontext->dstack, (void (*)(void *))d_r_dispatch,
                      NULL /*not on d_r_initstack*/, true /*return on error*/);
    /* In release builds, this will simply return and continue native
     * execution.  That's better than calling unexpected_return() which
     * goes into an infinite loop.
     */
    ASSERT_NOT_REACHED();
}

/* auto_setup: called by dynamo_auto_start for non-early follow children.
 * This routine itself would be dynamo_auto_start except that we want
 * our own go-native path separate from load_dynamo (we could still have
 * this by dynamo_auto_start and jump to an asm routine for go-native,
 * but keeping the entry in asm is more flexible).
 * Assumptions: The saved priv_mcontext_t for the start of the app is on
 * the stack, followed by a pointer to a region of memory to free
 * (which can be NULL) and its size.  If we decide not to take over
 * this process, this routine returns; otherwise it does not return.
 */
void
auto_setup(ptr_uint_t appstack)
{
    dcontext_t *dcontext;
    priv_mcontext_t *mcontext;
    byte *pappstack;
    byte *addr;

    pappstack = (byte *)appstack;

    /* Our parameter points at a priv_mcontext_t struct, beyond which are
     * two other fields:
       pappstack --> +0  priv_mcontext_t struct
                     +x  addr of memory to free (can be NULL)
                     +y  sizeof memory to free
    */

    automatic_startup = true;
    /* we should control all threads */
    control_all_threads = true;
    dynamorio_app_init();
    if (INTERNAL_OPTION(nullcalls)) {
        dynamorio_app_exit();
        return;
    }
    LOG(GLOBAL, LOG_TOP, 1, "taking over via late injection in %s\n", __FUNCTION__);

    /* For apps injected using follow_children, this is where control should be
     * allowed to go native for hotp_only & thin_client.
     */
    if (RUNNING_WITHOUT_CODE_CACHE())
        return;

    /* useful to debug fork-following */
    DOLOG(4, LOG_TOP, { SYSLOG_INTERNAL_INFO("dynamo auto start"); });

    dcontext = get_thread_private_dcontext();
    ASSERT(dcontext);
#ifdef WINDOWS
    LOG(THREAD, LOG_INTERP, 2, "thread_starting: interpreting thread " TIDFMT "\n",
        d_r_get_thread_id());
#endif

    /* Despite what *should* happen, there can be other threads if a statically
     * imported lib created one in its DllMain (Cygwin does this), or if a
     * thread was injected from the outside.  We go ahead and check for and
     * take over any other threads at this time.  Xref i#1304.
     * This routine now calls dynamo_thread_under_dynamo() for this thread as well.
     * XXX i#1305: we should really suspend all these other threads for DR init.
     */
    dynamorio_take_over_threads(dcontext);

    /* copy over the app state into mcontext */
    mcontext = get_mcontext(dcontext);
    *mcontext = *((priv_mcontext_t *)pappstack);
    pappstack += sizeof(priv_mcontext_t);
    dcontext->next_tag = mcontext->pc;
    ASSERT(dcontext->next_tag != NULL);

    /* free memory */
    addr = (byte *)*((byte **)pappstack);
    pappstack += sizeof(byte *);
    if (addr != NULL) {
        size_t size = *((size_t *)pappstack);
        heap_error_code_t error_code;
        /* since this is rx it was added to our exec list, remove now
         * ASSUMPTION: no fragments in the region so no need to flush
         */
        /* flushing would align for us but we have to do it ourselves here */
        size_t alloc_size = ALIGN_FORWARD(size, PAGE_SIZE);
        DODEBUG({
            if (SHARED_FRAGMENTS_ENABLED())
                ASSERT(!thread_vm_area_overlap(GLOBAL_DCONTEXT, addr, addr + alloc_size));
        });
        ASSERT(!thread_vm_area_overlap(dcontext, addr, addr + alloc_size));
        remove_executable_region(addr, alloc_size, false /*do not have lock*/);
        os_heap_free(addr, size, &error_code);
    }

    /* FIXME : for transparency should we zero out the appstack where we
     * stored injection information? would be safe to do so here */

    LOG(THREAD, LOG_INTERP, 1, "DynamoRIO auto start at 0x%08x\n", dcontext->next_tag);
    DOLOG(LOG_INTERP, 2, { dump_mcontext(mcontext, THREAD, DUMP_NOT_XML); });

    /* We didn't swap the stack ptr at loader init b/c we were on the app stack
     * then.  We do so now.
     */
    IF_WINDOWS(os_swap_context(dcontext, false /*to priv*/, DR_STATE_STACK_BOUNDS));
    call_switch_stack(dcontext, dcontext->dstack, (void (*)(void *))d_r_dispatch,
                      NULL /*not on d_r_initstack*/, false /*shouldn't return*/);
    ASSERT_NOT_REACHED();
}

/* Get the retstack index from the app stack and reset the mcontext to the
 * original app state.  The retstub saved it like this in x86.asm:
 *   push $retidx
 *   jmp back_from_native
 * back_from_native:
 *   push mcontext
 *   call return_from_native(mc)
 */
int
native_get_retstack_idx(priv_mcontext_t *mc)
{
    int retidx = (int)*(ptr_int_t *)mc->xsp;
    mc->xsp += sizeof(void *); /* Undo the push. */
    return retidx;
}

/****************************************************************************/
#ifdef UNIX

/* Called by new_thread_dynamo_start to initialize the dcontext
 * structure for the current thread and start executing at the
 * the pc stored in the clone_record_t * stored at *mc->xsp.
 * Assumes that it is called on the dstack.
 *
 * CAUTION: don't add a lot of stack variables in this routine or call a lot
 *          of functions before get_clone_record() because get_clone_record()
 *          makes assumptions about the usage of stack being less than a page.
 */
void
new_thread_setup(priv_mcontext_t *mc)
{
    dcontext_t *dcontext;
    void *crec;
    int rc;
    /* this is where a new thread first touches other than the dstack,
     * so we "enter" DR here
     */
    ENTERING_DR();

    /* i#149/PR 403015: clone_record_t is passed via dstack. */
    crec = get_clone_record(mc->xsp);
    LOG(GLOBAL, LOG_INTERP, 1,
        "new_thread_setup: thread " TIDFMT ", dstack " PFX " clone record " PFX "\n",
        d_r_get_thread_id(), get_clone_record_dstack(crec), crec);

    /* As we used dstack as app thread stack to pass clone record, we now need
     * to switch back to the real app thread stack before continuing.
     */
    mc->xsp = get_clone_record_app_xsp(crec);
    /* clear xax/r0 (was used as scratch in gencode, and app expects 0) */
    mc->IF_X86_ELSE(xax, r0) = 0;
    /* clear pc */
    mc->pc = 0;
#    ifdef AARCHXX
    /* set the stolen register's app value */
    set_stolen_reg_val(mc, get_clone_record_stolen_value(crec));
    /* set the thread register if necessary */
    set_thread_register_from_clone_record(crec);
#    endif

    rc = dynamo_thread_init(get_clone_record_dstack(crec), mc,
                            crec _IF_CLIENT_INTERFACE(false));
    ASSERT(rc != -1); /* this better be a new thread */
    dcontext = get_thread_private_dcontext();
    ASSERT(dcontext != NULL);
#    ifdef AARCHXX
    set_app_lib_tls_base_from_clone_record(dcontext, crec);
#    endif
#    ifdef ARM
    dr_set_isa_mode(dcontext, get_clone_record_isa_mode(crec), NULL);
#    endif

    /* Restore the original stack parameter to the syscall, which we clobbered
     * in create_clone_record().  Some apps examine it post-syscall (i#3171).
     */
    restore_clone_param_from_clone_record(dcontext, crec);

    thread_starting(dcontext);

    call_switch_stack(dcontext, dcontext->dstack, (void (*)(void *))d_r_dispatch,
                      NULL /*not on d_r_initstack*/, false /*shouldn't return*/);
    ASSERT_NOT_REACHED();
}

#    ifdef MACOS
/* Called from new_bsdthread_intercept for targeting a bsd thread user function.
 * new_bsdthread_intercept stored the arg to the user thread func in
 * mc->xax.  We're on the app stack -- but this is a temporary solution.
 * i#1403 covers intercepting in an earlier and better manner.
 */
void
new_bsdthread_setup(priv_mcontext_t *mc)
{
    dcontext_t *dcontext;
    void *crec, *func_arg;
    int rc;
    /* this is where a new thread first touches other than the dstack,
     * so we "enter" DR here
     */
    ENTERING_DR();

    crec = (void *)mc->xax; /* placed there by new_bsdthread_intercept */
    func_arg = (void *)get_clone_record_thread_arg(crec);
    LOG(GLOBAL, LOG_INTERP, 1,
        "new_thread_setup: thread " TIDFMT ", dstack " PFX " clone record " PFX "\n",
        d_r_get_thread_id(), get_clone_record_dstack(crec), crec);

    rc = dynamo_thread_init(get_clone_record_dstack(crec), mc,
                            crec _IF_CLIENT_INTERFACE(false));
    ASSERT(rc != -1); /* this better be a new thread */
    dcontext = get_thread_private_dcontext();
    ASSERT(dcontext != NULL);
    crec = NULL; /* now freed */
    thread_starting(dcontext);

    /* We assume that the only state that matters is the arg to the function. */
#        ifdef X64
    mc->rdi = (reg_t)func_arg;
#        else
    *(reg_t *)(mc->xsp + sizeof(reg_t)) = (reg_t)func_arg;
#        endif

    call_switch_stack(dcontext, dcontext->dstack, (void (*)(void *))d_r_dispatch,
                      NULL /*not on d_r_initstack*/, false /*shouldn't return*/);
    ASSERT_NOT_REACHED();
}
#    endif /* MACOS */

#endif /* UNIX */

#ifdef WINDOWS

/* Called by nt_continue_dynamo_start when we're about to execute
 * the continuation of an exception or APC: after NtContinue.
 * next_pc is bogus, the real next pc has been stored in dcontext->next_tag.
 * This routine is also used by NtSetContextThread.
 */
void
nt_continue_setup(priv_mcontext_t *mc)
{
    app_pc next_pc;
    dcontext_t *dcontext;
    ENTERING_DR();
    dcontext = get_thread_private_dcontext();
    ASSERT(dcontext != NULL);
    SELF_PROTECT_LOCAL(dcontext, WRITABLE);
    /* save target in temp var during init of dcontext */
    /* we have to use a different slot since next_tag ends up holding the do_syscall
     * entry when entered from d_r_dispatch
     */
    if (dcontext->asynch_target != NULL)
        next_pc = dcontext->asynch_target;
    else {
        ASSERT(DYNAMO_OPTION(shared_syscalls));
        next_pc = dcontext->next_tag;
    }
    LOG(THREAD, LOG_ASYNCH, 2, "nt_continue_setup: target is " PFX "\n", next_pc);
    initialize_dynamo_context(dcontext);
    dcontext->next_tag = next_pc;
    ASSERT(dcontext->next_tag != NULL);
    set_last_exit(dcontext, (linkstub_t *)get_asynch_linkstub());
    dcontext->whereami = DR_WHERE_TRAMPOLINE;

    *get_mcontext(dcontext) = *mc;
    /* clear pc */
    get_mcontext(dcontext)->pc = 0;
    /* We came straight from fcache, so swap to priv now (i#25) */
    IF_WINDOWS(swap_peb_pointer(dcontext, true /*to priv*/));

    call_switch_stack(dcontext, dcontext->dstack, (void (*)(void *))d_r_dispatch,
                      NULL /*not on d_r_initstack*/, false /*shouldn't return*/);
    ASSERT_NOT_REACHED();
}

#endif /* WINDOWS */

/****************************************************************************/

/* C-level wrapper around the asm implementation.  Shuffles arguments and
 * increments stats.
 * We used to use try/except on Linux and NtReadVirtualMemory on Windows, but
 * this is faster than both.
 */
bool
safe_read_fast(const void *base, size_t size, void *out_buf, size_t *bytes_read)
{
    byte *stop_pc;
    size_t nbytes;
    stop_pc = safe_read_asm(out_buf, base, size);
    nbytes = stop_pc - (byte *)base;
    if (bytes_read != NULL)
        *bytes_read = nbytes;
    return (nbytes == size);
}

bool
is_safe_read_pc(app_pc pc)
{
    return (pc == (app_pc)safe_read_asm_pre || pc == (app_pc)safe_read_asm_mid ||
            pc == (app_pc)safe_read_asm_post);
}

app_pc
safe_read_resume_pc(void)
{
    return (app_pc)&safe_read_asm_recover;
}

#if defined(STANDALONE_UNIT_TEST)

#    define CONST_BYTE 0x1f
#    define TEST_STACK_SIZE 4096
/* Align stack to 16 bytes: sufficient for all current architectures. */
byte ALIGN_VAR(16) test_stack[TEST_STACK_SIZE];
static dcontext_t *static_dc;

static void
check_var(byte *var)
{
    EXPECT(*var, CONST_BYTE);
}

static void (*check_var_ptr)(byte *) = check_var;

static void
test_func(dcontext_t *dcontext)
{
    /* i#1577: we want to read the stack without bothering with a separate
     * assembly routine and without getting an uninit var warning from the
     * compiler.  We go through a separate function and avoid compiler analysis
     * of that function via an indirect call.
     */
    byte var;
    check_var_ptr(&var);
    EXPECT((ptr_uint_t)dcontext, (ptr_uint_t)static_dc);
    return;
}

static void
test_call_switch_stack(dcontext_t *dc)
{
    byte *stack_ptr = test_stack + TEST_STACK_SIZE;
    static_dc = dc;
    print_file(STDERR, "testing asm call_switch_stack\n");
    memset(test_stack, CONST_BYTE, sizeof(test_stack));
    call_switch_stack(dc, stack_ptr, (void (*)(void *))test_func, NULL,
                      true /* should return */);
}

static void
test_cpuid()
{
#    ifdef X86
    int cpuid_res[4] = { 0 };
#    endif
    print_file(STDERR, "testing asm cpuid\n");
    EXPECT(cpuid_supported(), IF_X86_ELSE(true, false));
#    ifdef X86
    our_cpuid(cpuid_res, 0, 0); /* get vendor id */
    /* cpuid_res[1..3] stores vendor info like "GenuineIntel" or "AuthenticAMD" for X86 */
    EXPECT_NE(cpuid_res[1], 0);
    EXPECT_NE(cpuid_res[2], 0);
    EXPECT_NE(cpuid_res[3], 0);
#    endif
}

#    ifdef __AVX__

#        if 0
static void __attribute__((noinline)) write_ymm_aux(dr_ymm_t *buffer, int regno)
{
}
#        endif

static void
unit_test_get_ymm_caller_saved()
{
    /* XXX i#1312: Once get_ymm_caller_saved(byte* buf) changes to reflect a dr_zmm_t type
     * of the SIMD field in DynamoRIO's mcontext, this needs to become dr_zmm_t.
     */
    dr_ymm_t ref_buffer[MCXT_NUM_SIMD_SLOTS];
    dr_ymm_t get_buffer[MCXT_NUM_SIMD_SLOTS];
    ASSERT(sizeof(dr_ymm_t) == YMM_REG_SIZE);
    uint base = 0x78abcdef;

    register __m256 ymm0 asm("ymm0");
    register __m256 ymm1 asm("ymm1");
    register __m256 ymm2 asm("ymm2");
    register __m256 ymm3 asm("ymm3");
    register __m256 ymm4 asm("ymm4");
    register __m256 ymm5 asm("ymm5");
    register __m256 ymm6 asm("ymm6");
    register __m256 ymm7 asm("ymm7");
#        ifdef X64
    register __m256 ymm8 asm("ymm8");
    register __m256 ymm9 asm("ymm9");
    register __m256 ymm10 asm("ymm10");
    register __m256 ymm11 asm("ymm11");
    register __m256 ymm12 asm("ymm12");
    register __m256 ymm13 asm("ymm13");
    register __m256 ymm14 asm("ymm14");
    register __m256 ymm15 asm("ymm15");
#        endif

    for (int regno = 0; regno < proc_num_simd_registers(); ++regno) {
        for (int dword = 0; dword < sizeof(dr_ymm_t) / sizeof(uint); ++dword) {
            ref_buffer[regno].u32[dword] = 0;
            get_buffer[regno].u32[dword] = 0;
        }
        for (int dword = 0; dword < sizeof(dr_ymm_t) / sizeof(uint); ++dword) {
            ref_buffer[regno].u32[dword] = base++;
        }
    }

#        define MAKE_YMM_REG(num) ymm##num
#        define MOVE_TO_YMM(buf, num) \
            asm volatile("vmovdqu %1, %0" : "=v"(MAKE_YMM_REG(num)) : "m"(buf[num]) :);

    MOVE_TO_YMM(ref_buffer, 0)
    MOVE_TO_YMM(ref_buffer, 1)
    MOVE_TO_YMM(ref_buffer, 2)
    MOVE_TO_YMM(ref_buffer, 3)
    MOVE_TO_YMM(ref_buffer, 4)
    MOVE_TO_YMM(ref_buffer, 5)
    MOVE_TO_YMM(ref_buffer, 6)
    MOVE_TO_YMM(ref_buffer, 7)
#        ifdef X64
    MOVE_TO_YMM(ref_buffer, 8)
    MOVE_TO_YMM(ref_buffer, 9)
    MOVE_TO_YMM(ref_buffer, 10)
    MOVE_TO_YMM(ref_buffer, 11)
    MOVE_TO_YMM(ref_buffer, 12)
    MOVE_TO_YMM(ref_buffer, 13)
    MOVE_TO_YMM(ref_buffer, 14)
    MOVE_TO_YMM(ref_buffer, 15)
#        endif

    get_ymm_caller_saved(get_buffer);

    /* Even though it was experimentally determined that it is not needed, this barrier
     * prevents the compiler from moving SSE code before the call above.
     */
    asm volatile("" ::: "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
#        ifdef X64
    asm volatile("" ::
                     : "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14",
                       "xmm15");
#        endif

    for (int regno = 0; regno < proc_num_simd_registers(); ++regno) {
        print_file(STDERR, "YMM%d ref\n:", regno);
        dump_buffer_as_bytes(STDERR, &ref_buffer[regno], sizeof(ref_buffer[regno]),
                             DUMP_RAW | DUMP_DWORD);
        print_file(STDERR, "\nYMM%d get\n:", regno);
        dump_buffer_as_bytes(STDERR, &get_buffer[regno], sizeof(get_buffer[regno]),
                             DUMP_RAW | DUMP_DWORD);
        print_file(STDERR, "\n");
    }
    EXPECT(
        memcmp(ref_buffer, get_buffer, proc_num_simd_registers() * MCXT_SIMD_SLOT_SIZE),
        0);
}

#    endif

#    ifdef __AVX512F__

static void
unit_test_get_zmm_caller_saved()
{
    /* XXX i#1312: get_zmm_caller_saved(byte* buf) assumes that there is enough
     * space in the buffer it's being passed. MCXT_NUM_SIMD_SLOTS does not yet
     * reflect this. Once this happens, the array size should become
     * MCXT_NUM_SIMD_SLOTS.
     */
    if (MCXT_NUM_SIMD_SLOTS == 32) {
        /* This is a just reminder.*/
        FAIL();
    }
    dr_zmm_t ref_buffer[32];
    dr_zmm_t get_buffer[32];
    ASSERT(sizeof(dr_zmm_t) == ZMM_REG_SIZE);
    uint base = 0x78abcdef;

    register __m512 zmm0 asm("zmm0");
    register __m512 zmm1 asm("zmm1");
    register __m512 zmm2 asm("zmm2");
    register __m512 zmm3 asm("zmm3");
    register __m512 zmm4 asm("zmm4");
    register __m512 zmm5 asm("zmm5");
    register __m512 zmm6 asm("zmm6");
    register __m512 zmm7 asm("zmm7");
#        ifdef X64
    register __m512 zmm8 asm("zmm8");
    register __m512 zmm9 asm("zmm9");
    register __m512 zmm10 asm("zmm10");
    register __m512 zmm11 asm("zmm11");
    register __m512 zmm12 asm("zmm12");
    register __m512 zmm13 asm("zmm13");
    register __m512 zmm14 asm("zmm14");
    register __m512 zmm15 asm("zmm15");
    register __m512 zmm16 asm("zmm16");
    register __m512 zmm17 asm("zmm17");
    register __m512 zmm18 asm("zmm18");
    register __m512 zmm19 asm("zmm19");
    register __m512 zmm20 asm("zmm20");
    register __m512 zmm21 asm("zmm21");
    register __m512 zmm22 asm("zmm22");
    register __m512 zmm23 asm("zmm23");
    register __m512 zmm24 asm("zmm24");
    register __m512 zmm25 asm("zmm25");
    register __m512 zmm26 asm("zmm26");
    register __m512 zmm27 asm("zmm27");
    register __m512 zmm28 asm("zmm28");
    register __m512 zmm29 asm("zmm29");
    register __m512 zmm30 asm("zmm30");
    register __m512 zmm31 asm("zmm31");
#        endif

    for (int regno = 0; regno < proc_num_simd_registers(); ++regno) {
        for (int dword = 0; dword < sizeof(dr_zmm_t) / sizeof(uint); ++dword) {
            ref_buffer[regno].u32[dword] = 0;
            get_buffer[regno].u32[dword] = 0;
        }
        base += regno;
        for (int dword = 0; dword < sizeof(dr_zmm_t) / sizeof(uint); ++dword) {
            ref_buffer[regno].u32[dword] = base + dword;
        }
    }

#        define MAKE_ZMM_REG(num) zmm##num
#        define MOVE_TO_ZMM(buf, num) \
            asm volatile("vmovdqu32 %1, %0" : "=v"(MAKE_ZMM_REG(num)) : "m"(buf[num]) :);

    MOVE_TO_ZMM(ref_buffer, 0)
    MOVE_TO_ZMM(ref_buffer, 1)
    MOVE_TO_ZMM(ref_buffer, 2)
    MOVE_TO_ZMM(ref_buffer, 3)
    MOVE_TO_ZMM(ref_buffer, 4)
    MOVE_TO_ZMM(ref_buffer, 5)
    MOVE_TO_ZMM(ref_buffer, 6)
    MOVE_TO_ZMM(ref_buffer, 7)
#        ifdef X64
    MOVE_TO_ZMM(ref_buffer, 8)
    MOVE_TO_ZMM(ref_buffer, 9)
    MOVE_TO_ZMM(ref_buffer, 10)
    MOVE_TO_ZMM(ref_buffer, 11)
    MOVE_TO_ZMM(ref_buffer, 12)
    MOVE_TO_ZMM(ref_buffer, 13)
    MOVE_TO_ZMM(ref_buffer, 14)
    MOVE_TO_ZMM(ref_buffer, 15)
    MOVE_TO_ZMM(ref_buffer, 16)
    MOVE_TO_ZMM(ref_buffer, 17)
    MOVE_TO_ZMM(ref_buffer, 18)
    MOVE_TO_ZMM(ref_buffer, 19)
    MOVE_TO_ZMM(ref_buffer, 20)
    MOVE_TO_ZMM(ref_buffer, 21)
    MOVE_TO_ZMM(ref_buffer, 22)
    MOVE_TO_ZMM(ref_buffer, 23)
    MOVE_TO_ZMM(ref_buffer, 24)
    MOVE_TO_ZMM(ref_buffer, 25)
    MOVE_TO_ZMM(ref_buffer, 26)
    MOVE_TO_ZMM(ref_buffer, 27)
    MOVE_TO_ZMM(ref_buffer, 28)
    MOVE_TO_ZMM(ref_buffer, 29)
    MOVE_TO_ZMM(ref_buffer, 30)
    MOVE_TO_ZMM(ref_buffer, 31)
#        endif

    get_zmm_caller_saved(get_buffer);

    /* Even though it was experimentally determined that it is not needed, this barrier
     * prevents the compiler from moving SSE code before the call above.
     */
    asm volatile("" ::: "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7");
#        ifdef X64
    asm volatile("" ::
                     : "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14",
                       "xmm15", "xmm16", "xmm17", "xmm18", "xmm19", "xmm20", "xmm21",
                       "xmm22", "xmm23", "xmm24", "xmm25", "xmm26", "xmm27", "xmm28",
                       "xmm29", "xmm30", "xmm31");
#        endif

    for (int regno = 0; regno < proc_num_simd_registers(); ++regno) {
        print_file(STDERR, "ZMM%d ref\n:", regno);
        dump_buffer_as_bytes(STDERR, &ref_buffer[regno], sizeof(ref_buffer[regno]),
                             DUMP_RAW | DUMP_DWORD);
        print_file(STDERR, "\nZMM%d get\n:", regno);
        dump_buffer_as_bytes(STDERR, &get_buffer[regno], sizeof(get_buffer[regno]),
                             DUMP_RAW | DUMP_DWORD);
        print_file(STDERR, "\n");
    }
    EXPECT(
        memcmp(ref_buffer, get_buffer, proc_num_simd_registers() * MCXT_SIMD_SLOT_SIZE),
        0);
}

#    endif

void
unit_test_asm(dcontext_t *dc)
{
    print_file(STDERR, "testing asm\n");
    test_call_switch_stack(dc);
    test_cpuid();
#    ifdef UNIX
#        ifdef __AVX__
    unit_test_get_ymm_caller_saved();
#        endif
#        ifdef __AVX512F__
    unit_test_get_zmm_caller_saved();
#        endif
#    endif
}

#endif /* STANDALONE_UNIT_TEST */
