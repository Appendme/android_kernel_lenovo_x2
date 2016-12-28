#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/of_fdt.h>
#include <linux/lockdep.h>

#include <mach/irqs.h>
#include <mach/mt_cirq.h>
#include <mach/mt_spm_idle.h>
#include <mach/mt_cpuidle.h>
#include <mach/wd_api.h>
#include <mach/mt_gpt.h>
#include <mach/mt_cpufreq.h>
#include <mach/mt_ccci_common.h>

#include "mt_spm_internal.h"


/**************************************
 * only for internal debug
 **************************************/
//FIXME: for FPGA early porting
#define  CONFIG_MTK_LDVT

#ifdef CONFIG_MTK_LDVT
#define SPM_PWAKE_EN            0
#define SPM_BYPASS_SYSPWREQ     1
#else
#define SPM_PWAKE_EN            1
#define SPM_BYPASS_SYSPWREQ     0
#endif

#define WAKE_SRC_FOR_DPIDLE                                                                      \
    (WAKE_SRC_KP | WAKE_SRC_GPT | WAKE_SRC_EINT | WAKE_SRC_CONN_WDT| \
     WAKE_SRC_CCIF0_MD| WAKE_SRC_CCIF1_MD |WAKE_SRC_CONN2AP| WAKE_SRC_USB_CD| WAKE_SRC_USB_PDN| WAKE_SRC_SEJ|\
     WAKE_SRC_AFE | WAKE_SRC_CIRQ | WAKE_SRC_MD1_VRF18_WAKE|WAKE_SRC_SYSPWREQ | WAKE_SRC_MD_WDT | WAKE_SRC_C2K_WDT|\
     WAKE_SRC_CLDMA_MD)

#define WAKE_SRC_FOR_MD32  0

#define I2C_CHANNEL 2

#define spm_is_wakesrc_invalid(wakesrc)     (!!((u32)(wakesrc) & 0xc0003803))

#ifdef CONFIG_MTK_RAM_CONSOLE
#define SPM_AEE_RR_REC 1
#else
#define SPM_AEE_RR_REC 0
#endif

#if SPM_AEE_RR_REC
enum spm_deepidle_step
{
	SPM_DEEPIDLE_ENTER=0,
	SPM_DEEPIDLE_ENTER_UART_SLEEP,
	SPM_DEEPIDLE_ENTER_WFI,
	SPM_DEEPIDLE_LEAVE_WFI,
	SPM_DEEPIDLE_ENTER_UART_AWAKE,
	SPM_DEEPIDLE_LEAVE
};
#endif

/**********************************************************
 * PCM code for deep idle
 **********************************************************/ 
