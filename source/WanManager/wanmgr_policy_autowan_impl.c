/*
   If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2019 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

/* ---- Include Files ---------------------------------------- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <syscfg/syscfg.h>
#include "wanmgr_controller.h"
#include "wanmgr_data.h"
#include "wanmgr_rdkbus_utils.h"
#include "wanmgr_interface_sm.h"
#include "wanmgr_platform_events.h"
#include "wanmgr_sysevents.h"
#include <time.h>
#include <pthread.h>
#include "secure_wrapper.h"

/* ---- Global Constants -------------------------- */
#define LOOP_TIMEOUT 500000 // timeout in milliseconds. This is the state machine loop interval
#define WAN_PHY_NAME "erouter0"

/* fixed mode policy states */
typedef enum {
    STATE_WAN_SELECTING_INTERFACE = 0,
    STATE_WAN_INTERFACE_DOWN,
    STATE_WAN_INTERFACE_UP,
    STATE_WAN_WAITING_FOR_INTERFACE,
    STATE_WAN_CONFIGURING_INTERFACE,
    STATE_WAN_DECONFIGURING_INTERFACE,
    STATE_WAN_SCANNING_INTERFACE,
    STATE_WAN_INTERFACE_ACTIVE,
    STATE_WAN_INTERFACE_TEARDOWN,
    STATE_WAN_EXIT
} WcFmobPolicyState_t;

typedef enum WanMode
{
    WAN_MODE_AUTO = 0,
    WAN_MODE_SECONDARY, /* Ethwan Interface */
    WAN_MODE_PRIMARY,   /* Docsis Interface */
    WAN_MODE_UNKNOWN
}WanMode_t;

typedef enum
{
ERT_MODE_IPV4 = 1,
ERT_MODE_IPV6,
ERT_MODE_DUAL
}eroutermode_t;

typedef enum {
    STATUS_UP,
    STATUS_DOWN,
    STATUS_TIMEOUT,
    STATUS_UNKNOWN
}ValidationStatus_t;

// Wanmanager Auto Wan State Machine Info.
typedef  struct _WANMGR_AUTOWAN__SMINFO_
{
    WanMgr_Policy_Controller_t    wanPolicyCtrl;
    INT previousActiveInterfaceIndex;
}WanMgr_AutoWan_SMInfo_t;

#ifdef WAN_FAILOVER_SUPPORTED
/* Backup WAN Policy */
#define BACKUP_WAN_DHCPC_PID_FILE         "/var/run/bkupwan_dhcpc.pid"
#define BACKUP_WAN_DHCPC_SOURCE_FILE      "/etc/udhcpc_backupwan.script"

extern int sysevent_fd;
extern token_t sysevent_token;

typedef enum
{
    WAN_START_FOR_VALIDATION = 0,
    WAN_START_FOR_BACKUP_WAN
}WanStarCallSource_t;

typedef enum {
    STATE_BACKUP_WAN_SELECTING_INTERFACE = 0,
    STATE_BACKUP_WAN_INTERFACE_DOWN,
    STATE_BACKUP_WAN_VALIDATING_INTERFACE,
    STATE_BACKUP_WAN_AVAILABLE,
    STATE_BACKUP_WAN_INTERFACE_UP,
    STATE_BACKUP_WAN_INTERFACE_ACTIVE,
    STATE_BACKUP_WAN_INTERFACE_INACTIVE,
    STATE_BACKUP_WAN_WAITING,
    STATE_BACKUP_WAN_EXIT
} WcBWanPolicyState_t;

typedef  struct _WANMGR_BACKUPWAN_SMINFO_
{
    WanMgr_Policy_Controller_t    wanPolicyCtrl;
    ValidationStatus_t            ValidationStatus;
    INT                           ValidationRetries;
}WanMgr_BackupWan_SMInfo_t;
#endif /* WAN_FAILOVER_SUPPORTED */

/* ---- Global Variables -------------------------- */
int g_CurrentWanMode        = 0;
int g_LastKnowWanMode       = 0;
int g_SelectedWanMode       = 0;

static pthread_t  gBackupWanThread;

/* STATES */
static WcFmobPolicyState_t State_SelectingWanInterface(WanMgr_Policy_Controller_t* pWanController);
static WcFmobPolicyState_t State_FixedWanInterfaceDown(WanMgr_Policy_Controller_t* pWanController);
static WcFmobPolicyState_t State_FixedWanInterfaceUp(WanMgr_Policy_Controller_t* pWanController);

/* TRANSITIONS */
static WcFmobPolicyState_t Transition_Start(WanMgr_Policy_Controller_t* pWanController);
static WcFmobPolicyState_t Transition_WanInterfaceSelected(WanMgr_Policy_Controller_t* pWanController);
static WcFmobPolicyState_t Transition_FixedWanInterfaceUp(WanMgr_Policy_Controller_t* pWanController);
static WcFmobPolicyState_t Transition_FixedWanInterfaceDown(WanMgr_Policy_Controller_t* pWanController);

/* STATES */
static WcFmobPolicyState_t State_WaitingForInterface(WanMgr_AutoWan_SMInfo_t *pSmInfo);
static WcFmobPolicyState_t State_WanInterfaceTearDown(WanMgr_Policy_Controller_t* pWanController);


/* TRANSITIONS */
static WcFmobPolicyState_t Transition_StartAuto(WanMgr_AutoWan_SMInfo_t *pSmInfo);
static WcFmobPolicyState_t Transition_WanInterfaceActive(WanMgr_AutoWan_SMInfo_t *pSmInfo);
static WcFmobPolicyState_t Transition_WaitingForInterface(WanMgr_Iface_Data_t *pWanActiveIfaceData);
static WcFmobPolicyState_t Transition_WanInterfaceTearDown(WanMgr_Policy_Controller_t* pWanController);

#ifdef WAN_FAILOVER_SUPPORTED
/* BackUp WAN Policy */
ANSC_STATUS WanMgr_Policy_BackupWan(void);

