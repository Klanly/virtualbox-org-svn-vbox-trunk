/* $Id$ */
/** @file
 * VBox Qt GUI - UIVMInfoDialog class implementation.
 */

/*
 * Copyright (C) 2006-2014 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifdef VBOX_WITH_PRECOMPILED_HEADERS
# include "precomp.h"
#else  /* !VBOX_WITH_PRECOMPILED_HEADERS */

/* Qt includes: */
#include <QTimer>
#include <QScrollBar>
#include <QPushButton>

/* GUI includes: */
#include "UIVMInfoDialog.h"
#include "UISession.h"
#include "UIMachineLogic.h"
#include "UIMachineWindow.h"
#include "UIMachineView.h"
#include "UIConverter.h"
#include "UIIconPool.h"
#include "VBoxGlobal.h"
#include "QITabWidget.h"
#include "VBoxUtils.h"
#include "QIDialogButtonBox.h"

/* COM includes: */
#include "COMEnums.h"
#include "CMachine.h"
#include "CConsole.h"
#include "CSystemProperties.h"
#include "CMachineDebugger.h"
#include "CDisplay.h"
#include "CGuest.h"
#include "CStorageController.h"
#include "CMediumAttachment.h"
#include "CNetworkAdapter.h"
#include "CVRDEServerInfo.h"

/* Other VBox includes: */
#include <iprt/time.h>

#endif /* !VBOX_WITH_PRECOMPILED_HEADERS */

/* static */
UIVMInfoDialog* UIVMInfoDialog::m_spInstance = 0;

void UIVMInfoDialog::invoke(UIMachineWindow *pMachineWindow)
{
    /* Make sure dialog instance exists: */
    if (!m_spInstance)
    {
        /* Create new dialog instance if it doesn't exists yet: */
        new UIVMInfoDialog(pMachineWindow);
    }

    /* Show dialog: */
    m_spInstance->show();
    /* Raise it: */
    m_spInstance->raise();
    /* De-miniaturize if necessary: */
    m_spInstance->setWindowState(m_spInstance->windowState() & ~Qt::WindowMinimized);
    /* And activate finally: */
    m_spInstance->activateWindow();
}

UIVMInfoDialog::UIVMInfoDialog(UIMachineWindow *pMachineWindow)
    : QIWithRetranslateUI<QMainWindow>(0)
    , m_pMachineWindow(pMachineWindow)
    , m_fIsPolished(false)
    , m_iWidth(0), m_iHeight(0), m_fMax(false)
    , m_pTabWidget(0)
    , m_session(pMachineWindow->session())
    , m_pTimer(new QTimer(this))
{
    /* Initialize instance: */
    m_spInstance = this;

    /* Prepare: */
    prepare();
}

UIVMInfoDialog::~UIVMInfoDialog()
{
    /* Cleanup: */
    cleanup();

    /* Deinitialize instance: */
    m_spInstance = 0;
}

