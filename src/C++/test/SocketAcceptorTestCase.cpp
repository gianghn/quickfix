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
#pragma warning( disable : 4503 4355 4786 )
#include "stdafx.h"
#else
#include "config.h"
#endif

#include <SocketAcceptor.h>
#include <ThreadedSocketAcceptor.h>
#include <Utility.h>
#include <fix42/Logon.h>
#include "TestHelper.h"
#include <sstream>

#include "catch_amalgamated.hpp"

using namespace FIX;

TEST_CASE("SocketAcceptorTests")
{
  TestApplication application;
  MemoryStoreFactory factory;
  socket_handle socket;

  SessionSettings settings;
  std::string input =
    "[DEFAULT]\n"
    "ConnectionType=acceptor\n"
    "SocketAcceptPort=0\n"
    "SocketReuseAddress=Y\n"
    "SendBufferSize=1024\n"
    "ReceiveBufferSize=1024\n"
    "StartTime=00:00:00\n"
    "EndTime=00:00:00\n"
    "UseDataDictionary=N\n"
    "CheckLatency=N\n"
    "[SESSION]\n"
    "BeginString=FIX.4.2\n"
    "SenderCompID=ISLD\n"
    "TargetCompID=TW\n"
    "[SESSION]\n"
    "BeginString=FIX.4.1\n"
    "SenderCompID=ISLD\n"
    "TargetCompID=WT\n";
  std::stringstream stream( input );
  stream >> settings;

  SocketAcceptor object( application, factory, settings );
  object.poll();
  socket = createSocket( object.sessionToPort().find(SessionID("FIX.4.2", "ISLD", "TW"))->second, "127.0.0.1" );
  object.poll();

  SECTION("receivePartialMessage")
  {
    std::string firstPart = "8=FIX.4.29=28235=834=2369=31450"
                            "52=20041209-15:35:32.68749=TW50=G56=ISLD"
                            "60=20041209-15:35:3259=055=GE54=148=BLA000060467"
                            "107=BLF5167=FUT44=9740.0041=040=239=038=10"
                            "37=20041209004077151=10150=020=09717=67960"
                            "17=0068712004120909353214=011=000679606=0432=20041209"
                            "1=1234567810=1458=F";

    std::string secondPart = "IX.4.29=34035=834=3369=31450"
                            "52=20041209-15:35:32.69249=TW50=G56=ISLD"
                            "60=20041209-15:35:3259=055=GE54=148=CME000060467"
                            "107=BLF5167=FUT44=9740.0041=040=239=238=10"
                            "37=20041209004077337=0C032=1031=9740.00151=0150=2"
                            "20=09717=6796017=00687220041209093532TN0002843"
                            "75=2004120914=011=00067960375=BLA030A16=0"
                            "432=200412091=1234567810=217";

    FIX42::Logon logon;
    logon.getHeader().set( SenderCompID("TW") );
    logon.getHeader().set( TargetCompID("ISLD") );
    logon.getHeader().set( MsgSeqNum(1) );
    logon.getHeader().set( SendingTime::now() );
    logon.set( HeartBtInt(30) );

    CHECK( socket_send( socket, logon.toString().c_str(), (int)strlen(logon.toString().c_str()) ) );
    object.poll();
    CHECK( socket_send( socket, firstPart.c_str(), (int)strlen(firstPart.c_str()) ) );
    object.poll();
    CHECK( socket_send( socket, secondPart.c_str(), (int)strlen(secondPart.c_str()) ) );
    object.poll();
  }

  object.stop( true );
  destroySocket( socket );
}

// KRX fork: SocketAcceptHost binds the listening socket to one IPv4 address
namespace
{
SessionSettings acceptHostSettings( const std::string& port,
                                    const std::string& firstHost,
                                    const std::string& secondHost )
{
  std::string input =
    "[DEFAULT]\n"
    "ConnectionType=acceptor\n"
    "SocketReuseAddress=Y\n"
    "StartTime=00:00:00\n"
    "EndTime=00:00:00\n"
    "UseDataDictionary=N\n"
    "SocketAcceptPort=" + port + "\n"
    "[SESSION]\n"
    "BeginString=FIX.4.2\n"
    "SenderCompID=ISLD\n"
    "TargetCompID=TW\n"
    + firstHost +
    "[SESSION]\n"
    "BeginString=FIX.4.2\n"
    "SenderCompID=ISLD\n"
    "TargetCompID=WT\n"
    + secondHost;
  std::stringstream stream( input );
  SessionSettings settings;
  stream >> settings;
  return settings;
}

std::string hostLine( const std::string& host )
{
  return "SocketAcceptHost=" + host + "\n";
}

uint16_t freePort()
{
  socket_handle socket = socket_createAcceptor( 0, true );
  uint16_t port = (uint16_t)socket_hostport( socket );
  socket_close( socket );
  return port;
}

// A started acceptor is stopped again, so a failed check leaves no thread behind
bool startIsConfigError( Acceptor& acceptor )
{
  try
  {
    acceptor.start();
  }
  catch( ConfigError& )
  {
    return true;
  }
  catch( std::exception& )
  {
    return false;
  }
  acceptor.stop( true );
  return false;
}
}

