/* -*- C++ -*- */

/****************************************************************************
** Copyright (c) 2001-2014
**
** This file is part of the QuickFIX FIX Engine
**
** This file may be distributed under the terms of the quickfixengine.org
** license as defined by quickfixengine.org and appearing in the file
** LICENSE included in the packaging of this file.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
** See http://www.quickfixengine.org/LICENSE for licensing information.
**
** Contact ask@quickfixengine.org if any conditions of this licensing are
** not clear to you.
**
****************************************************************************/

#ifndef FIX_SESSION_H
#define FIX_SESSION_H

#ifdef _MSC_VER
#pragma warning( disable : 4503 4355 4786 4290 )
#endif

#include "SessionState.h"
#include "TimeRange.h"
#include "SessionID.h"
#include "Responder.h"
#include "Fields.h"
#include "DataDictionaryProvider.h"
#include "Application.h"
#include "Mutex.h"
#include "Log.h"

#include <atomic>
#include <utility>
#include <map>
#include <queue>
#include <functional>

namespace FIX
{
/// Maintains the state and implements the logic of a %FIX %session.
class Session
{
public:
  Session( std::function<UtcTimeStamp()> timestamper,
           Application&, 
           MessageStoreFactory&,
           const SessionID&,
           const DataDictionaryProvider&,
           const TimeRange&,
           int heartBtInt, 
           LogFactory* pLogFactory );
  virtual ~Session();

  void logon() 
  { m_state.enabled( true ); m_state.logoutReason( "" ); }
  void logout( const std::string& reason = "" ) 
  { m_state.enabled( false ); m_state.logoutReason( reason ); }
  bool isEnabled() 
  { return m_state.enabled(); }

  bool sentLogon() { return m_state.sentLogon(); }
  bool sentLogout() { return m_state.sentLogout(); }
  bool receivedLogon() { return m_state.receivedLogon(); }
  bool isLoggedOn() { return receivedLogon() && sentLogon(); }
  void reset() EXCEPT ( IOException ) 
  { generateLogout(); disconnect(); m_state.reset( m_timestamper() ); }
  void refresh() EXCEPT ( IOException )
  { m_state.refresh(); }
  void setNextSenderMsgSeqNum( int num ) EXCEPT ( IOException )
  { m_state.setNextSenderMsgSeqNum( num ); }
  void setNextTargetMsgSeqNum( int num ) EXCEPT ( IOException )
  { m_state.setNextTargetMsgSeqNum( num ); }

  const SessionID& getSessionID() const
  { return m_sessionID; }
  void setDataDictionaryProvider( const DataDictionaryProvider& dataDictionaryProvider )
  { m_dataDictionaryProvider = dataDictionaryProvider; }
  const DataDictionaryProvider& getDataDictionaryProvider() const
  { return m_dataDictionaryProvider; }

  static bool sendToTarget( Message& message,
                            const std::string& qualifier = "" )
  EXCEPT ( SessionNotFound );
  static bool sendToTarget( Message& message, const SessionID& sessionID )
  EXCEPT ( SessionNotFound );
  static bool sendToTarget( Message&,
                            const SenderCompID& senderCompID,
                            const TargetCompID& targetCompID,
                            const std::string& qualifier = "" )
  EXCEPT ( SessionNotFound );
  static bool sendToTarget( Message& message,
                            const std::string& senderCompID,
                            const std::string& targetCompID,
                            const std::string& qualifier = "" )
  EXCEPT ( SessionNotFound );

  static std::set<SessionID> getSessions();
  static bool doesSessionExist( const SessionID& );
  static Session* lookupSession( const SessionID& );
  static Session* lookupSession( const std::string&, bool reverse = false );
  static bool isSessionRegistered( const SessionID& );
  static Session* registerSession( const SessionID& );
  static void unregisterSession( const SessionID& );

  static size_t numSessions();


  bool isSessionTime( const UtcTimeStamp& now )
    { return m_sessionTime.isInRange( now ); }
  bool isLogonTime( const UtcTimeStamp& now )
    { return m_logonTime.isInRange( now ); }
  bool isInitiator()
    { return m_state.initiate(); }
  bool isAcceptor()
    { return !m_state.initiate(); }

  const TimeRange& getLogonTime()
    { return m_logonTime; }
  void setLogonTime( const TimeRange& value )
    { m_logonTime = value; }

  const std::string& getSenderDefaultApplVerID()
    { return m_senderDefaultApplVerID; }
  void setSenderDefaultApplVerID( const std::string& senderDefaultApplVerID )
    { m_senderDefaultApplVerID = senderDefaultApplVerID; }

  const std::string& getTargetDefaultApplVerID()
    { return m_targetDefaultApplVerID; }
  void setTargetDefaultApplVerID( const std::string& targetDefaultApplVerID )
    { m_targetDefaultApplVerID = targetDefaultApplVerID; }

  bool getSendRedundantResendRequests()
    { return m_sendRedundantResendRequests; }
  void setSendRedundantResendRequests ( bool value )
    { m_sendRedundantResendRequests = value; } 