static const u32 dpidle_binary[] = {
	0x81459801, 0xd8200285, 0x17c07c1f, 0x1a00001f, 0x10006604, 0xe2200004,
	0xc0c02920, 0x17c07c1f, 0xc0c02ae0, 0x17c07c1f, 0xe2200003, 0xc0c02920,
	0x17c07c1f, 0xc0c02ae0, 0x17c07c1f, 0xe2200002, 0xc0c02920, 0x17c07c1f,
	0xc0c02ae0, 0x17c07c1f, 0x80328400, 0x80318400, 0x18c0001f, 0x10006b18,
	0x1910001f, 0x10006b18, 0x813f0404, 0xe0c00004, 0xc2802820, 0x1290041f,
	0x1b00001f, 0x6f7cf7ff, 0xf0000000, 0x17c07c1f, 0x1b00001f, 0x2f7ce7ff,
	0x1b80001f, 0x20000004, 0xd800088c, 0x17c07c1f, 0xa0118400, 0xa0128400,
	0xc2802820, 0x1290841f, 0x81441801, 0xd8200805, 0x17c07c1f, 0x1a00001f,
	0x10006604, 0xe2200003, 0xc0c02920, 0x17c07c1f, 0xc0c02ae0, 0x17c07c1f,
	0xe2200004, 0xc0c02920, 0x17c07c1f, 0xc0c02ae0, 0x17c07c1f, 0xe2200005,
	0xc0c02920, 0x17c07c1f, 0xc0c02ae0, 0x17c07c1f, 0xc2802820, 0x129f041f,
	0x1b00001f, 0x2f7cefff, 0xf0000000, 0x17c07c1f, 0x81411801, 0xd8000aa5,
	0x17c07c1f, 0x18c0001f, 0x10006240, 0xe0e00016, 0xe0e0001e, 0xe0e0000e,
	0xe0e0000f, 0x18c0001f, 0x102135cc, 0x1910001f, 0x102135cc, 0x813f8404,
	0xe0c00004, 0x803e0400, 0x1b80001f, 0x20000222, 0x80380400, 0x1b80001f,
	0x20000280, 0x803b0400, 0x1b80001f, 0x2000001a, 0x803d0400, 0x1b80001f,
	0x20000208, 0x80340400, 0x80310400, 0x1b80001f, 0x2000000a, 0x18c0001f,
	0x10006240, 0xe0e0000d, 0xd8001085, 0x17c07c1f, 0x1b80001f, 0x20000020,
	0x18c0001f, 0x102130f0, 0x1910001f, 0x102130f0, 0xa9000004, 0x10000000,
	0xe0c00004, 0x1b80001f, 0x2000000a, 0x89000004, 0xefffffff, 0xe0c00004,
	0x18c0001f, 0x102140f4, 0x1910001f, 0x102140f4, 0xa9000004, 0x02000000,
	0xe0c00004, 0x1b80001f, 0x2000000a, 0x89000004, 0xfdffffff, 0xe0c00004,
	0x1b80001f, 0x20000100, 0x81fa0407, 0x81f08407, 0xe8208000, 0x10006354,
	0x00dffbe3, 0xa1d80407, 0xa1de8407, 0xa1df0407, 0xc2802820, 0x1291041f,
	0x1b00001f, 0xaf7ce7ff, 0xf0000000, 0x17c07c1f, 0x1b80001f, 0x2000030c,
	0x1a50001f, 0x10006608, 0x80c9a401, 0x810ba401, 0x10920c1f, 0xa0979002,
	0x80ca2401, 0xa0938c02, 0x8080080d, 0xd8201562, 0x17c07c1f, 0x1b00001f,
	0x2f7ce7ff, 0x1b80001f, 0x20000004, 0xd8001a2c, 0x17c07c1f, 0x1b00001f,
	0xaf7ce7ff, 0xd8001a2c, 0x17c07c1f, 0x81f80407, 0x81fe8407, 0x81ff0407,
	0x1880001f, 0x10006320, 0xc0c020a0, 0xe080000f, 0xd8001423, 0x17c07c1f,
	0xe080001f, 0xa1da0407, 0xa0110400, 0xa0140400, 0xa0180400, 0xa01b0400,
	0xa01d0400, 0x1b80001f, 0x20000068, 0xa01e0400, 0x1b80001f, 0x20000104,
	0x81411801, 0xd80019a5, 0x17c07c1f, 0x18c0001f, 0x10006240, 0xc0c01fe0,
	0x17c07c1f, 0x18c0001f, 0x102135cc, 0x1910001f, 0x102135cc, 0xa11f8404,
	0xe0c00004, 0xc2802820, 0x1291841f, 0x1b00001f, 0x6f7cf7ff, 0xf0000000,
	0x17c07c1f, 0x18c0001f, 0x1100b254, 0x1900001f, 0xffffffff, 0xe0c00004,
	0x18c0001f, 0x10006b04, 0x1910001f, 0x10006b04, 0xa1180404, 0xe0c00004,
	0x81380404, 0xe0c00004, 0x18c0001f, 0x10006810, 0x1910001f, 0x10006810,
	0xa1108404, 0xe0c00004, 0x1950001f, 0x100063e8, 0x81708405, 0x1300141f,
	0x81308404, 0xe0c00004, 0xa1508405, 0x1300141f, 0xf0000000, 0x17c07c1f,
	0xe0f07f16, 0x1380201f, 0xe0f07f1e, 0x1380201f, 0xe0f07f0e, 0x1b80001f,
	0x20000100, 0xe0f07f0c, 0xe0f07f0d, 0xe0f07e0d, 0xe0f07c0d, 0xe0f0780d,
	0xe0f0700d, 0xf0000000, 0x17c07c1f, 0xe0f07f0d, 0xe0f07f0f, 0xe0f07f1e,
	0xe0f07f12, 0xf0000000, 0x17c07c1f, 0x1112841f, 0xa1d08407, 0xd8202164,
	0x80eab401, 0xd80020e3, 0x01200404, 0x1a00001f, 0x10006814, 0xe2000003,
	0xf0000000, 0x17c07c1f, 0xa1d00407, 0x1b80001f, 0x20000100, 0x80ea3401,
	0x1a00001f, 0x10006814, 0xe2000003, 0xf0000000, 0x17c07c1f, 0xd80023ea,
	0x17c07c1f, 0xe2e00036, 0xe2e0003e, 0x1380201f, 0xe2e0003c, 0xd820252a,
	0x17c07c1f, 0x1b80001f, 0x20000018, 0xe2e0007c, 0x1b80001f, 0x20000003,
	0xe2e0005c, 0xe2e0004c, 0xe2e0004d, 0xf0000000, 0x17c07c1f, 0xa1d10407,
	0x1b80001f, 0x20000020, 0xf0000000, 0x17c07c1f, 0xa1d40407, 0x1391841f,
	0xa1d90407, 0xf0000000, 0x17c07c1f, 0xd800274a, 0x17c07c1f, 0xe2e0004f,
	0xe2e0006f, 0xe2e0002f, 0xd82027ea, 0x17c07c1f, 0xe2e0002e, 0xe2e0003e,
	0xe2e00032, 0xf0000000, 0x17c07c1f, 0x18c0001f, 0x10006b18, 0x1910001f,
	0x10006b18, 0xa1002804, 0xe0c00004, 0xf0000000, 0x17c07c1f, 0x81481801,
	0xd8202a05, 0x17c07c1f, 0x1b80001f, 0x20000050, 0xd8002aa5, 0x17c07c1f,
	0x18d0001f, 0x10006604, 0x10cf8c1f, 0xd8202923, 0x17c07c1f, 0xf0000000,
	0x17c07c1f, 0x1092041f, 0x81499801, 0xd8202c65, 0x17c07c1f, 0x18d0001f,
	0x40000000, 0x18d0001f, 0x60000000, 0xd8002b62, 0x00a00402, 0xd8202f22,
	0x17c07c1f, 0x814a1801, 0xd8202dc5, 0x17c07c1f, 0x18d0001f, 0x40000000,
	0x18d0001f, 0x80000000, 0xd8002cc2, 0x00a00402, 0xd8202f22, 0x17c07c1f,
	0x814a9801, 0xd8202f25, 0x17c07c1f, 0x18d0001f, 0x40000000, 0x18d0001f,
	0x80000000, 0xd8002e22, 0x00a00402, 0xd8202f22, 0x17c07c1f, 0xf0000000,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f, 0x17c07c1f,
	0x17c07c1f, 0x17c07c1f, 0x1840001f, 0x00000001, 0xa1d48407, 0x1990001f,
	0x10006b08, 0xe8208000, 0x10006b18, 0x00000000, 0x1b00001f, 0x2f7ce7ff,
	0x1b80001f, 0xd00f0000, 0x81469801, 0xd8204265, 0x17c07c1f, 0x8880000c,
	0x2f7ce7ff, 0xd8005a22, 0x17c07c1f, 0xe8208000, 0x10006354, 0x00dffbe3,
	0xc0c02560, 0x81401801, 0x18c0001f, 0x10000338, 0x1910001f, 0x10000338,
	0x81308404, 0xe0c00004, 0xd8004845, 0x17c07c1f, 0x81f60407, 0x18c0001f,
	0x10006200, 0xc0c026a0, 0x12807c1f, 0xe8208000, 0x1000625c, 0x00000001,
	0x1b80001f, 0x20000080, 0xc0c026a0, 0x1280041f, 0x18c0001f, 0x10006208,
	0xc0c026a0, 0x12807c1f, 0xe8208000, 0x10006248, 0x00000000, 0x1b80001f,
	0x20000080, 0xc0c026a0, 0x1280041f, 0x18c0001f, 0x10006290, 0xc0c026a0,
	0x12807c1f, 0xc0c026a0, 0x1280041f, 0x18c0001f, 0x100062dc, 0xe0c00001,
	0xc2802820, 0x1292041f, 0x81469801, 0xd8004925, 0x17c07c1f, 0x8880000c,
	0x2f7ce7ff, 0xd80054c2, 0x17c07c1f, 0xc0c02600, 0x17c07c1f, 0x18c0001f,
	0x10006294, 0xe0f07fff, 0xe0e00fff, 0xe0e000ff, 0x81449801, 0xd8004c65,
	0x17c07c1f, 0x1a00001f, 0x10006604, 0xe2200006, 0xc0c02920, 0x17c07c1f,
	0x1a00001f, 0x10006604, 0xe2200001, 0xc0c02920, 0x17c07c1f, 0x18c0001f,
	0x10001130, 0x1910001f, 0x10001130, 0xa1108404, 0xe0c00004, 0xa1d38407,
	0xa1d98407, 0xa0108400, 0xa0120400, 0xa0148400, 0xa0150400, 0xa0158400,
	0xa01b8400, 0xa01c0400, 0xa01c8400, 0xa0188400, 0xa0190400, 0xa0198400,
	0xe8208000, 0x10006310, 0x0b1601f8, 0x1b00001f, 0xaf7ce7ff, 0x1b80001f,
	0x90100000, 0x80c18001, 0xc8c00003, 0x17c07c1f, 0x80c10001, 0xc8c008c3,
	0x17c07c1f, 0x18c0001f, 0x10006294, 0xe0e001fe, 0xe0e003fc, 0xe0e007f8,
	0xe0e00ff0, 0x1b80001f, 0x20000020, 0xe0f07ff0, 0xe0f07f00, 0x81449801,
	0xd8005245, 0x17c07c1f, 0x1a00001f, 0x10006604, 0xe2200006, 0xc0c02920,
	0x17c07c1f, 0xe2200000, 0xc0c02920, 0x17c07c1f, 0x80388400, 0x80390400,
	0x80398400, 0x1b80001f, 0x20000300, 0x803b8400, 0x803c0400, 0x803c8400,
	0x1b80001f, 0x20000300, 0x80348400, 0x80350400, 0x80358400, 0x1b80001f,
	0x20000104, 0x10007c1f, 0x81f38407, 0x81f98407, 0x81f90407, 0x81f40407,
	0x81401801, 0xd8005965, 0x17c07c1f, 0x18c0001f, 0x100062dc, 0xe0c0001f,
	0x18c0001f, 0x10006290, 0x1212841f, 0xc0c02320, 0x12807c1f, 0xc0c02320,
	0x1280041f, 0x18c0001f, 0x10006208, 0x1212841f, 0xc0c02320, 0x12807c1f,
	0xe8208000, 0x10006248, 0x00000001, 0x1b80001f, 0x20000080, 0xc0c02320,
	0x1280041f, 0x18c0001f, 0x10006200, 0x1212841f, 0xc0c02320, 0x12807c1f,
	0xe8208000, 0x1000625c, 0x00000000, 0x1b80001f, 0x20000080, 0xc0c02320,
	0x1280041f, 0x18c0001f, 0x10000338, 0x1910001f, 0x10000338, 0xa1108404,
	0xe0c00004, 0x19c0001f, 0x60415c20, 0x18c0001f, 0x10006b14, 0x1910001f,
	0x10006b14, 0x09000004, 0x00000001, 0xe0c00004, 0xc2802820, 0x1293841f,
	0x1ac0001f, 0x55aa55aa, 0x10007c1f, 0xf0000000
};
static struct pcm_desc dpidle_pcm = {
	.version	= "pcm_deepidle_v0.1_20141124",
	.base		= dpidle_binary,
	.size		= 736,
	.sess		= 2,
	.replace	= 0,
	.vec0		= EVENT_VEC(11, 1, 0, 0),	/* FUNC_26M_WAKEUP */
	.vec1		= EVENT_VEC(12, 1, 0, 34),	/* FUNC_26M_SLEEP */
	.vec2		= EVENT_VEC(30, 1, 0, 70),	/* FUNC_APSRC_WAKEUP */
	.vec3		= EVENT_VEC(31, 1, 0, 148),	/* FUNC_APSRC_SLEEP */
	.vec4		= EVENT_VEC(1, 1, 0, 211),	/* FUNC_LTE_PTP_MON_INT */
};

