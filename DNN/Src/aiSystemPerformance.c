/**
 ******************************************************************************
 * @file    aiSystemPerformance.c
 * @author  MCD Vertical Application Team
 * @brief   Entry points for AI system performance application
 ******************************************************************************
 * @attention
 *
 * <h2><center>&copy; Copyright (c) YYYY STMicroelectronics.
 * All rights reserved.</center></h2>
 *
 * This software component is licensed by ST under Ultimate Liberty license
 * SLA0044, the "License"; You may not use this file except in compliance with
 * the License. You may obtain a copy of the License at:
 *                             www.st.com/SLA0044
 *
 ******************************************************************************
 */

/*
 * Description:
 *
 * - Simple STM32 application to measure and report the system performance of
 *   a generated NN
 * - STM32CubeMX.AI tools version: Client API "1.0" / AI Platform 3.2.x
 * - Only STM32F7, STM32F4 or STM32L4 MCU-base family are supported
 * - Random input values are injected in the NN to measure the inference time
 *   and to monitor the usage of the stack and/or the heap. Output value are
 *   skipped.
 * - After N iterations (_APP_ITER_ C-define), results are reported through a
 *   re-target printf
 * - aiSystemPerformanceInit()/aiSystemPerformanceProcess() functions should
 *   be called from the main application code.
 * - Only UART (to re-target the printf) & CORE clock setting are expected
 *   by the initial run-time (main function).
 *   CRC IP should be also enabled for AI Platform >= 3.0.0
 *
 * Atollic/AC6 IDE (GCC-base toolchain)
 *  - Linker options "-Wl,--wrap=malloc -Wl,--wrap=free" should be used
 *    to support the HEAP monitoring
 *
 * TODO:
 *  - complete the returned HEAP data
 *  - add HEAP monitoring for IAR tool-chain
 *  - add HEAP/STACK monitoring MDK-ARM Keil tool-chain
 *
 * History:
 *  - v1.0 - Initial version
 *  - v1.1 - Complete minimal interactive console
 *  - v1.2 - Adding STM32H7 MCU support
 *  - v1.3 - Adding STM32F3 MCU support
 *  - v1.4 - Adding Profiling mode
 *  - v2.0 - Adding Multiple Network support
 *  - v2.1 - Adding F3 str description
 *  - v3.0 - Adding FXP support
 *           Adding initial multiple IO support (legacy mode)
 *           Removing compile-time STM32 family checking
 *  - v3.1 - Fix cycle count overflow
 *           Add support for external memory for data activations
 *  - v4.0 - Adding multiple IO support
 *  - v4.1 - Adding L5 support
 *  - v4.2 - Adding support for inputs in activations buffer
 *  - v4.3 - Fix - fill input samples loop + HAL_delay report
 *  - v4.4 - Complete dev_id str description
 *  - v5.0 - Add inference time by layer (with runtime observer API support)
 *           Improve reported network info (minor)
 *           Fix stack calculation (minor)
 *
 */

/* System headers */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#define USE_OBSERVER         1 /* 0: remove the registration of the user CB to evaluate the inference time by layer */
#define USE_CORE_CLOCK_ONLY  0 /* 1: remove usage of the HAL_GetTick() to evaluate he clock number */

#define ENABLE_DEBUG     	 0 /* 1: add debug trace - application level */


#if defined(__GNUC__)
#include <errno.h>
#include <sys/unistd.h> /* STDOUT_FILENO, STDERR_FILENO */
#elif defined (__ICCARM__)
#if (__IAR_SYSTEMS_ICC__ <= 8) 
/* Temporary workaround - LowLevelIOInterface.h seems not available
   with IAR 7.80.4 */
#define _LLIO_STDIN  0
#define _LLIO_STDOUT 1
#define _LLIO_STDERR 2
#define _LLIO_ERROR ((size_t)-1) /* For __read and __write. */
#else
#include <LowLevelIOInterface.h> /* _LLIO_STDOUT, _LLIO_STDERR */
#endif

#elif defined (__CC_ARM)

#endif

#include <aiSystemPerformance.h>
#include <bsp_ai.h>

/* APP configuration 0: disableENABLE_DEBUGd */
#if defined(__GNUC__)
#define _APP_STACK_MONITOR_ 1
#define _APP_HEAP_MONITOR_  1
#elif defined (__ICCARM__)
#define _APP_STACK_MONITOR_ 1
#define _APP_HEAP_MONITOR_  0   /* not yet supported */
#else
#define _APP_STACK_MONITOR_ 0   /* not yet supported */
#define _APP_HEAP_MONITOR_  0   /* not yet supported */
#endif

#if defined(USE_CORE_CLOCK_ONLY) && USE_CORE_CLOCK_ONLY == 1
#define _APP_FIX_CLK_OVERFLOW 0
#else
#define _APP_FIX_CLK_OVERFLOW 1
#endif

extern UART_HandleTypeDef UartHandle;


/* AI header files */
#include "ai_platform_interface.h"


#if defined(CHECK_STM32_FAMILY)
#if !defined(STM32F7) && !defined(STM32L4) && !defined(STM32L5) && !defined(STM32F4) && !defined(STM32H7) && !defined(STM32F3)
#error Only STM32H7, STM32F7, STM32F4, STM32L4, STM32L5 or STM32F3 device are supported
#endif
#endif

#define _APP_VERSION_MAJOR_     (0x05)
#define _APP_VERSION_MINOR_     (0x00)
#define _APP_VERSION_   ((_APP_VERSION_MAJOR_ << 8) | _APP_VERSION_MINOR_)

#define _APP_NAME_      "AI system performance measurement"

#define _APP_ITER_       16  /* number of iteration for perf. test */

struct dwtTime {
    uint32_t fcpu;
    int s;
    int ms;
    int us;
};

static struct cyclesCount {
    uint32_t dwt_max;
    uint32_t dwt_start;
    uint32_t tick_start;
} cyclesCount;

static int dwtCyclesToTime(uint64_t clks, struct dwtTime *t);

/* -----------------------------------------------------------------------------
 * Device-related functions
 * -----------------------------------------------------------------------------
 */

__STATIC_INLINE void crcIpInit(void)
{
#if defined(STM32H7)
    /* By default the CRC IP clock is enabled */
    __HAL_RCC_CRC_CLK_ENABLE();  
#else
    if (!__HAL_RCC_CRC_IS_CLK_ENABLED())
        printf("W: CRC IP clock is NOT enabled\r\n");

    /* By default the CRC IP clock is enabled */
    __HAL_RCC_CRC_CLK_ENABLE();
#endif
}

