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

const int MILLI_TO_MICRO = 1000;

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

        ABR(uint16_t _conenction_id, Encoder & encoder) 
            : encoder(move(encoder))
        {
            connection_id = _conenction_id;
            frame_no = 0;
            for (unsigned i = 0; i < MAX_NUM_RTTS; i++) {
                encoded_output_by_rtt[i] = FECPre{connection_id};
            }
        }

        void add_fetch_frame(IVFReader & reader) {
            // TODO: Change to thread pool later???
            thread([this, &reader]() {
                Optional<RasterHandle> raster = reader.get_next_frame();
                if ( raster.initialized() ) {
                    fetched_frames.push_back(raster.get());
                }
            }).detach();
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

                // Algorithm pipeline: 
                // 1. latency deadline
                // 2. Max allowed rate 
                if ((int)focus.total_len <= (int)(cc.beliefs.min_c - cc.beliefs.max_q)) {
                    if (!encoded_output_by_rtt[1].initialized) {
                        int target_size = 2 * cc.beliefs.min_c - cc.beliefs.max_q;
                        fec_frame_no++;
                        FECFrame fec_frame {fec_frame_no, focus, (uint16_t)(target_size - focus.total_len)};
                        encoded_output_by_rtt[0] =  FECPre{connection_id};
                        int pkt_interdelay = (cc.beliefs.min_rtt) / fec_frame.total_pkts; 
                        for (FECPacket & pkt : fec_frame.pkts) {
                            pacer.push( pkt.to_string(), pkt_interdelay * MILLI_TO_MICRO);
                        }
                    } else {
                        FECPre & next_focus = encoded_output_by_rtt[1];
                        if ((focus.total_len + next_focus.total_len) <= (uint32_t)cc.no_loss_rate) {
                            uint16_t extra_fec = (uint16_t)cc.no_loss_rate - (uint16_t)(focus.total_len + next_focus.total_len);
                            fec_frame_no++;
                            FECFrame fec_frame1 {fec_frame_no, focus, extra_fec};
                            encoded_output_by_rtt[0] =  FECPre{connection_id};
                            fec_frame_no++;
                            FECFrame fec_frame2 {fec_frame_no, next_focus, extra_fec};
                            encoded_output_by_rtt[1] =  FECPre{connection_id};
                            int pkt_interdelay = (cc.beliefs.min_rtt) / (fec_frame1.total_pkts + fec_frame2.total_pkts); 
                            for (FECPacket & pkt : fec_frame1.pkts) {
                                pacer.push( pkt.to_string(), pkt_interdelay * MILLI_TO_MICRO);
                            }
                            for (FECPacket & pkt : fec_frame2.pkts) {
                                pacer.push( pkt.to_string(), pkt_interdelay * MILLI_TO_MICRO);
                            }
                        } else {
                            cout << "Somewhat Bad" << endl;  
                        }
                    }
                } else {
                    // can not catch up the deadline
                    cout << "Very Bad" << endl;
                }

                /*
                if (focus.total_len <= (uint32_t)target_size) {
                    fec_frame_no++;
                    cout << "Checkpoint1: " << cc.beliefs.min_c << " " << focus.total_len << " " <<(uint16_t)(target_size - focus.total_len) << endl;
                    FECFrame fec_frame {fec_frame_no, focus, (uint16_t)(target_size - focus.total_len)};
                    encoded_output_by_rtt[0] =  FECPre{connection_id};
                    int pkt_interdelay = (cc.beliefs.min_rtt) / fec_frame.total_pkts; 
                    for (FECPacket & pkt : fec_frame.pkts) {
                        pacer.push( pkt.to_string(), pkt_interdelay * MILLI_TO_MICRO);
                    }
                    cout << "Inter-delay is  " << pkt_interdelay << endl; 
                    cout << "gegeda1 " << fec_frame.pkts.size() << endl;
                } else {
                    fec_frame_no++;
                    cout << "Checkpoint1: " << cc.beliefs.min_c << " " << focus.total_len << " " <<(uint16_t)(target_size - focus.total_len) << endl;
                    FECFrame fec_frame {fec_frame_no, focus, 0};
                    encoded_output_by_rtt[0] =  FECPre{connection_id};
                    int pkt_interdelay = (cc.beliefs.min_rtt) / fec_frame.total_pkts; 
                    for (FECPacket & pkt : fec_frame.pkts) {
                        pacer.push( pkt.to_string(), pkt_interdelay * MILLI_TO_MICRO);
                    }
                    cout << "Inter-delay is  " << pkt_interdelay << endl; 
                    cout << "gegeda1 " << fec_frame.pkts.size() << endl;
                } */
            }
        }
};


#endif