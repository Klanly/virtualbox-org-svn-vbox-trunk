/* $Id$ */
/** @file
 * VBox Qt GUI - Extra-data related definitions.
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

#ifndef ___UIExtraDataDefs_h___
#define ___UIExtraDataDefs_h___

/* Qt includes: */
#include <QMetaType>

/* Other VBox includes: */
#include <iprt/cdefs.h>

/** Extra-data namespace. */
namespace UIExtraDataDefs
{
    /** @name General
      * @{ */
        /** VBox language ID. */
        extern const char* GUI_LanguageId;
    /** @} */

    /** @name Messaging
      * @{ */
        /** Holds the list of supressed messages for the Message/Popup center frameworks. */
        extern const char* GUI_SuppressMessages;
        /** Holds the list of messages for the Message/Popup center frameworks with inverted check-box state. */
        extern const char* GUI_InvertMessageOption;
#if !defined(VBOX_BLEEDING_EDGE) && !defined(DEBUG)
        /** Holds version for which user wants to prevent BETA build warning. */
        extern const char* GUI_PreventBetaWarning;
#endif /* !defined(VBOX_BLEEDING_EDGE) && !defined(DEBUG) */
    /** @} */

#ifdef VBOX_GUI_WITH_NETWORK_MANAGER
    /** @name Application Update
      * @{ */
        /** Holds whether Application Update functionality enabled. */
        extern const char* GUI_PreventApplicationUpdate;
        /** Holds Application Update data. */
        extern const char* GUI_UpdateDate;
        /** Holds Application Update check counter. */
        extern const char* GUI_UpdateCheckCount;
    /** @} */
#endif /* VBOX_GUI_WITH_NETWORK_MANAGER */

    /** @name Settings
      * @{ */
        /** Holds restricted Global Settings pages. */
        extern const char* GUI_RestrictedGlobalSettingsPages;
        /** Holds restricted Machine Settings pages. */
        extern const char* GUI_RestrictedMachineSettingsPages;
    /** @} */

    /** @name Settings: Display
      * @{ */
        /** Holds whether hovered machine-window should be activated. */
        extern const char* GUI_ActivateHoveredMachineWindow;
    /** @} */

    /** @name Settings: Keyboard
      * @{ */
        /** Holds Selector UI shortcut overrides. */
        extern const char* GUI_Input_SelectorShortcuts;
        /** Holds Runtime UI shortcut overrides. */
        extern const char* GUI_Input_MachineShortcuts;
    /** @} */

    /** @name Settings: Storage
      * @{ */
        /** Holds recent folder for hard-drives. */
        extern const char* GUI_RecentFolderHD;
        /** Holds recent folder for optical-disks. */
        extern const char* GUI_RecentFolderCD;
        /** Holds recent folder for floppy-disks. */
        extern const char* GUI_RecentFolderFD;
        /** Holds the list of recently used hard-drives. */
        extern const char* GUI_RecentListHD;
        /** Holds the list of recently used optical-disks. */
        extern const char* GUI_RecentListCD;
        /** Holds the list of recently used floppy-disks. */
        extern const char* GUI_RecentListFD;
    /** @} */

    /** @name VirtualBox Manager
      * @{ */
        /** Holds selector-window geometry. */
        extern const char* GUI_LastSelectorWindowPosition;
        /** Holds selector-window splitter hints. */
        extern const char* GUI_SplitterSizes;
        /** Holds whether selector-window tool-bar visible. */
        extern const char* GUI_Toolbar;
        /** Holds whether selector-window status-bar visible. */
        extern const char* GUI_Statusbar;
        /** Prefix used by composite extra-data keys,
          * which holds selector-window chooser-pane' groups definitions. */
        extern const char* GUI_GroupDefinitions;
        /** Holds last item chosen in selector-window chooser-pane. */
        extern const char* GUI_LastItemSelected;
        /** Holds selector-window details-pane' elements. */
        extern const char* GUI_DetailsPageBoxes;
        /** Holds selector-window details-pane' preview update interval. */
        extern const char* GUI_PreviewUpdate;
    /** @} */

    /** @name Wizards
      * @{ */
        /** Holds wizard types for which descriptions should be hidden. */
        extern const char* GUI_HideDescriptionForWizards;
    /** @} */