__STATIC_INLINE void dwtIpInit(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;

#ifdef STM32F7
    DWT->LAR = 0xC5ACCE55;
#endif

    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk | DWT_CTRL_CPIEVTENA_Msk;

}

__STATIC_INLINE void dwtReset(void)
{
    DWT->CYCCNT = 0; /* Clear DWT cycle counter */
}

__STATIC_INLINE uint32_t dwtGetCycles(void)
{
    return DWT->CYCCNT;
}

__STATIC_INLINE void cyclesCounterInit(void)
{
    struct dwtTime t;
    dwtCyclesToTime(UINT32_MAX, &t);
    cyclesCount.dwt_max = t.s * 1000 + t.ms;
    dwtIpInit();
}

__STATIC_INLINE void cyclesCounterStart(void)
{
    cyclesCount.tick_start = HAL_GetTick();
    dwtReset();
    cyclesCount.dwt_start = dwtGetCycles();
}

__STATIC_INLINE uint64_t cyclesCounterEnd(void)
{
#if _APP_FIX_CLK_OVERFLOW == 1
    struct dwtTime t;
    uint64_t dwt_e;
    uint64_t tick_e;
    dwt_e = dwtGetCycles() - cyclesCount.dwt_start;
    tick_e = HAL_GetTick() - cyclesCount.tick_start;
    dwtCyclesToTime(dwt_e, &t);
    if (tick_e > cyclesCount.dwt_max) {
        /* overflow detected */
        // dwt_e = (tick_e * (uint64_t)t.fcpu) / 1000;
        dwt_e = ((tick_e/cyclesCount.dwt_max) * (uint64_t)UINT32_MAX + (uint64_t)dwt_e);
    }
    return dwt_e;
#else
    return (uint64_t)(dwtGetCycles() - cyclesCount.dwt_start);
#endif
}


__STATIC_INLINE uint32_t systemCoreClock(void)
{
#if !defined(STM32H7) 
    return HAL_RCC_GetHCLKFreq();
#else
    return HAL_RCC_GetSysClockFreq();
#endif
}

static int dwtCyclesToTime(uint64_t clks, struct dwtTime *t)
{
    if (!t)
        return -1;
    uint32_t fcpu = systemCoreClock();
    uint64_t s  = clks / fcpu;
    uint64_t ms = (clks * 1000) / fcpu;
    uint64_t us = (clks * 1000 * 1000) / fcpu;
    ms -= (s * 1000);
    us -= (ms * 1000 + s * 1000000);
    t->fcpu = fcpu;
    t->s = s;
    t->ms = ms;
    t->us = us;
    return 0;
}

__STATIC_INLINE const char *devIdToStr(uint16_t dev_id)
{
    /* DEV_ID field from DBGMCU register */
    const char *str;
    switch (dev_id) {
    case 0x422: str = "STM32F303xB/C"; break;
    case 0x438: str = "STM32F303x6/8"; break;
    case 0x446: str = "STM32F303xD/E"; break;
    case 0x431: str = "STM32F411xC/E"; break;
    case 0x423: str = "STM32F401xB/C"; break;
    case 0x433: str = "STM32F401xD/E"; break;
    case 0x435: str = "STM32L43xxx"; break;
    case 0x462: str = "STM32L45xxx"; break;
    case 0x415: str = "STM32L4x6xx"; break;
    case 0x470: str = "STM32L4Rxxx"; break;
    case 0x472: str = "STM32L5[5,6]2xx"; break;
    case 0x449: str = "STM32F74xxx"; break;
    case 0x450: str = "STM32H743/53/50xx and STM32H745/55/47/57xx"; break; /* see RM0433 & RM0399 */
    default:    str = "UNKNOWN";
    }
    return str;
}

#if !defined(STM32F3)
__STATIC_INLINE const char* bitToStr(uint32_t val)
{
    if (val)
        return "True";
    else
        return "False";
}
#endif

__STATIC_INLINE void logDeviceConf(void)
{
    struct dwtTime t;
    uint32_t st;

#if !defined(STM32F3) && !defined(STM32L5)
    uint32_t acr = FLASH->ACR ;
#endif
    uint32_t val;

    printf("STM32 Runtime configuration...\r\n");

    printf(" Device       : DevID:0x%04x (%s) RevID:0x%04x\r\n",
            (int)HAL_GetDEVID(),
            devIdToStr(HAL_GetDEVID()),
            (int)HAL_GetREVID()
    );

    printf(" Core Arch.   : M%d - %s %s\r\n",
            __CORTEX_M,
#if (__FPU_PRESENT == 1)
            "FPU PRESENT",
            __FPU_USED ? "and used" : "and not used!"
#else
                    "!FPU NOT PRESENT",
                    ""
#endif
    );

    printf(" HAL version  : 0x%08x\r\n", (int)HAL_GetHalVersion());

    val = systemCoreClock()/1000000;

#if !defined(STM32H7)
    printf(" system clock : %u MHz\r\n", (int)val);
#else
    printf(" SYSCLK clock : %u MHz\r\n", (int)val);
    printf(" HCLK clock   : %u MHz\r\n", (int)HAL_RCC_GetHCLKFreq()/1000000);    
#endif

#if defined(STM32F7) || defined(STM32H7)
    val = SCB->CCR;
#if !defined(STM32H7)
    printf(" FLASH conf.  : ACR=0x%08x - Prefetch=%s ART=%s latency=%d\r\n",
            (int)acr,
            bitToStr((acr & FLASH_ACR_PRFTEN_Msk) >> FLASH_ACR_PRFTEN_Pos),
            bitToStr((acr & FLASH_ACR_ARTEN_Msk) >> FLASH_ACR_ARTEN_Pos),
            (int)((acr & FLASH_ACR_LATENCY_Msk) >> FLASH_ACR_LATENCY_Pos));
#else
    printf(" FLASH conf.  : ACR=0x%08x - latency=%d\r\n",
            (int)acr,
            (int)((acr & FLASH_ACR_LATENCY_Msk) >> FLASH_ACR_LATENCY_Pos));
#endif
#if !defined(CORE_M4)
    printf(" CACHE conf.  : $I/$D=(%s,%s)\r\n",
            bitToStr(val & SCB_CCR_IC_Msk),
            bitToStr(val & SCB_CCR_DC_Msk));
#endif
#else
#if !defined(STM32F3) && !defined(STM32L5)
    printf(" FLASH conf.  : ACR=0x%08x - Prefetch=%s $I/$D=(%s,%s) latency=%d\r\n",
            (int)acr,
            bitToStr((acr & FLASH_ACR_PRFTEN_Msk) >> FLASH_ACR_PRFTEN_Pos),
            bitToStr((acr & FLASH_ACR_ICEN_Msk) >> FLASH_ACR_ICEN_Pos),
            bitToStr((acr & FLASH_ACR_DCEN_Msk) >> FLASH_ACR_DCEN_Pos),
            (int)((acr & FLASH_ACR_LATENCY_Msk) >> FLASH_ACR_LATENCY_Pos));
#endif
#if defined(STM32L5)
    printf(" ICACHE       : %s\r\n", bitToStr(READ_BIT(ICACHE->CR, ICACHE_CR_EN)));
#endif
#endif

    dwtIpInit();
    dwtReset();
    HAL_Delay(100);
    st = dwtGetCycles();
    dwtCyclesToTime(st/100, &t);

    printf(" Calibration  : HAL_Delay(1)=%d.%03d ms\r\n",
            t.s * 100 + t.ms, t.us);
}

