/* $Id$ */
/** @file
 * VBoxService - Virtual Machine Information for the Host.
 */

/*
 * Copyright (C) 2009-2015 Oracle Corporation
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
#ifdef RT_OS_WINDOWS
# ifdef TARGET_NT4 /* HACK ALERT! PMIB_IPSTATS undefined if 0x0400 with newer SDKs. */
#  undef _WIN32_WINNT
#  define _WIN32_WINNT 0x0500
# endif
# include <winsock2.h>
# include <iphlpapi.h>
# include <ws2tcpip.h>
# include <windows.h>
# include <Ntsecapi.h>
#else
# define __STDC_LIMIT_MACROS
# include <arpa/inet.h>
# include <errno.h>
# include <netinet/in.h>
# include <sys/ioctl.h>
# include <sys/socket.h>
# include <net/if.h>
# include <pwd.h> /* getpwuid */
# include <unistd.h>
# if !defined(RT_OS_OS2) && !defined(RT_OS_FREEBSD) && !defined(RT_OS_HAIKU)
#  include <utmpx.h> /* @todo FreeBSD 9 should have this. */
# endif
# ifdef RT_OS_OS2
#  include <net/if_dl.h>
# endif
# ifdef RT_OS_SOLARIS
#  include <sys/sockio.h>
#  include <net/if_arp.h>
# endif
# if defined(RT_OS_DARWIN) || defined(RT_OS_FREEBSD)
#  include <ifaddrs.h> /* getifaddrs, freeifaddrs */
#  include <net/if_dl.h> /* LLADDR */
#  include <netdb.h> /* getnameinfo */
# endif
# ifdef VBOX_WITH_DBUS
#  include <VBox/dbus.h>
# endif
#endif

#include <iprt/mem.h>
#include <iprt/thread.h>
#include <iprt/string.h>
#include <iprt/semaphore.h>
#include <iprt/system.h>
#include <iprt/time.h>
#include <iprt/assert.h>
#include <VBox/version.h>
#include <VBox/VBoxGuestLib.h>
#include "VBoxServiceInternal.h"
#include "VBoxServiceUtils.h"
#include "VBoxServicePropCache.h"


/** Structure containing information about a location awarness
 *  client provided by the host. */
/** @todo Move this (and functions) into VbglR3. */
typedef struct VBOXSERVICELACLIENTINFO
{
    uint32_t    uID;
    char       *pszName;
    char       *pszLocation;
    char       *pszDomain;
    bool        fAttached;
    uint64_t    uAttachedTS;
} VBOXSERVICELACLIENTINFO, *PVBOXSERVICELACLIENTINFO;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
/** The vminfo interval (milliseconds). */
static uint32_t                 g_cMsVMInfoInterval = 0;
/** The semaphore we're blocking on. */
static RTSEMEVENTMULTI          g_hVMInfoEvent = NIL_RTSEMEVENTMULTI;
/** The guest property service client ID. */
static uint32_t                 g_uVMInfoGuestPropSvcClientID = 0;
/** Number of currently logged in users in OS. */
static uint32_t                 g_cVMInfoLoggedInUsers = 0;
/** The guest property cache. */
static VBOXSERVICEVEPROPCACHE   g_VMInfoPropCache;
static const char              *g_pszPropCacheValLoggedInUsersList = "/VirtualBox/GuestInfo/OS/LoggedInUsersList";
static const char              *g_pszPropCacheValLoggedInUsers = "/VirtualBox/GuestInfo/OS/LoggedInUsers";
static const char              *g_pszPropCacheValNoLoggedInUsers = "/VirtualBox/GuestInfo/OS/NoLoggedInUsers";
static const char              *g_pszPropCacheValNetCount = "/VirtualBox/GuestInfo/Net/Count";
/** A guest user's guest property root key. */
static const char              *g_pszPropCacheValUser = "/VirtualBox/GuestInfo/User/";
/** The VM session ID. Changes whenever the VM is restored or reset. */
static uint64_t                 g_idVMInfoSession;
/** The last attached locartion awareness (LA) client timestamp. */
static uint64_t                 g_LAClientAttachedTS = 0;
/** The current LA client info. */
static VBOXSERVICELACLIENTINFO  g_LAClientInfo;
/** User idle threshold (in ms). This specifies the minimum time a user is considered
 *  as being idle and then will be reported to the host. Default is 5s. */
uint32_t                        g_uVMInfoUserIdleThresholdMS = 5 * 1000;


/*********************************************************************************************************************************
*   Defines                                                                                                                      *
*********************************************************************************************************************************/
static const char *g_pszLAActiveClient = "/VirtualBox/HostInfo/VRDP/ActiveClient";

#ifdef VBOX_WITH_DBUS
/** ConsoleKit defines (taken from 0.4.5). */
#define CK_NAME      "org.freedesktop.ConsoleKit"
#define CK_PATH      "/org/freedesktop/ConsoleKit"
#define CK_INTERFACE "org.freedesktop.ConsoleKit"

#define CK_MANAGER_PATH      "/org/freedesktop/ConsoleKit/Manager"
#define CK_MANAGER_INTERFACE "org.freedesktop.ConsoleKit.Manager"
#define CK_SEAT_INTERFACE    "org.freedesktop.ConsoleKit.Seat"
#define CK_SESSION_INTERFACE "org.freedesktop.ConsoleKit.Session"
#endif



/**
 * Signals the event so that a re-enumeration of VM-specific
 * information (like logged in users) can happen.
 *
 * @return  IPRT status code.
 */
int VBoxServiceVMInfoSignal(void)
{
    /* Trigger a re-enumeration of all logged-in users by unblocking
     * the multi event semaphore of the VMInfo thread. */
    if (g_hVMInfoEvent)
        return RTSemEventMultiSignal(g_hVMInfoEvent);

    return VINF_SUCCESS;
}


/** @copydoc VBOXSERVICE::pfnPreInit */
static DECLCALLBACK(int) VBoxServiceVMInfoPreInit(void)
{
    return VINF_SUCCESS;
}


/** @copydoc VBOXSERVICE::pfnOption */
static DECLCALLBACK(int) VBoxServiceVMInfoOption(const char **ppszShort, int argc, char **argv, int *pi)
{
    /** @todo Use RTGetOpt here. */

    int rc = -1;
    if (ppszShort)
        /* no short options */;
    else if (!strcmp(argv[*pi], "--vminfo-interval"))
        rc = VBoxServiceArgUInt32(argc, argv, "", pi,
                                  &g_cMsVMInfoInterval, 1, UINT32_MAX - 1);
    else if (!strcmp(argv[*pi], "--vminfo-user-idle-threshold"))
        rc = VBoxServiceArgUInt32(argc, argv, "", pi,
                                  &g_uVMInfoUserIdleThresholdMS, 1, UINT32_MAX - 1);
    return rc;
}