/* STATES */
static WcBWanPolicyState_t State_SelectingBackupWanInterface(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t State_ValidatingBackupWanInterface(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t State_BackupWanInterfaceDown(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t State_BackupWanAvailable(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t State_BackupWanInterfaceUp(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t State_BackupWanInterfaceActive(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t State_BackupWanInterfaceInActive(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t State_BackupWanInterfaceWaitingPrimaryUp(WanMgr_Policy_Controller_t* pWanController);

/* TRANSITIONS */
static WcBWanPolicyState_t Transition_StartBakupWan(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t Transition_BackupWanSelectingInterface(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t Transition_ValidatingBackupWanInterface(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t Transition_BackupWanAvailable(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t Transition_BackupWanInterfaceUp(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t Transition_BackupWanInterfaceDown(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t Transition_BackupWanInterfaceActive(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t Transition_BackupWanInterfaceInActive(WanMgr_Policy_Controller_t* pWanController);
static WcBWanPolicyState_t Transition_BackupWanInterfaceWaitingPrimaryUp(WanMgr_Policy_Controller_t* pWanController);
#endif /* WAN_FAILOVER_SUPPORTED */

/* Auto Wan Detection Functions */
static int GetCurrentWanMode(void);
static void SetCurrentWanMode(int mode);
static int GetSelectedWanMode(void);
static void SelectedWanMode(int mode);
static int GetSelectedWanModeFromDb(void);
static int GetLastKnownWanModeFromDb(void);
static int GetLastKnownWanMode(void);
static void SetLastKnownWanMode(int mode);
static char *WanModeStr(int WanMode);
static void LogWanModeInfo(void);
static void IntializeAutoWanConfig(void);
static ANSC_STATUS Wanmgr_WanFixedMode_StartStateMachine(void);
static void AutoWan_BkupAndReboot(void);

/*********************************************************************************/
/**************************** ACTIONS ********************************************/
/*********************************************************************************/
static DML_WAN_IFACE_TYPE WanMgr_GetWanInterfaceType(INT  iWanInterfaceIndex)
{
    DML_WAN_IFACE_TYPE type = WAN_IFACE_TYPE_PRIMARY;
    WanMgr_Iface_Data_t*   pWanDmlIfaceData = WanMgr_GetIfaceData_locked(iWanInterfaceIndex);
    if (pWanDmlIfaceData != NULL)
    {
        DML_WAN_IFACE* pWanIfaceData = &(pWanDmlIfaceData->data);

        if (pWanIfaceData != NULL)
        {
            type = pWanIfaceData->Wan.Type;
        }
        WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
    }
    return type;
}

static INT WanMgr_Policy_AutoWan_CfgPostWanSelection(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    INT uiWanIdx = 0;
    UINT uiTotalIfaces = -1;
    if (!pSmInfo)
        return -1;

    //Get uiTotalIfaces
    uiTotalIfaces = WanMgr_IfaceData_GetTotalWanIface();

    if(uiTotalIfaces > 0)
    {
        // Check the policy to determine if any primary interface should be used for WAN
        for(uiWanIdx = 0; uiWanIdx < uiTotalIfaces; ++uiWanIdx )
        {
            WanMgr_Iface_Data_t*   pWanDmlIfaceData = WanMgr_GetIfaceData_locked(uiWanIdx);
            if(pWanDmlIfaceData != NULL)
            {
                DML_WAN_IFACE* pInterface = NULL;
                char acInstanceNumber[256] = {0};

                pInterface = &(pWanDmlIfaceData->data);

                if(pInterface == NULL)
                {
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                    return -1;
                }

#if defined (WAN_FAILOVER_SUPPORTED)
                //Filter only LOCAL interface
                if( LOCAL_IFACE != pInterface->Wan.IfaceType )
                {
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                    continue;
                }
#endif
                snprintf(acInstanceNumber,sizeof(acInstanceNumber),"%d",pInterface->uiInstanceNumber);
                ANSC_STATUS ret = WanMgr_RdkBus_SetRequestIfComponent(pInterface->Phy.Path,PARAM_NAME_POST_CFG_WAN_FINALIZE,acInstanceNumber,ccsp_string);
                if (ret == ANSC_STATUS_FAILURE)
                {
                    CcspTraceError(("%s WanMgr_RdkBus_SetRequestIfComponent failed for param %s.%s\n",__FUNCTION__,pInterface->Phy.Path,PARAM_NAME_POST_CFG_WAN_FINALIZE));
                }

                WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
            }
        }
    }

    return 0;
}

static WcFmobPolicyState_t WanMgr_Policy_AutoWan_SelectAlternateInterface(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WcFmobPolicyState_t retState = STATE_WAN_WAITING_FOR_INTERFACE;
    INT uiWanIdx = 0;
    UINT uiTotalIfaces = -1;
    DML_WAN_IFACE* pActiveInterface = NULL;
    if (!pSmInfo)
        return STATE_WAN_WAITING_FOR_INTERFACE;

    WanMgr_Policy_Controller_t* pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pActiveInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    //Get uiTotalIfaces
    uiTotalIfaces = WanMgr_IfaceData_GetTotalWanIface();

    if(uiTotalIfaces > 0)
    {
        // Check the policy to determine if any primary interface should be used for WAN
        for(uiWanIdx = 0; uiWanIdx < uiTotalIfaces; ++uiWanIdx )
        {
            WanMgr_Iface_Data_t*   pWanDmlIfaceData = WanMgr_GetIfaceData_locked(uiWanIdx);
            if(pWanDmlIfaceData != NULL)
            {
#if defined (WAN_FAILOVER_SUPPORTED)
                //Filter only LOCAL interface
                if( LOCAL_IFACE == pWanDmlIfaceData->data.Wan.IfaceType )
#endif
                {
                    /* Check and select alternate wan instance based on current active index.
                    * This logic is only applicable for two interface instance(Primary and Secondary) case only.
                    * if current active index == 0(Primary), then it selects index = 1 ( secondary)
                    * if current active index == 1(Secondary), then it selects index = 0 (Primary)
                    */
                    if ((pActiveInterface) && pActiveInterface->uiIfaceIdx != uiWanIdx)
                    {
                        pSmInfo->previousActiveInterfaceIndex =  pWanController->activeInterfaceIdx;
                        pWanController->activeInterfaceIdx = uiWanIdx;
                        retState = Transition_WaitingForInterface(pWanDmlIfaceData);
                        WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                        return retState;
                    }
                }

                WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
            }
        }
    }

    return STATE_WAN_WAITING_FOR_INTERFACE;
}

static INT WanMgr_Policy_AutoWan_GetLastKnownModeInterfaceIndex(void)
{
    INT iActiveWanIdx = 0;
    UINT uiTotalIfaces = -1;
    INT lastKnownMode = GetLastKnownWanMode();

    //Get uiTotalIfaces
    uiTotalIfaces = WanMgr_IfaceData_GetTotalWanIface();

    if(uiTotalIfaces > 0)
    {
        // Check the policy to determine if any primary interface should be used for WAN
        for(iActiveWanIdx = 0; iActiveWanIdx < uiTotalIfaces; ++iActiveWanIdx )
        {
            WanMgr_Iface_Data_t*   pWanDmlIfaceData = WanMgr_GetIfaceData_locked(iActiveWanIdx);
            if(pWanDmlIfaceData != NULL)
            {
                DML_WAN_IFACE* pWanIfaceData = &(pWanDmlIfaceData->data);
                if (pWanIfaceData != NULL)
                {
                    DML_WAN_IFACE_TYPE type = pWanIfaceData->Wan.Type;
                    IFACE_TYPE  IfaceType = pWanIfaceData->Wan.IfaceType;
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);

#if defined (WAN_FAILOVER_SUPPORTED)
                    //Filter only LOCAL Interface
                    if( LOCAL_IFACE == IfaceType )
#endif
                    {
                        CcspTraceInfo(("Booting-Up in Last known WanMode - %s\n",WanModeStr(lastKnownMode)));
                        /* Return index number if wan operational mode
                        * and wan interface type are same.
                        */
                        switch (lastKnownMode)
                        {
                            case WAN_MODE_PRIMARY:
                            {
                                if (type == WAN_IFACE_TYPE_PRIMARY)
                                {
                                    return iActiveWanIdx;
                                }

                            }
                            break;
                            case WAN_MODE_SECONDARY:
                            {
                                if (type == WAN_IFACE_TYPE_SECONDARY)
                                {
                                    return iActiveWanIdx;
                                }

                            }
                            break;
                            default:
                            break;
                        }
                    }
                }
                else
                {
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);

                }

            }
        }
    }

    return -1;
}



/*********************************************************************************/
/************************** TRANSITIONS ******************************************/
/*********************************************************************************/
static WcFmobPolicyState_t Transition_Start(WanMgr_Policy_Controller_t* pWanController)
{
    CcspTraceInfo(("%s %d - State changed to STATE_WAN_SELECTING_INTERFACE \n", __FUNCTION__, __LINE__));
    return STATE_WAN_SELECTING_INTERFACE;
}

static WcFmobPolicyState_t Transition_WanInterfaceSelected(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_WAN_SELECTING_INTERFACE;
    }

    //ActiveLink
    pFixedInterface->Wan.ActiveLink = TRUE;
    pFixedInterface->SelectionStatus = WAN_IFACE_SELECTED;
    pFixedInterface->InterfaceScanStatus = WAN_IFACE_STATUS_SCANNED;
    pFixedInterface->Wan.Status =  WAN_IFACE_STATUS_DISABLED;

    //Update current active interface variable
    Update_Interface_Status();

    CcspTraceInfo(("%s %d - State changed to STATE_WAN_INTERFACE_DOWN \n", __FUNCTION__, __LINE__));
    return STATE_WAN_INTERFACE_DOWN;
}

static WcFmobPolicyState_t Transition_FixedWanInterfaceDown(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_WAN_SELECTING_INTERFACE;
    }

    CcspTraceInfo(("%s %d - State changed to STATE_WAN_INTERFACE_DOWN \n", __FUNCTION__, __LINE__));

    pFixedInterface->SelectionStatus = WAN_IFACE_NOT_SELECTED;
    pFixedInterface->Wan.Status =  WAN_IFACE_STATUS_DISABLED;

    //Update current active interface variable
    Update_Interface_Status();

    // wan stop
    wanmgr_setwanstop();
    return STATE_WAN_INTERFACE_DOWN;
}

static WcFmobPolicyState_t Transition_FixedWanInterfaceUp(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;
    WanMgr_IfaceSM_Controller_t wanIfCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_WAN_SELECTING_INTERFACE;
    }

    pFixedInterface->SelectionStatus = WAN_IFACE_ACTIVE;
    pFixedInterface->Wan.Status =  WAN_IFACE_STATUS_UP;

    //Update current active interface variable
    Update_Interface_Status();

     // start wan
     wanmgr_setwanstart();
     wanmgr_sshd_restart();

    CcspTraceInfo(("%s %d - State changed to STATE_WAN_INTERFACE_UP \n", __FUNCTION__, __LINE__));
    return STATE_WAN_INTERFACE_UP;
}

/*********************************************************************************/
/**************************** STATES *********************************************/
/*********************************************************************************/
static WcFmobPolicyState_t State_SelectingWanInterface(WanMgr_Policy_Controller_t* pWanController)
{
    if(pWanController == NULL)
    {
        return ANSC_STATUS_FAILURE;
    }

    pWanController->activeInterfaceIdx = WanMgr_Policy_AutoWan_GetLastKnownModeInterfaceIndex();
    if(pWanController->activeInterfaceIdx != -1)
    {
        return Transition_WanInterfaceSelected(pWanController);
    }


    return STATE_WAN_SELECTING_INTERFACE;
}

static WcFmobPolicyState_t State_FixedWanInterfaceDown(WanMgr_Policy_Controller_t* pWanController)
{
    int iLoopCount;
    INT iSelectWanIdx = -1;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_WAN_SELECTING_INTERFACE;
    }

    if (pWanController->WanEnable == FALSE)
    {
        return Transition_WanInterfaceTearDown(pWanController);
    }

    if( pWanController->WanEnable == TRUE &&
        (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_UP ||
         pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_INITIALIZING) &&
        pFixedInterface->Wan.Status == WAN_IFACE_STATUS_DISABLED)
    {
        return Transition_FixedWanInterfaceUp(pWanController);
    }

    return STATE_WAN_INTERFACE_DOWN;
}

static WcFmobPolicyState_t State_FixedWanInterfaceUp(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_WAN_INTERFACE_DOWN;
    }

    if (pWanController->WanEnable == FALSE)
    {
        return Transition_WanInterfaceTearDown(pWanController);
    }

    if(pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_DOWN)
    {
        return Transition_FixedWanInterfaceDown(pWanController);
    }

    return STATE_WAN_INTERFACE_UP;
}


static INT StartWanClients(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    INT status = STATUS_DOWN;
    INT lastKnownMode = GetLastKnownWanMode();
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    DML_WAN_IFACE* pFixedInterface = NULL;
    char out_value[64] = {0};
    char wanPhyName[64] = {0};
    INT eRouterMode = ERT_MODE_IPV4;
    int ret =0;
#if defined(INTEL_PUMA7)
    char udhcpcEnable[20] = {0};
    char dibblerClientEnable[20] = {0};
#endif
    if (!pSmInfo)
        return status;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return status;
    }

    memset(out_value, 0, sizeof(out_value));
    syscfg_get(NULL, "wan_physical_ifname", out_value, sizeof(out_value));

    if(0 != strnlen(out_value,sizeof(out_value)))
    {
        snprintf(wanPhyName, sizeof(wanPhyName), "%s", out_value);
    }
    else
    {
        snprintf(wanPhyName, sizeof(wanPhyName), "%s", WAN_PHY_NAME);
    }
    if (!syscfg_get(NULL, "last_erouter_mode", out_value, sizeof(out_value)))
    {
        eRouterMode = atoi(out_value);
    }

#if defined(INTEL_PUMA7)
    memset(out_value, 0, sizeof(out_value));
    if (!syscfg_get(NULL, "UDHCPEnable", out_value, sizeof(out_value)))
    {
       snprintf(udhcpcEnable, sizeof(udhcpcEnable), "%s", out_value);
    }

    memset(out_value, 0, sizeof(out_value));
    if (!syscfg_get(NULL, "dibbler_client_enable", out_value, sizeof(out_value)))
    {
       snprintf(dibblerClientEnable, sizeof(dibblerClientEnable), "%s", out_value);
    }

#endif

    CcspTraceInfo(("%s %d - last known mode %d Current index %d If_name %s \n", __FUNCTION__, __LINE__,lastKnownMode,pFixedInterface->uiIfaceIdx,pFixedInterface->Wan.Name));
    switch (lastKnownMode)
    {
        case WAN_MODE_PRIMARY:
            {
                if (pFixedInterface->Wan.Type == WAN_IFACE_TYPE_PRIMARY)
                {
                    if (WanMgr_GetWanInterfaceType(pSmInfo->previousActiveInterfaceIndex) != WAN_IFACE_TYPE_PRIMARY)
                    {
                        wanmgr_setwanstop();
                        v_secure_system("killall udhcpc");
#if defined(INTEL_PUMA7)
                        if(0 == strncmp(dibblerClientEnable, "yes", sizeof(dibblerClientEnable)))
                        {
#endif
                            v_secure_system("killall dibbler-client");
#if defined(INTEL_PUMA7)
                        }
                        else
                        {
                            v_secure_system("killall ti_dhcpv6c");
                        }
#endif
#if defined(INTEL_PUMA7)
                        if(0 == strncmp(udhcpcEnable, "yes", sizeof(udhcpcEnable)))
                        {
#endif
                            v_secure_system("killall udhcpc");
#if defined(INTEL_PUMA7)
                        }
                        else
                        {
                            v_secure_system("killall ti_udhcpc");
                        }
#endif
                    }

                    // start wan
                    wanmgr_setwanstart();
                    wanmgr_sshd_restart();
                }
                else
                {
                    // need to start DHCPv6 client when eRouterMode == ERT_MODE_DUAL
                    if (eRouterMode == ERT_MODE_IPV6)
                    {
#if defined(INTEL_PUMA7)
                        if(0 == strncmp(dibblerClientEnable, "yes", sizeof(dibblerClientEnable)))
                        {
#endif
                            v_secure_system("killall dibbler-client");
                            v_secure_system("/etc/dibbler/dibbler-init.sh");
                            v_secure_system("/usr/sbin/dibbler-client start");
                            CcspTraceInfo(("%s %d - dibbler client start\n", __FUNCTION__, __LINE__));

#if defined(INTEL_PUMA7)
                        }
                        else
                        {
                            v_secure_system("killall ti_dhcpv6c");
                            ret = v_secure_system("ti_dhcp6c -plugin /lib/libgw_dhcp6plg.so -i %s -p /var/run/erouter_dhcp6c.pid &",pFixedInterface->Wan.Name);
                            if(ret != 0) {
                                CcspTraceWarning(("%s : Failure in executing command via v_secure_system. ret:[%d] \n",__FUNCTION__, ret));
                            }
                        }
#endif

                    } // (eRouterMode == ERT_MODE_IPV6)
                    else if(eRouterMode == ERT_MODE_IPV4 || eRouterMode == ERT_MODE_DUAL)
                    {
                        ret = v_secure_system("sysctl -w net.ipv6.conf.%s.accept_ra=2",pFixedInterface->Wan.Name);
                        if(ret != 0) {
                            CcspTraceWarning(("%s : Failure in executing command via v_secure_system. ret:[%d] \n",__FUNCTION__, ret));
                        }
                        //system("sysctl -w net.ipv6.conf.eth3.accept_ra=2");
                        v_secure_system("killall udhcpc");
                        ret = v_secure_system("udhcpc -i %s &", pFixedInterface->Wan.Name);
                        if(ret != 0) {
                            CcspTraceWarning(("%s : Failure in executing command via v_secure_system. ret:[%d] \n",__FUNCTION__, ret));
                        }
                        CcspTraceInfo(("%s %d - udhcpc start inf %s \n", __FUNCTION__, __LINE__,pFixedInterface->Wan.Name));
                    } // (eRouterMode == ERT_MODE_IPV4 || eRouterMode == ERT_MODE_DUAL)

                }
            }
            break;
        case WAN_MODE_SECONDARY:
            {
                if (pFixedInterface->Wan.Type == WAN_IFACE_TYPE_SECONDARY)
                {
                    if (WanMgr_GetWanInterfaceType(pSmInfo->previousActiveInterfaceIndex) != WAN_IFACE_TYPE_SECONDARY)
                    {
                        wanmgr_setwanstop();
                        v_secure_system("killall udhcpc");
#if defined(INTEL_PUMA7)
                        if(0 == strncmp(udhcpcEnable, "yes", sizeof(udhcpcEnable)))
                        {
                            v_secure_system("killall udhcpc");
                        }
                        if(0 == strncmp(dibblerClientEnable, "yes", sizeof(dibblerClientEnable)))
                        {
                            v_secure_system("killall dibbler-client");
                        }

                        v_secure_system("killall ti_udhcpc");
                        v_secure_system("killall ti_dhcpv6c");
#else
                        v_secure_system("killall udhcpc");
                        v_secure_system("killall dibbler-client");
#endif
                    }

                    // start wan
                    wanmgr_setwanstart();
                    wanmgr_sshd_restart();
                }
                else
                {
                    v_secure_system("killall udhcpc");
                    ret = v_secure_system("sysctl -w net.ipv6.conf.%s.accept_ra=2",pFixedInterface->Wan.Name);
                    if(ret != 0) {
                        CcspTraceWarning(("%s : Failure in executing command via v_secure_system. ret:[%d] \n",__FUNCTION__, ret));
                    }
#if defined(INTEL_PUMA7)
                    if(0 == strncmp(udhcpcEnable, "yes", sizeof(udhcpcEnable)))
                    {
                        v_secure_system("killall udhcpc");
                        ret = v_secure_system("/sbin/udhcpc -i %s -p /tmp/udhcpc.erouter0.pid -s /etc/udhcpc.script &",pFixedInterface->Wan.Name);
                        if(ret != 0) {
                            CcspTraceWarning(("%s : Failure in executing command via v_secure_system. ret:[%d] \n",__FUNCTION__, ret));
                        }
                    }
                    else
                    {
                        v_secure_system("killall ti_udhcpc");
                        ret = v_secure_system("ti_udhcpc -plugin /lib/libert_dhcpv4_plugin.so -i %s -H DocsisGateway -p /var/run/eRT_ti_udhcpc.pid -B -b 4 &",
                                pFixedInterface->Wan.Name);
                        if(ret != 0) {
                            CcspTraceWarning(("%s : Failure in executing command via v_secure_system. ret:[%d] \n",__FUNCTION__, ret));
                        }
                    }
#else

                    CcspTraceInfo(("%s - mode= %s wanPhyName= %s\n",__FUNCTION__,WanModeStr(WAN_MODE_PRIMARY),wanPhyName));

                    ret = v_secure_system("udhcpc -i %s &",pFixedInterface->Wan.Name);  
                    if(ret != 0) {
                        CcspTraceWarning(("%s : Failure in executing command via v_secure_system. ret:[%d] \n",__FUNCTION__, ret));
                    }
                    CcspTraceInfo(("%s %d - udhcpc start inf %s \n", __FUNCTION__, __LINE__,pFixedInterface->Wan.Name));
#endif
                }
            }
            break;
        default:
            break;
    }

    return status;

}

static WcFmobPolicyState_t Transition_StartAuto(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WanMgr_Iface_Data_t   *pWanActiveIfaceData = NULL;
    WanMgr_Policy_Controller_t    *pWanPolicyCtrl = NULL;
    char buf[16];
    char path[256];
    DML_WAN_IFACE  *pFixedInterface = NULL;
    if (!pSmInfo)
        return STATE_WAN_WAITING_FOR_INTERFACE;
    pWanPolicyCtrl = &pSmInfo->wanPolicyCtrl;
    if (!pWanPolicyCtrl)
        return STATE_WAN_WAITING_FOR_INTERFACE;

    //Update current active interface variable
    Update_Interface_Status();

    while (1)
    {
        pWanActiveIfaceData = WanMgr_GetIfaceData_locked(pWanPolicyCtrl->activeInterfaceIdx);
        if(pWanActiveIfaceData != NULL)
        {
            pFixedInterface = &pWanActiveIfaceData->data;
            if (pFixedInterface != NULL)
            {
                //ActiveLink
                pFixedInterface->Wan.ActiveLink = TRUE;

                if (strlen(pFixedInterface->Phy.Path) > 0)
                {
                    pFixedInterface->Phy.Status = WAN_IFACE_PHY_STATUS_UNKNOWN;
                    memset(buf,0,sizeof(buf));
                    memset(path,0,sizeof(path));
                    snprintf(buf, sizeof(buf),"%d",pFixedInterface->uiInstanceNumber);
                    snprintf(path, sizeof(path),"%s",pFixedInterface->Phy.Path);
                    // Release lock before goes for wait for interface component ready
                    WanMgrDml_GetIfaceData_release(pWanActiveIfaceData);
                    if (ANSC_STATUS_SUCCESS == WaitForInterfaceComponentReady(path))
                    {
                        char *mode = "WAN_SECONDARY";
                        if (pFixedInterface->Wan.Type == WAN_IFACE_TYPE_PRIMARY)
                        {
                            mode = "WAN_PRIMARY";
                        }

                        CcspTraceInfo(("%s %d - AUTOWAN Selected Interface %s Type:%s\n", __FUNCTION__, __LINE__,pFixedInterface->Wan.Name,mode));
                        ANSC_STATUS ret = WanMgr_RdkBus_SetRequestIfComponent(path,PARAM_NAME_REQUEST_PHY_STATUS,buf,ccsp_string);
                        if (ret == ANSC_STATUS_FAILURE)
                        {
                            CcspTraceError(("%s WanMgr_RdkBus_SetRequestIfComponent failed for param %s%s\n",__FUNCTION__,path,PARAM_NAME_REQUEST_PHY_STATUS));
                        }
                    }
                    CcspTraceInfo(("%s: Released Path Name %s\n", __FUNCTION__,path));
                    break;
                }
            }
            WanMgrDml_GetIfaceData_release(pWanActiveIfaceData);
        }
        sleep(1);
    }
    CcspTraceInfo(("%s %d - State changed to STATE_WAN_WAITING_FOR_INTERFACE \n", __FUNCTION__, __LINE__));
    return STATE_WAN_WAITING_FOR_INTERFACE;
}


static WcFmobPolicyState_t Transition_WanInterfacePhyUp(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if (!pSmInfo)
        return STATE_WAN_SELECTING_INTERFACE;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_WAN_SELECTING_INTERFACE;
    }

    CcspTraceInfo(("%s %d - State changed to STATE_WAN_SCANNING_INTERFACE If_Name %s \n", __FUNCTION__, __LINE__,pFixedInterface->Wan.Name));
    pFixedInterface->Wan.OperationalStatus = WAN_OPERSTATUS_UNKNOWN;
    pFixedInterface->IP.Ipv4Status = WAN_IFACE_IPV4_STATE_UNKNOWN;
    pFixedInterface->IP.Ipv6Status = WAN_IFACE_IPV6_STATE_UNKNOWN;
    pFixedInterface->SelectionStatus = WAN_IFACE_SELECTED;
    pFixedInterface->Wan.Status =  WAN_IFACE_STATUS_UP;

    //Update current active interface variable
    Update_Interface_Status();

    StartWanClients(pSmInfo);
    if (pFixedInterface->MonitorOperStatus == TRUE)
    {
        char buf[16];
        memset(buf,0,sizeof(buf));
        snprintf(buf, sizeof(buf),"%d",pFixedInterface->uiInstanceNumber);
        ANSC_STATUS ret = WanMgr_RdkBus_SetRequestIfComponent(pFixedInterface->Phy.Path,PARAM_NAME_REQUEST_OPERATIONAL_STATUS,buf,ccsp_string);
        if (ret == ANSC_STATUS_FAILURE)
        {
            CcspTraceError(("%s WanMgr_RdkBus_SetRequestIfComponent failed for param %s%s\n",__FUNCTION__,pFixedInterface->Phy.Path,PARAM_NAME_REQUEST_OPERATIONAL_STATUS));
        }
    }
    else
    {
        WanMgr_StartIpMonitor(pFixedInterface->uiIfaceIdx);
    }
    return STATE_WAN_SCANNING_INTERFACE;
}

static WcFmobPolicyState_t Transition_WaitingForInterface(WanMgr_Iface_Data_t *pWanActiveIfaceData)
{
    WcFmobPolicyState_t retState = STATE_WAN_WAITING_FOR_INTERFACE;
    DML_WAN_IFACE* pFixedInterface = NULL;
    char *mode = "WAN_SECONDARY";

        if (!pWanActiveIfaceData)
        return retState;


    if(pWanActiveIfaceData != NULL)
    {
        pFixedInterface = &pWanActiveIfaceData->data;
    }

    if(pFixedInterface == NULL)
    {
        return retState;
    }
       //ActiveLink
    pFixedInterface->Wan.ActiveLink = TRUE;
    pFixedInterface->SelectionStatus = WAN_IFACE_NOT_SELECTED;
    pFixedInterface->Wan.Status =  WAN_IFACE_STATUS_DISABLED;

    //Update current active interface variable
    Update_Interface_Status();

    pFixedInterface->Phy.Status = WAN_IFACE_PHY_STATUS_UNKNOWN;
    if (strlen(pFixedInterface->Phy.Path) > 0)
    {
        char buf[16];
        memset(buf,0,sizeof(buf));
        snprintf(buf, sizeof(buf),"%d",pFixedInterface->uiInstanceNumber);
        ANSC_STATUS ret = WanMgr_RdkBus_SetRequestIfComponent(pFixedInterface->Phy.Path,PARAM_NAME_REQUEST_PHY_STATUS,buf,ccsp_string);
        if (ret == ANSC_STATUS_FAILURE)
        {
            CcspTraceError(("%s WanMgr_RdkBus_SetRequestIfComponent failed for param %s%s\n",__FUNCTION__,pFixedInterface->Phy.Path,PARAM_NAME_REQUEST_PHY_STATUS));
        }
    }

    if (pFixedInterface->Wan.Type == WAN_IFACE_TYPE_PRIMARY)
    {
        mode = "WAN_PRIMARY";
    }

    CcspTraceInfo(("%s %d - AUTOWAN Selected Interface %s type %s \n", __FUNCTION__, __LINE__,pFixedInterface->Wan.Name,mode));
    CcspTraceInfo(("%s %d - State changed to STATE_WAN_WAITING_FOR_INTERFACE \n", __FUNCTION__, __LINE__));
    return STATE_WAN_WAITING_FOR_INTERFACE;
}

static WcFmobPolicyState_t Transition_WanInterfaceTearDown(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;
    WcFmobPolicyState_t retState = STATE_WAN_INTERFACE_TEARDOWN;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return retState;
    }

    // Reset Physical link status when state machine is teardown
    pFixedInterface->Phy.Status = WAN_IFACE_PHY_STATUS_UNKNOWN;
    pFixedInterface->SelectionStatus = WAN_IFACE_NOT_SELECTED;
    pFixedInterface->Wan.Status =  WAN_IFACE_STATUS_DISABLED;

    //Update current active interface variable
    Update_Interface_Status();

    wanmgr_setwanstop();
    v_secure_system("killall dibbler-client");
    v_secure_system("killall udhcpc");
    CcspTraceInfo(("%s %d - State changed to STATE_WAN_INTERFACE_TEARDOWN \n", __FUNCTION__, __LINE__));
    return retState;
}

static WcFmobPolicyState_t State_WanInterfaceTearDown(WanMgr_Policy_Controller_t* pWanController)
{
    WcFmobPolicyState_t retState = STATE_WAN_INTERFACE_TEARDOWN;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return retState;
    }

    if (pWanController->WanEnable == TRUE)
    {
        if (pWanController->WanOperationalMode != GetSelectedWanModeFromDb())
        {
            retState = STATE_WAN_EXIT;
            CcspTraceInfo(("%s %d - State changed to STATE_WAN_EXIT \n", __FUNCTION__, __LINE__));
        }
        else
        {
            // if wan operational mode is not changed then move to interface down state and
            // wait for phy status of current active interface.
            retState = STATE_WAN_INTERFACE_DOWN;
            if (WAN_MODE_AUTO == GetSelectedWanMode())
            {
                // if wan is not yet detected in autowan mode yet
                // then start the autowan state machine from begining.
                if (WAN_MODE_UNKNOWN == GetCurrentWanMode())
                {
                    retState = STATE_WAN_SELECTING_INTERFACE;
                    CcspTraceInfo(("%s %d - State changed to STATE_WAN_SELECTING_INTERFACE \n", __FUNCTION__, __LINE__));
                }
                else
                {
                    CcspTraceInfo(("%s %d - State changed to STATE_WAN_INTERFACE_DOWN \n", __FUNCTION__, __LINE__));
                }
            }
            else
            {
                CcspTraceInfo(("%s %d - State changed to STATE_WAN_INTERFACE_DOWN \n", __FUNCTION__, __LINE__));
            }
        }
    }

    return retState;
}

static WcFmobPolicyState_t Transition_WanInterfaceConfigured(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    CcspTraceInfo(("%s %d - State changed to STATE_WAN_SCANNING_INTERFACE \n", __FUNCTION__, __LINE__));
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if (!pSmInfo)
        return STATE_WAN_SELECTING_INTERFACE;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_WAN_SELECTING_INTERFACE;
    }

    pFixedInterface->Wan.OperationalStatus = WAN_OPERSTATUS_UNKNOWN;
    pFixedInterface->IP.Ipv4Status = WAN_IFACE_IPV4_STATE_UNKNOWN;
    pFixedInterface->IP.Ipv6Status = WAN_IFACE_IPV6_STATE_UNKNOWN;
    pFixedInterface->SelectionStatus = WAN_IFACE_SELECTED;
    pFixedInterface->Wan.Status =  WAN_IFACE_STATUS_DISABLED;

    //Update current active interface variable
    Update_Interface_Status();

    StartWanClients(pSmInfo);
    if (pFixedInterface->MonitorOperStatus == TRUE)
    {
        char buf[16];
        memset(buf,0,sizeof(buf));
        snprintf(buf, sizeof(buf),"%d",pFixedInterface->uiInstanceNumber);
        ANSC_STATUS ret = WanMgr_RdkBus_SetRequestIfComponent(pFixedInterface->Phy.Path,PARAM_NAME_REQUEST_OPERATIONAL_STATUS,buf,ccsp_string);
        if (ret == ANSC_STATUS_FAILURE)
        {
            CcspTraceError(("%s WanMgr_RdkBus_SetRequestIfComponent failed for param %s%s\n",__FUNCTION__,pFixedInterface->Phy.Path,PARAM_NAME_REQUEST_OPERATIONAL_STATUS));
        }
    }
    else
    {
        WanMgr_StartIpMonitor(pFixedInterface->uiIfaceIdx);
    }
    return STATE_WAN_SCANNING_INTERFACE;
}

static WcFmobPolicyState_t Transition_WanInterfaceActive(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WcFmobPolicyState_t retState = STATE_WAN_INTERFACE_ACTIVE;
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    DML_WAN_IFACE* pFixedInterface = NULL;
    INT lastKnownMode = GetLastKnownWanMode();

    if (!pSmInfo)
        return retState;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return retState;
    }

    pFixedInterface->SelectionStatus = WAN_IFACE_ACTIVE;
    pFixedInterface->Wan.Status = WAN_IFACE_STATUS_UP;
    //Update current active interface variable
    Update_Interface_Status();

    CcspTraceInfo(("%s %d - State changed to STATE_WAN_INTERFACE_ACTIVE if_name %s\n", __FUNCTION__, __LINE__,pFixedInterface->Wan.Name));
    CcspTraceInfo(("%s %d - LastKnownMode %d  Active index %d\n", __FUNCTION__, __LINE__,lastKnownMode,pWanController->activeInterfaceIdx));
#ifndef ENABLE_WANMODECHANGE_NOREBOOT
    // Do reboot during wan mode change if NOREBOOT feature is not enabled.
    pFixedInterface->Wan.RebootOnConfiguration = TRUE;
#endif
    switch (lastKnownMode)
    {
        case WAN_MODE_PRIMARY:
        {
            if (pFixedInterface->Wan.Type == WAN_IFACE_TYPE_SECONDARY)
            {
                SetLastKnownWanMode(WAN_MODE_SECONDARY);
                SetCurrentWanMode(WAN_MODE_SECONDARY);
                if (pFixedInterface->Wan.RebootOnConfiguration)
                {
                    CcspTraceInfo(("%s - WanMode %s is Locked, Set Current operational mode, rebooting... \n",__FUNCTION__,WanModeStr(WAN_MODE_SECONDARY)));
                    AutoWan_BkupAndReboot();
                }
                else
                {
                    StartWanClients(pSmInfo);
                    CcspTraceInfo(("%s - WanMode %s is Locked, Set Current operational mode, Lastknown mode and Current detected mode are different \n"
                                ,__FUNCTION__,WanModeStr(WAN_MODE_SECONDARY)));
                }
            }
            else
            {
                if (pFixedInterface->Wan.RebootOnConfiguration)
                {
                    CcspTraceInfo(("%s - WanMode %s is Locked, Set Current operational mode, reboot is not required\n",__FUNCTION__,WanModeStr(lastKnownMode)));
                }
                else
                {
                    CcspTraceInfo(("%s - WanMode %s is Locked, Set Current operational mode, Lastknown mode and Current detected mode are same \n",__FUNCTION__,WanModeStr(lastKnownMode)));
                }
                SetLastKnownWanMode(WAN_MODE_PRIMARY);
                SetCurrentWanMode(WAN_MODE_PRIMARY);
            }
        }
        break;
        case WAN_MODE_SECONDARY:
        {
            if (pFixedInterface->Wan.Type == WAN_IFACE_TYPE_PRIMARY)
            {
                SetLastKnownWanMode(WAN_MODE_PRIMARY);
                SetCurrentWanMode(WAN_MODE_PRIMARY);
                if (pFixedInterface->Wan.RebootOnConfiguration)
                {
                    CcspTraceInfo(("%s - WanMode %s is Locked, Set Current operational mode, rebooting... \n",__FUNCTION__,WanModeStr(WAN_MODE_PRIMARY)));
                    AutoWan_BkupAndReboot();
                }
                else
                {
                    StartWanClients(pSmInfo);
                    CcspTraceInfo(("%s - WanMode %s is Locked, Set Current operational mode, Lastknown mode and Current detected mode are different \n",
                                __FUNCTION__,WanModeStr(WAN_MODE_PRIMARY)));
                }

            }
            else
            {
                if (pFixedInterface->Wan.RebootOnConfiguration)
                {
                    CcspTraceInfo(("%s - WanMode %s is Locked, Set Current operational mode, reboot is not required\n",__FUNCTION__,WanModeStr(lastKnownMode)));
                }
                else
                {
                    CcspTraceInfo(("%s - WanMode %s is Locked, Set Current operational mode, Lastknown mode and Current detected mode are same \n",__FUNCTION__,WanModeStr(lastKnownMode)));
                }
                SetLastKnownWanMode(WAN_MODE_SECONDARY);
                SetCurrentWanMode(WAN_MODE_SECONDARY);
            }
        }
        break;
        default:
            break;
    }


    return STATE_WAN_INTERFACE_ACTIVE;
}


/*********************************************************************************/
/*********************************************************************************/
/*********************************************************************************/

static int GetCurrentWanMode(void)
{
    return g_CurrentWanMode;
}

static void SetCurrentWanMode(int mode)
{
    char buf[8];
    memset(buf, 0, sizeof(buf));
    g_CurrentWanMode = mode;
    CcspTraceInfo(("%s Set Current WanMode = %s\n",__FUNCTION__, WanModeStr(g_CurrentWanMode)));
    snprintf(buf, sizeof(buf), "%d", g_CurrentWanMode);
    if (syscfg_set_commit(NULL, "curr_wan_mode", buf) != 0)
    {
        CcspTraceInfo(("syscfg_set failed for curr_wan_mode\n"));
    }
}

static int GetSelectedWanMode(void)
{
    return g_SelectedWanMode;
}

static void SelectedWanMode(int mode)
{
    char buf[8];
    g_SelectedWanMode = mode;
    CcspTraceInfo(("%s Set  SelectedWanMode = %s\n",__FUNCTION__, WanModeStr(g_SelectedWanMode)));
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf), "%d", mode);
    if (syscfg_set_commit(NULL, "selected_wan_mode", buf) != 0)
    {
        CcspTraceInfo(("syscfg_set failed for curr_wan_mode\n"));
    }
}

