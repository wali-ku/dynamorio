#include "dr_api.h"
#ifdef WINDOWS
# define DISPLAY_STRING(msg) dr_messagebox(msg)
#else
# define DISPLAY_STRING(msg) dr_printf("%s\n", msg)
#endif

typedef struct bb_counts {
   uint64 blocks;
   uint64 total_size;
   uint64 idr_jumps;
} bb_counts;

static bb_counts counts_as_built;
void *as_built_lock;
static bb_counts counts_dynamic;
void *count_lock;
static void
event_exit(void);
static dr_emit_flags_t
event_basic_block(void *drcontext, void *tag, instrlist_t *bb,
                  bool for_trace, bool translating);
static void
clean_call(uint instruction_count);
DR_EXPORT void
dr_client_main(client_id_t id, int argc, const char *argv[])
{
    /* register events */
    dr_register_exit_event(event_exit);
    dr_register_bb_event(event_basic_block);
    /* initialize lock */
    as_built_lock = dr_mutex_create();
    count_lock = dr_mutex_create();
}
static void
event_exit(void)
{
    /* Display results - we must first snpritnf the string as on windows
     * dr_printf(), dr_messagebox() and dr_fprintf() can't print floats. */
    char msg[512];
    int len;
    len = snprintf(msg, sizeof(msg)/sizeof(msg[0]),
                   "Number of blocks built : %"UINT64_FORMAT_CODE"\n"
                   "     Average size      : %5.2lf instructions\n"
                   "Number of blocks executed  : %"UINT64_FORMAT_CODE"\n"
                   "     Average weighted size : %5.2lf instructions\n"
		   "Number of indirect jumps : %"UINT64_FORMAT_CODE"\n",
                   counts_as_built.blocks,
                   counts_as_built.total_size / (double)counts_as_built.blocks,
                   counts_dynamic.blocks,
                   counts_dynamic.total_size / (double)counts_dynamic.blocks,
    		   counts_as_built.idr_jumps);
    DR_ASSERT(len > 0);
    msg[sizeof(msg)/sizeof(msg[0])-1] = '\0';
    DISPLAY_STRING(msg);
    /* free mutex */
    dr_mutex_destroy(as_built_lock);
    dr_mutex_destroy(count_lock);
}
static void
clean_call(uint instruction_count)
{
    dr_mutex_lock(count_lock);
    counts_dynamic.blocks++;
    counts_dynamic.total_size += instruction_count;
    dr_mutex_unlock(count_lock);
}
static dr_emit_flags_t
event_basic_block(void *drcontext, void *tag, instrlist_t *bb,
                  bool for_trace, bool translating)
{
    uint num_instructions = 0;
    uint num_idr_jumps = 0;
    instr_t *instr;

    /* count the number of instructions in this block */
    for (instr = instrlist_first(bb); instr != NULL; instr = instr_get_next(instr)) {
        num_instructions++;
	if (instr_is_mbr(instr)) {
        //instrlist_disassemble(drcontext, tag, bb, STDOUT);
		num_idr_jumps++;
    }
    }
    /* update the as-built counts */
    dr_mutex_lock(as_built_lock);
    counts_as_built.blocks++;
    counts_as_built.total_size += num_instructions;
    counts_as_built.idr_jumps += num_idr_jumps;
    dr_mutex_unlock(as_built_lock);
    /* insert clean call */
    dr_insert_clean_call(drcontext, bb, instrlist_first(bb), clean_call, false, 1,
                         OPND_CREATE_INT32(num_instructions));

    return DR_EMIT_DEFAULT;
}