__STATIC_INLINE uint32_t disableInts(void)
{
    uint32_t state;

    state = __get_PRIMASK();
    __disable_irq();

    return state;
}

#if _APP_FIX_CLK_OVERFLOW != 1
__STATIC_INLINE void restoreInts(uint32_t state)
{
    __set_PRIMASK(state);
}
#endif


/* -----------------------------------------------------------------------------
 * low-level I/O functions
 * -----------------------------------------------------------------------------
 */

static struct ia_malloc {
    uint32_t cfg;
    uint32_t alloc;
    uint32_t free;
    uint32_t alloc_req;
    uint32_t free_req;
    uint32_t max;
    uint32_t used;
} ia_malloc;

#define MAGIC_MALLOC_NUMBER 0xefdcba98


static int ioGetUint8(uint8_t *buff, int count, uint32_t timeout)
{
    HAL_StatusTypeDef status;

    if ((!buff) || (count <= 0))
        return -1;

    status = HAL_UART_Receive(&UartHandle, (uint8_t *)buff, count,
            timeout);

    if (status == HAL_TIMEOUT)
        return -1;

    return (status == HAL_OK ? count : 0);
}


#if defined(__GNUC__)

int _write(int fd, const void *buff, int count);

int _write(int fd, const void *buff, int count)
{
    HAL_StatusTypeDef status;

    if ((count < 0) && (fd != STDOUT_FILENO) && (fd != STDERR_FILENO)) {
        errno = EBADF;
        return -1;
    }

    status = HAL_UART_Transmit(&UartHandle, (uint8_t *)buff, count,
            HAL_MAX_DELAY);

    return (status == HAL_OK ? count : 0);
}

void* __real_malloc(size_t bytes);
void __real_free(void *ptr);

void* __wrap_malloc(size_t bytes)
{
    uint8_t *ptr;

    ia_malloc.cfg |= 1 << 1;

    /* ensure alignment for magic number */
    bytes = (bytes + 3) & ~3;

    /* add 2x32-bit for size and magic  number */
    ptr = (uint8_t*)__real_malloc(bytes + 8);

    /* remember size */
    if (ptr) {
        *((uint32_t*)ptr) = bytes;
        *((uint32_t*)(ptr + 4 + bytes)) = MAGIC_MALLOC_NUMBER;
    }

    if ((ptr) && (ia_malloc.cfg & 1UL)) {
        ia_malloc.alloc_req++;
        ia_malloc.alloc += bytes;

        ia_malloc.used += bytes;

        if (ia_malloc.used > ia_malloc.max) {
        	ia_malloc.max = ia_malloc.used;
        }
    }
    return ptr?(ptr + 4):NULL;
}

void __wrap_free(void *ptr)
{
    uint8_t* p;
    uint32_t bytes;

    ia_malloc.cfg |= 1 << 2;

    if (!ptr)
        return;

    p = (uint8_t*)ptr - 4;
    bytes = *((uint32_t*)p);

    if (*((uint32_t*)(p + 4 + bytes)) == MAGIC_MALLOC_NUMBER) {
        *((uint32_t*)(p + 4 + bytes)) = 0;
    }

    if (ia_malloc.cfg & 1UL) {
        ia_malloc.free_req++;
        ia_malloc.free += bytes;
        ia_malloc.used -= bytes;
    }
    __real_free(p);
}


#elif defined (__ICCARM__)

__ATTRIBUTES  size_t __write(int handle, const unsigned char *buffer,
        size_t size);

__ATTRIBUTES  size_t __write(int handle, const unsigned char *buffer,
        size_t size)
{
    HAL_StatusTypeDef status;

    /*
     * This means that we should flush internal buffers.  Since we
     * don't we just return.  (Remember, "handle" == -1 means that all
     * handles should be flushed.)
     */
    if (buffer == 0)
        return 0;

    /* This template only writes to "standard out" and "standard err",
     * for all other file handles it returns failure.
     */
    if ((handle != _LLIO_STDOUT) && (handle != _LLIO_STDERR))
        return _LLIO_ERROR;

    status = HAL_UART_Transmit(&UartHandle, (uint8_t *)buffer, size,
            HAL_MAX_DELAY);

    return (status == HAL_OK ? size : _LLIO_ERROR);
}

#if _APP_HEAP_MONITOR_ == 1
#undef _APP_HEAP_MONITOR_
#define _APP_HEAP_MONITOR_ 0
#warning HEAP monitor is not YET supported
#endif

#elif defined (__CC_ARM)

int fputc(int ch, FILE *f)
{
    HAL_UART_Transmit(&UartHandle, (uint8_t *)&ch, 1,
            HAL_MAX_DELAY);
    return ch;
}

#if _APP_STACK_MONITOR_ == 1
#undef _APP_STACK_MONITOR_
#define _APP_STACK_MONITOR_ 0
#warning STACK monitor is not YET supported
#endif

#if _APP_HEAP_MONITOR_ == 1
#undef _APP_HEAP_MONITOR_
#define _APP_HEAP_MONITOR_ 0
#warning HEAP monitor is not YET supported
#endif

#else
#error ARM MCU tool-chain is not supported.
#endif


/* -----------------------------------------------------------------------------
 * AI-related functions
 * -----------------------------------------------------------------------------
 */

DEF_DATA_IN;

DEF_DATA_OUT;