void UIVMInfoDialog::retranslateUi()
{
    sltUpdateDetails();

    AssertReturnVoid(!m_session.isNull());
    CMachine machine = m_session.GetMachine();
    AssertReturnVoid(!machine.isNull());

    /* Setup dialog title: */
    setWindowTitle(tr("%1 - Session Information").arg(machine.GetName()));

    /* Translate tabs: */
    m_pTabWidget->setTabText(0, tr("Configuration &Details"));
    m_pTabWidget->setTabText(1, tr("&Runtime Information"));

    /* Clear counter names initially: */
    m_names.clear();
    m_units.clear();
    m_links.clear();

    /* Storage statistics: */
    CSystemProperties sp = vboxGlobal().virtualBox().GetSystemProperties();
    CStorageControllerVector controllers = m_session.GetMachine().GetStorageControllers();
    int iIDECount = 0, iSATACount = 0, iSCSICount = 0;
    foreach (const CStorageController &controller, controllers)
    {
        switch (controller.GetBus())
        {
            case KStorageBus_IDE:
            {
                for (ULONG i = 0; i < sp.GetMaxPortCountForStorageBus(KStorageBus_IDE); ++i)
                {
                    for (ULONG j = 0; j < sp.GetMaxDevicesPerPortForStorageBus(KStorageBus_IDE); ++j)
                    {
                        /* Names: */
                        m_names[QString("/Devices/IDE%1/ATA%2/Unit%3/*DMA")
                            .arg(iIDECount).arg(i).arg(j)] = tr("DMA Transfers");
                        m_names[QString("/Devices/IDE%1/ATA%2/Unit%3/*PIO")
                            .arg(iIDECount).arg(i).arg(j)] = tr("PIO Transfers");
                        m_names[QString("/Devices/IDE%1/ATA%2/Unit%3/ReadBytes")
                            .arg(iIDECount).arg(i).arg(j)] = tr("Data Read");
                        m_names[QString("/Devices/IDE%1/ATA%2/Unit%3/WrittenBytes")
                            .arg(iIDECount).arg(i).arg(j)] = tr("Data Written");

                        /* Units: */
                        m_units[QString("/Devices/IDE%1/ATA%2/Unit%3/*DMA")
                            .arg(iIDECount).arg(i).arg(j)] = "[B]";
                        m_units[QString("/Devices/IDE%1/ATA%2/Unit%3/*PIO")
                            .arg(iIDECount).arg(i).arg(j)] = "[B]";
                        m_units[QString("/Devices/IDE%1/ATA%2/Unit%3/ReadBytes")
                            .arg(iIDECount).arg(i).arg(j)] = "B";
                        m_units[QString("/Devices/IDE%1/ATA%2/Unit%3/WrittenBytes")
                            .arg(iIDECount).arg(i).arg(j)] = "B";

                        /* Belongs to */
                        m_links[QString("/Devices/IDE%1/ATA%2/Unit%3").arg(iIDECount).arg(i).arg(j)] = QStringList()
                            << QString("/Devices/IDE%1/ATA%2/Unit%3/*DMA").arg(iIDECount).arg(i).arg(j)
                            << QString("/Devices/IDE%1/ATA%2/Unit%3/*PIO").arg(iIDECount).arg(i).arg(j)
                            << QString("/Devices/IDE%1/ATA%2/Unit%3/ReadBytes").arg(iIDECount).arg(i).arg(j)
                            << QString("/Devices/IDE%1/ATA%2/Unit%3/WrittenBytes").arg(iIDECount).arg(i).arg(j);
                    }
                }
                ++iIDECount;
                break;
            }
            case KStorageBus_SATA:
            {
                for (ULONG i = 0; i < sp.GetMaxPortCountForStorageBus(KStorageBus_SATA); ++i)
                {
                    for (ULONG j = 0; j < sp.GetMaxDevicesPerPortForStorageBus(KStorageBus_SATA); ++j)
                    {
                        /* Names: */
                        m_names[QString("/Devices/SATA%1/Port%2/DMA").arg(iSATACount).arg(i)]
                            = tr("DMA Transfers");
                        m_names[QString("/Devices/SATA%1/Port%2/ReadBytes").arg(iSATACount).arg(i)]
                            = tr("Data Read");
                        m_names[QString("/Devices/SATA%1/Port%2/WrittenBytes").arg(iSATACount).arg(i)]
                            = tr("Data Written");

                        /* Units: */
                        m_units[QString("/Devices/SATA%1/Port%2/DMA").arg(iSATACount).arg(i)] = "[B]";
                        m_units[QString("/Devices/SATA%1/Port%2/ReadBytes").arg(iSATACount).arg(i)] = "B";
                        m_units[QString("/Devices/SATA%1/Port%2/WrittenBytes").arg(iSATACount).arg(i)] = "B";

                        /* Belongs to: */
                        m_links[QString("/Devices/SATA%1/Port%2").arg(iSATACount).arg(i)] = QStringList()
                            << QString("/Devices/SATA%1/Port%2/DMA").arg(iSATACount).arg(i)
                            << QString("/Devices/SATA%1/Port%2/ReadBytes").arg(iSATACount).arg(i)
                            << QString("/Devices/SATA%1/Port%2/WrittenBytes").arg(iSATACount).arg(i);
                    }
                }
                ++iSATACount;
                break;
            }
            case KStorageBus_SCSI:
            {
                for (ULONG i = 0; i < sp.GetMaxPortCountForStorageBus(KStorageBus_SCSI); ++i)
                {
                    for (ULONG j = 0; j < sp.GetMaxDevicesPerPortForStorageBus(KStorageBus_SCSI); ++j)
                    {
                        /* Names: */
                        m_names[QString("/Devices/SCSI%1/%2/ReadBytes").arg(iSCSICount).arg(i)]
                            = tr("Data Read");
                        m_names[QString("/Devices/SCSI%1/%2/WrittenBytes").arg(iSCSICount).arg(i)]
                            = tr("Data Written");

                        /* Units: */
                        m_units[QString("/Devices/SCSI%1/%2/ReadBytes").arg(iSCSICount).arg(i)] = "B";
                        m_units[QString("/Devices/SCSI%1/%2/WrittenBytes").arg(iSCSICount).arg(i)] = "B";

                        /* Belongs to: */
                        m_links[QString("/Devices/SCSI%1/%2").arg(iSCSICount).arg(i)] = QStringList()
                            << QString("/Devices/SCSI%1/%2/ReadBytes").arg(iSCSICount).arg(i)
                            << QString("/Devices/SCSI%1/%2/WrittenBytes").arg(iSCSICount).arg(i);
                    }
                }
                ++iSCSICount;
                break;
            }
            default:
                break;
        }
    }

    /* Network statistics: */
    ulong count = vboxGlobal().virtualBox().GetSystemProperties().GetMaxNetworkAdapters(KChipsetType_PIIX3);
    for (ulong i = 0; i < count; ++i)
    {
        CNetworkAdapter na = machine.GetNetworkAdapter(i);
        KNetworkAdapterType ty = na.GetAdapterType();
        const char *name;

        switch (ty)
        {
            case KNetworkAdapterType_I82540EM:
            case KNetworkAdapterType_I82543GC:
            case KNetworkAdapterType_I82545EM:
                name = "E1k";
                break;
            case KNetworkAdapterType_Virtio:
                name = "VNet";
                break;
            default:
                name = "PCNet";
                break;
        }

        /* Names: */
        m_names[QString("/Devices/%1%2/TransmitBytes").arg(name).arg(i)] = tr("Data Transmitted");
        m_names[QString("/Devices/%1%2/ReceiveBytes").arg(name).arg(i)] = tr("Data Received");

        /* Units: */
        m_units[QString("/Devices/%1%2/TransmitBytes").arg(name).arg(i)] = "B";
        m_units[QString("/Devices/%1%2/ReceiveBytes").arg(name).arg(i)] = "B";

        /* Belongs to: */
        m_links[QString("NA%1").arg(i)] = QStringList()
            << QString("/Devices/%1%2/TransmitBytes").arg(name).arg(i)
            << QString("/Devices/%1%2/ReceiveBytes").arg(name).arg(i);
    }

    /* Statistics page update: */
    refreshStatistics();
}

