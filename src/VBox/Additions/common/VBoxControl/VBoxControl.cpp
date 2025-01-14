/* $Id$ */
/** @file
 * VBoxControl - Guest Additions Command Line Management Interface.
 */

/*
 * Copyright (C) 2008-2015 Oracle Corporation
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
#include <iprt/alloca.h>
#include <iprt/cpp/autores.h>
#include <iprt/buildconfig.h>
#include <iprt/getopt.h>
#include <iprt/initterm.h>
#include <iprt/mem.h>
#include <iprt/message.h>
#include <iprt/path.h>
#include <iprt/string.h>
#include <iprt/stream.h>
#include <VBox/log.h>
#include <VBox/version.h>
#include <VBox/VBoxGuestLib.h>
#ifdef RT_OS_WINDOWS
# include <Windows.h>
#endif
#ifdef VBOX_WITH_GUEST_PROPS
# include <VBox/HostServices/GuestPropertySvc.h>
#endif
#ifdef VBOX_WITH_DPC_LATENCY_CHECKER
# include <VBox/VBoxGuest.h>
# include "../VBoxGuestLib/VBGLR3Internal.h" /* HACK ALERT! Using vbglR3DoIOCtl directly!! */
#endif


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/** The program name (derived from argv[0]). */
char const *g_pszProgName = "";
/** The current verbosity level. */
int g_cVerbosity = 0;


/**
 * Displays the program usage message.
 *
 * @param u64Which
 *
 * @{
 */

/** Helper function */
static void doUsage(char const *line, char const *name = "", char const *command = "")
{
    /* Allow for up to 15 characters command name length (VBoxControl.exe) with
     * perfect column alignment. Beyond that there's at least one space between
     * the command if there are command line parameters. */
    RTPrintf("%s %-*s%s%s\n", name, strlen(line) ? 35 - strlen(name) : 1,
                              command, strlen(line) ? " " : "", line);
}

/** Enumerate the different parts of the usage we might want to print out */
enum VBoxControlUsage
{
#ifdef RT_OS_WINDOWS
    GET_VIDEO_ACCEL,
    SET_VIDEO_ACCEL,
    VIDEO_FLAGS,
    LIST_CUST_MODES,
    ADD_CUST_MODE,
    REMOVE_CUST_MODE,
    SET_VIDEO_MODE,
#endif
#ifdef VBOX_WITH_GUEST_PROPS
    GUEST_PROP,
#endif
#ifdef VBOX_WITH_SHARED_FOLDERS
    GUEST_SHAREDFOLDERS,
#endif
#if !defined(VBOX_CONTROL_TEST)
    WRITE_CORE_DUMP,
#endif
    WRITE_LOG,
    TAKE_SNAPSHOT,
    SAVE_STATE,
    SUSPEND,
    POWER_OFF,
    VERSION,
    HELP,
    USAGE_ALL = UINT32_MAX
};

static RTEXITCODE usage(enum VBoxControlUsage eWhich = USAGE_ALL)
{
    RTPrintf("Usage:\n\n");
    doUsage("print version number and exit", g_pszProgName, "[-V|--version]");
    doUsage("suppress the logo", g_pszProgName, "--nologo ...");
    RTPrintf("\n");

    /* Exclude the Windows bits from the test version.  Anyone who needs to
       test them can fix this. */
#if defined(RT_OS_WINDOWS) && !defined(VBOX_CONTROL_TEST)
    if (eWhich  == GET_VIDEO_ACCEL || eWhich == USAGE_ALL)
        doUsage("", g_pszProgName, "getvideoacceleration");
    if (eWhich  == SET_VIDEO_ACCEL || eWhich == USAGE_ALL)
        doUsage("<on|off>", g_pszProgName, "setvideoacceleration");
    if (eWhich  == VIDEO_FLAGS || eWhich == USAGE_ALL)
        doUsage("<get|set|clear|delete> [hex mask]", g_pszProgName, "videoflags");
    if (eWhich  == LIST_CUST_MODES || eWhich == USAGE_ALL)
        doUsage("", g_pszProgName, "listcustommodes");
    if (eWhich  == ADD_CUST_MODE || eWhich == USAGE_ALL)
        doUsage("<width> <height> <bpp>", g_pszProgName, "addcustommode");
    if (eWhich  == REMOVE_CUST_MODE || eWhich == USAGE_ALL)
        doUsage("<width> <height> <bpp>", g_pszProgName, "removecustommode");
    if (eWhich  == SET_VIDEO_MODE || eWhich == USAGE_ALL)
        doUsage("<width> <height> <bpp> <screen>", g_pszProgName, "setvideomode");
#endif
#ifdef VBOX_WITH_GUEST_PROPS
    if (eWhich == GUEST_PROP || eWhich == USAGE_ALL)
    {
        doUsage("get <property> [--verbose]", g_pszProgName, "guestproperty");
        doUsage("set <property> [<value> [--flags <flags>]]", g_pszProgName, "guestproperty");
        doUsage("delete|unset <property>", g_pszProgName, "guestproperty");
        doUsage("enumerate [--patterns <patterns>]", g_pszProgName, "guestproperty");
        doUsage("wait <patterns>", g_pszProgName, "guestproperty");
        doUsage("[--timestamp <last timestamp>]");
        doUsage("[--timeout <timeout in ms>");
    }
#endif
#ifdef VBOX_WITH_SHARED_FOLDERS
    if (eWhich  == GUEST_SHAREDFOLDERS || eWhich == USAGE_ALL)
    {
        doUsage("list [-automount]", g_pszProgName, "sharedfolder");
    }
#endif

#if !defined(VBOX_CONTROL_TEST)
    if (eWhich == WRITE_CORE_DUMP || eWhich == USAGE_ALL)
        doUsage("", g_pszProgName, "writecoredump");
#endif
    if (eWhich == WRITE_LOG || eWhich == USAGE_ALL)
        doUsage("", g_pszProgName, "writelog [-n|--no-newline] [--] <msg>");
    if (eWhich == TAKE_SNAPSHOT || eWhich == USAGE_ALL)
        doUsage("", g_pszProgName, "takesnapshot");
    if (eWhich == SAVE_STATE || eWhich == USAGE_ALL)
        doUsage("", g_pszProgName, "savestate");
    if (eWhich == SUSPEND   || eWhich == USAGE_ALL)
        doUsage("", g_pszProgName, "suspend");
    if (eWhich == POWER_OFF  || eWhich == USAGE_ALL)
        doUsage("", g_pszProgName, "poweroff");
    if (eWhich == HELP      || eWhich == USAGE_ALL)
        doUsage("[command]", g_pszProgName, "help");
    if (eWhich == VERSION   || eWhich == USAGE_ALL)
        doUsage("", g_pszProgName, "version");

    return RTEXITCODE_SUCCESS;
}

/** @} */


/**
 * Implementation of the '--version' option.
 *
 * @returns RTEXITCODE_SUCCESS
 */
static RTEXITCODE printVersion(void)
{
    RTPrintf("%sr%u\n", VBOX_VERSION_STRING, RTBldCfgRevision());
    return RTEXITCODE_SUCCESS;
}


/**
 * Displays an error message.
 *
 * @returns RTEXITCODE_FAILURE.
 * @param   pszFormat   The message text. No newline.
 * @param   ...         Format arguments.
 */
static RTEXITCODE VBoxControlError(const char *pszFormat, ...)
{
    /** @todo prefix with current command. */
    va_list va;
    va_start(va, pszFormat);
    RTMsgErrorV(pszFormat, va);
    va_end(va);
    return RTEXITCODE_FAILURE;
}


/**
 * Displays a getopt error.
 *
 * @returns RTEXITCODE_FAILURE.
 * @param   ch          The RTGetOpt return value.
 * @param   pValueUnion The RTGetOpt return data.
 */
static RTEXITCODE VBoxCtrlGetOptError(int ch, PCRTGETOPTUNION pValueUnion)
{
    /** @todo prefix with current command. */
    return RTGetOptPrintError(ch, pValueUnion);
}


/**
 * Displays an syntax error message.
 *
 * @returns RTEXITCODE_FAILURE.
 * @param   pszFormat   The message text. No newline.
 * @param   ...         Format arguments.
 */
static RTEXITCODE VBoxControlSyntaxError(const char *pszFormat, ...)
{
    /** @todo prefix with current command. */
    va_list va;
    va_start(va, pszFormat);
    RTMsgErrorV(pszFormat, va);
    va_end(va);
    return RTEXITCODE_FAILURE;
}

#if defined(RT_OS_WINDOWS) && !defined(VBOX_CONTROL_TEST)

LONG (WINAPI * gpfnChangeDisplaySettingsEx)(LPCTSTR lpszDeviceName, LPDEVMODE lpDevMode, HWND hwnd, DWORD dwflags, LPVOID lParam);

