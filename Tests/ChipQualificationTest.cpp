// SPDX-License-Identifier: GPL-3.0-or-later
#include <Emulation/ExecutionProcess.hpp>
#include <Emulation/Opl/Executor/Node.hpp>
#include <Emulation/Psg/Executor/Node.hpp>
#include <Emulation/Sid/Executor/Node.hpp>
#include <Emulation/La/Executor/Node.hpp>
#include <Emulation/Wurli/Executor/Node.hpp>
#include <Process/ExecutionTransaction.hpp>
#include <Execution/DocumentPlugin.hpp>
#include <ossia/dataflow/data_copy.hpp>
#include <ossia/dataflow/execution_state.hpp>
#include <libremidi/ump_events.hpp>
#include <catch2/catch_test_macros.hpp>
#include <score_test/App.hpp>
#include <score_test/Project.hpp>
#include <algorithm>
#include <cmath>
#include <array>
#include <vector>
#include <type_traits>

// Stable profiler boundary: only graph-buffer recycling and Node::run belong
// here. Preparation, MIDI/control storage, Catch assertions and GC are outside.
// This harness does not infer an allocation count from retained capacities.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void chipQualificationCallback(ossia::graph_node& node, ossia::audio_port& audio,
    const ossia::token_request& token, ossia::execution_state& state, bool recycle = false)
{
  if(recycle) ossia::clear_data{}(audio);
  node.run(token, ossia::exec_state_facade{&state});
}

namespace
{
void drain(Execution::DocumentPlugin& plug)
{
  const auto& context = plug.context();
  Execution::ExecutionCommand command;
  ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
  while(context.executionQueue.try_dequeue(command))
  { command(); context.gcQueue.enqueue(Execution::gc(std::move(command))); }
  ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
  Execution::GCCommand retired;
  while(context.gcQueue.try_dequeue(retired)) retired();
}
ossia::token_request window(const ossia::execution_state& state, int begin, int length)
{
  ossia::token_request token;
  token.prev_date = ossia::time_value{int64_t(begin * ossia::flicks_per_second<double> / state.sampleRate)};
  token.date = ossia::time_value{int64_t((begin + length) * ossia::flicks_per_second<double> / state.sampleRate)};
  token.start_sample = begin; token.length_sample = length;
  return token;
}
template<typename Node>
void run(Node& node, ossia::execution_state& state, const ossia::token_request& token, bool recycle = false)
{
  ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
  chipQualificationCallback(node, node.audio->data, token, state, recycle);
  ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
}
template<typename Node>
void message(Node& node, libremidi::ump value, int64_t timestamp = 0)
{
  value.timestamp = timestamp;
  node.midi->data.messages.push_back(value);
}
template<typename Node>
void clearInputs(Node& node)
{
  node.midi->data.messages.clear();
  for(auto* control : node.controls) control->data.get_data().clear();
}
template<typename Node>
double peak(const Node& node)
{
  double value{};
  for(double sample : node.audio->data.channel(0))
  { CHECK(std::isfinite(sample)); value = std::max(value, std::abs(sample)); }
  return value;
}
template<typename Function>
void eachChip(ossia::execution_state& state, Function function)
{
  for(auto model : {Emulation::Psg::Model::Ay8910, Emulation::Psg::Model::Ym2149, Emulation::Psg::Model::SegaYm7101})
  {
    INFO("PSG model " << int(model));
    Emulation::Psg::Settings settings; settings.model = model;
    if(model == Emulation::Psg::Model::SegaYm7101) settings.clock = 3579545;
    function([&] {
      auto node = std::make_shared<Emulation::Psg::Executor::Node>(state, settings);
      auto engine = std::make_shared<Emulation::Psg::Engine>(state.sampleRate, settings);
      node->install(engine, settings.model, settings.clock);
      return node;
    }, Emulation::Psg::Volume);
  }
  for(auto model : {Emulation::Sid::Model::Mos6581, Emulation::Sid::Model::Csg8580})
  {
    INFO("SID model " << int(model));
    auto patch = Emulation::Sid::defaultPatch(); patch.model = model;
    function([&] {
      auto node = std::make_shared<Emulation::Sid::Executor::Node>(state);
      node->edit(patch);
      auto engine = std::make_shared<Emulation::Sid::Engine>(state.sampleRate, patch);
      node->install(engine);
      return node;
    }, Emulation::Sid::Volume);
  }
  for(auto model : {Emulation::Opl::Model::Opl2, Emulation::Opl::Model::Opl3})
  {
    INFO("OPL model " << int(model));
    Emulation::Opl::Settings settings; settings.model = model;
    auto bank = std::make_shared<const Emulation::Opl::Bank>(Emulation::Opl::factoryBank());
    function([&] {
      auto node = std::make_shared<Emulation::Opl::Executor::Node>(state);
      auto engine = std::make_shared<Emulation::Opl::Engine>(state.sampleRate, settings, *bank);
      auto published = bank;
      node->install(engine, published, settings, {false, 0, 0, 6});
      return node;
    }, Emulation::Opl::Volume);
  }
}
template<typename Function>
void withState(Function function)
{
  score::test::run_in_app([&](const score::GUIApplicationContext& context) {
    auto* document = score::test::new_document(context); REQUIRE(document);
    auto& plug = document->context().plugin<Execution::DocumentPlugin>();
    plug.reload(true, score::test::base_interval(*document)); drain(plug);
    auto& state = *plug.context().execState;
    REQUIRE(state.bufferSize >= 64);
    function(state);
  });
}
}

