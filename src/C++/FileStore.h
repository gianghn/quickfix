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

#ifndef FIX_FILESTORE_H
#define FIX_FILESTORE_H

#ifdef _MSC_VER
#pragma warning( disable : 4503 4355 4786 4290 )
#endif

#include "MessageStore.h"
#include "SessionSettings.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace FIX
{
class Session;

/// Creates a file based implementation of MessageStore.
class FileStoreFactory : public MessageStoreFactory
{
public:
  FileStoreFactory( const SessionSettings& settings )
: m_settings( settings ) {};
  FileStoreFactory( const std::string& path )
: m_path( path ) {};
  // KRX fork (P0-5): every store this factory makes syncs to disk when `sync`.
  FileStoreFactory( std::string path, bool sync )
: m_path( std::move( path ) ), m_sync( sync ) {}

  MessageStore* create( const UtcTimeStamp&, const SessionID& );
  void destroy( MessageStore* );
private:
  std::string m_path;
  SessionSettings m_settings;
  bool m_sync = false; // KRX fork (P0-5): with a path, every store syncs (with settings: FileStoreSync)
};
/*! @} */

/**
 * File based implementation of MessageStore.
 *
 * Four files are created by this implementation.  One for storing outgoing
 * messages, one for indexing message locations, one for storing sequence numbers,
 * and one for storing the session creation time.
 *
 * The formats of the files are:<br>
 * &nbsp;&nbsp;
 *   [path]+[BeginString]-[SenderCompID]-[TargetCompID].body<br>
 * &nbsp;&nbsp;
 *   [path]+[BeginString]-[SenderCompID]-[TargetCompID].header<br>
 * &nbsp;&nbsp;
 *   [path]+[BeginString]-[SenderCompID]-[TargetCompID].seqnums<br>
 * &nbsp;&nbsp;
 *   [path]+[BeginString]-[SenderCompID]-[TargetCompID].session<br>
 *
 *
 * The messages file is a pure stream of %FIX messages.<br><br>
 * The sequence number file is in the format of<br>
 * &nbsp;&nbsp;
 *   [SenderMsgSeqNum] : [TargetMsgSeqNum]<br><br>
 * The session file is a UTC timestamp in the format of<br>
 * &nbsp;&nbsp;
 *   YYYYMMDD-HH:MM:SS
 */
class FileStore : public MessageStore
{
public:
  // KRX fork (15.6): with `flushIntervalMs` > 0 and `sync` off, a background thread flushes the store
  // to disk that often (FileStoreFlushInterval).
  FileStore( const UtcTimeStamp& now, std::string path, const SessionID& sessionID, bool sync = false,
             int flushIntervalMs = 0 );
  virtual ~FileStore();

  // KRX fork (P0-5): how many file syncs the store has done (FileStoreSync=Y); a stored message
  // costs one (its body). 15.6: a background flush costs two (body, seqnums).
  [[nodiscard]] std::uint64_t getSyncCount() const { return m_syncCount.load(); }

  bool set( int, const std::string& ) EXCEPT ( IOException );
  void get( int, int, std::vector<std::string>& ) const EXCEPT ( IOException );

  int getNextSenderMsgSeqNum() const EXCEPT ( IOException );
  int getNextTargetMsgSeqNum() const EXCEPT ( IOException );
  void setNextSenderMsgSeqNum( int value ) EXCEPT ( IOException );
  void setNextTargetMsgSeqNum( int value ) EXCEPT ( IOException );
  void incrNextSenderMsgSeqNum() EXCEPT ( IOException );
  void incrNextTargetMsgSeqNum() EXCEPT ( IOException );

  UtcTimeStamp getCreationTime() const EXCEPT ( IOException );

  void reset( const UtcTimeStamp& now ) EXCEPT ( IOException );
  void refresh() EXCEPT ( IOException );

  // KRX fork (15.7): with FileStoreSync, set() skips its sync while a batch is open; endBatch syncs
  // the body once for the whole batch.
  void beginBatch() override;
  bool endBatch() override;

protected:
  // KRX fork (P0-5, 15.6): forces the file open as `fd` to disk; true once it is there.
  virtual bool syncFile( int fd );
  // KRX fork (15.6): ends the background flush, after a last flush of what is not on disk yet. A
  // subclass that overrides syncFile calls it in its own destructor.
  void stopFlusher();

private:
#ifdef _MSC_VER
  typedef std::pair<int, std::size_t> OffsetSize;
#else
  typedef std::pair<long, std::size_t> OffsetSize;
#endif
  typedef std::map<int, OffsetSize> NumToOffset;

  void open( bool deleteFile );
  void populateCache();
  bool readFromFile( int offset, int size, std::string& msg );
  void setSeqNum();
  void setSession();
  // KRX fork (P0-5): FileStoreSync. True once `file` is on disk (syncFile, counted).
  bool syncedToDisk( FILE* file );
  // KRX fork (P0-5): indexes the body's messages past the last header record (after a crash).
  void recoverUnindexed();
  void indexRecovered( int msgSeqNum, std::int64_t offset, std::size_t size );
  // KRX fork (P0-4): undoes a failed set() and throws.
  void rollBack( std::int64_t msgEnd, std::int64_t headerEnd, const std::string& error );

  bool get( int, std::string& ) const EXCEPT ( IOException );

  MemoryStore m_cache;
  NumToOffset m_offsets;

  std::string m_msgFileName;
  std::string m_headerFileName;
  std::string m_seqNumsFileName;
  std::string m_sessionFileName;

  FILE* m_msgFile;
  FILE* m_headerFile;
  FILE* m_seqNumsFile;
  FILE* m_sessionFile;

  // KRX fork (P0-5): FileStoreSync. Only the body is synced: the header and the next sender seqnum
  // are rebuilt from it after a crash (recoverUnindexed), and a next target seqnum that is behind
  // only makes the counterparty resend.
  bool m_sync = false;
  bool m_inBatch = false;      // KRX fork (15.7)
  bool m_batchWritten = false; // KRX fork (15.7): a message was stored in the open batch
  std::atomic<std::uint64_t> m_syncCount{ 0 };
  // KRX fork (15.1): where the next body and header record go: the files' ends, kept in memory.
  std::int64_t m_msgEnd = 0;
  std::int64_t m_headerEnd = 0;

  // KRX fork (15.6): the background flush. Writes mark the store unflushed; every interval the
  // flusher syncs copies of the body and seqnums descriptors (taken under m_flushMutex, which
  // open() and rollBack() hold while they swap the files), so a write never waits for a sync. A
  // failed flush is kept and fails the next set(). The thread starts with the first write after
  // construction, so it never calls a subclass's syncFile before that subclass exists.
  void markUnflushed();
  void flushLoop();
  bool flushOnce( bool viaOverride );
  bool syncCopy( int fd, bool viaOverride );
  void endFlusher( bool viaOverride );
  int m_flushIntervalMs = 0;
  bool m_constructed = false;
  std::thread m_flusher;
  std::mutex m_flushMutex;
  std::condition_variable m_flushWake;
  bool m_flusherStop = false;         // guarded by m_flushMutex
  std::string m_flushError;           // guarded by m_flushMutex
  std::atomic<bool> m_unflushed{ false };
  std::atomic<bool> m_flushFailed{ false };
};
}

#endif //FIX_FILESTORE_H
