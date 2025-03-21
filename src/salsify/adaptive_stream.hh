#ifndef ADAPTIVE_STREAM_HH
#define ADAPTIVE_STREAM_HH

#include <iostream>
#include <boost/circular_buffer.hpp>
#include <vector>
#include <thread>
#include <future>

#include "yuv4mpeg.hh"
#include "packet.hh"
#include "encoder.hh"
#include "ivf_reader.hh"
#include "reed_solomon.hpp"
#include "pacer.hh"

using namespace std;

struct EncodeJob
{
    RasterHandle raster;
    
    Encoder encoder;

    size_t target_size;

    EncodeJob(RasterHandle raster, const Encoder & encoder, const size_t target_size )
        : raster( raster ), encoder( encoder ), target_size( target_size )
    {}
};

struct EncodeOutput
{
    Encoder encoder;
    vector<uint8_t> frame;
    uint32_t source_minihash;

    EncodeOutput( Encoder && encoder, vector<uint8_t> && frame,
                    const uint32_t source_minihash)
        : encoder( std::move( encoder ) ), frame( std::move( frame ) ),
        source_minihash( source_minihash)
        {}
};

EncodeOutput do_encode_job( EncodeJob && encode_job )
{
    std::vector<uint8_t> output;
    uint32_t source_minihash = encode_job.encoder.minihash();

    output = encode_job.encoder.encode_with_target_size( encode_job.raster.get(),
                                                        encode_job.target_size );
    //output = encode_job.encoder.encode_with_quantizer( encode_job.raster.get(),
    //                                                    3 );
    return { move( encode_job.encoder ), move( output ), source_minihash};
}


class ABR {
    public: 
        // Fetched frames per RTT 
        vector<RasterHandle> fetched_frames;
        // Encoders should be parallel (>= 2)
        Encoder encoder{ 1280, 720, false, REALTIME_QUALITY };
        // Can only consider 2 RTTs' frames for each abr action  
        static constexpr unsigned MAX_NUM_RTTS = 2;
        FECPre encoded_output_by_rtt[MAX_NUM_RTTS];

        /* where we keep the outputs of parallel encoding jobs */
        vector<EncodeJob> encode_jobs;
        vector<future<EncodeOutput>> encode_outputs;

        uint16_t connection_id;
        uint32_t frame_no;
        uint32_t fec_frame_no;

        ABR(IVFReader & input, uint16_t _conenction_id) {
            encoder = Encoder{ input.display_width(), input.display_height(),
                                    false /* two-pass */, REALTIME_QUALITY };
            connection_id = _conenction_id;
            frame_no = 0;
            for (unsigned i = 0; i < MAX_NUM_RTTS; i++) {
                encoded_output_by_rtt[i] = FECPre{connection_id};
            }
        }

        void add_fetch_frame(RasterHandle & raster) {
            fetched_frames.push_back(raster);
        }

        void encode_fetched_frames([[maybe_unused]] size_t target_sizes) {
            if (fetched_frames.size() == 0) {
                return;
            }
            for (RasterHandle & raster : fetched_frames) {
                encode_jobs.emplace_back(raster, encoder, target_sizes / fetched_frames.size() /*TODO: change to min_c later */);
            }
            fetched_frames.clear();

            thread(
                [this]()
                {
                    encode_outputs.clear();
                    encode_outputs.reserve( encode_jobs.size() );

                    for ( auto & job : encode_jobs ) {
                        encode_outputs.push_back(
                            async( (launch::async),
                                do_encode_job, move( job ) ) );
                    }
                
                    for ( auto & future_res : encode_outputs ) {
                        future_res.wait();
                    }

                    cout << "Encoding Job done!!!" << endl;
                }
            ).detach();
        }

        // Assume all jobs are finished before calling
        void add_fec([[maybe_unused]] Pacer & pacer, [[maybe_unused]] CongCtrl & cc) {
            encode_jobs.clear();
            vector<EncodeOutput> good_outputs;
            for ( auto & out_future : encode_outputs ) {
              if ( out_future.valid() ) {
                good_outputs.push_back( move( out_future.get() ) );
              }
            }
            // Collect frames
            // Our algorithm can only handle up to MAX_NUM_RTTS frames
            assert(MAX_NUM_RTTS >= 2);
            for (unsigned i = 0; i < MAX_NUM_RTTS; i++) {
                if (i == 0) {
                    encoded_output_by_rtt[i].merge(encoded_output_by_rtt[i + 1]);
                    //encoded_output_by_rtt[i] = encoded_output_by_rtt[i + 1];
                } else {
                    if (i < MAX_NUM_RTTS - 1) {
                        encoded_output_by_rtt[i] = encoded_output_by_rtt[i + 1];
                    }
                }
            }
            vector<FragmentedFrame> encode_output = {};
            FECPre frames(connection_id);
            for ( size_t i = 0; i < good_outputs.size(); i++ ) {
                cout << "Pidan: " << good_outputs[ i ].frame.size() << endl;
                frame_no++;
                FragmentedFrame ff { connection_id, good_outputs[ i ].source_minihash, good_outputs[ i ].encoder.minihash(),
                    frame_no,
                    0 /* Unused */,
                    good_outputs[ i ].frame };
                frames.add_frame(frame_no, ff.packets());
            }
            // The frames added into FECPre will be sent anyway
            encoded_output_by_rtt[MAX_NUM_RTTS - 1] = frames;
            if (good_outputs.size() > 0) {
                encoder = move(good_outputs[ good_outputs.size() - 1 ].encoder);
            }
    
            // Sending behaviour and add FEC
            if (encoded_output_by_rtt[0].initialized) {
                FECPre & focus = encoded_output_by_rtt[0];
                //assert((uint32_t)focus.total_len <= (uint32_t)cc.beliefs.min_c);
                if (focus.total_len <= (uint32_t)cc.beliefs.min_c) {
                    fec_frame_no++;
                    cout << "Checkpoint1: " << cc.beliefs.min_c << " " << focus.total_len << " " <<(uint16_t)(cc.beliefs.min_c - focus.total_len) << endl;
                    FECFrame fec_frame {fec_frame_no, focus, (uint16_t)(cc.beliefs.min_c - focus.total_len)};
                    encoded_output_by_rtt[0] =  FECPre{connection_id};
                    //for (FECPacket & pkt : fec_frame.pkts) {
                        //pacer.push( pkt.to_string(), 0);
                    //}
                    cout << "gegeda1 " << fec_frame.pkts.size() << endl;
                }
            }
        }
};


#endif