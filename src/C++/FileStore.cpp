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
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#ifdef _MSC_VER
#include <io.h>
#else
#include <unistd.h>
#endif

namespace FIX
{
namespace
{
// KRX fork (P0-5): the number written in decimal at `pos` in `data`; `pos` moves past its digits
// (a std::string ends in '\0', which stops the loop). 0 when there are none.
std::size_t digitsAt( const std::string& data, std::size_t& pos )
{
  std::size_t value = 0;
  while ( std::isdigit( static_cast<unsigned char>( data[ pos ] ) ) )
    value = ( value * 10 ) + static_cast<std::size_t>( data[ pos++ ] - '0' );
  return value;
}

// KRX fork (P0-5): the size of the FIX message that starts at `pos` in `data` ("8=", BodyLength,
// then CheckSum "10=nnn"), or 0 when none starts there or it is cut short. `msgSeqNum` gets its
// MsgSeqNum (34), or 0 when it has none.
std::size_t frameAt( const std::string& data, std::size_t pos, int& msgSeqNum )
{
  const std::size_t lengthTag = data.find( "\0019=", pos );
  if ( data.compare( pos, 2, "8=" ) != 0 || lengthTag == std::string::npos )
    return 0;
  std::size_t bodyStart = lengthTag + 3;
  const std::size_t bodyLength = digitsAt( data, bodyStart );
  const std::size_t end = bodyStart + 1 + bodyLength + 7; // SOH, body, "10=nnn" SOH
  if ( end > data.size() || data.compare( end - 7, 3, "10=" ) != 0 || data[ end - 1 ] != '\001' )
    return 0;
  const std::string frame = data.substr( pos, end - pos );
  const std::size_t seqNumTag = frame.find( "\00134=" );
  std::size_t seqNumStart = seqNumTag + 4;
  msgSeqNum = seqNumTag == std::string::npos ? 0 : static_cast<int>( digitsAt( frame, seqNumStart ) );
  return end - pos;
}

// KRX fork (15.1): writes `data` at `offset` of `file` and hands it to the operating system, in one
// system call where there is pwrite (no seek, no stdio buffer). False when not all of it was written.
bool writeAt( FILE* file, std::int64_t offset, const std::string& data )
{
#ifdef _MSC_VER
  return fseek( file, static_cast<long>( offset ), SEEK_SET ) == 0
    && fwrite( data.data(), sizeof( char ), data.size(), file ) == data.size()
    && fflush( file ) != EOF;
#else
  return pwrite( fileno( file ), data.data(), data.size(), offset ) == static_cast<ssize_t>( data.size() );
#endif
}

// KRX fork (15.1): `value` as printf's "%10.10d" writes it: at least ten digits, zero-padded.
std::string tenDigits( int value )
{
  std::string digits = std::to_string( value < 0 ? -static_cast<std::int64_t>( value ) : value );
  if ( digits.size() < 10 ) digits.insert( 0, 10 - digits.size(), '0' );
  return value < 0 ? "-" + digits : digits;
}
}

FileStore::FileStore( const UtcTimeStamp& now, std::string path, const SessionID& sessionID, bool sync )
: m_cache( now ), m_msgFile( nullptr ), m_headerFile( nullptr ), m_seqNumsFile( nullptr ),
  m_sessionFile( nullptr ), m_sync( sync )
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

  recoverUnindexed(); // KRX fork (P0-5)
  // KRX fork (15.1): set() appends at these ends, kept here instead of seeking for them each time.
  m_msgEnd = static_cast<std::int64_t>( std::filesystem::file_size( m_msgFileName ) );
  m_headerEnd = static_cast<std::int64_t>( std::filesystem::file_size( m_headerFileName ) );
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
  if ( !m_path.empty() ) return std::make_unique<FileStore>( now, m_path, sessionID, m_sync ).release();

  std::string path;
  Dictionary settings = m_settings.get( sessionID );
  path = settings.getString( FILE_STORE_PATH );
  const bool sync = settings.has( FILE_STORE_SYNC ) && settings.getBool( FILE_STORE_SYNC );
  return std::make_unique<FileStore>( now, path, sessionID, sync ).release();
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
  const std::int64_t offset = m_msgEnd;
  const std::int64_t headerEnd = m_headerEnd;
  std::size_t size = msg.size();

