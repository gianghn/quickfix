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

#include "FileStore.h"
#include "SessionID.h"
#include "Parser.h"
#include "Utility.h"
#include <filesystem>
#include <fstream>
#include <system_error>

namespace FIX
{
FileStore::FileStore( const UtcTimeStamp& now, std::string path, const SessionID& sessionID )
: m_cache( now ), m_msgFile( 0 ), m_headerFile( 0 ), m_seqNumsFile( 0 ), m_sessionFile( 0 )
{
  file_mkdir( path.c_str() );

  if ( path.empty() ) path = ".";
  const std::string& begin =
    sessionID.getBeginString().getString();
  const std::string& sender =
    sessionID.getSenderCompID().getString();
  const std::string& target =
    sessionID.getTargetCompID().getString();
  const std::string& qualifier =
    sessionID.getSessionQualifier();

  std::string sessionid = begin + "-" + sender + "-" + target;
  if( qualifier.size() )
    sessionid += "-" + qualifier;

  std::string prefix
    = file_appendpath(path, sessionid + ".");

  m_msgFileName = prefix + "body";
  m_headerFileName = prefix + "header";
  m_seqNumsFileName = prefix + "seqnums";
  m_sessionFileName = prefix + "session";

  try
  {
    open( false );
  }
  catch ( IOException & e )
  {
    throw ConfigError( e.what() );
  }
}

FileStore::~FileStore()
{
  if( m_msgFile ) fclose( m_msgFile );
  if( m_headerFile ) fclose( m_headerFile );
  if( m_seqNumsFile ) fclose( m_seqNumsFile );
  if( m_sessionFile ) fclose( m_sessionFile );
}

void FileStore::open( bool deleteFile )
{
  if ( m_msgFile ) fclose( m_msgFile );
  if ( m_headerFile ) fclose( m_headerFile );
  if ( m_seqNumsFile ) fclose( m_seqNumsFile );
  if ( m_sessionFile ) fclose( m_sessionFile );

  m_msgFile = 0;
  m_headerFile = 0;
  m_seqNumsFile = 0;
  m_sessionFile = 0;

  if ( deleteFile )
  {
    file_unlink( m_msgFileName.c_str() );
    file_unlink( m_headerFileName.c_str() );
    file_unlink( m_seqNumsFileName.c_str() );
    file_unlink( m_sessionFileName.c_str() );
  }

  populateCache();
  m_msgFile = file_fopen( m_msgFileName.c_str(), "r+" );
  if ( !m_msgFile ) m_msgFile = file_fopen( m_msgFileName.c_str(), "w+" );
  if ( !m_msgFile ) throw ConfigError( 
    "Could not open body file: " + m_msgFileName + " " + error_strerror() );

  m_headerFile = file_fopen( m_headerFileName.c_str(), "r+" );
  if ( !m_headerFile ) m_headerFile = file_fopen( m_headerFileName.c_str(), "w+" );
  if ( !m_headerFile ) throw ConfigError( 
    "Could not open header file: " + m_headerFileName + " " + error_strerror() );

  m_seqNumsFile = file_fopen( m_seqNumsFileName.c_str(), "r+" );
  if ( !m_seqNumsFile ) m_seqNumsFile = file_fopen( m_seqNumsFileName.c_str(), "w+" );
  if ( !m_seqNumsFile ) throw ConfigError( 
    "Could not open seqnums file: " + m_seqNumsFileName + " " + error_strerror() );

  bool setCreationTime = false;
  m_sessionFile = file_fopen( m_sessionFileName.c_str(), "r" );
  if ( !m_sessionFile ) setCreationTime = true;
  else fclose( m_sessionFile );

  m_sessionFile = file_fopen( m_sessionFileName.c_str(), "r+" );
  if ( !m_sessionFile ) m_sessionFile = file_fopen( m_sessionFileName.c_str(), "w+" );
  if ( !m_sessionFile ) throw ConfigError( 
    "Could not open session file " + error_strerror() );
  if ( setCreationTime ) setSession();

  setNextSenderMsgSeqNum( getNextSenderMsgSeqNum() );
  setNextTargetMsgSeqNum( getNextTargetMsgSeqNum() );
}

void FileStore::populateCache()
{
  FILE* headerFile = file_fopen( m_headerFileName.c_str(), "r+" );
  if ( headerFile )
  {
    int num;
    long offset;
    std::size_t size;
    // KRX fork (P0-4): a record whose message runs past the end of the body file (the machine
    // stopped between the two writes, or the body file was cut short) is dropped, so a resend
    // gap-fills that one message instead of failing on it. No body file: no message fits.
    std::error_code noBody;
    const std::uintmax_t found = std::filesystem::file_size( m_msgFileName, noBody );
    const std::uintmax_t bodySize = noBody ? 0 : found;

    while (FILE_FSCANF(headerFile, "%d,%ld,%zu ", &num, &offset, &size) == 3)
    {
      if ( offset < 0 || size > bodySize || static_cast<std::uintmax_t>( offset ) > bodySize - size )
        continue;
      const std::pair<NumToOffset::iterator, bool> it =
        m_offsets.insert(NumToOffset::value_type(num, std::make_pair(offset, size)));

      if (it.second == false)
      {
        it.first->second = std::make_pair(offset, size);
      }
    }
    fclose( headerFile );
  }

  FILE* seqNumsFile = file_fopen( m_seqNumsFileName.c_str(), "r+" );
  if ( seqNumsFile )
  {
    int sender, target;
    if ( FILE_FSCANF( seqNumsFile, "%d : %d", &sender, &target ) == 2 )
    {
      m_cache.setNextSenderMsgSeqNum( sender );
      m_cache.setNextTargetMsgSeqNum( target );
    }
    fclose( seqNumsFile );
  }

  FILE* sessionFile = file_fopen( m_sessionFileName.c_str(), "r+" );
  if ( sessionFile )
  {
    char time[ 22 ];
#ifdef HAVE_FSCANF_S
    int result = FILE_FSCANF( sessionFile, "%s", time, 22 );
#else
    int result = FILE_FSCANF( sessionFile, "%s", time );
#endif
    if( result == 1 )
    {
      m_cache.setCreationTime( UtcTimeStampConvertor::convert( time ) );
    }
    fclose( sessionFile );
  }
}

MessageStore* FileStoreFactory::create( const UtcTimeStamp& now, const SessionID& sessionID )
{
  if ( m_path.size() ) return new FileStore( now, m_path, sessionID );

  std::string path;
  Dictionary settings = m_settings.get( sessionID );
  path = settings.getString( FILE_STORE_PATH );
  return new FileStore( now, path, sessionID );
}

void FileStoreFactory::destroy( MessageStore* pStore )
{
  delete pStore;
}

bool FileStore::set( int msgSeqNum, const std::string& msg )
EXCEPT ( IOException )
{
  if ( !m_msgFile || !m_headerFile )
    throw IOException( "Store files are not open: " + m_msgFileName );
  if ( fseek( m_msgFile, 0, SEEK_END ) )
    throw IOException( "Cannot seek to end of " + m_msgFileName );
  if ( fseek( m_headerFile, 0, SEEK_END ) )
    throw IOException( "Cannot seek to end of " + m_headerFileName );

  long offset = ftell( m_msgFile );
  if ( offset < 0 )
    throw IOException( "Unable to get file pointer position from " + m_msgFileName );
  const auto headerEnd = ftell( m_headerFile );
  if ( headerEnd < 0 )
    throw IOException( "Unable to get file pointer position from " + m_headerFileName );
  std::size_t size = msg.size();

  // KRX fork (P0-4): the body first, then its header record ("seqnum,offset,size "), each flushed;
  // the message is in m_offsets only once both are on file. Any failure puts both files back.
  if ( fwrite( msg.data(), sizeof( char ), size, m_msgFile ) != size || fflush( m_msgFile ) == EOF )
    rollBack( offset, headerEnd, "Unable to write to file " + m_msgFileName );
  const std::string record = std::to_string( msgSeqNum ) + "," + std::to_string( offset ) + ","
    + std::to_string( size ) + " ";
  if ( fwrite( record.data(), sizeof( char ), record.size(), m_headerFile ) != record.size()
       || fflush( m_headerFile ) == EOF )
    rollBack( offset, headerEnd, "Unable to write to file " + m_headerFileName );

  m_offsets.insert_or_assign( msgSeqNum, std::make_pair( offset, size ) );
  return true;
}

// KRX fork (P0-4): cuts the body and header files back to `msgEnd` and `headerEnd` bytes and throws
// `error`. Both are reopened, so neither a stream's error flag nor bytes left in its buffer outlive
// the failure (closing may write those bytes; the cut removes them).
void FileStore::rollBack( std::int64_t msgEnd, std::int64_t headerEnd, const std::string& error )
{
  fclose( m_msgFile );
  fclose( m_headerFile );
  bool restored = true;
#ifndef _MSC_VER
  restored = truncate( m_msgFileName.c_str(), msgEnd ) == 0
    && truncate( m_headerFileName.c_str(), headerEnd ) == 0;
#endif
  m_msgFile = file_fopen( m_msgFileName.c_str(), "r+" );
  m_headerFile = file_fopen( m_headerFileName.c_str(), "r+" );
  if ( !restored || !m_msgFile || !m_headerFile )
    throw IOException( error + "; the store could not be put back: " + error_strerror() );
  throw IOException( error );
}

void FileStore::get( int begin, int end,
                     std::vector < std::string > & result ) const
EXCEPT ( IOException )
{
  result.clear();
  std::string msg;
  for ( int i = begin; i <= end; ++i )
  {
    if ( get( i, msg ) )
      result.push_back( msg );
  }
}

int FileStore::getNextSenderMsgSeqNum() const EXCEPT ( IOException )
{
  return m_cache.getNextSenderMsgSeqNum();
}

int FileStore::getNextTargetMsgSeqNum() const EXCEPT ( IOException )
{
  return m_cache.getNextTargetMsgSeqNum();
}

void FileStore::setNextSenderMsgSeqNum( int value ) EXCEPT ( IOException )
{
  m_cache.setNextSenderMsgSeqNum( value );
  setSeqNum();
}

void FileStore::setNextTargetMsgSeqNum( int value ) EXCEPT ( IOException )
{
  m_cache.setNextTargetMsgSeqNum( value );
  setSeqNum();
}

void FileStore::incrNextSenderMsgSeqNum() EXCEPT ( IOException )
{
  m_cache.incrNextSenderMsgSeqNum();
  setSeqNum();
}

void FileStore::incrNextTargetMsgSeqNum() EXCEPT ( IOException )
{
  m_cache.incrNextTargetMsgSeqNum();
  setSeqNum();
}

UtcTimeStamp FileStore::getCreationTime() const EXCEPT ( IOException )
{
  return m_cache.getCreationTime();
}

void FileStore::reset( const UtcTimeStamp& now ) EXCEPT ( IOException )
{
  try
  {
    m_cache.reset( now );
    m_offsets.clear();
    open( true );
    setSession();
  }
  catch( std::exception& e )
  {
    throw IOException( e.what() );
  }
}

void FileStore::refresh() EXCEPT ( IOException )
{
  try
  {
    m_cache.reset( UtcTimeStamp::now() );
    m_offsets.clear();
    open( false );
  }
  catch( std::exception& e )
  {
    throw IOException( e.what() );
  }
}

void FileStore::setSeqNum()
{
  rewind( m_seqNumsFile );
  fprintf( m_seqNumsFile, "%10.10d : %10.10d",
           getNextSenderMsgSeqNum(), getNextTargetMsgSeqNum() );
  if ( ferror( m_seqNumsFile ) ) 
    throw IOException( "Unable to write to file " + m_seqNumsFileName );
  if ( fflush( m_seqNumsFile ) ) 
    throw IOException( "Unable to flush file " + m_seqNumsFileName );
}

void FileStore::setSession()
{
  rewind( m_sessionFile );
  fprintf( m_sessionFile, "%s",
           UtcTimeStampConvertor::convert( m_cache.getCreationTime() ).c_str() );
  if ( ferror( m_sessionFile ) ) 
    throw IOException( "Unable to write to file " + m_sessionFileName );
  if ( fflush( m_sessionFile ) ) 
    throw IOException( "Unable to flush file " + m_sessionFileName );
}

bool FileStore::get( int msgSeqNum, std::string& msg ) const
EXCEPT ( IOException )
{
  NumToOffset::const_iterator find = m_offsets.find( msgSeqNum );
  if ( find == m_offsets.end() ) return false;
  if ( !m_msgFile ) // KRX fork (P0-4): a failed roll back left the store closed
    throw IOException( "Store files are not open: " + m_msgFileName );
  const OffsetSize& offset = find->second;
  if ( fseek( m_msgFile, offset.first, SEEK_SET ) ) 
    throw IOException( "Unable to seek in file " + m_msgFileName );
  char* buffer = new char[ offset.second + 1 ];
  size_t result = fread( buffer, sizeof( char ), offset.second, m_msgFile );
  if ( ferror( m_msgFile ) || result != (size_t)offset.second ) 
  {
    delete [] buffer;
    throw IOException( "Unable to read from file " + m_msgFileName );
  }
  buffer[ offset.second ] = 0;
  msg = buffer;
  delete [] buffer;
  return true;
}

} //namespace FIX
