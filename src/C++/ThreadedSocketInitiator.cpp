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
#endif

#include "ThreadedSocketInitiator.h"
#include "Session.h"
#include "Settings.h"

namespace FIX
{
ThreadedSocketInitiator::ThreadedSocketInitiator(
  Application& application,
  MessageStoreFactory& factory,
  const SessionSettings& settings ) EXCEPT ( ConfigError )
: Initiator( application, factory, settings ),
  m_lastConnect( 0 ), m_reconnectInterval( 30 ), m_noDelay( false ), 
  m_sendBufSize( 0 ), m_rcvBufSize( 0 ) 
{ 
  socket_init(); 
}

ThreadedSocketInitiator::ThreadedSocketInitiator(
  Application& application,
  MessageStoreFactory& factory,
  const SessionSettings& settings,
  LogFactory& logFactory ) EXCEPT ( ConfigError )
: Initiator( application, factory, settings, logFactory ),
  m_lastConnect( 0 ), m_reconnectInterval( 30 ), m_noDelay( false ), 
  m_sendBufSize( 0 ), m_rcvBufSize( 0 ) 
{ 
  socket_init(); 
}

ThreadedSocketInitiator::~ThreadedSocketInitiator()
{ 
  socket_term(); 
}

void ThreadedSocketInitiator::onConfigure( const SessionSettings& s )
EXCEPT ( ConfigError )
{
  const Dictionary& dict = s.get();

  if( dict.has( RECONNECT_INTERVAL ) )
    m_reconnectInterval = dict.getInt( RECONNECT_INTERVAL );
  if( dict.has( SOCKET_NODELAY ) )
    m_noDelay = dict.getBool( SOCKET_NODELAY );
  if( dict.has( SOCKET_SEND_BUFFER_SIZE ) )
    m_sendBufSize = dict.getInt( SOCKET_SEND_BUFFER_SIZE );
  if( dict.has( SOCKET_RECEIVE_BUFFER_SIZE ) )
    m_rcvBufSize = dict.getInt( SOCKET_RECEIVE_BUFFER_SIZE );
}

void ThreadedSocketInitiator::onInitialize( const SessionSettings& s )
EXCEPT ( RuntimeError )
{
}

void ThreadedSocketInitiator::onStart()
{
  while ( !isStopped() )
  {
    time_t now;
    ::time( &now );

    if ( (now - m_lastConnect) >= m_reconnectInterval )
    {
      Locker l( m_mutex );
      connect();
      m_lastConnect = now;
    }

    process_sleep( 1 );
  }
}

bool ThreadedSocketInitiator::onPoll()
{
  return false;
}

void ThreadedSocketInitiator::onStop()
{
  SocketToThread threads;
  SocketToThread::iterator i;
  std::vector < thread_id > closedThreads;
  
  {
    Locker l(m_mutex);

    time_t start = 0;
    time_t now = 0;

    ::time( &start );
    while ( isLoggedOn() )
    {
      if( ::time(&now) -5 >= start )
        break;
    }

    // Under the lock, every socket in m_threads is still open (releaseConnection takes it out
    // before closing it): shut each down to wake its thread, which closes it.
    for ( i = m_threads.begin(); i != m_threads.end(); ++i )
      shutdown( i->first, 2 );
    threads = m_threads;
    m_threads.clear();
    closedThreads.swap( m_closedThreads );
  }

  for ( i = threads.begin(); i != threads.end(); ++i )
    thread_join( i->second );
  for ( const thread_id& thread : closedThreads )
    thread_join( thread );
  threads.clear();
}

void ThreadedSocketInitiator::doConnect( const SessionID& s, const Dictionary& d )
{
  try
  {
    Session* session = Session::lookupSession( s );
    if( !session->isSessionTime(UtcTimeStamp::now()) ) return;

    Log* log = session->getLog();

    HostDetails host = m_hostDetailsProvider.getHost(s, d);

    socket_handle socket = socket_createConnector();
    if( m_noDelay )
      socket_setsockopt( socket, TCP_NODELAY );
    if( m_sendBufSize )
      socket_setsockopt( socket, SO_SNDBUF, m_sendBufSize );
    if( m_rcvBufSize )
      socket_setsockopt( socket, SO_RCVBUF, m_rcvBufSize );

    setPending( s );
    log->onEvent( "Connecting to " + host.address + " on port " + IntConvertor::convert((unsigned short)host.port) + " (Source " + host.sourceAddress + ":" + IntConvertor::convert((unsigned short)host.sourcePort) + ")");

    ThreadedSocketConnection* pConnection =
      new ThreadedSocketConnection( s, socket, host.address, host.port, getLog(), host.sourceAddress, host.sourcePort );

    ThreadPair* pair = new ThreadPair( this, pConnection );

    {
      Locker l( m_mutex );
      thread_id thread;
      if ( thread_spawn( &socketThread, pair, thread ) )
      {
        addThread( socket, thread );
      }
      else
      {
        delete pair;
        pConnection->disconnect();
        delete pConnection;
        setDisconnected( s );
      }
    }
  }
  catch ( std::exception& ) {}
}

void ThreadedSocketInitiator::addThread(socket_handle s, thread_id t )
{
  Locker l(m_mutex);

  m_threads[ s ] = t;
}

void ThreadedSocketInitiator::releaseConnection( socket_handle s, ThreadedSocketConnection* pConnection )
{
  // Under the lock, deleting the connection (which closes its socket) and leaving m_threads are
  // one step, so onStop never shuts down a number that may already belong to someone else.
  const Locker l( m_mutex );
  std::unique_ptr < ThreadedSocketConnection > connection( pConnection );
  connection.reset();
  const auto i = m_threads.find( s );
  if ( i == m_threads.end() )
    return; // onStop took this thread and joins it
  if ( isStopped() )
    m_closedThreads.push_back( i->second ); // onStop is still to come and joins it
  else
    thread_detach( i->second );
  m_threads.erase( i );
}

THREAD_PROC ThreadedSocketInitiator::socketThread( void* p )
{
  ThreadPair * pair = reinterpret_cast < ThreadPair* > ( p );

  ThreadedSocketInitiator* pInitiator = pair->first;
  ThreadedSocketConnection* pConnection = pair->second;
  FIX::SessionID sessionID = pConnection->getSession()->getSessionID();
  FIX::Session* pSession = FIX::Session::lookupSession( sessionID );
  socket_handle socket = pConnection->getSocket();
  delete pair;

  pInitiator->lock();

  if( !pConnection->connect() )
  {
    pInitiator->getLog()->onEvent( "Connection failed" );
    pInitiator->releaseConnection( socket, pConnection );
    pInitiator->setDisconnected( sessionID );
    return 0;
  }

  pInitiator->setConnected( sessionID );
  pInitiator->getLog()->onEvent( "Connection succeeded" );

  pSession->next( UtcTimeStamp::now() );

  while ( pConnection->read() ) {}

  pInitiator->releaseConnection( socket, pConnection );
  pInitiator->setDisconnected( sessionID );
  return 0;
}
}
