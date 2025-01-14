/* $Id$ */
/** @file
 * IOM - Input / Output Monitor - Any Context.
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
#include <VBox/vmm/mm.h>
#if defined(IEM_VERIFICATION_MODE) && defined(IN_RING3)
# include <VBox/vmm/iem.h>
#endif
#include <VBox/param.h>
#include "IOMInternal.h"
#include <VBox/vmm/vm.h>
#include <VBox/vmm/vmm.h>
#include <VBox/vmm/selm.h>
#include <VBox/vmm/trpm.h>
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/pgm.h>
#include <VBox/vmm/cpum.h>
#include <VBox/err.h>
#include <VBox/log.h>
#include <iprt/assert.h>
#include "IOMInline.h"


/**
 * Check if this VCPU currently owns the IOM lock exclusively.
 *
 * @returns bool owner/not owner
 * @param   pVM         Pointer to the VM.
 */
VMMDECL(bool) IOMIsLockWriteOwner(PVM pVM)
{
#ifdef IOM_WITH_CRIT_SECT_RW
    return PDMCritSectRwIsInitialized(&pVM->iom.s.CritSect)
        && PDMCritSectRwIsWriteOwner(&pVM->iom.s.CritSect);
#else
    return PDMCritSectIsOwner(&pVM->iom.s.CritSect);
#endif
}


//#undef LOG_GROUP
//#define LOG_GROUP LOG_GROUP_IOM_IOPORT

/**
 * Reads an I/O port register.
 *
 * @returns Strict VBox status code. Informational status codes other than the one documented
 *          here are to be treated as internal failure. Use IOM_SUCCESS() to check for success.
 * @retval  VINF_SUCCESS                Success.
 * @retval  VINF_EM_FIRST-VINF_EM_LAST  Success with some exceptions (see IOM_SUCCESS()), the
 *                                      status code must be passed on to EM.
 * @retval  VINF_IOM_R3_IOPORT_READ     Defer the read to ring-3. (R0/RC only)
 *
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   Port        The port to read.
 * @param   pu32Value   Where to store the value read.
 * @param   cbValue     The size of the register to read in bytes. 1, 2 or 4 bytes.
 */
VMMDECL(VBOXSTRICTRC) IOMIOPortRead(PVM pVM, PVMCPU pVCpu, RTIOPORT Port, uint32_t *pu32Value, size_t cbValue)
{
/** @todo should initialize *pu32Value here because it can happen that some
 *        handle is buggy and doesn't handle all cases. */
    /* Take the IOM lock before performing any device I/O. */
    int rc2 = IOM_LOCK_SHARED(pVM);
#ifndef IN_RING3
    if (rc2 == VERR_SEM_BUSY)
        return VINF_IOM_R3_IOPORT_READ;
#endif
    AssertRC(rc2);
#if defined(IEM_VERIFICATION_MODE) && defined(IN_RING3)
    IEMNotifyIOPortRead(pVM, Port, cbValue);
#endif

#ifdef VBOX_WITH_STATISTICS
    /*
     * Get the statistics record.
     */
    PIOMIOPORTSTATS  pStats = pVCpu->iom.s.CTX_SUFF(pStatsLastRead);
    if (!pStats || pStats->Core.Key != Port)
    {
        pStats = (PIOMIOPORTSTATS)RTAvloIOPortGet(&pVM->iom.s.CTX_SUFF(pTrees)->IOPortStatTree, Port);
        if (pStats)
            pVCpu->iom.s.CTX_SUFF(pStatsLastRead) = pStats;
    }
#endif

    /*
     * Get handler for current context.
     */
    CTX_SUFF(PIOMIOPORTRANGE) pRange = pVCpu->iom.s.CTX_SUFF(pRangeLastRead);
    if (    !pRange
        ||   (unsigned)Port - (unsigned)pRange->Port >= (unsigned)pRange->cPorts)
    {
        pRange = iomIOPortGetRange(pVM, Port);
        if (pRange)
            pVCpu->iom.s.CTX_SUFF(pRangeLastRead) = pRange;
    }
    MMHYPER_RC_ASSERT_RCPTR(pVM, pRange);
    if (pRange)
    {
        /*
         * Found a range, get the data in case we leave the IOM lock.
         */
        PFNIOMIOPORTIN  pfnInCallback = pRange->pfnInCallback;
#ifndef IN_RING3
        if (pfnInCallback)
        { /* likely */ }
        else
        {
            STAM_STATS({ if (pStats) STAM_COUNTER_INC(&pStats->InRZToR3); });
            IOM_UNLOCK_SHARED(pVM);
            return VINF_IOM_R3_IOPORT_READ;
        }
#endif
        void           *pvUser    = pRange->pvUser;
        PPDMDEVINS      pDevIns   = pRange->pDevIns;
        IOM_UNLOCK_SHARED(pVM);

        /*
         * Call the device.
         */
        VBOXSTRICTRC rcStrict = PDMCritSectEnter(pDevIns->CTX_SUFF(pCritSectRo), VINF_IOM_R3_IOPORT_READ);
        if (rcStrict == VINF_SUCCESS)
        { /* likely */ }
        else
        {
            STAM_STATS({ if (pStats) STAM_COUNTER_INC(&pStats->InRZToR3); });
            return rcStrict;
        }
#ifdef VBOX_WITH_STATISTICS
        if (pStats)
        {
            STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfIn), a);
            rcStrict = pfnInCallback(pDevIns, pvUser, Port, pu32Value, (unsigned)cbValue);
            STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfIn), a);
        }
        else
