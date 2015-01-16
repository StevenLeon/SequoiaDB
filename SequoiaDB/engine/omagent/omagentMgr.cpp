/*******************************************************************************


   Copyright (C) 2011-2014 SequoiaDB Ltd.

   This program is free software: you can redistribute it and/or modify
   it under the term of the GNU Affero General Public License, version 3,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warrenty of
   MARCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU Affero General Public License for more details.

   You should have received a copy of the GNU Affero General Public License
   along with this program. If not, see <http://www.gnu.org/license/>.

   Source File Name = omagentMgr.cpp

   Dependencies: N/A

   Restrictions: N/A

   Change Activity:
   defect Date        Who Description
   ====== =========== === ==============================================
          04/15/2014  XJH Initial Draft

   Last Changed =

*******************************************************************************/


#include "omagentMgr.hpp"
#include "omagentSession.hpp"
#include "pmd.hpp"
#include "msgMessage.hpp"
#include "omagentUtil.hpp"

using namespace bson ;

namespace engine
{
   /*
      LOCAL DEFINE
   */
   #define OMAGENT_WAIT_CB_ATTACH_TIMEOUT             ( 300 * OSS_ONE_SEC )

   /*
      _omAgentOptions implement
   */
   _omAgentOptions::_omAgentOptions()
   {
      ossMemset( _dftSvcName, 0, sizeof( _dftSvcName ) ) ;
      ossMemset( _cmServiceName, 0, sizeof( _cmServiceName ) ) ;
      _restartCount        = -1 ;
      _restartInterval     = 0 ;
      _autoStart           = FALSE ;
      _isGeneralAgent      = FALSE ;
      _diagLevel           = PDWARNING ;

      ossMemset( _cfgFileName, 0, sizeof( _cfgFileName ) ) ;
      ossMemset( _localCfgPath, 0, sizeof( _localCfgPath ) ) ;
      ossMemset( _scriptPath, 0, sizeof( _scriptPath ) ) ;
      ossMemset( _startProcFile, 0, sizeof( _startProcFile ) ) ;
      ossMemset( _stopProcFile, 0, sizeof( _stopProcFile ) ) ;
      ossMemset( _omAddress, 0, sizeof( _omAddress ) ) ;

      ossSnprintf( _dftSvcName, OSS_MAX_SERVICENAME, "%u",
                   SDBCM_DFT_PORT ) ;

      _useCurUser = FALSE ;
   }

   _omAgentOptions::~_omAgentOptions()
   {
   }

   PDLEVEL _omAgentOptions::getDiagLevel() const
   {
      PDLEVEL level = PDWARNING ;
      if ( _diagLevel < PDSEVERE )
      {
         level = PDSEVERE ;
      }
      else if ( _diagLevel > PDDEBUG )
      {
         level = PDDEBUG ;
      }
      else
      {
         level= ( PDLEVEL )_diagLevel ;
      }
      return level ;
   }

