/* $Id$ */

/** @file
 *
 * Implementation of IStorageController.
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

#include "StorageControllerImpl.h"
#include "MachineImpl.h"
#include "VirtualBoxImpl.h"
#include "SystemPropertiesImpl.h"

#include <iprt/string.h>
#include <iprt/cpp/utils.h>

#include <VBox/err.h>
#include <VBox/settings.h>

#include <algorithm>

#include "AutoStateDep.h"
#include "AutoCaller.h"
#include "Logging.h"

// defines
/////////////////////////////////////////////////////////////////////////////
//
//
DEFINE_EMPTY_CTOR_DTOR(StorageController)

struct BackupableStorageControllerData
{
    /* Constructor. */
    BackupableStorageControllerData()
        : mStorageBus(StorageBus_IDE),
          mStorageControllerType(StorageControllerType_PIIX4),
          mInstance(0),
          mPortCount(2),
          fUseHostIOCache(true),
          fBootable(false)
    { }

    /** Unique name of the storage controller. */
    Utf8Str strName;
    /** The connection type of the storage controller. */
    StorageBus_T mStorageBus;
    /** Type of the Storage controller. */
    StorageControllerType_T mStorageControllerType;
    /** Instance number of the storage controller. */
    ULONG mInstance;
    /** Number of usable ports. */
    ULONG mPortCount;
    /** Whether to use the host IO caches. */
    BOOL fUseHostIOCache;
    /** Whether it is possible to boot from disks attached to this controller. */
    BOOL fBootable;
};

struct StorageController::Data
{
    Data(Machine * const aMachine)
        : pVirtualBox(NULL),
          pSystemProperties(NULL),
          pParent(aMachine)
    {
        unconst(pVirtualBox) = aMachine->i_getVirtualBox();
        unconst(pSystemProperties) = pVirtualBox->i_getSystemProperties();
    }

    VirtualBox * const                  pVirtualBox;
    SystemProperties * const            pSystemProperties;
    Machine * const                     pParent;
    const ComObjPtr<StorageController>  pPeer;

    Backupable<BackupableStorageControllerData> bd;
};


// constructor / destructor
/////////////////////////////////////////////////////////////////////////////

HRESULT StorageController::FinalConstruct()
{
    return BaseFinalConstruct();
}

void StorageController::FinalRelease()
{
    uninit();
    BaseFinalRelease();
}

// public initializer/uninitializer for internal purposes only
/////////////////////////////////////////////////////////////////////////////

/**
 * Initializes the storage controller object.
 *
 * @returns COM result indicator.
 * @param aParent       Pointer to our parent object.
 * @param aName         Name of the storage controller.
 * @param aInstance     Instance number of the storage controller.
 */
HRESULT StorageController::init(Machine *aParent,
                                const Utf8Str &aName,
                                StorageBus_T aStorageBus,
                                ULONG aInstance, bool fBootable)
{
    LogFlowThisFunc(("aParent=%p aName=\"%s\" aInstance=%u\n",
                     aParent, aName.c_str(), aInstance));

    ComAssertRet(aParent && !aName.isEmpty(), E_INVALIDARG);
    if (   (aStorageBus <= StorageBus_Null)
        || (aStorageBus >  StorageBus_USB))
        return setError(E_INVALIDARG,
                        tr("Invalid storage connection type"));

    ULONG maxInstances;
    ChipsetType_T chipsetType;
    HRESULT rc = aParent->COMGETTER(ChipsetType)(&chipsetType);
    if (FAILED(rc))
        return rc;
    rc = aParent->i_getVirtualBox()->i_getSystemProperties()->
        GetMaxInstancesOfStorageBus(chipsetType, aStorageBus, &maxInstances);
    if (FAILED(rc))
        return rc;
    if (aInstance >= maxInstances)
        return setError(E_INVALIDARG,
                        tr("Too many storage controllers of this type"));

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    m = new Data(aParent);

    /* m->pPeer is left null */

    m->bd.allocate();

    m->bd->strName = aName;
    m->bd->mInstance = aInstance;
    m->bd->fBootable = fBootable;
    m->bd->mStorageBus = aStorageBus;
    if (   aStorageBus != StorageBus_IDE
        && aStorageBus != StorageBus_Floppy)
        m->bd->fUseHostIOCache = false;
    else
        m->bd->fUseHostIOCache = true;

    switch (aStorageBus)
    {
        case StorageBus_IDE:
            m->bd->mPortCount = 2;
            m->bd->mStorageControllerType = StorageControllerType_PIIX4;
            break;
        case StorageBus_SATA:
            m->bd->mPortCount = 30;
            m->bd->mStorageControllerType = StorageControllerType_IntelAhci;
            break;
        case StorageBus_SCSI:
            m->bd->mPortCount = 16;
            m->bd->mStorageControllerType = StorageControllerType_LsiLogic;
            break;
        case StorageBus_Floppy:
            m->bd->mPortCount = 1;
            m->bd->mStorageControllerType = StorageControllerType_I82078;
            break;
        case StorageBus_SAS:
            m->bd->mPortCount = 8;
            m->bd->mStorageControllerType = StorageControllerType_LsiLogicSas;
        case StorageBus_USB:
            m->bd->mPortCount = 8;
            m->bd->mStorageControllerType = StorageControllerType_USB;
            break;
    }

    /* Confirm a successful initialization */
    autoInitSpan.setSucceeded();

    return S_OK;
}

