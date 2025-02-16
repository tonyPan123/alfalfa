#include "reed_solomon.hpp"

#define talloc(type, num) (type *) malloc(sizeof(type)*(num))

   FECPacket::FECPacket( const uint16_t connection_id, 
      const uint32_t frame_no, 
      const uint16_t pkt_no,
      const uint16_t pkts_in_this_frame,
      const uint16_t pkts_needed_for_decoding, 
      const std::string payload) 
      : connection_id_(connection_id),
      frame_no_(frame_no),
      pkt_no_(pkt_no),
      pkts_in_this_frame_(pkts_in_this_frame),
      pkts_needed_for_decoding_(pkts_needed_for_decoding),
      payload_(payload)
      {}

   FECPacket::FECPacket( const Chunk & str )
      : connection_id_( str( 0, 2 ).le16() ),
      frame_no_( str( 2, 4 ).le32() ),
      pkt_no_( str( 6, 2 ).le16() ),
      pkts_in_this_frame_( str( 8, 2 ).le16() ),
      pkts_needed_for_decoding_( str( 10, 2 ).le16() ),
      payload_( str( 12 ).to_string() )
   {}

   std::string FECPacket::put_header_field( const uint16_t n )
   {
      const uint16_t network_order = htole16( n );
      return std::string( reinterpret_cast<const char *>( &network_order ),
                  sizeof( network_order ) );
   }

   std::string FECPacket::put_header_field( const uint32_t n )
   {
      const uint32_t network_order = htole32( n );
      return std::string( reinterpret_cast<const char *>( &network_order ),
                     sizeof( network_order ) );
   }


   /* serialize a Packet */
   std::string FECPacket::to_string() const {
      return put_header_field( connection_id_ )
         + put_header_field( frame_no_ )
         + put_header_field( pkt_no_ )
         + put_header_field( pkts_in_this_frame_ )
         + put_header_field( pkts_needed_for_decoding_ )
         + payload_;
   }

   FECFrame::FECFrame() {}


   FECFrame::FECFrame(const std::vector<Packet> packets, const uint16_t connection_id, const uint32_t frame_no, const uint16_t fec_length) {
      const uint16_t total_num = packets.size();
      assert(total_num >= 1);

      // Add data pkts first
      uint16_t pkt_no = 0;
      for ( const auto & packet : packets ) {
         std::string payload = packet.to_string();
         fecpkts.push_back(FECPacket {connection_id, frame_no, pkt_no, (uint16_t)(total_num + fec_length), total_num, payload});
         pkt_no++;
      }
      // Add fec pkts
      std::vector<std::string> fec_payloads;
      for (int i = 0; i < fec_length; i++) {
         fec_payloads.push_back("");   
      }
      size_t fec_payload_len = fecpkts.at(0).payload_.length();
      //std::cout << "FEC packet length is:" << fec_payload_len << std::endl;

      for (size_t j = 0; j < fec_payload_len; j++) {
         std::string message;
         for (uint16_t k = 0; k < total_num; k++) {
            size_t datalog_len = fecpkts.at(k).payload_.length();
            if (j < datalog_len) {
               message += fecpkts.at(k).payload_[j];
            } else {
               message += (char)0x00;
            }
         }
         std::string parities = make_fec(message, fec_length);  
         assert(parities.length() == fec_length);
         for (uint16_t k = 0; k < fec_length; k++) {
            fec_payloads.at(k) += parities[k];
         }
      }

      for ( const auto & payload : fec_payloads ) {
         fecpkts.push_back(FECPacket {connection_id, frame_no, pkt_no, (uint16_t)(total_num + fec_length), total_num, payload});
         pkt_no++;
      }

      assert(fecpkts.size() == (std::size_t)(total_num + fec_length));
   }


   std::string FECFrame::make_fec(std::string message, const uint16_t fec_length) {
      if (fec_length == 1) {
         return "fec";
      } else {
         return message;
      }
   }

   


   int ReedSolomon::reed_test() {
      int k = 60;
      int m = 2;
      int w = 8;
      int l = 1500;

      unsigned char uc;

      int *matrix;
      char **data, **coding, **dcopy, **ccopy;

      matrix = reed_sol_vandermonde_coding_matrix(k, m, w);

      MOA_Seed(520);
      data = talloc(char *, k);
      dcopy = talloc(char *, k);
      for (int i = 0; i < k; i++) {
        data[i] = talloc(char, l);
        dcopy[i] = talloc(char, l);
        for (int j = 0; j < 1200; j++) {
          uc = MOA_Random_W(8, 1);
          data[i][j] = (char) uc;
        }
        memcpy(dcopy[i], data[i], l);
      }
    
      coding = talloc(char *, m);
      ccopy = talloc(char *, m);
      for (int i = 0; i < m; i++) {
        coding[i] = talloc(char, l);
        ccopy[i] = talloc(char, l);
      }
      //std::cout << *matrix << std::endl;
      jerasure_matrix_encode(k, m, w, matrix, data, coding, l);

      //for (int i = 0; i < m; i++) {
      //   std::cout << (int)coding[i][1200] << std::endl;
      //}

      return 1;
   }


   AckFECPacket::AckFECPacket( const uint16_t connection_id, const uint32_t frame_no, const uint16_t pkt_no, const std::string frame_ack)
      : connection_id_( connection_id ), frame_no_( frame_no ),
         pkt_no_( pkt_no ), frame_ack_( frame_ack )
      {}

   AckFECPacket::AckFECPacket( const Chunk & str )
      : connection_id_( str( 0, 2 ).le16() ),
        frame_no_( str( 2, 4 ).le32() ),
        pkt_no_( str( 6, 2 ).le16() ),
        frame_ack_( str( 8 ).to_string() )
      {}

   std::string AckFECPacket::to_string()
   {
      return FECPacket::put_header_field( connection_id_ )
                + FECPacket::put_header_field( frame_no_ )
                + FECPacket::put_header_field( pkt_no_ )
                + frame_ack_;
   }  


   void AckFECPacket::sendto( UDPSocket & socket, const Address & addr )
   {
      socket.sendto( addr, to_string() );
   }