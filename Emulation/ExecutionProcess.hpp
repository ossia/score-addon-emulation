// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <ossia/dataflow/node_process.hpp>

namespace Emulation
{
// node_process's pause and seek hooks are empty. Instrument voices must be
// stopped even when the paused graph does not deliver another token.
class ExecutionProcess final : public ossia::node_process
{
public:
  using ossia::node_process::node_process;
  void pause() override { node->all_notes_off(); }
  void offset_impl(ossia::time_value) override { node->all_notes_off(); }
  void transport_impl(ossia::time_value) override { node->all_notes_off(); }

  void state(const ossia::token_request& token) override
  {
    const auto request = [this](ossia::token_request part) {
      part.start_discontinuous |= m_loopStart;
      m_loopStart = false;
      node->request(part);
    };
    if(!m_loops)
      request(token.add_offset(m_start_offset));
    else
      token.loop(m_start_offset, m_loop_duration, request, [this](ossia::time_value) {
        // Loop tokens are queued before any of them render. An immediate panic
        // here would stop the preceding slice too early and miss its new notes.
        if(!node->requested_tokens.empty())
          node->requested_tokens.back().end_discontinuous = true;
        m_loopStart = true;
      });
  }
private:
  bool m_loopStart{};
};
}
