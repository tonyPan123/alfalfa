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
  size_t next_frame_no = 0;

  /* EWMA */
  AverageInterPacketDelay avg_delay;

  /* decoder states */
  Decoder decoder = Decoder {1280, 720};
  uint32_t current_state = decoder.minihash();
  const uint32_t initial_state = current_state;
  deque<uint32_t> complete_states;
  auto next_ack_is_due = chrono::system_clock::now();
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
      const Packet packet { new_fragment.payload };

      if ( packet.frame_no() < next_frame_no ) {
        /* we're not interested in this anymore */
        return ResultType::Continue;
      }
      else if ( packet.frame_no() > next_frame_no ) {
        /* current frame is not finished yet, but we just received a packet
           for the next frame, so here we just encode the partial frame and
           display it and move on to the next frame */
        cerr << "got a packet for frame #" << packet.frame_no()
             << ", display previous frame(s)." << endl;

        for ( size_t i = next_frame_no; i < packet.frame_no(); i++ ) {
          if ( fragmented_frames.count( i ) == 0 ) continue;

          //enqueue_frame( player, fragmented_frames.at( i ).partial_frame() );
          fragmented_frames.erase( i );
        }

        next_frame_no = packet.frame_no();
        current_state = decoder.minihash();
      }

      /* add to current frame */
      if ( fragmented_frames.count( packet.frame_no() ) ) {
        fragmented_frames.at( packet.frame_no() ).add_packet( packet );
      } else {
        /*
          This was judged "too fancy" by the Code Review Board of Dec. 29, 2016.

          fragmented_frames.emplace( std::piecewise_construct,
                                     forward_as_tuple( packet.frame_no() ),
                                     forward_as_tuple( connection_id, packet ) );
        */

        fragmented_frames.insert( make_pair( packet.frame_no(),
                                             FragmentedFrame( connection_id, packet ) ) );
      }

      /* is the next frame ready to be decoded? Assume no loss */
      if ( fragmented_frames.count( next_frame_no ) > 0 and fragmented_frames.at( next_frame_no ).complete() ) {
        auto & fragment = fragmented_frames.at( next_frame_no );

        uint32_t expected_source_state = fragment.source_state();

        if ( current_state == expected_source_state and
             expected_source_state != initial_state ) {
          /* Currently assume that there is no loss of frame*/

        }

        // here we apply the frame
        const Optional<RasterHandle> raster = decoder.parse_and_decode_frame(fragment.frame());
        //cout << "Decoding!!"  << next_frame_no << " "  << endl;

        // state "after" applying the frame
        current_state = decoder.minihash();

        if ( current_state != fragment.target_state()) {
          /* this is a full state. let's save it */
            cout << "Bad!!" << endl;
        }

        fragmented_frames.erase( next_frame_no );
        next_frame_no++;
      }

      avg_delay.add( new_fragment.timestamp_us, packet.time_since_last() );

      this_thread::sleep_until( next_ack_is_due );
      next_ack_is_due += interval_between_acks;

      AckPacket( connection_id, packet.frame_no(), packet.fragment_no(),
                 avg_delay.int_value(), current_state,
                 complete_states ).sendto( socket, new_fragment.source_address );
      cout << "Ack!" << packet.frame_no() <<" "<< packet.fragment_no() << endl;

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
