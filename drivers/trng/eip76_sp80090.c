/*
 * Copyright (C) 2017-2020 Alibaba Group Holding Limited
 */

/* eip76_sp80090.c
 *
 * Module implements the SP 800-90 Post Processor interface
 */


/*----------------------------------------------------------------------------
 * This module implements (provides) the following interface(s):
 */

// EIP-76 External Post Processor Interface
#include "eip76_pp.h"

// EIP-76 Internal Post Processor Interface
#include "eip76_internal_pp.h"  // EIP76_Internal_PostProcessor_*


/*----------------------------------------------------------------------------
 * This module uses (requires) the following interface(s):
 */

// Default configuration
#include "c_eip76.h"

// Driver Framework Basic Definitions API
#include "basic_defs.h"         // uint32_t

// Driver Framework Device API
#include "device_types.h"       // Device_Handle_t

// EIP-76 Driver Library Types API
#include "eip76_types.h"        // EIP76_* types

// EIP-76 Driver Library Internal interfaces
#include "eip76_level0.h"       // Level 0 macros
#include "eip76_internal.h"     // Internal macros
#include "eip76_fsm.h"          // State machine


/*----------------------------------------------------------------------------
 * Definitions and macros
 */


/*----------------------------------------------------------------------------
 * EIP76Lib_PS_AI_Write
 *
 */
static EIP76_Status_t
EIP76Lib_PS_AI_Write(
        const Device_Handle_t Device,
        const uint32_t * PS_AI_Data_p,
        const unsigned int PS_AI_WordCount,
        EIP76_EventStatus_t * const Events_p)
{
    uint32_t RegVal;

    RegVal = EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (RegVal & EIP76_EVENTS_MASK);

    // Ensure test ready state before writing AI for re-seed
    if(((RegVal & EIP76_STATUS_TEST_READY) == 0 ) &&
       ((RegVal & EIP76_STATUS_RESEED_AI) == 0 ))
        return EIP76_ILLEGAL_IN_STATE;

    EIP76_Internal_PostProcessor_PS_AI_Write(Device,
                                             PS_AI_Data_p,
                                             PS_AI_WordCount);

    // CDS point: check if PS / AI word 11 is written,
    // if not then write a dummy word for CDS with device
    if( PS_AI_WordCount < EIP76_MAX_PS_AI_WORD_COUNT )
        EIP76_Write32(Device, EIP76_REG_PS_AI_11, 0);

    return EIP76_NO_ERROR;
}


/*----------------------------------------------------------------------------
 * EIP76_Internal_PostProcessor_PS_AI_Write
 *
 */