  bool getCheckCompId()
    { return m_checkCompId; }
  void setCheckCompId ( bool value )
    { m_checkCompId = value; }

  bool getCheckLatency()
    { return m_checkLatency; }
  void setCheckLatency ( bool value )
    { m_checkLatency = value; }

  int getMaxLatency()
    { return m_maxLatency; }
  void setMaxLatency ( int value )
    { m_maxLatency = value; }

  int getLogonTimeout()
    { return m_state.logonTimeout(); }
  void setLogonTimeout ( int value )
    { m_state.logonTimeout( value ); }

  // KRX fork (P0-3, D11): how long one message may take to send before the connection is dropped:
  // SendTimeout if set, otherwise half the heartbeat interval (15 s while that is still unknown).
  int getSendTimeoutMs();
  // KRX fork (P0-3): the heartbeat interval in seconds; 0 while an acceptor awaits the Logon.
  int getHeartBtInt();
  void setSendTimeoutMs ( int value )
    { m_sendTimeoutMs = value; }

  int getLogoutTimeout()
    { return m_state.logoutTimeout(); }
  void setLogoutTimeout ( int value )
    { m_state.logoutTimeout( value ); }

  bool getResetOnLogon()
    { return m_resetOnLogon; }
  void setResetOnLogon ( bool value )
    { m_resetOnLogon = value; }

  bool getResetOnLogout()
    { return m_resetOnLogout; }
  void setResetOnLogout ( bool value )
    { m_resetOnLogout = value; }

  bool getResetOnDisconnect()
    { return m_resetOnDisconnect; }
  void setResetOnDisconnect( bool value )
    { m_resetOnDisconnect = value; }

  bool getRefreshOnLogon()
    { return m_refreshOnLogon; }
  void setRefreshOnLogon( bool value )
    { m_refreshOnLogon = value; } 

  bool getMillisecondsInTimeStamp()
    { return (m_timestampPrecision == 3); }
  void setMillisecondsInTimeStamp ( bool value )
    { if (value)
        m_timestampPrecision = 3;
      else
        m_timestampPrecision = 0;
    }
  int getTimestampPrecision()
    { return m_timestampPrecision; }
  void setTimestampPrecision(int precision)
    {
      if (precision < 0 || precision > 9)
        return;

      m_timestampPrecision = precision;
    }
  int getSupportedTimestampPrecision() 
    {
      return supportsSubSecondTimestamps(m_sessionID.getBeginString()) ? m_timestampPrecision : 0;
    }
  static bool supportsSubSecondTimestamps(const std::string &beginString) 
  {
    if( beginString == BeginString_FIXT11 )
      return true;
    else
      return beginString >= BeginString_FIX42;
  }
    

  bool getPersistMessages()
    { return m_persistMessages; }
  void setPersistMessages ( bool value )
    { m_persistMessages = value; }

  bool getValidateLengthAndChecksum()
    { return m_validateLengthAndChecksum; }
  void setValidateLengthAndChecksum ( bool value )
    { m_validateLengthAndChecksum = value; }

  bool getSendNextExpectedMsgSeqNum()
    { return m_sendNextExpectedMsgSeqNum; }
  void setSendNextExpectedMsgSeqNum ( bool value )
    { m_sendNextExpectedMsgSeqNum = value; }

  bool getIsNonStopSession() const
    { return m_isNonStopSession; }
  void setIsNonStopSession ( bool value )
    { m_isNonStopSession = value; }

  void setResponder( Responder* pR )
  {
    // KRX fork (P0-11): under m_mutex too, which a sender holds while it writes through the
    // responder, so the old one is never cleared (and deleted) mid-write.
    const Locker l( m_mutex );
    if (m_refreshOnLogon)
      refresh();
    if( !checkSessionTime(m_timestamper()) )
      reset();
    const Locker r( m_responderMutex );
    m_pResponder = pR;
  }

  bool send( Message& );
  void next( const UtcTimeStamp& now );
  void next( const std::string&, const UtcTimeStamp& now, bool queued = false );
  void next( const Message&, const UtcTimeStamp& now, bool queued = false );
  void disconnect();
  // KRX fork (P0-11): the connection `pR` has stopped reading. While it is still the session's
  // responder (it closed itself), the session disconnects as on any other drop: logon state
  // cleared, onLogout called.
  void releaseResponder( Responder* pR );

  int getExpectedSenderNum() { return m_state.getNextSenderMsgSeqNum(); }
  int getExpectedTargetNum() { return m_state.getNextTargetMsgSeqNum(); }

  Log* getLog() { return &m_state; }
  const MessageStore* getStore() { return &m_state; }

private:
  typedef std::map<SessionID, Session*> Sessions;
  typedef std::set<SessionID> SessionIDs;

  static bool addSession( Session& );
  static void removeSession( Session& );

  bool send( const std::string& );
  bool sendRaw( Message&, int msgSeqNum = 0 );
  bool resend( Message& message );
  void persist( const Message&, const std::string& ) EXCEPT ( IOException );