static unsigned nextAdjacentRectXP (RECTL *paRects, unsigned nRects, unsigned iRect)
{
    unsigned i;
    for (i = 0; i < nRects; i++)
    {
        if (paRects[iRect].right == paRects[i].left)
        {
            return i;
        }
    }
    return ~0;
}

static unsigned nextAdjacentRectXN (RECTL *paRects, unsigned nRects, unsigned iRect)
{
    unsigned i;
    for (i = 0; i < nRects; i++)
    {
        if (paRects[iRect].left == paRects[i].right)
        {
            return i;
        }
    }
    return ~0;
}

static unsigned nextAdjacentRectYP (RECTL *paRects, unsigned nRects, unsigned iRect)
{
    unsigned i;
    for (i = 0; i < nRects; i++)
    {
        if (paRects[iRect].bottom == paRects[i].top)
        {
            return i;
        }
    }
    return ~0;
}

unsigned nextAdjacentRectYN (RECTL *paRects, unsigned nRects, unsigned iRect)
{
    unsigned i;
    for (i = 0; i < nRects; i++)
    {
        if (paRects[iRect].top == paRects[i].bottom)
        {
            return i;
        }
    }
    return ~0;
}

void resizeRect(RECTL *paRects, unsigned nRects, unsigned iPrimary, unsigned iResized, int NewWidth, int NewHeight)
{
    RECTL *paNewRects = (RECTL *)alloca (sizeof (RECTL) * nRects);
    memcpy (paNewRects, paRects, sizeof (RECTL) * nRects);
    paNewRects[iResized].right += NewWidth - (paNewRects[iResized].right - paNewRects[iResized].left);
    paNewRects[iResized].bottom += NewHeight - (paNewRects[iResized].bottom - paNewRects[iResized].top);

    /* Verify all pairs of originally adjacent rectangles for all 4 directions.
     * If the pair has a "good" delta (that is the first rectangle intersects the second)
     * at a direction and the second rectangle is not primary one (which can not be moved),
     * move the second rectangle to make it adjacent to the first one.
     */

    /* X positive. */
    unsigned iRect;
    for (iRect = 0; iRect < nRects; iRect++)
    {
        /* Find the next adjacent original rect in x positive direction. */
        unsigned iNextRect = nextAdjacentRectXP (paRects, nRects, iRect);
        Log(("next %d -> %d\n", iRect, iNextRect));

        if (iNextRect == ~0 || iNextRect == iPrimary)
        {
            continue;
        }

        /* Check whether there is an X intersection between these adjacent rects in the new rectangles
         * and fix the intersection if delta is "good".
         */
        int delta = paNewRects[iRect].right - paNewRects[iNextRect].left;

        if (delta > 0)
        {
            Log(("XP intersection right %d left %d, diff %d\n",
                     paNewRects[iRect].right, paNewRects[iNextRect].left,
                     delta));

            paNewRects[iNextRect].left += delta;
            paNewRects[iNextRect].right += delta;
        }
    }

    /* X negative. */
    for (iRect = 0; iRect < nRects; iRect++)
    {
        /* Find the next adjacent original rect in x negative direction. */
        unsigned iNextRect = nextAdjacentRectXN (paRects, nRects, iRect);
        Log(("next %d -> %d\n", iRect, iNextRect));

        if (iNextRect == ~0 || iNextRect == iPrimary)
        {
            continue;
        }

        /* Check whether there is an X intersection between these adjacent rects in the new rectangles
         * and fix the intersection if delta is "good".
         */
        int delta = paNewRects[iRect].left - paNewRects[iNextRect].right;

        if (delta < 0)
        {
            Log(("XN intersection left %d right %d, diff %d\n",
                     paNewRects[iRect].left, paNewRects[iNextRect].right,
                     delta));

            paNewRects[iNextRect].left += delta;
            paNewRects[iNextRect].right += delta;
        }
    }

    /* Y positive (in the computer sense, top->down). */
    for (iRect = 0; iRect < nRects; iRect++)
    {
        /* Find the next adjacent original rect in y positive direction. */
        unsigned iNextRect = nextAdjacentRectYP (paRects, nRects, iRect);
        Log(("next %d -> %d\n", iRect, iNextRect));

        if (iNextRect == ~0 || iNextRect == iPrimary)
        {
            continue;
        }

        /* Check whether there is an Y intersection between these adjacent rects in the new rectangles
         * and fix the intersection if delta is "good".
         */
        int delta = paNewRects[iRect].bottom - paNewRects[iNextRect].top;

        if (delta > 0)
        {
            Log(("YP intersection bottom %d top %d, diff %d\n",
                     paNewRects[iRect].bottom, paNewRects[iNextRect].top,
                     delta));

            paNewRects[iNextRect].top += delta;
            paNewRects[iNextRect].bottom += delta;
        }
    }

    /* Y negative (in the computer sense, down->top). */
    for (iRect = 0; iRect < nRects; iRect++)
    {
        /* Find the next adjacent original rect in x negative direction. */
        unsigned iNextRect = nextAdjacentRectYN (paRects, nRects, iRect);
        Log(("next %d -> %d\n", iRect, iNextRect));

        if (iNextRect == ~0 || iNextRect == iPrimary)
        {
            continue;
        }

        /* Check whether there is an Y intersection between these adjacent rects in the new rectangles
         * and fix the intersection if delta is "good".
         */
        int delta = paNewRects[iRect].top - paNewRects[iNextRect].bottom;

        if (delta < 0)
        {
            Log(("YN intersection top %d bottom %d, diff %d\n",
                     paNewRects[iRect].top, paNewRects[iNextRect].bottom,
                     delta));

            paNewRects[iNextRect].top += delta;
            paNewRects[iNextRect].bottom += delta;
        }
    }

    memcpy (paRects, paNewRects, sizeof (RECTL) * nRects);
    return;
}

