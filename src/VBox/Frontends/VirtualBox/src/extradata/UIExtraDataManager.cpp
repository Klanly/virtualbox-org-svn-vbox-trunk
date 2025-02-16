/* $Id$ */
/** @file
 * VBox Qt GUI - UIExtraDataManager class implementation.
 */

/*
 * Copyright (C) 2010-2014 Oracle Corporation
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

/* Qt includes: */
# include <QMutex>
# include <QMetaEnum>
# include <QDesktopWidget>
# ifdef DEBUG
#  include <QMainWindow>
#  include <QMenuBar>
#  include <QListView>
#  include <QTableView>
#  include <QHeaderView>
#  include <QSortFilterProxyModel>
#  include <QStyledItemDelegate>
#  include <QPainter>
#  include <QLabel>
#  include <QLineEdit>
#  include <QComboBox>
#  include <QPushButton>
# endif /* DEBUG */

/* GUI includes: */
# include "UIExtraDataManager.h"
# include "UIMainEventListener.h"
# include "VBoxGlobalSettings.h"
# include "VBoxGlobal.h"
# include "UIActionPool.h"
# include "UIConverter.h"
# include "UISettingsDefs.h"
# include "UIMessageCenter.h"
# ifdef DEBUG
#  include "VBoxUtils.h"
#  include "UIVirtualBoxEventHandler.h"
#  include "UIIconPool.h"
#  include "UIToolBar.h"
#  include "QIWidgetValidator.h"
#  include "QIDialogButtonBox.h"
#  include "QIFileDialog.h"
#  include "QISplitter.h"
#  include "QIDialog.h"
# endif /* DEBUG */

/* COM includes: */
# include "COMEnums.h"
# include "CEventSource.h"
# include "CVirtualBox.h"
# include "CMachine.h"

#endif /* !VBOX_WITH_PRECOMPILED_HEADERS */

#ifdef DEBUG
# include <QStandardItemModel>
# include <QXmlStreamWriter>
# include <QXmlStreamReader>
#endif


/* Namespaces: */
using namespace UIExtraDataDefs;
using namespace UISettingsDefs;


/** QObject extension
  * notifying UIExtraDataManager whenever any of extra-data values changed. */
class UIExtraDataEventHandler : public QObject
{
    Q_OBJECT;

signals:

    /** Notifies about 'extra-data change' event: */
    void sigExtraDataChange(QString strMachineID, QString strKey, QString strValue);

public:

    /** Extra-data event-handler constructor. */
    UIExtraDataEventHandler(QObject *pParent);

public slots:

    /** Preprocess 'extra-data can change' event: */
    void sltPreprocessExtraDataCanChange(QString strMachineID, QString strKey, QString strValue, bool &fVeto, QString &strVetoReason);
    /** Preprocess 'extra-data change' event: */
    void sltPreprocessExtraDataChange(QString strMachineID, QString strKey, QString strValue);

private:

    /** Protects sltPreprocessExtraDataChange. */
    QMutex m_mutex;
};

UIExtraDataEventHandler::UIExtraDataEventHandler(QObject *pParent)
    : QObject(pParent)
{
}

void UIExtraDataEventHandler::sltPreprocessExtraDataCanChange(QString strMachineID, QString strKey, QString strValue, bool &fVeto, QString &strVetoReason)
{
    /* Preprocess global 'extra-data can change' event: */
    if (QUuid(strMachineID).isNull())
    {
        if (strKey.startsWith("GUI/"))
        {
            /* Try to set the global setting to check its syntax: */
            VBoxGlobalSettings gs(false /* non-null */);
            /* Known GUI property key? */
            if (gs.setPublicProperty(strKey, strValue))
            {
                /* But invalid GUI property value? */
                if (!gs)
                {
                    /* Remember veto reason: */
                    strVetoReason = gs.lastError();
                    /* And disallow that change: */
                    fVeto = true;
                }
            }
        }
    }
}

void UIExtraDataEventHandler::sltPreprocessExtraDataChange(QString strMachineID, QString strKey, QString strValue)
{
    /* Preprocess global 'extra-data change' event: */
    if (QUuid(strMachineID).isNull())
    {
        if (strKey.startsWith("GUI/"))
        {
            /* Apply global property: */
            m_mutex.lock();
            vboxGlobal().settings().setPublicProperty(strKey, strValue);
            m_mutex.unlock();
            AssertMsgReturnVoid(!!vboxGlobal().settings(), ("Failed to apply global property.\n"));
        }
    }

    /* Motify listener about 'extra-data change' event: */
    emit sigExtraDataChange(strMachineID, strKey, strValue);
}


#ifdef DEBUG
/** Data fields. */
enum Field
{
    Field_ID = Qt::UserRole + 1,
    Field_Name,
    Field_OsTypeID,
    Field_Known
};


/** QStyledItemDelegate extension
  * reflecting items of Extra Data Manager window: Chooser pane. */
class UIChooserPaneDelegate : public QStyledItemDelegate
{
    Q_OBJECT;

public:

    /** Constructor. */
    UIChooserPaneDelegate(QObject *pParent);

private:

    /** Size-hint calculation routine. */
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const;

    /** Paint routine. */
    void paint(QPainter *pPainter, const QStyleOptionViewItem &option, const QModelIndex &index) const;

    /** Fetch pixmap info for passed QModelIndex. */
    static void fetchPixmapInfo(const QModelIndex &index, QPixmap &pixmap, QSize &pixmapSize);

    /** Margin. */
    int m_iMargin;
    /** Spacing. */
    int m_iSpacing;
};

UIChooserPaneDelegate::UIChooserPaneDelegate(QObject *pParent)
    : QStyledItemDelegate(pParent)
    , m_iMargin(3)
    , m_iSpacing(3)
{
}

QSize UIChooserPaneDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    /* Font metrics: */
    const QFontMetrics &fm = option.fontMetrics;
    /* Pixmap: */
    QPixmap pixmap;
    QSize pixmapSize;
    fetchPixmapInfo(index, pixmap, pixmapSize);

    /* Calculate width: */
    const int iWidth = m_iMargin +
                       pixmapSize.width() +
                       2 * m_iSpacing +
                       qMax(fm.width(index.data(Field_Name).toString()),
                            fm.width(index.data(Field_ID).toString())) +
                       m_iMargin;
    /* Calculate height: */
    const int iHeight = m_iMargin +
                        qMax(pixmapSize.height(),
                             fm.height() + m_iSpacing + fm.height()) +
                        m_iMargin;

    /* Return result: */
    return QSize(iWidth, iHeight);
}

void UIChooserPaneDelegate::paint(QPainter *pPainter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    /* Item rect: */
    const QRect &optionRect = option.rect;
    /* Palette: */
    const QPalette &palette = option.palette;
    /* Font metrics: */
    const QFontMetrics &fm = option.fontMetrics;
    /* Pixmap: */
    QPixmap pixmap;
    QSize pixmapSize;
    fetchPixmapInfo(index, pixmap, pixmapSize);

    /* If item selected: */
    if (option.state & QStyle::State_Selected)
    {
        /* Fill background with selection color: */
        QColor highlight = palette.color(option.state & QStyle::State_Active ?
                                         QPalette::Active : QPalette::Inactive,
                                         QPalette::Highlight);
        QLinearGradient bgGrad(optionRect.topLeft(), optionRect.bottomLeft());
        bgGrad.setColorAt(0, highlight.lighter(120));
        bgGrad.setColorAt(1, highlight);
        pPainter->fillRect(optionRect, bgGrad);
        /* Draw focus frame: */
        QStyleOptionFocusRect focusOption;
        focusOption.rect = optionRect;
        QApplication::style()->drawPrimitive(QStyle::PE_FrameFocusRect, &focusOption, pPainter);
    }

    /* Draw pixmap: */
    const QPoint pixmapOrigin = optionRect.topLeft() +
                                QPoint(m_iMargin, m_iMargin);
    pPainter->drawPixmap(pixmapOrigin, pixmap);

    /* Is that known item? */
    bool fKnown = index.data(Field_Known).toBool();
    if (fKnown)
    {
        pPainter->save();
        QFont font = pPainter->font();
        font.setBold(true);
        pPainter->setFont(font);
    }

    /* Draw item name: */
    const QPoint nameOrigin = pixmapOrigin +
                              QPoint(pixmapSize.width(), 0) +
                              QPoint(2 * m_iSpacing, 0) +
                              QPoint(0, fm.ascent());
    pPainter->drawText(nameOrigin, index.data(Field_Name).toString());

    /* Was that known item? */
    if (fKnown)
        pPainter->restore();

    /* Draw item ID: */
    const QPoint idOrigin = nameOrigin +
                            QPoint(0, m_iSpacing) +
                            QPoint(0, fm.height());
    pPainter->drawText(idOrigin, index.data(Field_ID).toString());
}

/* static */
void UIChooserPaneDelegate::fetchPixmapInfo(const QModelIndex &index, QPixmap &pixmap, QSize &pixmapSize)
{
    /* If proper machine ID passed => return corresponding pixmap/size: */
    if (index.data(Field_ID).toString() != UIExtraDataManager::GlobalID)
        pixmap = vboxGlobal().vmGuestOSTypeIcon(index.data(Field_OsTypeID).toString(), &pixmapSize);
    else
    {
        /* For global ID we return static pixmap/size: */
        const QIcon icon = UIIconPool::iconSet(":/edataglobal_32px.png");
        pixmapSize = icon.availableSizes().first();
        pixmap = icon.pixmap(pixmapSize);
    }
}


/** QSortFilterProxyModel extension
  * used by the chooser-pane of the UIExtraDataManagerWindow. */
class UIChooserPaneSortingModel : public QSortFilterProxyModel
{
    Q_OBJECT;

public:

    /** Constructor, passes @a pParent to the QIRichToolButton constructor. */
    UIChooserPaneSortingModel(QObject *pParent) : QSortFilterProxyModel(pParent) {}

protected:

    /** Returns true if the value of the item referred to by the given index left
      * is less than the value of the item referred to by the given index right,
      * otherwise returns false. */
    bool lessThan(const QModelIndex &leftIdx, const QModelIndex &rightIdx) const
    {
        /* Compare by ID first: */
        const QString strID1 = leftIdx.data(Field_ID).toString();
        const QString strID2 = rightIdx.data(Field_ID).toString();
        if (strID1 == UIExtraDataManager::GlobalID)
            return true;
        else if (strID2 == UIExtraDataManager::GlobalID)
            return false;
        /* Compare role finally: */
        return QSortFilterProxyModel::lessThan(leftIdx, rightIdx);
    }
};


/** QMainWindow extension
  * providing Extra Data Manager with UI features. */
class UIExtraDataManagerWindow : public QMainWindow
{
    Q_OBJECT;

public:

    /** @name Constructor/Destructor
      * @{ */
        /** Extra-data Manager Window constructor. */
        UIExtraDataManagerWindow();
        /** Extra-data Manager Window destructor. */
        ~UIExtraDataManagerWindow();
    /** @} */

    /** @name Management
      * @{ */
        /** Show and raise. */
        void showAndRaise(QWidget *pCenterWidget);
    /** @} */

private slots:

    /** @name General
      * @{ */
        /** Handles machine (un)registration. */
        void sltMachineRegistered(QString strID, bool fAdded);
        /** Handles extra-data map acknowledging. */
        void sltExtraDataMapAcknowledging(QString strID);
        /** Handles extra-data change. */
        void sltExtraDataChange(QString strID, QString strKey, QString strValue);
    /** @} */

    /** @name Chooser-pane
      * @{ */
        /** Handles filter-apply signal for the chooser-pane. */
        void sltChooserApplyFilter(const QString &strFilter);
        /** Handles current-changed signal for the chooser-pane: */
        void sltChooserHandleCurrentChanged(const QModelIndex &index);
        /** Handles item-selection-changed signal for the chooser-pane: */
        void sltChooserHandleSelectionChanged(const QItemSelection &selected,
                                              const QItemSelection &deselected);
    /** @} */

    /** @name Data-pane
      * @{ */
        /** Handles filter-apply signal for the data-pane. */
        void sltDataApplyFilter(const QString &strFilter);
        /** Handles item-selection-changed signal for the data-pane: */
        void sltDataHandleSelectionChanged(const QItemSelection &selected,
                                           const QItemSelection &deselected);
        /** Handles item-changed signal for the data-pane: */
        void sltDataHandleItemChanged(QStandardItem *pItem);
        /** Handles context-menu-requested signal for the data-pane: */
        void sltDataHandleCustomContextMenuRequested(const QPoint &pos);
    /** @} */

    /** @name Actions
      * @{ */
        /** Add handler. */
        void sltAdd();
        /** Remove handler. */
        void sltDel();
        /** Save handler. */
        void sltSave();
        /** Load handler. */
        void sltLoad();
    /** @} */

private:

    /** @name Prepare/Cleanup
      * @{ */
        /** Prepare instance. */
        void prepare();
        /** Prepare this. */
        void prepareThis();
        /** Prepare connections. */
        void prepareConnections();
        /** Prepare menu. */
        void prepareMenu();
        /** Prepare central widget. */
        void prepareCentralWidget();
        /** Prepare tool-bar. */
        void prepareToolBar();
        /** Prepare splitter. */
        void prepareSplitter();
        /** Prepare panes: */
        void preparePanes();
        /** Prepare chooser pane. */
        void preparePaneChooser();
        /** Prepare data pane. */
        void preparePaneData();
        /** Prepare button-box. */
        void prepareButtonBox();
        /** Load window settings. */
        void loadSettings();

        /** Save window settings. */
        void saveSettings();
        /** Cleanup instance. */
        void cleanup();
    /** @} */

    /** @name Event Processing
      * @{ */
        /** Common event-handler. */
        bool event(QEvent *pEvent);
    /** @} */

    /** @name Actions
      * @{ */
        /** */
        void updateActionsAvailability();
    /** @} */

    /** @name Chooser-pane
      * @{ */
        /** Returns chooser index for @a iRow. */
        QModelIndex chooserIndex(int iRow) const;
        /** Returns current chooser index. */
        QModelIndex currentChooserIndex() const;

        /** Returns chooser ID for @a iRow. */
        QString chooserID(int iRow) const;
        /** Returns current chooser ID. */
        QString currentChooserID() const;

        /** Returns chooser Name for @a iRow. */
        QString chooserName(int iRow) const;
        /** Returns current Name. */
        QString currentChooserName() const;

        /** Adds chooser item. */
        void addChooserItem(const QString &strID,
                            const QString &strName,
                            const QString &strOsTypeID,
                            const int iPosition = -1);
        /** Adds chooser item by machine. */
        void addChooserItemByMachine(const CMachine &machine,
                                     const int iPosition = -1);
        /** Adds chooser item by ID. */
        void addChooserItemByID(const QString &strID,
                                const int iPosition = -1);

        /** Make sure chooser have current-index if possible. */
        void makeSureChooserHaveCurrentIndexIfPossible();
    /** @} */

    /** @name Data-pane
      * @{ */
        /** Returns data index for @a iRow and @a iColumn. */
        QModelIndex dataIndex(int iRow, int iColumn) const;

        /** Returns data-key index for @a iRow. */
        QModelIndex dataKeyIndex(int iRow) const;

        /** Returns data-value index for @a iRow. */
        QModelIndex dataValueIndex(int iRow) const;

        /** Returns current data-key. */
        QString dataKey(int iRow) const;

        /** Returns current data-value. */
        QString dataValue(int iRow) const;

        /** Adds data item. */
        void addDataItem(const QString &strKey,
                         const QString &strValue,
                         const int iPosition = -1);

        /** Sorts data items. */
        void sortData();

        /** Returns the list of known extra-data keys. */
        static QStringList knownExtraDataKeys();
    /** @} */


    /** @name General
      * @{ */
        /** Current geometry. */
        QRect m_geometry;
        QVBoxLayout *m_pMainLayout;
        /** Data pane: Tool-bar. */
        UIToolBar *m_pToolBar;
        /** Splitter. */
        QISplitter *m_pSplitter;
    /** @} */

    /** @name Chooser-pane
      * @{ */
        /** Chooser pane. */
        QWidget *m_pPaneOfChooser;
        /** Chooser filter. */
        QLineEdit *m_pFilterOfChooser;
        /** Chooser pane: List-view. */
        QListView *m_pViewOfChooser;
        /** Chooser pane: Source-model. */
        QStandardItemModel *m_pModelSourceOfChooser;
        /** Chooser pane: Proxy-model. */
        UIChooserPaneSortingModel *m_pModelProxyOfChooser;
    /** @} */

    /** @name Data-pane
      * @{ */
        /** Data pane. */
        QWidget *m_pPaneOfData;
        /** Data filter. */
        QLineEdit *m_pFilterOfData;
        /** Data pane: Table-view. */
        QTableView *m_pViewOfData;
        /** Data pane: Item-model. */
        QStandardItemModel *m_pModelSourceOfData;
        /** Data pane: Proxy-model. */
        QSortFilterProxyModel *m_pModelProxyOfData;
    /** @} */

    /** @name Button Box
      * @{ */
        /** Dialog button-box. */
        QIDialogButtonBox *m_pButtonBox;
    /** @} */

