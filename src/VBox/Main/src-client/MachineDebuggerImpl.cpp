/* $Id$ */
/** @file
 * VBox IMachineDebugger COM class implementation (VBoxC).
 */

/*
 * Copyright (C) 2006-2013 Oracle Corporation
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
#include "MachineDebuggerImpl.h"

#include "Global.h"
#include "ConsoleImpl.h"

#include "AutoCaller.h"
#include "Logging.h"

#include <VBox/vmm/em.h>
#include <VBox/vmm/patm.h>
#include <VBox/vmm/csam.h>
#include <VBox/vmm/uvm.h>
#include <VBox/vmm/tm.h>
#include <VBox/vmm/hm.h>
#include <VBox/err.h>
#include <iprt/cpp/utils.h>


// constructor / destructor
/////////////////////////////////////////////////////////////////////////////

MachineDebugger::MachineDebugger()
    : mParent(NULL)
{
}

MachineDebugger::~MachineDebugger()
{
}

HRESULT MachineDebugger::FinalConstruct()
{
    unconst(mParent) = NULL;
    return BaseFinalConstruct();
}

void MachineDebugger::FinalRelease()
{
    uninit();
    BaseFinalRelease();
}

// public initializer/uninitializer for internal purposes only
/////////////////////////////////////////////////////////////////////////////

/**
 * Initializes the machine debugger object.
 *
 * @returns COM result indicator
 * @param aParent handle of our parent object
 */
HRESULT MachineDebugger::init (Console *aParent)
{
    LogFlowThisFunc(("aParent=%p\n", aParent));

    ComAssertRet(aParent, E_INVALIDARG);

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    unconst(mParent) = aParent;

    for (unsigned i = 0; i < RT_ELEMENTS(maiQueuedEmExecPolicyParams); i++)
        maiQueuedEmExecPolicyParams[i] = UINT8_MAX;
    mSingleStepQueued = ~0;
    mRecompileUserQueued = ~0;
    mRecompileSupervisorQueued = ~0;
    mPatmEnabledQueued = ~0;
    mCsamEnabledQueued = ~0;
    mLogEnabledQueued = ~0;
    mVirtualTimeRateQueued = ~0;
    mFlushMode = false;

    /* Confirm a successful initialization */
    autoInitSpan.setSucceeded();

    return S_OK;
}

/**
 *  Uninitializes the instance and sets the ready flag to FALSE.
 *  Called either from FinalRelease() or by the parent when it gets destroyed.
 */
void MachineDebugger::uninit()
{
    LogFlowThisFunc(("\n"));

    /* Enclose the state transition Ready->InUninit->NotReady */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    unconst(mParent) = NULL;
    mFlushMode = false;
}

// IMachineDebugger properties
/////////////////////////////////////////////////////////////////////////////

/**
 * Returns the current singlestepping flag.
 *
 * @returns COM status code
 * @param   a_fEnabled      Where to store the result.
 */
HRESULT MachineDebugger::getSingleStep(BOOL *aSingleStep)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        /** @todo */
        ReturnComNotImplemented();
    }
    return hrc;
}

/**
 * Sets the singlestepping flag.
 *
 * @returns COM status code
 * @param   a_fEnable       The new state.
 */
HRESULT MachineDebugger::setSingleStep(BOOL aSingleStep)
{
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        /** @todo */
        ReturnComNotImplemented();
    }
    return hrc;
}

/**
 * Internal worker for getting an EM executable policy setting.
 *
 * @returns COM status code.
 * @param   enmPolicy           Which EM policy.
 * @param   pfEnforced          Where to return the policy setting.
 */
HRESULT MachineDebugger::i_getEmExecPolicyProperty(EMEXECPOLICY enmPolicy, BOOL *pfEnforced)
{
    CheckComArgOutPointerValid(pfEnforced);

    AutoCaller autoCaller(this);
    HRESULT hrc = autoCaller.rc();
    if (SUCCEEDED(hrc))
    {
        AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
        if (i_queueSettings())
            *pfEnforced = maiQueuedEmExecPolicyParams[enmPolicy] == 1;
        else
        {
            bool fEnforced = false;
            Console::SafeVMPtrQuiet ptrVM(mParent);
            hrc = ptrVM.rc();
            if (SUCCEEDED(hrc))
                EMR3QueryExecutionPolicy(ptrVM.rawUVM(), enmPolicy, &fEnforced);
            *pfEnforced = fEnforced;
        }
    }
    return hrc;
}

/**
 * Internal worker for setting an EM executable policy.
 *
 * @returns COM status code.
 * @param   enmPolicy           Which policy to change.
 * @param   fEnforce            Whether to enforce the policy or not.
 */
HRESULT MachineDebugger::i_setEmExecPolicyProperty(EMEXECPOLICY enmPolicy, BOOL fEnforce)
{
    AutoCaller autoCaller(this);
    HRESULT hrc = autoCaller.rc();
    if (SUCCEEDED(hrc))
    {
        AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
        if (i_queueSettings())
            maiQueuedEmExecPolicyParams[enmPolicy] = fEnforce ? 1 : 0;
        else
        {
            Console::SafeVMPtrQuiet ptrVM(mParent);
            hrc = ptrVM.rc();
            if (SUCCEEDED(hrc))
            {
                int vrc = EMR3SetExecutionPolicy(ptrVM.rawUVM(), enmPolicy, fEnforce != FALSE);
                if (RT_FAILURE(vrc))
                    hrc = setError(VBOX_E_VM_ERROR, tr("EMR3SetExecutionPolicy failed with %Rrc"), vrc);
            }
        }
    }
    return hrc;
}

/**
 * Returns the current recompile user mode code flag.
 *
 * @returns COM status code
 * @param   a_fEnabled address of result variable
 */
HRESULT MachineDebugger::getRecompileUser(BOOL *aRecompileUser)
{
    return i_getEmExecPolicyProperty(EMEXECPOLICY_RECOMPILE_RING3, aRecompileUser);
}