/** @copydoc VBOXSERVICE::pfnInit */
static DECLCALLBACK(int) VBoxServiceVMInfoInit(void)
{
    /*
     * If not specified, find the right interval default.
     * Then create the event sem to block on.
     */
    if (!g_cMsVMInfoInterval)
        g_cMsVMInfoInterval = g_DefaultInterval * 1000;
    if (!g_cMsVMInfoInterval)
    {
        /* Set it to 5s by default for location awareness checks. */
        g_cMsVMInfoInterval = 5 * 1000;
    }

    int rc = RTSemEventMultiCreate(&g_hVMInfoEvent);
    AssertRCReturn(rc, rc);

    VbglR3GetSessionId(&g_idVMInfoSession);
    /* The status code is ignored as this information is not available with VBox < 3.2.10. */

    /* Initialize the LA client object. */
    RT_ZERO(g_LAClientInfo);

    rc = VbglR3GuestPropConnect(&g_uVMInfoGuestPropSvcClientID);
    if (RT_SUCCESS(rc))
        VBoxServiceVerbose(3, "Property Service Client ID: %#x\n", g_uVMInfoGuestPropSvcClientID);
    else
    {
        /* If the service was not found, we disable this service without
           causing VBoxService to fail. */
        if (rc == VERR_HGCM_SERVICE_NOT_FOUND) /* Host service is not available. */
        {
            VBoxServiceVerbose(0, "Guest property service is not available, disabling the service\n");
            rc = VERR_SERVICE_DISABLED;
        }
        else
            VBoxServiceError("Failed to connect to the guest property service! Error: %Rrc\n", rc);
        RTSemEventMultiDestroy(g_hVMInfoEvent);
        g_hVMInfoEvent = NIL_RTSEMEVENTMULTI;
    }

    if (RT_SUCCESS(rc))
    {
        VBoxServicePropCacheCreate(&g_VMInfoPropCache, g_uVMInfoGuestPropSvcClientID);

        /*
         * Declare some guest properties with flags and reset values.
         */
        int rc2 = VBoxServicePropCacheUpdateEntry(&g_VMInfoPropCache, g_pszPropCacheValLoggedInUsersList,
                                                  VBOXSERVICEPROPCACHEFLAG_TEMPORARY | VBOXSERVICEPROPCACHEFLAG_TRANSIENT, NULL /* Delete on exit */);
        if (RT_FAILURE(rc2))
            VBoxServiceError("Failed to init property cache value \"%s\", rc=%Rrc\n", g_pszPropCacheValLoggedInUsersList, rc2);

        rc2 = VBoxServicePropCacheUpdateEntry(&g_VMInfoPropCache, g_pszPropCacheValLoggedInUsers,
                                              VBOXSERVICEPROPCACHEFLAG_TEMPORARY | VBOXSERVICEPROPCACHEFLAG_TRANSIENT, "0");
        if (RT_FAILURE(rc2))
            VBoxServiceError("Failed to init property cache value \"%s\", rc=%Rrc\n", g_pszPropCacheValLoggedInUsers, rc2);

        rc2 = VBoxServicePropCacheUpdateEntry(&g_VMInfoPropCache, g_pszPropCacheValNoLoggedInUsers,
                                              VBOXSERVICEPROPCACHEFLAG_TEMPORARY | VBOXSERVICEPROPCACHEFLAG_TRANSIENT, "true");
        if (RT_FAILURE(rc2))
            VBoxServiceError("Failed to init property cache value \"%s\", rc=%Rrc\n", g_pszPropCacheValNoLoggedInUsers, rc2);

        rc2 = VBoxServicePropCacheUpdateEntry(&g_VMInfoPropCache, g_pszPropCacheValNetCount,
                                              VBOXSERVICEPROPCACHEFLAG_TEMPORARY | VBOXSERVICEPROPCACHEFLAG_ALWAYS_UPDATE, NULL /* Delete on exit */);
        if (RT_FAILURE(rc2))
            VBoxServiceError("Failed to init property cache value \"%s\", rc=%Rrc\n", g_pszPropCacheValNetCount, rc2);

        /*
         * Get configuration guest properties from the host.
         * Note: All properties should have sensible defaults in case the lookup here fails.
         */
        char *pszValue;
        rc2 = VBoxServiceReadHostProp(g_uVMInfoGuestPropSvcClientID, "/VirtualBox/GuestAdd/VBoxService/--vminfo-user-idle-threshold", true /* Read only */,
                                      &pszValue, NULL /* Flags */, NULL /* Timestamp */);
        if (RT_SUCCESS(rc2))
        {
            AssertPtr(pszValue);
            g_uVMInfoUserIdleThresholdMS = RT_CLAMP(RTStrToUInt32(pszValue), 1000, UINT32_MAX - 1);
            RTStrFree(pszValue);
        }
    }
    return rc;
}


/**
 * Retrieves a specifiy client LA property.
 *
 * @return  IPRT status code.
 * @param   uClientID               LA client ID to retrieve property for.
 * @param   pszProperty             Property (without path) to retrieve.
 * @param   ppszValue               Where to store value of property.
 * @param   puTimestamp             Timestamp of property to retrieve. Optional.
 */
static int vboxServiceGetLAClientValue(uint32_t uClientID, const char *pszProperty,
                                       char **ppszValue, uint64_t *puTimestamp)
{
    AssertReturn(uClientID, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pszProperty, VERR_INVALID_POINTER);

    int rc;

    char pszClientPath[255];
    if (RTStrPrintf(pszClientPath, sizeof(pszClientPath),
                    "/VirtualBox/HostInfo/VRDP/Client/%RU32/%s", uClientID, pszProperty))
    {
        rc = VBoxServiceReadHostProp(g_uVMInfoGuestPropSvcClientID, pszClientPath, true /* Read only */,
                                     ppszValue, NULL /* Flags */, puTimestamp);
    }
    else
        rc = VERR_NO_MEMORY;

    return rc;
}


/**
 * Retrieves LA client information. On success the returned structure will have allocated
 * objects which need to be free'd with vboxServiceFreeLAClientInfo.
 *
 * @return  IPRT status code.
 * @param   uClientID               Client ID to retrieve information for.
 * @param   pClient                 Pointer where to store the client information.
 */
static int vboxServiceGetLAClientInfo(uint32_t uClientID, PVBOXSERVICELACLIENTINFO pClient)
{
    AssertReturn(uClientID, VERR_INVALID_PARAMETER);
    AssertPtrReturn(pClient, VERR_INVALID_POINTER);

    int rc = vboxServiceGetLAClientValue(uClientID, "Name", &pClient->pszName,
                                         NULL /* Timestamp */);
    if (RT_SUCCESS(rc))
    {
        char *pszAttach;
        rc = vboxServiceGetLAClientValue(uClientID, "Attach", &pszAttach,
                                         &pClient->uAttachedTS);
        if (RT_SUCCESS(rc))
        {
            AssertPtr(pszAttach);
            pClient->fAttached = !RTStrICmp(pszAttach, "1") ? true : false;

            RTStrFree(pszAttach);
        }
    }
    if (RT_SUCCESS(rc))
        rc = vboxServiceGetLAClientValue(uClientID, "Location", &pClient->pszLocation,
                                         NULL /* Timestamp */);
    if (RT_SUCCESS(rc))
        rc = vboxServiceGetLAClientValue(uClientID, "Domain", &pClient->pszDomain,
                                         NULL /* Timestamp */);
    if (RT_SUCCESS(rc))
        pClient->uID = uClientID;

    return rc;
}


/**
 * Frees all allocated LA client information of a structure.
 *
 * @param   pClient                 Pointer to client information structure to free.
 */
static void vboxServiceFreeLAClientInfo(PVBOXSERVICELACLIENTINFO pClient)
{
    if (pClient)
    {
        if (pClient->pszName)
        {
            RTStrFree(pClient->pszName);
            pClient->pszName = NULL;
        }
        if (pClient->pszLocation)
        {
            RTStrFree(pClient->pszLocation);
            pClient->pszLocation = NULL;
        }
        if (pClient->pszDomain)
        {
            RTStrFree(pClient->pszDomain);
            pClient->pszDomain = NULL;
        }
    }
}


/**
 * Updates a per-guest user guest property inside the given property cache.
 *
 * @return  IPRT status code.
 * @param   pCache                  Pointer to guest property cache to update user in.
 * @param   pszUser                 Name of guest user to update.
 * @param   pszDomain               Domain of guest user to update. Optional.
 * @param   pszKey                  Key name of guest property to update.
 * @param   pszValueFormat          Guest property value to set. Pass NULL for deleting
 *                                  the property.
 */
int vboxServiceUserUpdateF(PVBOXSERVICEVEPROPCACHE pCache, const char *pszUser, const char *pszDomain,
                           const char *pszKey, const char *pszValueFormat, ...)
{
    AssertPtrReturn(pCache, VERR_INVALID_POINTER);
    AssertPtrReturn(pszUser, VERR_INVALID_POINTER);
    /* pszDomain is optional. */
    AssertPtrReturn(pszKey, VERR_INVALID_POINTER);
    /* pszValueFormat is optional. */

    int rc = VINF_SUCCESS;

    char *pszName;
    if (pszDomain)
    {
        if (!RTStrAPrintf(&pszName, "%s%s@%s/%s", g_pszPropCacheValUser, pszUser, pszDomain, pszKey))
            rc = VERR_NO_MEMORY;
    }
    else
    {
        if (!RTStrAPrintf(&pszName, "%s%s/%s", g_pszPropCacheValUser, pszUser, pszKey))
            rc = VERR_NO_MEMORY;
    }

    char *pszValue = NULL;
    if (   RT_SUCCESS(rc)
        && pszValueFormat)
    {
        va_list va;
        va_start(va, pszValueFormat);
        if (RTStrAPrintfV(&pszValue, pszValueFormat, va) < 0)
            rc = VERR_NO_MEMORY;
        va_end(va);
        if (   RT_SUCCESS(rc)
            && !pszValue)
            rc = VERR_NO_STR_MEMORY;
    }

    if (RT_SUCCESS(rc))
        rc = VBoxServicePropCacheUpdate(pCache, pszName, pszValue);
    if (rc == VINF_SUCCESS) /* VBoxServicePropCacheUpdate will also return VINF_NO_CHANGE. */
    {
        /** @todo Combine updating flags w/ updating the actual value. */
        rc = VBoxServicePropCacheUpdateEntry(pCache, pszName,
                                             VBOXSERVICEPROPCACHEFLAG_TEMPORARY | VBOXSERVICEPROPCACHEFLAG_TRANSIENT,
                                             NULL /* Delete on exit */);
    }

    RTStrFree(pszValue);
    RTStrFree(pszName);
    return rc;
}


