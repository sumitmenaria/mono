/*
 * mini-ppc.c: PowerPC backend for the Mono code generator
 *
 * Authors:
 *   Paolo Molaro (lupus@ximian.com)
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Andreas Faerber <andreas.faerber@web.de>
 *
 * (C) 2003 Ximian, Inc.
 * (C) 2007-2008 Andreas Faerber
 */
#include "mini.h"
#include <string.h>

#include <mono/metadata/appdomain.h>
#include <mono/metadata/debug-helpers.h>

#include "mini-ppc.h"
#include "cpu-ppc64.h"
#include "trace.h"
#include "ir-emit.h"
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#define FORCE_INDIR_CALL 1

enum {
	TLS_MODE_DETECT,
	TLS_MODE_FAILED,
	TLS_MODE_LTHREADS,
	TLS_MODE_NPTL,
	TLS_MODE_DARWIN_G5
};

/* This mutex protects architecture specific caches */
#define mono_mini_arch_lock() EnterCriticalSection (&mini_arch_mutex)
#define mono_mini_arch_unlock() LeaveCriticalSection (&mini_arch_mutex)
static CRITICAL_SECTION mini_arch_mutex;

int mono_exc_esp_offset = 0;
static int tls_mode = TLS_MODE_DETECT;
static int lmf_pthread_key = -1;
static int monothread_key = -1;
static int monodomain_key = -1;

static int
offsets_from_pthread_key (guint32 key, int *offset2)
{
	int idx1 = key / 32;
	int idx2 = key % 32;
	*offset2 = idx2 * sizeof (gpointer);
	return 284 + idx1 * sizeof (gpointer);
}

#define emit_linuxthreads_tls(code,dreg,key) do {\
		int off1, off2;	\
		off1 = offsets_from_pthread_key ((key), &off2);	\
		ppc_load_reg ((code), (dreg), off1, ppc_r2);	\
		ppc_load_reg ((code), (dreg), off2, (dreg));	\
	} while (0);

#define emit_darwing5_tls(code,dreg,key) do {\
		int off1 = 0x48 + key * sizeof (gpointer);	\
		ppc_mfspr ((code), (dreg), 104);	\
		ppc_load_reg ((code), (dreg), off1, (dreg));	\
	} while (0);

#define emit_tls_access(code,dreg,key) do {	\
		switch (tls_mode) {	\
		case TLS_MODE_LTHREADS: emit_linuxthreads_tls(code,dreg,key); break;	\
		case TLS_MODE_DARWIN_G5: emit_darwing5_tls(code,dreg,key); break;	\
		default: g_assert_not_reached ();	\
		}	\
	} while (0)

#define MONO_EMIT_NEW_LOAD_R8(cfg,dr,addr) do { \
		MonoInst *inst;				   \
		MONO_INST_NEW ((cfg), (inst), OP_R8CONST); \
		inst->type = STACK_R8;			   \
		inst->dreg = (dr);		       \
		inst->inst_p0 = (void*)(addr);	       \
		mono_bblock_add_inst (cfg->cbb, inst); \
	} while (0)

const char*
mono_arch_regname (int reg) {
	static const char rnames[][4] = {
		"r0", "sp", "r2", "r3", "r4",
		"r5", "r6", "r7", "r8", "r9",
		"r10", "r11", "r12", "r13", "r14",
		"r15", "r16", "r17", "r18", "r19",
		"r20", "r21", "r22", "r23", "r24",
		"r25", "r26", "r27", "r28", "r29",
		"r30", "r31"
	};
	if (reg >= 0 && reg < 32)
		return rnames [reg];
	return "unknown";
}

const char*
mono_arch_fregname (int reg) {
	static const char rnames[][4] = {
		"f0", "f1", "f2", "f3", "f4",
		"f5", "f6", "f7", "f8", "f9",
		"f10", "f11", "f12", "f13", "f14",
		"f15", "f16", "f17", "f18", "f19",
		"f20", "f21", "f22", "f23", "f24",
		"f25", "f26", "f27", "f28", "f29",
		"f30", "f31"
	};
	if (reg >= 0 && reg < 32)
		return rnames [reg];
	return "unknown";
}

/* this function overwrites r0, r11, r12 */
static guint8*
emit_memcpy (guint8 *code, int size, int dreg, int doffset, int sreg, int soffset)
{
	/* unrolled, use the counter in big */
	if (size > sizeof (gpointer) * 5) {
		int shifted = size >> 3;
		guint8 *copy_loop_start, *copy_loop_jump;

		ppc_load (code, ppc_r0, shifted);
		ppc_mtctr (code, ppc_r0);
		g_assert (sreg == ppc_r11);
		ppc_addi (code, ppc_r12, dreg, (doffset - sizeof (gpointer)));
		ppc_addi (code, ppc_r11, sreg, (soffset - sizeof (gpointer)));
		copy_loop_start = code;
		ppc_load_reg_update (code, ppc_r0, ppc_r11, 8);
		ppc_store_reg_update (code, ppc_r0, 8, ppc_r12);
		copy_loop_jump = code;
		ppc_bc (code, PPC_BR_DEC_CTR_NONZERO, 0, 0);
		ppc_patch (copy_loop_jump, copy_loop_start);
		size -= shifted * 8;
		doffset = soffset = 0;
		dreg = ppc_r12;
	}
	while (size >= 8) {
		ppc_load_reg (code, ppc_r0, soffset, sreg);
		ppc_store_reg (code, ppc_r0, doffset, dreg);
		size -= 8;
		soffset += 8;
		doffset += 8;
	}
	while (size >= 4) {
		ppc_lwz (code, ppc_r0, soffset, sreg);
		ppc_stw (code, ppc_r0, doffset, dreg);
		size -= 4;
		soffset += 4;
		doffset += 4;
	}
	while (size >= 2) {
		ppc_lhz (code, ppc_r0, soffset, sreg);
		ppc_sth (code, ppc_r0, doffset, dreg);
		size -= 2;
		soffset += 2;
		doffset += 2;
	}
	while (size >= 1) {
		ppc_lbz (code, ppc_r0, soffset, sreg);
		ppc_stb (code, ppc_r0, doffset, dreg);
		size -= 1;
		soffset += 1;
		doffset += 1;
	}
	return code;
}

/*
 * mono_arch_get_argument_info:
 * @csig:  a method signature
 * @param_count: the number of parameters to consider
 * @arg_info: an array to store the result infos
 *
 * Gathers information on parameters such as size, alignment and
 * padding. arg_info should be large enought to hold param_count + 1 entries. 
 *
 * Returns the size of the activation frame.
 */
int
mono_arch_get_argument_info (MonoMethodSignature *csig, int param_count, MonoJitArgumentInfo *arg_info)
{
	g_assert_not_reached ();
	return -1;
}

static gboolean
is_load_sequence (guint32 *seq)
{
	return ppc_opcode (seq [0]) == 15 && /* lis */
		ppc_opcode (seq [1]) == 24 && /* ori */
		ppc_opcode (seq [2]) == 30 && /* sldi */
		ppc_opcode (seq [3]) == 25 && /* oris */
		ppc_opcode (seq [4]) == 24; /* ori */
}

/* code must point to the blrl */
gboolean
mono_ppc_is_direct_call_sequence (guint32 *code)
{
	g_assert(*code == 0x4e800021 || *code == 0x4e800020 || *code == 0x4e800420);

	/* the thunk-less direct call sequence: lis/ori/sldi/oris/ori/mtlr/blrl */
	if (ppc_opcode (code [-1]) == 31) { /* mtlr */
		if ((ppc_opcode (code [-2]) == 58 && ppc_opcode (code [-3]) == 58) || /* ld/ld */
		    (ppc_opcode (code [-2]) == 24 && ppc_opcode (code [-3]) == 31)) { /* mr/nop */
			if (is_load_sequence (&code [-8]))
				return TRUE;
		} else {
			if (is_load_sequence (&code [-6]))
				return TRUE;
		}
	}
	return FALSE;
}

gpointer
mono_arch_get_vcall_slot (guint8 *code_ptr, gpointer *regs, int *displacement)
{
	char *o = NULL;
	int reg, offset = 0;
	guint32* code = (guint32*)code_ptr;

	*displacement = 0;

	/* This is the 'blrl' instruction */
	--code;

	/* Sanity check: instruction must be 'blrl' */
	if (*code != 0x4e800021)
		return NULL;

	if (mono_ppc_is_direct_call_sequence (code))
		return NULL;

	/* FIXME: more sanity checks here */
	/* OK, we're now at the 'blrl' instruction. Now walk backwards
	till we get to a 'mtlr rA' */
	for (; --code;) {
		if((*code & 0x7c0803a6) == 0x7c0803a6) {
			gint16 soff;
			/* Here we are: we reached the 'mtlr rA'.
			Extract the register from the instruction */
			reg = (*code & 0x03e00000) >> 21;
			--code;
			/* ok, this is a lwz reg, offset (vtreg) 
			 * it is emitted with:
			 * ppc_emit32 (c, (32 << 26) | ((D) << 21) | ((a) << 16) | (guint16)(d))
			 */
			soff = (*code & 0xffff);
			offset = soff;
			reg = (*code >> 16) & 0x1f;
			g_assert (reg != ppc_r1);
			/*g_print ("patching reg is %d\n", reg);*/
			if (reg >= MONO_FIRST_SAVED_GREG) {
				MonoLMF *lmf = (MonoLMF*)((char*)regs + (MONO_FIRST_SAVED_FREG * sizeof (double)) + (MONO_FIRST_SAVED_GREG * sizeof (gulong)));
				/* saved in the MonoLMF structure */
				o = (gpointer)lmf->iregs [reg - MONO_FIRST_SAVED_GREG];
			} else {
				o = regs [reg];
			}
			break;
		}
	}
	*displacement = offset;
	return o;
}

gpointer*
mono_arch_get_vcall_slot_addr (guint8 *code, gpointer *regs)
{
	gpointer vt;
	int displacement;
	vt = mono_arch_get_vcall_slot (code, regs, &displacement);
	if (!vt)
		return NULL;
	return (gpointer*)((char*)vt + displacement);
}

#define MAX_ARCH_DELEGATE_PARAMS 7

gpointer
mono_arch_get_delegate_invoke_impl (MonoMethodSignature *sig, gboolean has_target)
{
	guint8 *code, *start;

	/* FIXME: Support more cases */
	if (MONO_TYPE_ISSTRUCT (sig->ret))
		return NULL;

	if (has_target) {
		static guint8* cached = NULL;
		mono_mini_arch_lock ();
		if (cached) {
			mono_mini_arch_unlock ();
			return cached;
		}
		
		start = code = mono_global_codeman_reserve (16);

		/* Replace the this argument with the target */
		ppc_load_reg (code, ppc_r0, G_STRUCT_OFFSET (MonoDelegate, method_ptr), ppc_r3);
		ppc_mtctr (code, ppc_r0);
		ppc_load_reg (code, ppc_r3, G_STRUCT_OFFSET (MonoDelegate, target), ppc_r3);
		/* FIXME: this might be a function descriptor */
		ppc_bcctr (code, PPC_BR_ALWAYS, 0);

		g_assert ((code - start) <= 16);

		mono_arch_flush_icache (start, 16);
		mono_ppc_emitted (start, 16, "delegate invoke target has_target 1");
		cached = start;
		mono_mini_arch_unlock ();
		return cached;
	} else {
		static guint8* cache [MAX_ARCH_DELEGATE_PARAMS + 1] = {NULL};
		int size, i;

		if (sig->param_count > MAX_ARCH_DELEGATE_PARAMS)
			return NULL;
		for (i = 0; i < sig->param_count; ++i)
			if (!mono_is_regsize_var (sig->params [i]))
				return NULL;

		mono_mini_arch_lock ();
		code = cache [sig->param_count];
		if (code) {
			mono_mini_arch_unlock ();
			return code;
		}

		size = 12 + sig->param_count * 4;
		start = code = mono_global_codeman_reserve (size);

		ppc_load_reg (code, ppc_r0, G_STRUCT_OFFSET (MonoDelegate, method_ptr), ppc_r3);
		ppc_mtctr (code, ppc_r0);
		/* slide down the arguments */
		for (i = 0; i < sig->param_count; ++i) {
			ppc_mr (code, (ppc_r3 + i), (ppc_r3 + i + 1));
		}
		/* FIXME: this might be a function descriptor */
		ppc_bcctr (code, PPC_BR_ALWAYS, 0);

		g_assert ((code - start) <= size);

		mono_arch_flush_icache (start, size);
		mono_ppc_emitted (start, size, "delegate invoke target has_target 0 params %d", sig->param_count);
		cache [sig->param_count] = start;
		mono_mini_arch_unlock ();
		return start;
	}
	return NULL;
}

gpointer
mono_arch_get_this_arg_from_call (MonoGenericSharingContext *gsctx, MonoMethodSignature *sig, gssize *regs, guint8 *code)
{
	/* FIXME: handle returning a struct */
	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		g_assert_not_reached ();
		return (gpointer)regs [ppc_r4];
	}
	return (gpointer)regs [ppc_r3];
}

/*
 * Initialize the cpu to execute managed code.
 */
void
mono_arch_cpu_init (void)
{
}

/*
 * Initialize architecture specific code.
 */
void
mono_arch_init (void)
{
	InitializeCriticalSection (&mini_arch_mutex);	
}

/*
 * Cleanup architecture specific code.
 */
void
mono_arch_cleanup (void)
{
	DeleteCriticalSection (&mini_arch_mutex);
}

/*
 * This function returns the optimizations supported on this cpu.
 */
guint32
mono_arch_cpu_optimizazions (guint32 *exclude_mask)
{
	guint32 opts = 0;

	/* no ppc-specific optimizations yet */
	*exclude_mask = 0;
	return opts;
}

static gboolean
is_regsize_var (MonoType *t) {
	if (t->byref)
		return TRUE;
	t = mini_type_get_underlying_type (NULL, t);
	switch (t->type) {
	case MONO_TYPE_I4:
	case MONO_TYPE_U4:
	case MONO_TYPE_I8:
	case MONO_TYPE_U8:
	case MONO_TYPE_I:
	case MONO_TYPE_U:
	case MONO_TYPE_PTR:
	case MONO_TYPE_FNPTR:
		return TRUE;
	case MONO_TYPE_OBJECT:
	case MONO_TYPE_STRING:
	case MONO_TYPE_CLASS:
	case MONO_TYPE_SZARRAY:
	case MONO_TYPE_ARRAY:
		return TRUE;
	case MONO_TYPE_GENERICINST:
		if (!mono_type_generic_inst_is_valuetype (t))
			return TRUE;
		return FALSE;
	case MONO_TYPE_VALUETYPE:
		return FALSE;
	}
	return FALSE;
}

GList *
mono_arch_get_allocatable_int_vars (MonoCompile *cfg)
{
	GList *vars = NULL;
	int i;

	for (i = 0; i < cfg->num_varinfo; i++) {
		MonoInst *ins = cfg->varinfo [i];
		MonoMethodVar *vmv = MONO_VARINFO (cfg, i);

		/* unused vars */
		if (vmv->range.first_use.abs_pos >= vmv->range.last_use.abs_pos)
			continue;

		if (ins->flags & (MONO_INST_VOLATILE|MONO_INST_INDIRECT) || (ins->opcode != OP_LOCAL && ins->opcode != OP_ARG))
			continue;

		/* we can only allocate 32 bit values */
		if (is_regsize_var (ins->inst_vtype)) {
			g_assert (MONO_VARINFO (cfg, i)->reg == -1);
			g_assert (i == vmv->idx);
			vars = mono_varlist_insert_sorted (cfg, vars, vmv, FALSE);
		}
	}

	return vars;
}

GList *
mono_arch_get_global_int_regs (MonoCompile *cfg)
{
	GList *regs = NULL;
	int i, top = 32;
	if (cfg->frame_reg != ppc_sp)
		top = 31;
	/* ppc_r13 is used by the system on PPC EABI */
	for (i = 14; i < top; ++i)
		regs = g_list_prepend (regs, GUINT_TO_POINTER (i));

	return regs;
}

/*
 * mono_arch_regalloc_cost:
 *
 *  Return the cost, in number of memory references, of the action of 
 * allocating the variable VMV into a register during global register
 * allocation.
 */
guint32
mono_arch_regalloc_cost (MonoCompile *cfg, MonoMethodVar *vmv)
{
	/* FIXME: */
	return 2;
}

typedef struct {
	long int type;
	long int value;
} AuxVec;

void
mono_arch_flush_icache (guint8 *code, gint size)
{
	register guint8 *p;
	guint8 *endp, *start;
	static int cachelinesize = 0;
	static int cachelineinc = 16;

	if (!cachelinesize) {
#ifdef __APPLE__
		int mib [3];
		size_t len;
		mib [0] = CTL_HW;
		mib [1] = HW_CACHELINE;
		len = sizeof (cachelinesize);
		if (sysctl(mib, 2, &cachelinesize, (size_t*)&len, NULL, 0) == -1) {
			perror ("sysctl");
			cachelinesize = 128;
		} else {
			cachelineinc = cachelinesize;
			/*g_print ("setting cl size to %d\n", cachelinesize);*/
		}
#elif defined(__linux__)
		/* sadly this will work only with 2.6 kernels... */
		FILE* f = fopen ("/proc/self/auxv", "rb");
		if (f) {
			AuxVec vec;
			while (fread (&vec, sizeof (vec), 1, f) == 1) {
				if (vec.type == 19) {
					cachelinesize = vec.value;
					break;
				}
			}
			fclose (f);
		}
		if (!cachelinesize)
			cachelinesize = 128;
#elif defined(G_COMPILER_CODEWARRIOR)
	cachelinesize = 32;
	cachelineinc = 32;
#else
#warning Need a way to get cache line size
		cachelinesize = 128;
#endif
	}
	p = start = code;
	endp = p + size;
	start = (guint8*)((gsize)start & ~(cachelinesize - 1));
	/* use dcbf for smp support, later optimize for UP, see pem._64bit.d20030611.pdf page 211 */
#if defined(G_COMPILER_CODEWARRIOR)
	if (1) {
		for (p = start; p < endp; p += cachelineinc) {
			asm { dcbf 0, p };
		}
	} else {
		for (p = start; p < endp; p += cachelineinc) {
			asm { dcbst 0, p };
		}
	}
	asm { sync };
	p = code;
	for (p = start; p < endp; p += cachelineinc) {
		asm {
			icbi 0, p
			sync
		}
	}
	asm {
		sync
		isync
	}
#else
	if (1) {
		for (p = start; p < endp; p += cachelineinc) {
			asm ("dcbf 0,%0;" : : "r"(p) : "memory");
		}
	} else {
		for (p = start; p < endp; p += cachelineinc) {
			asm ("dcbst 0,%0;" : : "r"(p) : "memory");
		}
	}
	asm ("sync");
	p = code;
	for (p = start; p < endp; p += cachelineinc) {
		asm ("icbi 0,%0; sync;" : : "r"(p) : "memory");
	}
	asm ("sync");
	asm ("isync");
#endif
}

void
mono_arch_flush_register_windows (void)
{
}

#ifdef __APPLE__
#define ALWAYS_ON_STACK(s) s
#define FP_ALSO_IN_REG(s) s
#else
#define ALWAYS_ON_STACK(s) s
#define FP_ALSO_IN_REG(s) s
#define ALIGN_DOUBLES
#endif

enum {
	RegTypeGeneral,
	RegTypeBase,
	RegTypeFP,
	RegTypeStructByVal,
	RegTypeStructByAddr
};

typedef struct {
	gint32  offset;
	guint32 vtsize; /* in param area */
	guint8  reg;
	guint8  regtype : 4; /* 0 general, 1 basereg, 2 floating point register, see RegType* */
	guint8  size    : 4; /* 1, 2, 4, 8, or regs used by RegTypeStructByVal */
} ArgInfo;

typedef struct {
	int nargs;
	guint32 stack_usage;
	guint32 struct_ret;
	ArgInfo ret;
	ArgInfo sig_cookie;
	ArgInfo args [1];
} CallInfo;

#define DEBUG(a)

static void inline
add_general (guint *gr, guint *stack_size, ArgInfo *ainfo, gboolean simple)
{
	g_assert (simple);

	if (*gr >= 3 + PPC_NUM_REG_ARGS) {
		ainfo->offset = PPC_STACK_PARAM_OFFSET + *stack_size;
		ainfo->reg = ppc_sp; /* in the caller */
		ainfo->regtype = RegTypeBase;
		*stack_size += sizeof (gpointer);
	} else {
		ALWAYS_ON_STACK (*stack_size += sizeof (gpointer));
		ainfo->reg = *gr;
	}
	(*gr) ++;
}

#if __APPLE__
static gboolean
has_only_a_r48_field (MonoClass *klass)
{
	gpointer iter;
	MonoClassField *f;
	gboolean have_field = FALSE;
	iter = NULL;
	while ((f = mono_class_get_fields (klass, &iter))) {
		if (!(f->type->attrs & FIELD_ATTRIBUTE_STATIC)) {
			if (have_field)
				return FALSE;
			if (!f->type->byref && (f->type->type == MONO_TYPE_R4 || f->type->type == MONO_TYPE_R8))
				have_field = TRUE;
			else
				return FALSE;
		}
	}
	return have_field;
}
#endif

