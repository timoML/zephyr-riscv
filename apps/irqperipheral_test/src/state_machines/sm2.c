#include "sm_common.h"
#include "cycles.h"
#include "state_manager.h"
#include "states.h"
#include "irqtestperipheral.h"
#include "utils.h"
#include "log_perf.h"
#include "sm2_tasks.h"
#include "globals.h"

#ifndef TEST_MINIMAL

// variables to be configured by sm2_config()
static int num_substates; // = num of batches
static int num_user_batch;
static int num_users;
static void (*ul_action_1)(struct ActionArg const *);



/**
 * Define actions for SM2, are cbs called from state_mng_run()
 * see also sm2_tasks.h
 * ----------------------------------------------------------------------------
 */

// allow higher prio thread to take over, if not driven by irq1
// should keep idling below 1/10000
/*
static void sm2_yield(){
    if(sm_com_get_i_run() % 10000 == 0)
        k_yield();
}
*/


/**
 * Helper config functions
 * ----------------------------------------------------------------------------
 */

static void config_handlers(struct State sm_arr[]){
    
    // 1. requested values handlers, checked in state_manager::state_mng_check_vals_ready()
    states_set_handler_reqval(sm_arr, CYCLE_STATE_UL, sm_com_handle_fail_rval_ul);
    
    // 2. timing handlers, checked in state_manager::check_time_goal
    // start: wait for timing goal
    // end:   warn if missed

    //states_set_handler_timing_goal_start(sm_arr, CYCLE_STATE_DL_CONFIG, sm_com_handle_timing_goal_start); 
    states_set_handler_timing_goal_end(sm_arr, CYCLE_STATE_DL_CONFIG, sm_com_handle_timing_goal_end);
    //states_set_handler_timing_goal_start(sm_arr, CYCLE_STATE_DL, sm_com_handle_timing_goal_start); 
    states_set_handler_timing_goal_end(sm_arr, CYCLE_STATE_DL, sm_com_handle_timing_goal_end);
    //states_set_handler_timing_goal_start(sm_arr, CYCLE_STATE_UL_CONFIG, sm_com_handle_timing_goal_start;
    states_set_handler_timing_goal_end(sm_arr, CYCLE_STATE_UL_CONFIG, sm_com_handle_timing_goal_end);
    //states_set_handler_timing_goal_start(sm_arr, CYCLE_STATE_UL, sm_com_handle_timing_goal_start);
    states_set_handler_timing_goal_end(sm_arr, CYCLE_STATE_UL, sm_com_handle_timing_goal_end);
    //states_set_handler_timing_goal_start(sm_arr, CYCLE_STATE_RL_CONFIG, sm_com_handle_timing_goal_start);
    states_set_handler_timing_goal_end(sm_arr, CYCLE_STATE_RL_CONFIG, sm_com_handle_timing_goal_end);
    //states_set_handler_timing_goal_start(sm_arr, CYCLE_STATE_RL, sm_com_handle_timing_goal_start);
    states_set_handler_timing_goal_end(sm_arr, CYCLE_STATE_RL, sm_com_handle_timing_goal_end);

    states_set_handler_timing_goal_end(sm_arr, CYCLE_STATE_END, sm_com_handle_timing_goal_end);
    
}


