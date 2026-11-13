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

#include <SessionState.h>
#include <Log.h>
#include <string>
#include <sstream>

#include "catch_amalgamated.hpp"

using namespace FIX;

TEST_CASE("SessionStateTests")
{
  class TestLog : public Log
  {
  public:
    void clear() {
      events = 0;
    }
    void backup() {
      eventsBackup = events;
    }
    void onIncoming( const std::string& ) {}
    void onOutgoing( const std::string& ) {}
    void onEvent( const std::string& ) {}

    int events = 0;
    int eventsBackup = 0;
  };

  SECTION("ClearSessionLog_StateLogNotNull_LogCleared")
  {
    SessionSettings settings;
    TestLog log;
    log.events = 5;

    SessionState state( UtcTimeStamp::now() );
    state.log(&log);

    state.clear();

    CHECK(0 == log.events);
  }

  SECTION("clearSessionLog_StateLogIsNull_LogNotCleared")
  {
    SessionSettings settings;
    TestLog log;
    log.events = 5;

    SessionState state( UtcTimeStamp::now() );

    state.clear();

    CHECK(5 == log.events);
  }

  SECTION("backupSessionLog_StateLogNotNull_LogBackedUp")
  {
    SessionSettings settings;
    TestLog log;
    log.events = 5;

    SessionState state( UtcTimeStamp::now() );
    state.log(&log);

    state.backup();

    CHECK(5 == log.eventsBackup);
  }

  // KRX fork (U-4): the reset flags and the resend range keep what was set (each setter stores).
  SECTION("resetFlagsAndResendRange_Set_ReadBack")
  {
    SessionState state( UtcTimeStamp::now() );

    state.receivedReset( true );
    state.sentReset( true );
    state.resendRange( 5, 9 );
    CHECK( state.receivedReset() );
    CHECK( state.sentReset() );
    CHECK( state.resendRequested() );
    CHECK( 5 == state.resendRange().first );
    CHECK( 9 == state.resendRange().second );

    state.receivedReset( false );
    state.sentReset( false );
    state.resendRange( 0, 0 );
    CHECK( !state.receivedReset() );
    CHECK( !state.sentReset() );
    CHECK( !state.resendRequested() );
  }

  // KRX fork (U-9): at most maxQueued() messages wait above a gap; 0 is no limit.
  SECTION("queue_Capped_RefusesPastTheCap")
  {
    SessionState state( UtcTimeStamp::now() );
    CHECK( 10000 == state.maxQueued() );

    state.maxQueued( 2 );
    CHECK( 2 == state.maxQueued() );
    CHECK( state.queue( 3, Message() ) );
    CHECK( state.queue( 4, Message() ) );
    CHECK( state.queue( 4, Message() ) );   // already queued: replaced, not refused
    CHECK( !state.queue( 5, Message() ) );

    state.maxQueued( 0 );
    CHECK( state.queue( 5, Message() ) );
  }

  SECTION("backupSessionLog_StateLogIsNull_LogBackedUp")
  {
    SessionSettings settings;
    TestLog log;
    log.events = 5;

    SessionState state( UtcTimeStamp::now() );

    state.backup();

    CHECK(0 == log.eventsBackup);
  }
}