  void insertSendingTime( Header& );
  void insertOrigSendingTime( Header&, const UtcTimeStamp& now );
  void fill( Header& );

  bool isGoodTime( const SendingTime& sendingTime )
  {
    if ( !m_checkLatency ) return true;
    return labs( m_timestamper() - sendingTime ) <= m_maxLatency;
  }
  bool checkSessionTime( const UtcTimeStamp& now )
  {
    if( getIsNonStopSession() ) return true;
    UtcTimeStamp creationTime = m_state.getCreationTime();
    return m_sessionTime.isInSameRange( now, creationTime );
  }
  bool isTargetTooHigh( const MsgSeqNum& msgSeqNum )
  { return msgSeqNum > ( m_state.getNextTargetMsgSeqNum() ); }
  bool isTargetTooLow( const MsgSeqNum& msgSeqNum )
  { return msgSeqNum < ( m_state.getNextTargetMsgSeqNum() ); }
  bool isCorrectCompID( const SenderCompID& senderCompID,
                        const TargetCompID& targetCompID )
  {
    if( !m_checkCompId ) return true;

    return
      m_sessionID.getSenderCompID().getValue() == targetCompID.getValue()
      && m_sessionID.getTargetCompID().getValue() == senderCompID.getValue();
  }
  bool shouldSendReset();

  bool validLogonState( const MsgType& msgType );
  void fromCallback( const MsgType& msgType, const Message& msg,
                     const SessionID& sessionID );

  void doBadTime( const Message& msg );
  void doBadCompID( const Message& msg );
  bool doPossDup( const Message& msg );
  bool doTargetTooLow( const Message& msg );
  void doTargetTooHigh( const Message& msg );
  void nextQueued( const UtcTimeStamp& now );
  bool nextQueued( int num, const UtcTimeStamp& now );

  void nextLogon( const Message&, const UtcTimeStamp& now );
  void nextHeartbeat( const Message&, const UtcTimeStamp& now );
  void nextTestRequest( const Message&, const UtcTimeStamp& now );
  void nextLogout( const Message&, const UtcTimeStamp& now );
  void nextReject( const Message&, const UtcTimeStamp& now );
  void nextSequenceReset( const Message&, const UtcTimeStamp& now );
  void dropQueuedBelow( int newSeqNo ); // KRX fork (P1-5)
  bool isNextTarget( const Message& message ); // KRX fork (U-4)
  void nextResendRequest( const Message&, const UtcTimeStamp& now );

  void generateLogon();
  void generateLogon( const Message& );
  void generateResendRequest( const BeginString&, const MsgSeqNum& );
  void generateSequenceReset( int, int );
  void generateRetransmits(int beginSeqNo, int endSeqNo);
  void generateHeartbeat();
  void generateHeartbeat( const Message& );
  void generateTestRequest( const std::string& );
  void generateReject( const Message&, int err, int field = 0 );
  void generateReject( const Message&, const std::string& text );
  void generateBusinessReject( const Message&, int err, int field = 0 );
  void generateLogout( const std::string& text = "" );

  void populateRejectReason( Message&, int field, const std::string& text );
  void populateRejectReason( Message&, const std::string& text );

  bool verify( const Message& msg,
               bool checkTooHigh = true, bool checkTooLow = true );

  Message newMessage( const MsgType & msgType ) const;

  std::function<UtcTimeStamp()> m_timestamper;
  Application& m_application;
  SessionID m_sessionID;
  TimeRange m_sessionTime;
  TimeRange m_logonTime;

  std::string m_senderDefaultApplVerID;
  std::string m_targetDefaultApplVerID;
  bool m_sendRedundantResendRequests;
  bool m_checkCompId;
  bool m_checkLatency;
  int m_maxLatency;
  int m_sendTimeoutMs{ 0 };
  // KRX fork (U-9): messages dropped above a full queue since the last disconnect.
  int m_queueDropped{ 0 };
  bool m_resetOnLogon;
  bool m_resetOnLogout;
  bool m_resetOnDisconnect;
  bool m_refreshOnLogon;
  int m_timestampPrecision;
  bool m_persistMessages;
  bool m_validateLengthAndChecksum;
  bool m_sendNextExpectedMsgSeqNum;
  bool m_isNonStopSession;

  SessionState m_state;
  DataDictionaryProvider m_dataDictionaryProvider;
  MessageStoreFactory& m_messageStoreFactory;
  LogFactory* m_pLogFactory;
  Responder* m_pResponder;
  Mutex m_mutex;
  // KRX fork (P0-3): threads waiting for m_mutex in sendRaw or disconnect; a long resend lets them
  // in between chunks (a mutex does not hand over by itself: the releasing thread takes it back).
  std::atomic<int> m_mutexWaiters{0};
  // KRX fork (P0-3): guards m_pResponder's changes, so disconnect() can shut the connection down
  // without m_mutex, which a sender stuck in send holds.
  Mutex m_responderMutex;

  static Sessions s_sessions;
  static SessionIDs s_sessionIDs;
  static Sessions s_registered;
  static Mutex s_mutex;
};
}

#endif //FIX_SESSION_H