/**
 * Writes the properties that won't change while the service is running.
 *
 * Errors are ignored.
 */
static void vboxserviceVMInfoWriteFixedProperties(void)
{
    /*
     * First get OS information that won't change.
     */
    char szInfo[256];
    int rc = RTSystemQueryOSInfo(RTSYSOSINFO_PRODUCT, szInfo, sizeof(szInfo));
    VBoxServiceWritePropF(g_uVMInfoGuestPropSvcClientID, "/VirtualBox/GuestInfo/OS/Product",
                          "%s", RT_FAILURE(rc) ? "" : szInfo);

    rc = RTSystemQueryOSInfo(RTSYSOSINFO_RELEASE, szInfo, sizeof(szInfo));
    VBoxServiceWritePropF(g_uVMInfoGuestPropSvcClientID, "/VirtualBox/GuestInfo/OS/Release",
                          "%s", RT_FAILURE(rc) ? "" : szInfo);

    rc = RTSystemQueryOSInfo(RTSYSOSINFO_VERSION, szInfo, sizeof(szInfo));
    VBoxServiceWritePropF(g_uVMInfoGuestPropSvcClientID, "/VirtualBox/GuestInfo/OS/Version",
                          "%s", RT_FAILURE(rc) ? "" : szInfo);

    rc = RTSystemQueryOSInfo(RTSYSOSINFO_SERVICE_PACK, szInfo, sizeof(szInfo));
    VBoxServiceWritePropF(g_uVMInfoGuestPropSvcClientID, "/VirtualBox/GuestInfo/OS/ServicePack",
                          "%s", RT_FAILURE(rc) ? "" : szInfo);

    /*
     * Retrieve version information about Guest Additions and installed files (components).
     */
    char *pszAddVer;
    char *pszAddVerExt;
    char *pszAddRev;
    rc = VbglR3GetAdditionsVersion(&pszAddVer, &pszAddVerExt, &pszAddRev);
    VBoxServiceWritePropF(g_uVMInfoGuestPropSvcClientID, "/VirtualBox/GuestAdd/Version",
                          "%s", RT_FAILURE(rc) ? "" : pszAddVer);
    VBoxServiceWritePropF(g_uVMInfoGuestPropSvcClientID, "/VirtualBox/GuestAdd/VersionExt",
                          "%s", RT_FAILURE(rc) ? "" : pszAddVerExt);
    VBoxServiceWritePropF(g_uVMInfoGuestPropSvcClientID, "/VirtualBox/GuestAdd/Revision",
                          "%s", RT_FAILURE(rc) ? "" : pszAddRev);
    if (RT_SUCCESS(rc))
    {
        RTStrFree(pszAddVer);
        RTStrFree(pszAddVerExt);
        RTStrFree(pszAddRev);
    }

#ifdef RT_OS_WINDOWS
    /*
     * Do windows specific properties.
     */
    char *pszInstDir;
    rc = VbglR3GetAdditionsInstallationPath(&pszInstDir);
    VBoxServiceWritePropF(g_uVMInfoGuestPropSvcClientID, "/VirtualBox/GuestAdd/InstallDir",
                          "%s", RT_FAILURE(rc) ? "" :  pszInstDir);
    if (RT_SUCCESS(rc))
        RTStrFree(pszInstDir);

    VBoxServiceWinGetComponentVersions(g_uVMInfoGuestPropSvcClientID);
#endif
}

#if defined(VBOX_WITH_DBUS) && defined(RT_OS_LINUX) /* Not yet for Solaris/FreeBSB. */
/*
 * Simple wrapper to work around compiler-specific va_list madness.
 */
static dbus_bool_t vboxService_dbus_message_get_args(DBusMessage *message,
                                                     DBusError   *error,
                                                     int first_arg_type,
                                                     ...)
{
    va_list va;
    va_start(va, first_arg_type);
    dbus_bool_t ret = dbus_message_get_args_valist(message, error,
                                                   first_arg_type, va);
    va_end(va);
    return ret;
}
#endif

/**
 * Provide information about active users.
 */
