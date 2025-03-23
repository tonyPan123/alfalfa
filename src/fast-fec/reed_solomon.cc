#include "reed_solomon.hpp"


using namespace std;

#define talloc(type, num) (type *) malloc(sizeof(type)*(num))

   FECPacket::FECPacket(const uint16_t connection_id_,
                        const uint32_t fec_frame_no_,
                        const uint16_t total_pkts,
                        const uint16_t pkts_needed_for_decoding,
                        const uint32_t frame_no_start,
                        const uint32_t frame_no_end,
                        const unordered_map<uint32_t, uint16_t> frame_no_to_length,
                        const uint16_t pkt_no_,
                        const std::string payload_)
      : connection_id_(connection_id_),
        fec_frame_no_(fec_frame_no_),
        total_pkts(total_pkts),
        pkts_needed_for_decoding(pkts_needed_for_decoding),
        frame_no_start(frame_no_start),
        frame_no_end(frame_no_end),
        frame_no_to_length(frame_no_to_length),
        pkt_no_(pkt_no_),
        payload_(payload_)
      {}
   
   FECPacket::FECPacket( const Chunk & str )
      : connection_id_( str( 0, 2 ).le16() ),
      fec_frame_no_( str( 2, 4 ).le32() ),
      total_pkts( str( 6, 2 ).le16() ),
      pkts_needed_for_decoding( str( 8, 2 ).le16() ),
      frame_no_start( str( 10, 4 ).le32() ),
      frame_no_end( str( 14, 4 ).le32() ),
      frame_no_to_length( {} )
   {
      int start = 18;
      for (uint32_t frame_no = frame_no_start; frame_no <= frame_no_end; frame_no++) {
         uint16_t length = str( start, 2 ).le16();
         frame_no_to_length[frame_no] = length;
         start += 2;
      }
      pkt_no_ = str( start, 2 ).le16();
      start += 2;
      payload_ = str( start ).to_string();
   }
   
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
      string frame_no_to_length_str = "";
      for (uint32_t frame_no = frame_no_start; frame_no <= frame_no_end; frame_no++) {
         uint16_t length = frame_no_to_length.at(frame_no);
         frame_no_to_length_str += put_header_field( length );
      }

      return put_header_field( connection_id_ )
         + put_header_field( fec_frame_no_ )
         + put_header_field( total_pkts )
         + put_header_field( pkts_needed_for_decoding )
         + put_header_field( frame_no_start ) 
         + put_header_field( frame_no_end )
         + frame_no_to_length_str 
         + put_header_field( pkt_no_ )
         + payload_;
      return "TODO";
   }


   FECFrame::FECFrame(const uint32_t fec_frame_no, FECPre & pre, const uint16_t fec_length)
      : connection_id_(pre.connection_id),
        fec_frame_no(fec_frame_no),
        frame_no_start(pre.frame_no_start),
        frame_no_end(pre.frame_no_end), 
        frame_no_to_length(), 
        pkts({})
   {
      assert(pre.frame_no_start <= pre.frame_no_end && pre.initialized);
      vector<Packet> all_packets = {};
      for (uint32_t frame_no = pre.frame_no_start; frame_no <= pre.frame_no_end; frame_no++) {
         vector<Packet> & packets = pre.frame_no_to_pkts[frame_no];
         frame_no_to_length[frame_no] = packets.size();
         for (Packet & packet : packets) {
            all_packets.push_back(packet);
         }
         //all_packets.insert(packets.end(), packets.begin(), packets.end());  
      }
      pkts_needed_for_decoding = all_packets.size();
      total_pkts = pkts_needed_for_decoding + fec_length;
      assert(pkts_needed_for_decoding >= 1);
      assert(pkts_needed_for_decoding + fec_length <= 256);
      //  Maximum size of packet
      size_t fec_payload_len = 1424 + 1;
      uint16_t pkt_no = 1;
      for ( const auto & pkt : all_packets ) {
         // Do we need padding here???
         pkts.push_back(FECPacket {connection_id_, fec_frame_no, total_pkts, pkts_needed_for_decoding, frame_no_start, frame_no_end, frame_no_to_length, pkt_no, pkt.to_string()});
         pkt_no++;
      }

      if (fec_length == 0) {
         return;
      }
      char **data, **coding;
      data = talloc(char *, pkts_needed_for_decoding);
      for (int i = 0; i < pkts_needed_for_decoding; i++) {
        data[i] = talloc(char, fec_payload_len);
        assert(all_packets[i].to_string().length() < fec_payload_len);
        strcpy (data[i], all_packets[i].to_string().c_str());
      }

      coding = talloc(char *, fec_length);
      for (int i = 0; i < fec_length; i++) {
        coding[i] = talloc(char, fec_payload_len);
      }
      // k + m <= 2^w
      int *matrix;
      matrix = reed_sol_vandermonde_coding_matrix(pkts_needed_for_decoding, fec_length, 8);
      jerasure_matrix_encode(pkts_needed_for_decoding, fec_length, 8, matrix, data, coding, fec_payload_len);

      for (int i = 0; i < fec_length; i++) {
         std::string parity(coding[i], fec_payload_len - 1);
         pkts.push_back(FECPacket {connection_id_, fec_frame_no, total_pkts, pkts_needed_for_decoding, frame_no_start, frame_no_end, frame_no_to_length, pkt_no, parity});
         pkt_no++;
      }

      // Free the space
      for (int i = 0; i < pkts_needed_for_decoding; i++) {
         free(data[i]);
      }
      free(data);
      for (int i = 0; i < fec_length; i++) {
         free(coding[i]);
      }
      free(coding);
      assert(pkts.size() == total_pkts);
   }


   int ReedSolomon::reed_test() {
      int k = 40;
      int m = 256 - k;
      int w = 8;
      int l = 1500;

      unsigned char uc;

      int *matrix;
      char **data, **coding, **dcopy, **ccopy;

      matrix = reed_sol_vandermonde_coding_matrix(k, m, w);

      MOA_Seed(510);
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

      for (int i = 0; i < m; i++) {
         std::string a(coding[i], l);
         //std::cout << a.length() << std::endl;
      }

      return 1;
   }


   AckFECPacket::AckFECPacket( const uint16_t connection_id, const uint32_t fec_frame_no, const uint16_t pkt_no, const std::string frame_ack)
      : connection_id_( connection_id ), fec_frame_no_( fec_frame_no ),
         pkt_no_( pkt_no ), frame_ack_( frame_ack )
      {}

   AckFECPacket::AckFECPacket( const Chunk & str )
      : connection_id_( str( 0, 2 ).le16() ),
        fec_frame_no_( str( 2, 4 ).le32() ),
        pkt_no_( str( 6, 2 ).le16() ),
        frame_ack_( str( 8 ).to_string() )
      {
      }

   std::string AckFECPacket::to_string()
   {
      return FECPacket::put_header_field( connection_id_ )
                + FECPacket::put_header_field( fec_frame_no_ )
                + FECPacket::put_header_field( pkt_no_ )
                + frame_ack_;
   }  


   void AckFECPacket::sendto( UDPSocket & socket, const Address & addr )
   {
      socket.sendto( addr, to_string() );
   }