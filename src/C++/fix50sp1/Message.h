#ifndef FIX50SP1_MESSAGES_H
#define FIX50SP1_MESSAGES_H

#include "../Message.h"
#include "../Group.h"

namespace FIX50SP1
{
  class Header : public FIX::Header
  {
  public:
  };

  class Trailer : public FIX::Trailer
  {
  public:
  };

  class Message : public FIX::Message
  {
  public:
    Message( const FIX::MsgType& msgtype )
    : FIX::Message(
      FIX::BeginString("FIXT.1.1"), msgtype )
     { getHeader().setField( FIX::ApplVerID("8") ); }

    Message(const FIX::Message& m) : FIX::Message(m) {}
    Message(const Message& m) = default;
    Message(Message&& m) = default;
    Message& operator=(Message&&) = default;
    Message& operator=(const Message&) = default;
  };

}

#endif
