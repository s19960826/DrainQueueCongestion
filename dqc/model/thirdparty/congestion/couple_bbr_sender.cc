#include "couple_bbr_sender.h"
#include "unacked_packet_map.h"
#include "flag_impl.h"
#include "flag_util_impl.h"
#include "rtt_stats.h"
#include "random.h"
#include "proto_constants.h"
#include <ostream>
#include <iostream>
#include "couple_cc_manager.h"
namespace dqc{
namespace {
/* Pace at ~1% below estimated bw, on average, to reduce queue at bottleneck.
 * In order to help drive the network toward lower queues and low latency while
 * maintaining high utilization, the average pacing rate aims to be slightly
 * lower than the estimated bandwidth. This is an important aspect of the
 * design.
 */
const int kPacingMarginPercent = 1;
// Constants based on TCP defaults.
// The minimum CWND to ensure delayed acks don't reduce bandwidth measurements.
// Does not inflate the pacing rate.
const QuicByteCount kDefaultMinimumCongestionWindow = 4 * kMaxSegmentSize;

// The gain used for the STARTUP, equal to 2/ln(2).
const float kDefaultHighGain = 2.885f;
// The newly derived gain for STARTUP, equal to 4 * ln(2)
const float kDerivedHighGain = 2.773f;
// The newly derived CWND gain for STARTUP, 2.
const float kDerivedHighCWNDGain = 2.0f;
// The gain used in STARTUP after loss has been detected.
// 1.5 is enough to allow for 25% exogenous loss and still observe a 25% growth
// in measured bandwidth.
const float kStartupAfterLossGain = 1.5f;
// The cycle of gains used during the PROBE_BW stage.
const float kPacingGain[] = {1.25, 0.75, 1, 1, 1, 1, 1, 1};
// The length of the gain cycle.
const size_t kGainCycleLength = sizeof(kPacingGain) / sizeof(kPacingGain[0]);
// The size of the bandwidth filter window, in round-trips.
const QuicRoundTripCount kBandwidthWindowSize = kGainCycleLength + 2;

// The time after which the current min_rtt value expires.
const TimeDelta kMinRttExpiry = TimeDelta::FromSeconds(10);
// The minimum time the connection can spend in PROBE_RTT mode.
const TimeDelta kProbeRttTime = TimeDelta::FromMilliseconds(200);
// If the bandwidth does not increase by the factor of |kStartupGrowthTarget|
// within |kRoundTripsWithoutGrowthBeforeExitingStartup| rounds, the connection
// will exit the STARTUP mode.
const float kStartupGrowthTarget = 1.25;//1.5; //do no figure out why I set it 1.5
const QuicRoundTripCount kRoundTripsWithoutGrowthBeforeExitingStartup = 3;
// Coefficient of target congestion window to use when basing PROBE_RTT on BDP.
const float kModerateProbeRttMultiplier = 0.75;
// Coefficient to determine if a new RTT is sufficiently similar to min_rtt that
// we don't need to enter PROBE_RTT.
const float kSimilarMinRttThreshold = 1.125;
static const uint32_t bbr_cycle_rand = 7;
enum bbr_pacing_gain_phase:uint8_t{
    BBR_BW_PROBE_UP     = 0,
    BBR_BW_PROBE_DOWN   = 1,
    BBR_BW_PROBE_CRUISE = 2,
};
}  // namespace


CoupleBbrSender::DebugState::DebugState(const CoupleBbrSender& sender)
    : mode(sender.mode_),
      max_bandwidth(sender.max_bandwidth_.GetBest()),
      round_trip_count(sender.round_trip_count_),
      gain_cycle_index(sender.cycle_current_offset_),
      congestion_window(sender.congestion_window_),
      is_at_full_bandwidth(sender.is_at_full_bandwidth_),
      bandwidth_at_last_round(sender.bandwidth_at_last_round_),
      rounds_without_bandwidth_gain(sender.rounds_without_bandwidth_gain_),
      min_rtt(sender.min_rtt_),
      min_rtt_timestamp(sender.min_rtt_timestamp_),
      recovery_state(sender.recovery_state_),
      recovery_window(sender.recovery_window_),
      last_sample_is_app_limited(sender.last_sample_is_app_limited_),
      end_of_app_limited_phase(sender.sampler_.end_of_app_limited_phase()) {}

CoupleBbrSender::DebugState::DebugState(const DebugState& state) = default;

CoupleBbrSender::CoupleBbrSender(ProtoTime now,
                     const RttStats* rtt_stats,
                     const UnackedPacketMap* unacked_packets,
                     QuicPacketCount initial_tcp_congestion_window,
                     QuicPacketCount max_tcp_congestion_window,
                     Random* random)
    : rtt_stats_(rtt_stats),
      unacked_packets_(unacked_packets),
      random_(random),
      mode_(STARTUP),
      round_trip_count_(0),
      max_bandwidth_(kBandwidthWindowSize, QuicBandwidth::Zero(), 0),
      max_ack_height_(kBandwidthWindowSize, 0, 0),
      aggregation_epoch_start_time_(ProtoTime::Zero()),
      aggregation_epoch_bytes_(0),
      min_rtt_(TimeDelta::Zero()),
      min_rtt_timestamp_(ProtoTime::Zero()),
      congestion_window_(initial_tcp_congestion_window * kDefaultTCPMSS),
      initial_congestion_window_(initial_tcp_congestion_window *
                                 kDefaultTCPMSS),
      max_congestion_window_(max_tcp_congestion_window * kDefaultTCPMSS),
      min_congestion_window_(kDefaultMinimumCongestionWindow),
      high_gain_(kDerivedHighCWNDGain/*kDefaultHighGain*/),
      high_cwnd_gain_(kDerivedHighCWNDGain/*kDefaultHighGain*/),
      drain_gain_(1.f /kDerivedHighCWNDGain/*kDefaultHighGain*/),
      pacing_rate_(QuicBandwidth::Zero()),
      pacing_gain_(1),
      congestion_window_gain_(1),
      congestion_window_gain_constant_(
          static_cast<float>(FLAG_quic_bbr_cwnd_gain)),
      num_startup_rtts_(kRoundTripsWithoutGrowthBeforeExitingStartup),
      exit_startup_on_loss_(false),
      cycle_current_offset_(0),
      last_cycle_start_(ProtoTime::Zero()),
      is_at_full_bandwidth_(false),
      rounds_without_bandwidth_gain_(0),
      bandwidth_at_last_round_(QuicBandwidth::Zero()),
      exiting_quiescence_(false),
      exit_probe_rtt_at_(ProtoTime::Zero()),
      probe_rtt_round_passed_(false),
      last_sample_is_app_limited_(false),
      has_non_app_limited_sample_(false),
      flexible_app_limited_(false),
      recovery_state_(NOT_IN_RECOVERY),
      recovery_window_(max_congestion_window_),
      is_app_limited_recovery_(false),
      slower_startup_(false),
      rate_based_startup_(false),
      startup_rate_reduction_multiplier_(0),
      startup_bytes_lost_(0),
      enable_ack_aggregation_during_startup_(false),
      expire_ack_aggregation_in_startup_(false),
      drain_to_target_(true),
      probe_rtt_based_on_bdp_(false),
      probe_rtt_skipped_if_similar_rtt_(false),
      probe_rtt_disabled_if_app_limited_(false),
      app_limited_since_last_probe_rtt_(false),
      min_rtt_since_last_probe_rtt_(TimeDelta::Infinite()),
      always_get_bw_sample_when_acked_(
          GetQuicReloadableFlag(quic_always_get_bw_sample_when_acked)) {
  EnterStartupMode(now);
}

CoupleBbrSender::~CoupleBbrSender() {}
void CoupleBbrSender::SetInitialCongestionWindowInPackets(
    QuicPacketCount congestion_window) {
  if (mode_ == STARTUP) {
    initial_congestion_window_ = congestion_window * kDefaultTCPMSS;
    congestion_window_ = congestion_window * kDefaultTCPMSS;
  }
}

bool CoupleBbrSender::InSlowStart() const {
  return mode_ == STARTUP;
}

void CoupleBbrSender::OnPacketSent(ProtoTime sent_time,
                             QuicByteCount bytes_in_flight,
                             QuicPacketNumber packet_number,
                             QuicByteCount bytes,
                             HasRetransmittableData is_retransmittable) {

  last_sent_packet_ = packet_number;

  if (bytes_in_flight == 0 && sampler_.is_app_limited()) {
    exiting_quiescence_ = true;
  }

  if (!aggregation_epoch_start_time_.IsInitialized()) {
    aggregation_epoch_start_time_ = sent_time;
  }

  sampler_.OnPacketSent(sent_time, packet_number, bytes, bytes_in_flight,
                        is_retransmittable);
}

bool CoupleBbrSender::CanSend(QuicByteCount bytes_in_flight) {
  return bytes_in_flight < GetCongestionWindow();
}

QuicBandwidth CoupleBbrSender::PacingRate(QuicByteCount bytes_in_flight) const {
  if (pacing_rate_.IsZero()) {
    return high_gain_ * QuicBandwidth::FromBytesAndTimeDelta(
                            initial_congestion_window_, GetMinRtt());
  }
  return pacing_rate_;
}

QuicBandwidth CoupleBbrSender::BandwidthEstimate() const {
  if(round_trip_count_>2){
	CHECK(!max_bandwidth_.GetBest().IsZero());
  }
  return max_bandwidth_.GetBest();
}

QuicByteCount CoupleBbrSender::GetCongestionWindow() const {
  if (mode_ == PROBE_RTT) {
    return ProbeRttCongestionWindow();
  }

  if (InRecovery() && !(rate_based_startup_ && mode_ == STARTUP)) {
    return std::min(congestion_window_, recovery_window_);
  }

  return congestion_window_;
}

QuicByteCount CoupleBbrSender::GetSlowStartThreshold() const {
  return 0;
}

bool CoupleBbrSender::InRecovery() const {
  return recovery_state_ != NOT_IN_RECOVERY;
}

bool CoupleBbrSender::ShouldSendProbingPacket() const {
  if (pacing_gain_ <= 1) {
    return false;
  }

  // TODO(b/77975811): If the pipe is highly under-utilized, consider not
  // sending a probing transmission, because the extra bandwidth is not needed.
  // If flexible_app_limited is enabled, check if the pipe is sufficiently full.
  if (flexible_app_limited_) {
    return !IsPipeSufficientlyFull();
  } else {
    return true;
  }
}

bool CoupleBbrSender::IsPipeSufficientlyFull() const {
  // See if we need more bytes in flight to see more bandwidth.
  if (mode_ == STARTUP) {
    // STARTUP exits if it doesn't observe a 25% bandwidth increase, so the CWND
    // must be more than 25% above the target.
    return unacked_packets_->bytes_in_flight() >=
           GetTargetCongestionWindow(1.5);
  }
  if (pacing_gain_ > 1) {
    // Super-unity PROBE_BW doesn't exit until 1.25 * BDP is achieved.
    return unacked_packets_->bytes_in_flight() >=
           GetTargetCongestionWindow(pacing_gain_);
  }
  // If bytes_in_flight are above the target congestion window, it should be
  // possible to observe the same or more bandwidth if it's available.
  return unacked_packets_->bytes_in_flight() >= GetTargetCongestionWindow(1.1);
}

void CoupleBbrSender::AdjustNetworkParameters(QuicBandwidth bandwidth,
                                        TimeDelta rtt,
                                        bool allow_cwnd_to_decrease) {
  if (!bandwidth.IsZero()) {
    max_bandwidth_.Update(bandwidth, round_trip_count_);
  }
  if (!rtt.IsZero() && (min_rtt_ > rtt || min_rtt_.IsZero())) {
    min_rtt_ = rtt;
  }
  if (GetQuicReloadableFlag(quic_fix_bbr_cwnd_in_bandwidth_resumption) &&
      mode_ == STARTUP) {
    if (bandwidth.IsZero()) {
      // Ignore bad bandwidth samples.
      QUIC_RELOADABLE_FLAG_COUNT_N(quic_fix_bbr_cwnd_in_bandwidth_resumption, 3,
                                   3);
      return;
    }
    const QuicByteCount new_cwnd =
        std::max(kMinInitialCongestionWindow * kDefaultTCPMSS,
                 std::min(kMaxInitialCongestionWindow * kDefaultTCPMSS,
                          bandwidth * rtt_stats_->SmoothedOrInitialRtt()));
    // Decreases cwnd gain and pacing gain. Please note, if pacing_rate_ has
    // been calculated, it cannot decrease in STARTUP phase.
    set_high_gain(kDerivedHighCWNDGain);
    set_high_cwnd_gain(kDerivedHighCWNDGain);
    if (new_cwnd > congestion_window_) {
      QUIC_RELOADABLE_FLAG_COUNT_N(quic_fix_bbr_cwnd_in_bandwidth_resumption, 1,
                                   3);
    } else {
      QUIC_RELOADABLE_FLAG_COUNT_N(quic_fix_bbr_cwnd_in_bandwidth_resumption, 2,
                                   3);
    }
    if (new_cwnd < congestion_window_ && !allow_cwnd_to_decrease) {
      // Only decrease cwnd if allow_cwnd_to_decrease is true.
      return;
    }
    congestion_window_ = new_cwnd;
  }
}

void CoupleBbrSender::OnCongestionEvent(bool /*rtt_updated*/,
                                  QuicByteCount prior_in_flight,
                                  ProtoTime event_time,
                                  const AckedPacketVector& acked_packets,
                                  const LostPacketVector& lost_packets) {
  const QuicByteCount total_bytes_acked_before = sampler_.total_bytes_acked();

  bool is_round_start = false;
  bool min_rtt_expired = false;

  DiscardLostPackets(lost_packets);

  // Input the new data into the BBR model of the connection.
  QuicByteCount excess_acked = 0;
  if (!acked_packets.empty()) {
    QuicPacketNumber last_acked_packet = acked_packets.rbegin()->packet_number;
    is_round_start = UpdateRoundTripCounter(last_acked_packet);
    min_rtt_expired = UpdateBandwidthAndMinRtt(event_time, acked_packets);
    UpdateRecoveryState(last_acked_packet, !lost_packets.empty(),
                        is_round_start);

    const QuicByteCount bytes_acked =
        sampler_.total_bytes_acked() - total_bytes_acked_before;

    excess_acked = UpdateAckAggregationBytes(event_time, bytes_acked);
  }

  // Handle logic specific to PROBE_BW mode.
  alpha_gain_is_negative_=false;
  if (mode_ == PROBE_BW) {
    UpdateGainCyclePhase(event_time, prior_in_flight, !lost_packets.empty());
    bool isAllInProbeMode=true;
    for(auto it=other_ccs_.begin();it!=other_ccs_.end();it++){
        if(!(*it)->IsInProbeMode()){
            isAllInProbeMode=false;
            break;
        }
    }
    if(isAllInProbeMode&&ShouldBehaveFriendlyToSinglepath()&&!other_ccs_.empty()){
        CalculateAlphaPacingGain();
    }
  }

  // Handle logic specific to STARTUP and DRAIN modes.
  if (is_round_start && !is_at_full_bandwidth_) {
    CheckIfFullBandwidthReached();
  }
  MaybeExitStartupOrDrain(event_time);

  // Handle logic specific to PROBE_RTT.
  MaybeEnterOrExitProbeRtt(event_time, is_round_start, min_rtt_expired);

  // Calculate number of packets acked and lost.
  QuicByteCount bytes_acked =
      sampler_.total_bytes_acked() - total_bytes_acked_before;
  QuicByteCount bytes_lost = 0;
  for (const auto& packet : lost_packets) {
    bytes_lost += packet.bytes_lost;
  }

  // After the model is updated, recalculate the pacing rate and congestion
  // window.
  CalculatePacingRate();
  CalculateCongestionWindow(bytes_acked, excess_acked);
  CalculateRecoveryWindow(bytes_acked, bytes_lost);

  // Cleanup internal state.
  sampler_.RemoveObsoletePackets(unacked_packets_->GetLeastUnacked());
}

CongestionControlType CoupleBbrSender::GetCongestionControlType() const {
  return kCoupleBBR;
}

TimeDelta CoupleBbrSender::GetMinRtt() const {
  return !min_rtt_.IsZero() ? min_rtt_ : rtt_stats_->initial_rtt();
}

QuicByteCount CoupleBbrSender::GetTargetCongestionWindow(float gain) const {
  QuicByteCount bdp = GetMinRtt() * BandwidthEstimate();
  QuicByteCount congestion_window = gain * bdp;

  // BDP estimate will be zero if no bandwidth samples are available yet.
  if (congestion_window == 0) {
    congestion_window = gain * initial_congestion_window_;
  }

  return std::max(congestion_window, min_congestion_window_);
}

QuicByteCount CoupleBbrSender::ProbeRttCongestionWindow() const {
  if (probe_rtt_based_on_bdp_) {
    return GetTargetCongestionWindow(kModerateProbeRttMultiplier);
  }
  return min_congestion_window_;
}

void CoupleBbrSender::EnterStartupMode(ProtoTime now) {
  mode_ = STARTUP;
  pacing_gain_ = high_gain_;
  congestion_window_gain_ = high_cwnd_gain_;
}

void CoupleBbrSender::EnterProbeBandwidthMode(ProtoTime now) {
  mode_ = PROBE_BW;
  congestion_window_gain_ = congestion_window_gain_constant_;

  // Pick a random offset for the gain cycle out of {0, 2..7} range. 1 is
  // excluded because in that case increased gain and decreased gain would not
  // follow each other.
  cycle_current_offset_ = random_->nextInt() % (kGainCycleLength - 1);
  if (cycle_current_offset_ >= 1) {
    cycle_current_offset_ += 1;
  }
  last_cycle_start_ = now;
  cycle_mstamp_=now;
  cycle_len_=0;
  pacing_gain_ = kPacingGain[cycle_current_offset_];
}

void CoupleBbrSender::DiscardLostPackets(const LostPacketVector& lost_packets) {
  for (const LostPacket& packet : lost_packets) {
    sampler_.OnPacketLost(packet.packet_number);
    if (mode_ == STARTUP) {
      /*if (stats_) {
        ++stats_->slowstart_packets_lost;
        stats_->slowstart_bytes_lost += packet.bytes_lost;
      }*/
      if (startup_rate_reduction_multiplier_ != 0) {
        startup_bytes_lost_ += packet.bytes_lost;
      }
    }
  }
}

bool CoupleBbrSender::UpdateRoundTripCounter(QuicPacketNumber last_acked_packet) {
  if (!current_round_trip_end_.IsInitialized()||
      last_acked_packet > current_round_trip_end_) {
    round_trip_count_++;
    current_round_trip_end_ = last_sent_packet_;
    /*if (stats_ && InSlowStart()) {
      ++stats_->slowstart_num_rtts;
    }*/
    return true;
  }

  return false;
}

bool CoupleBbrSender::UpdateBandwidthAndMinRtt(
    ProtoTime now,
    const AckedPacketVector& acked_packets) {
  TimeDelta sample_min_rtt = TimeDelta::Infinite();
  for (const auto& packet : acked_packets) {
    if (!always_get_bw_sample_when_acked_ && packet.bytes_acked == 0) {
      // Skip acked packets with 0 in flight bytes when updating bandwidth.
      continue;
    }
    BandwidthSample bandwidth_sample =
        sampler_.OnPacketAcknowledged(now, packet.packet_number);
    if (always_get_bw_sample_when_acked_ &&
        !bandwidth_sample.state_at_send.is_valid) {
      // From the sampler's perspective, the packet has never been sent, or the
      // packet has been acked or marked as lost previously.
      continue;
    }

    last_sample_is_app_limited_ = bandwidth_sample.state_at_send.is_app_limited;
    has_non_app_limited_sample_ |=
        !bandwidth_sample.state_at_send.is_app_limited;
    if (!bandwidth_sample.rtt.IsZero()) {
      sample_min_rtt = std::min(sample_min_rtt, bandwidth_sample.rtt);
    }

    if (!bandwidth_sample.state_at_send.is_app_limited ||
        bandwidth_sample.bandwidth > BandwidthEstimate()) {
      max_bandwidth_.Update(bandwidth_sample.bandwidth, round_trip_count_);
    }
  }

  // If none of the RTT samples are valid, return immediately.
  if (sample_min_rtt.IsInfinite()) {
    return false;
  }
  min_rtt_since_last_probe_rtt_ =
      std::min(min_rtt_since_last_probe_rtt_, sample_min_rtt);

  // Do not expire min_rtt if none was ever available.
  bool min_rtt_expired =
      !min_rtt_.IsZero() && (now > (min_rtt_timestamp_ + kMinRttExpiry));

  if (min_rtt_expired || sample_min_rtt < min_rtt_ || min_rtt_.IsZero()) {
    //QUIC_DVLOG(2) << "Min RTT updated, old value: " << min_rtt_
    //              << ", new value: " << sample_min_rtt
    //              << ", current time: " << now.ToDebuggingValue();

    if (min_rtt_expired && ShouldExtendMinRttExpiry()) {
      min_rtt_expired = false;
    } else {
      min_rtt_ = sample_min_rtt;
    }
    min_rtt_timestamp_ = now;
    // Reset since_last_probe_rtt fields.
    min_rtt_since_last_probe_rtt_ = TimeDelta::Infinite();
    app_limited_since_last_probe_rtt_ = false;
  }
  DCHECK(!min_rtt_.IsZero());

  return min_rtt_expired;
}

bool CoupleBbrSender::ShouldExtendMinRttExpiry() const {
  if (probe_rtt_disabled_if_app_limited_ && app_limited_since_last_probe_rtt_) {
    // Extend the current min_rtt if we've been app limited recently.
    return true;
  }
  const bool min_rtt_increased_since_last_probe =
      min_rtt_since_last_probe_rtt_ > min_rtt_ * kSimilarMinRttThreshold;
  if (probe_rtt_skipped_if_similar_rtt_ && app_limited_since_last_probe_rtt_ &&
      !min_rtt_increased_since_last_probe) {
    // Extend the current min_rtt if we've been app limited recently and an rtt
    // has been measured in that time that's less than 12.5% more than the
    // current min_rtt.
    return true;
  }
  return false;
}

void CoupleBbrSender::UpdateGainCyclePhase(ProtoTime now,
                                     QuicByteCount prior_in_flight,
                                     bool has_losses) {
  TimeDelta elapsed=now-cycle_mstamp_;
  const QuicByteCount bytes_in_flight = unacked_packets_->bytes_in_flight();
  if(elapsed>cycle_len_*GetMinRtt()){
	cycle_mstamp_=now;
	cycle_len_=kGainCycleLength-random_->nextInt(bbr_cycle_rand);
	cycle_current_offset_=0;
    pacing_gain_ = kPacingGain[BBR_BW_PROBE_UP];
    last_cycle_start_=now;
    cycle_state_=BBR_BW_PROBE_UP;
	return;
  }
  if(cycle_state_==BBR_BW_PROBE_CRUISE){
      bool should_advance_gain_cycling = now - last_cycle_start_ > GetMinRtt();
      if(should_advance_gain_cycling){
          cycle_current_offset_ = (cycle_current_offset_ + 1) % kGainCycleLength;
          last_cycle_start_ = now;
      }
	  return ;
  }
  
  if(cycle_state_==BBR_BW_PROBE_DOWN){
	  if(drain_to_target_){
        if(bytes_in_flight <= GetTargetCongestionWindow(1)){
        pacing_gain_=kPacingGain[BBR_BW_PROBE_CRUISE];
		cycle_current_offset_ = (cycle_current_offset_ + 1) % kGainCycleLength;
        last_cycle_start_=now;
        cycle_state_=BBR_BW_PROBE_CRUISE;
        }
	  }else{
        if(now - last_cycle_start_ > GetMinRtt()){
        pacing_gain_=kPacingGain[BBR_BW_PROBE_CRUISE];
        cycle_current_offset_ = (cycle_current_offset_ + 1) % kGainCycleLength;
        last_cycle_start_=now;
        cycle_state_=BBR_BW_PROBE_CRUISE;
        }
    }
  }
  if((elapsed>GetMinRtt())&&
	((bytes_in_flight>GetTargetCongestionWindow(pacing_gain_))||has_losses||
			last_sample_is_app_limited_)){
	  pacing_gain_=kPacingGain[BBR_BW_PROBE_DOWN];
      cycle_current_offset_=1;
      last_cycle_start_=now;
      cycle_state_=BBR_BW_PROBE_DOWN;
	  return;
  }
}

void CoupleBbrSender::CheckIfFullBandwidthReached() {
  if (last_sample_is_app_limited_) {
    return;
  }

  QuicBandwidth target = bandwidth_at_last_round_ * kStartupGrowthTarget;
  if (BandwidthEstimate() >= target) {
    bandwidth_at_last_round_ = BandwidthEstimate();
    rounds_without_bandwidth_gain_ = 0;
    if (expire_ack_aggregation_in_startup_) {
      // Expire old excess delivery measurements now that bandwidth increased.
      max_ack_height_.Reset(0, round_trip_count_);
    }
    return;
  }

  rounds_without_bandwidth_gain_++;
  if ((rounds_without_bandwidth_gain_ >= num_startup_rtts_) ||
      (exit_startup_on_loss_ && InRecovery())) {
    DCHECK(has_non_app_limited_sample_);
    is_at_full_bandwidth_ = true;
  }
}

void CoupleBbrSender::MaybeExitStartupOrDrain(ProtoTime now) {
  if (mode_ == STARTUP && is_at_full_bandwidth_) {
    OnExitStartup(now);
    mode_ = DRAIN;
    pacing_gain_ = drain_gain_;
    congestion_window_gain_ = high_cwnd_gain_;
  }
  if (mode_ == DRAIN &&
      unacked_packets_->bytes_in_flight() <= GetTargetCongestionWindow(1)) {
    EnterProbeBandwidthMode(now);
  }
}

void CoupleBbrSender::OnExitStartup(ProtoTime now) {
  DCHECK_EQ(mode_, STARTUP);
  /*if (stats_) {
    DCHECK_NE(stats_->slowstart_start_time, QuicTime::Zero());
    if (now > stats_->slowstart_start_time) {
      stats_->slowstart_duration =
          now - stats_->slowstart_start_time + stats_->slowstart_duration;
    }
    stats_->slowstart_start_time = QuicTime::Zero();
  }*/
}

void CoupleBbrSender::MaybeEnterOrExitProbeRtt(ProtoTime now,
                                         bool is_round_start,
                                         bool min_rtt_expired) {
  if (min_rtt_expired && !exiting_quiescence_ && mode_ != PROBE_RTT) {
    if (InSlowStart()) {
      OnExitStartup(now);
    }
    mode_ = PROBE_RTT;
    pacing_gain_ = 1;
    // Do not decide on the time to exit PROBE_RTT until the |bytes_in_flight|
    // is at the target small value.
    exit_probe_rtt_at_ = ProtoTime::Zero();
  }

  if (mode_ == PROBE_RTT) {
    sampler_.OnAppLimited();

    if (exit_probe_rtt_at_ == ProtoTime::Zero()) {
      // If the window has reached the appropriate size, schedule exiting
      // PROBE_RTT.  The CWND during PROBE_RTT is kMinimumCongestionWindow, but
      // we allow an extra packet since QUIC checks CWND before sending a
      // packet.
      if (unacked_packets_->bytes_in_flight() <
          ProbeRttCongestionWindow() + kMaxOutgoingPacketSize) {
        exit_probe_rtt_at_ = now + kProbeRttTime;
        probe_rtt_round_passed_ = false;
      }
    } else {
      if (is_round_start) {
        probe_rtt_round_passed_ = true;
      }
      if (now >= exit_probe_rtt_at_ && probe_rtt_round_passed_) {
        min_rtt_timestamp_ = now;
        if (!is_at_full_bandwidth_) {
          EnterStartupMode(now);
        } else {
          EnterProbeBandwidthMode(now);
        }
      }
    }
  }

  exiting_quiescence_ = false;
}

void CoupleBbrSender::UpdateRecoveryState(QuicPacketNumber last_acked_packet,
                                    bool has_losses,
                                    bool is_round_start) {
  // Exit recovery when there are no losses for a round.
  if (has_losses) {
    end_recovery_at_ = last_sent_packet_;
  }

  switch (recovery_state_) {
    case NOT_IN_RECOVERY:
      // Enter conservation on the first loss.
      if (has_losses) {
        recovery_state_ = CONSERVATION;
        // This will cause the |recovery_window_| to be set to the correct
        // value in CalculateRecoveryWindow().
        recovery_window_ = 0;
        // Since the conservation phase is meant to be lasting for a whole
        // round, extend the current round as if it were started right now.
        current_round_trip_end_ = last_sent_packet_;
        if (GetQuicReloadableFlag(quic_bbr_app_limited_recovery) &&
            last_sample_is_app_limited_) {
          //QUIC_RELOADABLE_FLAG_COUNT(quic_bbr_app_limited_recovery);
          is_app_limited_recovery_ = true;
        }
      }
      break;

    case CONSERVATION:
      if (is_round_start) {
        recovery_state_ = GROWTH;
      }
      //QUIC_FALLTHROUGH_INTENDED;

    case GROWTH:
      // Exit recovery if appropriate.
      if (!has_losses && last_acked_packet > end_recovery_at_) {
        recovery_state_ = NOT_IN_RECOVERY;
        is_app_limited_recovery_ = false;
      }

      break;
  }
  if (recovery_state_ != NOT_IN_RECOVERY && is_app_limited_recovery_) {
    sampler_.OnAppLimited();
  }
}

// TODO(ianswett): Move this logic into BandwidthSampler.
QuicByteCount CoupleBbrSender::UpdateAckAggregationBytes(
    ProtoTime ack_time,
    QuicByteCount newly_acked_bytes) {
  // Compute how many bytes are expected to be delivered, assuming max bandwidth
  // is correct.
  QuicByteCount expected_bytes_acked =
      max_bandwidth_.GetBest() * (ack_time - aggregation_epoch_start_time_);
  // Reset the current aggregation epoch as soon as the ack arrival rate is less
  // than or equal to the max bandwidth.
  if (aggregation_epoch_bytes_ <= expected_bytes_acked) {
    // Reset to start measuring a new aggregation epoch.
    aggregation_epoch_bytes_ = newly_acked_bytes;
    aggregation_epoch_start_time_ = ack_time;
    return 0;
  }

  // Compute how many extra bytes were delivered vs max bandwidth.
  // Include the bytes most recently acknowledged to account for stretch acks.
  aggregation_epoch_bytes_ += newly_acked_bytes;
  max_ack_height_.Update(aggregation_epoch_bytes_ - expected_bytes_acked,
                         round_trip_count_);
  return aggregation_epoch_bytes_ - expected_bytes_acked;
}

void CoupleBbrSender::CalculatePacingRate() {
  if (BandwidthEstimate().IsZero()) {
    return;
  }

  QuicBandwidth target_rate = pacing_gain_ * BandwidthEstimate();
  if (is_at_full_bandwidth_) {
    pacing_rate_ = target_rate;
    return;
  }

  // Pace at the rate of initial_window / RTT as soon as RTT measurements are
  // available.
  if (pacing_rate_.IsZero() && !rtt_stats_->min_rtt().IsZero()) {
    pacing_rate_ = QuicBandwidth::FromBytesAndTimeDelta(
        initial_congestion_window_, rtt_stats_->min_rtt());
    return;
  }
  // Slow the pacing rate in STARTUP once loss has ever been detected.
  const bool has_ever_detected_loss = end_recovery_at_.IsInitialized();
  if (slower_startup_ && has_ever_detected_loss &&
      has_non_app_limited_sample_) {
    pacing_rate_ = kStartupAfterLossGain * BandwidthEstimate();
    return;
  }

  // Slow the pacing rate in STARTUP by the bytes_lost / CWND.
  if (startup_rate_reduction_multiplier_ != 0 && has_ever_detected_loss &&
      has_non_app_limited_sample_) {
    pacing_rate_ =
        (1 - (startup_bytes_lost_ * startup_rate_reduction_multiplier_ * 1.0f /
              congestion_window_)) *
        target_rate;
    // Ensure the pacing rate doesn't drop below the startup growth target times
    // the bandwidth estimate.
    pacing_rate_ =
        std::max(pacing_rate_, kStartupGrowthTarget * BandwidthEstimate());
    return;
  }

  // Do not decrease the pacing rate during startup.
  pacing_rate_ = std::max(pacing_rate_, target_rate);
}

void CoupleBbrSender::CalculateCongestionWindow(QuicByteCount bytes_acked,
                                          QuicByteCount excess_acked) {
  if (mode_ == PROBE_RTT) {
    return;
  }

  QuicByteCount target_window =
      GetTargetCongestionWindow(congestion_window_gain_);
  if (is_at_full_bandwidth_) {
    // Add the max recently measured ack aggregation to CWND.
    target_window += max_ack_height_.GetBest();
  } else if (enable_ack_aggregation_during_startup_) {
    // Add the most recent excess acked.  Because CWND never decreases in
    // STARTUP, this will automatically create a very localized max filter.
    target_window += excess_acked;
  }

  // Instead of immediately setting the target CWND as the new one, BBR grows
  // the CWND towards |target_window| by only increasing it |bytes_acked| at a
  // time.
  const bool add_bytes_acked =
      !GetQuicReloadableFlag(quic_bbr_no_bytes_acked_in_startup_recovery) ||
      !InRecovery();
  if (is_at_full_bandwidth_) {
    congestion_window_ =
        std::min(target_window, congestion_window_ + bytes_acked);
  } else if (add_bytes_acked &&
             (congestion_window_ < target_window ||
              sampler_.total_bytes_acked() < initial_congestion_window_)) {
    // If the connection is not yet out of startup phase, do not decrease the
    // window.
    congestion_window_ = congestion_window_ + bytes_acked;
  }
  // Enforce the limits on the congestion window.
  congestion_window_ = std::max(congestion_window_, min_congestion_window_);
  congestion_window_ = std::min(congestion_window_, max_congestion_window_);
  if(alpha_gain_is_negative_){
      congestion_window_=min_congestion_window_;
  }
}

void CoupleBbrSender::CalculateRecoveryWindow(QuicByteCount bytes_acked,
                                        QuicByteCount bytes_lost) {
  if (rate_based_startup_ && mode_ == STARTUP) {
    return;
  }

  if (recovery_state_ == NOT_IN_RECOVERY) {
    return;
  }

  // Set up the initial recovery window.
  if (recovery_window_ == 0) {
    recovery_window_ = unacked_packets_->bytes_in_flight() + bytes_acked;
    recovery_window_ = std::max(min_congestion_window_, recovery_window_);
    return;
  }

  // Remove losses from the recovery window, while accounting for a potential
  // integer underflow.
  recovery_window_ = recovery_window_ >= bytes_lost
                         ? recovery_window_ - bytes_lost
                         : kMaxSegmentSize;

  // In CONSERVATION mode, just subtracting losses is sufficient.  In GROWTH,
  // release additional |bytes_acked| to achieve a slow-start-like behavior.
  if (recovery_state_ == GROWTH) {
    recovery_window_ += bytes_acked;
  }

  // Sanity checks.  Ensure that we always allow to send at least an MSS or
  // |bytes_acked| in response, whichever is larger.
  recovery_window_ = std::max(
      recovery_window_, unacked_packets_->bytes_in_flight() + bytes_acked);
  if (GetQuicReloadableFlag(quic_bbr_one_mss_conservation)) {
    recovery_window_ =
        std::max(recovery_window_,
                 unacked_packets_->bytes_in_flight() + kMaxSegmentSize);
  }
  recovery_window_ = std::max(min_congestion_window_, recovery_window_);
}

std::string CoupleBbrSender::GetDebugState() const {
  std::ostringstream stream;
  stream << ExportDebugState();
  return stream.str();
}

void CoupleBbrSender::OnApplicationLimited(QuicByteCount bytes_in_flight) {
  if (bytes_in_flight >= GetCongestionWindow()) {
    return;
  }
  if (flexible_app_limited_ && IsPipeSufficientlyFull()) {
    return;
  }

  app_limited_since_last_probe_rtt_ = true;
  sampler_.OnAppLimited();
  DLOG(INFO) << "Becoming application limited. Last sent packet: "
                << last_sent_packet_ << ", CWND: " << GetCongestionWindow();
}

CoupleBbrSender::DebugState CoupleBbrSender::ExportDebugState() const {
  return DebugState(*this);
}
void CoupleBbrSender::SetCongestionId(uint32_t cid){
	if(congestion_id_!=0||cid==0){
		return;
	}
	congestion_id_=cid;
	CoupleManager::Instance()->OnCongestionCreate(this);
}
void CoupleBbrSender::RegisterCoupleCC(SendAlgorithmInterface*cc){
    CoupleBbrSender *sender=dynamic_cast<CoupleBbrSender*>(cc);
    other_ccs_.push_back(sender);
}
void CoupleBbrSender::UnRegisterCoupleCC(SendAlgorithmInterface*cc){
    CoupleBbrSender *sender=dynamic_cast<CoupleBbrSender*>(cc);
    /*for(auto it=other_ccs_.begin();it!=other_ccs_.end();){
        if((*it)==sender){
            other_ccs_.erase(it++);
            break;
        }else{
            it++;
        }
    }*/
    if(!other_ccs_.empty()){
        other_ccs_.remove(sender);
    }
}
// End implementation of SendAlgorithmInterface.
bool CoupleBbrSender::IsInProbeMode() const{
    return mode_==PROBE_BW||mode_==PROBE_RTT;
}
void CoupleBbrSender::CalculateAlphaPacingGain(){
    if(cycle_len_<=2){
        return;
    }
    QuicBandwidth selfBandwidth=BandwidthEstimate();
    QuicBandwidth maxBandwidth=selfBandwidth;
    double bps=selfBandwidth.ToBitsPerSecond();
    double selfbps=bps;
    double accBandwidthSquare=0.0;
    std::vector<double> rate;
    rate.push_back(bps);
    for(auto it=other_ccs_.begin();it!=other_ccs_.end();it++){
        QuicBandwidth bandwidth=(*it)->BandwidthEstimate();
        bps=bandwidth.ToBitsPerSecond();
        if(bandwidth>maxBandwidth){
            maxBandwidth=bandwidth;
        }
        rate.push_back(bps);
    }
    double max_bps=maxBandwidth.ToBitsPerSecond();
    for(auto it=rate.begin();it!=rate.end();it++){
        bps=*it;
        accBandwidthSquare=accBandwidthSquare+(bps/max_bps)*bps;
    }
    double alpha=1.0,beta=1.0;
    beta=selfbps/accBandwidthSquare;
    alpha=(cycle_len_*beta-2)/(cycle_len_-2);
    if(alpha<=0){
		//std::cout<<congestion_id_<<" "<<alpha<<" "<<beta<<" "<<selfbps<<std::endl;
        alpha_gain_is_negative_=true;
        return ;
    }
    pacing_gain_=alpha;
}
bool CoupleBbrSender::ShouldBehaveFriendlyToSinglepath() const{
    bool supress_pacing_gain=false;
    if(mode_==PROBE_BW&&cycle_state_==BBR_BW_PROBE_CRUISE){
        supress_pacing_gain=true;
    }
    return supress_pacing_gain;
}
static std::string ModeToString(CoupleBbrSender::Mode mode) {
  switch (mode) {
    case CoupleBbrSender::STARTUP:
      return "STARTUP";
    case CoupleBbrSender::DRAIN:
      return "DRAIN";
    case CoupleBbrSender::PROBE_BW:
      return "PROBE_BW";
    case CoupleBbrSender::PROBE_RTT:
      return "PROBE_RTT";
  }
  return "???";
}

std::ostream& operator<<(std::ostream& os, const CoupleBbrSender::Mode& mode) {
  os << ModeToString(mode);
  return os;
}

std::ostream& operator<<(std::ostream& os, const CoupleBbrSender::DebugState& state) {
  os << "Mode: " << ModeToString(state.mode) << std::endl;
  os << "Maximum bandwidth: " << state.max_bandwidth << std::endl;
  os << "Round trip counter: " << state.round_trip_count << std::endl;
  os << "Gain cycle index: " << static_cast<int>(state.gain_cycle_index)
     << std::endl;
  os << "Congestion window: " << state.congestion_window << " bytes"
     << std::endl;

  if (state.mode == CoupleBbrSender::STARTUP) {
    os << "(startup) Bandwidth at last round: " << state.bandwidth_at_last_round
       << std::endl;
    os << "(startup) Rounds without gain: "
       << state.rounds_without_bandwidth_gain << std::endl;
  }

  os << "Minimum RTT: " << state.min_rtt << std::endl;
  os << "Minimum RTT timestamp: " << state.min_rtt_timestamp.ToDebuggingValue()
     << std::endl;

  os << "Last sample is app-limited: "
     << (state.last_sample_is_app_limited ? "yes" : "no");

  return os;
}

}