    /** @name Virtual Machine
      * @{ */
        /** Holds whether machine shouldn't be shown in selector-window chooser-pane. */
        extern const char* GUI_HideFromManager;
        /** Holds whether machine shouldn't be shown in selector-window details-pane. */
        extern const char* GUI_HideDetails;
        /** Holds whether machine reconfiguration disabled. */
        extern const char* GUI_PreventReconfiguration;
        /** Holds whether machine snapshot operations disabled. */
        extern const char* GUI_PreventSnapshotOperations;
        /** Holds whether this machine is first time started. */
        extern const char* GUI_FirstRun;
#ifndef Q_WS_MAC
        /** Except Mac OS X: Holds redefined machine-window icon names. */
        extern const char* GUI_MachineWindowIcons;
        /** Except Mac OS X: Holds redefined machine-window name postfix. */
        extern const char* GUI_MachineWindowNamePostfix;
#endif /* !Q_WS_MAC */
        /** Prefix used by composite extra-data keys,
          * which holds normal machine-window geometry per screen-index. */
        extern const char* GUI_LastNormalWindowPosition;
        /** Prefix used by composite extra-data keys,
          * which holds scaled machine-window geometry per screen-index. */
        extern const char* GUI_LastScaleWindowPosition;
        /** Holds machine-window geometry maximized state flag. */
        extern const char* GUI_Geometry_State_Max;
#ifndef Q_WS_MAC
        /** Holds Runtime UI menu-bar availability status. */
        extern const char* GUI_MenuBar_Enabled;
#endif /* !Q_WS_MAC */
        /** Holds restricted Runtime UI menu types. */
        extern const char* GUI_RestrictedRuntimeMenus;
        /** Holds restricted Runtime UI action types for 'Application' menu. */
        extern const char* GUI_RestrictedRuntimeApplicationMenuActions;
        /** Holds restricted Runtime UI action types for Machine menu. */
        extern const char* GUI_RestrictedRuntimeMachineMenuActions;
        /** Holds restricted Runtime UI action types for View menu. */
        extern const char* GUI_RestrictedRuntimeViewMenuActions;
        /** Holds restricted Runtime UI action types for Input menu. */
        extern const char* GUI_RestrictedRuntimeInputMenuActions;
        /** Holds restricted Runtime UI action types for Devices menu. */
        extern const char* GUI_RestrictedRuntimeDevicesMenuActions;
#ifdef VBOX_WITH_DEBUGGER_GUI
        /** Holds restricted Runtime UI action types for Debugger menu. */
        extern const char* GUI_RestrictedRuntimeDebuggerMenuActions;
#endif /* VBOX_WITH_DEBUGGER_GUI */
#ifdef Q_WS_MAC
        /** Mac OS X: Holds restricted Runtime UI action types for 'Window' menu. */
        extern const char* GUI_RestrictedRuntimeWindowMenuActions;
#endif /* Q_WS_MAC */
        /** Holds restricted Runtime UI action types for Help menu. */
        extern const char* GUI_RestrictedRuntimeHelpMenuActions;
        /** Holds restricted Runtime UI visual-states. */
        extern const char* GUI_RestrictedVisualStates;
        /** Holds whether full screen visual-state is requested. */
        extern const char* GUI_Fullscreen;
        /** Holds whether seamless visual-state is requested. */
        extern const char* GUI_Seamless;
        /** Holds whether scaled visual-state is requested. */
        extern const char* GUI_Scale;
#ifdef Q_WS_X11
        /** Holds whether legacy full-screen mode is requested. */
        extern const char* GUI_Fullscreen_LegacyMode;
#endif /* Q_WS_X11 */
        /** Holds whether guest-screen auto-resize according machine-window size is enabled. */
        extern const char* GUI_AutoresizeGuest;
        /** Prefix used by composite extra-data keys,
          * which holds last guest-screen visibility status per screen-index. */
        extern const char* GUI_LastVisibilityStatusForGuestScreen;
        /** Prefix used by composite extra-data keys,
          * which holds last guest-screen size-hint per screen-index. */
        extern const char* GUI_LastGuestSizeHint;
        /** Prefix used by composite extra-data keys,
          * which holds host-screen index per guest-screen index. */
        extern const char* GUI_VirtualScreenToHostScreen;
        /** Holds whether automatic mounting/unmounting of guest-screens enabled. */
        extern const char* GUI_AutomountGuestScreens;
#ifdef VBOX_WITH_VIDEOHWACCEL
        /** Holds whether 2D acceleration should use linear sretch. */
        extern const char* GUI_Accelerate2D_StretchLinear;
        /** Holds whether 2D acceleration should use YV12 pixel format. */
        extern const char* GUI_Accelerate2D_PixformatYV12;
        /** Holds whether 2D acceleration should use UYVY pixel format. */
        extern const char* GUI_Accelerate2D_PixformatUYVY;
        /** Holds whether 2D acceleration should use YUY2 pixel format. */
        extern const char* GUI_Accelerate2D_PixformatYUY2;
        /** Holds whether 2D acceleration should use AYUV pixel format. */
        extern const char* GUI_Accelerate2D_PixformatAYUV;
#endif /* VBOX_WITH_VIDEOHWACCEL */
        /** Holds whether Runtime UI should use unscaled HiDPI output. */
        extern const char* GUI_HiDPI_UnscaledOutput;
        /** Holds Runtime UI HiDPI optimization type. */
        extern const char* GUI_HiDPI_Optimization;
#ifndef Q_WS_MAC
        /** Holds whether mini-toolbar is enabled for full and seamless screens. */
        extern const char* GUI_ShowMiniToolBar;
        /** Holds whether mini-toolbar should auto-hide itself. */
        extern const char* GUI_MiniToolBarAutoHide;
        /** Holds mini-toolbar alignment. */
        extern const char* GUI_MiniToolBarAlignment;
#endif /* !Q_WS_MAC */
        /** Holds Runtime UI status-bar availability status. */
        extern const char* GUI_StatusBar_Enabled;
        /** Holds restricted Runtime UI status-bar indicators. */
        extern const char* GUI_RestrictedStatusBarIndicators;
        /** Holds Runtime UI status-bar indicator order. */
        extern const char* GUI_StatusBar_IndicatorOrder;
#ifdef Q_WS_MAC
        /** Mac OS X: Holds whether Dock icon should be updated at runtime. */
        extern const char* GUI_RealtimeDockIconUpdateEnabled;
        /** Mac OS X: Holds guest-screen which Dock icon should reflect at runtime. */
        extern const char* GUI_RealtimeDockIconUpdateMonitor;
#endif /* Q_WS_MAC */
        /** Holds whether machine should pass CAD to guest. */
        extern const char* GUI_PassCAD;
        /** Holds the mouse capture policy. */
        extern const char* GUI_MouseCapturePolicy;
        /** Holds redefined guru-meditation handler type. */
        extern const char* GUI_GuruMeditationHandler;
        /** Holds whether machine should perform HID LEDs synchronization. */
        extern const char* GUI_HidLedsSync;
        /** Holds the scale-factor. */
        extern const char* GUI_ScaleFactor;
        /** Holds the scaling optimization type. */
        extern const char* GUI_Scaling_Optimization;
    /** @} */