bool UIVMInfoDialog::event(QEvent *pEvent)
{
    /* Pre-process through base-class: */
    bool fResult = QMainWindow::event(pEvent);

    /* Process required events: */
    switch (pEvent->type())
    {
        /* Store window state for this VM: */
        case QEvent::WindowStateChange:
        {
            if (m_fIsPolished)
                m_fMax = isMaximized();
            break;
        }
        default:
            break;
    }

    /* Return result: */
    return fResult;
}

void UIVMInfoDialog::resizeEvent(QResizeEvent *pEvent)
{
    /* Pre-process through base-class: */
    QMainWindow::resizeEvent(pEvent);

    /* Store dialog size for this VM: */
    if (m_fIsPolished && !isMaximized())
    {
        m_iWidth = width();
        m_iHeight = height();
    }
}

void UIVMInfoDialog::showEvent(QShowEvent *pEvent)
{
    /* One may think that QWidget::polish() is the right place to do things
     * below, but apparently, by the time when QWidget::polish() is called,
     * the widget style & layout are not fully done, at least the minimum
     * size hint is not properly calculated. Since this is sometimes necessary,
     * we provide our own "polish" implementation */
    if (!m_fIsPolished)
    {
        /* Mark as polished: */
        m_fIsPolished = true;

        /* Load window size, adjust position and load window state finally: */
        resize(m_iWidth, m_iHeight);
        vboxGlobal().centerWidget(this, m_pMachineWindow, false);
        if (m_fMax)
            QTimer::singleShot(0, this, SLOT(showMaximized()));
    }

    /* Post-process through base-class: */
    QMainWindow::showEvent(pEvent);
}

void UIVMInfoDialog::sltUpdateDetails()
{
    /* Details page update: */
    m_browsers[0]->setText(vboxGlobal().detailsReport(m_session.GetMachine(), false /* with links */));
}