struct network_exec_ctx {
    ai_handle handle;
    ai_network_report report;
} net_exec_ctx[AI_MNETWORK_NUMBER] = {0};

#define AI_BUFFER_NULL(ptr_)  \
        AI_BUFFER_OBJ_INIT( \
                AI_BUFFER_FORMAT_NONE|AI_BUFFER_FMT_FLAG_CONST, \
                0, 0, 0, 0, \
                AI_HANDLE_PTR(ptr_))


#if defined(AI_MNETWORK_DATA_ACTIVATIONS_INT_SIZE)
#if AI_MNETWORK_DATA_ACTIVATIONS_INT_SIZE != 0
AI_ALIGNED(4)
static ai_u8 activations[AI_MNETWORK_DATA_ACTIVATIONS_INT_SIZE];
#else
AI_ALIGNED(4)
static ai_u8 activations[1];
#endif
#else
AI_ALIGNED(4)
static ai_u8 activations[AI_MNETWORK_DATA_ACTIVATIONS_SIZE];
#endif



__STATIC_INLINE void aiLogErr(const ai_error err, const char *fct)
{
    if (fct)
        printf("E: AI error (%s) - type=%d code=%d\r\n", fct,
                err.type, err.code);
    else
        printf("E: AI error - type=%d code=%d\r\n", err.type, err.code);
}

__STATIC_INLINE void aiPrintLayoutBuffer(const char *msg, int idx,
        const ai_buffer* buffer)
{
    uint32_t type_id = AI_BUFFER_FMT_GET_TYPE(buffer->format);
    printf("%s[%d] ",msg, idx);
    if (type_id == AI_BUFFER_FMT_TYPE_Q) {
        printf(" %s%d,",
                AI_BUFFER_FMT_GET_SIGN(buffer->format)?"s":"u",
                        (int)AI_BUFFER_FMT_GET_BITS(buffer->format));
        if (AI_BUFFER_META_INFO_INTQ(buffer->meta_info)) {
            ai_float scale = AI_BUFFER_META_INFO_INTQ_GET_SCALE(buffer->meta_info, 0);
            int zero_point = AI_BUFFER_META_INFO_INTQ_GET_ZEROPOINT(buffer->meta_info, 0);
            printf("scale=%f, zero=%d,", scale, zero_point);
        } else {
            printf("Q%d.%d,",
                    (int)AI_BUFFER_FMT_GET_BITS(buffer->format)
                    - ((int)AI_BUFFER_FMT_GET_FBITS(buffer->format) +
                            (int)AI_BUFFER_FMT_GET_SIGN(buffer->format)),
                            AI_BUFFER_FMT_GET_FBITS(buffer->format));
        }
    }
    else if (type_id == AI_BUFFER_FMT_TYPE_FLOAT)
        printf(" float%d,",
                (int)AI_BUFFER_FMT_GET_BITS(buffer->format));
    else
        printf("NONE");
    printf(" %ld bytes, shape=(%d,%d,%ld)",
            AI_BUFFER_BYTE_SIZE(AI_BUFFER_SIZE(buffer), buffer->format),
            buffer->height, buffer->width, buffer->channels);
    if (buffer->data)
        printf(" (@0x%08x)\r\n", (int)buffer->data);
    else
        printf(" (USER domain)\r\n");
}

__STATIC_INLINE void aiPrintNetworkInfo(const ai_network_report* report)
{
    int i;
    printf("Network informations...\r\n");
    printf(" model name         : %s\r\n", report->model_name);
    printf(" model signature    : %s\r\n", report->model_signature);
    printf(" model datetime     : %s\r\n", report->model_datetime);
    printf(" compile datetime   : %s\r\n", report->compile_datetime);
    printf(" runtime version    : %d.%d.%d\r\n",
            report->runtime_version.major,
            report->runtime_version.minor,
            report->runtime_version.micro);
    if (report->tool_revision[0])
    	printf(" Tool revision      : %s\r\n", (report->tool_revision[0])?report->tool_revision:"");
    printf(" tools version      : %d.%d.%d\r\n",
            report->tool_version.major,
            report->tool_version.minor,
            report->tool_version.micro);
    printf(" complexity         : %ld MACC\r\n", report->n_macc);
    printf(" c-nodes            : %ld\r\n", report->n_nodes);
    printf(" activations        : %ld bytes (@0x%08x)\r\n",
            AI_BUFFER_SIZE(&report->activations), (int)report->activations.data);
    printf(" weights            : %ld bytes (@0x%08x)\r\n",
            AI_BUFFER_SIZE(&report->params), (int)report->params.data);
    printf(" inputs/outputs     : %u/%u\r\n", report->n_inputs,
            report->n_outputs);
    for (i=0; i<report->n_inputs; i++)
        aiPrintLayoutBuffer("  I", i, &report->inputs[i]);
    for (i=0; i<report->n_outputs; i++)
        aiPrintLayoutBuffer("  O", i, &report->outputs[i]);
}

static int aiBootstrap(const char *nn_name, const int idx)
{
    ai_error err;
    ai_u32 ext_addr, sz;

    /* Creating the network */
    printf("Creating instance for \"%s\"..\r\n", nn_name);
    err = ai_mnetwork_create(nn_name, &net_exec_ctx[idx].handle, NULL);
    if (err.type) {
        aiLogErr(err, "ai_mnetwork_create");
        return -1;
    }

    /* Initialize the instance */
    printf("Initializing..\r\n");

    /* build params structure to provide the reference of the
     * activation and weight buffers */
#if !defined(AI_MNETWORK_DATA_ACTIVATIONS_INT_SIZE)
    const ai_network_params params = {
            AI_BUFFER_NULL(NULL),
            AI_BUFFER_NULL(activations) };
#else
    ai_network_params params = {
            AI_BUFFER_NULL(NULL),
            AI_BUFFER_NULL(NULL) };

    if (ai_mnetwork_get_ext_data_activations(net_exec_ctx[idx].handle, &ext_addr, &sz) == 0) {
        if (ext_addr == 0xFFFFFFFF) {
            params.activations.data = (ai_handle)activations;
            ext_addr = (ai_u32)activations;
        }
        else {
            params.activations.data = (ai_handle)ext_addr;
        }
    }
#endif

    if (!ai_mnetwork_init(net_exec_ctx[idx].handle, &params)) {
        err = ai_mnetwork_get_error(net_exec_ctx[idx].handle);
        aiLogErr(err, "ai_mnetwork_init");
        ai_mnetwork_destroy(net_exec_ctx[idx].handle);
        net_exec_ctx[idx].handle = AI_HANDLE_NULL;
        return -4;
    }

    /* Query the created network to get relevant info from it */
    if (ai_mnetwork_get_info(net_exec_ctx[idx].handle, &net_exec_ctx[idx].report)) {
        aiPrintNetworkInfo(&net_exec_ctx[idx].report);
    } else {
        err = ai_mnetwork_get_error(net_exec_ctx[idx].handle);
        aiLogErr(err, "ai_mnetwork_get_info");
        ai_mnetwork_destroy(net_exec_ctx[idx].handle);
        net_exec_ctx[idx].handle = AI_HANDLE_NULL;
        return -2;
    }

    return 0;
}