static CallInfo*
calculate_sizes (MonoMethodSignature *sig, gboolean is_pinvoke)
{
	guint i, fr, gr;
	int n = sig->hasthis + sig->param_count;
	guint32 simpletype;
	guint32 stack_size = 0;
	CallInfo *cinfo = g_malloc0 (sizeof (CallInfo) + sizeof (ArgInfo) * n);

	fr = PPC_FIRST_FPARG_REG;
	gr = PPC_FIRST_ARG_REG;

	/* FIXME: handle returning a struct */
	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		add_general (&gr, &stack_size, &cinfo->ret, TRUE);
		cinfo->struct_ret = PPC_FIRST_ARG_REG;
	}

	n = 0;
	if (sig->hasthis) {
		add_general (&gr, &stack_size, cinfo->args + n, TRUE);
		n++;
	}
        DEBUG(printf("params: %d\n", sig->param_count));
	for (i = 0; i < sig->param_count; ++i) {
		if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG) && (i == sig->sentinelpos)) {
                        /* Prevent implicit arguments and sig_cookie from
			   being passed in registers */
                        gr = PPC_LAST_ARG_REG + 1;
			/* FIXME: don't we have to set fr, too? */
                        /* Emit the signature cookie just before the implicit arguments */
                        add_general (&gr, &stack_size, &cinfo->sig_cookie, TRUE);
                }
                DEBUG(printf("param %d: ", i));
		if (sig->params [i]->byref) {
                        DEBUG(printf("byref\n"));
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			n++;
			continue;
		}
		simpletype = mini_type_get_underlying_type (NULL, sig->params [i])->type;
		switch (simpletype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
			cinfo->args [n].size = 1;
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			n++;
			break;
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
			cinfo->args [n].size = 2;
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			n++;
			break;
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
			cinfo->args [n].size = 4;
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			n++;
			break;
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_FNPTR:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_STRING:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_ARRAY:
			cinfo->args [n].size = sizeof (gpointer);
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			n++;
			break;
		case MONO_TYPE_GENERICINST:
			if (!mono_type_generic_inst_is_valuetype (sig->params [i])) {
				cinfo->args [n].size = sizeof (gpointer);
				add_general (&gr, &stack_size, cinfo->args + n, TRUE);
				n++;
				break;
			}
			/* Fall through */
		case MONO_TYPE_VALUETYPE: {
			gint size;
			MonoClass *klass;
			klass = mono_class_from_mono_type (sig->params [i]);
			if (is_pinvoke)
			    size = mono_class_native_size (klass, NULL);
			else
			    size = mono_class_value_size (klass, NULL);
#if __APPLE__
			if ((size == 4 || size == 8) && has_only_a_r48_field (klass)) {
				cinfo->args [n].size = size;

				/* It was 7, now it is 8 in LinuxPPC */
				if (fr <= PPC_LAST_FPARG_REG) {
					cinfo->args [n].regtype = RegTypeFP;
					cinfo->args [n].reg = fr;
					fr ++;
					FP_ALSO_IN_REG (gr ++);
					if (size == 8)
						FP_ALSO_IN_REG (gr ++);
					ALWAYS_ON_STACK (stack_size += size);
				} else {
					cinfo->args [n].offset = PPC_STACK_PARAM_OFFSET + stack_size;
					cinfo->args [n].regtype = RegTypeBase;
					cinfo->args [n].reg = ppc_sp; /* in the caller*/
					stack_size += 8;
				}
				n++;
				break;
			}
#endif
			DEBUG(printf ("load %d bytes struct\n",
				      mono_class_native_size (sig->params [i]->data.klass, NULL)));
#if PPC_PASS_STRUCTS_BY_VALUE
			{
				int align_size = size;
				int nwords = 0;
				int rest = PPC_LAST_ARG_REG - gr + 1;
				int n_in_regs;
				align_size += (sizeof (gpointer) - 1);
				align_size &= ~(sizeof (gpointer) - 1);
				nwords = (align_size + sizeof (gpointer) -1 ) / sizeof (gpointer);
				n_in_regs = rest >= nwords? nwords: rest;
				cinfo->args [n].regtype = RegTypeStructByVal;
				if (gr > PPC_LAST_ARG_REG || (size >= 3 && size % 4 != 0)) {
					cinfo->args [n].size = 0;
					cinfo->args [n].vtsize = nwords;
				} else {
					cinfo->args [n].size = n_in_regs;
					cinfo->args [n].vtsize = nwords - n_in_regs;
					cinfo->args [n].reg = gr;
				}
				gr += n_in_regs;
				cinfo->args [n].offset = PPC_STACK_PARAM_OFFSET + stack_size;
				/*g_print ("offset for arg %d at %d\n", n, PPC_STACK_PARAM_OFFSET + stack_size);*/
				stack_size += nwords * sizeof (gpointer);
			}
#else
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			cinfo->args [n].regtype = RegTypeStructByAddr;
			cinfo->args [n].vtsize = size;
#endif
			n++;
			break;
		}
		case MONO_TYPE_TYPEDBYREF: {
			int size = sizeof (MonoTypedRef);
			/* keep in sync or merge with the valuetype case */
#if PPC_PASS_STRUCTS_BY_VALUE
			{
				int nwords = (size + sizeof (gpointer) -1 ) / sizeof (gpointer);
				cinfo->args [n].regtype = RegTypeStructByVal;
				if (gr <= PPC_LAST_ARG_REG) {
					int rest = PPC_LAST_ARG_REG - gr + 1;
					int n_in_regs = rest >= nwords? nwords: rest;
					cinfo->args [n].size = n_in_regs;
					cinfo->args [n].vtsize = nwords - n_in_regs;
					cinfo->args [n].reg = gr;
					gr += n_in_regs;
				} else {
					cinfo->args [n].size = 0;
					cinfo->args [n].vtsize = nwords;
				}
				cinfo->args [n].offset = PPC_STACK_PARAM_OFFSET + stack_size;
				/*g_print ("offset for arg %d at %d\n", n, PPC_STACK_PARAM_OFFSET + stack_size);*/
				stack_size += nwords * sizeof (gpointer);
			}
#else
			add_general (&gr, &stack_size, cinfo->args + n, TRUE);
			cinfo->args [n].regtype = RegTypeStructByAddr;
			cinfo->args [n].vtsize = size;
#endif
			n++;
			break;
		}
		case MONO_TYPE_U8:
		case MONO_TYPE_I8:
			cinfo->args [n].size = 8;
			add_general (&gr, &stack_size, cinfo->args + n, sizeof (gpointer) == 8);
			n++;
			break;
		case MONO_TYPE_R4:
			cinfo->args [n].size = 4;

			/* It was 7, now it is 8 in LinuxPPC */
			if (fr <= PPC_LAST_FPARG_REG) {
				cinfo->args [n].regtype = RegTypeFP;
				cinfo->args [n].reg = fr;
				fr ++;
				FP_ALSO_IN_REG (gr ++);
				ALWAYS_ON_STACK (stack_size += 4);
			} else {
				cinfo->args [n].offset = PPC_STACK_PARAM_OFFSET + stack_size;
				cinfo->args [n].regtype = RegTypeBase;
				cinfo->args [n].reg = ppc_sp; /* in the caller*/
				stack_size += 4;
			}
			n++;
			break;
		case MONO_TYPE_R8:
			cinfo->args [n].size = 8;
			/* It was 7, now it is 8 in LinuxPPC */
			if (fr <= PPC_LAST_FPARG_REG) {
				cinfo->args [n].regtype = RegTypeFP;
				cinfo->args [n].reg = fr;
				fr ++;
				FP_ALSO_IN_REG (gr += 2);
				ALWAYS_ON_STACK (stack_size += 8);
			} else {
				cinfo->args [n].offset = PPC_STACK_PARAM_OFFSET + stack_size;
				cinfo->args [n].regtype = RegTypeBase;
				cinfo->args [n].reg = ppc_sp; /* in the caller*/
				stack_size += 8;
			}
			n++;
			break;
		default:
			g_error ("Can't trampoline 0x%x", sig->params [i]->type);
		}
	}

	if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG) && (i == sig->sentinelpos)) {
		/* Prevent implicit arguments and sig_cookie from
		   being passed in registers */
		gr = PPC_LAST_ARG_REG + 1;
		/* Emit the signature cookie just before the implicit arguments */
		add_general (&gr, &stack_size, &cinfo->sig_cookie, TRUE);
	}

	{
		simpletype = mini_type_get_underlying_type (NULL, sig->ret)->type;
		switch (simpletype) {
		case MONO_TYPE_BOOLEAN:
		case MONO_TYPE_I1:
		case MONO_TYPE_U1:
		case MONO_TYPE_I2:
		case MONO_TYPE_U2:
		case MONO_TYPE_CHAR:
		case MONO_TYPE_I4:
		case MONO_TYPE_U4:
		case MONO_TYPE_I:
		case MONO_TYPE_U:
		case MONO_TYPE_PTR:
		case MONO_TYPE_FNPTR:
		case MONO_TYPE_CLASS:
		case MONO_TYPE_OBJECT:
		case MONO_TYPE_SZARRAY:
		case MONO_TYPE_ARRAY:
		case MONO_TYPE_STRING:
			cinfo->ret.reg = ppc_r3;
			break;
		case MONO_TYPE_U8:
		case MONO_TYPE_I8:
			cinfo->ret.reg = ppc_r3;
			break;
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			cinfo->ret.reg = ppc_f1;
			cinfo->ret.regtype = RegTypeFP;
			break;
		case MONO_TYPE_GENERICINST:
			if (!mono_type_generic_inst_is_valuetype (sig->ret)) {
				cinfo->ret.reg = ppc_r3;
				break;
			}
			break;
		case MONO_TYPE_VALUETYPE:
			break;
		case MONO_TYPE_TYPEDBYREF:
		case MONO_TYPE_VOID:
			break;
		default:
			g_error ("Can't handle as return value 0x%x", sig->ret->type);
		}
	}

	/* align stack size to 16 */
	DEBUG (printf ("      stack size: %d (%d)\n", (stack_size + 15) & ~15, stack_size));
	stack_size = (stack_size + 15) & ~15;

	cinfo->stack_usage = stack_size;
	return cinfo;
}

static void
allocate_tailcall_valuetype_addrs (MonoCompile *cfg)
{
#if !PPC_PASS_STRUCTS_BY_VALUE
	MonoMethodSignature *sig = mono_method_signature (cfg->method);
	int num_structs = 0;
	int i;

	if (!(cfg->flags & MONO_CFG_HAS_TAIL))
		return;

	for (i = 0; i < sig->param_count; ++i) {
		MonoType *type = mono_type_get_underlying_type (sig->params [i]);
		if (type->type == MONO_TYPE_VALUETYPE)
			num_structs++;
	}

	if (num_structs) {
		cfg->tailcall_valuetype_addrs =
			mono_mempool_alloc0 (cfg->mempool, sizeof (MonoInst*) * num_structs);
		for (i = 0; i < num_structs; ++i) {
			cfg->tailcall_valuetype_addrs [i] =
				mono_compile_create_var (cfg, &mono_defaults.int_class->byval_arg, OP_LOCAL);
			cfg->tailcall_valuetype_addrs [i]->flags |= MONO_INST_INDIRECT;
		}
	}
#endif
}

/*
 * Set var information according to the calling convention. ppc version.
 * The locals var stuff should most likely be split in another method.
 */
void
mono_arch_allocate_vars (MonoCompile *m)
{
	MonoMethodSignature *sig;
	MonoMethodHeader *header;
	MonoInst *inst;
	int i, offset, size, align, curinst;
	int frame_reg = ppc_sp;
	gint32 *offsets;
	guint32 locals_stack_size, locals_stack_align;

	allocate_tailcall_valuetype_addrs (m);

	m->flags |= MONO_CFG_HAS_SPILLUP;

	/* allow room for the vararg method args: void* and long/double */
	if (mono_jit_trace_calls != NULL && mono_trace_eval (m->method))
		m->param_area = MAX (m->param_area, sizeof (gpointer)*8);
	/* this is bug #60332: remove when #59509 is fixed, so no weird vararg 
	 * call convs needs to be handled this way.
	 */
	if (m->flags & MONO_CFG_HAS_VARARGS)
		m->param_area = MAX (m->param_area, sizeof (gpointer)*8);
	/* gtk-sharp and other broken code will dllimport vararg functions even with
	 * non-varargs signatures. Since there is little hope people will get this right
	 * we assume they won't.
	 */
	if (m->method->wrapper_type == MONO_WRAPPER_MANAGED_TO_NATIVE)
		m->param_area = MAX (m->param_area, sizeof (gpointer)*8);

	header = mono_method_get_header (m->method);

	/* 
	 * We use the frame register also for any method that has
	 * exception clauses. This way, when the handlers are called,
	 * the code will reference local variables using the frame reg instead of
	 * the stack pointer: if we had to restore the stack pointer, we'd
	 * corrupt the method frames that are already on the stack (since
	 * filters get called before stack unwinding happens) when the filter
	 * code would call any method (this also applies to finally etc.).
	 */ 
	if ((m->flags & MONO_CFG_HAS_ALLOCA) || header->num_clauses)
		frame_reg = ppc_r31;
	m->frame_reg = frame_reg;
	if (frame_reg != ppc_sp) {
		m->used_int_regs |= 1 << frame_reg;
	}

	sig = mono_method_signature (m->method);
	
	offset = 0;
	curinst = 0;
	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		m->ret->opcode = OP_REGVAR;
		m->ret->inst_c0 = m->ret->dreg = ppc_r3;
	} else {
		/* FIXME: handle long values? */
		switch (mini_type_get_underlying_type (m->generic_sharing_context, sig->ret)->type) {
		case MONO_TYPE_VOID:
			break;
		case MONO_TYPE_R4:
		case MONO_TYPE_R8:
			m->ret->opcode = OP_REGVAR;
			m->ret->inst_c0 = m->ret->dreg = ppc_f1;
			break;
		default:
			m->ret->opcode = OP_REGVAR;
			m->ret->inst_c0 = m->ret->dreg = ppc_r3;
			break;
		}
	}
	/* local vars are at a positive offset from the stack pointer */
	/* 
	 * also note that if the function uses alloca, we use ppc_r31
	 * to point at the local variables.
	 */
	offset = PPC_MINIMAL_STACK_SIZE; /* linkage area */
	/* align the offset to 16 bytes: not sure this is needed here  */
	//offset += 16 - 1;
	//offset &= ~(16 - 1);

	/* add parameter area size for called functions */
	offset += m->param_area;
	offset += 16 - 1;
	offset &= ~(16 - 1);

	/* allow room to save the return value */
	if (mono_jit_trace_calls != NULL && mono_trace_eval (m->method))
		offset += 8;

	/* the MonoLMF structure is stored just below the stack pointer */

#if 0
	/* this stuff should not be needed on ppc and the new jit,
	 * because a call on ppc to the handlers doesn't change the 
	 * stack pointer and the jist doesn't manipulate the stack pointer
	 * for operations involving valuetypes.
	 */
	/* reserve space to store the esp */
	offset += sizeof (gpointer);

	/* this is a global constant */
	mono_exc_esp_offset = offset;
#endif
	if (sig->call_convention == MONO_CALL_VARARG) {
                m->sig_cookie = PPC_STACK_PARAM_OFFSET;
        }

	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		offset += sizeof(gpointer) - 1;
		offset &= ~(sizeof(gpointer) - 1);

		m->vret_addr->opcode = OP_REGOFFSET;
		m->vret_addr->inst_basereg = frame_reg;
		m->vret_addr->inst_offset = offset;

		if (G_UNLIKELY (m->verbose_level > 1)) {
			printf ("vret_addr =");
			mono_print_ins (m->vret_addr);
		}

		offset += sizeof(gpointer);
		if (sig->call_convention == MONO_CALL_VARARG)
			m->sig_cookie += sizeof (gpointer);
	}

	offsets = mono_allocate_stack_slots_full (m, FALSE, &locals_stack_size, &locals_stack_align);
	if (locals_stack_align) {
		offset += (locals_stack_align - 1);
		offset &= ~(locals_stack_align - 1);
	}
	for (i = m->locals_start; i < m->num_varinfo; i++) {
		if (offsets [i] != -1) {
			MonoInst *inst = m->varinfo [i];
			inst->opcode = OP_REGOFFSET;
			inst->inst_basereg = frame_reg;
			inst->inst_offset = offset + offsets [i];
			/*
			g_print ("allocating local %d (%s) to %d\n",
				i, mono_type_get_name (inst->inst_vtype), inst->inst_offset);
			*/
		}
	}
	offset += locals_stack_size;

	curinst = 0;
	if (sig->hasthis) {
		inst = m->args [curinst];
		if (inst->opcode != OP_REGVAR) {
			inst->opcode = OP_REGOFFSET;
			inst->inst_basereg = frame_reg;
			offset += sizeof (gpointer) - 1;
			offset &= ~(sizeof (gpointer) - 1);
			inst->inst_offset = offset;
			offset += sizeof (gpointer);
			if (sig->call_convention == MONO_CALL_VARARG)
				m->sig_cookie += sizeof (gpointer);
		}
		curinst++;
	}

	for (i = 0; i < sig->param_count; ++i) {
		inst = m->args [curinst];
		if (inst->opcode != OP_REGVAR) {
			inst->opcode = OP_REGOFFSET;
			inst->inst_basereg = frame_reg;
			if (sig->pinvoke) {
				size = mono_type_native_stack_size (sig->params [i], (guint32*)&align);
				inst->backend.is_pinvoke = 1;
			} else {
				size = mono_type_size (sig->params [i], &align);
			}
			offset += align - 1;
			offset &= ~(align - 1);
			inst->inst_offset = offset;
			offset += size;
			if ((sig->call_convention == MONO_CALL_VARARG) && (i < sig->sentinelpos)) 
				m->sig_cookie += size;
		}
		curinst++;
	}

	/* some storage for fp conversions */
	offset += 8 - 1;
	offset &= ~(8 - 1);
	m->arch.fp_conv_var_offset = offset;
	offset += 8;

	/* align the offset to 16 bytes */
	offset += 16 - 1;
	offset &= ~(16 - 1);

	/* change sign? */
	m->stack_offset = offset;

	if (sig->call_convention == MONO_CALL_VARARG) {
		CallInfo *cinfo = calculate_sizes (m->method->signature, m->method->signature->pinvoke);

		m->sig_cookie = cinfo->sig_cookie.offset;

		g_free(cinfo);
	}
}

void
mono_arch_create_vars (MonoCompile *cfg)
{
	MonoMethodSignature *sig = mono_method_signature (cfg->method);

	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		cfg->vret_addr = mono_compile_create_var (cfg, &mono_defaults.int_class->byval_arg, OP_ARG);
	}
}

static void
emit_sig_cookie (MonoCompile *cfg, MonoCallInst *call, CallInfo *cinfo)
{
	int sig_reg = mono_alloc_ireg (cfg);

	MONO_EMIT_NEW_ICONST (cfg, sig_reg, (gulong)call->signature);
	MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORE_MEMBASE_REG,
			ppc_r1, cinfo->sig_cookie.offset, sig_reg);
}

void
mono_arch_emit_call (MonoCompile *cfg, MonoCallInst *call)
{
	MonoInst *in, *ins;
	MonoMethodSignature *sig;
	int i, n;
	CallInfo *cinfo;

	sig = call->signature;
	n = sig->param_count + sig->hasthis;
	
	cinfo = calculate_sizes (sig, sig->pinvoke);

	for (i = 0; i < n; ++i) {
		ArgInfo *ainfo = cinfo->args + i;
		MonoType *t;

		if (i >= sig->hasthis)
			t = sig->params [i - sig->hasthis];
		else
			t = &mono_defaults.int_class->byval_arg;
		t = mini_type_get_underlying_type (cfg->generic_sharing_context, t);

		if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG) && (i == sig->sentinelpos))
			emit_sig_cookie (cfg, call, cinfo);

		in = call->args [i];

		if (ainfo->regtype == RegTypeGeneral) {
			MONO_INST_NEW (cfg, ins, OP_MOVE);
			ins->dreg = mono_alloc_ireg (cfg);
			ins->sreg1 = in->dreg;
			MONO_ADD_INS (cfg->cbb, ins);

			mono_call_inst_add_outarg_reg (cfg, call, ins->dreg, ainfo->reg, FALSE);
		} else if (ainfo->regtype == RegTypeStructByAddr) {
			MONO_INST_NEW (cfg, ins, OP_OUTARG_VT);
			ins->opcode = OP_OUTARG_VT;
			ins->sreg1 = in->dreg;
			ins->klass = in->klass;
			ins->inst_p0 = call;
			ins->inst_p1 = mono_mempool_alloc (cfg->mempool, sizeof (ArgInfo));
			memcpy (ins->inst_p1, ainfo, sizeof (ArgInfo));
			MONO_ADD_INS (cfg->cbb, ins);
		} else if (ainfo->regtype == RegTypeStructByVal) {
			/* this is further handled in mono_arch_emit_outarg_vt () */
			MONO_INST_NEW (cfg, ins, OP_OUTARG_VT);
			ins->opcode = OP_OUTARG_VT;
			ins->sreg1 = in->dreg;
			ins->klass = in->klass;
			ins->inst_p0 = call;
			ins->inst_p1 = mono_mempool_alloc (cfg->mempool, sizeof (ArgInfo));
			memcpy (ins->inst_p1, ainfo, sizeof (ArgInfo));
			MONO_ADD_INS (cfg->cbb, ins);
		} else if (ainfo->regtype == RegTypeBase) {
			if (!t->byref && ((t->type == MONO_TYPE_I8) || (t->type == MONO_TYPE_U8))) {
				MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI8_MEMBASE_REG, ppc_r1, ainfo->offset, in->dreg);
			} else if (!t->byref && ((t->type == MONO_TYPE_R4) || (t->type == MONO_TYPE_R8))) {
				if (t->type == MONO_TYPE_R8)
					MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORER8_MEMBASE_REG, ppc_r1, ainfo->offset, in->dreg);
				else
					MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORER4_MEMBASE_REG, ppc_r1, ainfo->offset, in->dreg);
			} else {
				MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORE_MEMBASE_REG, ppc_r1, ainfo->offset, in->dreg);
			}
		} else if (ainfo->regtype == RegTypeFP) {
			if (t->type == MONO_TYPE_VALUETYPE) {
				/* this is further handled in mono_arch_emit_outarg_vt () */
				MONO_INST_NEW (cfg, ins, OP_OUTARG_VT);
				ins->opcode = OP_OUTARG_VT;
				ins->sreg1 = in->dreg;
				ins->klass = in->klass;
				ins->inst_p0 = call;
				ins->inst_p1 = mono_mempool_alloc (cfg->mempool, sizeof (ArgInfo));
				memcpy (ins->inst_p1, ainfo, sizeof (ArgInfo));
				MONO_ADD_INS (cfg->cbb, ins);

				cfg->flags |= MONO_CFG_HAS_FPOUT;
			} else {
				int dreg = mono_alloc_freg (cfg);

				if (ainfo->size == 4) {
					MONO_EMIT_NEW_UNALU (cfg, OP_FCONV_TO_R4, dreg, in->dreg);
				} else {
					MONO_INST_NEW (cfg, ins, OP_FMOVE);
					ins->dreg = dreg;
					ins->sreg1 = in->dreg;
					MONO_ADD_INS (cfg->cbb, ins);
				}

				mono_call_inst_add_outarg_reg (cfg, call, dreg, ainfo->reg, TRUE);
				cfg->flags |= MONO_CFG_HAS_FPOUT;
			}
		} else {
			g_assert_not_reached ();
		}
	}

	/* Emit the signature cookie in the case that there is no
	   additional argument */
	if (!sig->pinvoke && (sig->call_convention == MONO_CALL_VARARG) && (n == sig->sentinelpos))
		emit_sig_cookie (cfg, call, cinfo);

	if (cinfo->struct_ret) {
		MonoInst *vtarg;

		MONO_INST_NEW (cfg, vtarg, OP_MOVE);
		vtarg->sreg1 = call->vret_var->dreg;
		vtarg->dreg = mono_alloc_preg (cfg);
		MONO_ADD_INS (cfg->cbb, vtarg);

		mono_call_inst_add_outarg_reg (cfg, call, vtarg->dreg, cinfo->struct_ret, FALSE);
	}

	call->stack_usage = cinfo->stack_usage;
	cfg->param_area = MAX (PPC_MINIMAL_PARAM_AREA_SIZE, MAX (cfg->param_area, cinfo->stack_usage));
	cfg->flags |= MONO_CFG_HAS_CALLS;

	g_free (cinfo);
}

