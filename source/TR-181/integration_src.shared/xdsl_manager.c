/*
   If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2020 RDK Management
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

/**************************************************************************

    module: xdsl_manager.c

        For COSA Data Model Library Development

    -------------------------------------------------------------------

    description:

        State machine to manage a DSL interface.

    -------------------------------------------------------------------

    environment:

        platform independent

    -------------------------------------------------------------------

    revision:

        13/08/2020    initial revision.

**************************************************************************/

/* ---- Include Files ---------------------------------------- */
#include "xdsl_apis.h"
#include <unistd.h>
#include <pthread.h>

#define LOOP_TIMEOUT                50000 // timeout in milliseconds. This is the state machine loop interval

typedef enum {
    STATE_EXIT = 0,
    STATE_DISCONNECTED,
    STATE_TRAINING,
    STATE_XTM_CONFIGURING,
    STATE_WAN_LINK_UP
} dslSmState_t;

/* ---- Global Variables ------------------------------------ */
pthread_key_t       sm_private_key;

/* ---- Private Function Prototypes -------------------------- */
/* STATES */
static dslSmState_t StateDisconnected( PXDSL_SM_PRIVATE_INFO pstPrivInfo );                    // waits for DSL to be physically connected.
static dslSmState_t StateTraining( PXDSL_SM_PRIVATE_INFO pstPrivInfo );                        // waits for DSL to train up.
static dslSmState_t StateXtmConfiguring( PXDSL_SM_PRIVATE_INFO pstPrivInfo );                     // waits for XTM interfaces to be configured.
static dslSmState_t StateWanLinkUp( PXDSL_SM_PRIVATE_INFO pstPrivInfo );                              // monitors the DSL interface.

/* TRANSITIONS */
static dslSmState_t TransitionStart( void );                         // initiliases the state machine.
static dslSmState_t TransitionTraining( PXDSL_SM_PRIVATE_INFO pstPrivInfo );                   // starts training DSL.
static dslSmState_t TransitionTrained( PXDSL_SM_PRIVATE_INFO pstPrivInfo );                    // starts configuring XTM.
static dslSmState_t TransitionWanLinkUp( PXDSL_SM_PRIVATE_INFO pstPrivInfo );                     // sets up the dsl interface.
static dslSmState_t TransitionPhyInterfaceDown( PXDSL_SM_PRIVATE_INFO pstPrivInfo );                   // tears down the dsl interface.
static dslSmState_t TransitionExit( PXDSL_SM_PRIVATE_INFO pstPrivInfo );                         //Exit from state machine

static void* DslStateMachineThread( void *arg );

/* ***************************************************************************************** */

/* XdslManager_Start_StateMachine() */
void XdslManager_Start_StateMachine( PXDSL_SM_PRIVATE_INFO pstMPrivateInfo )
{
    PXDSL_SM_PRIVATE_INFO     pstPInfo       = NULL;
    pthread_t                dslThreadId;
    int                      iErrorCode     = 0;
    static int               siKeyCreated   = 0;

    //Need to create pthread key at once
    if( 0 == siKeyCreated )
    {
        if ( 0 != pthread_key_create( &sm_private_key, NULL ) ) 
        {
          CcspTraceError(("%s %d Unable to create pthread_key\n", __FUNCTION__, __LINE__));
          return;
        }

        siKeyCreated = 1;
    }

    //Allocate memory and pass it to thread
    pstPInfo = ( PXDSL_SM_PRIVATE_INFO )malloc( sizeof( XDSL_SM_PRIVATE_INFO ) );
    if( NULL == pstPInfo )
    {
        CcspTraceError(("%s %d Failed to allocate memory\n", __FUNCTION__, __LINE__));
        return;
    }

    //Copy buffer
    memcpy( pstPInfo, pstMPrivateInfo, sizeof( XDSL_SM_PRIVATE_INFO ) ); 
 
    //DSL state machine thread
    iErrorCode = pthread_create( &dslThreadId, NULL, &DslStateMachineThread, (void*)pstPInfo );

    if( 0 != iErrorCode )
    {
        CcspTraceInfo(("%s %d - Failed to start DSL State Machine Thread EC:%d\n", __FUNCTION__, __LINE__, iErrorCode ));
    }
    else
    {
        CcspTraceInfo(("%s %d - DSL State Machine Thread Started Successfully\n", __FUNCTION__, __LINE__ ));
    }
}

