/* $Id$ */
/** @file
 * IOM - Input / Output Monitor - Any Context, MMIO & String I/O.
 */

/*
 * Copyright (C) 2006-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_IOM
#include <VBox/vmm/iom.h>
#include <VBox/vmm/cpum.h>
#include <VBox/vmm/pgm.h>
#include <VBox/vmm/selm.h>
#include <VBox/vmm/mm.h>
#include <VBox/vmm/em.h>
#include <VBox/vmm/pgm.h>
#include <VBox/vmm/trpm.h>
#include <VBox/vmm/iem.h>
#include "IOMInternal.h"
#include <VBox/vmm/vm.h>
#include <VBox/vmm/vmm.h>
#include <VBox/vmm/hm.h>
#include "IOMInline.h"

#include <VBox/dis.h>
#include <VBox/disopcode.h>
#include <VBox/vmm/pdmdev.h>
#include <VBox/param.h>
#include <VBox/err.h>
#include <iprt/assert.h>
#include <VBox/log.h>
#include <iprt/asm.h>
#include <iprt/string.h>


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** @def IEM_USE_IEM_INSTEAD
 * Use IEM instead of IOM for interpreting MMIO accesses.
 * Because of PATM/CSAM issues in raw-mode, we've split this up into 2nd and 3rd
 * IEM deployment step. */
#if  ((defined(IN_RING3) || defined(IN_RING0)) && defined(VBOX_WITH_2ND_IEM_STEP)) \
  || defined(VBOX_WITH_3RD_IEM_STEP)
# define IEM_USE_IEM_INSTEAD
#endif


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/

/**
 * Array for fast recode of the operand size (1/2/4/8 bytes) to bit shift value.
 */
static const unsigned g_aSize2Shift[] =
{
    ~0U,   /* 0 - invalid */
    0,     /* *1 == 2^0 */
    1,     /* *2 == 2^1 */
    ~0U,   /* 3 - invalid */
    2,     /* *4 == 2^2 */
    ~0U,   /* 5 - invalid */
    ~0U,   /* 6 - invalid */
    ~0U,   /* 7 - invalid */
    3      /* *8 == 2^3 */
};

/**
 * Macro for fast recode of the operand size (1/2/4/8 bytes) to bit shift value.
 */
#define SIZE_2_SHIFT(cb)    (g_aSize2Shift[cb])


/**
 * Returns the contents of register or immediate data of instruction's parameter.
 *
 * @returns true on success.
 *
 * @todo Get rid of this code. Use DISQueryParamVal instead
 *
 * @param   pCpu                Pointer to current disassembler context.
 * @param   pParam              Pointer to parameter of instruction to process.
 * @param   pRegFrame           Pointer to CPUMCTXCORE guest structure.
 * @param   pu64Data            Where to store retrieved data.
 * @param   pcbSize             Where to store the size of data (1, 2, 4, 8).
 */
bool iomGetRegImmData(PDISCPUSTATE pCpu, PCDISOPPARAM pParam, PCPUMCTXCORE pRegFrame, uint64_t *pu64Data, unsigned *pcbSize)
{
    NOREF(pCpu);
    if (pParam->fUse & (DISUSE_BASE | DISUSE_INDEX | DISUSE_SCALE | DISUSE_DISPLACEMENT8 | DISUSE_DISPLACEMENT16 | DISUSE_DISPLACEMENT32))
    {
        *pcbSize  = 0;
        *pu64Data = 0;
        return false;
    }

    /* divide and conquer */
    if (pParam->fUse & (DISUSE_REG_GEN64 | DISUSE_REG_GEN32 | DISUSE_REG_GEN16 | DISUSE_REG_GEN8))
    {
        if (pParam->fUse & DISUSE_REG_GEN32)
        {
            *pcbSize  = 4;
            DISFetchReg32(pRegFrame, pParam->Base.idxGenReg, (uint32_t *)pu64Data);
            return true;
        }

        if (pParam->fUse & DISUSE_REG_GEN16)
        {
            *pcbSize  = 2;
            DISFetchReg16(pRegFrame, pParam->Base.idxGenReg, (uint16_t *)pu64Data);
            return true;
        }

        if (pParam->fUse & DISUSE_REG_GEN8)
        {
            *pcbSize  = 1;
            DISFetchReg8(pRegFrame, pParam->Base.idxGenReg, (uint8_t *)pu64Data);
            return true;
        }

        Assert(pParam->fUse & DISUSE_REG_GEN64);
        *pcbSize  = 8;
        DISFetchReg64(pRegFrame, pParam->Base.idxGenReg, pu64Data);
        return true;
    }
    else
    {
        if (pParam->fUse & (DISUSE_IMMEDIATE64 | DISUSE_IMMEDIATE64_SX8))
        {
            *pcbSize  = 8;
            *pu64Data = pParam->uValue;
            return true;
        }

        if (pParam->fUse & (DISUSE_IMMEDIATE32 | DISUSE_IMMEDIATE32_SX8))
        {
            *pcbSize  = 4;
            *pu64Data = (uint32_t)pParam->uValue;
            return true;
        }

        if (pParam->fUse & (DISUSE_IMMEDIATE16 | DISUSE_IMMEDIATE16_SX8))
        {
            *pcbSize  = 2;
            *pu64Data = (uint16_t)pParam->uValue;
            return true;
        }

        if (pParam->fUse & DISUSE_IMMEDIATE8)
        {
            *pcbSize  = 1;
            *pu64Data = (uint8_t)pParam->uValue;
            return true;
        }

        if (pParam->fUse & DISUSE_REG_SEG)
        {
            *pcbSize  = 2;
            DISFetchRegSeg(pRegFrame, (DISSELREG)pParam->Base.idxSegReg, (RTSEL *)pu64Data);
            return true;
        } /* Else - error. */

        AssertFailed();
        *pcbSize  = 0;
        *pu64Data = 0;
        return false;
    }
}


/**
 * Saves data to 8/16/32 general purpose or segment register defined by
 * instruction's parameter.
 *
 * @returns true on success.
 * @param   pCpu                Pointer to current disassembler context.
 * @param   pParam              Pointer to parameter of instruction to process.
 * @param   pRegFrame           Pointer to CPUMCTXCORE guest structure.
 * @param   u64Data             8/16/32/64 bit data to store.
 */
bool iomSaveDataToReg(PDISCPUSTATE pCpu, PCDISOPPARAM pParam, PCPUMCTXCORE pRegFrame, uint64_t u64Data)
{
    NOREF(pCpu);
    if (pParam->fUse & (DISUSE_BASE | DISUSE_INDEX | DISUSE_SCALE | DISUSE_DISPLACEMENT8 | DISUSE_DISPLACEMENT16 | DISUSE_DISPLACEMENT32 | DISUSE_DISPLACEMENT64 | DISUSE_IMMEDIATE8 | DISUSE_IMMEDIATE16 | DISUSE_IMMEDIATE32 | DISUSE_IMMEDIATE32_SX8 | DISUSE_IMMEDIATE16_SX8))
    {
        return false;
    }

    if (pParam->fUse & DISUSE_REG_GEN32)
    {
        DISWriteReg32(pRegFrame, pParam->Base.idxGenReg, (uint32_t)u64Data);
        return true;
    }

    if (pParam->fUse & DISUSE_REG_GEN64)
    {
        DISWriteReg64(pRegFrame, pParam->Base.idxGenReg, u64Data);
        return true;
    }

    if (pParam->fUse & DISUSE_REG_GEN16)
    {
        DISWriteReg16(pRegFrame, pParam->Base.idxGenReg, (uint16_t)u64Data);
        return true;
    }

    if (pParam->fUse & DISUSE_REG_GEN8)
    {
        DISWriteReg8(pRegFrame, pParam->Base.idxGenReg, (uint8_t)u64Data);
        return true;
    }

    if (pParam->fUse & DISUSE_REG_SEG)
    {
        DISWriteRegSeg(pRegFrame, (DISSELREG)pParam->Base.idxSegReg, (RTSEL)u64Data);
        return true;
    }

    /* Else - error. */
    return false;
}


/**
 * Deals with complicated MMIO writes.
 *
 * Complicated means unaligned or non-dword/qword sized accesses depending on
 * the MMIO region's access mode flags.
 *
 * @returns Strict VBox status code. Any EM scheduling status code,
 *          VINF_IOM_R3_MMIO_WRITE, VINF_IOM_R3_MMIO_READ_WRITE or
 *          VINF_IOM_R3_MMIO_READ may be returned.
 *
 * @param   pVM                 Pointer to the VM.
 * @param   pRange              The range to write to.
 * @param   GCPhys              The physical address to start writing.
 * @param   pvValue             Where to store the value.
 * @param   cbValue             The size of the value to write.
 */
static VBOXSTRICTRC iomMMIODoComplicatedWrite(PVM pVM, PIOMMMIORANGE pRange, RTGCPHYS GCPhys, void const *pvValue, unsigned cbValue)
{
    AssertReturn(   (pRange->fFlags & IOMMMIO_FLAGS_WRITE_MODE) != IOMMMIO_FLAGS_WRITE_PASSTHRU
                 && (pRange->fFlags & IOMMMIO_FLAGS_WRITE_MODE) <= IOMMMIO_FLAGS_WRITE_DWORD_QWORD_READ_MISSING,
                 VERR_IOM_MMIO_IPE_1);
    AssertReturn(cbValue != 0 && cbValue <= 16, VERR_IOM_MMIO_IPE_2);
    RTGCPHYS const GCPhysStart  = GCPhys; NOREF(GCPhysStart);
    bool const     fReadMissing = (pRange->fFlags & IOMMMIO_FLAGS_WRITE_MODE) == IOMMMIO_FLAGS_WRITE_DWORD_READ_MISSING
                               || (pRange->fFlags & IOMMMIO_FLAGS_WRITE_MODE) == IOMMMIO_FLAGS_WRITE_DWORD_QWORD_READ_MISSING;

    /*
     * Do debug stop if requested.
     */
    int rc = VINF_SUCCESS; NOREF(pVM);
#ifdef VBOX_STRICT
    if (pRange->fFlags & IOMMMIO_FLAGS_DBGSTOP_ON_COMPLICATED_WRITE)
    {
# ifdef IN_RING3
        LogRel(("IOM: Complicated write %#x byte at %RGp to %s, initiating debugger intervention\n", cbValue, GCPhys,
                R3STRING(pRange->pszDesc)));
        rc = DBGFR3EventSrc(pVM, DBGFEVENT_DEV_STOP, RT_SRC_POS,
                            "Complicated write %#x byte at %RGp to %s\n", cbValue, GCPhys, R3STRING(pRange->pszDesc));
        if (rc == VERR_DBGF_NOT_ATTACHED)
            rc = VINF_SUCCESS;
# else
        return VINF_IOM_R3_MMIO_WRITE;
# endif
    }
#endif

    /*
     * Check if we should ignore the write.
     */
    if ((pRange->fFlags & IOMMMIO_FLAGS_WRITE_MODE) == IOMMMIO_FLAGS_WRITE_ONLY_DWORD)
    {
        Assert(cbValue != 4 || (GCPhys & 3));
        return VINF_SUCCESS;
    }
    if ((pRange->fFlags & IOMMMIO_FLAGS_WRITE_MODE) == IOMMMIO_FLAGS_WRITE_ONLY_DWORD_QWORD)
    {
        Assert((cbValue != 4 && cbValue != 8) || (GCPhys & (cbValue - 1)));
        return VINF_SUCCESS;
    }

    /*
     * Split and conquer.
     */
    for (;;)
    {
        unsigned const  offAccess  = GCPhys & 3;
        unsigned        cbThisPart = 4 - offAccess;
        if (cbThisPart > cbValue)
            cbThisPart = cbValue;

        /*
         * Get the missing bits (if any).
         */
        uint32_t u32MissingValue = 0;
        if (fReadMissing && cbThisPart != 4)
        {
            int rc2 = pRange->CTX_SUFF(pfnReadCallback)(pRange->CTX_SUFF(pDevIns), pRange->CTX_SUFF(pvUser),
                                                        GCPhys & ~(RTGCPHYS)3, &u32MissingValue, sizeof(u32MissingValue));
            switch (rc2)
            {
                case VINF_SUCCESS:
                    break;
                case VINF_IOM_MMIO_UNUSED_FF:
                    u32MissingValue = UINT32_C(0xffffffff);
                    break;
                case VINF_IOM_MMIO_UNUSED_00:
                    u32MissingValue = 0;
                    break;
                case VINF_IOM_R3_MMIO_READ:
                case VINF_IOM_R3_MMIO_READ_WRITE:
                case VINF_IOM_R3_MMIO_WRITE:
                    /** @todo What if we've split a transfer and already read
                     * something?  Since writes generally have sideeffects we
                     * could be kind of screwed here...
                     *
                     * Fix: Save the current state and resume it in ring-3. Requires EM to not go
                     *      to REM for MMIO accesses (like may currently do). */

                    LogFlow(("iomMMIODoComplicatedWrite: GCPhys=%RGp GCPhysStart=%RGp cbValue=%u rc=%Rrc [read]\n", GCPhys, GCPhysStart, cbValue, rc2));
                    return rc2;
                default:
                    if (RT_FAILURE(rc2))
                    {
                        Log(("iomMMIODoComplicatedWrite: GCPhys=%RGp GCPhysStart=%RGp cbValue=%u rc=%Rrc [read]\n", GCPhys, GCPhysStart, cbValue, rc2));
                        return rc2;
                    }
                    AssertMsgReturn(rc2 >= VINF_EM_FIRST && rc2 <= VINF_EM_LAST, ("%Rrc\n", rc2), VERR_IPE_UNEXPECTED_INFO_STATUS);
                    if (rc == VINF_SUCCESS || rc2 < rc)
                        rc = rc2;
                    break;
            }
        }

        /*
         * Merge missing and given bits.
         */
        uint32_t u32GivenMask;
        uint32_t u32GivenValue;
        switch (cbThisPart)
        {
            case 1:
                u32GivenValue = *(uint8_t  const *)pvValue;
                u32GivenMask  = UINT32_C(0x000000ff);
                break;
            case 2:
                u32GivenValue = *(uint16_t const *)pvValue;
                u32GivenMask  = UINT32_C(0x0000ffff);
                break;
            case 3:
                u32GivenValue = RT_MAKE_U32_FROM_U8(((uint8_t const *)pvValue)[0], ((uint8_t const *)pvValue)[1],
                                                    ((uint8_t const *)pvValue)[2], 0);
                u32GivenMask  = UINT32_C(0x00ffffff);
                break;
            case 4:
                u32GivenValue = *(uint32_t const *)pvValue;
                u32GivenMask  = UINT32_C(0xffffffff);
                break;
            default:
                AssertFailedReturn(VERR_IOM_MMIO_IPE_3);
        }
        if (offAccess)
        {
            u32GivenValue <<= offAccess * 8;
            u32GivenMask  <<= offAccess * 8;
        }

        uint32_t u32Value = (u32MissingValue & ~u32GivenMask)
                          | (u32GivenValue & u32GivenMask);

        /*
         * Do DWORD write to the device.
         */
        int rc2 = pRange->CTX_SUFF(pfnWriteCallback)(pRange->CTX_SUFF(pDevIns), pRange->CTX_SUFF(pvUser),
                                                     GCPhys & ~(RTGCPHYS)3, &u32Value, sizeof(u32Value));
        switch (rc2)
        {
            case VINF_SUCCESS:
                break;
            case VINF_IOM_R3_MMIO_READ:
            case VINF_IOM_R3_MMIO_READ_WRITE:
            case VINF_IOM_R3_MMIO_WRITE:
                /** @todo What if we've split a transfer and already read
                 * something?  Since reads can have sideeffects we could be
                 * kind of screwed here...
                 *
                 * Fix: Save the current state and resume it in ring-3. Requires EM to not go
                 *      to REM for MMIO accesses (like may currently do). */
                LogFlow(("iomMMIODoComplicatedWrite: GCPhys=%RGp GCPhysStart=%RGp cbValue=%u rc=%Rrc [write]\n", GCPhys, GCPhysStart, cbValue, rc2));
                return rc2;
            default:
                if (RT_FAILURE(rc2))
                {
                    Log(("iomMMIODoComplicatedWrite: GCPhys=%RGp GCPhysStart=%RGp cbValue=%u rc=%Rrc [write]\n", GCPhys, GCPhysStart, cbValue, rc2));
                    return rc2;
                }
                AssertMsgReturn(rc2 >= VINF_EM_FIRST && rc2 <= VINF_EM_LAST, ("%Rrc\n", rc2), VERR_IPE_UNEXPECTED_INFO_STATUS);
                if (rc == VINF_SUCCESS || rc2 < rc)
                    rc = rc2;
                break;
        }

        /*
         * Advance.
         */
        cbValue -= cbThisPart;
        if (!cbValue)
            break;
        GCPhys += cbThisPart;
        pvValue = (uint8_t const *)pvValue + cbThisPart;
    }

    return rc;
}




