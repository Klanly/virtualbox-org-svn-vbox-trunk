/* $Id$ */
/** @file
 * Drag and Drop manager: Handling of DnD messages on the host side.
 */

/*
 * Copyright (C) 2011-2015 Oracle Corporation
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

#ifdef LOG_GROUP
 #undef LOG_GROUP
#endif
#define LOG_GROUP LOG_GROUP_GUEST_DND

#include "dndmanager.h"

#include <VBox/log.h>
#include <iprt/file.h>
#include <iprt/dir.h>
#include <iprt/path.h>
#include <iprt/uri.h>


/*********************************************************************************************************************************
*   DnDManager                                                                                                                   *
*********************************************************************************************************************************/

int DnDManager::addMessage(uint32_t uMsg, uint32_t cParms, VBOXHGCMSVCPARM paParms[], bool fAppend /* = true */)
{
    int rc = VINF_SUCCESS;

    LogFlowFunc(("uMsg=%RU32, cParms=%RU32, fAppend=%RTbool\n", uMsg, cParms, fAppend));

    try
    {
        DnDMessage *pMessage = NULL;

        switch (uMsg)
        {
            case DragAndDropSvc::HOST_DND_HG_EVT_ENTER:
            {
                clear();
                LogFlowFunc(("HOST_DND_HG_EVT_ENTER\n"));
                break;
            }

            case DragAndDropSvc::HOST_DND_HG_EVT_MOVE:
            {
                LogFlowFunc(("HOST_DND_HG_EVT_MOVE\n"));
                break;
            }

            case DragAndDropSvc::HOST_DND_HG_EVT_LEAVE:
            {
                LogFlowFunc(("HOST_DND_HG_EVT_LEAVE\n"));
                break;
            }

            case DragAndDropSvc::HOST_DND_HG_EVT_DROPPED:
            {
                LogFlowFunc(("HOST_DND_HG_EVT_DROPPED\n"));
                break;
            }

            case DragAndDropSvc::HOST_DND_HG_EVT_CANCEL:
            {
                LogFlowFunc(("HOST_DND_HG_EVT_CANCEL\n"));

                pMessage = new DnDHGCancelMessage();
                break;
            }

            case DragAndDropSvc::HOST_DND_HG_SND_DATA:
            {
                LogFlowFunc(("HOST_DND_HG_SND_DATA\n"));
                break;
            }

            case DragAndDropSvc::HOST_DND_HG_SND_DIR:
            {
                LogFlowFunc(("HOST_DND_HG_SND_DIR\n"));
                break;
            }

            /* New since protocol version 2 (VBox 5.0). */
            case DragAndDropSvc::HOST_DND_HG_SND_FILE_HDR:
            {
                LogFlowFunc(("HOST_DND_HG_SND_FILE_HDR\n"));
                break;
            }

            case DragAndDropSvc::HOST_DND_HG_SND_FILE_DATA:
            {
                LogFlowFunc(("HOST_DND_HG_SND_FILE\n"));

                /* No parameter verification here as, depending on the protocol version
                 * being used, the parameter count + types might change. */
                break;
            }

#ifdef VBOX_WITH_DRAG_AND_DROP_GH
            case DragAndDropSvc::HOST_DND_GH_REQ_PENDING:
            {
                LogFlowFunc(("HOST_DND_GH_REQ_PENDING\n"));

                /* Verify parameter count and types. */
                if (   cParms != 1
                    || paParms[0].type != VBOX_HGCM_SVC_PARM_32BIT /* screen id */)
                {
                    rc = VERR_INVALID_PARAMETER;
                }
                break;
            }

            case DragAndDropSvc::HOST_DND_GH_EVT_DROPPED:
            {
                LogFlowFunc(("HOST_DND_GH_EVT_DROPPED\n"));

                /* Verify parameter count and types. */
                if (   cParms != 3
                    || paParms[0].type != VBOX_HGCM_SVC_PARM_PTR   /* format */
                    || paParms[1].type != VBOX_HGCM_SVC_PARM_32BIT /* format size */
                    || paParms[2].type != VBOX_HGCM_SVC_PARM_32BIT /* action */)
                {
                    rc = VERR_INVALID_PARAMETER;
                }
                break;
            }
#endif /* VBOX_WITH_DRAG_AND_DROP_GH */

            default:
                rc = VERR_NOT_IMPLEMENTED;
                break;
        }

        if (!pMessage) /* Generic message needed? */
            pMessage = new DnDGenericMessage(uMsg, cParms, paParms);

        if (fAppend)
            m_dndMessageQueue.append(pMessage);
        else
            m_dndMessageQueue.prepend(pMessage);
    }
    catch(std::bad_alloc &)
    {
        rc = VERR_NO_MEMORY;
    }

    return rc;
}