void
mono_arch_emit_outarg_vt (MonoCompile *cfg, MonoInst *ins, MonoInst *src)
{
	MonoCallInst *call = (MonoCallInst*)ins->inst_p0;
	ArgInfo *ainfo = ins->inst_p1;
	int ovf_size = ainfo->vtsize;
	int doffset = ainfo->offset;
	int i, soffset, dreg;

	if (ainfo->regtype == RegTypeStructByVal) {
		guint32 size = 0;
		soffset = 0;
#ifdef __APPLE__
		/*
		 * Darwin pinvokes needs some special handling for 1
		 * and 2 byte arguments
		 */
		g_assert (ins->klass);
		if (call->signature->pinvoke)
			size =  mono_class_native_size (ins->klass, NULL);
		if (size == 2 || size == 1) {
			int tmpr = mono_alloc_ireg (cfg);
			if (size == 1)
				MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI1_MEMBASE, tmpr, src->dreg, soffset);
			else
				MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI2_MEMBASE, tmpr, src->dreg, soffset);
			dreg = mono_alloc_ireg (cfg);
			MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, dreg, tmpr);
			mono_call_inst_add_outarg_reg (cfg, call, dreg, ainfo->reg, FALSE);
		} else
#endif
			for (i = 0; i < ainfo->size; ++i) {
				dreg = mono_alloc_ireg (cfg);
				MONO_EMIT_NEW_LOAD_MEMBASE (cfg, dreg, src->dreg, soffset);
				mono_call_inst_add_outarg_reg (cfg, call, dreg, ainfo->reg + i, FALSE);
				soffset += sizeof (gpointer);
			}
		if (ovf_size != 0)
			mini_emit_memcpy (cfg, ppc_r1, doffset + soffset, src->dreg, soffset, ovf_size * sizeof (gpointer), 0);
	} else if (ainfo->regtype == RegTypeFP) {
		int tmpr = mono_alloc_freg (cfg);
		if (ainfo->size == 4)
			MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADR4_MEMBASE, tmpr, src->dreg, 0);
		else
			MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADR8_MEMBASE, tmpr, src->dreg, 0);
		dreg = mono_alloc_freg (cfg);
		MONO_EMIT_NEW_UNALU (cfg, OP_FMOVE, dreg, tmpr);
		mono_call_inst_add_outarg_reg (cfg, call, dreg, ainfo->reg, TRUE);
	} else {
		MonoInst *vtcopy = mono_compile_create_var (cfg, &src->klass->byval_arg, OP_LOCAL);
		MonoInst *load;
		guint32 size;

		/* FIXME: alignment? */
		if (call->signature->pinvoke) {
			size = mono_type_native_stack_size (&src->klass->byval_arg, NULL);
			vtcopy->backend.is_pinvoke = 1;
		} else {
			size = mini_type_stack_size (cfg->generic_sharing_context, &src->klass->byval_arg, NULL);
		}
		if (size > 0)
			g_assert (ovf_size > 0);

		EMIT_NEW_VARLOADA (cfg, load, vtcopy, vtcopy->inst_vtype);
		mini_emit_memcpy (cfg, load->dreg, 0, src->dreg, 0, size, 0);

		if (ainfo->offset)
			MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORE_MEMBASE_REG, ppc_r1, ainfo->offset, load->dreg);
		else
			mono_call_inst_add_outarg_reg (cfg, call, load->dreg, ainfo->reg, FALSE);
	}
}

void
mono_arch_emit_setret (MonoCompile *cfg, MonoMethod *method, MonoInst *val)
{
	MonoType *ret = mini_type_get_underlying_type (cfg->generic_sharing_context,
			mono_method_signature (method)->ret);

	if (!ret->byref) {
		if (ret->type == MONO_TYPE_R8 || ret->type == MONO_TYPE_R4) {
			MONO_EMIT_NEW_UNALU (cfg, OP_FMOVE, cfg->ret->dreg, val->dreg);
			return;
		}
	}
	MONO_EMIT_NEW_UNALU (cfg, OP_MOVE, cfg->ret->dreg, val->dreg);
}

/* FIXME: this is just a useless hint: fix the interface to include the opcode */
gboolean
mono_arch_is_inst_imm (gint64 imm)
{
       return TRUE;
}

/*
 * Allow tracing to work with this interface (with an optional argument)
 */

void*
mono_arch_instrument_prolog (MonoCompile *cfg, void *func, void *p, gboolean enable_arguments)
{
	guchar *code = p;

	ppc_load (code, ppc_r3, cfg->method);
	ppc_li (code, ppc_r4, 0); /* NULL ebp for now */
	ppc_load_func (code, ppc_r0, func);
	ppc_mtlr (code, ppc_r0);
	ppc_blrl (code);
	return code;
}

enum {
	SAVE_NONE,
	SAVE_STRUCT,
	SAVE_ONE,
	SAVE_FP
};

void*
mono_arch_instrument_epilog (MonoCompile *cfg, void *func, void *p, gboolean enable_arguments)
{
	guchar *code = p;
	int save_mode = SAVE_NONE;
	int offset;
	MonoMethod *method = cfg->method;
	int rtype = mini_type_get_underlying_type (cfg->generic_sharing_context,
			mono_method_signature (method)->ret)->type;
	int save_offset = PPC_STACK_PARAM_OFFSET + cfg->param_area;
	save_offset += 15;
	save_offset &= ~15;
	
	offset = code - cfg->native_code;
	/* we need about 16 instructions */
	if (offset > (cfg->code_size - 16 * 4)) {
		cfg->code_size *= 2;
		cfg->native_code = g_realloc (cfg->native_code, cfg->code_size);
		code = cfg->native_code + offset;
	}

	switch (rtype) {
	case MONO_TYPE_VOID:
		/* special case string .ctor icall */
		if (strcmp (".ctor", method->name) && method->klass == mono_defaults.string_class)
			save_mode = SAVE_ONE;
		else
			save_mode = SAVE_NONE;
		break;
	case MONO_TYPE_R4:
	case MONO_TYPE_R8:
		save_mode = SAVE_FP;
		break;
	case MONO_TYPE_VALUETYPE:
		save_mode = SAVE_STRUCT;
		break;
	default:
		save_mode = SAVE_ONE;
		break;
	}

	switch (save_mode) {
	case SAVE_ONE:
		ppc_store_reg (code, ppc_r3, save_offset, cfg->frame_reg);
		if (enable_arguments) {
			ppc_mr (code, ppc_r4, ppc_r3);
		}
		break;
	case SAVE_FP:
		ppc_stfd (code, ppc_f1, save_offset, cfg->frame_reg);
		if (enable_arguments) {
			/* FIXME: what reg?  */
			ppc_fmr (code, ppc_f3, ppc_f1);
			/* FIXME: use 8 byte load */
			ppc_lwz (code, ppc_r4, save_offset, cfg->frame_reg);
			ppc_lwz (code, ppc_r5, save_offset + 4, cfg->frame_reg);
		}
		break;
	case SAVE_STRUCT:
		if (enable_arguments) {
			/* FIXME: get the actual address  */
			ppc_mr (code, ppc_r4, ppc_r3);
		}
		break;
	case SAVE_NONE:
	default:
		break;
	}

	ppc_load (code, ppc_r3, cfg->method);
	ppc_load_func (code, ppc_r0, func);
	ppc_mtlr (code, ppc_r0);
	ppc_blrl (code);

	switch (save_mode) {
	case SAVE_ONE:
		ppc_load_reg (code, ppc_r3, save_offset, cfg->frame_reg);
		break;
	case SAVE_FP:
		ppc_lfd (code, ppc_f1, save_offset, cfg->frame_reg);
		break;
	case SAVE_NONE:
	default:
		break;
	}

	return code;
}
/*
 * Conditional branches have a small offset, so if it is likely overflowed,
 * we do a branch to the end of the method (uncond branches have much larger
 * offsets) where we perform the conditional and jump back unconditionally.
 * It's slightly slower, since we add two uncond branches, but it's very simple
 * with the current patch implementation and such large methods are likely not
 * going to be perf critical anyway.
 */
typedef struct {
	union {
		MonoBasicBlock *bb;
		const char *exception;
	} data;
	guint32 ip_offset;
	guint16 b0_cond;
	guint16 b1_cond;
} MonoOvfJump;

#define EMIT_COND_BRANCH_FLAGS(ins,b0,b1) \
if (ins->flags & MONO_INST_BRLABEL) { \
        if (0 && ins->inst_i0->inst_c0) { \
		ppc_bc (code, (b0), (b1), (code - cfg->native_code + ins->inst_i0->inst_c0) & 0xffff);	\
        } else { \
	        mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_LABEL, ins->inst_i0); \
		ppc_bc (code, (b0), (b1), 0);	\
        } \
} else { \
        if (0 && ins->inst_true_bb->native_offset) { \
		ppc_bc (code, (b0), (b1), (code - cfg->native_code + ins->inst_true_bb->native_offset) & 0xffff); \
        } else { \
		int br_disp = ins->inst_true_bb->max_offset - offset;	\
		if (!ppc_is_imm16 (br_disp + 1024) || ! ppc_is_imm16 (ppc_is_imm16 (br_disp - 1024))) {	\
			MonoOvfJump *ovfj = mono_mempool_alloc (cfg->mempool, sizeof (MonoOvfJump));	\
			ovfj->data.bb = ins->inst_true_bb;	\
			ovfj->ip_offset = 0;	\
			ovfj->b0_cond = (b0);	\
			ovfj->b1_cond = (b1);	\
		        mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_BB_OVF, ovfj); \
			ppc_b (code, 0);	\
		} else {	\
		        mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_BB, ins->inst_true_bb); \
			ppc_bc (code, (b0), (b1), 0);	\
		}	\
        } \
}

#define EMIT_COND_BRANCH(ins,cond) EMIT_COND_BRANCH_FLAGS(ins, branch_b0_table [(cond)], branch_b1_table [(cond)])

/* emit an exception if condition is fail
 *
 * We assign the extra code used to throw the implicit exceptions
 * to cfg->bb_exit as far as the big branch handling is concerned
 */
#define EMIT_COND_SYSTEM_EXCEPTION_FLAGS(b0,b1,exc_name)            \
        do {                                                        \
		int br_disp = cfg->bb_exit->max_offset - offset;	\
		if (!ppc_is_imm16 (br_disp + 1024) || ! ppc_is_imm16 (ppc_is_imm16 (br_disp - 1024))) {	\
			MonoOvfJump *ovfj = mono_mempool_alloc (cfg->mempool, sizeof (MonoOvfJump));	\
			ovfj->data.exception = (exc_name);	\
			ovfj->ip_offset = code - cfg->native_code;	\
			ovfj->b0_cond = (b0);	\
			ovfj->b1_cond = (b1);	\
		        mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_EXC_OVF, ovfj); \
			ppc_bl (code, 0);	\
			cfg->bb_exit->max_offset += 24;	\
		} else {	\
			mono_add_patch_info (cfg, code - cfg->native_code,   \
				    MONO_PATCH_INFO_EXC, exc_name);  \
			ppc_bcl (code, (b0), (b1), 0);	\
		}	\
	} while (0); 

#define EMIT_COND_SYSTEM_EXCEPTION(cond,exc_name) EMIT_COND_SYSTEM_EXCEPTION_FLAGS(branch_b0_table [(cond)], branch_b1_table [(cond)], (exc_name))

void
mono_arch_peephole_pass_1 (MonoCompile *cfg, MonoBasicBlock *bb)
{
}

void
mono_arch_peephole_pass_2 (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins, *n, *last_ins = NULL;

	MONO_BB_FOR_EACH_INS_SAFE (bb, n, ins) {
		switch (ins->opcode) {
		case OP_MUL_IMM: 
			/* remove unnecessary multiplication with 1 */
			if (ins->inst_imm == 1) {
				if (ins->dreg != ins->sreg1) {
					ins->opcode = OP_MOVE;
				} else {
					MONO_DELETE_INS (bb, ins);
					continue;
				}
			} else {
				int power2 = mono_is_power_of_two (ins->inst_imm);
				if (power2 > 0) {
					ins->opcode = OP_SHL_IMM;
					ins->inst_imm = power2;
				}
			}
			break;
		case OP_LOAD_MEMBASE:
		case OP_LOADI4_MEMBASE:
			/* 
			 * OP_STORE_MEMBASE_REG reg, offset(basereg) 
			 * OP_LOAD_MEMBASE offset(basereg), reg
			 */
			if (last_ins && (last_ins->opcode == OP_STOREI4_MEMBASE_REG 
					 || last_ins->opcode == OP_STORE_MEMBASE_REG) &&
			    ins->inst_basereg == last_ins->inst_destbasereg &&
			    ins->inst_offset == last_ins->inst_offset) {
				if (ins->dreg == last_ins->sreg1) {
					MONO_DELETE_INS (bb, ins);
					continue;
				} else {
					//static int c = 0; printf ("MATCHX %s %d\n", cfg->method->name,c++);
					ins->opcode = OP_MOVE;
					ins->sreg1 = last_ins->sreg1;
				}

			/* 
			 * Note: reg1 must be different from the basereg in the second load
			 * OP_LOAD_MEMBASE offset(basereg), reg1
			 * OP_LOAD_MEMBASE offset(basereg), reg2
			 * -->
			 * OP_LOAD_MEMBASE offset(basereg), reg1
			 * OP_MOVE reg1, reg2
			 */
			} else if (last_ins && (last_ins->opcode == OP_LOADI4_MEMBASE
					   || last_ins->opcode == OP_LOAD_MEMBASE) &&
			      ins->inst_basereg != last_ins->dreg &&
			      ins->inst_basereg == last_ins->inst_basereg &&
			      ins->inst_offset == last_ins->inst_offset) {

				if (ins->dreg == last_ins->dreg) {
					MONO_DELETE_INS (bb, ins);
					continue;
				} else {
					ins->opcode = OP_MOVE;
					ins->sreg1 = last_ins->dreg;
				}

				//g_assert_not_reached ();

#if 0
			/* 
			 * OP_STORE_MEMBASE_IMM imm, offset(basereg) 
			 * OP_LOAD_MEMBASE offset(basereg), reg
			 * -->
			 * OP_STORE_MEMBASE_IMM imm, offset(basereg) 
			 * OP_ICONST reg, imm
			 */
			} else if (last_ins && (last_ins->opcode == OP_STOREI4_MEMBASE_IMM
						|| last_ins->opcode == OP_STORE_MEMBASE_IMM) &&
				   ins->inst_basereg == last_ins->inst_destbasereg &&
				   ins->inst_offset == last_ins->inst_offset) {
				//static int c = 0; printf ("MATCHX %s %d\n", cfg->method->name,c++);
				ins->opcode = OP_ICONST;
				ins->inst_c0 = last_ins->inst_imm;
				g_assert_not_reached (); // check this rule
#endif
			}
			break;
		case OP_LOADU1_MEMBASE:
		case OP_LOADI1_MEMBASE:
			if (last_ins && (last_ins->opcode == OP_STOREI1_MEMBASE_REG) &&
					ins->inst_basereg == last_ins->inst_destbasereg &&
					ins->inst_offset == last_ins->inst_offset) {
				ins->opcode = (ins->opcode == OP_LOADI1_MEMBASE) ? OP_ICONV_TO_I1 : OP_ICONV_TO_U1;
				ins->sreg1 = last_ins->sreg1;				
			}
			break;
		case OP_LOADU2_MEMBASE:
		case OP_LOADI2_MEMBASE:
			if (last_ins && (last_ins->opcode == OP_STOREI2_MEMBASE_REG) &&
					ins->inst_basereg == last_ins->inst_destbasereg &&
					ins->inst_offset == last_ins->inst_offset) {
				ins->opcode = (ins->opcode == OP_LOADI2_MEMBASE) ? OP_ICONV_TO_I2 : OP_ICONV_TO_U2;
				ins->sreg1 = last_ins->sreg1;				
			}
			break;
		case OP_MOVE:
			ins->opcode = OP_MOVE;
			/* 
			 * OP_MOVE reg, reg 
			 */
			if (ins->dreg == ins->sreg1) {
				MONO_DELETE_INS (bb, ins);
				continue;
			}
			/* 
			 * OP_MOVE sreg, dreg 
			 * OP_MOVE dreg, sreg
			 */
			if (last_ins && last_ins->opcode == OP_MOVE &&
			    ins->sreg1 == last_ins->dreg &&
			    ins->dreg == last_ins->sreg1) {
				MONO_DELETE_INS (bb, ins);
				continue;
			}
			break;
		}
		last_ins = ins;
		ins = ins->next;
	}
	bb->last_ins = last_ins;
}

void
mono_arch_decompose_opts (MonoCompile *cfg, MonoInst *ins)
{
	switch (ins->opcode) {
	case OP_ICONV_TO_R_UN: {
		static const guint64 adjust_val = 0x4330000000000000ULL;
		int msw_reg = mono_alloc_ireg (cfg);
		int adj_reg = mono_alloc_freg (cfg);
		int tmp_reg = mono_alloc_freg (cfg);
		int basereg = ppc_sp;
		int offset = -8;
		MONO_EMIT_NEW_ICONST (cfg, msw_reg, 0x43300000);
		if (!ppc_is_imm16 (offset + 4)) {
			basereg = mono_alloc_ireg (cfg);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IADD_IMM, basereg, cfg->frame_reg, offset);
		}
		MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, basereg, offset, msw_reg);
		MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, basereg, offset + 4, ins->sreg1);
		MONO_EMIT_NEW_LOAD_R8 (cfg, adj_reg, &adjust_val);
		MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADR8_MEMBASE, tmp_reg, basereg, offset);
		MONO_EMIT_NEW_BIALU (cfg, OP_FSUB, ins->dreg, tmp_reg, adj_reg);
		ins->opcode = OP_NOP;
		break;
	}
	case OP_ICONV_TO_R4:
	case OP_ICONV_TO_R8: {
		/* FIXME: change precision for CEE_CONV_R4 */
		static const guint64 adjust_val = 0x4330000080000000ULL;
		int msw_reg = mono_alloc_ireg (cfg);
		int xored = mono_alloc_ireg (cfg);
		int adj_reg = mono_alloc_freg (cfg);
		int tmp_reg = mono_alloc_freg (cfg);
		int basereg = ppc_sp;
		int offset = -8;
		if (!ppc_is_imm16 (offset + 4)) {
			basereg = mono_alloc_ireg (cfg);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IADD_IMM, basereg, cfg->frame_reg, offset);
		}
		MONO_EMIT_NEW_ICONST (cfg, msw_reg, 0x43300000);
		MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, basereg, offset, msw_reg);
		MONO_EMIT_NEW_BIALU_IMM (cfg, OP_XOR_IMM, xored, ins->sreg1, 0x80000000);
		MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STOREI4_MEMBASE_REG, basereg, offset + 4, xored);
		MONO_EMIT_NEW_LOAD_R8 (cfg, adj_reg, (gpointer)&adjust_val);
		MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADR8_MEMBASE, tmp_reg, basereg, offset);
		MONO_EMIT_NEW_BIALU (cfg, OP_FSUB, ins->dreg, tmp_reg, adj_reg);
		if (ins->opcode == OP_ICONV_TO_R4)
			MONO_EMIT_NEW_UNALU (cfg, OP_FCONV_TO_R4, ins->dreg, ins->dreg);
		ins->opcode = OP_NOP;
		break;
	}
	case OP_CKFINITE: {
		int msw_reg = mono_alloc_ireg (cfg);
		int basereg = ppc_sp;
		int offset = -8;
		if (!ppc_is_imm16 (offset + 4)) {
			basereg = mono_alloc_ireg (cfg);
			MONO_EMIT_NEW_BIALU_IMM (cfg, OP_IADD_IMM, basereg, cfg->frame_reg, offset);
		}
		MONO_EMIT_NEW_STORE_MEMBASE (cfg, OP_STORER8_MEMBASE_REG, basereg, offset, ins->sreg1);
		MONO_EMIT_NEW_LOAD_MEMBASE_OP (cfg, OP_LOADI4_MEMBASE, msw_reg, basereg, offset);
		MONO_EMIT_NEW_UNALU (cfg, OP_CHECK_FINITE, -1, msw_reg);
		MONO_EMIT_NEW_UNALU (cfg, OP_FMOVE, ins->dreg, ins->sreg1);
		ins->opcode = OP_NOP;
		break;
	}
	}
}

/* 
 * the branch_b0_table should maintain the order of these
 * opcodes.
case CEE_BEQ:
case CEE_BGE:
case CEE_BGT:
case CEE_BLE:
case CEE_BLT:
case CEE_BNE_UN:
case CEE_BGE_UN:
case CEE_BGT_UN:
case CEE_BLE_UN:
case CEE_BLT_UN:
 */
static const guchar 
branch_b0_table [] = {
	PPC_BR_TRUE, 
	PPC_BR_FALSE, 
	PPC_BR_TRUE, 
	PPC_BR_FALSE, 
	PPC_BR_TRUE, 
	
	PPC_BR_FALSE, 
	PPC_BR_FALSE, 
	PPC_BR_TRUE, 
	PPC_BR_FALSE,
	PPC_BR_TRUE
};