/**
 * Wrapper which does the write and updates range statistics when such are enabled.
 * @warning RT_SUCCESS(rc=VINF_IOM_R3_MMIO_WRITE) is TRUE!
 */
static VBOXSTRICTRC iomMMIODoWrite(PVM pVM, PVMCPU pVCpu, PIOMMMIORANGE pRange, RTGCPHYS GCPhysFault,
                                   const void *pvData, unsigned cb)
{
#ifdef VBOX_WITH_STATISTICS
    int rcSem = IOM_LOCK_SHARED(pVM);
    if (rcSem == VERR_SEM_BUSY)
        return VINF_IOM_R3_MMIO_WRITE;
    PIOMMMIOSTATS pStats = iomMmioGetStats(pVM, pVCpu, GCPhysFault, pRange);
    if (!pStats)
# ifdef IN_RING3
        return VERR_NO_MEMORY;
# else
        return VINF_IOM_R3_MMIO_WRITE;
# endif
    STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfWrite), a);
#endif

    VBOXSTRICTRC rcStrict;
    if (RT_LIKELY(pRange->CTX_SUFF(pfnWriteCallback)))
    {
        if (   (cb == 4 && !(GCPhysFault & 3))
            || (pRange->fFlags & IOMMMIO_FLAGS_WRITE_MODE) == IOMMMIO_FLAGS_WRITE_PASSTHRU
            || (cb == 8 && !(GCPhysFault & 7) && IOMMMIO_DOES_WRITE_MODE_ALLOW_QWORD(pRange->fFlags)) )
            rcStrict = pRange->CTX_SUFF(pfnWriteCallback)(pRange->CTX_SUFF(pDevIns), pRange->CTX_SUFF(pvUser),
                                                          GCPhysFault, (void *)pvData, cb); /** @todo fix const!! */
        else
            rcStrict = iomMMIODoComplicatedWrite(pVM, pRange, GCPhysFault, pvData, cb);
    }
    else
        rcStrict = VINF_SUCCESS;

    STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfWrite), a);
    STAM_COUNTER_INC(&pStats->Accesses);
    return rcStrict;
}


/**
 * Deals with complicated MMIO reads.
 *
 * Complicated means unaligned or non-dword/qword sized accesses depending on
 * the MMIO region's access mode flags.
 *
 * @returns Strict VBox status code. Any EM scheduling status code,
 *          VINF_IOM_R3_MMIO_READ, VINF_IOM_R3_MMIO_READ_WRITE or
 *          VINF_IOM_R3_MMIO_WRITE may be returned.
 *
 * @param   pVM                 Pointer to the VM.
 * @param   pRange              The range to read from.
 * @param   GCPhys              The physical address to start reading.
 * @param   pvValue             Where to store the value.
 * @param   cbValue             The size of the value to read.
 */
static VBOXSTRICTRC iomMMIODoComplicatedRead(PVM pVM, PIOMMMIORANGE pRange, RTGCPHYS GCPhys, void *pvValue, unsigned cbValue)
{
    AssertReturn(   (pRange->fFlags & IOMMMIO_FLAGS_READ_MODE) == IOMMMIO_FLAGS_READ_DWORD
                 || (pRange->fFlags & IOMMMIO_FLAGS_READ_MODE) == IOMMMIO_FLAGS_READ_DWORD_QWORD,
                 VERR_IOM_MMIO_IPE_1);
    AssertReturn(cbValue != 0 && cbValue <= 16, VERR_IOM_MMIO_IPE_2);
    RTGCPHYS const GCPhysStart = GCPhys; NOREF(GCPhysStart);

    /*
     * Do debug stop if requested.
     */
    int rc = VINF_SUCCESS; NOREF(pVM);
#ifdef VBOX_STRICT
    if (pRange->fFlags & IOMMMIO_FLAGS_DBGSTOP_ON_COMPLICATED_READ)
    {
# ifdef IN_RING3
        rc = DBGFR3EventSrc(pVM, DBGFEVENT_DEV_STOP, RT_SRC_POS,
                            "Complicated read %#x byte at %RGp to %s\n", cbValue, GCPhys, R3STRING(pRange->pszDesc));
        if (rc == VERR_DBGF_NOT_ATTACHED)
            rc = VINF_SUCCESS;
# else
        return VINF_IOM_R3_MMIO_READ;
# endif
    }
#endif

    /*
     * Split and conquer.
     */
    for (;;)
    {
        /*
         * Do DWORD read from the device.
         */
        uint32_t u32Value;
        int rc2 = pRange->CTX_SUFF(pfnReadCallback)(pRange->CTX_SUFF(pDevIns), pRange->CTX_SUFF(pvUser),
                                                    GCPhys & ~(RTGCPHYS)3, &u32Value, sizeof(u32Value));
        switch (rc2)
        {
            case VINF_SUCCESS:
                break;
            case VINF_IOM_MMIO_UNUSED_FF:
                u32Value = UINT32_C(0xffffffff);
                break;
            case VINF_IOM_MMIO_UNUSED_00:
                u32Value = 0;
                break;
            case VINF_IOM_R3_MMIO_READ:
            case VINF_IOM_R3_MMIO_READ_WRITE:
            case VINF_IOM_R3_MMIO_WRITE:
                /** @todo What if we've split a transfer and already read
                 * something?  Since reads can have sideeffects we could be
                 * kind of screwed here... */
                LogFlow(("iomMMIODoComplicatedRead: GCPhys=%RGp GCPhysStart=%RGp cbValue=%u rc=%Rrc\n", GCPhys, GCPhysStart, cbValue, rc2));
                return rc2;
            default:
                if (RT_FAILURE(rc2))
                {
                    Log(("iomMMIODoComplicatedRead: GCPhys=%RGp GCPhysStart=%RGp cbValue=%u rc=%Rrc\n", GCPhys, GCPhysStart, cbValue, rc2));
                    return rc2;
                }
                AssertMsgReturn(rc2 >= VINF_EM_FIRST && rc2 <= VINF_EM_LAST, ("%Rrc\n", rc2), VERR_IPE_UNEXPECTED_INFO_STATUS);
                if (rc == VINF_SUCCESS || rc2 < rc)
                    rc = rc2;
                break;
        }
        u32Value >>= (GCPhys & 3) * 8;

        /*
         * Write what we've read.
         */
        unsigned cbThisPart = 4 - (GCPhys & 3);
        if (cbThisPart > cbValue)
            cbThisPart = cbValue;

        switch (cbThisPart)
        {
            case 1:
                *(uint8_t *)pvValue = (uint8_t)u32Value;
                break;
            case 2:
                *(uint16_t *)pvValue = (uint16_t)u32Value;
                break;
            case 3:
                ((uint8_t *)pvValue)[0] = RT_BYTE1(u32Value);
                ((uint8_t *)pvValue)[1] = RT_BYTE2(u32Value);
                ((uint8_t *)pvValue)[2] = RT_BYTE3(u32Value);
                break;
            case 4:
                *(uint32_t *)pvValue = u32Value;
                break;
        }

        /*
         * Advance.
         */
        cbValue -= cbThisPart;
        if (!cbValue)
            break;
        GCPhys += cbThisPart;
        pvValue = (uint8_t *)pvValue + cbThisPart;
    }

    return rc;
}


/**
 * Implements VINF_IOM_MMIO_UNUSED_FF.
 *
 * @returns VINF_SUCCESS.
 * @param   pvValue             Where to store the zeros.
 * @param   cbValue             How many bytes to read.
 */
static int iomMMIODoReadFFs(void *pvValue, size_t cbValue)
{
    switch (cbValue)
    {
        case 1: *(uint8_t  *)pvValue = UINT8_C(0xff); break;
        case 2: *(uint16_t *)pvValue = UINT16_C(0xffff); break;
        case 4: *(uint32_t *)pvValue = UINT32_C(0xffffffff); break;
        case 8: *(uint64_t *)pvValue = UINT64_C(0xffffffffffffffff); break;
        default:
        {
            uint8_t *pb = (uint8_t *)pvValue;
            while (cbValue--)
                *pb++ = UINT8_C(0xff);
            break;
        }
    }
    return VINF_SUCCESS;
}


/**
 * Implements VINF_IOM_MMIO_UNUSED_00.
 *
 * @returns VINF_SUCCESS.
 * @param   pvValue             Where to store the zeros.
 * @param   cbValue             How many bytes to read.
 */
static int iomMMIODoRead00s(void *pvValue, size_t cbValue)
{
    switch (cbValue)
    {
        case 1: *(uint8_t  *)pvValue = UINT8_C(0x00); break;
        case 2: *(uint16_t *)pvValue = UINT16_C(0x0000); break;
        case 4: *(uint32_t *)pvValue = UINT32_C(0x00000000); break;
        case 8: *(uint64_t *)pvValue = UINT64_C(0x0000000000000000); break;
        default:
        {
            uint8_t *pb = (uint8_t *)pvValue;
            while (cbValue--)
                *pb++ = UINT8_C(0x00);
            break;
        }
    }
    return VINF_SUCCESS;
}


/**
 * Wrapper which does the read and updates range statistics when such are enabled.
 */
DECLINLINE(VBOXSTRICTRC) iomMMIODoRead(PVM pVM, PVMCPU pVCpu, PIOMMMIORANGE pRange, RTGCPHYS GCPhys,
                                       void *pvValue, unsigned cbValue)
{
#ifdef VBOX_WITH_STATISTICS
    int rcSem = IOM_LOCK_SHARED(pVM);
    if (rcSem == VERR_SEM_BUSY)
        return VINF_IOM_R3_MMIO_READ;
    PIOMMMIOSTATS pStats = iomMmioGetStats(pVM, pVCpu, GCPhys, pRange);
    if (!pStats)
# ifdef IN_RING3
        return VERR_NO_MEMORY;
# else
        return VINF_IOM_R3_MMIO_READ;
# endif
    STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfRead), a);
#endif

    VBOXSTRICTRC rcStrict;
    if (RT_LIKELY(pRange->CTX_SUFF(pfnReadCallback)))
    {
        if (   (   cbValue == 4
                && !(GCPhys & 3))
            || (pRange->fFlags & IOMMMIO_FLAGS_READ_MODE) == IOMMMIO_FLAGS_READ_PASSTHRU
            || (    cbValue == 8
                && !(GCPhys & 7)
                && (pRange->fFlags & IOMMMIO_FLAGS_READ_MODE) == IOMMMIO_FLAGS_READ_DWORD_QWORD ) )
            rcStrict = pRange->CTX_SUFF(pfnReadCallback)(pRange->CTX_SUFF(pDevIns), pRange->CTX_SUFF(pvUser), GCPhys,
                                                         pvValue, cbValue);
        else
            rcStrict = iomMMIODoComplicatedRead(pVM, pRange, GCPhys, pvValue, cbValue);
    }
    else
        rcStrict = VINF_IOM_MMIO_UNUSED_FF;
    if (rcStrict != VINF_SUCCESS)
    {
        switch (VBOXSTRICTRC_VAL(rcStrict))
        {
            case VINF_IOM_MMIO_UNUSED_FF: rcStrict = iomMMIODoReadFFs(pvValue, cbValue); break;
            case VINF_IOM_MMIO_UNUSED_00: rcStrict = iomMMIODoRead00s(pvValue, cbValue); break;
        }
    }

    STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfRead), a);
    STAM_COUNTER_INC(&pStats->Accesses);
    return rcStrict;
}


/**
 * Internal - statistics only.
 */
DECLINLINE(void) iomMMIOStatLength(PVM pVM, unsigned cb)
{
#ifdef VBOX_WITH_STATISTICS
    switch (cb)
    {
        case 1:
            STAM_COUNTER_INC(&pVM->iom.s.StatRZMMIO1Byte);
            break;
        case 2:
            STAM_COUNTER_INC(&pVM->iom.s.StatRZMMIO2Bytes);
            break;
        case 4:
            STAM_COUNTER_INC(&pVM->iom.s.StatRZMMIO4Bytes);
            break;
        case 8:
            STAM_COUNTER_INC(&pVM->iom.s.StatRZMMIO8Bytes);
            break;
        default:
            /* No way. */
            AssertMsgFailed(("Invalid data length %d\n", cb));
            break;
    }
#else
    NOREF(pVM); NOREF(cb);
#endif
}


#ifndef IEM_USE_IEM_INSTEAD

/**
 * MOV      reg, mem         (read)
 * MOVZX    reg, mem         (read)
 * MOVSX    reg, mem         (read)
 *
 * @returns VBox status code.
 *
 * @param   pVM         The virtual machine.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   pRegFrame   Pointer to CPUMCTXCORE guest registers structure.
 * @param   pCpu        Disassembler CPU state.
 * @param   pRange      Pointer MMIO range.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 */
