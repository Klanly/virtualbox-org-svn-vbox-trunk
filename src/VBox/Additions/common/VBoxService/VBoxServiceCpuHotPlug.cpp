/* $Id$ */
/** @file
 * VBoxService - Guest Additions CPU Hot Plugging Service.
 */

/*
 * Copyright (C) 2010-2015 Oracle Corporation
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
#include <iprt/assert.h>
#include <iprt/dir.h>
#include <iprt/file.h>
#include <iprt/mem.h>
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/thread.h>
#include <VBox/VBoxGuestLib.h>
#include "VBoxServiceInternal.h"

#ifdef RT_OS_LINUX
# include <iprt/linux/sysfs.h>
# include <errno.h> /* For the sysfs API */
#endif


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
#ifdef RT_OS_LINUX
/** @name Paths to access the CPU device
 * @{
 */
# define SYSFS_ACPI_CPU_PATH    "/sys/devices"
# define SYSFS_CPU_PATH         "/sys/devices/system/cpu"
/** @} */

/** Path component for the ACPI CPU path. */
typedef struct SYSFSCPUPATHCOMP
{
    /** Flag whether the name is suffixed with a number */
    bool        fNumberedSuffix;
    /** Name of the component */
    const char *pcszName;
} SYSFSCPUPATHCOMP, *PSYSFSCPUPATHCOMP;
/** Pointer to a const component. */
typedef const SYSFSCPUPATHCOMP *PCSYSFSCPUPATHCOMP;

/**
 * Structure which defines how the entries are assembled.
 */
typedef struct SYSFSCPUPATH
{
    /** Id when probing for the correct path. */
    uint32_t           uId;
    /** Array holding the possible components. */
    PCSYSFSCPUPATHCOMP aComponentsPossible;
    /** Number of entries in the array, excluding the terminator. */
    unsigned           cComponents;
    /** Directory handle */
    PRTDIR             pDir;
    /** Current directory to try. */
    char              *pszPath;
} SYSFSCPUPATH, *PSYSFSCPUPATH;

/** Content of uId if the path wasn't probed yet. */
#define ACPI_CPU_PATH_NOT_PROBED UINT32_MAX

/** Possible combinations of all path components for level 1. */
const SYSFSCPUPATHCOMP g_aAcpiCpuPathLvl1[] =
{
    /** LNXSYSTEM:<id> */
    {true, "LNXSYSTM:*"}
};

/** Possible combinations of all path components for level 2. */
const SYSFSCPUPATHCOMP g_aAcpiCpuPathLvl2[] =
{
    /** device:<id> */
    {true, "device:*"},
    /** LNXSYBUS:<id> */
    {true, "LNXSYBUS:*"}
};

/** Possible combinations of all path components for level 3 */
const SYSFSCPUPATHCOMP g_aAcpiCpuPathLvl3[] =
{
    /** ACPI0004:<id> */
    {true, "ACPI0004:*"}
};

/** Possible combinations of all path components for level 4 */
const SYSFSCPUPATHCOMP g_aAcpiCpuPathLvl4[] =
{
    /** LNXCPU:<id> */
    {true, "LNXCPU:*"},
    /** ACPI_CPU:<id> */
    {true, "ACPI_CPU:*"}
};

/** All possible combinations. */
SYSFSCPUPATH g_aAcpiCpuPath[] =
{
    /** Level 1 */
    {ACPI_CPU_PATH_NOT_PROBED, g_aAcpiCpuPathLvl1, RT_ELEMENTS(g_aAcpiCpuPathLvl1), NULL, NULL},
    /** Level 2 */
    {ACPI_CPU_PATH_NOT_PROBED, g_aAcpiCpuPathLvl2, RT_ELEMENTS(g_aAcpiCpuPathLvl2), NULL, NULL},
    /** Level 3 */
    {ACPI_CPU_PATH_NOT_PROBED, g_aAcpiCpuPathLvl3, RT_ELEMENTS(g_aAcpiCpuPathLvl3), NULL, NULL},
    /** Level 4 */
    {ACPI_CPU_PATH_NOT_PROBED, g_aAcpiCpuPathLvl4, RT_ELEMENTS(g_aAcpiCpuPathLvl4), NULL, NULL},
};

/**
 * Possible directories to get to the topology directory for reading core and package id.
 *
 * @remark: This is not part of the path above because the eject file is not in one of the directories
 *          below and would make the hot unplug code fail.
 */
const char *g_apszTopologyPath[] =
{
    "sysdev",
    "physical_node"
};
#endif

#ifdef RT_OS_LINUX
/**
 * Probes for the correct path to the ACPI CPU object in sysfs for the
 * various different kernel versions and distro's.
 *
 * @returns VBox status code.
 */
