/* $Id$ */
/** @file
 * UsbMSD - USB Mass Storage Device Emulation.
 */

/*
 * Copyright (C) 2007-2015 Oracle Corporation
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
#define LOG_GROUP   LOG_GROUP_USB_MSD
#include <VBox/vmm/pdmusb.h>
#include <VBox/log.h>
#include <VBox/err.h>
#include <VBox/scsi.h>
#include <iprt/assert.h>
#include <iprt/critsect.h>
#include <iprt/mem.h>
#include <iprt/semaphore.h>
#include <iprt/string.h>
#include <iprt/uuid.h>
#include "VBoxDD.h"


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** @name USB MSD string IDs
 * @{ */
#define USBMSD_STR_ID_MANUFACTURER  1
#define USBMSD_STR_ID_PRODUCT_HD    2
#define USBMSD_STR_ID_PRODUCT_CDROM 3
/** @} */

/** @name USB MSD vendor and product IDs
 * @{ */
#define VBOX_USB_VENDOR             0x80EE
#define USBMSD_PID_HD               0x0030
#define USBMSD_PID_CD               0x0031
/** @} */

/** Saved state version. */
#define USB_MSD_SAVED_STATE_VERSION 1


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/

/**
 * USB MSD Command Block Wrapper or CBW. The command block
 * itself (CBWCB) contains protocol-specific data (here SCSI).
 */
#pragma pack(1)
typedef struct USBCBW
{
    uint32_t    dCBWSignature;
#define USBCBW_SIGNATURE        UINT32_C(0x43425355)
    uint32_t    dCBWTag;
    uint32_t    dCBWDataTransferLength;
    uint8_t     bmCBWFlags;
#define USBCBW_DIR_MASK         RT_BIT(7)
#define USBCBW_DIR_OUT          0
#define USBCBW_DIR_IN           RT_BIT(7)
    uint8_t     bCBWLun;
    uint8_t     bCBWCBLength;
    uint8_t     CBWCB[16];
} USBCBW;
#pragma pack()
AssertCompileSize(USBCBW, 31);
/** Pointer to a Command Block Wrapper. */
typedef USBCBW *PUSBCBW;
/** Pointer to a const Command Block Wrapper. */
typedef const USBCBW *PCUSBCBW;

/**
 * USB MSD Command Status Wrapper or CSW.
 */
#pragma pack(1)
typedef struct USBCSW
{
    uint32_t    dCSWSignature;
#define USBCSW_SIGNATURE            UINT32_C(0x53425355)
    uint32_t    dCSWTag;
    uint32_t    dCSWDataResidue;
#define USBCSW_STATUS_OK            UINT8_C(0)
#define USBCSW_STATUS_FAILED        UINT8_C(1)
#define USBCSW_STATUS_PHASE_ERROR   UINT8_C(2)
    uint8_t     bCSWStatus;
} USBCSW;
#pragma pack()
AssertCompileSize(USBCSW, 13);
/** Pointer to a Command Status Wrapper. */
typedef USBCSW *PUSBCSW;
/** Pointer to a const Command Status Wrapper. */
typedef const USBCSW *PCUSBCSW;


/**
 * The USB MSD request state.
 */
typedef enum USBMSDREQSTATE
{
    /** Invalid status. */
    USBMSDREQSTATE_INVALID = 0,
    /** Ready to receive a new SCSI command. */
    USBMSDREQSTATE_READY,
    /** Waiting for the host to supply data. */
    USBMSDREQSTATE_DATA_FROM_HOST,
    /** The SCSI request is being executed by the driver. */
    USBMSDREQSTATE_EXECUTING,
    /** Have (more) data for the host. */
    USBMSDREQSTATE_DATA_TO_HOST,
    /** Waiting to supply status information to the host. */
    USBMSDREQSTATE_STATUS,
    /** Destroy the request upon completion.
     * This is set when the SCSI request doesn't complete before for the device or
     * mass storage reset operation times out.  USBMSD::pReq will be set to NULL
     * and the only reference to this request will be with DrvSCSI. */
    USBMSDREQSTATE_DESTROY_ON_COMPLETION,
    /** The end of the valid states. */
    USBMSDREQSTATE_END,
    /** 32bit blow up hack. */
    USBMSDREQSTATE_32BIT_HACK = 0x7fffffff
} USBMSDREQSTATE;


/**
 * A pending USB MSD request.
 */
typedef struct USBMSDREQ
{
    /** The state of the request. */
    USBMSDREQSTATE      enmState;
    /** The size of the data buffer. */
    uint32_t            cbBuf;
    /** Pointer to the data buffer. */
    uint8_t            *pbBuf;
    /** Current buffer offset. */
    uint32_t            offBuf;
    /** The current Cbw when we're in the pending state. */
    USBCBW              Cbw;
    /** The current SCSI request. */
    PDMSCSIREQUEST      ScsiReq;
    /** The scatter-gather segment used by ScsiReq for describing pbBuf. */
    RTSGSEG             ScsiReqSeg;
    /** The sense buffer for the current SCSI request. */
    uint8_t             ScsiReqSense[64];
    /** The status of a completed SCSI request. */
    int                 iScsiReqStatus;
    /** Pointer to the USB device instance owning it. */
    PPDMUSBINS          pUsbIns;
} USBMSDREQ;
/** Pointer to a USB MSD request. */
typedef USBMSDREQ *PUSBMSDREQ;


/**
 * Endpoint status data.
 */
typedef struct USBMSDEP
{
    bool                fHalted;
} USBMSDEP;
/** Pointer to the endpoint status. */
typedef USBMSDEP *PUSBMSDEP;


/**
 * A URB queue.
 */
typedef struct USBMSDURBQUEUE
{
    /** The head pointer. */
    PVUSBURB            pHead;
    /** Where to insert the next entry. */
    PVUSBURB           *ppTail;
} USBMSDURBQUEUE;
/** Pointer to a URB queue. */
typedef USBMSDURBQUEUE *PUSBMSDURBQUEUE;
/** Pointer to a const URB queue. */
typedef USBMSDURBQUEUE const *PCUSBMSDURBQUEUE;


/**
 * The USB MSD instance data.
 */
typedef struct USBMSD
{
    /** Pointer back to the PDM USB Device instance structure. */
    PPDMUSBINS          pUsbIns;
    /** Critical section protecting the device state. */
    RTCRITSECT          CritSect;

    /** The current configuration.
     * (0 - default, 1 - the only, i.e configured.) */
    uint8_t             bConfigurationValue;
#if 0
    /** The state of the MSD (state machine).*/
    USBMSDSTATE         enmState;
#endif
    /** Endpoint 0 is the default control pipe, 1 is the host->dev bulk pipe and 2
     * is the dev->host one. */
    USBMSDEP            aEps[3];
    /** The current request. */
    PUSBMSDREQ          pReq;

    /** Pending to-host queue.
     * The URBs waiting here are pending the completion of the current request and
     * data or status to become available.
     */
    USBMSDURBQUEUE      ToHostQueue;

    /** Done queue
     * The URBs stashed here are waiting to be reaped. */
    USBMSDURBQUEUE      DoneQueue;
    /** Signalled when adding an URB to the done queue and fHaveDoneQueueWaiter
     *  is set. */
    RTSEMEVENT          hEvtDoneQueue;
    /** Someone is waiting on the done queue. */
    bool                fHaveDoneQueueWaiter;

    /** Whether to signal the reset semaphore when the current request completes. */
    bool                fSignalResetSem;
    /** Semaphore usbMsdUsbReset waits on when a request is executing at reset
     *  time.  Only signalled when fSignalResetSem is set. */
    RTSEMEVENTMULTI     hEvtReset;
    /** The reset URB.
     * This is waiting for SCSI request completion before finishing the reset. */
    PVUSBURB            pResetUrb;
    /** Indicates that PDMUsbHlpAsyncNotificationCompleted should be called when
     * the MSD is entering the idle state. */
    volatile bool       fSignalIdle;

    /** Indicates that this device is a CD-ROM. */
    bool                fIsCdrom;

    /**
     * LUN\#0 data.
     */
    struct
    {
        /** The base interface for LUN\#0. */
        PDMIBASE            IBase;
        /** The SCSI port interface for LUN\#0  */
        PDMISCSIPORT        IScsiPort;

        /** The base interface for the SCSI driver connected to LUN\#0. */
        PPDMIBASE           pIBase;
        /** The SCSI connector interface for the SCSI driver connected to LUN\#0. */
        PPDMISCSICONNECTOR  pIScsiConnector;
    } Lun0;

} USBMSD;
/** Pointer to the USB MSD instance data. */
typedef USBMSD *PUSBMSD;


/*********************************************************************************************************************************
*   Global Variables                                                                                                             *
*********************************************************************************************************************************/
static const PDMUSBDESCCACHESTRING g_aUsbMsdStrings_en_US[] =
{
    { USBMSD_STR_ID_MANUFACTURER,   "VirtualBox"   },
    { USBMSD_STR_ID_PRODUCT_HD,     "USB Harddisk" },
    { USBMSD_STR_ID_PRODUCT_CDROM,  "USB CD-ROM"   }
};

static const PDMUSBDESCCACHELANG g_aUsbMsdLanguages[] =
{
    { 0x0409, RT_ELEMENTS(g_aUsbMsdStrings_en_US), g_aUsbMsdStrings_en_US }
};

static const VUSBDESCENDPOINTEX g_aUsbMsdEndpointDescsFS[2] =
{
    {
        {
            /* .bLength = */            sizeof(VUSBDESCENDPOINT),
            /* .bDescriptorType = */    VUSB_DT_ENDPOINT,
            /* .bEndpointAddress = */   0x81 /* ep=1, in */,
            /* .bmAttributes = */       2 /* bulk */,
            /* .wMaxPacketSize = */     64 /* maximum possible */,
            /* .bInterval = */          0 /* not applicable for bulk EP */
        },
        /* .pvMore = */     NULL,
        /* .pvClass = */    NULL,
        /* .cbClass = */    0,
        /* .pvSsepc = */    NULL,
        /* .cbSsepc = */    0
    },
    {
        {
            /* .bLength = */            sizeof(VUSBDESCENDPOINT),
            /* .bDescriptorType = */    VUSB_DT_ENDPOINT,
            /* .bEndpointAddress = */   0x02 /* ep=2, out */,
            /* .bmAttributes = */       2 /* bulk */,
            /* .wMaxPacketSize = */     64 /* maximum possible */,
            /* .bInterval = */          0 /* not applicable for bulk EP */
        },
        /* .pvMore = */     NULL,
        /* .pvClass = */    NULL,
        /* .cbClass = */    0,
        /* .pvSsepc = */    NULL,
        /* .cbSsepc = */    0
    }
};

static const VUSBDESCENDPOINTEX g_aUsbMsdEndpointDescsHS[2] =
{
    {
        {
            /* .bLength = */            sizeof(VUSBDESCENDPOINT),
            /* .bDescriptorType = */    VUSB_DT_ENDPOINT,
            /* .bEndpointAddress = */   0x81 /* ep=1, in */,
            /* .bmAttributes = */       2 /* bulk */,
            /* .wMaxPacketSize = */     512 /* HS bulk packet size */,
            /* .bInterval = */          0 /* no NAKs */
        },
        /* .pvMore = */     NULL,
        /* .pvClass = */    NULL,
        /* .cbClass = */    0,
        /* .pvSsepc = */    NULL,
        /* .cbSsepc = */    0
    },
    {
        {
            /* .bLength = */            sizeof(VUSBDESCENDPOINT),
            /* .bDescriptorType = */    VUSB_DT_ENDPOINT,
            /* .bEndpointAddress = */   0x02 /* ep=2, out */,
            /* .bmAttributes = */       2 /* bulk */,
            /* .wMaxPacketSize = */     512 /* HS bulk packet size */,
            /* .bInterval = */          0 /* no NAKs */
        },
        /* .pvMore = */     NULL,
        /* .pvClass = */    NULL,
        /* .cbClass = */    0,
        /* .pvSsepc = */    NULL,
        /* .cbSsepc = */    0
    }
};

static const VUSBDESCSSEPCOMPANION g_aUsbMsdEpCompanionSS =
{
    /* .bLength = */            sizeof(VUSBDESCSSEPCOMPANION),
    /* .bDescriptorType = */    VUSB_DT_SS_ENDPOINT_COMPANION,
    /* .bMaxBurst = */          15  /* we can burst all the way */,
    /* .bmAttributes = */       0   /* no streams */,
    /* .wBytesPerInterval = */  0   /* not a periodic endpoint */
};