static const guchar 
branch_b1_table [] = {
	PPC_BR_EQ, 
	PPC_BR_LT, 
	PPC_BR_GT, 
	PPC_BR_GT,
	PPC_BR_LT, 
	
	PPC_BR_EQ, 
	PPC_BR_LT, 
	PPC_BR_GT, 
	PPC_BR_GT,
	PPC_BR_LT 
};

#define NEW_INS(cfg,dest,op) do {					\
		MONO_INST_NEW((cfg), (dest), (op));			\
		mono_bblock_insert_after_ins (bb, last_ins, (dest));	\
	} while (0)

static int
map_to_reg_reg_op (int op)
{
	switch (op) {
	case OP_ADD_IMM:
		return OP_IADD;
	case OP_SUB_IMM:
		return OP_ISUB;
	case OP_AND_IMM:
		return OP_IAND;
	case OP_COMPARE_IMM:
		return OP_COMPARE;
	case OP_ICOMPARE_IMM:
		return OP_ICOMPARE;
	case OP_LCOMPARE_IMM:
		return OP_LCOMPARE;
	case OP_ADDCC_IMM:
		return OP_IADDCC;
	case OP_ADC_IMM:
		return OP_IADC;
	case OP_SUBCC_IMM:
		return OP_ISUBCC;
	case OP_SBB_IMM:
		return OP_ISBB;
	case OP_OR_IMM:
		return OP_IOR;
	case OP_XOR_IMM:
		return OP_IXOR;
	case OP_MUL_IMM:
		return OP_IMUL;
	case OP_LOAD_MEMBASE:
		return OP_LOAD_MEMINDEX;
	case OP_LOADI4_MEMBASE:
		return OP_LOADI4_MEMINDEX;
	case OP_LOADU4_MEMBASE:
		return OP_LOADU4_MEMINDEX;
	case OP_LOADI8_MEMBASE:
		return OP_LOADI8_MEMINDEX;
	case OP_LOADU1_MEMBASE:
		return OP_LOADU1_MEMINDEX;
	case OP_LOADI2_MEMBASE:
		return OP_LOADI2_MEMINDEX;
	case OP_LOADU2_MEMBASE:
		return OP_LOADU2_MEMINDEX;
	case OP_LOADI1_MEMBASE:
		return OP_LOADI1_MEMINDEX;
	case OP_LOADR4_MEMBASE:
		return OP_LOADR4_MEMINDEX;
	case OP_LOADR8_MEMBASE:
		return OP_LOADR8_MEMINDEX;
	case OP_STOREI1_MEMBASE_REG:
		return OP_STOREI1_MEMINDEX;
	case OP_STOREI2_MEMBASE_REG:
		return OP_STOREI2_MEMINDEX;
	case OP_STOREI4_MEMBASE_REG:
		return OP_STOREI4_MEMINDEX;
	case OP_STOREI8_MEMBASE_REG:
		return OP_STOREI8_MEMINDEX;
	case OP_STORE_MEMBASE_REG:
		return OP_STORE_MEMINDEX;
	case OP_STORER4_MEMBASE_REG:
		return OP_STORER4_MEMINDEX;
	case OP_STORER8_MEMBASE_REG:
		return OP_STORER8_MEMINDEX;
	case OP_STORE_MEMBASE_IMM:
		return OP_STORE_MEMBASE_REG;
	case OP_STOREI1_MEMBASE_IMM:
		return OP_STOREI1_MEMBASE_REG;
	case OP_STOREI2_MEMBASE_IMM:
		return OP_STOREI2_MEMBASE_REG;
	case OP_STOREI4_MEMBASE_IMM:
		return OP_STOREI4_MEMBASE_REG;
	case OP_STOREI8_MEMBASE_IMM:
		return OP_STOREI8_MEMBASE_REG;
	}
	return mono_op_imm_to_op (op);
}

//#define map_to_reg_reg_op(op) (cfg->new_ir? mono_op_imm_to_op (op): map_to_reg_reg_op (op))

#define compare_opcode_is_unsigned(opcode) \
		(((opcode) >= CEE_BNE_UN && (opcode) <= CEE_BLT_UN) ||	\
		((opcode) >= OP_IBNE_UN && (opcode) <= OP_IBLT_UN) ||	\
		((opcode) >= OP_LBNE_UN && (opcode) <= OP_LBLT_UN) ||	\
		((opcode) >= OP_COND_EXC_NE_UN && (opcode) <= OP_COND_EXC_LT_UN) ||	\
		((opcode) >= OP_COND_EXC_INE_UN && (opcode) <= OP_COND_EXC_ILT_UN) ||	\
		((opcode) == OP_CLT_UN || (opcode) == OP_CGT_UN ||	\
		 (opcode) == OP_ICLT_UN || (opcode) == OP_ICGT_UN ||	\
		 (opcode) == OP_LCLT_UN || (opcode) == OP_LCGT_UN))

/*
 * Remove from the instruction list the instructions that can't be
 * represented with very simple instructions with no register
 * requirements.
 */
void
mono_arch_lowering_pass (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins, *next, *temp, *last_ins = NULL;
	int imm;

	MONO_BB_FOR_EACH_INS (bb, ins) {
loop_start:
		switch (ins->opcode) {
		case OP_IDIV_UN_IMM:
		case OP_IDIV_IMM:
		case OP_IREM_IMM:
		case OP_IREM_UN_IMM:
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_imm;
			temp->dreg = mono_alloc_ireg (cfg);
			ins->sreg2 = temp->dreg;
			if (ins->opcode == OP_IDIV_IMM)
				ins->opcode = OP_IDIV;
			else if (ins->opcode == OP_IREM_IMM)
				ins->opcode = OP_IREM;
			else if (ins->opcode == OP_IDIV_UN_IMM)
				ins->opcode = OP_IDIV_UN;
			else if (ins->opcode == OP_IREM_UN_IMM)
				ins->opcode = OP_IREM_UN;
			last_ins = temp;
			/* handle rem separately */
			goto loop_start;
		case OP_IREM:
		case OP_IREM_UN: {
			MonoInst *mul;
			/* we change a rem dest, src1, src2 to
			 * div temp1, src1, src2
			 * mul temp2, temp1, src2
			 * sub dest, src1, temp2
			 */
			NEW_INS (cfg, mul, OP_IMUL);
			NEW_INS (cfg, temp, ins->opcode == OP_IREM? OP_IDIV: OP_IDIV_UN);
			temp->sreg1 = ins->sreg1;
			temp->sreg2 = ins->sreg2;
			temp->dreg = mono_alloc_ireg (cfg);
			mul->sreg1 = temp->dreg;
			mul->sreg2 = ins->sreg2;
			mul->dreg = mono_alloc_ireg (cfg);
			ins->opcode = OP_ISUB;
			ins->sreg2 = mul->dreg;
			break;
		}
		case OP_IADD_IMM:
		case OP_ADD_IMM:
		case OP_ADDCC_IMM:
			if (!ppc_is_imm16 (ins->inst_imm)) {
				NEW_INS (cfg,  temp, OP_ICONST);
				temp->inst_c0 = ins->inst_imm;
				temp->dreg = mono_alloc_ireg (cfg);
				ins->sreg2 = temp->dreg;
				ins->opcode = map_to_reg_reg_op (ins->opcode);
			}
			break;
		case OP_ISUB_IMM:
		case OP_SUB_IMM:
			if (!ppc_is_imm16 (-ins->inst_imm)) {
				NEW_INS (cfg, temp, OP_ICONST);
				temp->inst_c0 = ins->inst_imm;
				temp->dreg = mono_alloc_ireg (cfg);
				ins->sreg2 = temp->dreg;
				ins->opcode = map_to_reg_reg_op (ins->opcode);
			}
			break;
		case OP_AND_IMM:
		case OP_OR_IMM:
		case OP_XOR_IMM:
		case OP_IAND_IMM:
		case OP_IOR_IMM:
		case OP_IXOR_IMM:
		case OP_LAND_IMM:
		case OP_LOR_IMM:
		case OP_LXOR_IMM:
			if ((ins->inst_imm & ~0xffffUL) && (ins->inst_imm & 0xffff)) {
				NEW_INS (cfg, temp, OP_ICONST);
				temp->inst_c0 = ins->inst_imm;
				temp->dreg = mono_alloc_ireg (cfg);
				ins->sreg2 = temp->dreg;
				ins->opcode = map_to_reg_reg_op (ins->opcode);
			}
			break;
		case OP_ISBB_IMM:
		case OP_IADC_IMM:
		case OP_SBB_IMM:
		case OP_SUBCC_IMM:
		case OP_ADC_IMM:
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_imm;
			temp->dreg = mono_alloc_ireg (cfg);
			ins->sreg2 = temp->dreg;
			ins->opcode = map_to_reg_reg_op (ins->opcode);
			break;
		case OP_COMPARE_IMM:
		case OP_ICOMPARE_IMM:
		case OP_LCOMPARE_IMM:
			next = ins->next;
			/* Branch opts can eliminate the branch */
			if (!next || (!(MONO_IS_COND_BRANCH_OP (next) || MONO_IS_COND_EXC (next) || MONO_IS_SETCC (next)))) {
				ins->opcode = OP_NOP;
				break;
			}
			g_assert(next);
			if (compare_opcode_is_unsigned (next->opcode)) {
				if (!ppc_is_uimm16 (ins->inst_imm)) {
					NEW_INS (cfg, temp, OP_ICONST);
					temp->inst_c0 = ins->inst_imm;
					temp->dreg = mono_alloc_ireg (cfg);
					ins->sreg2 = temp->dreg;
					ins->opcode = map_to_reg_reg_op (ins->opcode);
				}
			} else {
				if (!ppc_is_imm16 (ins->inst_imm)) {
					NEW_INS (cfg, temp, OP_ICONST);
					temp->inst_c0 = ins->inst_imm;
					temp->dreg = mono_alloc_ireg (cfg);
					ins->sreg2 = temp->dreg;
					ins->opcode = map_to_reg_reg_op (ins->opcode);
				}
			}
			break;
		case OP_IMUL_IMM:
		case OP_MUL_IMM:
			if (ins->inst_imm == 1) {
				ins->opcode = OP_MOVE;
				break;
			}
			if (ins->inst_imm == 0) {
				ins->opcode = OP_ICONST;
				ins->inst_c0 = 0;
				break;
			}
			imm = mono_is_power_of_two (ins->inst_imm);
			if (imm > 0) {
				ins->opcode = OP_SHL_IMM;
				ins->inst_imm = imm;
				break;
			}
			if (!ppc_is_imm16 (ins->inst_imm)) {
				NEW_INS (cfg, temp, OP_ICONST);
				temp->inst_c0 = ins->inst_imm;
				temp->dreg = mono_alloc_ireg (cfg);
				ins->sreg2 = temp->dreg;
				ins->opcode = map_to_reg_reg_op (ins->opcode);
			}
			break;
		case OP_LOCALLOC_IMM:
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_imm;
			temp->dreg = mono_alloc_ireg (cfg);
			ins->sreg1 = temp->dreg;
			ins->opcode = OP_LOCALLOC;
			break;
		case OP_LOAD_MEMBASE:
		case OP_LOADI4_MEMBASE:
		case OP_LOADI8_MEMBASE:
		case OP_LOADU4_MEMBASE:
		case OP_LOADI2_MEMBASE:
		case OP_LOADU2_MEMBASE:
		case OP_LOADI1_MEMBASE:
		case OP_LOADU1_MEMBASE:
		case OP_LOADR4_MEMBASE:
		case OP_LOADR8_MEMBASE:
		case OP_STORE_MEMBASE_REG:
		case OP_STOREI8_MEMBASE_REG:
		case OP_STOREI4_MEMBASE_REG:
		case OP_STOREI2_MEMBASE_REG:
		case OP_STOREI1_MEMBASE_REG:
		case OP_STORER4_MEMBASE_REG:
		case OP_STORER8_MEMBASE_REG:
			/* we can do two things: load the immed in a register
			 * and use an indexed load, or see if the immed can be
			 * represented as an ad_imm + a load with a smaller offset
			 * that fits. We just do the first for now, optimize later.
			 */
			if (ppc_is_imm16 (ins->inst_offset))
				break;
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_offset;
			temp->dreg = mono_alloc_ireg (cfg);
			ins->sreg2 = temp->dreg;
			ins->opcode = map_to_reg_reg_op (ins->opcode);
			break;
		case OP_STORE_MEMBASE_IMM:
		case OP_STOREI1_MEMBASE_IMM:
		case OP_STOREI2_MEMBASE_IMM:
		case OP_STOREI4_MEMBASE_IMM:
		case OP_STOREI8_MEMBASE_IMM:
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = ins->inst_imm;
			temp->dreg = mono_alloc_ireg (cfg);
			ins->sreg1 = temp->dreg;
			ins->opcode = map_to_reg_reg_op (ins->opcode);
			last_ins = temp;
			goto loop_start; /* make it handle the possibly big ins->inst_offset */
		case OP_R8CONST:
		case OP_R4CONST:
			NEW_INS (cfg, temp, OP_ICONST);
			temp->inst_c0 = (gulong)ins->inst_p0;
			temp->dreg = mono_alloc_ireg (cfg);
			ins->inst_basereg = temp->dreg;
			ins->inst_offset = 0;
			ins->opcode = ins->opcode == OP_R4CONST? OP_LOADR4_MEMBASE: OP_LOADR8_MEMBASE;
			last_ins = temp;
			/* make it handle the possibly big ins->inst_offset
			 * later optimize to use lis + load_membase
			 */
			goto loop_start;
		}
		last_ins = ins;
	}
	bb->last_ins = last_ins;
	bb->max_vreg = cfg->next_vreg;	
}

static guchar*
emit_float_to_int (MonoCompile *cfg, guchar *code, int dreg, int sreg, int size, gboolean is_signed)
{
	int offset = cfg->arch.fp_conv_var_offset;
	/* sreg is a float, dreg is an integer reg. ppc_f0 is used a scratch */
	if (size == 8) {
		ppc_fctidz (code, ppc_f0, sreg);
	} else {
		ppc_fctiwz (code, ppc_f0, sreg);
	}
	if (ppc_is_imm16 (offset + 4)) {
		ppc_stfd (code, ppc_f0, offset, cfg->frame_reg);
		ppc_lwz (code, dreg, offset + 4, cfg->frame_reg);
	} else {
		ppc_load (code, dreg, offset);
		ppc_add (code, dreg, dreg, cfg->frame_reg);
		ppc_stfd (code, ppc_f0, 0, dreg);
		ppc_lwz (code, dreg, 4, dreg);
	}
	if (!is_signed) {
		if (size == 1)
			ppc_andid (code, dreg, dreg, 0xff);
		else if (size == 2)
			ppc_andid (code, dreg, dreg, 0xffff);
		else if (size == 4)
			ppc_clrldi (code, dreg, dreg, 32);
	} else {
		if (size == 1)
			ppc_extsb (code, dreg, dreg);
		else if (size == 2)
			ppc_extsh (code, dreg, dreg);
		else if (size == 4)
			ppc_extsw (code, dreg, dreg);
	}
	return code;
}

typedef struct {
	guchar *code;
	const guchar *target;
	int absolute;
	int found;
} PatchData;

#define is_call_imm(diff) ((glong)(diff) >= -33554432 && (glong)(diff) <= 33554431)

static int
search_thunk_slot (void *data, int csize, int bsize, void *user_data) {
	PatchData *pdata = (PatchData*)user_data;
	guchar *code = data;
	guint32 *thunks = data;
	guint32 *endthunks = (guint32*)(code + bsize);
	guint32 load [5];
	guchar *templ;
	int count = 0;
	int difflow, diffhigh;

	/* always ensure a call from pdata->code can reach to the thunks without further thunks */
	difflow = (char*)pdata->code - (char*)thunks;
	diffhigh = (char*)pdata->code - (char*)endthunks;
	if (!((is_call_imm (thunks) && is_call_imm (endthunks)) || (is_call_imm (difflow) && is_call_imm (diffhigh))))
		return 0;

	templ = (guchar*)load;
	ppc_load (templ, ppc_r0, pdata->target);

	g_assert_not_reached ();

	//g_print ("thunk nentries: %d\n", ((char*)endthunks - (char*)thunks)/16);
	if ((pdata->found == 2) || (pdata->code >= code && pdata->code <= code + csize)) {
		while (thunks < endthunks) {
			//g_print ("looking for target: %p at %p (%08x-%08x)\n", pdata->target, thunks, thunks [0], thunks [1]);
			if ((thunks [0] == load [0]) && (thunks [1] == load [1])) {
				ppc_patch (pdata->code, (guchar*)thunks);
				mono_arch_flush_icache (pdata->code, 4);
				pdata->found = 1;
				/*{
					static int num_thunks = 0;
					num_thunks++;
					if ((num_thunks % 20) == 0)
						g_print ("num_thunks lookup: %d\n", num_thunks);
				}*/
				return 1;
			} else if ((thunks [0] == 0) && (thunks [1] == 0)) {
				/* found a free slot instead: emit thunk */
				code = (guchar*)thunks;
				g_assert_not_reached ();
				ppc_lis (code, ppc_r0, (gulong)(pdata->target) >> 16);
				ppc_ori (code, ppc_r0, ppc_r0, (gulong)(pdata->target) & 0xffff);
				ppc_mtctr (code, ppc_r0);
				ppc_bcctr (code, PPC_BR_ALWAYS, 0);
				mono_arch_flush_icache ((guchar*)thunks, 16);

				ppc_patch (pdata->code, (guchar*)thunks);
				mono_arch_flush_icache (pdata->code, 4);
				pdata->found = 1;
				/*{
					static int num_thunks = 0;
					num_thunks++;
					if ((num_thunks % 20) == 0)
						g_print ("num_thunks: %d\n", num_thunks);
				}*/
				return 1;
			}
			/* skip 16 bytes, the size of the thunk */
			thunks += 4;
			count++;
		}
		//g_print ("failed thunk lookup for %p from %p at %p (%d entries)\n", pdata->target, pdata->code, data, count);
	}
	return 0;
}

static void
handle_thunk (int absolute, guchar *code, const guchar *target) {
	MonoDomain *domain = mono_domain_get ();
	PatchData pdata;

	pdata.code = code;
	pdata.target = target;
	pdata.absolute = absolute;
	pdata.found = 0;

	mono_domain_lock (domain);
	mono_code_manager_foreach (domain->code_mp, search_thunk_slot, &pdata);

	if (!pdata.found) {
		/* this uses the first available slot */
		pdata.found = 2;
		mono_code_manager_foreach (domain->code_mp, search_thunk_slot, &pdata);
	}
	mono_domain_unlock (domain);

	if (pdata.found != 1)
		g_print ("thunk failed for %p from %p\n", target, code);
	g_assert (pdata.found == 1);
}

void
ppc_patch_full (guchar *code, const guchar *target, gboolean is_fd)
{
	guint32 ins = *(guint32*)code;
	guint32 prim = ppc_opcode (ins);
	guint32 ovf;

	//g_print ("patching %p (0x%08x) to point to %p\n", code, ins, target);
	if (prim == 18) {
		// prefer relative branches, they are more position independent (e.g. for AOT compilation).
		gint diff = target - code;
		g_assert (!is_fd);
		if (diff >= 0){
			if (diff <= 33554431){
				ins = (18 << 26) | (diff) | (ins & 1);
				*(guint32*)code = ins;
				return;
			}
		} else {
			/* diff between 0 and -33554432 */
			if (diff >= -33554432){
				ins = (18 << 26) | (diff & ~0xfc000000) | (ins & 1);
				*(guint32*)code = ins;
				return;
			}
		}
		
		if ((glong)target >= 0){
			if ((glong)target <= 33554431){
				ins = (18 << 26) | ((gulong) target) | (ins & 1) | 2;
				*(guint32*)code = ins;
				return;
			}
		} else {
			if ((glong)target >= -33554432){
				ins = (18 << 26) | (((gulong)target) & ~0xfc000000) | (ins & 1) | 2;
				*(guint32*)code = ins;
				return;
			}
		}

		handle_thunk (TRUE, code, target);
		return;

		g_assert_not_reached ();
	}
	
	
	if (prim == 16) {
		g_assert (!is_fd);
		// absolute address
		if (ins & 2) {
			guint32 li = (gulong)target;
			ins = (ins & 0xffff0000) | (ins & 3);
			ovf  = li & 0xffff0000;
			if (ovf != 0 && ovf != 0xffff0000)
				g_assert_not_reached ();
			li &= 0xffff;
			ins |= li;
			// FIXME: assert the top bits of li are 0
		} else {
			gint diff = target - code;
			ins = (ins & 0xffff0000) | (ins & 3);
			ovf  = diff & 0xffff0000;
			if (ovf != 0 && ovf != 0xffff0000)
				g_assert_not_reached ();
			diff &= 0xffff;
			ins |= diff;
		}
		*(guint32*)code = ins;
		return;
	}

	if (prim == 15 || ins == 0x4e800021 || ins == 0x4e800020 || ins == 0x4e800420) {
		guint32 *seq = (guint32*)code;
		guint32 *branch_ins;

		/* the trampoline code will try to patch the blrl, blr, bcctr */
		if (ins == 0x4e800021 || ins == 0x4e800020 || ins == 0x4e800420) {
			branch_ins = seq;
			if (ppc_opcode (seq [-3]) == 58 || ppc_opcode (seq [-3]) == 31) /* ld || mr */
				code -= 32;
			else
				code -= 24;
		} else {
			if (ppc_opcode (seq [5]) == 58 || ppc_opcode (seq [5]) == 31) /* ld || mr */
				branch_ins = seq + 8;
			else
				branch_ins = seq + 6;
		}

		seq = (guint32*)code;
		/* this is the lis/ori/sldi/oris/ori/(ld/ld|mr/nop)/mtlr/blrl sequence */
		g_assert (mono_ppc_is_direct_call_sequence (branch_ins));

		if (ppc_opcode (seq [5]) == 58) {	/* ld */
			g_assert (ppc_opcode (seq [6]) == 58); /* ld */

			if (!is_fd) {
				guint8 *buf = (guint8*)&seq [5];
				ppc_mr (buf, ppc_r0, ppc_r11);
				ppc_nop (buf);
			}
		} else {
			if (is_fd)
				target = mono_get_addr_from_ftnptr ((gpointer)target);
		}

		/* FIXME: make this thread safe */
		/* FIXME: we're assuming we're using r11 here */
		ppc_load_sequence (code, ppc_r11, target);
		mono_arch_flush_icache (code, 28);
	} else {
		g_assert_not_reached ();
	}
}

