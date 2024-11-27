/* -*-mode:c++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */

/* Copyright 2013-2018 the Alfalfa authors
                       and the Massachusetts Institute of Technology

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:

      1. Redistributions of source code must retain the above copyright
         notice, this list of conditions and the following disclaimer.

      2. Redistributions in binary form must reproduce the above copyright
         notice, this list of conditions and the following disclaimer in the
         documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_map>

#include "yuv4mpeg.hh"
#include "paranoid.hh"
#include "encoder.hh"
#include "display.hh"
#include "ivf_reader.hh"
#include "poller.hh"
#include "socketpair.hh"
#include "socket.hh"
#include "packet.hh"
#include "pacer.hh"
#include "cong_ctrl.hh"

using namespace std;
using namespace std::chrono;
using namespace PollerShortNames;


void usage( const char *argv0 )
{
  cerr << "Usage: " << argv0 << " INPUT FPS HOST PORT CONNECTION_ID" << endl;
}

int main( int argc, char *argv[] )
{
  /* check the command-line arguments */
  if ( argc < 1 ) { /* for sticklers */
    abort();
  }

  if ( argc != 2 ) {
    usage( argv[ 0 ] );
    return EXIT_FAILURE;
  }

  /* open the YUV4MPEG input */
  //YUV4MPEGReader input { argv[ 1 ] };

  IVFReader input {argv[1]};

  /* parse the # of frames per second of playback */
  //unsigned int frames_per_second = paranoid::stoul( argv[ 2 ] );

  /* open the output */
  FileDescriptor stdout { STDOUT_FILENO };

  //const auto interval_between_frames = chrono::microseconds( int( 1.0e6 / frames_per_second ) );

  //auto next_frame_is_due = chrono::system_clock::now();

  // Test encoder
  Encoder base_encoder { input.display_width(), input.display_height(),
                         false /* two-pass */, REALTIME_QUALITY };

  auto encode_pipe = UnixDomainSocket::make_pair();

    /* construct Socket for outgoing datagrams */
  UDPSocket socket;
  socket.connect( Address( "0", "8888" ) );
  socket.set_timestamps();

  /* get connection_id */
  const uint16_t connection_id = 1337;

  Poller poller;
  Pacer pacer;

  /* counter variable */
  uint32_t frame_no = 0;
  auto start = chrono::system_clock::now();
  system_clock::time_point last_sent = system_clock::now();
  unordered_map<uint32_t, unordered_map<uint16_t, uint32_t>> pkt_nums; // TODO: in packet or map?
  unordered_map<uint32_t, unordered_map<uint16_t, uint32_t>> pkt_sent_time;

  /*TODO: Move into the congestion control class */
  bool initialized = false;
  uint32_t arrival = 0; // # of pkts
  uint32_t service = 0; // # of pkts
  //uint32_t loss = 0; // # of pkts
  typedef double Time;		 
  Time min_rtt = 0; // ms


  poller.add_action( Poller::Action( encode_pipe.second, Direction::In,
    [&]() -> Result {
      encode_pipe.second.read();

      /* wait until next frame is due */
      //this_thread::sleep_until( next_frame_is_due );
      //next_frame_is_due += interval_between_frames;
      std::chrono::duration<double, std::ratio<1, 1000>> diff = (system_clock::now() - last_sent);
      if (diff.count() < min_rtt) {
        encode_pipe.first.write( "1" );
        return ResultType::Continue;
      }
      // TODO: minRtt passed, update belief bound!



      const Optional<RasterHandle> raster = input.get_next_frame();

      if ( not raster.initialized() ) {
        return { ResultType::Exit, EXIT_FAILURE };
      }

      //const BaseRaster& rh = raster.get();
      //display.draw(rh);
    
      auto source_minihash = base_encoder.minihash();
      vector<uint8_t> output;
      if (not initialized) {
        // Probe with least amount of frames to get min RTT
        output = base_encoder.encode_with_quantizer( raster.get(), 127);
      } else {
        output = base_encoder.encode_with_quantizer( raster.get(), 127);
      }
      // Add reed-solomon code here 

      auto target_minihash = base_encoder.minihash();
      //cout << rh.display_width() << " " << rh.display_height() << " " << output.size() << " " << endl;

      FragmentedFrame ff { connection_id, source_minihash, target_minihash,
                           frame_no,
                           static_cast<uint32_t>( duration_cast<milliseconds>( system_clock::now() - last_sent).count() ),
                           output};

      // Sent out all the packets instanteneously 
      for ( const auto & packet : ff.packets() ) {
        //pacer.push( packet.to_string(), 0 );
        socket.send( packet.to_string() );
        arrival += 1;
      } 

      last_sent = system_clock::now();
      ++frame_no;
      if (initialized) {
        encode_pipe.first.write( "1" );
      }

      return ResultType::Continue;
    } ) 
  );

  poller.add_action( Poller::Action( socket, Direction::In,
    [&]()
    {
      auto packet = socket.recv();
      AckPacket ack( packet.payload );
      
      if (ack.frame_no() == 80) {
        std::chrono::duration<double> diff = (system_clock::now() - start);
        cout << "Spent: " << diff.count() << endl;
      }


      if ( ack.connection_id() != connection_id ) {
        /* this is not an ack for this session! */
        return ResultType::Continue;
      }

      service += 1;
      cout << "Ack!" << ack.frame_no() <<" "<< ack.fragment_no() << endl;

      // Need also to take loss into account
      // Need to check content of the pkt to detect dup
      if (arrival == service) {
        if (not initialized) {
          initialized = true;
          std::chrono::duration<double, std::ratio<1,1000>> diff = (system_clock::now() - last_sent);
          min_rtt = diff.count();
          cout << "Initial min_rtt is [] " << min_rtt << endl;
          encode_pipe.first.write( "1" );
        } else {
          std::chrono::duration<double, std::ratio<1,1000>> diff = (system_clock::now() - last_sent);
          if (diff.count() < min_rtt) {
            min_rtt = diff.count();
            cout << "Updated min_rtt is [] " << min_rtt << endl;
          }  
        }
      }

      return ResultType::Continue;
    } )
  );
/*
  poller.add_action( Poller::Action( socket, Direction::Out, [&]() {
        assert( pacer.ms_until_due() == 0 );

        while ( pacer.ms_until_due() == 0 ) {
          assert( not pacer.empty() );
          //cout << "sent!" << endl;
          socket.send( pacer.front() );
          arrival += 1;
          pacer.pop();
        }

        return ResultType::Continue;
    }, [&]() { return pacer.ms_until_due() == 0; } ) );
*/
  

  // Start!!!
  encode_pipe.first.write( "1" );

  while ( true ) {
    const auto poll_result = poller.poll( 2000u );
    if ( poll_result.result == Poller::Result::Type::Exit ) {
      if ( poll_result.exit_status ) {
        //cerr << "Connection error." << endl;
        continue;
      }

      return poll_result.exit_status;
    }
  }

  return EXIT_FAILURE;
}
