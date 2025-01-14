/* $Id$ */
/** @file
 * PDM - Pluggable Device and Driver Manager, Device Helpers.
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
#define LOG_GROUP LOG_GROUP_PDM_DEVICE
#include "PDMInternal.h"
#include <VBox/vmm/pdm.h>
#include <VBox/vmm/mm.h>
#include <VBox/vmm/hm.h>
#include <VBox/vmm/pgm.h>
#include <VBox/vmm/iom.h>
#ifdef VBOX_WITH_REM
# include <VBox/vmm/rem.h>
#endif
#include <VBox/vmm/dbgf.h>
#include <VBox/vmm/vmapi.h>
#include <VBox/vmm/vm.h>
#include <VBox/vmm/uvm.h>
#include <VBox/vmm/vmm.h>

#include <VBox/version.h>
#include <VBox/log.h>
#include <VBox/err.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/ctype.h>
#include <iprt/string.h>
#include <iprt/thread.h>

#include "dtrace/VBoxVMM.h"
#include "PDMInline.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** @def PDM_DEVHLP_DEADLOCK_DETECTION
 * Define this to enable the deadlock detection when accessing physical memory.
 */
#if /*defined(DEBUG_bird) ||*/ defined(DOXYGEN_RUNNING)
# define PDM_DEVHLP_DEADLOCK_DETECTION /**< @todo enable DevHlp deadlock detection! */
#endif



/**
 * Wrapper around PDMR3LdrGetSymbolRCLazy.
 */
DECLINLINE(int) pdmR3DevGetSymbolRCLazy(PPDMDEVINS pDevIns, const char *pszSymbol, PRTRCPTR ppvValue)
{
    PVM pVM = pDevIns->Internal.s.pVMR3;
    if (HMIsEnabled(pVM))
    {
        *ppvValue = NIL_RTRCPTR;
        return VINF_SUCCESS;
    }
    return PDMR3LdrGetSymbolRCLazy(pVM,
                                   pDevIns->Internal.s.pDevR3->pReg->szRCMod,
                                   pDevIns->Internal.s.pDevR3->pszRCSearchPath,
                                   pszSymbol, ppvValue);
}


/**
 * Wrapper around PDMR3LdrGetSymbolR0Lazy.
 */
DECLINLINE(int) pdmR3DevGetSymbolR0Lazy(PPDMDEVINS pDevIns, const char *pszSymbol, PRTR0PTR ppvValue)
{
    return PDMR3LdrGetSymbolR0Lazy(pDevIns->Internal.s.pVMR3,
                                   pDevIns->Internal.s.pDevR3->pReg->szR0Mod,
                                   pDevIns->Internal.s.pDevR3->pszR0SearchPath,
                                   pszSymbol, ppvValue);
}


/** @name R3 DevHlp
 * @{
 */


/** @interface_method_impl{PDMDEVHLPR3,pfnIOPortRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_IOPortRegister(PPDMDEVINS pDevIns, RTIOPORT Port, RTIOPORT cPorts, RTHCPTR pvUser, PFNIOMIOPORTOUT pfnOut, PFNIOMIOPORTIN pfnIn,
                                                    PFNIOMIOPORTOUTSTRING pfnOutStr, PFNIOMIOPORTINSTRING pfnInStr, const char *pszDesc)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_IOPortRegister: caller='%s'/%d: Port=%#x cPorts=%#x pvUser=%p pfnOut=%p pfnIn=%p pfnOutStr=%p pfnInStr=%p p32_tszDesc=%p:{%s}\n", pDevIns->pReg->szName, pDevIns->iInstance,
             Port, cPorts, pvUser, pfnOut, pfnIn, pfnOutStr, pfnInStr, pszDesc, pszDesc));
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);

#if 0 /** @todo needs a real string cache for this */
    if (pDevIns->iInstance > 0)
    {
         char *pszDesc2 = MMR3HeapAPrintf(pVM, MM_TAG_PDM_DEVICE_DESC, "%s [%u]", pszDesc, pDevIns->iInstance);
         if (pszDesc2)
             pszDesc = pszDesc2;
    }
