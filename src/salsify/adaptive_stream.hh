#ifndef ADAPTIVE_STREAM_HH
#define ADAPTIVE_STREAM_HH

#include <iostream>
#include <boost/circular_buffer.hpp>
#include <vector>
#include <thread>
#include <future>
#include <chrono>

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

    uint32_t timestamp;

    EncodeJob(RasterHandle raster, const Encoder & encoder, const size_t target_size, const uint32_t timestamp)
        : raster( raster ), encoder( encoder ), target_size( target_size ), timestamp(timestamp)
    {}
};

struct EncodeOutput
{
    Encoder encoder;
    vector<uint8_t> frame;
    uint32_t source_minihash;
    uint32_t timestamp;

    EncodeOutput( Encoder && encoder, vector<uint8_t> && frame,
                    const uint32_t source_minihash, const uint32_t timestamp)
        : encoder( std::move( encoder ) ), frame( std::move( frame ) ),
        source_minihash( source_minihash), timestamp(timestamp)
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
    return { move( encode_job.encoder ), move( output ), source_minihash, encode_job.timestamp};
}


class ABR {
    public: 
        // Fetched frames per RTT 
        vector<RasterHandle> fetched_frames;
        // Encoders should be parallel (>= 2)
        Encoder encoder{ 1280, 720, false, REALTIME_QUALITY };
        // Can only consider 2 RTTs' frames for each abr action  
        static constexpr unsigned MAX_NUM_RTTS = 3;
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