/* DslStateMachineThread() */
static void* DslStateMachineThread( void *arg )
{
    dslSmState_t currentSmState   = STATE_EXIT;

    // event handler
    int n=0;
    struct timeval tv;

    //Validate buffer
    if ( NULL == arg )
    {
       CcspTraceError(("%s %d Invalid buffer\n", __FUNCTION__,__LINE__));
       
       //Cleanup current thread when exit
       pthread_exit(NULL);
    }

    //detach thread from caller stack
    pthread_detach(pthread_self());

    //Set thread specific private data
    if ( 0 != pthread_setspecific( sm_private_key, arg ) )
    {
       CcspTraceError(("%s %d Unable to set private key so exiting\n", __FUNCTION__, __LINE__));
       if( NULL != arg )
       {
           free( arg );
           arg = NULL;
       }

       //Cleanup current thread when exit
       pthread_exit(NULL);
    }

    // local variables
    bool bRunning = true;

    // initialise state machine
    currentSmState = TransitionStart(); // do this first before anything else to init variables

    while (bRunning)
    {
        PXDSL_SM_PRIVATE_INFO  pstPrivInfo = NULL;

        /* Wait up to 500 milliseconds */
        tv.tv_sec = 0;
        tv.tv_usec = LOOP_TIMEOUT;

        n = select(0, NULL, NULL, NULL, &tv);
        if (n < 0)
        {
            /* interrupted by signal or something, continue */
            continue;
        }

        //Get thread specific data
        pstPrivInfo = ( PXDSL_SM_PRIVATE_INFO ) pthread_getspecific( sm_private_key );

        // process state
        switch (currentSmState)
        {
            case STATE_DISCONNECTED:
                {
                    currentSmState = StateDisconnected( pstPrivInfo );
                    break;
                }

            case STATE_TRAINING:
                {
                    currentSmState = StateTraining( pstPrivInfo );
                    break;
                }

            case STATE_XTM_CONFIGURING:
                {
                    currentSmState = StateXtmConfiguring( pstPrivInfo );
                    break;
                }

            case STATE_WAN_LINK_UP:
                {
                    currentSmState = StateWanLinkUp( pstPrivInfo );
                    break;
                }

            case STATE_EXIT:
            default:
            {
                //Free current private resource before exit
                if( NULL != pstPrivInfo )
                {
                    free(pstPrivInfo);
                    pstPrivInfo = NULL;

                }

                bRunning = false;

                CcspTraceInfo(("%s %d - Exit from state machine\n", __FUNCTION__, __LINE__));
                break; 
            } 

        }
    }

    //Cleanup current thread when exit
    pthread_exit(NULL);

    return NULL;
}

static dslSmState_t StateDisconnected( PXDSL_SM_PRIVATE_INFO pstPrivInfo )
{
    DML_XDSL_LINE_GLOBALINFO stGlobalInfo = { 0 };

    //Get current DSL link status
    DmlXdslLineGetCopyOfGlobalInfoForGivenIfName( pstPrivInfo->Name, &stGlobalInfo );

    if( FALSE == stGlobalInfo.Upstream )
    {
        return TransitionExit( pstPrivInfo );
    }
    else if( ( XDSL_LINK_STATUS_Up == stGlobalInfo.LinkStatus ) && ( XDSL_LINE_WAN_DOWN == stGlobalInfo.WanStatus ) )
    {
        return TransitionTrained( pstPrivInfo );
    }
    else if( ( XDSL_LINK_STATUS_Up == stGlobalInfo.LinkStatus ) && ( XDSL_LINE_WAN_UP == stGlobalInfo.WanStatus ) )
    {
        return TransitionWanLinkUp( pstPrivInfo );
    }
    else if ( ( XDSL_LINK_STATUS_Initializing == stGlobalInfo.LinkStatus ) ||
              ( XDSL_LINK_STATUS_EstablishingLink == stGlobalInfo.LinkStatus ) )
    {
        return TransitionTraining( pstPrivInfo );
    }

    return STATE_DISCONNECTED;
}

static dslSmState_t StateTraining( PXDSL_SM_PRIVATE_INFO pstPrivInfo )
{
    DML_XDSL_LINE_GLOBALINFO stGlobalInfo = { 0 };

    //Get current DSL link status
    DmlXdslLineGetCopyOfGlobalInfoForGivenIfName( pstPrivInfo->Name, &stGlobalInfo );

    if( XDSL_LINK_STATUS_Up == stGlobalInfo.LinkStatus )
    {
        return TransitionTrained( pstPrivInfo );
    }
    else if ( ( FALSE == stGlobalInfo.Upstream ) ||
              ( XDSL_LINK_STATUS_NoSignal == stGlobalInfo.LinkStatus ) ||
              ( XDSL_LINK_STATUS_Disabled == stGlobalInfo.LinkStatus ) ||
              ( XDSL_LINK_STATUS_Error == stGlobalInfo.LinkStatus ) )
    {
        return TransitionPhyInterfaceDown( pstPrivInfo );
    }

    return STATE_TRAINING;
}