/**
 * Sets the recompile user mode code flag.
 *
 * @returns COM status
 * @param   aEnable new user mode code recompile flag.
 */
HRESULT MachineDebugger::setRecompileUser(BOOL aRecompileUser)
{
    LogFlowThisFunc(("enable=%d\n", aRecompileUser));
    return i_setEmExecPolicyProperty(EMEXECPOLICY_RECOMPILE_RING3, aRecompileUser);
}

/**
 * Returns the current recompile supervisor code flag.
 *
 * @returns COM status code
 * @param   aEnabled address of result variable
 */
HRESULT MachineDebugger::getRecompileSupervisor(BOOL *aRecompileSupervisor)
{
    return i_getEmExecPolicyProperty(EMEXECPOLICY_RECOMPILE_RING0, aRecompileSupervisor);
}

/**
 * Sets the new recompile supervisor code flag.
 *
 * @returns COM status code
 * @param   aEnable new recompile supervisor code flag
 */
HRESULT MachineDebugger::setRecompileSupervisor(BOOL aRecompileSupervisor)
{
    LogFlowThisFunc(("enable=%d\n", aRecompileSupervisor));
    return i_setEmExecPolicyProperty(EMEXECPOLICY_RECOMPILE_RING0, aRecompileSupervisor);
}

/**
 * Returns the current execute-all-in-IEM setting.
 *
 * @returns COM status code
 * @param   aEnabled    Address of result variable.
 */
HRESULT MachineDebugger::getExecuteAllInIEM(BOOL *aExecuteAllInIEM)
{
    return i_getEmExecPolicyProperty(EMEXECPOLICY_IEM_ALL, aExecuteAllInIEM);
}

/**
 * Changes the execute-all-in-IEM setting.
 *
 * @returns COM status code
 * @param   aEnable     New setting.
 */
HRESULT MachineDebugger::setExecuteAllInIEM(BOOL aExecuteAllInIEM)
{
    LogFlowThisFunc(("enable=%d\n", aExecuteAllInIEM));
    return i_setEmExecPolicyProperty(EMEXECPOLICY_IEM_ALL, aExecuteAllInIEM);
}

/**
 * Returns the current patch manager enabled flag.
 *
 * @returns COM status code
 * @param   aEnabled address of result variable
 */
HRESULT MachineDebugger::getPATMEnabled(BOOL *aPATMEnabled)
{
#ifdef VBOX_WITH_RAW_MODE
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    Console::SafeVMPtrQuiet ptrVM(mParent);
    if (ptrVM.isOk())
        *aPATMEnabled = PATMR3IsEnabled (ptrVM.rawUVM());
    else
#endif
        *aPATMEnabled = false;

    return S_OK;
}

/**
 * Set the new patch manager enabled flag.
 *
 * @returns COM status code
 * @param   aEnable new patch manager enabled flag
 */
HRESULT MachineDebugger::setPATMEnabled(BOOL aPATMEnabled)
{
    LogFlowThisFunc(("enable=%d\n", aPATMEnabled));

#ifdef VBOX_WITH_RAW_MODE
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (i_queueSettings())
    {
        // queue the request
        mPatmEnabledQueued = aPATMEnabled;
        return S_OK;
    }

    Console::SafeVMPtr ptrVM(mParent);
    if (FAILED(ptrVM.rc()))
        return ptrVM.rc();

    int vrc = PATMR3AllowPatching(ptrVM.rawUVM(), RT_BOOL(aPATMEnabled));
    if (RT_FAILURE(vrc))
        return setError(VBOX_E_VM_ERROR, tr("PATMR3AllowPatching returned %Rrc"), vrc);

#else  /* !VBOX_WITH_RAW_MODE */
    if (aPATMEnabled)
        return setError(VBOX_E_VM_ERROR, tr("PATM not present"), VERR_NOT_SUPPORTED);
#endif /* !VBOX_WITH_RAW_MODE */
    return S_OK;
}

/**
 * Returns the current code scanner enabled flag.
 *
 * @returns COM status code
 * @param   aEnabled address of result variable
 */
HRESULT MachineDebugger::getCSAMEnabled(BOOL *aCSAMEnabled)
{
#ifdef VBOX_WITH_RAW_MODE
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    Console::SafeVMPtrQuiet ptrVM(mParent);

    if (ptrVM.isOk())
        *aCSAMEnabled = CSAMR3IsEnabled(ptrVM.rawUVM());
    else
#endif /* VBOX_WITH_RAW_MODE */
        *aCSAMEnabled = false;

    return S_OK;
}

/**
 * Sets the new code scanner enabled flag.
 *
 * @returns COM status code
 * @param   aEnable new code scanner enabled flag
 */
HRESULT MachineDebugger::setCSAMEnabled(BOOL aCSAMEnabled)
{
    LogFlowThisFunc(("enable=%d\n", aCSAMEnabled));

#ifdef VBOX_WITH_RAW_MODE
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (i_queueSettings())
    {
        // queue the request
        mCsamEnabledQueued = aCSAMEnabled;
        return S_OK;
    }

    Console::SafeVMPtr ptrVM(mParent);
    if (FAILED(ptrVM.rc()))
        return ptrVM.rc();

    int vrc = CSAMR3SetScanningEnabled(ptrVM.rawUVM(), aCSAMEnabled != FALSE);
    if (RT_FAILURE(vrc))
        return setError(VBOX_E_VM_ERROR, tr("CSAMR3SetScanningEnabled returned %Rrc"), vrc);

#else  /* !VBOX_WITH_RAW_MODE */
    if (aCSAMEnabled)
        return setError(VBOX_E_VM_ERROR, tr("CASM not present"), VERR_NOT_SUPPORTED);
#endif /* !VBOX_WITH_RAW_MODE */
    return S_OK;
}