static int vboxserviceVMInfoWriteUsers(void)
{
    int rc = VINF_SUCCESS;
    char *pszUserList = NULL;
    uint32_t cUsersInList = 0;

#ifdef RT_OS_WINDOWS
# ifndef TARGET_NT4
    rc = VBoxServiceVMInfoWinWriteUsers(&g_VMInfoPropCache,
                                        &pszUserList, &cUsersInList);
# else
    rc = VERR_NOT_IMPLEMENTED;
# endif

#elif defined(RT_OS_FREEBSD)
    /** @todo FreeBSD: Port logged on user info retrieval.
     *                 However, FreeBSD 9 supports utmpx, so we could use the code
     *                 block below (?). */
    rc = VERR_NOT_IMPLEMENTED;

#elif defined(RT_OS_HAIKU)
    /** @todo Haiku: Port logged on user info retrieval. */
    rc = VERR_NOT_IMPLEMENTED;

#elif defined(RT_OS_OS2)
    /** @todo OS/2: Port logged on (LAN/local/whatever) user info retrieval. */
    rc = VERR_NOT_IMPLEMENTED;

#else
    setutxent();
    utmpx *ut_user;
    uint32_t cListSize = 32;

    /* Allocate a first array to hold 32 users max. */
    char **papszUsers = (char **)RTMemAllocZ(cListSize * sizeof(char *));
    if (!papszUsers)
        rc = VERR_NO_MEMORY;

    /* Process all entries in the utmp file.
     * Note: This only handles */
    while (   (ut_user = getutxent())
           && RT_SUCCESS(rc))
    {
# ifdef RT_OS_DARWIN /* No ut_user->ut_session on Darwin */
        VBoxServiceVerbose(4, "Found entry \"%s\" (type: %d, PID: %RU32)\n",
                           ut_user->ut_user, ut_user->ut_type, ut_user->ut_pid);
# else
        VBoxServiceVerbose(4, "Found entry \"%s\" (type: %d, PID: %RU32, session: %RU32)\n",
                           ut_user->ut_user, ut_user->ut_type, ut_user->ut_pid, ut_user->ut_session);
# endif
        if (cUsersInList > cListSize)
        {
            cListSize += 32;
            void *pvNew = RTMemRealloc(papszUsers, cListSize * sizeof(char*));
            AssertBreakStmt(pvNew, cListSize -= 32);
            papszUsers = (char **)pvNew;
        }

        /* Make sure we don't add user names which are not
         * part of type USER_PROCES. */
        if (ut_user->ut_type == USER_PROCESS) /* Regular user process. */
        {
            bool fFound = false;
            for (uint32_t i = 0; i < cUsersInList && !fFound; i++)
                fFound = strcmp(papszUsers[i], ut_user->ut_user) == 0;

            if (!fFound)
            {
                VBoxServiceVerbose(4, "Adding user \"%s\" (type: %d) to list\n",
                                   ut_user->ut_user, ut_user->ut_type);

                rc = RTStrDupEx(&papszUsers[cUsersInList], (const char *)ut_user->ut_user);
                if (RT_FAILURE(rc))
                    break;
                cUsersInList++;
            }
        }
    }

# ifdef VBOX_WITH_DBUS
#  if defined(RT_OS_LINUX) /* Not yet for Solaris/FreeBSB. */
    DBusError dbErr;
    DBusConnection *pConnection = NULL;
    int rc2 = RTDBusLoadLib();
    bool fHaveLibDbus = false;
    if (RT_SUCCESS(rc2))
    {
        /* Handle desktop sessions using ConsoleKit. */
        VBoxServiceVerbose(4, "Checking ConsoleKit sessions ...\n");
        fHaveLibDbus = true;
        dbus_error_init(&dbErr);
        pConnection = dbus_bus_get(DBUS_BUS_SYSTEM, &dbErr);
    }

    if (   pConnection
        && !dbus_error_is_set(&dbErr))
    {
        /* Get all available sessions. */
        DBusMessage *pMsgSessions = dbus_message_new_method_call("org.freedesktop.ConsoleKit",
                                                                 "/org/freedesktop/ConsoleKit/Manager",
                                                                 "org.freedesktop.ConsoleKit.Manager",
                                                                 "GetSessions");
        if (   pMsgSessions
            && (dbus_message_get_type(pMsgSessions) == DBUS_MESSAGE_TYPE_METHOD_CALL))
        {
            DBusMessage *pReplySessions = dbus_connection_send_with_reply_and_block(pConnection,
                                                                                    pMsgSessions, 30 * 1000 /* 30s timeout */,
                                                                                    &dbErr);
            if (   pReplySessions
                && !dbus_error_is_set(&dbErr))
            {
                char **ppszSessions; int cSessions;
                if (   (dbus_message_get_type(pMsgSessions) == DBUS_MESSAGE_TYPE_METHOD_CALL)
                    && vboxService_dbus_message_get_args(pReplySessions, &dbErr, DBUS_TYPE_ARRAY,
                                                         DBUS_TYPE_OBJECT_PATH, &ppszSessions, &cSessions,
                                                         DBUS_TYPE_INVALID /* Termination */))
                {
                    VBoxServiceVerbose(4, "ConsoleKit: retrieved %RU16 session(s)\n", cSessions);

                    char **ppszCurSession = ppszSessions;
                    for (ppszCurSession;
                         ppszCurSession && *ppszCurSession; ppszCurSession++)
                    {
                        VBoxServiceVerbose(4, "ConsoleKit: processing session '%s' ...\n", *ppszCurSession);

                        /* Only respect active sessions .*/
                        bool fActive = false;
                        DBusMessage *pMsgSessionActive = dbus_message_new_method_call("org.freedesktop.ConsoleKit",
                                                                                      *ppszCurSession,
                                                                                      "org.freedesktop.ConsoleKit.Session",
                                                                                      "IsActive");
                        if (   pMsgSessionActive
                            && dbus_message_get_type(pMsgSessionActive) == DBUS_MESSAGE_TYPE_METHOD_CALL)
                        {
                            DBusMessage *pReplySessionActive = dbus_connection_send_with_reply_and_block(pConnection,
                                                                                                         pMsgSessionActive, 30 * 1000 /* 30s timeout */,
                                                                                                         &dbErr);
                            if (   pReplySessionActive
                                && !dbus_error_is_set(&dbErr))
                            {
                                DBusMessageIter itMsg;
                                if (   dbus_message_iter_init(pReplySessionActive, &itMsg)
                                    && dbus_message_iter_get_arg_type(&itMsg) == DBUS_TYPE_BOOLEAN)
                                {
                                    /* Get uid from message. */
                                    int val;
                                    dbus_message_iter_get_basic(&itMsg, &val);
                                    fActive = val >= 1;
                                }

                                if (pReplySessionActive)
                                    dbus_message_unref(pReplySessionActive);
                            }

                            if (pMsgSessionActive)
                                dbus_message_unref(pMsgSessionActive);
                        }

                        VBoxServiceVerbose(4, "ConsoleKit: session '%s' is %s\n",
                                           *ppszCurSession, fActive ? "active" : "not active");

                        /* *ppszCurSession now contains the object path
                         * (e.g. "/org/freedesktop/ConsoleKit/Session1"). */
                        DBusMessage *pMsgUnixUser = dbus_message_new_method_call("org.freedesktop.ConsoleKit",
                                                                                 *ppszCurSession,
                                                                                 "org.freedesktop.ConsoleKit.Session",
                                                                                 "GetUnixUser");
                        if (   fActive
                            && pMsgUnixUser
                            && dbus_message_get_type(pMsgUnixUser) == DBUS_MESSAGE_TYPE_METHOD_CALL)
                        {
                            DBusMessage *pReplyUnixUser = dbus_connection_send_with_reply_and_block(pConnection,
                                                                                                    pMsgUnixUser, 30 * 1000 /* 30s timeout */,
                                                                                                    &dbErr);
                            if (   pReplyUnixUser
                                && !dbus_error_is_set(&dbErr))
                            {
                                DBusMessageIter itMsg;
                                if (   dbus_message_iter_init(pReplyUnixUser, &itMsg)
                                    && dbus_message_iter_get_arg_type(&itMsg) == DBUS_TYPE_UINT32)
                                {
                                    /* Get uid from message. */
                                    uint32_t uid;
                                    dbus_message_iter_get_basic(&itMsg, &uid);

                                    /** @todo Add support for getting UID_MIN (/etc/login.defs on
                                     *        Debian). */
                                    uint32_t uid_min = 1000;

                                    /* Look up user name (realname) from uid. */
                                    setpwent();
                                    struct passwd *ppwEntry = getpwuid(uid);
                                    if (   ppwEntry
                                        && ppwEntry->pw_name)
                                    {
                                        if (ppwEntry->pw_uid >= uid_min /* Only respect users, not daemons etc. */)
                                        {
                                            VBoxServiceVerbose(4, "ConsoleKit: session '%s' -> %s (uid: %RU32)\n",
                                                               *ppszCurSession, ppwEntry->pw_name, uid);

                                            bool fFound = false;
                                            for (uint32_t i = 0; i < cUsersInList && !fFound; i++)
                                                fFound = strcmp(papszUsers[i], ppwEntry->pw_name) == 0;

                                            if (!fFound)
                                            {
                                                VBoxServiceVerbose(4, "ConsoleKit: adding user \"%s\" to list\n",
                                                                   ppwEntry->pw_name);

                                                rc = RTStrDupEx(&papszUsers[cUsersInList], (const char *)ppwEntry->pw_name);
                                                if (RT_FAILURE(rc))
                                                    break;
                                                cUsersInList++;
                                            }
                                        }
                                        /* else silently ignore the user */
                                    }
                                    else
                                        VBoxServiceError("ConsoleKit: unable to lookup user name for uid=%RU32\n", uid);
                                }
                                else
                                    AssertMsgFailed(("ConsoleKit: GetUnixUser returned a wrong argument type\n"));
                            }

                            if (pReplyUnixUser)
                                dbus_message_unref(pReplyUnixUser);
                        }
                        else if (fActive) /* don't bitch about inactive users */
                        {
                            static int s_iBitchedAboutConsoleKit = 0;
                            if (s_iBitchedAboutConsoleKit < 1)
                            {
                                s_iBitchedAboutConsoleKit++;
                                VBoxServiceError("ConsoleKit: unable to retrieve user for session '%s' (msg type=%d): %s\n",
                                                 *ppszCurSession, dbus_message_get_type(pMsgUnixUser),
                                                 dbus_error_is_set(&dbErr) ? dbErr.message : "No error information available");
                            }
                        }

                        if (pMsgUnixUser)
                            dbus_message_unref(pMsgUnixUser);
                    }

                    dbus_free_string_array(ppszSessions);
                }
                else
                {
                    VBoxServiceError("ConsoleKit: unable to retrieve session parameters (msg type=%d): %s\n",
                                     dbus_message_get_type(pMsgSessions),
                                     dbus_error_is_set(&dbErr) ? dbErr.message : "No error information available");
                }
                dbus_message_unref(pReplySessions);
            }

            if (pMsgSessions)
            {
                dbus_message_unref(pMsgSessions);
                pMsgSessions = NULL;
            }
        }
        else
        {
            static int s_iBitchedAboutConsoleKit = 0;
            if (s_iBitchedAboutConsoleKit < 3)
            {
                s_iBitchedAboutConsoleKit++;
                VBoxServiceError("Unable to invoke ConsoleKit (%d/3) -- maybe not installed / used? Error: %s\n",
                                 s_iBitchedAboutConsoleKit,
                                 dbus_error_is_set(&dbErr) ? dbErr.message : "No error information available");
            }
        }

        if (pMsgSessions)
            dbus_message_unref(pMsgSessions);
    }
    else
    {
        static int s_iBitchedAboutDBus = 0;
        if (s_iBitchedAboutDBus < 3)
        {
            s_iBitchedAboutDBus++;
            VBoxServiceError("Unable to connect to system D-Bus (%d/3): %s\n", s_iBitchedAboutDBus,
                             fHaveLibDbus && dbus_error_is_set(&dbErr) ? dbErr.message : "D-Bus not installed");
        }
    }

    if (   fHaveLibDbus
        && dbus_error_is_set(&dbErr))
        dbus_error_free(&dbErr);
#  endif /* RT_OS_LINUX */
# endif /* VBOX_WITH_DBUS */

    /** @todo Fedora/others: Handle systemd-loginctl. */

    /* Calc the string length. */
    size_t cchUserList = 0;
    if (RT_SUCCESS(rc))
    {
        for (uint32_t i = 0; i < cUsersInList; i++)
            cchUserList += (i != 0) + strlen(papszUsers[i]);
    }

    /* Build the user list. */
    if (cchUserList > 0)
    {
        if (RT_SUCCESS(rc))
            rc = RTStrAllocEx(&pszUserList, cchUserList + 1);
        if (RT_SUCCESS(rc))
        {
            char *psz = pszUserList;
            for (uint32_t i = 0; i < cUsersInList; i++)
            {
                if (i != 0)
                    *psz++ = ',';
                size_t cch = strlen(papszUsers[i]);
                memcpy(psz, papszUsers[i], cch);
                psz += cch;
            }
            *psz = '\0';
        }
    }

    /* Cleanup. */
    for (uint32_t i = 0; i < cUsersInList; i++)
        RTStrFree(papszUsers[i]);
    RTMemFree(papszUsers);

    endutxent(); /* Close utmpx file. */
#endif /* !RT_OS_WINDOWS && !RT_OS_FREEBSD && !RT_OS_HAIKU && !RT_OS_OS2 */

    Assert(RT_FAILURE(rc) || cUsersInList == 0 || (pszUserList && *pszUserList));

    /* If the user enumeration above failed, reset the user count to 0 except
     * we didn't have enough memory anymore. In that case we want to preserve
     * the previous user count in order to not confuse third party tools which
     * rely on that count. */
    if (RT_FAILURE(rc))
    {
        if (rc == VERR_NO_MEMORY)
        {
            static int s_iVMInfoBitchedOOM = 0;
            if (s_iVMInfoBitchedOOM++ < 3)
                VBoxServiceVerbose(0, "Warning: Not enough memory available to enumerate users! Keeping old value (%RU32)\n",
                                   g_cVMInfoLoggedInUsers);
            cUsersInList = g_cVMInfoLoggedInUsers;
        }
        else
            cUsersInList = 0;
    }
    else /* Preserve logged in users count. */
        g_cVMInfoLoggedInUsers = cUsersInList;

    VBoxServiceVerbose(4, "cUsersInList=%RU32, pszUserList=%s, rc=%Rrc\n",
                       cUsersInList, pszUserList ? pszUserList : "<NULL>", rc);

    if (pszUserList)
    {
        AssertMsg(cUsersInList, ("pszUserList contains users whereas cUsersInList is 0\n"));
        rc = VBoxServicePropCacheUpdate(&g_VMInfoPropCache, g_pszPropCacheValLoggedInUsersList, "%s", pszUserList);
    }
    else
        rc = VBoxServicePropCacheUpdate(&g_VMInfoPropCache, g_pszPropCacheValLoggedInUsersList, NULL);
    if (RT_FAILURE(rc))
        VBoxServiceError("Error writing logged in users list, rc=%Rrc\n", rc);

    rc = VBoxServicePropCacheUpdate(&g_VMInfoPropCache, g_pszPropCacheValLoggedInUsers, "%RU32", cUsersInList);
    if (RT_FAILURE(rc))
        VBoxServiceError("Error writing logged in users count, rc=%Rrc\n", rc);

    rc = VBoxServicePropCacheUpdate(&g_VMInfoPropCache, g_pszPropCacheValNoLoggedInUsers,
                                    cUsersInList == 0 ? "true" : "false");
    if (RT_FAILURE(rc))
        VBoxServiceError("Error writing no logged in users beacon, rc=%Rrc\n", rc);

    if (pszUserList)
        RTStrFree(pszUserList);

    VBoxServiceVerbose(4, "Writing users returned with rc=%Rrc\n", rc);
    return rc;
}