/**
 *  Initializes the object given another object
 *  (a kind of copy constructor). This object shares data with
 *  the object passed as an argument.
 *
 *  @param  aReshare
 *      When false, the original object will remain a data owner.
 *      Otherwise, data ownership will be transferred from the original
 *      object to this one.
 *
 *  @note This object must be destroyed before the original object
 *  it shares data with is destroyed.
 *
 *  @note Locks @a aThat object for writing if @a aReshare is @c true, or for
 *  reading if @a aReshare is false.
 */
HRESULT StorageController::init(Machine *aParent,
                                StorageController *aThat,
                                bool aReshare /* = false */)
{
    LogFlowThisFunc(("aParent=%p, aThat=%p, aReshare=%RTbool\n",
                      aParent, aThat, aReshare));

    ComAssertRet(aParent && aThat, E_INVALIDARG);

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    m = new Data(aParent);

    /* sanity */
    AutoCaller thatCaller(aThat);
    AssertComRCReturnRC(thatCaller.rc());

    if (aReshare)
    {
        AutoWriteLock thatLock(aThat COMMA_LOCKVAL_SRC_POS);

        unconst(aThat->m->pPeer) = this;
        m->bd.attach(aThat->m->bd);
    }
    else
    {
        unconst(m->pPeer) = aThat;

        AutoReadLock thatLock(aThat COMMA_LOCKVAL_SRC_POS);
        m->bd.share(aThat->m->bd);
    }

    /* Confirm successful initialization */
    autoInitSpan.setSucceeded();

    return S_OK;
}

/**
 *  Initializes the storage controller object given another guest object
 *  (a kind of copy constructor). This object makes a private copy of data
 *  of the original object passed as an argument.
 */
HRESULT StorageController::initCopy(Machine *aParent, StorageController *aThat)
{
    LogFlowThisFunc(("aParent=%p, aThat=%p\n", aParent, aThat));

    ComAssertRet(aParent && aThat, E_INVALIDARG);

    /* Enclose the state transition NotReady->InInit->Ready */
    AutoInitSpan autoInitSpan(this);
    AssertReturn(autoInitSpan.isOk(), E_FAIL);

    m = new Data(aParent);
    /* m->pPeer is left null */

    AutoCaller thatCaller(aThat);
    AssertComRCReturnRC(thatCaller.rc());

    AutoReadLock thatlock(aThat COMMA_LOCKVAL_SRC_POS);
    m->bd.attachCopy(aThat->m->bd);

    /* Confirm a successful initialization */
    autoInitSpan.setSucceeded();

    return S_OK;
}


/**
 * Uninitializes the instance and sets the ready flag to FALSE.
 * Called either from FinalRelease() or by the parent when it gets destroyed.
 */
void StorageController::uninit()
{
    LogFlowThisFunc(("\n"));

    /* Enclose the state transition Ready->InUninit->NotReady */
    AutoUninitSpan autoUninitSpan(this);
    if (autoUninitSpan.uninitDone())
        return;

    m->bd.free();

    unconst(m->pPeer) = NULL;
    unconst(m->pParent) = NULL;

    delete m;
    m = NULL;
}


// IStorageController properties
HRESULT StorageController::getName(com::Utf8Str &aName)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    aName = m->bd->strName;

    return S_OK;
}