static int iomInterpretMOVxXRead(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, PDISCPUSTATE pCpu,
                                 PIOMMMIORANGE pRange, RTGCPHYS GCPhysFault)
{
    Assert(pRange->CTX_SUFF(pfnReadCallback) || !pRange->pfnReadCallbackR3);

    /*
     * Get the data size from parameter 2,
     * and call the handler function to get the data.
     */
    unsigned cb = DISGetParamSize(pCpu, &pCpu->Param2);
    AssertMsg(cb > 0 && cb <= sizeof(uint64_t), ("cb=%d\n", cb));

    uint64_t u64Data = 0;
    int rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &u64Data, cb));
    if (rc == VINF_SUCCESS)
    {
        /*
         * Do sign extension for MOVSX.
         */
        /** @todo checkup MOVSX implementation! */
        if (pCpu->pCurInstr->uOpcode == OP_MOVSX)
        {
            if (cb == 1)
            {
                /* DWORD <- BYTE */
                int64_t iData = (int8_t)u64Data;
                u64Data = (uint64_t)iData;
            }
            else
            {
                /* DWORD <- WORD */
                int64_t iData = (int16_t)u64Data;
                u64Data = (uint64_t)iData;
            }
        }

        /*
         * Store the result to register (parameter 1).
         */
        bool fRc = iomSaveDataToReg(pCpu, &pCpu->Param1, pRegFrame, u64Data);
        AssertMsg(fRc, ("Failed to store register value!\n")); NOREF(fRc);
    }

    if (rc == VINF_SUCCESS)
        iomMMIOStatLength(pVM, cb);
    return rc;
}


/**
 * MOV      mem, reg|imm     (write)
 *
 * @returns VBox status code.
 *
 * @param   pVM         The virtual machine.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   pRegFrame   Pointer to CPUMCTXCORE guest registers structure.
 * @param   pCpu        Disassembler CPU state.
 * @param   pRange      Pointer MMIO range.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 */
static int iomInterpretMOVxXWrite(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, PDISCPUSTATE pCpu,
                                  PIOMMMIORANGE pRange, RTGCPHYS GCPhysFault)
{
    Assert(pRange->CTX_SUFF(pfnWriteCallback) || !pRange->pfnWriteCallbackR3);

    /*
     * Get data to write from second parameter,
     * and call the callback to write it.
     */
    unsigned cb = 0;
    uint64_t u64Data  = 0;
    bool fRc = iomGetRegImmData(pCpu, &pCpu->Param2, pRegFrame, &u64Data, &cb);
    AssertMsg(fRc, ("Failed to get reg/imm port number!\n")); NOREF(fRc);

    int rc = VBOXSTRICTRC_TODO(iomMMIODoWrite(pVM, pVCpu, pRange, GCPhysFault, &u64Data, cb));
    if (rc == VINF_SUCCESS)
        iomMMIOStatLength(pVM, cb);
    return rc;
}


/** Wrapper for reading virtual memory. */
DECLINLINE(int) iomRamRead(PVMCPU pVCpu, void *pDest, RTGCPTR GCSrc, uint32_t cb)
{
    /* Note: This will fail in R0 or RC if it hits an access handler. That
             isn't a problem though since the operation can be restarted in REM. */
#ifdef IN_RC
    NOREF(pVCpu);
    int rc = MMGCRamReadNoTrapHandler(pDest, (void *)(uintptr_t)GCSrc, cb);
    /* Page may be protected and not directly accessible. */
    if (rc == VERR_ACCESS_DENIED)
        rc = VINF_IOM_R3_IOPORT_WRITE;
    return rc;
#else
    return VBOXSTRICTRC_VAL(PGMPhysReadGCPtr(pVCpu, pDest, GCSrc, cb, PGMACCESSORIGIN_IOM));
#endif
}


/** Wrapper for writing virtual memory. */
DECLINLINE(int) iomRamWrite(PVMCPU pVCpu, PCPUMCTXCORE pCtxCore, RTGCPTR GCPtrDst, void *pvSrc, uint32_t cb)
{
    /** @todo Need to update PGMVerifyAccess to take access handlers into account for Ring-0 and
     *        raw mode code. Some thought needs to be spent on theoretical concurrency issues as
     *        as well since we're not behind the pgm lock and handler may change between calls.
     *
     *        PGMPhysInterpretedWriteNoHandlers/PGMPhysWriteGCPtr may mess up
     *        the state of some shadowed structures. */
#if defined(IN_RING0) || defined(IN_RC)
    return PGMPhysInterpretedWriteNoHandlers(pVCpu, pCtxCore, GCPtrDst, pvSrc, cb, false /*fRaiseTrap*/);
#else
    NOREF(pCtxCore);
    return VBOXSTRICTRC_VAL(PGMPhysWriteGCPtr(pVCpu, GCPtrDst, pvSrc, cb, PGMACCESSORIGIN_IOM));
#endif
}


#if defined(IOM_WITH_MOVS_SUPPORT) && 0 /* locking prevents this from working. has buggy ecx handling. */
/**
 * [REP] MOVSB
 * [REP] MOVSW
 * [REP] MOVSD
 *
 * Restricted implementation.
 *
 *
 * @returns VBox status code.
 *
 * @param   pVM         The virtual machine.
 * @param   uErrorCode  CPU Error code.
 * @param   pRegFrame   Trap register frame.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 * @param   pCpu        Disassembler CPU state.
 * @param   pRange      Pointer MMIO range.
 * @param   ppStat      Which sub-sample to attribute this call to.
 */
static int iomInterpretMOVS(PVM pVM, bool fWriteAccess, PCPUMCTXCORE pRegFrame, RTGCPHYS GCPhysFault, PDISCPUSTATE pCpu, PIOMMMIORANGE pRange,
                            PSTAMPROFILE *ppStat)
{
    /*
     * We do not support segment prefixes or REPNE.
     */
    if (pCpu->fPrefix & (DISPREFIX_SEG | DISPREFIX_REPNE))
        return VINF_IOM_R3_MMIO_READ_WRITE; /** @todo -> interpret whatever. */

    PVMCPU pVCpu = VMMGetCpu(pVM);

    /*
     * Get bytes/words/dwords/qword count to copy.
     */
    uint32_t cTransfers = 1;
    if (pCpu->fPrefix & DISPREFIX_REP)
    {
#ifndef IN_RC
        if (    CPUMIsGuestIn64BitCode(pVCpu, pRegFrame)
            &&  pRegFrame->rcx >= _4G)
            return VINF_EM_RAW_EMULATE_INSTR;
#endif

        cTransfers = pRegFrame->ecx;
        if (SELMGetCpuModeFromSelector(pVM, pRegFrame->eflags, pRegFrame->cs, &pRegFrame->csHid) == DISCPUMODE_16BIT)
            cTransfers &= 0xffff;

        if (!cTransfers)
            return VINF_SUCCESS;
    }

    /* Get the current privilege level. */
    uint32_t cpl = CPUMGetGuestCPL(pVCpu, pRegFrame);

    /*
     * Get data size.
     */
    unsigned cb = DISGetParamSize(pCpu, &pCpu->Param1);
    AssertMsg(cb > 0 && cb <= sizeof(uint64_t), ("cb=%d\n", cb));
    int      offIncrement = pRegFrame->eflags.Bits.u1DF ? -(signed)cb : (signed)cb;

#ifdef VBOX_WITH_STATISTICS
    if (pVM->iom.s.cMovsMaxBytes < (cTransfers << SIZE_2_SHIFT(cb)))
        pVM->iom.s.cMovsMaxBytes = cTransfers << SIZE_2_SHIFT(cb);
#endif

/** @todo re-evaluate on page boundaries. */

    RTGCPHYS Phys = GCPhysFault;
    int rc;
    if (fWriteAccess)
    {
        /*
         * Write operation: [Mem] -> [MMIO]
         * ds:esi (Virt Src) -> es:edi (Phys Dst)
         */
        STAM_STATS({ *ppStat = &pVM->iom.s.StatRZInstMovsToMMIO; });

        /* Check callback. */
        if (!pRange->CTX_SUFF(pfnWriteCallback))
            return VINF_IOM_R3_MMIO_WRITE;

        /* Convert source address ds:esi. */
        RTGCUINTPTR pu8Virt;
        rc = SELMToFlatEx(pVM, DISSELREG_DS, pRegFrame, (RTGCPTR)pRegFrame->rsi,
                          SELMTOFLAT_FLAGS_HYPER | SELMTOFLAT_FLAGS_NO_PL,
                          (PRTGCPTR)&pu8Virt);
        if (RT_SUCCESS(rc))
        {

            /* Access verification first; we currently can't recover properly from traps inside this instruction */
            rc = PGMVerifyAccess(pVCpu, pu8Virt, cTransfers * cb, (cpl == 3) ? X86_PTE_US : 0);
            if (rc != VINF_SUCCESS)
            {
                Log(("MOVS will generate a trap -> recompiler, rc=%d\n", rc));
                return VINF_EM_RAW_EMULATE_INSTR;
            }

#ifdef IN_RC
            MMGCRamRegisterTrapHandler(pVM);
#endif

            /* copy loop. */
            while (cTransfers)
            {
                uint32_t u32Data = 0;
                rc = iomRamRead(pVCpu, &u32Data, (RTGCPTR)pu8Virt, cb);
                if (rc != VINF_SUCCESS)
                    break;
                rc = VBOXSTRICTRC_TODO(iomMMIODoWrite(pVM, pRange, Phys, &u32Data, cb));
                if (rc != VINF_SUCCESS)
                    break;

                pu8Virt        += offIncrement;
                Phys           += offIncrement;
                pRegFrame->rsi += offIncrement;
                pRegFrame->rdi += offIncrement;
                cTransfers--;
            }
#ifdef IN_RC
            MMGCRamDeregisterTrapHandler(pVM);
#endif
            /* Update ecx. */
            if (pCpu->fPrefix & DISPREFIX_REP)
                pRegFrame->ecx = cTransfers;
        }
        else
            rc = VINF_IOM_R3_MMIO_READ_WRITE;
    }
    else
    {
        /*
         * Read operation: [MMIO] -> [mem] or [MMIO] -> [MMIO]
         * ds:[eSI] (Phys Src) -> es:[eDI] (Virt Dst)
         */
        STAM_STATS({ *ppStat = &pVM->iom.s.StatRZInstMovsFromMMIO; });

        /* Check callback. */
        if (!pRange->CTX_SUFF(pfnReadCallback))
            return VINF_IOM_R3_MMIO_READ;

        /* Convert destination address. */
        RTGCUINTPTR pu8Virt;
        rc = SELMToFlatEx(pVM, DISSELREG_ES, pRegFrame, (RTGCPTR)pRegFrame->rdi,
                          SELMTOFLAT_FLAGS_HYPER | SELMTOFLAT_FLAGS_NO_PL,
                          (RTGCPTR *)&pu8Virt);
        if (RT_FAILURE(rc))
            return VINF_IOM_R3_MMIO_READ;

        /* Check if destination address is MMIO. */
        PIOMMMIORANGE pMMIODst;
        RTGCPHYS PhysDst;
        rc = PGMGstGetPage(pVCpu, (RTGCPTR)pu8Virt, NULL, &PhysDst);
        PhysDst |= (RTGCUINTPTR)pu8Virt & PAGE_OFFSET_MASK;
        if (    RT_SUCCESS(rc)
            &&  (pMMIODst = iomMmioGetRangeWithRef(pVM, PhysDst)))
        {
            /** @todo implement per-device locks for MMIO access. */
            Assert(!pMMIODst->CTX_SUFF(pDevIns)->CTX_SUFF(pCritSect));

            /*
             * Extra: [MMIO] -> [MMIO]
             */
            STAM_STATS({ *ppStat = &pVM->iom.s.StatRZInstMovsMMIO; });
            if (!pMMIODst->CTX_SUFF(pfnWriteCallback) && pMMIODst->pfnWriteCallbackR3)
            {
                iomMmioReleaseRange(pVM, pRange);
                return VINF_IOM_R3_MMIO_READ_WRITE;
            }

            /* copy loop. */
            while (cTransfers)
            {
                uint32_t u32Data;
                rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pRange, Phys, &u32Data, cb));
                if (rc != VINF_SUCCESS)
                    break;
                rc = VBOXSTRICTRC_TODO(iomMMIODoWrite(pVM, pMMIODst, PhysDst, &u32Data, cb));
                if (rc != VINF_SUCCESS)
                    break;

                Phys           += offIncrement;
                PhysDst        += offIncrement;
                pRegFrame->rsi += offIncrement;
                pRegFrame->rdi += offIncrement;
                cTransfers--;
            }
            iomMmioReleaseRange(pVM, pRange);
        }
        else
        {
            /*
             * Normal: [MMIO] -> [Mem]
             */
            /* Access verification first; we currently can't recover properly from traps inside this instruction */
            rc = PGMVerifyAccess(pVCpu, pu8Virt, cTransfers * cb, X86_PTE_RW | ((cpl == 3) ? X86_PTE_US : 0));
            if (rc != VINF_SUCCESS)
            {
                Log(("MOVS will generate a trap -> recompiler, rc=%d\n", rc));
                return VINF_EM_RAW_EMULATE_INSTR;
            }

            /* copy loop. */
#ifdef IN_RC
            MMGCRamRegisterTrapHandler(pVM);
#endif
            while (cTransfers)
            {
                uint32_t u32Data;
                rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pRange, Phys, &u32Data, cb));
                if (rc != VINF_SUCCESS)
                    break;
                rc = iomRamWrite(pVCpu, pRegFrame, (RTGCPTR)pu8Virt, &u32Data, cb);
                if (rc != VINF_SUCCESS)
                {
                    Log(("iomRamWrite %08X size=%d failed with %d\n", pu8Virt, cb, rc));
                    break;
                }

                pu8Virt        += offIncrement;
                Phys           += offIncrement;
                pRegFrame->rsi += offIncrement;
                pRegFrame->rdi += offIncrement;
                cTransfers--;
            }
#ifdef IN_RC
            MMGCRamDeregisterTrapHandler(pVM);
#endif
        }

        /* Update ecx on exit. */
        if (pCpu->fPrefix & DISPREFIX_REP)
            pRegFrame->ecx = cTransfers;
    }

    /* work statistics. */
    if (rc == VINF_SUCCESS)
        iomMMIOStatLength(pVM, cb);
    NOREF(ppStat);
    return rc;
}
#endif /* IOM_WITH_MOVS_SUPPORT */


/**
 * Gets the address / opcode mask corresponding to the given CPU mode.
 *
 * @returns Mask.
 * @param   enmCpuMode          CPU mode.
 */
static uint64_t iomDisModeToMask(DISCPUMODE enmCpuMode)
{
    switch (enmCpuMode)
    {
        case DISCPUMODE_16BIT: return UINT16_MAX;
        case DISCPUMODE_32BIT: return UINT32_MAX;
        case DISCPUMODE_64BIT: return UINT64_MAX;
        default:
            AssertFailedReturn(UINT32_MAX);
    }
}