static struct pwr_ctrl dpidle_ctrl = {
	.wake_src		= WAKE_SRC_FOR_DPIDLE,
	.wake_src_md32		= WAKE_SRC_FOR_MD32,
	.r0_ctrl_en		= 1,
	.r7_ctrl_en		= 1,
	.infra_dcm_lock		= 1,
	.wfi_op			= WFI_OP_AND,
	.ca15_wfi0_en		= 1,
	.ca15_wfi1_en		= 1,
	.ca15_wfi2_en		= 1,
	.ca15_wfi3_en		= 1,
	.ca7_wfi0_en		= 1,
	.ca7_wfi1_en		= 1,
	.ca7_wfi2_en		= 1,
	.ca7_wfi3_en		= 1,
	.disp_req_mask		= 1,
	.mfg_req_mask		= 1,
	.lte_mask		= 1,
	.syspwreq_mask		= 1,
};

struct spm_lp_scen __spm_dpidle = {
	.pcmdesc	= &dpidle_pcm,
	.pwrctrl	= &dpidle_ctrl,
};

extern int mt_irq_mask_all(struct mtk_irq_mask *mask);
extern int mt_irq_mask_restore(struct mtk_irq_mask *mask);
extern void mt_irq_unmask_for_sleep(unsigned int irq);
extern int request_uart_to_sleep(void);
extern int request_uart_to_wakeup(void);
extern unsigned int mt_get_clk_mem_sel(void);
extern int is_ext_buck_exist(void);