/**
 * Provide information about the guest network.
 */
static int vboxserviceVMInfoWriteNetwork(void)
{
    int         rc = VINF_SUCCESS;
    uint32_t    cIfsReported = 0;
    char        szPropPath[256];

#ifdef RT_OS_WINDOWS
    IP_ADAPTER_INFO *pAdpInfo = NULL;

# ifndef TARGET_NT4
    ULONG cbAdpInfo = sizeof(*pAdpInfo);
    pAdpInfo = (IP_ADAPTER_INFO *)RTMemAlloc(cbAdpInfo);
    if (!pAdpInfo)
    {
        VBoxServiceError("VMInfo/Network: Failed to allocate IP_ADAPTER_INFO\n");
        return VERR_NO_MEMORY;
    }
    DWORD dwRet = GetAdaptersInfo(pAdpInfo, &cbAdpInfo);
    if (dwRet == ERROR_BUFFER_OVERFLOW)
    {
        IP_ADAPTER_INFO *pAdpInfoNew = (IP_ADAPTER_INFO*)RTMemRealloc(pAdpInfo, cbAdpInfo);
        if (pAdpInfoNew)
        {
            pAdpInfo = pAdpInfoNew;
            dwRet = GetAdaptersInfo(pAdpInfo, &cbAdpInfo);
        }
    }
    else if (dwRet == ERROR_NO_DATA)
    {
        VBoxServiceVerbose(3, "VMInfo/Network: No network adapters available\n");

        /* If no network adapters available / present in the
         * system we pretend success to not bail out too early. */
        dwRet = ERROR_SUCCESS;
    }

    if (dwRet != ERROR_SUCCESS)
    {
        if (pAdpInfo)
            RTMemFree(pAdpInfo);
        VBoxServiceError("VMInfo/Network: Failed to get adapter info: Error %d\n", dwRet);
        return RTErrConvertFromWin32(dwRet);
    }
# endif /* !TARGET_NT4 */

    SOCKET sd = WSASocket(AF_INET, SOCK_DGRAM, 0, 0, 0, 0);
    if (sd == SOCKET_ERROR) /* Socket invalid. */
    {
        int wsaErr = WSAGetLastError();
        /* Don't complain/bail out with an error if network stack is not up; can happen
         * on NT4 due to start up when not connected shares dialogs pop up. */
        if (WSAENETDOWN == wsaErr)
        {
            VBoxServiceVerbose(0, "VMInfo/Network: Network is not up yet.\n");
            wsaErr = VINF_SUCCESS;
        }
        else
            VBoxServiceError("VMInfo/Network: Failed to get a socket: Error %d\n", wsaErr);
        if (pAdpInfo)
            RTMemFree(pAdpInfo);
        return RTErrConvertFromWin32(wsaErr);
    }

    INTERFACE_INFO InterfaceList[20] = {0};
    unsigned long nBytesReturned = 0;
    if (WSAIoctl(sd,
                 SIO_GET_INTERFACE_LIST,
                 0,
                 0,
                 &InterfaceList,
                 sizeof(InterfaceList),
                 &nBytesReturned,
                 0,
                 0) ==  SOCKET_ERROR)
    {
        VBoxServiceError("VMInfo/Network: Failed to WSAIoctl() on socket: Error: %d\n", WSAGetLastError());
        if (pAdpInfo)
            RTMemFree(pAdpInfo);
        return RTErrConvertFromWin32(WSAGetLastError());
    }
    int cIfacesSystem = nBytesReturned / sizeof(INTERFACE_INFO);

    /** @todo Use GetAdaptersInfo() and GetAdapterAddresses (IPv4 + IPv6) for more information. */
    for (int i = 0; i < cIfacesSystem; ++i)
    {
        sockaddr_in *pAddress;
        u_long nFlags = 0;
        if (InterfaceList[i].iiFlags & IFF_LOOPBACK) /* Skip loopback device. */
            continue;
        nFlags = InterfaceList[i].iiFlags;
        pAddress = (sockaddr_in *)&(InterfaceList[i].iiAddress);
        Assert(pAddress);
        char szIp[32];
        RTStrPrintf(szIp, sizeof(szIp), "%s", inet_ntoa(pAddress->sin_addr));
        RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32/V4/IP", cIfsReported);
        VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", szIp);

        pAddress = (sockaddr_in *) & (InterfaceList[i].iiBroadcastAddress);
        RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32/V4/Broadcast", cIfsReported);
        VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", inet_ntoa(pAddress->sin_addr));

        pAddress = (sockaddr_in *)&(InterfaceList[i].iiNetmask);
        RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32/V4/Netmask", cIfsReported);
        VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", inet_ntoa(pAddress->sin_addr));

        RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32/Status", cIfsReported);
        VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, nFlags & IFF_UP ? "Up" : "Down");

# ifndef TARGET_NT4
        IP_ADAPTER_INFO *pAdp;
        for (pAdp = pAdpInfo; pAdp; pAdp = pAdp->Next)
            if (!strcmp(pAdp->IpAddressList.IpAddress.String, szIp))
                break;

        RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32/MAC", cIfsReported);
        if (pAdp)
        {
            char szMac[32];
            RTStrPrintf(szMac, sizeof(szMac), "%02X%02X%02X%02X%02X%02X",
                        pAdp->Address[0], pAdp->Address[1], pAdp->Address[2],
                        pAdp->Address[3], pAdp->Address[4], pAdp->Address[5]);
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", szMac);
        }
        else
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, NULL);
# endif /* !TARGET_NT4 */

        cIfsReported++;
    }
    if (pAdpInfo)
        RTMemFree(pAdpInfo);
    if (sd >= 0)
        closesocket(sd);