/**
 * Returns the log enabled / disabled status.
 *
 * @returns COM status code
 * @param   aEnabled     address of result variable
 */
HRESULT MachineDebugger::getLogEnabled(BOOL *aLogEnabled)
{
#ifdef LOG_ENABLED
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    const PRTLOGGER pLogInstance = RTLogDefaultInstance();
    *aLogEnabled = pLogInstance && !(pLogInstance->fFlags & RTLOGFLAGS_DISABLED);
#else
    *aLogEnabled = false;
#endif

    return S_OK;
}

/**
 * Enables or disables logging.
 *
 * @returns COM status code
 * @param   aEnabled    The new code log state.
 */
HRESULT MachineDebugger::setLogEnabled(BOOL aLogEnabled)
{
    LogFlowThisFunc(("aLogEnabled=%d\n", aLogEnabled));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (i_queueSettings())
    {
        // queue the request
        mLogEnabledQueued = aLogEnabled;
        return S_OK;
    }

    Console::SafeVMPtr ptrVM(mParent);
    if (FAILED(ptrVM.rc())) return ptrVM.rc();

#ifdef LOG_ENABLED
    int vrc = DBGFR3LogModifyFlags(ptrVM.rawUVM(), aLogEnabled ? "enabled" : "disabled");
    if (RT_FAILURE(vrc))
    {
        /** @todo handle error code. */
    }
#endif

    return S_OK;
}

HRESULT MachineDebugger::i_logStringProps(PRTLOGGER pLogger, PFNLOGGETSTR pfnLogGetStr,
                                          const char *pszLogGetStr, Utf8Str *pstrSettings)
{
    /* Make sure the VM is powered up. */
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (FAILED(hrc))
        return hrc;

    /* Make sure we've got a logger. */
    if (!pLogger)
    {
        *pstrSettings = "";
        return S_OK;
    }

    /* Do the job. */
    size_t cbBuf = _1K;
    for (;;)
    {
        char *pszBuf = (char *)RTMemTmpAlloc(cbBuf);
        AssertReturn(pszBuf, E_OUTOFMEMORY);
        int vrc = pstrSettings->reserveNoThrow(cbBuf);
        if (RT_SUCCESS(vrc))
        {
            vrc = pfnLogGetStr(pLogger, pstrSettings->mutableRaw(), cbBuf);
            if (RT_SUCCESS(vrc))
            {
                pstrSettings->jolt();
                return S_OK;
            }
            *pstrSettings = "";
            AssertReturn(vrc == VERR_BUFFER_OVERFLOW, setError(VBOX_E_IPRT_ERROR, tr("%s returned %Rrc"), pszLogGetStr, vrc));
        }
        else
            return E_OUTOFMEMORY;

        /* try again with a bigger buffer. */
        cbBuf *= 2;
        AssertReturn(cbBuf <= _256K, setError(E_FAIL, tr("%s returns too much data"), pszLogGetStr));
    }
}

HRESULT MachineDebugger::getLogDbgFlags(com::Utf8Str &aLogDbgFlags)
{
    return i_logStringProps(RTLogGetDefaultInstance(), RTLogGetFlags, "RTGetFlags", &aLogDbgFlags);
}

HRESULT MachineDebugger::getLogDbgGroups(com::Utf8Str &aLogDbgGroups)
{
    return i_logStringProps(RTLogGetDefaultInstance(), RTLogGetGroupSettings, "RTLogGetGroupSettings", &aLogDbgGroups);
}

HRESULT MachineDebugger::getLogDbgDestinations(com::Utf8Str &aLogDbgDestinations)
{
    return i_logStringProps(RTLogGetDefaultInstance(), RTLogGetDestinations, "RTLogGetDestinations", &aLogDbgDestinations);
}

HRESULT MachineDebugger::getLogRelFlags(com::Utf8Str &aLogRelFlags)
{
    return i_logStringProps(RTLogRelGetDefaultInstance(), RTLogGetFlags, "RTGetFlags", &aLogRelFlags);
}

HRESULT MachineDebugger::getLogRelGroups(com::Utf8Str &aLogRelGroups)
{
    return i_logStringProps(RTLogRelGetDefaultInstance(), RTLogGetGroupSettings, "RTLogGetGroupSettings", &aLogRelGroups);
}

HRESULT MachineDebugger::getLogRelDestinations(com::Utf8Str &aLogRelDestinations)
{
    return i_logStringProps(RTLogRelGetDefaultInstance(), RTLogGetDestinations, "RTLogGetDestinations", &aLogRelDestinations);
}

/**
 * Returns the current hardware virtualization flag.
 *
 * @returns COM status code
 * @param   aEnabled address of result variable
 */
HRESULT MachineDebugger::getHWVirtExEnabled(BOOL *aHWVirtExEnabled)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    Console::SafeVMPtrQuiet ptrVM(mParent);

    if (ptrVM.isOk())
        *aHWVirtExEnabled = HMR3IsEnabled(ptrVM.rawUVM());
    else
        *aHWVirtExEnabled = false;

    return S_OK;
}

/**
 * Returns the current nested paging flag.
 *
 * @returns COM status code
 * @param   aEnabled address of result variable
 */
HRESULT MachineDebugger::getHWVirtExNestedPagingEnabled(BOOL *aHWVirtExNestedPagingEnabled)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    Console::SafeVMPtrQuiet ptrVM(mParent);

    if (ptrVM.isOk())
        *aHWVirtExNestedPagingEnabled = HMR3IsNestedPagingActive(ptrVM.rawUVM());
    else
        *aHWVirtExNestedPagingEnabled = false;

    return S_OK;
}

/**
 * Returns the current VPID flag.
 *
 * @returns COM status code
 * @param   aEnabled address of result variable
 */