static const VUSBDESCENDPOINTEX g_aUsbMsdEndpointDescsSS[2] =
{
    {
        {
            /* .bLength = */            sizeof(VUSBDESCENDPOINT),
            /* .bDescriptorType = */    VUSB_DT_ENDPOINT,
            /* .bEndpointAddress = */   0x81 /* ep=1, in */,
            /* .bmAttributes = */       2 /* bulk */,
            /* .wMaxPacketSize = */     1024 /* SS bulk packet size */,
            /* .bInterval = */          0 /* no NAKs */
        },
        /* .pvMore = */     NULL,
        /* .pvClass = */    NULL,
        /* .cbClass = */    0,
        /* .pvSsepc = */    &g_aUsbMsdEpCompanionSS,
        /* .cbSsepc = */    sizeof(g_aUsbMsdEpCompanionSS)
    },
    {
        {
            /* .bLength = */            sizeof(VUSBDESCENDPOINT),
            /* .bDescriptorType = */    VUSB_DT_ENDPOINT,
            /* .bEndpointAddress = */   0x02 /* ep=2, out */,
            /* .bmAttributes = */       2 /* bulk */,
            /* .wMaxPacketSize = */     1024 /* SS bulk packet size */,
            /* .bInterval = */          0 /* no NAKs */
        },
        /* .pvMore = */     NULL,
        /* .pvClass = */    NULL,
        /* .cbClass = */    0,
        /* .pvSsepc = */    &g_aUsbMsdEpCompanionSS,
        /* .cbSsepc = */    sizeof(g_aUsbMsdEpCompanionSS)
    }
};

static const VUSBDESCINTERFACEEX g_UsbMsdInterfaceDescFS =
{
    {
        /* .bLength = */                sizeof(VUSBDESCINTERFACE),
        /* .bDescriptorType = */        VUSB_DT_INTERFACE,
        /* .bInterfaceNumber = */       0,
        /* .bAlternateSetting = */      0,
        /* .bNumEndpoints = */          2,
        /* .bInterfaceClass = */        8 /* Mass Storage */,
        /* .bInterfaceSubClass = */     6 /* SCSI transparent command set */,
        /* .bInterfaceProtocol = */     0x50 /* Bulk-Only Transport */,
        /* .iInterface = */             0
    },
    /* .pvMore = */     NULL,
    /* .pvClass = */    NULL,
    /* .cbClass = */    0,
    &g_aUsbMsdEndpointDescsFS[0],
    /* .pIAD = */ NULL,
    /* .cbIAD = */ 0
};

static const VUSBDESCINTERFACEEX g_UsbMsdInterfaceDescHS =
{
    {
        /* .bLength = */                sizeof(VUSBDESCINTERFACE),
        /* .bDescriptorType = */        VUSB_DT_INTERFACE,
        /* .bInterfaceNumber = */       0,
        /* .bAlternateSetting = */      0,
        /* .bNumEndpoints = */          2,
        /* .bInterfaceClass = */        8 /* Mass Storage */,
        /* .bInterfaceSubClass = */     6 /* SCSI transparent command set */,
        /* .bInterfaceProtocol = */     0x50 /* Bulk-Only Transport */,
        /* .iInterface = */             0
    },
    /* .pvMore = */     NULL,
    /* .pvClass = */    NULL,
    /* .cbClass = */    0,
    &g_aUsbMsdEndpointDescsHS[0],
    /* .pIAD = */ NULL,
    /* .cbIAD = */ 0
};

static const VUSBDESCINTERFACEEX g_UsbMsdInterfaceDescSS =
{
    {
        /* .bLength = */                sizeof(VUSBDESCINTERFACE),
        /* .bDescriptorType = */        VUSB_DT_INTERFACE,
        /* .bInterfaceNumber = */       0,
        /* .bAlternateSetting = */      0,
        /* .bNumEndpoints = */          2,
        /* .bInterfaceClass = */        8 /* Mass Storage */,
        /* .bInterfaceSubClass = */     6 /* SCSI transparent command set */,
        /* .bInterfaceProtocol = */     0x50 /* Bulk-Only Transport */,
        /* .iInterface = */             0
    },
    /* .pvMore = */     NULL,
    /* .pvClass = */    NULL,
    /* .cbClass = */    0,
    &g_aUsbMsdEndpointDescsSS[0],
    /* .pIAD = */ NULL,
    /* .cbIAD = */ 0
};

static const VUSBINTERFACE g_aUsbMsdInterfacesFS[] =
{
    { &g_UsbMsdInterfaceDescFS, /* .cSettings = */ 1 },
};

static const VUSBINTERFACE g_aUsbMsdInterfacesHS[] =
{
    { &g_UsbMsdInterfaceDescHS, /* .cSettings = */ 1 },
};

static const VUSBINTERFACE g_aUsbMsdInterfacesSS[] =
{
    { &g_UsbMsdInterfaceDescSS, /* .cSettings = */ 1 },
};

static const VUSBDESCCONFIGEX g_UsbMsdConfigDescFS =
{
    {
        /* .bLength = */            sizeof(VUSBDESCCONFIG),
        /* .bDescriptorType = */    VUSB_DT_CONFIG,
        /* .wTotalLength = */       0 /* recalculated on read */,
        /* .bNumInterfaces = */     RT_ELEMENTS(g_aUsbMsdInterfacesFS),
        /* .bConfigurationValue =*/ 1,
        /* .iConfiguration = */     0,
        /* .bmAttributes = */       RT_BIT(7),
        /* .MaxPower = */           50 /* 100mA */
    },
    NULL,                           /* pvMore */
    &g_aUsbMsdInterfacesFS[0],
    NULL                            /* pvOriginal */
};

static const VUSBDESCCONFIGEX g_UsbMsdConfigDescHS =
{
    {
        /* .bLength = */            sizeof(VUSBDESCCONFIG),
        /* .bDescriptorType = */    VUSB_DT_CONFIG,
        /* .wTotalLength = */       0 /* recalculated on read */,
        /* .bNumInterfaces = */     RT_ELEMENTS(g_aUsbMsdInterfacesHS),
        /* .bConfigurationValue =*/ 1,
        /* .iConfiguration = */     0,
        /* .bmAttributes = */       RT_BIT(7),
        /* .MaxPower = */           50 /* 100mA */
    },
    NULL,                           /* pvMore */
    &g_aUsbMsdInterfacesHS[0],
    NULL                            /* pvOriginal */
};

static const VUSBDESCCONFIGEX g_UsbMsdConfigDescSS =
{
    {
        /* .bLength = */            sizeof(VUSBDESCCONFIG),
        /* .bDescriptorType = */    VUSB_DT_CONFIG,
        /* .wTotalLength = */       0 /* recalculated on read */,
        /* .bNumInterfaces = */     RT_ELEMENTS(g_aUsbMsdInterfacesSS),
        /* .bConfigurationValue =*/ 1,
        /* .iConfiguration = */     0,
        /* .bmAttributes = */       RT_BIT(7),
        /* .MaxPower = */           50 /* 100mA */
    },
    NULL,                           /* pvMore */
    &g_aUsbMsdInterfacesSS[0],
    NULL                            /* pvOriginal */
};

static const VUSBDESCDEVICE g_UsbMsdDeviceDesc20 =
{
    /* .bLength = */                sizeof(g_UsbMsdDeviceDesc20),
    /* .bDescriptorType = */        VUSB_DT_DEVICE,
    /* .bcdUsb = */                 0x200, /* USB 2.0 */
    /* .bDeviceClass = */           0 /* Class specified in the interface desc. */,
    /* .bDeviceSubClass = */        0 /* Subclass specified in the interface desc. */,
    /* .bDeviceProtocol = */        0 /* Protocol specified in the interface desc. */,
    /* .bMaxPacketSize0 = */        64,
    /* .idVendor = */               VBOX_USB_VENDOR,
    /* .idProduct = */              USBMSD_PID_HD,
    /* .bcdDevice = */              0x0100, /* 1.0 */
    /* .iManufacturer = */          USBMSD_STR_ID_MANUFACTURER,
    /* .iProduct = */               USBMSD_STR_ID_PRODUCT_HD,
    /* .iSerialNumber = */          0,
    /* .bNumConfigurations = */     1
};

static const VUSBDESCDEVICE g_UsbCdDeviceDesc20 =
{
    /* .bLength = */                sizeof(g_UsbCdDeviceDesc20),
    /* .bDescriptorType = */        VUSB_DT_DEVICE,
    /* .bcdUsb = */                 0x200, /* USB 2.0 */
    /* .bDeviceClass = */           0 /* Class specified in the interface desc. */,
    /* .bDeviceSubClass = */        0 /* Subclass specified in the interface desc. */,
    /* .bDeviceProtocol = */        0 /* Protocol specified in the interface desc. */,
    /* .bMaxPacketSize0 = */        64,
    /* .idVendor = */               VBOX_USB_VENDOR,
    /* .idProduct = */              USBMSD_PID_CD,
    /* .bcdDevice = */              0x0100, /* 1.0 */
    /* .iManufacturer = */          USBMSD_STR_ID_MANUFACTURER,
    /* .iProduct = */               USBMSD_STR_ID_PRODUCT_CDROM,
    /* .iSerialNumber = */          0,
    /* .bNumConfigurations = */     1
};

static const VUSBDESCDEVICE g_UsbMsdDeviceDesc30 =
{
    /* .bLength = */                sizeof(g_UsbMsdDeviceDesc30),
    /* .bDescriptorType = */        VUSB_DT_DEVICE,
    /* .bcdUsb = */                 0x300, /* USB 2.0 */
    /* .bDeviceClass = */           0 /* Class specified in the interface desc. */,
    /* .bDeviceSubClass = */        0 /* Subclass specified in the interface desc. */,
    /* .bDeviceProtocol = */        0 /* Protocol specified in the interface desc. */,
    /* .bMaxPacketSize0 = */        9 /* 512, the only option for USB3. */,
    /* .idVendor = */               VBOX_USB_VENDOR,
    /* .idProduct = */              USBMSD_PID_HD,
    /* .bcdDevice = */              0x0110, /* 1.10 */
    /* .iManufacturer = */          USBMSD_STR_ID_MANUFACTURER,
    /* .iProduct = */               USBMSD_STR_ID_PRODUCT_HD,
    /* .iSerialNumber = */          0,
    /* .bNumConfigurations = */     1
};

static const VUSBDESCDEVICE g_UsbCdDeviceDesc30 =
{
    /* .bLength = */                sizeof(g_UsbCdDeviceDesc30),
    /* .bDescriptorType = */        VUSB_DT_DEVICE,
    /* .bcdUsb = */                 0x300, /* USB 2.0 */
    /* .bDeviceClass = */           0 /* Class specified in the interface desc. */,
    /* .bDeviceSubClass = */        0 /* Subclass specified in the interface desc. */,
    /* .bDeviceProtocol = */        0 /* Protocol specified in the interface desc. */,
    /* .bMaxPacketSize0 = */        9 /* 512, the only option for USB3. */,
    /* .idVendor = */               VBOX_USB_VENDOR,
    /* .idProduct = */              USBMSD_PID_CD,
    /* .bcdDevice = */              0x0110, /* 1.10 */
    /* .iManufacturer = */          USBMSD_STR_ID_MANUFACTURER,
    /* .iProduct = */               USBMSD_STR_ID_PRODUCT_CDROM,
    /* .iSerialNumber = */          0,
    /* .bNumConfigurations = */     1
};

static const VUSBDEVICEQUALIFIER g_UsbMsdDeviceQualifier =
{
    /* .bLength = */                sizeof(g_UsbMsdDeviceQualifier),
    /* .bDescriptorType = */        VUSB_DT_DEVICE_QUALIFIER,
    /* .bcdUsb = */                 0x200, /* USB 2.0 */
    /* .bDeviceClass = */           0 /* Class specified in the interface desc. */,
    /* .bDeviceSubClass = */        0 /* Subclass specified in the interface desc. */,
    /* .bDeviceProtocol = */        0 /* Protocol specified in the interface desc. */,
    /* .bMaxPacketSize0 = */        64,
    /* .bNumConfigurations = */     1,
    /* .bReserved = */              0
};

static const struct {
    VUSBDESCBOS         bos;
    VUSBDESCSSDEVCAP    sscap;
} g_UsbMsdBOS =
{
    {
        /* .bLength = */                sizeof(g_UsbMsdBOS.bos),
        /* .bDescriptorType = */        VUSB_DT_BOS,
        /* .wTotalLength = */           sizeof(g_UsbMsdBOS),
        /* .bNumDeviceCaps = */         1
    },
    {
        /* .bLength = */                sizeof(VUSBDESCSSDEVCAP),
        /* .bDescriptorType = */        VUSB_DT_DEVICE_CAPABILITY,
        /* .bDevCapabilityType = */     VUSB_DCT_SUPERSPEED_USB,
        /* .bmAttributes = */           0   /* No LTM. */,
        /* .wSpeedsSupported = */       0xe /* Any speed is good. */,
        /* .bFunctionalitySupport = */  2   /* Want HS at least. */,
        /* .bU1DevExitLat = */          0,  /* We are blazingly fast. */
        /* .wU2DevExitLat = */          0
    }
};

