// Copyright 2019 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "yasl/crypto/blake3_hash.h"

#include <vector>

#include "yasl/base/exception.h"

namespace yasl::crypto {

Blake3Hash::Blake3Hash()
    : hash_algo_(HashAlgorithm::BLAKE3), digest_size_(BLAKE3_OUT_LEN) {
  Init();
}

Blake3Hash::Blake3Hash(size_t output_len)
    : hash_algo_(HashAlgorithm::BLAKE3), digest_size_(output_len) {
  YASL_ENFORCE((output_len > 0) && (output_len <= BLAKE3_OUT_LEN),
               "blake3 hash out length shoud be in (0, {}]", BLAKE3_OUT_LEN);
  Init();
}

void Blake3Hash::Init() { blake3_hasher_init(&hasher_ctx_); }

Blake3Hash::~Blake3Hash() { Init(); }

HashAlgorithm Blake3Hash::GetHashAlgorithm() const { return hash_algo_; }

size_t Blake3Hash::DigestSize() const { return digest_size_; }

Blake3Hash& Blake3Hash::Reset() {
  Init();

  return *this;
}

Blake3Hash& Blake3Hash::Update(ByteContainerView data) {
  blake3_hasher_update(&hasher_ctx_, data.data(), data.size());
  return *this;
}

std::vector<uint8_t> Blake3Hash::CumulativeHash() const {
  // Do not finalize the internally stored hash context. Instead, finalize a
  // copy of the current context so that the current context can be updated in
  // future calls to Update.
  blake3_hasher blake3_ctx_snapshot = hasher_ctx_;

  std::vector<uint8_t> digest(digest_size_);
  blake3_hasher_finalize(&blake3_ctx_snapshot, digest.data(), digest_size_);

  return digest;
}

}  // namespace yasl::crypto
