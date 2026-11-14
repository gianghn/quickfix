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

#include "ThreadedSocketAcceptor.h"
#include "Settings.h"
#include "Utility.h"

#include <memory>

namespace FIX
{
// KRX fork (P1-4): whether the accept loop goes on after accept failed with `error`: at once after
// a signal or a connection reset before it was accepted, after 100 ms when out of descriptors or
// memory (the pending connection waits in the backlog), never otherwise (the socket was closed).
namespace
{
bool acceptAgain( int error )
{
#ifdef _MSC_VER
  (void)error;
  return false;
#else
  switch ( error )
  {
  case EINTR:
  case ECONNABORTED:
    return true;
  case EMFILE:
  case ENFILE:
  case ENOBUFS:
  case ENOMEM:
    process_sleep( 0.1 );
    return true;
  default:
    return false;
  }
#endif
}
}

ThreadedSocketAcceptor::ThreadedSocketAcceptor(
  Application& application,
  MessageStoreFactory& factory,
  const SessionSettings& settings ) EXCEPT ( ConfigError )
: Acceptor( application, factory, settings )
{ socket_init(); }

ThreadedSocketAcceptor::ThreadedSocketAcceptor(
  Application& application,
  MessageStoreFactory& factory,
  const SessionSettings& settings,
  LogFactory& logFactory ) EXCEPT ( ConfigError )
: Acceptor( application, factory, settings, logFactory )
{ 
  socket_init(); 
}

ThreadedSocketAcceptor::~ThreadedSocketAcceptor()
{ 
  socket_term(); 
}

void ThreadedSocketAcceptor::onConfigure( const SessionSettings& sessionSettings )
EXCEPT ( ConfigError )
{
  for( const SessionID& sessionID : sessionSettings.getSessions() )
  {
    const Dictionary& settings = sessionSettings.get( sessionID );
    settings.getInt( SOCKET_ACCEPT_PORT );
    if( settings.has(SOCKET_REUSE_ADDRESS) )
      settings.getBool( SOCKET_REUSE_ADDRESS );
    if( settings.has(SOCKET_NODELAY) )
      settings.getBool( SOCKET_NODELAY );
  }
}

void ThreadedSocketAcceptor::onInitialize( const SessionSettings& sessionSettings )
EXCEPT ( RuntimeError )
{
  short port = 0;
  std::set<int> ports;

  for( const SessionID& sessionID : sessionSettings.getSessions() )
  {
    const Dictionary& settings = sessionSettings.get( sessionID );
    port = (short)settings.getInt( SOCKET_ACCEPT_PORT );

    m_portToSessions[port].insert( sessionID );

    if( ports.find(port) != ports.end() )
      continue;
    ports.insert( port );

    const bool reuseAddress = settings.has( SOCKET_REUSE_ADDRESS ) ? 
      settings.getBool( SOCKET_REUSE_ADDRESS ) : true;

    const bool noDelay = settings.has( SOCKET_NODELAY ) ? 
      settings.getBool( SOCKET_NODELAY ) : false;

    const int sendBufSize = settings.has( SOCKET_SEND_BUFFER_SIZE ) ?
      settings.getInt( SOCKET_SEND_BUFFER_SIZE ) : 0;

    const int rcvBufSize = settings.has( SOCKET_RECEIVE_BUFFER_SIZE ) ?
      settings.getInt( SOCKET_RECEIVE_BUFFER_SIZE ) : 0;

    socket_handle socket = socket_createAcceptor( port, reuseAddress );
    if( socket == INVALID_SOCKET_HANDLE )
    {
      SocketException e;
      socket_close( socket );
      throw RuntimeError( "Unable to create, bind, or listen to port " 
                         + IntConvertor::convert( (unsigned short)port ) + " (" + e.what() + ")" );
    }
    if( noDelay )
      socket_setsockopt( socket, TCP_NODELAY );
    if( sendBufSize )
      socket_setsockopt( socket, SO_SNDBUF, sendBufSize );
    if( rcvBufSize )
      socket_setsockopt( socket, SO_RCVBUF, rcvBufSize );

    m_socketToPort[socket] = port;
    m_sockets.insert( socket );
  }    
}

void ThreadedSocketAcceptor::onStart()
{
  for( const Sockets::value_type& socket : m_sockets )
  {
    Locker l( m_mutex );
    int port = m_socketToPort[socket];
    AcceptorThreadInfo* info = new AcceptorThreadInfo( this, socket, port );
    thread_id thread;
    thread_spawn( &socketAcceptorThread, info, thread );
    addThread( socket, thread );
  }
}

bool ThreadedSocketAcceptor::onPoll()
{
  return false;
}

void ThreadedSocketAcceptor::onStop()
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
      process_sleep( 0.01 ); // KRX fork (P2-3): it spun a CPU for up to 5 s
    }

    // Under the lock, every socket in m_threads is still open (releaseConnection takes it out
    // before closing it). Listening sockets are closed here, their only close; connections are
    // shut down, and their own threads close them.
    for ( i = m_threads.begin(); i != m_threads.end(); ++i )
    {
      if ( m_sockets.find( i->first ) != m_sockets.end() )
        socket_close( i->first );
      else
        shutdown( i->first, 2 );
    }
    threads = m_threads;
    m_threads.clear();
    closedThreads.swap( m_closedThreads );
    // KRX fork (U-8, 8.4): these numbers are closed now and may be reused. Kept, the next start
    // accepted on them and the next stop closed a connection given one of them under its thread.
    // m_socketToPort is only read for sockets in m_sockets, and onInitialize rewrites each entry.
    m_sockets.clear();
  }

  for ( i = threads.begin(); i != threads.end(); ++i )
    thread_join( i->second );
  for ( const thread_id& thread : closedThreads )
    thread_join( thread );
}

