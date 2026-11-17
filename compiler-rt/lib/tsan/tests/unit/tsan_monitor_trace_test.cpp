#include "tsan_monitor.h"
#include "tsan_monitor_trace_config.h"

#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace __tsan {
namespace {

enum class TraceAction { Begin, End, Acquire, Release };

struct TraceEvent {
  TraceAction action;
  Tid thread_id;
  uptr lock_id;
};

struct TraceCase {
  std::string name;
  std::vector<TraceEvent> events;
  bool expects_cycle = false;
  std::string error;
};

std::string Trim(const std::string& text) {
  const auto first = text.find_first_not_of(" \t\r\n\f\v");
  return first == std::string::npos
             ? ""
             : text.substr(first, text.find_last_not_of(" \t\r\n\f\v") - first + 1);
}

std::string Lower(std::string text) {
  for (char& c : text)
    c = std::tolower(static_cast<unsigned char>(c));
  return text;
}

bool ParseId(const std::string& token, char prefix, uptr& result) {
  uptr pos = !token.empty() && token[0] == prefix ? 1 : 0;
  if (pos == token.size())
    return false;
  result = 0;
  for (; pos < token.size(); ++pos) {
    const char c = token[pos];
    if (c < '0' || c > '9' ||
        result > (std::numeric_limits<uptr>::max() - (c - '0')) / 10)
      return false;
    result = result * 10 + (c - '0');
  }
  return true;
}

// Mirrors monitor.parser.EventParser. State is reset for every <START> block.
class TraceParser {
 public:
  bool ParseEvent(const std::string& line, std::vector<TraceEvent>& events) {
    std::istringstream input(Lower(line));
    std::vector<std::string> tokens;
    for (std::string token; input >> token;)
      tokens.push_back(token);
    if (tokens.size() < 2)
      return false;
    uptr id;
    if (tokens[0] == "alias") {
      if (tokens.size() != 3 || !ValidAlias(tokens[1]) ||
          aliases_.count(tokens[1]) || ParseId(tokens[1], 't', id) ||
          !ParseId(tokens[2], 't', id) || id > std::numeric_limits<Tid>::max())
        return false;
      aliases_[tokens[1]] = static_cast<Tid>(id);
      return true;
    }

    Tid tid;
    if (!ThreadId(tokens[0], tid))
      return false;
    const auto& action = tokens[1];
    if (action == "b" || action == "begin" || action == "e" || action == "end") {
      if (tokens.size() != 2)
        return false;
      events.push_back({action == "b" || action == "begin" ? TraceAction::Begin
                                                         : TraceAction::End,
                        tid, 0});
      return true;
    }
    if (tokens.size() != 3)
      return false;
    if (action == "<") {
      Tid target;
      if (!ThreadId(tokens[2], target))
        return false;
      const uptr lock = next_lock_++;
      events.push_back({TraceAction::Acquire, tid, lock});
      events.push_back({TraceAction::Release, tid, lock});
      events.push_back({TraceAction::Acquire, target, lock});
      events.push_back({TraceAction::Release, target, lock});
      return true;
    }
    if (!ParseId(tokens[2], 'l', id))
      return false;
    if (action == "a" || action == "acq" || action == "acq-rel")
      events.push_back({TraceAction::Acquire, tid, id});
    else if (action != "r" && action != "rel")
      return false;
    if (action == "r" || action == "rel" || action == "acq-rel")
      events.push_back({TraceAction::Release, tid, id});
    return true;
  }

 private:
  bool ThreadId(const std::string& token, Tid& tid) const {
    const auto alias = aliases_.find(token);
    if (alias != aliases_.end()) {
      tid = alias->second;
      return true;
    }
    uptr id;
    if (!ParseId(token, 't', id) || id > std::numeric_limits<Tid>::max())
      return false;
    tid = static_cast<Tid>(id);
    return true;
  }

  static bool ValidAlias(const std::string& token) {
    if (token.empty() || token[0] < 'a' || token[0] > 'z')
      return false;
    for (char c : token)
      if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_')
        return false;
    return true;
  }

