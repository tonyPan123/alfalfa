

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
    double rate;
};

extern "C" {
    //From_Rust rust_function(From_Rust*);
    struct ExternalBeliefBound compute_belief_bounds_c_test();
    struct ExternalBeliefBound compute_belief_bounds_c(const ExternalHistory*, int);
    
}

class CongCtrl
{
    SeqNum loss_risk = 2;
    enum Action { Probe, Drain }; 

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
            bool complete;
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
        std::vector<Loss> cum_segs_loss_vector;
        SegsRate no_loss_rate;

    public:
        CongCtrl() : 
            history(HISTORY_SIZE),
            beliefs(BeliefBound()), 
            cum_segs_sent(0),  
            cum_segs_delivered(0),
            cum_segs_lost(0),
            cum_segs_loss_vector({{0, 0, true}}),
            no_loss_rate(0) {
                for (int i = 0; i < (HISTORY_SIZE - 1); i++) {
                    Loss loss = {0, 0, true};
                    std::vector<Loss> losses = {loss};
                    history.push_back( {MAX_DELAY, 0, 0, losses} );
                }
                updateHistory();
                //std::cout << "JiJI" << " " << history.back().loss.back().ago << " " << cum_segs_loss_vector.back().ago << std::endl;
                updateBeliefBound();
        }

        ~CongCtrl() {}

        void onSent();

        void onACK(SeqNum ack, Time rtt);

        SeqNum get_cca_action();

        TimeDelta get_action_intertime() { return beliefs.min_rtt; } // milli seconds

        void updateHistory() {
            // Vector copied by value
            History new_history = {beliefs.min_rtt, cum_segs_sent, cum_segs_delivered, cum_segs_loss_vector};
            history.push_back(new_history);
            // Need to make sure at least one complete loss
            std::vector<Loss> new_cum_segs_loss_vector = {};
            for (auto i = cum_segs_loss_vector.rbegin(); i != cum_segs_loss_vector.rend(); i++) {
                Loss loss = *i;
                loss.ago += 1;
                new_cum_segs_loss_vector.push_back(loss);
                if (loss.complete) {
                    break;
                }
            }
            cum_segs_loss_vector = {};
            for (auto i = new_cum_segs_loss_vector.rbegin(); i != new_cum_segs_loss_vector.rend(); i++) {
                cum_segs_loss_vector.push_back(*i);
            }
        }

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
                // The last loss entry violate rust library's assumption
                if (!ob.loss.back().complete) {
                    historys[i].length -= 1;
                }
            } 

            // Query the rust static library
    
            //ExternalBeliefBound bb = compute_belief_bounds_c_test();
            ExternalBeliefBound bb = compute_belief_bounds_c(&historys[0], HISTORY_SIZE);
            //std::cout << "New BB is: " <<" "<< bb.min_c <<  " " << bb.max_q << std::endl;
            //std::cout << "New max allowed rate is: " << bb.rate << std::endl;
            // Update the belief bound
            beliefs.min_c = bb.min_c;
            beliefs.max_c = bb.max_c;
            beliefs.min_b = bb.min_b;
            beliefs.max_b = bb.max_b;
            beliefs.min_q = bb.min_q;
            beliefs.max_q = bb.max_q;
            no_loss_rate = bb.rate;
        }
};

#endif