static int VBoxServiceCpuHotPlugProbePath(void)
{
    int rc = VINF_SUCCESS;

    /* Probe for the correct path if we didn't already. */
    if (RT_UNLIKELY(g_aAcpiCpuPath[0].uId == ACPI_CPU_PATH_NOT_PROBED))
    {
        char *pszPath = NULL;   /** < Current path, increasing while we dig deeper. */

        pszPath = RTStrDup(SYSFS_ACPI_CPU_PATH);
        if (!pszPath)
            return VERR_NO_MEMORY;

        /*
         * Simple algorithm to find the path.
         * Performance is not a real problem because it is
         * only executed once.
         */
        for (unsigned iLvlCurr = 0; iLvlCurr < RT_ELEMENTS(g_aAcpiCpuPath); iLvlCurr++)
        {
            PSYSFSCPUPATH pAcpiCpuPathLvl = &g_aAcpiCpuPath[iLvlCurr];

            for (unsigned iCompCurr = 0; iCompCurr < pAcpiCpuPathLvl->cComponents; iCompCurr++)
            {
                PCSYSFSCPUPATHCOMP pPathComponent = &pAcpiCpuPathLvl->aComponentsPossible[iCompCurr];

                /* Open the directory */
                PRTDIR pDirCurr = NULL;
                char *pszPathTmp = RTPathJoinA(pszPath, pPathComponent->pcszName);
                if (pszPathTmp)
                {
                    rc = RTDirOpenFiltered(&pDirCurr, pszPathTmp, RTDIRFILTER_WINNT, 0);
                    RTStrFree(pszPathTmp);
                }
                else
                    rc = VERR_NO_STR_MEMORY;
                if (RT_FAILURE(rc))
                    break;

                /* Search if the current directory contains one of the possible parts. */
                size_t cchName = strlen(pPathComponent->pcszName);
                RTDIRENTRY DirFolderContent;
                bool fFound = false;

                /* Get rid of the * filter which is in the path component. */
                if (pPathComponent->fNumberedSuffix)
                    cchName--;

                while (RT_SUCCESS(RTDirRead(pDirCurr, &DirFolderContent, NULL))) /* Assumption that szName has always enough space */
                {
                    if (   DirFolderContent.cbName >= cchName
                        && !strncmp(DirFolderContent.szName, pPathComponent->pcszName, cchName))
                    {
                        /* Found, use the complete name to dig deeper. */
                        fFound = true;
                        pAcpiCpuPathLvl->uId = iCompCurr;
                        char *pszPathLvl = RTPathJoinA(pszPath, DirFolderContent.szName);
                        if (pszPathLvl)
                        {
                            RTStrFree(pszPath);
                            pszPath = pszPathLvl;
                        }
                        else
                            rc = VERR_NO_STR_MEMORY;
                        break;
                    }
                }
                RTDirClose(pDirCurr);

                if (fFound)
                    break;
            } /* For every possible component. */

            /* No matching component for this part, no need to continue */
            if (RT_FAILURE(rc))
                break;
        } /* For every level */

        VBoxServiceVerbose(1, "Final path after probing %s rc=%Rrc\n", pszPath, rc);
        RTStrFree(pszPath);
    }

    return rc;
}

/**
 * Returns the path of the ACPI CPU device with the given core and package ID.
 *
 * @returns VBox status code.
 * @param   ppszPath     Where to store the path.
 * @param   idCpuCore    The core ID of the CPU.
 * @param   idCpuPackage The package ID of the CPU.
 */