void
EIP76_Internal_PostProcessor_PS_AI_Write(
        const Device_Handle_t Device,
        const uint32_t * PS_AI_Data_p,
        const unsigned int PS_AI_WordCount)
{
    unsigned int i;

    for(i = 0; i < PS_AI_WordCount; i++)
        EIP76_Write32(Device,
                      (unsigned int)(EIP76_REG_PS_AI_0 + i * sizeof(uint32_t)),
                      PS_AI_Data_p[i]);
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_BlockCount_Get
 *
 * Counter for 128 bits blocks generated by the post-processor, forced to zero
 * when the post-processor is disabled, cleared to zero when an internal
 * re-seed operation has finished. This register can be used
 * to determine when to re-seed the post-processor.
 *
 * In the case of SP 800-90 post-processing (EIP-76d), three 128-bit blocks
 * are post-processed from 384 bits of entropy resulting from a ‘Generate’
 * operation. Therefore, this counter runs 3 times as fast and does not count
 * the number of ‘Generate’ operations performed then.
 */
EIP76_Status_t
EIP76_PostProcessor_BlockCount_Get(
        EIP76_IOArea_t * const IOArea_p,
        uint32_t * const BlockCount_p)
{
#if (EIP76_POST_PROCESSOR_TYPE == EIP76_POST_PROCESSOR_NONE)
    IDENTIFIER_NOT_USED(IOArea_p);
    *BlockCount_p = 0;
#else
    Device_Handle_t Device;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);

    EIP76_CHECK_POINTER(BlockCount_p);

    Device = TrueIOArea_p->Device;

    *BlockCount_p = EIP76_BLOCKCNT_RD_BLOCKCOUNT(Device);
#endif

    return EIP76_NO_ERROR;
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_IsBusy
 *
 */
EIP76_Status_t
EIP76_PostProcessor_IsBusy(
        EIP76_IOArea_t * const IOArea_p,
        EIP76_EventStatus_t * const Events_p)
{
    Device_Handle_t Device;
    uint32_t StatusRegVal, ControlRegVal;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);

    EIP76_CHECK_POINTER(Events_p);

    // No events detected yet
    *Events_p = 0;

    Device = TrueIOArea_p->Device;

    StatusRegVal = EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (StatusRegVal & EIP76_EVENTS_MASK);

    // Check if re-seed is ready
    ControlRegVal = EIP76_CONTROL_RD(Device);

    // Check if re-seed is still ongoing
    if ( (ControlRegVal & EIP76_CONTROL_ENABLE_RESEED) == 0 )
    {
        // Re-seed operation is ready, transit to a new state
        return EIP76_State_Set((volatile EIP76_State_t* const)&TrueIOArea_p->State,
                               EIP76_STATE_RANDOM_GENERATING);
    }
    else
    {
        // Re-seed is not ready,
        // remain in EIP76_STATE_SP80090_RESEED_START state
        return EIP76_BUSY_RETRY_LATER;
    }
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_Reseed_Start
 *
 */
EIP76_Status_t
EIP76_PostProcessor_Reseed_Start(
        EIP76_IOArea_t * const IOArea_p,
        EIP76_EventStatus_t * const Events_p)
{
    Device_Handle_t Device;
    uint32_t RegVal;
    EIP76_Status_t rv;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

#if (EIP76_POST_PROCESSOR_TYPE == EIP76_POST_PROCESSOR_BC_DF)
    uint32_t Mask = EIP76_STATUS_RESEED_AI;
#else
    uint32_t Mask = EIP76_STATUS_TEST_READY;
#endif

    EIP76_CHECK_POINTER(IOArea_p);

    EIP76_CHECK_POINTER(Events_p);

    // No events detected yet
    *Events_p = 0;

    // Transit to a new state
    rv = EIP76_State_Set((volatile EIP76_State_t*)&TrueIOArea_p->State,
                         EIP76_STATE_SP80090_RESEED_START);
    if (rv != EIP76_NO_ERROR)
    {
        return rv;
    }

    Device = TrueIOArea_p->Device;

    RegVal = EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (RegVal & EIP76_EVENTS_MASK);

    /* 7 step */
    printf("===%s, %d\n", __FUNCTION__, __LINE__);
    EIP76_Write32(NULL, EIP76_REG_CONTROL, 0x10000);
    printf("===%s, %d\n", __FUNCTION__, __LINE__);
    // Start Post Processor re-seed
    EIP76_CONTROL_WR(Device, EIP76_CONTROL_ENABLE_RESEED);

    // Check if re-seed is ready
    RegVal = EIP76_STATUS_RD(Device);
    while ( (RegVal & Mask) == 0 ) {
        RegVal = EIP76_STATUS_RD(Device);
    }

    // if ( (RegVal & Mask) == 0 )
    // {
    //     // Re-seed is not ready
    //     return EIP76_BUSY_RETRY_LATER;
    // }

    // Transit to a new state
    return EIP76_State_Set((volatile EIP76_State_t*)&TrueIOArea_p->State,
            EIP76_STATE_SP80090_RESEED_READY);

}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_Reseed_Write
 *
 */

EIP76_Status_t
EIP76_PostProcessor_Reseed_Write(
        EIP76_IOArea_t * const IOArea_p,
        const uint32_t * PS_AI_Data_p,
        const unsigned int PS_AI_WordCount,
        EIP76_EventStatus_t * const Events_p)
{
    Device_Handle_t Device;
    EIP76_Status_t rv;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);

    EIP76_CHECK_POINTER(PS_AI_Data_p);

    EIP76_CHECK_INT_INRANGE(PS_AI_WordCount,
                            EIP76_MIN_PS_AI_WORD_COUNT,
                            EIP76_MAX_PS_AI_WORD_COUNT);

    EIP76_CHECK_POINTER(Events_p);

    // No events detected yet
    *Events_p = 0;

    Device = TrueIOArea_p->Device;

    rv = EIP76Lib_PS_AI_Write(Device, PS_AI_Data_p, PS_AI_WordCount, Events_p);
    if( rv != EIP76_NO_ERROR )
        return rv;
        
    /* 11 step */
    printf("===%s, %d\n", __FUNCTION__, __LINE__);
    while ((EIP76_Read32(NULL, EIP76_REG_CONTROL) & 0x00008000) !=
            0)
            ;
    printf("===%s, %d\n", __FUNCTION__, __LINE__);

    // Transit to a new state
    rv = EIP76_State_Set((volatile EIP76_State_t* const)&TrueIOArea_p->State,
                         EIP76_STATE_SP80090_RESEED_WRITING);
    if( rv != EIP76_NO_ERROR )
        return rv;

    //return EIP76_BUSY_RETRY_LATER;
    /* NOTE debug */
    return EIP76_NO_ERROR;
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_NIST_Write
 *
 */
EIP76_Status_t
EIP76_PostProcessor_NIST_Write(
        EIP76_IOArea_t * const IOArea_p,
        const uint32_t * PS_AI_Data_p,
        const unsigned int PS_AI_WordCount,
        const unsigned int VectorType,
        EIP76_EventStatus_t * const Events_p)
{
    Device_Handle_t Device;
    EIP76_Status_t rv;

    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);

    EIP76_CHECK_POINTER(PS_AI_Data_p);

    EIP76_CHECK_INT_INRANGE(PS_AI_WordCount,
                            EIP76_MIN_PS_AI_WORD_COUNT,
                            EIP76_MAX_PS_AI_WORD_COUNT);

    EIP76_CHECK_POINTER(Events_p);

    // No events detected yet
    *Events_p = 0;

    Device = TrueIOArea_p->Device;

    // Read and discard the output data so that
    // EIP76_PostProcessor_Result_Read can read the right test result
    if(VectorType != 0)
    {
        EIP76_OUTPUT_0_RD(Device);
        EIP76_OUTPUT_1_RD(Device);
        EIP76_OUTPUT_2_RD(Device);
        EIP76_OUTPUT_3_RD(Device);
    }

    rv = EIP76Lib_PS_AI_Write(Device, PS_AI_Data_p, PS_AI_WordCount, Events_p);
    if( rv != EIP76_NO_ERROR )
        return rv;

    return EIP76_State_Set((volatile EIP76_State_t*)&TrueIOArea_p->State,
                           EIP76_STATE_KAT_SP80090_PROCESSING);
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_PS_AI_Write
 *
 */
EIP76_Status_t
EIP76_PostProcessor_PS_AI_Write(
        EIP76_IOArea_t * const IOArea_p,
        const uint32_t * PS_AI_Data_p,
        const unsigned int PS_AI_WordCount,
        EIP76_EventStatus_t * const Events_p)
{
    Device_Handle_t Device;
    uint32_t RegVal;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);
    EIP76_CHECK_POINTER(PS_AI_Data_p);
    EIP76_CHECK_INT_INRANGE(PS_AI_WordCount,
                            EIP76_MIN_PS_AI_WORD_COUNT,
                            EIP76_MAX_PS_AI_WORD_COUNT);
    EIP76_CHECK_POINTER(Events_p);

    Device = TrueIOArea_p->Device;

    RegVal = EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (RegVal & EIP76_EVENTS_MASK);

    EIP76_Internal_PostProcessor_PS_AI_Write(Device,
                                             PS_AI_Data_p,
                                             PS_AI_WordCount);

    // Check if PS / AI word 11 is written.
    // If not then write a dummy word for CDS with device
    if( PS_AI_WordCount < EIP76_MAX_PS_AI_WORD_COUNT )
        EIP76_Write32(Device, EIP76_REG_PS_AI_11, 0);

    return EIP76_State_Set(
                (volatile EIP76_State_t* const)&TrueIOArea_p->State,
                EIP76_STATE_RANDOM_GENERATING);
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_Key_Write
 *
 */
EIP76_Status_t
EIP76_PostProcessor_Key_Write(
        EIP76_IOArea_t * const IOArea_p,
        const uint32_t * Key_Data_p)
{
    Device_Handle_t Device;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);

    EIP76_CHECK_POINTER(Key_Data_p);

    Device = TrueIOArea_p->Device;

    // Write 8 32-bit words as key-data, specific for SP 800-90 PP
    EIP76_KEY_WR(Device, Key_Data_p, 8);

    return EIP76_NO_ERROR;
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_Input_Write
 *
 * SP 800-90 AES-256 Core known-answer test only!
 */
EIP76_Status_t
EIP76_PostProcessor_Input_Write(
        EIP76_IOArea_t * const IOArea_p,
        const uint32_t * Input_Data_p,
        EIP76_EventStatus_t * const Events_p)
{
    Device_Handle_t Device;
    uint32_t StatusRegVal;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);

    EIP76_CHECK_POINTER(Events_p);

    EIP76_CHECK_POINTER(Input_Data_p);

    // No events detected yet
    *Events_p = 0;

    Device = TrueIOArea_p->Device;

    StatusRegVal = EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (StatusRegVal & EIP76_EVENTS_MASK);

    // Write the input data
    EIP76_INPUT_0_WR(Device, Input_Data_p[0]);
    EIP76_INPUT_1_WR(Device, Input_Data_p[1]);
    EIP76_INPUT_2_WR(Device, Input_Data_p[2]);
    // CDS point: device takes over here
    EIP76_INPUT_3_WR(Device, Input_Data_p[3]);

    // Input data written, transit to a new state
    return EIP76_State_Set((volatile EIP76_State_t*)&TrueIOArea_p->State,
                           EIP76_STATE_KAT_SP80090_PROCESSING);
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_Result_Read
 *
 * This function can also be used for the SP 800-90 Post Processor to read
 * 1) result of the AES-256 Core known-answer test
 * 2) result of the NIST known-answer test on the complete Post Processor
 */