    /** @name Actions
      * @{ */
        /** Add action. */
        QAction *m_pActionAdd;
        /** Del action. */
        QAction *m_pActionDel;
        /** Load action. */
        QAction *m_pActionLoad;
        /** Save action. */
        QAction *m_pActionSave;
    /** @} */
};

UIExtraDataManagerWindow::UIExtraDataManagerWindow()
    : m_pMainLayout(0), m_pToolBar(0), m_pSplitter(0)
    , m_pPaneOfChooser(0), m_pFilterOfChooser(0), m_pViewOfChooser(0)
    , m_pModelSourceOfChooser(0), m_pModelProxyOfChooser(0)
    , m_pPaneOfData(0), m_pFilterOfData(0), m_pViewOfData(0),
      m_pModelSourceOfData(0), m_pModelProxyOfData(0)
    , m_pButtonBox(0)
    , m_pActionAdd(0), m_pActionDel(0)
    , m_pActionLoad(0), m_pActionSave(0)
{
    /* Prepare: */
    prepare();
}

UIExtraDataManagerWindow::~UIExtraDataManagerWindow()
{
    /* Cleanup: */
    cleanup();
}

void UIExtraDataManagerWindow::showAndRaise(QWidget*)
{
    /* Show: */
    show();
    /* Restore from minimized state: */
    setWindowState(windowState() & ~Qt::WindowMinimized);
    /* Raise: */
    activateWindow();
//    /* Center according passed widget: */
//    VBoxGlobal::centerWidget(this, pCenterWidget, false);
}

void UIExtraDataManagerWindow::sltMachineRegistered(QString strID, bool fRegistered)
{
    /* Machine registered: */
    if (fRegistered)
    {
        /* Gather list of 'known IDs': */
        QStringList knownIDs;
        for (int iRow = 0; iRow < m_pModelSourceOfChooser->rowCount(); ++iRow)
            knownIDs << chooserID(iRow);

        /* Get machine items: */
        const CMachineVector machines = vboxGlobal().virtualBox().GetMachines();
        /* Look for the proper place to insert new machine item: */
        QString strPositionID = UIExtraDataManager::GlobalID;
        foreach (const CMachine &machine, machines)
        {
            /* Get iterated machine ID: */
            const QString strIteratedID = machine.GetId();
            /* If 'iterated ID' equal to 'added ID' => break now: */
            if (strIteratedID == strID)
                break;
            /* If 'iterated ID' is 'known ID' => remember it: */
            if (knownIDs.contains(strIteratedID))
                strPositionID = strIteratedID;
        }

        /* Add new chooser item into source-model: */
        addChooserItemByID(strID, knownIDs.indexOf(strPositionID) + 1);
        /* And sort proxy-model: */
        m_pModelProxyOfChooser->sort(0, Qt::AscendingOrder);
        /* Make sure chooser have current-index if possible: */
        makeSureChooserHaveCurrentIndexIfPossible();
    }
    /* Machine unregistered: */
    else
    {
        /* Remove chooser item with 'removed ID' if it is among 'known IDs': */
        for (int iRow = 0; iRow < m_pModelSourceOfChooser->rowCount(); ++iRow)
            if (chooserID(iRow) == strID)
                m_pModelSourceOfChooser->removeRow(iRow);
    }
}

void UIExtraDataManagerWindow::sltExtraDataMapAcknowledging(QString strID)
{
    /* Update item with 'changed ID' if it is among 'known IDs': */
    for (int iRow = 0; iRow < m_pModelSourceOfChooser->rowCount(); ++iRow)
        if (chooserID(iRow) == strID)
            m_pModelSourceOfChooser->itemFromIndex(chooserIndex(iRow))->setData(true, Field_Known);
}

void UIExtraDataManagerWindow::sltExtraDataChange(QString strID, QString strKey, QString strValue)
{
    /* Skip unrelated IDs: */
    if (currentChooserID() != strID)
        return;

    /* List of 'known keys': */
    QStringList knownKeys;
    for (int iRow = 0; iRow < m_pModelSourceOfData->rowCount(); ++iRow)
        knownKeys << dataKey(iRow);

    /* Check if 'changed key' is 'known key': */
    int iPosition = knownKeys.indexOf(strKey);
    /* If that is 'known key': */
    if (iPosition != -1)
    {
        /* If 'changed value' is empty => REMOVE item: */
        if (strValue.isEmpty())
            m_pModelSourceOfData->removeRow(iPosition);
        /* If 'changed value' is NOT empty => UPDATE item: */
        else
            m_pModelSourceOfData->itemFromIndex(dataValueIndex(iPosition))->setText(strValue);
    }
    /* Else if 'changed value' is NOT empty: */
    else if (!strValue.isEmpty())
    {
        /* Look for the proper place for 'changed key': */
        QString strPositionKey;
        foreach (const QString &strIteratedKey, gEDataManager->map(strID).keys())
        {
            /* If 'iterated key' equal to 'changed key' => break now: */
            if (strIteratedKey == strKey)
                break;
            /* If 'iterated key' is 'known key' => remember it: */
            if (knownKeys.contains(strIteratedKey))
                strPositionKey = strIteratedKey;
        }
        /* Calculate resulting position: */
        iPosition = knownKeys.indexOf(strPositionKey) + 1;
        /* INSERT item to the required position: */
        addDataItem(strKey, strValue, iPosition);
        /* And sort proxy-model: */
        sortData();
    }
}

void UIExtraDataManagerWindow::sltChooserApplyFilter(const QString &strFilter)
{
    /* Apply filtering rule: */
    m_pModelProxyOfChooser->setFilterWildcard(strFilter);
    /* Make sure chooser have current-index if possible: */
    makeSureChooserHaveCurrentIndexIfPossible();
}

void UIExtraDataManagerWindow::sltChooserHandleCurrentChanged(const QModelIndex &index)
{
    /* Remove all the old items first: */
    while (m_pModelSourceOfData->rowCount())
        m_pModelSourceOfData->removeRow(0);

    /* Ignore invalid indexes: */
    if (!index.isValid())
        return;

    /* Add all the new items finally: */
    const QString strID = index.data(Field_ID).toString();
    if (!gEDataManager->contains(strID))
        gEDataManager->hotloadMachineExtraDataMap(strID);
    const ExtraDataMap data = gEDataManager->map(strID);
    foreach (const QString &strKey, data.keys())
        addDataItem(strKey, data.value(strKey));
    /* And sort proxy-model: */
    sortData();
}

void UIExtraDataManagerWindow::sltChooserHandleSelectionChanged(const QItemSelection&,
                                                                const QItemSelection&)
{
    /* Update actions availability: */
    updateActionsAvailability();
}

void UIExtraDataManagerWindow::sltDataApplyFilter(const QString &strFilter)
{
    /* Apply filtering rule: */
    m_pModelProxyOfData->setFilterWildcard(strFilter);
}

void UIExtraDataManagerWindow::sltDataHandleSelectionChanged(const QItemSelection&,
                                                             const QItemSelection&)
{
    /* Update actions availability: */
    updateActionsAvailability();
}

void UIExtraDataManagerWindow::sltDataHandleItemChanged(QStandardItem *pItem)
{
    /* Value-data index: */
    const QModelIndex valueIndex = m_pModelSourceOfData->indexFromItem(pItem);
    const int iRow = valueIndex.row();
    const int iColumn = valueIndex.column();
    AssertMsgReturnVoid(iColumn == 1, ("Only 2nd column can be changed!\n"));

    /* Key-data index: */
    const QModelIndex keyIndex = dataKeyIndex(iRow);

    /* Update extra-data: */
    gEDataManager->setExtraDataString(keyIndex.data().toString(),
                                      valueIndex.data().toString(),
                                      currentChooserID());
}

void UIExtraDataManagerWindow::sltDataHandleCustomContextMenuRequested(const QPoint &pos)
{
    /* Prepare menu: */
    QMenu menu;
    menu.addAction(m_pActionAdd);
    menu.addAction(m_pActionDel);
    menu.addSeparator();
    menu.addAction(m_pActionSave);
    /* Execute menu: */
    m_pActionSave->setProperty("CalledFromContextMenu", true);
    menu.exec(m_pViewOfData->viewport()->mapToGlobal(pos));
    m_pActionSave->setProperty("CalledFromContextMenu", QVariant());
}

void UIExtraDataManagerWindow::sltAdd()
{
    /* Make sure this slot called by corresponding action only: */
    QAction *pSenderAction = qobject_cast<QAction*>(sender());
    AssertReturnVoid(pSenderAction && m_pActionAdd);

    /* Create input-dialog: */
    QPointer<QIDialog> pInputDialog = new QIDialog(this);
    AssertPtrReturnVoid(pInputDialog.data());
    {
        /* Configure input-dialog: */
        pInputDialog->setWindowTitle("Add extra-data record..");
        pInputDialog->setMinimumWidth(400);
        /* Create main-layout: */
        QVBoxLayout *pMainLayout = new QVBoxLayout(pInputDialog);
        AssertPtrReturnVoid(pMainLayout);
        {
            /* Create dialog validator group: */
            QObjectValidatorGroup *pValidatorGroup = new QObjectValidatorGroup(pInputDialog);
            AssertReturnVoid(pValidatorGroup);
            /* Create input-layout: */
            QGridLayout *pInputLayout = new QGridLayout;
            AssertPtrReturnVoid(pInputLayout);
            {
                /* Create key-label: */
                QLabel *pLabelKey = new QLabel("&Name:");
                {
                    /* Configure key-label: */
                    pLabelKey->setAlignment(Qt::AlignRight);
                    /* Add key-label into input-layout: */
                    pInputLayout->addWidget(pLabelKey, 0, 0);
                }
                /* Create key-editor: */
                QComboBox *pEditorKey = new QComboBox;
                {
                    /* Configure key-editor: */
                    pEditorKey->setEditable(true);
                    pEditorKey->addItems(knownExtraDataKeys());
                    pLabelKey->setBuddy(pEditorKey);
                    /* Create key-editor property setter: */
                    QObjectPropertySetter *pKeyPropertySetter = new QObjectPropertySetter(pInputDialog, "Key");
                    AssertPtrReturnVoid(pKeyPropertySetter);
                    {
                        /* Configure key-editor property setter: */
                        connect(pEditorKey, SIGNAL(editTextChanged(const QString&)),
                                pKeyPropertySetter, SLOT(sltAssignProperty(const QString&)));
                    }
                    /* Create key-editor validator: */
                    QObjectValidator *pKeyValidator = new QObjectValidator(new QRegExpValidator(QRegExp("[\\s\\S]+"), this));
                    AssertPtrReturnVoid(pKeyValidator);
                    {
                        /* Configure key-editor validator: */
                        connect(pEditorKey, SIGNAL(editTextChanged(const QString&)),
                                pKeyValidator, SLOT(sltValidate(QString)));
                        /* Add key-editor validator into dialog validator group: */
                        pValidatorGroup->addObjectValidator(pKeyValidator);
                    }
                    /* Add key-editor into input-layout: */
                    pInputLayout->addWidget(pEditorKey, 0, 1);
                }
                /* Create value-label: */
                QLabel *pLabelValue = new QLabel("&Value:");
                {
                    /* Configure value-label: */
                    pLabelValue->setAlignment(Qt::AlignRight);
                    /* Add value-label into input-layout: */
                    pInputLayout->addWidget(pLabelValue, 1, 0);
                }
                /* Create value-editor: */
                QLineEdit *pEditorValue = new QLineEdit;
                {
                    /* Configure value-editor: */
                    pLabelValue->setBuddy(pEditorValue);
                    /* Create value-editor property setter: */
                    QObjectPropertySetter *pValuePropertySetter = new QObjectPropertySetter(pInputDialog, "Value");
                    AssertPtrReturnVoid(pValuePropertySetter);
                    {
                        /* Configure value-editor property setter: */
                        connect(pEditorValue, SIGNAL(textEdited(const QString&)),
                                pValuePropertySetter, SLOT(sltAssignProperty(const QString&)));
                    }
                    /* Create value-editor validator: */
                    QObjectValidator *pValueValidator = new QObjectValidator(new QRegExpValidator(QRegExp("[\\s\\S]+"), this));
                    AssertPtrReturnVoid(pValueValidator);
                    {
                        /* Configure value-editor validator: */
                        connect(pEditorValue, SIGNAL(textEdited(const QString&)),
                                pValueValidator, SLOT(sltValidate(QString)));
                        /* Add value-editor validator into dialog validator group: */
                        pValidatorGroup->addObjectValidator(pValueValidator);
                    }
                    /* Add value-editor into input-layout: */
                    pInputLayout->addWidget(pEditorValue, 1, 1);
                }
                /* Add input-layout into main-layout: */
                pMainLayout->addLayout(pInputLayout);
            }
            /* Create stretch: */
            pMainLayout->addStretch();
            /* Create dialog button-box: */
            QIDialogButtonBox *pButtonBox = new QIDialogButtonBox;
            AssertPtrReturnVoid(pButtonBox);
            {
                /* Configure button-box: */
                pButtonBox->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
                pButtonBox->button(QDialogButtonBox::Ok)->setAutoDefault(true);
                pButtonBox->button(QDialogButtonBox::Ok)->setEnabled(pValidatorGroup->result());
                pButtonBox->button(QDialogButtonBox::Cancel)->setShortcut(Qt::Key_Escape);
                connect(pValidatorGroup, SIGNAL(sigValidityChange(bool)),
                        pButtonBox->button(QDialogButtonBox::Ok), SLOT(setEnabled(bool)));
                connect(pButtonBox, SIGNAL(accepted()), pInputDialog, SLOT(accept()));
                connect(pButtonBox, SIGNAL(rejected()), pInputDialog, SLOT(reject()));
                /* Add button-box into main-layout: */
                pMainLayout->addWidget(pButtonBox);
            }
        }
    }

    /* Execute input-dialog: */
    if (pInputDialog->exec() == QDialog::Accepted)
    {
        gEDataManager->setExtraDataString(pInputDialog->property("Key").toString(),
                                          pInputDialog->property("Value").toString(),
                                          currentChooserID());
    }

    /* Destroy input-dialog: */
    if (pInputDialog)
        delete pInputDialog;
}

void UIExtraDataManagerWindow::sltDel()
{
    /* Make sure this slot called by corresponding action only: */
    QAction *pSenderAction = qobject_cast<QAction*>(sender());
    AssertReturnVoid(pSenderAction && m_pActionDel);

    /* Gather the map of chosen items: */
    QMap<QString, QString> items;
    foreach (const QModelIndex &keyIndex, m_pViewOfData->selectionModel()->selectedRows(0))
        items.insert(keyIndex.data().toString(), dataValueIndex(keyIndex.row()).data().toString());

    /* Prepare details: */
    const QString strTableTemplate("<!--EOM--><table border=0 cellspacing=10 cellpadding=0 width=500>%1</table>");
    const QString strRowTemplate("<tr><td><tt>%1</tt></td><td align=right><tt>%2</tt></td></tr>");
    QString strDetails;
    foreach (const QString &strKey, items.keys())
        strDetails += strRowTemplate.arg(strKey, items.value(strKey));
    strDetails = strTableTemplate.arg(strDetails);

    /* Ask for user' confirmation: */
    if (!msgCenter().errorWithQuestion(this, MessageType_Question,
                                       QString("<p>Do you really wish to "
                                               "remove chosen records?</p>"),
                                       strDetails))
        return;

    /* Erase all the chosen extra-data records: */
    foreach (const QString &strKey, items.keys())
        gEDataManager->setExtraDataString(strKey, QString(), currentChooserID());
}

void UIExtraDataManagerWindow::sltSave()
{
    /* Make sure this slot called by corresponding action only: */
    QAction *pSenderAction = qobject_cast<QAction*>(sender());
    AssertReturnVoid(pSenderAction && m_pActionSave);

    /* Compose initial file-name: */
    const QString strInitialFileName = QDir(vboxGlobal().homeFolder()).absoluteFilePath(QString("%1_ExtraData.xml").arg(currentChooserName()));
    /* Open file-save dialog to choose file to save extra-data into: */
    const QString strFileName = QIFileDialog::getSaveFileName(strInitialFileName, "XML files (*.xml)", this,
                                                              "Choose file to save extra-data into..", 0, true, true);
    /* Make sure file-name was chosen: */
    if (strFileName.isEmpty())
        return;

    /* Create file: */
    QFile output(strFileName);
    /* Open file for writing: */
    bool fOpened = output.open(QIODevice::WriteOnly);
    AssertReturnVoid(fOpened);
    {
        /* Create XML stream writer: */
        QXmlStreamWriter stream(&output);
        /* Configure XML stream writer: */
        stream.setAutoFormatting(true);
        stream.setAutoFormattingIndent(2);
        /* Write document: */
        stream.writeStartDocument();
        {
            stream.writeStartElement("VirtualBox");
            {
                const QString strID = currentChooserID();
                bool fIsMachine = strID != UIExtraDataManager::GlobalID;
                const QString strType = fIsMachine ? "Machine" : "Global";
                stream.writeStartElement(strType);
                {
                    if (fIsMachine)
                        stream.writeAttribute("uuid", QString("{%1}").arg(strID));
                    stream.writeStartElement("ExtraData");
                    {
                        /* Called from context-menu: */
                        if (pSenderAction->property("CalledFromContextMenu").toBool() &&
                            !m_pViewOfData->selectionModel()->selection().isEmpty())
                        {
                            foreach (const QModelIndex &keyIndex, m_pViewOfData->selectionModel()->selectedRows())
                            {
                                /* Get data-value index: */
                                const QModelIndex valueIndex = dataValueIndex(keyIndex.row());
                                /* Write corresponding extra-data item into stream: */
                                stream.writeStartElement("ExtraDataItem");
                                {
                                    stream.writeAttribute("name", keyIndex.data().toString());
                                    stream.writeAttribute("value", valueIndex.data().toString());
                                }
                                stream.writeEndElement(); /* ExtraDataItem */
                            }
                        }
                        /* Called from menu-bar/tool-bar: */
                        else
                        {
                            for (int iRow = 0; iRow < m_pModelProxyOfData->rowCount(); ++iRow)
                            {
                                /* Get indexes: */
                                const QModelIndex keyIndex = m_pModelProxyOfData->index(iRow, 0);
                                const QModelIndex valueIndex = m_pModelProxyOfData->index(iRow, 1);
                                /* Write corresponding extra-data item into stream: */
                                stream.writeStartElement("ExtraDataItem");
                                {
                                    stream.writeAttribute("name", keyIndex.data().toString());
                                    stream.writeAttribute("value", valueIndex.data().toString());
                                }
                                stream.writeEndElement(); /* ExtraDataItem */
                            }
                        }
                    }
                    stream.writeEndElement(); /* ExtraData */
                }
                stream.writeEndElement(); /* strType */
            }
            stream.writeEndElement(); /* VirtualBox */
        }
        stream.writeEndDocument();
        /* Close file: */
        output.close();
    }
}