static int VBoxServiceCpuHotPlugGetACPIDevicePath(char **ppszPath, uint32_t idCpuCore, uint32_t idCpuPackage)
{
    int rc = VINF_SUCCESS;

    AssertPtrReturn(ppszPath, VERR_INVALID_PARAMETER);

    rc = VBoxServiceCpuHotPlugProbePath();
    if (RT_SUCCESS(rc))
    {
        /* Build the path from all components. */
        bool fFound = false;
        unsigned iLvlCurr = 0;
        char *pszPath = NULL;
        char *pszPathDir = NULL;
        PSYSFSCPUPATH pAcpiCpuPathLvl = &g_aAcpiCpuPath[iLvlCurr];

        /* Init everything. */
        Assert(pAcpiCpuPathLvl->uId != ACPI_CPU_PATH_NOT_PROBED);
        pszPath = RTPathJoinA(SYSFS_ACPI_CPU_PATH, pAcpiCpuPathLvl->aComponentsPossible[pAcpiCpuPathLvl->uId].pcszName);
        if (!pszPath)
            return VERR_NO_STR_MEMORY;

        pAcpiCpuPathLvl->pszPath = RTStrDup(SYSFS_ACPI_CPU_PATH);
        if (!pAcpiCpuPathLvl->pszPath)
        {
            RTStrFree(pszPath);
            return VERR_NO_STR_MEMORY;
        }

        /* Open the directory */
        rc = RTDirOpenFiltered(&pAcpiCpuPathLvl->pDir, pszPath, RTDIRFILTER_WINNT, 0);
        if (RT_SUCCESS(rc))
        {
            RTStrFree(pszPath);

            /* Search for CPU */
            while (!fFound)
            {
                /* Get the next directory. */
                RTDIRENTRY DirFolderContent;
                rc = RTDirRead(pAcpiCpuPathLvl->pDir, &DirFolderContent, NULL);
                if (RT_SUCCESS(rc))
                {
                    /* Create the new path. */
                    char *pszPathCurr = RTPathJoinA(pAcpiCpuPathLvl->pszPath, DirFolderContent.szName);
                    if (!pszPathCurr)
                    {
                        rc = VERR_NO_STR_MEMORY;
                        break;
                    }

                    /* If this is the last level check for the given core and package id. */
                    if (iLvlCurr == RT_ELEMENTS(g_aAcpiCpuPath) - 1)
                    {
                        /* Get the sysdev */
                        uint32_t idCore = 0;
                        uint32_t idPackage = 0;

                        for (unsigned i = 0; i < RT_ELEMENTS(g_apszTopologyPath); i++)
                        {
                            int64_t i64Core    = RTLinuxSysFsReadIntFile(10, "%s/%s/topology/core_id",
                                                                         pszPathCurr, g_apszTopologyPath[i]);
                            int64_t i64Package = RTLinuxSysFsReadIntFile(10, "%s/%s/topology/physical_package_id",
                                                                         pszPathCurr, g_apszTopologyPath[i]);

                            if (   i64Core != -1
                                && i64Package != -1)
                            {
                                idCore = (uint32_t)i64Core;
                                idPackage = (uint32_t)i64Package;
                                break;
                            }
                        }

                        if (   idCore    == idCpuCore
                            && idPackage == idCpuPackage)
                        {
                            /* Return the path */
                            pszPath = pszPathCurr;
                            fFound = true;
                            VBoxServiceVerbose(3, "CPU found\n");
                            break;
                        }
                        else
                        {
                            /* Get the next directory. */
                            RTStrFree(pszPathCurr);
                            VBoxServiceVerbose(3, "CPU doesn't match, next directory\n");
                        }
                    }
                    else
                    {
                        /* Go deeper */
                        iLvlCurr++;

                        VBoxServiceVerbose(3, "Going deeper (iLvlCurr=%u)\n", iLvlCurr);

                        pAcpiCpuPathLvl = &g_aAcpiCpuPath[iLvlCurr];

                        Assert(!pAcpiCpuPathLvl->pDir);
                        Assert(!pAcpiCpuPathLvl->pszPath);
                        pAcpiCpuPathLvl->pszPath = pszPathCurr;
                        PCSYSFSCPUPATHCOMP pPathComponent = &pAcpiCpuPathLvl->aComponentsPossible[pAcpiCpuPathLvl->uId];

                        Assert(pAcpiCpuPathLvl->uId != ACPI_CPU_PATH_NOT_PROBED);

                        pszPathDir = RTPathJoinA(pszPathCurr, pPathComponent->pcszName);
                        if (!pszPathDir)
                        {
                            rc = VERR_NO_STR_MEMORY;
                            break;
                        }

                        VBoxServiceVerbose(3, "New path %s\n", pszPathDir);

                        /* Open the directory */
                        rc = RTDirOpenFiltered(&pAcpiCpuPathLvl->pDir, pszPathDir, RTDIRFILTER_WINNT, 0);
                        if (RT_FAILURE(rc))
                            break;
                    }
                }
                else
                {
                    /* Go back one level and try to get the next entry. */
                    Assert(iLvlCurr > 0);

                    RTDirClose(pAcpiCpuPathLvl->pDir);
                    RTStrFree(pAcpiCpuPathLvl->pszPath);
                    pAcpiCpuPathLvl->pDir = NULL;
                    pAcpiCpuPathLvl->pszPath = NULL;

                    iLvlCurr--;
                    pAcpiCpuPathLvl = &g_aAcpiCpuPath[iLvlCurr];
                    VBoxServiceVerbose(3, "Directory not found, going back (iLvlCurr=%u)\n", iLvlCurr);
                }
            } /* while not found */
        } /* Successful init */

        /* Cleanup */
        for (unsigned i = 0; i < RT_ELEMENTS(g_aAcpiCpuPath); i++)
        {
            if (g_aAcpiCpuPath[i].pDir)
                RTDirClose(g_aAcpiCpuPath[i].pDir);
            if (g_aAcpiCpuPath[i].pszPath)
                RTStrFree(g_aAcpiCpuPath[i].pszPath);
            g_aAcpiCpuPath[i].pDir = NULL;
            g_aAcpiCpuPath[i].pszPath = NULL;
        }
        if (pszPathDir)
            RTStrFree(pszPathDir);
        if (RT_FAILURE(rc) && pszPath)
            RTStrFree(pszPath);

        if (RT_SUCCESS(rc))
            *ppszPath = pszPath;
    }

    return rc;
}
#endif /* RT_OS_LINUX */