/**
 * [REP] STOSB
 * [REP] STOSW
 * [REP] STOSD
 *
 * Restricted implementation.
 *
 *
 * @returns VBox status code.
 *
 * @param   pVM         The virtual machine.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   pRegFrame   Trap register frame.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 * @param   pCpu        Disassembler CPU state.
 * @param   pRange      Pointer MMIO range.
 */
static int iomInterpretSTOS(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, RTGCPHYS GCPhysFault,
                            PDISCPUSTATE pCpu, PIOMMMIORANGE pRange)
{
    /*
     * We do not support segment prefixes or REPNE..
     */
    if (pCpu->fPrefix & (DISPREFIX_SEG | DISPREFIX_REPNE))
        return VINF_IOM_R3_MMIO_READ_WRITE; /** @todo -> REM instead of HC */

    /*
     * Get bytes/words/dwords/qwords count to copy.
     */
    uint64_t const fAddrMask = iomDisModeToMask((DISCPUMODE)pCpu->uAddrMode);
    RTGCUINTREG cTransfers = 1;
    if (pCpu->fPrefix & DISPREFIX_REP)
    {
#ifndef IN_RC
        if (    CPUMIsGuestIn64BitCode(pVCpu)
            &&  pRegFrame->rcx >= _4G)
            return VINF_EM_RAW_EMULATE_INSTR;
#endif

        cTransfers = pRegFrame->rcx & fAddrMask;
        if (!cTransfers)
            return VINF_SUCCESS;
    }

/** @todo r=bird: bounds checks! */

    /*
     * Get data size.
     */
    unsigned cb = DISGetParamSize(pCpu, &pCpu->Param1);
    AssertMsg(cb > 0 && cb <= sizeof(uint64_t), ("cb=%d\n", cb));
    int      offIncrement = pRegFrame->eflags.Bits.u1DF ? -(signed)cb : (signed)cb;

#ifdef VBOX_WITH_STATISTICS
    if (pVM->iom.s.cStosMaxBytes < (cTransfers << SIZE_2_SHIFT(cb)))
        pVM->iom.s.cStosMaxBytes = cTransfers << SIZE_2_SHIFT(cb);
#endif


    RTGCPHYS    Phys    = GCPhysFault;
    int rc;
    if (   pRange->CTX_SUFF(pfnFillCallback)
        && cb <= 4 /* can only fill 32-bit values */)
    {
        /*
         * Use the fill callback.
         */
        /** @todo pfnFillCallback must return number of bytes successfully written!!! */
        if (offIncrement > 0)
        {
            /* addr++ variant. */
            rc = pRange->CTX_SUFF(pfnFillCallback)(pRange->CTX_SUFF(pDevIns), pRange->CTX_SUFF(pvUser), Phys,
                                                   pRegFrame->eax, cb, cTransfers);
            if (rc == VINF_SUCCESS)
            {
                /* Update registers. */
                pRegFrame->rdi = ((pRegFrame->rdi + (cTransfers << SIZE_2_SHIFT(cb))) & fAddrMask)
                               | (pRegFrame->rdi & ~fAddrMask);
                if (pCpu->fPrefix & DISPREFIX_REP)
                    pRegFrame->rcx &= ~fAddrMask;
            }
        }
        else
        {
            /* addr-- variant. */
            rc = pRange->CTX_SUFF(pfnFillCallback)(pRange->CTX_SUFF(pDevIns),  pRange->CTX_SUFF(pvUser),
                                                   Phys - ((cTransfers - 1) << SIZE_2_SHIFT(cb)),
                                                   pRegFrame->eax, cb, cTransfers);
            if (rc == VINF_SUCCESS)
            {
                /* Update registers. */
                pRegFrame->rdi = ((pRegFrame->rdi - (cTransfers << SIZE_2_SHIFT(cb))) & fAddrMask)
                               | (pRegFrame->rdi & ~fAddrMask);
                if (pCpu->fPrefix & DISPREFIX_REP)
                    pRegFrame->rcx &= ~fAddrMask;
            }
        }
    }
    else
    {
        /*
         * Use the write callback.
         */
        Assert(pRange->CTX_SUFF(pfnWriteCallback) || !pRange->pfnWriteCallbackR3);
        uint64_t u64Data = pRegFrame->rax;

        /* fill loop. */
        do
        {
            rc = VBOXSTRICTRC_TODO(iomMMIODoWrite(pVM, pVCpu, pRange, Phys, &u64Data, cb));
            if (rc != VINF_SUCCESS)
                break;

            Phys           += offIncrement;
            pRegFrame->rdi  = ((pRegFrame->rdi + offIncrement) & fAddrMask)
                            | (pRegFrame->rdi & ~fAddrMask);
            cTransfers--;
        } while (cTransfers);

        /* Update rcx on exit. */
        if (pCpu->fPrefix & DISPREFIX_REP)
            pRegFrame->rcx = (cTransfers & fAddrMask)
                           | (pRegFrame->rcx & ~fAddrMask);
    }

    /*
     * Work statistics and return.
     */
    if (rc == VINF_SUCCESS)
        iomMMIOStatLength(pVM, cb);
    return rc;
}


/**
 * [REP] LODSB
 * [REP] LODSW
 * [REP] LODSD
 *
 * Restricted implementation.
 *
 *
 * @returns VBox status code.
 *
 * @param   pVM         The virtual machine.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   pRegFrame   Trap register frame.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 * @param   pCpu        Disassembler CPU state.
 * @param   pRange      Pointer MMIO range.
 */
static int iomInterpretLODS(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, RTGCPHYS GCPhysFault, PDISCPUSTATE pCpu,
                            PIOMMMIORANGE pRange)
{
    Assert(pRange->CTX_SUFF(pfnReadCallback) || !pRange->pfnReadCallbackR3);

    /*
     * We do not support segment prefixes or REP*.
     */
    if (pCpu->fPrefix & (DISPREFIX_SEG | DISPREFIX_REP | DISPREFIX_REPNE))
        return VINF_IOM_R3_MMIO_READ_WRITE; /** @todo -> REM instead of HC */

    /*
     * Get data size.
     */
    unsigned cb = DISGetParamSize(pCpu, &pCpu->Param2);
    AssertMsg(cb > 0 && cb <= sizeof(uint64_t), ("cb=%d\n", cb));
    int     offIncrement = pRegFrame->eflags.Bits.u1DF ? -(signed)cb : (signed)cb;

    /*
     * Perform read.
     */
    int rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &pRegFrame->rax, cb));
    if (rc == VINF_SUCCESS)
    {
        uint64_t const fAddrMask = iomDisModeToMask((DISCPUMODE)pCpu->uAddrMode);
        pRegFrame->rsi = ((pRegFrame->rsi + offIncrement) & fAddrMask)
                       | (pRegFrame->rsi & ~fAddrMask);
    }

    /*
     * Work statistics and return.
     */
    if (rc == VINF_SUCCESS)
        iomMMIOStatLength(pVM, cb);
    return rc;
}


/**
 * CMP [MMIO], reg|imm
 * CMP reg|imm, [MMIO]
 *
 * Restricted implementation.
 *
 *
 * @returns VBox status code.
 *
 * @param   pVM         The virtual machine.
 * @param   pRegFrame   Trap register frame.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 * @param   pCpu        Disassembler CPU state.
 * @param   pRange      Pointer MMIO range.
 */
static int iomInterpretCMP(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, RTGCPHYS GCPhysFault, PDISCPUSTATE pCpu,
                           PIOMMMIORANGE pRange)
{
    Assert(pRange->CTX_SUFF(pfnReadCallback) || !pRange->pfnReadCallbackR3);

    /*
     * Get the operands.
     */
    unsigned cb = 0;
    uint64_t uData1 = 0;
    uint64_t uData2 = 0;
    int rc;
    if (iomGetRegImmData(pCpu, &pCpu->Param1, pRegFrame, &uData1, &cb))
        /* cmp reg, [MMIO]. */
        rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &uData2, cb));
    else if (iomGetRegImmData(pCpu, &pCpu->Param2, pRegFrame, &uData2, &cb))
        /* cmp [MMIO], reg|imm. */
        rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &uData1, cb));
    else
    {
        AssertMsgFailed(("Disassember CMP problem..\n"));
        rc = VERR_IOM_MMIO_HANDLER_DISASM_ERROR;
    }

    if (rc == VINF_SUCCESS)
    {
#if HC_ARCH_BITS == 32
        /* Can't deal with 8 byte operands in our 32-bit emulation code. */
        if (cb > 4)
            return VINF_IOM_R3_MMIO_READ_WRITE;
#endif
        /* Emulate CMP and update guest flags. */
        uint32_t eflags = EMEmulateCmp(uData1, uData2, cb);
        pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                              | (eflags                &  (X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));
        iomMMIOStatLength(pVM, cb);
    }

    return rc;
}


/**
 * AND [MMIO], reg|imm
 * AND reg, [MMIO]
 * OR [MMIO], reg|imm
 * OR reg, [MMIO]
 *
 * Restricted implementation.
 *
 *
 * @returns VBox status code.
 *
 * @param   pVM         The virtual machine.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   pRegFrame   Trap register frame.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 * @param   pCpu        Disassembler CPU state.
 * @param   pRange      Pointer MMIO range.
 * @param   pfnEmulate  Instruction emulation function.
 */
static int iomInterpretOrXorAnd(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, RTGCPHYS GCPhysFault, PDISCPUSTATE pCpu,
                                PIOMMMIORANGE pRange, PFNEMULATEPARAM3 pfnEmulate)
{
    unsigned    cb     = 0;
    uint64_t    uData1 = 0;
    uint64_t    uData2 = 0;
    bool        fAndWrite;
    int         rc;

#ifdef LOG_ENABLED
    const char *pszInstr;

    if (pCpu->pCurInstr->uOpcode == OP_XOR)
        pszInstr = "Xor";
    else if (pCpu->pCurInstr->uOpcode == OP_OR)
        pszInstr = "Or";
    else if (pCpu->pCurInstr->uOpcode == OP_AND)
        pszInstr = "And";
    else
        pszInstr = "OrXorAnd??";
#endif

    if (iomGetRegImmData(pCpu, &pCpu->Param1, pRegFrame, &uData1, &cb))
    {
#if HC_ARCH_BITS == 32
        /* Can't deal with 8 byte operands in our 32-bit emulation code. */
        if (cb > 4)
            return VINF_IOM_R3_MMIO_READ_WRITE;
#endif
        /* and reg, [MMIO]. */
        Assert(pRange->CTX_SUFF(pfnReadCallback) || !pRange->pfnReadCallbackR3);
        fAndWrite = false;
        rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &uData2, cb));
    }
    else if (iomGetRegImmData(pCpu, &pCpu->Param2, pRegFrame, &uData2, &cb))
    {
#if HC_ARCH_BITS == 32
        /* Can't deal with 8 byte operands in our 32-bit emulation code. */
        if (cb > 4)
            return VINF_IOM_R3_MMIO_READ_WRITE;
#endif
        /* and [MMIO], reg|imm. */
        fAndWrite = true;
        if (    (pRange->CTX_SUFF(pfnReadCallback) || !pRange->pfnReadCallbackR3)
            &&  (pRange->CTX_SUFF(pfnWriteCallback) || !pRange->pfnWriteCallbackR3))
            rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &uData1, cb));
        else
            rc = VINF_IOM_R3_MMIO_READ_WRITE;
    }
    else
    {
        AssertMsgFailed(("Disassember AND problem..\n"));
        return VERR_IOM_MMIO_HANDLER_DISASM_ERROR;
    }

    if (rc == VINF_SUCCESS)
    {
        /* Emulate AND and update guest flags. */
        uint32_t eflags = pfnEmulate((uint32_t *)&uData1, uData2, cb);

        LogFlow(("iomInterpretOrXorAnd %s result %RX64\n", pszInstr, uData1));

        if (fAndWrite)
            /* Store result to MMIO. */
            rc = VBOXSTRICTRC_TODO(iomMMIODoWrite(pVM, pVCpu, pRange, GCPhysFault, &uData1, cb));
        else
        {
            /* Store result to register. */
            bool fRc = iomSaveDataToReg(pCpu, &pCpu->Param1, pRegFrame, uData1);
            AssertMsg(fRc, ("Failed to store register value!\n")); NOREF(fRc);
        }
        if (rc == VINF_SUCCESS)
        {
            /* Update guest's eflags and finish. */
            pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                                  | (eflags                &  (X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));
            iomMMIOStatLength(pVM, cb);
        }
    }

    return rc;
}


/**
 * TEST [MMIO], reg|imm
 * TEST reg, [MMIO]
 *
 * Restricted implementation.
 *
 *
 * @returns VBox status code.
 *
 * @param   pVM         The virtual machine.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   pRegFrame   Trap register frame.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 * @param   pCpu        Disassembler CPU state.
 * @param   pRange      Pointer MMIO range.
 */
static int iomInterpretTEST(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, RTGCPHYS GCPhysFault, PDISCPUSTATE pCpu,
                            PIOMMMIORANGE pRange)
{
    Assert(pRange->CTX_SUFF(pfnReadCallback) || !pRange->pfnReadCallbackR3);

    unsigned    cb     = 0;
    uint64_t    uData1 = 0;
    uint64_t    uData2 = 0;
    int         rc;

    if (iomGetRegImmData(pCpu, &pCpu->Param1, pRegFrame, &uData1, &cb))
    {
        /* and test, [MMIO]. */
        rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &uData2, cb));
    }
    else if (iomGetRegImmData(pCpu, &pCpu->Param2, pRegFrame, &uData2, &cb))
    {
        /* test [MMIO], reg|imm. */
        rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &uData1, cb));
    }
    else
    {
        AssertMsgFailed(("Disassember TEST problem..\n"));
        return VERR_IOM_MMIO_HANDLER_DISASM_ERROR;
    }

    if (rc == VINF_SUCCESS)
    {
#if HC_ARCH_BITS == 32
        /* Can't deal with 8 byte operands in our 32-bit emulation code. */
        if (cb > 4)
            return VINF_IOM_R3_MMIO_READ_WRITE;
#endif

        /* Emulate TEST (=AND without write back) and update guest EFLAGS. */
        uint32_t eflags = EMEmulateAnd((uint32_t *)&uData1, uData2, cb);
        pRegFrame->eflags.u32 = (pRegFrame->eflags.u32 & ~(X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF))
                              | (eflags                &  (X86_EFL_CF | X86_EFL_PF | X86_EFL_AF | X86_EFL_ZF | X86_EFL_SF | X86_EFL_OF));
        iomMMIOStatLength(pVM, cb);
    }

    return rc;
}