static int GetSelectedWanModeFromDb(void)
{
    char buf[8] = {0};
    int wanMode = WAN_MODE_UNKNOWN;
     if (syscfg_get(NULL, "selected_wan_mode", buf, sizeof(buf)) == 0)
     {
        wanMode = atoi(buf);
     }
    return wanMode;
}

static int GetLastKnownWanModeFromDb(void)
{
    char buf[8] = {0};
    int wanMode = WAN_MODE_UNKNOWN;
     if (syscfg_get(NULL, "last_wan_mode", buf, sizeof(buf)) == 0)
     {
        wanMode = atoi(buf);
     }
    return wanMode;
}

static int GetLastKnownWanMode(void)
{
    return g_LastKnowWanMode;
}

static void SetLastKnownWanMode(int mode)
{
    char buf[8];
    g_LastKnowWanMode = mode;
    CcspTraceInfo(("%s Set Last Known WanMode = %s\n",__FUNCTION__, WanModeStr(g_LastKnowWanMode)));
    memset(buf, 0, sizeof(buf));
    snprintf(buf, sizeof(buf), "%d", mode);
    if (syscfg_set_commit(NULL, "last_wan_mode", buf) != 0)
    {
        CcspTraceInfo(("syscfg_set failed for last_wan_mode\n"));
    }
}