HRESULT StorageController::setName(const com::Utf8Str &aName)
{
    /* the machine needs to be mutable */
    AutoMutableStateDependency adep(m->pParent);
    if (FAILED(adep.rc())) return adep.rc();

    AutoMultiWriteLock2 alock(m->pParent, this COMMA_LOCKVAL_SRC_POS);

    if (m->bd->strName != aName)
    {
        ComObjPtr<StorageController> ctrl;
        HRESULT rc = m->pParent->i_getStorageControllerByName(aName, ctrl, false /* aSetError */);
        if (SUCCEEDED(rc))
            return setError(VBOX_E_OBJECT_IN_USE,
                            tr("Storage controller named '%s' already exists"),
                            aName.c_str());

        Machine::MediaData::AttachmentList atts;
        rc = m->pParent->i_getMediumAttachmentsOfController(m->bd->strName, atts);
        for (Machine::MediaData::AttachmentList::const_iterator it = atts.begin();
             it != atts.end();
             ++it)
        {
            IMediumAttachment *iA = *it;
            MediumAttachment *pAttach = static_cast<MediumAttachment *>(iA);
            AutoWriteLock attlock(pAttach COMMA_LOCKVAL_SRC_POS);
            pAttach->i_updateName(aName);
        }


        m->bd.backup();
        m->bd->strName = aName;

        m->pParent->i_setModified(Machine::IsModified_Storage);
        alock.release();

        m->pParent->i_onStorageControllerChange();
    }

    return S_OK;
}

HRESULT StorageController::getBus(StorageBus_T *aBus)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aBus = m->bd->mStorageBus;

    return S_OK;
}

HRESULT StorageController::getControllerType(StorageControllerType_T *aControllerType)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aControllerType = m->bd->mStorageControllerType;

    return S_OK;
}

HRESULT StorageController::setControllerType(StorageControllerType_T aControllerType)
{
    /* the machine needs to be mutable */
    AutoMutableStateDependency adep(m->pParent);
    if (FAILED(adep.rc())) return adep.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    HRESULT rc = S_OK;

    switch (m->bd->mStorageBus)
    {
        case StorageBus_IDE:
        {
            if (   (aControllerType != StorageControllerType_PIIX3)
                && (aControllerType != StorageControllerType_PIIX4)
                && (aControllerType != StorageControllerType_ICH6))
                rc = E_INVALIDARG;
            break;
        }
        case StorageBus_SATA:
        {
            if (aControllerType != StorageControllerType_IntelAhci)
                rc = E_INVALIDARG;
            break;
        }
        case StorageBus_SCSI:
        {
            if (   (aControllerType != StorageControllerType_LsiLogic)
                && (aControllerType != StorageControllerType_BusLogic))
                rc = E_INVALIDARG;
            break;
        }
        case StorageBus_Floppy:
        {
            if (aControllerType != StorageControllerType_I82078)
                rc = E_INVALIDARG;
            break;
        }
        case StorageBus_SAS:
        {
            if (aControllerType != StorageControllerType_LsiLogicSas)
                rc = E_INVALIDARG;
            break;
        }
        case StorageBus_USB:
        {
            if (aControllerType != StorageControllerType_USB)
                rc = E_INVALIDARG;
            break;
        }
        default:
            AssertMsgFailed(("Invalid controller type %d\n", m->bd->mStorageBus));
            rc = E_INVALIDARG;
    }

    if (!SUCCEEDED(rc))
        return setError(rc,
                        tr("Invalid controller type %d"),
                        aControllerType);

    if (m->bd->mStorageControllerType != aControllerType)
    {
        m->bd.backup();
        m->bd->mStorageControllerType = aControllerType;

        alock.release();
        AutoWriteLock mlock(m->pParent COMMA_LOCKVAL_SRC_POS);        // m->pParent is const, needs no locking
        m->pParent->i_setModified(Machine::IsModified_Storage);
        mlock.release();

        m->pParent->i_onStorageControllerChange();
    }

    return S_OK;
}

HRESULT StorageController::getMaxDevicesPerPortCount(ULONG *aMaxDevicesPerPortCount)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    HRESULT rc = m->pSystemProperties->GetMaxDevicesPerPortForStorageBus(m->bd->mStorageBus, aMaxDevicesPerPortCount);

    return rc;
}

HRESULT StorageController::getMinPortCount(ULONG *aMinPortCount)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    HRESULT rc = m->pSystemProperties->GetMinPortCountForStorageBus(m->bd->mStorageBus, aMinPortCount);
    return rc;
}

HRESULT StorageController::getMaxPortCount(ULONG *aMaxPortCount)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);
    HRESULT rc = m->pSystemProperties->GetMaxPortCountForStorageBus(m->bd->mStorageBus, aMaxPortCount);

    return rc;
}

HRESULT StorageController::getPortCount(ULONG *aPortCount)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aPortCount = m->bd->mPortCount;

    return S_OK;
}