        void add_fetch_frame(YUV4MPEGReader & reader) {
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
                auto now = chrono::steady_clock::now();
                uint32_t ms = chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                encode_jobs.emplace_back(raster, encoder, target_sizes / fetched_frames.size() /*TODO: change to min_c later */, ms);
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
            //auto probe = chrono::_V2::system_clock::now();
            vector<EncodeOutput> good_outputs;
            for ( auto & out_future : encode_outputs ) {
              if ( out_future.valid() ) {
                good_outputs.push_back( move( out_future.get() ) );
              }
            }
            //chrono::duration<double, std::ratio<1,1000>> diff = (chrono::_V2::system_clock::now() - probe); // in millis
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
            //cout << "Time is: " << 1 << endl;

            vector<FragmentedFrame> encode_output = {};
            FECPre frames(connection_id);
            for ( size_t i = 0; i < good_outputs.size(); i++ ) {
                cout << "Pidan: " << good_outputs[ i ].frame.size() << endl;
                frame_no++;
                FragmentedFrame ff { connection_id, good_outputs[ i ].source_minihash, good_outputs[ i ].encoder.minihash(),
                    frame_no,
                    0 /* Unused */,
                    good_outputs[ i ].frame };
                cout << "SSIM of " << frame_no << " is " << good_outputs[i].encoder.stats().ssim.get() << endl;
                // TODO: Need to include fetch and encoding time
                //auto now = chrono::steady_clock::now();
                //uint32_t ms = chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
                frames.add_frame(frame_no, ff.packets(), good_outputs[i].timestamp);
            }

            // The frames added into FECPre will be sent anyway
            encoded_output_by_rtt[MAX_NUM_RTTS - 1] = frames;
            if (good_outputs.size() > 0) {
                encoder = move(good_outputs[ good_outputs.size() - 1 ].encoder);
            }
            //good_outputs[ good_outputs.size() - 1 ].encoder.stats().ssim;
            // Sending behaviour and add FEC
            if (cc.beliefs.max_q == 0) {
                if (cc.beliefs.max_c < cc.MAX_BANDWIDTH) {
                    cout << "Upper Bound already shrink" << endl;
                    encoded_output_by_rtt[0] =  FECPre{connection_id};
                    encoded_output_by_rtt[1] =  FECPre{connection_id};
                    if (encoded_output_by_rtt[2].initialized) {
                        FECPre & sum = encoded_output_by_rtt[2];
                        uint16_t extra_fec = min((uint16_t)(cc.beliefs.min_c - (uint16_t)min((uint16_t)sum.total_len, (uint16_t)cc.beliefs.min_c)), (uint16_t)(255 - sum.total_len));
                        cout << "Total Frame: " << sum.total_len << endl;
                        cout << "Extra Fec: " << extra_fec << endl;
                        fec_frame_no++;
                        FECFrame fec_frame {fec_frame_no, sum, extra_fec};
                        encoded_output_by_rtt[2] =  FECPre{connection_id};
                        int pkt_interdelay = (cc.beliefs.min_rtt * MILLI_TO_MICRO) / (fec_frame.total_pkts); 
                        for (FECPacket & pkt : fec_frame.pkts) {
                            pacer.push( pkt.to_string(), pkt_interdelay);
                        }

                    }

                }
                // c_max does not shrink
                if (encoded_output_by_rtt[0].initialized) {
                    if (encoded_output_by_rtt[1].initialized) {
                        cout << "Per-frame Encoding" << endl;
                        // Best effort for latency and probing
                        // Non-determinsim at the encoder can make the normal condition check fail
                        int64_t lower_bound = (int64_t)encoded_output_by_rtt[0].total_len + (int64_t)encoded_output_by_rtt[1].total_len;
                        int64_t upper_bound = (int64_t)3 * cc.beliefs.min_c;
                        if (encoded_output_by_rtt[2].initialized) {
                            upper_bound -= (int64_t)encoded_output_by_rtt[2].total_len;
                        }
                        int64_t fec_loss = (int64_t)cc.no_loss_rate - lower_bound;
                        int64_t fec_latency = ((upper_bound - lower_bound) / 2);
                        int64_t extra_fec = max(min(fec_loss, fec_latency), (int64_t)0);
                        if (lower_bound >= 256) {
                            extra_fec = 0;
                        }
                        cout << "First Frame: " << encoded_output_by_rtt[0].total_len << " Second Frame: " << encoded_output_by_rtt[1].total_len << " Third Frame: " << encoded_output_by_rtt[2].total_len << endl;
                        cout << "Extra Fec: " << extra_fec << endl;
                        cout << cc.no_loss_rate << endl;
                        fec_frame_no++;
                        FECFrame fec_frame1 {fec_frame_no, encoded_output_by_rtt[0], (uint16_t)extra_fec};
                        encoded_output_by_rtt[0] =  FECPre{connection_id};
                        fec_frame_no++;
                        FECFrame fec_frame2 {fec_frame_no, encoded_output_by_rtt[1], (uint16_t)extra_fec};
                        encoded_output_by_rtt[1] =  FECPre{connection_id};
                        int pkt_interdelay = (cc.beliefs.min_rtt * MILLI_TO_MICRO) / (fec_frame1.total_pkts + fec_frame2.total_pkts); 
                        for (FECPacket & pkt : fec_frame1.pkts) {
                            pacer.push( pkt.to_string(), pkt_interdelay);
                        }
                        for (FECPacket & pkt : fec_frame2.pkts) {
                            pacer.push( pkt.to_string(), pkt_interdelay);
                        }

                    } else {
                        //throw std::runtime_error("Not Implemented 3\n");
                    }
                } else {
                    if (encoded_output_by_rtt[1].initialized) {
                        if (encoded_output_by_rtt[2].initialized) {
                            if ((encoded_output_by_rtt[1].total_len + encoded_output_by_rtt[2].total_len) <= (uint32_t)cc.no_loss_rate) {
                                cout << "Multi-frame Encoding" << endl;
                                FECPre & sum = encoded_output_by_rtt[1];
                                sum.merge(encoded_output_by_rtt[2]);
                                uint16_t extra_fec = min((uint16_t)(3 * cc.beliefs.min_c - sum.total_len), (uint16_t)(255 - sum.total_len));
                                cout << "Total Frame: " << sum.total_len << endl;
                                cout << "Extra Fec: " << extra_fec << endl;
                                fec_frame_no++;
                                FECFrame fec_frame {fec_frame_no, sum, extra_fec};
                                encoded_output_by_rtt[1] =  FECPre{connection_id};
                                encoded_output_by_rtt[2] =  FECPre{connection_id};
                                int pkt_interdelay = (cc.beliefs.min_rtt * MILLI_TO_MICRO) / (fec_frame.total_pkts); 
                                for (FECPacket & pkt : fec_frame.pkts) {
                                    pacer.push( pkt.to_string(), pkt_interdelay);
                                }
                            } else {
                                //throw std::runtime_error("Not Implemented 4\n");
                            }
                        } else {
                            //throw std::runtime_error("Not Implemented 5\n");
                        }
                    } else {
                        if (encoded_output_by_rtt[2].initialized) {
                            cout << "Single-frame Encoding" << endl;
                            FECPre & sum = encoded_output_by_rtt[2];
                            uint16_t extra_fec = min((uint16_t)(3 * cc.beliefs.min_c - sum.total_len), (uint16_t)(255 - sum.total_len));
                            cout << "Total Frame: " << sum.total_len << endl;
                            cout << "Extra Fec: " << extra_fec << endl;
                            fec_frame_no++;
                            FECFrame fec_frame {fec_frame_no, sum, extra_fec};
                            encoded_output_by_rtt[2] =  FECPre{connection_id};
                            int pkt_interdelay = (cc.beliefs.min_rtt * MILLI_TO_MICRO) / (fec_frame.total_pkts); 
                            for (FECPacket & pkt : fec_frame.pkts) {
                                pacer.push( pkt.to_string(), pkt_interdelay);
                            }
                        } else {

                        }
                    }
                }
            }
        }
};


#endif