static char *WanModeStr(int WanMode)
{
    if(WanMode == WAN_MODE_AUTO)
    {
         return "WAN_MODE_AUTO";
    }
    if(WanMode == WAN_MODE_SECONDARY)
    {
         return "WAN_MODE_SECONDARY";
    }
    if(WanMode == WAN_MODE_PRIMARY)
    {
         return "WAN_MODE_PRIMARY";
    }
    if(WanMode == WAN_MODE_UNKNOWN)
    {
         return "WAN_MODE_UNKNOWN";
    }
}

static void LogWanModeInfo(void)
{
    CcspTraceInfo(("CurrentWanMode  - %s\n",WanModeStr(g_CurrentWanMode)));
    CcspTraceInfo(("SelectedWanMode - %s\n",WanModeStr(g_SelectedWanMode)));
    CcspTraceInfo(("LastKnowWanMode - %s\n",WanModeStr(g_LastKnowWanMode)));
}

static void IntializeAutoWanConfig(void)
{
    CcspTraceInfo(("%s\n",__FUNCTION__));
    g_CurrentWanMode        = WAN_MODE_UNKNOWN;
    g_LastKnowWanMode       = GetLastKnownWanModeFromDb();
    g_SelectedWanMode       = WAN_MODE_AUTO;

    char out_value[20];
    int outbufsz = sizeof(out_value);
    memset(out_value,0,sizeof(out_value));
    if (!syscfg_get(NULL, "selected_wan_mode", out_value, outbufsz))
    {
       g_SelectedWanMode = atoi(out_value);
       CcspTraceInfo(("AUTOWAN %s Selected WAN mode = %s\n",__FUNCTION__,WanModeStr(g_SelectedWanMode)));
    }
    else
    {
       SelectedWanMode(WAN_MODE_PRIMARY);
       CcspTraceInfo(("AUTOWAN %s AutoWAN is not Enabled, Selected WAN mode - %s\n",__FUNCTION__, WanModeStr(g_SelectedWanMode)));
    }
    if (g_LastKnowWanMode == WAN_MODE_UNKNOWN)
    {
        g_LastKnowWanMode = WAN_MODE_PRIMARY;
    }

    SetCurrentWanMode(WAN_MODE_UNKNOWN);
    LogWanModeInfo();

}


static ANSC_STATUS Wanmgr_WanFixedMode_StartStateMachine(void)
{
    CcspTraceInfo(("%s %d \n", __FUNCTION__, __LINE__));

    //policy variables
    ANSC_STATUS retStatus = ANSC_STATUS_SUCCESS;
    WanMgr_Policy_Controller_t    WanPolicyCtrl;
    WcFmobPolicyState_t fmob_sm_state;
    bool bRunning = true;

    // event handler
    int n = 0;
    struct timeval tv;

    if(WanMgr_Controller_PolicyCtrlInit(&WanPolicyCtrl) != ANSC_STATUS_SUCCESS)
    {
        CcspTraceError(("%s %d Policy Controller Error \n", __FUNCTION__, __LINE__));
        return ANSC_STATUS_FAILURE;
    }
    WanPolicyCtrl.WanOperationalMode = GetSelectedWanMode();
    CcspTraceInfo(("%s %d  Fixed Mode On Bootup Policy Thread Starting \n", __FUNCTION__, __LINE__));

    // initialise state machine
    fmob_sm_state = Transition_Start(&WanPolicyCtrl); // do this first before anything else to init variables

    while (bRunning)
    {
        /* Wait up to 500 milliseconds */
        tv.tv_sec = 0;
        tv.tv_usec = LOOP_TIMEOUT;

        n = select(0, NULL, NULL, NULL, &tv);
        if (n < 0)
        {
            /* interrupted by signal or something, continue */
            continue;
        }

        //Update Wan config
        WanMgr_Config_Data_t*   pWanConfigData = WanMgr_GetConfigData_locked();
        if(pWanConfigData != NULL)
        {
            WanPolicyCtrl.WanEnable = pWanConfigData->data.Enable;
            WanMgrDml_GetConfigData_release(pWanConfigData);
        }

        //Lock Iface Data
        WanPolicyCtrl.pWanActiveIfaceData = WanMgr_GetIfaceData_locked(WanPolicyCtrl.activeInterfaceIdx);

        // process state
        switch (fmob_sm_state)
        {
            case STATE_WAN_SELECTING_INTERFACE:
                fmob_sm_state = State_SelectingWanInterface(&WanPolicyCtrl);
                break;
            case STATE_WAN_INTERFACE_DOWN:
                fmob_sm_state = State_FixedWanInterfaceDown(&WanPolicyCtrl);
                break;
            case STATE_WAN_INTERFACE_UP:
                fmob_sm_state = State_FixedWanInterfaceUp(&WanPolicyCtrl);
                break;
            case STATE_WAN_INTERFACE_TEARDOWN:
                fmob_sm_state = State_WanInterfaceTearDown(&WanPolicyCtrl);
                break;
            case STATE_WAN_EXIT:
                bRunning = false;
                break;
            default:
                CcspTraceInfo(("%s %d - Case: default \n", __FUNCTION__, __LINE__));
                bRunning = false;
                retStatus = ANSC_STATUS_FAILURE;
                break;
        }

        //Release Lock Iface Data
        if(WanPolicyCtrl.pWanActiveIfaceData != NULL)
        {
            WanMgrDml_GetIfaceData_release(WanPolicyCtrl.pWanActiveIfaceData);
        }
    }

    CcspTraceInfo(("%s %d - Exit from state machine\n", __FUNCTION__, __LINE__));
}


static void AutoWan_BkupAndReboot(void)
{
    if (syscfg_set(NULL, "X_RDKCENTRAL-COM_LastRebootReason", "WAN_Mode_Change") != 0)
    {
        CcspTraceError(("RDKB_REBOOT : RebootDevice syscfg_set failed GUI\n"));
    }

    if (syscfg_set_commit(NULL, "X_RDKCENTRAL-COM_LastRebootCounter", "1") != 0)
    {
        CcspTraceError(("syscfg_set failed\n"));
    }

    /* Need to do reboot the device here */
    WanMgr_RdkBus_SetParamValues(PAM_COMPONENT_NAME, PAM_DBUS_PATH, "Device.X_CISCO_COM_DeviceControl.RebootDevice","Device",ccsp_string,TRUE);
}


