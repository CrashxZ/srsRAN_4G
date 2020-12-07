/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include <srsenb/hdr/stack/mac/sched_ue.h>
#include <string.h>

#include "srsenb/hdr/stack/mac/sched.h"
#include "srsenb/hdr/stack/mac/sched_carrier.h"
#include "srsenb/hdr/stack/mac/sched_helpers.h"
#include "srslte/common/logmap.h"
#include "srslte/srslte.h"

#define Console(fmt, ...) srslte::console(fmt, ##__VA_ARGS__)
#define Error(fmt, ...) srslte::logmap::get("MAC ")->error(fmt, ##__VA_ARGS__)

using srslte::tti_point;

namespace srsenb {

/*******************************************************
 *
 * Initialization and sched configuration functions
 *
 *******************************************************/

sched::sched() : log_h(srslte::logmap::get("MAC")) {}

sched::~sched() {}

void sched::init(rrc_interface_mac* rrc_)
{
  rrc = rrc_;

  // Initialize first carrier scheduler
  carrier_schedulers.emplace_back(new carrier_sched{rrc, &ue_db, 0, &sched_results});

  reset();
}

int sched::reset()
{
  std::lock_guard<std::mutex> lock(sched_mutex);
  configured = false;
  for (std::unique_ptr<carrier_sched>& c : carrier_schedulers) {
    c->reset();
  }
  ue_db.clear();
  return 0;
}

void sched::set_sched_cfg(sched_interface::sched_args_t* sched_cfg_)
{
  std::lock_guard<std::mutex> lock(sched_mutex);
  if (sched_cfg_ != nullptr) {
    sched_cfg = *sched_cfg_;
  }
}

int sched::cell_cfg(const std::vector<sched_interface::cell_cfg_t>& cell_cfg)
{
  std::lock_guard<std::mutex> lock(sched_mutex);
  // Setup derived config params
  sched_cell_params.resize(cell_cfg.size());
  for (uint32_t cc_idx = 0; cc_idx < cell_cfg.size(); ++cc_idx) {
    if (not sched_cell_params[cc_idx].set_cfg(cc_idx, cell_cfg[cc_idx], sched_cfg)) {
      return SRSLTE_ERROR;
    }
  }

  // Create remaining cells, if not created yet
  uint32_t prev_size = carrier_schedulers.size();
  carrier_schedulers.resize(sched_cell_params.size());
  for (uint32_t i = prev_size; i < sched_cell_params.size(); ++i) {
    carrier_schedulers[i].reset(new carrier_sched{rrc, &ue_db, i, &sched_results});
  }

  // setup all carriers cfg params
  for (uint32_t i = 0; i < sched_cell_params.size(); ++i) {
    carrier_schedulers[i]->carrier_cfg(sched_cell_params[i]);
  }

  configured = true;

  return 0;
}

/*******************************************************
 *
 * FAPI-like main sched interface. Wrappers to UE object
 *
 *******************************************************/

int sched::ue_cfg(uint16_t rnti, const sched_interface::ue_cfg_t& ue_cfg)
{
  std::lock_guard<std::mutex> lock(sched_mutex);
  // Add or config user
  auto it = ue_db.find(rnti);
  if (it == ue_db.end()) {
    // create new user
    ue_db[rnti].init(rnti, sched_cell_params);
    it = ue_db.find(rnti);
  }
  it->second.set_cfg(ue_cfg);

  return SRSLTE_SUCCESS;
}

int sched::ue_rem(uint16_t rnti)
{
  std::lock_guard<std::mutex> lock(sched_mutex);
  if (ue_db.count(rnti) > 0) {
    ue_db.erase(rnti);
  } else {
    Error("User rnti=0x%x not found\n", rnti);
    return SRSLTE_ERROR;
  }
  return SRSLTE_SUCCESS;
}

bool sched::ue_exists(uint16_t rnti)
{
  return ue_db_access(rnti, [](sched_ue& ue) {}) >= 0;
}

void sched::phy_config_enabled(uint16_t rnti, bool enabled)
{
  // TODO: Check if correct use of last_tti
  ue_db_access(
      rnti, [this, enabled](sched_ue& ue) { ue.phy_config_enabled(last_tti, enabled); }, __PRETTY_FUNCTION__);
}

int sched::bearer_ue_cfg(uint16_t rnti, uint32_t lc_id, sched_interface::ue_bearer_cfg_t* cfg_)
{
  return ue_db_access(rnti, [lc_id, cfg_](sched_ue& ue) { ue.set_bearer_cfg(lc_id, *cfg_); });
}

int sched::bearer_ue_rem(uint16_t rnti, uint32_t lc_id)
{
  return ue_db_access(rnti, [lc_id](sched_ue& ue) { ue.rem_bearer(lc_id); });
}

uint32_t sched::get_dl_buffer(uint16_t rnti)
{
  uint32_t ret = SRSLTE_ERROR;
  ue_db_access(
      rnti, [&ret](sched_ue& ue) { ret = ue.get_pending_dl_rlc_data(); }, __PRETTY_FUNCTION__);
  return ret;
}

uint32_t sched::get_ul_buffer(uint16_t rnti)
{
  // TODO: Check if correct use of last_tti
  uint32_t ret = SRSLTE_ERROR;
  ue_db_access(
      rnti,
      [this, &ret](sched_ue& ue) { ret = ue.get_pending_ul_new_data(to_tx_ul(last_tti), -1); },
      __PRETTY_FUNCTION__);
  return ret;
}

int sched::dl_rlc_buffer_state(uint16_t rnti, uint32_t lc_id, uint32_t tx_queue, uint32_t retx_queue)
{
  return ue_db_access(rnti, [&](sched_ue& ue) { ue.dl_buffer_state(lc_id, tx_queue, retx_queue); });
}

int sched::dl_mac_buffer_state(uint16_t rnti, uint32_t ce_code, uint32_t nof_cmds)
{
  return ue_db_access(rnti, [ce_code, nof_cmds](sched_ue& ue) { ue.mac_buffer_state(ce_code, nof_cmds); });
}

int sched::dl_ack_info(uint32_t tti_rx, uint16_t rnti, uint32_t enb_cc_idx, uint32_t tb_idx, bool ack)
{
  int ret = -1;
  ue_db_access(
      rnti,
      [&](sched_ue& ue) { ret = ue.set_ack_info(tti_point{tti_rx}, enb_cc_idx, tb_idx, ack); },
      __PRETTY_FUNCTION__);
  return ret;
}

int sched::ul_crc_info(uint32_t tti_rx, uint16_t rnti, uint32_t enb_cc_idx, bool crc)
{
  return ue_db_access(rnti,
                      [tti_rx, enb_cc_idx, crc](sched_ue& ue) { ue.set_ul_crc(tti_point{tti_rx}, enb_cc_idx, crc); });
}

int sched::dl_ri_info(uint32_t tti, uint16_t rnti, uint32_t enb_cc_idx, uint32_t ri_value)
{
  return ue_db_access(
      rnti, [tti, enb_cc_idx, ri_value](sched_ue& ue) { ue.set_dl_ri(tti_point{tti}, enb_cc_idx, ri_value); });
}

int sched::dl_pmi_info(uint32_t tti, uint16_t rnti, uint32_t enb_cc_idx, uint32_t pmi_value)
{
  return ue_db_access(
      rnti, [tti, enb_cc_idx, pmi_value](sched_ue& ue) { ue.set_dl_pmi(tti_point{tti}, enb_cc_idx, pmi_value); });
}

int sched::dl_cqi_info(uint32_t tti, uint16_t rnti, uint32_t enb_cc_idx, uint32_t cqi_value)
{
  return ue_db_access(
      rnti, [tti, enb_cc_idx, cqi_value](sched_ue& ue) { ue.set_dl_cqi(tti_point{tti}, enb_cc_idx, cqi_value); });
}

int sched::dl_rach_info(uint32_t enb_cc_idx, dl_sched_rar_info_t rar_info)
{
  std::lock_guard<std::mutex> lock(sched_mutex);
  return carrier_schedulers[enb_cc_idx]->dl_rach_info(rar_info);
}

int sched::ul_snr_info(uint32_t tti_rx, uint16_t rnti, uint32_t enb_cc_idx, float snr, uint32_t ul_ch_code)
{
  return ue_db_access(rnti, [&](sched_ue& ue) { ue.set_ul_snr(tti_point{tti_rx}, enb_cc_idx, snr, ul_ch_code); });
}

int sched::ul_bsr(uint16_t rnti, uint32_t lcg_id, uint32_t bsr)
{
  return ue_db_access(rnti, [lcg_id, bsr](sched_ue& ue) { ue.ul_buffer_state(lcg_id, bsr); });
}

int sched::ul_buffer_add(uint16_t rnti, uint32_t lcid, uint32_t bytes)
{
  return ue_db_access(rnti, [lcid, bytes](sched_ue& ue) { ue.ul_buffer_add(lcid, bytes); });
}

int sched::ul_phr(uint16_t rnti, int phr)
{
  return ue_db_access(
      rnti, [phr](sched_ue& ue) { ue.ul_phr(phr); }, __PRETTY_FUNCTION__);
}

int sched::ul_sr_info(uint32_t tti, uint16_t rnti)
{
  return ue_db_access(
      rnti, [](sched_ue& ue) { ue.set_sr(); }, __PRETTY_FUNCTION__);
}

void sched::set_dl_tti_mask(uint8_t* tti_mask, uint32_t nof_sfs)
{
  std::lock_guard<std::mutex> lock(sched_mutex);
  carrier_schedulers[0]->set_dl_tti_mask(tti_mask, nof_sfs);
}

std::array<int, SRSLTE_MAX_CARRIERS> sched::get_enb_ue_cc_map(uint16_t rnti)
{
  std::array<int, SRSLTE_MAX_CARRIERS> ret{};
  ret.fill(-1); // -1 for inactive & non-existent carriers
  ue_db_access(
      rnti,
      [this, &ret](sched_ue& ue) {
        for (size_t enb_cc_idx = 0; enb_cc_idx < carrier_schedulers.size(); ++enb_cc_idx) {
          const cc_sched_ue* cc_ue = ue.find_ue_carrier(enb_cc_idx);
          if (cc_ue != nullptr) {
            ret[enb_cc_idx] = cc_ue->get_ue_cc_idx();
          }
        }
      },
      __PRETTY_FUNCTION__);
  return ret;
}

std::array<bool, SRSLTE_MAX_CARRIERS> sched::get_scell_activation_mask(uint16_t rnti)
{
  std::array<int, SRSLTE_MAX_CARRIERS>  enb_ue_cc_map = get_enb_ue_cc_map(rnti);
  std::array<bool, SRSLTE_MAX_CARRIERS> scell_mask    = {};
  for (int ue_cc : enb_ue_cc_map) {
    if (ue_cc <= 0) {
      // inactive or PCell
      continue;
    }
    scell_mask[ue_cc] = true;
  }
  return scell_mask;
}

/*******************************************************
 *
 * Main sched functions
 *
 *******************************************************/

// Downlink Scheduler API
int sched::dl_sched(uint32_t tti_tx_dl, uint32_t enb_cc_idx, sched_interface::dl_sched_res_t& sched_result)
{
  if (!configured) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(sched_mutex);
  if (enb_cc_idx >= carrier_schedulers.size()) {
    return 0;
  }

  tti_point tti_rx = tti_point{tti_tx_dl} - FDD_HARQ_DELAY_UL_MS;
  new_tti(tti_rx);

  // copy result
  sched_result = sched_results.get_sf(tti_rx)->get_cc(enb_cc_idx)->dl_sched_result;

  return 0;
}

// Uplink Scheduler API
int sched::ul_sched(uint32_t tti, uint32_t enb_cc_idx, srsenb::sched_interface::ul_sched_res_t& sched_result)
{
  if (!configured) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(sched_mutex);
  if (enb_cc_idx >= carrier_schedulers.size()) {
    return 0;
  }

  // Compute scheduling Result for tti_rx
  tti_point tti_rx = tti_point{tti} - FDD_HARQ_DELAY_UL_MS - FDD_HARQ_DELAY_DL_MS;
  new_tti(tti_rx);

  // copy result
  sched_result = sched_results.get_sf(tti_rx)->get_cc(enb_cc_idx)->ul_sched_result;

  return SRSLTE_SUCCESS;
}

/// Generate scheduling decision for tti_rx, if it wasn't already generated
/// NOTE: The scheduling decision is made for all CCs in a single call/lock, otherwise the UE can have different
///       configurations (e.g. different set of activated SCells) in different CC decisions
void sched::new_tti(tti_point tti_rx)
{
  last_tti = std::max(last_tti, tti_rx);

  // Generate sched results for all CCs, if not yet generated
  for (size_t cc_idx = 0; cc_idx < carrier_schedulers.size(); ++cc_idx) {
    if (not is_generated(tti_rx, cc_idx)) {
      // Generate carrier scheduling result
      carrier_schedulers[cc_idx]->generate_tti_result(tti_rx);
    }
  }
}

/// Check if TTI result is generated
bool sched::is_generated(srslte::tti_point tti_rx, uint32_t enb_cc_idx) const
{
  const sf_sched_result* sf_result = sched_results.get_sf(tti_rx);
  return sf_result != nullptr and sf_result->get_cc(enb_cc_idx) != nullptr and
         sf_result->get_cc(enb_cc_idx)->is_generated(tti_rx);
}

// Common way to access ue_db elements in a read locking way
template <typename Func>
int sched::ue_db_access(uint16_t rnti, Func f, const char* func_name)
{
  std::lock_guard<std::mutex> lock(sched_mutex);
  auto                        it = ue_db.find(rnti);
  if (it != ue_db.end()) {
    f(it->second);
  } else {
    if (func_name != nullptr) {
      Error("User rnti=0x%x not found. Failed to call %s.\n", rnti, func_name);
    } else {
      Error("User rnti=0x%x not found.\n", rnti);
    }
    return SRSLTE_ERROR;
  }
  return SRSLTE_SUCCESS;
}

} // namespace srsenb