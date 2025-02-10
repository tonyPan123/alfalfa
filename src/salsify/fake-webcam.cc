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

#include "reed_solomon.hpp"

using namespace std;
using namespace std::chrono;
using namespace PollerShortNames;

struct From_Rust {
  int a;
  int b;
};
/*
struct BeliefBound {
    double min_c;
    double max_c;
    double min_b;
    double max_b;
    double min_q;
    double max_q;
};

extern "C" {
    //From_Rust rust_function(From_Rust*);
    struct BeliefBound compute_belief_bounds_c_test();
    
}
*/

void usage( const char *argv0 )
{
  cerr << "Usage: " << argv0 << " INPUT FPS HOST PORT CONNECTION_ID" << endl;
}


int main( int argc, char *argv[] )
{  ReedSolomon rs;
  rs.reed_test();
  //return 1;
  //fec_test();
  //From_Rust in = {true, 1000};
  //From_Rust ret = rust_function(&in);
  //BeliefBound bb = compute_belief_bounds_c_test();
  //cout << "Pidan: " << bb.min_c << " " << bb.max_c << endl; 
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

  Encoder minimum_encoder { input.display_width(), input.display_height(),
                         false /* two-pass */, REALTIME_QUALITY };

  auto encode_pipe = UnixDomainSocket::make_pair();
  auto update_pipe = UnixDomainSocket::make_pair();

    /* construct Socket for outgoing datagrams */
  UDPSocket socket;
  socket.connect( Address( "0", "8889" ) );
  socket.set_timestamps();

  /* get connection_id */
  const uint16_t connection_id = 1337;

  Poller poller;
  Pacer pacer;

  /* counter variable */
  uint32_t frame_no = 0;
  SeqNum pkt_no = 0;
  //auto start = chrono::system_clock::now();
  system_clock::time_point last_sent = system_clock::now();
  unordered_map<uint32_t, unordered_map<uint16_t, SeqNum>> pkt_nums; // TODO: in packet or map?
  unordered_map<SeqNum, system_clock::time_point> pkt_sent_time;
  CongCtrl cc; 

  auto start = chrono::system_clock::now();

  // Skip the first small header file
  const Optional<RasterHandle> raster = input.get_next_frame();

  poller.add_action( Poller::Action( socket, Direction::In,
    [&]()
    {
      auto packet = socket.recv();
      AckFECPacket ack( packet.payload );

      if ( ack.connection_id_ != connection_id ) {
        /* this is not an ack for this session! */
        return ResultType::Continue;
      }


      // Need also to take loss into account
      // Need to check content of the pkt to detect dup
      uint32_t pkt_num = pkt_nums[ack.frame_no_][ack.pkt_no_];
      std::chrono::duration<double, std::ratio<1,1000>> diff = (system_clock::now() - pkt_sent_time[pkt_num]); // in millis
      cc.onACK(pkt_num, diff.count());
      cout << "Get Ack!" << ack.frame_no_ <<" "<< ack.pkt_no_ << " " << diff.count() << endl;

      return ResultType::Continue;
    } )
  );

  poller.add_action( Poller::Action( encode_pipe.second, Direction::In,
    [&]() -> Result {
      encode_pipe.second.read();

      /* wait until next frame is due */
      //this_thread::sleep_until( next_frame_is_due );
      //next_frame_is_due += interval_between_frames;
      // TODO: minRtt passed, update belief bound!

      //system_clock::time_point before_encoding = system_clock::now();

      //const BaseRaster& rh = raster.get();
      //display.draw(rh);
      /*
      if (frame_no == 51) {
        for (int i = 8000; i <= 200000; i = i + 1000) {
          auto checkptx = system_clock::now();
          Encoder test_encoder { input.display_width(), input.display_height(),
                         false , REALTIME_QUALITY };
          vector<uint8_t> zz = test_encoder.encode_with_target_size( raster.get(), i);
          auto checkptz = system_clock::now();
          std::chrono::duration<double, std::ratio<1,1000>> diff = (checkptz - checkptx);
          cout << i << " " << zz.size() << " " << diff.count() << endl;
        }
      } */
      ++frame_no;
      cout << "Add new thread!" << endl;

      // this thread will spawn all the encoding jobs and will wait on the results
      thread(
        [&update_pipe, &cc, &base_encoder, &input, &frame_no, &connection_id, &pacer, &start]()
        {
          const Optional<RasterHandle> raster = input.get_next_frame();
          if ( not raster.initialized() ) {
            std::chrono::duration<double> diff = (system_clock::now() - start);
            cout << "Spent: " << diff.count() << endl;
            //return { ResultType::Exit, EXIT_FAILURE };
          }

          auto source_minihash = base_encoder.minihash();
          //vector<uint8_t> output = base_encoder.encode_with_quantizer( raster.get(), 3);
          cout << "The bb is " << cc.beliefs.min_c <<":" << cc.beliefs.min_c * Packet::MAXIMUM_PAYLOAD << endl;
          auto checkpt1 = system_clock::now();
          vector<uint8_t> output = base_encoder.encode_with_target_size( raster.get(), cc.beliefs.min_c  * Packet::MAXIMUM_PAYLOAD);
          auto checkpt2 = system_clock::now();
          //vector<uint8_t> mini_output = minimum_encoder.encode_with_quantizer( raster.get(), 3);
          //cout << "Maximize: " << output.size() << endl;
          //auto checkpt3 = system_clock::now();
          std::chrono::duration<double, std::ratio<1,1000>> diffec = (checkpt2 - checkpt1); // in millis
          //std::chrono::duration<double, std::ratio<1,1000>> diff2 = (checkpt3 - checkpt2); // in millis
          cout << "Encoding time is " << diffec.count()  << endl;
          // Add reed-solomon code here 
    
          auto target_minihash = base_encoder.minihash();
          //cout << "Encoding out: " << output.size() << " while target is "  
            //<< 60 * Packet::MAXIMUM_PAYLOAD << endl;
          cout << "Encoding out: " << output.size() << endl;
    
          FragmentedFrame ff { connection_id, source_minihash, target_minihash,
                               frame_no,
                               0,
                               output};
          // FEC
          //cout << "Go go!" << endl;
          cout << "FEC size is: " << (uint16_t)(cc.get_cca_action() - ff.packets().size()) << endl;
          //cout << "Go go!" << endl;
          FECFrame fecframe {ff.packets(), ff.connection_id(), ff.frame_no(), (uint16_t)(cc.get_cca_action() - ff.packets().size())};
          auto checkpt3 = system_clock::now();
          std::chrono::duration<double, std::ratio<1,1000>> diff = (checkpt3 - checkpt2);
          cout << "FEC time is: " << diff.count() << endl;
          // Add packets into pacer
          for ( const auto & packet : fecframe.fecpkts ) {
            pacer.push( packet.to_string(), 0);
            //pkt_nums[packet.frame_no_][packet.pkt_no_] = pkt_no;
            //pkt_sent_time[pkt_no] = system_clock::now();
            //socket.send( packet.to_string() );
            //cout << "Send:" << packet.pkt_no_ << endl;
            //cc.onSent();
            //++pkt_no;
          } 
          update_pipe.first.write( "1" );
        }
      ).detach();




      return ResultType::Continue;
    } ) 
  );

  //SeqNum sent_pkt_count = 0;
  //system_clock::time_point last_sent_pkt = system_clock::now();
  //int rtprop = 200;

  poller.add_action( Poller::Action( socket, Direction::Out, [&]() {
      assert( pacer.ms_until_due() == 0 );

      while ( pacer.ms_until_due() == 0 ) {
        assert( not pacer.empty() );
        //cout << "sent!" << endl;
        string to_sent = pacer.front();
        FECPacket packet = FECPacket{to_sent};

        socket.send( to_sent );
        pacer.pop();
        pkt_nums[packet.frame_no_][packet.pkt_no_] = pkt_no;
        pkt_sent_time[pkt_no] = system_clock::now();
        ++pkt_no;
        // Utilize cwnd to bound???
        cc.onSent();
      }
      last_sent = system_clock::now();
      //last_sent_pkt = system_clock::now();

      return ResultType::Continue;
  }, [&]() { 
    //std::chrono::duration<double, std::ratio<1,1000>> diff = (system_clock::now() - last_sent); // in millis
    //return (diff.count() >= rtprop) && pacer.ms_until_due() == 0; 
    return pacer.ms_until_due() == 0; } ) );

  int grace_period = 15;
  // only send new frames after min_rtt
  poller.add_action( Poller::Action( update_pipe.second, Direction::In, [&]() {
      update_pipe.second.read();
      // update history and state of cong_ctrl 
      cc.updateHistory();
      cc.updateBeliefBound();
      encode_pipe.first.write( "1" );
      return ResultType::Continue;
    }, [&]() { 
      std::chrono::duration<double, std::ratio<1,1000>> diff = (system_clock::now() - last_sent); // in millis
      return pacer.empty() && diff.count() >= (cc.get_action_intertime() + grace_period); 
  } ) );


  // Start!!!
  encode_pipe.first.write( "1" );

  while ( true ) {
    const auto poll_result = poller.poll(grace_period);
    if ( poll_result.result == Poller::Result::Type::Exit ) {
      if ( poll_result.exit_status ) {
        //cerr << "Connection error." << endl;
        continue;
      }

      return poll_result.exit_status;
    }
    // Advanced to next action? 
    
  }

  return EXIT_FAILURE;
}