static WcFmobPolicyState_t State_WanConfiguringInterface(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WcFmobPolicyState_t retState = STATE_WAN_CONFIGURING_INTERFACE;
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    ANSC_STATUS ret = ANSC_STATUS_SUCCESS;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if (!pSmInfo)
        return STATE_WAN_WAITING_FOR_INTERFACE;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_WAN_WAITING_FOR_INTERFACE;
    }

    if (pWanController->WanEnable == FALSE)
    {
        return Transition_WanInterfaceTearDown(pWanController);
    }
    CcspTraceInfo(("%s %d - AUTOWAN ifname %s \n", __FUNCTION__, __LINE__,pFixedInterface->Wan.Name));
    if (pFixedInterface->WanConfigEnabled == TRUE)
    {
        ret = WanMgr_RdkBus_SetRequestIfComponent(pFixedInterface->Phy.Path,PARAM_NAME_CONFIGURE_WAN,"true",ccsp_boolean);
        if (ret == ANSC_STATUS_FAILURE)
        {
            CcspTraceError(("%s WanMgr_RdkBus_SetRequestIfComponent failed for param %s%s\n",__FUNCTION__,pFixedInterface->Phy.Path,PARAM_NAME_CONFIGURE_WAN));
            // Deconfigure current selected interface if configure is failed.
            return STATE_WAN_DECONFIGURING_INTERFACE;
        }

    }
    if (pFixedInterface->CustomConfigEnable == TRUE)
    {
        ret = WanMgr_RdkBus_SetRequestIfComponent(pFixedInterface->CustomConfigPath,PARAM_NAME_CUSTOM_CONFIG_WAN,"true",ccsp_boolean);
        if (ret == ANSC_STATUS_FAILURE)
        {
            CcspTraceError(("%s WanMgr_RdkBus_SetRequestIfComponent failed for param %s%s\n",__FUNCTION__,pFixedInterface->CustomConfigPath,PARAM_NAME_CUSTOM_CONFIG_WAN));
            // Deconfigure current selected interface if configure is failed.
            return STATE_WAN_DECONFIGURING_INTERFACE;
        }

    }
    retState = Transition_WanInterfaceConfigured(pSmInfo);
    CcspTraceInfo(("%s %d - going to state %d \n", __FUNCTION__, __LINE__,retState));
    return retState;
}

static WcFmobPolicyState_t State_WanDeConfiguringInterface(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WcFmobPolicyState_t retState = STATE_WAN_SELECTING_INTERFACE;
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    DML_WAN_IFACE* pFixedInterface = NULL;
    ANSC_STATUS ret = ANSC_STATUS_SUCCESS;

    if (!pSmInfo)
        return STATE_WAN_SELECTING_INTERFACE;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_WAN_SELECTING_INTERFACE;
    }

    if (pWanController->WanEnable == FALSE)
    {
        return Transition_WanInterfaceTearDown(pWanController);
    }
    //Update ActiveLink
    pFixedInterface->Wan.ActiveLink = FALSE;
    CcspTraceInfo(("%s %d - AUTOWAN ifname %s \n", __FUNCTION__, __LINE__,pFixedInterface->Wan.Name));

    if (pFixedInterface->CustomConfigEnable == TRUE)
    {
        WanMgr_RdkBus_SetRequestIfComponent(pFixedInterface->CustomConfigPath,PARAM_NAME_CUSTOM_CONFIG_WAN,"false",ccsp_boolean);
        if (ret == ANSC_STATUS_FAILURE)
        {
            CcspTraceError(("%s WanMgr_RdkBus_SetRequestIfComponent failed for param %s%s\n",__FUNCTION__,pFixedInterface->CustomConfigPath,PARAM_NAME_CUSTOM_CONFIG_WAN));
        }
    }

    if (pFixedInterface->WanConfigEnabled == TRUE)
    {
        WanMgr_RdkBus_SetRequestIfComponent(pFixedInterface->Phy.Path,PARAM_NAME_CONFIGURE_WAN,"false",ccsp_boolean);
        if (ret == ANSC_STATUS_FAILURE)
        {
            CcspTraceError(("%s WanMgr_RdkBus_SetRequestIfComponent failed for param %s%s\n",__FUNCTION__,pFixedInterface->Phy.Path,PARAM_NAME_CONFIGURE_WAN));
        }
    }
    CcspTraceInfo(("%s %d - going to state %d \n", __FUNCTION__, __LINE__,retState));
    return retState;
}

static WcFmobPolicyState_t State_WanScanningInterface(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WcFmobPolicyState_t retState = STATE_WAN_SCANNING_INTERFACE;
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    DML_WAN_IFACE* pFixedInterface = NULL;
    bool wanActive = false;
    INT wanStatus = -1;

    if (!pSmInfo)
        return retState;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return retState;
    }

    if (pWanController->WanEnable == FALSE)
    {
        return Transition_WanInterfaceTearDown(pWanController);
    }
    if (pFixedInterface->MonitorOperStatus)
    {
        switch (pFixedInterface->Wan.OperationalStatus)
        {
            case WAN_OPERSTATUS_OPERATIONAL:
            {
                wanActive = true;
            }
            break;
            case WAN_OPERSTATUS_NOT_OPERATIONAL:
            {
                 retState = STATE_WAN_DECONFIGURING_INTERFACE;
            }
            break;
            default:
                break;
        }
    }
    else
    {
        if ((pFixedInterface->IP.Ipv4Status == WAN_IFACE_IPV4_STATE_UP)
                || (pFixedInterface->IP.Ipv6Status == WAN_IFACE_IPV6_STATE_UP))
        {
             wanActive = true;
        }
        else if ((pFixedInterface->IP.Ipv4Status == WAN_IFACE_IPV4_STATE_DOWN)
                    && (pFixedInterface->IP.Ipv6Status == WAN_IFACE_IPV6_STATE_DOWN))
        {
            retState = STATE_WAN_DECONFIGURING_INTERFACE;
        }
    }

    if (wanActive == true)
    {
        WanMgr_Policy_AutoWan_CfgPostWanSelection(pSmInfo);
        retState = Transition_WanInterfaceActive(pSmInfo);
    }
    return retState;
}

static WcFmobPolicyState_t State_WanInterfaceActive(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WcFmobPolicyState_t retState = STATE_WAN_INTERFACE_ACTIVE;
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if (!pSmInfo)
        return retState;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return retState;
    }

    if (pWanController->WanEnable == FALSE)
    {
        return Transition_WanInterfaceTearDown(pWanController);
    }
    if (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_DOWN)
    {
        pFixedInterface->Wan.Status = WAN_IFACE_STATUS_DISABLED;
        pFixedInterface->SelectionStatus = WAN_IFACE_NOT_SELECTED;
        //Update current active interface variable
	Update_Interface_Status();

	// wan stop
        wanmgr_setwanstop();
        retState = STATE_WAN_INTERFACE_DOWN;
        CcspTraceInfo(("%s %d - RetState %d GOING DOWN if_name %s \n", __FUNCTION__, __LINE__,retState,pFixedInterface->Wan.Name));
    }

    return retState;
}

static WcFmobPolicyState_t State_WanInterfaceUp(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WcFmobPolicyState_t retState = STATE_WAN_INTERFACE_UP;
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if (!pSmInfo)
        return retState;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return retState;
    }

    if (pWanController->WanEnable == FALSE)
    {
        return Transition_WanInterfaceTearDown(pWanController);
    }
    if(wanmgr_isWanStarted() == 1)
    {
        retState = Transition_WanInterfaceActive(pSmInfo);
    }
    return retState;
}

static WcFmobPolicyState_t State_WanInterfaceDown(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WcFmobPolicyState_t retState = STATE_WAN_INTERFACE_DOWN;
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if (!pSmInfo)
        return retState;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return retState;
    }

    if (pWanController->WanEnable == FALSE)
    {
        return Transition_WanInterfaceTearDown(pWanController);
    }

    if((pWanController->WanEnable == TRUE) &&
            (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_UP))
    {
        // start wan
        wanmgr_setwanstart();
        wanmgr_sshd_restart();
        retState = STATE_WAN_INTERFACE_UP;
        CcspTraceInfo(("%s %d - AUTOWAN ifname %s \n", __FUNCTION__, __LINE__,pFixedInterface->Wan.Name));
        CcspTraceInfo(("%s %d - RetState %d WAN INTERFACE UP PhyStatus %d  \n", __FUNCTION__, __LINE__,retState,pFixedInterface->Phy.Status));
    }
    return retState;
}


static WcFmobPolicyState_t State_WaitingForInterface(WanMgr_AutoWan_SMInfo_t *pSmInfo)
{
    WcFmobPolicyState_t retState = STATE_WAN_WAITING_FOR_INTERFACE;
    WanMgr_Policy_Controller_t    *pWanController = NULL;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if (!pSmInfo)
        return STATE_WAN_WAITING_FOR_INTERFACE;

    pWanController = &pSmInfo->wanPolicyCtrl;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_WAN_WAITING_FOR_INTERFACE;
    }

    if (pWanController->WanEnable == FALSE)
    {
        return Transition_WanInterfaceTearDown(pWanController);
    }
    if(pWanController->WanEnable == TRUE)
    {
        switch (pFixedInterface->Phy.Status)
        {
            case WAN_IFACE_PHY_STATUS_UP:
            {
                pFixedInterface->InterfaceScanStatus = WAN_IFACE_STATUS_SCANNED;
                if (pWanController->activeInterfaceIdx != pSmInfo->previousActiveInterfaceIndex)
                {
                    retState = STATE_WAN_CONFIGURING_INTERFACE;
                }
                else
                {
                    retState = Transition_WanInterfacePhyUp(pSmInfo);
                }
            }
            break;
            case WAN_IFACE_PHY_STATUS_DOWN:
            {
                 retState = STATE_WAN_DECONFIGURING_INTERFACE;
                 pFixedInterface->InterfaceScanStatus = WAN_IFACE_STATUS_SCANNED;
            }
            break;
            case WAN_IFACE_PHY_STATUS_UNKNOWN:
            {
                 /* ?? */
            }
            break;
            case WAN_IFACE_PHY_STATUS_INITIALIZING:
            {
                 /* ?? */
            }
            break;
        }
    }
    return retState;
}

ANSC_STATUS Wanmgr_WanAutoMode_StartStateMachine(void)
{
    CcspTraceInfo(("%s %d \n", __FUNCTION__, __LINE__));

    //policy variables
    ANSC_STATUS retStatus = ANSC_STATUS_SUCCESS;
    WanMgr_AutoWan_SMInfo_t smInfo = {0};
    smInfo.previousActiveInterfaceIndex = -1;
    WanMgr_Policy_Controller_t    *pWanPolicyCtrl = NULL;
    WcFmobPolicyState_t fmob_sm_state;
    bool bRunning = true;

    // event handler
    int n = 0;
    struct timeval tv;

    if(WanMgr_Controller_PolicyCtrlInit(&smInfo.wanPolicyCtrl) != ANSC_STATUS_SUCCESS)
    {
        CcspTraceError(("%s %d Policy Controller Error \n", __FUNCTION__, __LINE__));
        return ANSC_STATUS_FAILURE;
    }
    pWanPolicyCtrl = &smInfo.wanPolicyCtrl;
    pWanPolicyCtrl->WanOperationalMode = GetSelectedWanMode();
    pWanPolicyCtrl->activeInterfaceIdx = WanMgr_Policy_AutoWan_GetLastKnownModeInterfaceIndex();
    smInfo.previousActiveInterfaceIndex =  pWanPolicyCtrl->activeInterfaceIdx;
    CcspTraceInfo(("%s %d  Fixed Mode On Bootup Policy Thread Starting \n", __FUNCTION__, __LINE__));

    // initialise state machine
    fmob_sm_state = Transition_StartAuto(&smInfo); // do this first before anything else to init variables

    while (bRunning)
    {
        /* Wait up to 500 milliseconds */
        tv.tv_sec = 0;
        tv.tv_usec = LOOP_TIMEOUT;

         if ( 0 == access( "/tmp/timeout" , F_OK ) )
         {
             tv.tv_sec = 1;
             tv.tv_usec = 0;

         }
        n = select(0, NULL, NULL, NULL, &tv);
        if (n < 0)
        {
            /* interrupted by signal or something, continue */
            continue;
        }

        //Update Wan config
        WanMgr_Config_Data_t*   pWanConfigData = WanMgr_GetConfigData_locked();
        if(pWanConfigData != NULL)
        {
            pWanPolicyCtrl->WanEnable = pWanConfigData->data.Enable;
            WanMgrDml_GetConfigData_release(pWanConfigData);
        }

        //Lock Iface Data
        pWanPolicyCtrl->pWanActiveIfaceData = WanMgr_GetIfaceData_locked(pWanPolicyCtrl->activeInterfaceIdx);

        // process state
        switch (fmob_sm_state)
        {
            case STATE_WAN_SELECTING_INTERFACE:
            {
                fmob_sm_state = WanMgr_Policy_AutoWan_SelectAlternateInterface(&smInfo);
            }
            break;
            case STATE_WAN_WAITING_FOR_INTERFACE:
            {
                fmob_sm_state = State_WaitingForInterface(&smInfo);
            }
            break;
            case STATE_WAN_CONFIGURING_INTERFACE:
            {
                fmob_sm_state = State_WanConfiguringInterface(&smInfo);
            }
            break;
            case STATE_WAN_SCANNING_INTERFACE:
            {
                fmob_sm_state = State_WanScanningInterface(&smInfo);
            }
            break;
            case STATE_WAN_DECONFIGURING_INTERFACE:
            {
                fmob_sm_state = State_WanDeConfiguringInterface(&smInfo);
            }
            break;
            case STATE_WAN_INTERFACE_ACTIVE:
            {
                fmob_sm_state = State_WanInterfaceActive(&smInfo);
            }
            break;
            case STATE_WAN_INTERFACE_UP:
            {
                fmob_sm_state = State_WanInterfaceUp(&smInfo);
            }
            break;
            case STATE_WAN_INTERFACE_DOWN:
            {
                fmob_sm_state = State_WanInterfaceDown(&smInfo);
            }
            break;
            case STATE_WAN_INTERFACE_TEARDOWN:
            {
                fmob_sm_state = State_WanInterfaceTearDown(&smInfo.wanPolicyCtrl);
            }
            break;
            case STATE_WAN_EXIT:
            {
                bRunning = false;
            }
            break;
            default:
            {
                CcspTraceInfo(("%s %d - Case: default \n", __FUNCTION__, __LINE__));
                bRunning = false;
                retStatus = ANSC_STATUS_FAILURE;
                break;
            }
        }

        //Release Lock Iface Data
        if(pWanPolicyCtrl->pWanActiveIfaceData != NULL)
        {
            WanMgrDml_GetIfaceData_release(pWanPolicyCtrl->pWanActiveIfaceData);
        }
    }

    CcspTraceInfo(("%s %d - Exit from state machine\n", __FUNCTION__, __LINE__));
}

