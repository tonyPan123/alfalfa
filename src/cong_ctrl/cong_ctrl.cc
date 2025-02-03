#include "cong_ctrl.hh"


void CongCtrl::onSent() {
    cum_segs_sent += 1;
}

// How to handle with pkts reordering?
void CongCtrl::onACK(SeqNum ack, Time rtt) {
    // Assume ack start from 0
    // Reordering regarded as lost
    if (ack >= (cum_segs_delivered + cum_segs_lost)) {
        // Deal with the new loss first!!!
        cum_segs_lost += (ack - (cum_segs_delivered + cum_segs_lost));
        while (cum_segs_loss_vector.back().creation_cum_lost_segs < cum_segs_lost) {
            // Make sure the last entry is not complete before digesting the loss
            if (cum_segs_loss_vector.back().complete) {
                Loss loss = cum_segs_loss_vector.back();
                loss.complete = false;
                loss.ago -= 1;
                assert(loss.ago >= 0);
                cum_segs_loss_vector.push_back(loss);
            }

            int cur_ago = cum_segs_loss_vector.back().ago;
            int index = HISTORY_SIZE - cur_ago;
            if (index < HISTORY_SIZE) {
                cum_segs_loss_vector.back().creation_cum_lost_segs = std::min (
                    cum_segs_lost, 
                    history.at(index).creation_cum_sent_segs - cum_segs_delivered
                );
                if (cum_segs_lost >= (history.at(index).creation_cum_sent_segs - cum_segs_delivered)) {
                    cum_segs_loss_vector.back().complete = true;
                }
            } else {
                cum_segs_loss_vector.back().creation_cum_lost_segs = std::min (
                    cum_segs_lost, 
                    cum_segs_sent - cum_segs_delivered
                );
                if (cum_segs_lost >= (cum_segs_sent - cum_segs_delivered)) {
                    cum_segs_loss_vector.back().complete = true;
                }
            }
        }
        // Deal with the new delivered
        cum_segs_delivered += 1;
        if (cum_segs_loss_vector.back().complete) {
            Loss loss = cum_segs_loss_vector.back();
            loss.complete = false;
            loss.ago -= 1;
            assert(loss.ago >= 0);
            cum_segs_loss_vector.push_back(loss);
        }
        int cur_ago = cum_segs_loss_vector.back().ago;
        int index = HISTORY_SIZE - cur_ago;
        if (index < HISTORY_SIZE) {
            if (cum_segs_lost >= (history.at(index).creation_cum_sent_segs - cum_segs_delivered)) {
                cum_segs_loss_vector.back().complete = true;
            }
        } else {
            if (cum_segs_lost >= (cum_segs_sent - cum_segs_delivered)) {
                cum_segs_loss_vector.back().complete = true;
            }
        }
    }
    // Update min_rtt
    beliefs.min_rtt = std::min(beliefs.min_rtt, rtt);
    //std::cout << "New rtt is " << beliefs.min_rtt << std::endl;

    return;
}

SeqNum CongCtrl::get_cca_action() {
    if (beliefs.max_q >= 1) {
        return std::max(0, (int)(beliefs.min_c - beliefs.max_q));
    } else {
        return no_loss_rate + loss_risk;
    }
}