#elif defined(RT_OS_HAIKU)
    /** @todo Haiku: implement network info. retreival */
    return VERR_NOT_IMPLEMENTED;

#elif defined(RT_OS_DARWIN) || defined(RT_OS_FREEBSD)
    struct ifaddrs *pIfHead = NULL;

    /* Get all available interfaces */
    rc = getifaddrs(&pIfHead);
    if (rc < 0)
    {
        rc = RTErrConvertFromErrno(errno);
        VBoxServiceError("VMInfo/Network: Failed to get all interfaces: Error %Rrc\n");
        return rc;
    }

    /* Loop through all interfaces and set the data. */
    for (struct ifaddrs *pIfCurr = pIfHead; pIfCurr; pIfCurr = pIfCurr->ifa_next)
    {
        /*
         * Only AF_INET and no loopback interfaces
         */
        /** @todo: IPv6 interfaces */
        if (   pIfCurr->ifa_addr->sa_family == AF_INET
            && !(pIfCurr->ifa_flags & IFF_LOOPBACK))
        {
            char szInetAddr[NI_MAXHOST];

            memset(szInetAddr, 0, NI_MAXHOST);
            getnameinfo(pIfCurr->ifa_addr, sizeof(struct sockaddr_in),
                        szInetAddr, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32/V4/IP", cIfsReported);
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", szInetAddr);

            memset(szInetAddr, 0, NI_MAXHOST);
            getnameinfo(pIfCurr->ifa_broadaddr, sizeof(struct sockaddr_in),
                        szInetAddr, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32/V4/Broadcast", cIfsReported);
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", szInetAddr);

            memset(szInetAddr, 0, NI_MAXHOST);
            getnameinfo(pIfCurr->ifa_netmask, sizeof(struct sockaddr_in),
                        szInetAddr, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32/V4/Netmask", cIfsReported);
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", szInetAddr);

            /* Search for the AF_LINK interface of the current AF_INET one and get the mac. */
            for (struct ifaddrs *pIfLinkCurr = pIfHead; pIfLinkCurr; pIfLinkCurr = pIfLinkCurr->ifa_next)
            {
                if (   pIfLinkCurr->ifa_addr->sa_family == AF_LINK
                    && !strcmp(pIfCurr->ifa_name, pIfLinkCurr->ifa_name))
                {
                    char szMac[32];
                    uint8_t *pu8Mac = NULL;
                    struct sockaddr_dl *pLinkAddress = (struct sockaddr_dl *)pIfLinkCurr->ifa_addr;

                    AssertPtr(pLinkAddress);
                    pu8Mac = (uint8_t *)LLADDR(pLinkAddress);
                    RTStrPrintf(szMac, sizeof(szMac), "%02X%02X%02X%02X%02X%02X",
                                pu8Mac[0], pu8Mac[1], pu8Mac[2], pu8Mac[3],  pu8Mac[4], pu8Mac[5]);
                    RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32/MAC", cIfsReported);
                    VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", szMac);
                    break;
                }
            }

            RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32/Status", cIfsReported);
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, pIfCurr->ifa_flags & IFF_UP ? "Up" : "Down");

            cIfsReported++;
        }
    }

    /* Free allocated resources. */
    freeifaddrs(pIfHead);

#else /* !RT_OS_WINDOWS && !RT_OS_FREEBSD */
    /*
     * Use SIOCGIFCONF to get a list of interface/protocol configurations.
     *
     * See "UNIX Network Programming Volume 1" by W. R. Stevens, section 17.6
     * for details on this ioctl.
     */
    int sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd < 0)
    {
        rc = RTErrConvertFromErrno(errno);
        VBoxServiceError("VMInfo/Network: Failed to get a socket: Error %Rrc\n", rc);
        return rc;
    }

    /* Call SIOCGIFCONF with the right sized buffer (remember the size). */
    static int      s_cbBuf = 256; // 1024
    int             cbBuf   = s_cbBuf;
    char           *pchBuf;
    struct ifconf   IfConf;
    rc = VINF_SUCCESS;
    for (;;)
    {
        pchBuf = (char *)RTMemTmpAllocZ(cbBuf);
        if (!pchBuf)
        {
            rc = VERR_NO_TMP_MEMORY;
            break;
        }

        IfConf.ifc_len = cbBuf;
        IfConf.ifc_buf = pchBuf;
        if (ioctl(sd, SIOCGIFCONF, &IfConf) >= 0)
        {
            /* Hard to anticipate how space an address might possibly take, so
               making some generous assumptions here to avoid performing the
               query twice with different buffer sizes. */
            if (IfConf.ifc_len + 128 < cbBuf)
                break;
        }
        else if (errno != EOVERFLOW)
        {
            rc = RTErrConvertFromErrno(errno);
            break;
        }

        /* grow the buffer */
        s_cbBuf = cbBuf *= 2;
        RTMemFree(pchBuf);
    }
    if (RT_FAILURE(rc))
    {
        close(sd);
        RTMemTmpFree(pchBuf);
        VBoxServiceError("VMInfo/Network: Error doing SIOCGIFCONF (cbBuf=%d): %Rrc\n", cbBuf, rc);
        return rc;
    }

    /*
     * Iterate the interface/protocol configurations.
     *
     * Note! The current code naively assumes one IPv4 address per interface.
     *       This means that guest assigning more than one address to an
     *       interface will get multiple entries for one physical interface.
     */
# ifdef RT_OS_OS2
    struct ifreq   *pPrevLinkAddr = NULL;
# endif
    struct ifreq   *pCur   = IfConf.ifc_req;
    size_t          cbLeft = IfConf.ifc_len;
    while (cbLeft >= sizeof(*pCur))
    {
# if defined(RT_OS_SOLARIS) || defined(RT_OS_LINUX)
        /* These two do not provide the sa_len member but only support address
         * families which do not need extra bytes on the end. */
#  define SA_LEN(pAddr) sizeof(struct sockaddr)
# elif !defined(SA_LEN)
#  define SA_LEN(pAddr) (pAddr)->sa_len
# endif
        /* Figure the size of the current request. */
        size_t cbCur = RT_OFFSETOF(struct ifreq, ifr_addr)
                     + SA_LEN(&pCur->ifr_addr);
        cbCur = RT_MAX(cbCur, sizeof(struct ifreq));
# if defined(RT_OS_SOLARIS) || defined(RT_OS_LINUX)
        Assert(pCur->ifr_addr.sa_family == AF_INET);
# endif
        AssertBreak(cbCur <= cbLeft);

# ifdef RT_OS_OS2
        /* On OS/2 we get the MAC address in the AF_LINK that the BSD 4.4 stack
           emits.  We boldly ASSUME these always comes first. */
        if (   pCur->ifr_addr.sa_family == AF_LINK
            && ((struct sockaddr_dl *)&pCur->ifr_addr)->sdl_alen == 6)
            pPrevLinkAddr = pCur;
# endif

        /* Skip it if it's not the kind of address we're looking for. */
        struct ifreq IfReqTmp;
        bool         fIfUp = false;
        bool         fSkip = false;
        if (pCur->ifr_addr.sa_family != AF_INET)
            fSkip = true;
        else
        {
            /* Get the interface flags so we can detect loopback and check if it's up. */
            IfReqTmp = *pCur;
            if (ioctl(sd, SIOCGIFFLAGS, &IfReqTmp) < 0)
            {
                rc = RTErrConvertFromErrno(errno);
                VBoxServiceError("VMInfo/Network: Failed to ioctl(SIOCGIFFLAGS,%s) on socket: Error %Rrc\n", pCur->ifr_name, rc);
                break;
            }
            fIfUp = !!(IfReqTmp.ifr_flags & IFF_UP);
            if (IfReqTmp.ifr_flags & IFF_LOOPBACK) /* Skip the loopback device. */
                fSkip = true;
        }
        if (!fSkip)
        {
            size_t offSubProp = RTStrPrintf(szPropPath, sizeof(szPropPath), "/VirtualBox/GuestInfo/Net/%RU32", cIfsReported);

            sockaddr_in *pAddress = (sockaddr_in *)&pCur->ifr_addr;
            strcpy(&szPropPath[offSubProp], "/V4/IP");
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", inet_ntoa(pAddress->sin_addr));

            /* Get the broadcast address. */
            IfReqTmp = *pCur;
            if (ioctl(sd, SIOCGIFBRDADDR, &IfReqTmp) < 0)
            {
                rc = RTErrConvertFromErrno(errno);
                VBoxServiceError("VMInfo/Network: Failed to ioctl(SIOCGIFBRDADDR) on socket: Error %Rrc\n", rc);
                break;
            }
            pAddress = (sockaddr_in *)&IfReqTmp.ifr_broadaddr;
            strcpy(&szPropPath[offSubProp], "/V4/Broadcast");
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", inet_ntoa(pAddress->sin_addr));

            /* Get the net mask. */
            IfReqTmp = *pCur;
            if (ioctl(sd, SIOCGIFNETMASK, &IfReqTmp) < 0)
            {
                rc = RTErrConvertFromErrno(errno);
                VBoxServiceError("VMInfo/Network: Failed to ioctl(SIOCGIFNETMASK) on socket: Error %Rrc\n", rc);
                break;
            }
# if defined(RT_OS_OS2) || defined(RT_OS_SOLARIS)
            pAddress = (sockaddr_in *)&IfReqTmp.ifr_addr;
# else
            pAddress = (sockaddr_in *)&IfReqTmp.ifr_netmask;
# endif
            strcpy(&szPropPath[offSubProp], "/V4/Netmask");
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%s", inet_ntoa(pAddress->sin_addr));

# if defined(RT_OS_SOLARIS)
            /*
             * "ifreq" is obsolete on Solaris. We use the recommended "lifreq".
             * We might fail if the interface has not been assigned an IP address.
             * That doesn't matter; as long as it's plumbed we can pick it up.
             * But, if it has not acquired an IP address we cannot obtain it's MAC
             * address this way, so we just use all zeros there.
             */
            RTMAC           IfMac;
            struct lifreq   IfReq;
            RT_ZERO(IfReq);
            AssertCompile(sizeof(IfReq.lifr_name) >= sizeof(pCur->ifr_name));
            strncpy(IfReq.lifr_name, pCur->ifr_name, sizeof(pCur->ifr_name));
            if (ioctl(sd, SIOCGLIFADDR, &IfReq) >= 0)
            {
                struct arpreq ArpReq;
                RT_ZERO(ArpReq);
                memcpy(&ArpReq.arp_pa, &IfReq.lifr_addr, sizeof(struct sockaddr_in));

                if (ioctl(sd, SIOCGARP, &ArpReq) >= 0)
                    memcpy(&IfMac, ArpReq.arp_ha.sa_data, sizeof(IfMac));
                else
                {
                    rc = RTErrConvertFromErrno(errno);
                    VBoxServiceError("VMInfo/Network: failed to ioctl(SIOCGARP) on socket: Error %Rrc\n", rc);
                    break;
                }
            }
            else
            {
                VBoxServiceVerbose(2, "VMInfo/Network: Interface \"%s\" has no assigned IP address, skipping ...\n", pCur->ifr_name);
                continue;
            }
# elif defined(RT_OS_OS2)
            RTMAC   IfMac;
            if (   pPrevLinkAddr
                && strncmp(pCur->ifr_name, pPrevLinkAddr->ifr_name, sizeof(pCur->ifr_name)) == 0)
            {
                struct sockaddr_dl *pDlAddr = (struct sockaddr_dl *)&pPrevLinkAddr->ifr_addr;
                IfMac = *(PRTMAC)&pDlAddr->sdl_data[pDlAddr->sdl_nlen];
            }
            else
                RT_ZERO(IfMac);
#else
            if (ioctl(sd, SIOCGIFHWADDR, &IfReqTmp) < 0)
            {
                rc = RTErrConvertFromErrno(errno);
                VBoxServiceError("VMInfo/Network: Failed to ioctl(SIOCGIFHWADDR) on socket: Error %Rrc\n", rc);
                break;
            }
            RTMAC IfMac = *(PRTMAC)&IfReqTmp.ifr_hwaddr.sa_data[0];
# endif
            strcpy(&szPropPath[offSubProp], "/MAC");
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%02X%02X%02X%02X%02X%02X",
                                       IfMac.au8[0], IfMac.au8[1], IfMac.au8[2], IfMac.au8[3], IfMac.au8[4], IfMac.au8[5]);

            strcpy(&szPropPath[offSubProp], "/Status");
            VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, fIfUp ? "Up" : "Down");

            /* The name. */
            int rc2 = RTStrValidateEncodingEx(pCur->ifr_name, sizeof(pCur->ifr_name), 0);
            if (RT_SUCCESS(rc2))
            {
                strcpy(&szPropPath[offSubProp], "/Name");
                VBoxServicePropCacheUpdate(&g_VMInfoPropCache, szPropPath, "%.*s", sizeof(pCur->ifr_name), pCur->ifr_name);
            }

            cIfsReported++;
        }

        /*
         * Next interface/protocol configuration.
         */
        pCur = (struct ifreq *)((uintptr_t)pCur + cbCur);
        cbLeft -= cbCur;
    }

    RTMemTmpFree(pchBuf);
    close(sd);
    if (RT_FAILURE(rc))
        VBoxServiceError("VMInfo/Network: Network enumeration for interface %RU32 failed with error %Rrc\n", cIfsReported, rc);

