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
        vector<Optional<FragmentedFrame>> encoded_output_by_rtt[MAX_NUM_RTTS];
        int encoded_output_length;

        /* where we keep the outputs of parallel encoding jobs */
        vector<EncodeJob> encode_jobs;
        vector<future<EncodeOutput>> encode_outputs;

        ABR(IVFReader & input) {
            encoder  = Encoder{ input.display_width(), input.display_height(),
                                    false /* two-pass */, REALTIME_QUALITY };
            encoded_output_length = 0;
        }

        void add_fetch_frame(RasterHandle & raster) {
            fetched_frames.push_back(raster);
        }

        void encode_fetched_frames(size_t target_sizes) {
            if (fetched_frames.size() == 0) {
                return;
            }
            for (RasterHandle & raster : fetched_frames) {
                encode_jobs.emplace_back(raster, encoder, target_sizes /*TODO: change to min_c later */);
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
        void add_fec() {
            encode_jobs.clear();
            vector<EncodeOutput> good_outputs;
            for ( auto & out_future : encode_outputs ) {
              if ( out_future.valid() ) {
                good_outputs.push_back( move( out_future.get() ) );
              }
            }

            for ( size_t i = 0; i < good_outputs.size(); i++ ) {
                cout << "Pidan: " << good_outputs[ i ].frame.size() << endl;
            }

            if (good_outputs.size() > 0) {
                encoder = move(good_outputs[ good_outputs.size() - 1 ].encoder);
            }
        }
};


#endif