/**
 * BT [MMIO], reg|imm
 *
 * Restricted implementation.
 *
 *
 * @returns VBox status code.
 *
 * @param   pVM         The virtual machine.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   pRegFrame   Trap register frame.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 * @param   pCpu        Disassembler CPU state.
 * @param   pRange      Pointer MMIO range.
 */
static int iomInterpretBT(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, RTGCPHYS GCPhysFault, PDISCPUSTATE pCpu,
                          PIOMMMIORANGE pRange)
{
    Assert(pRange->CTX_SUFF(pfnReadCallback) || !pRange->pfnReadCallbackR3);

    uint64_t    uBit  = 0;
    uint64_t    uData = 0;
    unsigned    cbIgnored;

    if (!iomGetRegImmData(pCpu, &pCpu->Param2, pRegFrame, &uBit, &cbIgnored))
    {
        AssertMsgFailed(("Disassember BT problem..\n"));
        return VERR_IOM_MMIO_HANDLER_DISASM_ERROR;
    }
    /* The size of the memory operand only matters here. */
    unsigned cbData = DISGetParamSize(pCpu, &pCpu->Param1);

    /* bt [MMIO], reg|imm. */
    int rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &uData, cbData));
    if (rc == VINF_SUCCESS)
    {
        /* Find the bit inside the faulting address */
        pRegFrame->eflags.Bits.u1CF = (uData >> uBit);
        iomMMIOStatLength(pVM, cbData);
    }

    return rc;
}

/**
 * XCHG [MMIO], reg
 * XCHG reg, [MMIO]
 *
 * Restricted implementation.
 *
 *
 * @returns VBox status code.
 *
 * @param   pVM         The virtual machine.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   pRegFrame   Trap register frame.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 * @param   pCpu        Disassembler CPU state.
 * @param   pRange      Pointer MMIO range.
 */
static int iomInterpretXCHG(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, RTGCPHYS GCPhysFault, PDISCPUSTATE pCpu,
                            PIOMMMIORANGE pRange)
{
    /* Check for read & write handlers since IOMMMIOHandler doesn't cover this. */
    if (    (!pRange->CTX_SUFF(pfnReadCallback)  && pRange->pfnReadCallbackR3)
        ||  (!pRange->CTX_SUFF(pfnWriteCallback) && pRange->pfnWriteCallbackR3))
        return VINF_IOM_R3_MMIO_READ_WRITE;

    int         rc;
    unsigned    cb     = 0;
    uint64_t    uData1 = 0;
    uint64_t    uData2 = 0;
    if (iomGetRegImmData(pCpu, &pCpu->Param1, pRegFrame, &uData1, &cb))
    {
        /* xchg reg, [MMIO]. */
        rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &uData2, cb));
        if (rc == VINF_SUCCESS)
        {
            /* Store result to MMIO. */
            rc = VBOXSTRICTRC_TODO(iomMMIODoWrite(pVM, pVCpu, pRange, GCPhysFault, &uData1, cb));

            if (rc == VINF_SUCCESS)
            {
                /* Store result to register. */
                bool fRc = iomSaveDataToReg(pCpu, &pCpu->Param1, pRegFrame, uData2);
                AssertMsg(fRc, ("Failed to store register value!\n")); NOREF(fRc);
            }
            else
                Assert(rc == VINF_IOM_R3_MMIO_WRITE || rc == VINF_PATM_HC_MMIO_PATCH_WRITE);
        }
        else
            Assert(rc == VINF_IOM_R3_MMIO_READ || rc == VINF_PATM_HC_MMIO_PATCH_READ);
    }
    else if (iomGetRegImmData(pCpu, &pCpu->Param2, pRegFrame, &uData2, &cb))
    {
        /* xchg [MMIO], reg. */
        rc = VBOXSTRICTRC_TODO(iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, &uData1, cb));
        if (rc == VINF_SUCCESS)
        {
            /* Store result to MMIO. */
            rc = VBOXSTRICTRC_TODO(iomMMIODoWrite(pVM, pVCpu, pRange, GCPhysFault, &uData2, cb));
            if (rc == VINF_SUCCESS)
            {
                /* Store result to register. */
                bool fRc = iomSaveDataToReg(pCpu, &pCpu->Param2, pRegFrame, uData1);
                AssertMsg(fRc, ("Failed to store register value!\n")); NOREF(fRc);
            }
            else
                AssertMsg(rc == VINF_IOM_R3_MMIO_READ_WRITE || rc == VINF_IOM_R3_MMIO_WRITE || rc == VINF_PATM_HC_MMIO_PATCH_WRITE || rc == VINF_EM_RAW_EMULATE_IO_BLOCK, ("rc=%Rrc\n", rc));
        }
        else
            AssertMsg(rc == VINF_IOM_R3_MMIO_READ_WRITE || rc == VINF_IOM_R3_MMIO_READ || rc == VINF_PATM_HC_MMIO_PATCH_READ || rc == VINF_EM_RAW_EMULATE_IO_BLOCK, ("rc=%Rrc\n", rc));
    }
    else
    {
        AssertMsgFailed(("Disassember XCHG problem..\n"));
        rc = VERR_IOM_MMIO_HANDLER_DISASM_ERROR;
    }
    return rc;
}

#endif /* !IEM_USE_IEM_INSTEAD */

/**
 * Common worker for the \#PF handler and IOMMMIOPhysHandler (APIC+VT-x).
 *
 * @returns VBox status code (appropriate for GC return).
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   uErrorCode  CPU Error code.  This is UINT32_MAX when we don't have
 *                      any error code (the EPT misconfig hack).
 * @param   pCtxCore    Trap register frame.
 * @param   GCPhysFault The GC physical address corresponding to pvFault.
 * @param   pvUser      Pointer to the MMIO ring-3 range entry.
 */
static VBOXSTRICTRC iomMmioCommonPfHandler(PVM pVM, PVMCPU pVCpu, uint32_t uErrorCode, PCPUMCTXCORE pCtxCore,
                                           RTGCPHYS GCPhysFault, void *pvUser)
{
    int rc = IOM_LOCK_SHARED(pVM);
#ifndef IN_RING3
    if (rc == VERR_SEM_BUSY)
        return VINF_IOM_R3_MMIO_READ_WRITE;
#endif
    AssertRC(rc);

    STAM_PROFILE_START(&pVM->iom.s.StatRZMMIOHandler, a);
    Log(("iomMmioCommonPfHandler: GCPhys=%RGp uErr=%#x rip=%RGv\n", GCPhysFault, uErrorCode, (RTGCPTR)pCtxCore->rip));

    PIOMMMIORANGE pRange = (PIOMMMIORANGE)pvUser;
    Assert(pRange);
    Assert(pRange == iomMmioGetRange(pVM, pVCpu, GCPhysFault));
    iomMmioRetainRange(pRange);
#ifndef VBOX_WITH_STATISTICS
    IOM_UNLOCK_SHARED(pVM);

#else
    /*
     * Locate the statistics.
     */
    PIOMMMIOSTATS pStats = iomMmioGetStats(pVM, pVCpu, GCPhysFault, pRange);
    if (!pStats)
    {
        iomMmioReleaseRange(pVM, pRange);
# ifdef IN_RING3
        return VERR_NO_MEMORY;
# else
        STAM_PROFILE_STOP(&pVM->iom.s.StatRZMMIOHandler, a);
        STAM_COUNTER_INC(&pVM->iom.s.StatRZMMIOFailures);
        return VINF_IOM_R3_MMIO_READ_WRITE;
# endif
    }
#endif

#ifndef IN_RING3
    /*
     * Should we defer the request right away?  This isn't usually the case, so
     * do the simple test first and the try deal with uErrorCode being N/A.
     */
    if (RT_UNLIKELY(   (   !pRange->CTX_SUFF(pfnWriteCallback)
                        || !pRange->CTX_SUFF(pfnReadCallback))
                    && (  uErrorCode == UINT32_MAX
                        ? pRange->pfnWriteCallbackR3 || pRange->pfnReadCallbackR3
                        : uErrorCode & X86_TRAP_PF_RW
                          ? !pRange->CTX_SUFF(pfnWriteCallback) && pRange->pfnWriteCallbackR3
                          : !pRange->CTX_SUFF(pfnReadCallback)  && pRange->pfnReadCallbackR3
                        )
                   )
       )
    {
        if (uErrorCode & X86_TRAP_PF_RW)
            STAM_COUNTER_INC(&pStats->CTX_MID_Z(Write,ToR3));
        else
            STAM_COUNTER_INC(&pStats->CTX_MID_Z(Read,ToR3));

        STAM_PROFILE_STOP(&pVM->iom.s.StatRZMMIOHandler, a);
        STAM_COUNTER_INC(&pVM->iom.s.StatRZMMIOFailures);
        iomMmioReleaseRange(pVM, pRange);
        return VINF_IOM_R3_MMIO_READ_WRITE;
    }
#endif /* !IN_RING3 */

    /*
     * Retain the range and do locking.
     */
    PPDMDEVINS pDevIns = pRange->CTX_SUFF(pDevIns);
    rc = PDMCritSectEnter(pDevIns->CTX_SUFF(pCritSectRo), VINF_IOM_R3_MMIO_READ_WRITE);
    if (rc != VINF_SUCCESS)
    {
        iomMmioReleaseRange(pVM, pRange);
        return rc;
    }

#ifdef IEM_USE_IEM_INSTEAD

    /*
     * Let IEM call us back via iomMmioHandler.
     */
    VBOXSTRICTRC rcStrict = IEMExecOne(pVCpu);

    STAM_PROFILE_STOP(&pVM->iom.s.StatRZMMIOHandler, a);
    PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));
    iomMmioReleaseRange(pVM, pRange);
    if (RT_SUCCESS(rcStrict))
        return rcStrict;
    if (   rcStrict == VERR_IEM_ASPECT_NOT_IMPLEMENTED
        || rcStrict == VERR_IEM_INSTR_NOT_IMPLEMENTED)
    {
        Log(("IOM: Hit unsupported IEM feature!\n"));
        rcStrict = VINF_EM_RAW_EMULATE_INSTR;
    }
    return rcStrict;

#else

    /*
     * Disassemble the instruction and interpret it.
     */
    PDISCPUSTATE    pDis  = &pVCpu->iom.s.DisState;
    unsigned        cbOp;
    rc = EMInterpretDisasCurrent(pVM, pVCpu, pDis, &cbOp);
    if (RT_FAILURE(rc))
    {
        PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));
        iomMmioReleaseRange(pVM, pRange);
        return rc;
    }
    switch (pDis->pCurInstr->uOpcode)
    {
        case OP_MOV:
        case OP_MOVZX:
        case OP_MOVSX:
        {
            STAM_PROFILE_START(&pVM->iom.s.StatRZInstMov, b);
            AssertMsg(uErrorCode == UINT32_MAX || DISUSE_IS_EFFECTIVE_ADDR(pDis->Param1.fUse) == !!(uErrorCode & X86_TRAP_PF_RW), ("flags1=%#llx/%RTbool flags2=%#llx/%RTbool ErrCd=%#x\n", pDis->Param1.fUse, DISUSE_IS_EFFECTIVE_ADDR(pDis->Param1.fUse), pDis->Param2.fUse, DISUSE_IS_EFFECTIVE_ADDR(pDis->Param2.fUse), uErrorCode));
            if (uErrorCode != UINT32_MAX    /* EPT+MMIO optimization */
                ? uErrorCode & X86_TRAP_PF_RW
                : DISUSE_IS_EFFECTIVE_ADDR(pDis->Param1.fUse))
                rc = iomInterpretMOVxXWrite(pVM, pVCpu, pCtxCore, pDis, pRange, GCPhysFault);
            else
                rc = iomInterpretMOVxXRead(pVM, pVCpu, pCtxCore, pDis, pRange, GCPhysFault);
            STAM_PROFILE_STOP(&pVM->iom.s.StatRZInstMov, b);
            break;
        }


# ifdef IOM_WITH_MOVS_SUPPORT
        case OP_MOVSB:
        case OP_MOVSWD:
        {
            if (uErrorCode == UINT32_MAX)
                rc = VINF_IOM_R3_MMIO_READ_WRITE;
            else
            {
                STAM_PROFILE_ADV_START(&pVM->iom.s.StatRZInstMovs, c);
                PSTAMPROFILE pStat = NULL;
                rc = iomInterpretMOVS(pVM, !!(uErrorCode & X86_TRAP_PF_RW), pCtxCore, GCPhysFault, pDis, pRange, &pStat);
                STAM_PROFILE_ADV_STOP_EX(&pVM->iom.s.StatRZInstMovs, pStat, c);
            }
            break;
        }
# endif

        case OP_STOSB:
        case OP_STOSWD:
            Assert(uErrorCode & X86_TRAP_PF_RW);
            STAM_PROFILE_START(&pVM->iom.s.StatRZInstStos, d);
            rc = iomInterpretSTOS(pVM, pVCpu, pCtxCore, GCPhysFault, pDis, pRange);
            STAM_PROFILE_STOP(&pVM->iom.s.StatRZInstStos, d);
            break;

        case OP_LODSB:
        case OP_LODSWD:
            Assert(!(uErrorCode & X86_TRAP_PF_RW) || uErrorCode == UINT32_MAX);
            STAM_PROFILE_START(&pVM->iom.s.StatRZInstLods, e);
            rc = iomInterpretLODS(pVM, pVCpu, pCtxCore, GCPhysFault, pDis, pRange);
            STAM_PROFILE_STOP(&pVM->iom.s.StatRZInstLods, e);
            break;

        case OP_CMP:
            Assert(!(uErrorCode & X86_TRAP_PF_RW) || uErrorCode == UINT32_MAX);
            STAM_PROFILE_START(&pVM->iom.s.StatRZInstCmp, f);
            rc = iomInterpretCMP(pVM, pVCpu, pCtxCore, GCPhysFault, pDis, pRange);
            STAM_PROFILE_STOP(&pVM->iom.s.StatRZInstCmp, f);
            break;

        case OP_AND:
            STAM_PROFILE_START(&pVM->iom.s.StatRZInstAnd, g);
            rc = iomInterpretOrXorAnd(pVM, pVCpu, pCtxCore, GCPhysFault, pDis, pRange, EMEmulateAnd);
            STAM_PROFILE_STOP(&pVM->iom.s.StatRZInstAnd, g);
            break;

        case OP_OR:
            STAM_PROFILE_START(&pVM->iom.s.StatRZInstOr, k);
            rc = iomInterpretOrXorAnd(pVM, pVCpu, pCtxCore, GCPhysFault, pDis, pRange, EMEmulateOr);
            STAM_PROFILE_STOP(&pVM->iom.s.StatRZInstOr, k);
            break;

        case OP_XOR:
            STAM_PROFILE_START(&pVM->iom.s.StatRZInstXor, m);
            rc = iomInterpretOrXorAnd(pVM, pVCpu, pCtxCore, GCPhysFault, pDis, pRange, EMEmulateXor);
            STAM_PROFILE_STOP(&pVM->iom.s.StatRZInstXor, m);
            break;

        case OP_TEST:
            Assert(!(uErrorCode & X86_TRAP_PF_RW) || uErrorCode == UINT32_MAX);
            STAM_PROFILE_START(&pVM->iom.s.StatRZInstTest, h);
            rc = iomInterpretTEST(pVM, pVCpu, pCtxCore, GCPhysFault, pDis, pRange);
            STAM_PROFILE_STOP(&pVM->iom.s.StatRZInstTest, h);
            break;

        case OP_BT:
            Assert(!(uErrorCode & X86_TRAP_PF_RW) || uErrorCode == UINT32_MAX);
            STAM_PROFILE_START(&pVM->iom.s.StatRZInstBt, l);
            rc = iomInterpretBT(pVM, pVCpu, pCtxCore, GCPhysFault, pDis, pRange);
            STAM_PROFILE_STOP(&pVM->iom.s.StatRZInstBt, l);
            break;

        case OP_XCHG:
            STAM_PROFILE_START(&pVM->iom.s.StatRZInstXchg, i);
            rc = iomInterpretXCHG(pVM, pVCpu, pCtxCore, GCPhysFault, pDis, pRange);
            STAM_PROFILE_STOP(&pVM->iom.s.StatRZInstXchg, i);
            break;


        /*
         * The instruction isn't supported. Hand it on to ring-3.
         */
        default:
            STAM_COUNTER_INC(&pVM->iom.s.StatRZInstOther);
            rc = VINF_IOM_R3_MMIO_READ_WRITE;
            break;
    }

    /*
     * On success advance EIP.
     */
    if (rc == VINF_SUCCESS)
        pCtxCore->rip += cbOp;
    else
    {
        STAM_COUNTER_INC(&pVM->iom.s.StatRZMMIOFailures);
# if defined(VBOX_WITH_STATISTICS) && !defined(IN_RING3)
        switch (rc)
        {
            case VINF_IOM_R3_MMIO_READ:
            case VINF_IOM_R3_MMIO_READ_WRITE:
                STAM_COUNTER_INC(&pStats->CTX_MID_Z(Read,ToR3));
                break;
            case VINF_IOM_R3_MMIO_WRITE:
                STAM_COUNTER_INC(&pStats->CTX_MID_Z(Write,ToR3));
                break;
        }
# endif
    }

    STAM_PROFILE_STOP(&pVM->iom.s.StatRZMMIOHandler, a);
    PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));
    iomMmioReleaseRange(pVM, pRange);
    return rc;