    /** @name Virtual Machine: Information dialog
      * @{ */
        /** Holds information-window geometry. */
        extern const char* GUI_InformationWindowGeometry;
    /** @} */

    /** @name Virtual Machine: Close dialog
      * @{ */
        /** Holds default machine close action. */
        extern const char* GUI_DefaultCloseAction;
        /** Holds restricted machine close actions. */
        extern const char* GUI_RestrictedCloseActions;
        /** Holds last machine close action. */
        extern const char* GUI_LastCloseAction;
        /** Holds machine close hook script name as simple string. */
        extern const char* GUI_CloseActionHook;
    /** @} */

#ifdef VBOX_WITH_DEBUGGER_GUI
    /** @name Virtual Machine: Debug UI
      * @{ */
        /** Holds whether debugger UI enabled. */
        extern const char* GUI_Dbg_Enabled;
        /** Holds whether debugger UI should be auto-shown. */
        extern const char* GUI_Dbg_AutoShow;
    /** @} */
#endif /* VBOX_WITH_DEBUGGER_GUI */

#ifdef DEBUG
    /** @name VirtualBox: Extra-data Manager window
      * @{ */
        /** Holds extra-data manager geometry. */
        extern const char* GUI_ExtraDataManager_Geometry;
        /** Holds extra-data manager splitter hints. */
        extern const char* GUI_ExtraDataManager_SplitterHints;
    /** @} */
#endif /* DEBUG */