static int aiInit(void)
{
    const char *nn_name;
    int idx;

    printf("\r\nAI Network (AI platform API %d.%d.%d)...\r\n",
            AI_PLATFORM_API_MAJOR,
            AI_PLATFORM_API_MINOR,
            AI_PLATFORM_API_MICRO);

    /* Discover and init the embedded network */
    idx = 0;
    do {
        nn_name = ai_mnetwork_find(NULL, idx);
        if (nn_name) {
            printf("\r\nFound the network \"%s\"\r\n", nn_name);
            if (aiBootstrap(nn_name, idx))
                return -1;
        }
        idx++;
    } while (nn_name);

    return 0;
}

static void aiDeInit(void)
{
    ai_error err;
    int idx;

    printf("Releasing the network(s)...\r\n");

    for (idx=0; idx<AI_MNETWORK_NUMBER; idx++) {
        if (net_exec_ctx[idx].handle) {
            if (ai_mnetwork_destroy(net_exec_ctx[idx].handle) != AI_HANDLE_NULL) {
                err = ai_mnetwork_get_error(net_exec_ctx[idx].handle);
                aiLogErr(err, "ai_mnetwork_destroy");
            }
            net_exec_ctx[idx].handle = NULL;
        }
    }
}

static bool hidden_mode = false;

#if defined(USE_OBSERVER) && USE_OBSERVER == 1

struct u_node_stat {
    uint64_t dur;
    uint32_t n_runs;
};

struct u_observer_ctx {
    uint64_t n_cb;
    uint64_t start_t;
    uint64_t u_dur_t;
    uint64_t k_dur_t;
    struct u_node_stat *nodes;
};

static struct u_observer_ctx u_observer_ctx;

/* User callback */
static ai_u32 user_observer_cb(const ai_handle cookie,
    const ai_u32 flags,
    const ai_observer_node *node) {

  struct u_observer_ctx *u_obs;

  volatile uint64_t ts = dwtGetCycles(); /* time stamp entry */

  u_obs = (struct u_observer_ctx *)cookie;
  u_obs->n_cb += 1;

  if (flags & AI_OBSERVER_POST_EVT) {
    const uint64_t end_t = ts - u_obs->start_t;
    u_obs->k_dur_t += end_t;
    u_obs->nodes[node->c_idx].dur += end_t;
    u_obs->nodes[node->c_idx].n_runs += 1;
  }

  u_obs->start_t = dwtGetCycles(); /* time stamp exit */
  u_obs->u_dur_t += u_obs->start_t  - ts; /* cumulate cycles used by the CB */
  return 0;
}

void aiObserverInit(struct network_exec_ctx *net_ctx)
{
  ai_handle  net_hdl;
  ai_network_params net_params;
  ai_bool res;
  int sz;

  if (!net_ctx || (net_ctx->handle == AI_HANDLE_NULL) || !net_ctx->report.n_nodes)
    return;

  if (hidden_mode)
    return;

  /* retrieve real handle */
  ai_mnetwork_get_private_handle(net_ctx->handle, &net_hdl, &net_params);

  memset((void *)&u_observer_ctx, 0, sizeof(struct u_observer_ctx));

  /* allocate resources to store the state of the nodes */
  sz = net_ctx->report.n_nodes * sizeof(struct u_node_stat);
  u_observer_ctx.nodes = (struct u_node_stat*)malloc(sz);
  if (!u_observer_ctx.nodes) {
    printf("W: enable to allocate the u_node_stats (sz=%d) ..\r\n", sz);
    return;
  }

  memset(u_observer_ctx.nodes, 0, sz);

  /* register the callback */
  res = ai_platform_observer_register(net_hdl, user_observer_cb,
      (ai_handle)&u_observer_ctx, AI_OBSERVER_PRE_EVT | AI_OBSERVER_POST_EVT);
  if (!res) {
    printf("W: enable to register the user CB\r\n");
    free(u_observer_ctx.nodes);
    u_observer_ctx.nodes = NULL;
    return;
  }
}

extern const char* ai_layer_type_name(const int type);

void aiObserverDone(struct network_exec_ctx *net_ctx)
{
  ai_handle  net_hdl;
  ai_network_params net_params;
  struct dwtTime t;
  uint64_t cumul;
  ai_observer_node node_info;

  if (!net_ctx || (net_ctx->handle == AI_HANDLE_NULL) ||
      !net_ctx->report.n_nodes || !u_observer_ctx.nodes)
    return;

  /* retrieve real handle */
  ai_mnetwork_get_private_handle(net_ctx->handle, &net_hdl, &net_params);

  ai_platform_observer_unregister(net_hdl, user_observer_cb,
      (ai_handle)&u_observer_ctx);

  printf("\r\n Inference time by c-node\r\n");
  dwtCyclesToTime(u_observer_ctx.k_dur_t / u_observer_ctx.nodes[0].n_runs, &t);
  printf("  kernel  : %d,%03dms (time passed in the c-kernel fcts)\n", t.s * 1000 + t.ms, t.us);
  dwtCyclesToTime(u_observer_ctx.u_dur_t / u_observer_ctx.nodes[0].n_runs, &t);
  printf("  user    : %d,%03dms (time passed in the user cb)\n", t.s * 1000 + t.ms, t.us);
#if defined(ENABLE_DEBUG) && ENABLE_DEBUG == 1
  printf("  cb #    : %d\n", (int)u_observer_ctx.n_cb);
#endif

  printf("\r\n %-6s%-20s%-7s %s\r\n", "c_id", "type", "id", "time (ms)");
  printf(" -------------------------------------------------\r\n");

  cumul = 0;
  node_info.c_idx = 0;
  while (ai_platform_observer_node_info(net_hdl, &node_info)) {
    struct u_node_stat *sn = &u_observer_ctx.nodes[node_info.c_idx];
    const char *fmt;
    cumul +=  sn->dur;
    dwtCyclesToTime(sn->dur / (uint64_t)sn->n_runs, &t);
    if ((node_info.type & (ai_u16)0x8000) >> 15)
      fmt = " %-6dTD-%-17s%-5d %4d,%03d %6.02f %c\n";
    else
      fmt = " %-6d%-20s%-5d %4d,%03d %6.02f %c\n";

    printf(fmt, node_info.c_idx,
        ai_layer_type_name(node_info.type  & (ai_u16)0x7FFF),
        (int)node_info.id,
        t.s * 1000 + t.ms, t.us,
        ((float)u_observer_ctx.nodes[node_info.c_idx].dur * 100.0f) / (float)u_observer_ctx.k_dur_t,
        '%');
    node_info.c_idx++;
  }

  printf(" -------------------------------------------------\r\n");
  cumul /= u_observer_ctx.nodes[0].n_runs;
  dwtCyclesToTime(cumul, &t);
  printf(" %31s %4d,%03d ms\r\n", "", t.s * 1000 + t.ms, t.us);

  free(u_observer_ctx.nodes);
  memset((void *)&u_observer_ctx, 0, sizeof(struct u_observer_ctx));

  return;
}
#endif