HRESULT MachineDebugger::getHWVirtExVPIDEnabled(BOOL *aHWVirtExVPIDEnabled)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    Console::SafeVMPtrQuiet ptrVM(mParent);

    if (ptrVM.isOk())
        *aHWVirtExVPIDEnabled = HMR3IsVpidActive(ptrVM.rawUVM());
    else
        *aHWVirtExVPIDEnabled = false;

    return S_OK;
}

/**
 * Returns the current unrestricted execution setting.
 *
 * @returns COM status code
 * @param   aEnabled address of result variable
 */
HRESULT MachineDebugger::getHWVirtExUXEnabled(BOOL *aHWVirtExUXEnabled)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    Console::SafeVMPtrQuiet ptrVM(mParent);

    if (ptrVM.isOk())
        *aHWVirtExUXEnabled = HMR3IsUXActive(ptrVM.rawUVM());
    else
        *aHWVirtExUXEnabled = false;

    return S_OK;
}

HRESULT MachineDebugger::getOSName(com::Utf8Str &aOSName)
{
    LogFlowThisFunc(("\n"));
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        /*
         * Do the job and try convert the name.
         */
        char szName[64];
        int vrc = DBGFR3OSQueryNameAndVersion(ptrVM.rawUVM(), szName, sizeof(szName), NULL, 0);
        if (RT_SUCCESS(vrc))
        {
            try
            {
                Bstr bstrName(szName);
                aOSName = Utf8Str(bstrName);
            }
            catch (std::bad_alloc)
            {
                hrc = E_OUTOFMEMORY;
            }
        }
        else
            hrc = setError(VBOX_E_VM_ERROR, tr("DBGFR3OSQueryNameAndVersion failed with %Rrc"), vrc);
    }
    return hrc;
}

HRESULT MachineDebugger::getOSVersion(com::Utf8Str &aOSVersion)
{
    LogFlowThisFunc(("\n"));
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        /*
         * Do the job and try convert the name.
         */
        char szVersion[256];
        int vrc = DBGFR3OSQueryNameAndVersion(ptrVM.rawUVM(), NULL, 0, szVersion, sizeof(szVersion));
        if (RT_SUCCESS(vrc))
        {
            try
            {
                Bstr bstrVersion(szVersion);
                aOSVersion = Utf8Str(bstrVersion);
            }
            catch (std::bad_alloc)
            {
                hrc = E_OUTOFMEMORY;
            }
        }
        else
            hrc = setError(VBOX_E_VM_ERROR, tr("DBGFR3OSQueryNameAndVersion failed with %Rrc"), vrc);
    }
    return hrc;
}

/**
 * Returns the current PAE flag.
 *
 * @returns COM status code
 * @param   aEnabled address of result variable
 */
HRESULT MachineDebugger::getPAEEnabled(BOOL *aPAEEnabled)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    Console::SafeVMPtrQuiet ptrVM(mParent);

    if (ptrVM.isOk())
    {
        uint32_t cr4;
        int rc = DBGFR3RegCpuQueryU32(ptrVM.rawUVM(), 0 /*idCpu*/,  DBGFREG_CR4, &cr4); AssertRC(rc);
        *aPAEEnabled = RT_BOOL(cr4 & X86_CR4_PAE);
    }
    else
        *aPAEEnabled = false;

    return S_OK;
}

/**
 * Returns the current virtual time rate.
 *
 * @returns COM status code.
 * @param   a_puPct      Where to store the rate.
 */
HRESULT MachineDebugger::getVirtualTimeRate(ULONG *aVirtualTimeRate)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
        *aVirtualTimeRate = TMR3GetWarpDrive(ptrVM.rawUVM());

    return hrc;
}

/**
 * Returns the current virtual time rate.
 *
 * @returns COM status code.
 * @param   aPct     Where to store the rate.
 */
HRESULT MachineDebugger::setVirtualTimeRate(ULONG aVirtualTimeRate)
{
    HRESULT hrc = S_OK;

    if (aVirtualTimeRate < 2 || aVirtualTimeRate > 20000)
        return setError(E_INVALIDARG, tr("%u is out of range [2..20000]"), aVirtualTimeRate);

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    if (i_queueSettings())
        mVirtualTimeRateQueued = aVirtualTimeRate;
    else
    {
        Console::SafeVMPtr ptrVM(mParent);
        hrc = ptrVM.rc();
        if (SUCCEEDED(hrc))
        {
            int vrc = TMR3SetWarpDrive(ptrVM.rawUVM(), aVirtualTimeRate);
            if (RT_FAILURE(vrc))
                hrc = setError(VBOX_E_VM_ERROR, tr("TMR3SetWarpDrive(, %u) failed with rc=%Rrc"), aVirtualTimeRate, vrc);
        }
    }

    return hrc;
}

/**
 * Hack for getting the user mode VM handle (UVM).
 *
 * This is only temporary (promise) while prototyping the debugger.
 *
 * @returns COM status code
 * @param   a_u64Vm     Where to store the vm handle. Since there is no
 *                      uintptr_t in COM, we're using the max integer.
 *                      (No, ULONG is not pointer sized!)
 * @remarks The returned handle must be passed to VMR3ReleaseUVM()!
 * @remarks Prior to 4.3 this returned PVM.
 */
HRESULT MachineDebugger::getVM(LONG64 *aVM)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        VMR3RetainUVM(ptrVM.rawUVM());
        *aVM = (intptr_t)ptrVM.rawUVM();
    }

    /*
     * Note! ptrVM protection provided by SafeVMPtr is no long effective
     *       after we return from this method.
     */
    return hrc;
}

// IMachineDebugger methods
/////////////////////////////////////////////////////////////////////////////

HRESULT MachineDebugger::dumpGuestCore(const com::Utf8Str &aFilename, const com::Utf8Str &aCompression)
{
    if (aCompression.length())
        return setError(E_INVALIDARG, tr("The compression parameter must be empty"));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        int vrc = DBGFR3CoreWrite(ptrVM.rawUVM(), aFilename.c_str(), false /*fReplaceFile*/);
        if (RT_SUCCESS(vrc))
            hrc = S_OK;
        else
            hrc = setError(E_FAIL, tr("DBGFR3CoreWrite failed with %Rrc"), vrc);
    }

    return hrc;
}