static void config_timing_goals(struct State sm_arr[], int period_irq1_us, int period_irq2_us, int num_substates){
    
    int t_irq1_cyc = CYCLES_US_2_CYC(period_irq1_us);
    int t_irq2_cyc = CYCLES_US_2_CYC(period_irq2_us);

    // all durations in cyc
    int frac_trx_config = 10;
    int t_cfg = t_irq1_cyc / frac_trx_config;   // duration of sum of all cfg states
    int t_trx = t_irq1_cyc - t_cfg;             // duration of sum of all rx/tx states

    int t_state_trx = t_trx / 3;             // duration of single ul, dl or rl
    int t_state_cfg = t_cfg / 3;             // duration of single ul_config, ...
    int t_substate = (num_substates == 0 ? 0 : t_state_trx / num_substates); 
        
    printk("State trx duration %i us / %i cyc \n", CYCLES_CYC_2_US(t_state_trx), t_state_trx);   
    printk("State cfg duration %i us / %i cyc \n", CYCLES_CYC_2_US(t_state_cfg), t_state_cfg);  
    printk("Substate trx duration %i us / %i cyc \n", CYCLES_CYC_2_US(t_substate), t_substate);   

    // currently: assume substates only in UL
    sm_arr[CYCLE_STATE_DL_CONFIG].timing_goal_start = 0;    // 0 isn't handled, START state is before 
    sm_arr[CYCLE_STATE_DL_CONFIG].timing_goal_end = t_state_cfg;
    sm_arr[CYCLE_STATE_DL].timing_goal_start = sm_arr[CYCLE_STATE_DL_CONFIG].timing_goal_end;
    sm_arr[CYCLE_STATE_DL].timing_goal_end   = sm_arr[CYCLE_STATE_DL].timing_goal_start + t_state_trx;

    sm_arr[CYCLE_STATE_UL_CONFIG].timing_goal_start = sm_arr[CYCLE_STATE_DL].timing_goal_end;
    sm_arr[CYCLE_STATE_UL_CONFIG].timing_goal_end = sm_arr[CYCLE_STATE_UL_CONFIG].timing_goal_start + t_state_cfg;
    sm_arr[CYCLE_STATE_UL].timing_goal_start = sm_arr[CYCLE_STATE_UL_CONFIG].timing_goal_end;
    // if substates, end of first substate
    sm_arr[CYCLE_STATE_UL].timing_goal_end   = sm_arr[CYCLE_STATE_UL].timing_goal_start + (num_substates == 0 ? t_state_trx : t_substate);  
    int t_state_ul = sm_arr[CYCLE_STATE_UL].timing_goal_end - sm_arr[CYCLE_STATE_UL].timing_goal_start;

    sm_arr[CYCLE_STATE_RL_CONFIG].timing_goal_start = sm_arr[CYCLE_STATE_UL].timing_goal_start + (num_substates == 0 ? t_state_trx : num_substates * (t_state_ul));
    sm_arr[CYCLE_STATE_RL_CONFIG].timing_goal_end = sm_arr[CYCLE_STATE_RL_CONFIG].timing_goal_start + t_state_cfg;
    sm_arr[CYCLE_STATE_RL].timing_goal_start = sm_arr[CYCLE_STATE_RL_CONFIG].timing_goal_end;
    sm_arr[CYCLE_STATE_RL].timing_goal_end   = sm_arr[CYCLE_STATE_RL].timing_goal_start + t_state_trx;

    sm_arr[CYCLE_STATE_END].timing_goal_end = sm_arr[CYCLE_STATE_RL].timing_goal_end;

    // make sure last end is < T(protocol cycle)
    if(sm_arr[CYCLE_STATE_END].timing_goal_end > t_irq1_cyc)
        printk("WARNING: STATE_END timing_goal_end %i us > period_1 %i us \n", 
            CYCLES_CYC_2_US(sm_arr[CYCLE_STATE_END].timing_goal_end), period_irq1_us);

    if(t_substate < t_irq2_cyc){
    printk("WARNING: substate duration %i us < period_2 %i us \n", 
        CYCLES_CYC_2_US(t_substate), period_irq2_us);    
    }
    
}

// config that is tied to an upper level (not sm itself) and could
// eg. be done by application code
static void sm2_appconfig(){
    // todo: in case of shared irq for > 1 substate:
    // need to check whether values have uninit value and set skip action then
    //state_mng_register_action(CYCLE_STATE_IDLE , sm1_print_cycles, NULL, 0);
    //state_mng_register_action(CYCLE_STATE_IDLE , state_mng_print_transition_table_config, NULL, 0);
    
    state_mng_register_action(CYCLE_STATE_IDLE , sm_com_check_last_state, NULL, 0);
    state_mng_register_action(CYCLE_STATE_START, sm_com_check_last_state, NULL, 0);

    state_mng_register_action(CYCLE_STATE_DL   , sm_com_clear_valflags, NULL, 0);

    // simulate requesting of a value, which is cleared in STATE_UL and every substate
    // disable for benchmarking (otherwise will wait on irq2)
    //irqt_val_id_t reqvals_ul[] = {VAL_IRQ_0_PERVAL};
    state_mng_register_action(CYCLE_STATE_UL   , ul_action_1, NULL, 0);
    //state_mng_register_action(CYCLE_STATE_UL   , ul_action_1, reqvals_ul, 1);

    //state_mng_register_action(CYCLE_STATE_UL   , sm_com_clear_valflags, reqvals_ul, 1);
    
    state_mng_register_action(CYCLE_STATE_RL   , sm_com_clear_valflags, NULL, 0);

    //state_mng_register_action(CYCLE_STATE_START, sm_com_print_cycles, NULL, 0);

    //state_mng_register_action(CYCLE_STATE_START, sm_com_speed_up_after_warmup, NULL, 0);


    state_mng_register_action(CYCLE_STATE_END  , sm_com_check_last_state, NULL, 0);
    // state_mng_register_action(CYCLE_STATE_END  , sm_com_check_clear_status, NULL, 0);
    state_mng_register_action(CYCLE_STATE_END  , sm_com_check_val_updates, NULL, 0);
    state_mng_register_action(CYCLE_STATE_END  , sm_com_update_counter, NULL, 0);
    state_mng_register_action(CYCLE_STATE_END  , sm_com_mes_mperf, NULL, 0);
    // for profiling (no wait for IRQ1) (NOT WORKING AS EXPECTED)
    //state_mng_register_action(CYCLE_STATE_END  , sm2_yield, NULL, 0);
    //state_mng_register_action(CYCLE_STATE_END  , sm_com_print_perf_log, NULL, 0);
}