    /** @name Virtual Machine: Log dialog
      * @{ */
        /** Holds log-window geometry. */
        extern const char* GUI_LogWindowGeometry;
    /** @} */
}

/** Extra-data meta definitions. */
class UIExtraDataMetaDefs : public QObject
{
    Q_OBJECT;
    Q_ENUMS(MenuType);
    Q_ENUMS(MenuApplicationActionType);
    Q_ENUMS(MenuHelpActionType);
    Q_ENUMS(RuntimeMenuMachineActionType);
    Q_ENUMS(RuntimeMenuViewActionType);
    Q_ENUMS(RuntimeMenuInputActionType);
    Q_ENUMS(RuntimeMenuDevicesActionType);
#ifdef VBOX_WITH_DEBUGGER_GUI
    Q_ENUMS(RuntimeMenuDebuggerActionType);
#endif /* VBOX_WITH_DEBUGGER_GUI */
#ifdef RT_OS_DARWIN
    Q_ENUMS(MenuWindowActionType);
#endif /* RT_OS_DARWIN */

public:

    /** Common UI: Menu types. */
    enum MenuType
    {
        MenuType_Invalid     = 0,
        MenuType_Application = RT_BIT(0),
        MenuType_Machine     = RT_BIT(1),
        MenuType_View        = RT_BIT(2),
        MenuType_Input       = RT_BIT(3),
        MenuType_Devices     = RT_BIT(4),
#ifdef VBOX_WITH_DEBUGGER_GUI
        MenuType_Debug       = RT_BIT(5),
#endif /* VBOX_WITH_DEBUGGER_GUI */
#ifdef RT_OS_DARWIN
        MenuType_Window      = RT_BIT(6),
#endif /* RT_OS_DARWIN */
        MenuType_Help        = RT_BIT(7),
        MenuType_All         = 0xFF
    };

    /** Menu "Application": Action types. */
    enum MenuApplicationActionType
    {
        MenuApplicationActionType_Invalid              = 0,
#ifdef RT_OS_DARWIN
        MenuApplicationActionType_About                = RT_BIT(0),
#endif /* RT_OS_DARWIN */
        MenuApplicationActionType_Preferences          = RT_BIT(1),
#ifdef VBOX_GUI_WITH_NETWORK_MANAGER
        MenuApplicationActionType_NetworkAccessManager = RT_BIT(2),
        MenuApplicationActionType_CheckForUpdates      = RT_BIT(3),
#endif /* VBOX_GUI_WITH_NETWORK_MANAGER */
        MenuApplicationActionType_ResetWarnings        = RT_BIT(4),
        MenuApplicationActionType_Close                = RT_BIT(5),
        MenuApplicationActionType_All                  = 0xFFFF
    };

    /** Menu "Help": Action types. */
    enum MenuHelpActionType
    {
        MenuHelpActionType_Invalid              = 0,
        MenuHelpActionType_Contents             = RT_BIT(0),
        MenuHelpActionType_WebSite              = RT_BIT(1),
#ifndef RT_OS_DARWIN
        MenuHelpActionType_About                = RT_BIT(2),
#endif /* !RT_OS_DARWIN */
        MenuHelpActionType_All                  = 0xFFFF
    };

    /** Runtime UI: Menu "Machine": Action types. */
    enum RuntimeMenuMachineActionType
    {
        RuntimeMenuMachineActionType_Invalid           = 0,
        RuntimeMenuMachineActionType_SettingsDialog    = RT_BIT(0),
        RuntimeMenuMachineActionType_TakeSnapshot      = RT_BIT(1),
        RuntimeMenuMachineActionType_InformationDialog = RT_BIT(2),
        RuntimeMenuMachineActionType_Pause             = RT_BIT(3),
        RuntimeMenuMachineActionType_Reset             = RT_BIT(4),
        RuntimeMenuMachineActionType_SaveState         = RT_BIT(5),
        RuntimeMenuMachineActionType_Shutdown          = RT_BIT(6),
        RuntimeMenuMachineActionType_PowerOff          = RT_BIT(7),
        RuntimeMenuMachineActionType_Nothing           = RT_BIT(8),
        RuntimeMenuMachineActionType_All               = 0xFFFF
    };

