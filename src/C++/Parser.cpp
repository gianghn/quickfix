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

#include "Parser.h"
#include "Utility.h"
#include "FieldConvertors.h"
#include <algorithm>

namespace FIX
{
bool Parser::extractLength( int& length, std::string::size_type& pos,
                            const std::string& buffer )
EXCEPT ( MessageParseError )
{
  if( !buffer.size() ) return false;

  // KRX fork (P0-7): BodyLength is the field after BeginString, never a later frame's.
  std::string::size_type startPos = buffer.find( '\001' );
  if( startPos == std::string::npos || buffer.size() < startPos + 3 ) return false;
  if( buffer.compare( startPos, 3, "\0019=" ) != 0 ) throw MessageParseError();
  startPos += 3;
  std::string::size_type endPos = buffer.find( "\001", startPos );
  if( endPos == std::string::npos ) return false;

  std::string strLength( buffer, startPos, endPos - startPos );

  try
  {
    length = IntConvertor::convert( strLength );
    if( length < 0 ) throw MessageParseError();
  }
  catch( FieldConvertError& )
  { throw MessageParseError(); }

  pos = endPos + 1;
  return true;
}

bool Parser::readFixMessage( std::string& str )
EXCEPT ( MessageParseError )
{
  std::string::size_type pos = 0;

  if( m_buffer.length() < 2 ) return false;
  pos = m_buffer.find( "8=" );
  if( pos == std::string::npos ) return false;
  m_buffer.erase( 0, pos );

  int length = 0;

  try
  {
    if( extractLength(length, pos, m_buffer) )
    {
      pos += length;
      if( m_buffer.size() < pos )
        return false;

      pos = m_buffer.find( "\00110=", pos-1 );
      if( pos == std::string::npos ) return false;
      pos += 4;
      pos = m_buffer.find( "\001", pos );
      if( pos == std::string::npos ) return false;
      pos += 1;

      str.assign( m_buffer, 0, pos );
      m_buffer.erase( 0, pos );
      return true;
    }
  }
  catch( MessageParseError& e )
  {
    // KRX fork (P0-7): drop this frame only, up to the SOH before the next one, instead of every
    // frame queued behind it. With no next frame yet, keep two bytes that may be its "\0018" start.
    const std::string::size_type next = m_buffer.find( "\0018=" );
    m_buffer.erase( 0, next != std::string::npos ? next : m_buffer.size() - 2 );
    throw e;
  }

  return false;
}

bool Parser::readNextFixMessage( std::string& str )
{
  // Each garbled frame drops at least three bytes, so the loop ends.
  while( true )
  {
    try
    {
      return readFixMessage( str );
    }
    catch( MessageParseError& )
    {
      continue;
    }
  }
}
}