EIP76_Status_t
EIP76_PostProcessor_Result_Read(
        EIP76_IOArea_t * const IOArea_p,
        uint32_t * Output_Data_p,
        EIP76_EventStatus_t * const Events_p)
{
    Device_Handle_t Device;
    uint32_t RegVal;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);

    EIP76_CHECK_POINTER(Events_p);

    EIP76_CHECK_POINTER(Output_Data_p);

    // No events detected yet
    *Events_p = 0;

    Device = TrueIOArea_p->Device;

    RegVal = EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (RegVal & EIP76_EVENTS_MASK);

    // Ensure test ready state before reading out test result
    if( (RegVal & EIP76_STATUS_TEST_READY) == 0 )
        return EIP76_ILLEGAL_IN_STATE;

    Output_Data_p[0] = EIP76_OUTPUT_0_RD(Device);
    Output_Data_p[1] = EIP76_OUTPUT_1_RD(Device);
    Output_Data_p[2] = EIP76_OUTPUT_2_RD(Device);
    Output_Data_p[3] = EIP76_OUTPUT_3_RD(Device);

    // Leave Test Mode
    RegVal = EIP76_TEST_RD(Device);
    // Clear all existing tests that could have been started
    RegVal &= (~( EIP76_TEST_POST_PROC | EIP76_TEST_SP_800_90 | EIP76_TEST_KNOWN_NOISE));
    EIP76_TEST_WR(Device, RegVal);

    // Restore TRNG_CONTROL register (internal TRNG HW state) stored
    // when the test was started
    EIP76_CONTROL_WR(Device, TrueIOArea_p->SavedControl);

    // Input data written, transit to a new state
    return EIP76_State_Set((volatile EIP76_State_t*)&TrueIOArea_p->State,
                           EIP76_STATE_RANDOM_GENERATING);
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_IsReady
 *
 */
