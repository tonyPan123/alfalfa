#include "cong_ctrl.hh"

// How to handle with pkts reordering?
void CongCtrl::onACK(SeqNum ack, Time rtt) {
    // Assume ack start from 0
    // Reordering regarded as lost
    if (ack >= (cum_segs_delivered + cum_segs_lost)) {
        cum_segs_lost += (ack - (cum_segs_delivered + cum_segs_lost));
        cum_segs_delivered += 1;
    }
    // new rtt only takes effect at next step
    beliefs.next_min_rtt = std::min(beliefs.next_min_rtt, rtt);
    
    return;
}