static const PDMUSBDESCCACHE g_UsbMsdDescCacheFS =
{
    /* .pDevice = */                &g_UsbMsdDeviceDesc20,
    /* .paConfigs = */              &g_UsbMsdConfigDescFS,
    /* .paLanguages = */            g_aUsbMsdLanguages,
    /* .cLanguages = */             RT_ELEMENTS(g_aUsbMsdLanguages),
    /* .fUseCachedDescriptors = */  true,
    /* .fUseCachedStringsDescriptors = */ true
};

static const PDMUSBDESCCACHE g_UsbCdDescCacheFS =
{
    /* .pDevice = */                &g_UsbCdDeviceDesc20,
    /* .paConfigs = */              &g_UsbMsdConfigDescFS,
    /* .paLanguages = */            g_aUsbMsdLanguages,
    /* .cLanguages = */             RT_ELEMENTS(g_aUsbMsdLanguages),
    /* .fUseCachedDescriptors = */  true,
    /* .fUseCachedStringsDescriptors = */ true
};

static const PDMUSBDESCCACHE g_UsbMsdDescCacheHS =
{
    /* .pDevice = */                &g_UsbMsdDeviceDesc20,
    /* .paConfigs = */              &g_UsbMsdConfigDescHS,
    /* .paLanguages = */            g_aUsbMsdLanguages,
    /* .cLanguages = */             RT_ELEMENTS(g_aUsbMsdLanguages),
    /* .fUseCachedDescriptors = */  true,
    /* .fUseCachedStringsDescriptors = */ true
};

static const PDMUSBDESCCACHE g_UsbCdDescCacheHS =
{
    /* .pDevice = */                &g_UsbCdDeviceDesc20,
    /* .paConfigs = */              &g_UsbMsdConfigDescHS,
    /* .paLanguages = */            g_aUsbMsdLanguages,
    /* .cLanguages = */             RT_ELEMENTS(g_aUsbMsdLanguages),
    /* .fUseCachedDescriptors = */  true,
    /* .fUseCachedStringsDescriptors = */ true
};

static const PDMUSBDESCCACHE g_UsbMsdDescCacheSS =
{
    /* .pDevice = */                &g_UsbMsdDeviceDesc30,
    /* .paConfigs = */              &g_UsbMsdConfigDescSS,
    /* .paLanguages = */            g_aUsbMsdLanguages,
    /* .cLanguages = */             RT_ELEMENTS(g_aUsbMsdLanguages),
    /* .fUseCachedDescriptors = */  true,
    /* .fUseCachedStringsDescriptors = */ true
};

static const PDMUSBDESCCACHE g_UsbCdDescCacheSS =
{
    /* .pDevice = */                &g_UsbCdDeviceDesc30,
    /* .paConfigs = */              &g_UsbMsdConfigDescSS,
    /* .paLanguages = */            g_aUsbMsdLanguages,
    /* .cLanguages = */             RT_ELEMENTS(g_aUsbMsdLanguages),
    /* .fUseCachedDescriptors = */  true,
    /* .fUseCachedStringsDescriptors = */ true
};


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
static int  usbMsdHandleBulkDevToHost(PUSBMSD pThis, PUSBMSDEP pEp, PVUSBURB pUrb);


/**
 * Initializes an URB queue.
 *
 * @param   pQueue              The URB queue.
 */
static void usbMsdQueueInit(PUSBMSDURBQUEUE pQueue)
{
    pQueue->pHead = NULL;
    pQueue->ppTail = &pQueue->pHead;
}



/**
 * Inserts an URB at the end of the queue.
 *
 * @param   pQueue              The URB queue.
 * @param   pUrb                The URB to insert.
 */
DECLINLINE(void) usbMsdQueueAddTail(PUSBMSDURBQUEUE pQueue, PVUSBURB pUrb)
{
    pUrb->Dev.pNext = NULL;
    *pQueue->ppTail = pUrb;
    pQueue->ppTail  = &pUrb->Dev.pNext;
}


/**
 * Unlinks the head of the queue and returns it.
 *
 * @returns The head entry.
 * @param   pQueue              The URB queue.
 */
DECLINLINE(PVUSBURB) usbMsdQueueRemoveHead(PUSBMSDURBQUEUE pQueue)
{
    PVUSBURB pUrb = pQueue->pHead;
    if (pUrb)
    {
        PVUSBURB pNext = pUrb->Dev.pNext;
        pQueue->pHead = pNext;
        if (!pNext)
            pQueue->ppTail = &pQueue->pHead;
        else
            pUrb->Dev.pNext = NULL;
    }
    return pUrb;
}


/**
 * Removes an URB from anywhere in the queue.
 *
 * @returns true if found, false if not.
 * @param   pQueue              The URB queue.
 * @param   pUrb                The URB to remove.
 */
DECLINLINE(bool) usbMsdQueueRemove(PUSBMSDURBQUEUE pQueue, PVUSBURB pUrb)
{
    PVUSBURB pCur = pQueue->pHead;
    if (pCur == pUrb)
        pQueue->pHead = pUrb->Dev.pNext;
    else
    {
        while (pCur)
        {
            if (pCur->Dev.pNext == pUrb)
            {
                pCur->Dev.pNext = pUrb->Dev.pNext;
                break;
            }
            pCur = pCur->Dev.pNext;
        }
        if (!pCur)
            return false;
    }
    if (!pUrb->Dev.pNext)
        pQueue->ppTail = &pQueue->pHead;
    return true;
}


/**
 * Checks if the queue is empty or not.
 *
 * @returns true if it is, false if it isn't.
 * @param   pQueue              The URB queue.
 */
DECLINLINE(bool) usbMsdQueueIsEmpty(PCUSBMSDURBQUEUE pQueue)
{
    return pQueue->pHead == NULL;
}


/**
 * Links an URB into the done queue.
 *
 * @param   pThis               The MSD instance.
 * @param   pUrb                The URB.
 */
static void usbMsdLinkDone(PUSBMSD pThis, PVUSBURB pUrb)
{
    usbMsdQueueAddTail(&pThis->DoneQueue, pUrb);

    if (pThis->fHaveDoneQueueWaiter)
    {
        int rc = RTSemEventSignal(pThis->hEvtDoneQueue);
        AssertRC(rc);
    }
}




/**
 * Allocates a new request and does basic init.
 *
 * @returns Pointer to the new request.  NULL if we're out of memory.
 * @param   pUsbIns             The instance allocating it.
 */
static PUSBMSDREQ usbMsdReqAlloc(PPDMUSBINS pUsbIns)
{
    PUSBMSDREQ pReq = (PUSBMSDREQ)PDMUsbHlpMMHeapAllocZ(pUsbIns, sizeof(*pReq));
    if (pReq)
    {
        pReq->enmState          = USBMSDREQSTATE_READY;
        pReq->iScsiReqStatus    = -1;
        pReq->pUsbIns           = pUsbIns;
    }
    else
        LogRel(("usbMsdReqAlloc: Out of memory\n"));
    return pReq;
}


/**
 * Frees a request.
 *
 * @param   pReq                The request.
 */
static void usbMsdReqFree(PUSBMSDREQ pReq)
{
    /*
     * Check the input.
     */
    AssertReturnVoid(    pReq->enmState > USBMSDREQSTATE_INVALID
                     &&  pReq->enmState != USBMSDREQSTATE_EXECUTING
                     &&  pReq->enmState < USBMSDREQSTATE_END);
    PPDMUSBINS pUsbIns = pReq->pUsbIns;
    AssertPtrReturnVoid(pUsbIns);
    AssertReturnVoid(PDM_VERSION_ARE_COMPATIBLE(pUsbIns->u32Version, PDM_USBINS_VERSION));

    /*
     * Invalidate it and free the associated resources.
     */
    pReq->enmState                      = USBMSDREQSTATE_INVALID;
    pReq->cbBuf                         = 0;
    pReq->offBuf                        = 0;
    pReq->ScsiReq.pbCDB                 = NULL;
    pReq->ScsiReq.paScatterGatherHead   = NULL;
    pReq->ScsiReq.pbSenseBuffer         = NULL;
    pReq->ScsiReq.pvUser                = NULL;
    pReq->ScsiReqSeg.cbSeg              = 0;
    pReq->ScsiReqSeg.pvSeg              = NULL;

    if (pReq->pbBuf)
    {
        PDMUsbHlpMMHeapFree(pUsbIns, pReq->pbBuf);
        pReq->pbBuf = NULL;
    }

    PDMUsbHlpMMHeapFree(pUsbIns, pReq);
}


/**
 * Prepares a request for execution or data buffering.
 *
 * @param   pReq                The request.
 * @param   pCbw                The SCSI command block wrapper.
 */
static void usbMsdReqPrepare(PUSBMSDREQ pReq, PCUSBCBW pCbw)
{
    /* Copy the CBW */
    size_t cbCopy = RT_OFFSETOF(USBCBW, CBWCB[pCbw->bCBWCBLength]);
    memcpy(&pReq->Cbw, pCbw, cbCopy);
    memset((uint8_t *)&pReq->Cbw + cbCopy, 0, sizeof(pReq->Cbw) - cbCopy);

    /* Setup the SCSI request. */
    pReq->ScsiReq.uLogicalUnit      = pReq->Cbw.bCBWLun;
    pReq->ScsiReq.uDataDirection    = (pReq->Cbw.bmCBWFlags & USBCBW_DIR_MASK) == USBCBW_DIR_OUT
                                    ? PDMSCSIREQUESTTXDIR_TO_DEVICE
                                    : PDMSCSIREQUESTTXDIR_FROM_DEVICE;
    pReq->ScsiReq.cbCDB             = pReq->Cbw.bCBWCBLength;

    pReq->ScsiReq.pbCDB             = &pReq->Cbw.CBWCB[0];
    pReq->offBuf                    = 0;
    pReq->ScsiReqSeg.pvSeg          = pReq->pbBuf;
    pReq->ScsiReqSeg.cbSeg          = pReq->Cbw.dCBWDataTransferLength;
    pReq->ScsiReq.cbScatterGather   = pReq->Cbw.dCBWDataTransferLength;
    pReq->ScsiReq.cScatterGatherEntries = 1;
    pReq->ScsiReq.paScatterGatherHead = &pReq->ScsiReqSeg;
    pReq->ScsiReq.cbSenseBuffer     = sizeof(pReq->ScsiReqSense);
    pReq->ScsiReq.pbSenseBuffer     = &pReq->ScsiReqSense[0];
    pReq->ScsiReq.pvUser            = NULL;
    RT_ZERO(pReq->ScsiReqSense);
    pReq->iScsiReqStatus            = -1;
}


/**
 * Makes sure that there is sufficient buffer space available.
 *
 * @returns Success indicator (true/false)
 * @param   pReq
 * @param   cbBuf       The required buffer space.
 */
static int usbMsdReqEnsureBuffer(PUSBMSDREQ pReq, uint32_t cbBuf)
{
    if (RT_LIKELY(pReq->cbBuf >= cbBuf))
        RT_BZERO(pReq->pbBuf, cbBuf);
    else
    {
        PDMUsbHlpMMHeapFree(pReq->pUsbIns, pReq->pbBuf);
        pReq->cbBuf = 0;

        cbBuf = RT_ALIGN_Z(cbBuf, 0x1000);
        pReq->pbBuf = (uint8_t *)PDMUsbHlpMMHeapAllocZ(pReq->pUsbIns, cbBuf);
        if (!pReq->pbBuf)
            return false;

        pReq->cbBuf = cbBuf;
    }
    return true;
}


/**
 * Completes the URB with a stalled state, halting the pipe.
 */
static int usbMsdCompleteStall(PUSBMSD pThis, PUSBMSDEP pEp, PVUSBURB pUrb, const char *pszWhy)
{
    Log(("usbMsdCompleteStall/#%u: pUrb=%p:%s: %s\n", pThis->pUsbIns->iInstance, pUrb, pUrb->pszDesc, pszWhy));

    pUrb->enmStatus = VUSBSTATUS_STALL;

    /** @todo figure out if the stall is global or pipe-specific or both. */
    if (pEp)
        pEp->fHalted = true;
    else
    {
        pThis->aEps[1].fHalted = true;
        pThis->aEps[2].fHalted = true;
    }

    usbMsdLinkDone(pThis, pUrb);
    return VINF_SUCCESS;
}