TEST_CASE("Chip native pause seek offset and stop clean held sustain without a paused token", "[emulation][qualification][transport]")
{
  withState([](ossia::execution_state& state) {
    eachChip(state, [&](auto makeNode, int) {
      auto node = makeNode(); REQUIRE(node->ready());
      Emulation::ExecutionProcess process{node};
      const auto token = window(state, 0, state.bufferSize);
      for(int action = 0; action < 4; ++action)
      {
        CAPTURE(action);
        clearInputs(*node);
        message(*node, libremidi::from_midi1::control_change(0, 64, 127));
        message(*node, libremidi::from_midi1::note_on(0, 60, 110));
        message(*node, libremidi::from_midi1::note_off(0, 60, 0), 1);
        double before{};
        for(int tick = 0; tick < 16; ++tick)
        {
          state.samples_since_start += state.bufferSize;
          run(*node, state, token); clearInputs(*node);
          before = std::max(before, peak(*node));
        }
        REQUIRE(before > .0001);
        if(action % 2 == 0)
        {
          // Exercise tails already in release as well as pedal-held gates.
          message(*node, libremidi::from_midi1::control_change(0, 64, 0));
          state.samples_since_start += state.bufferSize;
          run(*node, state, token); clearInputs(*node);
        }
        ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
        if(action == 0) process.pause();
        else if(action == 1) process.transport_impl(ossia::time_value{0});
        else if(action == 2) process.offset_impl(ossia::time_value{0});
        else process.stop();
        ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
        for(int tick = 0; tick < 32; ++tick)
        {
          state.samples_since_start += state.bufferSize;
          run(*node, state, token);
        }
        CHECK(peak(*node) < .000001);
        ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
        process.resume(); process.start();
        // A restart may reuse the current callback's window after stop.
        process.stop();
        ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
        message(*node, libremidi::from_midi1::note_on(0, 67, 110));
        run(*node, state, token); clearInputs(*node);
        double restarted = peak(*node);
        for(int tick = 0; tick < 16; ++tick)
        {
          state.samples_since_start += state.bufferSize;
          run(*node, state, token); restarted = std::max(restarted, peak(*node));
        }
        CHECK(restarted > .0001);
        ossia::set_thread_pinned(ossia::thread_type::Audio, 0);
        process.stop();
        ossia::set_thread_pinned(ossia::thread_type::Ui, 0);
      }
    });
  });
}