#endif
            rcStrict = pfnInCallback(pDevIns, pvUser, Port, pu32Value, (unsigned)cbValue);
        PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));

#ifdef VBOX_WITH_STATISTICS
        if (rcStrict == VINF_SUCCESS && pStats)
            STAM_COUNTER_INC(&pStats->CTX_SUFF_Z(In));
# ifndef IN_RING3
        else if (rcStrict == VINF_IOM_R3_IOPORT_READ && pStats)
            STAM_COUNTER_INC(&pStats->InRZToR3);
# endif
#endif
        if (rcStrict == VERR_IOM_IOPORT_UNUSED)
        {
            /* make return value */
            rcStrict = VINF_SUCCESS;
            switch (cbValue)
            {
                case 1: *(uint8_t  *)pu32Value = 0xff; break;
                case 2: *(uint16_t *)pu32Value = 0xffff; break;
                case 4: *(uint32_t *)pu32Value = UINT32_C(0xffffffff); break;
                default:
                    AssertMsgFailed(("Invalid I/O port size %d. Port=%d\n", cbValue, Port));
                    return VERR_IOM_INVALID_IOPORT_SIZE;
            }
        }
        Log3(("IOMIOPortRead: Port=%RTiop *pu32=%08RX32 cb=%d rc=%Rrc\n", Port, *pu32Value, cbValue, VBOXSTRICTRC_VAL(rcStrict)));
        return rcStrict;
    }

#ifndef IN_RING3
    /*
     * Handler in ring-3?
     */
    PIOMIOPORTRANGER3 pRangeR3 = iomIOPortGetRangeR3(pVM, Port);
    if (pRangeR3)
    {
# ifdef VBOX_WITH_STATISTICS
        if (pStats)
            STAM_COUNTER_INC(&pStats->InRZToR3);
# endif
        IOM_UNLOCK_SHARED(pVM);
        return VINF_IOM_R3_IOPORT_READ;
    }
#endif

    /*
     * Ok, no handler for this port.
     */
#ifdef VBOX_WITH_STATISTICS
    if (pStats)
        STAM_COUNTER_INC(&pStats->CTX_SUFF_Z(In));
#endif

    /* make return value */
    switch (cbValue)
    {
        case 1: *(uint8_t  *)pu32Value = 0xff; break;
        case 2: *(uint16_t *)pu32Value = 0xffff; break;
        case 4: *(uint32_t *)pu32Value = UINT32_C(0xffffffff); break;
        default:
            AssertMsgFailed(("Invalid I/O port size %d. Port=%d\n", cbValue, Port));
            IOM_UNLOCK_SHARED(pVM);
            return VERR_IOM_INVALID_IOPORT_SIZE;
    }
    Log3(("IOMIOPortRead: Port=%RTiop *pu32=%08RX32 cb=%d rc=VINF_SUCCESS\n", Port, *pu32Value, cbValue));
    IOM_UNLOCK_SHARED(pVM);
    return VINF_SUCCESS;
}


/**
 * Reads the string buffer of an I/O port register.
 *
 * @returns Strict VBox status code. Informational status codes other than the one documented
 *          here are to be treated as internal failure. Use IOM_SUCCESS() to check for success.
 * @retval  VINF_SUCCESS                Success or no string I/O callback in
 *                                      this context.
 * @retval  VINF_EM_FIRST-VINF_EM_LAST  Success with some exceptions (see IOM_SUCCESS()), the
 *                                      status code must be passed on to EM.
 * @retval  VINF_IOM_R3_IOPORT_READ     Defer the read to ring-3. (R0/RC only)
 *
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   Port        The port to read.
 * @param   pvDst       Pointer to the destination buffer.
 * @param   pcTransfers Pointer to the number of transfer units to read, on return remaining transfer units.
 * @param   cb          Size of the transfer unit (1, 2 or 4 bytes).
 */