ANSC_STATUS Wanmgr_StartPrimaryWan(void)
{
    ANSC_STATUS retStatus = ANSC_STATUS_SUCCESS;

    SetLastKnownWanMode(WAN_MODE_PRIMARY);
    SetCurrentWanMode(WAN_MODE_PRIMARY);

    CcspTraceInfo(("Booting-Up in SelectedWanMode - %s\n",WanModeStr(GetSelectedWanMode())));
    retStatus = Wanmgr_WanFixedMode_StartStateMachine();

    return retStatus;
}

ANSC_STATUS Wanmgr_StartSecondaryWan(void)
{
    ANSC_STATUS retStatus = ANSC_STATUS_SUCCESS;

    SetLastKnownWanMode(WAN_MODE_SECONDARY);
    SetCurrentWanMode(WAN_MODE_SECONDARY);

    CcspTraceInfo(("Booting-Up in SelectedWanMode - %s\n",WanModeStr(GetSelectedWanMode())));
    retStatus = Wanmgr_WanFixedMode_StartStateMachine();

    return retStatus;
}

ANSC_STATUS Wanmgr_StartAutoMode(void)
{
    ANSC_STATUS retStatus = ANSC_STATUS_SUCCESS;

    CcspTraceInfo(("Auto WAN Mode is enabled, try Last known WAN mode\n"));
    retStatus = Wanmgr_WanAutoMode_StartStateMachine();

    return retStatus;
}

/* WanMgr_Policy_AutoWan */
ANSC_STATUS WanMgr_Policy_AutoWan(void)
{
    int wanMode = -1;
    ANSC_STATUS retStatus = ANSC_STATUS_SUCCESS;
    bool bRunning = true;

    CcspTraceInfo(("%s %d \n", __FUNCTION__, __LINE__));

#ifdef WAN_FAILOVER_SUPPORTED
    WanMgr_Policy_BackupWan( );
#endif /* WAN_FAILOVER_SUPPORTED */

    while (bRunning)
    {
        IntializeAutoWanConfig();

        wanMode = GetSelectedWanMode();
        CcspTraceInfo(("%s %d - SelWanMode %d  \n", __FUNCTION__, __LINE__,wanMode));

        switch (wanMode)
        {
            case WAN_MODE_PRIMARY:
            {
                Wanmgr_StartPrimaryWan();
            }
            break;

            case WAN_MODE_SECONDARY:
            {
                Wanmgr_StartSecondaryWan();
            }
            break;

            case WAN_MODE_AUTO:
            default:
            {
                if (wanMode != WAN_MODE_AUTO)
                {
                    SelectedWanMode(WAN_MODE_AUTO);
                }
                Wanmgr_StartAutoMode();
            }
            break;
        }
    }
    CcspTraceInfo(("%s %d - Exit from Auto wan policy\n", __FUNCTION__, __LINE__));
    return retStatus;
}

#ifdef WAN_FAILOVER_SUPPORTED
/*********************************************************************************/
/************************** BACK UP WAN ******************************************/
/*********************************************************************************/

/*********************************************************************************/
/************************** Local Utils ******************************************/
/*********************************************************************************/
static bool WanMgr_FirewallRuleConfig(char *Action, char *IfaceName)
{
    char command[BUFLEN_128]={0};
    char name[BUFLEN_64] = {0};
    bool ret = false;
    bool setRules = false;

    /* If Action is deconfigure
     * 1. Configure current_wan_ifname and wan_ifname event as erouter0 since we are reverting from backup
     * 2. Delete backup wan routing entry
     * 3. Firewall Restart
     */

    /* If Action is configure
     * 1. Configure current_wan_ifname and wan_ifname event
     * 2. Make routing entry
     * 3. Firewall Restart
     */
    /*TODO - Fix me, Need to review later  */
    sysevent_get(sysevent_fd, sysevent_token, SYSEVENT_CURRENT_WAN_IFNAME, name, sizeof(name));
    CcspTraceInfo(("%s-%d : CurrentIfaceName (%s) \n",__FUNCTION__, __LINE__, name));

    if ( (strcmp(Action, "configure") == 0) && ( (strlen(name) == 0) || (strcmp(name, IfaceName) != 0) ) )
    {
        setRules = true;
    }
    else if ( (strcmp(Action, "deconfigure") == 0) && ( (strlen(name) != 0) && (strcmp(name, IfaceName) == 0) ) )
    {
        setRules = true;
    }

    if (setRules)
    {
        snprintf(command, sizeof(command), "sh %s %s",BACKUP_WAN_DHCPC_SOURCE_FILE, Action);
        WanManager_DoSystemAction("TearDownBackupWAN:", command);

        CcspTraceInfo(("%s Cmd Str[%s]\n",__FUNCTION__,command));
        wanmgr_firewall_restart();
        ret = true;
    }

    return ret;
}

static int WanMgr_Policy_BackupWan_GetCurrentBackupWanInterfaceIndex( WanMgr_Policy_Controller_t* pWanController )
{
    INT  uiWanIdx      = 0;
    UINT uiTotalIfaces = -1;

    //Get uiTotalIfaces
    uiTotalIfaces = WanMgr_IfaceData_GetTotalWanIface();

    if(uiTotalIfaces > 0)
    {
        // Check the policy to determine if any primary interface should be used for WAN
        for(uiWanIdx = 0; uiWanIdx < uiTotalIfaces; ++uiWanIdx )
        {
            WanMgr_Iface_Data_t*   pWanDmlIfaceData = WanMgr_GetIfaceData_locked(uiWanIdx);
            if(pWanDmlIfaceData != NULL)
            {
                DML_WAN_IFACE* pInterface = NULL;

                pInterface = &(pWanDmlIfaceData->data);
                if(pInterface == NULL)
                {
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                    return -1;
                }

                /* If WAN Interface Type is REMOTE then we need to return that index to proceed further */
                if( ( TRUE == pInterface->Wan.Enable ) &&
                    ( REMOTE_IFACE == pInterface->Wan.IfaceType ) )
                {
                    CcspTraceInfo(("%s - Matched Index:%d Type:%d Enable:%d\n", __FUNCTION__, uiWanIdx, pInterface->Wan.IfaceType, pInterface->Wan.Enable));
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                    return uiWanIdx;
                }

                WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
            }
        }
    }

    return -1;
}

static int WanMgr_Policy_BackupWan_CheckAndStopUDHCPClientOverBackupWanInterface(char *ifname)
{
    if (access(BACKUP_WAN_DHCPC_PID_FILE, F_OK) == 0)
    {
        char cmd[128] = {0},
             output[16] = {0};
        int iPIDofBackupWAN = -1;

        snprintf(cmd, sizeof(cmd), "cat %s", BACKUP_WAN_DHCPC_PID_FILE);
        WanManager_Util_GetShell_output(cmd, output, sizeof(output));

        if( '\0' != output[0] )
        {
            iPIDofBackupWAN = atoi(output);

            if ( iPIDofBackupWAN > 0 )
            {
                CcspTraceInfo(("%s: DHCP client has already running as PID %d\n",__FUNCTION__,iPIDofBackupWAN));
                kill(iPIDofBackupWAN, SIGKILL);
                CcspTraceInfo(("%s: Stopped local udhcpc client for '%s' interface\n",__FUNCTION__,ifname));
            }
        }

        unlink(BACKUP_WAN_DHCPC_PID_FILE);
    }
}

static int WanMgr_Policy_BackupWan_CheckAndStartUDHCPClientOverBackupWanInterface(WanMgr_Policy_Controller_t* pWanController, WanStarCallSource_t enCallSource)
{
    int iPIDofBackupWAN = -1;
    unsigned char bPIDFileAvailable   = FALSE,
                  bDHCPRunningAlready = FALSE;
    DML_WAN_IFACE* pFixedInterface = NULL;
    char command[256] = {0},
         logPrefixString[256] = {0},
         udhcpcString[64] = {0};

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return -1;
    }

    //Log Purpose
    if( WAN_START_FOR_VALIDATION == enCallSource )
    {
        snprintf(logPrefixString, sizeof(logPrefixString), "Validating");
        snprintf(udhcpcString, sizeof(udhcpcString), "-s /etc/udhcpc_backupwan_random.script");
    }
    else
    {
        snprintf(logPrefixString, sizeof(logPrefixString), "Effecting");
        snprintf(udhcpcString, sizeof(udhcpcString), "-s %s",BACKUP_WAN_DHCPC_SOURCE_FILE);
    }

    //Cleanup - Stop UDHCPC client before start another instance
    WanMgr_Policy_BackupWan_CheckAndStopUDHCPClientOverBackupWanInterface(pFixedInterface->Wan.Name);

    CcspTraceInfo(("%s BackupWAN: Starting udhcpc client locally for '%s' interface\n",logPrefixString,pFixedInterface->Wan.Name));

    /* To start local udhcpc server over interface to check whether it is getting leases or not */
    memset(command, 0, sizeof(command));
    snprintf(command, sizeof(command), "/sbin/udhcpc -t 5 -n -i %s -p %s %s",pFixedInterface->Wan.Name, BACKUP_WAN_DHCPC_PID_FILE, udhcpcString);
    WanManager_DoSystemAction("StartingUDHCPCviaBackupWAN:", command);

    CcspTraceInfo(("%s BackupWAN: Cmd Str[%s]\n",logPrefixString,command));

    /* DHCP client didn't able to get Ipv4 configurations */
    if ( -1 == access(BACKUP_WAN_DHCPC_PID_FILE, F_OK) )
    {
        CcspTraceInfo(("%s BackupWAN: Backup WAN service not able to get IPv4 configuration in 5 lease try\n",logPrefixString));
        return -1;
    }
    else
    {
        CcspTraceInfo(("%s BackupWAN: Backup WAN interface '%s' got leases\n",logPrefixString,pFixedInterface->Wan.Name));
    }

    //Stop UDHCPC process after starting WAN
    if( WAN_START_FOR_VALIDATION == enCallSource )
    {
        //Cleanup - Stop UDHCPC local client after start checking
        WanMgr_Policy_BackupWan_CheckAndStopUDHCPClientOverBackupWanInterface(pFixedInterface->Wan.Name);
    }

    return 0;
}

static int WanMgr_Policy_BackupWan_CheckAnyLocalWANIsActive( WanMgr_Policy_Controller_t* pWanController )
{
    INT  uiWanIdx      = 0;
    UINT uiTotalIfaces = -1;

    //Get uiTotalIfaces
    uiTotalIfaces = WanMgr_IfaceData_GetTotalWanIface();

    if(uiTotalIfaces > 0)
    {
        // Check the policy to determine if any primary interface should be used for WAN
        for(uiWanIdx = 0; uiWanIdx < uiTotalIfaces; ++uiWanIdx )
        {
            WanMgr_Iface_Data_t*   pWanDmlIfaceData = WanMgr_GetIfaceData_locked(uiWanIdx);
            if(pWanDmlIfaceData != NULL)
            {
                DML_WAN_IFACE* pInterface = NULL;

                pInterface = &(pWanDmlIfaceData->data);
                if(pInterface == NULL)
                {
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                    return -1;
                }

                /* Needs to determine any local interface is active or not */
                if( ( LOCAL_IFACE == pInterface->Wan.IfaceType ) &&
                    ( TRUE == pInterface->Wan.Enable ) &&
                    ( WAN_IFACE_PHY_STATUS_UP == pInterface->Phy.Status ) &&
                    ( WAN_IFACE_LINKSTATUS_UP == pInterface->Wan.LinkStatus ) &&
                    ( WAN_IFACE_STATUS_UP == pInterface->Wan.Status ) )
                {
                    CcspTraceInfo(("%s: LOCAL WAN interface '%s' is available and ready, index '%d'\n",__FUNCTION__,pInterface->Wan.Name,uiWanIdx));
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                    return uiWanIdx;
                }

                WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
            }
        }
    }

    return -1;
}

