/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Sorts functions by their static use count. This helps reduce the size of wasm
// binaries because fewer bytes are needed to encode references to frequently
// used functions.
//
// Secondarily, sorts by similarity, to keep similar functions close together,
// which can help with gzip size.
//


#include <memory>

#include <wasm.h>
#include <pass.h>
#include <wasm-binary.h>
#include <support/hash.h>

namespace wasm {

typedef std::unordered_map<Name, std::atomic<Index>> NameCountMap;

struct CallCountScanner : public WalkerPass<PostWalker<CallCountScanner>> {
  bool isFunctionParallel() override { return true; }

  CallCountScanner(NameCountMap* counts) : counts(counts) {}

  CallCountScanner* create() override {
    return new CallCountScanner(counts);
  }

  void visitCall(Call* curr) {
    assert(counts->count(curr->target) > 0); // can't add a new element in parallel
    (*counts)[curr->target]++;
  }

private:
  NameCountMap* counts;
};

struct ReorderFunctions : public Pass {
  void run(PassRunner* runner, Module* module) override {
    // note original indexes, to break ties
    std::unordered_map<Name, Index> originalIndexes;
    auto& functions = module->functions;
    auto numFunctions = functions.size();
    for (Index i = 0; i < numFunctions; i++) {
      originalIndexes[functions[i]->name] = i;
    }
    NameCountMap counts;
    // fill in info, as we operate on it in parallel (each function to its own entry)
    for (auto& func : functions) {
      counts[func->name];
    }
    // find counts on function calls
    {
      PassRunner runner(module);
      runner.setIsNested(true);
      runner.add<CallCountScanner>(&counts);
      runner.run();
    }
    // find counts on global usages
    if (module->start.is()) {
      counts[module->start]++;
    }
    for (auto& curr : module->exports) {
      counts[curr->value]++;
    }
    for (auto& segment : module->table.segments) {
      for (auto& curr : segment.data) {
        counts[curr]++;
      }
    }
    // sort by uses, break ties with original order
    std::sort(functions.begin(), functions.end(), [&counts, &originalIndexes](
      const std::unique_ptr<Function>& a,
      const std::unique_ptr<Function>& b) -> bool {
      if (counts[a->name] == counts[b->name]) {
        return originalIndexes[a->name] < originalIndexes[b->name];
      }
      return counts[a->name] > counts[b->name];
    });
    // secondarily, sort by similarity, but without changing LEB sizes
    // write out the binary so we can see function contents
    BufferWithRandomAccess buffer;
    WasmBinaryWriter writer(module, buffer);
    writer.write();
    // get a profile of each function, which we can then use to compare
    std::unordered_map<Name, Index> profiles;
    for (Index i = 0; i < numFunctions; i++) {
      auto& info = writer.tableOfContents.functionBodies[i];
      profiles[functions[i]->name] = Profile(&buffer[info.offset], info.size);
    }
    // sort in chunks: LEB uses 7 bits for data per 8, so functions with
    // index 0-127 take one bytes, and so forth. sort within each such chunk
    size_t start = 0;
    size_t end = 128; // not inclusive
    while (start < numFunctions) {
      end = std::min(end, numFunctions);
      // TODO this is N^2!
      for (Index i = start; i < end - 1; i++) {
        // find the most similar to the previous
        if (i == 0) continue; // the very first can stay right there
        auto& previousProfile = profiles[functions[i - 1]->name];
        Index best = -1;
        size_t bestDistance = -1;
        for (Index j = i; j < end; j++) {
          auto distance = previousProfile.distance(profiles[functions[j]->name]);
          if (distance < bestDistance) {
            best = j;
            bestDistance = distance;
          }
        }
        assert(best != -1);
        std::swap(functions[i], functions[best]);
      }
      // move on to next chunk
      start = end;
      end *= 128;
    }
  }

  // represents a profile of binary data, suitable for making fuzzy comparisons
  // of similarity
  struct Profile {
    enum {
      // we allow more then 256 hashes so that we look not just at individual bytes, but also larger windows
      MAX_HASHES = 1024
    };
    // the profile maps hashes of seen values or combinations with the amount of appearances of them
    std::unordered_map<uint32_t, size_t> hashCounts;
    Profile(char* data, size_t size) {
      // very simple algorithm, just use sliding windows of sizes 1, 2, and 4
      uint32_t curr = 0;
      for (size_t i = 0; i < size; i++) {
        curr = (curr << 24) | *data;
        data++;
        hashCounts[hash(curr & 0xff)]++;
        if (i > 0) hashCounts[hash(curr & 0xffff)]++;
        if (i > 2) hashCounts[hash(curr)]++;
      }
      // trim: ignore the long tail, leave just the popular ones
      if (hashCounts.size() > MAX_HASHES) {
        std::vector<size_t> keys;
        for (auto& pair : hashCounts) {
          keys.push_back(pair.first);
        }
        std::sort(keys.begin(), keys.end(), [&hashCounts](const size_t a, const size_t b) -> bool {
          auto diff = hashCounts[a->name] == hashCounts[b->name];
          if (diff == 0) {
            return a < b;
          }
          return diff > 0;
        });
        auto trimmed;
        for (size_t i = 0; i < MAX_HASHES; i++) {
          auto key = keys[i];
          trimmed[key] = hashCounts[key];
        }
        trimmed.swap(hashCounts);
      }
    }

    size_t hash(uint32_t x) {
      return rehash(x, 0);
    }

    size_t distance(const Profile& other) {
      size_t ret = 0;
      for (auto& pair : hashCounts) {
        auto value = pair.first;
        auto iter = other.hashCounts.find(value);
        if (iter != other.hashCounts.end()) {
          ret += std::abs(pair.second - iter->second);
        } else {
          ret += pair.second;
        }
      }
      for (auto& pair : other.hashCounts) {
        auto value = pair.first;
        auto iter = hashCounts.find(value);
        if (iter == hashCounts.end()) {
          ret += pair.second;
        }
      }
      return ret;
    }
  };
};

Pass *createReorderFunctionsPass() {
  return new ReorderFunctions();
}

} // namespace wasm