VMM_INT_DECL(VBOXSTRICTRC) IOMIOPortReadString(PVM pVM, PVMCPU pVCpu, RTIOPORT uPort,
                                               void *pvDst, uint32_t *pcTransfers, unsigned cb)
{
    /* Take the IOM lock before performing any device I/O. */
    int rc2 = IOM_LOCK_SHARED(pVM);
#ifndef IN_RING3
    if (rc2 == VERR_SEM_BUSY)
        return VINF_IOM_R3_IOPORT_READ;
#endif
    AssertRC(rc2);
#if defined(IEM_VERIFICATION_MODE) && defined(IN_RING3)
    IEMNotifyIOPortReadString(pVM, uPort, pvDst, *pcTransfers, cb);
#endif

    const uint32_t cRequestedTransfers = *pcTransfers;
    Assert(cRequestedTransfers > 0);

#ifdef VBOX_WITH_STATISTICS
    /*
     * Get the statistics record.
     */
    PIOMIOPORTSTATS pStats = pVCpu->iom.s.CTX_SUFF(pStatsLastRead);
    if (!pStats || pStats->Core.Key != uPort)
    {
        pStats = (PIOMIOPORTSTATS)RTAvloIOPortGet(&pVM->iom.s.CTX_SUFF(pTrees)->IOPortStatTree, uPort);
        if (pStats)
            pVCpu->iom.s.CTX_SUFF(pStatsLastRead) = pStats;
    }
#endif

    /*
     * Get handler for current context.
     */
    CTX_SUFF(PIOMIOPORTRANGE) pRange = pVCpu->iom.s.CTX_SUFF(pRangeLastRead);
    if (    !pRange
        ||   (unsigned)uPort - (unsigned)pRange->Port >= (unsigned)pRange->cPorts)
    {
        pRange = iomIOPortGetRange(pVM, uPort);
        if (pRange)
            pVCpu->iom.s.CTX_SUFF(pRangeLastRead) = pRange;
    }
    MMHYPER_RC_ASSERT_RCPTR(pVM, pRange);
    if (pRange)
    {
        /*
         * Found a range.
         */
        PFNIOMIOPORTINSTRING pfnInStrCallback = pRange->pfnInStrCallback;
        PFNIOMIOPORTIN       pfnInCallback    = pRange->pfnInCallback;
#ifndef IN_RING3
        if (pfnInStrCallback || pfnInCallback)
        { /* likely */ }
        else
        {
            STAM_STATS({ if (pStats)
                    STAM_COUNTER_INC(&pStats->InRZToR3); });
            IOM_UNLOCK_SHARED(pVM);
            return VINF_SUCCESS;
        }
#endif
        void           *pvUser    = pRange->pvUser;
        PPDMDEVINS      pDevIns   = pRange->pDevIns;
        IOM_UNLOCK_SHARED(pVM);

        /*
         * Call the device.
         */
        VBOXSTRICTRC rcStrict = PDMCritSectEnter(pDevIns->CTX_SUFF(pCritSectRo), VINF_IOM_R3_IOPORT_READ);
        if (rcStrict == VINF_SUCCESS)
        { /* likely */ }
        else
        {
            STAM_STATS({ if (pStats) STAM_COUNTER_INC(&pStats->InRZToR3); });
            return rcStrict;
        }

        /*
         * First using the string I/O callback.
         */
        if (pfnInStrCallback)
        {
#ifdef VBOX_WITH_STATISTICS
            if (pStats)
            {
                STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfIn), a);
                rcStrict = pfnInStrCallback(pDevIns, pvUser, uPort, (uint8_t *)pvDst, pcTransfers, cb);
                STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfIn), a);
            }
            else
#endif
                rcStrict = pfnInStrCallback(pDevIns, pvUser, uPort, (uint8_t *)pvDst, pcTransfers, cb);
        }

        /*
         * Then doing the single I/O fallback.
         */
        if (   *pcTransfers > 0
            && rcStrict == VINF_SUCCESS)
        {
            pvDst = (uint8_t *)pvDst + (cRequestedTransfers - *pcTransfers) * cb;
            do
            {
                uint32_t u32Value = 0;
#ifdef VBOX_WITH_STATISTICS
                if (pStats)
                {
                    STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfIn), a);
                    rcStrict = pfnInCallback(pDevIns, pvUser, uPort, &u32Value, cb);
                    STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfIn), a);
                }
                else