void UIExtraDataManagerWindow::sltLoad()
{
    /* Make sure this slot called by corresponding action only: */
    QAction *pSenderAction = qobject_cast<QAction*>(sender());
    AssertReturnVoid(pSenderAction && m_pActionLoad);

    /* Compose initial file-name: */
    const QString strInitialFileName = QDir(vboxGlobal().homeFolder()).absoluteFilePath(QString("%1_ExtraData.xml").arg(currentChooserName()));
    /* Open file-open dialog to choose file to open extra-data into: */
    const QString strFileName = QIFileDialog::getOpenFileName(strInitialFileName, "XML files (*.xml)", this,
                                                              "Choose file to load extra-data from..");
    /* Make sure file-name was chosen: */
    if (strFileName.isEmpty())
        return;

    /* Create file: */
    QFile input(strFileName);
    /* Open file for writing: */
    bool fOpened = input.open(QIODevice::ReadOnly);
    AssertReturnVoid(fOpened);
    {
        /* Create XML stream reader: */
        QXmlStreamReader stream(&input);
        /* Read XML stream: */
        while (!stream.atEnd())
        {
            /* Read subsequent token: */
            const QXmlStreamReader::TokenType tokenType = stream.readNext();
            /* Skip non-interesting tokens: */
            if (tokenType != QXmlStreamReader::StartElement)
                continue;

            /* Get the name of the current element: */
            const QStringRef strElementName = stream.name();

            /* Search for the scope ID: */
            QString strLoadingID;
            if (strElementName == "Global")
                strLoadingID = UIExtraDataManager::GlobalID;
            else if (strElementName == "Machine")
            {
                const QXmlStreamAttributes attributes = stream.attributes();
                if (attributes.hasAttribute("uuid"))
                {
                    const QString strUuid = attributes.value("uuid").toString();
                    const QUuid uuid = strUuid;
                    if (!uuid.isNull())
                        strLoadingID = uuid.toString().remove(QRegExp("[{}]"));
                    else
                        msgCenter().alert(this, MessageType_Warning,
                                          QString("<p>Invalid extra-data ID:</p>"
                                                  "<p>%1</p>").arg(strUuid));
                }
            }
            /* Look particular extra-data entries: */
            else if (strElementName == "ExtraDataItem")
            {
                const QXmlStreamAttributes attributes = stream.attributes();
                if (attributes.hasAttribute("name") && attributes.hasAttribute("value"))
                {
                    const QString strName = attributes.value("name").toString();
                    const QString strValue = attributes.value("value").toString();
                    gEDataManager->setExtraDataString(strName, strValue, currentChooserID());
                }
            }

            /* Check extra-data ID: */
            if (!strLoadingID.isNull() && strLoadingID != currentChooserID() &&
                !msgCenter().questionBinary(this, MessageType_Question,
                                            QString("<p>Inconsistent extra-data ID:</p>"
                                                    "<p>Current: {%1}</p>"
                                                    "<p>Loading: {%2}</p>"
                                                    "<p>Continue with loading?</p>")
                                                    .arg(currentChooserID(), strLoadingID)))
                break;
        }
        /* Handle XML stream error: */
        if (stream.hasError())
            msgCenter().alert(this, MessageType_Warning,
                              QString("<p>Error reading XML file:</p>"
                                      "<p>%1</p>").arg(stream.error()));
        /* Close file: */
        input.close();
    }
}

void UIExtraDataManagerWindow::prepare()
{
    /* Prepare this: */
    prepareThis();
    /* Prepare connections: */
    prepareConnections();
    /* Prepare menu: */
    prepareMenu();
    /* Prepare central-widget: */
    prepareCentralWidget();
    /* Load settings: */
    loadSettings();
}

void UIExtraDataManagerWindow::prepareThis()
{
#ifndef Q_WS_MAC
    /* Apply window icons: */
    setWindowIcon(UIIconPool::iconSetFull(":/edataman_32px.png",
                                          ":/edataman_16px.png"));
#endif /* !Q_WS_MAC */

    /* Apply window title: */
    setWindowTitle("Extra-data Manager");

    /* Do not count that window as important for application,
     * it will NOT be taken into account when other top-level windows will be closed: */
    setAttribute(Qt::WA_QuitOnClose, false);

    /* Delete window when closed: */
    setAttribute(Qt::WA_DeleteOnClose);
}

void UIExtraDataManagerWindow::prepareConnections()
{
    /* Prepare connections: */
    connect(gVBoxEvents, SIGNAL(sigMachineRegistered(QString, bool)),
            this, SLOT(sltMachineRegistered(QString, bool)));
}

void UIExtraDataManagerWindow::prepareMenu()
{
    /* Create 'Actions' menu: */
    QMenu *pActionsMenu = menuBar()->addMenu("Actions");
    AssertReturnVoid(pActionsMenu);
    {
        /* Create 'Add' action: */
        m_pActionAdd = pActionsMenu->addAction("Add");
        AssertReturnVoid(m_pActionAdd);
        {
            /* Configure 'Add' action: */
            m_pActionAdd->setIcon(UIIconPool::iconSetFull(":/edata_add_22px.png", ":/edata_add_16px.png",
                                                          ":/edata_add_disabled_22px.png", ":/edata_add_disabled_16px.png"));
            m_pActionAdd->setShortcut(QKeySequence("Ctrl+T"));
            connect(m_pActionAdd, SIGNAL(triggered(bool)), this, SLOT(sltAdd()));
        }
        /* Create 'Del' action: */
        m_pActionDel = pActionsMenu->addAction("Remove");
        AssertReturnVoid(m_pActionDel);
        {
            /* Configure 'Del' action: */
            m_pActionDel->setIcon(UIIconPool::iconSetFull(":/edata_remove_22px.png", ":/edata_remove_16px.png",
                                                          ":/edata_remove_disabled_22px.png", ":/edata_remove_disabled_16px.png"));
            m_pActionDel->setShortcut(QKeySequence("Ctrl+R"));
            connect(m_pActionDel, SIGNAL(triggered(bool)), this, SLOT(sltDel()));
        }

        /* Add separator: */
        pActionsMenu->addSeparator();

        /* Create 'Load' action: */
        m_pActionLoad = pActionsMenu->addAction("Load");
        AssertReturnVoid(m_pActionLoad);
        {
            /* Configure 'Load' action: */
            m_pActionLoad->setIcon(UIIconPool::iconSetFull(":/edata_load_22px.png", ":/edata_load_16px.png",
                                                           ":/edata_load_disabled_22px.png", ":/edata_load_disabled_16px.png"));
            m_pActionLoad->setShortcut(QKeySequence("Ctrl+L"));
            connect(m_pActionLoad, SIGNAL(triggered(bool)), this, SLOT(sltLoad()));
        }
        /* Create 'Save' action: */
        m_pActionSave = pActionsMenu->addAction("Save As...");
        AssertReturnVoid(m_pActionSave);
        {
            /* Configure 'Save' action: */
            m_pActionSave->setIcon(UIIconPool::iconSetFull(":/edata_save_22px.png", ":/edata_save_16px.png",
                                                           ":/edata_save_disabled_22px.png", ":/edata_save_disabled_16px.png"));
            m_pActionSave->setShortcut(QKeySequence("Ctrl+S"));
            connect(m_pActionSave, SIGNAL(triggered(bool)), this, SLOT(sltSave()));
        }
    }
}

void UIExtraDataManagerWindow::prepareCentralWidget()
{
    /* Prepare central-widget: */
    setCentralWidget(new QWidget);
    AssertPtrReturnVoid(centralWidget());
    {
        /* Prepare layout: */
        m_pMainLayout = new QVBoxLayout(centralWidget());
        AssertReturnVoid(m_pMainLayout && centralWidget()->layout() &&
                         m_pMainLayout == centralWidget()->layout());
        {
#if MAC_LEOPARD_STYLE
            /* No spacing/margins on the Mac: */
            m_pMainLayout->setContentsMargins(0, 0, 0, 0);
            m_pMainLayout->insertSpacing(0, 10);
#else /* !MAC_LEOPARD_STYLE */
            /* Set spacing/margin like in the selector window: */
            m_pMainLayout->setSpacing(5);
            m_pMainLayout->setContentsMargins(5, 5, 5, 5);
#endif /* !MAC_LEOPARD_STYLE */
            /* Prepare tool-bar: */
            prepareToolBar();
            /* Prepare splitter: */
            prepareSplitter();
            /* Prepare button-box: */
            prepareButtonBox();
        }
        /* Initial focus: */
        if (m_pViewOfChooser)
            m_pViewOfChooser->setFocus();
    }
}

void UIExtraDataManagerWindow::prepareToolBar()
{
    /* Create tool-bar: */
    m_pToolBar = new UIToolBar(this);
    AssertPtrReturnVoid(m_pToolBar);
    {
        /* Configure tool-bar: */
        m_pToolBar->setIconSize(QSize(22, 22));
        m_pToolBar->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        /* Add actions: */
        m_pToolBar->addAction(m_pActionAdd);
        m_pToolBar->addAction(m_pActionDel);
        m_pToolBar->addSeparator();
        m_pToolBar->addAction(m_pActionLoad);
        m_pToolBar->addAction(m_pActionSave);
        /* Integrate tool-bar into dialog: */
#if MAC_LEOPARD_STYLE
        /* Enable unified tool-bars on Mac OS X. Available on Qt >= 4.3: */
        addToolBar(m_pToolBar);
        m_pToolBar->enableMacToolbar();
#else /* !MAC_LEOPARD_STYLE */
        /* Add tool-bar into main-layout: */
        m_pMainLayout->addWidget(m_pToolBar);
#endif /* !MAC_LEOPARD_STYLE */
    }
}

void UIExtraDataManagerWindow::prepareSplitter()
{
    /* Create splitter: */
    m_pSplitter = new QISplitter;
    AssertPtrReturnVoid(m_pSplitter);
    {
        /* Prepare panes: */
        preparePanes();
        /* Configure splitter: */
        m_pSplitter->setChildrenCollapsible(false);
        m_pSplitter->setStretchFactor(0, 0);
        m_pSplitter->setStretchFactor(1, 1);
        /* Add splitter into main layout: */
        m_pMainLayout->addWidget(m_pSplitter);
    }
}

void UIExtraDataManagerWindow::preparePanes()
{
    /* Prepare chooser-pane: */
    preparePaneChooser();
    /* Prepare data-pane: */
    preparePaneData();
    /* Link chooser and data panes: */
    connect(m_pViewOfChooser->selectionModel(),
            SIGNAL(currentChanged(const QModelIndex&, const QModelIndex&)),
            this, SLOT(sltChooserHandleCurrentChanged(const QModelIndex&)));
    connect(m_pViewOfChooser->selectionModel(),
            SIGNAL(selectionChanged(const QItemSelection&, const QItemSelection&)),
            this, SLOT(sltChooserHandleSelectionChanged(const QItemSelection&, const QItemSelection&)));
    connect(m_pViewOfData->selectionModel(),
            SIGNAL(selectionChanged(const QItemSelection&, const QItemSelection&)),
            this, SLOT(sltDataHandleSelectionChanged(const QItemSelection&, const QItemSelection&)));
    connect(m_pModelSourceOfData, SIGNAL(itemChanged(QStandardItem*)),
            this, SLOT(sltDataHandleItemChanged(QStandardItem*)));
    /* Make sure chooser have current-index if possible: */
    makeSureChooserHaveCurrentIndexIfPossible();
}

void UIExtraDataManagerWindow::preparePaneChooser()
{
    /* Create chooser-pane: */
    m_pPaneOfChooser = new QWidget;
    AssertPtrReturnVoid(m_pPaneOfChooser);
    {
        /* Create layout: */
        QVBoxLayout *pLayout = new QVBoxLayout(m_pPaneOfChooser);
        AssertReturnVoid(pLayout && m_pPaneOfChooser->layout() &&
                         pLayout == m_pPaneOfChooser->layout());
        {
            /* Configure layout: */
            pLayout->setContentsMargins(0, 0, 3, 0);
            /* Create chooser-filter: */
            m_pFilterOfChooser = new QLineEdit;
            {
                /* Configure chooser-filter: */
#if QT_VERSION >= 0x040700
                m_pFilterOfChooser->setPlaceholderText("Search..");
#endif /* QT_VERSION >= 0x040700 */
                connect(m_pFilterOfChooser, SIGNAL(textChanged(const QString&)),
                        this, SLOT(sltChooserApplyFilter(const QString&)));
                /* Add chooser-filter into layout: */
                pLayout->addWidget(m_pFilterOfChooser);
            }
            /* Create chooser-view: */
            m_pViewOfChooser = new QListView;
            AssertPtrReturnVoid(m_pViewOfChooser);
            {
                /* Configure chooser-view: */
                delete m_pViewOfChooser->itemDelegate();
                m_pViewOfChooser->setItemDelegate(new UIChooserPaneDelegate(m_pViewOfChooser));
                m_pViewOfChooser->setSelectionMode(QAbstractItemView::SingleSelection);
                /* Create source-model: */
                m_pModelSourceOfChooser = new QStandardItemModel(m_pViewOfChooser);
                AssertPtrReturnVoid(m_pModelSourceOfChooser);
                {
                    /* Create proxy-model: */
                    m_pModelProxyOfChooser = new UIChooserPaneSortingModel(m_pViewOfChooser);
                    AssertPtrReturnVoid(m_pModelProxyOfChooser);
                    {
                        /* Configure proxy-model: */
                        m_pModelProxyOfChooser->setSortRole(Field_Name);
                        m_pModelProxyOfChooser->setFilterRole(Field_Name);
                        m_pModelProxyOfChooser->setSortCaseSensitivity(Qt::CaseInsensitive);
                        m_pModelProxyOfChooser->setFilterCaseSensitivity(Qt::CaseInsensitive);
                        m_pModelProxyOfChooser->setSourceModel(m_pModelSourceOfChooser);
                        m_pViewOfChooser->setModel(m_pModelProxyOfChooser);
                    }
                    /* Add global chooser item into source-model: */
                    addChooserItemByID(UIExtraDataManager::GlobalID);
                    /* Add machine chooser items into source-model: */
                    CMachineVector machines = vboxGlobal().virtualBox().GetMachines();
                    foreach (const CMachine &machine, machines)
                        addChooserItemByMachine(machine);
                    /* And sort proxy-model: */
                    m_pModelProxyOfChooser->sort(0, Qt::AscendingOrder);
                }
                /* Add chooser-view into layout: */
                pLayout->addWidget(m_pViewOfChooser);
            }
        }
        /* Add chooser-pane into splitter: */
        m_pSplitter->addWidget(m_pPaneOfChooser);
    }
}