/* -----------------------------------------------------------------------------
 * Specific APP/test functions
 * -----------------------------------------------------------------------------
 */

#if defined(__GNUC__)
extern uint32_t _estack[];
#elif defined (__ICCARM__)
extern int CSTACK$$Limit;
extern int CSTACK$$Base;
#elif defined (__CC_ARM)
#if _APP_STACK_MONITOR_ == 1
#error STACK monitoring is not yet supported
#endif
#else
#error ARM MCU tool-chain is not supported.
#endif

static bool profiling_mode = false;
static int  profiling_factor = 5;


static int aiTestPerformance(int idx)
{
    int iter;
#if _APP_FIX_CLK_OVERFLOW == 0
    uint32_t irqs;
#endif
    ai_i32 batch;
    int niter;

    struct dwtTime t;
    uint64_t tcumul;
    uint64_t tend;
    uint64_t tmin;
    uint64_t tmax;
    uint32_t cmacc;

    ai_buffer ai_input[AI_MNETWORK_IN_NUM];
    ai_buffer ai_output[AI_MNETWORK_OUT_NUM];

#if _APP_STACK_MONITOR_ == 1
    uint32_t ctrl;
    bool stack_mon;
    uint32_t susage;

    uint32_t ustack_size; /* used stack before test */
    uint32_t estack;      /* end of stack @ */
    uint32_t mstack_size; /* minimal master stack size */
    uint32_t cstack;      /* current stack @ */
    uint32_t bstack;      /* base stack @ */
#endif

    if (net_exec_ctx[idx].handle == AI_HANDLE_NULL) {
        printf("E: network handle is NULL\r\n");
        return -1;
    }

#if _APP_STACK_MONITOR_ == 1
    /* Reading ARM Core registers */
    ctrl = __get_CONTROL();
    cstack = __get_MSP();

#if defined(__GNUC__)
    estack = (uint32_t)_estack;
    bstack = estack - MIN_STACK_SIZE;
    mstack_size = MIN_STACK_SIZE;
#elif defined (__ICCARM__)
    estack = (uint32_t)&CSTACK$$Limit;
    bstack = (uint32_t)&CSTACK$$Base;
    mstack_size = (uint32_t)&CSTACK$$Limit - (uint32_t)&CSTACK$$Base;
#endif

#endif

    if (profiling_mode)
        niter = _APP_ITER_ * profiling_factor;
    else
        niter = _APP_ITER_;

    printf("\r\nRunning PerfTest on \"%s\" with random inputs (%d iterations)...\r\n",
            net_exec_ctx[idx].report.model_name, niter);

#if _APP_STACK_MONITOR_ == 1
    /* Check that MSP is the active stack */
    if (ctrl & CONTROL_SPSEL_Msk) {
        printf("E: MSP is not the active stack (stack monitoring is disabled)\r\n");
        stack_mon = false;
    } else
        stack_mon = true;

    /* Calculating used stack before test */
    ustack_size = estack - cstack;

    if ((stack_mon) && (ustack_size > mstack_size)) {
        printf("E: !stack overflow detected %ld > %ld\r\n", ustack_size,
                mstack_size);
        stack_mon = false;
    }
#endif

#if ENABLE_DEBUG == 1
    printf("D: stack before test (0x%08lx-0x%08lx %ld/%ld ctrl=0x%08lx\n",
            estack, cstack, ustack_size, mstack_size, ctrl);
#endif

#if _APP_FIX_CLK_OVERFLOW == 0
    irqs = disableInts();
#endif

#if _APP_STACK_MONITOR_ == 1
    /* Fill the remaining part of the stack */
    if (stack_mon) {
        uint32_t *pw =  (uint32_t*)((bstack + 3) & (~3));

#if ENABLE_DEBUG == 1
        printf("D: fill stack 0x%08lx -> 0x%08lx (%ld)\n", pw, cstack,
                cstack - (uint32_t)pw);
#endif
        while ((uint32_t)pw < cstack) {
            *pw = 0xDEDEDEDE;
            pw++;
        }
    }
#endif

    /* reset/init cpu clock counters */
    tcumul = 0ULL;
    tmin = UINT64_MAX;
    tmax = 0UL;

    memset(&ia_malloc,0,sizeof(struct ia_malloc));

    if ((net_exec_ctx[idx].report.n_inputs > AI_MNETWORK_IN_NUM) ||
            (net_exec_ctx[idx].report.n_outputs > AI_MNETWORK_OUT_NUM))
    {
        printf("E: AI_MNETWORK_IN/OUT_NUM definition are incoherent\r\n");
        HAL_Delay(100);
        return -1;
    }

    /* Fill the input tensor descriptors */
    for (int i = 0; i < net_exec_ctx[idx].report.n_inputs; i++) {
        ai_input[i] = net_exec_ctx[idx].report.inputs[i];
        ai_input[i].n_batches  = 1;
        if (net_exec_ctx[idx].report.inputs[i].data)
            ai_input[i].data = AI_HANDLE_PTR(net_exec_ctx[idx].report.inputs[i].data);
        else
            ai_input[i].data = AI_HANDLE_PTR(data_ins[i]);
    }

    /* Fill the output tensor descriptors */
    for (int i = 0; i < net_exec_ctx[idx].report.n_outputs; i++) {
        ai_output[i] = net_exec_ctx[idx].report.outputs[i];
        ai_output[i].n_batches = 1;
        ai_output[i].data = AI_HANDLE_PTR(data_outs[i]);
    }

    if (profiling_mode) {
        printf("Profiling mode (%d)...\r\n", profiling_factor);
        fflush(stdout);
    }

#if defined(USE_OBSERVER) && USE_OBSERVER == 1
    /* Enable observer */
    aiObserverInit(&net_exec_ctx[idx]);
#endif

    /* Main inference loop */
    for (iter = 0; iter < niter; iter++) {

        /* Fill input tensors with random data */
        for (int i = 0; i < net_exec_ctx[idx].report.n_inputs; i++) {
            const ai_buffer_format fmt = AI_BUFFER_FORMAT(&ai_input[i]);
            ai_i8 *in_data = (ai_i8 *)ai_input[i].data;
            for (ai_size j = 0; j < AI_BUFFER_SIZE(&ai_input[i]); ++j) {
                /* uniform distribution between -1.0 and 1.0 */
                const float v = 2.0f * (ai_float) rand() / (ai_float) RAND_MAX - 1.0f;
                if  (AI_BUFFER_FMT_GET_TYPE(fmt) == AI_BUFFER_FMT_TYPE_FLOAT) {
                    *(ai_float *)(in_data + j * 4) = v;
                }
                else {
                    in_data[j] = (ai_i8)(v * 127);
                }
            }
        }

#if _APP_HEAP_MONITOR_ == 1
        /* force a call of wrap functions */
        free(malloc(10));
        ia_malloc.cfg |= 1UL;
#endif

        cyclesCounterStart();
        batch = ai_mnetwork_run(net_exec_ctx[idx].handle, ai_input, ai_output);
        if (batch != 1) {
            aiLogErr(ai_mnetwork_get_error(net_exec_ctx[idx].handle),
                    "ai_mnetwork_run");
            break;
        }
        tend = cyclesCounterEnd();

#if _APP_HEAP_MONITOR_ == 1
        ia_malloc.cfg &= ~1UL;
#endif

        if (tend < tmin)
            tmin = tend;

        if (tend > tmax)
            tmax = tend;

        tcumul += tend;

        dwtCyclesToTime(tend, &t);

#if ENABLE_DEBUG == 1
        printf(" #%02d %8d.%03dms (%lu cycles)\r\n", iter,
                t.ms, t.us, tend);
#else
        if (!profiling_mode) {
            if (t.s > 10)
                niter = iter;
            printf(".");
            fflush(stdout);
        }
#endif
    }

#if ENABLE_DEBUG != 1
    printf("\r\n");
#endif

#if _APP_STACK_MONITOR_ == 1
    if (__get_MSP() != cstack) {
        printf("E: !current stack address is not coherent 0x%08lx instead 0x%08lx\r\n",
                __get_MSP(), cstack);
    }

    /* Calculating the used stack */
    susage = 0UL;
    if (stack_mon) {
        uint32_t rstack = mstack_size - ustack_size;
        uint32_t *pr =  (uint32_t*)((bstack + 3) & (~3));
        bool overflow = false;

        /* check potential stack overflow with 8 last words*/
        for (int i = 0; i < 8; i++) {
            if (*pr != 0xDEDEDEDE)
                overflow = true;
            pr++;
        }

        if (!overflow) {
            susage = 8*4;
            while ((*pr == 0xDEDEDEDE) && ((uint32_t)pr < cstack)) {
                pr++;
                susage += 4;
            }
            susage = rstack - susage;
        } else {
            printf("E: !stack overflow detected > %ld\r\n", rstack);
            printf("note: MIN_STACK_SIZE value/definition should be verified (app_x-cube-ai.h & linker file)");
        }
    }
#endif

#if _APP_FIX_CLK_OVERFLOW == 0
    restoreInts(irqs);
#endif

    printf("\r\n");

#if defined(USE_OBSERVER) && USE_OBSERVER == 1
    tmin = tmin - u_observer_ctx.u_dur_t / (uint64_t)iter;
    tmax = tmax - u_observer_ctx.u_dur_t / (uint64_t)iter;
    tcumul -= u_observer_ctx.u_dur_t;
#endif

    tcumul /= (uint64_t)iter;

    dwtCyclesToTime(tcumul, &t);

    printf("Results for \"%s\", %d inferences @%ldMHz/%ldMHz (complexity: %lu MACC)\r\n",
            net_exec_ctx[idx].report.model_name, iter,
            HAL_RCC_GetSysClockFreq() / 1000000,
            HAL_RCC_GetHCLKFreq() / 1000000,
            net_exec_ctx[idx].report.n_macc);

    printf(" duration     : %d.%03d ms (average)\r\n", t.s * 1000 + t.ms, t.us);
    printf(" CPU cycles   : %lu -%lu/+%lu (average,-/+)\r\n",
            (uint32_t)(tcumul), (uint32_t)(tcumul - tmin),
            (uint32_t)(tmax - tcumul));
    printf(" CPU Workload : %d%c (duty cycle = 1s)\r\n", (int)((tcumul * 100) / t.fcpu), '%');
    cmacc = (uint32_t)((tcumul * 100)/ net_exec_ctx[idx].report.n_macc);
    printf(" cycles/MACC  : %lu.%02lu (average for all layers)\r\n",
            cmacc / 100, cmacc - ((cmacc / 100) * 100));
#if _APP_STACK_MONITOR_ == 1
    if (stack_mon)
        printf(" used stack   : %ld bytes\r\n", susage);
    else
        printf(" used stack   : NOT CALCULATED\r\n");
#else
    printf(" used stack   : DISABLED\r\n");
#endif
#if _APP_HEAP_MONITOR_ == 1
    printf(" used heap    : %ld:%ld %ld:%ld (req:allocated,req:released) max=%ld used=%ld cfg=%ld\r\n",
            ia_malloc.alloc_req, ia_malloc.alloc,
            ia_malloc.free_req, ia_malloc.free, ia_malloc.max, ia_malloc.used,
            (ia_malloc.cfg & (3 << 1)) >> 1);
#else
    printf(" used heap    : DISABLED or NOT YET SUPPORTED\r\n");
#endif

#if defined(USE_OBSERVER) && USE_OBSERVER == 1
    aiObserverDone(&net_exec_ctx[idx]);
#endif

    return 0;
}