#if SPM_AEE_RR_REC
extern void aee_rr_rec_deepidle_val(u32 val);
extern u32 aee_rr_curr_deepidle_val(void);
#endif

//FIXME: Denali early porting
#if 1
void __attribute__((weak)) mt_cirq_clone_gic(void){}
void __attribute__((weak)) mt_cirq_enable(void){}
void __attribute__((weak)) mt_cirq_flush(void){}
void __attribute__((weak)) mt_cirq_disable(void){}
#endif

static void spm_trigger_wfi_for_dpidle(struct pwr_ctrl *pwrctrl)
{
    if (is_cpu_pdn(pwrctrl->pcm_flags)) {
        mt_cpu_dormant(CPU_DEEPIDLE_MODE);
    } else {
        //Mp0_axi_config[4] is one by default. No need to program it before entering suspend.
        wfi_with_sync();
    }
}

/*
 * wakesrc: WAKE_SRC_XXX
 * enable : enable or disable @wakesrc
 * replace: if true, will replace the default setting
 */
int spm_set_dpidle_wakesrc(u32 wakesrc, bool enable, bool replace)
{
    unsigned long flags;

    if (spm_is_wakesrc_invalid(wakesrc))
        return -EINVAL;

    spin_lock_irqsave(&__spm_lock, flags);
    if (enable) {
        if (replace)
            __spm_dpidle.pwrctrl->wake_src = wakesrc;
        else
            __spm_dpidle.pwrctrl->wake_src |= wakesrc;
    } else {
        if (replace)
            __spm_dpidle.pwrctrl->wake_src = 0;
        else
            __spm_dpidle.pwrctrl->wake_src &= ~wakesrc;
    }
    spin_unlock_irqrestore(&__spm_lock, flags);

    return 0;
}
/*  //TODO
extern void spm_i2c_control(u32 channel, bool onoff);
U32 g_bus_ctrl = 0;
U32 g_sys_ck_sel = 0;
*/
static void spm_dpidle_pre_process(void)
{
/*  //TODO
    spm_i2c_control(I2C_CHANNEL, 1);//D1,D2 no ext bulk
    g_bus_ctrl=spm_read(0xF0001070);
    spm_write(0xF0001070 , g_bus_ctrl | (1 << 21)); //bus dcm disable
    g_sys_ck_sel = spm_read(0xF0001108);
    //spm_write(0xF0001108 , g_sys_ck_sel &~ (1<<1) );
    spm_write(0xF0001108 , 0x0);
    spm_write(0xF0000204 , spm_read(0xF0000204) | (1 << 0));  // BUS 26MHz enable 
*/
}