#endif
                    rcStrict = pfnInCallback(pDevIns, pvUser, uPort, &u32Value, cb);
                if (rcStrict == VERR_IOM_IOPORT_UNUSED)
                {
                    u32Value = UINT32_MAX;
                    rcStrict = VINF_SUCCESS;
                }
                if (IOM_SUCCESS(rcStrict))
                {
                    switch (cb)
                    {
                        case 4: *(uint32_t *)pvDst =           u32Value; pvDst = (uint8_t *)pvDst + 4; break;
                        case 2: *(uint16_t *)pvDst = (uint16_t)u32Value; pvDst = (uint8_t *)pvDst + 2; break;
                        case 1: *(uint8_t  *)pvDst = (uint8_t )u32Value; pvDst = (uint8_t *)pvDst + 1; break;
                        default: AssertFailed();
                    }
                    *pcTransfers -= 1;
                }
            } while (   *pcTransfers > 0
                     && rcStrict == VINF_SUCCESS);
        }
        PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));

#ifdef VBOX_WITH_STATISTICS
        if (rcStrict == VINF_SUCCESS && pStats)
            STAM_COUNTER_INC(&pStats->CTX_SUFF_Z(In));
# ifndef IN_RING3
        else if (rcStrict == VINF_IOM_R3_IOPORT_READ && pStats)
            STAM_COUNTER_INC(&pStats->InRZToR3);
# endif
#endif
        Log3(("IOMIOPortReadStr: uPort=%RTiop pvDst=%p pcTransfer=%p:{%#x->%#x} cb=%d rc=%Rrc\n",
              uPort, pvDst, pcTransfers, cRequestedTransfers, *pcTransfers, cb, VBOXSTRICTRC_VAL(rcStrict)));
        return rcStrict;
    }

#ifndef IN_RING3
    /*
     * Handler in ring-3?
     */
    PIOMIOPORTRANGER3 pRangeR3 = iomIOPortGetRangeR3(pVM, uPort);
    if (pRangeR3)
    {
# ifdef VBOX_WITH_STATISTICS
        if (pStats)
            STAM_COUNTER_INC(&pStats->InRZToR3);
# endif
        IOM_UNLOCK_SHARED(pVM);
        return VINF_IOM_R3_IOPORT_READ;
    }
#endif

    /*
     * Ok, no handler for this port.
     */
#ifdef VBOX_WITH_STATISTICS
    if (pStats)
        STAM_COUNTER_INC(&pStats->CTX_SUFF_Z(In));
#endif

    Log3(("IOMIOPortReadStr: uPort=%RTiop pvDst=%p pcTransfer=%p:{%#x->%#x} cb=%d rc=VINF_SUCCESS\n",
          uPort, pvDst, pcTransfers, cRequestedTransfers, *pcTransfers, cb));
    IOM_UNLOCK_SHARED(pVM);
    return VINF_SUCCESS;
}


/**
 * Writes to an I/O port register.
 *
 * @returns Strict VBox status code. Informational status codes other than the one documented
 *          here are to be treated as internal failure. Use IOM_SUCCESS() to check for success.
 * @retval  VINF_SUCCESS                Success.
 * @retval  VINF_EM_FIRST-VINF_EM_LAST  Success with some exceptions (see IOM_SUCCESS()), the
 *                                      status code must be passed on to EM.
 * @retval  VINF_IOM_R3_IOPORT_WRITE    Defer the write to ring-3. (R0/RC only)
 *
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   Port        The port to write to.
 * @param   u32Value    The value to write.
 * @param   cbValue     The size of the register to read in bytes. 1, 2 or 4 bytes.
 */
