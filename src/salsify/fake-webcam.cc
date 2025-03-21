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
#include <mutex>

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
#include "slow_conv.hh"
#include "slow_conv_manual.hh"
#include "adaptive_stream.hh"

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

double current_timestamp( chrono::high_resolution_clock::time_point &start_time_point ){
  using namespace chrono;
  high_resolution_clock::time_point cur_time_point = high_resolution_clock::now();
  // convert to milliseconds, because that is the scale on which the
  // rats have been trained
  return duration_cast<duration<double>>(cur_time_point - start_time_point).count()*1000;
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
  Encoder first_encoder { input.display_width(), input.display_height(),
                         false /* two-pass */, REALTIME_QUALITY };

  Encoder second_encoder { input.display_width(), input.display_height(),
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
  std::mutex first_encoder_lock;
  std::mutex second_encoder_lock;

  /* counter variable */
  uint32_t frame_no = 0;
  [[maybe_unused]] uint32_t real_frame_no = 0;

  SeqNum pkt_no = 0;
  //auto start = chrono::system_clock::now();
  system_clock::time_point last_sent = system_clock::now();
  [[maybe_unused]] chrono::high_resolution_clock::time_point start_time_point = chrono::high_resolution_clock::now();
  unordered_map<uint32_t, unordered_map<uint16_t, SeqNum>> pkt_nums; // TODO: in packet or map?
  unordered_map<SeqNum, system_clock::time_point> pkt_sent_time;

  //SlowConvManual congctrl("./log", 1);
  CongCtrl cc; 
  ABR abr {input, connection_id};

  [[maybe_unused]] auto start = chrono::system_clock::now();

  // Skip the first small header file
  const Optional<RasterHandle> raster = input.get_next_frame();

  auto fetch_start = system_clock::now();
  poller.add_action( Poller::Action( encode_pipe.second, Direction::In,
    [&]() -> Result {
      encode_pipe.second.read();
      // Simulate 30 fps
      fetch_start = system_clock::now();
      Optional<RasterHandle> raster = input.get_next_frame();
      if ( not raster.initialized() ) {
        return { ResultType::Exit, EXIT_FAILURE };
      }
      ++frame_no;
      abr.add_fetch_frame(raster.get());
      encode_pipe.first.write( "1" );

      return ResultType::Continue;
    }, [&]() { 
      std::chrono::duration<double, std::ratio<1,1000>> diff = (system_clock::now() - fetch_start); // in millis
      return diff.count() >= (33); 
    } ) 
  );

    // only send new frames after min_rtt
  poller.add_action( Poller::Action( update_pipe.second, Direction::In, [&]() {
      update_pipe.second.read();
      // update history and state of cong_ctrl 
      cc.updateHistory();
      cc.updateBeliefBound();
      abr.add_fec(pacer, cc);
      abr.encode_fetched_frames(cc.beliefs.min_c * Packet::MAXIMUM_PAYLOAD);
      last_sent = system_clock::now();
      update_pipe.first.write( "1" );
      return ResultType::Continue;
    }, [&]() { 
      std::chrono::duration<double, std::ratio<1,1000>> diff = (system_clock::now() - last_sent); // in millis
      return diff.count() >= (100); 
  } ) );

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

  poller.add_action( Poller::Action( socket, Direction::Out, [&]() {
      assert( pacer.ms_until_due() == 0 );
      //double cur_time;
      while ( pacer.ms_until_due() == 0 ) {
        assert( not pacer.empty() );

        //cur_time = current_timestamp( start_time_point );
        //congctrl.set_timestamp(cur_time);

        string to_sent = pacer.front();
        //FECPacket packet = FECPacket{to_sent};
        socket.send( to_sent );
        pacer.pop();
        //pkt_nums[packet.frame_no_][packet.pkt_no_] = pkt_no;
        //pkt_sent_time[pkt_no] = system_clock::now();

        //congctrl.onPktSent( pkt_no );
        ++pkt_no;
      }

      if (pacer.empty()) {
        update_pipe.first.write( "1" );
      }

      return ResultType::Continue;
  }, [&]() { 
    //std::chrono::duration<double, std::ratio<1,1000>> diff = (system_clock::now() - last_sent); // in millis
    //return (diff.count() >= rtprop) && pacer.ms_until_due() == 0; 
    return pacer.ms_until_due() == 0; } ) );


  // Start!!!
  encode_pipe.first.write( "1" );
  update_pipe.first.write( "1" );

  while ( true ) {
    const auto poll_result = poller.poll(0);
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
      // this thread will spawn all the encoding jobs and will wait on the results
      /** 
      thread(
        [raster, &cc, &first_encoder, &second_encoder, connection_id, &frame_no, &real_frame_no, &first_encoder_lock, &second_encoder_lock, &start]()
        {
          auto encode_start = system_clock::now();

          uint32_t source_minihash;
          vector<uint8_t> output;
          uint32_t target_minihash; 
          Encoder encoder_copy_first = first_encoder;
          Encoder encoder_copy_second = second_encoder;
          size_t target_size = 1 * cc.beliefs.min_c;
          uint32_t frame_no_to_use = 0;
          if (frame_no % 2  == 1) {
            first_encoder_lock.lock();
            source_minihash = first_encoder.minihash();
            //output = first_encoder.encode_with_quantizer( raster.get(), 120);
            output = first_encoder.encode_with_target_size( raster.get(), target_size * Packet::MAXIMUM_PAYLOAD);
            target_minihash = first_encoder.minihash();
            // Skip the frame if too big
            if (output.size() > 3 * target_size * Packet::MAXIMUM_PAYLOAD) {
              first_encoder = move(encoder_copy_first);
              cout << "Oversize" << endl;
              first_encoder_lock.unlock();
              return;
            }
            real_frame_no++;
            frame_no_to_use = real_frame_no;
            first_encoder_lock.unlock();
          } else {
            second_encoder_lock.lock();
            source_minihash = second_encoder.minihash();
            //output = second_encoder.encode_with_quantizer( raster.get(), 120);
            output = second_encoder.encode_with_target_size( raster.get(), target_size * Packet::MAXIMUM_PAYLOAD);
            target_minihash = second_encoder.minihash();
            // Skip the frame if too big
            if (output.size() > 3 * target_size * Packet::MAXIMUM_PAYLOAD) {
              second_encoder = move(encoder_copy_second);
              cout << "Oversize" << endl;
              second_encoder_lock.unlock();
              return;
            }
            real_frame_no++;
            frame_no_to_use = real_frame_no;
            second_encoder_lock.unlock();
          }

          FragmentedFrame ff { connection_id, source_minihash, target_minihash,
                               frame_no_to_use,
                               0,
                               output};

          cout << "Output size is: " << ff.packets().size() << endl;
          // FEC
          //FECFrame fecframe {ff.packets(), ff.connection_id(), ff.frame_no(), (uint16_t)(256 - ff.packets().size())};

          auto encode_end = system_clock::now();
          std::chrono::duration<double, std::ratio<1,1000>> encode_duration = (encode_end - encode_start);
          cout << "Encoding of " <<  ff.frame_no() << " takes: " << encode_duration.count() << endl;

          //if (frame_no == 80) {
          //  auto end = system_clock::now();
          //  std::chrono::duration<double, std::ratio<1,1000>> full_encode_duration = (end - start);
          //  cout << "Encoding totally takes: " << full_encode_duration.count() << endl;
          //}

          // Add packets into pacer??
          //for ( const auto & packet : fecframe.fecpkts ) {
          //  pacer.push( packet.to_string(), 0);
            //pkt_nums[packet.frame_no_][packet.pkt_no_] = pkt_no;
            //pkt_sent_time[pkt_no] = system_clock::now();
            //socket.send( packet.to_string() );
            //cout << "Send:" << packet.pkt_no_ << endl;
            //cc.onSent();
            //++pkt_no;
          //} 
        }
      ).detach();
      **/