void UIExtraDataManagerWindow::preparePaneData()
{
    /* Create data-pane: */
    m_pPaneOfData = new QWidget;
    AssertPtrReturnVoid(m_pPaneOfData);
    {
        /* Create layout: */
        QVBoxLayout *pLayout = new QVBoxLayout(m_pPaneOfData);
        AssertReturnVoid(pLayout && m_pPaneOfData->layout() &&
                         pLayout == m_pPaneOfData->layout());
        {
            /* Configure layout: */
            pLayout->setContentsMargins(3, 0, 0, 0);
            /* Create data-filter: */
            m_pFilterOfData = new QLineEdit;
            {
                /* Configure data-filter: */
#if QT_VERSION >= 0x040700
                m_pFilterOfData->setPlaceholderText("Search..");
#endif /* QT_VERSION >= 0x040700 */
                connect(m_pFilterOfData, SIGNAL(textChanged(const QString&)),
                        this, SLOT(sltDataApplyFilter(const QString&)));
                /* Add data-filter into layout: */
                pLayout->addWidget(m_pFilterOfData);
            }
            /* Create data-view: */
            m_pViewOfData = new QTableView;
            AssertPtrReturnVoid(m_pViewOfData);
            {
                /* Create item-model: */
                m_pModelSourceOfData = new QStandardItemModel(0, 2, m_pViewOfData);
                AssertPtrReturnVoid(m_pModelSourceOfData);
                {
                    /* Create proxy-model: */
                    m_pModelProxyOfData = new QSortFilterProxyModel(m_pViewOfChooser);
                    AssertPtrReturnVoid(m_pModelProxyOfData);
                    {
                        /* Configure proxy-model: */
                        m_pModelProxyOfData->setSortCaseSensitivity(Qt::CaseInsensitive);
                        m_pModelProxyOfData->setFilterCaseSensitivity(Qt::CaseInsensitive);
                        m_pModelProxyOfData->setSourceModel(m_pModelSourceOfData);
                        m_pViewOfData->setModel(m_pModelProxyOfData);
                    }
                    /* Configure item-model: */
                    m_pModelSourceOfData->setHorizontalHeaderLabels(QStringList() << "Key" << "Value");
                }
                /* Configure data-view: */
                m_pViewOfData->setSortingEnabled(true);
                m_pViewOfData->setAlternatingRowColors(true);
                m_pViewOfData->setContextMenuPolicy(Qt::CustomContextMenu);
                m_pViewOfData->setSelectionMode(QAbstractItemView::ExtendedSelection);
                m_pViewOfData->setSelectionBehavior(QAbstractItemView::SelectRows);
                connect(m_pViewOfData, SIGNAL(customContextMenuRequested(const QPoint&)),
                        this, SLOT(sltDataHandleCustomContextMenuRequested(const QPoint&)));
                QHeaderView *pVHeader = m_pViewOfData->verticalHeader();
                QHeaderView *pHHeader = m_pViewOfData->horizontalHeader();
                pVHeader->hide();
                pHHeader->setSortIndicator(0, Qt::AscendingOrder);
                pHHeader->resizeSection(0, qMin(300, pHHeader->width() / 3));
                pHHeader->setStretchLastSection(true);
                /* Add data-view into layout: */
                pLayout->addWidget(m_pViewOfData);
            }
        }
        /* Add data-pane into splitter: */
        m_pSplitter->addWidget(m_pPaneOfData);
    }
}

void UIExtraDataManagerWindow::prepareButtonBox()
{
    /* Create button-box: */
    m_pButtonBox = new QIDialogButtonBox;
    AssertPtrReturnVoid(m_pButtonBox);
    {
        /* Configure button-box: */
        m_pButtonBox->setStandardButtons(QDialogButtonBox::Help | QDialogButtonBox::Close);
        m_pButtonBox->button(QDialogButtonBox::Close)->setShortcut(Qt::Key_Escape);
        connect(m_pButtonBox, SIGNAL(helpRequested()), &msgCenter(), SLOT(sltShowHelpHelpDialog()));
        connect(m_pButtonBox, SIGNAL(rejected()), this, SLOT(close()));
        /* Add button-box into main layout: */
        m_pMainLayout->addWidget(m_pButtonBox);
    }
}

void UIExtraDataManagerWindow::loadSettings()
{
    /* Load window geometry: */
    {
        /* Load geometry: */
        m_geometry = gEDataManager->extraDataManagerGeometry(this);
#ifdef Q_WS_MAC
        move(m_geometry.topLeft());
        resize(m_geometry.size());
#else /* Q_WS_MAC */
        setGeometry(m_geometry);
#endif /* !Q_WS_MAC */
        LogRel(("GUI: UIExtraDataManagerWindow: Geometry loaded to: %dx%d @ %dx%d\n",
                m_geometry.x(), m_geometry.y(), m_geometry.width(), m_geometry.height()));

        /* Maximize (if necessary): */
        if (gEDataManager->extraDataManagerShouldBeMaximized())
            showMaximized();
    }

    /* Load splitter hints: */
    {
        m_pSplitter->setSizes(gEDataManager->extraDataManagerSplitterHints(this));
    }
}

void UIExtraDataManagerWindow::saveSettings()
{
    /* Save splitter hints: */
    {
        gEDataManager->setExtraDataManagerSplitterHints(m_pSplitter->sizes());
    }

    /* Save window geometry: */
    {
        /* Save geometry: */
#ifdef Q_WS_MAC
        gEDataManager->setExtraDataManagerGeometry(m_geometry, ::darwinIsWindowMaximized(this));
#else /* Q_WS_MAC */
        gEDataManager->setExtraDataManagerGeometry(m_geometry, isMaximized());
#endif /* !Q_WS_MAC */
        LogRel(("GUI: UIExtraDataManagerWindow: Geometry saved as: %dx%d @ %dx%d\n",
                m_geometry.x(), m_geometry.y(), m_geometry.width(), m_geometry.height()));
    }
}

void UIExtraDataManagerWindow::cleanup()
{
    /* Save settings: */
    saveSettings();
}

bool UIExtraDataManagerWindow::event(QEvent *pEvent)
{
    /* Pre-process through base-class: */
    bool fResult = QMainWindow::event(pEvent);

    /* Process required events: */
    switch (pEvent->type())
    {
        /* Handle every Resize and Move we keep track of the geometry. */
        case QEvent::Resize:
        {
            if (isVisible() && (windowState() & (Qt::WindowMaximized | Qt::WindowMinimized | Qt::WindowFullScreen)) == 0)
            {
                QResizeEvent *pResizeEvent = static_cast<QResizeEvent*>(pEvent);
                m_geometry.setSize(pResizeEvent->size());
            }
            break;
        }
        case QEvent::Move:
        {
            if (isVisible() && (windowState() & (Qt::WindowMaximized | Qt::WindowMinimized | Qt::WindowFullScreen)) == 0)
            {
#ifdef Q_WS_MAC
                QMoveEvent *pMoveEvent = static_cast<QMoveEvent*>(pEvent);
                m_geometry.moveTo(pMoveEvent->pos());
#else /* !Q_WS_MAC */
                m_geometry.moveTo(geometry().x(), geometry().y());
#endif /* !Q_WS_MAC */
            }
            break;
        }
        default:
            break;
    }

    /* Return result: */
    return fResult;
}

void UIExtraDataManagerWindow::updateActionsAvailability()
{
    /* Is there something selected in chooser-view? */
    bool fChooserHasSelection = !m_pViewOfChooser->selectionModel()->selection().isEmpty();
    /* Is there something selected in data-view? */
    bool fDataHasSelection = !m_pViewOfData->selectionModel()->selection().isEmpty();

    /* Enable/disable corresponding actions: */
    m_pActionAdd->setEnabled(fChooserHasSelection);
    m_pActionDel->setEnabled(fChooserHasSelection && fDataHasSelection);
    m_pActionLoad->setEnabled(fChooserHasSelection);
    m_pActionSave->setEnabled(fChooserHasSelection);
}

QModelIndex UIExtraDataManagerWindow::chooserIndex(int iRow) const
{
    return m_pModelSourceOfChooser->index(iRow, 0);
}

QModelIndex UIExtraDataManagerWindow::currentChooserIndex() const
{
    return m_pViewOfChooser->currentIndex();
}

QString UIExtraDataManagerWindow::chooserID(int iRow) const
{
    return chooserIndex(iRow).data(Field_ID).toString();
}

QString UIExtraDataManagerWindow::currentChooserID() const
{
    return currentChooserIndex().data(Field_ID).toString();
}

QString UIExtraDataManagerWindow::chooserName(int iRow) const
{
    return chooserIndex(iRow).data(Field_Name).toString();
}

QString UIExtraDataManagerWindow::currentChooserName() const
{
    return currentChooserIndex().data(Field_Name).toString();
}

void UIExtraDataManagerWindow::addChooserItem(const QString &strID,
                                              const QString &strName,
                                              const QString &strOsTypeID,
                                              const int iPosition /* = -1 */)
{
    /* Create item: */
    QStandardItem *pItem = new QStandardItem;
    AssertPtrReturnVoid(pItem);
    {
        /* Which is NOT editable: */
        pItem->setEditable(false);
        /* Contains passed ID: */
        pItem->setData(strID, Field_ID);
        /* Contains passed name: */
        pItem->setData(strName, Field_Name);
        /* Contains passed OS Type ID: */
        pItem->setData(strOsTypeID, Field_OsTypeID);
        /* And designated as known/unknown depending on extra-data manager status: */
        pItem->setData(gEDataManager->contains(strID), Field_Known);
        /* If insert position defined: */
        if (iPosition != -1)
        {
            /* Insert this item at specified position: */
            m_pModelSourceOfChooser->insertRow(iPosition, pItem);
        }
        /* If insert position undefined: */
        else
        {
            /* Add this item as the last one: */
            m_pModelSourceOfChooser->appendRow(pItem);
        }
    }
}

void UIExtraDataManagerWindow::addChooserItemByMachine(const CMachine &machine,
                                                       const int iPosition /* = -1 */)
{
    /* Make sure VM is accessible: */
    if (!machine.isNull() && machine.GetAccessible())
        return addChooserItem(machine.GetId(), machine.GetName(), machine.GetOSTypeId(), iPosition);
}

void UIExtraDataManagerWindow::addChooserItemByID(const QString &strID,
                                                  const int iPosition /* = -1 */)
{
    /* Global ID? */
    if (strID == UIExtraDataManager::GlobalID)
        return addChooserItem(strID, QString("Global"), QString(), iPosition);

    /* Search for the corresponding machine by ID: */
    CVirtualBox vbox = vboxGlobal().virtualBox();
    const CMachine machine = vbox.FindMachine(strID);
    /* Make sure VM is accessible: */
    if (vbox.isOk() && !machine.isNull() && machine.GetAccessible())
        return addChooserItem(strID, machine.GetName(), machine.GetOSTypeId(), iPosition);
}

void UIExtraDataManagerWindow::makeSureChooserHaveCurrentIndexIfPossible()
{
    /* Make sure chooser have current-index if possible: */
    if (!m_pViewOfChooser->currentIndex().isValid())
    {
        /* Do we still have anything to select? */
        const QModelIndex firstIndex = m_pModelProxyOfChooser->index(0, 0);
        if (firstIndex.isValid())
            m_pViewOfChooser->setCurrentIndex(firstIndex);
    }
}

QModelIndex UIExtraDataManagerWindow::dataIndex(int iRow, int iColumn) const
{
    return m_pModelSourceOfData->index(iRow, iColumn);
}

QModelIndex UIExtraDataManagerWindow::dataKeyIndex(int iRow) const
{
    return dataIndex(iRow, 0);
}

QModelIndex UIExtraDataManagerWindow::dataValueIndex(int iRow) const
{
    return dataIndex(iRow, 1);
}

QString UIExtraDataManagerWindow::dataKey(int iRow) const
{
    return dataKeyIndex(iRow).data().toString();
}

QString UIExtraDataManagerWindow::dataValue(int iRow) const
{
    return dataValueIndex(iRow).data().toString();
}

void UIExtraDataManagerWindow::addDataItem(const QString &strKey,
                                           const QString &strValue,
                                           const int iPosition /* = -1 */)
{
    /* Prepare items: */
    QList<QStandardItem*> items;
    /* Create key item: */
    items << new QStandardItem(strKey);
    items.last()->setEditable(false);
    AssertPtrReturnVoid(items.last());
    /* Create value item: */
    items << new QStandardItem(strValue);
    AssertPtrReturnVoid(items.last());
    /* If insert position defined: */
    if (iPosition != -1)
    {
        /* Insert these items as the row at the required position: */
        m_pModelSourceOfData->insertRow(iPosition, items);
    }
    /* If insert position undefined: */
    else
    {
        /* Add these items as the last one row: */
        m_pModelSourceOfData->appendRow(items);
    }
}

void UIExtraDataManagerWindow::sortData()
{
    /* Sort using current rules: */
    const QHeaderView *pHHeader = m_pViewOfData->horizontalHeader();
    const int iSortSection = pHHeader->sortIndicatorSection();
    const Qt::SortOrder sortOrder = pHHeader->sortIndicatorOrder();
    m_pModelProxyOfData->sort(iSortSection, sortOrder);
}

/* static */
QStringList UIExtraDataManagerWindow::knownExtraDataKeys()
{
    return QStringList()
           << QString()
           << GUI_LanguageId
           << GUI_SuppressMessages << GUI_InvertMessageOption
#ifdef VBOX_GUI_WITH_NETWORK_MANAGER
           << GUI_PreventApplicationUpdate << GUI_UpdateDate << GUI_UpdateCheckCount
#endif /* VBOX_GUI_WITH_NETWORK_MANAGER */
           << GUI_RestrictedGlobalSettingsPages << GUI_RestrictedMachineSettingsPages
           << GUI_ActivateHoveredMachineWindow
           << GUI_Input_SelectorShortcuts << GUI_Input_MachineShortcuts
           << GUI_RecentFolderHD << GUI_RecentFolderCD << GUI_RecentFolderFD
           << GUI_RecentListHD << GUI_RecentListCD << GUI_RecentListFD
           << GUI_LastSelectorWindowPosition << GUI_SplitterSizes
           << GUI_Toolbar << GUI_Statusbar
           << GUI_GroupDefinitions << GUI_LastItemSelected
           << GUI_DetailsPageBoxes << GUI_PreviewUpdate
           << GUI_HideDescriptionForWizards
           << GUI_HideFromManager << GUI_HideDetails
           << GUI_PreventReconfiguration << GUI_PreventSnapshotOperations
           << GUI_FirstRun
#ifndef Q_WS_MAC
           << GUI_MachineWindowIcons << GUI_MachineWindowNamePostfix
#endif /* !Q_WS_MAC */
           << GUI_LastNormalWindowPosition << GUI_LastScaleWindowPosition
           << GUI_RestrictedRuntimeMenus
           << GUI_RestrictedRuntimeApplicationMenuActions
           << GUI_RestrictedRuntimeMachineMenuActions
           << GUI_RestrictedRuntimeViewMenuActions
           << GUI_RestrictedRuntimeInputMenuActions
           << GUI_RestrictedRuntimeDevicesMenuActions
#ifdef VBOX_WITH_DEBUGGER_GUI
           << GUI_RestrictedRuntimeDebuggerMenuActions
#endif /* VBOX_WITH_DEBUGGER_GUI */
#ifdef Q_WS_MAC
           << GUI_RestrictedRuntimeWindowMenuActions
#endif /* Q_WS_MAC */
           << GUI_RestrictedRuntimeHelpMenuActions
           << GUI_RestrictedVisualStates
           << GUI_Fullscreen << GUI_Seamless << GUI_Scale
#ifdef Q_WS_X11
           << GUI_Fullscreen_LegacyMode
#endif /* Q_WS_X11 */
           << GUI_AutoresizeGuest << GUI_LastVisibilityStatusForGuestScreen << GUI_LastGuestSizeHint
           << GUI_VirtualScreenToHostScreen << GUI_AutomountGuestScreens
#ifdef VBOX_WITH_VIDEOHWACCEL
           << GUI_Accelerate2D_StretchLinear
           << GUI_Accelerate2D_PixformatYV12 << GUI_Accelerate2D_PixformatUYVY
           << GUI_Accelerate2D_PixformatYUY2 << GUI_Accelerate2D_PixformatAYUV
#endif /* VBOX_WITH_VIDEOHWACCEL */
           << GUI_HiDPI_UnscaledOutput
           << GUI_HiDPI_Optimization
#ifndef Q_WS_MAC
           << GUI_ShowMiniToolBar << GUI_MiniToolBarAutoHide << GUI_MiniToolBarAlignment
#endif /* !Q_WS_MAC */
           << GUI_StatusBar_Enabled << GUI_RestrictedStatusBarIndicators << GUI_StatusBar_IndicatorOrder
#ifdef Q_WS_MAC
           << GUI_RealtimeDockIconUpdateEnabled << GUI_RealtimeDockIconUpdateMonitor
#endif /* Q_WS_MAC */
           << GUI_PassCAD
           << GUI_MouseCapturePolicy
           << GUI_GuruMeditationHandler
           << GUI_HidLedsSync
           << GUI_ScaleFactor << GUI_Scaling_Optimization
           << GUI_InformationWindowGeometry
           << GUI_DefaultCloseAction << GUI_RestrictedCloseActions
           << GUI_LastCloseAction << GUI_CloseActionHook
#ifdef VBOX_WITH_DEBUGGER_GUI
           << GUI_Dbg_Enabled << GUI_Dbg_AutoShow
#endif /* VBOX_WITH_DEBUGGER_GUI */
           << GUI_ExtraDataManager_Geometry << GUI_ExtraDataManager_SplitterHints
           << GUI_LogWindowGeometry;
}
#endif /* DEBUG */


/* static */
UIExtraDataManager *UIExtraDataManager::m_spInstance = 0;
const QString UIExtraDataManager::GlobalID = QUuid().toString().remove(QRegExp("[{}]"));

/* static */
UIExtraDataManager* UIExtraDataManager::instance()
{
    /* Create/prepare instance if not yet exists: */
    if (!m_spInstance)
    {
        new UIExtraDataManager;
        m_spInstance->prepare();
    }
    /* Return instance: */
    return m_spInstance;
}

