/******************************************************************************
* Copyright (C) 2018, Divideon.
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation; either
* version 2.1 of the License, or (at your option) any later version.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*
* This library is also available under a commercial license.
* Please visit https://xvc.io/license/ for more information.
******************************************************************************/

#include "xvc_enc_lib/cu_encoder.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>
#include <utility>

#include "xvc_common_lib/restrictions.h"
#include "xvc_common_lib/utils.h"
#include "xvc_enc_lib/sample_metric.h"

namespace xvc {

struct CuEncoder::RdoCost {
  RdoCost() = default;
  explicit RdoCost(Cost c) : cost(c), dist(0) {}
  RdoCost(Cost c, Distortion d) : cost(c), dist(d) {}
  bool operator<(const RdoCost &other) { return cost < other.cost; }
  Cost cost;
  Distortion dist;
};

CuEncoder::CuEncoder(const EncoderSimdFunctions &simd,
                     const YuvPicture &orig_pic, YuvPicture *rec_pic,
                     PictureData *pic_data,
                     const EncoderSettings &encoder_settings)
  : TransformEncoder(simd, rec_pic->GetBitdepth(),
                     pic_data->GetMaxNumComponents(),
                     orig_pic, encoder_settings),
  orig_pic_(orig_pic),
  encoder_settings_(encoder_settings),
  rec_pic_(*rec_pic),
  pic_data_(*pic_data),
  inter_search_(simd, *pic_data, orig_pic, *rec_pic,
                *pic_data->GetRefPicLists(), encoder_settings),
  intra_search_(simd, rec_pic->GetBitdepth(), *pic_data, orig_pic,
                encoder_settings),
  cu_writer_(pic_data_, &intra_search_),
  cu_cache_(pic_data) {
  for (int tree_idx = 0; tree_idx < constants::kMaxNumCuTrees; tree_idx++) {
    const CuTree cu_tree = static_cast<CuTree>(tree_idx);
    const int max_depth = static_cast<int>(rdo_temp_cu_[tree_idx].size());
    for (int depth = 0; depth < max_depth; depth++) {
      rdo_temp_cu_[tree_idx][depth] =
        pic_data_.CreateCu(cu_tree, depth, -1, -1, 0, 0);
    }
  }
}

CuEncoder::~CuEncoder() {
  for (int tree_idx = 0; tree_idx < constants::kMaxNumCuTrees; tree_idx++) {
    const int max_depth = static_cast<int>(rdo_temp_cu_[tree_idx].size());
    for (int depth = 0; depth < max_depth; depth++) {
      // Invariant: These must be released after coding each picture since
      // cu objects are recycled without any reference counting
      pic_data_.ReleaseCu(rdo_temp_cu_[tree_idx][depth]);
    }
  }
}

void CuEncoder::EncodeCtu(int rsaddr, SyntaxWriter *bitstream_writer) {
  uint32_t frac_bits = bitstream_writer->GetFractionalBits();
  if (!EncoderSettings::kEncoderCountActualWrittenBits) {
    frac_bits = rsaddr == 0 ? 0 : last_ctu_frac_bits_;
  }
  RdoSyntaxWriter rdo_writer(*bitstream_writer, 0, frac_bits);

  CodingUnit *ctu = pic_data_.GetCtu(CuTree::Primary, rsaddr);
  int ctu_qp = pic_data_.GetPicQp()->GetQpRaw(YuvComponent::kY);
  if (encoder_settings_.adaptive_qp) {
    ctu_qp += CalcDeltaQpFromVariance(ctu);
  }
  ctu->SetQp(ctu_qp);
  CompressCu(&ctu, 0, SplitRestriction::kNone, &rdo_writer, ctu->GetQp());
  pic_data_.SetCtu(CuTree::Primary, rsaddr, ctu);
  if (pic_data_.HasSecondaryCuTree()) {
    CodingUnit *ctu2 = pic_data_.GetCtu(CuTree::Secondary, rsaddr);
    ctu2->SetQp(ctu_qp);
    if (EncoderSettings::kEncoderStrictRdoBitCounting) {
      CompressCu(&ctu2, 0, SplitRestriction::kNone, &rdo_writer, ctu2->GetQp());
    } else {
      RdoSyntaxWriter rdo_writer2(*bitstream_writer);
      CompressCu(&ctu2, 0, SplitRestriction::kNone, &rdo_writer2,
                 ctu2->GetQp());
    }
    pic_data_.SetCtu(CuTree::Secondary, rsaddr, ctu2);
  }
  last_ctu_frac_bits_ = rdo_writer.GetFractionalBits();

  WriteCtu(rsaddr, bitstream_writer);
  if (EncoderSettings::kEncoderStrictRdoBitCounting &&
      EncoderSettings::kEncoderCountActualWrittenBits) {
    assert(rdo_writer.GetNumWrittenBits() ==
           bitstream_writer->GetNumWrittenBits());
    assert(rdo_writer.GetFractionalBits() ==
           bitstream_writer->GetFractionalBits());
  }
}

Distortion CuEncoder::CompressCu(CodingUnit **best_cu, int rdo_depth,
                                 SplitRestriction split_restiction,
                                 RdoSyntaxWriter *writer, const Qp &qp) {
  const int kMaxTrSize =
    !Restrictions::Get().disable_ext_transform_size_64 ? 64 : 32;
  CodingUnit *cu = *best_cu;  // Invariant: cu always points to *best_cu
  cu->SetQp(qp);
  const int cu_tree = static_cast<int>(cu->GetCuTree());
  const int depth = cu->GetDepth();
  const bool do_quad_split = cu->GetBinaryDepth() == 0 &&
    depth < pic_data_.GetMaxDepth(cu->GetCuTree());
  const bool can_binary_split = cu->IsBinarySplitValid() &&
    cu->IsFullyWithinPicture() &&
    cu->GetWidth(YuvComponent::kY) <= kMaxTrSize &&
    cu->GetHeight(YuvComponent::kY) <= kMaxTrSize;
  const bool do_hor_split = can_binary_split &&
    split_restiction != SplitRestriction::kNoHorizontal &&
    cu->GetHeight(YuvComponent::kY) > constants::kMinBinarySplitSize;
  const bool do_ver_split = can_binary_split &&
    split_restiction != SplitRestriction::kNoVertical &&
    cu->GetWidth(YuvComponent::kY) > constants::kMinBinarySplitSize;
  const bool do_full = cu->IsFullyWithinPicture() &&
    cu->GetWidth(YuvComponent::kY) <= kMaxTrSize &&
    cu->GetHeight(YuvComponent::kY) <= kMaxTrSize;
  const bool do_split_any = do_quad_split || do_hor_split || do_ver_split;
  assert(do_full || do_split_any);

  if (!do_split_any) {
    return CompressNoSplit(best_cu, rdo_depth, split_restiction, writer);
  }
  RdoCost best_cost(std::numeric_limits<Cost>::max());
  CodingUnit::ReconstructionState *best_state = &temp_cu_state_[rdo_depth];
  RdoSyntaxWriter best_writer(*writer);
  CodingUnit **temp_cu = &rdo_temp_cu_[cu_tree][rdo_depth];
  (*temp_cu)->CopyPositionAndSizeFrom(*cu);

  if (cu->GetBinaryDepth() == 0) {
    // First CU in quad split, clear up cache
    cu_cache_.Invalidate(cu->GetCuTree(), cu->GetDepth());
  }

  // First eval without CU split
  if (do_full) {
    Bits start_bits = writer->GetNumWrittenBits();
    best_cost.dist =
      CompressNoSplit(best_cu, rdo_depth, split_restiction, &best_writer);
    cu = *best_cu;
    Bits full_bits = best_writer.GetNumWrittenBits() - start_bits;
    best_cost.cost =
      best_cost.dist + static_cast<Cost>(full_bits * qp.GetLambda() + 0.5);
    cu->SaveStateTo(best_state, rec_pic_);
  }

  // Skip split eval speed-up
  if (encoder_settings_.fast_cu_split_based_on_full_cu &&
      do_full && CanSkipAnySplitForCu(*cu)) {
    *writer = best_writer;
    return best_cost.dist;
  }

  bool best_binary_depth_greater_than_one = false;
  Cost hor_cost = 0;
  // Horizontal split
  if (do_hor_split) {
    RdoSyntaxWriter splitcu_writer(*writer);
    RdoCost split_cost =
      CompressSplitCu(*temp_cu, rdo_depth, qp, SplitType::kHorizontal,
                      split_restiction, &splitcu_writer);
    hor_cost = split_cost.cost;
    for (auto &sub_cu : (*temp_cu)->GetSubCu()) {
      best_binary_depth_greater_than_one |=
        sub_cu && sub_cu->GetSplit() != SplitType::kNone;
    }
    if (split_cost.cost < best_cost.cost) {
      std::swap(*best_cu, *temp_cu);
      cu = *best_cu;
      if (!do_quad_split && !do_ver_split) {
        // No more split evaluations
        *writer = splitcu_writer;
        return split_cost.dist;
      }
      best_cost = split_cost;
      best_writer = splitcu_writer;
      cu->SaveStateTo(best_state, rec_pic_);
    } else {
      // Restore (previous) best state
      cu->LoadStateFrom(*best_state, &rec_pic_);
      pic_data_.MarkUsedInPic(cu);
    }
  }

  // Vertical split
  if (do_ver_split) {
    RdoSyntaxWriter splitcu_writer(*writer);
    RdoCost split_cost =
      CompressSplitCu(*temp_cu, rdo_depth, qp, SplitType::kVertical,
                      split_restiction, &splitcu_writer);
    if (split_cost.cost < hor_cost) {
      best_binary_depth_greater_than_one = false;
      for (auto &sub_cu : (*temp_cu)->GetSubCu()) {
        best_binary_depth_greater_than_one |=
          sub_cu && sub_cu->GetSplit() != SplitType::kNone;
      }
    }
    if (split_cost.cost < best_cost.cost) {
      std::swap(*best_cu, *temp_cu);
      cu = *best_cu;
      if (!do_quad_split) {
        // No more split evaluations
        *writer = splitcu_writer;
        return split_cost.dist;
      }
      best_cost = split_cost;
      best_writer = splitcu_writer;
      cu->SaveStateTo(best_state, rec_pic_);
    } else {
      // Restore (previous) best state
      cu->LoadStateFrom(*best_state, &rec_pic_);
      pic_data_.MarkUsedInPic(cu);
    }
  }

  // Quad split speed-up
  if (encoder_settings_.fast_quad_split_based_on_binary_split &&
      do_quad_split && do_hor_split && do_ver_split &&
      CanSkipQuadSplitForCu(*cu, best_binary_depth_greater_than_one)) {
    *writer = best_writer;
    return best_cost.dist;
  }

  // Quad split
  if (do_quad_split) {
    RdoSyntaxWriter splitcu_writer(*writer);
    RdoCost split_cost =
      CompressSplitCu(*temp_cu, rdo_depth, qp, SplitType::kQuad,
                      split_restiction, &splitcu_writer);
    if (split_cost.cost < best_cost.cost) {
      std::swap(*best_cu, *temp_cu);
      // No more split evaluations
      *writer = splitcu_writer;
      return split_cost.dist;
    } else {
      // Restore (previous) best state
      cu->LoadStateFrom(*best_state, &rec_pic_);
      pic_data_.MarkUsedInPic(cu);
    }
  }

  *writer = best_writer;
  return best_cost.dist;
}

CuEncoder::RdoCost
CuEncoder::CompressSplitCu(CodingUnit *cu, int rdo_depth, const Qp &qp,
                           SplitType split_type,
                           SplitRestriction split_restriction,
                           RdoSyntaxWriter *rdo_writer) {
  if (cu->GetSplit() != SplitType::kNone) {
    cu->UnSplit();
  }
  cu->Split(split_type);
  pic_data_.ClearMarkCuInPic(cu);
  Distortion dist = 0;
  Bits start_bits = rdo_writer->GetNumWrittenBits();
  SplitRestriction sub_split_restriction = SplitRestriction::kNone;
  if (EncoderSettings::kEncoderStrictRdoBitCounting) {
    cu_writer_.WriteSplit(*cu, split_restriction, rdo_writer);
  }
  for (auto &sub_cu : cu->GetSubCu()) {
    if (sub_cu) {
      dist +=
        CompressCu(&sub_cu, rdo_depth + 1, sub_split_restriction, rdo_writer,
                   qp);
      sub_split_restriction = sub_cu->DeriveSiblingSplitRestriction(split_type);
    }
  }
  if (!EncoderSettings::kEncoderStrictRdoBitCounting) {
    cu_writer_.WriteSplit(*cu, split_restriction, rdo_writer);
  }
  Bits bits = rdo_writer->GetNumWrittenBits() - start_bits;
  Cost cost = dist + static_cast<Cost>(bits * qp.GetLambda() + 0.5);
  return RdoCost(cost, dist);
}


int CuEncoder::CalcDeltaQpFromVariance(const CodingUnit *cu) {
  const double kStrength = 1.0 * encoder_settings_.aqp_strength / 10;
  const double kOffset = 15;
  const int kVarBlocksize = 16;
  const int kMeanDiv = 2;
  const int kMinQpOffset = -3;
  const int kMaxQpOffset = 7;
  const YuvComponent luma = YuvComponent::kY;
  const int x = cu->GetPosX(luma);
  const int y = cu->GetPosY(luma);

  auto calc_variance = [](const Sample* src, int block_size, ptrdiff_t stride) {
    uint64_t sum = 0;
    uint64_t squares = 0;
    uint64_t num = 0;
    for (int k = 0; k < block_size; k++) {
      for (int l = 0; l < block_size; l++) {
        sum += *src;
        squares += (*src)*(*src);
        num++;
        src++;
      }
      src += stride - block_size;
    }
    return (256 * (squares - (sum * sum) / num)) / num;
  };

  const int h = cu->GetHeight(luma) / kVarBlocksize;
  const int w = cu->GetHeight(luma) / kVarBlocksize;
  std::vector<uint64_t> v(h * w, std::numeric_limits<uint64_t>::max());
  int blocks = 0;
  for (int i = 0; i < h; i++) {
    if (y + i * kVarBlocksize >= pic_data_.GetPictureHeight(luma)) {
      continue;
    }
    const Sample *orig = orig_pic_.GetSamplePtr(luma, x, y) +
      i * kVarBlocksize * orig_pic_.GetStride(luma);
    for (int j = 0; j < w; j++) {
      if (x + j * kVarBlocksize >= pic_data_.GetPictureWidth(luma)) {
        continue;
      }
      uint64_t variance =
        calc_variance(orig, kVarBlocksize, orig_pic_.GetStride(luma));
      v[blocks++] = variance;
      orig += kVarBlocksize;
    }
  }
  std::sort(v.begin(), v.end());
  uint64_t variance;
  variance = 1 + v[blocks / kMeanDiv];

  int bd = orig_pic_.GetBitdepth();
  double dqp = kStrength * (1.5 * std::log(variance) - kOffset - 2 * (bd - 8));

  return util::Clip3(static_cast<int>(dqp), kMinQpOffset, kMaxQpOffset);
}


Distortion CuEncoder::CompressNoSplit(CodingUnit **best_cu, int rdo_depth,
                                      SplitRestriction split_restriction,
                                      RdoSyntaxWriter *writer) {
  CodingUnit *cu = *best_cu;
  const Qp &qp = cu->GetQp();
  if (cu->GetSplit() != SplitType::kNone) {
    cu->UnSplit();
  }
  cu->SetQp(qp);

  const int cu_tree = static_cast<int>(cu->GetCuTree());
  CuCache::Result cache_result = cu_cache_.Lookup(*cu);

  RdoCost best_cost(std::numeric_limits<Cost>::max());
  if (encoder_settings_.skip_mode_decision_for_identical_cu &&
      cache_result.cu && cu->IsFirstCuInQuad(cu->GetDepth() - 1)) {
    // Use cached CU
    cu->CopyPredictionDataFrom(*cache_result.cu);
    best_cost.cost = 0;
    best_cost.dist = CompressFast(cu, qp, *writer);
  } else if (pic_data_.IsIntraPic()) {
    // Intra pic
    best_cost = CompressIntra(cu, qp, *writer);
  } else {
    best_cost = CompressInterPic(best_cu, &rdo_temp_cu_[cu_tree][rdo_depth + 1],
                                 qp, rdo_depth, cache_result, *writer);
    cu = *best_cu;
  }
  pic_data_.MarkUsedInPic(cu);

  if (cache_result.cacheable) {
    // Save prediction data in cache
    cu_cache_.Store(*cu);
  }

  if (EncoderSettings::kEncoderStrictRdoBitCounting) {
    cu_writer_.WriteSplit(*cu, split_restriction, writer);
  }
  for (YuvComponent comp : pic_data_.GetComponents(cu->GetCuTree())) {
    cu_writer_.WriteComponent(*cu, comp, writer);
  }
  if (!EncoderSettings::kEncoderStrictRdoBitCounting) {
    cu_writer_.WriteSplit(*cu, split_restriction, writer);
  }
  return best_cost.dist;
}

Distortion CuEncoder::CompressFast(CodingUnit *cu, const Qp &qp,
                                   const SyntaxWriter &writer) {
  assert(cu->GetSplit() == SplitType::kNone);
  Distortion dist = 0;
  if (cu->IsIntra()) {
    for (YuvComponent comp : pic_data_.GetComponents(cu->GetCuTree())) {
      dist +=
        intra_search_.CompressIntraFast(cu, comp, qp, writer, this, &rec_pic_);
    }
  } else {
    for (YuvComponent comp : pic_data_.GetComponents(cu->GetCuTree())) {
      dist += inter_search_.CompressInterFast(cu, comp, qp, writer, this,
                                              &rec_pic_);
    }
  }
  return dist;
}

CuEncoder::RdoCost
CuEncoder::CompressInterPic(CodingUnit **best_cu_ref, CodingUnit **temp_cu_ref,
                            const Qp &qp, int rdo_depth,
                            const CuCache::Result &cache_result,
                            const SyntaxWriter &writer) {
  CodingUnit::ReconstructionState *best_state = &temp_cu_state_[rdo_depth + 1];
  CodingUnit *best_cu = *best_cu_ref;
  CodingUnit *cu = *temp_cu_ref;
  assert(best_cu->GetSplit() == SplitType::kNone);
  cu->CopyPositionAndSizeFrom(*best_cu);
  if (cu->GetSplit() != SplitType::kNone) {
    cu->UnSplit();
  }

  const bool fast_skip_inter =
    encoder_settings_.fast_mode_selection_for_cached_cu &&
    (cache_result.any_intra || cache_result.any_skip) &&
    !Restrictions::Get().disable_inter_merge_mode;
  const bool fast_skip_intra =
    encoder_settings_.fast_mode_selection_for_cached_cu &&
    cache_result.any_inter;
  RdoCost best_cost(std::numeric_limits<Cost>::max());
  const auto save_if_best_cost = [&](RdoCost cost) {
    if (cost < best_cost) {
      best_cost = cost;
      cu->SaveStateTo(best_state, rec_pic_);
      std::swap(best_cu, cu);
    }
  };

  if (cu->CanAffineMerge() &&
      !Restrictions::Get().disable_ext2_inter_affine_merge &&
      !Restrictions::Get().disable_inter_merge_mode &&
      !Restrictions::Get().disable_ext2_inter_affine) {
    RdoCost cost = CompressAffineMerge(cu, qp, writer, best_cost.cost);
    save_if_best_cost(cost);
  }

  if (!Restrictions::Get().disable_inter_merge_mode) {
    const bool fast_merge_skip =
      encoder_settings_.fast_merge_eval && cache_result.any_skip;
    RdoCost cost = CompressMerge(cu, qp, writer, best_cost.cost,
                                 fast_merge_skip);
    save_if_best_cost(cost);
  }

  if (!fast_skip_inter) {
    RdoCost cost = CompressInter(cu, qp, writer, RdMode::kInterMe,
                                 best_cost.cost);
    save_if_best_cost(cost);
  }

  if (!fast_skip_inter && pic_data_.GetUseLocalIlluminationCompensation() &&
      !Restrictions::Get().disable_ext2_inter_local_illumination_comp) {
    RdoCost cost = CompressInter(cu, qp, writer, RdMode::kInterLic,
                                 best_cost.cost);
    save_if_best_cost(cost);
  }

  if (!Restrictions::Get().disable_ext2_inter_adaptive_fullpel_mv) {
    RdoCost cost = CompressInter(cu, qp, writer, RdMode::kInterFullpel,
                                 best_cost.cost);
    save_if_best_cost(cost);
  }

  if (pic_data_.GetUseLocalIlluminationCompensation() &&
      !Restrictions::Get().disable_ext2_inter_local_illumination_comp &&
      !Restrictions::Get().disable_ext2_inter_adaptive_fullpel_mv) {
    RdoCost cost = CompressInter(cu, qp, writer, RdMode::kInterLicFullpel,
                                 best_cost.cost);
    save_if_best_cost(cost);
  }

  if ((!fast_skip_intra && best_cu->GetHasAnyCbf()) ||
      encoder_settings_.always_evaluate_intra_in_inter) {
    RdoCost cost = CompressIntra(cu, qp, writer);
    save_if_best_cost(cost);
  }

  assert(best_cost.cost < std::numeric_limits<Cost>::max());
  best_cu->LoadStateFrom(*best_state, &rec_pic_);
  *best_cu_ref = best_cu;
  *temp_cu_ref = cu;
  return best_cost;
}

CuEncoder::RdoCost
CuEncoder::CompressIntra(CodingUnit *cu, const Qp &qp,
                         const SyntaxWriter &bitstream_writer) {
  cu->ResetPredictionState();
  cu->SetPredMode(PredictionMode::kIntra);
  cu->SetSkipFlag(false);
  RdoSyntaxWriter rdo_writer(bitstream_writer, 0);
  Distortion dist = 0;
  if (YuvComponent::kY == pic_data_.GetComponents(cu->GetCuTree())[0]) {
    dist += intra_search_.CompressIntraLuma(cu, qp, bitstream_writer, this,
                                            &rec_pic_);
    cu_writer_.WriteComponent(*cu, YuvComponent::kY, &rdo_writer);
  }
  if (pic_data_.GetComponents(cu->GetCuTree()).size() > 1) {
    // TODO(PH) This should optimally use rdo_writer as starting state
    dist += intra_search_.CompressIntraChroma(cu, qp, bitstream_writer, this,
                                              &rec_pic_);
    cu_writer_.WriteComponent(*cu, YuvComponent::kU, &rdo_writer);
    cu_writer_.WriteComponent(*cu, YuvComponent::kV, &rdo_writer);
  }
  Bits bits = rdo_writer.GetNumWrittenBits();
  Cost cost = dist + static_cast<Cost>(bits * qp.GetLambda() + 0.5);
  return RdoCost(cost, dist);
}

CuEncoder::RdoCost
CuEncoder::CompressInter(CodingUnit *cu, const Qp &qp,
                         const SyntaxWriter &bitstream_writer,
                         CuEncoder::RdMode rd_mode, Cost best_cu_cost) {
  InterSearchFlags search_flags = InterSearchFlags::kDefault;
  if (cu->GetPicType() == PicturePredictionType::kUni) {
    search_flags |= InterSearchFlags::kUniPredOnly;
  }
  switch (rd_mode) {
    case CuEncoder::RdMode::kInterMe:
      if (cu->CanUseAffine() &&
          !Restrictions::Get().disable_ext2_inter_affine) {
        search_flags |= InterSearchFlags::kAffine;
      }
      break;
    case CuEncoder::RdMode::kInterFullpel:
      search_flags |= InterSearchFlags::kFullPelMv;
      break;
    case CuEncoder::RdMode::kInterLic:
      search_flags |= InterSearchFlags::kLic;
      break;
    case CuEncoder::RdMode::kInterLicFullpel:
      search_flags |= InterSearchFlags::kFullPelMv;
      search_flags |= InterSearchFlags::kLic;
      break;
    default:
      assert(0);
  }
  Distortion dist =
    inter_search_.CompressInter(cu, qp, bitstream_writer, search_flags,
                                best_cu_cost, this, &rec_pic_);
  if (dist == std::numeric_limits<Distortion>::max()) {
    return RdoCost(std::numeric_limits<Cost>::max(), dist);
  }
  return GetCuCostWithoutSplit(*cu, qp, bitstream_writer, dist);
}

CuEncoder::RdoCost
CuEncoder::CompressMerge(CodingUnit *cu, const Qp &qp,
                         const SyntaxWriter &bitstream_writer,
                         Cost best_cu_cost, bool fast_merge_skip) {
  std::array<bool,
    constants::kNumInterMergeCandidates> skip_evaluated = { false };
  int num_merge_cand = Restrictions::Get().disable_inter_merge_candidates ?
    1 : constants::kNumInterMergeCandidates;
  cu->ResetPredictionState();
  cu->SetPredMode(PredictionMode::kInter);
  cu->SetMergeFlag(true);

  InterMergeCandidateList merge_list = inter_search_.GetMergeCandidates(*cu);
  std::array<int, constants::kNumInterMergeCandidates> cand_lookup;
  if (encoder_settings_.fast_merge_eval &&
      !fast_merge_skip && num_merge_cand > 1) {
    // TODO(PH) Prediction samples for each candidate can be used in loop below
    num_merge_cand =
      inter_search_.SearchMergeCandidates(cu, qp, bitstream_writer, merge_list,
                                          this, &cand_lookup);
  } else {
    for (int merge_idx = 0; merge_idx < num_merge_cand; merge_idx++) {
      cand_lookup[merge_idx] = merge_idx;
    }
  }

  RdoCost best_cost(std::numeric_limits<Cost>::max());
  CodingUnit::ResidualState &best_transform_state = rd_transform_state_;
  int best_merge_idx = -1;
  const int skip_eval_init = fast_merge_skip ? 1 : 0;
  for (int skip_eval_idx = skip_eval_init; skip_eval_idx < 2; skip_eval_idx++) {
    bool force_skip = skip_eval_idx != 0;
    for (int i = 0; i < num_merge_cand; i++) {
      const int merge_idx = cand_lookup[i];
      if (skip_evaluated[merge_idx]) {
        continue;
      }
      Distortion dist =
        inter_search_.CompressMergeCand(cu, qp, bitstream_writer, merge_list,
                                        merge_idx, force_skip, best_cu_cost,
                                        this, &rec_pic_);
      RdoCost cost = GetCuCostWithoutSplit(*cu, qp, bitstream_writer, dist);
      if (!cu->GetHasAnyCbf()) {
        skip_evaluated[merge_idx] = true;
      }
      if (cost.cost < best_cost.cost) {
        best_cu_cost = std::min(cost.cost, best_cu_cost);
        best_cost = cost;
        best_merge_idx = merge_idx;
        cu->SaveStateTo(&best_transform_state, rec_pic_);
        if (!cu->GetHasAnyCbf() && !force_skip) {
          // Encoder optimization, assume skip is always best
          break;
        }
      }
    }
  }
  cu->SetMergeIdx(best_merge_idx);
  inter_search_.ApplyMergeCand(cu, merge_list[best_merge_idx]);
  cu->LoadStateFrom(best_transform_state, &rec_pic_);
  cu->SetSkipFlag(!cu->GetHasAnyCbf() &&
                  !Restrictions::Get().disable_inter_skip_mode);
  return best_cost;
}

CuEncoder::RdoCost
CuEncoder::CompressAffineMerge(CodingUnit *cu, const Qp &qp,
                               const SyntaxWriter &bitstream_writer,
                               Cost best_cu_cost) {
  cu->ResetPredictionState();
  cu->SetPredMode(PredictionMode::kInter);
  cu->SetMergeFlag(true);
  cu->SetUseAffine(true);
  cu->SetMergeIdx(0);

  CodingUnit::ResidualState &best_transform_state = rd_transform_state_;
  AffineMergeCandidate merge_cand = inter_search_.GetAffineMergeCand(*cu);
  Distortion dist =
    inter_search_.CompressAffineMerge(cu, qp, bitstream_writer, merge_cand,
                                      false, best_cu_cost, this, &rec_pic_);
  RdoCost best_cost = GetCuCostWithoutSplit(*cu, qp, bitstream_writer, dist);
  if (cu->GetHasAnyCbf()) {
    cu->SaveStateTo(&best_transform_state, rec_pic_);
    Distortion dist_skip =
      inter_search_.CompressAffineMerge(cu, qp, bitstream_writer, merge_cand,
                                        true, best_cu_cost, this, &rec_pic_);
    RdoCost cost = GetCuCostWithoutSplit(*cu, qp, bitstream_writer, dist_skip);
    if (cost < best_cost) {
      return cost;
    }
    cu->SetSkipFlag(false);
    cu->LoadStateFrom(best_transform_state, &rec_pic_);
  }
  return best_cost;
}

CuEncoder::RdoCost
CuEncoder::GetCuCostWithoutSplit(const CodingUnit &cu, const Qp &qp,
                                 const SyntaxWriter &bitstream_writer,
                                 Distortion ssd) {
  RdoSyntaxWriter rdo_writer(bitstream_writer, 0);
  for (YuvComponent comp : pic_data_.GetComponents(cu.GetCuTree())) {
    cu_writer_.WriteComponent(cu, comp, &rdo_writer);
  }
  Bits bits = rdo_writer.GetNumWrittenBits();
  Cost cost = ssd + static_cast<Cost>(bits * qp.GetLambda() + 0.5);
  return RdoCost(cost, ssd);
}

void CuEncoder::WriteCtu(int rsaddr, SyntaxWriter *writer) {
  if (EncoderSettings::kEncoderCountActualWrittenBits) {
    writer->ResetBitCounting();
  }
  CodingUnit *ctu = pic_data_.GetCtu(CuTree::Primary, rsaddr);
  bool write_delta_qp = cu_writer_.WriteCtu(ctu, &pic_data_, writer);
  if (pic_data_.HasSecondaryCuTree()) {
    CodingUnit *ctu2 = pic_data_.GetCtu(CuTree::Secondary, rsaddr);
    write_delta_qp |= cu_writer_.WriteCtu(ctu2, &pic_data_, writer);;
  }

  const int predicted_qp = ctu->GetPredictedQp();
  if (pic_data_.GetAdaptiveQp() > 0 && write_delta_qp) {
    writer->WriteQp(ctu->GetQp().GetQpRaw(YuvComponent::kY),
                    predicted_qp,
                    pic_data_.GetAdaptiveQp());
  } else {
    // Delta qp is not written if there was no cbf in the entire CTU.
    const int derived_qp = pic_data_.GetAdaptiveQp() == 2 ? predicted_qp :
      pic_data_.GetPicQp()->GetQpRaw(YuvComponent::kY);
    SetQpForAllCusInCtu(ctu, derived_qp);
    if (pic_data_.HasSecondaryCuTree()) {
      CodingUnit *ctu2 = pic_data_.GetCtu(CuTree::Secondary, rsaddr);
      SetQpForAllCusInCtu(ctu2, derived_qp);
    }
  }

  if (Restrictions::Get().disable_ext_implicit_last_ctu) {
    writer->WriteEndOfSlice(false);
  }
}

void CuEncoder::SetQpForAllCusInCtu(CodingUnit *ctu, int qp) {
  ctu->SetQp(qp);
  const int h = ctu->GetHeight(YuvComponent::kY);
  const int w = ctu->GetWidth(YuvComponent::kY);
  for (int i = 0; i < h; i += constants::kMinBlockSize) {
    for (int j = 0; j < w; j += constants::kMinBlockSize) {
      CodingUnit *tmp_cu =
        pic_data_.GetCuAtForModification(ctu->GetCuTree(),
                                         ctu->GetPosX(YuvComponent::kY) + j,
                                         ctu->GetPosY(YuvComponent::kY) + i);
      if (tmp_cu) {
        tmp_cu->SetQp(qp);
      }
    }
  }
}


bool CuEncoder::CanSkipAnySplitForCu(const CodingUnit &cu) const {
  const int binary_depth_threshold = pic_data_.IsHighestLayer() ? 2 : 3;
  return cu.GetSkipFlag() && cu.GetBinaryDepth() >= binary_depth_threshold;
}

bool
CuEncoder::CanSkipQuadSplitForCu(const CodingUnit &cu,
                                 bool binary_depth_greater_than_one) const {
  const YuvComponent comp = YuvComponent::kY;
  const CodingUnit *cu_top_left =
    pic_data_.GetCuAt(cu.GetCuTree(), cu.GetPosX(comp), cu.GetPosY(comp));
  const CodingUnit *cu_bottom_right =
    pic_data_.GetCuAt(cu.GetCuTree(), cu.GetPosX(comp) + cu.GetWidth(comp) - 1,
                      cu.GetPosY(comp) + cu.GetHeight(comp) - 1);
  if (encoder_settings_.fast_quad_split_based_on_binary_split == 1 &&
      binary_depth_greater_than_one) {
    return false;   // always evaluate quad split if binary spliting twice
  }
  const bool best_is_no_split = cu_top_left->GetBinaryDepth() == 0;
  const bool best_is_single_bt_split = cu_top_left->GetBinaryDepth() == 1 &&
    cu_bottom_right->GetBinaryDepth() == 1;
  switch (pic_data_.GetMaxBinarySplitDepth(cu.GetCuTree())) {
    case 1:
      return best_is_no_split && !pic_data_.IsIntraPic();

    case 2:
      return best_is_no_split && !pic_data_.IsIntraPic();

    case 3:
      return best_is_no_split ||
        (best_is_single_bt_split && !pic_data_.IsIntraPic());

    case 4:
      return best_is_no_split || best_is_single_bt_split;
  }
  return false;
}

}   // namespace xvc