/**
 * Completes the URB with a OK state.
 */
static int usbMsdCompleteOk(PUSBMSD pThis, PVUSBURB pUrb, size_t cbData)
{
    Log(("usbMsdCompleteOk/#%u: pUrb=%p:%s cbData=%#zx\n", pThis->pUsbIns->iInstance, pUrb, pUrb->pszDesc, cbData));

    pUrb->enmStatus = VUSBSTATUS_OK;
    pUrb->cbData    = (uint32_t)cbData;

    usbMsdLinkDone(pThis, pUrb);
    return VINF_SUCCESS;
}


/**
 * Reset worker for usbMsdUsbReset, usbMsdUsbSetConfiguration and
 * usbMsdUrbHandleDefaultPipe.
 *
 * @returns VBox status code.
 * @param   pThis               The MSD instance.
 * @param   pUrb                Set when usbMsdUrbHandleDefaultPipe is the
 *                              caller.
 * @param   fSetConfig          Set when usbMsdUsbSetConfiguration is the
 *                              caller.
 */
static int usbMsdResetWorker(PUSBMSD pThis, PVUSBURB pUrb, bool fSetConfig)
{
    /*
     * Wait for the any command currently executing to complete before
     * resetting.  (We cannot cancel its execution.)  How we do this depends
     * on the reset method.
     */
    PUSBMSDREQ pReq = pThis->pReq;
    if (   pReq
        && pReq->enmState == USBMSDREQSTATE_EXECUTING)
    {
        /* Don't try to deal with the set config variant nor multiple build-only
           mass storage resets. */
        if (pThis->pResetUrb && (pUrb || fSetConfig))
        {
            Log(("usbMsdResetWorker: pResetUrb is already %p:%s - stalling\n", pThis->pResetUrb, pThis->pResetUrb->pszDesc));
            return usbMsdCompleteStall(pThis, NULL, pUrb, "pResetUrb");
        }

        /* Bulk-Only Mass Storage Reset: Complete the reset on request completion. */
        if (pUrb)
        {
            pThis->pResetUrb = pUrb;
            Log(("usbMsdResetWorker: Setting pResetUrb to %p:%s\n", pThis->pResetUrb, pThis->pResetUrb->pszDesc));
            return VINF_SUCCESS;
        }

        /* Device reset: Wait for up to 10 ms.  If it doesn't work, ditch
           whoe the request structure.  We'll allocate a new one when needed. */
        Log(("usbMsdResetWorker: Waiting for completion...\n"));
        Assert(!pThis->fSignalResetSem);
        pThis->fSignalResetSem = true;
        RTSemEventMultiReset(pThis->hEvtReset);
        RTCritSectLeave(&pThis->CritSect);

        int rc = RTSemEventMultiWait(pThis->hEvtReset, 10 /*ms*/);

        RTCritSectEnter(&pThis->CritSect);
        pThis->fSignalResetSem = false;
        if (    RT_FAILURE(rc)
            ||  pReq->enmState == USBMSDREQSTATE_EXECUTING)
        {
            Log(("usbMsdResetWorker: Didn't complete, ditching the current request (%p)!\n", pReq));
            Assert(pReq == pThis->pReq);
            pReq->enmState = USBMSDREQSTATE_DESTROY_ON_COMPLETION;
            pThis->pReq = NULL;
            pReq = NULL;
        }
    }

    /*
     * Reset the request and device state.
     */
    if (pReq)
    {
        pReq->enmState       = USBMSDREQSTATE_READY;
        pReq->iScsiReqStatus = -1;
    }

    for (unsigned i = 0; i < RT_ELEMENTS(pThis->aEps); i++)
        pThis->aEps[i].fHalted = false;

    if (!pUrb && !fSetConfig) /* (only device reset) */
        pThis->bConfigurationValue = 0; /* default */

    /*
     * Ditch all pending URBs.
     */
    PVUSBURB pCurUrb;
    while ((pCurUrb = usbMsdQueueRemoveHead(&pThis->ToHostQueue)) != NULL)
    {
        pCurUrb->enmStatus = VUSBSTATUS_CRC;
        usbMsdLinkDone(pThis, pCurUrb);
    }

    pCurUrb = pThis->pResetUrb;
    if (pCurUrb)
    {
        pThis->pResetUrb = NULL;
        pCurUrb->enmStatus  = VUSBSTATUS_CRC;
        usbMsdLinkDone(pThis, pCurUrb);
    }

    if (pUrb)
        return usbMsdCompleteOk(pThis, pUrb, 0);
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMISCSIPORT,pfnSCSIRequestCompleted}
 */
static DECLCALLBACK(int) usbMsdLun0ScsiRequestCompleted(PPDMISCSIPORT pInterface, PPDMSCSIREQUEST pSCSIRequest,
                                                        int rcCompletion, bool fRedo, int rcReq)
{
    PUSBMSD     pThis = RT_FROM_MEMBER(pInterface, USBMSD, Lun0.IScsiPort);
    PUSBMSDREQ  pReq  = RT_FROM_MEMBER(pSCSIRequest, USBMSDREQ, ScsiReq);

    Log(("usbMsdLun0ScsiRequestCompleted: pReq=%p dCBWTag=%#x iScsiReqStatus=%u \n", pReq, pReq->Cbw.dCBWTag, rcCompletion));
    RTCritSectEnter(&pThis->CritSect);

    if (pReq->enmState != USBMSDREQSTATE_DESTROY_ON_COMPLETION)
    {
        Assert(pReq->enmState == USBMSDREQSTATE_EXECUTING);
        Assert(pThis->pReq == pReq);
        pReq->iScsiReqStatus = rcCompletion;

        /*
         * Advance the state machine.  The state machine is not affected by
         * SCSI errors.
         */
        if ((pReq->Cbw.bmCBWFlags & USBCBW_DIR_MASK) == USBCBW_DIR_OUT)
        {
            pReq->enmState = USBMSDREQSTATE_STATUS;
            Log(("usbMsdLun0ScsiRequestCompleted: Entering STATUS\n"));
        }
        else
        {
            pReq->enmState = USBMSDREQSTATE_DATA_TO_HOST;
            Log(("usbMsdLun0ScsiRequestCompleted: Entering DATA_TO_HOST\n"));
        }

        /*
         * Deal with pending to-host URBs.
         */
        for (;;)
        {
            PVUSBURB pUrb = usbMsdQueueRemoveHead(&pThis->ToHostQueue);
            if (!pUrb)
                break;

            /* Process it the normal way. */
            usbMsdHandleBulkDevToHost(pThis, &pThis->aEps[1], pUrb);
        }
    }
    else
    {
        Log(("usbMsdLun0ScsiRequestCompleted: freeing %p\n", pReq));
        usbMsdReqFree(pReq);
    }

    if (pThis->fSignalResetSem)
        RTSemEventMultiSignal(pThis->hEvtReset);

    if (pThis->pResetUrb)
    {
        pThis->pResetUrb = NULL;
        usbMsdResetWorker(pThis, pThis->pResetUrb, false /*fSetConfig*/);
    }

    RTCritSectLeave(&pThis->CritSect);
    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMISCSIPORT,pfnQueryDeviceLocation}
 */
static DECLCALLBACK(int) usbMsdLun0QueryDeviceLocation(PPDMISCSIPORT pInterface, const char **ppcszController,
                                                       uint32_t *piInstance, uint32_t *piLUN)
{
    PUSBMSD    pThis = RT_FROM_MEMBER(pInterface, USBMSD, Lun0.IScsiPort);
    PPDMUSBINS pUsbIns = pThis->pUsbIns;

    AssertPtrReturn(ppcszController, VERR_INVALID_POINTER);
    AssertPtrReturn(piInstance, VERR_INVALID_POINTER);
    AssertPtrReturn(piLUN, VERR_INVALID_POINTER);

    *ppcszController = pUsbIns->pReg->szName;
    *piInstance = pUsbIns->iInstance;
    *piLUN = 0;

    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMIBASE,pfnQueryInterface}
 */
static DECLCALLBACK(void *) usbMsdLun0QueryInterface(PPDMIBASE pInterface, const char *pszIID)
{
    PUSBMSD pThis = RT_FROM_MEMBER(pInterface, USBMSD, Lun0.IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMIBASE, &pThis->Lun0.IBase);
    PDMIBASE_RETURN_INTERFACE(pszIID, PDMISCSIPORT, &pThis->Lun0.IScsiPort);
    return NULL;
}


/**
 * Checks if all asynchronous I/O is finished.
 *
 * Used by usbMsdVMReset, usbMsdVMSuspend and usbMsdVMPowerOff.
 *
 * @returns true if quiesced, false if busy.
 * @param   pUsbIns         The USB device instance.
 */
static bool usbMsdAllAsyncIOIsFinished(PPDMUSBINS pUsbIns)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);

    if (   VALID_PTR(pThis->pReq)
        && pThis->pReq->enmState == USBMSDREQSTATE_EXECUTING)
        return false;

    return true;
}

/**
 * @callback_method_impl{FNPDMDEVASYNCNOTIFY,
 * Callback employed by usbMsdVMSuspend and usbMsdVMPowerOff.}
 */
static DECLCALLBACK(bool) usbMsdIsAsyncSuspendOrPowerOffDone(PPDMUSBINS pUsbIns)
{
    if (!usbMsdAllAsyncIOIsFinished(pUsbIns))
        return false;

    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    ASMAtomicWriteBool(&pThis->fSignalIdle, false);
    return true;
}

/**
 * Common worker for usbMsdVMSuspend and usbMsdVMPowerOff.
 */
static void usbMsdSuspendOrPowerOff(PPDMUSBINS pUsbIns)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);

    ASMAtomicWriteBool(&pThis->fSignalIdle, true);
    if (!usbMsdAllAsyncIOIsFinished(pUsbIns))
        PDMUsbHlpSetAsyncNotification(pUsbIns, usbMsdIsAsyncSuspendOrPowerOffDone);
    else
        ASMAtomicWriteBool(&pThis->fSignalIdle, false);
}


/* -=-=-=-=- Saved State -=-=-=-=- */

/**
 * @copydoc FNUSBSSMSAVEPREP
 */
static DECLCALLBACK(int) usbMsdSavePrep(PPDMUSBINS pUsbIns, PSSMHANDLE pSSM)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);

    Assert(usbMsdAllAsyncIOIsFinished(pUsbIns));
    Assert(usbMsdQueueIsEmpty(&pThis->ToHostQueue));
    Assert(usbMsdQueueIsEmpty(&pThis->DoneQueue));
    return VINF_SUCCESS;
}

/**
 * @copydoc FNUSBSSMLOADPREP
 */
static DECLCALLBACK(int) usbMsdLoadPrep(PPDMUSBINS pUsbIns, PSSMHANDLE pSSM)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);

    Assert(usbMsdAllAsyncIOIsFinished(pUsbIns));
    Assert(usbMsdQueueIsEmpty(&pThis->ToHostQueue));
    Assert(usbMsdQueueIsEmpty(&pThis->DoneQueue));
    return VINF_SUCCESS;
}

/**
 * @copydoc FNUSBSSMLIVEEXEC
 */
static DECLCALLBACK(int) usbMsdLiveExec(PPDMUSBINS pUsbIns, PSSMHANDLE pSSM, uint32_t uPass)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);

    /* config. */
    SSMR3PutBool(pSSM, pThis->Lun0.pIBase != NULL);
    return VINF_SSM_DONT_CALL_AGAIN;
}

/**
 * @copydoc FNUSBSSMSAVEEXEC
 */