/**
 * Public functions: Set up, run SM2 and print diagnostics
 * ----------------------------------------------------------------------------
 */


void sm2_config(int users, int usr_per_batch, void (*ul_task)(struct ActionArg const*), int param, int pos_param){
    num_substates = users / usr_per_batch;
    num_user_batch = usr_per_batch;
    num_users = users;
    
    printk("SM2 configuring: param: %i, pos_param: %i \n" \
           "num_users: %i, substates: %i, usr_per_batch: %i \n", 
            param, pos_param, 
            num_users, num_substates, num_user_batch);

    if(users % usr_per_batch != 0){
        num_substates += 1;
        printk("WARNING: Fraction num_users / usr_per_batch non-integer. Setting num_substates= %i\n", num_substates);
    }

    if(ul_task != NULL){
        ul_action_1 = ul_task;

        // config for sm2 tasks
        if(ul_task == sm2_task_bench_basic_ops){
            switch(pos_param){
                case 0:
                    sm2_config_bench(param, 0, 0, 0);
                    break;
                case 1:
                    sm2_config_bench(0, param, 0, 0);
                    break;
                case 2:
                    sm2_config_bench(0, 0, param, 0);
                    break;
                case 3:
                    sm2_config_bench(0, 0, 0, param);
                    break;
                default:
                    printk("Error: Unknown pos_param %i. Aborting.", pos_param);
                    return;
            }
        }
        if(ul_task == sm2_task_calc_cfo_1){
            sm2_config_user_batch(num_user_batch);
        }

    }
}



void sm2_init(struct device * dev, int period_irq1_us, int period_irq2_us){


    /**
     * Define states for SM2
     * Currently, this is tightly coupled to cycle_state_id_t and cycle_event_id_t
     * declared in states.h
     * ----------------------------------------------------------------------------
     */
    struct State sm2_idle 
    = {.id_name = CYCLE_STATE_IDLE,     .default_next_state = CYCLE_STATE_IDLE};
    struct State sm2_start 
    = {.id_name = CYCLE_STATE_START,    .default_next_state = CYCLE_STATE_DL_CONFIG};
    struct State sm2_dl_config 
    = {.id_name = CYCLE_STATE_DL_CONFIG,.default_next_state = CYCLE_STATE_DL};
    struct State sm2_dl 
    = {.id_name = CYCLE_STATE_DL,       .default_next_state = CYCLE_STATE_UL_CONFIG};
    struct State sm2_ul_config 
    = {.id_name = CYCLE_STATE_UL_CONFIG,.default_next_state = CYCLE_STATE_UL};
    struct State sm2_ul 
    = {.id_name = CYCLE_STATE_UL,       .default_next_state = CYCLE_STATE_RL_CONFIG};
    struct State sm2_rl_config
    = {.id_name = CYCLE_STATE_RL_CONFIG,.default_next_state = CYCLE_STATE_RL};
    struct State sm2_rl 
    = {.id_name = CYCLE_STATE_RL,   .default_next_state = CYCLE_STATE_END};
    struct State sm2_end 
    //  = {.id_name = CYCLE_STATE_END,  .default_next_state = CYCLE_STATE_START};   // for profiling
    = {.id_name = CYCLE_STATE_END,  .default_next_state = CYCLE_STATE_IDLE};    

    // state array
    struct State sm2_states[_NUM_CYCLE_STATES] = {_NIL_CYCLE_STATE};    
    sm2_states[CYCLE_STATE_IDLE] = sm2_idle;
    sm2_states[CYCLE_STATE_START] = sm2_start;
    sm2_states[CYCLE_STATE_DL_CONFIG] = sm2_dl_config;
    sm2_states[CYCLE_STATE_DL] = sm2_dl;
    sm2_states[CYCLE_STATE_UL_CONFIG] = sm2_ul_config;
    sm2_states[CYCLE_STATE_UL] = sm2_ul;
    sm2_states[CYCLE_STATE_RL_CONFIG] = sm2_rl_config;
    sm2_states[CYCLE_STATE_RL] = sm2_rl;
    sm2_states[CYCLE_STATE_END] = sm2_end;
    
    /**
     * Define transition table for SM1
     * First column: default event, is set automatically
     * ----------------------------------------------------------------------------
     */
    cycle_state_id_t sm2_tt[_NUM_CYCLE_STATES][_NUM_CYCLE_EVENTS];


    printk("SM2 initializing: \n" \
           "period_1: %i us, %i cyc, period_2: %i us, %i cyc\n",
            period_irq1_us, CYCLES_US_2_CYC(period_irq1_us), period_irq2_us, CYCLES_US_2_CYC(period_irq2_us));

    // additional config to states defined above
    if(period_irq2_us != 0){
        config_timing_goals(sm2_states, period_irq1_us, period_irq2_us, num_substates);
    }

    // act on sm_states defined above
    states_configure_substates(&sm2_ul, num_substates, 0);

   
    // init transition table
    // all resets lead to start state
    for(int i=0; i<_NUM_CYCLE_STATES; i++){
        sm2_tt[i][CYCLE_EVENT_RESET_IRQ] = CYCLE_STATE_START;   
    }
    // act on state_array
    config_handlers(sm2_states);
    

    // pass sm2 config to state manager
    state_mng_configure(sm2_states, (cycle_state_id_t *)sm2_tt, _NUM_CYCLE_STATES, _NUM_CYCLE_EVENTS);

    // mainly register actions for different states
    sm2_appconfig();
   
    // state config done, dbg-print
    state_mng_print_state_config();
    state_mng_print_transition_table_config();

    // replace generic isr with optimized handler
    irqtester_fe310_register_callback(g_dev_irqt, IRQ_1, _irq_1_handler_0);
    irqtester_fe310_register_callback(g_dev_irqt, IRQ_2, _irq_2_handler_0);

    // make state manager ready to start
    state_mng_init(g_dev_irqt);

}

