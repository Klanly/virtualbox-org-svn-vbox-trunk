/* $Id$ */
/** @file
 * VirtualBox Appliance private data definitions
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

#ifndef ____H_APPLIANCEIMPLPRIVATE
#define ____H_APPLIANCEIMPLPRIVATE

class VirtualSystemDescription;

#include "ovfreader.h"
#include "SecretKeyStore.h"
#include <map>
#include <vector>
#include <iprt/vfs.h>

////////////////////////////////////////////////////////////////////////////////
//
// Appliance data definition
//
////////////////////////////////////////////////////////////////////////////////

typedef std::pair<Utf8Str, Utf8Str> STRPAIR;

typedef std::vector<com::Guid> GUIDVEC;

/* Describe a location for the import/export. The location could be a file on a
 * local hard disk or a remote target based on the supported inet protocols. */
struct LocationInfo
{
    LocationInfo()
      : storageType(VFSType_File) {}
    VFSType_T storageType; /* Which type of storage should be handled */
    Utf8Str strPath;       /* File path for the import/export */
    Utf8Str strHostname;   /* Hostname on remote storage locations (could be empty) */
    Utf8Str strUsername;   /* Username on remote storage locations (could be empty) */
    Utf8Str strPassword;   /* Password on remote storage locations (could be empty) */
};

// opaque private instance data of Appliance class
struct Appliance::Data
{
    enum ApplianceState { ApplianceIdle, ApplianceImporting, ApplianceExporting };
    enum digest_T {SHA1, SHA256};

    Data()
      : state(ApplianceIdle)
      , fManifest(true)
      , fSha256(false)
      , fExportISOImages(false)
      , pReader(NULL)
      , ulWeightForXmlOperation(0)
      , ulWeightForManifestOperation(0)
      , ulTotalDisksMB(0)
      , cDisks(0)
      , m_cPwProvided(0)
    {
    }

    ~Data()
    {
        if (pReader)
        {
            delete pReader;
            pReader = NULL;
        }
    }

    ApplianceState      state;

    LocationInfo        locInfo;        // location info for the currently processed OVF
    bool                fManifest;      // Create a manifest file on export
    bool                fSha256;        // true = SHA256 (OVF 2.0), false = SHA1 (OVF 1.0)
    Utf8Str             strOVFSHADigest;//SHA digest of OVf file. It is stored here after reading OVF file (before import)

    bool                fExportISOImages;// when 1 the ISO images are exported
    bool                fX509;// wether X509 is used or not

    RTCList<ImportOptions_T> optListImport;
    RTCList<ExportOptions_T> optListExport;

    ovf::OVFReader      *pReader;

    std::list< ComObjPtr<VirtualSystemDescription> >
                        virtualSystemDescriptions;

    std::list<Utf8Str>  llWarnings;

    ULONG               ulWeightForXmlOperation;
    ULONG               ulWeightForManifestOperation;
    ULONG               ulTotalDisksMB;
    ULONG               cDisks;

    std::list<Guid>     llGuidsMachinesCreated;

    /** Sequence of password identifiers to encrypt disk images during export. */
    std::vector<com::Utf8Str> m_vecPasswordIdentifiers;
    /** Map to get all medium identifiers assoicated with a given password identifier. */
    std::map<com::Utf8Str, GUIDVEC> m_mapPwIdToMediumIds;
    /** Secret key store used to hold the passwords during export. */
    SecretKeyStore            *m_pSecretKeyStore;
    /** Number of passwords provided. */
    uint32_t                  m_cPwProvided;
};

struct Appliance::XMLStack
{
    std::map<Utf8Str, const VirtualSystemDescriptionEntry*> mapDisks;
    std::map<Utf8Str, bool> mapNetworks;
};

struct Appliance::TaskOVF
{
    enum TaskType
    {
        Read,
        Import,
        Write
    };

    TaskOVF(Appliance *aThat,
            TaskType aType,
            LocationInfo aLocInfo,
            ComObjPtr<Progress> &aProgress)
      : pAppliance(aThat),
        taskType(aType),
        locInfo(aLocInfo),
        pProgress(aProgress),
        enFormat(ovf::OVFVersion_unknown),
        rc(S_OK)
    {}

    static DECLCALLBACK(int) updateProgress(unsigned uPercent, void *pvUser);

    HRESULT startThread();

    Appliance *pAppliance;
    TaskType taskType;
    const LocationInfo locInfo;
    ComObjPtr<Progress> pProgress;

    ovf::OVFVersion_T enFormat;

    HRESULT rc;
};

struct MyHardDiskAttachment
{
    ComPtr<IMachine>    pMachine;
    Bstr                controllerType;
    int32_t             lControllerPort;        // 0-29 for SATA
    int32_t             lDevice;                // IDE: 0 or 1, otherwise 0 always
};

/**
 * Used by Appliance::importMachineGeneric() to store
 * input parameters and rollback information.
 */
struct Appliance::ImportStack
{
    // input pointers
    const LocationInfo              &locInfo;           // ptr to location info from Appliance::importFS()
    Utf8Str                         strSourceDir;       // directory where source files reside
    const ovf::DiskImagesMap        &mapDisks;          // ptr to disks map in OVF
    ComObjPtr<Progress>             &pProgress;         // progress object passed into Appliance::importFS()