#endif /* !IEM_USE_IEM_INSTEAD */
}


/**
 * @callback_method_impl{FNPGMRZPHYSPFHANDLER,
 *      \#PF access handler callback for MMIO pages.}
 *
 * @remarks The @a pvUser argument points to the IOMMMIORANGE.
 */
DECLEXPORT(VBOXSTRICTRC) iomMmioPfHandler(PVM pVM, PVMCPU pVCpu, RTGCUINT uErrorCode, PCPUMCTXCORE pCtxCore, RTGCPTR pvFault,
                                          RTGCPHYS GCPhysFault, void *pvUser)
{
    LogFlow(("iomMmioPfHandler: GCPhys=%RGp uErr=%#x pvFault=%RGv rip=%RGv\n",
             GCPhysFault, (uint32_t)uErrorCode, pvFault, (RTGCPTR)pCtxCore->rip));
    return iomMmioCommonPfHandler(pVM, pVCpu, (uint32_t)uErrorCode, pCtxCore, GCPhysFault, pvUser);
}


/**
 * Physical access handler for MMIO ranges.
 *
 * @returns VBox status code (appropriate for GC return).
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   uErrorCode  CPU Error code.
 * @param   pCtxCore    Trap register frame.
 * @param   GCPhysFault The GC physical address.
 */
VMMDECL(VBOXSTRICTRC) IOMMMIOPhysHandler(PVM pVM, PVMCPU pVCpu, RTGCUINT uErrorCode, PCPUMCTXCORE pCtxCore, RTGCPHYS GCPhysFault)
{
    /*
     * We don't have a range here, so look it up before calling the common function.
     */
    int rc2 = IOM_LOCK_SHARED(pVM); NOREF(rc2);
#ifndef IN_RING3
    if (rc2 == VERR_SEM_BUSY)
        return VINF_IOM_R3_MMIO_READ_WRITE;
#endif
    PIOMMMIORANGE pRange = iomMmioGetRange(pVM, pVCpu, GCPhysFault);
    if (RT_UNLIKELY(!pRange))
    {
        IOM_UNLOCK_SHARED(pVM);
        return VERR_IOM_MMIO_RANGE_NOT_FOUND;
    }
    iomMmioRetainRange(pRange);
    IOM_UNLOCK_SHARED(pVM);

    VBOXSTRICTRC rcStrict = iomMmioCommonPfHandler(pVM, pVCpu, (uint32_t)uErrorCode, pCtxCore, GCPhysFault, pRange);

    iomMmioReleaseRange(pVM, pRange);
    return VBOXSTRICTRC_VAL(rcStrict);
}


/**
 * @callback_method_impl{FNPGMPHYSHANDLER, MMIO page accesses}
 *
 * @remarks The @a pvUser argument points to the MMIO range entry.
 */
PGM_ALL_CB2_DECL(VBOXSTRICTRC) iomMmioHandler(PVM pVM, PVMCPU pVCpu, RTGCPHYS GCPhysFault, void *pvPhys, void *pvBuf,
                                              size_t cbBuf, PGMACCESSTYPE enmAccessType, PGMACCESSORIGIN enmOrigin, void *pvUser)
{
    PIOMMMIORANGE pRange = (PIOMMMIORANGE)pvUser;
    STAM_COUNTER_INC(&pVM->iom.s.StatR3MMIOHandler);

    AssertMsg(cbBuf >= 1 && cbBuf <= 16, ("%zu\n", cbBuf));
    AssertPtr(pRange);
    NOREF(pvPhys); NOREF(enmOrigin);

    /*
     * Validate the range.
     */
    int rc = IOM_LOCK_SHARED(pVM);
#ifndef IN_RING3
    if (rc == VERR_SEM_BUSY)
        return VINF_IOM_R3_MMIO_READ_WRITE;
#endif
    AssertRC(rc);
    Assert(pRange == iomMmioGetRange(pVM, pVCpu, GCPhysFault));

    /*
     * Perform locking.
     */
    iomMmioRetainRange(pRange);
    PPDMDEVINS pDevIns = pRange->CTX_SUFF(pDevIns);
    IOM_UNLOCK_SHARED(pVM);
    VBOXSTRICTRC rcStrict = PDMCritSectEnter(pDevIns->CTX_SUFF(pCritSectRo), VINF_IOM_R3_MMIO_READ_WRITE);
    if (rcStrict == VINF_SUCCESS)
    {
        /*
         * Perform the access.
         */
        if (enmAccessType == PGMACCESSTYPE_READ)
            rcStrict = iomMMIODoRead(pVM, pVCpu, pRange, GCPhysFault, pvBuf, (unsigned)cbBuf);
        else
            rcStrict = iomMMIODoWrite(pVM, pVCpu, pRange, GCPhysFault, pvBuf, (unsigned)cbBuf);

        /* Check the return code. */
#ifdef IN_RING3
        AssertMsg(rcStrict == VINF_SUCCESS, ("%Rrc - %RGp - %s\n", VBOXSTRICTRC_VAL(rcStrict), GCPhysFault, pRange->pszDesc));
#else
        AssertMsg(   rcStrict == VINF_SUCCESS
                  || rcStrict == (enmAccessType == PGMACCESSTYPE_READ ? VINF_IOM_R3_MMIO_READ :  VINF_IOM_R3_MMIO_WRITE)
                  || rcStrict == VINF_IOM_R3_MMIO_READ_WRITE
                  || rcStrict == VINF_EM_DBG_STOP
                  || rcStrict == VINF_EM_DBG_BREAKPOINT
                  || rcStrict == VINF_EM_OFF
                  || rcStrict == VINF_EM_SUSPEND
                  || rcStrict == VINF_EM_RESET
                  || rcStrict == VINF_EM_RAW_EMULATE_IO_BLOCK
                  //|| rcStrict == VINF_EM_HALT       /* ?? */
                  //|| rcStrict == VINF_EM_NO_MEMORY  /* ?? */
                  , ("%Rrc - %RGp - %p\n", VBOXSTRICTRC_VAL(rcStrict), GCPhysFault, pDevIns));
#endif

        iomMmioReleaseRange(pVM, pRange);
        PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));
    }
    else
        iomMmioReleaseRange(pVM, pRange);
    return rcStrict;
}


#ifdef IN_RING3 /* Only used by REM. */

/**
 * Reads a MMIO register.
 *
 * @returns VBox status code.
 *
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   GCPhys      The physical address to read.
 * @param   pu32Value   Where to store the value read.
 * @param   cbValue     The size of the register to read in bytes. 1, 2 or 4 bytes.
 */
VMMDECL(VBOXSTRICTRC) IOMMMIORead(PVM pVM, PVMCPU pVCpu, RTGCPHYS GCPhys, uint32_t *pu32Value, size_t cbValue)
{
    /* Take the IOM lock before performing any MMIO. */
    VBOXSTRICTRC rc = IOM_LOCK_SHARED(pVM);
#ifndef IN_RING3
    if (rc == VERR_SEM_BUSY)
        return VINF_IOM_R3_MMIO_WRITE;
#endif
    AssertRC(VBOXSTRICTRC_VAL(rc));
#if defined(IEM_VERIFICATION_MODE) && defined(IN_RING3)
    IEMNotifyMMIORead(pVM, GCPhys, cbValue);
#endif

    /*
     * Lookup the current context range node and statistics.
     */
    PIOMMMIORANGE pRange = iomMmioGetRange(pVM, pVCpu, GCPhys);
    if (!pRange)
    {
        AssertMsgFailed(("Handlers and page tables are out of sync or something! GCPhys=%RGp cbValue=%d\n", GCPhys, cbValue));
        IOM_UNLOCK_SHARED(pVM);
        return VERR_IOM_MMIO_RANGE_NOT_FOUND;
    }
    iomMmioRetainRange(pRange);
#ifndef VBOX_WITH_STATISTICS
    IOM_UNLOCK_SHARED(pVM);

#else  /* VBOX_WITH_STATISTICS */
    PIOMMMIOSTATS pStats = iomMmioGetStats(pVM, pVCpu, GCPhys, pRange);
    if (!pStats)
    {
        iomMmioReleaseRange(pVM, pRange);
# ifdef IN_RING3
        return VERR_NO_MEMORY;
# else
        return VINF_IOM_R3_MMIO_READ;
# endif
    }
    STAM_COUNTER_INC(&pStats->Accesses);
#endif /* VBOX_WITH_STATISTICS */

    if (pRange->CTX_SUFF(pfnReadCallback))
    {
        /*
         * Perform locking.
         */
        PPDMDEVINS pDevIns = pRange->CTX_SUFF(pDevIns);
        rc = PDMCritSectEnter(pDevIns->CTX_SUFF(pCritSectRo), VINF_IOM_R3_MMIO_WRITE);
        if (rc != VINF_SUCCESS)
        {
            iomMmioReleaseRange(pVM, pRange);
            return rc;
        }

        /*
         * Perform the read and deal with the result.
         */
        STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfRead), a);
        if (   (cbValue == 4 && !(GCPhys & 3))
            || (pRange->fFlags & IOMMMIO_FLAGS_READ_MODE) == IOMMMIO_FLAGS_READ_PASSTHRU
            || (cbValue == 8 && !(GCPhys & 7)) )
            rc = pRange->CTX_SUFF(pfnReadCallback)(pRange->CTX_SUFF(pDevIns), pRange->CTX_SUFF(pvUser), GCPhys,
                                                   pu32Value, (unsigned)cbValue);
        else
            rc = iomMMIODoComplicatedRead(pVM, pRange, GCPhys, pu32Value, (unsigned)cbValue);
        STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfRead), a);
        switch (VBOXSTRICTRC_VAL(rc))
        {
            case VINF_SUCCESS:
                Log4(("IOMMMIORead: GCPhys=%RGp *pu32=%08RX32 cb=%d rc=VINF_SUCCESS\n", GCPhys, *pu32Value, cbValue));
                PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));
                iomMmioReleaseRange(pVM, pRange);
                return rc;
#ifndef IN_RING3
            case VINF_IOM_R3_MMIO_READ:
            case VINF_IOM_R3_MMIO_READ_WRITE:
                STAM_COUNTER_INC(&pStats->CTX_MID_Z(Read,ToR3));
#endif
            default:
                Log4(("IOMMMIORead: GCPhys=%RGp *pu32=%08RX32 cb=%d rc=%Rrc\n", GCPhys, *pu32Value, cbValue, VBOXSTRICTRC_VAL(rc)));
                PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));
                iomMmioReleaseRange(pVM, pRange);
                return rc;

            case VINF_IOM_MMIO_UNUSED_00:
                iomMMIODoRead00s(pu32Value, cbValue);
                Log4(("IOMMMIORead: GCPhys=%RGp *pu32=%08RX32 cb=%d rc=%Rrc\n", GCPhys, *pu32Value, cbValue, VBOXSTRICTRC_VAL(rc)));
                PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));
                iomMmioReleaseRange(pVM, pRange);
                return VINF_SUCCESS;

            case VINF_IOM_MMIO_UNUSED_FF:
                iomMMIODoReadFFs(pu32Value, cbValue);
                Log4(("IOMMMIORead: GCPhys=%RGp *pu32=%08RX32 cb=%d rc=%Rrc\n", GCPhys, *pu32Value, cbValue, VBOXSTRICTRC_VAL(rc)));
                PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));
                iomMmioReleaseRange(pVM, pRange);
                return VINF_SUCCESS;
        }
        /* not reached */
    }
#ifndef IN_RING3
    if (pRange->pfnReadCallbackR3)
    {
        STAM_COUNTER_INC(&pStats->CTX_MID_Z(Read,ToR3));
        iomMmioReleaseRange(pVM, pRange);
        return VINF_IOM_R3_MMIO_READ;
    }
#endif

    /*
     * Unassigned memory - this is actually not supposed t happen...
     */
    STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfRead), a); /** @todo STAM_PROFILE_ADD_ZERO_PERIOD */
    STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfRead), a);
    iomMMIODoReadFFs(pu32Value, cbValue);
    Log4(("IOMMMIORead: GCPhys=%RGp *pu32=%08RX32 cb=%d rc=VINF_SUCCESS\n", GCPhys, *pu32Value, cbValue));
    iomMmioReleaseRange(pVM, pRange);
    return VINF_SUCCESS;
}