VMMDECL(VBOXSTRICTRC) IOMIOPortWrite(PVM pVM, PVMCPU pVCpu, RTIOPORT Port, uint32_t u32Value, size_t cbValue)
{
    /* Take the IOM lock before performing any device I/O. */
    int rc2 = IOM_LOCK_SHARED(pVM);
#ifndef IN_RING3
    if (rc2 == VERR_SEM_BUSY)
        return VINF_IOM_R3_IOPORT_WRITE;
#endif
    AssertRC(rc2);
#if defined(IEM_VERIFICATION_MODE) && defined(IN_RING3)
    IEMNotifyIOPortWrite(pVM, Port, u32Value, cbValue);
#endif

/** @todo bird: When I get time, I'll remove the RC/R0 trees and link the RC/R0
 *        entries to the ring-3 node. */
#ifdef VBOX_WITH_STATISTICS
    /*
     * Find the statistics record.
     */
    PIOMIOPORTSTATS pStats = pVCpu->iom.s.CTX_SUFF(pStatsLastWrite);
    if (!pStats || pStats->Core.Key != Port)
    {
        pStats = (PIOMIOPORTSTATS)RTAvloIOPortGet(&pVM->iom.s.CTX_SUFF(pTrees)->IOPortStatTree, Port);
        if (pStats)
            pVCpu->iom.s.CTX_SUFF(pStatsLastWrite) = pStats;
    }
#endif

    /*
     * Get handler for current context.
     */
    CTX_SUFF(PIOMIOPORTRANGE) pRange = pVCpu->iom.s.CTX_SUFF(pRangeLastWrite);
    if (    !pRange
        ||   (unsigned)Port - (unsigned)pRange->Port >= (unsigned)pRange->cPorts)
    {
        pRange = iomIOPortGetRange(pVM, Port);
        if (pRange)
            pVCpu->iom.s.CTX_SUFF(pRangeLastWrite) = pRange;
    }
    MMHYPER_RC_ASSERT_RCPTR(pVM, pRange);
    if (pRange)
    {
        /*
         * Found a range.
         */
        PFNIOMIOPORTOUT pfnOutCallback = pRange->pfnOutCallback;
#ifndef IN_RING3
        if (pfnOutCallback)
        { /* likely */ }
        else
        {
            STAM_STATS({ if (pStats) STAM_COUNTER_INC(&pStats->OutRZToR3); });
            IOM_UNLOCK_SHARED(pVM);
            return VINF_IOM_R3_IOPORT_WRITE;
        }
#endif
        void           *pvUser    = pRange->pvUser;
        PPDMDEVINS      pDevIns   = pRange->pDevIns;
        IOM_UNLOCK_SHARED(pVM);

        /*
         * Call the device.
         */
        VBOXSTRICTRC rcStrict = PDMCritSectEnter(pDevIns->CTX_SUFF(pCritSectRo), VINF_IOM_R3_IOPORT_WRITE);
        if (rcStrict == VINF_SUCCESS)
        { /* likely */ }
        else
        {
            STAM_STATS({ if (pStats) STAM_COUNTER_INC(&pStats->OutRZToR3); });
            return rcStrict;
        }
#ifdef VBOX_WITH_STATISTICS
        if (pStats)
        {
            STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfOut), a);
            rcStrict = pfnOutCallback(pDevIns, pvUser, Port, u32Value, (unsigned)cbValue);
            STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfOut), a);
        }
        else
#endif
            rcStrict = pfnOutCallback(pDevIns, pvUser, Port, u32Value, (unsigned)cbValue);
        PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));

#ifdef VBOX_WITH_STATISTICS
        if (rcStrict == VINF_SUCCESS && pStats)
            STAM_COUNTER_INC(&pStats->CTX_SUFF_Z(Out));
# ifndef IN_RING3
        else if (rcStrict == VINF_IOM_R3_IOPORT_WRITE && pStats)
            STAM_COUNTER_INC(&pStats->OutRZToR3);
# endif
#endif
        Log3(("IOMIOPortWrite: Port=%RTiop u32=%08RX32 cb=%d rc=%Rrc\n", Port, u32Value, cbValue, VBOXSTRICTRC_VAL(rcStrict)));
        return rcStrict;
    }

#ifndef IN_RING3
    /*
     * Handler in ring-3?
     */
    PIOMIOPORTRANGER3 pRangeR3 = iomIOPortGetRangeR3(pVM, Port);
    if (pRangeR3)
    {
# ifdef VBOX_WITH_STATISTICS
        if (pStats)
            STAM_COUNTER_INC(&pStats->OutRZToR3);
# endif
        IOM_UNLOCK_SHARED(pVM);
        return VINF_IOM_R3_IOPORT_WRITE;
    }
#endif

    /*
     * Ok, no handler for that port.
     */
#ifdef VBOX_WITH_STATISTICS
    /* statistics. */
    if (pStats)
        STAM_COUNTER_INC(&pStats->CTX_SUFF_Z(Out));
#endif
    Log3(("IOMIOPortWrite: Port=%RTiop u32=%08RX32 cb=%d nop\n", Port, u32Value, cbValue));
    IOM_UNLOCK_SHARED(pVM);
    return VINF_SUCCESS;
}


