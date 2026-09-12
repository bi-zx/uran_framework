#pragma once
#include <vector>
#include "uran_plugin_api/types.hpp"

namespace uran_plugin_api
{
struct StoredState
{
  StateFieldSpec specification;
  StateValue value;
};

/** Host-provided durable backend. Save replaces a complete persistent snapshot
 * atomically; it must leave the previous snapshot intact on failure. */
class StateStorage
{
public:
  virtual ~StateStorage() = default;
  virtual std::vector<StoredState> load() = 0;
  virtual Status save(const std::vector<StoredState> &) = 0;
};
}