void UIVMInfoDialog::sltProcessStatistics()
{
    /* Get machine debugger: */
    CMachineDebugger dbg = m_session.GetConsole().GetDebugger();
    QString strInfo;

    /* Process selected VM statistics: */
    for (DataMapType::const_iterator it = m_names.begin(); it != m_names.end(); ++it)
    {
        strInfo = dbg.GetStats(it.key(), true);
        m_values[it.key()] = parseStatistics(strInfo);
    }

    /* Update VM statistics page: */
    refreshStatistics();
}

void UIVMInfoDialog::sltHandlePageChanged(int iIndex)
{
    /* Focus the browser on shown page: */
    m_pTabWidget->widget(iIndex)->setFocus();
}

void UIVMInfoDialog::prepare()
{
    /* Prepare dialog: */
    prepareThis();
    /* Load settings: */
    loadSettings();
}

void UIVMInfoDialog::prepareThis()
{
    /* Delete dialog on close: */
    setAttribute(Qt::WA_DeleteOnClose);
    /* Delete dialog on machine-window destruction: */
    connect(m_pMachineWindow, SIGNAL(destroyed(QObject*)), this, SLOT(suicide()));

#ifdef Q_WS_MAC
    /* No window-icon on Mac OX X, because it acts as proxy icon which isn't necessary here. */
    setWindowIcon(QIcon());
#else /* !Q_WS_MAC */
    /* Assign window-icon(s: */
    setWindowIcon(UIIconPool::iconSetFull(":/session_info_32px.png", ":/session_info_16px.png"));
#endif /* !Q_WS_MAC */

    /* Prepare central-widget: */
    prepareCentralWidget();

    /* Configure handlers: */
    connect(m_pMachineWindow->uisession(), SIGNAL(sigMediumChange(const CMediumAttachment&)), this, SLOT(sltUpdateDetails()));
    connect(m_pMachineWindow->uisession(), SIGNAL(sigSharedFolderChange()), this, SLOT(sltUpdateDetails()));
    /* TODO_NEW_CORE: this is ofc not really right in the mm sense. There are more than one screens. */
    connect(m_pMachineWindow->machineView(), SIGNAL(resizeHintDone()), this, SLOT(sltProcessStatistics()));
    connect(m_pTabWidget, SIGNAL(currentChanged(int)), this, SLOT(sltHandlePageChanged(int)));
    connect(&vboxGlobal(), SIGNAL(sigMediumEnumerationFinished()), this, SLOT(sltUpdateDetails()));
    connect(m_pTimer, SIGNAL(timeout()), this, SLOT(sltProcessStatistics()));

    /* Retranslate: */
    retranslateUi();

    /* Details page update: */
    sltUpdateDetails();

    /* Statistics page update: */
    sltProcessStatistics();
    m_pTimer->start(5000);
}

void UIVMInfoDialog::prepareCentralWidget()
{
    /* Create central-widget: */
    setCentralWidget(new QWidget);
    AssertPtrReturnVoid(centralWidget());
    {
        /* Create main-layout: */
        new QVBoxLayout(centralWidget());
        AssertPtrReturnVoid(centralWidget()->layout());
        {
            /* Create tab-widget: */
            prepareTabWidget();
            /* Create button-box: */
            prepareButtonBox();
        }
    }
}

void UIVMInfoDialog::prepareTabWidget()
{
    /* Create tab-widget: */
    m_pTabWidget = new QITabWidget;
    AssertPtrReturnVoid(m_pTabWidget);
    {
        /* Create tabs: */
        for (int iTabIndex = 0; iTabIndex < 2; ++iTabIndex)
            prepareTab(iTabIndex);
        /* Configure tab-widget: */
        m_pTabWidget->setTabIcon(0, UIIconPool::iconSet(":/session_info_details_16px.png"));
        m_pTabWidget->setTabIcon(1, UIIconPool::iconSet(":/session_info_runtime_16px.png"));
        m_pTabWidget->setCurrentIndex(1);
        /* Add tab-widget into main-layout: */
        centralWidget()->layout()->addWidget(m_pTabWidget);
    }
}