void
ppc_patch (guchar *code, const guchar *target)
{
	ppc_patch_full (code, target, FALSE);
}

static guint8*
emit_move_return_value (MonoCompile *cfg, MonoInst *ins, guint8 *code)
{
	switch (ins->opcode) {
	case OP_FCALL:
	case OP_FCALL_REG:
	case OP_FCALL_MEMBASE:
		if (ins->dreg != ppc_f1)
			ppc_fmr (code, ins->dreg, ppc_f1);
		break;
	}

	return code;
}

/*
 * emit_load_volatile_arguments:
 *
 *  Load volatile arguments from the stack to the original input registers.
 * Required before a tail call.
 */
static guint8*
emit_load_volatile_arguments (MonoCompile *cfg, guint8 *code)
{
	MonoMethod *method = cfg->method;
	MonoMethodSignature *sig;
	MonoInst *inst;
	CallInfo *cinfo;
	guint32 i, pos;
	int struct_index = 0;

	g_assert_not_reached ();

	/* FIXME: Generate intermediate code instead */

	sig = mono_method_signature (method);

	/* This is the opposite of the code in emit_prolog */

	pos = 0;

	cinfo = calculate_sizes (sig, sig->pinvoke);

	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		ArgInfo *ainfo = &cinfo->ret;
		inst = cfg->vret_addr;
		g_assert (ppc_is_imm16 (inst->inst_offset));
		ppc_load_reg (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
	}
	for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
		ArgInfo *ainfo = cinfo->args + i;
		inst = cfg->args [pos];

		g_assert (inst->opcode != OP_REGVAR);
		g_assert (ppc_is_imm16 (inst->inst_offset));

		switch (ainfo->regtype) {
		case RegTypeGeneral:
			switch (ainfo->size) {
				case 1:
					ppc_lbz (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
					break;
				case 2:
					ppc_lhz (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
					break;
				default:
					ppc_lwz (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
					break;
			}
			break;

		case RegTypeFP:
			switch (ainfo->size) {
				case 4:
					ppc_lfs (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
					break;
				case 8:
					ppc_lfd (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
					break;
				default:
					g_assert_not_reached ();
			}
			break;

		case RegTypeBase: {
			MonoType *type = mini_type_get_underlying_type (cfg->generic_sharing_context,
				&inst->klass->byval_arg);

			if (!MONO_TYPE_IS_REFERENCE (type) && type->type != MONO_TYPE_I4)
				NOT_IMPLEMENTED;

			ppc_lwz (code, ppc_r0, inst->inst_offset, inst->inst_basereg);
			ppc_stw (code, ppc_r0, ainfo->offset, ainfo->reg);
			break;
		}

		case RegTypeStructByVal: {
			guint32 size = 0;
			int j;

			/* FIXME: */
			if (ainfo->vtsize)
				NOT_IMPLEMENTED;
#ifdef __APPLE__
			/*
			 * Darwin pinvokes needs some special handling
			 * for 1 and 2 byte arguments
			 */
			if (method->signature->pinvoke)
				size = mono_class_native_size (inst->klass, NULL);
			if (size == 1 || size == 2) {
				/* FIXME: */
				NOT_IMPLEMENTED;
			} else
#endif
				for (j = 0; j < ainfo->size; ++j) {
					ppc_lwz (code, ainfo->reg  + j,
						inst->inst_offset + j * sizeof (gpointer), inst->inst_basereg);
				}
			break;
		}

		case RegTypeStructByAddr: {
			MonoInst *addr = cfg->tailcall_valuetype_addrs [struct_index];

			g_assert (ppc_is_imm16 (addr->inst_offset));
			g_assert (!ainfo->offset);
			ppc_lwz (code, ainfo->reg, addr->inst_offset, addr->inst_basereg);

			struct_index++;
			break;
		}

		default:
			g_assert_not_reached ();
		}

		pos ++;
	}

	g_free (cinfo);

	return code;
}

/* This must be kept in sync with emit_load_volatile_arguments(). */
static int
ins_native_length (MonoCompile *cfg, MonoInst *ins)
{
	int len = ((guint8 *)ins_get_spec (ins->opcode))[MONO_INST_LEN];
	MonoMethodSignature *sig;
	MonoCallInst *call;
	CallInfo *cinfo;
	int i;

	if (ins->opcode != OP_JMP)
		return len;

	g_assert_not_reached ();

	call = (MonoCallInst*)ins;
	sig = mono_method_signature (cfg->method);
	cinfo = calculate_sizes (sig, sig->pinvoke);

	if (MONO_TYPE_ISSTRUCT (sig->ret))
		len += 4;
	for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
		ArgInfo *ainfo = cinfo->args + i;

		switch (ainfo->regtype) {
		case RegTypeGeneral:
		case RegTypeFP:
			len += 4;
			break;

		case RegTypeBase:
			len += 8;
			break;

		case RegTypeStructByVal:
			len += 4 * ainfo->size;
			break;

		case RegTypeStructByAddr:
			len += 4;
			break;

		default:
			g_assert_not_reached ();
		}
	}

	g_free (cinfo);

	return len;
}

static guint8*
emit_reserve_param_area (MonoCompile *cfg, guint8 *code)
{
	int size = cfg->param_area;

	size += MONO_ARCH_FRAME_ALIGNMENT - 1;
	size &= -MONO_ARCH_FRAME_ALIGNMENT;

	if (!size)
		return code;

	ppc_load_reg (code, ppc_r0, 0, ppc_sp);
	if (ppc_is_imm16 (-size)) {
		ppc_store_reg_update (code, ppc_r0, -size, ppc_sp);
	} else {
		ppc_load (code, ppc_r11, -size);
		ppc_store_reg_update_indexed (code, ppc_r0, ppc_sp, ppc_r11);
	}

	return code;
}

static guint8*
emit_unreserve_param_area (MonoCompile *cfg, guint8 *code)
{
	int size = cfg->param_area;

	size += MONO_ARCH_FRAME_ALIGNMENT - 1;
	size &= -MONO_ARCH_FRAME_ALIGNMENT;

	if (!size)
		return code;

	ppc_load_reg (code, ppc_r0, 0, ppc_sp);
	if (ppc_is_imm16 (size)) {
		ppc_store_reg_update (code, ppc_r0, size, ppc_sp);
	} else {
		ppc_load (code, ppc_r11, size);
		ppc_store_reg_update_indexed (code, ppc_r0, ppc_sp, ppc_r11);
	}

	return code;
}

void
mono_arch_output_basic_block (MonoCompile *cfg, MonoBasicBlock *bb)
{
	MonoInst *ins, *next;
	MonoCallInst *call;
	guint offset;
	guint8 *code = cfg->native_code + cfg->code_len;
	MonoInst *last_ins = NULL;
	guint last_offset = 0;
	int max_len, cpos;
	int L;

	/* we don't align basic blocks of loops on ppc */

	if (cfg->verbose_level > 2)
		g_print ("Basic block %d starting at offset 0x%x\n", bb->block_num, bb->native_offset);

	cpos = bb->max_offset;

	if (cfg->prof_options & MONO_PROFILE_COVERAGE) {
		//MonoCoverageInfo *cov = mono_get_coverage_info (cfg->method);
		//g_assert (!mono_compile_aot);
		//cpos += 6;
		//if (bb->cil_code)
		//	cov->data [bb->dfn].iloffset = bb->cil_code - cfg->cil_code;
		/* this is not thread save, but good enough */
		/* fixme: howto handle overflows? */
		//x86_inc_mem (code, &cov->data [bb->dfn].count);
	}

	MONO_BB_FOR_EACH_INS (bb, ins) {
		offset = code - cfg->native_code;

		max_len = ins_native_length (cfg, ins);

		if (offset > (cfg->code_size - max_len - 16)) {
			cfg->code_size *= 2;
			cfg->native_code = g_realloc (cfg->native_code, cfg->code_size);
			code = cfg->native_code + offset;
		}
	//	if (ins->cil_code)
	//		g_print ("cil code\n");
		mono_debug_record_line_number (cfg, ins, offset);

		switch (ins->opcode) {
		case OP_RELAXED_NOP:
		case OP_NOP:
		case OP_DUMMY_USE:
		case OP_DUMMY_STORE:
		case OP_NOT_REACHED:
		case OP_NOT_NULL:
			break;
		case OP_TLS_GET:
			emit_tls_access (code, ins->dreg, ins->inst_offset);
			break;
		case OP_BIGMUL:
			ppc_mullw (code, ppc_r0, ins->sreg1, ins->sreg2);
			ppc_mulhw (code, ppc_r3, ins->sreg1, ins->sreg2);
			ppc_mr (code, ppc_r4, ppc_r0);
			break;
		case OP_BIGMUL_UN:
			ppc_mullw (code, ppc_r0, ins->sreg1, ins->sreg2);
			ppc_mulhwu (code, ppc_r3, ins->sreg1, ins->sreg2);
			ppc_mr (code, ppc_r4, ppc_r0);
			break;
		case OP_MEMORY_BARRIER:
			ppc_sync (code);
			break;
		case OP_STOREI1_MEMBASE_REG:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_stb (code, ins->sreg1, ins->inst_offset, ins->inst_destbasereg);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_stbx (code, ins->sreg1, ins->inst_destbasereg, ppc_r0);
			}
			break;
		case OP_STOREI2_MEMBASE_REG:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_sth (code, ins->sreg1, ins->inst_offset, ins->inst_destbasereg);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_sthx (code, ins->sreg1, ins->inst_destbasereg, ppc_r0);
			}
			break;
		case OP_STOREI4_MEMBASE_REG:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_stw (code, ins->sreg1, ins->inst_offset, ins->inst_destbasereg);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_stwx (code, ins->sreg1, ins->inst_destbasereg, ppc_r0);
			}
			break;
		case OP_STORE_MEMBASE_REG:
		case OP_STOREI8_MEMBASE_REG:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_store_reg (code, ins->sreg1, ins->inst_offset, ins->inst_destbasereg);
			} else {
				/* FIXME: implement */
				g_assert_not_reached ();
			}
			break;
		case OP_STOREI1_MEMINDEX:
			ppc_stbx (code, ins->sreg1, ins->sreg2, ins->inst_destbasereg);
			break;
		case OP_STOREI2_MEMINDEX:
			ppc_sthx (code, ins->sreg1, ins->sreg2, ins->inst_destbasereg);
			break;
		case OP_STOREI4_MEMINDEX:
			ppc_stwx (code, ins->sreg1, ins->sreg2, ins->inst_destbasereg);
			break;
		case OP_STORE_MEMINDEX:
		case OP_STOREI8_MEMINDEX:
			ppc_stdx (code, ins->sreg1, ins->sreg2, ins->inst_destbasereg);
			break;
		case OP_LOADU4_MEM:
			g_assert_not_reached ();
			break;
		case OP_LOAD_MEMBASE:
		case OP_LOADI8_MEMBASE:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_load_reg (code, ins->dreg, ins->inst_offset, ins->inst_basereg);
			} else {
				g_assert_not_reached ();
			}
			break;
		case OP_LOADI4_MEMBASE:
		case OP_LOADU4_MEMBASE:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_lwz (code, ins->dreg, ins->inst_offset, ins->inst_basereg);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_lwzx (code, ins->dreg, ins->inst_basereg, ppc_r0);
			}
			break;
		case OP_LOADI1_MEMBASE:
		case OP_LOADU1_MEMBASE:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_lbz (code, ins->dreg, ins->inst_offset, ins->inst_basereg);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_lbzx (code, ins->dreg, ins->inst_basereg, ppc_r0);
			}
			if (ins->opcode == OP_LOADI1_MEMBASE)
				ppc_extsb (code, ins->dreg, ins->dreg);
			break;
		case OP_LOADU2_MEMBASE:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_lhz (code, ins->dreg, ins->inst_offset, ins->inst_basereg);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_lhzx (code, ins->dreg, ins->inst_basereg, ppc_r0);
			}
			break;
		case OP_LOADI2_MEMBASE:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_lha (code, ins->dreg, ins->inst_basereg, ins->inst_offset);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_lhax (code, ins->dreg, ins->inst_basereg, ppc_r0);
			}
			break;
		case OP_LOAD_MEMINDEX:
		case OP_LOADI8_MEMINDEX:
			ppc_ldx (code, ins->dreg, ins->sreg2, ins->inst_basereg);
			break;
		case OP_LOADI4_MEMINDEX:
		case OP_LOADU4_MEMINDEX:
			ppc_lwzx (code, ins->dreg, ins->sreg2, ins->inst_basereg);
			break;
		case OP_LOADU2_MEMINDEX:
			ppc_lhzx (code, ins->dreg, ins->sreg2, ins->inst_basereg);
			break;
		case OP_LOADI2_MEMINDEX:
			ppc_lhax (code, ins->dreg, ins->sreg2, ins->inst_basereg);
			break;
		case OP_LOADU1_MEMINDEX:
			ppc_lbzx (code, ins->dreg, ins->sreg2, ins->inst_basereg);
			break;
		case OP_LOADI1_MEMINDEX:
			ppc_lbzx (code, ins->dreg, ins->sreg2, ins->inst_basereg);
			ppc_extsb (code, ins->dreg, ins->dreg);
			break;
		case OP_ICONV_TO_I1:
		case OP_LCONV_TO_I1:
			ppc_extsb (code, ins->dreg, ins->sreg1);
			break;
		case OP_ICONV_TO_I2:
		case OP_LCONV_TO_I2:
			ppc_extsh (code, ins->dreg, ins->sreg1);
			break;
		case OP_ICONV_TO_I4:
		case OP_SEXT_I4:
			ppc_extsw (code, ins->dreg, ins->sreg1);
			break;
		case OP_ICONV_TO_U1:
		case OP_LCONV_TO_U1:
			ppc_clrlwi (code, ins->dreg, ins->sreg1, 24);
			break;
		case OP_ICONV_TO_U2:
		case OP_LCONV_TO_U2:
			ppc_clrlwi (code, ins->dreg, ins->sreg1, 16);
			break;
		case OP_ICONV_TO_U4:
		case OP_ZEXT_I4:
			ppc_clrldi (code, ins->dreg, ins->sreg1, 32);
			break;
		case OP_COMPARE:
		case OP_ICOMPARE:
		case OP_LCOMPARE:
			L = (ins->opcode == OP_LCOMPARE) ? 1 : 0;
			next = ins->next;
			if (next && compare_opcode_is_unsigned (next->opcode))
				ppc_cmpl (code, 0, L, ins->sreg1, ins->sreg2);
			else
				ppc_cmp (code, 0, L, ins->sreg1, ins->sreg2);
			break;
		case OP_COMPARE_IMM:
		case OP_ICOMPARE_IMM:
		case OP_LCOMPARE_IMM:
			L = (ins->opcode == OP_LCOMPARE_IMM) ? 1 : 0;
			next = ins->next;
			if (next && compare_opcode_is_unsigned (next->opcode)) {
				if (ppc_is_uimm16 (ins->inst_imm)) {
					ppc_cmpli (code, 0, L, ins->sreg1, (ins->inst_imm & 0xffff));
				} else {
					g_assert_not_reached ();
				}
			} else {
				if (ppc_is_imm16 (ins->inst_imm)) {
					ppc_cmpi (code, 0, L, ins->sreg1, (ins->inst_imm & 0xffff));
				} else {
					g_assert_not_reached ();
				}
			}
			break;
		case OP_BREAK:
			ppc_break (code);
			break;
		case OP_ADDCC:
		case OP_IADDCC:
			ppc_addco (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_IADD:
		case OP_LADD:
			ppc_add (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_ADC:
		case OP_IADC:
			ppc_adde (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_ADDCC_IMM:
			if (ppc_is_imm16 (ins->inst_imm)) {
				ppc_addic (code, ins->dreg, ins->sreg1, ins->inst_imm);
			} else {
				g_assert_not_reached ();
			}
			break;
		case OP_ADD_IMM:
		case OP_IADD_IMM:
		case OP_LADD_IMM:
			if (ppc_is_imm16 (ins->inst_imm)) {
				ppc_addi (code, ins->dreg, ins->sreg1, ins->inst_imm);
			} else {
				g_assert_not_reached ();
			}
			break;
		case OP_IADD_OVF:
			/* check XER [0-3] (SO, OV, CA): we can't use mcrxr
			 */
			ppc_addo (code, ins->dreg, ins->sreg1, ins->sreg2);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<14));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_IADD_OVF_UN:
			/* check XER [0-3] (SO, OV, CA): we can't use mcrxr
			 */
			ppc_addco (code, ins->dreg, ins->sreg1, ins->sreg2);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<13));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_ISUB_OVF:
		case OP_LSUB_OVF:
			/* check XER [0-3] (SO, OV, CA): we can't use mcrxr
			 */
			ppc_subfo (code, ins->dreg, ins->sreg2, ins->sreg1);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<14));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_ISUB_OVF_UN:
		case OP_LSUB_OVF_UN:
			/* check XER [0-3] (SO, OV, CA): we can't use mcrxr
			 */
			ppc_subfc (code, ins->dreg, ins->sreg2, ins->sreg1);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<13));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_TRUE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_ADD_OVF_CARRY:
			/* check XER [0-3] (SO, OV, CA): we can't use mcrxr
			 */
			ppc_addeo (code, ins->dreg, ins->sreg1, ins->sreg2);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<14));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_ADD_OVF_UN_CARRY:
			/* check XER [0-3] (SO, OV, CA): we can't use mcrxr
			 */
			ppc_addeo (code, ins->dreg, ins->sreg1, ins->sreg2);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<13));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_SUB_OVF_CARRY:
			/* check XER [0-3] (SO, OV, CA): we can't use mcrxr
			 */
			ppc_subfeo (code, ins->dreg, ins->sreg2, ins->sreg1);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<14));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_SUB_OVF_UN_CARRY:
			/* check XER [0-3] (SO, OV, CA): we can't use mcrxr
			 */
			ppc_subfeo (code, ins->dreg, ins->sreg2, ins->sreg1);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<13));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_TRUE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_SUBCC:
		case OP_ISUBCC:
			ppc_subfco (code, ins->dreg, ins->sreg2, ins->sreg1);
			break;
		case OP_ISUB:
		case OP_LSUB:
			ppc_subf (code, ins->dreg, ins->sreg2, ins->sreg1);
			break;
		case OP_SBB:
		case OP_ISBB:
			ppc_subfe (code, ins->dreg, ins->sreg2, ins->sreg1);
			break;
		case OP_SUB_IMM:
		case OP_ISUB_IMM:
		case OP_LSUB_IMM:
			// we add the negated value
			if (ppc_is_imm16 (-ins->inst_imm))
				ppc_addi (code, ins->dreg, ins->sreg1, -ins->inst_imm);
			else {
				g_assert_not_reached ();
			}
			break;
		case OP_PPC_SUBFIC:
			g_assert (ppc_is_imm16 (ins->inst_imm));
			ppc_subfic (code, ins->dreg, ins->sreg1, ins->inst_imm);
			break;
		case OP_PPC_SUBFZE:
			ppc_subfze (code, ins->dreg, ins->sreg1);
			break;
		case OP_IAND:
		case OP_LAND:
			/* FIXME: the ppc macros as inconsistent here: put dest as the first arg! */
			ppc_and (code, ins->sreg1, ins->dreg, ins->sreg2);
			break;
		case OP_AND_IMM:
		case OP_IAND_IMM:
		case OP_LAND_IMM:
			if (!(ins->inst_imm & 0xffff0000)) {
				ppc_andid (code, ins->sreg1, ins->dreg, ins->inst_imm);
			} else if (!(ins->inst_imm & 0xffff)) {
				ppc_andisd (code, ins->sreg1, ins->dreg, ((guint32)ins->inst_imm >> 16));
			} else {
				g_assert_not_reached ();
			}
			break;
		case OP_IDIV: {
			guint8 *divisor_is_m1;
                         /* XER format: SO, OV, CA, reserved [21 bits], count [8 bits]
                         */
			ppc_cmpi (code, 0, 1, ins->sreg2, -1);
			divisor_is_m1 = code;
			ppc_bc (code, PPC_BR_FALSE | PPC_BR_LIKELY, PPC_BR_EQ, 0);
			ppc_lis (code, ppc_r0, 0x8000);
			ppc_cmp (code, 0, 1, ins->sreg1, ppc_r0);
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_TRUE, PPC_BR_EQ, "ArithmeticException");
			ppc_patch (divisor_is_m1, code);
			 /* XER format: SO, OV, CA, reserved [21 bits], count [8 bits]
			 */
			ppc_divwod (code, ins->dreg, ins->sreg1, ins->sreg2);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<14));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "DivideByZeroException");
			break;
		}
		case OP_LDIV:
			ppc_divd (code, ins->dreg, ins->sreg1, ins->sreg2);
			/* FIXME: div by zero check */
			break;
		case OP_LDIV_UN:
			ppc_divdu (code, ins->dreg, ins->sreg1, ins->sreg2);
			/* FIXME: div by zero check */
			break;
		case OP_IDIV_UN:
			ppc_divwuod (code, ins->dreg, ins->sreg1, ins->sreg2);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<14));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "DivideByZeroException");
			break;
		case OP_DIV_IMM:
		case OP_IREM:
		case OP_IREM_UN:
		case OP_REM_IMM:
			g_assert_not_reached ();
		case OP_IOR:
		case OP_LOR:
			ppc_or (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_OR_IMM:
		case OP_IOR_IMM:
		case OP_LOR_IMM:
			if (!(ins->inst_imm & 0xffff0000)) {
				ppc_ori (code, ins->sreg1, ins->dreg, ins->inst_imm);
			} else if (!(ins->inst_imm & 0xffff)) {
				ppc_oris (code, ins->dreg, ins->sreg1, ((guint32)(ins->inst_imm) >> 16));
			} else {
				g_assert_not_reached ();
			}
			break;
		case OP_IXOR:
		case OP_LXOR:
			ppc_xor (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_IXOR_IMM:
		case OP_XOR_IMM:
		case OP_LXOR_IMM:
			if (!(ins->inst_imm & 0xffff0000)) {
				ppc_xori (code, ins->sreg1, ins->dreg, ins->inst_imm);
			} else if (!(ins->inst_imm & 0xffff)) {
				ppc_xoris (code, ins->sreg1, ins->dreg, ((guint32)(ins->inst_imm) >> 16));
			} else {
				g_assert_not_reached ();
			}
			break;
		case OP_ISHL:
		case OP_LSHL:
			ppc_sld (code, ins->sreg1, ins->dreg, ins->sreg2);
			break;
		case OP_SHL_IMM:
		case OP_ISHL_IMM:
		case OP_LSHL_IMM:
			ppc_sldi (code, ins->dreg, ins->sreg1, (ins->inst_imm & 0x3f));
			break;
		case OP_ISHR:
			ppc_sraw (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_LSHR:
			ppc_srad (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_SHR_IMM:
		case OP_LSHR_IMM:
			ppc_sradi (code, ins->dreg, ins->sreg1, (ins->inst_imm & 0x3f));
			break;
		case OP_ISHR_IMM:
			ppc_srawi (code, ins->dreg, ins->sreg1, (ins->inst_imm & 0x1f));
			break;
		case OP_SHR_UN_IMM:
		case OP_LSHR_UN_IMM:
			ppc_srdi (code, ins->dreg, ins->sreg1, (ins->inst_imm & 0x3f));
			break;
		case OP_ISHR_UN_IMM:
			ppc_srwi (code, ins->dreg, ins->sreg1, (ins->inst_imm & 0x1f));
			break;
		case OP_ISHR_UN:
			ppc_srw (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_LSHR_UN:
			ppc_srd (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_INOT:
		case OP_LNOT:
			ppc_not (code, ins->dreg, ins->sreg1);
			break;
		case OP_INEG:
		case OP_LNEG:
			ppc_neg (code, ins->dreg, ins->sreg1);
			break;
		case OP_IMUL:
		case OP_LMUL:
			ppc_mulld (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_IMUL_IMM:
		case OP_MUL_IMM:
			if (ppc_is_imm16 (ins->inst_imm)) {
			    ppc_mulli (code, ins->dreg, ins->sreg1, ins->inst_imm);
			} else {
			    g_assert_not_reached ();
			}
			break;
		case OP_IMUL_OVF:
			/* we annot use mcrxr, since it's not implemented on some processors 
			 * XER format: SO, OV, CA, reserved [21 bits], count [8 bits]
			 */
			ppc_mulldo (code, ins->dreg, ins->sreg1, ins->sreg2);
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1<<14));
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, "OverflowException");
			break;
		case OP_IMUL_OVF_UN:
			/* we first multiply to get the high word and compare to 0
			 * to set the flags, then the result is discarded and then 
			 * we multiply to get the lower * bits result
			 */
			ppc_mulhdu (code, ppc_r0, ins->sreg1, ins->sreg2);
			ppc_cmpi (code, 0, 1, ppc_r0, 0);
			EMIT_COND_SYSTEM_EXCEPTION (CEE_BNE_UN - CEE_BEQ, "OverflowException");
			ppc_mulld (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_ICONST:
		case OP_I8CONST:
			ppc_load (code, ins->dreg, ins->inst_c0);
			break;
		case OP_AOTCONST:
			mono_add_patch_info (cfg, offset, (MonoJumpInfoType)ins->inst_i1, ins->inst_p0);
			ppc_load_sequence (code, ins->dreg, 0);
			break;
		case OP_MOVE:
			ppc_mr (code, ins->dreg, ins->sreg1);
			break;
		case OP_SETLRET: {
			int saved = ins->sreg1;
			if (ins->sreg1 == ppc_r3) {
				ppc_mr (code, ppc_r0, ins->sreg1);
				saved = ppc_r0;
			}
			if (ins->sreg2 != ppc_r3)
				ppc_mr (code, ppc_r3, ins->sreg2);
			if (saved != ppc_r4)
				ppc_mr (code, ppc_r4, saved);
			break;
		}
		case OP_FMOVE:
			ppc_fmr (code, ins->dreg, ins->sreg1);
			break;
		case OP_FCONV_TO_R4:
			ppc_frsp (code, ins->dreg, ins->sreg1);
			break;
		case OP_JMP: {
			int i, pos = 0;
			
			/*
			 * Keep in sync with mono_arch_emit_epilog
			 */
			g_assert (!cfg->method->save_lmf);
			/*
			 * Note: we can use ppc_r11 here because it is dead anyway:
			 * we're leaving the method.
			 */
			if (1 || cfg->flags & MONO_CFG_HAS_CALLS) {
				if (ppc_is_imm16 (cfg->stack_usage + PPC_RET_ADDR_OFFSET)) {
					ppc_load_reg (code, ppc_r0, cfg->stack_usage + PPC_RET_ADDR_OFFSET, cfg->frame_reg);
				} else {
					ppc_load (code, ppc_r11, cfg->stack_usage + PPC_RET_ADDR_OFFSET);
					ppc_load_reg_indexed (code, ppc_r0, cfg->frame_reg, ppc_r11);
				}
				ppc_mtlr (code, ppc_r0);
			}

			code = emit_load_volatile_arguments (cfg, code);

			if (ppc_is_imm16 (cfg->stack_usage)) {
				ppc_addic (code, ppc_sp, cfg->frame_reg, cfg->stack_usage);
			} else {
				ppc_load (code, ppc_r11, cfg->stack_usage);
				ppc_add (code, ppc_sp, cfg->frame_reg, ppc_r11);
			}
			if (!cfg->method->save_lmf) {
				/*for (i = 31; i >= 14; --i) {
					if (cfg->used_float_regs & (1 << i)) {
						pos += sizeof (double);
						ppc_lfd (code, i, -pos, cfg->frame_reg);
					}
				}*/
				/* FIXME: restore registers before changing ppc_sp */
				for (i = MONO_LAST_SAVED_GREG; i >= MONO_FIRST_SAVED_GREG; --i) {
					if (cfg->used_int_regs & (1 << i)) {
						pos += sizeof (gulong);
						ppc_load_reg_indexed (code, i, -pos, ppc_sp);
					}
				}
			} else {
				/* FIXME restore from MonoLMF: though this can't happen yet */
			}
			mono_add_patch_info (cfg, (guint8*) code - cfg->native_code, MONO_PATCH_INFO_METHOD_JUMP, ins->inst_p0);
			ppc_b (code, 0);
			break;
		}
		case OP_CHECK_THIS:
			/* ensure ins->sreg1 is not NULL */
			ppc_load_reg (code, ppc_r0, 0, ins->sreg1);
			break;
		case OP_ARGLIST: {
			if (ppc_is_imm16 (cfg->sig_cookie + cfg->stack_usage)) {
				ppc_addi (code, ppc_r0, cfg->frame_reg, cfg->sig_cookie + cfg->stack_usage);
			} else {
				ppc_load (code, ppc_r0, cfg->sig_cookie + cfg->stack_usage);
				ppc_add (code, ppc_r0, cfg->frame_reg, ppc_r0);
			}
			ppc_store_reg (code, ppc_r0, 0, ins->sreg1);
			break;
		}
		case OP_FCALL:
		case OP_LCALL:
		case OP_VCALL:
		case OP_VCALL2:
		case OP_VOIDCALL:
		case OP_CALL:
			call = (MonoCallInst*)ins;
			if (ins->flags & MONO_INST_HAS_METHOD)
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_METHOD, call->method);
			else
				mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_ABS, call->fptr);
			if (FORCE_INDIR_CALL || cfg->method->dynamic) {
				ppc_load_func (code, ppc_r0, 0);
				ppc_mtlr (code, ppc_r0);
				ppc_blrl (code);
			} else {
				ppc_bl (code, 0);
			}
			/* FIXME: this should be handled somewhere else in the new jit */
			code = emit_move_return_value (cfg, ins, code);
			break;
		case OP_FCALL_REG:
		case OP_LCALL_REG:
		case OP_VCALL_REG:
		case OP_VCALL2_REG:
		case OP_VOIDCALL_REG:
		case OP_CALL_REG:
			ppc_load_reg (code, ppc_r0, 0, ins->sreg1);
			/* FIXME: if we know that this is a method, we
			   can omit this load */
			ppc_load_reg (code, ppc_r2, 8, ins->sreg1);
			ppc_mtlr (code, ppc_r0);
			ppc_blrl (code);
			/* FIXME: this should be handled somewhere else in the new jit */
			code = emit_move_return_value (cfg, ins, code);
			break;
		case OP_FCALL_MEMBASE:
		case OP_LCALL_MEMBASE:
		case OP_VCALL_MEMBASE:
		case OP_VCALL2_MEMBASE:
		case OP_VOIDCALL_MEMBASE:
		case OP_CALL_MEMBASE:
			ppc_load_reg (code, ppc_r0, ins->inst_offset, ins->sreg1);
			ppc_mtlr (code, ppc_r0);
			ppc_blrl (code);
			/* FIXME: this should be handled somewhere else in the new jit */
			code = emit_move_return_value (cfg, ins, code);
			break;
		case OP_LOCALLOC: {
			guint8 * zero_loop_jump, * zero_loop_start;
			/* keep alignment */
			int alloca_waste = PPC_STACK_PARAM_OFFSET + cfg->param_area + 31;
			int area_offset = alloca_waste;
			area_offset &= ~31;
			ppc_addi (code, ppc_r11, ins->sreg1, alloca_waste + 31);
			/* FIXME: should be calculated from MONO_ARCH_FRAME_ALIGNMENT */
			ppc_clrrdi (code, ppc_r11, ppc_r11, 4);
			/* use ctr to store the number of words to 0 if needed */
			if (ins->flags & MONO_INST_INIT) {
				/* we zero 4 bytes at a time:
				 * we add 7 instead of 3 so that we set the counter to
				 * at least 1, otherwise the bdnz instruction will make
				 * it negative and iterate billions of times.
				 */
				ppc_addi (code, ppc_r0, ins->sreg1, 7);
				ppc_sradi (code, ppc_r0, ppc_r0, 2);
				ppc_mtctr (code, ppc_r0);
			}
			ppc_load_reg (code, ppc_r0, 0, ppc_sp);
			ppc_neg (code, ppc_r11, ppc_r11);
			ppc_store_reg_update_indexed (code, ppc_r0, ppc_sp, ppc_r11);

			/* FIXME: make this loop work in 8 byte increments */
			if (ins->flags & MONO_INST_INIT) {
				/* adjust the dest reg by -4 so we can use stwu */
				/* we actually adjust -8 because we let the loop
				 * run at least once
				 */
				ppc_addi (code, ins->dreg, ppc_sp, (area_offset - 8));
				ppc_li (code, ppc_r11, 0);
				zero_loop_start = code;
				ppc_stwu (code, ppc_r11, 4, ins->dreg);
				zero_loop_jump = code;
				ppc_bc (code, PPC_BR_DEC_CTR_NONZERO, 0, 0);
				ppc_patch (zero_loop_jump, zero_loop_start);
			}
			ppc_addi (code, ins->dreg, ppc_sp, area_offset);
			break;
		}
		case OP_THROW: {
			//ppc_break (code);
			ppc_mr (code, ppc_r3, ins->sreg1);
			mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_INTERNAL_METHOD,
					     (gpointer)"mono_arch_throw_exception");
			if (FORCE_INDIR_CALL || cfg->method->dynamic) {
				ppc_load_func (code, ppc_r0, 0);
				ppc_mtlr (code, ppc_r0);
				ppc_blrl (code);
			} else {
				ppc_bl (code, 0);
			}
			break;
		}
		case OP_RETHROW: {
			//ppc_break (code);
			ppc_mr (code, ppc_r3, ins->sreg1);
			mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_INTERNAL_METHOD,
					     (gpointer)"mono_arch_rethrow_exception");
			if (FORCE_INDIR_CALL || cfg->method->dynamic) {
				ppc_load_func (code, ppc_r0, 0);
				ppc_mtlr (code, ppc_r0);
				ppc_blrl (code);
			} else {
				ppc_bl (code, 0);
			}
			break;
		}
		case OP_START_HANDLER: {
			MonoInst *spvar = mono_find_spvar_for_region (cfg, bb->region);
			g_assert (spvar->inst_basereg != ppc_sp);
			code = emit_reserve_param_area (cfg, code);
			ppc_mflr (code, ppc_r0);
			if (ppc_is_imm16 (spvar->inst_offset)) {
				ppc_store_reg (code, ppc_r0, spvar->inst_offset, spvar->inst_basereg);
			} else {
				ppc_load (code, ppc_r11, spvar->inst_offset);
				ppc_store_reg_indexed (code, ppc_r0, ppc_r11, spvar->inst_basereg);
			}
			break;
		}
		case OP_ENDFILTER: {
			MonoInst *spvar = mono_find_spvar_for_region (cfg, bb->region);
			g_assert (spvar->inst_basereg != ppc_sp);
			code = emit_unreserve_param_area (cfg, code);
			if (ins->sreg1 != ppc_r3)
				ppc_mr (code, ppc_r3, ins->sreg1);
			if (ppc_is_imm16 (spvar->inst_offset)) {
				ppc_load_reg (code, ppc_r0, spvar->inst_offset, spvar->inst_basereg);
			} else {
				ppc_load (code, ppc_r11, spvar->inst_offset);
				ppc_load_reg_indexed (code, ppc_r0, spvar->inst_basereg, ppc_r11);
			}
			ppc_mtlr (code, ppc_r0);
			ppc_blr (code);
			break;
		}
		case OP_ENDFINALLY: {
			MonoInst *spvar = mono_find_spvar_for_region (cfg, bb->region);
			g_assert (spvar->inst_basereg != ppc_sp);
			code = emit_unreserve_param_area (cfg, code);
			ppc_load_reg (code, ppc_r0, spvar->inst_offset, spvar->inst_basereg);
			ppc_mtlr (code, ppc_r0);
			ppc_blr (code);
			break;
		}
		case OP_CALL_HANDLER: 
			mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_BB, ins->inst_target_bb);
			ppc_bl (code, 0);
			break;
		case OP_LABEL:
			ins->inst_c0 = code - cfg->native_code;
			break;
		case OP_BR:
			if (ins->flags & MONO_INST_BRLABEL) {
				/*if (ins->inst_i0->inst_c0) {
					ppc_b (code, 0);
					//x86_jump_code (code, cfg->native_code + ins->inst_i0->inst_c0);
				} else*/ {
					mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_LABEL, ins->inst_i0);
					ppc_b (code, 0);
				}
			} else {
				/*if (ins->inst_target_bb->native_offset) {
					ppc_b (code, 0);
					//x86_jump_code (code, cfg->native_code + ins->inst_target_bb->native_offset); 
				} else*/ {
					mono_add_patch_info (cfg, offset, MONO_PATCH_INFO_BB, ins->inst_target_bb);
					ppc_b (code, 0);
				} 
			}
			break;
		case OP_BR_REG:
			ppc_mtctr (code, ins->sreg1);
			ppc_bcctr (code, PPC_BR_ALWAYS, 0);
			break;
		case OP_CEQ:
		case OP_ICEQ:
		case OP_LCEQ:
			ppc_li (code, ins->dreg, 0);
			ppc_bc (code, PPC_BR_FALSE, PPC_BR_EQ, 2);
			ppc_li (code, ins->dreg, 1);
			break;
		case OP_CLT:
		case OP_CLT_UN:
		case OP_ICLT:
		case OP_ICLT_UN:
		case OP_LCLT:
		case OP_LCLT_UN:
			ppc_li (code, ins->dreg, 1);
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_LT, 2);
			ppc_li (code, ins->dreg, 0);
			break;
		case OP_CGT:
		case OP_CGT_UN:
		case OP_ICGT:
		case OP_ICGT_UN:
		case OP_LCGT:
		case OP_LCGT_UN:
			ppc_li (code, ins->dreg, 1);
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_GT, 2);
			ppc_li (code, ins->dreg, 0);
			break;
		case OP_COND_EXC_EQ:
		case OP_COND_EXC_NE_UN:
		case OP_COND_EXC_LT:
		case OP_COND_EXC_LT_UN:
		case OP_COND_EXC_GT:
		case OP_COND_EXC_GT_UN:
		case OP_COND_EXC_GE:
		case OP_COND_EXC_GE_UN:
		case OP_COND_EXC_LE:
		case OP_COND_EXC_LE_UN:
			EMIT_COND_SYSTEM_EXCEPTION (ins->opcode - OP_COND_EXC_EQ, ins->inst_p1);
			break;
		case OP_COND_EXC_IEQ:
		case OP_COND_EXC_INE_UN:
		case OP_COND_EXC_ILT:
		case OP_COND_EXC_ILT_UN:
		case OP_COND_EXC_IGT:
		case OP_COND_EXC_IGT_UN:
		case OP_COND_EXC_IGE:
		case OP_COND_EXC_IGE_UN:
		case OP_COND_EXC_ILE:
		case OP_COND_EXC_ILE_UN:
			EMIT_COND_SYSTEM_EXCEPTION (ins->opcode - OP_COND_EXC_IEQ, ins->inst_p1);
			break;
		case OP_COND_EXC_C:
			/* check XER [0-3] (SO, OV, CA): we can't use mcrxr
			 */
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1 << 13)); /* CA */
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, ins->inst_p1);
			break;
		case OP_COND_EXC_OV:
			ppc_mfspr (code, ppc_r0, ppc_xer);
			ppc_andisd (code, ppc_r0, ppc_r0, (1 << 14)); /* OV */
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_FALSE, PPC_BR_EQ, ins->inst_p1);
			break;
		case OP_COND_EXC_NC:
		case OP_COND_EXC_NO:
			g_assert_not_reached ();
			break;
		case OP_IBEQ:
		case OP_IBNE_UN:
		case OP_IBLT:
		case OP_IBLT_UN:
		case OP_IBGT:
		case OP_IBGT_UN:
		case OP_IBGE:
		case OP_IBGE_UN:
		case OP_IBLE:
		case OP_IBLE_UN:
		case OP_LBEQ:
		case OP_LBNE_UN:
		case OP_LBLT:
		case OP_LBLT_UN:
		case OP_LBGT:
		case OP_LBGT_UN:
		case OP_LBGE:
		case OP_LBGE_UN:
		case OP_LBLE:
		case OP_LBLE_UN:
			EMIT_COND_BRANCH (ins, ins->opcode -
	  			((ins->opcode >= OP_LBEQ && ins->opcode <= OP_LBLT_UN) ? OP_LBEQ : OP_IBEQ));
			break;

		/* floating point opcodes */
		case OP_R8CONST:
		case OP_R4CONST:
			g_assert_not_reached ();
		case OP_STORER8_MEMBASE_REG:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_stfd (code, ins->sreg1, ins->inst_offset, ins->inst_destbasereg);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_stfdx (code, ins->sreg1, ins->inst_destbasereg, ppc_r0);
			}
			break;
		case OP_LOADR8_MEMBASE:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_lfd (code, ins->dreg, ins->inst_offset, ins->inst_basereg);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_lfdx (code, ins->dreg, ins->inst_destbasereg, ppc_r0);
			}
			break;
		case OP_STORER4_MEMBASE_REG:
			ppc_frsp (code, ins->sreg1, ins->sreg1);
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_stfs (code, ins->sreg1, ins->inst_offset, ins->inst_destbasereg);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_stfsx (code, ins->sreg1, ins->inst_destbasereg, ppc_r0);
			}
			break;
		case OP_LOADR4_MEMBASE:
			if (ppc_is_imm16 (ins->inst_offset)) {
				ppc_lfs (code, ins->dreg, ins->inst_offset, ins->inst_basereg);
			} else {
				ppc_load (code, ppc_r0, ins->inst_offset);
				ppc_lfsx (code, ins->dreg, ins->inst_destbasereg, ppc_r0);
			}
			break;
		case OP_LOADR4_MEMINDEX:
			ppc_lfsx (code, ins->dreg, ins->sreg2, ins->inst_basereg);
			break;
		case OP_LOADR8_MEMINDEX:
			ppc_lfdx (code, ins->dreg, ins->sreg2, ins->inst_basereg);
			break;
		case OP_STORER4_MEMINDEX:
			ppc_frsp (code, ins->sreg1, ins->sreg1);
			ppc_stfsx (code, ins->sreg1, ins->sreg2, ins->inst_destbasereg);
			break;
		case OP_STORER8_MEMINDEX:
			ppc_stfdx (code, ins->sreg1, ins->sreg2, ins->inst_destbasereg);
			break;
		case CEE_CONV_R_UN:
		case CEE_CONV_R4: /* FIXME: change precision */
		case CEE_CONV_R8:
			g_assert_not_reached ();
		case OP_FCONV_TO_I1:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 1, TRUE);
			break;
		case OP_FCONV_TO_U1:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 1, FALSE);
			break;
		case OP_FCONV_TO_I2:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 2, TRUE);
			break;
		case OP_FCONV_TO_U2:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 2, FALSE);
			break;
		case OP_FCONV_TO_I4:
		case OP_FCONV_TO_I:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 4, TRUE);
			break;
		case OP_FCONV_TO_U4:
		case OP_FCONV_TO_U:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 4, FALSE);
			break;
		case OP_FCONV_TO_I8:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 8, TRUE);
			break;
		case OP_FCONV_TO_U8:
			code = emit_float_to_int (cfg, code, ins->dreg, ins->sreg1, 8, FALSE);
			break;
		case OP_LCONV_TO_R_UN:
			g_assert_not_reached ();
			/* Implemented as helper calls */
			break;
		case OP_LCONV_TO_OVF_I4_2:
		case OP_LCONV_TO_OVF_I: {
			guint8 *negative_branch, *msword_positive_branch, *msword_negative_branch, *ovf_ex_target;
			g_assert_not_reached (); /* FIXME: L in cmps */
			// Check if its negative
			ppc_cmpi (code, 0, 0, ins->sreg1, 0);
			negative_branch = code;
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_LT, 0);
			// Its positive msword == 0
			ppc_cmpi (code, 0, 0, ins->sreg2, 0);
			msword_positive_branch = code;
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_EQ, 0);

			ovf_ex_target = code;
			EMIT_COND_SYSTEM_EXCEPTION_FLAGS (PPC_BR_ALWAYS, 0, "OverflowException");
			// Negative
			ppc_patch (negative_branch, code);
			ppc_cmpi (code, 0, 0, ins->sreg2, -1);
			msword_negative_branch = code;
			ppc_bc (code, PPC_BR_FALSE, PPC_BR_EQ, 0);
			ppc_patch (msword_negative_branch, ovf_ex_target);
			
			ppc_patch (msword_positive_branch, code);
			if (ins->dreg != ins->sreg1)
				ppc_mr (code, ins->dreg, ins->sreg1);
			break;
		}
		case OP_SQRT:
			ppc_fsqrtd (code, ins->dreg, ins->sreg1);
			break;
		case OP_FADD:
			ppc_fadd (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;
		case OP_FSUB:
			ppc_fsub (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;		
		case OP_FMUL:
			ppc_fmul (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;		
		case OP_FDIV:
			ppc_fdiv (code, ins->dreg, ins->sreg1, ins->sreg2);
			break;		
		case OP_FNEG:
			ppc_fneg (code, ins->dreg, ins->sreg1);
			break;		
		case OP_FREM:
			/* emulated */
			g_assert_not_reached ();
			break;
		case OP_FCOMPARE:
			ppc_fcmpu (code, 0, ins->sreg1, ins->sreg2);
			break;
		case OP_FCEQ:
			ppc_fcmpo (code, 0, ins->sreg1, ins->sreg2);
			ppc_li (code, ins->dreg, 0);
			ppc_bc (code, PPC_BR_FALSE, PPC_BR_EQ, 2);
			ppc_li (code, ins->dreg, 1);
			break;
		case OP_FCLT:
			ppc_fcmpo (code, 0, ins->sreg1, ins->sreg2);
			ppc_li (code, ins->dreg, 1);
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_LT, 2);
			ppc_li (code, ins->dreg, 0);
			break;
		case OP_FCLT_UN:
			ppc_fcmpu (code, 0, ins->sreg1, ins->sreg2);
			ppc_li (code, ins->dreg, 1);
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_SO, 3);
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_LT, 2);
			ppc_li (code, ins->dreg, 0);
			break;
		case OP_FCGT:
			ppc_fcmpo (code, 0, ins->sreg1, ins->sreg2);
			ppc_li (code, ins->dreg, 1);
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_GT, 2);
			ppc_li (code, ins->dreg, 0);
			break;
		case OP_FCGT_UN:
			ppc_fcmpu (code, 0, ins->sreg1, ins->sreg2);
			ppc_li (code, ins->dreg, 1);
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_SO, 3);
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_GT, 2);
			ppc_li (code, ins->dreg, 0);
			break;
		case OP_FBEQ:
			EMIT_COND_BRANCH (ins, CEE_BEQ - CEE_BEQ);
			break;
		case OP_FBNE_UN:
			EMIT_COND_BRANCH (ins, CEE_BNE_UN - CEE_BEQ);
			break;
		case OP_FBLT:
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_SO, 2);
			EMIT_COND_BRANCH (ins, CEE_BLT - CEE_BEQ);
			break;
		case OP_FBLT_UN:
			EMIT_COND_BRANCH_FLAGS (ins, PPC_BR_TRUE, PPC_BR_SO);
			EMIT_COND_BRANCH (ins, CEE_BLT_UN - CEE_BEQ);
			break;
		case OP_FBGT:
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_SO, 2);
			EMIT_COND_BRANCH (ins, CEE_BGT - CEE_BEQ);
			break;
		case OP_FBGT_UN:
			EMIT_COND_BRANCH_FLAGS (ins, PPC_BR_TRUE, PPC_BR_SO);
			EMIT_COND_BRANCH (ins, CEE_BGT_UN - CEE_BEQ);
			break;
		case OP_FBGE:
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_SO, 2);
			EMIT_COND_BRANCH (ins, CEE_BGE - CEE_BEQ);
			break;
		case OP_FBGE_UN:
			EMIT_COND_BRANCH (ins, CEE_BGE_UN - CEE_BEQ);
			break;
		case OP_FBLE:
			ppc_bc (code, PPC_BR_TRUE, PPC_BR_SO, 2);
			EMIT_COND_BRANCH (ins, CEE_BLE - CEE_BEQ);
			break;
		case OP_FBLE_UN:
			EMIT_COND_BRANCH (ins, CEE_BLE_UN - CEE_BEQ);
			break;
		case OP_CKFINITE:
			g_assert_not_reached ();
		case OP_CHECK_FINITE: {
			ppc_rlwinm (code, ins->sreg1, ins->sreg1, 0, 1, 31);
			ppc_addis (code, ins->sreg1, ins->sreg1, -32752);
			ppc_rlwinmd (code, ins->sreg1, ins->sreg1, 1, 31, 31);
			EMIT_COND_SYSTEM_EXCEPTION (CEE_BEQ - CEE_BEQ, "ArithmeticException");
			break;
		case OP_JUMP_TABLE:
			mono_add_patch_info (cfg, offset, (MonoJumpInfoType)ins->inst_i1, ins->inst_p0);
			ppc_load_sequence (code, ins->dreg, 0x0f0f0f0f0f0f0f0fL);
			break;
		}
		default:
			g_warning ("unknown opcode %s in %s()\n", mono_inst_name (ins->opcode), __FUNCTION__);
			g_assert_not_reached ();
		}

		if ((cfg->opt & MONO_OPT_BRANCH) && ((code - cfg->native_code - offset) > max_len)) {
			g_warning ("wrong maximal instruction length of instruction %s (expected %d, got %ld)",
				   mono_inst_name (ins->opcode), max_len, code - cfg->native_code - offset);
			g_assert_not_reached ();
		}
	       
		cpos += max_len;

		last_ins = ins;
		last_offset = offset;
	}

	cfg->code_len = code - cfg->native_code;
}