/**
 * Writes to a MMIO register.
 *
 * @returns VBox status code.
 *
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   GCPhys      The physical address to write to.
 * @param   u32Value    The value to write.
 * @param   cbValue     The size of the register to read in bytes. 1, 2 or 4 bytes.
 */
VMMDECL(VBOXSTRICTRC) IOMMMIOWrite(PVM pVM, PVMCPU pVCpu, RTGCPHYS GCPhys, uint32_t u32Value, size_t cbValue)
{
    /* Take the IOM lock before performing any MMIO. */
    VBOXSTRICTRC rc = IOM_LOCK_SHARED(pVM);
#ifndef IN_RING3
    if (rc == VERR_SEM_BUSY)
        return VINF_IOM_R3_MMIO_WRITE;
#endif
    AssertRC(VBOXSTRICTRC_VAL(rc));
#if defined(IEM_VERIFICATION_MODE) && defined(IN_RING3)
    IEMNotifyMMIOWrite(pVM, GCPhys, u32Value, cbValue);
#endif

    /*
     * Lookup the current context range node.
     */
    PIOMMMIORANGE pRange = iomMmioGetRange(pVM, pVCpu, GCPhys);
    if (!pRange)
    {
        AssertMsgFailed(("Handlers and page tables are out of sync or something! GCPhys=%RGp cbValue=%d\n", GCPhys, cbValue));
        IOM_UNLOCK_SHARED(pVM);
        return VERR_IOM_MMIO_RANGE_NOT_FOUND;
    }
    iomMmioRetainRange(pRange);
#ifndef VBOX_WITH_STATISTICS
    IOM_UNLOCK_SHARED(pVM);

#else  /* VBOX_WITH_STATISTICS */
    PIOMMMIOSTATS pStats = iomMmioGetStats(pVM, pVCpu, GCPhys, pRange);
    if (!pStats)
    {
        iomMmioReleaseRange(pVM, pRange);
# ifdef IN_RING3
        return VERR_NO_MEMORY;
# else
        return VINF_IOM_R3_MMIO_WRITE;
# endif
    }
    STAM_COUNTER_INC(&pStats->Accesses);
#endif /* VBOX_WITH_STATISTICS */

    if (pRange->CTX_SUFF(pfnWriteCallback))
    {
        /*
         * Perform locking.
         */
        PPDMDEVINS pDevIns = pRange->CTX_SUFF(pDevIns);
        rc = PDMCritSectEnter(pDevIns->CTX_SUFF(pCritSectRo), VINF_IOM_R3_MMIO_READ);
        if (rc != VINF_SUCCESS)
        {
            iomMmioReleaseRange(pVM, pRange);
            return rc;
        }

        /*
         * Perform the write.
         */
        STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfWrite), a);
        if (   (cbValue == 4 && !(GCPhys & 3))
            || (pRange->fFlags & IOMMMIO_FLAGS_WRITE_MODE) == IOMMMIO_FLAGS_WRITE_PASSTHRU
            || (cbValue == 8 && !(GCPhys & 7)) )
            rc = pRange->CTX_SUFF(pfnWriteCallback)(pRange->CTX_SUFF(pDevIns), pRange->CTX_SUFF(pvUser),
                                                    GCPhys, &u32Value, (unsigned)cbValue);
        else
            rc = iomMMIODoComplicatedWrite(pVM, pRange, GCPhys, &u32Value, (unsigned)cbValue);
        STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfWrite), a);
#ifndef IN_RING3
        if (    rc == VINF_IOM_R3_MMIO_WRITE
            ||  rc == VINF_IOM_R3_MMIO_READ_WRITE)
            STAM_COUNTER_INC(&pStats->CTX_MID_Z(Write,ToR3));
#endif
        Log4(("IOMMMIOWrite: GCPhys=%RGp u32=%08RX32 cb=%d rc=%Rrc\n", GCPhys, u32Value, cbValue, VBOXSTRICTRC_VAL(rc)));
        iomMmioReleaseRange(pVM, pRange);
        PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));
        return rc;
    }
#ifndef IN_RING3
    if (pRange->pfnWriteCallbackR3)
    {
        STAM_COUNTER_INC(&pStats->CTX_MID_Z(Write,ToR3));
        iomMmioReleaseRange(pVM, pRange);
        return VINF_IOM_R3_MMIO_WRITE;
    }
#endif

    /*
     * No write handler, nothing to do.
     */
    STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfWrite), a);
    STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfWrite), a);
    Log4(("IOMMMIOWrite: GCPhys=%RGp u32=%08RX32 cb=%d rc=%Rrc\n", GCPhys, u32Value, cbValue, VINF_SUCCESS));
    iomMmioReleaseRange(pVM, pRange);
    return VINF_SUCCESS;
}

#endif /* IN_RING3 - only used by REM. */
#ifndef IEM_USE_IEM_INSTEAD

/**
 * [REP*] INSB/INSW/INSD
 * ES:EDI,DX[,ECX]
 *
 * @remark Assumes caller checked the access privileges (IOMInterpretCheckPortIOAccess)
 *
 * @returns Strict VBox status code. Informational status codes other than the one documented
 *          here are to be treated as internal failure. Use IOM_SUCCESS() to check for success.
 * @retval  VINF_SUCCESS                Success.
 * @retval  VINF_EM_FIRST-VINF_EM_LAST  Success with some exceptions (see IOM_SUCCESS()), the
 *                                      status code must be passed on to EM.
 * @retval  VINF_IOM_R3_IOPORT_READ     Defer the read to ring-3. (R0/GC only)
 * @retval  VINF_EM_RAW_EMULATE_INSTR   Defer the read to the REM.
 * @retval  VINF_EM_RAW_GUEST_TRAP      The exception was left pending. (TRPMRaiseXcptErr)
 * @retval  VINF_TRPM_XCPT_DISPATCHED   The exception was raised and dispatched for raw-mode execution. (TRPMRaiseXcptErr)
 * @retval  VINF_EM_RESCHEDULE_REM      The exception was dispatched and cannot be executed in raw-mode. (TRPMRaiseXcptErr)
 *
 * @param   pVM             The virtual machine.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   pRegFrame       Pointer to CPUMCTXCORE guest registers structure.
 * @param   uPort           IO Port
 * @param   uPrefix         IO instruction prefix
 * @param   enmAddrMode     The address mode.
 * @param   cbTransfer      Size of transfer unit
 */
VMMDECL(VBOXSTRICTRC) IOMInterpretINSEx(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, uint32_t uPort, uint32_t uPrefix,
                                        DISCPUMODE enmAddrMode, uint32_t cbTransfer)
{
    STAM_COUNTER_INC(&pVM->iom.s.StatInstIns);

    /*
     * We do not support REPNE or decrementing destination
     * pointer. Segment prefixes are deliberately ignored, as per the instruction specification.
     */
    if (   (uPrefix & DISPREFIX_REPNE)
        || pRegFrame->eflags.Bits.u1DF)
        return VINF_EM_RAW_EMULATE_INSTR;

    /*
     * Get bytes/words/dwords count to transfer.
     */
    uint64_t const fAddrMask = iomDisModeToMask(enmAddrMode);
    RTGCUINTREG cTransfers = 1;
    if (uPrefix & DISPREFIX_REP)
    {
#ifndef IN_RC
        if (    CPUMIsGuestIn64BitCode(pVCpu)
            &&  pRegFrame->rcx >= _4G)
            return VINF_EM_RAW_EMULATE_INSTR;
#endif
        cTransfers = pRegFrame->rcx & fAddrMask;
        if (!cTransfers)
            return VINF_SUCCESS;
    }

    /* Convert destination address es:edi. */
    RTGCPTR GCPtrDst;
    int rc2 = SELMToFlatEx(pVCpu, DISSELREG_ES, pRegFrame, pRegFrame->rdi & fAddrMask,
                           SELMTOFLAT_FLAGS_HYPER | SELMTOFLAT_FLAGS_NO_PL,
                           &GCPtrDst);
    if (RT_FAILURE(rc2))
    {
        Log(("INS destination address conversion failed -> fallback, rc2=%d\n", rc2));
        return VINF_EM_RAW_EMULATE_INSTR;
    }

    /* Access verification first; we can't recover from traps inside this instruction, as the port read cannot be repeated. */
    uint32_t const cpl = CPUMGetGuestCPL(pVCpu);
    rc2 = PGMVerifyAccess(pVCpu, (RTGCUINTPTR)GCPtrDst, cTransfers * cbTransfer,
                          X86_PTE_RW | ((cpl == 3) ? X86_PTE_US : 0));
    if (rc2 != VINF_SUCCESS)
    {
        Log(("INS will generate a trap -> fallback, rc2=%d\n", rc2));
        return VINF_EM_RAW_EMULATE_INSTR;
    }

    Log(("IOM: rep ins%d port %#x count %d\n", cbTransfer * 8, uPort, cTransfers));
    VBOXSTRICTRC rcStrict = VINF_SUCCESS;
    if (cTransfers > 1)
    {
        /*
         * Work the string page by page, letting the device handle as much
         * as it likes via the string I/O interface.
         */
        for (;;)
        {
            PGMPAGEMAPLOCK Lock;
            void          *pvDst;
            rc2 = PGMPhysGCPtr2CCPtr(pVCpu, GCPtrDst, &pvDst, &Lock);
            if (RT_SUCCESS(rc2))
            {
                uint32_t cMaxThisTime = (PAGE_SIZE - (GCPtrDst & PAGE_OFFSET_MASK)) / cbTransfer;
                if (cMaxThisTime > cTransfers)
                    cMaxThisTime      = cTransfers;
                if (!cMaxThisTime)
                    break;
                uint32_t cThisTime    = cMaxThisTime;

                rcStrict = IOMIOPortReadString(pVM, pVCpu, uPort, pvDst, &cThisTime, cbTransfer);
                AssertRC(VBOXSTRICTRC_VAL(rcStrict));
                Assert(cThisTime <= cMaxThisTime); /* cThisTime is now how many transfers we have left. */

                uint32_t const cActual  = cMaxThisTime - cThisTime;
                if (cActual)
                {   /* Must dirty the page.  */
                    uint8_t b = *(uint8_t *)pvDst;
                    iomRamWrite(pVCpu, pRegFrame, GCPtrDst, &b, 1);
                }

                PGMPhysReleasePageMappingLock(pVM, &Lock);

                uint32_t const cbActual = cActual * cbTransfer;
                cTransfers    -= cActual;
                pRegFrame->rdi = ((pRegFrame->rdi + cbActual) & fAddrMask)
                               | (pRegFrame->rdi & ~fAddrMask);
                GCPtrDst      += cbActual;

                if (   cThisTime
                    || !cTransfers
                    || rcStrict != VINF_SUCCESS
                    || (GCPtrDst & PAGE_OFFSET_MASK))
                    break;
            }
            else
            {
                Log(("IOMInterpretOUTSEx: PGMPhysGCPtr2CCPtr %#RGv -> %Rrc\n", GCPtrDst, rc2));
                break;
            }
        }
    }

    /*
     * Single transfer / unmapped memory fallback.
     */
#ifdef IN_RC
    MMGCRamRegisterTrapHandler(pVM);
#endif
    while (cTransfers && rcStrict == VINF_SUCCESS)
    {
        uint32_t u32Value;
        rcStrict = IOMIOPortRead(pVM, pVCpu, uPort, &u32Value, cbTransfer);
        if (!IOM_SUCCESS(rcStrict))
            break;
        rc2 = iomRamWrite(pVCpu, pRegFrame, GCPtrDst, &u32Value, cbTransfer);
        Assert(rc2 == VINF_SUCCESS); NOREF(rc2);
        GCPtrDst = (RTGCPTR)((RTGCUINTPTR)GCPtrDst + cbTransfer);
        pRegFrame->rdi  = ((pRegFrame->rdi + cbTransfer) & fAddrMask)
                        | (pRegFrame->rdi & ~fAddrMask);
        cTransfers--;
    }
#ifdef IN_RC
    MMGCRamDeregisterTrapHandler(pVM);
#endif

    /* Update rcx on exit. */
    if (uPrefix & DISPREFIX_REP)
        pRegFrame->rcx = (cTransfers & fAddrMask)
                       | (pRegFrame->rcx & ~fAddrMask);

    AssertMsg(rcStrict == VINF_SUCCESS || rcStrict == VINF_IOM_R3_IOPORT_READ || (rcStrict >= VINF_EM_FIRST && rcStrict <= VINF_EM_LAST) || RT_FAILURE(rcStrict), ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));
    return rcStrict;
}


/**
 * [REP*] OUTSB/OUTSW/OUTSD
 * DS:ESI,DX[,ECX]
 *
 * @remark  Assumes caller checked the access privileges (IOMInterpretCheckPortIOAccess)
 *
 * @returns Strict VBox status code. Informational status codes other than the one documented
 *          here are to be treated as internal failure. Use IOM_SUCCESS() to check for success.
 * @retval  VINF_SUCCESS                Success.
 * @retval  VINF_EM_FIRST-VINF_EM_LAST  Success with some exceptions (see IOM_SUCCESS()), the
 *                                      status code must be passed on to EM.
 * @retval  VINF_IOM_R3_IOPORT_WRITE    Defer the write to ring-3. (R0/GC only)
 * @retval  VINF_EM_RAW_GUEST_TRAP      The exception was left pending. (TRPMRaiseXcptErr)
 * @retval  VINF_TRPM_XCPT_DISPATCHED   The exception was raised and dispatched for raw-mode execution. (TRPMRaiseXcptErr)
 * @retval  VINF_EM_RESCHEDULE_REM      The exception was dispatched and cannot be executed in raw-mode. (TRPMRaiseXcptErr)
 *
 * @param   pVM             The virtual machine.
 * @param   pVCpu           Pointer to the virtual CPU structure of the caller.
 * @param   pRegFrame       Pointer to CPUMCTXCORE guest registers structure.
 * @param   uPort           IO Port
 * @param   uPrefix         IO instruction prefix
 * @param   enmAddrMode     The address mode.
 * @param   cbTransfer      Size of transfer unit
 *
 * @remarks This API will probably be relaced by IEM before long, so no use in
 *          optimizing+fixing stuff too much here.
 */