static int WanMgr_Policy_BackupWan_CheckAnyLocalWANIsPhysicallyActive(void)
{
    INT  uiWanIdx      = 0;
    UINT uiTotalIfaces = -1;

    //Get uiTotalIfaces
    uiTotalIfaces = WanMgr_IfaceData_GetTotalWanIface();

    if(uiTotalIfaces > 0)
    {
        // Check the policy to determine if any primary interface should be used for WAN
        for(uiWanIdx = 0; uiWanIdx < uiTotalIfaces; ++uiWanIdx )
        {
            WanMgr_Iface_Data_t*   pWanDmlIfaceData = WanMgr_GetIfaceData_locked(uiWanIdx);
            if(pWanDmlIfaceData != NULL)
            {
                DML_WAN_IFACE* pInterface = NULL;

                pInterface = &(pWanDmlIfaceData->data);
                if(pInterface == NULL)
                {
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                    return -1;
                }

                /* Needs to determine any local interface is physically active or not */
                if( ( LOCAL_IFACE == pInterface->Wan.IfaceType ) &&
                    ( TRUE == pInterface->Wan.Enable ) &&
                    ( WAN_IFACE_PHY_STATUS_UP == pInterface->Phy.Status ) )
                {
                    //CcspTraceInfo(("%s: LOCAL WAN interface '%s' is physically available, index '%d'\n",__FUNCTION__,pInterface->Wan.Name,uiWanIdx));
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                    return uiWanIdx;
                }

                WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
            }
        }
    }

    return -1;
}

static unsigned char WanMgr_Policy_BackupWan_CheckLocalWANsScannedOnce( WanMgr_Policy_Controller_t* pWanController )
{
    INT  uiWanIdx      = 0;
    UINT uiTotalIfaces = -1,
         uiTotalLocalIfaces = 0,
         uiTotalScannedLocalIfaces = 0;
    INT selectedMode = GetSelectedWanMode();
    INT currentWANMode = GetCurrentWanMode();

    //Get uiTotalIfaces
    uiTotalIfaces = WanMgr_IfaceData_GetTotalWanIface();

    if(uiTotalIfaces > 0)
    {
        // Check the policy to determine if any primary interface should be used for WAN
        for(uiWanIdx = 0; uiWanIdx < uiTotalIfaces; ++uiWanIdx )
        {
            WanMgr_Iface_Data_t*   pWanDmlIfaceData = WanMgr_GetIfaceData_locked(uiWanIdx);
            if(pWanDmlIfaceData != NULL)
            {
                DML_WAN_IFACE* pInterface = NULL;

                pInterface = &(pWanDmlIfaceData->data);
                if(pInterface == NULL)
                {
                    WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                    return FALSE;
                }

                /* Needs to determine any local interface is physically active or not */
                if( LOCAL_IFACE == pInterface->Wan.IfaceType )
                {
                    uiTotalLocalIfaces++;

                    if( WAN_IFACE_STATUS_SCANNED == pInterface->InterfaceScanStatus )
                    {
                        //CcspTraceInfo(("%s: LOCAL WAN interface '%s' is scanned, index '%d'\n",__FUNCTION__,pInterface->Wan.Name,uiWanIdx));
                        uiTotalScannedLocalIfaces++;

                        if( ( WAN_MODE_PRIMARY == selectedMode ) &&
                            ( WAN_IFACE_TYPE_PRIMARY == pInterface->Wan.Type ) )
                        {
                            WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                            return TRUE;
                        }
                        else if( ( WAN_MODE_SECONDARY == selectedMode ) &&
                                 ( WAN_IFACE_TYPE_SECONDARY == pInterface->Wan.Type ) )
                        {
                            WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
                            return TRUE;
                        }
                    }
                }

                WanMgrDml_GetIfaceData_release(pWanDmlIfaceData);
            }
        }

        //Check whether all local interfaces are scanned at least once
        if( WAN_MODE_AUTO == selectedMode )
        {
            /*
             * We need to make sure already WAN selected or not. If choosed already then we need to return
             * TRUE
             */
            if ( WAN_MODE_UNKNOWN == currentWANMode )
            {
                if ( uiTotalScannedLocalIfaces == uiTotalLocalIfaces )
                {
                    return TRUE;
                }
            }
            else
            {
                return TRUE;
            }
        }
    }

    return FALSE;
}

/*********************************************************************************/
/************************** TRANSITIONS ******************************************/
/*********************************************************************************/
static WcBWanPolicyState_t Transition_StartBakupWan(WanMgr_Policy_Controller_t* pWanController)
{
    CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_SELECTING_INTERFACE \n", __FUNCTION__, __LINE__));
    return Transition_BackupWanSelectingInterface(pWanController);
}

static WcBWanPolicyState_t Transition_BackupWanSelectingInterface(WanMgr_Policy_Controller_t* pWanController)
{
    pWanController->activeInterfaceIdx = -1;

    CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_SELECTING_INTERFACE \n", __FUNCTION__, __LINE__));
    return STATE_BACKUP_WAN_SELECTING_INTERFACE;
}

static WcBWanPolicyState_t Transition_BackupWanInterfaceSelected(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return STATE_BACKUP_WAN_SELECTING_INTERFACE;
    }

    pFixedInterface->Wan.ActiveLink = FALSE;
    pFixedInterface->Wan.LinkStatus = WAN_IFACE_LINKSTATUS_DOWN;
    pFixedInterface->SelectionStatus = WAN_IFACE_NOT_SELECTED;
    pFixedInterface->IP.Ipv4Status = WAN_IFACE_IPV4_STATE_UNKNOWN;
    pFixedInterface->IP.Ipv6Status = WAN_IFACE_IPV6_STATE_UNKNOWN;
    pFixedInterface->Wan.Status = WAN_IFACE_STATUS_DISABLED;

    /* If WAN Interface Type is REMOTE then set Wan.Name to brRWAN */
    if( REMOTE_IFACE == pFixedInterface->Wan.IfaceType )
    {
        strncpy(pFixedInterface->Wan.Name, REMOTE_INTERFACE_NAME, sizeof(pFixedInterface->Wan.Name));
    }

    //Update current active interface variable
    Update_Interface_Status();

    CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_INTERFACE_DOWN \n", __FUNCTION__, __LINE__));
    return STATE_BACKUP_WAN_INTERFACE_DOWN;
}

static WcBWanPolicyState_t Transition_ValidatingBackupWanInterface(WanMgr_Policy_Controller_t* pWanController)
{
    CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_VALIDATING_INTERFACE \n", __FUNCTION__, __LINE__));
    return STATE_BACKUP_WAN_VALIDATING_INTERFACE;
}

static WcBWanPolicyState_t Transition_BackupWanAvailable(WanMgr_Policy_Controller_t* pWanController)
{
    CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_AVAILABLE \n", __FUNCTION__, __LINE__));

    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    //ActiveLink
    pFixedInterface->Wan.ActiveLink = TRUE;
    pFixedInterface->SelectionStatus = WAN_IFACE_SELECTED;
    pFixedInterface->Wan.Status =  WAN_IFACE_STATUS_DISABLED;

    //Update current active interface variable
    Update_Interface_Status();

    return STATE_BACKUP_WAN_AVAILABLE;
}

static WcBWanPolicyState_t Transition_BackupWanInterfaceUp(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    //Start wan over backup wan interface
    CcspTraceInfo(("%s %d - Starting WAN services\n", __FUNCTION__, __LINE__));
    WanMgr_Policy_BackupWan_CheckAndStopUDHCPClientOverBackupWanInterface(pFixedInterface->Wan.Name);

    pFixedInterface->Wan.LinkStatus = WAN_IFACE_LINKSTATUS_UP;
    pFixedInterface->Wan.Status = WAN_IFACE_STATUS_UP;
    Update_Interface_Status();

    //WAN UDHCPC Start
    WanMgr_Policy_BackupWan_CheckAndStartUDHCPClientOverBackupWanInterface( pWanController, WAN_START_FOR_BACKUP_WAN );

    CcspTraceInfo(("%s - Starting IP Monitoring for '%d' instance\n", __FUNCTION__, pWanController->activeInterfaceIdx + 1));
    WanMgr_StartIpMonitor(pWanController->activeInterfaceIdx + 1);

    CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_INTERFACE_UP \n", __FUNCTION__, __LINE__));
    return STATE_BACKUP_WAN_INTERFACE_UP;
}

static WcBWanPolicyState_t Transition_BackupWanInterfaceActive(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    pFixedInterface->SelectionStatus = WAN_IFACE_ACTIVE;
    pFixedInterface->Wan.Status =  WAN_IFACE_STATUS_UP;

    //Update current active interface variable
    Update_Interface_Status();

    if (!WanMgr_FirewallRuleConfig("configure", pFixedInterface->Wan.Name))
    {
        CcspTraceInfo(("%s-%d : Firewall Rules already configured \n",__FUNCTION__, __LINE__));
    }

    CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_INTERFACE_ACTIVE \n", __FUNCTION__, __LINE__));
    return STATE_BACKUP_WAN_INTERFACE_ACTIVE;
}

static WcBWanPolicyState_t Transition_BackupWanInterfaceInActive(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    pFixedInterface->SelectionStatus = WAN_IFACE_SELECTED;
    pFixedInterface->Wan.Status =  WAN_IFACE_STATUS_DISABLED;

    //Update current active interface variable
    Update_Interface_Status();

    if(-1 == WanMgr_Policy_BackupWan_CheckAnyLocalWANIsPhysicallyActive() )
    {
        CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_WAITING \n", __FUNCTION__, __LINE__));
        return STATE_BACKUP_WAN_WAITING;
    }
    else
    {
        if (!WanMgr_FirewallRuleConfig("deconfigure", pFixedInterface->Wan.Name))
        {
            CcspTraceInfo(("%s-%d : Firewall Rules already deconfigured \n",__FUNCTION__, __LINE__));
        }
    }

    CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_INTERFACE_INACTIVE \n", __FUNCTION__, __LINE__));
    return STATE_BACKUP_WAN_INTERFACE_INACTIVE;
}

static WcBWanPolicyState_t Transition_BackupWanInterfaceDown(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    // Reset Physical link status when state machine is teardown
    pFixedInterface->Wan.ActiveLink = FALSE;
    pFixedInterface->Wan.LinkStatus = WAN_IFACE_LINKSTATUS_DOWN;
    pFixedInterface->SelectionStatus = WAN_IFACE_NOT_SELECTED;
    pFixedInterface->Wan.Status = WAN_IFACE_STATUS_DISABLED;
    pFixedInterface->IP.Ipv4Status = WAN_IFACE_IPV4_STATE_UNKNOWN;
    pFixedInterface->IP.Ipv6Status = WAN_IFACE_IPV6_STATE_UNKNOWN;

    //Update current active interface variable
    Update_Interface_Status();

    if(-1 == WanMgr_Policy_BackupWan_CheckAnyLocalWANIsPhysicallyActive() )
    {
        CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_WAITING \n", __FUNCTION__, __LINE__));
        return STATE_BACKUP_WAN_WAITING;
    }
    else
    {
        if (!WanMgr_FirewallRuleConfig("deconfigure", pFixedInterface->Wan.Name))
        {
            CcspTraceInfo(("%s-%d : Firewall Rules already deconfigured \n",__FUNCTION__, __LINE__));
        }
    }

    CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_INTERFACE_DOWN \n", __FUNCTION__, __LINE__));
    return STATE_BACKUP_WAN_INTERFACE_DOWN;
}

static WcBWanPolicyState_t Transition_BackupWanInterfaceWaitingPrimaryUp(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if(pFixedInterface == NULL)
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    if (!WanMgr_FirewallRuleConfig("deconfigure", pFixedInterface->Wan.Name))
    {
        CcspTraceInfo(("%s-%d : Firewall Rules already deconfigured \n",__FUNCTION__, __LINE__));
    }

    if ( (pWanController->WanEnable == FALSE) ||
         (pWanController->AllowRemoteInterfaces == FALSE) ||
         (pFixedInterface->Wan.Enable == FALSE) ||
         (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_DOWN)||
         (pFixedInterface->Wan.LinkStatus != WAN_IFACE_LINKSTATUS_UP) ||
         (WAN_IFACE_STATUS_UP != pFixedInterface->Wan.Status) )
    {
        CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_INTERFACE_DOWN \n", __FUNCTION__, __LINE__));
        return STATE_BACKUP_WAN_INTERFACE_DOWN;
    }

    CcspTraceInfo(("%s %d - State changed to STATE_BACKUP_WAN_INTERFACE_INACTIVE \n", __FUNCTION__, __LINE__));
    return STATE_BACKUP_WAN_INTERFACE_INACTIVE;
}