static void spm_dpidle_post_process(void)
{
/*   //TODO
    spm_i2c_control(I2C_CHANNEL, 0);
    spm_write(0xF0001070 , g_bus_ctrl); //26:26 enable 
*/
}

wake_reason_t spm_go_to_dpidle(u32 spm_flags, u32 spm_data)
{
    struct wake_status wakesta;
    unsigned long flags;
    struct mtk_irq_mask mask;
    wake_reason_t wr = WR_NONE;
    struct pcm_desc *pcmdesc = __spm_dpidle.pcmdesc;
    struct pwr_ctrl *pwrctrl = __spm_dpidle.pwrctrl;

#if SPM_AEE_RR_REC
    aee_rr_rec_deepidle_val(1<<SPM_DEEPIDLE_ENTER);
#endif 

    set_pwrctrl_pcm_flags(pwrctrl, spm_flags);

    /* set PMIC WRAP table for deepidle power control */
    mt_cpufreq_set_pmic_phase(PMIC_WRAP_PHASE_DEEPIDLE);

    spm_dpidle_before_wfi();

    lockdep_off();
    spin_lock_irqsave(&__spm_lock, flags);
    mt_irq_mask_all(&mask);
    mt_irq_unmask_for_sleep(SPM_IRQ0_ID);
    mt_cirq_clone_gic();
    mt_cirq_enable();

#if SPM_AEE_RR_REC
    aee_rr_rec_deepidle_val(aee_rr_curr_deepidle_val()|(1<<SPM_DEEPIDLE_ENTER_UART_SLEEP));
#endif     

    if (request_uart_to_sleep()) {
        wr = WR_UART_BUSY;
        goto RESTORE_IRQ;
    }
	
    __spm_reset_and_init_pcm(pcmdesc);

    __spm_kick_im_to_fetch(pcmdesc);
	
    __spm_init_pcm_register();

    __spm_init_event_vector(pcmdesc);

    __spm_set_power_control(pwrctrl);

    __spm_set_wakeup_event(pwrctrl);

    __spm_kick_pcm_to_run(pwrctrl);

    spm_dpidle_pre_process();

#if SPM_AEE_RR_REC
    aee_rr_rec_deepidle_val(aee_rr_curr_deepidle_val()|(1<<SPM_DEEPIDLE_ENTER_WFI));
#endif

    spm_trigger_wfi_for_dpidle(pwrctrl);

#if SPM_AEE_RR_REC
    aee_rr_rec_deepidle_val(aee_rr_curr_deepidle_val()|(1<<SPM_DEEPIDLE_LEAVE_WFI));
#endif 

    spm_dpidle_post_process();

    __spm_get_wakeup_status(&wakesta);

    __spm_clean_after_wakeup();

#if SPM_AEE_RR_REC
    aee_rr_rec_deepidle_val(aee_rr_curr_deepidle_val()|(1<<SPM_DEEPIDLE_ENTER_UART_AWAKE));
#endif 
    request_uart_to_wakeup();

    wr = __spm_output_wake_reason(&wakesta, pcmdesc, false);

RESTORE_IRQ:
    mt_cirq_flush();
    mt_cirq_disable();
    mt_irq_mask_restore(&mask);
    spin_unlock_irqrestore(&__spm_lock, flags);
    lockdep_on();
    spm_dpidle_after_wfi();
    /* set PMIC WRAP table for normal power control */
    mt_cpufreq_set_pmic_phase(PMIC_WRAP_PHASE_NORMAL);
#if SPM_AEE_RR_REC
    aee_rr_rec_deepidle_val(0);
#endif 
    return wr;
}