   INT32 _omAgentOptions::init ( const CHAR *pRootPath )
   {
      INT32 rc = SDB_OK ;
      CHAR hostName[ OSS_MAX_HOSTNAME + 1 ] = { 0 } ;
      po::options_description desc ( "Command options" ) ;
      po::variables_map vm ;

      ossGetHostName( hostName, OSS_MAX_HOSTNAME ) ;

      _hostKey = hostName ;
      _hostKey += SDBCM_CONF_PORT ;

      PMD_ADD_PARAM_OPTIONS_BEGIN( desc )
         ( SDBCM_CONF_DFTPORT, po::value<string>(),
         "sdbcm default listening port" )
         ( _hostKey.c_str(), po::value<string>(),
         "sdbcm specified listening port" )
         ( SDBCM_RESTART_COUNT, po::value<INT32>(),
         "sequoiadb node restart max count" )
         ( SDBCM_RESTART_INTERVAL, po::value<INT32>(),
         "sequoiadb node restart time interval" )
         ( SDBCM_AUTO_START, po::value<string>(),
         "start sequoiadb node automatically when CM start" )
         ( SDBCM_DIALOG_LEVEL, po::value<INT32>(),
         "Dialog level" )
         ( SDBCM_CONF_OMADDR, po::value<string>(),
         "OM address" )
         ( SDBCM_CONF_ISGENERAL, po::value<string>(),
         "Is general agent" )
      PMD_ADD_PARAM_OPTIONS_END

      if ( !pRootPath )
      {
         rc = SDB_INVALIDARG ;
         goto error ;
      }

      rc = utilBuildFullPath( pRootPath, SDBCM_LOCAL_PATH, OSS_MAX_PATHSIZE,
                              _localCfgPath ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Root path is too long: %s", pRootPath ) ;
         goto error ;
      }

      rc = utilBuildFullPath( pRootPath, SDBOMA_SCRIPT_PATH, OSS_MAX_PATHSIZE,
                              _scriptPath ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Root path is too long: %s", pRootPath ) ;
         goto error ;
      }

      rc = utilBuildFullPath ( pRootPath, SDBSTARTPROG, OSS_MAX_PATHSIZE,
                               _startProcFile ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Root path is too long: %s", pRootPath ) ;
         goto error ;
      }

      rc = utilBuildFullPath ( pRootPath, SDBSTOPPROG, OSS_MAX_PATHSIZE,
                               _stopProcFile ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Root path is too long: %s", pRootPath ) ;
         goto error ;
      }

      rc = utilBuildFullPath( pRootPath, SDBCM_CONF_PATH_FILE,
                              OSS_MAX_PATHSIZE, _cfgFileName ) ;
      if ( rc )
      {
         PD_LOG ( PDERROR, "Root path is too long: %s", pRootPath ) ;
         goto error ;
      }

      rc = utilReadConfigureFile( _cfgFileName, desc, vm ) ;
      if ( rc )
      {
         if ( SDB_FNE == rc )
         {
            PD_LOG( PDWARNING, "Config[%s] not exist, use default config",
                    _cfgFileName ) ;
            rc = postLoaded() ;
            goto done ;
         }
         PD_LOG( PDERROR, "Failed to read config from file[%s], rc: %d",
                 _cfgFileName, rc ) ;
         goto error ;
      }

      rc = pmdCfgRecord::init( &vm, NULL ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to init config record, rc: %d", rc ) ;
         goto error ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omAgentOptions::save()
   {
      INT32 rc = SDB_OK ;
      std::string line ;

      rc = pmdCfgRecord::toString( line ) ;
      if ( SDB_OK != rc )
      {
         PD_LOG( PDERROR, "Failed to get the line str:%d", rc ) ;
         goto error ;
      }

      rc = utilWriteConfigFile( _cfgFileName, line.c_str(), FALSE ) ;
      PD_RC_CHECK( rc, PDERROR, "Failed to write config[%s], rc: %d",
                   _cfgFileName, rc ) ;

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omAgentOptions::doDataExchange( pmdCfgExchange * pEX )
   {
      resetResult () ;

      pEX->setCfgStep( PMD_CFG_STEP_INIT ) ;


      rdxString( pEX, SDBCM_CONF_DFTPORT , _dftSvcName,
                 sizeof( _dftSvcName ), FALSE, FALSE,
                 _dftSvcName ) ;
      rdxString( pEX, _hostKey.c_str(), _cmServiceName,
                 sizeof( _cmServiceName ), FALSE, FALSE,
                 _dftSvcName ) ;
      rdxInt( pEX, SDBCM_RESTART_COUNT, _restartCount, FALSE, TRUE,
              _restartCount ) ;
      rdxInt( pEX, SDBCM_RESTART_INTERVAL, _restartInterval, FALSE, TRUE,
              _restartInterval ) ;
      rdxBooleanS( pEX, SDBCM_AUTO_START, _autoStart, FALSE, TRUE,
                   _autoStart ) ;
      rdxInt( pEX, SDBCM_DIALOG_LEVEL, _diagLevel, FALSE, TRUE,
              _diagLevel ) ;
      rdxString( pEX, SDBCM_CONF_OMADDR, _omAddress, sizeof( _omAddress ),
                 FALSE, FALSE, "", FALSE ) ;
      rdxBooleanS( pEX, SDBCM_CONF_ISGENERAL, _isGeneralAgent, FALSE,
                   FALSE, FALSE, FALSE ) ;


      return getResult () ;
   }

   INT32 _omAgentOptions::postLoaded()
   {
      INT32 rc = SDB_OK ;

      rc = ossMkdir( getLocalCfgPath(), OSS_CREATE|OSS_READWRITE ) ;
      if ( rc && SDB_FE != rc )
      {
         PD_LOG( PDERROR, "Failed to create dir: %s, rc: %d",
                 getLocalCfgPath(), rc ) ;
         goto error ;
      }
      rc = SDB_OK ;

      if ( 0 != _omAddress[ 0 ] )
      {
         rc = parseAddressLine( _omAddress, _vecOMAddr ) ;
         PD_RC_CHECK( rc, PDERROR, "Parse om address[%s] failed, rc: %d",
                      _omAddress, rc ) ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omAgentOptions::preSaving()
   {
      string omAddrLine = makeAddressLine( _vecOMAddr ) ;
      ossStrncpy( _omAddress, omAddrLine.c_str(), OSS_MAX_PATHSIZE ) ;
      _omAddress[ OSS_MAX_PATHSIZE ] = 0 ;

      return SDB_OK ;
   }

   void _omAgentOptions::addOMAddr( const CHAR * host, const CHAR * service )
   {
      if ( _vecOMAddr.size() < CLS_REPLSET_MAX_NODE_SIZE )
      {
         pmdAddrPair addr ;
         ossStrncpy( addr._host, host, OSS_MAX_HOSTNAME ) ;
         addr._host[ OSS_MAX_HOSTNAME ] = 0 ;
         ossStrncpy( addr._service, service, OSS_MAX_SERVICENAME ) ;
         addr._service[ OSS_MAX_SERVICENAME ] = 0 ;
         _vecOMAddr.push_back( addr ) ;

         if ( !_isGeneralAgent &&
              0 == ossStrcmp( host, pmdGetKRCB()->getHostName() ) )
         {
            _isGeneralAgent = TRUE ;
         }

         string str = makeAddressLine( _vecOMAddr ) ;
         ossStrncpy( _omAddress, str.c_str(), OSS_MAX_PATHSIZE ) ;
         _omAddress[ OSS_MAX_PATHSIZE ] = 0 ;
      }
   }

   void _omAgentOptions::delOMAddr( const CHAR * host, const CHAR * service )
   {
      BOOLEAN removed = FALSE ;
      _isGeneralAgent = FALSE ;
      vector< pmdAddrPair >::iterator it = _vecOMAddr.begin() ;
      while ( it != _vecOMAddr.end() )
      {
         pmdAddrPair &addr = *it ;
         if ( 0 == ossStrcmp( host, addr._host ) &&
              0 == ossStrcmp( service, addr._service ) )
         {
            it = _vecOMAddr.erase( it ) ;
            removed = TRUE ;
            continue ;
         }
         if ( !_isGeneralAgent &&
              0 == ossStrcmp( addr._host, pmdGetKRCB()->getHostName() ) )
         {
            _isGeneralAgent = TRUE ;
         }
         ++it ;
      }

      if ( removed )
      {
         string str = makeAddressLine( _vecOMAddr ) ;
         ossStrncpy( _omAddress, str.c_str(), OSS_MAX_PATHSIZE ) ;
         _omAddress[ OSS_MAX_PATHSIZE ] = 0 ;
      }
   }

   void _omAgentOptions::setCMServiceName( const CHAR * serviceName )
   {
      if ( serviceName && *serviceName )
      {
         ossStrncpy( _cmServiceName, serviceName, OSS_MAX_SERVICENAME ) ;
         _cmServiceName[ OSS_MAX_SERVICENAME ] = 0 ;
      }
   }

   void _omAgentOptions::lock( INT32 type )
   {
      if ( SHARED == type )
      {
         _latch.get_shared() ;
      }
      else
      {
         _latch.get() ;
      }
   }

   void _omAgentOptions::unLock( INT32 type )
   {
      if ( SHARED == type )
      {
         _latch.release_shared() ;
      }
      else
      {
         _latch.release() ;
      }
   }

   /*
      _omAgentSessionMgr implement
   */
   _omAgentSessionMgr::_omAgentSessionMgr()
   {
   }

   _omAgentSessionMgr::~_omAgentSessionMgr()
   {
   }

   UINT64 _omAgentSessionMgr::makeSessionID( const NET_HANDLE & handle,
                                             const MsgHeader * header )
   {
      return ossPack32To64( PMD_BASE_HANDLE_ID + handle, header->TID ) ;
   }

   SDB_SESSION_TYPE _omAgentSessionMgr::_prepareCreate( UINT64 sessionID,
                                                        INT32 startType,
                                                        INT32 opCode )
   {
      return SDB_SESSION_OMAGENT ;
   }

   BOOLEAN _omAgentSessionMgr::_canReuse( SDB_SESSION_TYPE sessionType )
   {
      return FALSE ;
   }

   UINT32 _omAgentSessionMgr::_maxCacheSize() const
   {
      return 0 ;
   }

   void _omAgentSessionMgr::_onPushMsgFailed( INT32 rc, const MsgHeader *pReq,
                                              const NET_HANDLE &handle,
                                              pmdAsyncSession *pSession )
   {
      _reply( handle, rc, pReq ) ;
   }

   pmdAsyncSession* _omAgentSessionMgr::_createSession( SDB_SESSION_TYPE sessionType,
                                                        INT32 startType,
                                                        UINT64 sessionID,
                                                        void * data )
   {
      pmdAsyncSession *pSession = NULL ;

      if ( SDB_SESSION_OMAGENT == sessionType )
      {
         pSession = SDB_OSS_NEW omaSession( sessionID ) ;
      }
      else
      {
         PD_LOG( PDERROR, "Invalid session type[%d]", sessionType ) ;
      }

      return pSession ;
   }

   /*
      omAgentMgr Message MAP
   */
   BEGIN_OBJ_MSG_MAP( _omAgentMgr, _pmdObjBase )
      ON_MSG( MSG_BS_QUERY_RES, _onOMQueryTaskRes )
   END_OBJ_MSG_MAP()

   /*
      omAgentMgr implement
   */
   _omAgentMgr::_omAgentMgr()
   : _msgHandler( &_sessionMgr ),
     _timerHandler( &_sessionMgr ),
     _netAgent( &_msgHandler )
   {
      _oneSecTimer         = NET_INVALID_TIMER_ID ;
      _nodeMonitorTimer    = NET_INVALID_TIMER_ID ;
      _watchAndCleanTimer  = NET_INVALID_TIMER_ID ;
      _primaryPos          = -1 ;
      _taskID              = 0 ;
   }

   _omAgentMgr::~_omAgentMgr()
   {
   }

   SDB_CB_TYPE _omAgentMgr::cbType() const
   {
      return SDB_CB_OMAGT ;
   }

   const CHAR* _omAgentMgr::cbName() const
   {
      return "OMAGENT" ;
   }

   INT32 _omAgentMgr::init()
   {
      INT32 rc = SDB_OK ;
      const CHAR *hostName = pmdGetKRCB()->getHostName() ;
      const CHAR *cmService = _options.getCMServiceName() ;
      MsgRouteID nodeID ;

      _initOMAddr( _vecOmNode ) ;
      if ( _vecOmNode.size() > 0 )
      {
         _primaryPos = 0 ;
      }
      else
      {
         _primaryPos = -1 ;
      }

      if ( _options.isGeneralAgent() )
      {
         pmdGetKRCB()->setBusinessOK( FALSE ) ;
         startTaskCheck( BSON( FIELD_NAME_HOST <<
                               pmdGetKRCB()->getHostName() ) ) ;
      }

      nodeID.columns.groupID = OMAGENT_GROUPID ;
      nodeID.columns.nodeID = 1 ;
      nodeID.columns.serviceID = MSG_ROUTE_LOCAL_SERVICE ;
      _netAgent.updateRoute( nodeID, hostName, cmService ) ;
      rc = _netAgent.listen( nodeID ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Create listen[ServiceName: %s] failed, rc: %d",
                 cmService, rc ) ;
         goto error ;
      }
      PD_LOG ( PDEVENT, "Create listen[ServiceName:%s] succeed",
               cmService ) ;

      rc = _sessionMgr.init( &_netAgent, &_timerHandler, OSS_ONE_SEC ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Init session manager failed, rc: %d", rc ) ;
         goto error ;
      }

      rc = _nodeMgr.init() ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Init node manager failed, rc: %d", rc ) ;
         goto error ;
      }

      rc = _sptScopePool.init() ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Init container failed, rc: %d", rc ) ;
         goto error ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   void _omAgentMgr::_initOMAddr( vector< MsgRouteID > &vecNode )
   {
      MsgRouteID nodeID ;
      MsgRouteID srcID ;
      UINT16 omNID = 1 ;
      _netRoute *pRoute = _netAgent.getRoute() ;
      INT32 rc = SDB_OK ;

      vector< _pmdOptionsMgr::_pmdAddrPair > omAddrs = _options.omAddrs() ;
      for ( UINT32 i = 0 ; i < omAddrs.size() ; ++i )
      {
         if ( 0 == omAddrs[i]._host[ 0 ] )
         {
            break ;
         }
         nodeID.columns.groupID = OM_GROUPID ;
         nodeID.columns.nodeID = omNID++ ;
         nodeID.columns.serviceID = MSG_ROUTE_OM_SERVICE ;

         if ( SDB_OK == pRoute->route( omAddrs[ i ]._host,
                                       omAddrs[ i ]._service,
                                       MSG_ROUTE_OM_SERVICE,
                                       srcID ) )
         {
            rc = _netAgent.updateRoute( srcID, nodeID ) ;
         }
         else
         {
            rc = _netAgent.updateRoute( nodeID, omAddrs[ i ]._host,
                                        omAddrs[ i ]._service ) ;
         }

         if ( SDB_OK == rc )
         {
            vecNode.push_back( nodeID ) ;
         }
      }
   }

   INT32 _omAgentMgr::active()
   {
      INT32 rc = SDB_OK ;
      pmdEDUMgr *pEDUMgr = pmdGetKRCB()->getEDUMgr() ;
      EDUID eduID = PMD_INVALID_EDUID ;

      pmdSetPrimary( TRUE ) ;

      rc = _nodeMgr.active() ;
      PD_RC_CHECK( rc, PDERROR, "Active node manager failed, rc: %d", rc ) ;

      rc = pEDUMgr->startEDU( EDU_TYPE_OMMGR, (_pmdObjBase*)this, &eduID ) ;
      PD_RC_CHECK( rc, PDERROR, "Failed to start OM Manager edu, rc: %d", rc ) ;
      pEDUMgr->regSystemEDU( EDU_TYPE_OMMGR, eduID ) ;
      rc = _attachEvent.wait( OMAGENT_WAIT_CB_ATTACH_TIMEOUT ) ;
      PD_RC_CHECK( rc, PDERROR, "Wait OM Manager edu attach failed, rc: %d",
                   rc ) ;

      rc = pEDUMgr->startEDU( EDU_TYPE_OMNET, (netRouteAgent*)&_netAgent,
                              &eduID ) ;
      PD_RC_CHECK( rc, PDERROR, "Failed to start om net, rc: %d", rc ) ;
      pEDUMgr->regSystemEDU( EDU_TYPE_OMNET, eduID ) ;

      rc = _netAgent.addTimer( OSS_ONE_SEC, &_timerHandler, _oneSecTimer ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to set timer, rc: %d", rc ) ;
         goto error ;
      }

      rc = _netAgent.addTimer( 2 * OSS_ONE_SEC, &_timerHandler,
                               _nodeMonitorTimer ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to set timer, rc: %d", rc ) ;
         goto error ;
      }
      rc = _netAgent.addTimer( 120 * OSS_ONE_SEC, &_timerHandler,
                               _watchAndCleanTimer ) ;
      if ( rc )
      {
         PD_LOG( PDERROR, "Failed to set timer, rc: %d", rc ) ;
         goto error ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omAgentMgr::deactive()
   {
      iPmdProc::stop( 0 ) ;

      if ( NET_INVALID_TIMER_ID != _oneSecTimer )
      {
         _netAgent.removeTimer( _oneSecTimer ) ;
         _oneSecTimer = NET_INVALID_TIMER_ID ;
      }

      _netAgent.closeListen() ;

      _netAgent.stop() ;

      _sessionMgr.setForced() ;

      return SDB_OK ;
   }

   INT32 _omAgentMgr::fini()
   {
      _nodeMgr.fini() ;
      _sessionMgr.fini() ;
      _sptScopePool.fini() ;

      return SDB_OK ;
   }

   void _omAgentMgr::attachCB( _pmdEDUCB * cb )
   {
      _msgHandler.attach( cb ) ;
      _timerHandler.attach( cb ) ;
      _attachEvent.signalAll() ;
   }

   void _omAgentMgr::detachCB( _pmdEDUCB * cb )
   {
      _msgHandler.detach() ;
      _timerHandler.detach() ;
   }

   void _omAgentMgr::onConfigChange()
   {
      vector< MsgRouteID >::iterator it ;
      BOOLEAN bFound = FALSE ;
      vector< MsgRouteID > vecNodes ;
      _initOMAddr( vecNodes ) ;

      ossScopedLock lock( &_mgrLatch, EXCLUSIVE ) ;

      it = _vecOmNode.begin() ;
      while ( it != _vecOmNode.end() )
      {
         bFound = FALSE ;
         for ( UINT32 i = 0 ; i < vecNodes.size() ; ++i )
         {
            if ( vecNodes[ i ].value == (*it).value )
            {
               bFound = TRUE ;
               break ;
            }
         }

         if ( !bFound )
         {
            _netAgent.delRoute( *it ) ;
         }
         ++it ;
      }
      _vecOmNode = vecNodes ;
      if ( _vecOmNode.size() > 0 )
      {
         _primaryPos = 0 ;
      }
      else
      {
         _primaryPos = -1 ;
      }
   }

   INT32 _omAgentMgr::_prepareTask()
   {
      INT32 rc = SDB_OK ;
      ossScopedLock lock ( &_mgrLatch, SHARED ) ;
      MAPTASKQUERY::iterator it = _mapTaskQuery.begin () ;
      while ( it != _mapTaskQuery.end() )
      {
         rc = _sendQueryTaskReq ( it->first, OM_CS_DEPLOY_CL_TASKINFO,
                                  &(it->second) ) ;
         if ( SDB_OK != rc )
         {
            break ;
         }
         ++it ;
      }
      return rc ;
   }

   INT32 _omAgentMgr::_sendQueryTaskReq ( UINT64 requestID,
                                          const CHAR * clFullName,
                                          const BSONObj* match )
   {
      CHAR *pBuff = NULL ;
      INT32 buffSize = 0 ;
      MsgHeader *msg = NULL ;
      INT32 rc = SDB_OK ;

      rc = msgBuildQueryMsg ( &pBuff, &buffSize, clFullName, 0, requestID,
                              0, -1, match, NULL, NULL, NULL ) ;
      if ( SDB_OK != rc )
      {
         goto error ;
      }
      msg = ( MsgHeader* )pBuff ;
      msg->TID = 0 ;
      msg->routeID.value = 0 ;

      rc = sendToOM( msg ) ;
      PD_LOG ( PDDEBUG, "Send query[%s] to om[rc:%d]",
               match->toString().c_str(), rc ) ;

   done:
      if ( pBuff )
      {
         SDB_OSS_FREE ( pBuff ) ;
         pBuff = NULL ;
      }
      return rc ;
   error:
      goto done ;
   }

   void _omAgentMgr::onTimer( UINT64 timerID, UINT32 interval )
   {
      if ( _oneSecTimer == timerID )
      {
         _sessionMgr.onTimer( interval ) ;

         _prepareTask() ;
      }
      else if ( _nodeMonitorTimer == timerID )
      {
         _nodeMgr.monitorNodes() ;
      }
      else if ( _watchAndCleanTimer == timerID )
      {
         _nodeMgr.watchManualNodes() ;
         _nodeMgr.cleanDeadNodes() ;
      }
   }

   omAgentOptions* _omAgentMgr::getOptions()
   {
      return &_options ;
   }

   omAgentNodeMgr* _omAgentMgr::getNodeMgr()
   {
      return &_nodeMgr ;
   }

   netRouteAgent* _omAgentMgr::getRouteAgent()
   {
      return &_netAgent ;
   }

   sptContainer* _omAgentMgr::getSptScopePool()
   {
      return &_sptScopePool ;
   }

   sptScope* _omAgentMgr::getScope()
   {
      return _sptScopePool.newScope() ;
   }

   void _omAgentMgr::releaseScope( sptScope * pScope )
   {
      _sptScopePool.releaseScope( pScope ) ;
   }

   INT32 _omAgentMgr::sendToOM( MsgHeader * msg, INT32 * pSendNum )
   {
      INT32 rc = SDB_OK ;

      if ( pSendNum )
      {
         *pSendNum = 0 ;
      }

      ossScopedLock lock( &_mgrLatch, SHARED ) ;

      INT32 tmpPrimary = _primaryPos ;

      if ( _vecOmNode.size() == 0 )
      {
         rc = SDB_SYS ;
         goto error ;
      }

      if ( tmpPrimary >= 0 && (UINT32)tmpPrimary < _vecOmNode.size() )
      {
         rc = _netAgent.syncSend ( _vecOmNode[tmpPrimary],
                                   (void*)msg ) ;
         if ( rc != SDB_OK )
         {
            PD_LOG ( PDWARNING,
                     "Send message to primary om[%d] failed[rc:%d]",
                     _vecOmNode[tmpPrimary].columns.nodeID,
                     rc ) ;
            _primaryPos = -1 ;
         }
         else
         {
            if ( pSendNum )
            {
               *pSendNum = 1 ;
            }
            goto done ;
         }
      }

      {
         UINT32 index = 0 ;
         INT32 rc1 = SDB_OK ;
         rc = SDB_NET_SEND_ERR ;

         while ( index < _vecOmNode.size () )
         {
            rc1 = _netAgent.syncSend ( _vecOmNode[index], (void*)msg ) ;
            if ( rc1 == SDB_OK )
            {
               rc = rc1 ;
               if ( pSendNum )
               {
                  ++(*pSendNum) ;
               }
            }
            else
            {
               PD_LOG ( PDWARNING, "Send message to om[%d] failed[rc:%d]. "
                        "It is possible because the remote service was not "
                        "started yet",
                        _vecOmNode[index].columns.nodeID,
                        rc1 ) ;
            }

            index++ ;
         }
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   INT32 _omAgentMgr::startTaskCheck( const BSONObj & match )
   {
      ossScopedLock lock ( &_mgrLatch, EXCLUSIVE ) ;
      _mapTaskQuery[++_taskID] = match.copy() ;

      return SDB_OK ;
   }

   INT32 _omAgentMgr::_onOMQueryTaskRes ( NET_HANDLE handle, MsgHeader *msg )
   {
      MsgOpReply *res = ( MsgOpReply* )msg ;
      PD_LOG ( PDDEBUG, "Recieve omsvc query task response[requestID:%lld, "
               "flag: %d]", msg->requestID, res->flags ) ;

      INT32 rc = SDB_OK ;
      INT32 flag = 0 ;
      INT64 contextID = -1 ;
      INT32 startFrom = 0 ;
      INT32 numReturned = 0 ;
      vector<BSONObj> objList ;

      if ( SDB_DMS_EOC == res->flags ||
           SDB_CAT_TASK_NOTFOUND == res->flags )
      {
         _mgrLatch.get() ;
         _mapTaskQuery.erase ( msg->requestID ) ;
         _mgrLatch.release() ;
         PD_LOG ( PDINFO, "The query task[%lld] has 0 jobs", msg->requestID ) ;
      }
      else if ( SDB_OK != res->flags )
      {
         PD_LOG ( PDERROR, "Query task[%lld] failed[rc=%d]",
                  msg->requestID, res->flags ) ;
         goto error ;
      }
      else
      {
         rc = msgExtractReply ( (CHAR *)msg, &flag, &contextID, &startFrom,
                                &numReturned, objList ) ;
         if ( SDB_OK != rc )
         {
            PD_LOG ( PDERROR, "Failed to extract task infos from omsvc, "
                     "rc = %d", rc ) ;
            goto error ;
         }
         {
            ossScopedLock lock ( &_mgrLatch, EXCLUSIVE ) ;
            MAPTASKQUERY::iterator it = _mapTaskQuery.find ( msg->requestID ) ;
            if ( it == _mapTaskQuery.end() )
            {
               PD_LOG ( PDWARNING, "The query task response[%lld] is not exist",
                        msg->requestID ) ;
               rc = SDB_INVALIDARG ;
               goto error ;
            }
            _mapTaskQuery.erase ( it ) ;
         }

         PD_LOG ( PDINFO, "The query task[%lld] has %d jobs", msg->requestID,
                  numReturned ) ;

         {
            UINT32 index = 0 ;
            UINT64 taskID = 0 ;
            while ( index < objList.size() )
            {
               BSONObj tmpObj = objList[index].getOwned() ;
               BSONElement e = tmpObj.getField( OM_TASKINFO_FIELD_TASKID ) ;
               if ( !e.isNumber() )
               {
                  PD_LOG( PDERROR, "Get taskid from obj[%s] failed",
                          tmpObj.toString().c_str() ) ;
                  ++index ;
                  continue ;
               }
               taskID = (UINT64)e.numberLong() ;

               if ( !isTaskInfoExist( taskID ) )
               {
                  if ( SDB_OK == _startTask ( tmpObj ) )
                  {
                     registerTaskInfo( taskID, tmpObj ) ;
                  }
               }
               ++index ;
            }
         }
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   BOOLEAN _omAgentMgr::isTaskInfoExist( UINT64 taskID )
   {
      ossScopedLock lock( &_mgrLatch, SHARED ) ;
      MAP_TASKINFO::iterator it = _mapTaskInfo.find( taskID ) ;
      if ( it == _mapTaskInfo.end() )
      {
         return FALSE ;
      }
      return TRUE ;
   }

   void _omAgentMgr::registerTaskInfo( UINT64 taskID, const BSONObj &obj )
   {
      ossScopedLock lock( &_mgrLatch, EXCLUSIVE ) ;
      _mapTaskInfo[ taskID ] = obj.getOwned() ;
   }

   void _omAgentMgr::submitTaskInfo( UINT64 taskID )
   {
      ossScopedLock lock( &_mgrLatch, EXCLUSIVE ) ;

      MAP_TASKINFO::iterator it = _mapTaskInfo.find( taskID ) ;
      if ( it != _mapTaskInfo.end() )
      {
         _mapTaskInfo.erase( it ) ;
      }

      if ( _mapTaskInfo.size() == 0 && !pmdGetKRCB()->isBusinessOK() )
      {
         pmdGetKRCB()->setBusinessOK( TRUE ) ;
      }
   }

   INT32 _omAgentMgr::_startTask( const BSONObj &obj )
   {
      INT32 rc = SDB_OK ;
      INT32 taskType = OMA_TASK_UNKNOW ;
      EDUID eduID = PMD_INVALID_EDUID ;
      BSONObj data ;

      try
      {
         rc = omaGetIntElement( obj, OMA_FIELD_TASKTYPE, taskType ) ;
         PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                   "Get field[%s] failed, rc: %d", OMA_FIELD_TASKTYPE, rc ) ;
         rc = omaGetObjElement( obj, OMA_FIELD_DETAIL, data ) ;
         PD_CHECK( SDB_OK == rc, rc, error, PDERROR,
                   "Get field[%s] failed, rc: %d", OMA_FIELD_DETAIL, rc ) ;
      }
      catch ( std::exception &e )
      {
         PD_LOG ( PDERROR, "omagent start task exception: %s", e.what() ) ;
         rc = SDB_SYS ;
         goto error ;
      }

      switch ( taskType )
      {
         case OMA_TASK_ADD_HOST :
            rc = startAddHostTaskJob( data.objdata(), &eduID ) ;
            if ( rc )
            {
               PD_LOG( PDERROR, "Failed to start add hosts task "
                       "rc = %d", rc ) ;
               goto error ;
            }
            break ;
         case OMA_TASK_INSTALL_DB :
            rc = startInsDBBusTaskJob( data.objdata(), &eduID ) ;
            if ( rc )
            {
               PD_LOG( PDERROR, "Failed to start install db business task "
                       "rc = %d", rc ) ;
               goto error ;
            }
            break ;
         case OMA_TASK_REMOVE_DB :
            rc = startRmDBBusTaskJob( data.objdata(), &eduID ) ;
            if ( rc )
            {
               PD_LOG( PDERROR, "Failed to start add hosts task "
                       "rc = %d", rc ) ;
               goto error ;
            }
            break ;
         default :
            PD_LOG ( PDERROR, "Unknow task type[%d]", taskType ) ;
            rc = SDB_INVALIDARG ;
            break ;
      }

   done:
      return rc ;
   error:
      goto done ;
   }

   /*
      get the global om manager object point
   */
   omAgentMgr *sdbGetOMAgentMgr()
   {
      static omAgentMgr s_omagent ;
      return &s_omagent ;
   }

   omAgentOptions* sdbGetOMAgentOptions()
   {
      return sdbGetOMAgentMgr()->getOptions() ;
   }

}