/*********************************************************************************/
/**************************** STATES *********************************************/
/*********************************************************************************/
static WcBWanPolicyState_t State_SelectingBackupWanInterface(WanMgr_Policy_Controller_t* pWanController)
{
    if(pWanController == NULL)
    {
        return ANSC_STATUS_FAILURE;
    }

    if( ( pWanController->WanEnable == FALSE )  ||
        ( pWanController->AllowRemoteInterfaces == FALSE ) )
    {
        return STATE_BACKUP_WAN_SELECTING_INTERFACE;
    }

    pWanController->activeInterfaceIdx = WanMgr_Policy_BackupWan_GetCurrentBackupWanInterfaceIndex(pWanController);
    if(pWanController->activeInterfaceIdx != -1)
    {
        CcspTraceInfo(("%s - Backup WAN Interface index '%d' selected\n", __FUNCTION__,pWanController->activeInterfaceIdx));
        return Transition_BackupWanInterfaceSelected(pWanController);
    }

    return STATE_BACKUP_WAN_SELECTING_INTERFACE;
}

static WcBWanPolicyState_t State_BackupWanInterfaceDown(WanMgr_Policy_Controller_t* pWanController)
{
    int iLoopCount;
    INT iSelectWanIdx = -1;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if( ( pFixedInterface == NULL ) ||
        (pFixedInterface->Wan.Enable == FALSE) ||
        (pWanController->WanEnable == FALSE) ||
        ( pWanController->AllowRemoteInterfaces == FALSE ) )
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    if( (pWanController->WanEnable == TRUE) &&
        (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_UP) &&
        (pFixedInterface->Wan.Status == WAN_IFACE_STATUS_DISABLED) )
    {
        return Transition_ValidatingBackupWanInterface(pWanController);
    }

    return STATE_BACKUP_WAN_INTERFACE_DOWN;
}

static WcBWanPolicyState_t State_ValidatingBackupWanInterface(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if( ( pFixedInterface == NULL ) ||
        ( pWanController->AllowRemoteInterfaces == FALSE ) )
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    if ( (pWanController->WanEnable == FALSE) ||
         (pFixedInterface->Wan.Enable == FALSE) ||
         (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_DOWN) )
    {
        return Transition_BackupWanInterfaceDown(pWanController);
    }

    //Check if there is any IP leases is getting or not over Backup WAN
    if( 0 == WanMgr_Policy_BackupWan_CheckAndStartUDHCPClientOverBackupWanInterface( pWanController, WAN_START_FOR_VALIDATION ) )
    {
        return Transition_BackupWanAvailable(pWanController);
    }

    return STATE_BACKUP_WAN_VALIDATING_INTERFACE;
}

static WcBWanPolicyState_t State_BackupWanAvailable(WanMgr_Policy_Controller_t* pWanController)
{
    int iLoopCount;
    INT iSelectWanIdx = -1;
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if( pFixedInterface == NULL )
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    if ( (pWanController->WanEnable == FALSE) ||
         (pFixedInterface->Wan.Enable == FALSE) ||
         (pWanController->AllowRemoteInterfaces == FALSE) ||
         (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_DOWN) )
    {
        return Transition_BackupWanInterfaceDown(pWanController);
    }

    /*
     * Needs to wait till both LOCAL WANs are not active state.
     * Once we got if there is not LOCAL WANs then we need to proceed to use backup WAN
     * until any one of primary link detected
     */
    if ( (pWanController->WanEnable == TRUE) &&
         (pWanController->AllowRemoteInterfaces == TRUE) &&
         (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_UP) )
    {
        return Transition_BackupWanInterfaceUp(pWanController);
    }

    return STATE_BACKUP_WAN_AVAILABLE;
}

static WcBWanPolicyState_t State_BackupWanInterfaceUp(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if( pFixedInterface == NULL )
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    if ( (pWanController->WanEnable == FALSE) ||
         (pWanController->AllowRemoteInterfaces == FALSE) ||
         (pFixedInterface->Wan.Enable == FALSE) ||
         (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_DOWN)||
         (pFixedInterface->Wan.LinkStatus != WAN_IFACE_LINKSTATUS_UP) ||
         ( WAN_IFACE_STATUS_UP != pFixedInterface->Wan.Status ) )
    {
        return Transition_BackupWanInterfaceDown(pWanController);
    }

    if( ( WAN_IFACE_STATUS_UP == pFixedInterface->Wan.RemoteStatus ) && 
        ( -1 == WanMgr_Policy_BackupWan_CheckAnyLocalWANIsPhysicallyActive() ) && 
        ( TRUE == WanMgr_Policy_BackupWan_CheckLocalWANsScannedOnce( pWanController ) ) )
    {
        return Transition_BackupWanInterfaceActive(pWanController);
    }

    return STATE_BACKUP_WAN_INTERFACE_UP;
}

static WcBWanPolicyState_t State_BackupWanInterfaceActive(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if( pFixedInterface == NULL )
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    if ( (pWanController->WanEnable == FALSE) ||
         (pWanController->AllowRemoteInterfaces == FALSE) ||
         (pFixedInterface->Wan.Enable == FALSE) ||
         (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_DOWN)||
         (pFixedInterface->Wan.LinkStatus != WAN_IFACE_LINKSTATUS_UP) ||
         (WAN_IFACE_STATUS_UP != pFixedInterface->Wan.Status) )
    {
        return Transition_BackupWanInterfaceDown(pWanController);
    }

    if( ( WAN_IFACE_STATUS_UP != pFixedInterface->Wan.RemoteStatus ) ||
        (-1 != WanMgr_Policy_BackupWan_CheckAnyLocalWANIsPhysicallyActive() ) )
    {
        return Transition_BackupWanInterfaceInActive(pWanController);
    }

    return STATE_BACKUP_WAN_INTERFACE_ACTIVE;
}

static WcBWanPolicyState_t State_BackupWanInterfaceInActive(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if( pFixedInterface == NULL )
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    if ( (pWanController->WanEnable == FALSE) ||
         (pWanController->AllowRemoteInterfaces == FALSE) ||
         (pFixedInterface->Wan.Enable == FALSE) ||
         (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_DOWN)||
         (pFixedInterface->Wan.LinkStatus != WAN_IFACE_LINKSTATUS_UP) ||
         (WAN_IFACE_STATUS_UP != pFixedInterface->Wan.Status) )
    {
        return Transition_BackupWanInterfaceDown(pWanController);
    }

    if( ( WAN_IFACE_STATUS_UP == pFixedInterface->Wan.RemoteStatus ) && 
        (-1 == WanMgr_Policy_BackupWan_CheckAnyLocalWANIsPhysicallyActive() ) )
    {
        return Transition_BackupWanInterfaceActive(pWanController);
    }

    return STATE_BACKUP_WAN_INTERFACE_INACTIVE;
}

static WcBWanPolicyState_t State_BackupWanInterfaceWaitingPrimaryUp(WanMgr_Policy_Controller_t* pWanController)
{
    DML_WAN_IFACE* pFixedInterface = NULL;

    if((pWanController != NULL) && (pWanController->pWanActiveIfaceData != NULL))
    {
        pFixedInterface = &(pWanController->pWanActiveIfaceData->data);
    }

    if( ( pFixedInterface == NULL ) ||
        (pFixedInterface->Wan.Enable == FALSE) ||
        (pWanController->WanEnable == FALSE) ||
        ( pWanController->AllowRemoteInterfaces == FALSE ) )
    {
        return Transition_BackupWanSelectingInterface(pWanController);
    }

    if( (pWanController->WanEnable == TRUE) &&
        (pFixedInterface->Phy.Status == WAN_IFACE_PHY_STATUS_UP) &&
        (pFixedInterface->Wan.Status == WAN_IFACE_STATUS_DISABLED) )
    {
        return Transition_ValidatingBackupWanInterface(pWanController);
    }

    if(-1 != WanMgr_Policy_BackupWan_CheckAnyLocalWANIsPhysicallyActive() )
    {
        return Transition_BackupWanInterfaceWaitingPrimaryUp(pWanController);
    }

    if( ( WAN_IFACE_STATUS_UP == pFixedInterface->Wan.RemoteStatus ) &&
        (-1 == WanMgr_Policy_BackupWan_CheckAnyLocalWANIsPhysicallyActive() ) )
    {
        return Transition_BackupWanInterfaceActive(pWanController);
    }

    return STATE_BACKUP_WAN_WAITING;
}

ANSC_STATUS Wanmgr_BackupWan_StateMachineThread(void *arg)
{
    CcspTraceInfo(("%s %d \n", __FUNCTION__, __LINE__));

    //policy variables
    ANSC_STATUS retStatus = ANSC_STATUS_SUCCESS;
    WanMgr_Policy_Controller_t  WanPolicyCtrl = {0};
    WcBWanPolicyState_t fmob_sm_state;
    bool bRunning = true;

    // event handler
    int n = 0;
    struct timeval tv;

    CcspTraceInfo(("%s %d - Fixed Mode On Bootup Policy for Backup WAN Thread Starting\n", __FUNCTION__, __LINE__));

    //Initialise state machine
    memset(&WanPolicyCtrl, 0, sizeof(WanMgr_Policy_Controller_t));
    if(WanMgr_Controller_PolicyCtrlInit(&WanPolicyCtrl) != ANSC_STATUS_SUCCESS)
    {
        CcspTraceError(("%s %d Policy Controller Error \n", __FUNCTION__, __LINE__));
        return ANSC_STATUS_FAILURE;
    }

    fmob_sm_state = Transition_StartBakupWan(&WanPolicyCtrl);

    while (bRunning)
    {
        /* Wait up to 500 milliseconds */
        tv.tv_sec = 0;
        tv.tv_usec = LOOP_TIMEOUT;

        n = select(0, NULL, NULL, NULL, &tv);
        if (n < 0)
        {
            /* interrupted by signal or something, continue */
            continue;
        }

        //Update Wan config
        WanMgr_Config_Data_t  *pWanConfigData = WanMgr_GetConfigData_locked();
        if(pWanConfigData != NULL)
        {
            WanPolicyCtrl.WanEnable = pWanConfigData->data.Enable;
            WanPolicyCtrl.AllowRemoteInterfaces = pWanConfigData->data.AllowRemoteInterfaces;
            WanMgrDml_GetConfigData_release(pWanConfigData);
        }

        //Lock Iface Data
        WanPolicyCtrl.pWanActiveIfaceData = WanMgr_GetIfaceData_locked(WanPolicyCtrl.activeInterfaceIdx);

        // process state
        switch (fmob_sm_state)
        {
            case STATE_BACKUP_WAN_SELECTING_INTERFACE:
                fmob_sm_state = State_SelectingBackupWanInterface(&WanPolicyCtrl);
                break;
            case STATE_BACKUP_WAN_INTERFACE_DOWN:
                fmob_sm_state = State_BackupWanInterfaceDown(&WanPolicyCtrl);
                break;
            case STATE_BACKUP_WAN_VALIDATING_INTERFACE:
                fmob_sm_state = State_ValidatingBackupWanInterface(&WanPolicyCtrl);
                break;
            case STATE_BACKUP_WAN_AVAILABLE:
                fmob_sm_state = State_BackupWanAvailable(&WanPolicyCtrl);
                break;
            case STATE_BACKUP_WAN_INTERFACE_UP:
                fmob_sm_state = State_BackupWanInterfaceUp(&WanPolicyCtrl);
                break;
            case STATE_BACKUP_WAN_INTERFACE_ACTIVE:
                fmob_sm_state = State_BackupWanInterfaceActive(&WanPolicyCtrl);
                break;
            case STATE_BACKUP_WAN_INTERFACE_INACTIVE:
                fmob_sm_state = State_BackupWanInterfaceInActive(&WanPolicyCtrl);
                break;
            case STATE_BACKUP_WAN_WAITING:
                fmob_sm_state = State_BackupWanInterfaceWaitingPrimaryUp(&WanPolicyCtrl);
                break;
            case STATE_BACKUP_WAN_EXIT:
                bRunning = false;
                break;
            default:
                CcspTraceInfo(("%s %d - Case: default \n", __FUNCTION__, __LINE__));
                bRunning = false;
                retStatus = ANSC_STATUS_FAILURE;
                break;
        }

        //Release Lock Iface Data
        if(WanPolicyCtrl.pWanActiveIfaceData != NULL)
        {
            WanMgrDml_GetIfaceData_release(WanPolicyCtrl.pWanActiveIfaceData);
        }
    }

    CcspTraceInfo(("%s %d - Exit from state machine\n", __FUNCTION__, __LINE__));
}

ANSC_STATUS WanMgr_Policy_BackupWan(void)
{
    //Initiate the thread for Backup WAN policy
    pthread_create( &gBackupWanThread, NULL, Wanmgr_BackupWan_StateMachineThread, (void*)NULL);
    CcspTraceInfo(("%s %d - Backup WAN Thread Started\n", __FUNCTION__, __LINE__));
}
#endif /* WAN_FAILOVER_SUPPORTED */
