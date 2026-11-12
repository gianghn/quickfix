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

#ifdef _MSC_VER
#include "stdafx.h"
#else
#include "config.h"
#include <poll.h>
#include <cerrno>
#endif

#include <chrono>
#include <string>

#include "ThreadedSocketConnection.h"
#include "ThreadedSocketAcceptor.h"
#include "ThreadedSocketInitiator.h"
#include "Session.h"
#include "Utility.h"

namespace FIX
{
ThreadedSocketConnection::ThreadedSocketConnection
(socket_handle s, Sessions sessions, Log* pLog )
: m_socket( s ), m_pLog( pLog ),
  m_sessions( sessions ), m_pSession( 0 ),
  m_disconnect( false )
{
#if _MSC_VER
  FD_ZERO( &m_fds );
  FD_SET( m_socket, &m_fds );
#endif
}

ThreadedSocketConnection::ThreadedSocketConnection
( const SessionID& sessionID, socket_handle s,
  const std::string& address, short port, 
  Log* pLog,
  const std::string& sourceAddress, short sourcePort )
  : m_socket( s ), m_address( address ), m_port( port ),
    m_sourceAddress( sourceAddress ), m_sourcePort( sourcePort ),
    m_pLog( pLog ),
    m_pSession( Session::lookupSession( sessionID ) ),
    m_disconnect( false )
{
#if _MSC_VER
  FD_ZERO( &m_fds );
  FD_SET( m_socket, &m_fds );
#endif
  if ( m_pSession ) m_pSession->setResponder( this );
}

ThreadedSocketConnection::~ThreadedSocketConnection()
{
  if ( m_pSession )
  {
    m_pSession->setResponder( 0 );
    Session::unregisterSession( m_pSession->getSessionID() );
  }
  // The one place the socket's number is released; the owner deletes the connection when no
  // other thread can still reach the number (ThreadedSocketAcceptor/Initiator::releaseConnection).
  socket_close( m_socket );
}

#ifndef _MSC_VER
namespace
{
// KRX fork (P0-3): false once `deadline` has passed; otherwise true when `s` takes more bytes or a
// signal woke the wait, and the caller sends again (the send itself reports a real error).
bool waitWritable( socket_handle s, std::chrono::steady_clock::time_point deadline )
{
  const int left = std::chrono::duration_cast<std::chrono::duration<int, std::milli>>(
    deadline - std::chrono::steady_clock::now() ).count();
  pollfd pfd{ s, POLLOUT, 0 };
  return left > 0 && poll( &pfd, 1, left ) != 0;
}

// KRX fork (P0-3): keepalive, and the kernel drops the connection once sent data has gone
// unacknowledged for two heartbeat intervals, so a dead peer is found even while we send. 0 while
// the heartbeat interval is unknown: the system default.
void setLiveness( socket_handle s, int heartBtInt )
{
  const int on = 1;
  setsockopt( s, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof( on ) );
#ifdef TCP_USER_TIMEOUT
  const int userTimeoutMs = 2 * heartBtInt * 1000;
  setsockopt( s, IPPROTO_TCP, TCP_USER_TIMEOUT, &userTimeoutMs, sizeof( userTimeoutMs ) );
#else
#ifdef TCP_RXT_CONNDROPTIME
  // macOS: the same, in seconds.
  const int dropSeconds = 2 * heartBtInt;
  setsockopt( s, IPPROTO_TCP, TCP_RXT_CONNDROPTIME, &dropSeconds, sizeof( dropSeconds ) );
#endif
#endif
}
}
#endif

bool ThreadedSocketConnection::send( const std::string& msg )
{
#ifndef _MSC_VER
  // KRX fork (P0-3): never block for ever on a peer that stopped reading. The whole message must
  // go out within the session's send timeout; past it the socket is shut down, which wakes the
  // reading thread to disconnect the session, and the send fails. The socket is made
  // non-blocking (macOS ignores MSG_DONTWAIT on a blocking one); the reading thread only calls
  // recv after poll reports data, so it is not affected.
  if( !m_socketConfigured )
  {
    socket_setnonblock( m_socket );
    setLiveness( m_socket, m_pSession ? m_pSession->getHeartBtInt() : 0 );
    m_socketConfigured = true;
  }
  const int timeoutMs = m_pSession ? m_pSession->getSendTimeoutMs() : 15000;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds( timeoutMs );
#endif
  ssize_t totalSent = 0;
  while(totalSent < (int)msg.length())
  {
    // Only what is left: the whole length after a partial send read past the message and put
    // those bytes on the wire (quickfix/quickfix#742).
    const ssize_t sent = socket_send( m_socket, &msg[ totalSent ], msg.length() - totalSent );
#ifndef _MSC_VER
    // A signal before any byte went out is not a failed send.
    if( sent < 0 && errno == EINTR ) continue;
    // KRX fork (P0-3): a full buffer waits for room until the deadline (EAGAIN is EWOULDBLOCK on
    // Linux and macOS).
    if( sent < 0 && errno == EAGAIN )
    {
      if( waitWritable( m_socket, deadline ) ) continue;
      const std::string event = "Send timed out after " + std::to_string( timeoutMs ) + " ms: disconnecting";
      if( m_pSession ) m_pSession->getLog()->onEvent( event );
      shutdown( m_socket, 2 );
      return false;
    }
#endif
    if(sent < 0) return false;
    totalSent += sent;
  }

  return true;
}

bool ThreadedSocketConnection::connect()
{
  // do the bind in the thread as name resolution may block
  if ( !m_sourceAddress.empty() || m_sourcePort )
    socket_bind( m_socket, m_sourceAddress.c_str(), m_sourcePort );

  return socket_connect(getSocket(), m_address.c_str(), m_port) >= 0;
}

void ThreadedSocketConnection::disconnect()
{
  m_disconnect = true;
  // Shut down, don't close: it wakes the reading thread and the number stays ours, so a second
  // disconnect, from any thread, can never reach a file or socket opened since.
  shutdown( m_socket, 2 );
}

bool ThreadedSocketConnection::read()
{
#if _MSC_VER
  struct timeval timeout = { 1, 0 };
  fd_set readset = m_fds;
#else
  int timeout = 1000; // 1000ms = 1 second
  struct pollfd pfd = { m_socket, POLLIN | POLLPRI, 0 };
#endif

  try
  {
    // Wait for input (1 second timeout)
#if _MSC_VER
    int result = select( 0, &readset, 0, 0, &timeout );
#else
    int result = poll( &pfd, 1, timeout );
#endif

    if( result > 0 ) // Something to read
    {
      // We can read without blocking
      ssize_t size = socket_recv( m_socket, m_buffer, sizeof(m_buffer) );
      if ( size <= 0 ) { throw SocketRecvFailed( size ); }
      m_parser.addToStream( m_buffer, size );
    }
    else if( result < 0 ) // Error
    {
#ifndef _MSC_VER
      // KRX fork (P1-3): a signal is not a socket error; read again.
      if( errno == EINTR ) return true;
#endif
      throw SocketRecvFailed( result );
    }

    // KRX fork (P1-10): the session's timers ran only when poll timed out, so a peer sending a
    // byte every half second was never timed out. They now run once a second whatever arrives.
    const auto now = std::chrono::steady_clock::now();
    if( m_pSession && now - m_lastTimerCheck >= std::chrono::seconds( 1 ) )
    {
      m_lastTimerCheck = now;
      // A named time stamp: Mull reads a temporary's destructor as the call (MAINTENANCE §5).
      const UtcTimeStamp timeStamp = UtcTimeStamp::now();
      m_pSession->next( timeStamp );
    }

    processStream();
    return true;
  }
  // KRX fork (P0-2): not only SocketRecvFailed; any error ends this connection, never the process.
  catch ( std::exception& e )
  {
    if( m_disconnect )
      return false;

    if( m_pSession )
    {
      m_pSession->getLog()->onEvent( e.what() );
      m_pSession->disconnect();
    }
    else
    {
      disconnect();
    }

    return false;
  }
}

bool ThreadedSocketConnection::readMessage( std::string& msg )
EXCEPT ( SocketRecvFailed )
{
  // KRX fork (P0-7): a garbled frame is skipped; true no longer hands back the previous msg.
  return m_parser.readNextFixMessage( msg );
}

void ThreadedSocketConnection::processStream()
{
  std::string msg;
  while( readMessage(msg) )
  {
    if ( !m_pSession )
    {
      if ( !setSession( msg ) )
      { disconnect(); continue; }
    }
    try
    {
      m_pSession->next( msg, UtcTimeStamp::now() );
    }
    catch( InvalidMessage& )
    {
      if( !m_pSession->isLoggedOn() )
      {
        disconnect();
        return;
      }
    }
  }
}

bool ThreadedSocketConnection::setSession( const std::string& msg )
{
  m_pSession = Session::lookupSession( msg, true );
  if ( !m_pSession ) 
  {
    if( m_pLog )
    {
      m_pLog->onEvent( "Session not found for incoming message: " + msg );
      m_pLog->onIncoming( msg );
    }
    return false;
  }

  SessionID sessionID = m_pSession->getSessionID();
  m_pSession = 0;

  // see if the session frees up within 5 seconds
  for( int i = 1; i <= 5; i++ )
  {
    if( !Session::isSessionRegistered( sessionID ) )
      m_pSession = Session::registerSession( sessionID );
    if( m_pSession ) break;
    process_sleep( 1 );
  }

  if ( !m_pSession ) 
    return false;
  if ( m_sessions.find(m_pSession->getSessionID()) == m_sessions.end() )
    return false;

  m_pSession->setResponder( this );
  return true;
}

} // namespace FIX