HRESULT StorageController::setPortCount(ULONG aPortCount)
{
    /* the machine needs to be mutable */
    AutoMutableStateDependency adep(m->pParent);
    if (FAILED(adep.rc())) return adep.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    switch (m->bd->mStorageBus)
    {
        case StorageBus_SATA:
        {
            /* AHCI SATA supports a maximum of 30 ports. */
            if (aPortCount < 1 || aPortCount > 30)
                return setError(E_INVALIDARG,
                                tr("Invalid port count: %lu (must be in range [%lu, %lu])"),
                                aPortCount, 1, 30);
            break;
        }
        case StorageBus_SCSI:
        {
            /*
             * SCSI does not support setting different ports.
             * (doesn't make sense here either).
             * The maximum and minimum is 16 and unless the callee
             * tries to set a different value we return an error.
             */
            if (aPortCount != 16)
                return setError(E_INVALIDARG,
                                tr("Invalid port count: %lu (must be in range [%lu, %lu])"),
                                aPortCount, 16, 16);
            break;
        }
        case StorageBus_IDE:
        {
            /*
             * The port count is fixed to 2.
             */
            if (aPortCount != 2)
                return setError(E_INVALIDARG,
                                tr("Invalid port count: %lu (must be in range [%lu, %lu])"),
                                aPortCount, 2, 2);
            break;
        }
        case StorageBus_Floppy:
        {
            /*
             * The port count is fixed to 1.
             */
            if (aPortCount != 1)
                return setError(E_INVALIDARG,
                                tr("Invalid port count: %lu (must be in range [%lu, %lu])"),
                                aPortCount, 1, 1);
            break;
        }
        case StorageBus_SAS:
        {
            /* SAS supports a maximum of 255 ports. */
            if (aPortCount < 1 || aPortCount > 255)
                return setError(E_INVALIDARG,
                                tr("Invalid port count: %lu (must be in range [%lu, %lu])"),
                                aPortCount, 1, 255);
            break;
        }
        case StorageBus_USB:
        {
            /*
             * The port count is fixed to 8.
             */
            if (aPortCount != 8)
                return setError(E_INVALIDARG,
                                tr("Invalid port count: %lu (must be in range [%lu, %lu])"),
                                aPortCount, 8, 8);
            break;
        }
        default:
            AssertMsgFailed(("Invalid controller type %d\n", m->bd->mStorageBus));
    }

    if (m->bd->mPortCount != aPortCount)
    {
        m->bd.backup();
        m->bd->mPortCount = aPortCount;

        alock.release();
        AutoWriteLock mlock(m->pParent COMMA_LOCKVAL_SRC_POS);        // m->pParent is const, needs no locking
        m->pParent->i_setModified(Machine::IsModified_Storage);
        mlock.release();

        m->pParent->i_onStorageControllerChange();
    }

    return S_OK;
}

HRESULT StorageController::getInstance(ULONG *aInstance)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *aInstance = m->bd->mInstance;

    return S_OK;
}

HRESULT StorageController::setInstance(ULONG aInstance)
{
    /* the machine needs to be mutable */
    AutoMutableStateDependency adep(m->pParent);
    if (FAILED(adep.rc())) return adep.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (m->bd->mInstance != aInstance)
    {
        m->bd.backup();
        m->bd->mInstance = aInstance;

        alock.release();
        AutoWriteLock mlock(m->pParent COMMA_LOCKVAL_SRC_POS);        // m->pParent is const, needs no locking
        m->pParent->i_setModified(Machine::IsModified_Storage);
        mlock.release();

        m->pParent->i_onStorageControllerChange();
    }

    return S_OK;
}

HRESULT StorageController::getUseHostIOCache(BOOL *fUseHostIOCache)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *fUseHostIOCache = m->bd->fUseHostIOCache;

    return S_OK;
}

HRESULT StorageController::setUseHostIOCache(BOOL fUseHostIOCache)
{
    /* the machine needs to be mutable */
    AutoMutableStateDependency adep(m->pParent);
    if (FAILED(adep.rc())) return adep.rc();

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    if (m->bd->fUseHostIOCache != !!fUseHostIOCache)
    {
        m->bd.backup();
        m->bd->fUseHostIOCache = !!fUseHostIOCache;

        alock.release();
        AutoWriteLock mlock(m->pParent COMMA_LOCKVAL_SRC_POS);        // m->pParent is const, needs no locking
        m->pParent->i_setModified(Machine::IsModified_Storage);
        mlock.release();

        m->pParent->i_onStorageControllerChange();
    }

    return S_OK;
}