    /** Runtime UI: Menu "View": Action types. */
    enum RuntimeMenuViewActionType
    {
        RuntimeMenuViewActionType_Invalid              = 0,
        RuntimeMenuViewActionType_Fullscreen           = RT_BIT(0),
        RuntimeMenuViewActionType_Seamless             = RT_BIT(1),
        RuntimeMenuViewActionType_Scale                = RT_BIT(2),
#ifndef RT_OS_DARWIN
        RuntimeMenuViewActionType_MinimizeWindow       = RT_BIT(3),
#endif /* !RT_OS_DARWIN */
        RuntimeMenuViewActionType_AdjustWindow         = RT_BIT(4),
        RuntimeMenuViewActionType_GuestAutoresize      = RT_BIT(5),
        RuntimeMenuViewActionType_TakeScreenshot       = RT_BIT(6),
        RuntimeMenuViewActionType_VideoCapture         = RT_BIT(7),
        RuntimeMenuViewActionType_VideoCaptureSettings = RT_BIT(8),
        RuntimeMenuViewActionType_StartVideoCapture    = RT_BIT(9),
        RuntimeMenuViewActionType_VRDEServer           = RT_BIT(10),
        RuntimeMenuViewActionType_MenuBar              = RT_BIT(11),
        RuntimeMenuViewActionType_MenuBarSettings      = RT_BIT(12),
#ifndef RT_OS_DARWIN
        RuntimeMenuViewActionType_ToggleMenuBar        = RT_BIT(13),
#endif /* !RT_OS_DARWIN */
        RuntimeMenuViewActionType_StatusBar            = RT_BIT(14),
        RuntimeMenuViewActionType_StatusBarSettings    = RT_BIT(15),
        RuntimeMenuViewActionType_ToggleStatusBar      = RT_BIT(16),
        RuntimeMenuViewActionType_ScaleFactor          = RT_BIT(17),
        RuntimeMenuViewActionType_Resize               = RT_BIT(18),
        RuntimeMenuViewActionType_Multiscreen          = RT_BIT(19),
        RuntimeMenuViewActionType_All                  = 0xFFFF
    };

    /** Runtime UI: Menu "Input": Action types. */
    enum RuntimeMenuInputActionType
    {
        RuntimeMenuInputActionType_Invalid           = 0,
        RuntimeMenuInputActionType_Keyboard          = RT_BIT(0),
        RuntimeMenuInputActionType_KeyboardSettings  = RT_BIT(1),
        RuntimeMenuInputActionType_TypeCAD           = RT_BIT(2),
#ifdef Q_WS_X11
        RuntimeMenuInputActionType_TypeCABS          = RT_BIT(3),
#endif /* Q_WS_X11 */
        RuntimeMenuInputActionType_TypeCtrlBreak     = RT_BIT(4),
        RuntimeMenuInputActionType_TypeInsert        = RT_BIT(5),
        RuntimeMenuInputActionType_Mouse             = RT_BIT(6),
        RuntimeMenuInputActionType_MouseIntegration  = RT_BIT(7),
        RuntimeMenuInputActionType_All               = 0xFFFF
    };

