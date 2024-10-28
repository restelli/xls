// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/passes/query_engine.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/base/optimization.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/data_structures/leaf_type_tree.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/interval_ops.h"
#include "xls/ir/interval_set.h"
#include "xls/ir/node.h"
#include "xls/ir/ternary.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"
#include "xls/ir/value_utils.h"
#include "xls/passes/predicate_state.h"

namespace xls {
namespace {

// Converts the bits of the given node into a vector of BitLocations.
std::vector<TreeBitLocation> ToTreeBitLocations(Node* node) {
  CHECK(node->GetType()->IsBits());
  std::vector<TreeBitLocation> locations;
  locations.reserve(node->BitCountOrDie());
  for (int64_t i = 0; i < node->BitCountOrDie(); ++i) {
    locations.emplace_back(TreeBitLocation(node, i));
  }
  return locations;
}

// Converts the single-bit Nodes in preds into a vector of BitLocations. Each
// element in preds must be a single-bit bits-typed Node.
std::vector<TreeBitLocation> ToTreeBitLocations(absl::Span<Node* const> preds) {
  std::vector<TreeBitLocation> locations;
  for (Node* pred : preds) {
    CHECK(pred->GetType()->IsBits());
    CHECK_EQ(pred->BitCountOrDie(), 1);
    locations.emplace_back(TreeBitLocation(pred, 0));
  }
  return locations;
}

}  // namespace

LeafTypeTree<IntervalSet> QueryEngine::GetIntervals(Node* node) const {
  // How many non-trailing bits we want to consider when creating intervals from
  // a ternary. Each interval set will be made up of up to
  // `1 << kMaxTernaryIntervalBits` separate intervals.
  // "4" is arbitrary, but keeps the number of intervals from blowing up.
  constexpr int64_t kMaxTernaryIntervalBits = 4;
  std::optional<LeafTypeTree<TernaryVector>> tern = GetTernary(node);
  if (!tern.has_value()) {
    return *LeafTypeTree<IntervalSet>::CreateFromFunction(
        node->GetType(), [](Type* leaf_type) -> absl::StatusOr<IntervalSet> {
          return IntervalSet::Maximal(leaf_type->GetFlatBitCount());
        });
  }
  return leaf_type_tree::Map<IntervalSet, TernaryVector>(
      tern->AsView(), [](TernarySpan tv) -> IntervalSet {
        return interval_ops::FromTernary(
            tv, /*max_interval_bits=*/kMaxTernaryIntervalBits);
      });
}

std::optional<TreeBitLocation> QueryEngine::ExactlyOneBitUnknown(
    Node* node) const {
  std::optional<TreeBitLocation> unknown;
  for (const TreeBitLocation& bit : ToTreeBitLocations(node)) {
    if (!IsKnown(bit)) {
      if (unknown.has_value()) {
        return std::nullopt;
      }
      unknown = bit;
    }
  }
  return unknown;
}

bool QueryEngine::AtMostOneNodeTrue(absl::Span<Node* const> preds) const {
  return AtMostOneTrue(ToTreeBitLocations(preds));
}

bool QueryEngine::AtMostOneBitTrue(Node* node) const {
  return AtMostOneTrue(ToTreeBitLocations(node));
}

bool QueryEngine::AtLeastOneNodeTrue(absl::Span<Node* const> preds) const {
  return AtLeastOneTrue(ToTreeBitLocations(preds));
}

bool QueryEngine::AtLeastOneBitTrue(Node* node) const {
  return AtLeastOneTrue(ToTreeBitLocations(node));
}

bool QueryEngine::ExactlyOneBitTrue(Node* node) const {
  return AtLeastOneBitTrue(node) && AtMostOneBitTrue(node);
}

bool QueryEngine::IsKnown(const TreeBitLocation& bit) const {
  if (!IsTracked(bit.node())) {
    return false;
  }
  std::optional<LeafTypeTree<TernaryVector>> ternary = GetTernary(bit.node());
  if (!ternary.has_value()) {
    return false;
  }
  return ternary->Get(bit.tree_index())[bit.bit_index()] !=
         TernaryValue::kUnknown;
}

std::optional<bool> QueryEngine::KnownValue(const TreeBitLocation& bit) const {
  if (!IsTracked(bit.node())) {
    return std::nullopt;
  }

  std::optional<LeafTypeTree<TernaryVector>> ternary = GetTernary(bit.node());
  if (!ternary.has_value()) {
    return std::nullopt;
  }
  switch (ternary->Get(bit.tree_index())[bit.bit_index()]) {
    case TernaryValue::kUnknown:
      return std::nullopt;
    case TernaryValue::kKnownZero:
      return false;
    case TernaryValue::kKnownOne:
      return true;
  }

  ABSL_UNREACHABLE();
  return std::nullopt;
}

std::optional<Value> QueryEngine::KnownValue(Node* node) const {
  if (!IsTracked(node)) {
    return std::nullopt;
  }

  std::optional<LeafTypeTree<TernaryVector>> ternary = GetTernary(node);
  if (!ternary.has_value() ||
      !absl::c_all_of(ternary->elements(), [](const TernaryVector& v) {
        return ternary_ops::IsFullyKnown(v);
      })) {
    return std::nullopt;
  }

  absl::StatusOr<LeafTypeTree<Value>> value =
      leaf_type_tree::MapIndex<Value, TernaryVector>(
          ternary->AsView(),
          [](Type* leaf_type, const TernaryVector& v,
             absl::Span<const int64_t>) -> absl::StatusOr<Value> {
            if (leaf_type->IsToken()) {
              return Value::Token();
            }
            CHECK(leaf_type->IsBits());
            return Value(ternary_ops::ToKnownBitsValues(v));
          });
  CHECK_OK(value.status());
  absl::StatusOr<Value> result = LeafTypeTreeToValue(value->AsView());
  CHECK_OK(result.status());
  return *result;
}

std::optional<Bits> QueryEngine::KnownValueAsBits(Node* node) const {
  CHECK(node->GetType()->IsBits());
  if (!IsTracked(node)) {
    return std::nullopt;
  }

  std::optional<Value> value = KnownValue(node);
  if (!value.has_value()) {
    return std::nullopt;
  }
  return value->bits();
}

bool QueryEngine::IsMsbKnown(Node* node) const {
  CHECK(node->GetType()->IsBits());
  if (!IsTracked(node)) {
    return false;
  }
  if (node->BitCountOrDie() == 0) {
    // Zero-length is considered unknown.
    return false;
  }
  return IsKnown(TreeBitLocation(node, node->BitCountOrDie() - 1));
}

bool QueryEngine::IsOne(const TreeBitLocation& bit) const {
  std::optional<bool> known_value = KnownValue(bit);
  if (!known_value.has_value()) {
    return false;
  }
  return *known_value;
}

bool QueryEngine::IsZero(const TreeBitLocation& bit) const {
  std::optional<bool> known_value = KnownValue(bit);
  if (!known_value.has_value()) {
    return false;
  }
  return !*known_value;
}

bool QueryEngine::GetKnownMsb(Node* node) const {
  CHECK(node->GetType()->IsBits());
  CHECK(IsMsbKnown(node));
  return KnownValue(TreeBitLocation(node, node->BitCountOrDie() - 1)).value();
}

bool QueryEngine::IsAllZeros(Node* node) const {
  if (!IsTracked(node) || TypeHasToken(node->GetType())) {
    return false;
  }
  std::optional<LeafTypeTree<TernaryVector>> ternary_value = GetTernary(node);
  return ternary_value.has_value() &&
         absl::c_all_of(ternary_value->elements(), [](const TernaryVector& v) {
           return ternary_ops::IsKnownZero(v);
         });
}

bool QueryEngine::IsAllOnes(Node* node) const {
  if (!IsTracked(node) || TypeHasToken(node->GetType())) {
    return false;
  }
  std::optional<LeafTypeTree<TernaryVector>> ternary_value = GetTernary(node);
  return ternary_value.has_value() &&
         absl::c_all_of(ternary_value->elements(), [](const TernaryVector& v) {
           return ternary_ops::IsKnownOne(v);
         });
}

bool QueryEngine::IsFullyKnown(Node* node) const {
  if (!IsTracked(node) || TypeHasToken(node->GetType())) {
    return false;
  }

  std::optional<LeafTypeTree<TernaryVector>> ternary = GetTernary(node);
  return ternary.has_value() &&
         absl::c_all_of(ternary->elements(), [](const TernaryVector& v) {
           return ternary_ops::IsFullyKnown(v);
         });
}

Bits QueryEngine::MaxUnsignedValue(Node* node) const {
  CHECK(node->GetType()->IsBits());
  absl::InlinedVector<bool, 1> bits(node->BitCountOrDie());
  for (int64_t i = 0; i < node->BitCountOrDie(); ++i) {
    bits[i] = IsZero(TreeBitLocation(node, i)) ? false : true;
  }
  return Bits(bits);
}

Bits QueryEngine::MinUnsignedValue(Node* node) const {
  CHECK(node->GetType()->IsBits());
  absl::InlinedVector<bool, 16> bits(node->BitCountOrDie());
  for (int64_t i = 0; i < node->BitCountOrDie(); ++i) {
    bits[i] = IsOne(TreeBitLocation(node, i));
  }
  return Bits(bits);
}

bool QueryEngine::NodesKnownUnsignedNotEquals(Node* a, Node* b) const {
  CHECK(a->GetType()->IsBits());
  CHECK(b->GetType()->IsBits());
  int64_t max_width = std::max(a->BitCountOrDie(), b->BitCountOrDie());
  auto get_known_bit = [this](Node* n, int64_t index) {
    if (index >= n->BitCountOrDie()) {
      return TernaryValue::kKnownZero;
    }
    TreeBitLocation location(n, index);
    if (IsZero(location)) {
      return TernaryValue::kKnownZero;
    }
    if (IsOne(location)) {
      return TernaryValue::kKnownOne;
    }
    return TernaryValue::kUnknown;
  };

  for (int64_t i = 0; i < max_width; ++i) {
    TernaryValue a_bit = get_known_bit(a, i);
    TernaryValue b_bit = get_known_bit(b, i);
    if (a_bit != b_bit && a_bit != TernaryValue::kUnknown &&
        b_bit != TernaryValue::kUnknown) {
      return true;
    }
  }
  return false;
}

bool QueryEngine::NodesKnownUnsignedEquals(Node* a, Node* b) const {
  CHECK(a->GetType()->IsBits());
  CHECK(b->GetType()->IsBits());
  if (a == b) {
    return true;
  }
  std::optional<Bits> a_value = KnownValueAsBits(a);
  if (!a_value.has_value()) {
    return false;
  }
  std::optional<Bits> b_value = KnownValueAsBits(b);
  if (!b_value.has_value()) {
    return false;
  }
  return bits_ops::UEqual(*a_value, *b_value);
}

std::string QueryEngine::ToString(Node* node) const {
  CHECK(IsTracked(node));
  std::optional<LeafTypeTree<TernaryVector>> ternary = GetTernary(node);
  if (!ternary.has_value()) {
    ternary = *LeafTypeTree<TernaryVector>::CreateFromFunction(
        node->GetType(), [](Type* leaf_type) -> absl::StatusOr<TernaryVector> {
          return TernaryVector(leaf_type->GetFlatBitCount(),
                               TernaryValue::kUnknown);
        });
  }
  if (node->GetType()->IsBits()) {
    return xls::ToString(ternary->Get({}));
  }
  return ternary->ToString(
      [](const TernaryVector& v) -> std::string { return xls::ToString(v); });
}

// A forwarder for query engine.
class ForwardingQueryEngine final : public QueryEngine {
 public:
  explicit ForwardingQueryEngine(const QueryEngine& real) : real_(real) {}
  absl::StatusOr<ReachedFixpoint> Populate(FunctionBase* f) override {
    return absl::UnimplementedError("Cannot populate forwarding engine!");
  }