HRESULT MachineDebugger::dumpHostProcessCore(const com::Utf8Str &aFilename, const com::Utf8Str &aCompression)
{
    ReturnComNotImplemented();
}

/**
 * Debug info string buffer formatter.
 */
typedef struct MACHINEDEBUGGERINOFHLP
{
    /** The core info helper structure. */
    DBGFINFOHLP Core;
    /** Pointer to the buffer. */
    char       *pszBuf;
    /** The size of the buffer. */
    size_t      cbBuf;
    /** The offset into the buffer */
    size_t      offBuf;
    /** Indicates an out-of-memory condition. */
    bool        fOutOfMemory;
} MACHINEDEBUGGERINOFHLP;
/** Pointer to a Debug info string buffer formatter. */
typedef MACHINEDEBUGGERINOFHLP *PMACHINEDEBUGGERINOFHLP;


/**
 * @callback_method_impl{FNRTSTROUTPUT}
 */
static DECLCALLBACK(size_t) MachineDebuggerInfoOutput(void *pvArg, const char *pachChars, size_t cbChars)
{
    PMACHINEDEBUGGERINOFHLP pHlp = (PMACHINEDEBUGGERINOFHLP)pvArg;

    /*
     * Grow the buffer if required.
     */
    size_t const cbRequired  = cbChars + pHlp->offBuf + 1;
    if (cbRequired > pHlp->cbBuf)
    {
        if (RT_UNLIKELY(pHlp->fOutOfMemory))
            return 0;

        size_t cbBufNew = pHlp->cbBuf * 2;
        if (cbRequired > cbBufNew)
            cbBufNew = RT_ALIGN_Z(cbRequired, 256);
        void *pvBufNew = RTMemRealloc(pHlp->pszBuf, cbBufNew);
        if (RT_UNLIKELY(!pvBufNew))
        {
            pHlp->fOutOfMemory = true;
            RTMemFree(pHlp->pszBuf);
            pHlp->pszBuf = NULL;
            pHlp->cbBuf  = 0;
            pHlp->offBuf = 0;
            return 0;
        }

        pHlp->pszBuf = (char *)pvBufNew;
        pHlp->cbBuf  = cbBufNew;
    }

    /*
     * Copy the bytes into the buffer and terminate it.
     */
    memcpy(&pHlp->pszBuf[pHlp->offBuf], pachChars, cbChars);
    pHlp->offBuf += cbChars;
    pHlp->pszBuf[pHlp->offBuf] = '\0';
    Assert(pHlp->offBuf < pHlp->cbBuf);
    return cbChars;
}

/**
 * @interface_method_impl{DBGFINFOHLP, pfnPrintfV}
 */
static DECLCALLBACK(void) MachineDebuggerInfoPrintfV(PCDBGFINFOHLP pHlp, const char *pszFormat, va_list va)
{
    RTStrFormatV(MachineDebuggerInfoOutput, (void *)pHlp, NULL,  NULL, pszFormat, va);
}

/**
 * @interface_method_impl{DBGFINFOHLP, pfnPrintf}
 */
static DECLCALLBACK(void) MachineDebuggerInfoPrintf(PCDBGFINFOHLP pHlp, const char *pszFormat, ...)
{
    va_list va;
    va_start(va, pszFormat);
    MachineDebuggerInfoPrintfV(pHlp, pszFormat, va);
    va_end(va);
}

/**
 * Initializes the debug info string buffer formatter
 *
 * @param   pHlp                The help structure to init.
 */
static void MachineDebuggerInfoInit(PMACHINEDEBUGGERINOFHLP pHlp)
{
    pHlp->Core.pfnPrintf    = MachineDebuggerInfoPrintf;
    pHlp->Core.pfnPrintfV   = MachineDebuggerInfoPrintfV;
    pHlp->pszBuf            = NULL;
    pHlp->cbBuf             = 0;
    pHlp->offBuf            = 0;
    pHlp->fOutOfMemory      = false;
}

/**
 * Deletes the debug info string buffer formatter.
 * @param   pHlp                The helper structure to delete.
 */
static void MachineDebuggerInfoDelete(PMACHINEDEBUGGERINOFHLP pHlp)
{
    RTMemFree(pHlp->pszBuf);
    pHlp->pszBuf = NULL;
}

HRESULT MachineDebugger::info(const com::Utf8Str &aName, const com::Utf8Str &aArgs, com::Utf8Str &aInfo)
{
    LogFlowThisFunc(("\n"));

    /*
     * Do the autocaller and lock bits.
     */
    AutoCaller autoCaller(this);
    HRESULT hrc = autoCaller.rc();
    if (SUCCEEDED(hrc))
    {
        AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
        Console::SafeVMPtr ptrVM(mParent);
        hrc = ptrVM.rc();
        if (SUCCEEDED(hrc))
        {
            /*
             * Create a helper and call DBGFR3Info.
             */
            MACHINEDEBUGGERINOFHLP Hlp;
            MachineDebuggerInfoInit(&Hlp);
            int vrc = DBGFR3Info(ptrVM.rawUVM(),  aName.c_str(),  aArgs.c_str(), &Hlp.Core);
            if (RT_SUCCESS(vrc))
            {
                if (!Hlp.fOutOfMemory)
                {
                    /*
                     * Convert the info string, watching out for allocation errors.
                     */
                    try
                    {
                        Bstr bstrInfo(Hlp.pszBuf);
                        aInfo = bstrInfo;
                    }
                    catch (std::bad_alloc)
                    {
                        hrc = E_OUTOFMEMORY;
                    }
                }
                else
                    hrc = E_OUTOFMEMORY;
            }
            else
                hrc = setError(VBOX_E_VM_ERROR, tr("DBGFR3Info failed with %Rrc"), vrc);
            MachineDebuggerInfoDelete(&Hlp);
        }
    }
    return hrc;
}