void
mono_arch_register_lowlevel_calls (void)
{
}

#define patch_lis_ori(ip,val) do {\
		guint16 *__lis_ori = (guint16*)(ip);	\
		__lis_ori [1] = (((gulong)(val)) >> 16) & 0xffff;	\
		__lis_ori [3] = ((gulong)(val)) & 0xffff;	\
	} while (0)
#define patch_load_sequence(ip,val) do {\
		guint16 *__load = (guint16*)(ip);	\
		__load [1] = (((guint64)(val)) >> 48) & 0xffff;	\
		__load [3] = (((guint64)(val)) >> 32) & 0xffff;	\
		__load [7] = (((guint64)(val)) >> 16) & 0xffff;	\
		__load [9] =  ((guint64)(val))        & 0xffff;	\
	} while (0)

void
mono_arch_patch_code (MonoMethod *method, MonoDomain *domain, guint8 *code, MonoJumpInfo *ji, gboolean run_cctors)
{
	MonoJumpInfo *patch_info;

	for (patch_info = ji; patch_info; patch_info = patch_info->next) {
		unsigned char *ip = patch_info->ip.i + code;
		unsigned char *target;
		gboolean is_fd = FALSE;

		target = mono_resolve_patch_target (method, domain, code, patch_info, run_cctors);

		//g_print ("patching %p to %p (type %d)\n", ip, target, patch_info->type);

		switch (patch_info->type) {
		case MONO_PATCH_INFO_IP:
			patch_load_sequence (ip, ip);
			continue;
		case MONO_PATCH_INFO_METHOD_REL:
			g_assert_not_reached ();
			*((gpointer *)(ip)) = code + patch_info->data.offset;
			continue;
		case MONO_PATCH_INFO_SWITCH: {
			gpointer *table = (gpointer *)patch_info->data.table->table;
			int i;

			patch_load_sequence (ip, table);

			for (i = 0; i < patch_info->data.table->table_size; i++) {
				table [i] = (glong)patch_info->data.table->table [i] + code;
			}
			/* we put into the table the absolute address, no need for ppc_patch in this case */
			continue;
		}
		case MONO_PATCH_INFO_METHODCONST:
		case MONO_PATCH_INFO_CLASS:
		case MONO_PATCH_INFO_IMAGE:
		case MONO_PATCH_INFO_FIELD:
		case MONO_PATCH_INFO_VTABLE:
		case MONO_PATCH_INFO_IID:
		case MONO_PATCH_INFO_SFLDA:
		case MONO_PATCH_INFO_LDSTR:
		case MONO_PATCH_INFO_TYPE_FROM_HANDLE:
		case MONO_PATCH_INFO_LDTOKEN:
			/* from OP_AOTCONST : lis + ori */
			patch_load_sequence (ip, target);
			continue;
		case MONO_PATCH_INFO_R4:
		case MONO_PATCH_INFO_R8:
			g_assert_not_reached ();
			*((gconstpointer *)(ip + 2)) = patch_info->data.target;
			continue;
		case MONO_PATCH_INFO_EXC_NAME:
			g_assert_not_reached ();
			*((gconstpointer *)(ip + 1)) = patch_info->data.name;
			continue;
		case MONO_PATCH_INFO_NONE:
		case MONO_PATCH_INFO_BB_OVF:
		case MONO_PATCH_INFO_EXC_OVF:
			/* everything is dealt with at epilog output time */
			continue;
		case MONO_PATCH_INFO_INTERNAL_METHOD:
		case MONO_PATCH_INFO_ABS:
		case MONO_PATCH_INFO_CLASS_INIT:
			is_fd = TRUE;
			break;
		default:
			break;
		}
		ppc_patch_full (ip, target, is_fd);
	}
}

