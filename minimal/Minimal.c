#include "spike_util.c"
#include "pqueue.c"
#include "reactor.c"
// Code generated by the Lingua Franca compiler for reactor Minimal in /mnt/data/dev/lf-flexpret-scripts/Minimal.lf
// =============== START reactor class Minimal
void minimal_rfunc_0(void* instance_args) {
    #line 5 "file:/mnt/data/dev/lf-flexpret-scripts/Minimal.lf"
    uint8_t* ptr = (uint8_t*)0x40000000;
    *ptr = 0x42;
    __spike_return(12345);
}
// =============== END reactor class Minimal

// ************* Instance Minimal of class Minimal
reaction_t minimal_reaction_0 = {&minimal_rfunc_0, NULL, 0, 0, 0, 0, NULL, NULL, NULL, false, 0LL, NULL};
// --- Reaction and trigger objects for reaction to trigger t of instance Minimal
reaction_t* minimal_t_trigger_reactions[1] = {&minimal_reaction_0};
trigger_t minimal_t_trigger = {
    minimal_t_trigger_reactions, 1, 0LL, 0LL, NULL, false, NEVER, NONE
};
void __set_default_command_line_options() {
}
void __initialize_trigger_objects() {
    //***** Start initializing Minimal
    minimal_t_trigger.offset = 0LL;
    minimal_t_trigger.period = 0LL;
    //***** End initializing Minimal
    // doDeferredInitialize
    // Connect inputs and outputs.
    minimal_reaction_0.index = 0;
    minimal_reaction_0.chain_id = 1;
}
void __start_timers() {
    __schedule(&minimal_t_trigger, 0LL, NULL);
}
void __start_time_step() {
    
}
bool __wrapup() {
    return false;
}