HRESULT MachineDebugger::injectNMI()
{
    LogFlowThisFunc(("\n"));

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        int vrc = DBGFR3InjectNMI(ptrVM.rawUVM(), 0);
        if (RT_SUCCESS(vrc))
            hrc = S_OK;
        else
            hrc = setError(E_FAIL, tr("DBGFR3InjectNMI failed with %Rrc"), vrc);
    }
    return hrc;
}

HRESULT MachineDebugger::modifyLogFlags(const com::Utf8Str &aSettings)
{
    LogFlowThisFunc(("aSettings=%s\n", aSettings.c_str()));
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        int vrc = DBGFR3LogModifyFlags(ptrVM.rawUVM(), aSettings.c_str());
        if (RT_SUCCESS(vrc))
            hrc = S_OK;
        else
            hrc = setError(E_FAIL, tr("DBGFR3LogModifyFlags failed with %Rrc"), vrc);
    }
    return hrc;
}

HRESULT MachineDebugger::modifyLogGroups(const com::Utf8Str &aSettings)
{
    LogFlowThisFunc(("aSettings=%s\n", aSettings.c_str()));
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        int vrc = DBGFR3LogModifyGroups(ptrVM.rawUVM(), aSettings.c_str());
        if (RT_SUCCESS(vrc))
            hrc = S_OK;
        else
            hrc = setError(E_FAIL, tr("DBGFR3LogModifyGroups failed with %Rrc"), vrc);
    }
    return hrc;
}

HRESULT MachineDebugger::modifyLogDestinations(const com::Utf8Str &aSettings)
{
    LogFlowThisFunc(("aSettings=%s\n", aSettings.c_str()));
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        int vrc = DBGFR3LogModifyDestinations(ptrVM.rawUVM(), aSettings.c_str());
        if (RT_SUCCESS(vrc))
            hrc = S_OK;
        else
            hrc = setError(E_FAIL, tr("DBGFR3LogModifyDestinations failed with %Rrc"), vrc);
    }
    return hrc;
}

HRESULT MachineDebugger::readPhysicalMemory(LONG64 aAddress, ULONG aSize, std::vector<BYTE> &aBytes)
{
    ReturnComNotImplemented();
}

HRESULT MachineDebugger::writePhysicalMemory(LONG64 aAddress, ULONG aSize, const std::vector<BYTE> &aBytes)
{
    ReturnComNotImplemented();
}

HRESULT MachineDebugger::readVirtualMemory(ULONG aCpuId, LONG64 aAddress, ULONG aSize, std::vector<BYTE> &aBytes)
{
    ReturnComNotImplemented();
}

HRESULT MachineDebugger::writeVirtualMemory(ULONG aCpuId, LONG64 aAddress, ULONG aSize, const std::vector<BYTE> &aBytes)
{
    ReturnComNotImplemented();
}

HRESULT MachineDebugger::loadPlugIn(const com::Utf8Str &aName, com::Utf8Str &aPlugInName)
{
    /*
     * Lock the debugger and get the VM pointer
     */
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        /*
         * Do the job and try convert the name.
         */
        if (aName.equals("all"))
        {
            DBGFR3PlugInLoadAll(ptrVM.rawUVM());
            try
            {
                aPlugInName = "all";
                hrc = S_OK;
            }
            catch (std::bad_alloc)
            {
                hrc = E_OUTOFMEMORY;
            }
        }
        else
        {
            RTERRINFOSTATIC ErrInfo;
            char            szName[80];
            int vrc = DBGFR3PlugInLoad(ptrVM.rawUVM(), aName.c_str(), szName, sizeof(szName), RTErrInfoInitStatic(&ErrInfo));
            if (RT_SUCCESS(vrc))
            {
                try
                {
                    aPlugInName = "all";
                    hrc = S_OK;
                }
                catch (std::bad_alloc)
                {
                    hrc = E_OUTOFMEMORY;
                }
            }
            else
                hrc = setErrorVrc(vrc, "%s", ErrInfo.szMsg);
        }
    }
    return hrc;

}

HRESULT MachineDebugger::unloadPlugIn(const com::Utf8Str &aName)
{
    /*
     * Lock the debugger and get the VM pointer
     */
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        /*
         * Do the job and try convert the name.
         */
        if (aName.equals("all"))
        {
            DBGFR3PlugInUnloadAll(ptrVM.rawUVM());
            hrc = S_OK;
        }
        else
        {
            int vrc = DBGFR3PlugInUnload(ptrVM.rawUVM(), aName.c_str());
            if (RT_SUCCESS(vrc))
                hrc = S_OK;
            else if (vrc == VERR_NOT_FOUND)
                hrc = setErrorBoth(E_FAIL, vrc, "Plug-in '%s' was not found", aName.c_str());
            else
                hrc = setErrorVrc(vrc, "Error unloading '%s': %Rrc", aName.c_str(), vrc);
        }
    }
    return hrc;

}

HRESULT MachineDebugger::detectOS(com::Utf8Str &aOs)
{
    LogFlowThisFunc(("\n"));

    /*
     * Lock the debugger and get the VM pointer
     */
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        /*
         * Do the job.
         */
        char szName[64];
        int vrc = DBGFR3OSDetect(ptrVM.rawUVM(), szName, sizeof(szName));
        if (RT_SUCCESS(vrc) && vrc != VINF_DBGF_OS_NOT_DETCTED)
        {
            try
            {
                aOs = szName;
            }
            catch (std::bad_alloc)
            {
                hrc = E_OUTOFMEMORY;
            }
        }
        else
            hrc = setError(VBOX_E_VM_ERROR, tr("DBGFR3OSDetect failed with %Rrc"), vrc);
    }
    return hrc;
}