/**
 * Writes the string buffer of an I/O port register.
 *
 * @returns Strict VBox status code. Informational status codes other than the one documented
 *          here are to be treated as internal failure. Use IOM_SUCCESS() to check for success.
 * @retval  VINF_SUCCESS                Success or no string I/O callback in
 *                                      this context.
 * @retval  VINF_EM_FIRST-VINF_EM_LAST  Success with some exceptions (see IOM_SUCCESS()), the
 *                                      status code must be passed on to EM.
 * @retval  VINF_IOM_R3_IOPORT_WRITE    Defer the write to ring-3. (R0/RC only)
 *
 * @param   pVM         Pointer to the VM.
 * @param   pVCpu       Pointer to the virtual CPU structure of the caller.
 * @param   uPort       The port to write to.
 * @param   pvSrc       The guest page to read from.
 * @param   pcTransfers Pointer to the number of transfer units to write, on
 *                      return remaining transfer units.
 * @param   cb          Size of the transfer unit (1, 2 or 4 bytes).
 */
VMM_INT_DECL(VBOXSTRICTRC) IOMIOPortWriteString(PVM pVM, PVMCPU pVCpu, RTIOPORT uPort, void const *pvSrc,
                                                uint32_t *pcTransfers, unsigned cb)
{
    Assert(cb == 1 || cb == 2 || cb == 4);

    /* Take the IOM lock before performing any device I/O. */
    int rc2 = IOM_LOCK_SHARED(pVM);
#ifndef IN_RING3
    if (rc2 == VERR_SEM_BUSY)
        return VINF_IOM_R3_IOPORT_WRITE;
#endif
    AssertRC(rc2);
#if defined(IEM_VERIFICATION_MODE) && defined(IN_RING3)
    IEMNotifyIOPortWriteString(pVM, uPort, pvSrc, *pcTransfers, cb);
#endif

    const uint32_t cRequestedTransfers = *pcTransfers;
    Assert(cRequestedTransfers > 0);

#ifdef VBOX_WITH_STATISTICS
    /*
     * Get the statistics record.
     */
    PIOMIOPORTSTATS     pStats = pVCpu->iom.s.CTX_SUFF(pStatsLastWrite);
    if (!pStats || pStats->Core.Key != uPort)
    {
        pStats = (PIOMIOPORTSTATS)RTAvloIOPortGet(&pVM->iom.s.CTX_SUFF(pTrees)->IOPortStatTree, uPort);
        if (pStats)
            pVCpu->iom.s.CTX_SUFF(pStatsLastWrite) = pStats;
    }
#endif

    /*
     * Get handler for current context.
     */
    CTX_SUFF(PIOMIOPORTRANGE) pRange = pVCpu->iom.s.CTX_SUFF(pRangeLastWrite);
    if (    !pRange
        ||   (unsigned)uPort - (unsigned)pRange->Port >= (unsigned)pRange->cPorts)
    {
        pRange = iomIOPortGetRange(pVM, uPort);
        if (pRange)
            pVCpu->iom.s.CTX_SUFF(pRangeLastWrite) = pRange;
    }
    MMHYPER_RC_ASSERT_RCPTR(pVM, pRange);
    if (pRange)
    {
        /*
         * Found a range.
         */
        PFNIOMIOPORTOUTSTRING   pfnOutStrCallback = pRange->pfnOutStrCallback;
        PFNIOMIOPORTOUT         pfnOutCallback    = pRange->pfnOutCallback;
#ifndef IN_RING3
        if (pfnOutStrCallback || pfnOutCallback)
        { /* likely */ }
        else
        {
            STAM_STATS({ if (pStats) STAM_COUNTER_INC(&pStats->OutRZToR3); });
            IOM_UNLOCK_SHARED(pVM);
            return VINF_SUCCESS;
        }
#endif
        void           *pvUser    = pRange->pvUser;
        PPDMDEVINS      pDevIns   = pRange->pDevIns;
        IOM_UNLOCK_SHARED(pVM);

        /*
         * Call the device.
         */
        VBOXSTRICTRC rcStrict = PDMCritSectEnter(pDevIns->CTX_SUFF(pCritSectRo), VINF_IOM_R3_IOPORT_WRITE);
        if (rcStrict == VINF_SUCCESS)
        { /* likely */ }
        else
        {
            STAM_STATS({ if (pStats) STAM_COUNTER_INC(&pStats->OutRZToR3); });
            return rcStrict;
        }

        /*
         * First using string I/O if possible.
         */
        if (pfnOutStrCallback)
        {
#ifdef VBOX_WITH_STATISTICS
            if (pStats)
            {
                STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfOut), a);
                rcStrict = pfnOutStrCallback(pDevIns, pvUser, uPort, (uint8_t const *)pvSrc, pcTransfers, cb);
                STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfOut), a);
            }
            else