static dslSmState_t StateXtmConfiguring( PXDSL_SM_PRIVATE_INFO pstPrivInfo )
{
    DML_XDSL_LINE_GLOBALINFO stGlobalInfo = { 0 }; 

    //Get current DSL link status
    DmlXdslLineGetCopyOfGlobalInfoForGivenIfName( pstPrivInfo->Name, &stGlobalInfo );

    if( XDSL_LINE_WAN_UP == stGlobalInfo.WanStatus )
    {
        return TransitionWanLinkUp( pstPrivInfo );
    }
    else if ( ( FALSE == stGlobalInfo.Upstream ) || 
              ( XDSL_LINK_STATUS_Up != stGlobalInfo.LinkStatus ) )
    {
        return TransitionPhyInterfaceDown( pstPrivInfo );
    }

    return STATE_XTM_CONFIGURING;
}

static dslSmState_t StateWanLinkUp( PXDSL_SM_PRIVATE_INFO pstPrivInfo )
{
    DML_XDSL_LINE_GLOBALINFO stGlobalInfo = { 0 }; 

    //Get current DSL link status
    DmlXdslLineGetCopyOfGlobalInfoForGivenIfName( pstPrivInfo->Name, &stGlobalInfo );

    if ( ( FALSE == stGlobalInfo.Upstream ) || 
         ( XDSL_LINE_WAN_DOWN == stGlobalInfo.WanStatus ) ||  
         ( XDSL_LINK_STATUS_Up != stGlobalInfo.LinkStatus ) )
    {
        return TransitionPhyInterfaceDown( pstPrivInfo );
    }

    return STATE_WAN_LINK_UP;
}

static dslSmState_t TransitionStart( void )
{
    CcspTraceInfo(("%s - %s:DSL state start\n",__FUNCTION__,XDSL_MARKER_SM_TRANSITION));

    return STATE_DISCONNECTED;
}

static dslSmState_t TransitionTraining( PXDSL_SM_PRIVATE_INFO pstPrivInfo )
{
    CcspTraceInfo(("%s - %s:IfName:%s STATE_TRAINING\n",__FUNCTION__,XDSL_MARKER_SM_TRANSITION,pstPrivInfo->Name));

    return STATE_TRAINING;
}

static dslSmState_t TransitionTrained( PXDSL_SM_PRIVATE_INFO pstPrivInfo )
{
    /*
     *   1. Notify to XTM to create and enable interface link
     */

    if ( ANSC_STATUS_SUCCESS != DmlXdslCreateXTMLink( pstPrivInfo->Name ) )
    {
        CcspTraceError(("%s Failed to create XTM link\n", __FUNCTION__));
    }

    CcspTraceInfo(("%s - %s:IfName:%s STATE_XTM_CONFIGURING\n",__FUNCTION__,XDSL_MARKER_SM_TRANSITION,pstPrivInfo->Name));

    return STATE_XTM_CONFIGURING;
}

static dslSmState_t TransitionWanLinkUp( PXDSL_SM_PRIVATE_INFO pstPrivInfo )
{
    /*
     *   1. Notify to WAN for Up event
     */

    if ( ANSC_STATUS_SUCCESS != DmlXdslSetWanLinkStatusForWanManager( pstPrivInfo->Name, "Up" ) )
    {
        CcspTraceError(("%s Failed to set LinkUp to WAN\n", __FUNCTION__)); 
    }

    CcspTraceInfo(("%s - %s:IfName:%s STATE_WAN_LINK_UP\n",__FUNCTION__,XDSL_MARKER_SM_TRANSITION,pstPrivInfo->Name));

    return STATE_WAN_LINK_UP;
}

static dslSmState_t TransitionPhyInterfaceDown( PXDSL_SM_PRIVATE_INFO pstPrivInfo )
{
    /* 
     *   1. Notify to XTM to disable and delete interface link
     *   2. Notify to WAN for Down event
     */
    if ( ANSC_STATUS_SUCCESS != DmlXdslDeleteXTMLink( pstPrivInfo->Name ) )
    {
        CcspTraceError(("%s Failed to delete XTM link\n", __FUNCTION__));
    }

    if ( ANSC_STATUS_SUCCESS != DmlXdslSetWanLinkStatusForWanManager( pstPrivInfo->Name, "Down" ) )
    {
        CcspTraceError(("%s Failed to set LinkDown to WAN\n", __FUNCTION__));
    }

    CcspTraceInfo(("%s - %s:IfName:%s STATE_DISCONNECTED\n",__FUNCTION__,XDSL_MARKER_SM_TRANSITION,pstPrivInfo->Name));

    return STATE_DISCONNECTED;
}

static dslSmState_t TransitionExit( PXDSL_SM_PRIVATE_INFO pstPrivInfo ) 
{
    /* 
     *  1. Exit fro state machine
     */

    CcspTraceInfo(("%s - %s:IfName:%s STATE_EXIT\n",__FUNCTION__,XDSL_MARKER_SM_TRANSITION,pstPrivInfo->Name));

    return STATE_EXIT;
}