/* Returns TRUE to try again. */
static BOOL ResizeDisplayDevice(ULONG Id, DWORD Width, DWORD Height, DWORD BitsPerPixel)
{
    BOOL fModeReset = (Width == 0 && Height == 0 && BitsPerPixel == 0);

    DISPLAY_DEVICE DisplayDevice;

    ZeroMemory(&DisplayDevice, sizeof(DisplayDevice));
    DisplayDevice.cb = sizeof(DisplayDevice);

    /* Find out how many display devices the system has */
    DWORD NumDevices = 0;
    DWORD i = 0;
    while (EnumDisplayDevices (NULL, i, &DisplayDevice, 0))
    {
        Log(("[%d] %s\n", i, DisplayDevice.DeviceName));

        if (DisplayDevice.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
        {
            Log(("Found primary device. err %d\n", GetLastError ()));
            NumDevices++;
        }
        else if (!(DisplayDevice.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER))
        {

            Log(("Found secondary device. err %d\n", GetLastError ()));
            NumDevices++;
        }

        ZeroMemory(&DisplayDevice, sizeof(DisplayDevice));
        DisplayDevice.cb = sizeof(DisplayDevice);
        i++;
    }

    Log(("Found total %d devices. err %d\n", NumDevices, GetLastError ()));

    if (NumDevices == 0 || Id >= NumDevices)
    {
        Log(("Requested identifier %d is invalid. err %d\n", Id, GetLastError ()));
        return FALSE;
    }

    DISPLAY_DEVICE *paDisplayDevices = (DISPLAY_DEVICE *)alloca (sizeof (DISPLAY_DEVICE) * NumDevices);
    DEVMODE *paDeviceModes = (DEVMODE *)alloca (sizeof (DEVMODE) * NumDevices);
    RECTL *paRects = (RECTL *)alloca (sizeof (RECTL) * NumDevices);

    /* Fetch information about current devices and modes. */
    DWORD DevNum = 0;
    DWORD DevPrimaryNum = 0;

    ZeroMemory(&DisplayDevice, sizeof(DISPLAY_DEVICE));
    DisplayDevice.cb = sizeof(DISPLAY_DEVICE);

    i = 0;
    while (EnumDisplayDevices (NULL, i, &DisplayDevice, 0))
    {
        Log(("[%d(%d)] %s\n", i, DevNum, DisplayDevice.DeviceName));

        BOOL bFetchDevice = FALSE;

        if (DisplayDevice.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)
        {
            Log(("Found primary device. err %d\n", GetLastError ()));
            DevPrimaryNum = DevNum;
            bFetchDevice = TRUE;
        }
        else if (!(DisplayDevice.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER))
        {

            Log(("Found secondary device. err %d\n", GetLastError ()));
            bFetchDevice = TRUE;
        }

        if (bFetchDevice)
        {
            if (DevNum >= NumDevices)
            {
                Log(("%d >= %d\n", NumDevices, DevNum));
                return FALSE;
            }

            paDisplayDevices[DevNum] = DisplayDevice;

            ZeroMemory(&paDeviceModes[DevNum], sizeof(DEVMODE));
            paDeviceModes[DevNum].dmSize = sizeof(DEVMODE);
            if (!EnumDisplaySettings((LPSTR)DisplayDevice.DeviceName,
                 ENUM_REGISTRY_SETTINGS, &paDeviceModes[DevNum]))
            {
                Log(("EnumDisplaySettings err %d\n", GetLastError ()));
                return FALSE;
            }

            Log(("%dx%d at %d,%d\n",
                    paDeviceModes[DevNum].dmPelsWidth,
                    paDeviceModes[DevNum].dmPelsHeight,
                    paDeviceModes[DevNum].dmPosition.x,
                    paDeviceModes[DevNum].dmPosition.y));

            paRects[DevNum].left   = paDeviceModes[DevNum].dmPosition.x;
            paRects[DevNum].top    = paDeviceModes[DevNum].dmPosition.y;
            paRects[DevNum].right  = paDeviceModes[DevNum].dmPosition.x + paDeviceModes[DevNum].dmPelsWidth;
            paRects[DevNum].bottom = paDeviceModes[DevNum].dmPosition.y + paDeviceModes[DevNum].dmPelsHeight;
            DevNum++;
        }

        ZeroMemory(&DisplayDevice, sizeof(DISPLAY_DEVICE));
        DisplayDevice.cb = sizeof(DISPLAY_DEVICE);
        i++;
    }

    if (Width == 0)
    {
        Width = paRects[Id].right - paRects[Id].left;
    }

    if (Height == 0)
    {
        Height = paRects[Id].bottom - paRects[Id].top;
    }

    /* Check whether a mode reset or a change is requested. */
    if (   !fModeReset
        && paRects[Id].right - paRects[Id].left == Width
        && paRects[Id].bottom - paRects[Id].top == Height
        && paDeviceModes[Id].dmBitsPerPel == BitsPerPixel)
    {
        Log(("VBoxDisplayThread : already at desired resolution.\n"));
        return FALSE;
    }

    resizeRect(paRects, NumDevices, DevPrimaryNum, Id, Width, Height);
#ifdef Log
    for (i = 0; i < NumDevices; i++)
    {
        Log(("[%d]: %d,%d %dx%d\n",
                i, paRects[i].left, paRects[i].top,
                paRects[i].right - paRects[i].left,
                paRects[i].bottom - paRects[i].top));
    }
#endif /* Log */

    /* Without this, Windows will not ask the miniport for its
     * mode table but uses an internal cache instead.
     */
    DEVMODE tempDevMode;
    ZeroMemory (&tempDevMode, sizeof (tempDevMode));
    tempDevMode.dmSize = sizeof(DEVMODE);
    EnumDisplaySettings(NULL, 0xffffff, &tempDevMode);

    /* Assign the new rectangles to displays. */
    for (i = 0; i < NumDevices; i++)
    {
        paDeviceModes[i].dmPosition.x = paRects[i].left;
        paDeviceModes[i].dmPosition.y = paRects[i].top;
        paDeviceModes[i].dmPelsWidth  = paRects[i].right - paRects[i].left;
        paDeviceModes[i].dmPelsHeight = paRects[i].bottom - paRects[i].top;

        paDeviceModes[i].dmFields = DM_POSITION | DM_PELSHEIGHT | DM_PELSWIDTH;

        if (   i == Id
            && BitsPerPixel != 0)
        {
            paDeviceModes[i].dmFields |= DM_BITSPERPEL;
            paDeviceModes[i].dmBitsPerPel = BitsPerPixel;
        }
        Log(("calling pfnChangeDisplaySettingsEx %x\n", gpfnChangeDisplaySettingsEx));
        gpfnChangeDisplaySettingsEx((LPSTR)paDisplayDevices[i].DeviceName,
                 &paDeviceModes[i], NULL, CDS_NORESET | CDS_UPDATEREGISTRY, NULL);
        Log(("ChangeDisplaySettings position err %d\n", GetLastError ()));
    }

    /* A second call to ChangeDisplaySettings updates the monitor. */
    LONG status = ChangeDisplaySettings(NULL, 0);
    Log(("ChangeDisplaySettings update status %d\n", status));
    if (status == DISP_CHANGE_SUCCESSFUL || status == DISP_CHANGE_BADMODE)
    {
        /* Successfully set new video mode or our driver can not set the requested mode. Stop trying. */
        return FALSE;
    }

    /* Retry the request. */
    return TRUE;
}

static DECLCALLBACK(RTEXITCODE) handleSetVideoMode(int argc, char *argv[])
{
    if (argc != 3 && argc != 4)
    {
        usage(SET_VIDEO_MODE);
        return RTEXITCODE_FAILURE;
    }

    DWORD xres = atoi(argv[0]);
    DWORD yres = atoi(argv[1]);
    DWORD bpp  = atoi(argv[2]);
    DWORD scr  = 0;

    if (argc == 4)
    {
        scr = atoi(argv[3]);
    }

    HMODULE hUser = GetModuleHandle("user32.dll");

    if (hUser)
    {
        *(uintptr_t *)&gpfnChangeDisplaySettingsEx = (uintptr_t)GetProcAddress(hUser, "ChangeDisplaySettingsExA");
        Log(("VBoxService: pChangeDisplaySettingsEx = %p\n", gpfnChangeDisplaySettingsEx));

        if (gpfnChangeDisplaySettingsEx)
        {
            /* The screen index is 0 based in the ResizeDisplayDevice call. */
            scr = scr > 0? scr - 1: 0;

            /* Horizontal resolution must be a multiple of 8, round down. */
            xres &= ~0x7;

            RTPrintf("Setting resolution of display %d to %dx%dx%d ...", scr, xres, yres, bpp);
            ResizeDisplayDevice(scr, xres, yres, bpp);
            RTPrintf("done.\n");
        }
        else
            VBoxControlError("Error retrieving API for display change!");
    }
    else
        VBoxControlError("Error retrieving handle to user32.dll!");

    return RTEXITCODE_SUCCESS;
}

static int checkVBoxVideoKey(HKEY hkeyVideo)
{
    char szValue[128];
    DWORD len = sizeof(szValue);
    DWORD dwKeyType;
    LONG status = RegQueryValueExA(hkeyVideo, "Device Description", NULL, &dwKeyType,
                                   (LPBYTE)szValue, &len);

    if (status == ERROR_SUCCESS)
    {
        /* WDDM has additional chars after "Adapter" */
        static char sszDeviceDescription[] = "VirtualBox Graphics Adapter";
        if (_strnicmp(szValue, sszDeviceDescription, sizeof(sszDeviceDescription) - sizeof(char)) == 0)
        {
            return VINF_SUCCESS;
        }
    }

    return VERR_NOT_FOUND;
}

static HKEY getVideoKey(bool writable)
{
    HKEY hkeyDeviceMap = 0;
    LONG status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\VIDEO", 0, KEY_READ, &hkeyDeviceMap);
    if (status != ERROR_SUCCESS || !hkeyDeviceMap)
    {
        VBoxControlError("Error opening video device map registry key!\n");
        return 0;
    }

    HKEY hkeyVideo = 0;
    ULONG iDevice;
    DWORD dwKeyType;

    /*
     * Scan all '\Device\VideoX' REG_SZ keys to find VBox video driver entry.
     * 'ObjectNumberList' REG_BINARY is an array of 32 bit device indexes (X).
     */

    /* Get the 'ObjectNumberList' */
    ULONG numDevices = 0;
    DWORD adwObjectNumberList[256];
    DWORD len = sizeof(adwObjectNumberList);
    status = RegQueryValueExA(hkeyDeviceMap, "ObjectNumberList", NULL, &dwKeyType, (LPBYTE)&adwObjectNumberList[0], &len);

    if (   status == ERROR_SUCCESS
        && dwKeyType == REG_BINARY)
    {
        numDevices = len / sizeof(DWORD);
    }
    else
    {
       /* The list might not exists. Use 'MaxObjectNumber' REG_DWORD and build a list. */
       DWORD dwMaxObjectNumber = 0;
       len = sizeof(dwMaxObjectNumber);
       status = RegQueryValueExA(hkeyDeviceMap, "MaxObjectNumber", NULL, &dwKeyType, (LPBYTE)&dwMaxObjectNumber, &len);

       if (   status == ERROR_SUCCESS
           && dwKeyType == REG_DWORD)
       {
           /* 'MaxObjectNumber' is inclusive. */
           numDevices = RT_MIN(dwMaxObjectNumber + 1, RT_ELEMENTS(adwObjectNumberList));
           for (iDevice = 0; iDevice < numDevices; iDevice++)
           {
               adwObjectNumberList[iDevice] = iDevice;
           }
       }
    }

    if (numDevices == 0)
    {
        /* Always try '\Device\Video0' as the old code did. Enum can be used in this case in principle. */
        adwObjectNumberList[0] = 0;
        numDevices = 1;
    }

    /* Scan device entries */
    for (iDevice = 0; iDevice < numDevices; iDevice++)
    {
        char szValueName[64];
        RTStrPrintf(szValueName, sizeof(szValueName), "\\Device\\Video%u", adwObjectNumberList[iDevice]);

        char szVideoLocation[256];
        len = sizeof(szVideoLocation);
        status = RegQueryValueExA(hkeyDeviceMap, szValueName, NULL, &dwKeyType, (LPBYTE)&szVideoLocation[0], &len);

        /* This value starts with '\REGISTRY\Machine' */
        if (   status == ERROR_SUCCESS
            && dwKeyType == REG_SZ
            && _strnicmp(szVideoLocation, "\\REGISTRY\\Machine", 17) == 0)
        {
            status = RegOpenKeyExA(HKEY_LOCAL_MACHINE, &szVideoLocation[18], 0,
                                   KEY_READ | (writable ? KEY_WRITE : 0), &hkeyVideo);
            if (status == ERROR_SUCCESS)
            {
                int rc = checkVBoxVideoKey(hkeyVideo);
                if (RT_SUCCESS(rc))
                {
                    /* Found, return hkeyVideo to the caller. */
                    break;
                }

                RegCloseKey(hkeyVideo);
                hkeyVideo = 0;
            }
        }
    }

    if (hkeyVideo == 0)
    {
        VBoxControlError("Error opening video registry key!\n");
    }

    RegCloseKey(hkeyDeviceMap);
    return hkeyVideo;
}

static DECLCALLBACK(RTEXITCODE) handleGetVideoAcceleration(int argc, char *argv[])
{
    ULONG status;
    HKEY hkeyVideo = getVideoKey(false);

    if (hkeyVideo)
    {
        /* query the actual value */
        DWORD fAcceleration = 1;
        DWORD len = sizeof(fAcceleration);
        DWORD dwKeyType;
        status = RegQueryValueExA(hkeyVideo, "EnableVideoAccel", NULL, &dwKeyType, (LPBYTE)&fAcceleration, &len);
        if (status != ERROR_SUCCESS)
            RTPrintf("Video acceleration: default\n");
        else
            RTPrintf("Video acceleration: %s\n", fAcceleration ? "on" : "off");
        RegCloseKey(hkeyVideo);
    }
    return RTEXITCODE_SUCCESS;
}

static DECLCALLBACK(RTEXITCODE) handleSetVideoAcceleration(int argc, char *argv[])
{
    ULONG status;
    HKEY hkeyVideo;

    /* must have exactly one argument: the new offset */
    if (   (argc != 1)
        || (   RTStrICmp(argv[0], "on")
            && RTStrICmp(argv[0], "off")))
    {
        usage(SET_VIDEO_ACCEL);
        return RTEXITCODE_FAILURE;
    }

    hkeyVideo = getVideoKey(true);

    if (hkeyVideo)
    {
        int fAccel = 0;
        if (RTStrICmp(argv[0], "on") == 0)
            fAccel = 1;
        /* set a new value */
        status = RegSetValueExA(hkeyVideo, "EnableVideoAccel", 0, REG_DWORD, (LPBYTE)&fAccel, sizeof(fAccel));
        if (status != ERROR_SUCCESS)
        {
            VBoxControlError("Error %d writing video acceleration status!\n", status);
        }
        RegCloseKey(hkeyVideo);
    }
    return RTEXITCODE_SUCCESS;
}

static DECLCALLBACK(RTEXITCODE) videoFlagsGet(void)
{
    HKEY hkeyVideo = getVideoKey(false);

    if (hkeyVideo)
    {
        DWORD dwFlags = 0;
        DWORD len = sizeof(dwFlags);
        DWORD dwKeyType;
        ULONG status = RegQueryValueExA(hkeyVideo, "VBoxVideoFlags", NULL, &dwKeyType, (LPBYTE)&dwFlags, &len);
        if (status != ERROR_SUCCESS)
            RTPrintf("Video flags: default\n");
        else
            RTPrintf("Video flags: 0x%08X\n", dwFlags);
        RegCloseKey(hkeyVideo);
        return RTEXITCODE_SUCCESS;
    }

    return RTEXITCODE_FAILURE;
}

static DECLCALLBACK(RTEXITCODE) videoFlagsDelete(void)
{
    HKEY hkeyVideo = getVideoKey(true);

    if (hkeyVideo)
    {
        ULONG status = RegDeleteValueA(hkeyVideo, "VBoxVideoFlags");
        if (status != ERROR_SUCCESS)
            VBoxControlError("Error %d deleting video flags.\n", status);
        RegCloseKey(hkeyVideo);
        return RTEXITCODE_SUCCESS;
    }

    return RTEXITCODE_FAILURE;
}

static DECLCALLBACK(RTEXITCODE) videoFlagsModify(bool fSet, int argc, char *argv[])
{
    if (argc != 1)
    {
        VBoxControlError("Mask required.\n");
        return RTEXITCODE_FAILURE;
    }

    uint32_t u32Mask = 0;
    int rc = RTStrToUInt32Full(argv[0], 16, &u32Mask);
    if (RT_FAILURE(rc))
    {
        VBoxControlError("Invalid video flags mask.\n");
        return RTEXITCODE_FAILURE;
    }

    RTEXITCODE exitCode = RTEXITCODE_SUCCESS;

    HKEY hkeyVideo = getVideoKey(true);
    if (hkeyVideo)
    {
        DWORD dwFlags = 0;
        DWORD len = sizeof(dwFlags);
        DWORD dwKeyType;
        ULONG status = RegQueryValueExA(hkeyVideo, "VBoxVideoFlags", NULL, &dwKeyType, (LPBYTE)&dwFlags, &len);
        if (status != ERROR_SUCCESS)
        {
            dwFlags = 0;
        }

        dwFlags = fSet? (dwFlags | u32Mask):
                        (dwFlags & ~u32Mask);

        status = RegSetValueExA(hkeyVideo, "VBoxVideoFlags", 0, REG_DWORD, (LPBYTE)&dwFlags, sizeof(dwFlags));
        if (status != ERROR_SUCCESS)
        {
            VBoxControlError("Error %d writing video flags.\n", status);
            exitCode = RTEXITCODE_FAILURE;
        }

        RegCloseKey(hkeyVideo);
    }
    else
    {
        exitCode = RTEXITCODE_FAILURE;
    }

    return exitCode;
}

static DECLCALLBACK(RTEXITCODE) handleVideoFlags(int argc, char *argv[])
{
    /* Must have a keyword and optional value (32 bit hex string). */
    if (argc != 1 && argc != 2)
    {
        VBoxControlError("Invalid number of arguments.\n");
        usage(VIDEO_FLAGS);
        return RTEXITCODE_FAILURE;
    }

    RTEXITCODE exitCode = RTEXITCODE_SUCCESS;

    if (RTStrICmp(argv[0], "get") == 0)
    {
        exitCode = videoFlagsGet();
    }
    else if (RTStrICmp(argv[0], "delete") == 0)
    {
        exitCode = videoFlagsDelete();
    }
    else if (RTStrICmp(argv[0], "set") == 0)
    {
        exitCode = videoFlagsModify(true, argc - 1, &argv[1]);
    }
    else if (RTStrICmp(argv[0], "clear") == 0)
    {
        exitCode = videoFlagsModify(false, argc - 1, &argv[1]);
    }
    else
    {
        VBoxControlError("Invalid command.\n");
        exitCode = RTEXITCODE_FAILURE;
    }

    if (exitCode != RTEXITCODE_SUCCESS)
    {
        usage(VIDEO_FLAGS);
    }

    return exitCode;
}

#define MAX_CUSTOM_MODES 128

/* the table of custom modes */
struct
{
    DWORD xres;
    DWORD yres;
    DWORD bpp;
} customModes[MAX_CUSTOM_MODES] = {0};

void getCustomModes(HKEY hkeyVideo)
{
    ULONG status;
    int curMode = 0;

    /* null out the table */
    RT_ZERO(customModes);

    do
    {
        char valueName[20];
        DWORD xres, yres, bpp = 0;
        DWORD dwType;
        DWORD dwLen = sizeof(DWORD);

        RTStrPrintf(valueName, sizeof(valueName), "CustomMode%dWidth", curMode);
        status = RegQueryValueExA(hkeyVideo, valueName, NULL, &dwType, (LPBYTE)&xres, &dwLen);
        if (status != ERROR_SUCCESS)
            break;
        RTStrPrintf(valueName, sizeof(valueName), "CustomMode%dHeight", curMode);
        status = RegQueryValueExA(hkeyVideo, valueName, NULL, &dwType, (LPBYTE)&yres, &dwLen);
        if (status != ERROR_SUCCESS)
            break;
        RTStrPrintf(valueName, sizeof(valueName), "CustomMode%dBPP", curMode);
        status = RegQueryValueExA(hkeyVideo, valueName, NULL, &dwType, (LPBYTE)&bpp, &dwLen);
        if (status != ERROR_SUCCESS)
            break;

        /* check if the mode is OK */
        if (   (xres > (1 << 16))
            || (yres > (1 << 16))
            || (   (bpp != 16)
                && (bpp != 24)
                && (bpp != 32)))
            break;

        /* add mode to table */
        customModes[curMode].xres = xres;
        customModes[curMode].yres = yres;
        customModes[curMode].bpp  = bpp;

        ++curMode;

        if (curMode >= MAX_CUSTOM_MODES)
            break;
    } while(1);
}

void writeCustomModes(HKEY hkeyVideo)
{
    ULONG status;
    int tableIndex = 0;
    int modeIndex = 0;

    /* first remove all values */
    for (int i = 0; i < MAX_CUSTOM_MODES; i++)
    {
        char valueName[20];
        RTStrPrintf(valueName, sizeof(valueName), "CustomMode%dWidth", i);
        RegDeleteValueA(hkeyVideo, valueName);
        RTStrPrintf(valueName, sizeof(valueName), "CustomMode%dHeight", i);
        RegDeleteValueA(hkeyVideo, valueName);
        RTStrPrintf(valueName, sizeof(valueName), "CustomMode%dBPP", i);
        RegDeleteValueA(hkeyVideo, valueName);
    }

    do
    {
        if (tableIndex >= MAX_CUSTOM_MODES)
            break;

        /* is the table entry present? */
        if (   (!customModes[tableIndex].xres)
            || (!customModes[tableIndex].yres)
            || (!customModes[tableIndex].bpp))
        {
            tableIndex++;
            continue;
        }

        RTPrintf("writing mode %d (%dx%dx%d)\n", modeIndex, customModes[tableIndex].xres, customModes[tableIndex].yres, customModes[tableIndex].bpp);
        char valueName[20];
        RTStrPrintf(valueName, sizeof(valueName), "CustomMode%dWidth", modeIndex);
        status = RegSetValueExA(hkeyVideo, valueName, 0, REG_DWORD, (LPBYTE)&customModes[tableIndex].xres,
                                sizeof(customModes[tableIndex].xres));
        RTStrPrintf(valueName, sizeof(valueName), "CustomMode%dHeight", modeIndex);
        RegSetValueExA(hkeyVideo, valueName, 0, REG_DWORD, (LPBYTE)&customModes[tableIndex].yres,
                       sizeof(customModes[tableIndex].yres));
        RTStrPrintf(valueName, sizeof(valueName), "CustomMode%dBPP", modeIndex);
        RegSetValueExA(hkeyVideo, valueName, 0, REG_DWORD, (LPBYTE)&customModes[tableIndex].bpp,
                       sizeof(customModes[tableIndex].bpp));

        modeIndex++;
        tableIndex++;

    } while(1);

}

static DECLCALLBACK(RTEXITCODE) handleListCustomModes(int argc, char *argv[])
{
    if (argc != 0)
    {
        usage(LIST_CUST_MODES);
        return RTEXITCODE_FAILURE;
    }

    HKEY hkeyVideo = getVideoKey(false);

    if (hkeyVideo)
    {
        getCustomModes(hkeyVideo);
        for (int i = 0; i < (sizeof(customModes) / sizeof(customModes[0])); i++)
        {
            if (   !customModes[i].xres
                || !customModes[i].yres
                || !customModes[i].bpp)
                continue;

            RTPrintf("Mode: %d x %d x %d\n",
                             customModes[i].xres, customModes[i].yres, customModes[i].bpp);
        }
        RegCloseKey(hkeyVideo);
    }
    return RTEXITCODE_SUCCESS;
}

static DECLCALLBACK(RTEXITCODE) handleAddCustomMode(int argc, char *argv[])
{
    if (argc != 3)
    {
        usage(ADD_CUST_MODE);
        return RTEXITCODE_FAILURE;
    }

    DWORD xres = atoi(argv[0]);
    DWORD yres = atoi(argv[1]);
    DWORD bpp  = atoi(argv[2]);

    /** @todo better check including xres mod 8 = 0! */
    if (   (xres > (1 << 16))
        || (yres > (1 << 16))
        || (   (bpp != 16)
            && (bpp != 24)
            && (bpp != 32)))
    {
        VBoxControlError("invalid mode specified!\n");
        return RTEXITCODE_FAILURE;
    }

    HKEY hkeyVideo = getVideoKey(true);

    if (hkeyVideo)
    {
        int i;
        int fModeExists = 0;
        getCustomModes(hkeyVideo);
        for (i = 0; i < MAX_CUSTOM_MODES; i++)
        {
            /* mode exists? */
            if (   customModes[i].xres == xres
                && customModes[i].yres == yres
                && customModes[i].bpp  == bpp
               )
            {
                fModeExists = 1;
            }
        }
        if (!fModeExists)
        {
            for (i = 0; i < MAX_CUSTOM_MODES; i++)
            {
                /* item free? */
                if (!customModes[i].xres)
                {
                    customModes[i].xres = xres;
                    customModes[i].yres = yres;
                    customModes[i].bpp  = bpp;
                    break;
                }
            }
            writeCustomModes(hkeyVideo);
        }
        RegCloseKey(hkeyVideo);
    }
    return RTEXITCODE_SUCCESS;
}

static DECLCALLBACK(RTEXITCODE) handleRemoveCustomMode(int argc, char *argv[])
{
    if (argc != 3)
    {
        usage(REMOVE_CUST_MODE);
        return RTEXITCODE_FAILURE;
    }

    DWORD xres = atoi(argv[0]);
    DWORD yres = atoi(argv[1]);
    DWORD bpp  = atoi(argv[2]);

    HKEY hkeyVideo = getVideoKey(true);

    if (hkeyVideo)
    {
        getCustomModes(hkeyVideo);
        for (int i = 0; i < MAX_CUSTOM_MODES; i++)
        {
            /* correct item? */
            if (   (customModes[i].xres == xres)
                && (customModes[i].yres == yres)
                && (customModes[i].bpp  == bpp))
            {
                RTPrintf("found mode at index %d\n", i);
                RT_ZERO(customModes[i]);
                break;
            }
        }
        writeCustomModes(hkeyVideo);
        RegCloseKey(hkeyVideo);
    }

    return RTEXITCODE_SUCCESS;
}

#endif /* RT_OS_WINDOWS */

#ifdef VBOX_WITH_GUEST_PROPS
/**
 * Retrieves a value from the guest property store.
 * This is accessed through the "VBoxGuestPropSvc" HGCM service.
 *
 * @returns Command exit code.
 * @note see the command line API description for parameters
 */
static RTEXITCODE getGuestProperty(int argc, char **argv)
{
    using namespace guestProp;

    bool fVerbose = false;
    if (   2 == argc
        && (   strcmp(argv[1], "-verbose")  == 0
            || strcmp(argv[1], "--verbose") == 0)
       )
        fVerbose = true;
    else if (argc != 1)
    {
        usage(GUEST_PROP);
        return RTEXITCODE_FAILURE;
    }

    uint32_t u32ClientId = 0;
    int rc = VINF_SUCCESS;

    rc = VbglR3GuestPropConnect(&u32ClientId);
    if (RT_FAILURE(rc))
        VBoxControlError("Failed to connect to the guest property service, error %Rrc\n", rc);

    /*
     * Here we actually retrieve the value from the host.
     */
    const char *pszName = argv[0];
    char *pszValue = NULL;
    uint64_t u64Timestamp = 0;
    char *pszFlags = NULL;
    /* The buffer for storing the data and its initial size.  We leave a bit
     * of space here in case the maximum values are raised. */
    void *pvBuf = NULL;
    uint32_t cbBuf = MAX_VALUE_LEN + MAX_FLAGS_LEN + 1024;
    if (RT_SUCCESS(rc))
    {
        /* Because there is a race condition between our reading the size of a
         * property and the guest updating it, we loop a few times here and
         * hope.  Actually this should never go wrong, as we are generous
         * enough with buffer space. */
        bool finish = false;
        for (unsigned i = 0; (i < 10) && !finish; ++i)
        {
            void *pvTmpBuf = RTMemRealloc(pvBuf, cbBuf);
            if (NULL == pvTmpBuf)
            {
                rc = VERR_NO_MEMORY;
                VBoxControlError("Out of memory\n");
            }
            else
            {
                pvBuf = pvTmpBuf;
                rc = VbglR3GuestPropRead(u32ClientId, pszName, pvBuf, cbBuf,
                                         &pszValue, &u64Timestamp, &pszFlags,
                                         &cbBuf);
            }
            if (VERR_BUFFER_OVERFLOW == rc)
                /* Leave a bit of extra space to be safe */
                cbBuf += 1024;
            else
                finish = true;
        }
        if (VERR_TOO_MUCH_DATA == rc)
            VBoxControlError("Temporarily unable to retrieve the property\n");
        else if (RT_FAILURE(rc) && rc != VERR_NOT_FOUND)
            VBoxControlError("Failed to retrieve the property value, error %Rrc\n", rc);
    }

    /*
     * And display it on the guest console.
     */
    if (VERR_NOT_FOUND == rc)
        RTPrintf("No value set!\n");
    else if (RT_SUCCESS(rc))
    {
        RTPrintf("Value: %s\n", pszValue);
        if (fVerbose)
        {
            RTPrintf("Timestamp: %lld ns\n", u64Timestamp);
            RTPrintf("Flags: %s\n", pszFlags);
        }
    }

    if (u32ClientId != 0)
        VbglR3GuestPropDisconnect(u32ClientId);
    RTMemFree(pvBuf);
    return RT_SUCCESS(rc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}


/**
 * Writes a value to the guest property store.
 * This is accessed through the "VBoxGuestPropSvc" HGCM service.
 *
 * @returns Command exit code.
 * @note see the command line API description for parameters
 */
static RTEXITCODE setGuestProperty(int argc, char *argv[])
{
    /*
     * Check the syntax.  We can deduce the correct syntax from the number of
     * arguments.
     */
    bool usageOK = true;
    const char *pszName = NULL;
    const char *pszValue = NULL;
    const char *pszFlags = NULL;
    if (2 == argc)
    {
        pszValue = argv[1];
    }
    else if (3 == argc)
        usageOK = false;
    else if (4 == argc)
    {
        pszValue = argv[1];
        if (   strcmp(argv[2], "-flags") != 0
            && strcmp(argv[2], "--flags") != 0)
            usageOK = false;
        pszFlags = argv[3];
    }
    else if (argc != 1)
        usageOK = false;
    if (!usageOK)
    {
        usage(GUEST_PROP);
        return RTEXITCODE_FAILURE;
    }
    /* This is always needed. */
    pszName = argv[0];

    /*
     * Do the actual setting.
     */
    uint32_t u32ClientId = 0;
    int rc = VINF_SUCCESS;
    rc = VbglR3GuestPropConnect(&u32ClientId);
    if (RT_FAILURE(rc))
        VBoxControlError("Failed to connect to the guest property service, error %Rrc\n", rc);
    else
    {
        if (pszFlags != NULL)
            rc = VbglR3GuestPropWrite(u32ClientId, pszName, pszValue, pszFlags);
        else
            rc = VbglR3GuestPropWriteValue(u32ClientId, pszName, pszValue);
        if (RT_FAILURE(rc))
            VBoxControlError("Failed to store the property value, error %Rrc\n", rc);
    }

    if (u32ClientId != 0)
        VbglR3GuestPropDisconnect(u32ClientId);
    return RT_SUCCESS(rc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}


/**
 * Deletes a guest property from the guest property store.
 * This is accessed through the "VBoxGuestPropSvc" HGCM service.
 *
 * @returns Command exit code.
 * @note see the command line API description for parameters
 */
static RTEXITCODE deleteGuestProperty(int argc, char *argv[])
{
    /*
     * Check the syntax.  We can deduce the correct syntax from the number of
     * arguments.
     */
    bool usageOK = true;
    const char *pszName = NULL;
    if (argc < 1)
        usageOK = false;
    if (!usageOK)
    {
        usage(GUEST_PROP);
        return RTEXITCODE_FAILURE;
    }
    /* This is always needed. */
    pszName = argv[0];

    /*
     * Do the actual setting.
     */
    uint32_t u32ClientId = 0;
    int rc = VbglR3GuestPropConnect(&u32ClientId);
    if (RT_FAILURE(rc))
        VBoxControlError("Failed to connect to the guest property service, error %Rrc\n", rc);
    else
    {
        rc = VbglR3GuestPropDelete(u32ClientId, pszName);
        if (RT_FAILURE(rc))
            VBoxControlError("Failed to delete the property value, error %Rrc\n", rc);
    }

    if (u32ClientId != 0)
        VbglR3GuestPropDisconnect(u32ClientId);
    return RT_SUCCESS(rc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}


/**
 * Enumerates the properties in the guest property store.
 * This is accessed through the "VBoxGuestPropSvc" HGCM service.
 *
 * @returns Command exit code.
 * @note see the command line API description for parameters
 */
static RTEXITCODE enumGuestProperty(int argc, char *argv[])
{
    /*
     * Check the syntax.  We can deduce the correct syntax from the number of
     * arguments.
     */
    char const * const *papszPatterns = NULL;
    uint32_t cPatterns = 0;
    if (    argc > 1
        && (   strcmp(argv[0], "-patterns") == 0
            || strcmp(argv[0], "--patterns") == 0))
    {
        papszPatterns = (char const * const *)&argv[1];
        cPatterns = argc - 1;
    }
    else if (argc != 0)
    {
        usage(GUEST_PROP);
        return RTEXITCODE_FAILURE;
    }

    /*
     * Do the actual enumeration.
     */
    uint32_t u32ClientId = 0;
    int rc = VbglR3GuestPropConnect(&u32ClientId);
    if (RT_SUCCESS(rc))
    {
        PVBGLR3GUESTPROPENUM pHandle;
        const char *pszName, *pszValue, *pszFlags;
        uint64_t u64Timestamp;

        rc = VbglR3GuestPropEnum(u32ClientId, papszPatterns, cPatterns, &pHandle,
                                 &pszName, &pszValue, &u64Timestamp, &pszFlags);
        if (RT_SUCCESS(rc))
        {
            while (RT_SUCCESS(rc) && pszName)
            {
                RTPrintf("Name: %s, value: %s, timestamp: %lld, flags: %s\n",
                         pszName, pszValue, u64Timestamp, pszFlags);

                rc = VbglR3GuestPropEnumNext(pHandle, &pszName, &pszValue, &u64Timestamp, &pszFlags);
                if (RT_FAILURE(rc))
                    VBoxControlError("Error while enumerating guest properties: %Rrc\n", rc);
            }

            VbglR3GuestPropEnumFree(pHandle);
        }
        else if (VERR_NOT_FOUND == rc)
            RTPrintf("No properties found.\n");
        else
            VBoxControlError("Failed to enumerate the guest properties! Error: %Rrc\n", rc);
        VbglR3GuestPropDisconnect(u32ClientId);
    }
    else
        VBoxControlError("Failed to connect to the guest property service! Error: %Rrc\n", rc);
    return RT_SUCCESS(rc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}


/**
 * Waits for notifications of changes to guest properties.
 * This is accessed through the "VBoxGuestPropSvc" HGCM service.
 *
 * @returns Command exit code.
 * @note see the command line API description for parameters
 */
static RTEXITCODE waitGuestProperty(int argc, char **argv)
{
    using namespace guestProp;

    /*
     * Handle arguments
     */
    const char *pszPatterns = NULL;
    uint64_t u64TimestampIn = 0;
    uint32_t u32Timeout = RT_INDEFINITE_WAIT;
    bool usageOK = true;
    if (argc < 1)
        usageOK = false;
    pszPatterns = argv[0];
    for (int i = 1; usageOK && i < argc; ++i)
    {
        if (   strcmp(argv[i], "-timeout")  == 0
            || strcmp(argv[i], "--timeout") == 0)
        {
            if (   i + 1 >= argc
                || RTStrToUInt32Full(argv[i + 1], 10, &u32Timeout)
                       != VINF_SUCCESS
               )
                usageOK = false;
            else
                ++i;
        }
        else if (   strcmp(argv[i], "-timestamp")  == 0
                 || strcmp(argv[i], "--timestamp") == 0)
        {
            if (   i + 1 >= argc
                || RTStrToUInt64Full(argv[i + 1], 10, &u64TimestampIn)
                       != VINF_SUCCESS
               )
                usageOK = false;
            else
                ++i;
        }
        else
            usageOK = false;
    }
    if (!usageOK)
    {
        usage(GUEST_PROP);
        return RTEXITCODE_FAILURE;
    }

    /*
     * Connect to the service
     */
    uint32_t u32ClientId = 0;
    int rc = VINF_SUCCESS;

    rc = VbglR3GuestPropConnect(&u32ClientId);
    if (RT_FAILURE(rc))
        VBoxControlError("Failed to connect to the guest property service, error %Rrc\n", rc);

    /*
     * Retrieve the notification from the host
     */
    char *pszName = NULL;
    char *pszValue = NULL;
    uint64_t u64TimestampOut = 0;
    char *pszFlags = NULL;
    /* The buffer for storing the data and its initial size.  We leave a bit
     * of space here in case the maximum values are raised. */
    void *pvBuf = NULL;
    uint32_t cbBuf = MAX_NAME_LEN + MAX_VALUE_LEN + MAX_FLAGS_LEN + 1024;
    /* Because there is a race condition between our reading the size of a
     * property and the guest updating it, we loop a few times here and
     * hope.  Actually this should never go wrong, as we are generous
     * enough with buffer space. */
    bool finish = false;
    for (unsigned i = 0;
         (RT_SUCCESS(rc) || rc == VERR_BUFFER_OVERFLOW) && !finish && (i < 10);
         ++i)
    {
        void *pvTmpBuf = RTMemRealloc(pvBuf, cbBuf);
        if (NULL == pvTmpBuf)
        {
            rc = VERR_NO_MEMORY;
            VBoxControlError("Out of memory\n");
        }
        else
        {
            pvBuf = pvTmpBuf;
            rc = VbglR3GuestPropWait(u32ClientId, pszPatterns, pvBuf, cbBuf,
                                     u64TimestampIn, u32Timeout,
                                     &pszName, &pszValue, &u64TimestampOut,
                                     &pszFlags, &cbBuf);
        }
        if (VERR_BUFFER_OVERFLOW == rc)
            /* Leave a bit of extra space to be safe */
            cbBuf += 1024;
        else
            finish = true;
        if (rc == VERR_TOO_MUCH_DATA)
            VBoxControlError("Temporarily unable to get a notification\n");
        else if (rc == VERR_INTERRUPTED)
            VBoxControlError("The request timed out or was interrupted\n");
#ifndef RT_OS_WINDOWS  /* Windows guests do not do this right */
        else if (RT_FAILURE(rc) && rc != VERR_NOT_FOUND)
            VBoxControlError("Failed to get a notification, error %Rrc\n", rc);
#endif
    }

    /*
     * And display it on the guest console.
     */
    if (VERR_NOT_FOUND == rc)
        RTPrintf("No value set!\n");
    else if (rc == VERR_BUFFER_OVERFLOW)
        RTPrintf("Internal error: unable to determine the size of the data!\n");
    else if (RT_SUCCESS(rc))
    {
        RTPrintf("Name: %s\n", pszName);
        RTPrintf("Value: %s\n", pszValue);
        RTPrintf("Timestamp: %lld ns\n", u64TimestampOut);
        RTPrintf("Flags: %s\n", pszFlags);
    }

    if (u32ClientId != 0)
        VbglR3GuestPropDisconnect(u32ClientId);
    RTMemFree(pvBuf);
    return RT_SUCCESS(rc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}


/**
 * Access the guest property store through the "VBoxGuestPropSvc" HGCM
 * service.
 *
 * @returns 0 on success, 1 on failure
 * @note see the command line API description for parameters
 */
static DECLCALLBACK(RTEXITCODE) handleGuestProperty(int argc, char *argv[])
{
    if (0 == argc)
    {
        usage(GUEST_PROP);
        return RTEXITCODE_FAILURE;
    }
    if (!strcmp(argv[0], "get"))
        return getGuestProperty(argc - 1, argv + 1);
    else if (!strcmp(argv[0], "set"))
        return setGuestProperty(argc - 1, argv + 1);
    else if (!strcmp(argv[0], "delete") || !strcmp(argv[0], "unset"))
        return deleteGuestProperty(argc - 1, argv + 1);
    else if (!strcmp(argv[0], "enumerate"))
        return enumGuestProperty(argc - 1, argv + 1);
    else if (!strcmp(argv[0], "wait"))
        return waitGuestProperty(argc - 1, argv + 1);
    /* else */
    usage(GUEST_PROP);
    return RTEXITCODE_FAILURE;
}
#endif

#ifdef VBOX_WITH_SHARED_FOLDERS
/**
 * Lists the Shared Folders provided by the host.
 */
static RTEXITCODE listSharedFolders(int argc, char **argv)
{
    bool usageOK = true;
    bool fOnlyShowAutoMount = false;
    if (argc == 1)
    {
        if (   !strcmp(argv[0], "-automount")
            || !strcmp(argv[0], "--automount"))
            fOnlyShowAutoMount = true;
        else
            usageOK = false;
    }
    else if (argc > 1)
        usageOK = false;

    if (!usageOK)
    {
        usage(GUEST_SHAREDFOLDERS);
        return RTEXITCODE_FAILURE;
    }

    uint32_t u32ClientId;
    int rc = VbglR3SharedFolderConnect(&u32ClientId);
    if (RT_FAILURE(rc))
        VBoxControlError("Failed to connect to the shared folder service, error %Rrc\n", rc);
    else
    {
        PVBGLR3SHAREDFOLDERMAPPING paMappings;
        uint32_t cMappings;
        rc = VbglR3SharedFolderGetMappings(u32ClientId, fOnlyShowAutoMount,
                                           &paMappings, &cMappings);
        if (RT_SUCCESS(rc))
        {
            if (fOnlyShowAutoMount)
                RTPrintf("Auto-mounted Shared Folder mappings (%u):\n\n", cMappings);
            else
                RTPrintf("Shared Folder mappings (%u):\n\n", cMappings);

            for (uint32_t i = 0; i < cMappings; i++)
            {
                char *pszName;
                rc = VbglR3SharedFolderGetName(u32ClientId, paMappings[i].u32Root, &pszName);
                if (RT_SUCCESS(rc))
                {
                    RTPrintf("%02u - %s\n", i + 1, pszName);
                    RTStrFree(pszName);
                }
                else
                    VBoxControlError("Error while getting the shared folder name for root node = %u, rc = %Rrc\n",
                                     paMappings[i].u32Root, rc);
            }
            if (!cMappings)
                RTPrintf("No Shared Folders available.\n");
            VbglR3SharedFolderFreeMappings(paMappings);
        }
        else
            VBoxControlError("Error while getting the shared folder mappings, rc = %Rrc\n", rc);
        VbglR3SharedFolderDisconnect(u32ClientId);
    }
    return RT_SUCCESS(rc) ? RTEXITCODE_SUCCESS : RTEXITCODE_FAILURE;
}

/**
 * Handles Shared Folders control.
 *
 * @returns 0 on success, 1 on failure
 * @note see the command line API description for parameters
 *      (r=bird: yeah, right. The API description contains nil about params)
 */
static DECLCALLBACK(RTEXITCODE) handleSharedFolder(int argc, char *argv[])
{
    if (0 == argc)
    {
        usage(GUEST_SHAREDFOLDERS);
        return RTEXITCODE_FAILURE;
    }
    if (!strcmp(argv[0], "list"))
        return listSharedFolders(argc - 1, argv + 1);
    /* else */
    usage(GUEST_SHAREDFOLDERS);
    return RTEXITCODE_FAILURE;
}
#endif

#if !defined(VBOX_CONTROL_TEST)
/**
 * @callback_method_impl{FNVBOXCTRLCMDHANDLER, Command: writecoredump}
 */
static DECLCALLBACK(RTEXITCODE) handleWriteCoreDump(int argc, char *argv[])
{
    int rc = VbglR3WriteCoreDump();
    if (RT_SUCCESS(rc))
    {
        RTPrintf("Guest core dump successful.\n");
        return RTEXITCODE_SUCCESS;
    }
    else
    {
        VBoxControlError("Error while taking guest core dump. rc=%Rrc\n", rc);
        return RTEXITCODE_FAILURE;
    }
}
#endif

#ifdef VBOX_WITH_DPC_LATENCY_CHECKER
/**
 * @callback_method_impl{FNVBOXCTRLCMDHANDLER, Command: help}
 */
static DECLCALLBACK(RTEXITCODE) handleDpc(int argc, char *argv[])
{
# ifndef VBOX_CONTROL_TEST
    int rc;
    for (int i = 0; i < 30; i++)
    {
        rc = vbglR3DoIOCtl(VBOXGUEST_IOCTL_DPC_LATENCY_CHECKER, NULL, 0);
        if (RT_FAILURE(rc))
            break;
        RTPrintf("%d\n", i);
    }
# else
    int rc = VERR_NOT_IMPLEMENTED;
# endif
    if (RT_FAILURE(rc))
        return VBoxControlError("Error. rc=%Rrc\n", rc);
    RTPrintf("Samples collection completed.\n");
    return RTEXITCODE_SUCCESS;
}
#endif /* VBOX_WITH_DPC_LATENCY_CHECKER */


/**
 * @callback_method_impl{FNVBOXCTRLCMDHANDLER, Command: writelog}
 */
static DECLCALLBACK(RTEXITCODE) handleWriteLog(int argc, char *argv[])
{
    static const RTGETOPTDEF s_aOptions[] =
    {
        { "--no-newline", 'n', RTGETOPT_REQ_NOTHING },
    };
    bool fNoNewline = false;

    RTGETOPTSTATE GetOptState;
    int rc = RTGetOptInit(&GetOptState, argc, argv, s_aOptions, RT_ELEMENTS(s_aOptions),
                          0 /*iFirst*/, RTGETOPTINIT_FLAGS_OPTS_FIRST);
    if (RT_SUCCESS(rc))
    {
        RTGETOPTUNION   ValueUnion;
        int             ch;
        while ((ch = RTGetOpt(&GetOptState, &ValueUnion)) != 0)
        {
            switch (ch)
            {
                case VINF_GETOPT_NOT_OPTION:
                {
                    size_t cch = strlen(ValueUnion.psz);
                    if (   fNoNewline
                        || (cch > 0 && ValueUnion.psz[cch - 1] == '\n') )
                        rc = VbglR3WriteLog(ValueUnion.psz, cch);
                    else
                    {
                        char *pszDup = (char *)RTMemDupEx(ValueUnion.psz, cch, 2);
                        if (RT_SUCCESS(rc))
                        {
                            pszDup[cch++] = '\n';
                            pszDup[cch]   = '\0';
                            rc = VbglR3WriteLog(pszDup, cch);
                            RTMemFree(pszDup);
                        }
                        else
                            rc = VERR_NO_MEMORY;
                    }
                    if (RT_FAILURE(rc))
                        return VBoxControlError("VbglR3WriteLog: %Rrc", rc);
                    break;
                }

                case 'n':
                    fNoNewline = true;
                    break;

                case 'h': return usage(WRITE_LOG);
                case 'V': return printVersion();
                default:
                    return VBoxCtrlGetOptError(ch, &ValueUnion);
            }
        }
    }
    else
        return VBoxControlError("RTGetOptInit: %Rrc", rc);
    return RTEXITCODE_SUCCESS;
}


/**
 * @callback_method_impl{FNVBOXCTRLCMDHANDLER, Command: takesnapshot}
 */
static DECLCALLBACK(RTEXITCODE) handleTakeSnapshot(int argc, char *argv[])
{
    //VbglR3VmTakeSnapshot(argv[0], argv[1]);
    return VBoxControlError("not implemented");
}

/**
 * @callback_method_impl{FNVBOXCTRLCMDHANDLER, Command: savestate}
 */
static DECLCALLBACK(RTEXITCODE) handleSaveState(int argc, char *argv[])
{
    //VbglR3VmSaveState();
    return VBoxControlError("not implemented");
}

/**
 * @callback_method_impl{FNVBOXCTRLCMDHANDLER, Command: suspend|pause}
 */
static DECLCALLBACK(RTEXITCODE) handleSuspend(int argc, char *argv[])
{
    //VbglR3VmSuspend();
    return VBoxControlError("not implemented");
}

/**
 * @callback_method_impl{FNVBOXCTRLCMDHANDLER, Command: poweroff|powerdown}
 */
static DECLCALLBACK(RTEXITCODE) handlePowerOff(int argc, char *argv[])
{
    //VbglR3VmPowerOff();
    return VBoxControlError("not implemented");
}

/**
 * @callback_method_impl{FNVBOXCTRLCMDHANDLER, Command: version}
 */
static DECLCALLBACK(RTEXITCODE) handleVersion(int argc, char *argv[])
{
    if (argc)
        return VBoxControlSyntaxError("getversion does not take any arguments");
    return printVersion();
}

/**
 * @callback_method_impl{FNVBOXCTRLCMDHANDLER, Command: help}
 */
static DECLCALLBACK(RTEXITCODE) handleHelp(int argc, char *argv[])
{
    /* ignore arguments for now. */
    usage();
    return RTEXITCODE_SUCCESS;
}


/** command handler type */
typedef DECLCALLBACK(RTEXITCODE) FNVBOXCTRLCMDHANDLER(int argc, char *argv[]);
typedef FNVBOXCTRLCMDHANDLER *PFNVBOXCTRLCMDHANDLER;

/** The table of all registered command handlers. */
struct COMMANDHANDLER
{
    const char *pszCommand;
    PFNVBOXCTRLCMDHANDLER pfnHandler;
} g_aCommandHandlers[] =
{
#if defined(RT_OS_WINDOWS) && !defined(VBOX_CONTROL_TEST)
    { "getvideoacceleration",   handleGetVideoAcceleration },
    { "setvideoacceleration",   handleSetVideoAcceleration },
    { "videoflags",             handleVideoFlags },
    { "listcustommodes",        handleListCustomModes },
    { "addcustommode",          handleAddCustomMode },
    { "removecustommode",       handleRemoveCustomMode },
    { "setvideomode",           handleSetVideoMode },
#endif
#ifdef VBOX_WITH_GUEST_PROPS
    { "guestproperty",          handleGuestProperty },
#endif
#ifdef VBOX_WITH_SHARED_FOLDERS
    { "sharedfolder",           handleSharedFolder },
#endif
#if !defined(VBOX_CONTROL_TEST)
    { "writecoredump",          handleWriteCoreDump },
#endif
#ifdef VBOX_WITH_DPC_LATENCY_CHECKER
    { "dpc",                    handleDpc },
#endif
    { "writelog",               handleWriteLog },
    { "takesnapshot",           handleTakeSnapshot },
    { "savestate",              handleSaveState },
    { "suspend",                handleSuspend },
    { "pause",                  handleSuspend },
    { "poweroff",               handlePowerOff },
    { "powerdown",              handlePowerOff },
    { "getversion",             handleVersion },
    { "version",                handleVersion },
    { "help",                   handleHelp }
};

/** Main function */
int main(int argc, char **argv)
{
    /** The application's global return code */
    RTEXITCODE rcExit = RTEXITCODE_SUCCESS;
    /** An IPRT return code for local use */
    int rrc = VINF_SUCCESS;
    /** The index of the command line argument we are currently processing */
    int iArg = 1;
    /** Should we show the logo text? */
    bool fShowLogo = true;
    /** Should we print the usage after the logo?  For the -help switch. */
    bool fDoHelp = false;
    /** Will we be executing a command or just printing information? */
    bool fOnlyInfo = false;

    rrc = RTR3InitExe(argc, &argv, 0);
    if (RT_FAILURE(rrc))
        return RTMsgInitFailure(rrc);

    /*
     * Start by handling command line switches
     */
    /** @todo RTGetOpt conversion of the whole file. */
    bool done = false;      /**< Are we finished with handling switches? */
    while (!done && (iArg < argc))
    {
        if (   !strcmp(argv[iArg], "-V")
            || !strcmp(argv[iArg], "-v")
            || !strcmp(argv[iArg], "--version")
            || !strcmp(argv[iArg], "-version")
           )
        {
            /* Print version number, and do nothing else. */
            printVersion();
            fOnlyInfo = true;
            fShowLogo = false;
            done = true;
        }
        else if (   !strcmp(argv[iArg], "-nologo")
                 || !strcmp(argv[iArg], "--nologo"))
            fShowLogo = false;
        else if (   !strcmp(argv[iArg], "-help")
                 || !strcmp(argv[iArg], "--help"))
        {
            fOnlyInfo = true;
            fDoHelp = true;
            done = true;
        }
        else
            /* We have found an argument which isn't a switch.  Exit to the
             * command processing bit. */
            done = true;
        if (!done)
            ++iArg;
    }

    /*
     * Find the application name, show our logo if the user hasn't suppressed it,
     * and show the usage if the user asked us to
     */
    g_pszProgName = RTPathFilename(argv[0]);
    if (fShowLogo)
        RTPrintf(VBOX_PRODUCT " Guest Additions Command Line Management Interface Version "
                 VBOX_VERSION_STRING "\n"
                 "(C) 2008-" VBOX_C_YEAR " " VBOX_VENDOR "\n"
                 "All rights reserved.\n\n");
    if (fDoHelp)
        usage();

    /*
     * Do global initialisation for the programme if we will be handling a command
     */
    if (!fOnlyInfo)
    {
        rrc = VbglR3Init();
        if (RT_FAILURE(rrc))
        {
            VBoxControlError("Could not contact the host system.  Make sure that you are running this\n"
                             "application inside a VirtualBox guest system, and that you have sufficient\n"
                             "user permissions.\n");
            rcExit = RTEXITCODE_FAILURE;
        }
    }

    /*
     * Now look for an actual command in the argument list and handle it.
     */

    if (!fOnlyInfo && rcExit == RTEXITCODE_SUCCESS)
    {
        if (argc > iArg)
        {
            /*
             * Try locate the command and execute it, complain if not found.
             */
            unsigned i;
            for (i = 0; i < RT_ELEMENTS(g_aCommandHandlers); i++)
                if (!strcmp(argv[iArg], g_aCommandHandlers[i].pszCommand))
                {
                    rcExit = g_aCommandHandlers[i].pfnHandler(argc - iArg - 1, argv + iArg + 1);
                    break;
                }
            if (i >= RT_ELEMENTS(g_aCommandHandlers))
            {
                rcExit = RTEXITCODE_FAILURE;
                usage();
            }
        }
        else
        {
            /* The user didn't specify a command. */
            rcExit = RTEXITCODE_FAILURE;
            usage();
        }
    }

    /*
     * And exit, returning the status
     */
    return rcExit;
}