    /** Runtime UI: Menu "Devices": Action types. */
    enum RuntimeMenuDevicesActionType
    {
        RuntimeMenuDevicesActionType_Invalid               = 0,
        RuntimeMenuDevicesActionType_HardDrives            = RT_BIT(0),
        RuntimeMenuDevicesActionType_HardDrivesSettings    = RT_BIT(1),
        RuntimeMenuDevicesActionType_OpticalDevices        = RT_BIT(2),
        RuntimeMenuDevicesActionType_FloppyDevices         = RT_BIT(3),
        RuntimeMenuDevicesActionType_Network               = RT_BIT(4),
        RuntimeMenuDevicesActionType_NetworkSettings       = RT_BIT(5),
        RuntimeMenuDevicesActionType_USBDevices            = RT_BIT(6),
        RuntimeMenuDevicesActionType_USBDevicesSettings    = RT_BIT(7),
        RuntimeMenuDevicesActionType_WebCams               = RT_BIT(8),
        RuntimeMenuDevicesActionType_SharedClipboard       = RT_BIT(9),
        RuntimeMenuDevicesActionType_DragAndDrop           = RT_BIT(10),
        RuntimeMenuDevicesActionType_SharedFolders         = RT_BIT(11),
        RuntimeMenuDevicesActionType_SharedFoldersSettings = RT_BIT(12),
        RuntimeMenuDevicesActionType_InstallGuestTools     = RT_BIT(13),
        RuntimeMenuDevicesActionType_Nothing               = RT_BIT(14),
        RuntimeMenuDevicesActionType_All                   = 0xFFFF
    };

#ifdef VBOX_WITH_DEBUGGER_GUI
    /** Runtime UI: Menu "Debugger": Action types. */
    enum RuntimeMenuDebuggerActionType
    {
        RuntimeMenuDebuggerActionType_Invalid     = 0,
        RuntimeMenuDebuggerActionType_Statistics  = RT_BIT(0),
        RuntimeMenuDebuggerActionType_CommandLine = RT_BIT(1),
        RuntimeMenuDebuggerActionType_Logging     = RT_BIT(2),
        RuntimeMenuDebuggerActionType_LogDialog   = RT_BIT(3),
        RuntimeMenuDebuggerActionType_All         = 0xFFFF
    };
#endif /* VBOX_WITH_DEBUGGER_GUI */

#ifdef RT_OS_DARWIN
    /** Menu "Window": Action types. */
    enum MenuWindowActionType
    {
        MenuWindowActionType_Invalid  = 0,
        MenuWindowActionType_Minimize = RT_BIT(0),
        MenuWindowActionType_Switch   = RT_BIT(1),
        MenuWindowActionType_All      = 0xFFFF
    };
#endif /* RT_OS_DARWIN */
};

/** Common UI: Global settings page types. */
enum GlobalSettingsPageType
{
    GlobalSettingsPageType_Invalid,
    GlobalSettingsPageType_General,
    GlobalSettingsPageType_Input,
#ifdef VBOX_GUI_WITH_NETWORK_MANAGER
    GlobalSettingsPageType_Update,
#endif /* VBOX_GUI_WITH_NETWORK_MANAGER */
    GlobalSettingsPageType_Language,
    GlobalSettingsPageType_Display,
    GlobalSettingsPageType_Network,
    GlobalSettingsPageType_Extensions,
#ifdef VBOX_GUI_WITH_NETWORK_MANAGER
    GlobalSettingsPageType_Proxy,
#endif /* VBOX_GUI_WITH_NETWORK_MANAGER */
    GlobalSettingsPageType_Max
};
Q_DECLARE_METATYPE(GlobalSettingsPageType);

/** Common UI: Machine settings page types. */
enum MachineSettingsPageType
{
    MachineSettingsPageType_Invalid,
    MachineSettingsPageType_General,
    MachineSettingsPageType_System,
    MachineSettingsPageType_Display,
    MachineSettingsPageType_Storage,
    MachineSettingsPageType_Audio,
    MachineSettingsPageType_Network,
    MachineSettingsPageType_Ports,
    MachineSettingsPageType_Serial,
    MachineSettingsPageType_Parallel,
    MachineSettingsPageType_USB,
    MachineSettingsPageType_SF,
    MachineSettingsPageType_Interface,
    MachineSettingsPageType_Max
};
Q_DECLARE_METATYPE(MachineSettingsPageType);

/** Common UI: Wizard types. */
enum WizardType
{
    WizardType_Invalid,
    WizardType_NewVM,
    WizardType_CloneVM,
    WizardType_ExportAppliance,
    WizardType_ImportAppliance,
    WizardType_FirstRun,
    WizardType_NewVD,
    WizardType_CloneVD
};

