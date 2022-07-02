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

#pragma once

#include "yasl/base/buffer.h"
#include "yasl/link/context.h"

namespace yasl::link {

Buffer Scatter(const std::shared_ptr<Context>& ctx,
               const std::vector<ByteContainerView>& inputs, size_t root,
               std::string_view tag);

}  // namespace yasl::link