void UIVMInfoDialog::prepareTab(int iTabIndex)
{
    /* Create tab: */
    m_tabs.insert(iTabIndex, new QWidget);
    AssertPtrReturnVoid(m_tabs.value(iTabIndex));
    {
        /* Create tab-layout: */
        QVBoxLayout *pLayout = new QVBoxLayout(m_tabs.value(iTabIndex));
        {
            /* Configure tab-layout: */
            pLayout->setContentsMargins(0, 0, 0, 0);
            /* Create browser: */
            m_browsers.insert(iTabIndex, new QRichTextEdit);
            AssertPtrReturnVoid(m_browsers.value(iTabIndex));
            {
                /* Configure browser: */
                m_browsers[iTabIndex]->setReadOnly(true);
                m_browsers[iTabIndex]->setFrameShadow(QFrame::Plain);
                m_browsers[iTabIndex]->setFrameShape(QFrame::NoFrame);
                m_browsers[iTabIndex]->setViewportMargins(5, 5, 5, 5);
                m_browsers[iTabIndex]->viewport()->setAutoFillBackground(false);
                m_tabs[iTabIndex]->setFocusProxy(m_browsers.value(iTabIndex));
                /* Add browser into tab-layout: */
                pLayout->addWidget(m_browsers.value(iTabIndex));
            }
        }
        /* Add tab into tab-widget: */
        m_pTabWidget->addTab(m_tabs.value(iTabIndex), QString());
    }
}

void UIVMInfoDialog::prepareButtonBox()
{
    /* Create button-box: */
    m_pButtonBox = new QIDialogButtonBox;
    AssertPtrReturnVoid(m_pButtonBox);
    {
        /* Configure button-box: */
        m_pButtonBox->setStandardButtons(QDialogButtonBox::Close);
        m_pButtonBox->button(QDialogButtonBox::Close)->setShortcut(Qt::Key_Escape);
        connect(m_pButtonBox, SIGNAL(rejected()), this, SLOT(close()));
        /* Add button-box into main-layout: */
        centralWidget()->layout()->addWidget(m_pButtonBox);
    }
}

void UIVMInfoDialog::loadSettings()
{
    /* Load dialog geometry: */
    QString strSize = m_session.GetMachine().GetExtraData(GUI_InfoDlgState);
    if (strSize.isEmpty())
    {
        m_iWidth = 400;
        m_iHeight = 450;
        m_fMax = false;
    }
    else
    {
        QStringList list = strSize.split(',');
        m_iWidth = list[0].toInt(), m_iHeight = list[1].toInt();
        m_fMax = list[2] == "max";
    }
}

void UIVMInfoDialog::saveSettings()
{
    /* Save dialog geometry: */
    QString strSize("%1,%2,%3");
    m_session.GetMachine().SetExtraData(GUI_InfoDlgState,
                                        strSize.arg(m_iWidth).arg(m_iHeight).arg(isMaximized() ? "max" : "normal"));
}

void UIVMInfoDialog::cleanup()
{
    /* Save settings: */
    saveSettings();
}

QString UIVMInfoDialog::parseStatistics(const QString &strText)
{
    /* Filters VM statistics counters body: */
    QRegExp query("^.+<Statistics>\n(.+)\n</Statistics>.*$");
    if (query.indexIn(strText) == -1)
        return QString();

    /* Split whole VM statistics text to lines: */
    const QStringList text = query.cap(1).split("\n");

    /* Iterate through all VM statistics: */
    ULONG64 uSumm = 0;
    for (QStringList::const_iterator lineIt = text.begin(); lineIt != text.end(); ++lineIt)
    {
        /* Get current line: */
        QString strLine = *lineIt;
        strLine.remove(1, 1);
        strLine.remove(strLine.length() -2, 2);

        /* Parse incoming counter and fill the counter-element values: */
        CounterElementType counter;
        counter.type = strLine.section(" ", 0, 0);
        strLine = strLine.section(" ", 1);
        QStringList list = strLine.split("\" ");
        for (QStringList::Iterator it = list.begin(); it != list.end(); ++it)
        {
            QString pair = *it;
            QRegExp regExp("^(.+)=\"([^\"]*)\"?$");
            regExp.indexIn(pair);
            counter.list.insert(regExp.cap(1), regExp.cap(2));
        }

        /* Fill the output with the necessary counter's value.
         * Currently we are using "c" field of simple counter only. */
        QString result = counter.list.contains("c") ? counter.list["c"] : "0";
        uSumm += result.toULongLong();
    }

    return QString::number(uSumm);
}

