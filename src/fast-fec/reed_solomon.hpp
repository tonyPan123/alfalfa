#ifndef REED_SOLOMON_HPP
#define REED_SOLOMON_HPP

#include <cstddef>
#include <iostream>
#include <string>


#include "packet.hh"

extern "C" {
    #include "cauchy.h"
    #include <gf_rand.h>
    #include "galois.h"
    #include "jerasure.h"
    #include "reed_sol.h"
}

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

class AckFECPacket
{
public:
  uint16_t connection_id_;
  uint32_t frame_no_;
  uint16_t pkt_no_;

  std::string frame_ack_;

  AckFECPacket( const uint16_t connection_id, const uint32_t frame_no,
             const uint16_t pkt_no, const std::string frame_ack);

  AckFECPacket( const Chunk & str );

  std::string to_string();

  void sendto( UDPSocket & socket, const Address & addr );
};



#endif