/**
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srsenb/hdr/stack/mac/nr/sched_nr_rb_grid.h"
#include "srsenb/hdr/stack/mac/nr/sched_nr_helpers.h"

namespace srsenb {
namespace sched_nr_impl {

#define NUMEROLOGY_IDX 0

bwp_slot_grid::bwp_slot_grid(const bwp_params& bwp_cfg_, uint32_t slot_idx_) :
  dl_prbs(bwp_cfg_.cfg.rb_width, bwp_cfg_.cfg.start_rb, bwp_cfg_.cfg.pdsch.rbg_size_cfg_1),
  ul_prbs(bwp_cfg_.cfg.rb_width, bwp_cfg_.cfg.start_rb, bwp_cfg_.cfg.pdsch.rbg_size_cfg_1),
  slot_idx(slot_idx_),
  cfg(&bwp_cfg_),
  is_dl(srsran_tdd_nr_is_dl(&bwp_cfg_.cell_cfg.tdd, NUMEROLOGY_IDX, slot_idx_)),
  is_ul(srsran_tdd_nr_is_ul(&bwp_cfg_.cell_cfg.tdd, NUMEROLOGY_IDX, slot_idx_))
{
  for (uint32_t cs_idx = 0; cs_idx < SRSRAN_UE_DL_NR_MAX_NOF_CORESET; ++cs_idx) {
    if (cfg->cfg.pdcch.coreset_present[cs_idx]) {
      uint32_t cs_id = cfg->cfg.pdcch.coreset[cs_idx].id;
      coresets[cs_id].emplace(*cfg, cs_id, slot_idx_, dl_pdcchs, ul_pdcchs);
    }
  }
}

void bwp_slot_grid::reset()
{
  for (auto& coreset : coresets) {
    if (coreset.has_value()) {
      coreset->reset();
    }
  }
  dl_prbs.reset();
  ul_prbs.reset();
  dl_pdcchs.clear();
  ul_pdcchs.clear();
  pending_acks.clear();
}

bwp_res_grid::bwp_res_grid(const bwp_params& bwp_cfg_) : cfg(&bwp_cfg_)
{
  for (uint32_t sl = 0; sl < slots.capacity(); ++sl) {
    slots.emplace_back(*cfg, sl % static_cast<uint32_t>(SRSRAN_NSLOTS_PER_FRAME_NR(0u)));
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bwp_slot_allocator::bwp_slot_allocator(bwp_res_grid& bwp_grid_) :
  logger(srslog::fetch_basic_logger("MAC")), cfg(*bwp_grid_.cfg), bwp_grid(bwp_grid_)
{}

alloc_result bwp_slot_allocator::alloc_rar(uint32_t                                    aggr_idx,
                                           const srsenb::sched_nr_impl::pending_rar_t& rar,
                                           prb_interval                                interv,
                                           uint32_t                                    nof_grants)
{
  static const uint32_t msg3_nof_prbs = 3;

  bwp_slot_grid& bwp_pdcch_slot = bwp_grid[pdcch_tti];
  bwp_slot_grid& bwp_msg3_slot  = bwp_grid[pdcch_tti + 4];

  if (bwp_pdcch_slot.dl_pdcchs.full()) {
    logger.warning("SCHED: Maximum number of DL allocations reached");
    return alloc_result::no_grant_space;
  }

  // Check DL RB collision
  const prb_bitmap& pdsch_mask = bwp_pdcch_slot.dl_prbs.prbs();
  prb_bitmap        dl_mask(pdsch_mask.size());
  dl_mask.fill(interv.start(), interv.stop());
  if ((pdsch_mask & dl_mask).any()) {
    logger.debug("SCHED: Provided RBG mask collides with allocation previously made.");
    return alloc_result::sch_collision;
  }

  // Check Msg3 RB collision
  uint32_t     total_ul_nof_prbs = msg3_nof_prbs * nof_grants;
  uint32_t     total_ul_nof_rbgs = srsran::ceil_div(total_ul_nof_prbs, get_P(bwp_grid.nof_prbs(), false));
  prb_interval msg3_rbgs         = find_empty_interval_of_length(bwp_msg3_slot.ul_prbs.prbs(), total_ul_nof_rbgs);
  if (msg3_rbgs.length() < total_ul_nof_rbgs) {
    logger.debug("SCHED: No space in PUSCH for Msg3.");
    return alloc_result::sch_collision;
  }

  // Find PDCCH position
  const uint32_t coreset_id      = cfg.cfg.pdcch.ra_search_space.coreset_id;
  const uint32_t search_space_id = cfg.cfg.pdcch.ra_search_space.id;
  if (not bwp_pdcch_slot.coresets[coreset_id]->alloc_dci(pdcch_grant_type_t::rar, aggr_idx, search_space_id, nullptr)) {
    // Could not find space in PDCCH
    logger.debug("SCHED: No space in PDCCH for DL tx.");
    return alloc_result::no_cch_space;
  }

  // Generate DCI for RAR
  pdcch_dl_t& pdcch = bwp_pdcch_slot.dl_pdcchs.back();
  if (not fill_dci_rar(interv, *bwp_grid.cfg, pdcch.dci)) {
    // Cancel on-going PDCCH allocation
    bwp_pdcch_slot.coresets[coreset_id]->rem_last_dci();
    return alloc_result::invalid_coderate;
  }

  // RAR allocation successful.
  bwp_pdcch_slot.dl_prbs.add(interv);

  return alloc_result::success;
}

alloc_result bwp_slot_allocator::alloc_pdsch(slot_ue& ue, const prb_grant& dl_grant)
{
  if (ue.cfg->active_bwp().bwp_id != bwp_grid.cfg->bwp_id) {
    logger.warning(
        "SCHED: Trying to allocate PDSCH for rnti=0x%x in inactive BWP id=%d", ue.rnti, ue.cfg->active_bwp().bwp_id);
    return alloc_result::no_rnti_opportunity;
  }
  if (ue.h_dl == nullptr) {
    logger.warning("SCHED: Trying to allocate PDSCH for rnti=0x%x with no available HARQs", ue.rnti);
    return alloc_result::no_rnti_opportunity;
  }
  bwp_slot_grid& bwp_pdcch_slot = bwp_grid[ue.pdcch_tti];
  bwp_slot_grid& bwp_pdsch_slot = bwp_grid[ue.pdsch_tti];
  bwp_slot_grid& bwp_uci_slot   = bwp_grid[ue.uci_tti];
  if (not bwp_pdsch_slot.is_dl) {
    logger.warning("SCHED: Trying to allocate PDSCH in TDD non-DL slot index=%d", bwp_pdsch_slot.slot_idx);
    return alloc_result::no_sch_space;
  }
  pdcch_dl_list_t& pdsch_grants = bwp_pdsch_slot.dl_pdcchs;
  if (pdsch_grants.full()) {
    logger.warning("SCHED: Maximum number of DL allocations reached");
    return alloc_result::no_grant_space;
  }
  if (bwp_pdcch_slot.dl_prbs.collides(dl_grant)) {
    return alloc_result::sch_collision;
  }

  // Find space in PUCCH
  // TODO

  // Find space and allocate PDCCH
  const uint32_t aggr_idx = 2, ss_id = 1;
  uint32_t       coreset_id = ue.cfg->phy().pdcch.search_space[ss_id].coreset_id;
  if (not bwp_pdcch_slot.coresets[coreset_id]->alloc_dci(pdcch_grant_type_t::dl_data, aggr_idx, ss_id, &ue)) {
    // Could not find space in PDCCH
    return alloc_result::no_cch_space;
  }

  // Allocate HARQ
  if (ue.h_dl->empty()) {
    int  mcs = 20;
    int  tbs = 100;
    bool ret = ue.h_dl->new_tx(ue.pdsch_tti, ue.uci_tti, dl_grant, mcs, tbs, 4);
    srsran_assert(ret, "Failed to allocate DL HARQ");
  } else {
    bool ret = ue.h_dl->new_retx(ue.pdsch_tti, ue.uci_tti, dl_grant);
    srsran_assert(ret, "Failed to allocate DL HARQ retx");
  }

  // Allocation Successful

  // Generate PDCCH
  pdcch_dl_t& pdcch = bwp_pdcch_slot.dl_pdcchs.back();
  fill_dl_dci_ue_fields(ue, *bwp_grid.cfg, ss_id, pdcch.dci.ctx.location, pdcch.dci);
  pdcch.dci.pucch_resource = 0;
  pdcch.dci.dai            = std::count_if(bwp_uci_slot.pending_acks.begin(),
                                bwp_uci_slot.pending_acks.end(),
                                [&ue](const harq_ack_t& p) { return p.res.rnti == ue.rnti; });
  pdcch.dci.dai %= 4;

  // Generate PUCCH
  bwp_uci_slot.pending_acks.emplace_back();
  bwp_uci_slot.pending_acks.back().phy_cfg = &ue.cfg->phy();
  srsran_assert(ue.cfg->phy().get_pdsch_ack_resource(pdcch.dci, bwp_uci_slot.pending_acks.back().res),
                "Error getting ack resource");

  // Generate PDSCH
  bwp_pdsch_slot.dl_prbs |= dl_grant;
  bwp_pdsch_slot.pdschs.emplace_back();
  pdsch_t&          pdsch = bwp_pdsch_slot.pdschs.back();
  srsran_slot_cfg_t slot_cfg;
  slot_cfg.idx = ue.pdsch_tti.sf_idx();
  bool ret     = ue.cfg->phy().get_pdsch_cfg(slot_cfg, pdcch.dci, pdsch.sch);
  srsran_assert(ret, "Error converting DCI to grant");
  if (ue.h_dl->nof_retx() == 0) {
    ue.h_dl->set_tbs(pdsch.sch.grant.tb[0].tbs); // update HARQ with correct TBS
  } else {
    srsran_assert(pdsch.sch.grant.tb[0].tbs == (int)ue.h_dl->tbs(), "The TBS did not remain constant in retx");
  }
  pdsch.sch.grant.tb[0].softbuffer.tx = ue.h_dl->get_softbuffer().get();

  return alloc_result::success;
}

alloc_result bwp_slot_allocator::alloc_pusch(slot_ue& ue, const rbg_bitmap& ul_mask)
{
  if (ue.h_ul == nullptr) {
    logger.warning("SCHED: Trying to allocate PUSCH for rnti=0x%x with no available HARQs", ue.rnti);
    return alloc_result::no_rnti_opportunity;
  }
  auto& bwp_pdcch_slot = bwp_grid[ue.pdcch_tti];
  auto& bwp_pusch_slot = bwp_grid[ue.pusch_tti];
  if (not bwp_pusch_slot.is_ul) {
    logger.warning("SCHED: Trying to allocate PUSCH in TDD non-UL slot index=%d", bwp_pusch_slot.slot_idx);
    return alloc_result::no_sch_space;
  }
  pdcch_ul_list_t& pdcchs = bwp_pdcch_slot.ul_pdcchs;
  if (pdcchs.full()) {
    logger.warning("SCHED: Maximum number of UL allocations reached");
    return alloc_result::no_grant_space;
  }
  const rbg_bitmap& pusch_mask = bwp_pusch_slot.ul_prbs.rbgs();
  if ((pusch_mask & ul_mask).any()) {
    return alloc_result::sch_collision;
  }
  const uint32_t aggr_idx = 2, ss_id = 1;
  uint32_t       coreset_id = ue.cfg->phy().pdcch.search_space[ss_id].coreset_id;
  if (not bwp_pdcch_slot.coresets[coreset_id].value().alloc_dci(pdcch_grant_type_t::ul_data, aggr_idx, ss_id, &ue)) {
    // Could not find space in PDCCH
    return alloc_result::no_cch_space;
  }

  if (ue.h_ul->empty()) {
    int  mcs = 20;
    int  tbs = 100;
    bool ret = ue.h_ul->new_tx(ue.pusch_tti, ue.pusch_tti, ul_mask, mcs, tbs, ue.cfg->ue_cfg()->maxharq_tx);
    srsran_assert(ret, "Failed to allocate UL HARQ");
  } else {
    srsran_assert(ue.h_ul->new_retx(ue.pusch_tti, ue.pusch_tti, ul_mask), "Failed to allocate UL HARQ retx");
  }

  // Allocation Successful
  // Generate PDCCH
  pdcch_ul_t& pdcch = pdcchs.back();
  fill_ul_dci_ue_fields(ue, *bwp_grid.cfg, ss_id, pdcch.dci.ctx.location, pdcch.dci);
  // Generate PUSCH
  bwp_pusch_slot.ul_prbs.add(ul_mask);

  return alloc_result::success;
}

} // namespace sched_nr_impl
} // namespace srsenb