    // input parameters from VirtualSystemDescriptions
    Utf8Str                         strNameVBox;        // VM name
    Utf8Str                         strMachineFolder;   // FQ host folder where the VirtualBox machine would be created
    Utf8Str                         strOsTypeVBox;      // VirtualBox guest OS type as string
    Utf8Str                         strDescription;
    uint32_t                        cCPUs;              // CPU count
    bool                            fForceHWVirt;       // if true, we force enabling hardware virtualization
    bool                            fForceIOAPIC;       // if true, we force enabling the IOAPIC
    uint32_t                        ulMemorySizeMB;     // virtual machine RAM in megabytes
#ifdef VBOX_WITH_USB
    bool                            fUSBEnabled;
#endif
    Utf8Str                         strAudioAdapter;    // if not empty, then the guest has audio enabled, and this is the decimal
                                                        // representation of the audio adapter (should always be "0" for AC97 presently)

    // session (not initially created)
    ComPtr<ISession>                pSession;           // session opened in Appliance::importFS() for machine manipulation
    bool                            fSessionOpen;       // true if the pSession is currently open and needs closing

    // a list of images that we created/imported; this is initially empty
    // and will be cleaned up on errors
    std::list<MyHardDiskAttachment> llHardDiskAttachments;      // disks that were attached
    std::list<STRPAIR>              llSrcDisksDigest;           // Digests of the source disks
    std::map<Utf8Str , Utf8Str> mapNewUUIDsToOriginalUUIDs;

    ImportStack(const LocationInfo &aLocInfo,
                const ovf::DiskImagesMap &aMapDisks,
                ComObjPtr<Progress> &aProgress)
        : locInfo(aLocInfo),
          mapDisks(aMapDisks),
          pProgress(aProgress),
          cCPUs(1),
          fForceHWVirt(false),
          fForceIOAPIC(false),
          ulMemorySizeMB(0),
          fSessionOpen(false)
    {
        // disk images have to be on the same place as the OVF file. So
        // strip the filename out of the full file path
        strSourceDir = aLocInfo.strPath;
        strSourceDir.stripFilename();
    }

    HRESULT restoreOriginalUUIDOfAttachedDevice(settings::MachineConfigFile *config);
    HRESULT saveOriginalUUIDOfAttachedDevice(settings::AttachedDevice &device,
                                                  const Utf8Str &newlyUuid);
};

////////////////////////////////////////////////////////////////////////////////
//
// VirtualSystemDescription data definition
//
////////////////////////////////////////////////////////////////////////////////

struct VirtualSystemDescription::Data
{
    std::vector<VirtualSystemDescriptionEntry>
                            maDescriptions;     // item descriptions

    ComPtr<Machine>         pMachine;           // VirtualBox machine this description was exported from (export only)

    settings::MachineConfigFile
                            *pConfig;           // machine config created from <vbox:Machine> element if found (import only)
};

////////////////////////////////////////////////////////////////////////////////
//
// Internal helpers
//
////////////////////////////////////////////////////////////////////////////////

void convertCIMOSType2VBoxOSType(Utf8Str &strType, ovf::CIMOSType_T c, const Utf8Str &cStr);

ovf::CIMOSType_T convertVBoxOSType2CIMOSType(const char *pcszVBox, BOOL fLongMode);

Utf8Str convertNetworkAttachmentTypeToString(NetworkAttachmentType_T type);


typedef struct SHASTORAGE
{
    PVDINTERFACE pVDImageIfaces;
    bool         fCreateDigest;
    bool         fSha256;        /* false = SHA1 (OVF 1.x), true = SHA256 (OVF 2.0) */
    Utf8Str      strDigest;
} SHASTORAGE, *PSHASTORAGE;

PVDINTERFACEIO ShaCreateInterface();
PVDINTERFACEIO FileCreateInterface();
PVDINTERFACEIO tarWriterCreateInterface(void);

/** Pointer to the instance data for the fssRdOnly_ methods. */
typedef struct FSSRDONLYINTERFACEIO *PFSSRDONLYINTERFACEIO;

int  fssRdOnlyCreateInterfaceForTarFile(const char *pszFilename, PFSSRDONLYINTERFACEIO *pTarIo);
void fssRdOnlyDestroyInterface(PFSSRDONLYINTERFACEIO pFssIo);
int  fssRdOnlyGetCurrentName(PFSSRDONLYINTERFACEIO pFssIo, const char **ppszName);
int  fssRdOnlySkipCurrent(PFSSRDONLYINTERFACEIO pFssIo);
bool fssRdOnlyIsCurrentDirectory(PFSSRDONLYINTERFACEIO pFssIo);

int readFileIntoBuffer(const char *pcszFilename, void **ppvBuf, size_t *pcbSize, PVDINTERFACEIO pIfIo, void *pvUser);
int writeBufferToFile(const char *pcszFilename, void *pvBuf, size_t cbSize, PVDINTERFACEIO pIfIo, void *pvUser);
int decompressImageAndSave(const char *pcszFullFilenameIn, const char *pcszFullFilenameOut, PVDINTERFACEIO pIfIo, void *pvUser);
int copyFileAndCalcShaDigest(const char *pcszSourceFilename, const char *pcszTargetFilename, PVDINTERFACEIO pIfIo, void *pvUser);
#endif // !____H_APPLIANCEIMPLPRIVATE