  bool IsTracked(Node* node) const override { return real_.IsTracked(node); }

  std::optional<LeafTypeTree<TernaryVector>> GetTernary(
      Node* node) const override {
    return real_.GetTernary(node);
  };

  std::unique_ptr<QueryEngine> SpecializeGivenPredicate(
      const absl::flat_hash_set<PredicateState>& state) const override {
    return real_.SpecializeGivenPredicate(state);
  }

  LeafTypeTree<IntervalSet> GetIntervals(Node* node) const override {
    return real_.GetIntervals(node);
  }

  bool AtMostOneTrue(absl::Span<TreeBitLocation const> bits) const override {
    return real_.AtMostOneTrue(bits);
  }

  bool AtLeastOneTrue(absl::Span<TreeBitLocation const> bits) const override {
    return real_.AtLeastOneTrue(bits);
  }

  bool Implies(const TreeBitLocation& a,
               const TreeBitLocation& b) const override {
    return real_.Implies(a, b);
  }

  std::optional<Bits> ImpliedNodeValue(
      absl::Span<const std::pair<TreeBitLocation, bool>> predicate_bit_values,
      Node* node) const override {
    return real_.ImpliedNodeValue(predicate_bit_values, node);
  }

  std::optional<TernaryVector> ImpliedNodeTernary(
      absl::Span<const std::pair<TreeBitLocation, bool>> predicate_bit_values,
      Node* node) const override {
    return real_.ImpliedNodeTernary(predicate_bit_values, node);
  }

