/* $Id$ */
/** @file
 * IPRT - Tricks to make the Visual C++ 2010 CRT work on NT4, W2K and XP.
 */

/*
 * Copyright (C) 2012-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * The contents of this file may alternatively be used under the terms
 * of the Common Development and Distribution License Version 1.0
 * (CDDL) only, as it comes in the "COPYING.CDDL" file of the
 * VirtualBox OSE distribution, in which case the provisions of the
 * CDDL are applicable instead of those of the GPL.
 *
 * You may elect to license modified versions of this file under the
 * terms and conditions of either the GPL or the CDDL or both.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#include <iprt/cdefs.h>

#ifndef RT_ARCH_X86
# error "This code is X86 only"
#endif

#define DecodePointer                           Ignore_DecodePointer
#define EncodePointer                           Ignore_EncodePointer
#define InitializeCriticalSectionAndSpinCount   Ignore_InitializeCriticalSectionAndSpinCount
#define HeapSetInformation                      Ignore_HeapSetInformation
#define HeapQueryInformation                    Ignore_HeapQueryInformation
#include <Windows.h>
#undef DecodePointer
#undef EncodePointer
#undef InitializeCriticalSectionAndSpinCount
#undef HeapSetInformation
#undef HeapQueryInformation


#ifndef HEAP_STANDARD
# define HEAP_STANDARD 0
#endif


/** @todo Try dynamically resolve the functions the first time one of them is
 *        called. */

extern "C"
__declspec(dllexport) PVOID WINAPI
DecodePointer(PVOID pvEncoded)
{
    /*
     * Fallback code.
     */
    return pvEncoded;
}


extern "C"
__declspec(dllexport) PVOID WINAPI
EncodePointer(PVOID pvNative)
{
    /*
     * Fallback code.
     */
    return pvNative;
}


extern "C"
__declspec(dllexport) BOOL WINAPI
InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION pCritSect, DWORD cSpin)
{

    /*
     * Fallback code.
     */
    InitializeCriticalSection(pCritSect);
    return TRUE;
}


extern "C"
__declspec(dllexport) BOOL WINAPI
HeapSetInformation(HANDLE hHeap, HEAP_INFORMATION_CLASS enmInfoClass, PVOID pvBuf, SIZE_T cbBuf)
{
    /*
     * Fallback code.
     */
    if (enmInfoClass == HeapCompatibilityInformation)
    {
        if (   cbBuf != sizeof(ULONG)
            || !pvBuf
            || *(PULONG)pvBuf == HEAP_STANDARD
           )
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        return TRUE;
    }

    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}


extern "C"
__declspec(dllexport) BOOL WINAPI
HeapQueryInformation(HANDLE hHeap, HEAP_INFORMATION_CLASS enmInfoClass, PVOID pvBuf, SIZE_T cbBuf, PSIZE_T pcbRet)
{

    /*
     * Fallback code.
     */
    if (enmInfoClass == HeapCompatibilityInformation)
    {
        *pcbRet = sizeof(ULONG);
        if (cbBuf < sizeof(ULONG) || !pvBuf)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        *(PULONG)pvBuf = HEAP_STANDARD;
        return TRUE;
    }

    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