TEST_CASE("Chip loop transport queues cleanup at rendered slice boundaries", "[emulation][qualification][transport]")
{
  withState([](ossia::execution_state& state) {
    eachChip(state, [&](auto makeNode, int) {
      auto looped = makeNode(), reference = makeNode();
      Emulation::ExecutionProcess process{looped};
      process.set_loops(true);
      process.set_loop_duration(ossia::time_value{1000000});
      auto token = window(state, 0, state.bufferSize);
      token.prev_date = ossia::time_value{0}; token.date = ossia::time_value{2000000};
      process.state(token);
      REQUIRE(looped->requested_tokens.size() == 2);
      const auto first = looped->requested_tokens[0], second = looped->requested_tokens[1];
      CHECK(first.end_discontinuous); CHECK(second.start_discontinuous); CHECK(second.end_discontinuous);
      REQUIRE(first.start_sample + first.length_sample == second.start_sample);
      for(auto* node : {looped.get(), reference.get()})
      {
        message(*node, libremidi::from_midi1::control_change(0, 64, 127));
        message(*node, libremidi::from_midi1::note_on(0, 60, 110));
        message(*node, libremidi::from_midi1::note_off(0, 60, 0), 1);
      }
      state.samples_since_start += state.bufferSize;
      auto uninterrupted = first; uninterrupted.end_discontinuous = false;
      run(*reference, state, uninterrupted);
      run(*looped, state, first);
      const auto& expected = reference->audio->data.channel(0);
      CHECK(std::equal(expected.begin(), expected.begin() + first.length_sample, looped->audio->data.channel(0).begin()));
      reference->all_notes_off();
      clearInputs(*reference);
      run(*reference, state, second); run(*looped, state, second);
      const auto& actual = looped->audio->data.channel(0);
      CHECK(std::equal(expected.begin() + second.start_sample, expected.begin() + state.bufferSize, actual.begin() + second.start_sample));
    });
  });
}

TEST_CASE("Chip out-of-order duplicate windows and paused boundary events never replay", "[emulation][qualification][execution]")
{
  withState([](ossia::execution_state& state) {
    eachChip(state, [&](auto makeNode, int volume) {
      auto node = makeNode();
      const int half = state.bufferSize / 2;
      const auto first = window(state, 0, half), second = window(state, half, state.bufferSize - half);
      message(*node, libremidi::from_midi1::note_on(0, 60, 110), half);
      state.samples_since_start += state.bufferSize;
      run(*node, state, second);
      const auto& output = node->audio->data.channel(0);
      const std::vector<double> saved(output.begin(), output.end());
      run(*node, state, first); run(*node, state, second);
      CHECK(std::equal(saved.begin(), saved.end(), output.begin()));
      node->all_notes_off(); clearInputs(*node);
      node->controls[volume]->data.write_value(0.f, 0);
      node->controls[volume]->data.write_value(.5f, half);
      message(*node, libremidi::from_midi1::note_on(0, 67, 110), 0);
      state.samples_since_start += state.bufferSize;
      auto paused = window(state, 0, 0);
      run(*node, state, paused); run(*node, state, paused);
      // If the ignored note-on is replayed here it remains audible after unmute.
      run(*node, state, window(state, 0, state.bufferSize));
      clearInputs(*node);
      for(int tick = 0; tick < 32; ++tick)
      {
        state.samples_since_start += state.bufferSize;
        run(*node, state, window(state, 0, state.bufferSize));
      }
      CHECK(peak(*node) < .000001);
    });
  });
}