/**
 * Handles VMMDevCpuEventType_Plug.
 *
 * @param   idCpuCore       The CPU core ID.
 * @param   idCpuPackage    The CPU package ID.
 */
static void VBoxServiceCpuHotPlugHandlePlugEvent(uint32_t idCpuCore, uint32_t idCpuPackage)
{
#ifdef RT_OS_LINUX
    /*
     * The topology directory (containing the physical and core id properties)
     * is not available until the CPU is online. So we just iterate over all directories
     * and enable every CPU which is not online already.
     * Because the directory might not be available immediately we try a few times.
     *
     * @todo: Maybe use udev to monitor hot-add events from the kernel
     */
    bool fCpuOnline = false;
    unsigned cTries = 5;

    do
    {
        PRTDIR pDirDevices = NULL;
        int rc = RTDirOpen(&pDirDevices, SYSFS_CPU_PATH);
        if (RT_SUCCESS(rc))
        {
            RTDIRENTRY DirFolderContent;
            while (RT_SUCCESS(RTDirRead(pDirDevices, &DirFolderContent, NULL))) /* Assumption that szName has always enough space */
            {
                /** @todo r-bird: This code is bringing all CPUs online; the idCpuCore and
                 *        idCpuPackage parameters are unused!
                 *        aeichner: These files are not available at this point unfortunately. (see comment above)
                 *        bird: Yes, but isn't that easily dealt with by doing:
                 *              if (matching_topology() || !have_topology_directory())
                 *                  bring_cpu_online()
                 *              That could save you the cpu0 and cpuidle checks to.
                 */
                /*
                 * Check if this is a CPU object.
                 * cpu0 is excluded because it is not possible to change the state
                 * of the first CPU on Linux (it doesn't even have an online file)
                 * and cpuidle is no CPU device. Prevents error messages later.
                 */
                if(   !strncmp(DirFolderContent.szName, "cpu", 3)
                    && strncmp(DirFolderContent.szName, "cpu0", 4)
                    && strncmp(DirFolderContent.szName, "cpuidle", 7))
                {
                    /* Get the sysdev */
                    RTFILE hFileCpuOnline = NIL_RTFILE;

                    rc = RTFileOpenF(&hFileCpuOnline, RTFILE_O_WRITE | RTFILE_O_OPEN | RTFILE_O_DENY_NONE,
                                     "%s/%s/online", SYSFS_CPU_PATH, DirFolderContent.szName);
                    if (RT_SUCCESS(rc))
                    {
                        /* Write a 1 to online the CPU */
                        rc = RTFileWrite(hFileCpuOnline, "1", 1, NULL);
                        RTFileClose(hFileCpuOnline);
                        if (RT_SUCCESS(rc))
                        {
                            VBoxServiceVerbose(1, "CpuHotPlug: CPU %u/%u was brought online\n", idCpuPackage, idCpuCore);
                            fCpuOnline = true;
                            break;
                        }
                        /* Error means CPU not present or online already  */
                    }
                    else
                        VBoxServiceError("CpuHotPlug: Failed to open \"%s/%s/online\" rc=%Rrc\n",
                                         SYSFS_CPU_PATH, DirFolderContent.szName, rc);
                }
            }
        }
        else
            VBoxServiceError("CpuHotPlug: Failed to open path %s rc=%Rrc\n", SYSFS_CPU_PATH, rc);

        /* Sleep a bit */
        if (!fCpuOnline)
            RTThreadSleep(10);

    } while (   !fCpuOnline
             && cTries-- > 0);
#else
# error "Port me"
#endif
}


/**
 * Handles VMMDevCpuEventType_Unplug.
 *
 * @param   idCpuCore       The CPU core ID.
 * @param   idCpuPackage    The CPU package ID.
 */