#if defined(__GNUC__)
// #pragma GCC pop_options
#endif

#define CONS_EVT_TIMEOUT    (0)
#define CONS_EVT_QUIT       (1)
#define CONS_EVT_RESTART    (2)
#define CONS_EVT_HELP       (3)
#define CONS_EVT_PAUSE      (4)
#define CONS_EVT_PROF       (5)
#define CONS_EVT_HIDE       (6)

#define CONS_EVT_UNDEFINED  (100)

static int aiTestConsole(void)
{
    uint8_t c = 0;

    if (ioGetUint8(&c, 1, 5000) == -1) /* Timeout */
        return CONS_EVT_TIMEOUT;

    if ((c == 'q') || (c == 'Q'))
        return CONS_EVT_QUIT;

    if ((c == 'd') || (c == 'D'))
        return CONS_EVT_HIDE;

    if ((c == 'r') || (c == 'R'))
        return CONS_EVT_RESTART;

    if ((c == 'h') || (c == 'H') || (c == '?'))
        return CONS_EVT_HELP;

    if ((c == 'p') || (c == 'P'))
        return CONS_EVT_PAUSE;

    if ((c == 'x') || (c == 'X'))
        return CONS_EVT_PROF;

    return CONS_EVT_UNDEFINED;
}


/* -----------------------------------------------------------------------------
 * Exported/Public functions
 * -----------------------------------------------------------------------------
 */