static DECLCALLBACK(int) usbMsdSaveExec(PPDMUSBINS pUsbIns, PSSMHANDLE pSSM)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    int rc;

    /* The config */
    rc = usbMsdLiveExec(pUsbIns, pSSM, SSM_PASS_FINAL);
    AssertRCReturn(rc, rc);

    SSMR3PutU8(pSSM, pThis->bConfigurationValue);
    SSMR3PutBool(pSSM, pThis->aEps[0].fHalted);
    SSMR3PutBool(pSSM, pThis->aEps[1].fHalted);
    SSMR3PutBool(pSSM, pThis->aEps[2].fHalted);
    SSMR3PutBool(pSSM, pThis->pReq != NULL);

    if (pThis->pReq)
    {
        PUSBMSDREQ pReq = pThis->pReq;

        SSMR3PutU32(pSSM, pReq->enmState);
        SSMR3PutU32(pSSM, pReq->cbBuf);
        if (pReq->cbBuf)
        {
            AssertPtr(pReq->pbBuf);
            SSMR3PutMem(pSSM, pReq->pbBuf, pReq->cbBuf);
        }

        SSMR3PutU32(pSSM, pReq->offBuf);
        SSMR3PutMem(pSSM, &pReq->Cbw, sizeof(pReq->Cbw));
        SSMR3PutU32(pSSM, pReq->ScsiReq.uLogicalUnit);
        SSMR3PutU32(pSSM, pReq->ScsiReq.uDataDirection);
        SSMR3PutU32(pSSM, pReq->ScsiReq.cbCDB);
        SSMR3PutU32(pSSM, pReq->ScsiReq.cbScatterGather);
        SSMR3PutMem(pSSM, &pReq->ScsiReqSense[0], sizeof(pReq->ScsiReqSense));
        SSMR3PutS32(pSSM, pReq->iScsiReqStatus);
    }

    return SSMR3PutU32(pSSM, UINT32_MAX); /* sanity/terminator */
}

/**
 * @copydoc FNUSBSSMLOADEXEC
 */
static DECLCALLBACK(int) usbMsdLoadExec(PPDMUSBINS pUsbIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    uint32_t u32;
    int rc;

    if (uVersion > USB_MSD_SAVED_STATE_VERSION)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;

    /* Verify config. */
    bool fInUse;
    rc = SSMR3GetBool(pSSM, &fInUse);
    AssertRCReturn(rc, rc);
    if (fInUse != (pThis->Lun0.pIBase != NULL))
        return SSMR3SetCfgError(pSSM, RT_SRC_POS,
                                N_("The %s VM is missing a USB mass storage device. Please make sure the source and target VMs have compatible storage configurations"),
                                fInUse ? "target" : "source");

    if (uPass == SSM_PASS_FINAL)
    {
        /* Restore data. */
        bool fReqAlloc = false;

        Assert(!pThis->pReq);

        SSMR3GetU8(pSSM, &pThis->bConfigurationValue);
        SSMR3GetBool(pSSM, &pThis->aEps[0].fHalted);
        SSMR3GetBool(pSSM, &pThis->aEps[1].fHalted);
        SSMR3GetBool(pSSM, &pThis->aEps[2].fHalted);
        SSMR3GetBool(pSSM, &fReqAlloc);

        if (fReqAlloc)
        {
            PUSBMSDREQ pReq = usbMsdReqAlloc(pUsbIns);

            if (pReq)
            {
                uint32_t cbBuf = 0;

                pThis->pReq = pReq;

                SSMR3GetU32(pSSM, (uint32_t *)&pReq->enmState);
                SSMR3GetU32(pSSM, &cbBuf);
                if (cbBuf)
                {
                    if (usbMsdReqEnsureBuffer(pReq, cbBuf))
                    {
                        AssertPtr(pReq->pbBuf);
                        Assert(cbBuf == pReq->cbBuf);
                        SSMR3GetMem(pSSM, pReq->pbBuf, pReq->cbBuf);
                    }
                    else
                        rc = VERR_NO_MEMORY;
                }

                if (RT_SUCCESS(rc))
                {
                    SSMR3GetU32(pSSM, &pReq->offBuf);
                    SSMR3GetMem(pSSM, &pReq->Cbw, sizeof(pReq->Cbw));
                    SSMR3GetU32(pSSM, &pReq->ScsiReq.uLogicalUnit);
                    SSMR3GetU32(pSSM, (uint32_t *)&pReq->ScsiReq.uDataDirection);
                    SSMR3GetU32(pSSM, &pReq->ScsiReq.cbCDB);
                    SSMR3GetU32(pSSM, &pReq->ScsiReq.cbScatterGather);
                    SSMR3GetMem(pSSM, &pReq->ScsiReqSense[0], sizeof(pReq->ScsiReqSense));
                    SSMR3GetS32(pSSM, &pReq->iScsiReqStatus);

                    /* Setup the rest of the SCSI request. */
                    pReq->ScsiReq.cbCDB             = pReq->Cbw.bCBWCBLength;
                    pReq->ScsiReq.pbCDB             = &pReq->Cbw.CBWCB[0];
                    pReq->ScsiReqSeg.pvSeg          = pReq->pbBuf;
                    pReq->ScsiReqSeg.cbSeg          = pReq->ScsiReq.cbScatterGather;
                    pReq->ScsiReq.cScatterGatherEntries = 1;
                    pReq->ScsiReq.paScatterGatherHead = &pReq->ScsiReqSeg;
                    pReq->ScsiReq.cbSenseBuffer     = sizeof(pReq->ScsiReqSense);
                    pReq->ScsiReq.pbSenseBuffer     = &pReq->ScsiReqSense[0];
                    pReq->ScsiReq.pvUser            = NULL;
                }
            }
            else
                rc = VERR_NO_MEMORY;
        }

        if (RT_SUCCESS(rc))
            rc = SSMR3GetU32(pSSM, &u32);

        if (RT_FAILURE(rc))
            return rc;
        AssertMsgReturn(u32 == UINT32_MAX, ("%#x\n", u32), VERR_SSM_DATA_UNIT_FORMAT_CHANGED);
    }

    return VINF_SUCCESS;
}


/**
 * @copydoc PDMUSBREG::pfnUrbReap
 */
static DECLCALLBACK(PVUSBURB) usbMsdUrbReap(PPDMUSBINS pUsbIns, RTMSINTERVAL cMillies)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    LogFlow(("usbMsdUrbReap/#%u: cMillies=%u\n", pUsbIns->iInstance, cMillies));

    RTCritSectEnter(&pThis->CritSect);

    PVUSBURB pUrb = usbMsdQueueRemoveHead(&pThis->DoneQueue);
    if (!pUrb && cMillies)
    {
        /* Wait */
        pThis->fHaveDoneQueueWaiter = true;
        RTCritSectLeave(&pThis->CritSect);

        RTSemEventWait(pThis->hEvtDoneQueue, cMillies);

        RTCritSectEnter(&pThis->CritSect);
        pThis->fHaveDoneQueueWaiter = false;

        pUrb = usbMsdQueueRemoveHead(&pThis->DoneQueue);
    }

    RTCritSectLeave(&pThis->CritSect);

    if (pUrb)
        Log(("usbMsdUrbReap/#%u: pUrb=%p:%s\n", pUsbIns->iInstance, pUrb, pUrb->pszDesc));
    return pUrb;
}


/**
 * @copydoc PDMUSBREG::pfnWakeup
 */
static DECLCALLBACK(int) usbMsdWakeup(PPDMUSBINS pUsbIns)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    LogFlow(("usbMsdUrbReap/#%u:\n", pUsbIns->iInstance));

    return RTSemEventSignal(pThis->hEvtDoneQueue);
}


/**
 * @copydoc PDMUSBREG::pfnUrbCancel
 */
static DECLCALLBACK(int) usbMsdUrbCancel(PPDMUSBINS pUsbIns, PVUSBURB pUrb)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    LogFlow(("usbMsdUrbCancel/#%u: pUrb=%p:%s\n", pUsbIns->iInstance, pUrb, pUrb->pszDesc));
    RTCritSectEnter(&pThis->CritSect);

    /*
     * Remove the URB from the to-host queue and move it onto the done queue.
     */
    if (usbMsdQueueRemove(&pThis->ToHostQueue, pUrb))
        usbMsdLinkDone(pThis, pUrb);

    RTCritSectLeave(&pThis->CritSect);
    return VINF_SUCCESS;
}


/**
 * Fails an illegal SCSI request.
 *
 * @returns VBox status code.
 * @param   pThis               The MSD instance data.
 * @param   pReq                The MSD request.
 * @param   bAsc                The ASC for the SCSI_SENSE_ILLEGAL_REQUEST.
 * @param   bAscq               The ASC qualifier.
 * @param   pszWhy              For logging why.
 */
static int usbMsdScsiIllegalRequest(PUSBMSD pThis, PUSBMSDREQ pReq, uint8_t bAsc, uint8_t bAscq, const char *pszWhy)
{
    Log(("usbMsdScsiIllegalRequest: bAsc=%#x bAscq=%#x %s\n", bAsc, bAscq, pszWhy));

    RT_ZERO(pReq->ScsiReqSense);
    pReq->ScsiReqSense[0]  = 0x80 | SCSI_SENSE_RESPONSE_CODE_CURR_FIXED;
    pReq->ScsiReqSense[2]  = SCSI_SENSE_ILLEGAL_REQUEST;
    pReq->ScsiReqSense[7]  = 10;
    pReq->ScsiReqSense[12] = SCSI_ASC_INVALID_MESSAGE;
    pReq->ScsiReqSense[13] = 0; /* Should be ASCQ but it has the same value for success. */

    usbMsdLun0ScsiRequestCompleted(&pThis->Lun0.IScsiPort, &pReq->ScsiReq, SCSI_STATUS_CHECK_CONDITION, false, VINF_SUCCESS);
    return VINF_SUCCESS;
}


/**
 * The SCSI driver doesn't handle SCSI_REQUEST_SENSE but instead
 * returns the sense info with the request.
 *
 */
static int usbMsdHandleScsiReqestSense(PUSBMSD pThis, PUSBMSDREQ pReq, PCUSBCBW pCbw)
{
    Log(("usbMsdHandleScsiReqestSense: Entering EXECUTING (dCBWTag=%#x).\n", pReq->Cbw.dCBWTag));
    Assert(pReq == pThis->pReq);
    pReq->enmState = USBMSDREQSTATE_EXECUTING;

    /* validation */
    if ((pCbw->bmCBWFlags & USBCBW_DIR_MASK) != USBCBW_DIR_IN)
        return usbMsdScsiIllegalRequest(pThis, pReq, SCSI_ASC_INVALID_MESSAGE, 0, "direction");
    if (pCbw->bCBWCBLength < 6)
        return usbMsdScsiIllegalRequest(pThis, pReq, SCSI_ASC_INVALID_MESSAGE, 0, "length");
    if ((pCbw->CBWCB[1] >> 5) != pCbw->bCBWLun)
        return usbMsdScsiIllegalRequest(pThis, pReq, SCSI_ASC_INV_FIELD_IN_CMD_PACKET, 0, "lun");
    if (pCbw->bCBWLun != 0)
        return usbMsdScsiIllegalRequest(pThis, pReq, SCSI_ASC_INVALID_MESSAGE, 0, "lun0");
    if (pCbw->CBWCB[4] < 6)
        return usbMsdScsiIllegalRequest(pThis, pReq, SCSI_ASC_INV_FIELD_IN_CMD_PACKET, 0, "out length");

    /* If the previous command succeeded successfully, whip up some sense data. */
    if (   pReq->iScsiReqStatus == SCSI_STATUS_OK
        && pReq->ScsiReqSense[0] == 0)
    {
        RT_ZERO(pReq->ScsiReqSense);
#if 0  /** @todo something upsets linux about this stuff. Needs investigation. */
        pReq->ScsiReqSense[0]  = 0x80 | SCSI_SENSE_RESPONSE_CODE_CURR_FIXED;
        pReq->ScsiReqSense[0]  = SCSI_SENSE_RESPONSE_CODE_CURR_FIXED;
        pReq->ScsiReqSense[2]  = SCSI_SENSE_NONE;
        pReq->ScsiReqSense[7]  = 10;
        pReq->ScsiReqSense[12] = SCSI_ASC_NONE;
        pReq->ScsiReqSense[13] = SCSI_ASC_NONE; /* Should be ASCQ but it has the same value for success. */
#endif
    }

    /* Copy the data into the result buffer. */
    size_t cbCopy = RT_MIN(pCbw->dCBWDataTransferLength, sizeof(pReq->ScsiReqSense));
    Log(("usbMsd: SCSI_REQUEST_SENSE - CBWCB[4]=%#x iOldState=%d, %u bytes, raw: %.*Rhxs\n",
         pCbw->CBWCB[4], pReq->iScsiReqStatus, pCbw->dCBWDataTransferLength, RT_MAX(1, cbCopy), pReq->ScsiReqSense));
    memcpy(pReq->pbBuf, &pReq->ScsiReqSense[0], cbCopy);

    usbMsdReqPrepare(pReq, pCbw);

    /* Do normal completion.  */
    usbMsdLun0ScsiRequestCompleted(&pThis->Lun0.IScsiPort, &pReq->ScsiReq, SCSI_STATUS_OK, false, VINF_SUCCESS);
    return VINF_SUCCESS;
}


