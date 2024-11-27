

#ifndef CONG_CTRL_HH
#define CONG_CTRL_HH

#include <boost/circular_buffer.hpp>

typedef double Time;		  // ms (cumulative)
typedef int SeqNum;		  // Pkts
typedef double SegsRate;	  // Pkts per rtprop
class CongCtrl
{

    public:
        // Important constant used in simulation
        static constexpr int HISTORY_SIZE = 6;
        static constexpr Time MAX_DELAY = 1000; // 1000ms initially
        static constexpr SegsRate MIN_BANDWIDTH = 5; 
        static constexpr SegsRate MAX_BANDWIDTH = 1000;
        static constexpr SeqNum MIN_BUFFER = 5;
        static constexpr SeqNum MAX_BUFFER = 1000;

        // Entry to be filled into rust repo
        struct History {
            Time min_rtt;

            SeqNum creation_cum_sent_segs;
		    SeqNum creation_cum_delivered_segs;
		    SeqNum creation_cum_lost_segs;

        };

        // Result from the rust repo
        struct BeliefBound {

            Time min_rtt;
            Time next_min_rtt; // min_rtt potentially updated during the current belief

		    SegsRate min_c;
		    SegsRate max_c;

            SeqNum min_b;
		    SeqNum max_b;

            SeqNum min_q;
		    SeqNum max_q;

            BeliefBound() // change to query rust repo
                : min_rtt(MAX_DELAY), 
                  next_min_rtt(MAX_DELAY), 
                  min_c(MIN_BANDWIDTH),
                  max_c(MAX_BANDWIDTH),
                  min_b(MIN_BUFFER),
                  max_b(MAX_BUFFER),
                  min_q(0),
                  max_q(0) {

            }

        };

        boost::circular_buffer<History> history; // current history

        BeliefBound beliefs; // current belief bound

	    SeqNum cum_segs_sent;
	    SeqNum cum_segs_delivered;
	    SeqNum cum_segs_lost;

    public:
        CongCtrl() : 
            history(HISTORY_SIZE),
            beliefs(BeliefBound()), 
            cum_segs_sent(0),  
            cum_segs_delivered(0),
            cum_segs_lost(0) {
        }

        ~CongCtrl() {}

        void onACK(SeqNum ack, Time rtt);

        double get_packet_interval() { return beliefs.min_rtt / beliefs.min_c; }
};

#endif