HRESULT StorageController::getBootable(BOOL *fBootable)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    *fBootable = m->bd->fBootable;

    return S_OK;
}

// public methods only for internal purposes
/////////////////////////////////////////////////////////////////////////////

const Utf8Str& StorageController::i_getName() const
{
    return m->bd->strName;
}

StorageControllerType_T StorageController::i_getControllerType() const
{
    return m->bd->mStorageControllerType;
}

StorageBus_T StorageController::i_getStorageBus() const
{
    return m->bd->mStorageBus;
}

ULONG StorageController::i_getInstance() const
{
    return m->bd->mInstance;
}

bool StorageController::i_getBootable() const
{
    return !!m->bd->fBootable;
}

/**
 * Returns S_OK if the given port and device numbers are within the range supported
 * by this controller. If not, it sets an error and returns E_INVALIDARG.
 * @param ulPort
 * @param ulDevice
 * @return
 */
HRESULT StorageController::i_checkPortAndDeviceValid(LONG aControllerPort,
                                                     LONG aDevice)
{
    AutoReadLock alock(this COMMA_LOCKVAL_SRC_POS);

    ULONG portCount = m->bd->mPortCount;
    ULONG devicesPerPort;
    HRESULT rc = m->pSystemProperties->GetMaxDevicesPerPortForStorageBus(m->bd->mStorageBus, &devicesPerPort);
    if (FAILED(rc)) return rc;

    if (   aControllerPort < 0
        || aControllerPort >= (LONG)portCount
        || aDevice < 0
        || aDevice >= (LONG)devicesPerPort
       )
        return setError(E_INVALIDARG,
                        tr("The port and/or device parameter are out of range: port=%d (must be in range [0, %d]), device=%d (must be in range [0, %d])"),
                        (int)aControllerPort, (int)portCount-1, (int)aDevice, (int)devicesPerPort-1);

    return S_OK;
}

/** @note Locks objects for writing! */
void StorageController::i_setBootable(BOOL fBootable)
{
    AutoCaller autoCaller(this);
    AssertComRCReturnVoid(autoCaller.rc());

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    m->bd.backup();
    m->bd->fBootable = fBootable;
}

/** @note Locks objects for writing! */
void StorageController::i_rollback()
{
    AutoCaller autoCaller(this);
    AssertComRCReturnVoid(autoCaller.rc());

    AutoWriteLock alock(this COMMA_LOCKVAL_SRC_POS);

    m->bd.rollback();
}

/**
 *  @note Locks this object for writing, together with the peer object (also
 *  for writing) if there is one.
 */
void StorageController::i_commit()
{
    /* sanity */
    AutoCaller autoCaller(this);
    AssertComRCReturnVoid(autoCaller.rc());

    /* sanity too */
    AutoCaller peerCaller(m->pPeer);
    AssertComRCReturnVoid(peerCaller.rc());

    /* lock both for writing since we modify both (m->pPeer is "master" so locked
     * first) */
    AutoMultiWriteLock2 alock(m->pPeer, this COMMA_LOCKVAL_SRC_POS);

    if (m->bd.isBackedUp())
    {
        m->bd.commit();
        if (m->pPeer)
        {
            // attach new data to the peer and reshare it
            m->pPeer->m->bd.attach(m->bd);
        }
    }
}

/**
 *  Cancels sharing (if any) by making an independent copy of data.
 *  This operation also resets this object's peer to NULL.
 *
 *  @note Locks this object for writing, together with the peer object
 *  represented by @a aThat (locked for reading).
 */
void StorageController::i_unshare()
{
    /* sanity */
    AutoCaller autoCaller(this);
    AssertComRCReturnVoid(autoCaller.rc());

    /* sanity too */
    AutoCaller peerCaller(m->pPeer);
    AssertComRCReturnVoid(peerCaller.rc());

    /* peer is not modified, lock it for reading (m->pPeer is "master" so locked
     * first) */
    AutoReadLock rl(m->pPeer COMMA_LOCKVAL_SRC_POS);
    AutoWriteLock wl(this COMMA_LOCKVAL_SRC_POS);

    if (m->bd.isShared())
    {
        if (!m->bd.isBackedUp())
            m->bd.backup();

        m->bd.commit();
    }

    unconst(m->pPeer) = NULL;
}

Machine* StorageController::i_getMachine()
{
    return m->pParent;
}

ComObjPtr<StorageController> StorageController::i_getPeer()
{
    return m->pPeer;
}

// private methods
/////////////////////////////////////////////////////////////////////////////


/* vi: set tabstop=4 shiftwidth=4 expandtab: */