/* static */
void UIExtraDataManager::destroy()
{
    /* Destroy/cleanup instance if still exists: */
    if (m_spInstance)
    {
        m_spInstance->cleanup();
        delete m_spInstance;
    }
}

#ifdef DEBUG
/* static */
void UIExtraDataManager::openWindow(QWidget *pCenterWidget)
{
    /* Pass to instance: */
    instance()->open(pCenterWidget);
}
#endif /* DEBUG */

void UIExtraDataManager::hotloadMachineExtraDataMap(const QString &strID)
{
    /* Make sure it is valid ID: */
    AssertMsgReturnVoid(!strID.isNull() && strID != GlobalID,
                        ("Invalid VM ID = {%s}\n", strID.toAscii().constData()));
    /* Which is not loaded yet: */
    AssertReturnVoid(!m_data.contains(strID));

    /* Search for corresponding machine: */
    CVirtualBox vbox = vboxGlobal().virtualBox();
    CMachine machine = vbox.FindMachine(strID);
    AssertReturnVoid(vbox.isOk() && !machine.isNull());

    /* Make sure at least empty map is created: */
    m_data[strID] = ExtraDataMap();

    /* Do not handle inaccessible machine: */
    if (!machine.GetAccessible())
        return;

    /* Load machine extra-data map: */
    foreach (const QString &strKey, machine.GetExtraDataKeys())
        m_data[strID][strKey] = machine.GetExtraData(strKey);

    /* Notifies about extra-data map acknowledged: */
    emit sigExtraDataMapAcknowledging(strID);
}

QString UIExtraDataManager::extraDataString(const QString &strKey, const QString &strID /* = GlobalID */)
{
    /* Hot-load machine extra-data map if necessary: */
    if (strID != GlobalID && !m_data.contains(strID))
        hotloadMachineExtraDataMap(strID);

    /* Read-only access corresponding map: */
    const ExtraDataMap data = m_data.value(strID);

    /* QString() if value was not set: */
    if (!data.contains(strKey))
        return QString();

    /* Returns corresponding value: */
    return data[strKey];
}

void UIExtraDataManager::setExtraDataString(const QString &strKey, const QString &strValue, const QString &strID /* = GlobalID */)
{
    /* Make sure VBoxSVC is available: */
    if (!vboxGlobal().isVBoxSVCAvailable())
        return;

    /* Hot-load machine extra-data map if necessary: */
    if (strID != GlobalID && !m_data.contains(strID))
        hotloadMachineExtraDataMap(strID);

    /* Access corresponding map: */
    ExtraDataMap &data = m_data[strID];

    /* [Re]cache passed value: */
    data[strKey] = strValue;

    /* Global extra-data: */
    if (strID == GlobalID)
    {
        /* Get global object: */
        CVirtualBox vbox = vboxGlobal().virtualBox();
        /* Update global extra-data: */
        vbox.SetExtraData(strKey, strValue);
        if (!vbox.isOk())
            msgCenter().cannotSetExtraData(vbox, strKey, strValue);
    }
    /* Machine extra-data: */
    else
    {
        /* Search for corresponding machine: */
        CVirtualBox vbox = vboxGlobal().virtualBox();
        const CMachine machine = vbox.FindMachine(strID);
        AssertReturnVoid(vbox.isOk() && !machine.isNull());
        /* Check the configuration access-level: */
        const KMachineState machineState = machine.GetState();
        const KSessionState sessionState = machine.GetSessionState();
        const ConfigurationAccessLevel cLevel = configurationAccessLevel(sessionState, machineState);
        /* Prepare machine session: */
        CSession session;
        if (cLevel == ConfigurationAccessLevel_Full)
            session = vboxGlobal().openSession(strID);
        else
            session = vboxGlobal().openExistingSession(strID);
        AssertReturnVoid(!session.isNull());
        /* Get machine from that session: */
        CMachine sessionMachine = session.GetMachine();
        /* Update machine extra-data: */
        sessionMachine.SetExtraData(strKey, strValue);
        if (!sessionMachine.isOk())
            msgCenter().cannotSetExtraData(sessionMachine, strKey, strValue);
        session.UnlockMachine();
    }
}

QStringList UIExtraDataManager::extraDataStringList(const QString &strKey, const QString &strID /* = GlobalID */)
{
    /* Hot-load machine extra-data map if necessary: */
    if (strID != GlobalID && !m_data.contains(strID))
        hotloadMachineExtraDataMap(strID);

    /* Read-only access corresponding map: */
    const ExtraDataMap data = m_data.value(strID);

    /* QStringList() if machine value was not set: */
    if (!data.contains(strKey))
        return QStringList();

    /* Few old extra-data string-lists were separated with 'semicolon' symbol.
     * All new separated by 'comma'. We have to take that into account. */
    return data[strKey].split(QRegExp("[;,]"), QString::SkipEmptyParts);
}

void UIExtraDataManager::setExtraDataStringList(const QString &strKey, const QStringList &value, const QString &strID /* = GlobalID */)
{
    /* Make sure VBoxSVC is available: */
    if (!vboxGlobal().isVBoxSVCAvailable())
        return;

    /* Hot-load machine extra-data map if necessary: */
    if (strID != GlobalID && !m_data.contains(strID))
        hotloadMachineExtraDataMap(strID);

    /* Access corresponding map: */
    ExtraDataMap &data = m_data[strID];

    /* [Re]cache passed value: */
    data[strKey] = value.join(",");

    /* Global extra-data: */
    if (strID == GlobalID)
    {
        /* Get global object: */
        CVirtualBox vbox = vboxGlobal().virtualBox();
        /* Update global extra-data: */
        vbox.SetExtraDataStringList(strKey, value);
        if (!vbox.isOk())
            msgCenter().cannotSetExtraData(vbox, strKey, value.join(","));
    }
    /* Machine extra-data: */
    else
    {
        /* Search for corresponding machine: */
        CVirtualBox vbox = vboxGlobal().virtualBox();
        const CMachine machine = vbox.FindMachine(strID);
        AssertReturnVoid(vbox.isOk() && !machine.isNull());
        /* Check the configuration access-level: */
        const KMachineState machineState = machine.GetState();
        const KSessionState sessionState = machine.GetSessionState();
        const ConfigurationAccessLevel cLevel = configurationAccessLevel(sessionState, machineState);
        /* Prepare machine session: */
        CSession session;
        if (cLevel == ConfigurationAccessLevel_Full)
            session = vboxGlobal().openSession(strID);
        else
            session = vboxGlobal().openExistingSession(strID);
        AssertReturnVoid(!session.isNull());
        /* Get machine from that session: */
        CMachine sessionMachine = session.GetMachine();
        /* Update machine extra-data: */
        sessionMachine.SetExtraDataStringList(strKey, value);
        if (!sessionMachine.isOk())
            msgCenter().cannotSetExtraData(sessionMachine, strKey, value.join(","));
        session.UnlockMachine();
    }
}

UIExtraDataManager::UIExtraDataManager()
    : m_pHandler(0)
{
    /* Connect to static instance: */
    m_spInstance = this;
}

UIExtraDataManager::~UIExtraDataManager()
{
    /* Disconnect from static instance: */
    m_spInstance = 0;
}

QStringList UIExtraDataManager::suppressedMessages()
{
    return extraDataStringList(GUI_SuppressMessages);
}

void UIExtraDataManager::setSuppressedMessages(const QStringList &list)
{
    setExtraDataStringList(GUI_SuppressMessages, list);
}

QStringList UIExtraDataManager::messagesWithInvertedOption()
{
    return extraDataStringList(GUI_InvertMessageOption);
}

#if !defined(VBOX_BLEEDING_EDGE) && !defined(DEBUG)
QString UIExtraDataManager::preventBetaBuildWarningForVersion()
{
    return extraDataString(GUI_PreventBetaWarning);
}
#endif /* !defined(VBOX_BLEEDING_EDGE) && !defined(DEBUG) */

#ifdef VBOX_GUI_WITH_NETWORK_MANAGER
bool UIExtraDataManager::applicationUpdateEnabled()
{
    /* 'True' unless 'restriction' feature allowed: */
    return !isFeatureAllowed(GUI_PreventApplicationUpdate);
}

QString UIExtraDataManager::applicationUpdateData()
{
    return extraDataString(GUI_UpdateDate);
}

void UIExtraDataManager::setApplicationUpdateData(const QString &strValue)
{
    setExtraDataString(GUI_UpdateDate, strValue);
}

qulonglong UIExtraDataManager::applicationUpdateCheckCounter()
{
    /* Read subsequent update check counter value: */
    qulonglong uResult = 1;
    const QString strCheckCount = extraDataString(GUI_UpdateCheckCount);
    if (!strCheckCount.isEmpty())
    {
        bool ok = false;
        qulonglong uCheckCount = strCheckCount.toULongLong(&ok);
        if (ok) uResult = uCheckCount;
    }
    /* Return update check counter value: */
    return uResult;
}

void UIExtraDataManager::incrementApplicationUpdateCheckCounter()
{
    /* Increment update check counter value: */
    setExtraDataString(GUI_UpdateCheckCount, QString::number(applicationUpdateCheckCounter() + 1));
}
#endif /* VBOX_GUI_WITH_NETWORK_MANAGER */

QList<GlobalSettingsPageType> UIExtraDataManager::restrictedGlobalSettingsPages()
{
    /* Prepare result: */
    QList<GlobalSettingsPageType> result;
    /* Get restricted global-settings-pages: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedGlobalSettingsPages))
    {
        GlobalSettingsPageType value = gpConverter->fromInternalString<GlobalSettingsPageType>(strValue);
        if (value != GlobalSettingsPageType_Invalid)
            result << value;
    }
    /* Return result: */
    return result;
}

QList<MachineSettingsPageType> UIExtraDataManager::restrictedMachineSettingsPages(const QString &strID)
{
    /* Prepare result: */
    QList<MachineSettingsPageType> result;
    /* Get restricted machine-settings-pages: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedMachineSettingsPages, strID))
    {
        MachineSettingsPageType value = gpConverter->fromInternalString<MachineSettingsPageType>(strValue);
        if (value != MachineSettingsPageType_Invalid)
            result << value;
    }
    /* Return result: */
    return result;
}

bool UIExtraDataManager::activateHoveredMachineWindow()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_ActivateHoveredMachineWindow);
}

void UIExtraDataManager::setActivateHoveredMachineWindow(bool fActivate)
{
    /* 'True' if feature allowed, null-string otherwise: */
    setExtraDataString(GUI_ActivateHoveredMachineWindow, toFeatureAllowed(fActivate));
}

QStringList UIExtraDataManager::shortcutOverrides(const QString &strPoolExtraDataID)
{
    if (strPoolExtraDataID == GUI_Input_SelectorShortcuts)
        return extraDataStringList(GUI_Input_SelectorShortcuts);
    if (strPoolExtraDataID == GUI_Input_MachineShortcuts)
        return extraDataStringList(GUI_Input_MachineShortcuts);
    return QStringList();
}

QString UIExtraDataManager::recentFolderForHardDrives()
{
    return extraDataString(GUI_RecentFolderHD);
}

QString UIExtraDataManager::recentFolderForOpticalDisks()
{
    return extraDataString(GUI_RecentFolderCD);
}

QString UIExtraDataManager::recentFolderForFloppyDisks()
{
    return extraDataString(GUI_RecentFolderFD);
}

void UIExtraDataManager::setRecentFolderForHardDrives(const QString &strValue)
{
    setExtraDataString(GUI_RecentFolderHD, strValue);
}

void UIExtraDataManager::setRecentFolderForOpticalDisks(const QString &strValue)
{
    setExtraDataString(GUI_RecentFolderCD, strValue);
}

void UIExtraDataManager::setRecentFolderForFloppyDisks(const QString &strValue)
{
    setExtraDataString(GUI_RecentFolderFD, strValue);
}

QStringList UIExtraDataManager::recentListOfHardDrives()
{
    return extraDataStringList(GUI_RecentListHD);
}

QStringList UIExtraDataManager::recentListOfOpticalDisks()
{
    return extraDataStringList(GUI_RecentListCD);
}

QStringList UIExtraDataManager::recentListOfFloppyDisks()
{
    return extraDataStringList(GUI_RecentListFD);
}

void UIExtraDataManager::setRecentListOfHardDrives(const QStringList &value)
{
    setExtraDataStringList(GUI_RecentListHD, value);
}

void UIExtraDataManager::setRecentListOfOpticalDisks(const QStringList &value)
{
    setExtraDataStringList(GUI_RecentListCD, value);
}

void UIExtraDataManager::setRecentListOfFloppyDisks(const QStringList &value)
{
    setExtraDataStringList(GUI_RecentListFD, value);
}

QRect UIExtraDataManager::selectorWindowGeometry(QWidget *pWidget)
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_LastSelectorWindowPosition);

    /* Parse loaded data: */
    int iX = 0, iY = 0, iW = 0, iH = 0;
    bool fOk = data.size() >= 4;
    do
    {
        if (!fOk) break;
        iX = data[0].toInt(&fOk);
        if (!fOk) break;
        iY = data[1].toInt(&fOk);
        if (!fOk) break;
        iW = data[2].toInt(&fOk);
        if (!fOk) break;
        iH = data[3].toInt(&fOk);
    }
    while (0);

    /* Use geometry (loaded or default): */
    QRect geometry = fOk ? QRect(iX, iY, iW, iH) : QRect(0, 0, 770, 550);

    /* Take hint-widget into account: */
    if (pWidget)
        geometry.setSize(geometry.size().expandedTo(pWidget->minimumSizeHint()));

    /* Get screen-geometry [of screen with point (iX, iY) if possible]: */
    const QRect screenGeometry = fOk ? QApplication::desktop()->availableGeometry(QPoint(iX, iY)) :
                                       QApplication::desktop()->availableGeometry();

    /* Make sure resulting geometry is within current bounds: */
    geometry = geometry.intersected(screenGeometry);

    /* Move default-geometry to screen-geometry' center: */
    if (!fOk)
        geometry.moveCenter(screenGeometry.center());

    /* Return result: */
    return geometry;
}

bool UIExtraDataManager::selectorWindowShouldBeMaximized()
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_LastSelectorWindowPosition);

    /* Make sure 5th item has required value: */
    return data.size() == 5 && data[4] == GUI_Geometry_State_Max;
}

void UIExtraDataManager::setSelectorWindowGeometry(const QRect &geometry, bool fMaximized)
{
    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(GUI_LastSelectorWindowPosition, data);
}

QList<int> UIExtraDataManager::selectorWindowSplitterHints()
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_SplitterSizes);

    /* Parse loaded data: */
    QList<int> hints;
    hints << (data.size() > 0 ? data[0].toInt() : 0);
    hints << (data.size() > 1 ? data[1].toInt() : 0);

    /* Return hints: */
    return hints;
}

void UIExtraDataManager::setSelectorWindowSplitterHints(const QList<int> &hints)
{
    /* Parse passed hints: */
    QStringList data;
    data << (hints.size() > 0 ? QString::number(hints[0]) : QString());
    data << (hints.size() > 1 ? QString::number(hints[1]) : QString());

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(GUI_SplitterSizes, data);
}

bool UIExtraDataManager::selectorWindowToolBarVisible()
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_Toolbar);
}

void UIExtraDataManager::setSelectorWindowToolBarVisible(bool fVisible)
{
    /* 'False' if feature restricted, null-string otherwise: */
    setExtraDataString(GUI_Toolbar, toFeatureRestricted(!fVisible));
}

bool UIExtraDataManager::selectorWindowStatusBarVisible()
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_Statusbar);
}

void UIExtraDataManager::setSelectorWindowStatusBarVisible(bool fVisible)
{
    /* 'False' if feature restricted, null-string otherwise: */
    setExtraDataString(GUI_Statusbar, toFeatureRestricted(!fVisible));
}

void UIExtraDataManager::clearSelectorWindowGroupsDefinitions()
{
    /* Wipe-out each the group definition record: */
    foreach (const QString &strKey, m_data.value(GlobalID).keys())
        if (strKey.startsWith(GUI_GroupDefinitions))
            setExtraDataString(strKey, QString());
}

QStringList UIExtraDataManager::selectorWindowGroupsDefinitions(const QString &strGroupID)
{
    return extraDataStringList(GUI_GroupDefinitions + strGroupID);
}

void UIExtraDataManager::setSelectorWindowGroupsDefinitions(const QString &strGroupID, const QStringList &definitions)
{
    setExtraDataStringList(GUI_GroupDefinitions + strGroupID, definitions);
}

QString UIExtraDataManager::selectorWindowLastItemChosen()
{
    return extraDataString(GUI_LastItemSelected);
}

void UIExtraDataManager::setSelectorWindowLastItemChosen(const QString &strItemID)
{
    setExtraDataString(GUI_LastItemSelected, strItemID);
}

QMap<DetailsElementType, bool> UIExtraDataManager::selectorWindowDetailsElements()
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_DetailsPageBoxes);

    /* Desearialize passed elements: */
    QMap<DetailsElementType, bool> elements;
    foreach (QString strItem, data)
    {
        bool fOpened = true;
        if (strItem.endsWith("Closed", Qt::CaseInsensitive))
        {
            fOpened = false;
            strItem.remove("Closed");
        }
        DetailsElementType type = gpConverter->fromInternalString<DetailsElementType>(strItem);
        if (type != DetailsElementType_Invalid)
            elements[type] = fOpened;
    }

    /* Return elements: */
    return elements;
}