  // KRX fork (P0-4): the body first, then its header record ("seqnum,offset,size "), each handed to
  // the operating system; the message is in m_offsets only once both are on file. Any failure puts
  // both files back. P0-5: with FileStoreSync the body is on disk before the header record is
  // written. 15.1: each is one write at the end offset kept in memory.
  // Named error strings: Mull reads a temporary's destructor as the call (MAINTENANCE §5).
  const std::string bodyError = "Unable to write or sync file " + m_msgFileName;
  if ( !writeAt( m_msgFile, offset, msg ) || ( m_sync && !syncedToDisk( m_msgFile ) ) )
    rollBack( offset, headerEnd, bodyError );
  const std::string record = std::to_string( msgSeqNum ) + "," + std::to_string( offset ) + ","
    + std::to_string( size ) + " ";
  const std::string headerError = "Unable to write to file " + m_headerFileName;
  if ( !writeAt( m_headerFile, headerEnd, record ) )
    rollBack( offset, headerEnd, headerError );

  m_msgEnd = offset + static_cast<std::int64_t>( size );
  m_headerEnd = headerEnd + static_cast<std::int64_t>( record.size() );
  m_offsets.insert_or_assign( msgSeqNum, OffsetSize( static_cast<OffsetSize::first_type>( offset ), size ) );
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
  // KRX fork (15.1): one write at the start ("%10.10d : %10.10d", the format populateCache reads).
  const std::string seqNums
    = tenDigits( getNextSenderMsgSeqNum() ) + " : " + tenDigits( getNextTargetMsgSeqNum() );
  if ( !writeAt( m_seqNumsFile, 0, seqNums ) )
    throw IOException( "Unable to write to file " + m_seqNumsFileName );
  // KRX fork (P0-5): the body rebuilds the next sender seqnum up to its last message, so only a
  // seqnum past it (PersistMessages=N, a reset) needs a sync; the target seqnum never does.
  const int lastStored = m_offsets.empty() ? 0 : m_offsets.rbegin()->first;
  if ( m_sync && getNextSenderMsgSeqNum() > lastStored + 1 && !syncedToDisk( m_seqNumsFile ) )
    throw IOException( "Unable to sync file " + m_seqNumsFileName + ": " + error_strerror() );
}

bool FileStore::syncedToDisk( FILE* file )
{
#ifdef _MSC_VER
  const int result = _commit( _fileno( file ) );
#elif defined( __APPLE__ )
  const int result = fsync( fileno( file ) ); // macOS has no fdatasync
#else
  const int result = fdatasync( fileno( file ) );
#endif
  if ( result != 0 ) return false;
  ++m_syncCount;
  return true;
}

// KRX fork (P0-5): with FileStoreSync only the body is synced, so after a crash the header and
// seqnums files can lag behind it. The body's messages past the last header record are indexed
// again (their records written now), the next sender seqnum follows the last of them, and a
// message cut short at the end of the body is removed.
void FileStore::recoverUnindexed()
{
  std::int64_t covered = 0;
  for ( const auto& entry : m_offsets )
    covered = std::max<std::int64_t>( covered, entry.second.first + static_cast<std::int64_t>( entry.second.second ) );

  if ( fseek( m_msgFile, covered, SEEK_SET ) ) return;
  std::string tail; // usually empty: only a crash leaves messages past the last header record
  for ( int c = fgetc( m_msgFile ); c != EOF; c = fgetc( m_msgFile ) )
    tail.push_back( static_cast<char>( c ) );

  std::size_t pos = 0;
  int msgSeqNum = 0;
  for ( std::size_t size = frameAt( tail, pos, msgSeqNum ); size != 0; size = frameAt( tail, pos, msgSeqNum ) )
  {
    indexRecovered( msgSeqNum, covered + static_cast<std::int64_t>( pos ), size );
    pos += size;
  }
#ifndef _MSC_VER
  if ( pos != tail.size() && truncate( m_msgFileName.c_str(), covered + static_cast<std::int64_t>( pos ) ) != 0 )
    throw IOException( "Unable to cut the unfinished message off " + m_msgFileName );
#endif
}

void FileStore::indexRecovered( int msgSeqNum, std::int64_t offset, std::size_t size )
{
  const std::string record = std::to_string( msgSeqNum ) + "," + std::to_string( offset ) + ","
    + std::to_string( size ) + " ";
  if ( fseek( m_headerFile, 0, SEEK_END )
       || fwrite( record.data(), sizeof( char ), record.size(), m_headerFile ) != record.size()
       || fflush( m_headerFile ) == EOF )
    throw IOException( "Unable to write to file " + m_headerFileName );
  m_offsets.insert_or_assign( msgSeqNum, std::make_pair( offset, size ) );
  if ( msgSeqNum >= m_cache.getNextSenderMsgSeqNum() )
    m_cache.setNextSenderMsgSeqNum( msgSeqNum + 1 );
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
  msg.assign( buffer, offset.second ); // KRX fork (P2-4): keeps embedded NULs
  delete [] buffer;
  return true;
}

} //namespace FIX