#endif /* !RT_OS_WINDOWS */

#if 0 /* Zapping not enabled yet, needs more testing first. */
    /*
     * Zap all stale network interface data if the former (saved) network ifaces count
     * is bigger than the current one.
     */

    /* Get former count. */
    uint32_t cIfsReportedOld;
    rc = VBoxServiceReadPropUInt32(g_uVMInfoGuestPropSvcClientID, g_pszPropCacheValNetCount, &cIfsReportedOld,
                                   0 /* Min */, UINT32_MAX /* Max */);
    if (   RT_SUCCESS(rc)
        && cIfsReportedOld > cIfsReported) /* Are some ifaces not around anymore? */
    {
        VBoxServiceVerbose(3, "VMInfo/Network: Stale interface data detected (%RU32 old vs. %RU32 current)\n",
                           cIfsReportedOld, cIfsReported);

        uint32_t uIfaceDeleteIdx = cIfsReported;
        do
        {
            VBoxServiceVerbose(3, "VMInfo/Network: Deleting stale data of interface %d ...\n", uIfaceDeleteIdx);
            rc = VBoxServicePropCacheUpdateByPath(&g_VMInfoPropCache, NULL /* Value, delete */, 0 /* Flags */, "/VirtualBox/GuestInfo/Net/%RU32", uIfaceDeleteIdx++);
        } while (RT_SUCCESS(rc));
    }
    else if (   RT_FAILURE(rc)
             && rc != VERR_NOT_FOUND)
    {
        VBoxServiceError("VMInfo/Network: Failed retrieving old network interfaces count with error %Rrc\n", rc);
    }
#endif

    /*
     * This property is a beacon which is _always_ written, even if the network configuration
     * does not change. If this property is missing, the host assumes that all other GuestInfo
     * properties are no longer valid.
     */
    VBoxServicePropCacheUpdate(&g_VMInfoPropCache, g_pszPropCacheValNetCount, "%RU32",
                               cIfsReported);

    /* Don't fail here; just report everything we got. */
    return VINF_SUCCESS;
}


/** @copydoc VBOXSERVICE::pfnWorker */
DECLCALLBACK(int) VBoxServiceVMInfoWorker(bool volatile *pfShutdown)
{
    int rc;

    /*
     * Tell the control thread that it can continue
     * spawning services.
     */
    RTThreadUserSignal(RTThreadSelf());

#ifdef RT_OS_WINDOWS
    /* Required for network information (must be called per thread). */
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData))
        VBoxServiceError("VMInfo/Network: WSAStartup failed! Error: %Rrc\n", RTErrConvertFromWin32(WSAGetLastError()));
