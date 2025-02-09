#ifndef REED_SOLOMON_HPP
#define REED_SOLOMON_HPP

#include <cstddef>
#include <iostream>
#include <string>

#include "schifra_galois_field.hpp"
#include "schifra_galois_field_polynomial.hpp"
#include "schifra_sequential_root_generator_polynomial_creator.hpp"
#include "schifra_reed_solomon_encoder.hpp"
#include "schifra_reed_solomon_decoder.hpp"
#include "schifra_reed_solomon_block.hpp"
#include "schifra_error_processes.hpp"

#include "packet.hh"



class ReedSolomon {
    public: 
        int reed_test();
   
};

class FECPacket {

    public: 
        uint16_t connection_id_;
        uint32_t frame_no_;
        uint16_t pkt_no_;
        uint16_t pkts_in_this_frame_;
        uint16_t pkts_needed_for_decoding_;

        std::string payload_;

        FECPacket( const uint16_t connection_id, 
            const uint32_t frame_no, 
            const uint16_t pkt_no,
            const uint16_t pkts_in_this_frame,
            const uint16_t pkts_needed_for_decoding, 
            const std::string payload);
        FECPacket( const Chunk & str );

        std::string to_string() const;
        static std::string put_header_field( const uint16_t n );
        static std::string put_header_field( const uint32_t n );
};



class FECFrame
{

    public: 
    std::vector<FECPacket> fecpkts;

    FECFrame();
    FECFrame(const std::vector<Packet> packets, const uint16_t connection_id, const uint32_t frame_no, const uint16_t fec_length);
    std::string make_fec(const std::string message, const uint16_t fec_length);

};





#endif