HRESULT MachineDebugger::queryOSKernelLog(ULONG aMaxMessages, com::Utf8Str &aDmesg)
{
    /*
     * Lock the debugger and get the VM pointer
     */
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        PDBGFOSIDMESG pDmesg = (PDBGFOSIDMESG)DBGFR3OSQueryInterface(ptrVM.rawUVM(), DBGFOSINTERFACE_DMESG);
        if (pDmesg)
        {
            size_t   cbActual;
            size_t   cbBuf  = _512K;
            int vrc = aDmesg.reserveNoThrow(cbBuf);
            if (RT_SUCCESS(vrc))
            {
                uint32_t cMessages = aMaxMessages == 0 ? UINT32_MAX : aMaxMessages;
                vrc = pDmesg->pfnQueryKernelLog(pDmesg, ptrVM.rawUVM(), 0 /*fFlags*/, cMessages,
                                                aDmesg.mutableRaw(), cbBuf, &cbActual);

                uint32_t cTries = 10;
                while (vrc == VERR_BUFFER_OVERFLOW && cbBuf < 16*_1M && cTries-- > 0)
                {
                    cbBuf = RT_ALIGN_Z(cbActual + _4K, _4K);
                    vrc = aDmesg.reserveNoThrow(cbBuf);
                    if (RT_SUCCESS(vrc))
                        vrc = pDmesg->pfnQueryKernelLog(pDmesg, ptrVM.rawUVM(), 0 /*fFlags*/, cMessages,
                                                        aDmesg.mutableRaw(), cbBuf, &cbActual);
                }
                if (RT_SUCCESS(vrc))
                    aDmesg.jolt();
                else if (vrc == VERR_BUFFER_OVERFLOW)
                    hrc = setError(E_FAIL, "Too much log available, must use the maxMessages parameter to restrict.");
                else
                    hrc = setErrorVrc(vrc);
            }
            else
                hrc = setErrorBoth(E_OUTOFMEMORY, vrc);
        }
        else
            hrc = setError(E_FAIL, "The dmesg interface isn't implemented by guest OS digger, or detectOS() has not been called.");
    }
    return hrc;
}

/**
 * Formats a register value.
 *
 * This is used by both register getter methods.
 *
 * @returns
 * @param   a_pbstr             The output Bstr variable.
 * @param   a_pValue            The value to format.
 * @param   a_enmType           The type of the value.
 */
DECLINLINE(HRESULT) formatRegisterValue(Bstr *a_pbstr, PCDBGFREGVAL a_pValue, DBGFREGVALTYPE a_enmType)
{
    char szHex[160];
    ssize_t cch = DBGFR3RegFormatValue(szHex, sizeof(szHex), a_pValue, a_enmType, true /*fSpecial*/);
    if (RT_UNLIKELY(cch <= 0))
        return E_UNEXPECTED;
    *a_pbstr = szHex;
    return S_OK;
}

HRESULT MachineDebugger::getRegister(ULONG aCpuId, const com::Utf8Str &aName, com::Utf8Str &aValue)
{
    /*
     * The prologue.
     */
    LogFlowThisFunc(("\n"));
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        /*
         * Real work.
         */
        DBGFREGVAL      Value;
        DBGFREGVALTYPE  enmType;
        int vrc = DBGFR3RegNmQuery(ptrVM.rawUVM(), aCpuId, aName.c_str(), &Value, &enmType);
        if (RT_SUCCESS(vrc))
        {
            try
            {
                Bstr bstrValue;
                hrc = formatRegisterValue(&bstrValue, &Value, enmType);
                if (SUCCEEDED(hrc))
                    aValue = Utf8Str(bstrValue);
            }
            catch (std::bad_alloc)
            {
                hrc = E_OUTOFMEMORY;
            }
        }
        else if (vrc == VERR_DBGF_REGISTER_NOT_FOUND)
            hrc = setError(E_FAIL, tr("Register '%s' was not found"), aName.c_str());
        else if (vrc == VERR_INVALID_CPU_ID)
            hrc = setError(E_FAIL, tr("Invalid CPU ID: %u"), aCpuId);
        else
            hrc = setError(VBOX_E_VM_ERROR,
                           tr("DBGFR3RegNmQuery failed with rc=%Rrc querying register '%s' with default cpu set to %u"),
                           vrc, aName.c_str(), aCpuId);
    }

    return hrc;
}

HRESULT MachineDebugger::getRegisters(ULONG aCpuId, std::vector<com::Utf8Str> &aNames, std::vector<com::Utf8Str> &aValues)
{
    /*
     * The prologue.
     */
    LogFlowThisFunc(("\n"));
    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);
    Console::SafeVMPtr ptrVM(mParent);
    HRESULT hrc = ptrVM.rc();
    if (SUCCEEDED(hrc))
    {
        /*
         * Real work.
         */
        size_t cRegs;
        int vrc = DBGFR3RegNmQueryAllCount(ptrVM.rawUVM(), &cRegs);
        if (RT_SUCCESS(vrc))
        {
            PDBGFREGENTRYNM paRegs = (PDBGFREGENTRYNM)RTMemAllocZ(sizeof(paRegs[0]) * cRegs);
            if (paRegs)
            {
                vrc = DBGFR3RegNmQueryAll(ptrVM.rawUVM(), paRegs, cRegs);
                if (RT_SUCCESS(vrc))
                {
                    try
                    {
                        aValues.resize(cRegs);
                        aNames.resize(cRegs);
                        for (uint32_t iReg = 0; iReg < cRegs; iReg++)
                        {
                            char szHex[160];
                            szHex[159] = szHex[0] = '\0';
                            ssize_t cch = DBGFR3RegFormatValue(szHex, sizeof(szHex), &paRegs[iReg].Val,
                                                               paRegs[iReg].enmType, true /*fSpecial*/);
                            Assert(cch > 0);
                            aNames[iReg] = Utf8Str(paRegs[iReg].pszName);
                            aValues[iReg] = Utf8Str(szHex);
                        }
                    }
                    catch (std::bad_alloc)
                    {
                        hrc = E_OUTOFMEMORY;
                    }
                }
                else
                    hrc = setError(E_FAIL, tr("DBGFR3RegNmQueryAll failed with %Rrc"), vrc);

                RTMemFree(paRegs);
            }
            else
                hrc = E_OUTOFMEMORY;
        }
        else
            hrc = setError(E_FAIL, tr("DBGFR3RegNmQueryAllCount failed with %Rrc"), vrc);
    }
    return hrc;
}

