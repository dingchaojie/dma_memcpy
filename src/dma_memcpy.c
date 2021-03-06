/****************************************************************************
*
* Copyright (C) 2017, Jon Magnuson <my.name at google's mail service>
* All Rights Reserved.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along
* with this program; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*
****************************************************************************/


/*****************************************************************************
 *
 *                              INCLUDES
 *
 ****************************************************************************/

/* Standard includes */
#include <stdint.h>
#include <stdbool.h>

/* Platform includes */
#include "inc/hw_ints.h"
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "driverlib/fpu.h"
#include "driverlib/gpio.h"
#include "driverlib/interrupt.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/udma.h"

/* Application includes */
#include "dma_memcpy.h"

#if defined(ccs)
#include "ccs_atomic.h"                   /* atomic builtins not defined in ccs */
#endif

#define MAX_XFER_LEN 1024                 /* CM4F size limit for uDMA transfer */

/*****************************************************************************
 *
 *                          STATIC VARIABLES
 *
 *****************************************************************************/
static uint32_t udma_error_cnt = 0,       /* udma error count */
                udma_xfer_fail_cnt = 0,   /* udma failed transfer count */
                udma_xfer_succ_cnt = 0,   /* udma successful transfer count */
                udma_xfer_len = 0,        /* udma transfer length */
                udma_channel_lock = 0,    /* udma busy flag/lock */
                udma_is_initialized = 0,  /* udma initialization flag */
                udma_channel = NULL,      /* udma channel register */
                *udma_xfer_src = 0,       /* udma source pointer */
                *udma_xfer_dst = 0;       /* udma dest pointer */

typedef void (*fn_ptr_t)( int );          /* typedef with int as argument */
static fn_ptr_t udma_callback_ptr = NULL; /* udma callback function pointer */


/*****************************************************************************
 *
 *                   INTERRUPT & FUNCTION DEFINITIONS
 *
 ****************************************************************************/

/* Interrupt for general udma error */
void
uDMAErrorHandler(void)
{
    uint32_t ui32Status;

    /* Get udma error status */
    ui32Status = ROM_uDMAErrorStatusGet();

    if(ui32Status)
    {
        /* Clear udma error status */
        ROM_uDMAErrorStatusClear();

        /* Increment transmit failure count */
        udma_error_cnt++;

        /* Callback (error) */
        if (udma_callback_ptr)
        {
            (*udma_callback_ptr)(2);
            udma_callback_ptr = NULL;
        }
    }

    /* Set channel to free */
    udma_channel_lock = 0;
}

/*
 * Interrupt for udma transfer completion --
 *     handles successful and failed transfers
 */
void
uDMAIntHandler(void)
{
    static uint32_t len=0;
    uint32_t ui32Mode;

    /* Get udma channel mode */
    ui32Mode = ROM_uDMAChannelModeGet(udma_channel);

    if(ui32Mode == UDMA_MODE_STOP)
    {
        /* Increment successful transmit count */
        udma_xfer_succ_cnt++;

        /* Callback (success) */
        if (udma_callback_ptr && (udma_xfer_len==0))
        {
            (*udma_callback_ptr)(0);
            udma_callback_ptr = NULL;

        }
        else {
            len = (udma_xfer_len > MAX_XFER_LEN) ? MAX_XFER_LEN : udma_xfer_len;
            udma_xfer_len -= len;

            /* Set up udma transfer parameters */
            ROM_uDMAChannelTransferSet(
               udma_channel,
               UDMA_MODE_AUTO,
               udma_xfer_src,
               udma_xfer_dst,
               len
            );

            udma_xfer_src += len;
            udma_xfer_dst += len;

            ROM_uDMAChannelEnable(udma_channel);
            ROM_uDMAChannelRequest(udma_channel);
        }

    }
    else
    {
        /* Increment transmit failure count */
        udma_xfer_fail_cnt++;

        /* Callback (fail) */
        if (udma_callback_ptr)
        {
            (*udma_callback_ptr)(1);
            udma_callback_ptr = NULL;
        }
    }

    /* Set channel to free */
    udma_channel_lock = 0;
}


int
init_dma_memcpy(uint32_t chan)
{

    /* Disable DMA attributes */
    ROM_uDMAChannelAttributeDisable(
        chan,
          UDMA_ATTR_USEBURST
        | UDMA_ATTR_ALTSELECT
        | (UDMA_ATTR_HIGH_PRIORITY | UDMA_ATTR_REQMASK)
    );

    /* Configure DMA control parameters */
    ROM_uDMAChannelControlSet(
        chan,
          UDMA_SIZE_32
        | UDMA_SRC_INC_32
        | UDMA_DST_INC_32
        | UDMA_ARB_8
    );

    udma_is_initialized = 1;

    return 0; /* return success */
}

int __inline
dma_memcpy(uint32_t *dst, uint32_t *src, size_t len, uint32_t chan, void *cb)
{

    /* Return busy if udma channel is in use, otherwise set busy and continue */
    if (!__sync_bool_compare_and_swap(&udma_channel_lock, 0,  1))
    {
        return 1;
    }

    if (!udma_is_initialized)
    {
        /* Just initialize and proceed to memcpy */
        init_dma_memcpy(chan);
    }

    /* Set udma channel */
    udma_channel = chan;

    /* Point static callback ptr to input */
    if (cb)
    {
        udma_callback_ptr = (fn_ptr_t)cb;
    }

    udma_xfer_len = len;

    len = (udma_xfer_len > MAX_XFER_LEN) ? MAX_XFER_LEN : udma_xfer_len;
    udma_xfer_len -= len;

    /* Set up udma transfer parameters */
    ROM_uDMAChannelTransferSet(
        chan | UDMA_PRI_SELECT,
        UDMA_MODE_AUTO,
        src,
        dst,
        len
    );

    /* Increment before starting transfer */
    udma_xfer_src = src + len;
    udma_xfer_dst = dst + len;

    /* Start udma transfer */
    ROM_uDMAChannelEnable(chan);
    ROM_uDMAChannelRequest(chan);

    return 0; /* return success */
}