TEST_CASE("Chip warmed callback profiling exercises MIDI gain and pooled output", "[emulation][qualification][allocation-profile]")
{
  withState([](ossia::execution_state& state) {
    eachChip(state, [&](auto makeNode, int volume) {
      auto node = makeNode();
      message(*node, libremidi::from_midi1::note_on(0, 60, 110));
      state.samples_since_start += state.bufferSize;
      run(*node, state, window(state, 0, state.bufferSize), true);
      clearInputs(*node);
      double audible{};
      // Attach allocation instrumentation to chipQualificationCallback, not to
      // this loop: inlet storage and the test framework are deliberately outside.
      for(int tick = 0; tick < 512; ++tick)
      {
        const int frames = tick % 3 == 0 ? 1 : tick % 3 == 1 ? 37 : state.bufferSize;
        clearInputs(*node);
        if(tick % 32 == 0) message(*node, libremidi::from_midi1::note_on(0, 60, 110));
        if(tick % 32 == 16) message(*node, libremidi::from_midi1::note_off(0, 60, 0));
        node->controls[volume]->data.write_value(tick % 8 == 0 ? 0.f : .5f, 0);
        if(tick % 32 == 0)
        {
          using Node = std::remove_reference_t<decltype(*node)>;
          if constexpr(std::is_same_v<Node, Emulation::Opl::Executor::Node>)
          {
            node->controls[Emulation::Opl::InstrumentIndex]->data.write_value(tick % 64 == 0 ? 6 : 0, 0);
            node->controls[Emulation::Opl::operatorControl(1, Emulation::Opl::Level)]->data.write_value(18, frames / 2);
          }
          else if constexpr(std::is_same_v<Node, Emulation::Psg::Executor::Node>)
            node->controls[Emulation::Psg::channelControl(0, Emulation::Psg::Level)]->data.write_value(tick % 64 == 0 ? 15 : 10, 0);
          else
            node->controls[Emulation::Sid::Cutoff]->data.write_value(tick % 64 == 0 ? 800 : 1600, 0);
        }
        state.samples_since_start += state.bufferSize;
        run(*node, state, window(state, 0, frames), tick % 2 == 0);
        REQUIRE(node->audio->data.channels() == 2);
        REQUIRE(node->audio->data.channel(0).size() >= std::size_t(frames));
        for(int sample = 0; sample < frames; ++sample)
        {
          const auto value = node->audio->data.channel(0)[sample];
          CHECK(std::isfinite(value));
          if(tick % 8 == 0) CHECK(value == 0.);
          audible = std::max(audible, std::abs(value));
        }
      }
      CHECK(audible > .0001);
      node->all_notes_off();
    });
  });
}

TEST_CASE("Chip unsorted events match chronological MIDI and automation", "[emulation][qualification][execution]")
{
  withState([](ossia::execution_state& state) {
    eachChip(state, [&](auto makeNode, int volume) {
      auto ordered = makeNode(), shuffled = makeNode();
      const int quarter = state.bufferSize / 4;
      const std::array<libremidi::ump, 4> events{
        libremidi::from_midi1::note_on(0, 60, 110),
        libremidi::from_midi1::note_on(0, 64, 110),
        libremidi::from_midi1::note_off(0, 60, 0),
        libremidi::from_midi1::note_off(0, 64, 0)};
      for(int i : {0, 1, 2, 3}) message(*ordered, events[i], i * quarter);
      for(int i : {3, 1, 0, 2}) message(*shuffled, events[i], i * quarter);
      ordered->controls[volume]->data.write_value(.5f, quarter);
      ordered->controls[volume]->data.write_value(.25f, 2 * quarter);
      ordered->controls[volume]->data.write_value(1.f, 3 * quarter);
      shuffled->controls[volume]->data.write_value(1.f, 3 * quarter);
      shuffled->controls[volume]->data.write_value(.5f, quarter);
      shuffled->controls[volume]->data.write_value(.25f, 2 * quarter);
      for(int tick = 0; tick < 8; ++tick)
      {
        state.samples_since_start += state.bufferSize;
        const auto token = window(state, 0, state.bufferSize);
        run(*ordered, state, token); run(*shuffled, state, token);
        for(int channel = 0; channel < 2; ++channel)
        {
          const auto& a = ordered->audio->data.channel(channel);
          const auto& b = shuffled->audio->data.channel(channel);
          CHECK(std::equal(a.begin(), a.end(), b.begin(), b.end()));
        }
        clearInputs(*ordered); clearInputs(*shuffled);
      }
    });
  });
}