HRESULT MachineDebugger::setRegister(ULONG aCpuId, const com::Utf8Str &aName, const com::Utf8Str &aValue)
{
    ReturnComNotImplemented();
}

HRESULT MachineDebugger::setRegisters(ULONG aCpuId, const std::vector<com::Utf8Str> &aNames,
                                      const std::vector<com::Utf8Str> &aValues)
{
    ReturnComNotImplemented();
}

HRESULT MachineDebugger::dumpGuestStack(ULONG aCpuId, com::Utf8Str &aStack)
{
    ReturnComNotImplemented();
}

/**
 * Resets VM statistics.
 *
 * @returns COM status code.
 * @param   aPattern            The selection pattern. A bit similar to filename globbing.
 */
HRESULT MachineDebugger::resetStats(const com::Utf8Str &aPattern)
{
    Console::SafeVMPtrQuiet ptrVM(mParent);

    if (!ptrVM.isOk())
        return setError(VBOX_E_INVALID_VM_STATE, "Machine is not running");

    STAMR3Reset(ptrVM.rawUVM(), aPattern.c_str());

    return S_OK;
}

/**
 * Dumps VM statistics to the log.
 *
 * @returns COM status code.
 * @param   aPattern            The selection pattern. A bit similar to filename globbing.
 */
HRESULT MachineDebugger::dumpStats(const com::Utf8Str &aPattern)
{
    Console::SafeVMPtrQuiet ptrVM(mParent);

    if (!ptrVM.isOk())
        return setError(VBOX_E_INVALID_VM_STATE, "Machine is not running");

    STAMR3Dump(ptrVM.rawUVM(), aPattern.c_str());

    return S_OK;
}

/**
 * Get the VM statistics in an XML format.
 *
 * @returns COM status code.
 * @param   aPattern            The selection pattern. A bit similar to filename globbing.
 * @param   aWithDescriptions   Whether to include the descriptions.
 * @param   aStats              The XML document containing the statistics.
 */
HRESULT MachineDebugger::getStats(const com::Utf8Str &aPattern, BOOL aWithDescriptions, com::Utf8Str &aStats)
{
    Console::SafeVMPtrQuiet ptrVM (mParent);

    if (!ptrVM.isOk())
        return setError(VBOX_E_INVALID_VM_STATE, "Machine is not running");

    char *pszSnapshot;
    int vrc = STAMR3Snapshot(ptrVM.rawUVM(), aPattern.c_str(), &pszSnapshot, NULL,
                             !!aWithDescriptions);
    if (RT_FAILURE(vrc))
        return vrc == VERR_NO_MEMORY ? E_OUTOFMEMORY : E_FAIL;

    /** @todo this is horribly inefficient! And it's kinda difficult to tell whether it failed...
     * Must use UTF-8 or ASCII here and completely avoid these two extra copy operations.
     * Until that's done, this method is kind of useless for debugger statistics GUI because
     * of the amount statistics in a debug build. */
    aStats = Utf8Str(pszSnapshot);
    STAMR3SnapshotFree(ptrVM.rawUVM(), pszSnapshot);

    return S_OK;
}


// public methods only for internal purposes
/////////////////////////////////////////////////////////////////////////////

void MachineDebugger::i_flushQueuedSettings()
{
    mFlushMode = true;
    if (mSingleStepQueued != ~0)
    {
        COMSETTER(SingleStep)(mSingleStepQueued);
        mSingleStepQueued = ~0;
    }
    for (unsigned i = 0; i < EMEXECPOLICY_END; i++)
        if (maiQueuedEmExecPolicyParams[i] != UINT8_MAX)
        {
            i_setEmExecPolicyProperty((EMEXECPOLICY)i, RT_BOOL(maiQueuedEmExecPolicyParams[i]));
            maiQueuedEmExecPolicyParams[i] = UINT8_MAX;
        }
    if (mPatmEnabledQueued != ~0)
    {
        COMSETTER(PATMEnabled)(mPatmEnabledQueued);
        mPatmEnabledQueued = ~0;
    }
    if (mCsamEnabledQueued != ~0)
    {
        COMSETTER(CSAMEnabled)(mCsamEnabledQueued);
        mCsamEnabledQueued = ~0;
    }
    if (mLogEnabledQueued != ~0)
    {
        COMSETTER(LogEnabled)(mLogEnabledQueued);
        mLogEnabledQueued = ~0;
    }
    if (mVirtualTimeRateQueued != ~(uint32_t)0)
    {
        COMSETTER(VirtualTimeRate)(mVirtualTimeRateQueued);
        mVirtualTimeRateQueued = ~0;
    }
    mFlushMode = false;
}

// private methods
/////////////////////////////////////////////////////////////////////////////

bool MachineDebugger::i_queueSettings() const
{
    if (!mFlushMode)
    {
        // check if the machine is running
        MachineState_T machineState;
        mParent->COMGETTER(State)(&machineState);
        switch (machineState)
        {
            // queue the request
            default:
                return true;

            case MachineState_Running:
            case MachineState_Paused:
            case MachineState_Stuck:
            case MachineState_LiveSnapshotting:
            case MachineState_Teleporting:
                break;
        }
    }
    return false;
}
/* vi: set tabstop=4 shiftwidth=4 expandtab: */
