/* $Id$ */
/** @file
 * VBox Qt GUI - VBoxAboutDlg class implementation.
 */

/*
 * Copyright (C) 2006-2011 Oracle Corporation
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
# include <precomp.h>
#else  /* !VBOX_WITH_PRECOMPILED_HEADERS */

/* Global includes */
# include <QDir>
# include <QEvent>
# include <QPainter>
# include <QLabel>
# include <iprt/path.h>
# include <VBox/version.h> /* VBOX_VENDOR */

/* Local includes */
# include "VBoxAboutDlg.h"
# include "VBoxGlobal.h"
# include "UIConverter.h"
# include "UIExtraDataManager.h"
# include "UIIconPool.h"

#endif /* !VBOX_WITH_PRECOMPILED_HEADERS */


VBoxAboutDlg::VBoxAboutDlg(QWidget *pParent, const QString &strVersion)
    : QIWithRetranslateUI2<QIDialog>(pParent)
    , m_strVersion(strVersion)
{
    /* Delete dialog on close: */
    setAttribute(Qt::WA_DeleteOnClose);

    /* Choose default image: */
    QString strPath(":/about.png");

    /* Branding: Use a custom about splash picture if set: */
    QString strSplash = vboxGlobal().brandingGetKey("UI/AboutSplash");
    if (vboxGlobal().brandingIsActive() && !strSplash.isEmpty())
    {
        char szExecPath[1024];
        RTPathExecDir(szExecPath, 1024);
        QString strTmpPath = QString("%1/%2").arg(szExecPath).arg(strSplash);
        if (QFile::exists(strTmpPath))
            strPath = strTmpPath;
    }

    /* Load image: */
    QIcon icon = UIIconPool::iconSet(strPath);
    m_size = icon.availableSizes().first();
    m_pixmap = icon.pixmap(m_size);

    /* Create main layout: */
    QVBoxLayout *pMainLayout = new QVBoxLayout(this);

    /* Create label for version text: */
    m_pLabel = new QLabel();
    pMainLayout->addWidget(m_pLabel);

    QPalette palette;
    /* Branding: Set a different text color (because splash also could be white),
     * otherwise use white as default color: */
    QString strColor = vboxGlobal().brandingGetKey("UI/AboutTextColor");
    if (!strColor.isEmpty())
        palette.setColor(QPalette::WindowText, QColor(strColor).name());
    else
        palette.setColor(QPalette::WindowText, Qt::black);
    m_pLabel->setPalette(palette);
    m_pLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_pLabel->setFont(font());

    pMainLayout->setAlignment(m_pLabel, Qt::AlignRight | Qt::AlignBottom);

    /* Translate: */
    retranslateUi();
}

bool VBoxAboutDlg::event(QEvent *pEvent)
{
    if (pEvent->type() == QEvent::Polish)
        setFixedSize(m_size);
    return QIDialog::event(pEvent);
}

void VBoxAboutDlg::paintEvent(QPaintEvent* /* pEvent */)
{
    QPainter painter(this);
    painter.drawPixmap(0, 0, m_pixmap);
}

void VBoxAboutDlg::retranslateUi()
{
    setWindowTitle(tr("VirtualBox - About"));
    QString strAboutText = tr("VirtualBox Graphical User Interface");
#ifdef VBOX_BLEEDING_EDGE
    QString strVersionText = "EXPERIMENTAL build %1 - " + QString(VBOX_BLEEDING_EDGE);
#else
    QString strVersionText = tr("Version %1");
#endif
#if VBOX_OSE
    m_strAboutText = strAboutText + " " + strVersionText.arg(m_strVersion) + "\n" +
                     QString("%1 2004-" VBOX_C_YEAR " " VBOX_VENDOR).arg(QChar(0xa9));
#else /* VBOX_OSE */
    m_strAboutText = strAboutText + "\n" + strVersionText.arg(m_strVersion);
#endif /* VBOX_OSE */
    m_strAboutText = m_strAboutText + "\n" + QString("Copyright %1 2015 Oracle and/or its affiliates. All rights reserved.").arg(QChar(0xa9));
    m_pLabel->setText(m_strAboutText);
}