void UIExtraDataManager::setSelectorWindowDetailsElements(const QMap<DetailsElementType, bool> &elements)
{
    /* Prepare corresponding extra-data: */
    QStringList data;

    /* Searialize passed elements: */
    foreach (DetailsElementType type, elements.keys())
    {
        QString strValue = gpConverter->toInternalString(type);
        if (!elements[type])
            strValue += "Closed";
        data << strValue;
    }

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(GUI_DetailsPageBoxes, data);
}

PreviewUpdateIntervalType UIExtraDataManager::selectorWindowPreviewUpdateInterval()
{
    return gpConverter->fromInternalString<PreviewUpdateIntervalType>(extraDataString(GUI_PreviewUpdate));
}

void UIExtraDataManager::setSelectorWindowPreviewUpdateInterval(PreviewUpdateIntervalType interval)
{
    setExtraDataString(GUI_PreviewUpdate, gpConverter->toInternalString(interval));
}

WizardMode UIExtraDataManager::modeForWizardType(WizardType type)
{
    /* Some wizard use only 'basic' mode: */
    if (type == WizardType_FirstRun)
        return WizardMode_Basic;
    /* Otherwise get mode from cached extra-data: */
    return extraDataStringList(GUI_HideDescriptionForWizards).contains(gpConverter->toInternalString(type))
           ? WizardMode_Expert : WizardMode_Basic;
}

void UIExtraDataManager::setModeForWizardType(WizardType type, WizardMode mode)
{
    /* Get wizard name: */
    const QString strWizardName = gpConverter->toInternalString(type);
    /* Get current value: */
    const QStringList oldValue = extraDataStringList(GUI_HideDescriptionForWizards);
    QStringList newValue = oldValue;
    /* Include wizard-name into expert-mode wizard list if necessary: */
    if (mode == WizardMode_Expert && !newValue.contains(strWizardName))
        newValue << strWizardName;
    /* Exclude wizard-name from expert-mode wizard list if necessary: */
    else if (mode == WizardMode_Basic && newValue.contains(strWizardName))
        newValue.removeAll(strWizardName);
    /* Update extra-data if necessary: */
    if (newValue != oldValue)
        setExtraDataStringList(GUI_HideDescriptionForWizards, newValue);
}

bool UIExtraDataManager::showMachineInSelectorChooser(const QString &strID)
{
    /* 'True' unless 'restriction' feature allowed: */
    return !isFeatureAllowed(GUI_HideFromManager, strID);
}

bool UIExtraDataManager::showMachineInSelectorDetails(const QString &strID)
{
    /* 'True' unless 'restriction' feature allowed: */
    return !isFeatureAllowed(GUI_HideDetails, strID);
}

bool UIExtraDataManager::machineReconfigurationEnabled(const QString &strID)
{
    /* 'True' unless 'restriction' feature allowed: */
    return !isFeatureAllowed(GUI_PreventReconfiguration, strID);
}

bool UIExtraDataManager::machineSnapshotOperationsEnabled(const QString &strID)
{
    /* 'True' unless 'restriction' feature allowed: */
    return !isFeatureAllowed(GUI_PreventSnapshotOperations, strID);
}

bool UIExtraDataManager::machineFirstTimeStarted(const QString &strID)
{
    /* 'True' only if feature is allowed: */
    return isFeatureAllowed(GUI_FirstRun, strID);
}

void UIExtraDataManager::setMachineFirstTimeStarted(bool fFirstTimeStarted, const QString &strID)
{
    /* 'True' if feature allowed, null-string otherwise: */
    setExtraDataString(GUI_FirstRun, toFeatureAllowed(fFirstTimeStarted), strID);
}

#ifndef Q_WS_MAC
QStringList UIExtraDataManager::machineWindowIconNames(const QString &strID)
{
    return extraDataStringList(GUI_MachineWindowIcons, strID);
}

QString UIExtraDataManager::machineWindowNamePostfix(const QString &strID)
{
    return extraDataString(GUI_MachineWindowNamePostfix, strID);
}
#endif /* !Q_WS_MAC */

QRect UIExtraDataManager::machineWindowGeometry(UIVisualStateType visualStateType, ulong uScreenIndex, const QString &strID)
{
    /* Choose corresponding key: */
    QString strKey;
    switch (visualStateType)
    {
        case UIVisualStateType_Normal: strKey = extraDataKeyPerScreen(GUI_LastNormalWindowPosition, uScreenIndex); break;
        case UIVisualStateType_Scale:  strKey = extraDataKeyPerScreen(GUI_LastScaleWindowPosition, uScreenIndex); break;
        default: AssertFailedReturn(QRect());
    }

    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(strKey, strID);

    /* Parse loaded data: */
    int iX = 0, iY = 0, iW = 0, iH = 0;
    bool fOk = data.size() >= 4;
    do
    {
        if (!fOk) break;
        iX = data[0].toInt(&fOk);
        if (!fOk) break;
        iY = data[1].toInt(&fOk);
        if (!fOk) break;
        iW = data[2].toInt(&fOk);
        if (!fOk) break;
        iH = data[3].toInt(&fOk);
    }
    while (0);

    /* Return geometry (loaded or null): */
    return fOk ? QRect(iX, iY, iW, iH) : QRect();
}

bool UIExtraDataManager::machineWindowShouldBeMaximized(UIVisualStateType visualStateType, ulong uScreenIndex, const QString &strID)
{
    /* Choose corresponding key: */
    QString strKey;
    switch (visualStateType)
    {
        case UIVisualStateType_Normal: strKey = extraDataKeyPerScreen(GUI_LastNormalWindowPosition, uScreenIndex); break;
        case UIVisualStateType_Scale:  strKey = extraDataKeyPerScreen(GUI_LastScaleWindowPosition, uScreenIndex); break;
        default: AssertFailedReturn(false);
    }

    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(strKey, strID);

    /* Make sure 5th item has required value: */
    return data.size() == 5 && data[4] == GUI_Geometry_State_Max;
}

void UIExtraDataManager::setMachineWindowGeometry(UIVisualStateType visualStateType, ulong uScreenIndex, const QRect &geometry, bool fMaximized, const QString &strID)
{
    /* Choose corresponding key: */
    QString strKey;
    switch (visualStateType)
    {
        case UIVisualStateType_Normal: strKey = extraDataKeyPerScreen(GUI_LastNormalWindowPosition, uScreenIndex); break;
        case UIVisualStateType_Scale:  strKey = extraDataKeyPerScreen(GUI_LastScaleWindowPosition, uScreenIndex); break;
        default: AssertFailedReturnVoid();
    }

    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(strKey, data, strID);
}

#ifndef Q_WS_MAC
bool UIExtraDataManager::menuBarEnabled(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_MenuBar_Enabled, strID);
}

void UIExtraDataManager::setMenuBarEnabled(bool fEnabled, const QString &strID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    setExtraDataString(GUI_MenuBar_Enabled, toFeatureRestricted(!fEnabled), strID);
}
#endif /* !Q_WS_MAC */

UIExtraDataMetaDefs::MenuType UIExtraDataManager::restrictedRuntimeMenuTypes(const QString &strID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::MenuType result = UIExtraDataMetaDefs::MenuType_Invalid;
    /* Get restricted runtime-menu-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeMenus, strID))
    {
        UIExtraDataMetaDefs::MenuType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::MenuType>(strValue);
        if (value != UIExtraDataMetaDefs::MenuType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::MenuType>(result | value);
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setRestrictedRuntimeMenuTypes(UIExtraDataMetaDefs::MenuType types, const QString &strID)
{
    /* We have MenuType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("MenuType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle MenuType_All enum-value: */
    if (types == UIExtraDataMetaDefs::MenuType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::MenuType enumValue =
                static_cast<const UIExtraDataMetaDefs::MenuType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip MenuType_Invalid & MenuType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::MenuType_Invalid ||
                enumValue == UIExtraDataMetaDefs::MenuType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    setExtraDataStringList(GUI_RestrictedRuntimeMenus, result, strID);
}

UIExtraDataMetaDefs::MenuApplicationActionType UIExtraDataManager::restrictedRuntimeMenuApplicationActionTypes(const QString &strID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::MenuApplicationActionType result = UIExtraDataMetaDefs::MenuApplicationActionType_Invalid;
    /* Get restricted runtime-application-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeApplicationMenuActions, strID))
    {
        UIExtraDataMetaDefs::MenuApplicationActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::MenuApplicationActionType>(strValue);
        if (value != UIExtraDataMetaDefs::MenuApplicationActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::MenuApplicationActionType>(result | value);
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setRestrictedRuntimeMenuApplicationActionTypes(UIExtraDataMetaDefs::MenuApplicationActionType types, const QString &strID)
{
    /* We have MenuApplicationActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("MenuApplicationActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle MenuApplicationActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::MenuApplicationActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::MenuApplicationActionType enumValue =
                static_cast<const UIExtraDataMetaDefs::MenuApplicationActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip MenuApplicationActionType_Invalid & MenuApplicationActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::MenuApplicationActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::MenuApplicationActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    setExtraDataStringList(GUI_RestrictedRuntimeApplicationMenuActions, result, strID);
}

UIExtraDataMetaDefs::RuntimeMenuMachineActionType UIExtraDataManager::restrictedRuntimeMenuMachineActionTypes(const QString &strID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::RuntimeMenuMachineActionType result = UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Invalid;
    /* Get restricted runtime-machine-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeMachineMenuActions, strID))
    {
        UIExtraDataMetaDefs::RuntimeMenuMachineActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::RuntimeMenuMachineActionType>(strValue);
        /* Since empty value has default restriction, we are supporting special 'Nothing' value: */
        if (value == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Nothing)
        {
            result = UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Nothing;
            break;
        }
        if (value != UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::RuntimeMenuMachineActionType>(result | value);
    }
    /* Defaults: */
    if (result == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Invalid)
    {
        result = static_cast<UIExtraDataMetaDefs::RuntimeMenuMachineActionType>(result | UIExtraDataMetaDefs::RuntimeMenuMachineActionType_SaveState);
        result = static_cast<UIExtraDataMetaDefs::RuntimeMenuMachineActionType>(result | UIExtraDataMetaDefs::RuntimeMenuMachineActionType_PowerOff);
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setRestrictedRuntimeMenuMachineActionTypes(UIExtraDataMetaDefs::RuntimeMenuMachineActionType types, const QString &strID)
{
    /* We have RuntimeMenuMachineActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("RuntimeMenuMachineActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle RuntimeMenuMachineActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::RuntimeMenuMachineActionType enumValue =
                static_cast<const UIExtraDataMetaDefs::RuntimeMenuMachineActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip RuntimeMenuMachineActionType_Invalid, RuntimeMenuMachineActionType_Nothing & RuntimeMenuMachineActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Nothing ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuMachineActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Since empty value has default restriction, we are supporting special 'Nothing' value: */
    if (result.isEmpty())
        result << gpConverter->toInternalString(UIExtraDataMetaDefs::RuntimeMenuMachineActionType_Nothing);
    /* Save result: */
    setExtraDataStringList(GUI_RestrictedRuntimeMachineMenuActions, result, strID);
}

UIExtraDataMetaDefs::RuntimeMenuViewActionType UIExtraDataManager::restrictedRuntimeMenuViewActionTypes(const QString &strID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::RuntimeMenuViewActionType result = UIExtraDataMetaDefs::RuntimeMenuViewActionType_Invalid;
    /* Get restricted runtime-view-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeViewMenuActions, strID))
    {
        UIExtraDataMetaDefs::RuntimeMenuViewActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::RuntimeMenuViewActionType>(strValue);
        if (value != UIExtraDataMetaDefs::RuntimeMenuViewActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::RuntimeMenuViewActionType>(result | value);
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setRestrictedRuntimeMenuViewActionTypes(UIExtraDataMetaDefs::RuntimeMenuViewActionType types, const QString &strID)
{
    /* We have RuntimeMenuViewActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("RuntimeMenuViewActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle RuntimeMenuViewActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::RuntimeMenuViewActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::RuntimeMenuViewActionType enumValue =
                static_cast<const UIExtraDataMetaDefs::RuntimeMenuViewActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip RuntimeMenuViewActionType_Invalid & RuntimeMenuViewActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::RuntimeMenuViewActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuViewActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    setExtraDataStringList(GUI_RestrictedRuntimeViewMenuActions, result, strID);
}

UIExtraDataMetaDefs::RuntimeMenuInputActionType UIExtraDataManager::restrictedRuntimeMenuInputActionTypes(const QString &strID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::RuntimeMenuInputActionType result = UIExtraDataMetaDefs::RuntimeMenuInputActionType_Invalid;
    /* Get restricted runtime-machine-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeInputMenuActions, strID))
    {
        UIExtraDataMetaDefs::RuntimeMenuInputActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::RuntimeMenuInputActionType>(strValue);
        if (value != UIExtraDataMetaDefs::RuntimeMenuInputActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::RuntimeMenuInputActionType>(result | value);
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setRestrictedRuntimeMenuInputActionTypes(UIExtraDataMetaDefs::RuntimeMenuInputActionType types, const QString &strID)
{
    /* We have RuntimeMenuInputActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("RuntimeMenuInputActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle RuntimeMenuInputActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::RuntimeMenuInputActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::RuntimeMenuInputActionType enumValue =
                static_cast<const UIExtraDataMetaDefs::RuntimeMenuInputActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip RuntimeMenuInputActionType_Invalid & RuntimeMenuInputActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::RuntimeMenuInputActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuInputActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    setExtraDataStringList(GUI_RestrictedRuntimeInputMenuActions, result, strID);
}

UIExtraDataMetaDefs::RuntimeMenuDevicesActionType UIExtraDataManager::restrictedRuntimeMenuDevicesActionTypes(const QString &strID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::RuntimeMenuDevicesActionType result = UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Invalid;
    /* Get restricted runtime-devices-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeDevicesMenuActions, strID))
    {
        UIExtraDataMetaDefs::RuntimeMenuDevicesActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::RuntimeMenuDevicesActionType>(strValue);
        /* Since empty value has default restriction, we are supporting special 'Nothing' value: */
        if (value == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Nothing)
        {
            result = UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Nothing;
            break;
        }
        if (value != UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::RuntimeMenuDevicesActionType>(result | value);
    }
    /* Defaults: */
    if (result == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Invalid)
    {
        result = static_cast<UIExtraDataMetaDefs::RuntimeMenuDevicesActionType>(result | UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_HardDrives);
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setRestrictedRuntimeMenuDevicesActionTypes(UIExtraDataMetaDefs::RuntimeMenuDevicesActionType types, const QString &strID)
{
    /* We have RuntimeMenuDevicesActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("RuntimeMenuDevicesActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle RuntimeMenuDevicesActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::RuntimeMenuDevicesActionType enumValue =
                static_cast<const UIExtraDataMetaDefs::RuntimeMenuDevicesActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip RuntimeMenuDevicesActionType_Invalid, RuntimeMenuDevicesActionType_Nothing & RuntimeMenuDevicesActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Nothing ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Since empty value has default restriction, we are supporting special 'Nothing' value: */
    if (result.isEmpty())
        result << gpConverter->toInternalString(UIExtraDataMetaDefs::RuntimeMenuDevicesActionType_Nothing);
    /* Save result: */
    setExtraDataStringList(GUI_RestrictedRuntimeDevicesMenuActions, result, strID);
}

#ifdef VBOX_WITH_DEBUGGER_GUI
UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType UIExtraDataManager::restrictedRuntimeMenuDebuggerActionTypes(const QString &strID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType result = UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType_Invalid;
    /* Get restricted runtime-debugger-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeDebuggerMenuActions, strID))
    {
        UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType>(strValue);
        if (value != UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType>(result | value);
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setRestrictedRuntimeMenuDebuggerActionTypes(UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType types, const QString &strID)
{
    /* We have RuntimeMenuDebuggerActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("RuntimeMenuDebuggerActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle RuntimeMenuDebuggerActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType enumValue =
                static_cast<const UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip RuntimeMenuDebuggerActionType_Invalid & RuntimeMenuDebuggerActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::RuntimeMenuDebuggerActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    setExtraDataStringList(GUI_RestrictedRuntimeDebuggerMenuActions, result, strID);
}
#endif /* VBOX_WITH_DEBUGGER_GUI */

#ifdef Q_WS_MAC
UIExtraDataMetaDefs::MenuWindowActionType UIExtraDataManager::restrictedRuntimeMenuWindowActionTypes(const QString &strID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::MenuWindowActionType result = UIExtraDataMetaDefs::MenuWindowActionType_Invalid;
    /* Get restricted runtime-window-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeWindowMenuActions, strID))
    {
        UIExtraDataMetaDefs::MenuWindowActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::MenuWindowActionType>(strValue);
        if (value != UIExtraDataMetaDefs::MenuWindowActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::MenuWindowActionType>(result | value);
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setRestrictedRuntimeMenuWindowActionTypes(UIExtraDataMetaDefs::MenuWindowActionType types, const QString &strID)
{
    /* We have MenuWindowActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("MenuWindowActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle MenuWindowActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::MenuWindowActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::MenuWindowActionType enumValue =
                static_cast<const UIExtraDataMetaDefs::MenuWindowActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip MenuWindowActionType_Invalid & MenuWindowActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::MenuWindowActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::MenuWindowActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    setExtraDataStringList(GUI_RestrictedRuntimeWindowMenuActions, result, strID);
}
#endif /* Q_WS_MAC */

UIExtraDataMetaDefs::MenuHelpActionType UIExtraDataManager::restrictedRuntimeMenuHelpActionTypes(const QString &strID)
{
    /* Prepare result: */
    UIExtraDataMetaDefs::MenuHelpActionType result = UIExtraDataMetaDefs::MenuHelpActionType_Invalid;
    /* Get restricted runtime-help-menu action-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedRuntimeHelpMenuActions, strID))
    {
        UIExtraDataMetaDefs::MenuHelpActionType value = gpConverter->fromInternalString<UIExtraDataMetaDefs::MenuHelpActionType>(strValue);
        if (value != UIExtraDataMetaDefs::MenuHelpActionType_Invalid)
            result = static_cast<UIExtraDataMetaDefs::MenuHelpActionType>(result | value);
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setRestrictedRuntimeMenuHelpActionTypes(UIExtraDataMetaDefs::MenuHelpActionType types, const QString &strID)
{
    /* We have MenuHelpActionType enum registered, so we can enumerate it: */
    const QMetaObject &smo = UIExtraDataMetaDefs::staticMetaObject;
    const int iEnumIndex = smo.indexOfEnumerator("MenuHelpActionType");
    QMetaEnum metaEnum = smo.enumerator(iEnumIndex);

    /* Prepare result: */
    QStringList result;
    /* Handle MenuHelpActionType_All enum-value: */
    if (types == UIExtraDataMetaDefs::MenuHelpActionType_All)
        result << gpConverter->toInternalString(types);
    else
    {
        /* Handle other enum-values: */
        for (int iKeyIndex = 0; iKeyIndex < metaEnum.keyCount(); ++iKeyIndex)
        {
            /* Get iterated enum-value: */
            const UIExtraDataMetaDefs::MenuHelpActionType enumValue =
                static_cast<const UIExtraDataMetaDefs::MenuHelpActionType>(metaEnum.keyToValue(metaEnum.key(iKeyIndex)));
            /* Skip MenuHelpActionType_Invalid && MenuHelpActionType_All enum-values: */
            if (enumValue == UIExtraDataMetaDefs::MenuHelpActionType_Invalid ||
                enumValue == UIExtraDataMetaDefs::MenuHelpActionType_All)
                continue;
            if (types & enumValue)
                result << gpConverter->toInternalString(enumValue);
        }
    }
    /* Save result: */
    setExtraDataStringList(GUI_RestrictedRuntimeHelpMenuActions, result, strID);
}

UIVisualStateType UIExtraDataManager::restrictedVisualStates(const QString &strID)
{
    /* Prepare result: */
    UIVisualStateType result = UIVisualStateType_Invalid;
    /* Get restricted visual-state-types: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedVisualStates, strID))
    {
        UIVisualStateType value = gpConverter->fromInternalString<UIVisualStateType>(strValue);
        if (value != UIVisualStateType_Invalid)
            result = static_cast<UIVisualStateType>(result | value);
    }
    /* Return result: */
    return result;
}

UIVisualStateType UIExtraDataManager::requestedVisualState(const QString &strID)
{
    if (isFeatureAllowed(GUI_Fullscreen, strID)) return UIVisualStateType_Fullscreen;
    if (isFeatureAllowed(GUI_Seamless, strID)) return UIVisualStateType_Seamless;
    if (isFeatureAllowed(GUI_Scale, strID)) return UIVisualStateType_Scale;
    return UIVisualStateType_Normal;
}

void UIExtraDataManager::setRequestedVisualState(UIVisualStateType visualState, const QString &strID)
{
    setExtraDataString(GUI_Fullscreen, toFeatureAllowed(visualState == UIVisualStateType_Fullscreen), strID);
    setExtraDataString(GUI_Seamless, toFeatureAllowed(visualState == UIVisualStateType_Seamless), strID);
    setExtraDataString(GUI_Scale, toFeatureAllowed(visualState == UIVisualStateType_Scale), strID);
}

#ifdef Q_WS_X11
bool UIExtraDataManager::legacyFullscreenModeRequested()
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_Fullscreen_LegacyMode);
}
#endif /* Q_WS_X11 */

bool UIExtraDataManager::guestScreenAutoResizeEnabled(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_AutoresizeGuest, strID);
}

void UIExtraDataManager::setGuestScreenAutoResizeEnabled(bool fEnabled, const QString &strID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    setExtraDataString(GUI_AutoresizeGuest, toFeatureRestricted(!fEnabled), strID);
}

bool UIExtraDataManager::lastGuestScreenVisibilityStatus(ulong uScreenIndex, const QString &strID)
{
    /* Not for primary screen: */
    AssertReturn(uScreenIndex > 0, true);

    /* Compose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_LastVisibilityStatusForGuestScreen, uScreenIndex);

    /* 'False' unless feature allowed: */
    return isFeatureAllowed(strKey, strID);
}

void UIExtraDataManager::setLastGuestScreenVisibilityStatus(ulong uScreenIndex, bool fEnabled, const QString &strID)
{
    /* Not for primary screen: */
    AssertReturnVoid(uScreenIndex > 0);

    /* Compose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_LastVisibilityStatusForGuestScreen, uScreenIndex);

    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(strKey, toFeatureAllowed(fEnabled), strID);
}

QSize UIExtraDataManager::lastGuestScreenSizeHint(ulong uScreenIndex, const QString &strID)
{
    /* Choose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_LastGuestSizeHint, uScreenIndex);

    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(strKey, strID);

    /* Parse loaded data: */
    int iW = 0, iH = 0;
    bool fOk = data.size() == 2;
    do
    {
        if (!fOk) break;
        iW = data[0].toInt(&fOk);
        if (!fOk) break;
        iH = data[1].toInt(&fOk);
    }
    while (0);

    /* Return size (loaded or invalid): */
    return fOk ? QSize(iW, iH) : QSize();
}

void UIExtraDataManager::setLastGuestScreenSizeHint(ulong uScreenIndex, const QSize &sizeHint, const QString &strID)
{
    /* Choose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_LastGuestSizeHint, uScreenIndex);

    /* Serialize passed values: */
    QStringList data;
    data << QString::number(sizeHint.width());
    data << QString::number(sizeHint.height());

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(strKey, data, strID);
}

int UIExtraDataManager::hostScreenForPassedGuestScreen(int iGuestScreenIndex, const QString &strID)
{
    /* Choose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_VirtualScreenToHostScreen, iGuestScreenIndex, true);

    /* Get value and convert it to index: */
    const QString strValue = extraDataString(strKey, strID);
    bool fOk = false;
    const int iHostScreenIndex = strValue.toULong(&fOk);

    /* Return corresponding index: */
    return fOk ? iHostScreenIndex : -1;
}

void UIExtraDataManager::setHostScreenForPassedGuestScreen(int iGuestScreenIndex, int iHostScreenIndex, const QString &strID)
{
    /* Choose corresponding key: */
    const QString strKey = extraDataKeyPerScreen(GUI_VirtualScreenToHostScreen, iGuestScreenIndex, true);

    /* Save passed index under corresponding value: */
    setExtraDataString(strKey, iHostScreenIndex != -1 ? QString::number(iHostScreenIndex) : QString(), strID);
}

bool UIExtraDataManager::autoMountGuestScreensEnabled(const QString &strID)
{
    /* Show only if 'allowed' flag is set: */
    return isFeatureAllowed(GUI_AutomountGuestScreens, strID);
}

#ifdef VBOX_WITH_VIDEOHWACCEL
bool UIExtraDataManager::useLinearStretch(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_Accelerate2D_StretchLinear, strID);
}

bool UIExtraDataManager::usePixelFormatYV12(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_Accelerate2D_PixformatYV12, strID);
}

bool UIExtraDataManager::usePixelFormatUYVY(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_Accelerate2D_PixformatUYVY, strID);
}

bool UIExtraDataManager::usePixelFormatYUY2(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_Accelerate2D_PixformatYUY2, strID);
}