void UIVMInfoDialog::refreshStatistics()
{
    /* Skip for inactive session: */
    if (m_session.isNull())
        return;

    /* Prepare templates: */
    QString strTable = "<table width=100% cellspacing=1 cellpadding=0>%1</table>";
    QString strHeader = "<tr><td width=22><img src='%1'></td>"
                        "<td colspan=2><nobr><b>%2</b></nobr></td></tr>";
    QString strParagraph = "<tr><td colspan=3></td></tr>";
    QString strResult;

    /* Get current machine: */
    CMachine m = m_session.GetMachine();

    /* Runtime Information: */
    {
        /* Get current console: */
        CConsole console = m_session.GetConsole();

        /* Determine resolution: */
        ULONG uWidth = 0;
        ULONG uHeight = 0;
        ULONG uBpp = 0;
        LONG xOrigin = 0;
        LONG yOrigin = 0;
        console.GetDisplay().GetScreenResolution(0, uWidth, uHeight, uBpp, xOrigin, yOrigin);
        QString strResolution = QString("%1x%2").arg(uWidth).arg(uHeight);
        if (uBpp)
            strResolution += QString("x%1").arg(uBpp);
        strResolution += QString(" @%1,%2").arg(xOrigin).arg(yOrigin);

        /* Calculate uptime: */
        uint32_t uUpSecs = (RTTimeProgramSecTS() / 5) * 5;
        char szUptime[32];
        uint32_t uUpDays = uUpSecs / (60 * 60 * 24);
        uUpSecs -= uUpDays * 60 * 60 * 24;
        uint32_t uUpHours = uUpSecs / (60 * 60);
        uUpSecs -= uUpHours * 60 * 60;
        uint32_t uUpMins  = uUpSecs / 60;
        uUpSecs -= uUpMins * 60;
        RTStrPrintf(szUptime, sizeof(szUptime), "%dd %02d:%02d:%02d",
                    uUpDays, uUpHours, uUpMins, uUpSecs);
        QString strUptime = QString(szUptime);

        /* Determine clipboard mode: */
        QString strClipboardMode = gpConverter->toString(m.GetClipboardMode());
        /* Determine Drag&Drop mode: */
        QString strDragAndDropMode = gpConverter->toString(m.GetDragAndDropMode());

        /* Deterine virtualization attributes: */
        CMachineDebugger debugger = console.GetDebugger();
        QString strVirtualization = debugger.GetHWVirtExEnabled() ?
            VBoxGlobal::tr("Enabled", "details report (VT-x/AMD-V)") :
            VBoxGlobal::tr("Disabled", "details report (VT-x/AMD-V)");
        QString strNestedPaging = debugger.GetHWVirtExNestedPagingEnabled() ?
            VBoxGlobal::tr("Enabled", "details report (Nested Paging)") :
            VBoxGlobal::tr("Disabled", "details report (Nested Paging)");
        QString strUnrestrictedExecution = debugger.GetHWVirtExUXEnabled() ?
            VBoxGlobal::tr("Enabled", "details report (Unrestricted Execution)") :
            VBoxGlobal::tr("Disabled", "details report (Unrestricted Execution)");

        /* Guest information: */
        CGuest guest = console.GetGuest();
        QString strGAVersion = guest.GetAdditionsVersion();
        if (strGAVersion.isEmpty())
            strGAVersion = tr("Not Detected", "guest additions");
        else
        {
            ULONG uRevision = guest.GetAdditionsRevision();
            if (uRevision != 0)
                strGAVersion += QString(" r%1").arg(uRevision);
        }
        QString strOSType = guest.GetOSTypeId();
        if (strOSType.isEmpty())
            strOSType = tr("Not Detected", "guest os type");
        else
            strOSType = vboxGlobal().vmGuestOSTypeDescription(strOSType);

        /* VRDE information: */
        int iVRDEPort = console.GetVRDEServerInfo().GetPort();
        QString strVRDEInfo = (iVRDEPort == 0 || iVRDEPort == -1)?
            tr("Not Available", "details report (VRDE server port)") :
            QString("%1").arg(iVRDEPort);

        /* Searching for longest string: */
        QStringList values;
        values << strResolution << strUptime
               << strVirtualization << strNestedPaging << strUnrestrictedExecution
               << strGAVersion << strOSType << strVRDEInfo;
        int iMaxLength = 0;
        foreach (const QString &strValue, values)
            iMaxLength = iMaxLength < fontMetrics().width(strValue)
                         ? fontMetrics().width(strValue) : iMaxLength;

        /* Summary: */
        strResult += strHeader.arg(":/state_running_16px.png").arg(tr("Runtime Attributes"));
        strResult += formatValue(tr("Screen Resolution"), strResolution, iMaxLength);
        strResult += formatValue(tr("VM Uptime"), strUptime, iMaxLength);
        strResult += formatValue(tr("Clipboard Mode"), strClipboardMode, iMaxLength);
        strResult += formatValue(tr("Drag'n'Drop Mode"), strDragAndDropMode, iMaxLength);
        strResult += formatValue(VBoxGlobal::tr("VT-x/AMD-V", "details report"), strVirtualization, iMaxLength);
        strResult += formatValue(VBoxGlobal::tr("Nested Paging", "details report"), strNestedPaging, iMaxLength);
        strResult += formatValue(VBoxGlobal::tr("Unrestricted Execution", "details report"), strUnrestrictedExecution, iMaxLength);
        strResult += formatValue(tr("Guest Additions"), strGAVersion, iMaxLength);
        strResult += formatValue(tr("Guest OS Type"), strOSType, iMaxLength);
        strResult += formatValue(VBoxGlobal::tr("Remote Desktop Server Port", "details report (VRDE Server)"), strVRDEInfo, iMaxLength);
        strResult += strParagraph;
    }

    /* Storage statistics: */
    {
        /* Prepare storage-statistics: */
        QString strStorageStat;

        /* Append result with storage-statistics header: */
        strResult += strHeader.arg(":/hd_16px.png").arg(tr("Storage Statistics"));

        /* Enumerate storage-controllers: */
        CStorageControllerVector controllers = m.GetStorageControllers();
        int iIDECount = 0, iSATACount = 0, iSCSICount = 0;
        foreach (const CStorageController &controller, controllers)
        {
            /* Get controller attributes: */
            QString strName = controller.GetName();
            KStorageBus busType = controller.GetBus();
            CMediumAttachmentVector attachments = m.GetMediumAttachmentsOfController(strName);
            /* Skip empty and floppy attachments: */
            if (!attachments.isEmpty() && busType != KStorageBus_Floppy)
            {
                /* Prepare storage templates: */
                QString strHeaderStorage = "<tr><td></td><td colspan=2><nobr>%1</nobr></td></tr>";
                /* Prepare full controller name: */
                QString strControllerName = QApplication::translate("UIMachineSettingsStorage", "Controller: %1");
                /* Append storage-statistics with controller name: */
                strStorageStat += strHeaderStorage.arg(strControllerName.arg(controller.GetName()));
                int iSCSIIndex = 0;
                /* Enumerate storage-attachments: */
                foreach (const CMediumAttachment &attachment, attachments)
                {
                    const LONG iPort = attachment.GetPort();
                    const LONG iDevice = attachment.GetDevice();
                    switch (busType)
                    {
                        case KStorageBus_IDE:
                        {
                            /* Append storage-statistics with IDE controller statistics: */
                            strStorageStat += formatStorageElement(strName, iPort, iDevice,
                                                                   QString("/Devices/IDE%1/ATA%2/Unit%3")
                                                                          .arg(iIDECount).arg(iPort).arg(iDevice));
                            break;
                        }
                        case KStorageBus_SATA:
                        {
                            /* Append storage-statistics with SATA controller statistics: */
                            strStorageStat += formatStorageElement(strName, iPort, iDevice,
                                                                   QString("/Devices/SATA%1/Port%2")
                                                                          .arg(iSATACount).arg(iPort));
                            break;
                        }
                        case KStorageBus_SCSI:
                        {
                            /* Append storage-statistics with SCSI controller statistics: */
                            strStorageStat += formatStorageElement(strName, iPort, iDevice,
                                                                   QString("/Devices/SCSI%1/%2")
                                                                          .arg(iSCSICount).arg(iSCSIIndex));
                            ++iSCSIIndex;
                            break;
                        }
                        default:
                            break;
                    }
                    strStorageStat += strParagraph;
                }
            }
            /* Increment controller counters: */
            switch (busType)
            {
                case KStorageBus_IDE:  ++iIDECount; break;
                case KStorageBus_SATA: ++iSATACount; break;
                case KStorageBus_SCSI: ++iSCSICount; break;
                default: break;
            }
        }

        /* If there are no storage devices: */
        if (strStorageStat.isNull())
        {
            strStorageStat = composeArticle(tr("No Storage Devices"));
            strStorageStat += strParagraph;
        }

        /* Append result with storage-statistics: */
        strResult += strStorageStat;
    }

    /* Network statistics: */
    {
        /* Prepare netork-statistics: */
        QString strNetworkStat;

        /* Append result with network-statistics header: */
        strResult += strHeader.arg(":/nw_16px.png").arg(tr("Network Statistics"));

        /* Enumerate network-adapters: */
        ulong uCount = vboxGlobal().virtualBox().GetSystemProperties().GetMaxNetworkAdapters(m.GetChipsetType());
        for (ulong uSlot = 0; uSlot < uCount; ++uSlot)
        {
            /* Skip disabled adapters: */
            if (m.GetNetworkAdapter(uSlot).GetEnabled())
            {
                /* Append network-statistics with adapter-statistics: */
                strNetworkStat += formatNetworkElement(uSlot, QString("NA%1").arg(uSlot));
                strNetworkStat += strParagraph;
            }
        }

        /* If there are no network adapters: */
        if (strNetworkStat.isNull())
        {
            strNetworkStat = composeArticle(tr("No Network Adapters"));
            strNetworkStat += strParagraph;
        }

        /* Append result with network-statistics: */
        strResult += strNetworkStat;
    }

    /* Show full composed page & save/restore scroll-bar position: */
    int iScrollBarValue = m_browsers[1]->verticalScrollBar()->value();
    m_browsers[1]->setText(strTable.arg(strResult));
    m_browsers[1]->verticalScrollBar()->setValue(iScrollBarValue);
}

