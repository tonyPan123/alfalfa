

#ifndef CONG_CTRL_HH
#define CONG_CTRL_HH

#include <iostream>
#include <boost/circular_buffer.hpp>
#include <vector>

typedef double Time;		  // ms (cumulative)
typedef double TimeDelta;		  // ms (cumulative)
typedef int SeqNum;		  // Pkts
typedef double SegsRate;	  // Pkts per rtprop
typedef int Step;

struct ExternalLoss {
    int t;
    double l;
};

struct ExternalHistory {
    double a;
    double s;
    const ExternalLoss* lo;
    int length;
};

struct ExternalBeliefBound {
    double min_c;
    double max_c;
    double min_b;
    double max_b;
    double min_q;
    double max_q;
};

extern "C" {
    //From_Rust rust_function(From_Rust*);
    struct ExternalBeliefBound compute_belief_bounds_c_test();
    struct ExternalBeliefBound compute_belief_bounds_c(const ExternalHistory*, int);
    
}

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
        static constexpr TimeDelta MILLI_TO_MICRO = 1e3;

        // Entry to be filled into rust repo
        struct Loss {
            Step ago;
            SeqNum creation_cum_lost_segs;
        };


        struct History {
            Time min_rtt;

            SeqNum creation_cum_sent_segs;
		    SeqNum creation_cum_delivered_segs;
		    std::vector<Loss> loss;

        };

        // Result from the rust repo
        struct BeliefBound {

            Time min_rtt;
            //Time next_min_rtt; // min_rtt potentially updated during the current belief

		    SegsRate min_c;
		    SegsRate max_c;

            SeqNum min_b;
		    SeqNum max_b;

            SeqNum min_q;
		    SeqNum max_q;

            BeliefBound() // change to query rust repo
                : min_rtt(MAX_DELAY), 
                  //next_min_rtt(MAX_DELAY), 
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
                for (int i = 0; i < HISTORY_SIZE; i++) {
                    Loss loss = {0, 0};
                    std::vector<Loss> losses = {loss};
                    history.push_back( {MAX_DELAY, 0, 0, losses} );
                }

                updateBeliefBound();
        }

        ~CongCtrl() {}

        void onACK(SeqNum ack, Time rtt);

        double get_action_intertime() { return beliefs.min_rtt; } // milli seconds

        void updateHistory();

        void updateBeliefBound() {
            // Prepare for the argument
            ExternalHistory historys[HISTORY_SIZE];
            ExternalLoss lossess[HISTORY_SIZE][HISTORY_SIZE];
            for (int i = 0; i < HISTORY_SIZE; i++) {
                History ob = history[i];
                for (int j = 0; j < (int)ob.loss.size(); j++) {
                    lossess[i][j].t = ob.loss.at(j).ago;
                    lossess[i][j].l = ob.loss.at(j).creation_cum_lost_segs;
                }
                historys[i].a = (double)ob.creation_cum_sent_segs;
                historys[i].s = (double)ob.creation_cum_delivered_segs;
                historys[i].lo = &(lossess[i][0]);
                historys[i].length = (int)ob.loss.size();
            } 

            // Query the rust static library
    
            //ExternalBeliefBound bb = compute_belief_bounds_c_test();
            ExternalBeliefBound bb = compute_belief_bounds_c(&historys[0], HISTORY_SIZE);
            std::cout << "Giegie: " <<" "<< bb.min_c << " " << bb.max_c << " " << bb.max_q << std::endl;
            // Update the belief bound
            beliefs.min_c = bb.min_c;
            beliefs.max_c = bb.max_c;
            beliefs.min_b = bb.min_b;
            beliefs.max_b = bb.max_b;
            beliefs.min_q = bb.min_q;
            beliefs.max_q = bb.max_q;
        }
};

#endif