HGCM::Message* DnDManager::nextHGCMMessage(void)
{
    if (m_pCurMsg)
        return m_pCurMsg->nextHGCMMessage();

    if (m_dndMessageQueue.isEmpty())
        return NULL;

    return m_dndMessageQueue.first()->nextHGCMMessage();
}

int DnDManager::nextMessageInfo(uint32_t *puMsg, uint32_t *pcParms)
{
    AssertPtrReturn(puMsg, VERR_INVALID_POINTER);
    AssertPtrReturn(pcParms, VERR_INVALID_POINTER);

    int rc;
    if (m_pCurMsg)
        rc = m_pCurMsg->currentMessageInfo(puMsg, pcParms);
    else
    {
        if (m_dndMessageQueue.isEmpty())
            rc = VERR_NO_DATA;
        else
            rc = m_dndMessageQueue.first()->currentMessageInfo(puMsg, pcParms);
    }

    LogFlowFunc(("Returning puMsg=%RU32, pcParms=%RU32, rc=%Rrc\n", *puMsg, *pcParms, rc));
    return rc;
}

int DnDManager::nextMessage(uint32_t uMsg, uint32_t cParms, VBOXHGCMSVCPARM paParms[])
{
    LogFlowFunc(("uMsg=%RU32, cParms=%RU32\n", uMsg, cParms));

    if (!m_pCurMsg)
    {
        /* Check for pending messages in our queue. */
        if (m_dndMessageQueue.isEmpty())
            return VERR_NO_DATA;

        m_pCurMsg = m_dndMessageQueue.first();
        AssertPtr(m_pCurMsg);
        m_dndMessageQueue.removeFirst();
    }

    /* Fetch the current message info */
    int rc = m_pCurMsg->currentMessage(uMsg, cParms, paParms);
    /* If this message doesn't provide any additional sub messages, clear it. */
    if (!m_pCurMsg->isMessageWaiting())
    {
        delete m_pCurMsg;
        m_pCurMsg = NULL;
    }

    /*
     * If there was an error handling the current message or the user has canceled
     * the operation, we need to cleanup all pending events and inform the progress
     * callback about our exit.
     */
    if (RT_FAILURE(rc))
    {
        /* Clear any pending messages. */
        clear();

        /* Create a new cancel message to inform the guest + call
         * the host whether the current transfer was canceled or aborted
         * due to an error. */
        try
        {
            if (rc == VERR_CANCELLED)
                LogFlowFunc(("Operation was cancelled\n"));

            Assert(!m_pCurMsg);
            m_pCurMsg = new DnDHGCancelMessage();

            if (m_pfnProgressCallback)
            {
                LogFlowFunc(("Notifying host about aborting operation (%Rrc) ...\n", rc));
                m_pfnProgressCallback(  rc == VERR_CANCELLED
                                      ? DragAndDropSvc::DND_PROGRESS_CANCELLED
                                      : DragAndDropSvc::DND_PROGRESS_ERROR,
                                      100 /* Percent */, rc,
                                      m_pvProgressUser);
            }
        }
        catch(std::bad_alloc &)
        {
            rc = VERR_NO_MEMORY;
        }
    }

    LogFlowFunc(("Message processed with rc=%Rrc\n", rc));
    return rc;
}

void DnDManager::clear(void)
{
    if (m_pCurMsg)
    {
        delete m_pCurMsg;
        m_pCurMsg = NULL;
    }

    while (!m_dndMessageQueue.isEmpty())
    {
        delete m_dndMessageQueue.last();
        m_dndMessageQueue.removeLast();
    }
}

/**
 * Triggers a rescheduling of the manager's message queue by setting the first
 * message available in the queue as the current one to process.
 *
 * @return  IPRT status code. VERR_NO_DATA if not message to process is available at
 *          the time of calling.
 */
int DnDManager::doReschedule(void)
{
    LogFlowFunc(("Rescheduling ...\n"));

    if (!m_dndMessageQueue.isEmpty())
    {
        m_pCurMsg = m_dndMessageQueue.first();
        m_dndMessageQueue.removeFirst();

        return VINF_SUCCESS;
    }

    return VERR_NO_DATA;
}