/*
 * cpu_pdn:
 *    true  = CPU dormant
 *    false = CPU standby
 * pwrlevel:
 *    0 = AXI is off
 *    1 = AXI is 26M
 * pwake_time:
 *    >= 0  = specific wakeup period
 */
wake_reason_t spm_go_to_sleep_dpidle(u32 spm_flags, u32 spm_data)
{
    u32 sec = 0;
    int wd_ret;
    struct wake_status wakesta;
    unsigned long flags;
    struct mtk_irq_mask mask;
    struct wd_api *wd_api;
    static wake_reason_t last_wr = WR_NONE;
    struct pcm_desc *pcmdesc = __spm_dpidle.pcmdesc;
    struct pwr_ctrl *pwrctrl = __spm_dpidle.pwrctrl;

    set_pwrctrl_pcm_flags(pwrctrl, spm_flags);

#if SPM_PWAKE_EN
    sec = spm_get_wake_period(-1 /* FIXME */, last_wr);
#endif
    pwrctrl->timer_val = sec * 32768;

    pwrctrl->wake_src = spm_get_sleep_wakesrc();

    wd_ret = get_wd_api(&wd_api);
    if (!wd_ret)
        wd_api->wd_suspend_notify();

    spin_lock_irqsave(&__spm_lock, flags);

    mt_irq_mask_all(&mask);
    mt_irq_unmask_for_sleep(195/*MT_SPM_IRQ_ID*/);
	
    mt_cirq_clone_gic();
    mt_cirq_enable();

    /* set PMIC WRAP table for deepidle power control */
    mt_cpufreq_set_pmic_phase(PMIC_WRAP_PHASE_DEEPIDLE);

    spm_crit2("sleep_deepidle, sec = %u, wakesrc = 0x%x [%u]\n",
              sec, pwrctrl->wake_src, is_cpu_pdn(pwrctrl->pcm_flags));

    __spm_reset_and_init_pcm(pcmdesc);

    __spm_kick_im_to_fetch(pcmdesc);

    if (request_uart_to_sleep()) {
        last_wr = WR_UART_BUSY;
        goto RESTORE_IRQ;
    }

    __spm_init_pcm_register();

    __spm_init_event_vector(pcmdesc);

    __spm_set_power_control(pwrctrl);

    __spm_set_wakeup_event(pwrctrl);

    __spm_kick_pcm_to_run(pwrctrl);

    spm_trigger_wfi_for_dpidle(pwrctrl);

    __spm_get_wakeup_status(&wakesta);

    __spm_clean_after_wakeup();

    request_uart_to_wakeup();

    last_wr = __spm_output_wake_reason(&wakesta, pcmdesc, true);

RESTORE_IRQ:

    /* set PMIC WRAP table for normal power control */
    mt_cpufreq_set_pmic_phase(PMIC_WRAP_PHASE_NORMAL);

    mt_cirq_flush();
    mt_cirq_disable();

    mt_irq_mask_restore(&mask);

    spin_unlock_irqrestore(&__spm_lock, flags);

    if (!wd_ret)
        wd_api->wd_resume_notify();

    return last_wr;  
}

MODULE_DESCRIPTION("SPM-DPIdle Driver v0.1");