  bool KnownEquals(const TreeBitLocation& a,
                   const TreeBitLocation& b) const override {
    return real_.KnownEquals(a, b);
  }

  bool KnownNotEquals(const TreeBitLocation& a,
                      const TreeBitLocation& b) const override {
    return real_.KnownNotEquals(a, b);
  }

  bool AtMostOneBitTrue(Node* node) const override {
    return real_.AtMostOneBitTrue(node);
  }
  bool AtLeastOneBitTrue(Node* node) const override {
    return real_.AtLeastOneBitTrue(node);
  }
  bool ExactlyOneBitTrue(Node* node) const override {
    return real_.ExactlyOneBitTrue(node);
  }
  bool IsKnown(const TreeBitLocation& bit) const override {
    return real_.IsKnown(bit);
  }
  std::optional<bool> KnownValue(const TreeBitLocation& bit) const override {
    return real_.KnownValue(bit);
  }
  std::optional<Value> KnownValue(Node* node) const override {
    return real_.KnownValue(node);
  }
  bool IsAllZeros(Node* n) const override { return real_.IsAllZeros(n); }
  bool IsAllOnes(Node* n) const override { return real_.IsAllOnes(n); }
  bool IsFullyKnown(Node* n) const override { return real_.IsFullyKnown(n); }
  Bits MaxUnsignedValue(Node* node) const override {
    return real_.MaxUnsignedValue(node);
  }
  Bits MinUnsignedValue(Node* node) const override {
    return real_.MinUnsignedValue(node);
  }

 private:
  const QueryEngine& real_;
};

std::unique_ptr<QueryEngine> QueryEngine::SpecializeGivenPredicate(
    const absl::flat_hash_set<PredicateState>& state) const {
  return std::make_unique<ForwardingQueryEngine>(*this);
}

}  // namespace xls