/**
 * Wrapper around  PDMISCSICONNECTOR::pfnSCSIRequestSend that deals with
 * SCSI_REQUEST_SENSE.
 *
 * @returns VBox status code.
 * @param   pThis               The MSD instance data.
 * @param   pReq                The MSD request.
 * @param   pszCaller           Where we're called from.
 */
static int usbMsdSubmitScsiCommand(PUSBMSD pThis, PUSBMSDREQ pReq, const char *pszCaller)
{
    Log(("%s: Entering EXECUTING (dCBWTag=%#x).\n", pszCaller, pReq->Cbw.dCBWTag));
    Assert(pReq == pThis->pReq);
    pReq->enmState = USBMSDREQSTATE_EXECUTING;

    switch (pReq->ScsiReq.pbCDB[0])
    {
        case SCSI_REQUEST_SENSE:
        {
        }

        default:
            return pThis->Lun0.pIScsiConnector->pfnSCSIRequestSend(pThis->Lun0.pIScsiConnector, &pReq->ScsiReq);
    }
}

/**
 * Validates a SCSI request before passing it down to the SCSI driver.
 *
 * @returns true / false.  The request will be completed on failure.
 * @param   pThis               The MSD instance data.
 * @param   pCbw                The USB command block wrapper.
 * @param   pUrb                The URB.
 */
static bool usbMsdIsValidCommand(PUSBMSD pThis, PCUSBCBW pCbw, PVUSBURB pUrb)
{
    switch (pCbw->CBWCB[0])
    {
        case SCSI_REQUEST_SENSE:
            /** @todo validate this. */
            return true;

        default:
            return true;
    }
}


/**
 * Handle requests sent to the outbound (to device) bulk pipe.
 */
static int usbMsdHandleBulkHostToDev(PUSBMSD pThis, PUSBMSDEP pEp, PVUSBURB pUrb)
{
    /*
     * Stall the request if the pipe is halted.
     */
    if (RT_UNLIKELY(pEp->fHalted))
        return usbMsdCompleteStall(pThis, NULL, pUrb, "Halted pipe");

    /*
     * Deal with the URB according to the current state.
     */
    PUSBMSDREQ      pReq     = pThis->pReq;
    USBMSDREQSTATE  enmState = pReq ? pReq->enmState : USBMSDREQSTATE_READY;
    switch (enmState)
    {
        case USBMSDREQSTATE_STATUS:
            LogFlow(("usbMsdHandleBulkHostToDev: Skipping pending status.\n"));
            pReq->enmState = USBMSDREQSTATE_READY;
            /* fall thru */

        /*
         * We're ready to receive a command.  Start off by validating the
         * incoming request.
         */
        case USBMSDREQSTATE_READY:
        {
            PCUSBCBW pCbw = (PUSBCBW)&pUrb->abData[0];
            if (pUrb->cbData < RT_UOFFSETOF(USBCBW, CBWCB[1]))
            {
                Log(("usbMsd: Bad CBW: cbData=%#x < min=%#x\n", pUrb->cbData, RT_UOFFSETOF(USBCBW, CBWCB[1]) ));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "BAD CBW");
            }
            if (pCbw->dCBWSignature != USBCBW_SIGNATURE)
            {
                Log(("usbMsd: CBW: Invalid dCBWSignature value: %#x\n", pCbw->dCBWSignature));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Bad CBW");
            }
            Log(("usbMsd: CBW: dCBWTag=%#x dCBWDataTransferLength=%#x bmCBWFlags=%#x bCBWLun=%#x bCBWCBLength=%#x  cbData=%#x fShortNotOk=%RTbool\n",
                 pCbw->dCBWTag, pCbw->dCBWDataTransferLength, pCbw->bmCBWFlags, pCbw->bCBWLun, pCbw->bCBWCBLength, pUrb->cbData, pUrb->fShortNotOk));
            if (pCbw->bmCBWFlags & ~USBCBW_DIR_MASK)
            {
                Log(("usbMsd: CBW: Bad bmCBWFlags value: %#x\n", pCbw->bmCBWFlags));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Bad CBW");

            }
            if (pCbw->bCBWLun != 0)
            {
                Log(("usbMsd: CBW: Bad bCBWLun value: %#x\n", pCbw->bCBWLun));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Bad CBW");
            }
            if (pCbw->bCBWCBLength == 0)
            {
                Log(("usbMsd: CBW: Bad bCBWCBLength value: %#x\n", pCbw->bCBWCBLength));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Bad CBW");
            }
            if (pUrb->cbData < RT_UOFFSETOF(USBCBW, CBWCB[pCbw->bCBWCBLength]))
            {
                Log(("usbMsd: CBW: Mismatching cbData and bCBWCBLength values: %#x vs. %#x (%#x)\n",
                     pUrb->cbData, RT_UOFFSETOF(USBCBW, CBWCB[pCbw->bCBWCBLength]), pCbw->bCBWCBLength));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Bad CBW");
            }
            if (pCbw->dCBWDataTransferLength > _1M)
            {
                Log(("usbMsd: CBW: dCBWDataTransferLength is too large: %#x (%u)\n",
                     pCbw->dCBWDataTransferLength, pCbw->dCBWDataTransferLength));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Too big transfer");
            }

            if (!usbMsdIsValidCommand(pThis, pCbw, pUrb))
                return VINF_SUCCESS;

            /*
             * Make sure we've got a request and a sufficient buffer space.
             *
             * Note! This will make sure the buffer is ZERO as well, thus
             *       saving us the trouble of clearing the output buffer on
             *       failure later.
             */
            if (!pReq)
            {
                pReq = usbMsdReqAlloc(pThis->pUsbIns);
                if (!pReq)
                    return usbMsdCompleteStall(pThis, NULL, pUrb, "Request allocation failure");
                pThis->pReq = pReq;
            }
            if (!usbMsdReqEnsureBuffer(pReq, pCbw->dCBWDataTransferLength))
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Buffer allocation failure");

            /*
             * Special case REQUEST SENSE requests, usbMsdReqPrepare will
             * trash the sense data otherwise.
             */
            if (pCbw->CBWCB[0] == SCSI_REQUEST_SENSE)
                usbMsdHandleScsiReqestSense(pThis, pReq, pCbw);
            else
            {
                /*
                 * Prepare the request.  Kick it off right away if possible.
                 */
                usbMsdReqPrepare(pReq, pCbw);

                if (   pReq->Cbw.dCBWDataTransferLength == 0
                    || (pReq->Cbw.bmCBWFlags & USBCBW_DIR_MASK) == USBCBW_DIR_IN)
                {
                    int rc = usbMsdSubmitScsiCommand(pThis, pReq, "usbMsdHandleBulkHostToDev");
                    if (RT_FAILURE(rc))
                    {
                        Log(("usbMsd: Failed sending SCSI request to driver: %Rrc\n", rc));
                        return usbMsdCompleteStall(pThis, NULL, pUrb, "SCSI Submit #1");
                    }
                }
                else
                {
                    Log(("usbMsdHandleBulkHostToDev: Entering DATA_FROM_HOST.\n"));
                    pReq->enmState = USBMSDREQSTATE_DATA_FROM_HOST;
                }
            }

            return usbMsdCompleteOk(pThis, pUrb, pUrb->cbData);
        }

        /*
         * Stuff the data into the buffer.
         */
        case USBMSDREQSTATE_DATA_FROM_HOST:
        {
            uint32_t    cbData = pUrb->cbData;
            uint32_t    cbLeft = pReq->Cbw.dCBWDataTransferLength - pReq->offBuf;
            if (cbData > cbLeft)
            {
                Log(("usbMsd: Too much data: cbData=%#x offBuf=%#x dCBWDataTransferLength=%#x cbLeft=%#x\n",
                     cbData, pReq->offBuf, pReq->Cbw.dCBWDataTransferLength, cbLeft));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Too much data");
            }
            memcpy(&pReq->pbBuf[pReq->offBuf], &pUrb->abData[0], cbData);
            pReq->offBuf += cbData;

            if (pReq->offBuf == pReq->Cbw.dCBWDataTransferLength)
            {
                int rc = usbMsdSubmitScsiCommand(pThis, pReq, "usbMsdHandleBulkHostToDev");
                if (RT_FAILURE(rc))
                {
                    Log(("usbMsd: Failed sending SCSI request to driver: %Rrc\n", rc));
                    return usbMsdCompleteStall(pThis, NULL, pUrb, "SCSI Submit #2");
                }
            }
            return usbMsdCompleteOk(pThis, pUrb, cbData);
        }

        /*
         * Bad state, stall.
         */
        case USBMSDREQSTATE_DATA_TO_HOST:
            return usbMsdCompleteStall(pThis, NULL, pUrb, "Bad state H2D: DATA_TO_HOST");

        case USBMSDREQSTATE_EXECUTING:
            return usbMsdCompleteStall(pThis, NULL, pUrb, "Bad state H2D: EXECUTING");

        default:
            AssertMsgFailed(("enmState=%d\n", enmState));
            return usbMsdCompleteStall(pThis, NULL, pUrb, "Bad state (H2D)");
    }
}


/**
 * Handle requests sent to the inbound (to host) bulk pipe.
 */
static int usbMsdHandleBulkDevToHost(PUSBMSD pThis, PUSBMSDEP pEp, PVUSBURB pUrb)
{
    /*
     * Stall the request if the pipe is halted OR if there is no
     * pending request yet.
     */
    PUSBMSDREQ pReq = pThis->pReq;
    if (RT_UNLIKELY(pEp->fHalted || !pReq))
        return usbMsdCompleteStall(pThis, NULL, pUrb, pEp->fHalted ? "Halted pipe" : "No request");

    /*
     * Deal with the URB according to the state.
     */
    switch (pReq->enmState)
    {
        /*
         * We've data left to transfer to the host.
         */
        case USBMSDREQSTATE_DATA_TO_HOST:
        {
            uint32_t cbData = pUrb->cbData;
            uint32_t cbCopy = pReq->Cbw.dCBWDataTransferLength - pReq->offBuf;
            if (cbData <= cbCopy)
                cbCopy = cbData;
            else if (pUrb->fShortNotOk)
            {
                Log(("usbMsd: Requested more data that we've got; cbData=%#x offBuf=%#x dCBWDataTransferLength=%#x cbLeft=%#x\n",
                     cbData, pReq->offBuf, pReq->Cbw.dCBWDataTransferLength, cbCopy));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Data underrun");
            }
            memcpy(&pUrb->abData[0], &pReq->pbBuf[pReq->offBuf], cbCopy);
            pReq->offBuf += cbCopy;

            if (pReq->offBuf == pReq->Cbw.dCBWDataTransferLength)
            {
                Log(("usbMsdHandleBulkDevToHost: Entering STATUS\n"));
                pReq->enmState = USBMSDREQSTATE_STATUS;
            }
            return usbMsdCompleteOk(pThis, pUrb, cbCopy);
        }

        /*
         * Status transfer.
         */
        case USBMSDREQSTATE_STATUS:
        {
            if ((pUrb->cbData < sizeof(USBCSW)) || (pUrb->cbData > sizeof(USBCSW) && pUrb->fShortNotOk))
            {
                Log(("usbMsd: Unexpected status request size: %#x (expected %#x), fShortNotOK=%RTbool\n", pUrb->cbData, sizeof(USBCSW), pUrb->fShortNotOk));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Invalid CSW size");
            }

            /* Enter a CSW into the URB data buffer. */
            PUSBCSW pCsw = (PUSBCSW)&pUrb->abData[0];
            pCsw->dCSWSignature = USBCSW_SIGNATURE;
            pCsw->dCSWTag       = pReq->Cbw.dCBWTag;
            pCsw->bCSWStatus    = pReq->iScsiReqStatus == SCSI_STATUS_OK
                                ? USBCSW_STATUS_OK
                                : pReq->iScsiReqStatus >= 0
                                ? USBCSW_STATUS_FAILED
                                : USBCSW_STATUS_PHASE_ERROR;
            /** @todo the following is not always accurate; VSCSI needs
             *        to implement residual counts properly! */
            if ((pReq->Cbw.bmCBWFlags & USBCBW_DIR_MASK) == USBCBW_DIR_OUT)
                pCsw->dCSWDataResidue = pCsw->bCSWStatus == USBCSW_STATUS_OK
                                      ? pReq->Cbw.dCBWDataTransferLength - pReq->ScsiReq.cbScatterGather
                                      : pReq->Cbw.dCBWDataTransferLength;
            else
                pCsw->dCSWDataResidue = pCsw->bCSWStatus == USBCSW_STATUS_OK
                                      ? 0
                                      : pReq->ScsiReq.cbScatterGather;
            Log(("usbMsd: CSW: dCSWTag=%#x bCSWStatus=%d dCSWDataResidue=%#x\n",
                 pCsw->dCSWTag, pCsw->bCSWStatus, pCsw->dCSWDataResidue));

            Log(("usbMsdHandleBulkDevToHost: Entering READY\n"));
            pReq->enmState = USBMSDREQSTATE_READY;
            return usbMsdCompleteOk(pThis, pUrb, sizeof(*pCsw));
        }

        /*
         * Status request before we've received all (or even any) data.
         * Linux 2.4.31 does this sometimes.  The recommended behavior is to
         * to accept the current data amount and execute the request.  (The
         * alternative behavior is to stall.)
         */
        case USBMSDREQSTATE_DATA_FROM_HOST:
        {
            if (pUrb->cbData != sizeof(USBCSW))
            {
                Log(("usbMsdHandleBulkDevToHost: DATA_FROM_HOST; cbData=%#x -> stall\n", pUrb->cbData));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "Invalid CSW size");
            }

            /* Adjust the request and kick it off.  Special case the no-data
               case since the SCSI driver doesn't like that. */
            pReq->ScsiReq.cbScatterGather = pReq->offBuf;
            pReq->ScsiReqSeg.cbSeg        = pReq->offBuf;
            if (!pReq->offBuf)
            {
                Log(("usbMsdHandleBulkDevToHost: Entering EXECUTING (offBuf=0x0).\n"));
                pReq->enmState = USBMSDREQSTATE_EXECUTING;

                usbMsdQueueAddTail(&pThis->ToHostQueue, pUrb);
                LogFlow(("usbMsdHandleBulkDevToHost: Added %p:%s to the to-host queue\n", pUrb, pUrb->pszDesc));

                usbMsdLun0ScsiRequestCompleted(&pThis->Lun0.IScsiPort, &pReq->ScsiReq, SCSI_STATUS_OK, false, VINF_SUCCESS);
                return VINF_SUCCESS;
            }

            int rc = usbMsdSubmitScsiCommand(pThis, pReq, "usbMsdHandleBulkDevToHost");
            if (RT_FAILURE(rc))
            {
                Log(("usbMsd: Failed sending SCSI request to driver: %Rrc\n", rc));
                return usbMsdCompleteStall(pThis, NULL, pUrb, "SCSI Submit #3");
            }

            /* fall thru */
        }

        /*
         * The SCSI command is still pending, queue the URB awaiting its
         * completion.
         */
        case USBMSDREQSTATE_EXECUTING:
            usbMsdQueueAddTail(&pThis->ToHostQueue, pUrb);
            LogFlow(("usbMsdHandleBulkDevToHost: Added %p:%s to the to-host queue\n", pUrb, pUrb->pszDesc));
            return VINF_SUCCESS;

        /*
         * Bad states, stall.
         */
        case USBMSDREQSTATE_READY:
            Log(("usbMsdHandleBulkDevToHost: enmState=READ(%d) (cbData=%#x)\n", pReq->enmState, pUrb->cbData));
            return usbMsdCompleteStall(pThis, NULL, pUrb, "Bad state D2H: READY");

        default:
            Log(("usbMsdHandleBulkDevToHost: enmState=%d cbData=%#x\n", pReq->enmState, pUrb->cbData));
            return usbMsdCompleteStall(pThis, NULL, pUrb, "Really bad state (D2H)!");
    }
}