EIP76_Status_t
EIP76_PostProcessor_IsReady(
        EIP76_IOArea_t * const IOArea_p,
        EIP76_EventStatus_t * const Events_p)
{
    Device_Handle_t Device;
    uint32_t StatusRegVal;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);

    EIP76_CHECK_POINTER(Events_p);

    // No events detected yet
    *Events_p = 0;

    Device = TrueIOArea_p->Device;

    StatusRegVal = EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (StatusRegVal & EIP76_EVENTS_MASK);

#if (EIP76_POST_PROCESSOR_TYPE == EIP76_POST_PROCESSOR_BC_DF)
    if ((StatusRegVal & EIP76_STATUS_RESEED_AI) != 0 )
    {
        // Goto next state.
        return EIP76_State_Set((volatile EIP76_State_t* const)&TrueIOArea_p->State,
                               EIP76_STATE_SP80090_RESEED_READY);
    }
#else
    if ((StatusRegVal & EIP76_STATUS_TEST_READY) != 0 )
    {
        // Goto next state.
        return EIP76_State_Set((volatile EIP76_State_t* const)&TrueIOArea_p->State,
                                EIP76_STATE_RANDOM_GENERATING);
    }
#endif
    else
    {
        // reseed_ai/test bit is not active
        return EIP76_BUSY_RETRY_LATER;
    }
}