void ThreadedSocketAcceptor::addThread(socket_handle s, thread_id t )
{
  Locker l(m_mutex);

  m_threads[ s ] = t;
}

void ThreadedSocketAcceptor::removeThread(socket_handle s )
{
  Locker l(m_mutex);
  SocketToThread::iterator i = m_threads.find( s );
  if ( i != m_threads.end() )
  {
    thread_detach( i->second );
    m_threads.erase( i );
  }
}

THREAD_PROC ThreadedSocketAcceptor::socketAcceptorThread( void* p )
{
  AcceptorThreadInfo * info = reinterpret_cast < AcceptorThreadInfo* > ( p );

  ThreadedSocketAcceptor* pAcceptor = info->m_pAcceptor;
  socket_handle s = info->m_socket;
  int port = info->m_port;
  delete info;

  int noDelay = 0;
  int sendBufSize = 0;
  int rcvBufSize = 0;
  socket_getsockopt( s, TCP_NODELAY, noDelay );
  socket_getsockopt( s, SO_SNDBUF, sendBufSize );
  socket_getsockopt( s, SO_RCVBUF, rcvBufSize );

  socket_handle socket = 0;
  while ( !pAcceptor->isStopped() )
  {
    socket = socket_accept( s );
    if ( socket == INVALID_SOCKET_HANDLE )
    {
      // KRX fork (P1-4): a failed accept used to end this thread, so the port stopped accepting.
      int error = errno;  // not const: error_strerror takes decltype(errno), an int&
      if ( pAcceptor->isStopped() ) break;
      {
        const Locker l( pAcceptor->m_mutex );
        const std::string event = "Accept failed on port " + IntConvertor::convert( port ) + ": " + error_strerror( error );
        if( pAcceptor->getLog() )
          pAcceptor->getLog()->onEvent( event );
      }
      if ( !acceptAgain( error ) ) break;
      continue;
    }

    if( noDelay )
      socket_setsockopt( socket, TCP_NODELAY );
    if( sendBufSize )
      socket_setsockopt( socket, SO_SNDBUF, sendBufSize );
    if( rcvBufSize )
      socket_setsockopt( socket, SO_RCVBUF, rcvBufSize );

    Sessions sessions = pAcceptor->m_portToSessions[port];

    ThreadedSocketConnection * pConnection =
      new ThreadedSocketConnection
        ( socket, sessions, pAcceptor->getLog() );

    ConnectionThreadInfo* info = new ConnectionThreadInfo( pAcceptor, pConnection );

    {
      Locker l( pAcceptor->m_mutex );

      // KRX fork (U-4, P0-10): accepted as stop() began, after onStop took the threads it joins
      // (under this lock): never joined, the connection outlived the acceptor and could take over
      // a session after a restart. It is closed instead.
      if ( pAcceptor->isStopped() )
      {
        const std::unique_ptr < ConnectionThreadInfo > ownedInfo( info );
        const std::unique_ptr < ThreadedSocketConnection > ownedConnection( pConnection );
        break;
      }

      std::stringstream stream;
      stream << "Accepted connection from " << socket_peername( socket ) << " on port " << port;

      if( pAcceptor->getLog() )
        pAcceptor->getLog()->onEvent( stream.str() );

      thread_id thread;
      if ( !thread_spawn( &socketConnectionThread, info, thread ) )
      {
        delete info;
        delete pConnection;
      }
      else
        pAcceptor->addThread( socket, thread );
    }
  }

  if( !pAcceptor->isStopped() )
    pAcceptor->removeThread( s );

  return 0;
}

THREAD_PROC ThreadedSocketAcceptor::socketConnectionThread( void* p )
{
  ConnectionThreadInfo * info = reinterpret_cast < ConnectionThreadInfo* > ( p );

  ThreadedSocketAcceptor* pAcceptor = info->m_pAcceptor;
  ThreadedSocketConnection* pConnection = info->m_pConnection;
  delete info;

  socket_handle socket = pConnection->getSocket();

  while ( pConnection->read() ) {}
  pAcceptor->releaseConnection( socket, pConnection );
  return 0;
}

void ThreadedSocketAcceptor::releaseConnection( socket_handle s, ThreadedSocketConnection* pConnection )
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
}