/**
 * Handles request send to the default control pipe.
 */
static int usbMsdHandleDefaultPipe(PUSBMSD pThis, PUSBMSDEP pEp, PVUSBURB pUrb)
{
    PVUSBSETUP pSetup = (PVUSBSETUP)&pUrb->abData[0];
    AssertReturn(pUrb->cbData >= sizeof(*pSetup), VERR_VUSB_FAILED_TO_QUEUE_URB);

    if ((pSetup->bmRequestType & VUSB_REQ_MASK) == VUSB_REQ_STANDARD)
    {
        switch (pSetup->bRequest)
        {
            case VUSB_REQ_GET_DESCRIPTOR:
            {
                if (pSetup->bmRequestType != (VUSB_TO_DEVICE | VUSB_REQ_STANDARD | VUSB_DIR_TO_HOST))
                {
                    Log(("usbMsd: Bad GET_DESCRIPTOR req: bmRequestType=%#x\n", pSetup->bmRequestType));
                    return usbMsdCompleteStall(pThis, pEp, pUrb, "Bad GET_DESCRIPTOR");
                }

                switch (pSetup->wValue >> 8)
                {
                    uint32_t    cbCopy;

                    case VUSB_DT_STRING:
                        Log(("usbMsd: GET_DESCRIPTOR DT_STRING wValue=%#x wIndex=%#x\n", pSetup->wValue, pSetup->wIndex));
                        break;
                    case VUSB_DT_DEVICE_QUALIFIER:
                        Log(("usbMsd: GET_DESCRIPTOR DT_DEVICE_QUALIFIER wValue=%#x wIndex=%#x\n", pSetup->wValue, pSetup->wIndex));
                        /* Returned data is written after the setup message. */
                        cbCopy = pUrb->cbData - sizeof(*pSetup);
                        cbCopy = RT_MIN(cbCopy, sizeof(g_UsbMsdDeviceQualifier));
                        memcpy(&pUrb->abData[sizeof(*pSetup)], &g_UsbMsdDeviceQualifier, cbCopy);
                        return usbMsdCompleteOk(pThis, pUrb, cbCopy + sizeof(*pSetup));
                    case VUSB_DT_BOS:
                        Log(("usbMsd: GET_DESCRIPTOR DT_BOS wValue=%#x wIndex=%#x\n", pSetup->wValue, pSetup->wIndex));
                        /* Returned data is written after the setup message. */
                        cbCopy = pUrb->cbData - sizeof(*pSetup);
                        cbCopy = RT_MIN(cbCopy, sizeof(g_UsbMsdBOS));
                        memcpy(&pUrb->abData[sizeof(*pSetup)], &g_UsbMsdBOS, cbCopy);
                        return usbMsdCompleteOk(pThis, pUrb, cbCopy + sizeof(*pSetup));
                    default:
                        Log(("usbMsd: GET_DESCRIPTOR, huh? wValue=%#x wIndex=%#x\n", pSetup->wValue, pSetup->wIndex));
                        break;
                }
                break;
            }

            case VUSB_REQ_CLEAR_FEATURE:
                break;
        }

        /** @todo implement this. */
        Log(("usbMsd: Implement standard request: bmRequestType=%#x bRequest=%#x wValue=%#x wIndex=%#x wLength=%#x\n",
             pSetup->bmRequestType, pSetup->bRequest, pSetup->wValue, pSetup->wIndex, pSetup->wLength));

        usbMsdCompleteStall(pThis, pEp, pUrb, "TODO: standard request stuff");
    }
    /* 3.1 Bulk-Only Mass Storage Reset */
    else if (    pSetup->bmRequestType == (VUSB_REQ_CLASS | VUSB_TO_INTERFACE)
             &&  pSetup->bRequest == 0xff
             &&  !pSetup->wValue
             &&  !pSetup->wLength
             &&  pSetup->wIndex == 0)
    {
        Log(("usbMsdHandleDefaultPipe: Bulk-Only Mass Storage Reset\n"));
        return usbMsdResetWorker(pThis, pUrb, false /*fSetConfig*/);
    }
    /* 3.2 Get Max LUN, may stall if we like (but we don't). */
    else if (   pSetup->bmRequestType == (VUSB_REQ_CLASS | VUSB_TO_INTERFACE | VUSB_DIR_TO_HOST)
             &&  pSetup->bRequest == 0xfe
             &&  !pSetup->wValue
             &&  pSetup->wLength == 1
             &&  pSetup->wIndex == 0)
    {
        *(uint8_t *)(pSetup + 1) = 0; /* max lun is 0 */
        usbMsdCompleteOk(pThis, pUrb, 1);
    }
    else
    {
        Log(("usbMsd: Unknown control msg: bmRequestType=%#x bRequest=%#x wValue=%#x wIndex=%#x wLength=%#x\n",
             pSetup->bmRequestType, pSetup->bRequest, pSetup->wValue, pSetup->wIndex, pSetup->wLength));
        return usbMsdCompleteStall(pThis, pEp, pUrb, "Unknown control msg");
    }

    return VINF_SUCCESS;
}


/**
 * @copydoc PDMUSBREG::pfnQueue
 */
static DECLCALLBACK(int) usbMsdQueue(PPDMUSBINS pUsbIns, PVUSBURB pUrb)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    LogFlow(("usbMsdQueue/#%u: pUrb=%p:%s EndPt=%#x\n", pUsbIns->iInstance, pUrb, pUrb->pszDesc, pUrb->EndPt));
    RTCritSectEnter(&pThis->CritSect);

    /*
     * Parse on a per end-point basis.
     */
    int rc;
    switch (pUrb->EndPt)
    {
        case 0:
            rc = usbMsdHandleDefaultPipe(pThis, &pThis->aEps[0], pUrb);
            break;

        case 0x81:
            AssertFailed();
        case 0x01:
            rc = usbMsdHandleBulkDevToHost(pThis, &pThis->aEps[1], pUrb);
            break;

        case 0x02:
            rc = usbMsdHandleBulkHostToDev(pThis, &pThis->aEps[2], pUrb);
            break;

        default:
            AssertMsgFailed(("EndPt=%d\n", pUrb->EndPt));
            rc = VERR_VUSB_FAILED_TO_QUEUE_URB;
            break;
    }

    RTCritSectLeave(&pThis->CritSect);
    return rc;
}


/**
 * @copydoc PDMUSBREG::pfnUsbClearHaltedEndpoint
 */
static DECLCALLBACK(int) usbMsdUsbClearHaltedEndpoint(PPDMUSBINS pUsbIns, unsigned uEndpoint)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    LogFlow(("usbMsdUsbClearHaltedEndpoint/#%u: uEndpoint=%#x\n", pUsbIns->iInstance, uEndpoint));

    if ((uEndpoint & ~0x80) < RT_ELEMENTS(pThis->aEps))
    {
        RTCritSectEnter(&pThis->CritSect);
        pThis->aEps[(uEndpoint & ~0x80)].fHalted = false;
        RTCritSectLeave(&pThis->CritSect);
    }

    return VINF_SUCCESS;
}


/**
 * @copydoc PDMUSBREG::pfnUsbSetInterface
 */
static DECLCALLBACK(int) usbMsdUsbSetInterface(PPDMUSBINS pUsbIns, uint8_t bInterfaceNumber, uint8_t bAlternateSetting)
{
    LogFlow(("usbMsdUsbSetInterface/#%u: bInterfaceNumber=%u bAlternateSetting=%u\n", pUsbIns->iInstance, bInterfaceNumber, bAlternateSetting));
    Assert(bAlternateSetting == 0);
    return VINF_SUCCESS;
}


/**
 * @copydoc PDMUSBREG::pfnUsbSetConfiguration
 */
static DECLCALLBACK(int) usbMsdUsbSetConfiguration(PPDMUSBINS pUsbIns, uint8_t bConfigurationValue,
                                                   const void *pvOldCfgDesc, const void *pvOldIfState, const void *pvNewCfgDesc)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    LogFlow(("usbMsdUsbSetConfiguration/#%u: bConfigurationValue=%u\n", pUsbIns->iInstance, bConfigurationValue));
    Assert(bConfigurationValue == 1);
    RTCritSectEnter(&pThis->CritSect);

    /*
     * If the same config is applied more than once, it's a kind of reset.
     */
    if (pThis->bConfigurationValue == bConfigurationValue)
        usbMsdResetWorker(pThis, NULL, true /*fSetConfig*/); /** @todo figure out the exact difference */
    pThis->bConfigurationValue = bConfigurationValue;

    RTCritSectLeave(&pThis->CritSect);
    return VINF_SUCCESS;
}


/**
 * @copydoc PDMUSBREG::pfnUsbGetDescriptorCache
 */
static DECLCALLBACK(PCPDMUSBDESCCACHE) usbMsdUsbGetDescriptorCache(PPDMUSBINS pUsbIns)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    LogFlow(("usbMsdUsbGetDescriptorCache/#%u:\n", pUsbIns->iInstance));
    if (pThis->pUsbIns->enmSpeed == VUSB_SPEED_SUPER)
        return pThis->fIsCdrom ? &g_UsbCdDescCacheSS : &g_UsbMsdDescCacheSS;
    else if (pThis->pUsbIns->enmSpeed == VUSB_SPEED_HIGH)
        return pThis->fIsCdrom ? &g_UsbCdDescCacheHS : &g_UsbMsdDescCacheHS;
    else
        return pThis->fIsCdrom ? &g_UsbCdDescCacheFS : &g_UsbMsdDescCacheFS;
}