#endif
                rcStrict = pfnOutStrCallback(pDevIns, pvUser, uPort, (uint8_t const *)pvSrc, pcTransfers, cb);
        }

        /*
         * Then doing the single I/O fallback.
         */
        if (   *pcTransfers > 0
            && rcStrict == VINF_SUCCESS)
        {
            pvSrc = (uint8_t *)pvSrc + (cRequestedTransfers - *pcTransfers) * cb;
            do
            {
                uint32_t u32Value;
                switch (cb)
                {
                    case 4: u32Value = *(uint32_t *)pvSrc; pvSrc = (uint8_t const *)pvSrc + 4; break;
                    case 2: u32Value = *(uint16_t *)pvSrc; pvSrc = (uint8_t const *)pvSrc + 2; break;
                    case 1: u32Value = *(uint8_t  *)pvSrc; pvSrc = (uint8_t const *)pvSrc + 1; break;
                    default: AssertFailed(); u32Value = UINT32_MAX;
                }
#ifdef VBOX_WITH_STATISTICS
                if (pStats)
                {
                    STAM_PROFILE_START(&pStats->CTX_SUFF_Z(ProfOut), a);
                    rcStrict = pfnOutCallback(pDevIns, pvUser, uPort, u32Value, cb);
                    STAM_PROFILE_STOP(&pStats->CTX_SUFF_Z(ProfOut), a);
                }
                else
#endif
                    rcStrict = pfnOutCallback(pDevIns, pvUser, uPort, u32Value, cb);
                if (IOM_SUCCESS(rcStrict))
                    *pcTransfers -= 1;
            } while (   *pcTransfers > 0
                     && rcStrict == VINF_SUCCESS);
        }

        PDMCritSectLeave(pDevIns->CTX_SUFF(pCritSectRo));

#ifdef VBOX_WITH_STATISTICS
        if (rcStrict == VINF_SUCCESS && pStats)
            STAM_COUNTER_INC(&pStats->CTX_SUFF_Z(Out));
# ifndef IN_RING3
        else if (rcStrict == VINF_IOM_R3_IOPORT_WRITE && pStats)
            STAM_COUNTER_INC(&pStats->OutRZToR3);
# endif
#endif
        Log3(("IOMIOPortWriteStr: uPort=%RTiop pvSrc=%p pcTransfer=%p:{%#x->%#x} cb=%d rcStrict=%Rrc\n",
              uPort, pvSrc, pcTransfers, cRequestedTransfers, *pcTransfers, cb, VBOXSTRICTRC_VAL(rcStrict)));
        return rcStrict;
    }

#ifndef IN_RING3
    /*
     * Handler in ring-3?
     */
    PIOMIOPORTRANGER3 pRangeR3 = iomIOPortGetRangeR3(pVM, uPort);
    if (pRangeR3)
    {
# ifdef VBOX_WITH_STATISTICS
        if (pStats)
            STAM_COUNTER_INC(&pStats->OutRZToR3);
# endif
        IOM_UNLOCK_SHARED(pVM);
        return VINF_IOM_R3_IOPORT_WRITE;
    }
#endif

    /*
     * Ok, no handler for this port.
     */
#ifdef VBOX_WITH_STATISTICS
    if (pStats)
        STAM_COUNTER_INC(&pStats->CTX_SUFF_Z(Out));
#endif

    Log3(("IOMIOPortWriteStr: uPort=%RTiop pvSrc=%p pcTransfer=%p:{%#x->%#x} cb=%d rc=VINF_SUCCESS\n",
          uPort, pvSrc, pcTransfers, cRequestedTransfers, *pcTransfers, cb));
    IOM_UNLOCK_SHARED(pVM);
    return VINF_SUCCESS;
}


/**
 * Checks that the operation is allowed according to the IOPL
 * level and I/O bitmap.
 *
 * @returns Strict VBox status code. Informational status codes other than the one documented
 *          here are to be treated as internal failure.
 * @retval  VINF_SUCCESS                Success.
 * @retval  VINF_EM_RAW_GUEST_TRAP      The exception was left pending. (TRPMRaiseXcptErr)
 * @retval  VINF_TRPM_XCPT_DISPATCHED   The exception was raised and dispatched for raw-mode execution. (TRPMRaiseXcptErr)
 * @retval  VINF_EM_RESCHEDULE_REM      The exception was dispatched and cannot be executed in raw-mode. (TRPMRaiseXcptErr)
 *
 * @param   pVM         Pointer to the VM.
 * @param   pCtxCore    Pointer to register frame.
 * @param   Port        The I/O port number.
 * @param   cb          The access size.
 */