static void VBoxServiceCpuHotPlugHandleUnplugEvent(uint32_t idCpuCore, uint32_t idCpuPackage)
{
#ifdef RT_OS_LINUX
    char *pszCpuDevicePath = NULL;
    int rc = VBoxServiceCpuHotPlugGetACPIDevicePath(&pszCpuDevicePath, idCpuCore, idCpuPackage);
    if (RT_SUCCESS(rc))
    {
        RTFILE hFileCpuEject;
        rc = RTFileOpenF(&hFileCpuEject, RTFILE_O_WRITE | RTFILE_O_OPEN | RTFILE_O_DENY_NONE,
                         "%s/eject", pszCpuDevicePath);
        if (RT_SUCCESS(rc))
        {
            /* Write a 1 to eject the CPU */
            rc = RTFileWrite(hFileCpuEject, "1", 1, NULL);
            if (RT_SUCCESS(rc))
                VBoxServiceVerbose(1, "CpuHotPlug: CPU %u/%u was ejected\n", idCpuPackage, idCpuCore);
            else
                VBoxServiceError("CpuHotPlug: Failed to eject CPU %u/%u rc=%Rrc\n", idCpuPackage, idCpuCore, rc);

            RTFileClose(hFileCpuEject);
        }
        else
            VBoxServiceError("CpuHotPlug: Failed to open \"%s/eject\" rc=%Rrc\n", pszCpuDevicePath, rc);
        RTStrFree(pszCpuDevicePath);
    }
    else
        VBoxServiceError("CpuHotPlug: Failed to get CPU device path rc=%Rrc\n", rc);
#else
# error "Port me"
#endif
}


/** @copydoc VBOXSERVICE::pfnWorker */
DECLCALLBACK(int) VBoxServiceCpuHotPlugWorker(bool volatile *pfShutdown)
{
    /*
     * Tell the control thread that it can continue spawning services.
     */
    RTThreadUserSignal(RTThreadSelf());

    /*
     * Enable the CPU hotplug notifier.
     */
    int rc = VbglR3CpuHotPlugInit();
    if (RT_FAILURE(rc))
        return rc;

    /*
     * The Work Loop.
     */
    for (;;)
    {
        /* Wait for CPU hot plugging event. */
        uint32_t            idCpuCore;
        uint32_t            idCpuPackage;
        VMMDevCpuEventType  enmEventType;
        rc = VbglR3CpuHotPlugWaitForEvent(&enmEventType, &idCpuCore, &idCpuPackage);
        if (RT_SUCCESS(rc))
        {
            VBoxServiceVerbose(3, "CpuHotPlug: Event happened idCpuCore=%u idCpuPackage=%u enmEventType=%d\n",
                               idCpuCore, idCpuPackage, enmEventType);
            switch (enmEventType)
            {
                case VMMDevCpuEventType_Plug:
                    VBoxServiceCpuHotPlugHandlePlugEvent(idCpuCore, idCpuPackage);
                    break;

                case VMMDevCpuEventType_Unplug:
                    VBoxServiceCpuHotPlugHandleUnplugEvent(idCpuCore, idCpuPackage);
                    break;

                default:
                {
                    static uint32_t s_iErrors = 0;
                    if (s_iErrors++ < 10)
                        VBoxServiceError("CpuHotPlug: Unknown event: idCpuCore=%u idCpuPackage=%u enmEventType=%d\n",
                                         idCpuCore, idCpuPackage, enmEventType);
                    break;
                }
            }
        }
        else if (rc != VERR_INTERRUPTED && rc != VERR_TRY_AGAIN)
        {
            VBoxServiceError("CpuHotPlug: VbglR3CpuHotPlugWaitForEvent returned %Rrc\n", rc);
            break;
        }

        if (*pfShutdown)
            break;
    }

    VbglR3CpuHotPlugTerm();
    return rc;
}


/** @copydoc VBOXSERVICE::pfnStop */
static DECLCALLBACK(void) VBoxServiceCpuHotPlugStop(void)
{
    VbglR3InterruptEventWaits();
    return;
}


/**
 * The 'CpuHotPlug' service description.
 */
VBOXSERVICE g_CpuHotPlug =
{
    /* pszName. */
    "cpuhotplug",
    /* pszDescription. */
    "CPU hot plugging monitor",
    /* pszUsage. */
    NULL,
    /* pszOptions. */
    NULL,
    /* methods */
    VBoxServiceDefaultPreInit,
    VBoxServiceDefaultOption,
    VBoxServiceDefaultInit,
    VBoxServiceCpuHotPlugWorker,
    VBoxServiceCpuHotPlugStop,
    VBoxServiceDefaultTerm
};