#if (EIP76_POST_PROCESSOR_TYPE == EIP76_POST_PROCESSOR_BC_DF)
/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_BCDF_PS_AI_Write
 *
 */
EIP76_Status_t
EIP76_PostProcessor_BCDF_PS_AI_Write(
        EIP76_IOArea_t * const IOArea_p,
        const uint32_t * PS_AI_Data_p,
        const unsigned int PS_AI_WordCount,
        EIP76_EventStatus_t * const Events_p)
{
    Device_Handle_t Device;
    uint32_t RegVal;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);
    EIP76_CHECK_POINTER(PS_AI_Data_p);
    EIP76_CHECK_INT_INRANGE(PS_AI_WordCount,
                            EIP76_MAX_PS_AI_WORD_COUNT,
                            EIP76_MAX_PS_AI_WORD_COUNT);
    EIP76_CHECK_POINTER(Events_p);

    Device = TrueIOArea_p->Device;

    RegVal = EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (RegVal & EIP76_EVENTS_MASK);

    EIP76_Internal_PostProcessor_PS_AI_Write(Device,
                                             PS_AI_Data_p,
                                             PS_AI_WordCount);

    TrueIOArea_p->Index = 0;

    return EIP76_State_Set(
                (volatile EIP76_State_t* const)&TrueIOArea_p->State,
                EIP76_STATE_KAT_SP80090_BCDF_RESEEDED);
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_BCDF_Noise_Write
 *
 */
EIP76_Status_t
EIP76_PostProcessor_BCDF_Noise_Write(
        EIP76_IOArea_t * const IOArea_p,
        uint32_t * const Noise_Data,
        const unsigned int Noise_Count)
{
    uint32_t RegValue;
    Device_Handle_t Device;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);

    Device = TrueIOArea_p->Device;

    RegValue =
         (((Noise_Data[TrueIOArea_p->Index] & 0x7FFFFFFF) << 1) |
          ((Noise_Data[TrueIOArea_p->Index+1] >> 31) & 0x00000001));

    EIP76_MAINSHIFTREG_L_WR(Device, RegValue);

    RegValue =
          (((Noise_Data[TrueIOArea_p->Index+1] & 0x7FFFFFFF) << 1) |
            ((Noise_Data[TrueIOArea_p->Index] >> 31) & 0x00000001));

    EIP76_MAINSHIFTREG_H_WR(Device, RegValue);

    TrueIOArea_p->Index += 2;

    if (TrueIOArea_p->Index >= Noise_Count)
        TrueIOArea_p->Index = 0; // Reset index for next loop

    // One noise block is written, transit to a new state
    return EIP76_State_Set(
                    (volatile EIP76_State_t* const)&TrueIOArea_p->State,
                    EIP76_STATE_KAT_SP80090_BCDF_NOISE);
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_BCDF_Status_Get
 *
 */