#endif /* RT_OS_WINDOWS */

    /*
     * Write the fixed properties first.
     */
    vboxserviceVMInfoWriteFixedProperties();

    /*
     * Now enter the loop retrieving runtime data continuously.
     */
    for (;;)
    {
        rc = vboxserviceVMInfoWriteUsers();
        if (RT_FAILURE(rc))
            break;

        rc = vboxserviceVMInfoWriteNetwork();
        if (RT_FAILURE(rc))
            break;

        /* Whether to wait for event semaphore or not. */
        bool fWait = true;

        /* Check for location awareness. This most likely only
         * works with VBox (latest) 4.1 and up. */

        /* Check for new connection. */
        char *pszLAClientID = NULL;
        int rc2 = VBoxServiceReadHostProp(g_uVMInfoGuestPropSvcClientID, g_pszLAActiveClient, true /* Read only */,
                                          &pszLAClientID, NULL /* Flags */, NULL /* Timestamp */);
        if (RT_SUCCESS(rc2))
        {
            AssertPtr(pszLAClientID);
            if (RTStrICmp(pszLAClientID, "0")) /* Is a client connected? */
            {
                uint32_t uLAClientID = RTStrToInt32(pszLAClientID);
                uint64_t uLAClientAttachedTS;

                /* Peek at "Attach" value to figure out if hotdesking happened. */
                char *pszAttach = NULL;
                rc2 = vboxServiceGetLAClientValue(uLAClientID, "Attach", &pszAttach,
                                                 &uLAClientAttachedTS);

                if (   RT_SUCCESS(rc2)
                    && (   !g_LAClientAttachedTS
                        || (g_LAClientAttachedTS != uLAClientAttachedTS)))
                {
                    vboxServiceFreeLAClientInfo(&g_LAClientInfo);

                    /* Note: There is a race between setting the guest properties by the host and getting them by
                     *       the guest. */
                    rc2 = vboxServiceGetLAClientInfo(uLAClientID, &g_LAClientInfo);
                    if (RT_SUCCESS(rc2))
                    {
                        VBoxServiceVerbose(1, "VRDP: Hotdesk client %s with ID=%RU32, Name=%s, Domain=%s\n",
                                           /* If g_LAClientAttachedTS is 0 this means there already was an active
                                            * hotdesk session when VBoxService started. */
                                           !g_LAClientAttachedTS ? "already active" : g_LAClientInfo.fAttached ? "connected" : "disconnected",
                                           uLAClientID, g_LAClientInfo.pszName, g_LAClientInfo.pszDomain);

                        g_LAClientAttachedTS = g_LAClientInfo.uAttachedTS;

                        /* Don't wait for event semaphore below anymore because we now know that the client
                         * changed. This means we need to iterate all VM information again immediately. */
                        fWait = false;
                    }
                    else
                    {
                        static int s_iBitchedAboutLAClientInfo = 0;
                        if (s_iBitchedAboutLAClientInfo < 10)
                        {
                            s_iBitchedAboutLAClientInfo++;
                            VBoxServiceError("Error getting active location awareness client info, rc=%Rrc\n", rc2);
                        }
                    }
                }
                else if (RT_FAILURE(rc2))
                     VBoxServiceError("Error getting attached value of location awareness client %RU32, rc=%Rrc\n",
                                      uLAClientID, rc2);
                if (pszAttach)
                    RTStrFree(pszAttach);
            }
            else
            {
                VBoxServiceVerbose(1, "VRDP: UTTSC disconnected from VRDP server\n");
                vboxServiceFreeLAClientInfo(&g_LAClientInfo);
            }

            RTStrFree(pszLAClientID);
        }
        else
        {
            static int s_iBitchedAboutLAClient = 0;
            if (   (rc2 != VERR_NOT_FOUND) /* No location awareness installed, skip. */
                && s_iBitchedAboutLAClient < 3)
            {
                s_iBitchedAboutLAClient++;
                VBoxServiceError("VRDP: Querying connected location awareness client failed with rc=%Rrc\n", rc2);
            }
        }

        VBoxServiceVerbose(3, "VRDP: Handling location awareness done\n");

        /*
         * Flush all properties if we were restored.
         */
        uint64_t idNewSession = g_idVMInfoSession;
        VbglR3GetSessionId(&idNewSession);
        if (idNewSession != g_idVMInfoSession)
        {
            VBoxServiceVerbose(3, "The VM session ID changed, flushing all properties\n");
            vboxserviceVMInfoWriteFixedProperties();
            VBoxServicePropCacheFlush(&g_VMInfoPropCache);
            g_idVMInfoSession = idNewSession;
        }

        /*
         * Block for a while.
         *
         * The event semaphore takes care of ignoring interruptions and it
         * allows us to implement service wakeup later.
         */
        if (*pfShutdown)
            break;
        if (fWait)
            rc2 = RTSemEventMultiWait(g_hVMInfoEvent, g_cMsVMInfoInterval);
        if (*pfShutdown)
            break;
        if (rc2 != VERR_TIMEOUT && RT_FAILURE(rc2))
        {
            VBoxServiceError("RTSemEventMultiWait failed; rc2=%Rrc\n", rc2);
            rc = rc2;
            break;
        }
        else if (RT_LIKELY(RT_SUCCESS(rc2)))
        {
            /* Reset event semaphore if it got triggered. */
            rc2 = RTSemEventMultiReset(g_hVMInfoEvent);
            if (RT_FAILURE(rc2))
                rc2 = VBoxServiceError("RTSemEventMultiReset failed; rc2=%Rrc\n", rc2);
        }
    }

#ifdef RT_OS_WINDOWS
    WSACleanup();
#endif

    return rc;
}


/** @copydoc VBOXSERVICE::pfnStop */
static DECLCALLBACK(void) VBoxServiceVMInfoStop(void)
{
    RTSemEventMultiSignal(g_hVMInfoEvent);
}


/** @copydoc VBOXSERVICE::pfnTerm */
static DECLCALLBACK(void) VBoxServiceVMInfoTerm(void)
{
    if (g_hVMInfoEvent != NIL_RTSEMEVENTMULTI)
    {
        /** @todo temporary solution: Zap all values which are not valid
         *        anymore when VM goes down (reboot/shutdown ). Needs to
         *        be replaced with "temporary properties" later.
         *
         *        One idea is to introduce a (HGCM-)session guest property
         *        flag meaning that a guest property is only valid as long
         *        as the HGCM session isn't closed (e.g. guest application
         *        terminates). [don't remove till implemented]
         */
        /** @todo r=bird: Drop the VbglR3GuestPropDelSet call here and use the cache
         *        since it remembers what we've written. */
        /* Delete the "../Net" branch. */
        const char *apszPat[1] = { "/VirtualBox/GuestInfo/Net/*" };
        int rc = VbglR3GuestPropDelSet(g_uVMInfoGuestPropSvcClientID, &apszPat[0], RT_ELEMENTS(apszPat));

        /* Destroy LA client info. */
        vboxServiceFreeLAClientInfo(&g_LAClientInfo);

        /* Destroy property cache. */
        VBoxServicePropCacheDestroy(&g_VMInfoPropCache);

        /* Disconnect from guest properties service. */
        rc = VbglR3GuestPropDisconnect(g_uVMInfoGuestPropSvcClientID);
        if (RT_FAILURE(rc))
            VBoxServiceError("Failed to disconnect from guest property service! Error: %Rrc\n", rc);
        g_uVMInfoGuestPropSvcClientID = 0;

        RTSemEventMultiDestroy(g_hVMInfoEvent);
        g_hVMInfoEvent = NIL_RTSEMEVENTMULTI;
    }
}


/**
 * The 'vminfo' service description.
 */
VBOXSERVICE g_VMInfo =
{
    /* pszName. */
    "vminfo",
    /* pszDescription. */
    "Virtual Machine Information",
    /* pszUsage. */
    "              [--vminfo-interval <ms>] [--vminfo-user-idle-threshold <ms>]"
    ,
    /* pszOptions. */
    "    --vminfo-interval       Specifies the interval at which to retrieve the\n"
    "                            VM information. The default is 10000 ms.\n"
    "    --vminfo-user-idle-threshold <ms>\n"
    "                            Specifies the user idle threshold (in ms) for\n"
    "                            considering a guest user as being idle. The default\n"
    "                            is 5000 (5 seconds).\n"
    ,
    /* methods */
    VBoxServiceVMInfoPreInit,
    VBoxServiceVMInfoOption,
    VBoxServiceVMInfoInit,
    VBoxServiceVMInfoWorker,
    VBoxServiceVMInfoStop,
    VBoxServiceVMInfoTerm
};

