#include <getopt.h>

#include <cstdlib>
#include <random>
#include <unordered_map>
#include <utility>
#include <tuple>
#include <queue>
#include <deque>
#include <thread>
#include <condition_variable>
#include <future>

#include "socket.hh"
#include "packet.hh"
#include "poller.hh"
#include "optional.hh"
#include "player.hh"
#include "display.hh"
#include "paranoid.hh"
#include "procinfo.hh"

#include "reed_solomon.hpp"

using namespace std;
using namespace std::chrono;
using namespace PollerShortNames;

class AverageInterPacketDelay
{
private:
  static constexpr double ALPHA = 0.1;

  double value_ { -1.0 };
  uint64_t last_update_{ 0 };

public:
  void add( const uint64_t timestamp_us, const int32_t grace )
  {
    assert( timestamp_us >= last_update_ );

    if ( value_ < 0 ) {
      value_ = 0;
    }
    // else if ( timestamp_us - last_update_ > 0.2 * 1000 * 1000 /* 0.2 seconds */ ) {
    //   value_ /= 4;
    // }
    else {
      double new_value = max( 0l, static_cast<int64_t>( timestamp_us - last_update_ - grace ) );
      value_ = ALPHA * new_value + ( 1 - ALPHA ) * value_;
    }

    last_update_ = timestamp_us;
  }

  uint32_t int_value() const { return static_cast<uint32_t>( value_ ); }
};

void usage( const char *argv0 )
{
  cerr << "Usage: " << argv0 << " [-f, --fullscreen] [--verbose] PORT WIDTH HEIGHT" << endl;
}

uint16_t ezrand()
{
  random_device rd;
  uniform_int_distribution<uint16_t> ud;

  return ud( rd );
}

queue<RasterHandle> display_queue;
mutex mtx;
condition_variable cv;


//int main( int argc, char *argv[] )
int main()
{
  ReedSolomon rs;
  auto checkpt1 = system_clock::now();
  rs.reed_test();
  auto checkpt2 = system_clock::now();
  std::chrono::duration<double, std::ratio<1,1000>> diffec = (checkpt2 - checkpt1); 
  cout << "FEC Encoding time is " << diffec.count()  << endl;
  /* check the command-line arguments */


  /* choose a random connection_id */
  const uint16_t connection_id = 1337; // ezrand();
  cerr << "Connection ID: " << connection_id << endl;

  /* construct Socket for incoming  datagrams */
  UDPSocket socket;
  socket.bind( Address( "0", "8889" ) );
  socket.set_timestamps();

  /* frame no => FragmentedFrame; used when receiving packets out of order */
  unordered_map<size_t, FragmentedFrame> fragmented_frames;
  //size_t next_frame_no = 0;

  /* EWMA */
  AverageInterPacketDelay avg_delay;

  /* decoder states */
  Decoder decoder = Decoder {1280, 720};
  //uint32_t current_state = decoder.minihash();
  //const uint32_t initial_state = current_state;
  deque<uint32_t> complete_states;
  //auto next_ack_is_due = chrono::system_clock::now();
  const auto interval_between_acks = chrono::microseconds( 0 );
  /* memory usage logs */
  //system_clock::time_point next_mem_usage_report = system_clock::now();

  Poller poller;
  poller.add_action( Poller::Action( socket, Direction::In,
    [&]()
    {
      /* wait for next UDP datagram */
      const auto new_fragment = socket.recv();
      //cout << new_fragment.source_address.to_string() << " " << socket.local_address().to_string()  << endl;
      /* parse into Packet */
      const FECPacket fecpacket { new_fragment.payload };
      cout << "Receive:" << fecpacket.fec_frame_no_ << " " << fecpacket.pkt_no_ << endl;

      AckFECPacket ack = AckFECPacket (connection_id, fecpacket.fec_frame_no_, fecpacket.pkt_no_, "");
      ack.sendto( socket, new_fragment.source_address );   

      std::this_thread::sleep_for(std::chrono::milliseconds(1));

      return ResultType::Continue;
    },
    [&]() { return not socket.eof(); } )
  );

  /* handle events */
  while ( true ) {
    const auto poll_result = poller.poll( -1 );
    if ( poll_result.result == Poller::Result::Type::Exit ) {
      return poll_result.exit_status;
    }
  }

  return EXIT_SUCCESS;
}