void sm2_run(){

    print_dash_line(0);
    printk_framed(0, "Now running state machine sm2");
    print_dash_line(0);
   
    // start the sm thread, created in main()
	if(0 != state_mng_start()){
        printk("ERROR: Couldn't start sm2. Issue with thread. Aborting...\n");
        return;
    }
    
    //printk("DEBUG: SM2 offhanding to state manager thread \n");
}

// start signals (irqs) needed to drive sm
void sm2_fire_irqs(int period_irq1_us, int period_irq2_us){
    // program IRQ1 and IRQ2 to fire periodically
    // todo: hw support for infinite repetitions?
    u32_t period_1_cyc = CYCLES_US_2_CYC(period_irq1_us); // x * ~1000 us
    u32_t period_2_cyc = CYCLES_US_2_CYC(period_irq2_us);
    // fire once to start running even if irq1 disabled
    u32_t num_irq_1 = (period_1_cyc == 0 ? 1 : UINT32_MAX);

	struct DrvValue_uint reg_num = {.payload=num_irq_1};
	struct DrvValue_uint reg_period = {.payload=period_1_cyc};	

	irqtester_fe310_set_reg(g_dev_irqt, VAL_IRQ_1_NUM_REP, &reg_num);
	irqtester_fe310_set_reg(g_dev_irqt, VAL_IRQ_1_PERIOD, &reg_period);

    // start firing reset irqs
	irqtester_fe310_fire_1(g_dev_irqt);

    // start firing val update irqs
    if(period_irq2_us != 0){
        int div_1_2 = period_irq1_us / period_irq2_us;    // integer division, p_1 always > p_2
        if(div_1_2 * period_irq2_us != period_irq1_us){
            printk("WARNING: Non integer divisible irq2 %u, irq1 %u lead to unstable timing relation. \n", period_irq2_us, period_irq1_us);
        }
        sm_com_set_val_uptd_per_cycle(div_1_2);

        reg_period.payload = period_2_cyc;
        irqtester_fe310_set_reg(g_dev_irqt, VAL_IRQ_2_NUM_REP, &reg_num);
        irqtester_fe310_set_reg(g_dev_irqt, VAL_IRQ_2_PERIOD, &reg_period);
        irqtester_fe310_fire_2(g_dev_irqt);
    }


}

void sm2_print_report(){
    sm_com_print_report();
}

void sm2_reset(){
    sm_com_reset();
    state_mng_reset();
}

#endif //TEST_MINIMAL