QString UIVMInfoDialog::formatValue(const QString &strValueName,
                                    const QString &strValue,
                                    int iMaxSize)
{
    if (m_session.isNull())
        return QString();

    QString strMargin;
    int size = iMaxSize - fontMetrics().width(strValue);
    for (int i = 0; i < size; ++i)
        strMargin += QString("<img width=1 height=1 src=:/tpixel.png>");

    QString bdyRow = "<tr>"
                     "<td></td>"
                     "<td><nobr>%1</nobr></td>"
                     "<td align=right><nobr>%2%3</nobr></td>"
                     "</tr>";

    return bdyRow.arg(strValueName).arg(strValue).arg(strMargin);
}

QString UIVMInfoDialog::formatStorageElement(const QString &strControllerName,
                                             LONG iPort, LONG iDevice,
                                             const QString &strBelongsTo)
{
    if (m_session.isNull())
        return QString();

    QString strHeader = "<tr><td></td><td colspan=2><nobr>&nbsp;&nbsp;%1:</nobr></td></tr>";
    CStorageController ctr = m_session.GetMachine().GetStorageControllerByName(strControllerName);
    QString strName = gpConverter->toString(StorageSlot(ctr.GetBus(), iPort, iDevice));

    return strHeader.arg(strName) + composeArticle(strBelongsTo, 2);
}