/*
 * Stack frame layout:
 * 
 *   ------------------- sp
 *   	MonoLMF structure or saved registers
 *   -------------------
 *   	spilled regs
 *   -------------------
 *   	locals
 *   -------------------
 *   	optional 8 bytes for tracing
 *   -------------------
 *   	param area             size is cfg->param_area
 *   -------------------
 *   	linkage area           size is PPC_STACK_PARAM_OFFSET
 *   ------------------- sp
 *   	red zone
 */
guint8 *
mono_arch_emit_prolog (MonoCompile *cfg)
{
	MonoMethod *method = cfg->method;
	MonoBasicBlock *bb;
	MonoMethodSignature *sig;
	MonoInst *inst;
	int alloc_size, pos, max_offset, i;
	guint8 *code;
	CallInfo *cinfo;
	int tracing = 0;
	int lmf_offset = 0;
	int tailcall_struct_index;

	if (mono_jit_trace_calls != NULL && mono_trace_eval (method))
		tracing = 1;

	sig = mono_method_signature (method);
	cfg->code_size = 384 + sig->param_count * 20;
	code = cfg->native_code = g_malloc (cfg->code_size);

	if (1 || cfg->flags & MONO_CFG_HAS_CALLS) {
		ppc_mflr (code, ppc_r0);
		ppc_store_reg (code, ppc_r0, PPC_RET_ADDR_OFFSET, ppc_sp);
	}

	alloc_size = cfg->stack_offset;
	pos = 0;

	if (!method->save_lmf) {
		/*for (i = 31; i >= 14; --i) {
			if (cfg->used_float_regs & (1 << i)) {
				pos += sizeof (gdouble);
				ppc_stfd (code, i, -pos, ppc_sp);
			}
		}*/
		for (i = MONO_LAST_SAVED_GREG; i >= MONO_FIRST_SAVED_GREG; --i) {
			if (cfg->used_int_regs & (1 << i)) {
				pos += sizeof (gulong);
				ppc_store_reg (code, i, -pos, ppc_sp);
			}
		}
	} else {
		pos += sizeof (MonoLMF);
		lmf_offset = pos;
		for (i = MONO_FIRST_SAVED_GREG; i <= MONO_LAST_SAVED_GREG; i++) {
			ppc_store_reg (code, i, (-pos + G_STRUCT_OFFSET(MonoLMF, iregs) +
				((i-MONO_FIRST_SAVED_GREG) * sizeof (gulong))), ppc_r1);
		}
		for (i = MONO_FIRST_SAVED_FREG; i <= MONO_LAST_SAVED_FREG; i++) {
			ppc_stfd (code, i, (-pos + G_STRUCT_OFFSET(MonoLMF, fregs) +
				((i-MONO_FIRST_SAVED_FREG) * sizeof (gdouble))), ppc_r1);
		}
	}
	alloc_size += pos;
	// align to MONO_ARCH_FRAME_ALIGNMENT bytes
	if (alloc_size & (MONO_ARCH_FRAME_ALIGNMENT - 1)) {
		alloc_size += MONO_ARCH_FRAME_ALIGNMENT - 1;
		alloc_size &= ~(MONO_ARCH_FRAME_ALIGNMENT - 1);
	}

	cfg->stack_usage = alloc_size;
	g_assert ((alloc_size & (MONO_ARCH_FRAME_ALIGNMENT-1)) == 0);
	if (alloc_size) {
		if (ppc_is_imm16 (-alloc_size)) {
			ppc_store_reg_update (code, ppc_sp, -alloc_size, ppc_sp);
		} else {
			ppc_load (code, ppc_r11, -alloc_size);
			ppc_store_reg_update_indexed (code, ppc_sp, ppc_sp, ppc_r11);
		}
	}
	if (cfg->frame_reg != ppc_sp)
		ppc_mr (code, cfg->frame_reg, ppc_sp);

	/* store runtime generic context */
	if (cfg->rgctx_var) {
		g_assert (cfg->rgctx_var->opcode == OP_REGOFFSET &&
				(cfg->rgctx_var->inst_basereg == ppc_r1 || cfg->rgctx_var->inst_basereg == ppc_r31));

		ppc_store_reg (code, MONO_ARCH_RGCTX_REG, cfg->rgctx_var->inst_offset, cfg->rgctx_var->inst_basereg);
	}

        /* compute max_offset in order to use short forward jumps
	 * we always do it on ppc because the immediate displacement
	 * for jumps is too small 
	 */
	max_offset = 0;
	for (bb = cfg->bb_entry; bb; bb = bb->next_bb) {
		MonoInst *ins;
		bb->max_offset = max_offset;

		if (cfg->prof_options & MONO_PROFILE_COVERAGE)
			max_offset += 6; 

		MONO_BB_FOR_EACH_INS (bb, ins)
			max_offset += ins_native_length (cfg, ins);
	}

	/* load arguments allocated to register from the stack */
	pos = 0;

	cinfo = calculate_sizes (sig, sig->pinvoke);

	if (MONO_TYPE_ISSTRUCT (sig->ret)) {
		ArgInfo *ainfo = &cinfo->ret;

		inst = cfg->vret_addr;
		g_assert (inst);

		if (ppc_is_imm16 (inst->inst_offset)) {
			ppc_store_reg (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
		} else {
			ppc_load (code, ppc_r11, inst->inst_offset);
			ppc_store_reg_indexed (code, ainfo->reg, ppc_r11, inst->inst_basereg);
		}
	}

	tailcall_struct_index = 0;
	for (i = 0; i < sig->param_count + sig->hasthis; ++i) {
		ArgInfo *ainfo = cinfo->args + i;
		inst = cfg->args [pos];
		
		if (cfg->verbose_level > 2)
			g_print ("Saving argument %d (type: %d)\n", i, ainfo->regtype);
		if (inst->opcode == OP_REGVAR) {
			if (ainfo->regtype == RegTypeGeneral)
				ppc_mr (code, inst->dreg, ainfo->reg);
			else if (ainfo->regtype == RegTypeFP)
				ppc_fmr (code, inst->dreg, ainfo->reg);
			else if (ainfo->regtype == RegTypeBase) {
				ppc_load_reg (code, ppc_r11, 0, ppc_sp);
				ppc_load_reg (code, inst->dreg, ainfo->offset, ppc_r11);
			} else
				g_assert_not_reached ();

			if (cfg->verbose_level > 2)
				g_print ("Argument %d assigned to register %s\n", pos, mono_arch_regname (inst->dreg));
		} else {
			/* the argument should be put on the stack: FIXME handle size != word  */
			if (ainfo->regtype == RegTypeGeneral) {
				switch (ainfo->size) {
				case 1:
					if (ppc_is_imm16 (inst->inst_offset)) {
						ppc_stb (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
					} else {
						ppc_load (code, ppc_r11, inst->inst_offset);
						ppc_stbx (code, ainfo->reg, ppc_r11, inst->inst_basereg);
					}
					break;
				case 2:
					if (ppc_is_imm16 (inst->inst_offset)) {
						ppc_sth (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
					} else {
						ppc_load (code, ppc_r11, inst->inst_offset);
						ppc_sthx (code, ainfo->reg, ppc_r11, inst->inst_basereg);
					}
					break;
				case 4:
					if (ppc_is_imm16 (inst->inst_offset)) {
						ppc_stw (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
					} else {
						ppc_load (code, ppc_r11, inst->inst_offset);
						ppc_stwx (code, ainfo->reg, ppc_r11, inst->inst_basereg);
					}
					break;
				default:
					if (ppc_is_imm16 (inst->inst_offset)) {
						ppc_store_reg (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
					} else {
						ppc_load (code, ppc_r11, inst->inst_offset);
						ppc_store_reg_indexed (code, ainfo->reg, ppc_r11, inst->inst_basereg);
					}
					break;
				}
			} else if (ainfo->regtype == RegTypeBase) {
				/* load the previous stack pointer in r11 */
				ppc_load_reg (code, ppc_r11, 0, ppc_sp);
				ppc_load_reg (code, ppc_r0, ainfo->offset, ppc_r11);
				switch (ainfo->size) {
				case 1:
					if (ppc_is_imm16 (inst->inst_offset)) {
						ppc_stb (code, ppc_r0, inst->inst_offset, inst->inst_basereg);
					} else {
						ppc_load (code, ppc_r11, inst->inst_offset);
						ppc_stbx (code, ppc_r0, ppc_r11, inst->inst_basereg);
					}
					break;
				case 2:
					if (ppc_is_imm16 (inst->inst_offset)) {
						ppc_sth (code, ppc_r0, inst->inst_offset, inst->inst_basereg);
					} else {
						ppc_load (code, ppc_r11, inst->inst_offset);
						ppc_sthx (code, ppc_r0, ppc_r11, inst->inst_basereg);
					}
					break;
				case 4:
					if (ppc_is_imm16 (inst->inst_offset)) {
						ppc_stw (code, ppc_r0, inst->inst_offset, inst->inst_basereg);
					} else {
						ppc_load (code, ppc_r11, inst->inst_offset);
						ppc_stwx (code, ppc_r0, ppc_r11, inst->inst_basereg);
					}
					break;
				default:
					if (ppc_is_imm16 (inst->inst_offset)) {
						ppc_store_reg (code, ppc_r0, inst->inst_offset, inst->inst_basereg);
					} else {
						ppc_load (code, ppc_r11, inst->inst_offset);
						ppc_store_reg_indexed (code, ppc_r0, ppc_r11, inst->inst_basereg);
					}
					break;
				}
			} else if (ainfo->regtype == RegTypeFP) {
				g_assert (ppc_is_imm16 (inst->inst_offset));
				if (ainfo->size == 8)
					ppc_stfd (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
				else if (ainfo->size == 4)
					ppc_stfs (code, ainfo->reg, inst->inst_offset, inst->inst_basereg);
				else
					g_assert_not_reached ();
			} else if (ainfo->regtype == RegTypeStructByVal) {
				int doffset = inst->inst_offset;
				int soffset = 0;
				int cur_reg;
				int size = 0;
				g_assert (ppc_is_imm16 (inst->inst_offset));
				g_assert (ppc_is_imm16 (inst->inst_offset + ainfo->size * sizeof (gpointer)));
				/* FIXME: what if there is no class? */
				if (sig->pinvoke && mono_class_from_mono_type (inst->inst_vtype))
					size = mono_class_native_size (mono_class_from_mono_type (inst->inst_vtype), NULL);
				for (cur_reg = 0; cur_reg < ainfo->size; ++cur_reg) {
#if __APPLE__
					/*
					 * Darwin handles 1 and 2 byte
					 * structs specially by
					 * loading h/b into the arg
					 * register.  Only done for
					 * pinvokes.
					 */
					if (size == 2)
						ppc_sth (code, ainfo->reg + cur_reg, doffset, inst->inst_basereg);
					else if (size == 1)
						ppc_stb (code, ainfo->reg + cur_reg, doffset, inst->inst_basereg);
					else
#endif
						ppc_store_reg (code, ainfo->reg + cur_reg, doffset, inst->inst_basereg);
					soffset += sizeof (gpointer);
					doffset += sizeof (gpointer);
				}
				if (ainfo->vtsize) {
					/* load the previous stack pointer in r11 (r0 gets overwritten by the memcpy) */
					ppc_load_reg (code, ppc_r11, 0, ppc_sp);
					if ((size & 7) != 0) {
						code = emit_memcpy (code, size - soffset,
							inst->inst_basereg, doffset,
							ppc_r11, ainfo->offset + soffset);
					} else {
						code = emit_memcpy (code, ainfo->vtsize * sizeof (gpointer),
							inst->inst_basereg, doffset,
							ppc_r11, ainfo->offset + soffset);
					}
				}
			} else if (ainfo->regtype == RegTypeStructByAddr) {
				/* if it was originally a RegTypeBase */
				if (ainfo->offset) {
					/* load the previous stack pointer in r11 */
					ppc_load_reg (code, ppc_r11, 0, ppc_sp);
					ppc_load_reg (code, ppc_r11, ainfo->offset, ppc_r11);
				} else {
					ppc_mr (code, ppc_r11, ainfo->reg);
				}

				if (cfg->tailcall_valuetype_addrs) {
					MonoInst *addr = cfg->tailcall_valuetype_addrs [tailcall_struct_index];

					g_assert (ppc_is_imm16 (addr->inst_offset));
					ppc_store_reg (code, ppc_r11, addr->inst_offset, addr->inst_basereg);

					tailcall_struct_index++;
				}

				g_assert (ppc_is_imm16 (inst->inst_offset));
				code = emit_memcpy (code, ainfo->vtsize, inst->inst_basereg, inst->inst_offset, ppc_r11, 0);
				/*g_print ("copy in %s: %d bytes from %d to offset: %d\n", method->name, ainfo->vtsize, ainfo->reg, inst->inst_offset);*/
			} else
				g_assert_not_reached ();
		}
		pos++;
	}

	if (method->wrapper_type == MONO_WRAPPER_NATIVE_TO_MANAGED) {
		ppc_load (code, ppc_r3, cfg->domain);
		mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_INTERNAL_METHOD, (gpointer)"mono_jit_thread_attach");
		if (FORCE_INDIR_CALL || cfg->method->dynamic) {
			ppc_load_func (code, ppc_r0, 0);
			ppc_mtlr (code, ppc_r0);
			ppc_blrl (code);
		} else {
			ppc_bl (code, 0);
		}
	}

	if (method->save_lmf) {
		if (lmf_pthread_key != -1) {
			emit_tls_access (code, ppc_r3, lmf_pthread_key);
			if (G_STRUCT_OFFSET (MonoJitTlsData, lmf))
				ppc_addi (code, ppc_r3, ppc_r3, G_STRUCT_OFFSET (MonoJitTlsData, lmf));
		} else {
			mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_INTERNAL_METHOD, 
				     (gpointer)"mono_get_lmf_addr");
			if (FORCE_INDIR_CALL || cfg->method->dynamic) {
				ppc_load_func (code, ppc_r0, 0);
				ppc_mtlr (code, ppc_r0);
				ppc_blrl (code);
			} else {
				ppc_bl (code, 0);
			}
		}
		/* we build the MonoLMF structure on the stack - see mini-ppc.h */
		/* lmf_offset is the offset from the previous stack pointer,
		 * alloc_size is the total stack space allocated, so the offset
		 * of MonoLMF from the current stack ptr is alloc_size - lmf_offset.
		 * The pointer to the struct is put in ppc_r11 (new_lmf).
		 * The callee-saved registers are already in the MonoLMF structure
		 */
		ppc_addi (code, ppc_r11, ppc_sp, alloc_size - lmf_offset);
		/* ppc_r3 is the result from mono_get_lmf_addr () */
		ppc_store_reg (code, ppc_r3, G_STRUCT_OFFSET(MonoLMF, lmf_addr), ppc_r11);
		/* new_lmf->previous_lmf = *lmf_addr */
		ppc_load_reg (code, ppc_r0, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r3);
		ppc_store_reg (code, ppc_r0, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r11);
		/* *(lmf_addr) = r11 */
		ppc_store_reg (code, ppc_r11, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r3);
		/* save method info */
		ppc_load (code, ppc_r0, method);
		ppc_store_reg (code, ppc_r0, G_STRUCT_OFFSET(MonoLMF, method), ppc_r11);
		ppc_store_reg (code, ppc_sp, G_STRUCT_OFFSET(MonoLMF, ebp), ppc_r11);
		/* save the current IP */
		mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_IP, NULL);
		ppc_load_sequence (code, ppc_r0, 0x0101010101010101L);
		ppc_store_reg (code, ppc_r0, G_STRUCT_OFFSET(MonoLMF, eip), ppc_r11);
	}

	if (tracing)
		code = mono_arch_instrument_prolog (cfg, mono_trace_enter_method, code, TRUE);

	cfg->code_len = code - cfg->native_code;
	g_assert (cfg->code_len < cfg->code_size);
	g_free (cinfo);

	return code;
}

void
mono_arch_emit_epilog (MonoCompile *cfg)
{
	MonoMethod *method = cfg->method;
	int pos, i;
	int max_epilog_size = 16 + 20*4;
	guint8 *code;

	if (cfg->method->save_lmf)
		max_epilog_size += 128;
	
	if (mono_jit_trace_calls != NULL)
		max_epilog_size += 50;

	if (cfg->prof_options & MONO_PROFILE_ENTER_LEAVE)
		max_epilog_size += 50;

	while (cfg->code_len + max_epilog_size > (cfg->code_size - 16)) {
		cfg->code_size *= 2;
		cfg->native_code = g_realloc (cfg->native_code, cfg->code_size);
		mono_jit_stats.code_reallocs++;
	}

	/*
	 * Keep in sync with OP_JMP
	 */
	code = cfg->native_code + cfg->code_len;

	if (mono_jit_trace_calls != NULL && mono_trace_eval (method)) {
		code = mono_arch_instrument_epilog (cfg, mono_trace_leave_method, code, TRUE);
	}
	pos = 0;

	if (method->save_lmf) {
		int lmf_offset;
		pos +=  sizeof (MonoLMF);
		lmf_offset = pos;
		/* save the frame reg in r8 */
		ppc_mr (code, ppc_r8, cfg->frame_reg);
		ppc_addi (code, ppc_r11, cfg->frame_reg, cfg->stack_usage - lmf_offset);
		/* r5 = previous_lmf */
		ppc_load_reg (code, ppc_r5, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r11);
		/* r6 = lmf_addr */
		ppc_load_reg (code, ppc_r6, G_STRUCT_OFFSET(MonoLMF, lmf_addr), ppc_r11);
		/* *(lmf_addr) = previous_lmf */
		ppc_store_reg (code, ppc_r5, G_STRUCT_OFFSET(MonoLMF, previous_lmf), ppc_r6);
		/* FIXME: speedup: there is no actual need to restore the registers if
		 * we didn't actually change them (idea from Zoltan).
		 */
		/* restore iregs */
		for (i = MONO_FIRST_SAVED_GREG; i <= MONO_LAST_SAVED_FREG; ++i) {
			ppc_load_reg (code, i, G_STRUCT_OFFSET (MonoLMF, iregs) +
				(i - MONO_FIRST_SAVED_GREG) * sizeof (gulong), ppc_r11);
		}
		/* restore fregs */
		/*for (i = 14; i < 32; i++) {
			ppc_lfd (code, i, G_STRUCT_OFFSET(MonoLMF, fregs) + ((i-14) * sizeof (gdouble)), ppc_r11);
		}*/
		g_assert (ppc_is_imm16 (cfg->stack_usage + PPC_RET_ADDR_OFFSET));
		/* use the saved copy of the frame reg in r8 */
		if (1 || cfg->flags & MONO_CFG_HAS_CALLS) {
			ppc_load_reg (code, ppc_r0, cfg->stack_usage + PPC_RET_ADDR_OFFSET, ppc_r8);
			ppc_mtlr (code, ppc_r0);
		}
		ppc_addic (code, ppc_sp, ppc_r8, cfg->stack_usage);
	} else {
		if (1 || cfg->flags & MONO_CFG_HAS_CALLS) {
			if (ppc_is_imm16 (cfg->stack_usage + PPC_RET_ADDR_OFFSET)) {
				ppc_load_reg (code, ppc_r0, cfg->stack_usage + PPC_RET_ADDR_OFFSET, cfg->frame_reg);
			} else {
				ppc_load (code, ppc_r11, cfg->stack_usage + PPC_RET_ADDR_OFFSET);
				ppc_load_reg_indexed (code, ppc_r0, cfg->frame_reg, ppc_r11);
			}
			ppc_mtlr (code, ppc_r0);
		}
		if (ppc_is_imm16 (cfg->stack_usage)) {
			ppc_addic (code, ppc_sp, cfg->frame_reg, cfg->stack_usage);
		} else {
			ppc_load (code, ppc_r11, cfg->stack_usage);
			ppc_add (code, ppc_sp, cfg->frame_reg, ppc_r11);
		}

		/*for (i = 31; i >= 14; --i) {
			if (cfg->used_float_regs & (1 << i)) {
				pos += sizeof (double);
				ppc_lfd (code, i, -pos, ppc_sp);
			}
		}*/
		for (i = MONO_LAST_SAVED_GREG; i >= MONO_FIRST_SAVED_GREG; --i) {
			if (cfg->used_int_regs & (1 << i)) {
				pos += sizeof (gulong);
				ppc_load_reg (code, i, -pos, ppc_sp);
			}
		}
	}
	ppc_blr (code);

	cfg->code_len = code - cfg->native_code;

	g_assert (cfg->code_len < cfg->code_size);

}

/* remove once throw_exception_by_name is eliminated */
static int
exception_id_by_name (const char *name)
{
	if (strcmp (name, "IndexOutOfRangeException") == 0)
		return MONO_EXC_INDEX_OUT_OF_RANGE;
	if (strcmp (name, "OverflowException") == 0)
		return MONO_EXC_OVERFLOW;
	if (strcmp (name, "ArithmeticException") == 0)
		return MONO_EXC_ARITHMETIC;
	if (strcmp (name, "DivideByZeroException") == 0)
		return MONO_EXC_DIVIDE_BY_ZERO;
	if (strcmp (name, "InvalidCastException") == 0)
		return MONO_EXC_INVALID_CAST;
	if (strcmp (name, "NullReferenceException") == 0)
		return MONO_EXC_NULL_REF;
	if (strcmp (name, "ArrayTypeMismatchException") == 0)
		return MONO_EXC_ARRAY_TYPE_MISMATCH;
	g_error ("Unknown intrinsic exception %s\n", name);
	return 0;
}

void
mono_arch_emit_exceptions (MonoCompile *cfg)
{
	MonoJumpInfo *patch_info;
	int i;
	guint8 *code;
	const guint8* exc_throw_pos [MONO_EXC_INTRINS_NUM] = {NULL};
	guint8 exc_throw_found [MONO_EXC_INTRINS_NUM] = {0};
	int max_epilog_size = 50;

	/* count the number of exception infos */
     
	/* 
	 * make sure we have enough space for exceptions
	 * 24 is the simulated call to throw_exception_by_name
	 */
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		if (patch_info->type == MONO_PATCH_INFO_EXC) {
			i = exception_id_by_name (patch_info->data.target);
			if (!exc_throw_found [i]) {
				max_epilog_size += 24;
				exc_throw_found [i] = TRUE;
			}
		} else if (patch_info->type == MONO_PATCH_INFO_BB_OVF)
			max_epilog_size += 12;
		else if (patch_info->type == MONO_PATCH_INFO_EXC_OVF) {
			MonoOvfJump *ovfj = (MonoOvfJump*)patch_info->data.target;
			i = exception_id_by_name (ovfj->data.exception);
			if (!exc_throw_found [i]) {
				max_epilog_size += 24;
				exc_throw_found [i] = TRUE;
			}
			max_epilog_size += 8;
		}
	}

	while (cfg->code_len + max_epilog_size > (cfg->code_size - 16)) {
		cfg->code_size *= 2;
		cfg->native_code = g_realloc (cfg->native_code, cfg->code_size);
		mono_jit_stats.code_reallocs++;
	}

	code = cfg->native_code + cfg->code_len;

	/* add code to raise exceptions */
	for (patch_info = cfg->patch_info; patch_info; patch_info = patch_info->next) {
		switch (patch_info->type) {
		case MONO_PATCH_INFO_BB_OVF: {
			MonoOvfJump *ovfj = (MonoOvfJump*)patch_info->data.target;
			unsigned char *ip = patch_info->ip.i + cfg->native_code;
			/* patch the initial jump */
			ppc_patch (ip, code);
			ppc_bc (code, ovfj->b0_cond, ovfj->b1_cond, 2);
			ppc_b (code, 0);
			ppc_patch (code - 4, ip + 4); /* jump back after the initiali branch */
			/* jump back to the true target */
			ppc_b (code, 0);
			ip = ovfj->data.bb->native_offset + cfg->native_code;
			ppc_patch (code - 4, ip);
			break;
		}
		case MONO_PATCH_INFO_EXC_OVF: {
			MonoOvfJump *ovfj = (MonoOvfJump*)patch_info->data.target;
			MonoJumpInfo *newji;
			unsigned char *ip = patch_info->ip.i + cfg->native_code;
			unsigned char *bcl = code;
			/* patch the initial jump: we arrived here with a call */
			ppc_patch (ip, code);
			ppc_bc (code, ovfj->b0_cond, ovfj->b1_cond, 0);
			ppc_b (code, 0);
			ppc_patch (code - 4, ip + 4); /* jump back after the initiali branch */
			/* patch the conditional jump to the right handler */
			/* make it processed next */
			newji = mono_mempool_alloc (cfg->mempool, sizeof (MonoJumpInfo));
			newji->type = MONO_PATCH_INFO_EXC;
			newji->ip.i = bcl - cfg->native_code;
			newji->data.target = ovfj->data.exception;
			newji->next = patch_info->next;
			patch_info->next = newji;
			break;
		}
		case MONO_PATCH_INFO_EXC: {
			unsigned char *ip = patch_info->ip.i + cfg->native_code;
			i = exception_id_by_name (patch_info->data.target);
			if (exc_throw_pos [i]) {
				ppc_patch (ip, exc_throw_pos [i]);
				patch_info->type = MONO_PATCH_INFO_NONE;
				break;
			} else {
				exc_throw_pos [i] = code;
			}
			ppc_patch (ip, code);
			/*mono_add_patch_info (cfg, code - cfg->native_code, MONO_PATCH_INFO_EXC_NAME, patch_info->data.target);*/
			ppc_load (code, ppc_r3, patch_info->data.target);
			/* we got here from a conditional call, so the calling ip is set in lr already */
			patch_info->type = MONO_PATCH_INFO_INTERNAL_METHOD;
			patch_info->data.name = "mono_arch_throw_exception_by_name";
			patch_info->ip.i = code - cfg->native_code;
			if (FORCE_INDIR_CALL || cfg->method->dynamic) {
				ppc_load_func (code, ppc_r0, 0);
				ppc_mtctr (code, ppc_r0);
				ppc_bcctr (code, PPC_BR_ALWAYS, 0);
			} else {
				ppc_b (code, 0);
			}
			break;
		}
		default:
			/* do nothing */
			break;
		}
	}

	cfg->code_len = code - cfg->native_code;

	g_assert (cfg->code_len < cfg->code_size);

}

static void
setup_tls_access (void)
{
	int ptk;

	/* FIXME */
	tls_mode = TLS_MODE_FAILED;
	return;

	if (monodomain_key == -1) {
		ptk = mono_domain_get_tls_key ();
		if (ptk < 1024) {
			ptk = mono_pthread_key_for_tls (ptk);
			if (ptk < 1024) {
				monodomain_key = ptk;
			}
		}
	}
	if (lmf_pthread_key == -1) {
		ptk = mono_pthread_key_for_tls (mono_jit_tls_id);
		if (ptk < 1024) {
			/*g_print ("MonoLMF at: %d\n", ptk);*/
			/*if (!try_offset_access (mono_get_lmf_addr (), ptk)) {
				init_tls_failed = 1;
				return;
			}*/
			lmf_pthread_key = ptk;
		}
	}
	if (monothread_key == -1) {
		ptk = mono_thread_get_tls_key ();
		if (ptk < 1024) {
			ptk = mono_pthread_key_for_tls (ptk);
			if (ptk < 1024) {
				monothread_key = ptk;
				/*g_print ("thread inited: %d\n", ptk);*/
			}
		} else {
			/*g_print ("thread not inited yet %d\n", ptk);*/
		}
	}
}

void
mono_arch_setup_jit_tls_data (MonoJitTlsData *tls)
{
	setup_tls_access ();
}

void
mono_arch_free_jit_tls_data (MonoJitTlsData *tls)
{
}

#ifdef MONO_ARCH_HAVE_IMT

#define CMP_SIZE 12
#define BR_SIZE 4
#define JUMP_IMM_SIZE 12
#define JUMP_IMM32_SIZE 16
#define ENABLE_WRONG_METHOD_CHECK 0

/*
 * LOCKING: called with the domain lock held
 */
gpointer
mono_arch_build_imt_thunk (MonoVTable *vtable, MonoDomain *domain, MonoIMTCheckItem **imt_entries, int count,
	gpointer fail_tramp)
{
	int i;
	int size = 0;
	guint8 *code, *start;

	for (i = 0; i < count; ++i) {
		MonoIMTCheckItem *item = imt_entries [i];
		if (item->is_equals) {
			if (item->check_target_idx) {
				if (!item->compare_done)
					item->chunk_size += CMP_SIZE;
				if (fail_tramp)
					item->chunk_size += BR_SIZE + JUMP_IMM32_SIZE;
				else
					item->chunk_size += BR_SIZE + JUMP_IMM_SIZE;
			} else {
				if (fail_tramp) {
					item->chunk_size += CMP_SIZE + BR_SIZE + JUMP_IMM32_SIZE * 2;
				} else {
					item->chunk_size += JUMP_IMM_SIZE;
#if ENABLE_WRONG_METHOD_CHECK
					item->chunk_size += CMP_SIZE + BR_SIZE + 4;
#endif
				}
			}
		} else {
			item->chunk_size += CMP_SIZE + BR_SIZE;
			imt_entries [item->check_target_idx]->compare_done = TRUE;
		}
		size += item->chunk_size;
	}
	if (fail_tramp) {
		code = mono_method_alloc_generic_virtual_thunk (domain, size);
	} else {
		/* the initial load of the vtable address */
		size += 8;
		code = mono_code_manager_reserve (domain->code_mp, size);
	}
	start = code;
	if (!fail_tramp)
		ppc_load (code, ppc_r11, (gulong)(& (vtable->vtable [0])));
	for (i = 0; i < count; ++i) {
		MonoIMTCheckItem *item = imt_entries [i];
		item->code_target = code;
		if (item->is_equals) {
			if (item->check_target_idx) {
				if (!item->compare_done) {
					ppc_load (code, ppc_r0, (gulong)item->key);
					ppc_cmpl (code, 0, 1, MONO_ARCH_IMT_REG, ppc_r0);
				}
				item->jmp_code = code;
				ppc_bc (code, PPC_BR_FALSE, PPC_BR_EQ, 0);
				if (fail_tramp)
					ppc_load (code, ppc_r0, item->value.target_code);
				else
					ppc_load_reg (code, ppc_r0, (sizeof (gpointer) * item->value.vtable_slot), ppc_r11);
				ppc_mtctr (code, ppc_r0);
				ppc_bcctr (code, PPC_BR_ALWAYS, 0);
			} else {
				if (fail_tramp) {
					ppc_load (code, ppc_r0, (gulong)item->key);
					ppc_cmpl (code, 0, 1, MONO_ARCH_IMT_REG, ppc_r0);
					item->jmp_code = code;
					ppc_bc (code, PPC_BR_FALSE, PPC_BR_EQ, 0);
					ppc_load (code, ppc_r0, item->value.target_code);
					ppc_mtctr (code, ppc_r0);
					ppc_bcctr (code, PPC_BR_ALWAYS, 0);
					ppc_patch (item->jmp_code, code);
					ppc_load (code, ppc_r0, fail_tramp);
					ppc_mtctr (code, ppc_r0);
					ppc_bcctr (code, PPC_BR_ALWAYS, 0);
					item->jmp_code = NULL;
				} else {
					/* enable the commented code to assert on wrong method */
#if ENABLE_WRONG_METHOD_CHECK
					ppc_load (code, ppc_r0, (guint32)item->key);
					ppc_cmpl (code, 0, 1, MONO_ARCH_IMT_REG, ppc_r0);
					item->jmp_code = code;
					ppc_bc (code, PPC_BR_FALSE, PPC_BR_EQ, 0);
#endif
					ppc_load_reg (code, ppc_r0, (sizeof (gpointer) * item->value.vtable_slot), ppc_r11);
					ppc_mtctr (code, ppc_r0);
					ppc_bcctr (code, PPC_BR_ALWAYS, 0);
#if ENABLE_WRONG_METHOD_CHECK
					ppc_patch (item->jmp_code, code);
					ppc_break (code);
					item->jmp_code = NULL;
#endif
				}
			}
		} else {
			ppc_load (code, ppc_r0, (gulong)item->key);
			ppc_cmpl (code, 0, 1, MONO_ARCH_IMT_REG, ppc_r0);
			item->jmp_code = code;
			ppc_bc (code, PPC_BR_FALSE, PPC_BR_LT, 0);
		}
	}
	/* patch the branches to get to the target items */
	for (i = 0; i < count; ++i) {
		MonoIMTCheckItem *item = imt_entries [i];
		if (item->jmp_code) {
			if (item->check_target_idx) {
				ppc_patch (item->jmp_code, imt_entries [item->check_target_idx]->code_target);
			}
		}
	}

	if (!fail_tramp)
		mono_stats.imt_thunks_size += code - start;
	g_assert (code - start <= size);
	mono_arch_flush_icache (start, size);
	mono_ppc_emitted (start, size, "imt thunk vtable %p count %d fail_tramp %d", vtable, count, fail_tramp);
	return start;
}

MonoMethod*
mono_arch_find_imt_method (gpointer *regs, guint8 *code)
{
	return (MonoMethod*) regs [MONO_ARCH_IMT_REG];
}

MonoObject*
mono_arch_find_this_argument (gpointer *regs, MonoMethod *method, MonoGenericSharingContext *gsctx)
{
	return mono_arch_get_this_arg_from_call (gsctx, mono_method_signature (method), (gssize*)regs, NULL);
}
#endif

MonoVTable*
mono_arch_find_static_call_vtable (gpointer *regs, guint8 *code)
{
	return (MonoVTable*) regs [MONO_ARCH_RGCTX_REG];
}

MonoInst*
mono_arch_emit_inst_for_method (MonoCompile *cfg, MonoMethod *cmethod, MonoMethodSignature *fsig, MonoInst **args)
{
	/* FIXME: */
	return NULL;
}

gboolean
mono_arch_print_tree (MonoInst *tree, int arity)
{
	return 0;
}

MonoInst* mono_arch_get_domain_intrinsic (MonoCompile* cfg)
{
	MonoInst* ins;

	setup_tls_access ();
	if (monodomain_key == -1)
		return NULL;
	
	MONO_INST_NEW (cfg, ins, OP_TLS_GET);
	ins->inst_offset = monodomain_key;
	return ins;
}

MonoInst* 
mono_arch_get_thread_intrinsic (MonoCompile* cfg)
{
	MonoInst* ins;

	setup_tls_access ();
	if (monothread_key == -1)
		return NULL;
	
	MONO_INST_NEW (cfg, ins, OP_TLS_GET);
	ins->inst_offset = monothread_key;
	return ins;
}

gpointer
mono_arch_context_get_int_reg (MonoContext *ctx, int reg)
{
	g_assert (reg >= MONO_FIRST_SAVED_GREG);

	return (gpointer)ctx->regs [reg - MONO_FIRST_SAVED_GREG];
}

void
mono_ppc_emitted (guint8 *code, ssize_t length, const char *format, ...)
{
	va_list args;
	char *name;

	va_start (args, format);
	name = g_strdup_vprintf (format, args);
	va_end (args);

	g_print ("emitted [%s] at %p %p (length %ld)\n", name, code, code + length, length);

	g_free (name);
}