  std::map<std::string, Tid> aliases_;
  uptr next_lock_ = 1000000;
};

bool Marker(const std::string& line, const std::string& marker,
            std::string& tag) {
  if (line.compare(0, marker.size(), marker) != 0 || line.back() != '>')
    return false;
  const auto rest = line.substr(marker.size(), line.size() - marker.size() - 1);
  if (rest.find('>') != std::string::npos ||
      (!rest.empty() && !std::isspace(static_cast<unsigned char>(rest[0]))))
    return false;
  tag = Trim(rest);
  return true;
}

bool ParseTraceFile(std::istream& input, const std::string& filename,
                    std::vector<TraceCase>& traces, std::string& error) {
  TraceParser parser;
  TraceCase current;
  bool open = false;
  uptr line_number = 0;
  uptr block_number = 0;
  for (std::string raw; std::getline(input, raw);) {
    ++line_number;
    const auto line = Trim(raw.substr(0, raw.find('#')));
    if (line.empty())
      continue;
    std::string tag;
    std::string problem;
    if (Marker(line, "<START", tag)) {
      if (open) {
        problem = "Unexpected <START> before previous block closed";
      } else {
        open = true;
        parser = TraceParser();
        current = TraceCase();
        current.name = filename + "_" + std::to_string(block_number++) + "_" +
                       (tag.empty() ? "acyclic" : tag);
        for (char& c : current.name)
          if (!std::isalnum(static_cast<unsigned char>(c)))
            c = '_';
        current.expects_cycle = Lower(tag).find("cycle") != std::string::npos;
      }
    } else if (Marker(line, "<END", tag)) {
      if (!open) {
        problem = "Unexpected <END> without an open <START>";
      } else {
        traces.push_back(current);
        open = false;
      }
    } else if (open && !parser.ParseEvent(line, current.events)) {
      problem = "Invalid format: " + line;
    }
    if (!problem.empty()) {
      error = filename + ":" + std::to_string(line_number) + ": " + problem;
      return false;
    }
  }
  if (open || input.bad()) {
    error = filename + (open ? ": File ended before <END>" : ": Read failed");
    return false;
  }
  return true;
}

std::vector<TraceCase> LoadTraces() {
  std::vector<TraceCase> traces;
  for (const char* filename : {"cycles.txt", "no_cycles.txt"}) {
    const std::string path = std::string(TSAN_MONITOR_TRACE_DIR) + "/" + filename;
    std::ifstream input(path);
    std::string error;
    const auto count = traces.size();
    if (!input.is_open())
      error = "Cannot open " + path;
    else if (ParseTraceFile(input, filename, traces, error) &&
             traces.size() == count)
      error = "No trace blocks in " + path;
    if (!error.empty()) {
      TraceCase failure;
      failure.name = "InputError_" + std::to_string(traces.size());
      failure.error = error;
      traces.push_back(failure);
    }
  }
  return traces;
}

class MonitorTraceTest : public testing::TestWithParam<TraceCase> {};

TEST_P(MonitorTraceTest, DetectsExpectedCycle) {
  const auto& trace = GetParam();
  ASSERT_TRUE(trace.error.empty()) << trace.error;
  std::map<Tid, std::unique_ptr<Monitor>> threads;
  std::map<uptr, std::unique_ptr<Monitor>> locks;
  bool found_cycle = false;

  for (uptr i = 0; i < trace.events.size(); ++i) {
    const auto& event = trace.events[i];
    SCOPED_TRACE(testing::Message() << trace.name << ": event " << i + 1);
    auto& p_m = threads[event.thread_id];
    if (!p_m)
      p_m.reset(new Monitor(event.thread_id));

    AcquireResult result = AcquireResult::Ok;
    if (event.action == TraceAction::Begin) {
      p_m->BeginTransaction();
    } else if (event.action == TraceAction::End) {
      p_m->EndTransaction();
    } else {
      auto& l_m = locks[event.lock_id];
      if (!l_m)
        l_m.reset(new Monitor(0));
      if (event.action == TraceAction::Acquire)
        result = p_m->Acquire(*l_m);
      else
        p_m->Release(*l_m);
    }
    if (result != AcquireResult::Ok) {
      ASSERT_TRUE(trace.expects_cycle) << "Unexpected cycle";
      found_cycle = true;
      break;
    }
  }
  EXPECT_EQ(found_cycle, trace.expects_cycle) << "Expected cycle was missed";
}

INSTANTIATE_TEST_SUITE_P(
    SharedPythonTraces, MonitorTraceTest, testing::ValuesIn(LoadTraces()),
    [](const testing::TestParamInfo<TraceCase>& info) { return info.param.name; });

TEST(MonitorTraceParser, ExpandsPythonNotation) {
  std::istringstream input(
      "ignored outside blocks\n<START mixed_case_CYCLE>\n"
      "alias p t1\nalias q 2\nP BEGIN # comment\nq b\n"
      "p < q\np acq-rel l7\nq a 8\nq r l8\np end\nq e\n<END tag>\n"
      "<START>\n1 < t2\n<END>\n");
  std::vector<TraceCase> traces;
  std::string error;
  ASSERT_TRUE(ParseTraceFile(input, "sample", traces, error)) << error;
  ASSERT_EQ(traces.size(), 2u);
  EXPECT_TRUE(traces[0].expects_cycle);
  EXPECT_FALSE(traces[1].expects_cycle);
  const std::vector<TraceEvent> expected = {
      {TraceAction::Begin, 1, 0}, {TraceAction::Begin, 2, 0},
      {TraceAction::Acquire, 1, 1000000}, {TraceAction::Release, 1, 1000000},
      {TraceAction::Acquire, 2, 1000000}, {TraceAction::Release, 2, 1000000},
      {TraceAction::Acquire, 1, 7}, {TraceAction::Release, 1, 7},
      {TraceAction::Acquire, 2, 8}, {TraceAction::Release, 2, 8},
      {TraceAction::End, 1, 0}, {TraceAction::End, 2, 0}};
  ASSERT_EQ(traces[0].events.size(), expected.size());
  for (uptr i = 0; i < expected.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(traces[0].events[i].action, expected[i].action);
    EXPECT_EQ(traces[0].events[i].thread_id, expected[i].thread_id);
    EXPECT_EQ(traces[0].events[i].lock_id, expected[i].lock_id);
  }
  EXPECT_EQ(traces[1].events[0].lock_id, 1000000u);
}

TEST(MonitorTraceParser, RejectsMalformedInputAndResetsAliases) {
  for (const char* text : {
           "<END>\n", "<START>\n<START>\n", "<START>\n1 begin\n",
           "<START>\n1 acq\n<END>\n", "<START>\n1 b extra\n<END>\n",
           "<START>\nalias t1 t2\n<END>\n",
           "<START>\nalias p missing\n<END>\n",
           "<START>\nalias p t1\nalias p t2\n<END>\n",
           "<START>\nalias p t1\n<END>\n<START>\np b\n<END>\n",
           "<START>\n1 < missing\n<END>\n"}) {
    SCOPED_TRACE(text);
    std::istringstream input(text);
    std::vector<TraceCase> traces;
    std::string error;
    EXPECT_FALSE(ParseTraceFile(input, "sample", traces, error));
    EXPECT_FALSE(error.empty());
  }
}

}  // namespace
}  // namespace __tsan