bool UIExtraDataManager::usePixelFormatAYUV(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_Accelerate2D_PixformatAYUV, strID);
}
#endif /* VBOX_WITH_VIDEOHWACCEL */

bool UIExtraDataManager::useUnscaledHiDPIOutput(const QString &strID)
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_HiDPI_UnscaledOutput, strID);
}

void UIExtraDataManager::setUseUnscaledHiDPIOutput(bool fUseUnscaledHiDPIOutput, const QString &strID)
{
    /* 'True' if feature allowed, null-string otherwise: */
    return setExtraDataString(GUI_HiDPI_UnscaledOutput, toFeatureAllowed(fUseUnscaledHiDPIOutput), strID);
}

HiDPIOptimizationType UIExtraDataManager::hiDPIOptimizationType(const QString &strID)
{
    return gpConverter->fromInternalString<HiDPIOptimizationType>(extraDataString(GUI_HiDPI_Optimization, strID));
}

#ifndef Q_WS_MAC
bool UIExtraDataManager::miniToolbarEnabled(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_ShowMiniToolBar, strID);
}

void UIExtraDataManager::setMiniToolbarEnabled(bool fEnabled, const QString &strID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    setExtraDataString(GUI_ShowMiniToolBar, toFeatureRestricted(!fEnabled), strID);
}

bool UIExtraDataManager::autoHideMiniToolbar(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_MiniToolBarAutoHide, strID);
}

void UIExtraDataManager::setAutoHideMiniToolbar(bool fAutoHide, const QString &strID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    setExtraDataString(GUI_MiniToolBarAutoHide, toFeatureRestricted(!fAutoHide), strID);
}

Qt::AlignmentFlag UIExtraDataManager::miniToolbarAlignment(const QString &strID)
{
    /* Return Qt::AlignBottom unless MiniToolbarAlignment_Top specified separately: */
    switch (gpConverter->fromInternalString<MiniToolbarAlignment>(extraDataString(GUI_MiniToolBarAlignment, strID)))
    {
        case MiniToolbarAlignment_Top: return Qt::AlignTop;
        default: break;
    }
    return Qt::AlignBottom;
}

void UIExtraDataManager::setMiniToolbarAlignment(Qt::AlignmentFlag alignment, const QString &strID)
{
    /* Remove record unless Qt::AlignTop specified separately: */
    switch (alignment)
    {
        case Qt::AlignTop: setExtraDataString(GUI_MiniToolBarAlignment, gpConverter->toInternalString(MiniToolbarAlignment_Top), strID); return;
        default: break;
    }
    setExtraDataString(GUI_MiniToolBarAlignment, QString(), strID);
}
#endif /* Q_WS_MAC */

bool UIExtraDataManager::statusBarEnabled(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_StatusBar_Enabled, strID);
}

void UIExtraDataManager::setStatusBarEnabled(bool fEnabled, const QString &strID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    setExtraDataString(GUI_StatusBar_Enabled, toFeatureRestricted(!fEnabled), strID);
}

QList<IndicatorType> UIExtraDataManager::restrictedStatusBarIndicators(const QString &strID)
{
    /* Prepare result: */
    QList<IndicatorType> result;
    /* Get restricted status-bar indicators: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedStatusBarIndicators, strID))
    {
        const IndicatorType value = gpConverter->fromInternalString<IndicatorType>(strValue);
        if (value != IndicatorType_Invalid && !result.contains(value))
            result << value;
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setRestrictedStatusBarIndicators(const QList<IndicatorType> &list, const QString &strID)
{
    /* Parse passed list: */
    QStringList data;
    foreach (const IndicatorType &indicatorType, list)
        data << gpConverter->toInternalString(indicatorType);

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(GUI_RestrictedStatusBarIndicators, data, strID);
}

QList<IndicatorType> UIExtraDataManager::statusBarIndicatorOrder(const QString &strID)
{
    /* Prepare result: */
    QList<IndicatorType> result;
    /* Get status-bar indicator order: */
    foreach (const QString &strValue, extraDataStringList(GUI_StatusBar_IndicatorOrder, strID))
    {
        const IndicatorType value = gpConverter->fromInternalString<IndicatorType>(strValue);
        if (value != IndicatorType_Invalid && !result.contains(value))
            result << value;
    }
    /* Return result: */
    return result;
}

void UIExtraDataManager::setStatusBarIndicatorOrder(const QList<IndicatorType> &list, const QString &strID)
{
    /* Parse passed list: */
    QStringList data;
    foreach (const IndicatorType &indicatorType, list)
        data << gpConverter->toInternalString(indicatorType);

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(GUI_StatusBar_IndicatorOrder, data, strID);
}

#ifdef Q_WS_MAC
bool UIExtraDataManager::realtimeDockIconUpdateEnabled(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_RealtimeDockIconUpdateEnabled, strID);
}

void UIExtraDataManager::setRealtimeDockIconUpdateEnabled(bool fEnabled, const QString &strID)
{
    /* 'False' if feature restricted, null-string otherwise: */
    setExtraDataString(GUI_RealtimeDockIconUpdateEnabled, toFeatureRestricted(!fEnabled), strID);
}

int UIExtraDataManager::realtimeDockIconUpdateMonitor(const QString &strID)
{
    return extraDataString(GUI_RealtimeDockIconUpdateMonitor, strID).toInt();
}

void UIExtraDataManager::setRealtimeDockIconUpdateMonitor(int iIndex, const QString &strID)
{
    setExtraDataString(GUI_RealtimeDockIconUpdateMonitor, iIndex ? QString::number(iIndex) : QString(), strID);
}
#endif /* Q_WS_MAC */

bool UIExtraDataManager::passCADtoGuest(const QString &strID)
{
    /* 'False' unless feature allowed: */
    return isFeatureAllowed(GUI_PassCAD, strID);
}

MouseCapturePolicy UIExtraDataManager::mouseCapturePolicy(const QString &strID)
{
    return gpConverter->fromInternalString<MouseCapturePolicy>(extraDataString(GUI_MouseCapturePolicy, strID));
}

GuruMeditationHandlerType UIExtraDataManager::guruMeditationHandlerType(const QString &strID)
{
    return gpConverter->fromInternalString<GuruMeditationHandlerType>(extraDataString(GUI_GuruMeditationHandler, strID));
}

bool UIExtraDataManager::hidLedsSyncState(const QString &strID)
{
    /* 'True' unless feature restricted: */
    return !isFeatureRestricted(GUI_HidLedsSync, strID);
}

double UIExtraDataManager::scaleFactor(const QString &strID)
{
    /* Get corresponding extra-data value: */
    const QString strValue = extraDataString(GUI_ScaleFactor, strID);

    /* Try to convert loaded data to double: */
    bool fOk = false;
    double dValue = strValue.toDouble(&fOk);

    /* Invent the default value: */
    if (!fOk || !dValue)
        dValue = 1;

    /* Return value: */
    return dValue;
}

void UIExtraDataManager::setScaleFactor(double dScaleFactor, const QString &strID)
{
    /* Set corresponding extra-data value: */
    setExtraDataString(GUI_ScaleFactor, QString::number(dScaleFactor), strID);
}

ScalingOptimizationType UIExtraDataManager::scalingOptimizationType(const QString &strID)
{
    return gpConverter->fromInternalString<ScalingOptimizationType>(extraDataString(GUI_Scaling_Optimization, strID));
}

QRect UIExtraDataManager::informationWindowGeometry(QWidget *pWidget, QWidget *pParentWidget, const QString &strID)
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_InformationWindowGeometry, strID);

    /* Parse loaded data: */
    int iX = 0, iY = 0, iW = 0, iH = 0;
    bool fOk = data.size() >= 4;
    do
    {
        if (!fOk) break;
        iX = data[0].toInt(&fOk);
        if (!fOk) break;
        iY = data[1].toInt(&fOk);
        if (!fOk) break;
        iW = data[2].toInt(&fOk);
        if (!fOk) break;
        iH = data[3].toInt(&fOk);
    }
    while (0);

    /* Use geometry (loaded or default): */
    QRect geometry = fOk ? QRect(iX, iY, iW, iH) : QRect(0, 0, 600, 450);

    /* Take hint-widget into account: */
    if (pWidget)
        geometry.setSize(geometry.size().expandedTo(pWidget->minimumSizeHint()));

    /* Get screen-geometry [of screen with point (iX, iY) if possible]: */
    const QRect screenGeometry = fOk ? QApplication::desktop()->availableGeometry(QPoint(iX, iY)) :
                                       QApplication::desktop()->availableGeometry();

    /* Make sure resulting geometry is within current bounds: */
    geometry = geometry.intersected(screenGeometry);

    /* Move default-geometry to pParentWidget' geometry center: */
    if (!fOk && pParentWidget)
        geometry.moveCenter(pParentWidget->geometry().center());

    /* Return result: */
    return geometry;
}

bool UIExtraDataManager::informationWindowShouldBeMaximized(const QString &strID)
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_InformationWindowGeometry, strID);

    /* Make sure 5th item has required value: */
    return data.size() == 5 && data[4] == GUI_Geometry_State_Max;
}