int aiSystemPerformanceInit(void)
{
    printf("\r\n#\r\n");
    printf("# %s %d.%d\r\n", _APP_NAME_ , _APP_VERSION_MAJOR_,
            _APP_VERSION_MINOR_ );
    printf("#\r\n");

#if defined(__GNUC__)
    printf("Compiled with GCC %d.%d.%d\r\n", __GNUC__, __GNUC_MINOR__,
            __GNUC_PATCHLEVEL__);
#elif defined(__ICCARM__)
    printf("Compiled with IAR %d (build %d)\r\n", __IAR_SYSTEMS_ICC__,
            __BUILD_NUMBER__
    );
#elif defined (__CC_ARM)
    printf("Compiled with MDK-ARM Keil %d\r\n", __ARMCC_VERSION);
#endif

    crcIpInit();
    logDeviceConf();
    cyclesCounterInit();

    aiInit();

    srand(3); /* deterministic outcome */

    dwtReset();
    return 0;
}

int aiSystemPerformanceProcess(void)
{

    int idx = 0;
    int batch = 0;
    int y_pred;
    ai_buffer ai_input[AI_MNETWORK_IN_NUM];
    ai_buffer ai_output[AI_MNETWORK_OUT_NUM];

    ai_float input[1] = {0};  // initial
    ai_float output[1] = {0};

    if (net_exec_ctx[idx].handle == AI_HANDLE_NULL)
    {
        printf("E: network handle is NULL\r\n");
        return -1;
    }

    ai_input[0] = net_exec_ctx[idx].report.inputs[0];
    ai_output[0] = net_exec_ctx[idx].report.outputs[0];

//    ai_float test_data[] = {0, 1, 2, 3, 4, 5, 6, 7};

    for (int i=0; i < 999; i++)
    {
    	input[0] = rand()%20 - 15;  // 随机生成[-10, 10] 的值
    	output[0] = 0;
    	ai_input[0].data = AI_HANDLE_PTR(input);
    	ai_output[0].data = AI_HANDLE_PTR(output);
    	batch = ai_mnetwork_run(net_exec_ctx[idx].handle, &ai_input[0], &ai_output[0]);
    	if (batch != 1)
    	{
    		aiLogErr(ai_mnetwork_get_error(net_exec_ctx[idx].handle),
    				"ai_mnetwork_run");
    		break;
    	}
    	y_pred = 6 * input[0] + 10;
    	printf("input  : %.2f \r\n", input[0]);
    	printf("y_pre  : %.2f \r\n", output[0]);
    	printf("y_true : %d \r\n", y_pred);
    	printf("\r\n===========================\r\n\r\n\r\n");
    	HAL_Delay(5000);
    }

//    int r;
//    do {
//        r = aiTestPerformance(idx);
//        idx = (idx+1) % AI_MNETWORK_NUMBER;
//
//        if (!r) {
//            r = aiTestConsole();
//
//            if (r == CONS_EVT_UNDEFINED) {
//                r = 0;
//            } else if (r == CONS_EVT_HELP) {
//                printf("\r\n");
//                printf("Possible key for the interactive console:\r\n");
//                printf("  [q,Q]      quit the application\r\n");
//                printf("  [r,R]      re-start (NN de-init and re-init)\r\n");
//                printf("  [p,P]      pause\r\n");
//                printf("  [d,D]      hide detailed information ('r' to restore)\r\n");
//                printf("  [h,H,?]    this information\r\n");
//                printf("   xx        continue immediately\r\n");
//                printf("\r\n");
//                printf("Press any key to continue..\r\n");
//
//                while ((r = aiTestConsole()) == CONS_EVT_TIMEOUT) {
//                    HAL_Delay(1000);
//                }
//                if (r == CONS_EVT_UNDEFINED)
//                    r = 0;
//            }
//            if (r == CONS_EVT_PROF) {
//                profiling_mode = true;
//                profiling_factor *= 2;
//                r = 0;
//            }
//
//            if (r == CONS_EVT_HIDE) {
//            	hidden_mode = true;
//            	r = 0;
//            }
//
//            if (r == CONS_EVT_RESTART) {
//                profiling_mode = false;
//            	hidden_mode = false;
//                profiling_factor = 5;
//                printf("\r\n");
//                aiDeInit();
//                aiSystemPerformanceInit();
//                r = 0;
//            }
//            if (r == CONS_EVT_QUIT) {
//                profiling_mode = false;
//                printf("\r\n");
//                disableInts();
//                aiDeInit();
//                printf("\r\n");
//                printf("Board should be reseted...\r\n");
//                while (1) {
//                    HAL_Delay(1000);
//                }
//            }
//            if (r == CONS_EVT_PAUSE) {
//                printf("\r\n");
//                printf("Press any key to continue..\r\n");
//                while ((r = aiTestConsole()) == CONS_EVT_TIMEOUT) {
//                    HAL_Delay(1000);
//                }
//                r = 0;
//            }
//        }
//    } while (r==0);
//
//    return r;

}

void aiSystemPerformanceDeInit(void)
{
    printf("\r\n");
    aiDeInit();
    printf("bye bye ...\r\n");
}