TEST_CASE("Chip short loop slices retain full host output and earlier samples", "[emulation][qualification][transport][buffers]")
{
  withState([](ossia::execution_state& state) {
    const auto checkSlices = [&](auto node, bool audible) {
      const int frames = state.bufferSize, cut = frames / 3;
      message(*node, libremidi::from_midi1::note_on(0, 60, 110));
      for(int tick = 0; tick < 16; ++tick)
      {
        state.samples_since_start += frames;
        run(*node, state, window(state, 0, frames));
        clearInputs(*node);
      }
      if(audible) REQUIRE(peak(*node) > .000001);
      auto first = window(state, 0, cut);
      auto remainder = window(state, cut, frames - cut);
      first.end_discontinuous = true;
      remainder.start_discontinuous = true;
      message(*node, libremidi::from_midi1::note_on(0, 67, 110), cut);
      // Recycling drops channel sizes before the short first slice, as in the
      // native graph. Both consumers must already see the full host block.
      state.samples_since_start += frames;
      run(*node, state, first, true);
      REQUIRE(node->audio->data.channels() == 2);
      std::array<std::vector<double>, 2> prefix;
      for(int channel = 0; channel < 2; ++channel)
      {
        const auto& output = node->audio->data.channel(channel);
        REQUIRE(output.size() == std::size_t(frames));
        prefix[channel].assign(output.begin(), output.begin() + cut);
      }
      if(audible)
        REQUIRE(std::any_of(prefix[0].begin(), prefix[0].end(),
            [](double sample) { return std::abs(sample) > .000001; }));
      run(*node, state, remainder);
      for(int channel = 0; channel < 2; ++channel)
      {
        const auto& output = node->audio->data.channel(channel);
        REQUIRE(output.size() == std::size_t(frames));
        CHECK(std::equal(prefix[channel].begin(), prefix[channel].end(), output.begin()));
      }
      // A recycled or retained channel can already have the right size but
      // contain stale samples outside an interval's short middle window.
      clearInputs(*node);
      for(auto& channel : node->audio->data.get())
        std::fill(channel.begin(), channel.end(), 1.25);
      const int middleBegin = frames / 4, middleEnd = middleBegin + cut;
      state.samples_since_start += frames;
      run(*node, state, window(state, middleBegin, cut));
      for(int channel = 0; channel < 2; ++channel)
      {
        const auto& output = node->audio->data.channel(channel);
        REQUIRE(output.size() == std::size_t(frames));
        CHECK(std::all_of(output.begin(), output.begin() + middleBegin,
            [](double sample) { return sample == 0.; }));
        CHECK(std::all_of(output.begin() + middleEnd, output.end(),
            [](double sample) { return sample == 0.; }));
      }
      node->all_notes_off();
    };
    eachChip(state, [&](auto makeNode, int) { checkSlices(makeNode(), true); });
    {
      INFO("Wurlitzer");
      auto node = std::make_shared<Emulation::Wurli::Executor::Node>(state);
      auto engine = std::make_shared<Emulation::Wurli::Engine>(state.sampleRate);
      node->install(engine);
      REQUIRE(node->ready());
      checkSlices(node, true);
    }
    {
      INFO("Roland LA without ROMs");
      auto node = std::make_shared<Emulation::La::Executor::Node>(state);
      CHECK_FALSE(node->ready());
      checkSlices(node, false);
    }
  });
}