#endif

    int rc = IOMR3IOPortRegisterR3(pDevIns->Internal.s.pVMR3, pDevIns, Port, cPorts, pvUser,
                                   pfnOut, pfnIn, pfnOutStr, pfnInStr, pszDesc);

    LogFlow(("pdmR3DevHlp_IOPortRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnIOPortRegisterRC} */
static DECLCALLBACK(int) pdmR3DevHlp_IOPortRegisterRC(PPDMDEVINS pDevIns, RTIOPORT Port, RTIOPORT cPorts, RTRCPTR pvUser,
                                                      const char *pszOut, const char *pszIn,
                                                      const char *pszOutStr, const char *pszInStr, const char *pszDesc)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_IOPortRegisterRC: caller='%s'/%d: Port=%#x cPorts=%#x pvUser=%p pszOut=%p:{%s} pszIn=%p:{%s} pszOutStr=%p:{%s} pszInStr=%p:{%s} pszDesc=%p:{%s}\n", pDevIns->pReg->szName, pDevIns->iInstance,
             Port, cPorts, pvUser, pszOut, pszOut, pszIn, pszIn, pszOutStr, pszOutStr, pszInStr, pszInStr, pszDesc, pszDesc));

    /*
     * Resolve the functions (one of the can be NULL).
     */
    int rc = VINF_SUCCESS;
    if (   pDevIns->pReg->szRCMod[0]
        && (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_RC)
        && !HMIsEnabled(pVM))
    {
        RTRCPTR RCPtrIn = NIL_RTRCPTR;
        if (pszIn)
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pszIn, &RCPtrIn);
            AssertMsgRC(rc, ("Failed to resolve %s.%s (pszIn)\n", pDevIns->pReg->szRCMod, pszIn));
        }
        RTRCPTR RCPtrOut = NIL_RTRCPTR;
        if (pszOut && RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pszOut, &RCPtrOut);
            AssertMsgRC(rc, ("Failed to resolve %s.%s (pszOut)\n", pDevIns->pReg->szRCMod, pszOut));
        }
        RTRCPTR RCPtrInStr = NIL_RTRCPTR;
        if (pszInStr && RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pszInStr, &RCPtrInStr);
            AssertMsgRC(rc, ("Failed to resolve %s.%s (pszInStr)\n", pDevIns->pReg->szRCMod, pszInStr));
        }
        RTRCPTR RCPtrOutStr = NIL_RTRCPTR;
        if (pszOutStr && RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pszOutStr, &RCPtrOutStr);
            AssertMsgRC(rc, ("Failed to resolve %s.%s (pszOutStr)\n", pDevIns->pReg->szRCMod, pszOutStr));
        }

        if (RT_SUCCESS(rc))
        {
#if 0 /** @todo needs a real string cache for this */
            if (pDevIns->iInstance > 0)
            {
                 char *pszDesc2 = MMR3HeapAPrintf(pVM, MM_TAG_PDM_DEVICE_DESC, "%s [%u]", pszDesc, pDevIns->iInstance);
                 if (pszDesc2)
                     pszDesc = pszDesc2;
            }
#endif

            rc = IOMR3IOPortRegisterRC(pVM, pDevIns, Port, cPorts, pvUser, RCPtrOut, RCPtrIn, RCPtrOutStr, RCPtrInStr, pszDesc);
        }
    }
    else if (!HMIsEnabled(pVM))
    {
        AssertMsgFailed(("No RC module for this driver!\n"));
        rc = VERR_INVALID_PARAMETER;
    }

    LogFlow(("pdmR3DevHlp_IOPortRegisterRC: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnIOPortRegisterR0} */
static DECLCALLBACK(int) pdmR3DevHlp_IOPortRegisterR0(PPDMDEVINS pDevIns, RTIOPORT Port, RTIOPORT cPorts, RTR0PTR pvUser,
                                                      const char *pszOut, const char *pszIn,
                                                      const char *pszOutStr, const char *pszInStr, const char *pszDesc)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_IOPortRegisterR0: caller='%s'/%d: Port=%#x cPorts=%#x pvUser=%p pszOut=%p:{%s} pszIn=%p:{%s} pszOutStr=%p:{%s} pszInStr=%p:{%s} pszDesc=%p:{%s}\n", pDevIns->pReg->szName, pDevIns->iInstance,
             Port, cPorts, pvUser, pszOut, pszOut, pszIn, pszIn, pszOutStr, pszOutStr, pszInStr, pszInStr, pszDesc, pszDesc));

    /*
     * Resolve the functions (one of the can be NULL).
     */
    int rc = VINF_SUCCESS;
    if (    pDevIns->pReg->szR0Mod[0]
        &&  (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_R0))
    {
        R0PTRTYPE(PFNIOMIOPORTIN) pfnR0PtrIn = 0;
        if (pszIn)
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pszIn, &pfnR0PtrIn);
            AssertMsgRC(rc, ("Failed to resolve %s.%s (pszIn)\n", pDevIns->pReg->szR0Mod, pszIn));
        }
        R0PTRTYPE(PFNIOMIOPORTOUT) pfnR0PtrOut = 0;
        if (pszOut && RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pszOut, &pfnR0PtrOut);
            AssertMsgRC(rc, ("Failed to resolve %s.%s (pszOut)\n", pDevIns->pReg->szR0Mod, pszOut));
        }
        R0PTRTYPE(PFNIOMIOPORTINSTRING) pfnR0PtrInStr = 0;
        if (pszInStr && RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pszInStr, &pfnR0PtrInStr);
            AssertMsgRC(rc, ("Failed to resolve %s.%s (pszInStr)\n", pDevIns->pReg->szR0Mod, pszInStr));
        }
        R0PTRTYPE(PFNIOMIOPORTOUTSTRING) pfnR0PtrOutStr = 0;
        if (pszOutStr && RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pszOutStr, &pfnR0PtrOutStr);
            AssertMsgRC(rc, ("Failed to resolve %s.%s (pszOutStr)\n", pDevIns->pReg->szR0Mod, pszOutStr));
        }

        if (RT_SUCCESS(rc))
        {
#if 0 /** @todo needs a real string cache for this */
            if (pDevIns->iInstance > 0)
            {
                 char *pszDesc2 = MMR3HeapAPrintf(pVM, MM_TAG_PDM_DEVICE_DESC, "%s [%u]", pszDesc, pDevIns->iInstance);
                 if (pszDesc2)
                     pszDesc = pszDesc2;
            }
#endif

            rc = IOMR3IOPortRegisterR0(pDevIns->Internal.s.pVMR3, pDevIns, Port, cPorts, pvUser, pfnR0PtrOut, pfnR0PtrIn, pfnR0PtrOutStr, pfnR0PtrInStr, pszDesc);
        }
    }
    else
    {
        AssertMsgFailed(("No R0 module for this driver!\n"));
        rc = VERR_INVALID_PARAMETER;
    }

    LogFlow(("pdmR3DevHlp_IOPortRegisterR0: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnIOPortDeregister} */
static DECLCALLBACK(int) pdmR3DevHlp_IOPortDeregister(PPDMDEVINS pDevIns, RTIOPORT Port, RTIOPORT cPorts)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_IOPortDeregister: caller='%s'/%d: Port=%#x cPorts=%#x\n", pDevIns->pReg->szName, pDevIns->iInstance,
             Port, cPorts));

    int rc = IOMR3IOPortDeregister(pDevIns->Internal.s.pVMR3, pDevIns, Port, cPorts);

    LogFlow(("pdmR3DevHlp_IOPortDeregister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMMIORegister} */
static DECLCALLBACK(int) pdmR3DevHlp_MMIORegister(PPDMDEVINS pDevIns, RTGCPHYS GCPhysStart, uint32_t cbRange, RTHCPTR pvUser,
                                                  PFNIOMMMIOWRITE pfnWrite, PFNIOMMMIOREAD pfnRead, PFNIOMMMIOFILL pfnFill,
                                                  uint32_t fFlags, const char *pszDesc)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_MMIORegister: caller='%s'/%d: GCPhysStart=%RGp cbRange=%#x pvUser=%p pfnWrite=%p pfnRead=%p pfnFill=%p fFlags=%#x pszDesc=%p:{%s}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPhysStart, cbRange, pvUser, pfnWrite, pfnRead, pfnFill, pszDesc, fFlags, pszDesc));

    if (pDevIns->iInstance > 0)
    {
        char *pszDesc2 = MMR3HeapAPrintf(pVM, MM_TAG_PDM_DEVICE_DESC, "%s [%u]", pszDesc, pDevIns->iInstance);
        if (pszDesc2)
            pszDesc = pszDesc2;
    }

    int rc = IOMR3MmioRegisterR3(pVM, pDevIns, GCPhysStart, cbRange, pvUser,
                                 pfnWrite, pfnRead, pfnFill, fFlags, pszDesc);

    LogFlow(("pdmR3DevHlp_MMIORegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMMIORegisterRC} */
static DECLCALLBACK(int) pdmR3DevHlp_MMIORegisterRC(PPDMDEVINS pDevIns, RTGCPHYS GCPhysStart, uint32_t cbRange, RTRCPTR pvUser,
                                                    const char *pszWrite, const char *pszRead, const char *pszFill)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_MMIORegisterRC: caller='%s'/%d: GCPhysStart=%RGp cbRange=%#x pvUser=%p pszWrite=%p:{%s} pszRead=%p:{%s} pszFill=%p:{%s}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPhysStart, cbRange, pvUser, pszWrite, pszWrite, pszRead, pszRead, pszFill, pszFill));


    /*
     * Resolve the functions.
     * Not all function have to present, leave it to IOM to enforce this.
     */
    int rc = VINF_SUCCESS;
    if (   pDevIns->pReg->szRCMod[0]
        && (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_RC)
        && !HMIsEnabled(pVM))
    {
        RTRCPTR RCPtrWrite = NIL_RTRCPTR;
        if (pszWrite)
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pszWrite, &RCPtrWrite);

        RTRCPTR RCPtrRead = NIL_RTRCPTR;
        int rc2 = VINF_SUCCESS;
        if (pszRead)
            rc2 = pdmR3DevGetSymbolRCLazy(pDevIns, pszRead, &RCPtrRead);

        RTRCPTR RCPtrFill = NIL_RTRCPTR;
        int rc3 = VINF_SUCCESS;
        if (pszFill)
            rc3 = pdmR3DevGetSymbolRCLazy(pDevIns, pszFill, &RCPtrFill);

        if (RT_SUCCESS(rc) && RT_SUCCESS(rc2) && RT_SUCCESS(rc3))
            rc = IOMR3MmioRegisterRC(pVM, pDevIns, GCPhysStart, cbRange, pvUser, RCPtrWrite, RCPtrRead, RCPtrFill);
        else
        {
            AssertMsgRC(rc,  ("Failed to resolve %s.%s (pszWrite)\n", pDevIns->pReg->szRCMod, pszWrite));
            AssertMsgRC(rc2, ("Failed to resolve %s.%s (pszRead)\n",  pDevIns->pReg->szRCMod, pszRead));
            AssertMsgRC(rc3, ("Failed to resolve %s.%s (pszFill)\n",  pDevIns->pReg->szRCMod, pszFill));
            if (RT_FAILURE(rc2) && RT_SUCCESS(rc))
                rc = rc2;
            if (RT_FAILURE(rc3) && RT_SUCCESS(rc))
                rc = rc3;
        }
    }
    else if (!HMIsEnabled(pVM))
    {
        AssertMsgFailed(("No RC module for this driver!\n"));
        rc = VERR_INVALID_PARAMETER;
    }

    LogFlow(("pdmR3DevHlp_MMIORegisterRC: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}

/** @interface_method_impl{PDMDEVHLPR3,pfnMMIORegisterR0} */
static DECLCALLBACK(int) pdmR3DevHlp_MMIORegisterR0(PPDMDEVINS pDevIns, RTGCPHYS GCPhysStart, uint32_t cbRange, RTR0PTR pvUser,
                                                    const char *pszWrite, const char *pszRead, const char *pszFill)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_MMIORegisterHC: caller='%s'/%d: GCPhysStart=%RGp cbRange=%#x pvUser=%p pszWrite=%p:{%s} pszRead=%p:{%s} pszFill=%p:{%s}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPhysStart, cbRange, pvUser, pszWrite, pszWrite, pszRead, pszRead, pszFill, pszFill));

    /*
     * Resolve the functions.
     * Not all function have to present, leave it to IOM to enforce this.
     */
    int rc = VINF_SUCCESS;
    if (    pDevIns->pReg->szR0Mod[0]
        &&  (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_R0))
    {
        R0PTRTYPE(PFNIOMMMIOWRITE) pfnR0PtrWrite = 0;
        if (pszWrite)
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pszWrite, &pfnR0PtrWrite);
        R0PTRTYPE(PFNIOMMMIOREAD) pfnR0PtrRead = 0;
        int rc2 = VINF_SUCCESS;
        if (pszRead)
            rc2 = pdmR3DevGetSymbolR0Lazy(pDevIns, pszRead, &pfnR0PtrRead);
        R0PTRTYPE(PFNIOMMMIOFILL) pfnR0PtrFill = 0;
        int rc3 = VINF_SUCCESS;
        if (pszFill)
            rc3 = pdmR3DevGetSymbolR0Lazy(pDevIns, pszFill, &pfnR0PtrFill);
        if (RT_SUCCESS(rc) && RT_SUCCESS(rc2) && RT_SUCCESS(rc3))
            rc = IOMR3MmioRegisterR0(pDevIns->Internal.s.pVMR3, pDevIns, GCPhysStart, cbRange, pvUser, pfnR0PtrWrite, pfnR0PtrRead, pfnR0PtrFill);
        else
        {
            AssertMsgRC(rc,  ("Failed to resolve %s.%s (pszWrite)\n", pDevIns->pReg->szR0Mod, pszWrite));
            AssertMsgRC(rc2, ("Failed to resolve %s.%s (pszRead)\n",  pDevIns->pReg->szR0Mod, pszRead));
            AssertMsgRC(rc3, ("Failed to resolve %s.%s (pszFill)\n",  pDevIns->pReg->szR0Mod, pszFill));
            if (RT_FAILURE(rc2) && RT_SUCCESS(rc))
                rc = rc2;
            if (RT_FAILURE(rc3) && RT_SUCCESS(rc))
                rc = rc3;
        }
    }
    else
    {
        AssertMsgFailed(("No R0 module for this driver!\n"));
        rc = VERR_INVALID_PARAMETER;
    }

    LogFlow(("pdmR3DevHlp_MMIORegisterR0: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMMIODeregister} */
static DECLCALLBACK(int) pdmR3DevHlp_MMIODeregister(PPDMDEVINS pDevIns, RTGCPHYS GCPhysStart, uint32_t cbRange)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_MMIODeregister: caller='%s'/%d: GCPhysStart=%RGp cbRange=%#x\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPhysStart, cbRange));

    int rc = IOMR3MmioDeregister(pDevIns->Internal.s.pVMR3, pDevIns, GCPhysStart, cbRange);

    LogFlow(("pdmR3DevHlp_MMIODeregister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/**
 * @copydoc PDMDEVHLPR3::pfnMMIO2Register
 */
static DECLCALLBACK(int) pdmR3DevHlp_MMIO2Register(PPDMDEVINS pDevIns, uint32_t iRegion, RTGCPHYS cb, uint32_t fFlags, void **ppv, const char *pszDesc)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_MMIO2Register: caller='%s'/%d: iRegion=%#x cb=%#RGp fFlags=%RX32 ppv=%p pszDescp=%p:{%s}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iRegion, cb, fFlags, ppv, pszDesc, pszDesc));

/** @todo PGMR3PhysMMIO2Register mangles the description, move it here and
 *        use a real string cache. */
    int rc = PGMR3PhysMMIO2Register(pDevIns->Internal.s.pVMR3, pDevIns, iRegion, cb, fFlags, ppv, pszDesc);

    LogFlow(("pdmR3DevHlp_MMIO2Register: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/**
 * @copydoc PDMDEVHLPR3::pfnMMIO2Deregister
 */
static DECLCALLBACK(int) pdmR3DevHlp_MMIO2Deregister(PPDMDEVINS pDevIns, uint32_t iRegion)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_MMIO2Deregister: caller='%s'/%d: iRegion=%#x\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iRegion));

    AssertReturn(iRegion <= UINT8_MAX || iRegion == UINT32_MAX, VERR_INVALID_PARAMETER);

    int rc = PGMR3PhysMMIO2Deregister(pDevIns->Internal.s.pVMR3, pDevIns, iRegion);

    LogFlow(("pdmR3DevHlp_MMIO2Deregister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/**
 * @copydoc PDMDEVHLPR3::pfnMMIO2Map
 */
static DECLCALLBACK(int) pdmR3DevHlp_MMIO2Map(PPDMDEVINS pDevIns, uint32_t iRegion, RTGCPHYS GCPhys)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_MMIO2Map: caller='%s'/%d: iRegion=%#x GCPhys=%#RGp\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iRegion, GCPhys));

    int rc = PGMR3PhysMMIO2Map(pDevIns->Internal.s.pVMR3, pDevIns, iRegion, GCPhys);

    LogFlow(("pdmR3DevHlp_MMIO2Map: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/**
 * @copydoc PDMDEVHLPR3::pfnMMIO2Unmap
 */
static DECLCALLBACK(int) pdmR3DevHlp_MMIO2Unmap(PPDMDEVINS pDevIns, uint32_t iRegion, RTGCPHYS GCPhys)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_MMIO2Unmap: caller='%s'/%d: iRegion=%#x GCPhys=%#RGp\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iRegion, GCPhys));

    int rc = PGMR3PhysMMIO2Unmap(pDevIns->Internal.s.pVMR3, pDevIns, iRegion, GCPhys);

    LogFlow(("pdmR3DevHlp_MMIO2Unmap: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/**
 * @copydoc PDMDEVHLPR3::pfnMMHyperMapMMIO2
 */
static DECLCALLBACK(int) pdmR3DevHlp_MMHyperMapMMIO2(PPDMDEVINS pDevIns, uint32_t iRegion, RTGCPHYS off, RTGCPHYS cb,
                                                     const char *pszDesc, PRTRCPTR pRCPtr)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_MMHyperMapMMIO2: caller='%s'/%d: iRegion=%#x off=%RGp cb=%RGp pszDesc=%p:{%s} pRCPtr=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iRegion, off, cb, pszDesc, pszDesc, pRCPtr));

    if (pDevIns->iInstance > 0)
    {
         char *pszDesc2 = MMR3HeapAPrintf(pVM, MM_TAG_PDM_DEVICE_DESC, "%s [%u]", pszDesc, pDevIns->iInstance);
         if (pszDesc2)
             pszDesc = pszDesc2;
    }

    int rc = MMR3HyperMapMMIO2(pVM, pDevIns, iRegion, off, cb, pszDesc, pRCPtr);

    LogFlow(("pdmR3DevHlp_MMHyperMapMMIO2: caller='%s'/%d: returns %Rrc *pRCPtr=%RRv\n", pDevIns->pReg->szName, pDevIns->iInstance, rc, *pRCPtr));
    return rc;
}


/**
 * @copydoc PDMDEVHLPR3::pfnMMIO2MapKernel
 */
static DECLCALLBACK(int) pdmR3DevHlp_MMIO2MapKernel(PPDMDEVINS pDevIns, uint32_t iRegion, RTGCPHYS off, RTGCPHYS cb,
                                                    const char *pszDesc, PRTR0PTR pR0Ptr)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_MMIO2MapKernel: caller='%s'/%d: iRegion=%#x off=%RGp cb=%RGp pszDesc=%p:{%s} pR0Ptr=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iRegion, off, cb, pszDesc, pszDesc, pR0Ptr));

    if (pDevIns->iInstance > 0)
    {
         char *pszDesc2 = MMR3HeapAPrintf(pVM, MM_TAG_PDM_DEVICE_DESC, "%s [%u]", pszDesc, pDevIns->iInstance);
         if (pszDesc2)
             pszDesc = pszDesc2;
    }

    int rc = PGMR3PhysMMIO2MapKernel(pVM, pDevIns, iRegion, off, cb, pszDesc, pR0Ptr);

    LogFlow(("pdmR3DevHlp_MMIO2MapKernel: caller='%s'/%d: returns %Rrc *pR0Ptr=%RHv\n", pDevIns->pReg->szName, pDevIns->iInstance, rc, *pR0Ptr));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnROMRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_ROMRegister(PPDMDEVINS pDevIns, RTGCPHYS GCPhysStart, uint32_t cbRange,
                                                 const void *pvBinary, uint32_t cbBinary, uint32_t fFlags, const char *pszDesc)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_ROMRegister: caller='%s'/%d: GCPhysStart=%RGp cbRange=%#x pvBinary=%p cbBinary=%#x fFlags=%#RX32 pszDesc=%p:{%s}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPhysStart, cbRange, pvBinary, cbBinary, fFlags, pszDesc, pszDesc));

/** @todo can we mangle pszDesc? */
    int rc = PGMR3PhysRomRegister(pDevIns->Internal.s.pVMR3, pDevIns, GCPhysStart, cbRange, pvBinary, cbBinary, fFlags, pszDesc);

    LogFlow(("pdmR3DevHlp_ROMRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnROMProtectShadow} */
static DECLCALLBACK(int) pdmR3DevHlp_ROMProtectShadow(PPDMDEVINS pDevIns, RTGCPHYS GCPhysStart, uint32_t cbRange, PGMROMPROT enmProt)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_ROMProtectShadow: caller='%s'/%d: GCPhysStart=%RGp cbRange=%#x enmProt=%d\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPhysStart, cbRange, enmProt));

    int rc = PGMR3PhysRomProtect(pDevIns->Internal.s.pVMR3, GCPhysStart, cbRange, enmProt);

    LogFlow(("pdmR3DevHlp_ROMProtectShadow: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSSMRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_SSMRegister(PPDMDEVINS pDevIns, uint32_t uVersion, size_t cbGuess, const char *pszBefore,
                                                 PFNSSMDEVLIVEPREP pfnLivePrep, PFNSSMDEVLIVEEXEC pfnLiveExec, PFNSSMDEVLIVEVOTE pfnLiveVote,
                                                 PFNSSMDEVSAVEPREP pfnSavePrep, PFNSSMDEVSAVEEXEC pfnSaveExec, PFNSSMDEVSAVEDONE pfnSaveDone,
                                                 PFNSSMDEVLOADPREP pfnLoadPrep, PFNSSMDEVLOADEXEC pfnLoadExec, PFNSSMDEVLOADDONE pfnLoadDone)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_SSMRegister: caller='%s'/%d: uVersion=%#x cbGuess=%#x pszBefore=%p:{%s}\n"
             "    pfnLivePrep=%p pfnLiveExec=%p pfnLiveVote=%p pfnSavePrep=%p pfnSaveExec=%p pfnSaveDone=%p pszLoadPrep=%p pfnLoadExec=%p pfnLoadDone=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, uVersion, cbGuess, pszBefore, pszBefore,
             pfnLivePrep, pfnLiveExec, pfnLiveVote,
             pfnSavePrep, pfnSaveExec, pfnSaveDone,
             pfnLoadPrep, pfnLoadExec, pfnLoadDone));

    int rc = SSMR3RegisterDevice(pDevIns->Internal.s.pVMR3, pDevIns, pDevIns->pReg->szName, pDevIns->iInstance,
                                 uVersion, cbGuess, pszBefore,
                                 pfnLivePrep, pfnLiveExec, pfnLiveVote,
                                 pfnSavePrep, pfnSaveExec, pfnSaveDone,
                                 pfnLoadPrep, pfnLoadExec, pfnLoadDone);

    LogFlow(("pdmR3DevHlp_SSMRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnTMTimerCreate} */
static DECLCALLBACK(int) pdmR3DevHlp_TMTimerCreate(PPDMDEVINS pDevIns, TMCLOCK enmClock, PFNTMTIMERDEV pfnCallback, void *pvUser, uint32_t fFlags, const char *pszDesc, PPTMTIMERR3 ppTimer)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_TMTimerCreate: caller='%s'/%d: enmClock=%d pfnCallback=%p pvUser=%p fFlags=%#x pszDesc=%p:{%s} ppTimer=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, enmClock, pfnCallback, pvUser, fFlags, pszDesc, pszDesc, ppTimer));

    if (pDevIns->iInstance > 0) /** @todo use a string cache here later. */
    {
         char *pszDesc2 = MMR3HeapAPrintf(pVM, MM_TAG_PDM_DEVICE_DESC, "%s [%u]", pszDesc, pDevIns->iInstance);
         if (pszDesc2)
             pszDesc = pszDesc2;
    }

    int rc = TMR3TimerCreateDevice(pVM, pDevIns, enmClock, pfnCallback, pvUser, fFlags, pszDesc, ppTimer);

    LogFlow(("pdmR3DevHlp_TMTimerCreate: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnTMUtcNow} */
static DECLCALLBACK(PRTTIMESPEC) pdmR3DevHlp_TMUtcNow(PPDMDEVINS pDevIns, PRTTIMESPEC pTime)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_TMUtcNow: caller='%s'/%d: pTime=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pTime));

    pTime = TMR3UtcNow(pDevIns->Internal.s.pVMR3, pTime);

    LogFlow(("pdmR3DevHlp_TMUtcNow: caller='%s'/%d: returns %RU64\n", pDevIns->pReg->szName, pDevIns->iInstance, RTTimeSpecGetNano(pTime)));
    return pTime;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnTMTimeVirtGet} */
static DECLCALLBACK(uint64_t) pdmR3DevHlp_TMTimeVirtGet(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_TMTimeVirtGet: caller='%s'/%d\n",
             pDevIns->pReg->szName, pDevIns->iInstance));

    uint64_t u64Time = TMVirtualSyncGet(pDevIns->Internal.s.pVMR3);

    LogFlow(("pdmR3DevHlp_TMTimeVirtGet: caller='%s'/%d: returns %RU64\n", pDevIns->pReg->szName, pDevIns->iInstance, u64Time));
    return u64Time;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnTMTimeVirtGetFreq} */
static DECLCALLBACK(uint64_t) pdmR3DevHlp_TMTimeVirtGetFreq(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_TMTimeVirtGetFreq: caller='%s'/%d\n",
             pDevIns->pReg->szName, pDevIns->iInstance));

    uint64_t u64Freq = TMVirtualGetFreq(pDevIns->Internal.s.pVMR3);

    LogFlow(("pdmR3DevHlp_TMTimeVirtGetFreq: caller='%s'/%d: returns %RU64\n", pDevIns->pReg->szName, pDevIns->iInstance, u64Freq));
    return u64Freq;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnTMTimeVirtGetNano} */
static DECLCALLBACK(uint64_t) pdmR3DevHlp_TMTimeVirtGetNano(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_TMTimeVirtGetNano: caller='%s'/%d\n",
             pDevIns->pReg->szName, pDevIns->iInstance));

    uint64_t u64Time = TMVirtualGet(pDevIns->Internal.s.pVMR3);
    uint64_t u64Nano = TMVirtualToNano(pDevIns->Internal.s.pVMR3, u64Time);

    LogFlow(("pdmR3DevHlp_TMTimeVirtGetNano: caller='%s'/%d: returns %RU64\n", pDevIns->pReg->szName, pDevIns->iInstance, u64Nano));
    return u64Nano;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetSupDrvSession} */
static DECLCALLBACK(PSUPDRVSESSION) pdmR3DevHlp_GetSupDrvSession(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_GetSupDrvSession: caller='%s'/%d\n",
             pDevIns->pReg->szName, pDevIns->iInstance));

    PSUPDRVSESSION pSession = pDevIns->Internal.s.pVMR3->pSession;

    LogFlow(("pdmR3DevHlp_GetSupDrvSession: caller='%s'/%d: returns %#p\n", pDevIns->pReg->szName, pDevIns->iInstance, pSession));
    return pSession;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPhysRead} */
static DECLCALLBACK(int) pdmR3DevHlp_PhysRead(PPDMDEVINS pDevIns, RTGCPHYS GCPhys, void *pvBuf, size_t cbRead)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    LogFlow(("pdmR3DevHlp_PhysRead: caller='%s'/%d: GCPhys=%RGp pvBuf=%p cbRead=%#x\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPhys, pvBuf, cbRead));

#if defined(VBOX_STRICT) && defined(PDM_DEVHLP_DEADLOCK_DETECTION)
    if (!VM_IS_EMT(pVM))
    {
        char szNames[128];
        uint32_t cLocks = PDMR3CritSectCountOwned(pVM, szNames, sizeof(szNames));
        AssertMsg(cLocks == 0, ("cLocks=%u %s\n", cLocks, szNames));
    }
#endif

    VBOXSTRICTRC rcStrict;
    if (VM_IS_EMT(pVM))
        rcStrict = PGMPhysRead(pVM, GCPhys, pvBuf, cbRead, PGMACCESSORIGIN_DEVICE);
    else
        rcStrict = PGMR3PhysReadExternal(pVM, GCPhys, pvBuf, cbRead, PGMACCESSORIGIN_DEVICE);
    AssertMsg(rcStrict == VINF_SUCCESS, ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict))); /** @todo track down the users for this bugger. */

    Log(("pdmR3DevHlp_PhysRead: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VBOXSTRICTRC_VAL(rcStrict) ));
    return VBOXSTRICTRC_VAL(rcStrict);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPhysWrite} */
static DECLCALLBACK(int) pdmR3DevHlp_PhysWrite(PPDMDEVINS pDevIns, RTGCPHYS GCPhys, const void *pvBuf, size_t cbWrite)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    LogFlow(("pdmR3DevHlp_PhysWrite: caller='%s'/%d: GCPhys=%RGp pvBuf=%p cbWrite=%#x\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPhys, pvBuf, cbWrite));

#if defined(VBOX_STRICT) && defined(PDM_DEVHLP_DEADLOCK_DETECTION)
    if (!VM_IS_EMT(pVM))
    {
        char szNames[128];
        uint32_t cLocks = PDMR3CritSectCountOwned(pVM, szNames, sizeof(szNames));
        AssertMsg(cLocks == 0, ("cLocks=%u %s\n", cLocks, szNames));
    }
#endif

    VBOXSTRICTRC rcStrict;
    if (VM_IS_EMT(pVM))
        rcStrict = PGMPhysWrite(pVM, GCPhys, pvBuf, cbWrite, PGMACCESSORIGIN_DEVICE);
    else
        rcStrict = PGMR3PhysWriteExternal(pVM, GCPhys, pvBuf, cbWrite, PGMACCESSORIGIN_DEVICE);
    AssertMsg(rcStrict == VINF_SUCCESS, ("%Rrc\n", VBOXSTRICTRC_VAL(rcStrict))); /** @todo track down the users for this bugger. */

    Log(("pdmR3DevHlp_PhysWrite: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VBOXSTRICTRC_VAL(rcStrict) ));
    return VBOXSTRICTRC_VAL(rcStrict);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPhysGCPhys2CCPtr} */
static DECLCALLBACK(int) pdmR3DevHlp_PhysGCPhys2CCPtr(PPDMDEVINS pDevIns, RTGCPHYS GCPhys, uint32_t fFlags, void **ppv, PPGMPAGEMAPLOCK pLock)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    LogFlow(("pdmR3DevHlp_PhysGCPhys2CCPtr: caller='%s'/%d: GCPhys=%RGp fFlags=%#x ppv=%p pLock=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPhys, fFlags, ppv, pLock));
    AssertReturn(!fFlags, VERR_INVALID_PARAMETER);

#if defined(VBOX_STRICT) && defined(PDM_DEVHLP_DEADLOCK_DETECTION)
    if (!VM_IS_EMT(pVM))
    {
        char szNames[128];
        uint32_t cLocks = PDMR3CritSectCountOwned(pVM, szNames, sizeof(szNames));
        AssertMsg(cLocks == 0, ("cLocks=%u %s\n", cLocks, szNames));
    }
#endif

    int rc = PGMR3PhysGCPhys2CCPtrExternal(pVM, GCPhys, ppv, pLock);

    Log(("pdmR3DevHlp_PhysGCPhys2CCPtr: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPhysGCPhys2CCPtrReadOnly} */
static DECLCALLBACK(int) pdmR3DevHlp_PhysGCPhys2CCPtrReadOnly(PPDMDEVINS pDevIns, RTGCPHYS GCPhys, uint32_t fFlags, const void **ppv, PPGMPAGEMAPLOCK pLock)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    LogFlow(("pdmR3DevHlp_PhysGCPhys2CCPtrReadOnly: caller='%s'/%d: GCPhys=%RGp fFlags=%#x ppv=%p pLock=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPhys, fFlags, ppv, pLock));
    AssertReturn(!fFlags, VERR_INVALID_PARAMETER);

#if defined(VBOX_STRICT) && defined(PDM_DEVHLP_DEADLOCK_DETECTION)
    if (!VM_IS_EMT(pVM))
    {
        char szNames[128];
        uint32_t cLocks = PDMR3CritSectCountOwned(pVM, szNames, sizeof(szNames));
        AssertMsg(cLocks == 0, ("cLocks=%u %s\n", cLocks, szNames));
    }
#endif

    int rc = PGMR3PhysGCPhys2CCPtrReadOnlyExternal(pVM, GCPhys, ppv, pLock);

    Log(("pdmR3DevHlp_PhysGCPhys2CCPtrReadOnly: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPhysReleasePageMappingLock} */
static DECLCALLBACK(void) pdmR3DevHlp_PhysReleasePageMappingLock(PPDMDEVINS pDevIns, PPGMPAGEMAPLOCK pLock)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    LogFlow(("pdmR3DevHlp_PhysReleasePageMappingLock: caller='%s'/%d: pLock=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pLock));

    PGMPhysReleasePageMappingLock(pVM, pLock);

    Log(("pdmR3DevHlp_PhysReleasePageMappingLock: caller='%s'/%d: returns void\n", pDevIns->pReg->szName, pDevIns->iInstance));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPhysReadGCVirt} */
static DECLCALLBACK(int) pdmR3DevHlp_PhysReadGCVirt(PPDMDEVINS pDevIns, void *pvDst, RTGCPTR GCVirtSrc, size_t cb)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_PhysReadGCVirt: caller='%s'/%d: pvDst=%p GCVirt=%RGv cb=%#x\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pvDst, GCVirtSrc, cb));

    PVMCPU pVCpu = VMMGetCpu(pVM);
    if (!pVCpu)
        return VERR_ACCESS_DENIED;
#if defined(VBOX_STRICT) && defined(PDM_DEVHLP_DEADLOCK_DETECTION)
    /** @todo SMP. */
#endif

    int rc = PGMPhysSimpleReadGCPtr(pVCpu, pvDst, GCVirtSrc, cb);

    LogFlow(("pdmR3DevHlp_PhysReadGCVirt: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));

    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPhysWriteGCVirt} */
static DECLCALLBACK(int) pdmR3DevHlp_PhysWriteGCVirt(PPDMDEVINS pDevIns, RTGCPTR GCVirtDst, const void *pvSrc, size_t cb)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_PhysWriteGCVirt: caller='%s'/%d: GCVirtDst=%RGv pvSrc=%p cb=%#x\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCVirtDst, pvSrc, cb));

    PVMCPU pVCpu = VMMGetCpu(pVM);
    if (!pVCpu)
        return VERR_ACCESS_DENIED;
#if defined(VBOX_STRICT) && defined(PDM_DEVHLP_DEADLOCK_DETECTION)
    /** @todo SMP. */
#endif

    int rc = PGMPhysSimpleWriteGCPtr(pVCpu, GCVirtDst, pvSrc, cb);

    LogFlow(("pdmR3DevHlp_PhysWriteGCVirt: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));

    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPhysGCPtr2GCPhys} */
static DECLCALLBACK(int) pdmR3DevHlp_PhysGCPtr2GCPhys(PPDMDEVINS pDevIns, RTGCPTR GCPtr, PRTGCPHYS pGCPhys)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_PhysGCPtr2GCPhys: caller='%s'/%d: GCPtr=%RGv pGCPhys=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, GCPtr, pGCPhys));

    PVMCPU pVCpu = VMMGetCpu(pVM);
    if (!pVCpu)
        return VERR_ACCESS_DENIED;
#if defined(VBOX_STRICT) && defined(PDM_DEVHLP_DEADLOCK_DETECTION)
    /** @todo SMP. */
#endif

    int rc = PGMPhysGCPtr2GCPhys(pVCpu, GCPtr, pGCPhys);

    LogFlow(("pdmR3DevHlp_PhysGCPtr2GCPhys: caller='%s'/%d: returns %Rrc *pGCPhys=%RGp\n", pDevIns->pReg->szName, pDevIns->iInstance, rc, *pGCPhys));

    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMMHeapAlloc} */
static DECLCALLBACK(void *) pdmR3DevHlp_MMHeapAlloc(PPDMDEVINS pDevIns, size_t cb)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_MMHeapAlloc: caller='%s'/%d: cb=%#x\n", pDevIns->pReg->szName, pDevIns->iInstance, cb));

    void *pv = MMR3HeapAlloc(pDevIns->Internal.s.pVMR3, MM_TAG_PDM_DEVICE_USER, cb);

    LogFlow(("pdmR3DevHlp_MMHeapAlloc: caller='%s'/%d: returns %p\n", pDevIns->pReg->szName, pDevIns->iInstance, pv));
    return pv;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMMHeapAllocZ} */
static DECLCALLBACK(void *) pdmR3DevHlp_MMHeapAllocZ(PPDMDEVINS pDevIns, size_t cb)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_MMHeapAllocZ: caller='%s'/%d: cb=%#x\n", pDevIns->pReg->szName, pDevIns->iInstance, cb));

    void *pv = MMR3HeapAllocZ(pDevIns->Internal.s.pVMR3, MM_TAG_PDM_DEVICE_USER, cb);

    LogFlow(("pdmR3DevHlp_MMHeapAllocZ: caller='%s'/%d: returns %p\n", pDevIns->pReg->szName, pDevIns->iInstance, pv));
    return pv;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnMMHeapFree} */
static DECLCALLBACK(void) pdmR3DevHlp_MMHeapFree(PPDMDEVINS pDevIns, void *pv)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_MMHeapFree: caller='%s'/%d: pv=%p\n", pDevIns->pReg->szName, pDevIns->iInstance, pv));

    MMR3HeapFree(pv);

    LogFlow(("pdmR3DevHlp_MMHeapAlloc: caller='%s'/%d: returns void\n", pDevIns->pReg->szName, pDevIns->iInstance));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMState} */
static DECLCALLBACK(VMSTATE) pdmR3DevHlp_VMState(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);

    VMSTATE enmVMState = VMR3GetState(pDevIns->Internal.s.pVMR3);

    LogFlow(("pdmR3DevHlp_VMState: caller='%s'/%d: returns %d (%s)\n", pDevIns->pReg->szName, pDevIns->iInstance,
             enmVMState, VMR3GetStateName(enmVMState)));
    return enmVMState;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMTeleportedAndNotFullyResumedYet} */
static DECLCALLBACK(bool) pdmR3DevHlp_VMTeleportedAndNotFullyResumedYet(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);

    bool fRc = VMR3TeleportedAndNotFullyResumedYet(pDevIns->Internal.s.pVMR3);

    LogFlow(("pdmR3DevHlp_VMState: caller='%s'/%d: returns %RTbool\n", pDevIns->pReg->szName, pDevIns->iInstance,
             fRc));
    return fRc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMSetError} */
static DECLCALLBACK(int) pdmR3DevHlp_VMSetError(PPDMDEVINS pDevIns, int rc, RT_SRC_POS_DECL, const char *pszFormat, ...)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    va_list args;
    va_start(args, pszFormat);
    int rc2 = VMSetErrorV(pDevIns->Internal.s.pVMR3, rc, RT_SRC_POS_ARGS, pszFormat, args); Assert(rc2 == rc); NOREF(rc2);
    va_end(args);
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMSetErrorV} */
static DECLCALLBACK(int) pdmR3DevHlp_VMSetErrorV(PPDMDEVINS pDevIns, int rc, RT_SRC_POS_DECL, const char *pszFormat, va_list va)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    int rc2 = VMSetErrorV(pDevIns->Internal.s.pVMR3, rc, RT_SRC_POS_ARGS, pszFormat, va); Assert(rc2 == rc); NOREF(rc2);
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMSetRuntimeError} */
static DECLCALLBACK(int) pdmR3DevHlp_VMSetRuntimeError(PPDMDEVINS pDevIns, uint32_t fFlags, const char *pszErrorId, const char *pszFormat, ...)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    va_list args;
    va_start(args, pszFormat);
    int rc = VMSetRuntimeErrorV(pDevIns->Internal.s.pVMR3, fFlags, pszErrorId, pszFormat, args);
    va_end(args);
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMSetRuntimeErrorV} */
static DECLCALLBACK(int) pdmR3DevHlp_VMSetRuntimeErrorV(PPDMDEVINS pDevIns, uint32_t fFlags, const char *pszErrorId, const char *pszFormat, va_list va)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    int rc = VMSetRuntimeErrorV(pDevIns->Internal.s.pVMR3, fFlags, pszErrorId, pszFormat, va);
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDBGFStopV} */
static DECLCALLBACK(int) pdmR3DevHlp_DBGFStopV(PPDMDEVINS pDevIns, const char *pszFile, unsigned iLine, const char *pszFunction, const char *pszFormat, va_list args)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
#ifdef LOG_ENABLED
    va_list va2;
    va_copy(va2, args);
    LogFlow(("pdmR3DevHlp_DBGFStopV: caller='%s'/%d: pszFile=%p:{%s} iLine=%d pszFunction=%p:{%s} pszFormat=%p:{%s} (%N)\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pszFile, pszFile, iLine, pszFunction, pszFunction, pszFormat, pszFormat, pszFormat, &va2));
    va_end(va2);
#endif

    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    int rc = DBGFR3EventSrcV(pVM, DBGFEVENT_DEV_STOP, pszFile, iLine, pszFunction, pszFormat, args);
    if (rc == VERR_DBGF_NOT_ATTACHED)
        rc = VINF_SUCCESS;

    LogFlow(("pdmR3DevHlp_DBGFStopV: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDBGFInfoRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_DBGFInfoRegister(PPDMDEVINS pDevIns, const char *pszName, const char *pszDesc, PFNDBGFHANDLERDEV pfnHandler)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_DBGFInfoRegister: caller='%s'/%d: pszName=%p:{%s} pszDesc=%p:{%s} pfnHandler=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pszName, pszName, pszDesc, pszDesc, pfnHandler));

    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    int rc = DBGFR3InfoRegisterDevice(pVM, pszName, pszDesc, pfnHandler, pDevIns);

    LogFlow(("pdmR3DevHlp_DBGFInfoRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDBGFRegRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_DBGFRegRegister(PPDMDEVINS pDevIns, PCDBGFREGDESC paRegisters)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_DBGFRegRegister: caller='%s'/%d: paRegisters=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, paRegisters));

    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    int rc = DBGFR3RegRegisterDevice(pVM, paRegisters, pDevIns, pDevIns->pReg->szName, pDevIns->iInstance);

    LogFlow(("pdmR3DevHlp_DBGFRegRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDBGFTraceBuf} */
static DECLCALLBACK(RTTRACEBUF) pdmR3DevHlp_DBGFTraceBuf(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    RTTRACEBUF hTraceBuf = pDevIns->Internal.s.pVMR3->hTraceBufR3;
    LogFlow(("pdmR3DevHlp_DBGFTraceBuf: caller='%s'/%d: returns %p\n", pDevIns->pReg->szName, pDevIns->iInstance, hTraceBuf));
    return hTraceBuf;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSTAMRegister} */
static DECLCALLBACK(void) pdmR3DevHlp_STAMRegister(PPDMDEVINS pDevIns, void *pvSample, STAMTYPE enmType, const char *pszName, STAMUNIT enmUnit, const char *pszDesc)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);

    STAM_REG(pVM, pvSample, enmType, pszName, enmUnit, pszDesc);
    NOREF(pVM);
}



/** @interface_method_impl{PDMDEVHLPR3,pfnSTAMRegisterF} */
static DECLCALLBACK(void) pdmR3DevHlp_STAMRegisterF(PPDMDEVINS pDevIns, void *pvSample, STAMTYPE enmType, STAMVISIBILITY enmVisibility,
                                                    STAMUNIT enmUnit, const char *pszDesc, const char *pszName, ...)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);

    va_list args;
    va_start(args, pszName);
    int rc = STAMR3RegisterV(pVM, pvSample, enmType, enmVisibility, enmUnit, pszDesc, pszName, args);
    va_end(args);
    AssertRC(rc);

    NOREF(pVM);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSTAMRegisterV} */
static DECLCALLBACK(void) pdmR3DevHlp_STAMRegisterV(PPDMDEVINS pDevIns, void *pvSample, STAMTYPE enmType, STAMVISIBILITY enmVisibility,
                                                    STAMUNIT enmUnit, const char *pszDesc, const char *pszName, va_list args)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);

    int rc = STAMR3RegisterV(pVM, pvSample, enmType, enmVisibility, enmUnit, pszDesc, pszName, args);
    AssertRC(rc);

    NOREF(pVM);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_PCIRegister(PPDMDEVINS pDevIns, PPCIDEVICE pPciDev)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_PCIRegister: caller='%s'/%d: pPciDev=%p:{.config={%#.256Rhxs}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pPciDev, pPciDev->config));

    /*
     * Validate input.
     */
    if (!pPciDev)
    {
        Assert(pPciDev);
        LogFlow(("pdmR3DevHlp_PCIRegister: caller='%s'/%d: returns %Rrc (pPciDev)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (!pPciDev->config[0] && !pPciDev->config[1])
    {
        Assert(pPciDev->config[0] || pPciDev->config[1]);
        LogFlow(("pdmR3DevHlp_PCIRegister: caller='%s'/%d: returns %Rrc (vendor)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (pDevIns->Internal.s.pPciDeviceR3)
    {
        /** @todo the PCI device vs. PDM device designed is a bit flawed if we have to
         * support a PDM device with multiple PCI devices. This might become a problem
         * when upgrading the chipset for instance because of multiple functions in some
         * devices...
         */
        AssertMsgFailed(("Only one PCI device per device is currently implemented!\n"));
        return VERR_PDM_ONE_PCI_FUNCTION_PER_DEVICE;
    }

    /*
     * Choose the PCI bus for the device.
     *
     * This is simple. If the device was configured for a particular bus, the PCIBusNo
     * configuration value will be set. If not the default bus is 0.
     */
    int rc;
    PPDMPCIBUS pBus = pDevIns->Internal.s.pPciBusR3;
    if (!pBus)
    {
        uint8_t u8Bus;
        rc = CFGMR3QueryU8Def(pDevIns->Internal.s.pCfgHandle, "PCIBusNo", &u8Bus, 0);
        AssertLogRelMsgRCReturn(rc, ("Configuration error: PCIBusNo query failed with rc=%Rrc (%s/%d)\n",
                                     rc, pDevIns->pReg->szName, pDevIns->iInstance), rc);
        AssertLogRelMsgReturn(u8Bus < RT_ELEMENTS(pVM->pdm.s.aPciBuses),
                              ("Configuration error: PCIBusNo=%d, max is %d. (%s/%d)\n", u8Bus,
                               RT_ELEMENTS(pVM->pdm.s.aPciBuses), pDevIns->pReg->szName, pDevIns->iInstance),
                              VERR_PDM_NO_PCI_BUS);
        pBus = pDevIns->Internal.s.pPciBusR3 = &pVM->pdm.s.aPciBuses[u8Bus];
    }
    if (pBus->pDevInsR3)
    {
        if (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_R0)
            pDevIns->Internal.s.pPciBusR0 = MMHyperR3ToR0(pVM, pDevIns->Internal.s.pPciBusR3);
        else
            pDevIns->Internal.s.pPciBusR0 = NIL_RTR0PTR;

        if (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_RC)
            pDevIns->Internal.s.pPciBusRC = MMHyperR3ToRC(pVM, pDevIns->Internal.s.pPciBusR3);
        else
            pDevIns->Internal.s.pPciBusRC = NIL_RTRCPTR;

        /*
         * Check the configuration for PCI device and function assignment.
         */
        int iDev = -1;
        uint8_t     u8Device;
        rc = CFGMR3QueryU8(pDevIns->Internal.s.pCfgHandle, "PCIDeviceNo", &u8Device);
        if (RT_SUCCESS(rc))
        {
            AssertMsgReturn(u8Device <= 31,
                            ("Configuration error: PCIDeviceNo=%d, max is 31. (%s/%d)\n",
                             u8Device, pDevIns->pReg->szName, pDevIns->iInstance),
                            VERR_PDM_BAD_PCI_CONFIG);

            uint8_t u8Function;
            rc = CFGMR3QueryU8(pDevIns->Internal.s.pCfgHandle, "PCIFunctionNo", &u8Function);
            AssertMsgRCReturn(rc, ("Configuration error: PCIDeviceNo, but PCIFunctionNo query failed with rc=%Rrc (%s/%d)\n",
                                   rc, pDevIns->pReg->szName, pDevIns->iInstance),
                              rc);
            AssertMsgReturn(u8Function <= 7,
                            ("Configuration error: PCIFunctionNo=%d, max is 7. (%s/%d)\n",
                             u8Function, pDevIns->pReg->szName, pDevIns->iInstance),
                            VERR_PDM_BAD_PCI_CONFIG);

            iDev = (u8Device << 3) | u8Function;
        }
        else if (rc != VERR_CFGM_VALUE_NOT_FOUND)
        {
            AssertMsgFailed(("Configuration error: PCIDeviceNo query failed with rc=%Rrc (%s/%d)\n",
                             rc, pDevIns->pReg->szName, pDevIns->iInstance));
            return rc;
        }

        /*
         * Call the pci bus device to do the actual registration.
         */
        pdmLock(pVM);
        rc = pBus->pfnRegisterR3(pBus->pDevInsR3, pPciDev, pDevIns->pReg->szName, iDev);
        pdmUnlock(pVM);
        if (RT_SUCCESS(rc))
        {
            pPciDev->pDevIns = pDevIns;

            pDevIns->Internal.s.pPciDeviceR3 = pPciDev;
            if (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_R0)
                pDevIns->Internal.s.pPciDeviceR0 = MMHyperR3ToR0(pVM, pPciDev);
            else
                pDevIns->Internal.s.pPciDeviceR0 = NIL_RTR0PTR;

            if (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_RC)
                pDevIns->Internal.s.pPciDeviceRC = MMHyperR3ToRC(pVM, pPciDev);
            else
                pDevIns->Internal.s.pPciDeviceRC = NIL_RTRCPTR;

            Log(("PDM: Registered device '%s'/%d as PCI device %d on bus %d\n",
                 pDevIns->pReg->szName, pDevIns->iInstance, pPciDev->devfn, pDevIns->Internal.s.pPciBusR3->iBus));
        }
    }
    else
    {
        AssertLogRelMsgFailed(("Configuration error: No PCI bus available. This could be related to init order too!\n"));
        rc = VERR_PDM_NO_PCI_BUS;
    }

    LogFlow(("pdmR3DevHlp_PCIRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIIORegionRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_PCIIORegionRegister(PPDMDEVINS pDevIns, int iRegion, uint32_t cbRegion, PCIADDRESSSPACE enmType, PFNPCIIOREGIONMAP pfnCallback)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_PCIIORegionRegister: caller='%s'/%d: iRegion=%d cbRegion=%#x enmType=%d pfnCallback=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iRegion, cbRegion, enmType, pfnCallback));

    /*
     * Validate input.
     */
    if (iRegion < 0 || iRegion >= PCI_NUM_REGIONS)
    {
        Assert(iRegion >= 0 && iRegion < PCI_NUM_REGIONS);
        LogFlow(("pdmR3DevHlp_PCIIORegionRegister: caller='%s'/%d: returns %Rrc (iRegion)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    switch ((int)enmType)
    {
        case PCI_ADDRESS_SPACE_IO:
            /*
             * Sanity check: don't allow to register more than 32K of the PCI I/O space.
             */
            AssertMsgReturn(cbRegion <= _32K,
                            ("caller='%s'/%d: %#x\n", pDevIns->pReg->szName, pDevIns->iInstance, cbRegion),
                            VERR_INVALID_PARAMETER);
            break;

        case PCI_ADDRESS_SPACE_MEM:
        case PCI_ADDRESS_SPACE_MEM_PREFETCH:
        case PCI_ADDRESS_SPACE_MEM | PCI_ADDRESS_SPACE_BAR64:
        case PCI_ADDRESS_SPACE_MEM_PREFETCH | PCI_ADDRESS_SPACE_BAR64:
            /*
             * Sanity check: don't allow to register more than 512MB of the PCI MMIO space for
             * now. If this limit is increased beyond 2GB, adapt the aligned check below as well!
             */
            AssertMsgReturn(cbRegion <= 512 * _1M,
                            ("caller='%s'/%d: %#x\n", pDevIns->pReg->szName, pDevIns->iInstance, cbRegion),
                            VERR_INVALID_PARAMETER);
            break;
        default:
            AssertMsgFailed(("enmType=%#x is unknown\n", enmType));
            LogFlow(("pdmR3DevHlp_PCIIORegionRegister: caller='%s'/%d: returns %Rrc (enmType)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
            return VERR_INVALID_PARAMETER;
    }
    if (!pfnCallback)
    {
        Assert(pfnCallback);
        LogFlow(("pdmR3DevHlp_PCIIORegionRegister: caller='%s'/%d: returns %Rrc (callback)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    AssertRelease(VMR3GetState(pVM) != VMSTATE_RUNNING);

    /*
     * Must have a PCI device registered!
     */
    int rc;
    PPCIDEVICE pPciDev = pDevIns->Internal.s.pPciDeviceR3;
    if (pPciDev)
    {
        /*
         * We're currently restricted to page aligned MMIO regions.
         */
        if (    ((enmType & ~(PCI_ADDRESS_SPACE_BAR64 | PCI_ADDRESS_SPACE_MEM_PREFETCH)) == PCI_ADDRESS_SPACE_MEM)
            &&  cbRegion != RT_ALIGN_32(cbRegion, PAGE_SIZE))
        {
            Log(("pdmR3DevHlp_PCIIORegionRegister: caller='%s'/%d: aligning cbRegion %#x -> %#x\n",
                 pDevIns->pReg->szName, pDevIns->iInstance, cbRegion, RT_ALIGN_32(cbRegion, PAGE_SIZE)));
            cbRegion = RT_ALIGN_32(cbRegion, PAGE_SIZE);
        }

        /*
         * For registering PCI MMIO memory or PCI I/O memory, the size of the region must be a power of 2!
         */
        int iLastSet = ASMBitLastSetU32(cbRegion);
        Assert(iLastSet > 0);
        uint32_t cbRegionAligned = RT_BIT_32(iLastSet - 1);
        if (cbRegion > cbRegionAligned)
            cbRegion = cbRegionAligned * 2; /* round up */

        PPDMPCIBUS pBus = pDevIns->Internal.s.pPciBusR3;
        Assert(pBus);
        pdmLock(pVM);
        rc = pBus->pfnIORegionRegisterR3(pBus->pDevInsR3, pPciDev, iRegion, cbRegion, enmType, pfnCallback);
        pdmUnlock(pVM);
    }
    else
    {
        AssertMsgFailed(("No PCI device registered!\n"));
        rc = VERR_PDM_NOT_PCI_DEVICE;
    }

    LogFlow(("pdmR3DevHlp_PCIIORegionRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCISetConfigCallbacks} */
static DECLCALLBACK(void) pdmR3DevHlp_PCISetConfigCallbacks(PPDMDEVINS pDevIns, PPCIDEVICE pPciDev, PFNPCICONFIGREAD pfnRead, PPFNPCICONFIGREAD ppfnReadOld,
                                                            PFNPCICONFIGWRITE pfnWrite, PPFNPCICONFIGWRITE ppfnWriteOld)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_PCISetConfigCallbacks: caller='%s'/%d: pPciDev=%p pfnRead=%p ppfnReadOld=%p pfnWrite=%p ppfnWriteOld=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pPciDev, pfnRead, ppfnReadOld, pfnWrite, ppfnWriteOld));

    /*
     * Validate input and resolve defaults.
     */
    AssertPtr(pfnRead);
    AssertPtr(pfnWrite);
    AssertPtrNull(ppfnReadOld);
    AssertPtrNull(ppfnWriteOld);
    AssertPtrNull(pPciDev);

    if (!pPciDev)
        pPciDev = pDevIns->Internal.s.pPciDeviceR3;
    AssertReleaseMsg(pPciDev, ("You must register your device first!\n"));
    PPDMPCIBUS pBus = pDevIns->Internal.s.pPciBusR3;
    AssertRelease(pBus);
    AssertRelease(VMR3GetState(pVM) != VMSTATE_RUNNING);

    /*
     * Do the job.
     */
    pdmLock(pVM);
    pBus->pfnSetConfigCallbacksR3(pBus->pDevInsR3, pPciDev, pfnRead, ppfnReadOld, pfnWrite, ppfnWriteOld);
    pdmUnlock(pVM);

    LogFlow(("pdmR3DevHlp_PCISetConfigCallbacks: caller='%s'/%d: returns void\n", pDevIns->pReg->szName, pDevIns->iInstance));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIPhysRead} */
static DECLCALLBACK(int) pdmR3DevHlp_PCIPhysRead(PPDMDEVINS pDevIns, RTGCPHYS GCPhys, void *pvBuf, size_t cbRead)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);

#ifndef PDM_DO_NOT_RESPECT_PCI_BM_BIT
    /*
     * Just check the busmaster setting here and forward the request to the generic read helper.
     */
    PPCIDEVICE pPciDev = pDevIns->Internal.s.pPciDeviceR3;
    AssertReleaseMsg(pPciDev, ("No PCI device registered!\n"));

    if (!PCIDevIsBusmaster(pPciDev))
    {
        Log(("pdmR3DevHlp_PCIPhysRead: caller='%s'/%d: returns %Rrc - Not bus master! GCPhys=%RGp cbRead=%#zx\n",
             pDevIns->pReg->szName, pDevIns->iInstance, VERR_PDM_NOT_PCI_BUS_MASTER, GCPhys, cbRead));
        return VERR_PDM_NOT_PCI_BUS_MASTER;
    }
#endif

    return pDevIns->pHlpR3->pfnPhysRead(pDevIns, GCPhys, pvBuf, cbRead);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIPhysRead} */
static DECLCALLBACK(int) pdmR3DevHlp_PCIPhysWrite(PPDMDEVINS pDevIns, RTGCPHYS GCPhys, const void *pvBuf, size_t cbWrite)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);

#ifndef PDM_DO_NOT_RESPECT_PCI_BM_BIT
    /*
     * Just check the busmaster setting here and forward the request to the generic read helper.
     */
    PPCIDEVICE pPciDev = pDevIns->Internal.s.pPciDeviceR3;
    AssertReleaseMsg(pPciDev, ("No PCI device registered!\n"));

    if (!PCIDevIsBusmaster(pPciDev))
    {
        Log(("pdmR3DevHlp_PCIPhysWrite: caller='%s'/%d: returns %Rrc - Not bus master! GCPhys=%RGp cbWrite=%#zx\n",
             pDevIns->pReg->szName, pDevIns->iInstance, VERR_PDM_NOT_PCI_BUS_MASTER, GCPhys, cbWrite));
        return VERR_PDM_NOT_PCI_BUS_MASTER;
    }
#endif

    return pDevIns->pHlpR3->pfnPhysWrite(pDevIns, GCPhys, pvBuf, cbWrite);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCISetIrq} */
static DECLCALLBACK(void) pdmR3DevHlp_PCISetIrq(PPDMDEVINS pDevIns, int iIrq, int iLevel)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_PCISetIrq: caller='%s'/%d: iIrq=%d iLevel=%d\n", pDevIns->pReg->szName, pDevIns->iInstance, iIrq, iLevel));

    /*
     * Validate input.
     */
    Assert(iIrq == 0);
    Assert((uint32_t)iLevel <= PDM_IRQ_LEVEL_FLIP_FLOP);

    /*
     * Must have a PCI device registered!
     */
    PPCIDEVICE pPciDev = pDevIns->Internal.s.pPciDeviceR3;
    if (pPciDev)
    {
        PPDMPCIBUS pBus = pDevIns->Internal.s.pPciBusR3; /** @todo the bus should be associated with the PCI device not the PDM device. */
        Assert(pBus);
        PVM pVM = pDevIns->Internal.s.pVMR3;

        pdmLock(pVM);
        uint32_t uTagSrc;
        if (iLevel & PDM_IRQ_LEVEL_HIGH)
        {
            pDevIns->Internal.s.uLastIrqTag = uTagSrc = pdmCalcIrqTag(pVM, pDevIns->idTracing);
            if (iLevel == PDM_IRQ_LEVEL_HIGH)
                VBOXVMM_PDM_IRQ_HIGH(VMMGetCpu(pVM), RT_LOWORD(uTagSrc), RT_HIWORD(uTagSrc));
            else
                VBOXVMM_PDM_IRQ_HILO(VMMGetCpu(pVM), RT_LOWORD(uTagSrc), RT_HIWORD(uTagSrc));
        }
        else
            uTagSrc = pDevIns->Internal.s.uLastIrqTag;

        pBus->pfnSetIrqR3(pBus->pDevInsR3, pPciDev, iIrq, iLevel, uTagSrc);

        if (iLevel == PDM_IRQ_LEVEL_LOW)
            VBOXVMM_PDM_IRQ_LOW(VMMGetCpu(pVM), RT_LOWORD(uTagSrc), RT_HIWORD(uTagSrc));
        pdmUnlock(pVM);
    }
    else
        AssertReleaseMsgFailed(("No PCI device registered!\n"));

    LogFlow(("pdmR3DevHlp_PCISetIrq: caller='%s'/%d: returns void\n", pDevIns->pReg->szName, pDevIns->iInstance));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCISetIrqNoWait} */
static DECLCALLBACK(void) pdmR3DevHlp_PCISetIrqNoWait(PPDMDEVINS pDevIns, int iIrq, int iLevel)
{
    pdmR3DevHlp_PCISetIrq(pDevIns, iIrq, iLevel);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIRegisterMsi} */
static DECLCALLBACK(int) pdmR3DevHlp_PCIRegisterMsi(PPDMDEVINS pDevIns, PPDMMSIREG pMsiReg)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_PCIRegisterMsi: caller='%s'/%d: %d MSI vectors %d MSI-X vectors\n", pDevIns->pReg->szName, pDevIns->iInstance, pMsiReg->cMsiVectors,pMsiReg->cMsixVectors ));
    int rc = VINF_SUCCESS;

    /*
     * Must have a PCI device registered!
     */
    PPCIDEVICE pPciDev = pDevIns->Internal.s.pPciDeviceR3;
    if (pPciDev)
    {
        PPDMPCIBUS pBus = pDevIns->Internal.s.pPciBusR3; /** @todo the bus should be associated with the PCI device not the PDM device. */
        Assert(pBus);

        PVM pVM = pDevIns->Internal.s.pVMR3;
        pdmLock(pVM);
        if (pBus->pfnRegisterMsiR3)
            rc = pBus->pfnRegisterMsiR3(pBus->pDevInsR3, pPciDev, pMsiReg);
        else
            rc = VERR_NOT_IMPLEMENTED;
        pdmUnlock(pVM);
    }
    else
        AssertReleaseMsgFailed(("No PCI device registered!\n"));

    LogFlow(("pdmR3DevHlp_PCISetIrq: caller='%s'/%d: returns void\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return rc;
}

/** @interface_method_impl{PDMDEVHLPR3,pfnISASetIrq} */
static DECLCALLBACK(void) pdmR3DevHlp_ISASetIrq(PPDMDEVINS pDevIns, int iIrq, int iLevel)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_ISASetIrq: caller='%s'/%d: iIrq=%d iLevel=%d\n", pDevIns->pReg->szName, pDevIns->iInstance, iIrq, iLevel));

    /*
     * Validate input.
     */
    Assert(iIrq < 16);
    Assert((uint32_t)iLevel <= PDM_IRQ_LEVEL_FLIP_FLOP);

    PVM pVM = pDevIns->Internal.s.pVMR3;

    /*
     * Do the job.
     */
    pdmLock(pVM);
    uint32_t uTagSrc;
    if (iLevel & PDM_IRQ_LEVEL_HIGH)
    {
        pDevIns->Internal.s.uLastIrqTag = uTagSrc = pdmCalcIrqTag(pVM, pDevIns->idTracing);
        if (iLevel == PDM_IRQ_LEVEL_HIGH)
            VBOXVMM_PDM_IRQ_HIGH(VMMGetCpu(pVM), RT_LOWORD(uTagSrc), RT_HIWORD(uTagSrc));
        else
            VBOXVMM_PDM_IRQ_HILO(VMMGetCpu(pVM), RT_LOWORD(uTagSrc), RT_HIWORD(uTagSrc));
    }
    else
        uTagSrc = pDevIns->Internal.s.uLastIrqTag;

    PDMIsaSetIrq(pVM, iIrq, iLevel, uTagSrc);  /* (The API takes the lock recursively.) */

    if (iLevel == PDM_IRQ_LEVEL_LOW)
        VBOXVMM_PDM_IRQ_LOW(VMMGetCpu(pVM), RT_LOWORD(uTagSrc), RT_HIWORD(uTagSrc));
    pdmUnlock(pVM);

    LogFlow(("pdmR3DevHlp_ISASetIrq: caller='%s'/%d: returns void\n", pDevIns->pReg->szName, pDevIns->iInstance));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnISASetIrqNoWait} */
static DECLCALLBACK(void) pdmR3DevHlp_ISASetIrqNoWait(PPDMDEVINS pDevIns, int iIrq, int iLevel)
{
    pdmR3DevHlp_ISASetIrq(pDevIns, iIrq, iLevel);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDriverAttach} */
static DECLCALLBACK(int) pdmR3DevHlp_DriverAttach(PPDMDEVINS pDevIns, uint32_t iLun, PPDMIBASE pBaseInterface, PPDMIBASE *ppBaseInterface, const char *pszDesc)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_DriverAttach: caller='%s'/%d: iLun=%d pBaseInterface=%p ppBaseInterface=%p pszDesc=%p:{%s}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iLun, pBaseInterface, ppBaseInterface, pszDesc, pszDesc));

    /*
     * Lookup the LUN, it might already be registered.
     */
    PPDMLUN pLunPrev = NULL;
    PPDMLUN pLun = pDevIns->Internal.s.pLunsR3;
    for (; pLun; pLunPrev = pLun, pLun = pLun->pNext)
        if (pLun->iLun == iLun)
            break;

    /*
     * Create the LUN if if wasn't found, else check if driver is already attached to it.
     */
    if (!pLun)
    {
        if (    !pBaseInterface
            ||  !pszDesc
            ||  !*pszDesc)
        {
            Assert(pBaseInterface);
            Assert(pszDesc || *pszDesc);
            return VERR_INVALID_PARAMETER;
        }

        pLun = (PPDMLUN)MMR3HeapAlloc(pVM, MM_TAG_PDM_LUN, sizeof(*pLun));
        if (!pLun)
            return VERR_NO_MEMORY;

        pLun->iLun      = iLun;
        pLun->pNext     = pLunPrev ? pLunPrev->pNext : NULL;
        pLun->pTop      = NULL;
        pLun->pBottom   = NULL;
        pLun->pDevIns   = pDevIns;
        pLun->pUsbIns   = NULL;
        pLun->pszDesc   = pszDesc;
        pLun->pBase     = pBaseInterface;
        if (!pLunPrev)
            pDevIns->Internal.s.pLunsR3 = pLun;
        else
            pLunPrev->pNext = pLun;
        Log(("pdmR3DevHlp_DriverAttach: Registered LUN#%d '%s' with device '%s'/%d.\n",
             iLun, pszDesc, pDevIns->pReg->szName, pDevIns->iInstance));
    }
    else if (pLun->pTop)
    {
        AssertMsgFailed(("Already attached! The device should keep track of such things!\n"));
        LogFlow(("pdmR3DevHlp_DriverAttach: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_PDM_DRIVER_ALREADY_ATTACHED));
        return VERR_PDM_DRIVER_ALREADY_ATTACHED;
    }
    Assert(pLun->pBase == pBaseInterface);


    /*
     * Get the attached driver configuration.
     */
    int rc;
    PCFGMNODE pNode = CFGMR3GetChildF(pDevIns->Internal.s.pCfgHandle, "LUN#%u", iLun);
    if (pNode)
        rc = pdmR3DrvInstantiate(pVM, pNode, pBaseInterface, NULL /*pDrvAbove*/, pLun, ppBaseInterface);
    else
        rc = VERR_PDM_NO_ATTACHED_DRIVER;


    LogFlow(("pdmR3DevHlp_DriverAttach: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnQueueCreate} */
static DECLCALLBACK(int) pdmR3DevHlp_QueueCreate(PPDMDEVINS pDevIns, size_t cbItem, uint32_t cItems, uint32_t cMilliesInterval,
                                                 PFNPDMQUEUEDEV pfnCallback, bool fGCEnabled, const char *pszName, PPDMQUEUE *ppQueue)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_QueueCreate: caller='%s'/%d: cbItem=%#x cItems=%#x cMilliesInterval=%u pfnCallback=%p fGCEnabled=%RTbool pszName=%p:{%s} ppQueue=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, cbItem, cItems, cMilliesInterval, pfnCallback, fGCEnabled, pszName, pszName, ppQueue));

    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);

    if (pDevIns->iInstance > 0)
    {
        pszName = MMR3HeapAPrintf(pVM, MM_TAG_PDM_DEVICE_DESC, "%s_%u", pszName, pDevIns->iInstance);
        AssertLogRelReturn(pszName, VERR_NO_MEMORY);
    }

    int rc = PDMR3QueueCreateDevice(pVM, pDevIns, cbItem, cItems, cMilliesInterval, pfnCallback, fGCEnabled, pszName, ppQueue);

    LogFlow(("pdmR3DevHlp_QueueCreate: caller='%s'/%d: returns %Rrc *ppQueue=%p\n", pDevIns->pReg->szName, pDevIns->iInstance, rc, *ppQueue));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCritSectInit} */
static DECLCALLBACK(int) pdmR3DevHlp_CritSectInit(PPDMDEVINS pDevIns, PPDMCRITSECT pCritSect, RT_SRC_POS_DECL,
                                                  const char *pszNameFmt, va_list va)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_CritSectInit: caller='%s'/%d: pCritSect=%p pszNameFmt=%p:{%s}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pCritSect, pszNameFmt, pszNameFmt));

    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    int rc = pdmR3CritSectInitDevice(pVM, pDevIns, pCritSect, RT_SRC_POS_ARGS, pszNameFmt, va);

    LogFlow(("pdmR3DevHlp_CritSectInit: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCritSectGetNop} */
static DECLCALLBACK(PPDMCRITSECT) pdmR3DevHlp_CritSectGetNop(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);

    PPDMCRITSECT pCritSect = PDMR3CritSectGetNop(pVM);
    LogFlow(("pdmR3DevHlp_CritSectGetNop: caller='%s'/%d: return %p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pCritSect));
    return pCritSect;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCritSectGetNopR0} */
static DECLCALLBACK(R0PTRTYPE(PPDMCRITSECT)) pdmR3DevHlp_CritSectGetNopR0(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);

    R0PTRTYPE(PPDMCRITSECT) pCritSect = PDMR3CritSectGetNopR0(pVM);
    LogFlow(("pdmR3DevHlp_CritSectGetNopR0: caller='%s'/%d: return %RHv\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pCritSect));
    return pCritSect;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCritSectGetNopRC} */
static DECLCALLBACK(RCPTRTYPE(PPDMCRITSECT)) pdmR3DevHlp_CritSectGetNopRC(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);

    RCPTRTYPE(PPDMCRITSECT) pCritSect = PDMR3CritSectGetNopRC(pVM);
    LogFlow(("pdmR3DevHlp_CritSectGetNopRC: caller='%s'/%d: return %RRv\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pCritSect));
    return pCritSect;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSetDeviceCritSect} */
static DECLCALLBACK(int) pdmR3DevHlp_SetDeviceCritSect(PPDMDEVINS pDevIns, PPDMCRITSECT pCritSect)
{
    /*
     * Validate input.
     *
     * Note! We only allow the automatically created default critical section
     *       to be replaced by this API.
     */
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertPtrReturn(pCritSect, VERR_INVALID_POINTER);
    LogFlow(("pdmR3DevHlp_SetDeviceCritSect: caller='%s'/%d: pCritSect=%p (%s)\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pCritSect, pCritSect->s.pszName));
    AssertReturn(PDMCritSectIsInitialized(pCritSect), VERR_INVALID_PARAMETER);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    AssertReturn(pCritSect->s.pVMR3 == pVM, VERR_INVALID_PARAMETER);

    VM_ASSERT_EMT(pVM);
    VM_ASSERT_STATE_RETURN(pVM, VMSTATE_CREATING, VERR_WRONG_ORDER);

    AssertReturn(pDevIns->pCritSectRoR3, VERR_PDM_DEV_IPE_1);
    AssertReturn(pDevIns->pCritSectRoR3->s.fAutomaticDefaultCritsect, VERR_WRONG_ORDER);
    AssertReturn(!pDevIns->pCritSectRoR3->s.fUsedByTimerOrSimilar, VERR_WRONG_ORDER);
    AssertReturn(pDevIns->pCritSectRoR3 != pCritSect, VERR_INVALID_PARAMETER);

    /*
     * Replace the critical section and destroy the automatic default section.
     */
    PPDMCRITSECT pOldCritSect = pDevIns->pCritSectRoR3;
    pDevIns->pCritSectRoR3 = pCritSect;
    if (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_R0)
        pDevIns->pCritSectRoR0 = MMHyperCCToR0(pVM, pDevIns->pCritSectRoR3);
    else
        Assert(pDevIns->pCritSectRoR0 == NIL_RTRCPTR);

    if (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_RC)
        pDevIns->pCritSectRoRC = MMHyperCCToRC(pVM, pDevIns->pCritSectRoR3);
    else
        Assert(pDevIns->pCritSectRoRC == NIL_RTRCPTR);

    PDMR3CritSectDelete(pOldCritSect);
    if (pDevIns->pReg->fFlags & (PDM_DEVREG_FLAGS_RC | PDM_DEVREG_FLAGS_R0))
        MMHyperFree(pVM, pOldCritSect);
    else
        MMR3HeapFree(pOldCritSect);

    LogFlow(("pdmR3DevHlp_SetDeviceCritSect: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VINF_SUCCESS));
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnThreadCreate} */
static DECLCALLBACK(int) pdmR3DevHlp_ThreadCreate(PPDMDEVINS pDevIns, PPPDMTHREAD ppThread, void *pvUser, PFNPDMTHREADDEV pfnThread,
                                                  PFNPDMTHREADWAKEUPDEV pfnWakeup, size_t cbStack, RTTHREADTYPE enmType, const char *pszName)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_ThreadCreate: caller='%s'/%d: ppThread=%p pvUser=%p pfnThread=%p pfnWakeup=%p cbStack=%#zx enmType=%d pszName=%p:{%s}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, ppThread, pvUser, pfnThread, pfnWakeup, cbStack, enmType, pszName, pszName));

    int rc = pdmR3ThreadCreateDevice(pDevIns->Internal.s.pVMR3, pDevIns, ppThread, pvUser, pfnThread, pfnWakeup, cbStack, enmType, pszName);

    LogFlow(("pdmR3DevHlp_ThreadCreate: caller='%s'/%d: returns %Rrc *ppThread=%RTthrd\n", pDevIns->pReg->szName, pDevIns->iInstance,
            rc, *ppThread));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnSetAsyncNotification} */
static DECLCALLBACK(int) pdmR3DevHlp_SetAsyncNotification(PPDMDEVINS pDevIns, PFNPDMDEVASYNCNOTIFY pfnAsyncNotify)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT0(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_SetAsyncNotification: caller='%s'/%d: pfnAsyncNotify=%p\n", pDevIns->pReg->szName, pDevIns->iInstance, pfnAsyncNotify));

    int rc = VINF_SUCCESS;
    AssertStmt(pfnAsyncNotify, rc = VERR_INVALID_PARAMETER);
    AssertStmt(!pDevIns->Internal.s.pfnAsyncNotify, rc = VERR_WRONG_ORDER);
    AssertStmt(pDevIns->Internal.s.fIntFlags & (PDMDEVINSINT_FLAGS_SUSPENDED | PDMDEVINSINT_FLAGS_RESET), rc = VERR_WRONG_ORDER);
    VMSTATE enmVMState = VMR3GetState(pDevIns->Internal.s.pVMR3);
    AssertStmt(   enmVMState == VMSTATE_SUSPENDING
               || enmVMState == VMSTATE_SUSPENDING_EXT_LS
               || enmVMState == VMSTATE_SUSPENDING_LS
               || enmVMState == VMSTATE_RESETTING
               || enmVMState == VMSTATE_RESETTING_LS
               || enmVMState == VMSTATE_POWERING_OFF
               || enmVMState == VMSTATE_POWERING_OFF_LS,
               rc = VERR_INVALID_STATE);

    if (RT_SUCCESS(rc))
        pDevIns->Internal.s.pfnAsyncNotify = pfnAsyncNotify;

    LogFlow(("pdmR3DevHlp_SetAsyncNotification: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnAsyncNotificationCompleted} */
static DECLCALLBACK(void) pdmR3DevHlp_AsyncNotificationCompleted(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;

    VMSTATE enmVMState = VMR3GetState(pVM);
    if (   enmVMState == VMSTATE_SUSPENDING
        || enmVMState == VMSTATE_SUSPENDING_EXT_LS
        || enmVMState == VMSTATE_SUSPENDING_LS
        || enmVMState == VMSTATE_RESETTING
        || enmVMState == VMSTATE_RESETTING_LS
        || enmVMState == VMSTATE_POWERING_OFF
        || enmVMState == VMSTATE_POWERING_OFF_LS)
    {
        LogFlow(("pdmR3DevHlp_AsyncNotificationCompleted: caller='%s'/%d:\n", pDevIns->pReg->szName, pDevIns->iInstance));
        VMR3AsyncPdmNotificationWakeupU(pVM->pUVM);
    }
    else
        LogFlow(("pdmR3DevHlp_AsyncNotificationCompleted: caller='%s'/%d: enmVMState=%d\n", pDevIns->pReg->szName, pDevIns->iInstance, enmVMState));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnRTCRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_RTCRegister(PPDMDEVINS pDevIns, PCPDMRTCREG pRtcReg, PCPDMRTCHLP *ppRtcHlp)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_RTCRegister: caller='%s'/%d: pRtcReg=%p:{.u32Version=%#x, .pfnWrite=%p, .pfnRead=%p} ppRtcHlp=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pRtcReg, pRtcReg->u32Version, pRtcReg->pfnWrite,
             pRtcReg->pfnWrite, ppRtcHlp));

    /*
     * Validate input.
     */
    if (pRtcReg->u32Version != PDM_RTCREG_VERSION)
    {
        AssertMsgFailed(("u32Version=%#x expected %#x\n", pRtcReg->u32Version,
                         PDM_RTCREG_VERSION));
        LogFlow(("pdmR3DevHlp_RTCRegister: caller='%s'/%d: returns %Rrc (version)\n",
                 pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    !pRtcReg->pfnWrite
        ||  !pRtcReg->pfnRead)
    {
        Assert(pRtcReg->pfnWrite);
        Assert(pRtcReg->pfnRead);
        LogFlow(("pdmR3DevHlp_RTCRegister: caller='%s'/%d: returns %Rrc (callbacks)\n",
                 pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    if (!ppRtcHlp)
    {
        Assert(ppRtcHlp);
        LogFlow(("pdmR3DevHlp_RTCRegister: caller='%s'/%d: returns %Rrc (ppRtcHlp)\n",
                 pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Only one DMA device.
     */
    PVM pVM = pDevIns->Internal.s.pVMR3;
    if (pVM->pdm.s.pRtc)
    {
        AssertMsgFailed(("Only one RTC device is supported!\n"));
        LogFlow(("pdmR3DevHlp_RTCRegister: caller='%s'/%d: returns %Rrc\n",
                 pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Allocate and initialize pci bus structure.
     */
    int rc = VINF_SUCCESS;
    PPDMRTC pRtc = (PPDMRTC)MMR3HeapAlloc(pDevIns->Internal.s.pVMR3, MM_TAG_PDM_DEVICE, sizeof(*pRtc));
    if (pRtc)
    {
        pRtc->pDevIns   = pDevIns;
        pRtc->Reg       = *pRtcReg;
        pVM->pdm.s.pRtc = pRtc;

        /* set the helper pointer. */
        *ppRtcHlp = &g_pdmR3DevRtcHlp;
        Log(("PDM: Registered RTC device '%s'/%d pDevIns=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pDevIns));
    }
    else
        rc = VERR_NO_MEMORY;

    LogFlow(("pdmR3DevHlp_RTCRegister: caller='%s'/%d: returns %Rrc\n",
             pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDMARegister} */
static DECLCALLBACK(int) pdmR3DevHlp_DMARegister(PPDMDEVINS pDevIns, unsigned uChannel, PFNDMATRANSFERHANDLER pfnTransferHandler, void *pvUser)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_DMARegister: caller='%s'/%d: uChannel=%d pfnTransferHandler=%p pvUser=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, uChannel, pfnTransferHandler, pvUser));
    int rc = VINF_SUCCESS;
    if (pVM->pdm.s.pDmac)
        pVM->pdm.s.pDmac->Reg.pfnRegister(pVM->pdm.s.pDmac->pDevIns, uChannel, pfnTransferHandler, pvUser);
    else
    {
        AssertMsgFailed(("Configuration error: No DMAC controller available. This could be related to init order too!\n"));
        rc = VERR_PDM_NO_DMAC_INSTANCE;
    }
    LogFlow(("pdmR3DevHlp_DMARegister: caller='%s'/%d: returns %Rrc\n",
             pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDMAReadMemory} */
static DECLCALLBACK(int) pdmR3DevHlp_DMAReadMemory(PPDMDEVINS pDevIns, unsigned uChannel, void *pvBuffer, uint32_t off, uint32_t cbBlock, uint32_t *pcbRead)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_DMAReadMemory: caller='%s'/%d: uChannel=%d pvBuffer=%p off=%#x cbBlock=%#x pcbRead=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, uChannel, pvBuffer, off, cbBlock, pcbRead));
    int rc = VINF_SUCCESS;
    if (pVM->pdm.s.pDmac)
    {
        uint32_t cb = pVM->pdm.s.pDmac->Reg.pfnReadMemory(pVM->pdm.s.pDmac->pDevIns, uChannel, pvBuffer, off, cbBlock);
        if (pcbRead)
            *pcbRead = cb;
    }
    else
    {
        AssertMsgFailed(("Configuration error: No DMAC controller available. This could be related to init order too!\n"));
        rc = VERR_PDM_NO_DMAC_INSTANCE;
    }
    LogFlow(("pdmR3DevHlp_DMAReadMemory: caller='%s'/%d: returns %Rrc\n",
             pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDMAWriteMemory} */
static DECLCALLBACK(int) pdmR3DevHlp_DMAWriteMemory(PPDMDEVINS pDevIns, unsigned uChannel, const void *pvBuffer, uint32_t off, uint32_t cbBlock, uint32_t *pcbWritten)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_DMAWriteMemory: caller='%s'/%d: uChannel=%d pvBuffer=%p off=%#x cbBlock=%#x pcbWritten=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, uChannel, pvBuffer, off, cbBlock, pcbWritten));
    int rc = VINF_SUCCESS;
    if (pVM->pdm.s.pDmac)
    {
        uint32_t cb = pVM->pdm.s.pDmac->Reg.pfnWriteMemory(pVM->pdm.s.pDmac->pDevIns, uChannel, pvBuffer, off, cbBlock);
        if (pcbWritten)
            *pcbWritten = cb;
    }
    else
    {
        AssertMsgFailed(("Configuration error: No DMAC controller available. This could be related to init order too!\n"));
        rc = VERR_PDM_NO_DMAC_INSTANCE;
    }
    LogFlow(("pdmR3DevHlp_DMAWriteMemory: caller='%s'/%d: returns %Rrc\n",
             pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDMASetDREQ} */
static DECLCALLBACK(int) pdmR3DevHlp_DMASetDREQ(PPDMDEVINS pDevIns, unsigned uChannel, unsigned uLevel)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_DMASetDREQ: caller='%s'/%d: uChannel=%d uLevel=%d\n",
             pDevIns->pReg->szName, pDevIns->iInstance, uChannel, uLevel));
    int rc = VINF_SUCCESS;
    if (pVM->pdm.s.pDmac)
        pVM->pdm.s.pDmac->Reg.pfnSetDREQ(pVM->pdm.s.pDmac->pDevIns, uChannel, uLevel);
    else
    {
        AssertMsgFailed(("Configuration error: No DMAC controller available. This could be related to init order too!\n"));
        rc = VERR_PDM_NO_DMAC_INSTANCE;
    }
    LogFlow(("pdmR3DevHlp_DMASetDREQ: caller='%s'/%d: returns %Rrc\n",
             pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}

/** @interface_method_impl{PDMDEVHLPR3,pfnDMAGetChannelMode} */
static DECLCALLBACK(uint8_t) pdmR3DevHlp_DMAGetChannelMode(PPDMDEVINS pDevIns, unsigned uChannel)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_DMAGetChannelMode: caller='%s'/%d: uChannel=%d\n",
             pDevIns->pReg->szName, pDevIns->iInstance, uChannel));
    uint8_t u8Mode;
    if (pVM->pdm.s.pDmac)
        u8Mode = pVM->pdm.s.pDmac->Reg.pfnGetChannelMode(pVM->pdm.s.pDmac->pDevIns, uChannel);
    else
    {
        AssertMsgFailed(("Configuration error: No DMAC controller available. This could be related to init order too!\n"));
        u8Mode = 3 << 2 /* illegal mode type */;
    }
    LogFlow(("pdmR3DevHlp_DMAGetChannelMode: caller='%s'/%d: returns %#04x\n",
             pDevIns->pReg->szName, pDevIns->iInstance, u8Mode));
    return u8Mode;
}

/** @interface_method_impl{PDMDEVHLPR3,pfnDMASchedule} */
static DECLCALLBACK(void) pdmR3DevHlp_DMASchedule(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_DMASchedule: caller='%s'/%d: VM_FF_PDM_DMA %d -> 1\n",
             pDevIns->pReg->szName, pDevIns->iInstance, VM_FF_IS_SET(pVM, VM_FF_PDM_DMA)));

    AssertMsg(pVM->pdm.s.pDmac, ("Configuration error: No DMAC controller available. This could be related to init order too!\n"));
    VM_FF_SET(pVM, VM_FF_PDM_DMA);
#ifdef VBOX_WITH_REM
    REMR3NotifyDmaPending(pVM);
#endif
    VMR3NotifyGlobalFFU(pVM->pUVM, VMNOTIFYFF_FLAGS_DONE_REM);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCMOSWrite} */
static DECLCALLBACK(int) pdmR3DevHlp_CMOSWrite(PPDMDEVINS pDevIns, unsigned iReg, uint8_t u8Value)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);

    LogFlow(("pdmR3DevHlp_CMOSWrite: caller='%s'/%d: iReg=%#04x u8Value=%#04x\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iReg, u8Value));
    int rc;
    if (pVM->pdm.s.pRtc)
    {
        PPDMDEVINS pDevInsRtc = pVM->pdm.s.pRtc->pDevIns;
        rc = PDMCritSectEnter(pDevInsRtc->pCritSectRoR3, VERR_IGNORED);
        if (RT_SUCCESS(rc))
        {
            rc = pVM->pdm.s.pRtc->Reg.pfnWrite(pDevInsRtc, iReg, u8Value);
            PDMCritSectLeave(pDevInsRtc->pCritSectRoR3);
        }
    }
    else
        rc = VERR_PDM_NO_RTC_INSTANCE;

    LogFlow(("pdmR3DevHlp_CMOSWrite: caller='%s'/%d: return %Rrc\n",
             pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnCMOSRead} */
static DECLCALLBACK(int) pdmR3DevHlp_CMOSRead(PPDMDEVINS pDevIns, unsigned iReg, uint8_t *pu8Value)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);

    LogFlow(("pdmR3DevHlp_CMOSWrite: caller='%s'/%d: iReg=%#04x pu8Value=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iReg, pu8Value));
    int rc;
    if (pVM->pdm.s.pRtc)
    {
        PPDMDEVINS pDevInsRtc = pVM->pdm.s.pRtc->pDevIns;
        rc = PDMCritSectEnter(pDevInsRtc->pCritSectRoR3, VERR_IGNORED);
        if (RT_SUCCESS(rc))
        {
            rc = pVM->pdm.s.pRtc->Reg.pfnRead(pDevInsRtc, iReg, pu8Value);
            PDMCritSectLeave(pDevInsRtc->pCritSectRoR3);
        }
    }
    else
        rc = VERR_PDM_NO_RTC_INSTANCE;

    LogFlow(("pdmR3DevHlp_CMOSWrite: caller='%s'/%d: return %Rrc\n",
             pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnAssertEMT} */
static DECLCALLBACK(bool) pdmR3DevHlp_AssertEMT(PPDMDEVINS pDevIns, const char *pszFile, unsigned iLine, const char *pszFunction)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    if (VM_IS_EMT(pDevIns->Internal.s.pVMR3))
        return true;

    char szMsg[100];
    RTStrPrintf(szMsg, sizeof(szMsg), "AssertEMT '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance);
    RTAssertMsg1Weak(szMsg, iLine, pszFile, pszFunction);
    AssertBreakpoint();
    return false;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnAssertOther} */
static DECLCALLBACK(bool) pdmR3DevHlp_AssertOther(PPDMDEVINS pDevIns, const char *pszFile, unsigned iLine, const char *pszFunction)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    if (!VM_IS_EMT(pDevIns->Internal.s.pVMR3))
        return true;

    char szMsg[100];
    RTStrPrintf(szMsg, sizeof(szMsg), "AssertOther '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance);
    RTAssertMsg1Weak(szMsg, iLine, pszFile, pszFunction);
    AssertBreakpoint();
    return false;
}


/** @interface_method_impl{PDMDEVHLP,pfnLdrGetRCInterfaceSymbols} */
static DECLCALLBACK(int) pdmR3DevHlp_LdrGetRCInterfaceSymbols(PPDMDEVINS pDevIns, void *pvInterface, size_t cbInterface,
                                                              const char *pszSymPrefix, const char *pszSymList)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_PDMLdrGetRCInterfaceSymbols: caller='%s'/%d: pvInterface=%p cbInterface=%zu pszSymPrefix=%p:{%s} pszSymList=%p:{%s}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pvInterface, cbInterface, pszSymPrefix, pszSymPrefix, pszSymList, pszSymList));

    int rc;
    if (   strncmp(pszSymPrefix, "dev", 3) == 0
        && RTStrIStr(pszSymPrefix + 3, pDevIns->pReg->szName) != NULL)
    {
        if (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_RC)
            rc = PDMR3LdrGetInterfaceSymbols(pDevIns->Internal.s.pVMR3,
                                             pvInterface, cbInterface,
                                             pDevIns->pReg->szRCMod, pDevIns->Internal.s.pDevR3->pszRCSearchPath,
                                             pszSymPrefix, pszSymList,
                                             false /*fRing0OrRC*/);
        else
        {
            AssertMsgFailed(("Not a raw-mode enabled driver\n"));
            rc = VERR_PERMISSION_DENIED;
        }
    }
    else
    {
        AssertMsgFailed(("Invalid prefix '%s' for '%s'; must start with 'dev' and contain the driver name!\n",
                         pszSymPrefix, pDevIns->pReg->szName));
        rc = VERR_INVALID_NAME;
    }

    LogFlow(("pdmR3DevHlp_PDMLdrGetRCInterfaceSymbols: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName,
             pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLP,pfnLdrGetR0InterfaceSymbols} */
static DECLCALLBACK(int) pdmR3DevHlp_LdrGetR0InterfaceSymbols(PPDMDEVINS pDevIns, void *pvInterface, size_t cbInterface,
                                                              const char *pszSymPrefix, const char *pszSymList)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_PDMLdrGetR0InterfaceSymbols: caller='%s'/%d: pvInterface=%p cbInterface=%zu pszSymPrefix=%p:{%s} pszSymList=%p:{%s}\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pvInterface, cbInterface, pszSymPrefix, pszSymPrefix, pszSymList, pszSymList));

    int rc;
    if (   strncmp(pszSymPrefix, "dev", 3) == 0
        && RTStrIStr(pszSymPrefix + 3, pDevIns->pReg->szName) != NULL)
    {
        if (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_R0)
            rc = PDMR3LdrGetInterfaceSymbols(pDevIns->Internal.s.pVMR3,
                                             pvInterface, cbInterface,
                                             pDevIns->pReg->szR0Mod, pDevIns->Internal.s.pDevR3->pszR0SearchPath,
                                             pszSymPrefix, pszSymList,
                                             true /*fRing0OrRC*/);
        else
        {
            AssertMsgFailed(("Not a ring-0 enabled driver\n"));
            rc = VERR_PERMISSION_DENIED;
        }
    }
    else
    {
        AssertMsgFailed(("Invalid prefix '%s' for '%s'; must start with 'dev' and contain the driver name!\n",
                         pszSymPrefix, pDevIns->pReg->szName));
        rc = VERR_INVALID_NAME;
    }

    LogFlow(("pdmR3DevHlp_PDMLdrGetR0InterfaceSymbols: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName,
             pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLP,pfnCallR0} */
static DECLCALLBACK(int) pdmR3DevHlp_CallR0(PPDMDEVINS pDevIns, uint32_t uOperation, uint64_t u64Arg)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_CallR0: caller='%s'/%d: uOperation=%#x u64Arg=%#RX64\n",
             pDevIns->pReg->szName, pDevIns->iInstance, uOperation, u64Arg));

    /*
     * Resolve the ring-0 entry point.  There is not need to remember this like
     * we do for drivers since this is mainly for construction time hacks and
     * other things that aren't performance critical.
     */
    int rc;
    if (pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_R0)
    {
        char szSymbol[          sizeof("devR0") + sizeof(pDevIns->pReg->szName) + sizeof("ReqHandler")];
        strcat(strcat(strcpy(szSymbol, "devR0"),         pDevIns->pReg->szName),         "ReqHandler");
        szSymbol[sizeof("devR0") - 1] = RT_C_TO_UPPER(szSymbol[sizeof("devR0") - 1]);

        PFNPDMDRVREQHANDLERR0 pfnReqHandlerR0;
        rc = pdmR3DevGetSymbolR0Lazy(pDevIns, szSymbol, &pfnReqHandlerR0);
        if (RT_SUCCESS(rc))
        {
            /*
             * Make the ring-0 call.
             */
            PDMDEVICECALLREQHANDLERREQ Req;
            Req.Hdr.u32Magic    = SUPVMMR0REQHDR_MAGIC;
            Req.Hdr.cbReq       = sizeof(Req);
            Req.pDevInsR0       = PDMDEVINS_2_R0PTR(pDevIns);
            Req.pfnReqHandlerR0 = pfnReqHandlerR0;
            Req.uOperation      = uOperation;
            Req.u32Alignment    = 0;
            Req.u64Arg          = u64Arg;
            rc = SUPR3CallVMMR0Ex(pVM->pVMR0, NIL_VMCPUID, VMMR0_DO_PDM_DEVICE_CALL_REQ_HANDLER, 0, &Req.Hdr);
        }
        else
            pfnReqHandlerR0 = NIL_RTR0PTR;
    }
    else
        rc = VERR_ACCESS_DENIED;
    LogFlow(("pdmR3DevHlp_CallR0: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName,
             pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLP,pfnVMGetSuspendReason} */
static DECLCALLBACK(VMSUSPENDREASON) pdmR3DevHlp_VMGetSuspendReason(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    VMSUSPENDREASON enmReason = VMR3GetSuspendReason(pVM->pUVM);
    LogFlow(("pdmR3DevHlp_VMGetSuspendReason: caller='%s'/%d: returns %d\n",
             pDevIns->pReg->szName, pDevIns->iInstance, enmReason));
    return enmReason;
}


/** @interface_method_impl{PDMDEVHLP,pfnVMGetResumeReason} */
static DECLCALLBACK(VMRESUMEREASON) pdmR3DevHlp_VMGetResumeReason(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    VMRESUMEREASON enmReason = VMR3GetResumeReason(pVM->pUVM);
    LogFlow(("pdmR3DevHlp_VMGetResumeReason: caller='%s'/%d: returns %d\n",
             pDevIns->pReg->szName, pDevIns->iInstance, enmReason));
    return enmReason;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetUVM} */
static DECLCALLBACK(PUVM) pdmR3DevHlp_GetUVM(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_GetUVM: caller='%s'/%d: returns %p\n", pDevIns->pReg->szName, pDevIns->iInstance, pDevIns->Internal.s.pVMR3));
    return pDevIns->Internal.s.pVMR3->pUVM;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetVM} */
static DECLCALLBACK(PVM) pdmR3DevHlp_GetVM(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    LogFlow(("pdmR3DevHlp_GetVM: caller='%s'/%d: returns %p\n", pDevIns->pReg->szName, pDevIns->iInstance, pDevIns->Internal.s.pVMR3));
    return pDevIns->Internal.s.pVMR3;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetVMCPU} */
static DECLCALLBACK(PVMCPU) pdmR3DevHlp_GetVMCPU(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_GetVMCPU: caller='%s'/%d for CPU %u\n", pDevIns->pReg->szName, pDevIns->iInstance, VMMGetCpuId(pDevIns->Internal.s.pVMR3)));
    return VMMGetCpu(pDevIns->Internal.s.pVMR3);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetCurrentCpuId} */
static DECLCALLBACK(VMCPUID) pdmR3DevHlp_GetCurrentCpuId(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VMCPUID idCpu = VMMGetCpuId(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_GetCurrentCpuId: caller='%s'/%d for CPU %u\n", pDevIns->pReg->szName, pDevIns->iInstance, idCpu));
    return idCpu;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPCIBusRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_PCIBusRegister(PPDMDEVINS pDevIns, PPDMPCIBUSREG pPciBusReg, PCPDMPCIHLPR3 *ppPciHlpR3)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_PCIBusRegister: caller='%s'/%d: pPciBusReg=%p:{.u32Version=%#x, .pfnRegisterR3=%p, .pfnIORegionRegisterR3=%p, "
             ".pfnSetIrqR3=%p, .pfnFakePCIBIOSR3=%p, .pszSetIrqRC=%p:{%s}, .pszSetIrqR0=%p:{%s}} ppPciHlpR3=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pPciBusReg, pPciBusReg->u32Version, pPciBusReg->pfnRegisterR3,
             pPciBusReg->pfnIORegionRegisterR3, pPciBusReg->pfnSetIrqR3, pPciBusReg->pfnFakePCIBIOSR3,
             pPciBusReg->pszSetIrqRC, pPciBusReg->pszSetIrqRC, pPciBusReg->pszSetIrqR0, pPciBusReg->pszSetIrqR0, ppPciHlpR3));

    /*
     * Validate the structure.
     */
    if (pPciBusReg->u32Version != PDM_PCIBUSREG_VERSION)
    {
        AssertMsgFailed(("u32Version=%#x expected %#x\n", pPciBusReg->u32Version, PDM_PCIBUSREG_VERSION));
        LogFlow(("pdmR3DevHlp_PCIRegister: caller='%s'/%d: returns %Rrc (version)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    !pPciBusReg->pfnRegisterR3
        ||  !pPciBusReg->pfnIORegionRegisterR3
        ||  !pPciBusReg->pfnSetIrqR3
        ||  (!pPciBusReg->pfnFakePCIBIOSR3 && !pVM->pdm.s.aPciBuses[0].pDevInsR3)) /* Only the first bus needs to do the BIOS work. */
    {
        Assert(pPciBusReg->pfnRegisterR3);
        Assert(pPciBusReg->pfnIORegionRegisterR3);
        Assert(pPciBusReg->pfnSetIrqR3);
        Assert(pPciBusReg->pfnFakePCIBIOSR3);
        LogFlow(("pdmR3DevHlp_PCIBusRegister: caller='%s'/%d: returns %Rrc (R3 callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    pPciBusReg->pszSetIrqRC
        &&  !VALID_PTR(pPciBusReg->pszSetIrqRC))
    {
        Assert(VALID_PTR(pPciBusReg->pszSetIrqRC));
        LogFlow(("pdmR3DevHlp_PCIBusRegister: caller='%s'/%d: returns %Rrc (GC callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    pPciBusReg->pszSetIrqR0
        &&  !VALID_PTR(pPciBusReg->pszSetIrqR0))
    {
        Assert(VALID_PTR(pPciBusReg->pszSetIrqR0));
        LogFlow(("pdmR3DevHlp_PCIBusRegister: caller='%s'/%d: returns %Rrc (GC callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
     if (!ppPciHlpR3)
    {
        Assert(ppPciHlpR3);
        LogFlow(("pdmR3DevHlp_PCIBusRegister: caller='%s'/%d: returns %Rrc (ppPciHlpR3)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Find free PCI bus entry.
     */
    unsigned iBus = 0;
    for (iBus = 0; iBus < RT_ELEMENTS(pVM->pdm.s.aPciBuses); iBus++)
        if (!pVM->pdm.s.aPciBuses[iBus].pDevInsR3)
            break;
    if (iBus >= RT_ELEMENTS(pVM->pdm.s.aPciBuses))
    {
        AssertMsgFailed(("Too many PCI buses. Max=%u\n", RT_ELEMENTS(pVM->pdm.s.aPciBuses)));
        LogFlow(("pdmR3DevHlp_PCIBusRegister: caller='%s'/%d: returns %Rrc (pci bus)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    PPDMPCIBUS pPciBus = &pVM->pdm.s.aPciBuses[iBus];

    /*
     * Resolve and init the RC bits.
     */
    if (pPciBusReg->pszSetIrqRC)
    {
        int rc = pdmR3DevGetSymbolRCLazy(pDevIns, pPciBusReg->pszSetIrqRC, &pPciBus->pfnSetIrqRC);
        AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pPciBusReg->pszSetIrqRC, rc));
        if (RT_FAILURE(rc))
        {
            LogFlow(("pdmR3DevHlp_PCIRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
            return rc;
        }
        pPciBus->pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);
    }
    else
    {
        pPciBus->pfnSetIrqRC  = 0;
        pPciBus->pDevInsRC    = 0;
    }

    /*
     * Resolve and init the R0 bits.
     */
    if (pPciBusReg->pszSetIrqR0)
    {
        int rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pPciBusReg->pszSetIrqR0, &pPciBus->pfnSetIrqR0);
        AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pPciBusReg->pszSetIrqR0, rc));
        if (RT_FAILURE(rc))
        {
            LogFlow(("pdmR3DevHlp_PCIRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
            return rc;
        }
        pPciBus->pDevInsR0 = PDMDEVINS_2_R0PTR(pDevIns);
    }
    else
    {
        pPciBus->pfnSetIrqR0 = 0;
        pPciBus->pDevInsR0   = 0;
    }

    /*
     * Init the R3 bits.
     */
    pPciBus->iBus                    = iBus;
    pPciBus->pDevInsR3               = pDevIns;
    pPciBus->pfnRegisterR3           = pPciBusReg->pfnRegisterR3;
    pPciBus->pfnRegisterMsiR3        = pPciBusReg->pfnRegisterMsiR3;
    pPciBus->pfnIORegionRegisterR3   = pPciBusReg->pfnIORegionRegisterR3;
    pPciBus->pfnSetConfigCallbacksR3 = pPciBusReg->pfnSetConfigCallbacksR3;
    pPciBus->pfnSetIrqR3             = pPciBusReg->pfnSetIrqR3;
    pPciBus->pfnFakePCIBIOSR3        = pPciBusReg->pfnFakePCIBIOSR3;

    Log(("PDM: Registered PCI bus device '%s'/%d pDevIns=%p\n", pDevIns->pReg->szName, pDevIns->iInstance, pDevIns));

    /* set the helper pointer and return. */
    *ppPciHlpR3 = &g_pdmR3DevPciHlp;
    LogFlow(("pdmR3DevHlp_PCIBusRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VINF_SUCCESS));
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPICRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_PICRegister(PPDMDEVINS pDevIns, PPDMPICREG pPicReg, PCPDMPICHLPR3 *ppPicHlpR3)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: pPicReg=%p:{.u32Version=%#x, .pfnSetIrqR3=%p, .pfnGetInterruptR3=%p, .pszGetIrqRC=%p:{%s}, .pszGetInterruptRC=%p:{%s}, .pszGetIrqR0=%p:{%s}, .pszGetInterruptR0=%p:{%s} } ppPicHlpR3=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pPicReg, pPicReg->u32Version, pPicReg->pfnSetIrqR3, pPicReg->pfnGetInterruptR3,
             pPicReg->pszSetIrqRC, pPicReg->pszSetIrqRC, pPicReg->pszGetInterruptRC, pPicReg->pszGetInterruptRC,
             pPicReg->pszSetIrqR0, pPicReg->pszSetIrqR0, pPicReg->pszGetInterruptR0, pPicReg->pszGetInterruptR0,
             ppPicHlpR3));

    /*
     * Validate input.
     */
    if (pPicReg->u32Version != PDM_PICREG_VERSION)
    {
        AssertMsgFailed(("u32Version=%#x expected %#x\n", pPicReg->u32Version, PDM_PICREG_VERSION));
        LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: returns %Rrc (version)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    !pPicReg->pfnSetIrqR3
        ||  !pPicReg->pfnGetInterruptR3)
    {
        Assert(pPicReg->pfnSetIrqR3);
        Assert(pPicReg->pfnGetInterruptR3);
        LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: returns %Rrc (R3 callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    (   pPicReg->pszSetIrqRC
             || pPicReg->pszGetInterruptRC)
        &&  (   !VALID_PTR(pPicReg->pszSetIrqRC)
             || !VALID_PTR(pPicReg->pszGetInterruptRC))
       )
    {
        Assert(VALID_PTR(pPicReg->pszSetIrqRC));
        Assert(VALID_PTR(pPicReg->pszGetInterruptRC));
        LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: returns %Rrc (RC callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    pPicReg->pszSetIrqRC
        &&  !(pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_RC))
    {
        Assert(pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_RC);
        LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: returns %Rrc (RC flag)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    pPicReg->pszSetIrqR0
        &&  !(pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_R0))
    {
        Assert(pDevIns->pReg->fFlags & PDM_DEVREG_FLAGS_R0);
        LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: returns %Rrc (R0 flag)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (!ppPicHlpR3)
    {
        Assert(ppPicHlpR3);
        LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: returns %Rrc (ppPicHlpR3)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Only one PIC device.
     */
    PVM pVM = pDevIns->Internal.s.pVMR3;
    if (pVM->pdm.s.Pic.pDevInsR3)
    {
        AssertMsgFailed(("Only one pic device is supported!\n"));
        LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * RC stuff.
     */
    if (pPicReg->pszSetIrqRC)
    {
        int rc = pdmR3DevGetSymbolRCLazy(pDevIns, pPicReg->pszSetIrqRC, &pVM->pdm.s.Pic.pfnSetIrqRC);
        AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pPicReg->pszSetIrqRC, rc));
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pPicReg->pszGetInterruptRC, &pVM->pdm.s.Pic.pfnGetInterruptRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pPicReg->pszGetInterruptRC, rc));
        }
        if (RT_FAILURE(rc))
        {
            LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
            return rc;
        }
        pVM->pdm.s.Pic.pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);
    }
    else
    {
        pVM->pdm.s.Pic.pDevInsRC = 0;
        pVM->pdm.s.Pic.pfnSetIrqRC = 0;
        pVM->pdm.s.Pic.pfnGetInterruptRC = 0;
    }

    /*
     * R0 stuff.
     */
    if (pPicReg->pszSetIrqR0)
    {
        int rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pPicReg->pszSetIrqR0, &pVM->pdm.s.Pic.pfnSetIrqR0);
        AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pPicReg->pszSetIrqR0, rc));
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pPicReg->pszGetInterruptR0, &pVM->pdm.s.Pic.pfnGetInterruptR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pPicReg->pszGetInterruptR0, rc));
        }
        if (RT_FAILURE(rc))
        {
            LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
            return rc;
        }
        pVM->pdm.s.Pic.pDevInsR0 = PDMDEVINS_2_R0PTR(pDevIns);
        Assert(pVM->pdm.s.Pic.pDevInsR0);
    }
    else
    {
        pVM->pdm.s.Pic.pfnSetIrqR0 = 0;
        pVM->pdm.s.Pic.pfnGetInterruptR0 = 0;
        pVM->pdm.s.Pic.pDevInsR0 = 0;
    }

    /*
     * R3 stuff.
     */
    pVM->pdm.s.Pic.pDevInsR3 = pDevIns;
    pVM->pdm.s.Pic.pfnSetIrqR3 = pPicReg->pfnSetIrqR3;
    pVM->pdm.s.Pic.pfnGetInterruptR3 = pPicReg->pfnGetInterruptR3;
    Log(("PDM: Registered PIC device '%s'/%d pDevIns=%p\n", pDevIns->pReg->szName, pDevIns->iInstance, pDevIns));

    /* set the helper pointer and return. */
    *ppPicHlpR3 = &g_pdmR3DevPicHlp;
    LogFlow(("pdmR3DevHlp_PICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VINF_SUCCESS));
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnAPICRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_APICRegister(PPDMDEVINS pDevIns, PPDMAPICREG pApicReg, PCPDMAPICHLPR3 *ppApicHlpR3)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_APICRegister: caller='%s'/%d: pApicReg=%p:{.u32Version=%#x, .pfnGetInterruptR3=%p, .pfnSetBaseR3=%p, .pfnGetBaseR3=%p, "
             ".pfnSetTPRR3=%p, .pfnGetTPRR3=%p, .pfnWriteMSR3=%p, .pfnReadMSR3=%p, .pfnBusDeliverR3=%p, .pfnLocalInterruptR3=%p .pfnGetTimerFreqR3=%p, pszGetInterruptRC=%p:{%s}, pszSetBaseRC=%p:{%s}, pszGetBaseRC=%p:{%s}, "
             ".pszSetTPRRC=%p:{%s}, .pszGetTPRRC=%p:{%s}, .pszWriteMSRRC=%p:{%s}, .pszReadMSRRC=%p:{%s}, .pszBusDeliverRC=%p:{%s}, .pszLocalInterruptRC=%p:{%s}, .pszGetTimerFreqRC=%p:{%s}} ppApicHlpR3=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pApicReg, pApicReg->u32Version, pApicReg->pfnGetInterruptR3, pApicReg->pfnSetBaseR3,
             pApicReg->pfnGetBaseR3, pApicReg->pfnSetTPRR3, pApicReg->pfnGetTPRR3, pApicReg->pfnWriteMSRR3, pApicReg->pfnReadMSRR3, pApicReg->pfnBusDeliverR3, pApicReg->pfnLocalInterruptR3, pApicReg->pfnGetTimerFreqR3, pApicReg->pszGetInterruptRC,
             pApicReg->pszGetInterruptRC, pApicReg->pszSetBaseRC, pApicReg->pszSetBaseRC, pApicReg->pszGetBaseRC, pApicReg->pszGetBaseRC,
             pApicReg->pszSetTPRRC, pApicReg->pszSetTPRRC, pApicReg->pszGetTPRRC, pApicReg->pszGetTPRRC, pApicReg->pszWriteMSRRC, pApicReg->pszWriteMSRRC, pApicReg->pszReadMSRRC, pApicReg->pszReadMSRRC, pApicReg->pszBusDeliverRC,
             pApicReg->pszBusDeliverRC, pApicReg->pszLocalInterruptRC, pApicReg->pszLocalInterruptRC, pApicReg->pszGetTimerFreqRC, pApicReg->pszGetTimerFreqRC, ppApicHlpR3));

    /*
     * Validate input.
     */
    if (pApicReg->u32Version != PDM_APICREG_VERSION)
    {
        AssertMsgFailed(("u32Version=%#x expected %#x\n", pApicReg->u32Version, PDM_APICREG_VERSION));
        LogFlow(("pdmR3DevHlp_APICRegister: caller='%s'/%d: returns %Rrc (version)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    !pApicReg->pfnGetInterruptR3
        ||  !pApicReg->pfnHasPendingIrqR3
        ||  !pApicReg->pfnSetBaseR3
        ||  !pApicReg->pfnGetBaseR3
        ||  !pApicReg->pfnSetTPRR3
        ||  !pApicReg->pfnGetTPRR3
        ||  !pApicReg->pfnWriteMSRR3
        ||  !pApicReg->pfnReadMSRR3
        ||  !pApicReg->pfnBusDeliverR3
        ||  !pApicReg->pfnLocalInterruptR3
        ||  !pApicReg->pfnGetTimerFreqR3)
    {
        Assert(pApicReg->pfnGetInterruptR3);
        Assert(pApicReg->pfnHasPendingIrqR3);
        Assert(pApicReg->pfnSetBaseR3);
        Assert(pApicReg->pfnGetBaseR3);
        Assert(pApicReg->pfnSetTPRR3);
        Assert(pApicReg->pfnGetTPRR3);
        Assert(pApicReg->pfnWriteMSRR3);
        Assert(pApicReg->pfnReadMSRR3);
        Assert(pApicReg->pfnBusDeliverR3);
        Assert(pApicReg->pfnLocalInterruptR3);
        Assert(pApicReg->pfnGetTimerFreqR3);
        LogFlow(("pdmR3DevHlp_APICRegister: caller='%s'/%d: returns %Rrc (R3 callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (   (    pApicReg->pszGetInterruptRC
            ||  pApicReg->pszHasPendingIrqRC
            ||  pApicReg->pszSetBaseRC
            ||  pApicReg->pszGetBaseRC
            ||  pApicReg->pszSetTPRRC
            ||  pApicReg->pszGetTPRRC
            ||  pApicReg->pszWriteMSRRC
            ||  pApicReg->pszReadMSRRC
            ||  pApicReg->pszBusDeliverRC
            ||  pApicReg->pszLocalInterruptRC
            ||  pApicReg->pszGetTimerFreqRC)
        &&  (   !VALID_PTR(pApicReg->pszGetInterruptRC)
            ||  !VALID_PTR(pApicReg->pszHasPendingIrqRC)
            ||  !VALID_PTR(pApicReg->pszSetBaseRC)
            ||  !VALID_PTR(pApicReg->pszGetBaseRC)
            ||  !VALID_PTR(pApicReg->pszSetTPRRC)
            ||  !VALID_PTR(pApicReg->pszGetTPRRC)
            ||  !VALID_PTR(pApicReg->pszWriteMSRRC)
            ||  !VALID_PTR(pApicReg->pszReadMSRRC)
            ||  !VALID_PTR(pApicReg->pszBusDeliverRC)
            ||  !VALID_PTR(pApicReg->pszLocalInterruptRC)
            ||  !VALID_PTR(pApicReg->pszGetTimerFreqRC))
       )
    {
        Assert(VALID_PTR(pApicReg->pszGetInterruptRC));
        Assert(VALID_PTR(pApicReg->pszHasPendingIrqRC));
        Assert(VALID_PTR(pApicReg->pszSetBaseRC));
        Assert(VALID_PTR(pApicReg->pszGetBaseRC));
        Assert(VALID_PTR(pApicReg->pszSetTPRRC));
        Assert(VALID_PTR(pApicReg->pszGetTPRRC));
        Assert(VALID_PTR(pApicReg->pszReadMSRRC));
        Assert(VALID_PTR(pApicReg->pszWriteMSRRC));
        Assert(VALID_PTR(pApicReg->pszBusDeliverRC));
        Assert(VALID_PTR(pApicReg->pszLocalInterruptRC));
        Assert(VALID_PTR(pApicReg->pszGetTimerFreqRC));
        LogFlow(("pdmR3DevHlp_APICRegister: caller='%s'/%d: returns %Rrc (RC callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (   (    pApicReg->pszGetInterruptR0
            ||  pApicReg->pszHasPendingIrqR0
            ||  pApicReg->pszSetBaseR0
            ||  pApicReg->pszGetBaseR0
            ||  pApicReg->pszSetTPRR0
            ||  pApicReg->pszGetTPRR0
            ||  pApicReg->pszWriteMSRR0
            ||  pApicReg->pszReadMSRR0
            ||  pApicReg->pszBusDeliverR0
            ||  pApicReg->pszLocalInterruptR0
            ||  pApicReg->pszGetTimerFreqR0)
        &&  (   !VALID_PTR(pApicReg->pszGetInterruptR0)
            ||  !VALID_PTR(pApicReg->pszHasPendingIrqR0)
            ||  !VALID_PTR(pApicReg->pszSetBaseR0)
            ||  !VALID_PTR(pApicReg->pszGetBaseR0)
            ||  !VALID_PTR(pApicReg->pszSetTPRR0)
            ||  !VALID_PTR(pApicReg->pszGetTPRR0)
            ||  !VALID_PTR(pApicReg->pszReadMSRR0)
            ||  !VALID_PTR(pApicReg->pszWriteMSRR0)
            ||  !VALID_PTR(pApicReg->pszBusDeliverR0)
            ||  !VALID_PTR(pApicReg->pszLocalInterruptR0)
            ||  !VALID_PTR(pApicReg->pszGetTimerFreqR0))
       )
    {
        Assert(VALID_PTR(pApicReg->pszGetInterruptR0));
        Assert(VALID_PTR(pApicReg->pszHasPendingIrqR0));
        Assert(VALID_PTR(pApicReg->pszSetBaseR0));
        Assert(VALID_PTR(pApicReg->pszGetBaseR0));
        Assert(VALID_PTR(pApicReg->pszSetTPRR0));
        Assert(VALID_PTR(pApicReg->pszGetTPRR0));
        Assert(VALID_PTR(pApicReg->pszReadMSRR0));
        Assert(VALID_PTR(pApicReg->pszWriteMSRR0));
        Assert(VALID_PTR(pApicReg->pszBusDeliverR0));
        Assert(VALID_PTR(pApicReg->pszLocalInterruptR0));
        Assert(VALID_PTR(pApicReg->pszGetTimerFreqR0));
        LogFlow(("pdmR3DevHlp_APICRegister: caller='%s'/%d: returns %Rrc (R0 callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (!ppApicHlpR3)
    {
        Assert(ppApicHlpR3);
        LogFlow(("pdmR3DevHlp_APICRegister: caller='%s'/%d: returns %Rrc (ppApicHlpR3)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Only one APIC device. On SMP we have single logical device covering all LAPICs,
     * as they need to communicate and share state easily.
     */
    PVM pVM = pDevIns->Internal.s.pVMR3;
    if (pVM->pdm.s.Apic.pDevInsR3)
    {
        AssertMsgFailed(("Only one apic device is supported!\n"));
        LogFlow(("pdmR3DevHlp_APICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Resolve & initialize the RC bits.
     */
    if (pApicReg->pszGetInterruptRC)
    {
        int rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszGetInterruptRC, &pVM->pdm.s.Apic.pfnGetInterruptRC);
        AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszGetInterruptRC, rc));
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszHasPendingIrqRC, &pVM->pdm.s.Apic.pfnHasPendingIrqRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszHasPendingIrqRC, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszSetBaseRC, &pVM->pdm.s.Apic.pfnSetBaseRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszSetBaseRC, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszGetBaseRC, &pVM->pdm.s.Apic.pfnGetBaseRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszGetBaseRC, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszSetTPRRC, &pVM->pdm.s.Apic.pfnSetTPRRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszSetTPRRC, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszGetTPRRC, &pVM->pdm.s.Apic.pfnGetTPRRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszGetTPRRC, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszWriteMSRRC, &pVM->pdm.s.Apic.pfnWriteMSRRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszWriteMSRRC, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszReadMSRRC, &pVM->pdm.s.Apic.pfnReadMSRRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszReadMSRRC, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszBusDeliverRC, &pVM->pdm.s.Apic.pfnBusDeliverRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszBusDeliverRC, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszLocalInterruptRC, &pVM->pdm.s.Apic.pfnLocalInterruptRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszLocalInterruptRC, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolRCLazy(pDevIns, pApicReg->pszGetTimerFreqRC, &pVM->pdm.s.Apic.pfnGetTimerFreqRC);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pApicReg->pszGetTimerFreqRC, rc));
        }
        if (RT_FAILURE(rc))
        {
            LogFlow(("pdmR3DevHlp_APICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
            return rc;
        }
        pVM->pdm.s.Apic.pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);
    }
    else
    {
        pVM->pdm.s.Apic.pDevInsRC           = 0;
        pVM->pdm.s.Apic.pfnGetInterruptRC   = 0;
        pVM->pdm.s.Apic.pfnHasPendingIrqRC  = 0;
        pVM->pdm.s.Apic.pfnSetBaseRC        = 0;
        pVM->pdm.s.Apic.pfnGetBaseRC        = 0;
        pVM->pdm.s.Apic.pfnSetTPRRC         = 0;
        pVM->pdm.s.Apic.pfnGetTPRRC         = 0;
        pVM->pdm.s.Apic.pfnWriteMSRRC       = 0;
        pVM->pdm.s.Apic.pfnReadMSRRC        = 0;
        pVM->pdm.s.Apic.pfnBusDeliverRC     = 0;
        pVM->pdm.s.Apic.pfnLocalInterruptRC = 0;
        pVM->pdm.s.Apic.pfnGetTimerFreqRC   = 0;
    }

    /*
     * Resolve & initialize the R0 bits.
     */
    if (pApicReg->pszGetInterruptR0)
    {
        int rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszGetInterruptR0, &pVM->pdm.s.Apic.pfnGetInterruptR0);
        AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszGetInterruptR0, rc));
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszHasPendingIrqR0, &pVM->pdm.s.Apic.pfnHasPendingIrqR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszHasPendingIrqR0, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszSetBaseR0, &pVM->pdm.s.Apic.pfnSetBaseR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszSetBaseR0, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszGetBaseR0, &pVM->pdm.s.Apic.pfnGetBaseR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszGetBaseR0, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszSetTPRR0, &pVM->pdm.s.Apic.pfnSetTPRR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszSetTPRR0, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszGetTPRR0, &pVM->pdm.s.Apic.pfnGetTPRR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszGetTPRR0, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszWriteMSRR0, &pVM->pdm.s.Apic.pfnWriteMSRR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszWriteMSRR0, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszReadMSRR0, &pVM->pdm.s.Apic.pfnReadMSRR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszReadMSRR0, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszBusDeliverR0, &pVM->pdm.s.Apic.pfnBusDeliverR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszBusDeliverR0, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszLocalInterruptR0, &pVM->pdm.s.Apic.pfnLocalInterruptR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszLocalInterruptR0, rc));
        }
        if (RT_SUCCESS(rc))
        {
            rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pApicReg->pszGetTimerFreqR0, &pVM->pdm.s.Apic.pfnGetTimerFreqR0);
            AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pApicReg->pszGetTimerFreqR0, rc));
        }
        if (RT_FAILURE(rc))
        {
            LogFlow(("pdmR3DevHlp_APICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
            return rc;
        }
        pVM->pdm.s.Apic.pDevInsR0 = PDMDEVINS_2_R0PTR(pDevIns);
        Assert(pVM->pdm.s.Apic.pDevInsR0);
    }
    else
    {
        pVM->pdm.s.Apic.pfnGetInterruptR0   = 0;
        pVM->pdm.s.Apic.pfnHasPendingIrqR0  = 0;
        pVM->pdm.s.Apic.pfnSetBaseR0        = 0;
        pVM->pdm.s.Apic.pfnGetBaseR0        = 0;
        pVM->pdm.s.Apic.pfnSetTPRR0         = 0;
        pVM->pdm.s.Apic.pfnGetTPRR0         = 0;
        pVM->pdm.s.Apic.pfnWriteMSRR0       = 0;
        pVM->pdm.s.Apic.pfnReadMSRR0        = 0;
        pVM->pdm.s.Apic.pfnBusDeliverR0     = 0;
        pVM->pdm.s.Apic.pfnLocalInterruptR0 = 0;
        pVM->pdm.s.Apic.pfnGetTimerFreqR0   = 0;
        pVM->pdm.s.Apic.pDevInsR0           = 0;
    }

    /*
     * Initialize the HC bits.
     */
    pVM->pdm.s.Apic.pDevInsR3           = pDevIns;
    pVM->pdm.s.Apic.pfnGetInterruptR3   = pApicReg->pfnGetInterruptR3;
    pVM->pdm.s.Apic.pfnHasPendingIrqR3  = pApicReg->pfnHasPendingIrqR3;
    pVM->pdm.s.Apic.pfnSetBaseR3        = pApicReg->pfnSetBaseR3;
    pVM->pdm.s.Apic.pfnGetBaseR3        = pApicReg->pfnGetBaseR3;
    pVM->pdm.s.Apic.pfnSetTPRR3         = pApicReg->pfnSetTPRR3;
    pVM->pdm.s.Apic.pfnGetTPRR3         = pApicReg->pfnGetTPRR3;
    pVM->pdm.s.Apic.pfnWriteMSRR3       = pApicReg->pfnWriteMSRR3;
    pVM->pdm.s.Apic.pfnReadMSRR3        = pApicReg->pfnReadMSRR3;
    pVM->pdm.s.Apic.pfnBusDeliverR3     = pApicReg->pfnBusDeliverR3;
    pVM->pdm.s.Apic.pfnLocalInterruptR3 = pApicReg->pfnLocalInterruptR3;
    pVM->pdm.s.Apic.pfnGetTimerFreqR3   = pApicReg->pfnGetTimerFreqR3;
    Log(("PDM: Registered APIC device '%s'/%d pDevIns=%p\n", pDevIns->pReg->szName, pDevIns->iInstance, pDevIns));

    /* set the helper pointer and return. */
    *ppApicHlpR3 = &g_pdmR3DevApicHlp;
    LogFlow(("pdmR3DevHlp_APICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VINF_SUCCESS));
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnIOAPICRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_IOAPICRegister(PPDMDEVINS pDevIns, PPDMIOAPICREG pIoApicReg, PCPDMIOAPICHLPR3 *ppIoApicHlpR3)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: pIoApicReg=%p:{.u32Version=%#x, .pfnSetIrqR3=%p, .pszSetIrqRC=%p:{%s}, .pszSetIrqR0=%p:{%s}} ppIoApicHlpR3=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pIoApicReg, pIoApicReg->u32Version, pIoApicReg->pfnSetIrqR3,
             pIoApicReg->pszSetIrqRC, pIoApicReg->pszSetIrqRC, pIoApicReg->pszSetIrqR0, pIoApicReg->pszSetIrqR0, ppIoApicHlpR3));

    /*
     * Validate input.
     */
    if (pIoApicReg->u32Version != PDM_IOAPICREG_VERSION)
    {
        AssertMsgFailed(("u32Version=%#x expected %#x\n", pIoApicReg->u32Version, PDM_IOAPICREG_VERSION));
        LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc (version)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (!pIoApicReg->pfnSetIrqR3 || !pIoApicReg->pfnSendMsiR3)
    {
        Assert(pIoApicReg->pfnSetIrqR3);
        LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc (R3 callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    pIoApicReg->pszSetIrqRC
        &&  !VALID_PTR(pIoApicReg->pszSetIrqRC))
    {
        Assert(VALID_PTR(pIoApicReg->pszSetIrqRC));
        LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc (GC callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    pIoApicReg->pszSendMsiRC
        &&  !VALID_PTR(pIoApicReg->pszSendMsiRC))
    {
        Assert(VALID_PTR(pIoApicReg->pszSendMsiRC));
        LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc (GC callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    pIoApicReg->pszSetIrqR0
        &&  !VALID_PTR(pIoApicReg->pszSetIrqR0))
    {
        Assert(VALID_PTR(pIoApicReg->pszSetIrqR0));
        LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc (GC callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    pIoApicReg->pszSendMsiR0
        &&  !VALID_PTR(pIoApicReg->pszSendMsiR0))
    {
        Assert(VALID_PTR(pIoApicReg->pszSendMsiR0));
        LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc (GC callbacks)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (!ppIoApicHlpR3)
    {
        Assert(ppIoApicHlpR3);
        LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc (ppApicHlp)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * The I/O APIC requires the APIC to be present (hacks++).
     * If the I/O APIC does GC stuff so must the APIC.
     */
    PVM pVM = pDevIns->Internal.s.pVMR3;
    if (!pVM->pdm.s.Apic.pDevInsR3)
    {
        AssertMsgFailed(("Configuration error / Init order error! No APIC!\n"));
        LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc (no APIC)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    pIoApicReg->pszSetIrqRC
        &&  !pVM->pdm.s.Apic.pDevInsRC)
    {
        AssertMsgFailed(("Configuration error! APIC doesn't do GC, I/O APIC does!\n"));
        LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc (no GC APIC)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Only one I/O APIC device.
     */
    if (pVM->pdm.s.IoApic.pDevInsR3)
    {
        AssertMsgFailed(("Only one ioapic device is supported!\n"));
        LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc (only one)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Resolve & initialize the GC bits.
     */
    if (pIoApicReg->pszSetIrqRC)
    {
        int rc = pdmR3DevGetSymbolRCLazy(pDevIns, pIoApicReg->pszSetIrqRC, &pVM->pdm.s.IoApic.pfnSetIrqRC);
        AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pIoApicReg->pszSetIrqRC, rc));
        if (RT_FAILURE(rc))
        {
            LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
            return rc;
        }
        pVM->pdm.s.IoApic.pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);
    }
    else
    {
        pVM->pdm.s.IoApic.pDevInsRC   = 0;
        pVM->pdm.s.IoApic.pfnSetIrqRC = 0;
    }

    if (pIoApicReg->pszSendMsiRC)
    {
        int rc = pdmR3DevGetSymbolRCLazy(pDevIns, pIoApicReg->pszSetIrqRC, &pVM->pdm.s.IoApic.pfnSendMsiRC);
        AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szRCMod, pIoApicReg->pszSendMsiRC, rc));
        if (RT_FAILURE(rc))
        {
            LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
            return rc;
        }
    }
    else
    {
        pVM->pdm.s.IoApic.pfnSendMsiRC = 0;
    }

    /*
     * Resolve & initialize the R0 bits.
     */
    if (pIoApicReg->pszSetIrqR0)
    {
        int rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pIoApicReg->pszSetIrqR0, &pVM->pdm.s.IoApic.pfnSetIrqR0);
        AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pIoApicReg->pszSetIrqR0, rc));
        if (RT_FAILURE(rc))
        {
            LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
            return rc;
        }
        pVM->pdm.s.IoApic.pDevInsR0 = PDMDEVINS_2_R0PTR(pDevIns);
        Assert(pVM->pdm.s.IoApic.pDevInsR0);
    }
    else
    {
        pVM->pdm.s.IoApic.pfnSetIrqR0 = 0;
        pVM->pdm.s.IoApic.pDevInsR0   = 0;
    }

    if (pIoApicReg->pszSendMsiR0)
    {
        int rc = pdmR3DevGetSymbolR0Lazy(pDevIns, pIoApicReg->pszSendMsiR0, &pVM->pdm.s.IoApic.pfnSendMsiR0);
        AssertMsgRC(rc, ("%s::%s rc=%Rrc\n", pDevIns->pReg->szR0Mod, pIoApicReg->pszSendMsiR0, rc));
        if (RT_FAILURE(rc))
        {
            LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
            return rc;
        }
    }
    else
    {
        pVM->pdm.s.IoApic.pfnSendMsiR0 = 0;
    }


    /*
     * Initialize the R3 bits.
     */
    pVM->pdm.s.IoApic.pDevInsR3   = pDevIns;
    pVM->pdm.s.IoApic.pfnSetIrqR3 = pIoApicReg->pfnSetIrqR3;
    pVM->pdm.s.IoApic.pfnSendMsiR3 = pIoApicReg->pfnSendMsiR3;
    Log(("PDM: Registered I/O APIC device '%s'/%d pDevIns=%p\n", pDevIns->pReg->szName, pDevIns->iInstance, pDevIns));

    /* set the helper pointer and return. */
    *ppIoApicHlpR3 = &g_pdmR3DevIoApicHlp;
    LogFlow(("pdmR3DevHlp_IOAPICRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VINF_SUCCESS));
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnHPETRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_HPETRegister(PPDMDEVINS pDevIns, PPDMHPETREG pHpetReg, PCPDMHPETHLPR3 *ppHpetHlpR3)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_HPETRegister: caller='%s'/%d:\n", pDevIns->pReg->szName, pDevIns->iInstance));

    /*
     * Validate input.
     */
    if (pHpetReg->u32Version != PDM_HPETREG_VERSION)
    {
        AssertMsgFailed(("u32Version=%#x expected %#x\n", pHpetReg->u32Version, PDM_HPETREG_VERSION));
        LogFlow(("pdmR3DevHlp_HPETRegister: caller='%s'/%d: returns %Rrc (version)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    if (!ppHpetHlpR3)
    {
        Assert(ppHpetHlpR3);
        LogFlow(("pdmR3DevHlp_HPETRegister: caller='%s'/%d: returns %Rrc (ppApicHlpR3)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /* set the helper pointer and return. */
    *ppHpetHlpR3 = &g_pdmR3DevHpetHlp;
    LogFlow(("pdmR3DevHlp_HPETRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VINF_SUCCESS));
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnPciRawRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_PciRawRegister(PPDMDEVINS pDevIns, PPDMPCIRAWREG pPciRawReg, PCPDMPCIRAWHLPR3 *ppPciRawHlpR3)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_PciRawRegister: caller='%s'/%d:\n", pDevIns->pReg->szName, pDevIns->iInstance));

    /*
     * Validate input.
     */
    if (pPciRawReg->u32Version != PDM_PCIRAWREG_VERSION)
    {
        AssertMsgFailed(("u32Version=%#x expected %#x\n", pPciRawReg->u32Version, PDM_PCIRAWREG_VERSION));
        LogFlow(("pdmR3DevHlp_PciRawRegister: caller='%s'/%d: returns %Rrc (version)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    if (!ppPciRawHlpR3)
    {
        Assert(ppPciRawHlpR3);
        LogFlow(("pdmR3DevHlp_PciRawRegister: caller='%s'/%d: returns %Rrc (ppApicHlpR3)\n", pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /* set the helper pointer and return. */
    *ppPciRawHlpR3 = &g_pdmR3DevPciRawHlp;
    LogFlow(("pdmR3DevHlp_PciRawRegister: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, VINF_SUCCESS));
    return VINF_SUCCESS;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnDMACRegister} */
static DECLCALLBACK(int) pdmR3DevHlp_DMACRegister(PPDMDEVINS pDevIns, PPDMDMACREG pDmacReg, PCPDMDMACHLP *ppDmacHlp)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_DMACRegister: caller='%s'/%d: pDmacReg=%p:{.u32Version=%#x, .pfnRun=%p, .pfnRegister=%p, .pfnReadMemory=%p, .pfnWriteMemory=%p, .pfnSetDREQ=%p, .pfnGetChannelMode=%p} ppDmacHlp=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pDmacReg, pDmacReg->u32Version, pDmacReg->pfnRun, pDmacReg->pfnRegister,
             pDmacReg->pfnReadMemory, pDmacReg->pfnWriteMemory, pDmacReg->pfnSetDREQ, pDmacReg->pfnGetChannelMode, ppDmacHlp));

    /*
     * Validate input.
     */
    if (pDmacReg->u32Version != PDM_DMACREG_VERSION)
    {
        AssertMsgFailed(("u32Version=%#x expected %#x\n", pDmacReg->u32Version,
                         PDM_DMACREG_VERSION));
        LogFlow(("pdmR3DevHlp_DMACRegister: caller='%s'/%d: returns %Rrc (version)\n",
                 pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }
    if (    !pDmacReg->pfnRun
        ||  !pDmacReg->pfnRegister
        ||  !pDmacReg->pfnReadMemory
        ||  !pDmacReg->pfnWriteMemory
        ||  !pDmacReg->pfnSetDREQ
        ||  !pDmacReg->pfnGetChannelMode)
    {
        Assert(pDmacReg->pfnRun);
        Assert(pDmacReg->pfnRegister);
        Assert(pDmacReg->pfnReadMemory);
        Assert(pDmacReg->pfnWriteMemory);
        Assert(pDmacReg->pfnSetDREQ);
        Assert(pDmacReg->pfnGetChannelMode);
        LogFlow(("pdmR3DevHlp_DMACRegister: caller='%s'/%d: returns %Rrc (callbacks)\n",
                 pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    if (!ppDmacHlp)
    {
        Assert(ppDmacHlp);
        LogFlow(("pdmR3DevHlp_DMACRegister: caller='%s'/%d: returns %Rrc (ppDmacHlp)\n",
                 pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Only one DMA device.
     */
    PVM pVM = pDevIns->Internal.s.pVMR3;
    if (pVM->pdm.s.pDmac)
    {
        AssertMsgFailed(("Only one DMA device is supported!\n"));
        LogFlow(("pdmR3DevHlp_DMACRegister: caller='%s'/%d: returns %Rrc\n",
                 pDevIns->pReg->szName, pDevIns->iInstance, VERR_INVALID_PARAMETER));
        return VERR_INVALID_PARAMETER;
    }

    /*
     * Allocate and initialize pci bus structure.
     */
    int rc = VINF_SUCCESS;
    PPDMDMAC  pDmac = (PPDMDMAC)MMR3HeapAlloc(pDevIns->Internal.s.pVMR3, MM_TAG_PDM_DEVICE, sizeof(*pDmac));
    if (pDmac)
    {
        pDmac->pDevIns   = pDevIns;
        pDmac->Reg       = *pDmacReg;
        pVM->pdm.s.pDmac = pDmac;

        /* set the helper pointer. */
        *ppDmacHlp = &g_pdmR3DevDmacHlp;
        Log(("PDM: Registered DMAC device '%s'/%d pDevIns=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, pDevIns));
    }
    else
        rc = VERR_NO_MEMORY;

    LogFlow(("pdmR3DevHlp_DMACRegister: caller='%s'/%d: returns %Rrc\n",
             pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/**
 * @copydoc PDMDEVHLPR3::pfnRegisterVMMDevHeap
 */
static DECLCALLBACK(int) pdmR3DevHlp_RegisterVMMDevHeap(PPDMDEVINS pDevIns, RTGCPHYS GCPhys, RTR3PTR pvHeap, unsigned cbSize)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);

    int rc = PDMR3VmmDevHeapRegister(pDevIns->Internal.s.pVMR3, GCPhys, pvHeap, cbSize);
    return rc;
}


/**
 * @copydoc PDMDEVHLPR3::pfnUnregisterVMMDevHeap
 */
static DECLCALLBACK(int) pdmR3DevHlp_UnregisterVMMDevHeap(PPDMDEVINS pDevIns, RTGCPHYS GCPhys)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);

    int rc = PDMR3VmmDevHeapUnregister(pDevIns->Internal.s.pVMR3, GCPhys);
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMReset} */
static DECLCALLBACK(int) pdmR3DevHlp_VMReset(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_VMReset: caller='%s'/%d: VM_FF_RESET %d -> 1\n",
             pDevIns->pReg->szName, pDevIns->iInstance, VM_FF_IS_SET(pVM, VM_FF_RESET)));

    /*
     * We postpone this operation because we're likely to be inside a I/O instruction
     * and the EIP will be updated when we return.
     * We still return VINF_EM_RESET to break out of any execution loops and force FF evaluation.
     */
    bool fHaltOnReset;
    int rc = CFGMR3QueryBool(CFGMR3GetChild(CFGMR3GetRoot(pVM), "PDM"), "HaltOnReset", &fHaltOnReset);
    if (RT_SUCCESS(rc) && fHaltOnReset)
    {
        Log(("pdmR3DevHlp_VMReset: Halt On Reset!\n"));
        rc = VINF_EM_HALT;
    }
    else
    {
        VM_FF_SET(pVM, VM_FF_RESET);
        rc = VINF_EM_RESET;
    }

    LogFlow(("pdmR3DevHlp_VMReset: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMSuspend} */
static DECLCALLBACK(int) pdmR3DevHlp_VMSuspend(PPDMDEVINS pDevIns)
{
    int rc;
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_VMSuspend: caller='%s'/%d:\n",
             pDevIns->pReg->szName, pDevIns->iInstance));

    /** @todo Always take the SMP path - fewer code paths. */
    if (pVM->cCpus > 1)
    {
        /* We own the IOM lock here and could cause a deadlock by waiting for a VCPU that is blocking on the IOM lock. */
        rc = VMR3ReqCallNoWait(pVM, VMCPUID_ANY_QUEUE, (PFNRT)VMR3Suspend, 2, pVM->pUVM, VMSUSPENDREASON_VM);
        AssertRC(rc);
        rc = VINF_EM_SUSPEND;
    }
    else
        rc = VMR3Suspend(pVM->pUVM, VMSUSPENDREASON_VM);

    LogFlow(("pdmR3DevHlp_VMSuspend: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/**
 * Worker for pdmR3DevHlp_VMSuspendSaveAndPowerOff that is invoked via a queued
 * EMT request to avoid deadlocks.
 *
 * @returns VBox status code fit for scheduling.
 * @param   pVM                 Pointer to the VM.
 * @param   pDevIns             The device that triggered this action.
 */
static DECLCALLBACK(int) pdmR3DevHlp_VMSuspendSaveAndPowerOffWorker(PVM pVM, PPDMDEVINS pDevIns)
{
    /*
     * Suspend the VM first then do the saving.
     */
    int rc = VMR3Suspend(pVM->pUVM, VMSUSPENDREASON_VM);
    if (RT_SUCCESS(rc))
    {
        PUVM pUVM = pVM->pUVM;
        rc = pUVM->pVmm2UserMethods->pfnSaveState(pVM->pUVM->pVmm2UserMethods, pUVM);

        /*
         * On success, power off the VM, on failure we'll leave it suspended.
         */
        if (RT_SUCCESS(rc))
        {
            rc = VMR3PowerOff(pVM->pUVM);
            if (RT_FAILURE(rc))
                LogRel(("%s/SSP: VMR3PowerOff failed: %Rrc\n", pDevIns->pReg->szName, rc));
        }
        else
            LogRel(("%s/SSP: pfnSaveState failed: %Rrc\n", pDevIns->pReg->szName, rc));
    }
    else
        LogRel(("%s/SSP: Suspend failed: %Rrc\n", pDevIns->pReg->szName, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMSuspendSaveAndPowerOff} */
static DECLCALLBACK(int) pdmR3DevHlp_VMSuspendSaveAndPowerOff(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_VMSuspendSaveAndPowerOff: caller='%s'/%d:\n",
             pDevIns->pReg->szName, pDevIns->iInstance));

    int rc;
    if (   pVM->pUVM->pVmm2UserMethods
        && pVM->pUVM->pVmm2UserMethods->pfnSaveState)
    {
        rc = VMR3ReqCallNoWait(pVM, VMCPUID_ANY_QUEUE, (PFNRT)pdmR3DevHlp_VMSuspendSaveAndPowerOffWorker, 2, pVM, pDevIns);
        if (RT_SUCCESS(rc))
        {
            LogRel(("%s: Suspending, Saving and Powering Off the VM\n", pDevIns->pReg->szName));
            rc = VINF_EM_SUSPEND;
        }
    }
    else
        rc = VERR_NOT_SUPPORTED;

    LogFlow(("pdmR3DevHlp_VMSuspendSaveAndPowerOff: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMPowerOff} */
static DECLCALLBACK(int) pdmR3DevHlp_VMPowerOff(PPDMDEVINS pDevIns)
{
    int rc;
    PDMDEV_ASSERT_DEVINS(pDevIns);
    PVM pVM = pDevIns->Internal.s.pVMR3;
    VM_ASSERT_EMT(pVM);
    LogFlow(("pdmR3DevHlp_VMPowerOff: caller='%s'/%d:\n",
             pDevIns->pReg->szName, pDevIns->iInstance));

    /** @todo Always take the SMP path - fewer code paths. */
    if (pVM->cCpus > 1)
    {
        /* We might be holding locks here and could cause a deadlock since
           VMR3PowerOff rendezvous with the other CPUs. */
        rc = VMR3ReqCallNoWait(pVM, VMCPUID_ANY_QUEUE, (PFNRT)VMR3PowerOff, 1, pVM->pUVM);
        AssertRC(rc);
        /* Set the VCPU state to stopped here as well to make sure no
           inconsistency with the EM state occurs. */
        VMCPU_SET_STATE(VMMGetCpu(pVM), VMCPUSTATE_STOPPED);
        rc = VINF_EM_OFF;
    }
    else
        rc = VMR3PowerOff(pVM->pUVM);

    LogFlow(("pdmR3DevHlp_VMPowerOff: caller='%s'/%d: returns %Rrc\n", pDevIns->pReg->szName, pDevIns->iInstance, rc));
    return rc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnA20IsEnabled} */
static DECLCALLBACK(bool) pdmR3DevHlp_A20IsEnabled(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);

    bool fRc = PGMPhysIsA20Enabled(VMMGetCpu(pDevIns->Internal.s.pVMR3));

    LogFlow(("pdmR3DevHlp_A20IsEnabled: caller='%s'/%d: returns %d\n", pDevIns->pReg->szName, pDevIns->iInstance, fRc));
    return fRc;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnA20Set} */
static DECLCALLBACK(void) pdmR3DevHlp_A20Set(PPDMDEVINS pDevIns, bool fEnable)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);
    LogFlow(("pdmR3DevHlp_A20Set: caller='%s'/%d: fEnable=%d\n", pDevIns->pReg->szName, pDevIns->iInstance, fEnable));
    PGMR3PhysSetA20(VMMGetCpu(pDevIns->Internal.s.pVMR3), fEnable);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetCpuId} */
static DECLCALLBACK(void) pdmR3DevHlp_GetCpuId(PPDMDEVINS pDevIns, uint32_t iLeaf,
                                               uint32_t *pEax, uint32_t *pEbx, uint32_t *pEcx, uint32_t *pEdx)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    VM_ASSERT_EMT(pDevIns->Internal.s.pVMR3);

    LogFlow(("pdmR3DevHlp_GetCpuId: caller='%s'/%d: iLeaf=%d pEax=%p pEbx=%p pEcx=%p pEdx=%p\n",
             pDevIns->pReg->szName, pDevIns->iInstance, iLeaf, pEax, pEbx, pEcx, pEdx));
    AssertPtr(pEax); AssertPtr(pEbx); AssertPtr(pEcx); AssertPtr(pEdx);

    CPUMGetGuestCpuId(VMMGetCpu(pDevIns->Internal.s.pVMR3), iLeaf, 0 /*iSubLeaf*/, pEax, pEbx, pEcx, pEdx);

    LogFlow(("pdmR3DevHlp_GetCpuId: caller='%s'/%d: returns void - *pEax=%#x *pEbx=%#x *pEcx=%#x *pEdx=%#x\n",
             pDevIns->pReg->szName, pDevIns->iInstance, *pEax, *pEbx, *pEcx, *pEdx));
}


/**
 * The device helper structure for trusted devices.
 */
const PDMDEVHLPR3 g_pdmR3DevHlpTrusted =
{
    PDM_DEVHLPR3_VERSION,
    pdmR3DevHlp_IOPortRegister,
    pdmR3DevHlp_IOPortRegisterRC,
    pdmR3DevHlp_IOPortRegisterR0,
    pdmR3DevHlp_IOPortDeregister,
    pdmR3DevHlp_MMIORegister,
    pdmR3DevHlp_MMIORegisterRC,
    pdmR3DevHlp_MMIORegisterR0,
    pdmR3DevHlp_MMIODeregister,
    pdmR3DevHlp_MMIO2Register,
    pdmR3DevHlp_MMIO2Deregister,
    pdmR3DevHlp_MMIO2Map,
    pdmR3DevHlp_MMIO2Unmap,
    pdmR3DevHlp_MMHyperMapMMIO2,
    pdmR3DevHlp_MMIO2MapKernel,
    pdmR3DevHlp_ROMRegister,
    pdmR3DevHlp_ROMProtectShadow,
    pdmR3DevHlp_SSMRegister,
    pdmR3DevHlp_TMTimerCreate,
    pdmR3DevHlp_TMUtcNow,
    pdmR3DevHlp_PhysRead,
    pdmR3DevHlp_PhysWrite,
    pdmR3DevHlp_PhysGCPhys2CCPtr,
    pdmR3DevHlp_PhysGCPhys2CCPtrReadOnly,
    pdmR3DevHlp_PhysReleasePageMappingLock,
    pdmR3DevHlp_PhysReadGCVirt,
    pdmR3DevHlp_PhysWriteGCVirt,
    pdmR3DevHlp_PhysGCPtr2GCPhys,
    pdmR3DevHlp_MMHeapAlloc,
    pdmR3DevHlp_MMHeapAllocZ,
    pdmR3DevHlp_MMHeapFree,
    pdmR3DevHlp_VMState,
    pdmR3DevHlp_VMTeleportedAndNotFullyResumedYet,
    pdmR3DevHlp_VMSetError,
    pdmR3DevHlp_VMSetErrorV,
    pdmR3DevHlp_VMSetRuntimeError,
    pdmR3DevHlp_VMSetRuntimeErrorV,
    pdmR3DevHlp_DBGFStopV,
    pdmR3DevHlp_DBGFInfoRegister,
    pdmR3DevHlp_DBGFRegRegister,
    pdmR3DevHlp_DBGFTraceBuf,
    pdmR3DevHlp_STAMRegister,
    pdmR3DevHlp_STAMRegisterF,
    pdmR3DevHlp_STAMRegisterV,
    pdmR3DevHlp_PCIRegister,
    pdmR3DevHlp_PCIRegisterMsi,
    pdmR3DevHlp_PCIIORegionRegister,
    pdmR3DevHlp_PCISetConfigCallbacks,
    pdmR3DevHlp_PCIPhysRead,
    pdmR3DevHlp_PCIPhysWrite,
    pdmR3DevHlp_PCISetIrq,
    pdmR3DevHlp_PCISetIrqNoWait,
    pdmR3DevHlp_ISASetIrq,
    pdmR3DevHlp_ISASetIrqNoWait,
    pdmR3DevHlp_DriverAttach,
    pdmR3DevHlp_QueueCreate,
    pdmR3DevHlp_CritSectInit,
    pdmR3DevHlp_CritSectGetNop,
    pdmR3DevHlp_CritSectGetNopR0,
    pdmR3DevHlp_CritSectGetNopRC,
    pdmR3DevHlp_SetDeviceCritSect,
    pdmR3DevHlp_ThreadCreate,
    pdmR3DevHlp_SetAsyncNotification,
    pdmR3DevHlp_AsyncNotificationCompleted,
    pdmR3DevHlp_RTCRegister,
    pdmR3DevHlp_PCIBusRegister,
    pdmR3DevHlp_PICRegister,
    pdmR3DevHlp_APICRegister,
    pdmR3DevHlp_IOAPICRegister,
    pdmR3DevHlp_HPETRegister,
    pdmR3DevHlp_PciRawRegister,
    pdmR3DevHlp_DMACRegister,
    pdmR3DevHlp_DMARegister,
    pdmR3DevHlp_DMAReadMemory,
    pdmR3DevHlp_DMAWriteMemory,
    pdmR3DevHlp_DMASetDREQ,
    pdmR3DevHlp_DMAGetChannelMode,
    pdmR3DevHlp_DMASchedule,
    pdmR3DevHlp_CMOSWrite,
    pdmR3DevHlp_CMOSRead,
    pdmR3DevHlp_AssertEMT,
    pdmR3DevHlp_AssertOther,
    pdmR3DevHlp_LdrGetRCInterfaceSymbols,
    pdmR3DevHlp_LdrGetR0InterfaceSymbols,
    pdmR3DevHlp_CallR0,
    pdmR3DevHlp_VMGetSuspendReason,
    pdmR3DevHlp_VMGetResumeReason,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    pdmR3DevHlp_GetUVM,
    pdmR3DevHlp_GetVM,
    pdmR3DevHlp_GetVMCPU,
    pdmR3DevHlp_GetCurrentCpuId,
    pdmR3DevHlp_RegisterVMMDevHeap,
    pdmR3DevHlp_UnregisterVMMDevHeap,
    pdmR3DevHlp_VMReset,
    pdmR3DevHlp_VMSuspend,
    pdmR3DevHlp_VMSuspendSaveAndPowerOff,
    pdmR3DevHlp_VMPowerOff,
    pdmR3DevHlp_A20IsEnabled,
    pdmR3DevHlp_A20Set,
    pdmR3DevHlp_GetCpuId,
    pdmR3DevHlp_TMTimeVirtGet,
    pdmR3DevHlp_TMTimeVirtGetFreq,
    pdmR3DevHlp_TMTimeVirtGetNano,
    pdmR3DevHlp_GetSupDrvSession,
    PDM_DEVHLPR3_VERSION /* the end */
};




/** @interface_method_impl{PDMDEVHLPR3,pfnGetUVM} */
static DECLCALLBACK(PUVM) pdmR3DevHlp_Untrusted_GetUVM(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return NULL;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetVM} */
static DECLCALLBACK(PVM) pdmR3DevHlp_Untrusted_GetVM(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return NULL;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetVMCPU} */
static DECLCALLBACK(PVMCPU) pdmR3DevHlp_Untrusted_GetVMCPU(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return NULL;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetCurrentCpuId} */
static DECLCALLBACK(VMCPUID) pdmR3DevHlp_Untrusted_GetCurrentCpuId(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return NIL_VMCPUID;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnRegisterVMMDevHeap} */
static DECLCALLBACK(int) pdmR3DevHlp_Untrusted_RegisterVMMDevHeap(PPDMDEVINS pDevIns, RTGCPHYS GCPhys, RTR3PTR pvHeap, unsigned cbSize)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    NOREF(GCPhys); NOREF(pvHeap); NOREF(cbSize);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return VERR_ACCESS_DENIED;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnUnregisterVMMDevHeap} */
static DECLCALLBACK(int) pdmR3DevHlp_Untrusted_UnregisterVMMDevHeap(PPDMDEVINS pDevIns, RTGCPHYS GCPhys)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    NOREF(GCPhys);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return VERR_ACCESS_DENIED;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMReset} */
static DECLCALLBACK(int) pdmR3DevHlp_Untrusted_VMReset(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return VERR_ACCESS_DENIED;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMSuspend} */
static DECLCALLBACK(int) pdmR3DevHlp_Untrusted_VMSuspend(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return VERR_ACCESS_DENIED;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMSuspendSaveAndPowerOff} */
static DECLCALLBACK(int) pdmR3DevHlp_Untrusted_VMSuspendSaveAndPowerOff(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return VERR_ACCESS_DENIED;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnVMPowerOff} */
static DECLCALLBACK(int) pdmR3DevHlp_Untrusted_VMPowerOff(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return VERR_ACCESS_DENIED;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnA20IsEnabled} */
static DECLCALLBACK(bool) pdmR3DevHlp_Untrusted_A20IsEnabled(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return false;
}


/** @interface_method_impl{PDMDEVHLPR3,pfnA20Set} */
static DECLCALLBACK(void) pdmR3DevHlp_Untrusted_A20Set(PPDMDEVINS pDevIns, bool fEnable)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    NOREF(fEnable);
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetCpuId} */
static DECLCALLBACK(void) pdmR3DevHlp_Untrusted_GetCpuId(PPDMDEVINS pDevIns, uint32_t iLeaf,
                                                         uint32_t *pEax, uint32_t *pEbx, uint32_t *pEcx, uint32_t *pEdx)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    NOREF(iLeaf); NOREF(pEax); NOREF(pEbx); NOREF(pEcx); NOREF(pEdx);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
}


/** @interface_method_impl{PDMDEVHLPR3,pfnGetSupDrvSession} */
static DECLCALLBACK(PSUPDRVSESSION) pdmR3DevHlp_Untrusted_GetSupDrvSession(PPDMDEVINS pDevIns)
{
    PDMDEV_ASSERT_DEVINS(pDevIns);
    AssertReleaseMsgFailed(("Untrusted device called trusted helper! '%s'/%d\n", pDevIns->pReg->szName, pDevIns->iInstance));
    return (PSUPDRVSESSION)0;
}


/**
 * The device helper structure for non-trusted devices.
 */
const PDMDEVHLPR3 g_pdmR3DevHlpUnTrusted =
{
    PDM_DEVHLPR3_VERSION,
    pdmR3DevHlp_IOPortRegister,
    pdmR3DevHlp_IOPortRegisterRC,
    pdmR3DevHlp_IOPortRegisterR0,
    pdmR3DevHlp_IOPortDeregister,
    pdmR3DevHlp_MMIORegister,
    pdmR3DevHlp_MMIORegisterRC,
    pdmR3DevHlp_MMIORegisterR0,
    pdmR3DevHlp_MMIODeregister,
    pdmR3DevHlp_MMIO2Register,
    pdmR3DevHlp_MMIO2Deregister,
    pdmR3DevHlp_MMIO2Map,
    pdmR3DevHlp_MMIO2Unmap,
    pdmR3DevHlp_MMHyperMapMMIO2,
    pdmR3DevHlp_MMIO2MapKernel,
    pdmR3DevHlp_ROMRegister,
    pdmR3DevHlp_ROMProtectShadow,
    pdmR3DevHlp_SSMRegister,
    pdmR3DevHlp_TMTimerCreate,
    pdmR3DevHlp_TMUtcNow,
    pdmR3DevHlp_PhysRead,
    pdmR3DevHlp_PhysWrite,
    pdmR3DevHlp_PhysGCPhys2CCPtr,
    pdmR3DevHlp_PhysGCPhys2CCPtrReadOnly,
    pdmR3DevHlp_PhysReleasePageMappingLock,
    pdmR3DevHlp_PhysReadGCVirt,
    pdmR3DevHlp_PhysWriteGCVirt,
    pdmR3DevHlp_PhysGCPtr2GCPhys,
    pdmR3DevHlp_MMHeapAlloc,
    pdmR3DevHlp_MMHeapAllocZ,
    pdmR3DevHlp_MMHeapFree,
    pdmR3DevHlp_VMState,
    pdmR3DevHlp_VMTeleportedAndNotFullyResumedYet,
    pdmR3DevHlp_VMSetError,
    pdmR3DevHlp_VMSetErrorV,
    pdmR3DevHlp_VMSetRuntimeError,
    pdmR3DevHlp_VMSetRuntimeErrorV,
    pdmR3DevHlp_DBGFStopV,
    pdmR3DevHlp_DBGFInfoRegister,
    pdmR3DevHlp_DBGFRegRegister,
    pdmR3DevHlp_DBGFTraceBuf,
    pdmR3DevHlp_STAMRegister,
    pdmR3DevHlp_STAMRegisterF,
    pdmR3DevHlp_STAMRegisterV,
    pdmR3DevHlp_PCIRegister,
    pdmR3DevHlp_PCIRegisterMsi,
    pdmR3DevHlp_PCIIORegionRegister,
    pdmR3DevHlp_PCISetConfigCallbacks,
    pdmR3DevHlp_PCIPhysRead,
    pdmR3DevHlp_PCIPhysWrite,
    pdmR3DevHlp_PCISetIrq,
    pdmR3DevHlp_PCISetIrqNoWait,
    pdmR3DevHlp_ISASetIrq,
    pdmR3DevHlp_ISASetIrqNoWait,
    pdmR3DevHlp_DriverAttach,
    pdmR3DevHlp_QueueCreate,
    pdmR3DevHlp_CritSectInit,
    pdmR3DevHlp_CritSectGetNop,
    pdmR3DevHlp_CritSectGetNopR0,
    pdmR3DevHlp_CritSectGetNopRC,
    pdmR3DevHlp_SetDeviceCritSect,
    pdmR3DevHlp_ThreadCreate,
    pdmR3DevHlp_SetAsyncNotification,
    pdmR3DevHlp_AsyncNotificationCompleted,
    pdmR3DevHlp_RTCRegister,
    pdmR3DevHlp_PCIBusRegister,
    pdmR3DevHlp_PICRegister,
    pdmR3DevHlp_APICRegister,
    pdmR3DevHlp_IOAPICRegister,
    pdmR3DevHlp_HPETRegister,
    pdmR3DevHlp_PciRawRegister,
    pdmR3DevHlp_DMACRegister,
    pdmR3DevHlp_DMARegister,
    pdmR3DevHlp_DMAReadMemory,
    pdmR3DevHlp_DMAWriteMemory,
    pdmR3DevHlp_DMASetDREQ,
    pdmR3DevHlp_DMAGetChannelMode,
    pdmR3DevHlp_DMASchedule,
    pdmR3DevHlp_CMOSWrite,
    pdmR3DevHlp_CMOSRead,
    pdmR3DevHlp_AssertEMT,
    pdmR3DevHlp_AssertOther,
    pdmR3DevHlp_LdrGetRCInterfaceSymbols,
    pdmR3DevHlp_LdrGetR0InterfaceSymbols,
    pdmR3DevHlp_CallR0,
    pdmR3DevHlp_VMGetSuspendReason,
    pdmR3DevHlp_VMGetResumeReason,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    pdmR3DevHlp_Untrusted_GetUVM,
    pdmR3DevHlp_Untrusted_GetVM,
    pdmR3DevHlp_Untrusted_GetVMCPU,
    pdmR3DevHlp_Untrusted_GetCurrentCpuId,
    pdmR3DevHlp_Untrusted_RegisterVMMDevHeap,
    pdmR3DevHlp_Untrusted_UnregisterVMMDevHeap,
    pdmR3DevHlp_Untrusted_VMReset,
    pdmR3DevHlp_Untrusted_VMSuspend,
    pdmR3DevHlp_Untrusted_VMSuspendSaveAndPowerOff,
    pdmR3DevHlp_Untrusted_VMPowerOff,
    pdmR3DevHlp_Untrusted_A20IsEnabled,
    pdmR3DevHlp_Untrusted_A20Set,
    pdmR3DevHlp_Untrusted_GetCpuId,
    pdmR3DevHlp_TMTimeVirtGet,
    pdmR3DevHlp_TMTimeVirtGetFreq,
    pdmR3DevHlp_TMTimeVirtGetNano,
    pdmR3DevHlp_Untrusted_GetSupDrvSession,
    PDM_DEVHLPR3_VERSION /* the end */
};



/**
 * Queue consumer callback for internal component.
 *
 * @returns Success indicator.
 *          If false the item will not be removed and the flushing will stop.
 * @param   pVM         Pointer to the VM.
 * @param   pItem       The item to consume. Upon return this item will be freed.
 */
DECLCALLBACK(bool) pdmR3DevHlpQueueConsumer(PVM pVM, PPDMQUEUEITEMCORE pItem)
{
    PPDMDEVHLPTASK pTask = (PPDMDEVHLPTASK)pItem;
    LogFlow(("pdmR3DevHlpQueueConsumer: enmOp=%d pDevIns=%p\n", pTask->enmOp, pTask->pDevInsR3));
    switch (pTask->enmOp)
    {
        case PDMDEVHLPTASKOP_ISA_SET_IRQ:
            PDMIsaSetIrq(pVM, pTask->u.SetIRQ.iIrq, pTask->u.SetIRQ.iLevel, pTask->u.SetIRQ.uTagSrc);
            break;

        case PDMDEVHLPTASKOP_PCI_SET_IRQ:
        {
            /* Same as pdmR3DevHlp_PCISetIrq, except we've got a tag already. */
            PPDMDEVINS pDevIns = pTask->pDevInsR3;
            PPCIDEVICE pPciDev = pDevIns->Internal.s.pPciDeviceR3;
            if (pPciDev)
            {
                PPDMPCIBUS pBus = pDevIns->Internal.s.pPciBusR3; /** @todo the bus should be associated with the PCI device not the PDM device. */
                Assert(pBus);

                pdmLock(pVM);
                pBus->pfnSetIrqR3(pBus->pDevInsR3, pPciDev, pTask->u.SetIRQ.iIrq,
                                  pTask->u.SetIRQ.iLevel, pTask->u.SetIRQ.uTagSrc);
                pdmUnlock(pVM);
            }
            else
                AssertReleaseMsgFailed(("No PCI device registered!\n"));
            break;
        }

        case PDMDEVHLPTASKOP_IOAPIC_SET_IRQ:
            PDMIoApicSetIrq(pVM, pTask->u.SetIRQ.iIrq, pTask->u.SetIRQ.iLevel, pTask->u.SetIRQ.uTagSrc);
            break;

        default:
            AssertReleaseMsgFailed(("Invalid operation %d\n", pTask->enmOp));
            break;
    }
    return true;
}

/** @} */