EIP76_Status_t
EIP76_PostProcessor_BCDF_Status_Get(
        EIP76_IOArea_t * const IOArea_p,
        EIP76_EventStatus_t * const Events_p)
{
    EIP76_Status_t rv;
    Device_Handle_t Device;
    uint32_t StatusRegVal;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);
    EIP76_CHECK_POINTER(Events_p);

    Device = TrueIOArea_p->Device;

    StatusRegVal = EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (StatusRegVal & EIP76_EVENTS_MASK);

    if (StatusRegVal & EIP76_STATUS_TEST_READY)
    {
        // Raw noise block is processed, check if the last one
        if (TrueIOArea_p->Index)
        {
            EIP76_Status_t rv = EIP76_State_Set(
                        (volatile EIP76_State_t* const)&TrueIOArea_p->State,
                        EIP76_STATE_KAT_SP80090_BCDF_RESEEDED);
            if( rv != EIP76_NO_ERROR )
                return rv;

            // Not all noise blocks are processed yet, more input data needed
            return EIP76_PROCESSING;
        }
        else
            // All noise blocks are processed
            return EIP76_State_Set(
                        (volatile EIP76_State_t* const)&TrueIOArea_p->State,
                        EIP76_STATE_KAT_SP80090_BCDF_READY);
    }

    // status is not ready, stay in current state
    rv = EIP76_State_Set((volatile EIP76_State_t* const)&TrueIOArea_p->State,
                         EIP76_STATE_KAT_SP80090_BCDF_NOISE);
    if (rv != EIP76_NO_ERROR)
        return rv;

    return EIP76_BUSY_RETRY_LATER;
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_BCDF_Generate_Start
 *
 */
EIP76_Status_t
EIP76_PostProcessor_BCDF_Generate_Start(
        EIP76_IOArea_t * const IOArea_p,
        const unsigned int WordCount,
        EIP76_EventStatus_t * const Events_p)
{
    Device_Handle_t Device;
    uint32_t ControlValue, StatusValue, AvailBlkCnt, ReqBlkCnt, WriteValue;

    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);

    Device = TrueIOArea_p->Device;

    ControlValue = EIP76_CONTROL_RD(Device);

    // First check if number of data_bloacks is zero, else return
    if ((ControlValue >> 20) & MASK_12_BITS)
        return EIP76_BUSY_RETRY_LATER;

    // Calculate requested 128-bit random data blocks
    ReqBlkCnt = (WordCount + 3) / 4; // Round up

    StatusValue =  EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (StatusValue & EIP76_EVENTS_MASK);

    // Get the number of available 128-bit random data blocks
    AvailBlkCnt = (StatusValue & MASK_1_BIT) +          // in output registers
                   ((StatusValue >> 16) & MASK_8_BITS); // in buffer RAM

    // Check if requested number of bytes is already available
    if(AvailBlkCnt < ReqBlkCnt)
    {
        EIP76_CHECK_INT_ATMOST(ReqBlkCnt - AvailBlkCnt,
                               EIP76_REQUEST_DATA_MAX_BLK_COUNT);

        // Only data_blocks field is updated in register
        WriteValue = EIP76_REQUEST_DATA |
                      (((ReqBlkCnt - AvailBlkCnt) & MASK_12_BITS) << 20);

        EIP76_CONTROL_WR(Device, WriteValue);
    }

    // Transit to a new state
    return EIP76_State_Set(
            (volatile EIP76_State_t* const)&TrueIOArea_p->State,
            EIP76_STATE_KAT_SP80090_BCDF_PROCESSING);
}


/*----------------------------------------------------------------------------
 * EIP76_PostProcessor_BCDF_Result_Read
 *
 */