void UIExtraDataManager::setInformationWindowGeometry(const QRect &geometry, bool fMaximized, const QString &strID)
{
    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(GUI_InformationWindowGeometry, data, strID);
}

MachineCloseAction UIExtraDataManager::defaultMachineCloseAction(const QString &strID)
{
    return gpConverter->fromInternalString<MachineCloseAction>(extraDataString(GUI_DefaultCloseAction, strID));
}

MachineCloseAction UIExtraDataManager::restrictedMachineCloseActions(const QString &strID)
{
    /* Prepare result: */
    MachineCloseAction result = MachineCloseAction_Invalid;
    /* Get restricted machine-close-actions: */
    foreach (const QString &strValue, extraDataStringList(GUI_RestrictedCloseActions, strID))
    {
        MachineCloseAction value = gpConverter->fromInternalString<MachineCloseAction>(strValue);
        if (value != MachineCloseAction_Invalid)
            result = static_cast<MachineCloseAction>(result | value);
    }
    /* Return result: */
    return result;
}

MachineCloseAction UIExtraDataManager::lastMachineCloseAction(const QString &strID)
{
    return gpConverter->fromInternalString<MachineCloseAction>(extraDataString(GUI_LastCloseAction, strID));
}

void UIExtraDataManager::setLastMachineCloseAction(MachineCloseAction machineCloseAction, const QString &strID)
{
    setExtraDataString(GUI_LastCloseAction, gpConverter->toInternalString(machineCloseAction), strID);
}

QString UIExtraDataManager::machineCloseHookScript(const QString &strID)
{
    return extraDataString(GUI_CloseActionHook, strID);
}

#ifdef VBOX_WITH_DEBUGGER_GUI
QString UIExtraDataManager::debugFlagValue(const QString &strDebugFlagKey)
{
    return extraDataString(strDebugFlagKey).toLower().trimmed();
}
#endif /* VBOX_WITH_DEBUGGER_GUI */

#ifdef DEBUG
QRect UIExtraDataManager::extraDataManagerGeometry(QWidget *pWidget)
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_ExtraDataManager_Geometry);

    /* Parse loaded data: */
    int iX = 0, iY = 0, iW = 0, iH = 0;
    bool fOk = data.size() >= 4;
    do
    {
        if (!fOk) break;
        iX = data[0].toInt(&fOk);
        if (!fOk) break;
        iY = data[1].toInt(&fOk);
        if (!fOk) break;
        iW = data[2].toInt(&fOk);
        if (!fOk) break;
        iH = data[3].toInt(&fOk);
    }
    while (0);

    /* Use geometry (loaded or default): */
    QRect geometry = fOk ? QRect(iX, iY, iW, iH) : QRect(0, 0, 800, 600);

    /* Take hint-widget into account: */
    if (pWidget)
        geometry.setSize(geometry.size().expandedTo(pWidget->minimumSizeHint()));

    /* Get screen-geometry [of screen with point (iX, iY) if possible]: */
    const QRect screenGeometry = fOk ? QApplication::desktop()->availableGeometry(QPoint(iX, iY)) :
                                       QApplication::desktop()->availableGeometry();

    /* Make sure resulting geometry is within current bounds: */
    geometry = geometry.intersected(screenGeometry);

    /* Move default-geometry to current screen center: */
    if (!fOk)
        geometry.moveCenter(screenGeometry.center());

    /* Return result: */
    return geometry;
}

bool UIExtraDataManager::extraDataManagerShouldBeMaximized()
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_ExtraDataManager_Geometry);

    /* Make sure 5th item has required value: */
    return data.size() == 5 && data[4] == GUI_Geometry_State_Max;
}

void UIExtraDataManager::setExtraDataManagerGeometry(const QRect &geometry, bool fMaximized)
{
    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(GUI_ExtraDataManager_Geometry, data);
}

QList<int> UIExtraDataManager::extraDataManagerSplitterHints(QWidget *pWidget)
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_ExtraDataManager_SplitterHints);

    /* Parse loaded data: */
    int iLeft = 0, iRight = 0;
    bool fOk = data.size() == 2;
    do
    {
        if (!fOk) break;
        iLeft = data[0].toInt(&fOk);
        if (!fOk) break;
        iRight = data[1].toInt(&fOk);
    }
    while (0);

    /* Prepare hints (loaded or adviced): */
    QList<int> hints;
    if (fOk)
    {
        hints << iLeft;
        hints << iRight;
    }
    else
    {
        hints << (int)(pWidget->width() * .9 * (1.0 / 3));
        hints << (int)(pWidget->width() * .9 * (2.0 / 3));
    }

    /* Return hints: */
    return hints;
}

void UIExtraDataManager::setExtraDataManagerSplitterHints(const QList<int> &hints)
{
    /* Parse passed hints: */
    QStringList data;
    data << (hints.size() > 0 ? QString::number(hints[0]) : QString());
    data << (hints.size() > 1 ? QString::number(hints[1]) : QString());

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(GUI_ExtraDataManager_SplitterHints, data);
}
#endif /* DEBUG */

QRect UIExtraDataManager::logWindowGeometry(QWidget *pWidget, const QRect &defaultGeometry)
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_LogWindowGeometry);

    /* Parse loaded data: */
    int iX = 0, iY = 0, iW = 0, iH = 0;
    bool fOk = data.size() >= 4;    
    do
    {
        if (!fOk) break;
        iX = data[0].toInt(&fOk);
        if (!fOk) break;
        iY = data[1].toInt(&fOk);
        if (!fOk) break;
        iW = data[2].toInt(&fOk);
        if (!fOk) break;
        iH = data[3].toInt(&fOk);
    }
    while (0);

    /* Use geometry (loaded or default): */
    QRect geometry = fOk ? QRect(iX, iY, iW, iH) : defaultGeometry;

    /* Take hint-widget into account: */
    if (pWidget)
        geometry.setSize(geometry.size().expandedTo(pWidget->minimumSizeHint()));

    /* In Windows Qt fails to reposition out of screen window properly, so moving to centre: */
#ifdef Q_WS_WIN
    /* Get screen-geometry [of screen with point (iX, iY) if possible]: */
    const QRect screenGeometry = QApplication::desktop()->availableGeometry(QPoint(iX, iY));

    /* Make sure resulting geometry is within current bounds: */
    if (!screenGeometry.contains(geometry, true))
        geometry.moveCenter(defaultGeometry.center());
#endif /* Q_WS_WIN */

    /* Return result: */
    return geometry;
}

bool UIExtraDataManager::logWindowShouldBeMaximized()
{
    /* Get corresponding extra-data: */
    const QStringList data = extraDataStringList(GUI_LogWindowGeometry);

    /* Make sure 5th item has required value: */
    return data.size() == 5 && data[4] == GUI_Geometry_State_Max;
}

void UIExtraDataManager::setLogWindowGeometry(const QRect &geometry, bool fMaximized)
{
    /* Serialize passed values: */
    QStringList data;
    data << QString::number(geometry.x());
    data << QString::number(geometry.y());
    data << QString::number(geometry.width());
    data << QString::number(geometry.height());
    if (fMaximized)
        data << GUI_Geometry_State_Max;

    /* Re-cache corresponding extra-data: */
    setExtraDataStringList(GUI_LogWindowGeometry, data);
}

void UIExtraDataManager::sltExtraDataChange(QString strMachineID, QString strKey, QString strValue)
{
    /* Re-cache value only if strMachineID known already: */
    if (m_data.contains(strMachineID))
    {
        if (!strValue.isEmpty())
            m_data[strMachineID][strKey] = strValue;
        else
            m_data[strMachineID].remove(strKey);
    }

    /* Global extra-data 'change' event: */
    if (strMachineID == GlobalID)
    {
        if (strKey.startsWith("GUI/"))
        {
            /* Language changed? */
            if (strKey == GUI_LanguageId)
                emit sigLanguageChange(extraDataString(strKey));
            /* Selector UI shortcut changed? */
            else if (strKey == GUI_Input_SelectorShortcuts)
                emit sigSelectorUIShortcutChange();
            /* Runtime UI shortcut changed? */
            else if (strKey == GUI_Input_MachineShortcuts)
                emit sigRuntimeUIShortcutChange();
        }
    }
    /* Machine extra-data 'change' event: */
    else
    {
        /* Current VM only: */
        if (   vboxGlobal().isVMConsoleProcess()
            && strMachineID == vboxGlobal().managedVMUuid())
        {
            /* HID LEDs sync state changed (allowed if not restricted)? */
            if (strKey == GUI_HidLedsSync)
                emit sigHidLedsSyncStateChange(!isFeatureRestricted(strKey, strMachineID));
#ifdef Q_WS_MAC
            /* 'Dock icon' appearance changed (allowed if not restricted)? */
            else if (   strKey == GUI_RealtimeDockIconUpdateEnabled
                     || strKey == GUI_RealtimeDockIconUpdateMonitor)
                emit sigDockIconAppearanceChange(!isFeatureRestricted(strKey, strMachineID));
#endif /* Q_WS_MAC */
        }

        /* Menu-bar configuration change: */
        if (
#ifndef Q_WS_MAC
            strKey == GUI_MenuBar_Enabled ||
#endif /* !Q_WS_MAC */
            strKey == GUI_RestrictedRuntimeMenus ||
            strKey == GUI_RestrictedRuntimeApplicationMenuActions ||
            strKey == GUI_RestrictedRuntimeMachineMenuActions ||
            strKey == GUI_RestrictedRuntimeViewMenuActions ||
            strKey == GUI_RestrictedRuntimeInputMenuActions ||
            strKey == GUI_RestrictedRuntimeDevicesMenuActions ||
#ifdef VBOX_WITH_DEBUGGER_GUI
            strKey == GUI_RestrictedRuntimeDebuggerMenuActions ||
#endif /* VBOX_WITH_DEBUGGER_GUI */
#ifdef Q_WS_MAC
            strKey == GUI_RestrictedRuntimeWindowMenuActions ||
#endif /* Q_WS_MAC */
            strKey == GUI_RestrictedRuntimeHelpMenuActions)
            emit sigMenuBarConfigurationChange(strMachineID);
        /* Status-bar configuration change: */
        else if (strKey == GUI_StatusBar_Enabled ||
                 strKey == GUI_RestrictedStatusBarIndicators ||
                 strKey == GUI_StatusBar_IndicatorOrder)
            emit sigStatusBarConfigurationChange(strMachineID);
        /* Scale-factor change: */
        else if (strKey == GUI_ScaleFactor)
            emit sigScaleFactorChange(strMachineID);
        /* Scaling optimization type change: */
        else if (strKey == GUI_Scaling_Optimization)
            emit sigScalingOptimizationTypeChange(strMachineID);
        /* HiDPI optimization type change: */
        else if (strKey == GUI_HiDPI_Optimization)
            emit sigHiDPIOptimizationTypeChange(strMachineID);
        /* Unscaled HiDPI Output mode change: */
        else if (strKey == GUI_HiDPI_UnscaledOutput)
            emit sigUnscaledHiDPIOutputModeChange(strMachineID);
    }

    /* Notify listeners: */
    emit sigExtraDataChange(strMachineID, strKey, strValue);
}

void UIExtraDataManager::prepare()
{
    /* Prepare global extra-data map: */
    prepareGlobalExtraDataMap();
    /* Prepare extra-data event-handler: */
    prepareExtraDataEventHandler();
}

void UIExtraDataManager::prepareGlobalExtraDataMap()
{
    /* Get CVirtualBox: */
    CVirtualBox vbox = vboxGlobal().virtualBox();

    /* Make sure at least empty map is created: */
    m_data[GlobalID] = ExtraDataMap();

    /* Load global extra-data map: */
    foreach (const QString &strKey, vbox.GetExtraDataKeys())
        m_data[GlobalID][strKey] = vbox.GetExtraData(strKey);
}

void UIExtraDataManager::prepareExtraDataEventHandler()
{
    /* Create extra-data event-handler: */
    m_pHandler = new UIExtraDataEventHandler(this);
    /* Configure extra-data event-handler: */
    AssertPtrReturnVoid(m_pHandler);
    {
        /* Extra-data change signal: */
        connect(m_pHandler, SIGNAL(sigExtraDataChange(QString, QString, QString)),
                this, SLOT(sltExtraDataChange(QString, QString, QString)),
                Qt::QueuedConnection);

        /* Prepare Main event-listener: */
        prepareMainEventListener();
    }
}

void UIExtraDataManager::prepareMainEventListener()
{
    /* Register Main event-listener:  */
    const CVirtualBox vbox = vboxGlobal().virtualBox();
    ComObjPtr<UIMainEventListenerImpl> pListener;
    pListener.createObject();
    pListener->init(new UIMainEventListener, this);
    m_listener = CEventListener(pListener);
    QVector<KVBoxEventType> events;
    events
        << KVBoxEventType_OnExtraDataCanChange
        << KVBoxEventType_OnExtraDataChanged;
    vbox.GetEventSource().RegisterListener(m_listener, events, TRUE);
    AssertWrapperOk(vbox);

    /* This is a vetoable event, so we have to respond to the event and have to use a direct connection therefor: */
    connect(pListener->getWrapped(), SIGNAL(sigExtraDataCanChange(QString, QString, QString, bool&, QString&)),
            m_pHandler, SLOT(sltPreprocessExtraDataCanChange(QString, QString, QString, bool&, QString&)),
            Qt::DirectConnection);
    /* Use a direct connection to the helper class: */
    connect(pListener->getWrapped(), SIGNAL(sigExtraDataChange(QString, QString, QString)),
            m_pHandler, SLOT(sltPreprocessExtraDataChange(QString, QString, QString)),
            Qt::DirectConnection);
}

#ifdef DEBUG
void UIExtraDataManager::cleanupWindow()
{
    delete m_pWindow;
}
#endif /* DEBUG */

void UIExtraDataManager::cleanupMainEventListener()
{
    /* Make sure VBoxSVC is available: */
    if (!vboxGlobal().isVBoxSVCAvailable())
        return;

    /* Unregister Main event-listener: */
    const CVirtualBox vbox = vboxGlobal().virtualBox();
    vbox.GetEventSource().UnregisterListener(m_listener);
}

void UIExtraDataManager::cleanup()
{
#ifdef DEBUG
    /* Cleanup window: */
    cleanupWindow();
#endif /* DEBUG */
    /* Cleanup Main event-listener: */
    cleanupMainEventListener();
}

#ifdef DEBUG
void UIExtraDataManager::open(QWidget *pCenterWidget)
{
    /* If necessary: */
    if (!m_pWindow)
    {
        /* Create window: */
        m_pWindow = new UIExtraDataManagerWindow;
        /* Configure window connections: */
        connect(this, SIGNAL(sigExtraDataMapAcknowledging(QString)),
                m_pWindow, SLOT(sltExtraDataMapAcknowledging(QString)));
        connect(this, SIGNAL(sigExtraDataChange(QString, QString, QString)),
                m_pWindow, SLOT(sltExtraDataChange(QString, QString, QString)));
    }
    /* Show and raise window: */
    m_pWindow->showAndRaise(pCenterWidget);
}
#endif /* DEBUG */

bool UIExtraDataManager::isFeatureAllowed(const QString &strKey, const QString &strID /* = GlobalID */)
{
    /* Hot-load machine extra-data map if necessary: */
    if (strID != GlobalID && !m_data.contains(strID))
        hotloadMachineExtraDataMap(strID);

    /* Read-only access corresponding map: */
    const ExtraDataMap data = m_data.value(strID);

    /* 'false' if value was not set: */
    if (!data.contains(strKey))
        return false;

    /* Check corresponding value: */
    const QString &strValue = data[strKey];
    return    strValue.compare("true", Qt::CaseInsensitive) == 0
           || strValue.compare("yes", Qt::CaseInsensitive) == 0
           || strValue.compare("on", Qt::CaseInsensitive) == 0
           || strValue == "1";
}

bool UIExtraDataManager::isFeatureRestricted(const QString &strKey, const QString &strID /* = GlobalID */)
{
    /* Hot-load machine extra-data map if necessary: */
    if (strID != GlobalID && !m_data.contains(strID))
        hotloadMachineExtraDataMap(strID);

    /* Read-only access corresponding map: */
    const ExtraDataMap data = m_data.value(strID);

    /* 'false' if value was not set: */
    if (!data.contains(strKey))
        return false;

    /* Check corresponding value: */
    const QString &strValue = data[strKey];
    return    strValue.compare("false", Qt::CaseInsensitive) == 0
           || strValue.compare("no", Qt::CaseInsensitive) == 0
           || strValue.compare("off", Qt::CaseInsensitive) == 0
           || strValue == "0";
}

QString UIExtraDataManager::toFeatureAllowed(bool fAllowed)
{
    return fAllowed ? QString("true") : QString();
}

QString UIExtraDataManager::toFeatureRestricted(bool fRestricted)
{
    return fRestricted ? QString("false") : QString();
}

/* static */
QString UIExtraDataManager::extraDataKeyPerScreen(const QString &strBase, ulong uScreenIndex, bool fSameRuleForPrimary /* = false */)
{
    return fSameRuleForPrimary || uScreenIndex ? strBase + QString::number(uScreenIndex) : strBase;
}

#include "UIExtraDataManager.moc"