/** Common UI: Wizard modes. */
enum WizardMode
{
    WizardMode_Auto,
    WizardMode_Basic,
    WizardMode_Expert
};


/** Selector UI: Details-element types. */
enum DetailsElementType
{
    DetailsElementType_Invalid,
    DetailsElementType_General,
    DetailsElementType_System,
    DetailsElementType_Preview,
    DetailsElementType_Display,
    DetailsElementType_Storage,
    DetailsElementType_Audio,
    DetailsElementType_Network,
    DetailsElementType_Serial,
#ifdef VBOX_WITH_PARALLEL_PORTS
    DetailsElementType_Parallel,
#endif /* VBOX_WITH_PARALLEL_PORTS */
    DetailsElementType_USB,
    DetailsElementType_SF,
    DetailsElementType_UI,
    DetailsElementType_Description
};
Q_DECLARE_METATYPE(DetailsElementType);

/** Selector UI: Preview update interval types. */
enum PreviewUpdateIntervalType
{
    PreviewUpdateIntervalType_Disabled,
    PreviewUpdateIntervalType_500ms,
    PreviewUpdateIntervalType_1000ms,
    PreviewUpdateIntervalType_2000ms,
    PreviewUpdateIntervalType_5000ms,
    PreviewUpdateIntervalType_10000ms,
    PreviewUpdateIntervalType_Max
};


/** Runtime UI: Visual-state types. */
enum UIVisualStateType
{
    UIVisualStateType_Invalid    = 0,
    UIVisualStateType_Normal     = RT_BIT(0),
    UIVisualStateType_Fullscreen = RT_BIT(1),
    UIVisualStateType_Seamless   = RT_BIT(2),
    UIVisualStateType_Scale      = RT_BIT(3),
    UIVisualStateType_All        = 0xFF
};
Q_DECLARE_METATYPE(UIVisualStateType);

/** Runtime UI: Indicator types. */
enum IndicatorType
{
    IndicatorType_Invalid,
    IndicatorType_HardDisks,
    IndicatorType_OpticalDisks,
    IndicatorType_FloppyDisks,
    IndicatorType_Network,
    IndicatorType_USB,
    IndicatorType_SharedFolders,
    IndicatorType_Display,
    IndicatorType_VideoCapture,
    IndicatorType_Features,
    IndicatorType_Mouse,
    IndicatorType_Keyboard,
    IndicatorType_KeyboardExtension,
    IndicatorType_Max
};
Q_DECLARE_METATYPE(IndicatorType);

/** Runtime UI: Machine close actions. */
enum MachineCloseAction
{
    MachineCloseAction_Invalid                    = 0,
    MachineCloseAction_Detach                     = RT_BIT(0),
    MachineCloseAction_SaveState                  = RT_BIT(1),
    MachineCloseAction_Shutdown                   = RT_BIT(2),
    MachineCloseAction_PowerOff                   = RT_BIT(3),
    MachineCloseAction_PowerOff_RestoringSnapshot = RT_BIT(4),
    MachineCloseAction_All                        = 0xFF
};
Q_DECLARE_METATYPE(MachineCloseAction);

/** Runtime UI: Mouse capture policy types. */
enum MouseCapturePolicy
{
    MouseCapturePolicy_Default,
    MouseCapturePolicy_HostComboOnly,
    MouseCapturePolicy_Disabled
};

/** Guru Meditation handler types. */
enum GuruMeditationHandlerType
{
    GuruMeditationHandlerType_Default,
    GuruMeditationHandlerType_PowerOff,
    GuruMeditationHandlerType_Ignore
};

/** Runtime UI: Scaling optimization types. */
enum ScalingOptimizationType
{
    ScalingOptimizationType_None,
    ScalingOptimizationType_Performance
};

/** Runtime UI: HiDPI optimization types. */
enum HiDPIOptimizationType
{
    HiDPIOptimizationType_None,
    HiDPIOptimizationType_Performance
};

#ifndef Q_WS_MAC
/** Runtime UI: Mini-toolbar alignment. */
enum MiniToolbarAlignment
{
    MiniToolbarAlignment_Bottom,
    MiniToolbarAlignment_Top
};
#endif /* !Q_WS_MAC */

#endif /* !___UIExtraDataDefs_h___ */