EIP76_Status_t
EIP76_PostProcessor_BCDF_Result_Read(
        EIP76_IOArea_t * const IOArea_p,
        EIP76_EventStatus_t * const Events_p,
        uint32_t * const Data_p,
        const unsigned int Data_WordCount)
{
    EIP76_Status_t rv;
    Device_Handle_t Device;
    uint32_t RegVal;
    volatile EIP76_True_IOArea_t * const TrueIOArea_p = IOAREA(IOArea_p);

    EIP76_CHECK_POINTER(IOArea_p);
    EIP76_CHECK_POINTER(Events_p);
    EIP76_CHECK_INT_ATMOST(Data_WordCount, (unsigned int)MASK_31_BITS);

    Device = TrueIOArea_p->Device;

    RegVal = EIP76_STATUS_RD(Device);

    // Store event status
    *Events_p = (RegVal & EIP76_EVENTS_MASK);

    if (EIP76_STATUS_IS_READY(RegVal))
    {
        Data_p[TrueIOArea_p->Index + 0] = EIP76_OUTPUT_0_RD(Device);
        Data_p[TrueIOArea_p->Index + 1] = EIP76_OUTPUT_1_RD(Device);
        Data_p[TrueIOArea_p->Index + 2] = EIP76_OUTPUT_2_RD(Device);
        Data_p[TrueIOArea_p->Index + 3] = EIP76_OUTPUT_3_RD(Device);

        TrueIOArea_p->Index += 4;

        // Clear ready bit when done reading result
        EIP76_INTACK_WR(Device, CLEAR_READY_BIT);

        if (TrueIOArea_p->Index >= Data_WordCount)
        {
            // Reset back for next loop
            TrueIOArea_p->Index = 0;

            // Check if 2nd Generate function must be requested
            if (TrueIOArea_p->Flag)
            {
                // Leave Test Mode
                RegVal = EIP76_TEST_RD(Device);

                // Clear all existing tests that could have been started
                RegVal &= (~(EIP76_TEST_POST_PROC |
                             EIP76_TEST_SP_800_90 |
                             EIP76_TEST_KNOWN_NOISE));
                EIP76_TEST_WR(Device, RegVal);

                // Restore TRNG_CONTROL register (internal TRNG HW state) stored
                // when the test was started
                EIP76_CONTROL_WR(Device, TrueIOArea_p->SavedControl);

                TrueIOArea_p->Flag = false;

                // Advance the FSM to prepare for the Personalization String
                // re-write after test
                rv = EIP76_State_Set(
                        (volatile EIP76_State_t* const)&TrueIOArea_p->State,
                        EIP76_STATE_RANDOM_GENERATING);
                if (rv != EIP76_NO_ERROR)
                    return rv;

                // Now the FSM is ready for EIP76_PostProcessor_IsReady()
                // and consequent EIP76_PostProcessor_Reseed_Write() calls
                // to re-write the Personalization String
                return EIP76_State_Set(
                        (volatile EIP76_State_t* const)&TrueIOArea_p->State,
                        EIP76_STATE_SP80090_RESEED_START);
            }
            else
            {
                // Ignore the result of the 1st Generate function and
                // repeat part of the test for the 2nd Generate function
                rv = EIP76_State_Set(
                        (volatile EIP76_State_t* const)&TrueIOArea_p->State,
                        EIP76_STATE_KAT_START);
                if (rv != EIP76_NO_ERROR)
                    return rv;

                // Request 2nd Generate function
                TrueIOArea_p->Flag = true;

                // Request a re-seed
                EIP76_CONTROL_WR(Device, EIP76_CONTROL_ENABLE_RESEED);

                return EIP76_PROCESSING;
            }
        }
    }

    // Requested random data is not ready or
    // not all requested data blocks are red, stay in current state
    rv = EIP76_State_Set((volatile EIP76_State_t* const)&TrueIOArea_p->State,
                         EIP76_STATE_KAT_SP80090_BCDF_PROCESSING);
    if (rv != EIP76_NO_ERROR)
        return rv;

    return EIP76_BUSY_RETRY_LATER;
}
#endif  // (EIP76_POST_PROCESSOR_TYPE == EIP76_POST_PROCESSOR_BC_DF)
/* end of file eip76_sp80090.c */