QString UIVMInfoDialog::formatNetworkElement(ULONG uSlot,
                                             const QString &strBelongsTo)
{
    if (m_session.isNull())
        return QString();

    QString strHeader = "<tr><td></td><td colspan=2><nobr>%1</nobr></td></tr>";
    QString strName = VBoxGlobal::tr("Adapter %1", "details report (network)").arg(uSlot + 1);

    return strHeader.arg(strName) + composeArticle(strBelongsTo, 1);
}

QString UIVMInfoDialog::composeArticle(const QString &strBelongsTo, int iSpacesCount /* = 0 */)
{
    QFontMetrics fm = QApplication::fontMetrics();

    QString strBody = "<tr><td></td><td width=50%><nobr>%1%2</nobr></td>"
                      "<td align=right><nobr>%3%4</nobr></td></tr>";
    QString strIndent;
    for (int i = 0; i < iSpacesCount; ++i)
        strIndent += "&nbsp;&nbsp;";
    strBody = strBody.arg(strIndent);

    QString strResult;

    if (m_links.contains(strBelongsTo))
    {
        QStringList keys = m_links[strBelongsTo];
        foreach (const QString &key, keys)
        {
            QString line(strBody);
            if (m_names.contains(key) && m_values.contains(key) && m_units.contains(key))
            {
                line = line.arg(m_names[key]).arg(QString("%L1").arg(m_values[key].toULongLong()));
                line = m_units[key].contains(QRegExp("\\[\\S+\\]")) ?
                    line.arg(QString("<img src=:/tpixel.png width=%1 height=1>")
                                    .arg(fm.width(QString(" %1").arg(m_units[key].mid(1, m_units[key].length() - 2))))) :
                    line.arg(QString (" %1").arg(m_units[key]));
                strResult += line;
            }
        }
    }
    else
        strResult = strBody.arg(strBelongsTo).arg(QString()).arg(QString());

    return strResult;
}