/**
 * @copydoc PDMUSBREG::pfnUsbReset
 */
static DECLCALLBACK(int) usbMsdUsbReset(PPDMUSBINS pUsbIns, bool fResetOnLinux)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    LogFlow(("usbMsdUsbReset/#%u:\n", pUsbIns->iInstance));
    RTCritSectEnter(&pThis->CritSect);

    int rc = usbMsdResetWorker(pThis, NULL, false /*fSetConfig*/);

    RTCritSectLeave(&pThis->CritSect);
    return rc;
}


/**
 * @copydoc PDMUSBREG::pfnVMSuspend
 */
static DECLCALLBACK(void) usbMsdVMSuspend(PPDMUSBINS pUsbIns)
{
    LogFlow(("usbMsdVMSuspend/#%u:\n", pUsbIns->iInstance));
    usbMsdSuspendOrPowerOff(pUsbIns);
}


/**
 * @copydoc PDMUSBREG::pfnVMSuspend
 */
static DECLCALLBACK(void) usbMsdVMPowerOff(PPDMUSBINS pUsbIns)
{
    LogFlow(("usbMsdVMPowerOff/#%u:\n", pUsbIns->iInstance));
    usbMsdSuspendOrPowerOff(pUsbIns);
}


/**
 * @copydoc PDMUSBREG::pfnDriverAttach
 */
static DECLCALLBACK(int) usbMsdDriverAttach(PPDMUSBINS pUsbIns, unsigned iLUN, uint32_t fFlags)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    int rc;

    LogFlow(("usbMsdDetach/#%u:\n", pUsbIns->iInstance));

    AssertMsg(iLUN == 0, ("UsbMsd: No other LUN than 0 is supported\n"));
    AssertMsg(fFlags & PDM_TACH_FLAGS_NOT_HOT_PLUG,
              ("UsbMsd: Device does not support hotplugging\n"));

    /* the usual paranoia */
    AssertRelease(!pThis->Lun0.pIBase);
    AssertRelease(!pThis->Lun0.pIScsiConnector);

    /*
     * Try attach the block device and get the interfaces,
     * required as well as optional.
     */
    rc = PDMUsbHlpDriverAttach(pUsbIns, iLUN, &pThis->Lun0.IBase, &pThis->Lun0.pIBase, NULL);
    if (RT_SUCCESS(rc))
    {
        /* Get SCSI connector interface. */
        pThis->Lun0.pIScsiConnector = PDMIBASE_QUERY_INTERFACE(pThis->Lun0.pIBase, PDMISCSICONNECTOR);
        AssertMsgReturn(pThis->Lun0.pIScsiConnector, ("Missing SCSI interface below\n"), VERR_PDM_MISSING_INTERFACE);
    }
    else
        AssertMsgFailed(("Failed to attach LUN#%d. rc=%Rrc\n", iLUN, rc));

    if (RT_FAILURE(rc))
    {
        pThis->Lun0.pIBase = NULL;
        pThis->Lun0.pIScsiConnector = NULL;
    }

    /*
     * Find out what kind of device we are.
     */
    PDMSCSILUNTYPE enmLunType;
    pThis->fIsCdrom = false;
    rc = pThis->Lun0.pIScsiConnector->pfnQueryLUNType(pThis->Lun0.pIScsiConnector, 0 /*iLun*/, &enmLunType);
    if (RT_SUCCESS(rc))
    {
        /* Anything else will be reported as a hard disk. */
        if (enmLunType == PDMSCSILUNTYPE_MMC)
            pThis->fIsCdrom = true;
    }

    return rc;
}


/**
 * @copydoc PDMUSBREG::pfnDriverDetach
 */
static DECLCALLBACK(void) usbMsdDriverDetach(PPDMUSBINS pUsbIns, unsigned iLUN, uint32_t fFlags)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);

    LogFlow(("usbMsdDetach/#%u:\n", pUsbIns->iInstance));

    AssertMsg(iLUN == 0, ("UsbMsd: No other LUN than 0 is supported\n"));
    AssertMsg(fFlags & PDM_TACH_FLAGS_NOT_HOT_PLUG,
              ("UsbMsd: Device does not support hotplugging\n"));

    /*
     * Zero some important members.
     */
    pThis->Lun0.pIBase = NULL;
    pThis->Lun0.pIScsiConnector = NULL;
}


/**
 * @callback_method_impl{FNPDMDEVASYNCNOTIFY,
 * Callback employed by usbMsdVMReset.}
 */
static DECLCALLBACK(bool) usbMsdIsAsyncResetDone(PPDMUSBINS pUsbIns)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);

    if (!usbMsdAllAsyncIOIsFinished(pUsbIns))
        return false;
    ASMAtomicWriteBool(&pThis->fSignalIdle, false);

    int rc = usbMsdResetWorker(pThis, NULL, false /*fSetConfig*/);
    AssertRC(rc);
    return true;
}

/**
 * @interface_method_impl{PDMDEVREG,pfnReset}
 */
static DECLCALLBACK(void) usbMsdVMReset(PPDMUSBINS pUsbIns)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);

    ASMAtomicWriteBool(&pThis->fSignalIdle, true);
    if (!usbMsdAllAsyncIOIsFinished(pUsbIns))
        PDMUsbHlpSetAsyncNotification(pUsbIns, usbMsdIsAsyncResetDone);
    else
    {
        ASMAtomicWriteBool(&pThis->fSignalIdle, false);
        int rc = usbMsdResetWorker(pThis, NULL, false /*fSetConfig*/);
        AssertRC(rc);
    }
}


/**
 * @copydoc PDMUSBREG::pfnDestruct
 */
static DECLCALLBACK(void) usbMsdDestruct(PPDMUSBINS pUsbIns)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    LogFlow(("usbMsdDestruct/#%u:\n", pUsbIns->iInstance));

    if (RTCritSectIsInitialized(&pThis->CritSect))
    {
        RTCritSectEnter(&pThis->CritSect);
        RTCritSectLeave(&pThis->CritSect);
        RTCritSectDelete(&pThis->CritSect);
    }

    if (pThis->pReq)
    {
        usbMsdReqFree(pThis->pReq);
        pThis->pReq = NULL;
    }

    if (pThis->hEvtDoneQueue != NIL_RTSEMEVENT)
    {
        RTSemEventDestroy(pThis->hEvtDoneQueue);
        pThis->hEvtDoneQueue = NIL_RTSEMEVENT;
    }

    if (pThis->hEvtReset != NIL_RTSEMEVENTMULTI)
    {
        RTSemEventMultiDestroy(pThis->hEvtReset);
        pThis->hEvtReset = NIL_RTSEMEVENTMULTI;
    }
}


/**
 * @copydoc PDMUSBREG::pfnConstruct
 */
static DECLCALLBACK(int) usbMsdConstruct(PPDMUSBINS pUsbIns, int iInstance, PCFGMNODE pCfg, PCFGMNODE pCfgGlobal)
{
    PUSBMSD pThis = PDMINS_2_DATA(pUsbIns, PUSBMSD);
    Log(("usbMsdConstruct/#%u:\n", iInstance));

    /*
     * Perform the basic structure initialization first so the destructor
     * will not misbehave.
     */
    pThis->pUsbIns                                  = pUsbIns;
    pThis->hEvtDoneQueue                            = NIL_RTSEMEVENT;
    pThis->hEvtReset                                = NIL_RTSEMEVENTMULTI;
    pThis->Lun0.IBase.pfnQueryInterface             = usbMsdLun0QueryInterface;
    pThis->Lun0.IScsiPort.pfnSCSIRequestCompleted   = usbMsdLun0ScsiRequestCompleted;
    pThis->Lun0.IScsiPort.pfnQueryDeviceLocation    = usbMsdLun0QueryDeviceLocation;
    usbMsdQueueInit(&pThis->ToHostQueue);
    usbMsdQueueInit(&pThis->DoneQueue);

    int rc = RTCritSectInit(&pThis->CritSect);
    AssertRCReturn(rc, rc);

    rc = RTSemEventCreate(&pThis->hEvtDoneQueue);
    AssertRCReturn(rc, rc);

    rc = RTSemEventMultiCreate(&pThis->hEvtReset);
    AssertRCReturn(rc, rc);

    /*
     * Validate and read the configuration.
     */
    rc = CFGMR3ValidateConfig(pCfg, "/", "", "", "UsbMsd", iInstance);
    if (RT_FAILURE(rc))
        return rc;

    /*
     * Attach the SCSI driver.
     */
    rc = PDMUsbHlpDriverAttach(pUsbIns, 0 /*iLun*/, &pThis->Lun0.IBase, &pThis->Lun0.pIBase, "SCSI Port");
    if (RT_FAILURE(rc))
        return PDMUsbHlpVMSetError(pUsbIns, rc, RT_SRC_POS, N_("MSD failed to attach SCSI driver"));
    pThis->Lun0.pIScsiConnector = PDMIBASE_QUERY_INTERFACE(pThis->Lun0.pIBase, PDMISCSICONNECTOR);
    if (!pThis->Lun0.pIScsiConnector)
        return PDMUsbHlpVMSetError(pUsbIns, VERR_PDM_MISSING_INTERFACE_BELOW, RT_SRC_POS,
                                   N_("MSD failed to query the PDMISCSICONNECTOR from the driver below it"));

    /*
     * Find out what kind of device we are.
     */
    PDMSCSILUNTYPE enmLunType;
    pThis->fIsCdrom = false;
    rc = pThis->Lun0.pIScsiConnector->pfnQueryLUNType(pThis->Lun0.pIScsiConnector, 0 /*iLun*/, &enmLunType);
    if (RT_SUCCESS(rc))
    {
        /* Anything else will be reported as a hard disk. */
        if (enmLunType == PDMSCSILUNTYPE_MMC)
            pThis->fIsCdrom = true;
    }

    /*
     * Register the saved state data unit.
     */
    rc = PDMUsbHlpSSMRegister(pUsbIns, USB_MSD_SAVED_STATE_VERSION, sizeof(*pThis),
                              NULL,           usbMsdLiveExec, NULL,
                              usbMsdSavePrep, usbMsdSaveExec, NULL,
                              usbMsdLoadPrep, usbMsdLoadExec, NULL);
    if (RT_FAILURE(rc))
        return PDMUsbHlpVMSetError(pUsbIns, rc, RT_SRC_POS,
                                   N_("MSD failed to register SSM save state handlers"));

    return VINF_SUCCESS;
}


/**
 * The USB Mass Storage Device (MSD) registration record.
 */
const PDMUSBREG g_UsbMsd =
{
    /* u32Version */
    PDM_USBREG_VERSION,
    /* szName */
    "Msd",
    /* pszDescription */
    "USB Mass Storage Device, one LUN.",
    /* fFlags */
      PDM_USBREG_HIGHSPEED_CAPABLE | PDM_USBREG_SUPERSPEED_CAPABLE
    | PDM_USBREG_SAVED_STATE_SUPPORTED,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(USBMSD),
    /* pfnConstruct */
    usbMsdConstruct,
    /* pfnDestruct */
    usbMsdDestruct,
    /* pfnVMInitComplete */
    NULL,
    /* pfnVMPowerOn */
    NULL,
    /* pfnVMReset */
    usbMsdVMReset,
    /* pfnVMSuspend */
    usbMsdVMSuspend,
    /* pfnVMResume */
    NULL,
    /* pfnVMPowerOff */
    usbMsdVMPowerOff,
    /* pfnHotPlugged */
    NULL,
    /* pfnHotUnplugged */
    NULL,
    /* pfnDriverAttach */
    usbMsdDriverAttach,
    /* pfnDriverDetach */
    usbMsdDriverDetach,
    /* pfnQueryInterface */
    NULL,
    /* pfnUsbReset */
    usbMsdUsbReset,
    /* pfnUsbGetCachedDescriptors */
    usbMsdUsbGetDescriptorCache,
    /* pfnUsbSetConfiguration */
    usbMsdUsbSetConfiguration,
    /* pfnUsbSetInterface */
    usbMsdUsbSetInterface,
    /* pfnUsbClearHaltedEndpoint */
    usbMsdUsbClearHaltedEndpoint,
    /* pfnUrbNew */
    NULL/*usbMsdUrbNew*/,
    /* pfnQueue */
    usbMsdQueue,
    /* pfnUrbCancel */
    usbMsdUrbCancel,
    /* pfnUrbReap */
    usbMsdUrbReap,
    /* pfnWakeup */
    usbMsdWakeup,
    /* u32TheEnd */
    PDM_USBREG_VERSION
};