TEST_CASE("SocketAcceptHostTests")
{
  TestApplication application;
  MemoryStoreFactory factory;

  SECTION("bindsTheGivenAddress")
  {
    SessionSettings settings = acceptHostSettings( "0", hostLine( "127.0.0.1" ), hostLine( "127.0.0.1" ) );
    SocketAcceptor object( application, factory, settings );
    object.poll();
    socket_handle socket = createSocket( object.sessionToPort().find(SessionID("FIX.4.2", "ISLD", "TW"))->second, "127.0.0.1" );
    CHECK( socket != INVALID_SOCKET_HANDLE );
    object.stop( true );
    destroySocket( socket );
  }

  SECTION("emptyHostBindsEveryAddress")
  {
    SessionSettings settings = acceptHostSettings( "0", hostLine( "" ), "" );
    SocketAcceptor object( application, factory, settings );
    object.poll();
    socket_handle socket = createSocket( object.sessionToPort().find(SessionID("FIX.4.2", "ISLD", "TW"))->second, "127.0.0.1" );
    CHECK( socket != INVALID_SOCKET_HANDLE );
    object.stop( true );
    destroySocket( socket );
  }

  SECTION("hostThatIsNotANumericIPv4AddressIsAConfigError")
  {
    const char* hosts[] = { "localhost", "127.0.0", "256.0.0.1", "1.2.3.4.5", "::1", "x" };
    for( const char* host : hosts )
    {
      INFO( host );
      SessionSettings settings = acceptHostSettings( IntConvertor::convert( freePort() ), hostLine( host ), hostLine( host ) );
      SocketAcceptor plain( application, factory, settings );
      CHECK( startIsConfigError( plain ) );
      ThreadedSocketAcceptor threaded( application, factory, settings );
      CHECK( startIsConfigError( threaded ) );
    }
  }

  SECTION("samePortWithTwoHostsIsAConfigError")
  {
    const std::string port = IntConvertor::convert( freePort() );
    SessionSettings twoHosts = acceptHostSettings( port, hostLine( "127.0.0.1" ), hostLine( "127.0.0.2" ) );
    SocketAcceptor plain( application, factory, twoHosts );
    CHECK( startIsConfigError( plain ) );
    ThreadedSocketAcceptor threaded( application, factory, twoHosts );
    CHECK( startIsConfigError( threaded ) );

    SessionSettings oneWithout = acceptHostSettings( port, hostLine( "127.0.0.1" ), "" );
    ThreadedSocketAcceptor threadedOneWithout( application, factory, oneWithout );
    CHECK( startIsConfigError( threadedOneWithout ) );
  }

  SECTION("samePortWithTheSameHostIsAccepted")
  {
    SessionSettings settings = acceptHostSettings( IntConvertor::convert( freePort() ), hostLine( "127.0.0.1" ), hostLine( "127.0.0.1" ) );
    ThreadedSocketAcceptor object( application, factory, settings );
    CHECK_NOTHROW( object.start() );
    object.stop( true );
  }

  SECTION("addressNotOnThisHostFailsWithPortAndHost")
  {
    // 192.0.2.1 is TEST-NET-1: never a local address, so bind() fails
    const std::string port = IntConvertor::convert( freePort() );
    SessionSettings settings = acceptHostSettings( port, hostLine( "192.0.2.1" ), hostLine( "192.0.2.1" ) );
    ThreadedSocketAcceptor object( application, factory, settings );
    std::string error;
    try
    {
      object.start();
      object.stop( true );
    }
    catch( RuntimeError& e )
    {
      error = e.what();
    }
    INFO( error );
    CHECK( error.find( "port " + port + " on 192.0.2.1" ) != std::string::npos );
  }
}