VMMDECL(VBOXSTRICTRC) IOMInterpretCheckPortIOAccess(PVM pVM, PCPUMCTXCORE pCtxCore, RTIOPORT Port, unsigned cb)
{
    PVMCPU pVCpu = VMMGetCpu(pVM);

    /*
     * If this isn't ring-0, we have to check for I/O privileges.
     */
    uint32_t efl = CPUMRawGetEFlags(pVCpu);
    uint32_t cpl = CPUMGetGuestCPL(pVCpu);

    if (    (    cpl > 0
             &&  X86_EFL_GET_IOPL(efl) < cpl)
        ||  pCtxCore->eflags.Bits.u1VM      /* IOPL is ignored in V86 mode; always check TSS bitmap */
       )
    {
        /*
         * Get TSS location and check if there can be a I/O bitmap.
         */
        RTGCUINTPTR GCPtrTss;
        RTGCUINTPTR cbTss;
        bool        fCanHaveIOBitmap;
        int rc2 = SELMGetTSSInfo(pVM, pVCpu, &GCPtrTss, &cbTss, &fCanHaveIOBitmap);
        if (RT_FAILURE(rc2))
        {
            Log(("iomInterpretCheckPortIOAccess: Port=%RTiop cb=%d %Rrc -> #GP(0)\n", Port, cb, rc2));
            return TRPMRaiseXcptErr(pVCpu, pCtxCore, X86_XCPT_GP, 0);
        }

        if (    !fCanHaveIOBitmap
            ||  cbTss <= sizeof(VBOXTSS)) /** @todo r=bird: Should this really include the interrupt redirection bitmap? */
        {
            Log(("iomInterpretCheckPortIOAccess: Port=%RTiop cb=%d cbTss=%#x fCanHaveIOBitmap=%RTbool -> #GP(0)\n",
                 Port, cb, cbTss, fCanHaveIOBitmap));
            return TRPMRaiseXcptErr(pVCpu, pCtxCore, X86_XCPT_GP, 0);
        }

        /*
         * Fetch the I/O bitmap offset.
         */
        uint16_t offIOPB;
        VBOXSTRICTRC rcStrict = PGMPhysInterpretedRead(pVCpu, pCtxCore, &offIOPB, GCPtrTss + RT_OFFSETOF(VBOXTSS, offIoBitmap), sizeof(offIOPB));
        if (rcStrict != VINF_SUCCESS)
        {
            Log(("iomInterpretCheckPortIOAccess: Port=%RTiop cb=%d GCPtrTss=%RGv %Rrc\n",
                 Port, cb, GCPtrTss, VBOXSTRICTRC_VAL(rcStrict)));
            return rcStrict;
        }

        /*
         * Check the limit and read the two bitmap bytes.
         */
        uint32_t offTss = offIOPB + (Port >> 3);
        if (offTss + 1 >= cbTss)
        {
            Log(("iomInterpretCheckPortIOAccess: Port=%RTiop cb=%d offTss=%#x cbTss=%#x -> #GP(0)\n",
                 Port, cb, offTss, cbTss));
            return TRPMRaiseXcptErr(pVCpu, pCtxCore, X86_XCPT_GP, 0);
        }
        uint16_t u16;
        rcStrict = PGMPhysInterpretedRead(pVCpu, pCtxCore, &u16, GCPtrTss + offTss, sizeof(u16));
        if (rcStrict != VINF_SUCCESS)
        {
            Log(("iomInterpretCheckPortIOAccess: Port=%RTiop cb=%d GCPtrTss=%RGv offTss=%#x -> %Rrc\n",
                 Port, cb, GCPtrTss, offTss, VBOXSTRICTRC_VAL(rcStrict)));
            return rcStrict;
        }

        /*
         * All the bits must be clear.
         */
        if ((u16 >> (Port & 7)) & ((1 << cb) - 1))
        {
            Log(("iomInterpretCheckPortIOAccess: Port=%RTiop cb=%d u16=%#x (offTss=%#x) -> #GP(0)\n",
                 Port, cb, u16, offTss));
            return TRPMRaiseXcptErr(pVCpu, pCtxCore, X86_XCPT_GP, 0);
        }
        LogFlow(("iomInterpretCheckPortIOAccess: Port=%RTiop cb=%d offTss=%#x cbTss=%#x u16=%#x -> OK\n",
                 Port, cb, u16, offTss, cbTss));
    }
    return VINF_SUCCESS;
}


/**
 * Fress an MMIO range after the reference counter has become zero.
 *
 * @param   pVM                 Pointer to the VM.
 * @param   pRange              The range to free.
 */
void iomMmioFreeRange(PVM pVM, PIOMMMIORANGE pRange)
{
    MMHyperFree(pVM, pRange);
}