VMMDECL(VBOXSTRICTRC) IOMInterpretOUTSEx(PVM pVM, PVMCPU pVCpu, PCPUMCTXCORE pRegFrame, uint32_t uPort, uint32_t uPrefix,
                                         DISCPUMODE enmAddrMode, uint32_t cbTransfer)
{
    STAM_COUNTER_INC(&pVM->iom.s.StatInstOuts);

    /*
     * We do not support segment prefixes, REPNE or
     * decrementing source pointer.
     */
    if (   (uPrefix & (DISPREFIX_SEG | DISPREFIX_REPNE))
        || pRegFrame->eflags.Bits.u1DF)
        return VINF_EM_RAW_EMULATE_INSTR;

    /*
     * Get bytes/words/dwords count to transfer.
     */
    uint64_t const fAddrMask = iomDisModeToMask(enmAddrMode);
    RTGCUINTREG cTransfers = 1;
    if (uPrefix & DISPREFIX_REP)
    {
#ifndef IN_RC
        if (    CPUMIsGuestIn64BitCode(pVCpu)
            &&  pRegFrame->rcx >= _4G)
            return VINF_EM_RAW_EMULATE_INSTR;
#endif
        cTransfers = pRegFrame->rcx & fAddrMask;
        if (!cTransfers)
            return VINF_SUCCESS;
    }

    /* Convert source address ds:esi. */
    RTGCPTR GCPtrSrc;
    int rc2 = SELMToFlatEx(pVCpu, DISSELREG_DS, pRegFrame, pRegFrame->rsi & fAddrMask,
                           SELMTOFLAT_FLAGS_HYPER | SELMTOFLAT_FLAGS_NO_PL,
                           &GCPtrSrc);
    if (RT_FAILURE(rc2))
    {
        Log(("OUTS source address conversion failed -> fallback, rc2=%Rrc\n", rc2));
        return VINF_EM_RAW_EMULATE_INSTR;
    }

    /* Access verification first; we currently can't recover properly from traps inside this instruction */
    uint32_t const cpl = CPUMGetGuestCPL(pVCpu);
    rc2 = PGMVerifyAccess(pVCpu, (RTGCUINTPTR)GCPtrSrc, cTransfers * cbTransfer,
                          (cpl == 3) ? X86_PTE_US : 0);
    if (rc2 != VINF_SUCCESS)
    {
        Log(("OUTS will generate a trap -> fallback, rc2=%Rrc\n", rc2));
        return VINF_EM_RAW_EMULATE_INSTR;
    }

    Log(("IOM: rep outs%d port %#x count %d\n", cbTransfer * 8, uPort, cTransfers));
    VBOXSTRICTRC rcStrict = VINF_SUCCESS;
    if (cTransfers > 1)
    {
        /*
         * Work the string page by page, letting the device handle as much
         * as it likes via the string I/O interface.
         */
        for (;;)
        {
            PGMPAGEMAPLOCK Lock;
            void const    *pvSrc;
            rc2 = PGMPhysGCPtr2CCPtrReadOnly(pVCpu, GCPtrSrc, &pvSrc, &Lock);
            if (RT_SUCCESS(rc2))
            {
                uint32_t cMaxThisTime = (PAGE_SIZE - (GCPtrSrc & PAGE_OFFSET_MASK)) / cbTransfer;
                if (cMaxThisTime > cTransfers)
                    cMaxThisTime      = cTransfers;
                if (!cMaxThisTime)
                    break;
                uint32_t cThisTime    = cMaxThisTime;

                rcStrict = IOMIOPortWriteString(pVM, pVCpu, uPort, pvSrc, &cThisTime, cbTransfer);
                AssertRC(VBOXSTRICTRC_VAL(rcStrict));
                Assert(cThisTime <= cMaxThisTime); /* cThisTime is now how many transfers we have left. */

                PGMPhysReleasePageMappingLock(pVM, &Lock);

                uint32_t const cActual  = cMaxThisTime - cThisTime;
                uint32_t const cbActual = cActual * cbTransfer;
                cTransfers    -= cActual;
                pRegFrame->rsi = ((pRegFrame->rsi + cbActual) & fAddrMask)
                               | (pRegFrame->rsi & ~fAddrMask);
                GCPtrSrc      += cbActual;

                if (   cThisTime
                    || !cTransfers
                    || rcStrict != VINF_SUCCESS
                    || (GCPtrSrc & PAGE_OFFSET_MASK))
                    break;
            }
            else
            {
                Log(("IOMInterpretOUTSEx: PGMPhysGCPtr2CCPtrReadOnly %#RGv -> %Rrc\n", GCPtrSrc, rc2));
                break;
            }
        }
    }

    /*
     * Single transfer / unmapped memory fallback.
     */
#ifdef IN_RC
    MMGCRamRegisterTrapHandler(pVM);
#endif

    while (cTransfers && rcStrict == VINF_SUCCESS)
    {
        uint32_t u32Value = 0;
        rcStrict = iomRamRead(pVCpu, &u32Value, GCPtrSrc, cbTransfer);
        if (rcStrict != VINF_SUCCESS)
            break;
        rcStrict = IOMIOPortWrite(pVM, pVCpu, uPort, u32Value, cbTransfer);
        if (!IOM_SUCCESS(rcStrict))
            break;
        GCPtrSrc = (RTGCPTR)((RTUINTPTR)GCPtrSrc + cbTransfer);
        pRegFrame->rsi  = ((pRegFrame->rsi + cbTransfer) & fAddrMask)
                        | (pRegFrame->rsi & ~fAddrMask);
        cTransfers--;
    }

#ifdef IN_RC
    MMGCRamDeregisterTrapHandler(pVM);
#endif

    /* Update rcx on exit. */
    if (uPrefix & DISPREFIX_REP)
        pRegFrame->rcx = (cTransfers & fAddrMask)
                       | (pRegFrame->rcx & ~fAddrMask);

    AssertMsg(rcStrict == VINF_SUCCESS || rcStrict == VINF_IOM_R3_IOPORT_WRITE || (rcStrict >= VINF_EM_FIRST && rcStrict <= VINF_EM_LAST) || RT_FAILURE(rcStrict), ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict)));
    return rcStrict;
}

#endif /* !IEM_USE_IEM_INSTEAD */


#ifndef IN_RC

/**
 * Mapping an MMIO2 page in place of an MMIO page for direct access.
 *
 * (This is a special optimization used by the VGA device.)
 *
 * @returns VBox status code.  This API may return VINF_SUCCESS even if no
 *          remapping is made,.
 *
 * @param   pVM             The virtual machine.
 * @param   GCPhys          The address of the MMIO page to be changed.
 * @param   GCPhysRemapped  The address of the MMIO2 page.
 * @param   fPageFlags      Page flags to set. Must be (X86_PTE_RW | X86_PTE_P)
 *                          for the time being.
 */
VMMDECL(int) IOMMMIOMapMMIO2Page(PVM pVM, RTGCPHYS GCPhys, RTGCPHYS GCPhysRemapped, uint64_t fPageFlags)
{
# ifndef IEM_VERIFICATION_MODE_FULL
    /* Currently only called from the VGA device during MMIO. */
    Log(("IOMMMIOMapMMIO2Page %RGp -> %RGp flags=%RX64\n", GCPhys, GCPhysRemapped, fPageFlags));
    AssertReturn(fPageFlags == (X86_PTE_RW | X86_PTE_P), VERR_INVALID_PARAMETER);
    PVMCPU pVCpu = VMMGetCpu(pVM);

    /* This currently only works in real mode, protected mode without paging or with nested paging. */
    if (    !HMIsEnabled(pVM)       /* useless without VT-x/AMD-V */
        ||  (   CPUMIsGuestInPagedProtectedMode(pVCpu)
             && !HMIsNestedPagingActive(pVM)))
        return VINF_SUCCESS;    /* ignore */

    int rc = IOM_LOCK_SHARED(pVM);
    if (RT_FAILURE(rc))
        return VINF_SUCCESS; /* better luck the next time around */

    /*
     * Lookup the context range node the page belongs to.
     */
    PIOMMMIORANGE pRange = iomMmioGetRange(pVM, pVCpu, GCPhys);
    AssertMsgReturn(pRange,
                    ("Handlers and page tables are out of sync or something! GCPhys=%RGp\n", GCPhys), VERR_IOM_MMIO_RANGE_NOT_FOUND);

    Assert((pRange->GCPhys       & PAGE_OFFSET_MASK) == 0);
    Assert((pRange->Core.KeyLast & PAGE_OFFSET_MASK) == PAGE_OFFSET_MASK);

    /*
     * Do the aliasing; page align the addresses since PGM is picky.
     */
    GCPhys         &= ~(RTGCPHYS)PAGE_OFFSET_MASK;
    GCPhysRemapped &= ~(RTGCPHYS)PAGE_OFFSET_MASK;

    rc = PGMHandlerPhysicalPageAlias(pVM, pRange->GCPhys, GCPhys, GCPhysRemapped);

    IOM_UNLOCK_SHARED(pVM);
    AssertRCReturn(rc, rc);

    /*
     * Modify the shadow page table. Since it's an MMIO page it won't be present and we
     * can simply prefetch it.
     *
     * Note: This is a NOP in the EPT case; we'll just let it fault again to resync the page.
     */
#  if 0 /* The assertion is wrong for the PGM_SYNC_CLEAR_PGM_POOL and VINF_PGM_HANDLER_ALREADY_ALIASED cases. */
#   ifdef VBOX_STRICT
    uint64_t fFlags;
    RTHCPHYS HCPhys;
    rc = PGMShwGetPage(pVCpu, (RTGCPTR)GCPhys, &fFlags, &HCPhys);
    Assert(rc == VERR_PAGE_NOT_PRESENT || rc == VERR_PAGE_TABLE_NOT_PRESENT);
#   endif
#  endif
    rc = PGMPrefetchPage(pVCpu, (RTGCPTR)GCPhys);
    Assert(rc == VINF_SUCCESS || rc == VERR_PAGE_NOT_PRESENT || rc == VERR_PAGE_TABLE_NOT_PRESENT);
# endif /* !IEM_VERIFICATION_MODE_FULL */
    return VINF_SUCCESS;
}


# ifndef IEM_VERIFICATION_MODE_FULL
/**
 * Mapping a HC page in place of an MMIO page for direct access.
 *
 * (This is a special optimization used by the APIC in the VT-x case.)
 *
 * @returns VBox status code.
 *
 * @param   pVM             Pointer to the VM.
 * @param   pVCpu           Pointer to the VMCPU.
 * @param   GCPhys          The address of the MMIO page to be changed.
 * @param   HCPhys          The address of the host physical page.
 * @param   fPageFlags      Page flags to set. Must be (X86_PTE_RW | X86_PTE_P)
 *                          for the time being.
 */
VMMDECL(int) IOMMMIOMapMMIOHCPage(PVM pVM, PVMCPU pVCpu, RTGCPHYS GCPhys, RTHCPHYS HCPhys, uint64_t fPageFlags)
{
    /* Currently only called from VT-x code during a page fault. */
    Log(("IOMMMIOMapMMIOHCPage %RGp -> %RGp flags=%RX64\n", GCPhys, HCPhys, fPageFlags));

    AssertReturn(fPageFlags == (X86_PTE_RW | X86_PTE_P), VERR_INVALID_PARAMETER);
    Assert(HMIsEnabled(pVM));

    /*
     * Lookup the context range node the page belongs to.
     */
#  ifdef VBOX_STRICT
    /* Can't lock IOM here due to potential deadlocks in the VGA device; not safe to access. */
    PIOMMMIORANGE pRange = iomMMIOGetRangeUnsafe(pVM, pVCpu, GCPhys);
    AssertMsgReturn(pRange,
            ("Handlers and page tables are out of sync or something! GCPhys=%RGp\n", GCPhys), VERR_IOM_MMIO_RANGE_NOT_FOUND);
    Assert((pRange->GCPhys       & PAGE_OFFSET_MASK) == 0);
    Assert((pRange->Core.KeyLast & PAGE_OFFSET_MASK) == PAGE_OFFSET_MASK);
#  endif

    /*
     * Do the aliasing; page align the addresses since PGM is picky.
     */
    GCPhys &= ~(RTGCPHYS)PAGE_OFFSET_MASK;
    HCPhys &= ~(RTHCPHYS)PAGE_OFFSET_MASK;

    int rc = PGMHandlerPhysicalPageAliasHC(pVM, GCPhys, GCPhys, HCPhys);
    AssertRCReturn(rc, rc);

    /*
     * Modify the shadow page table. Since it's an MMIO page it won't be present and we
     * can simply prefetch it.
     *
     * Note: This is a NOP in the EPT case; we'll just let it fault again to resync the page.
     */
    rc = PGMPrefetchPage(pVCpu, (RTGCPTR)GCPhys);
    Assert(rc == VINF_SUCCESS || rc == VERR_PAGE_NOT_PRESENT || rc == VERR_PAGE_TABLE_NOT_PRESENT);
    return VINF_SUCCESS;
}
# endif /* !IEM_VERIFICATION_MODE_FULL */


/**
 * Reset a previously modified MMIO region; restore the access flags.
 *
 * @returns VBox status code.
 *
 * @param   pVM             The virtual machine.
 * @param   GCPhys          Physical address that's part of the MMIO region to be reset.
 */
VMMDECL(int) IOMMMIOResetRegion(PVM pVM, RTGCPHYS GCPhys)
{
    Log(("IOMMMIOResetRegion %RGp\n", GCPhys));

    PVMCPU pVCpu = VMMGetCpu(pVM);

    /* This currently only works in real mode, protected mode without paging or with nested paging. */
    if (    !HMIsEnabled(pVM)       /* useless without VT-x/AMD-V */
        ||  (   CPUMIsGuestInPagedProtectedMode(pVCpu)
             && !HMIsNestedPagingActive(pVM)))
        return VINF_SUCCESS;    /* ignore */

    /*
     * Lookup the context range node the page belongs to.
     */
# ifdef VBOX_STRICT
    /* Can't lock IOM here due to potential deadlocks in the VGA device; not safe to access. */
    PIOMMMIORANGE pRange = iomMMIOGetRangeUnsafe(pVM, pVCpu, GCPhys);
    AssertMsgReturn(pRange,
            ("Handlers and page tables are out of sync or something! GCPhys=%RGp\n", GCPhys), VERR_IOM_MMIO_RANGE_NOT_FOUND);
    Assert((pRange->GCPhys       & PAGE_OFFSET_MASK) == 0);
    Assert((pRange->Core.KeyLast & PAGE_OFFSET_MASK) == PAGE_OFFSET_MASK);
# endif

    /*
     * Call PGM to do the job work.
     *
     * After the call, all the pages should be non-present... unless there is
     * a page pool flush pending (unlikely).
     */
    int rc = PGMHandlerPhysicalReset(pVM, GCPhys);
    AssertRC(rc);

# ifdef VBOX_STRICT
    if (!VMCPU_FF_IS_SET(pVCpu, VMCPU_FF_PGM_SYNC_CR3))
    {
        uint32_t cb = pRange->cb;
        GCPhys = pRange->GCPhys;
        while (cb)
        {
            uint64_t fFlags;
            RTHCPHYS HCPhys;
            rc = PGMShwGetPage(pVCpu, (RTGCPTR)GCPhys, &fFlags, &HCPhys);
            Assert(rc == VERR_PAGE_NOT_PRESENT || rc == VERR_PAGE_TABLE_NOT_PRESENT);
            cb     -= PAGE_SIZE;
            GCPhys += PAGE_SIZE;
        }
    }
# endif
    return rc;
}

#endif /* !IN_RC */

