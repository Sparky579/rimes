#include "BrokerClient.h"
#include "key_routing.hpp"

#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

// Deterministic console contract checks for BrokerClient while disconnected.
// Does not launch or wait on a Broker: unavailable keys must fail open
// immediately, must not be marked consumed, and must not invent a snapshot
// that could be applied later as delayed replay.

namespace {

int g_failures = 0;

void Fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  ++g_failures;
}

void Expect(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

using rimes::windows::tsf::BrokerClient;
using rimes::windows::tsf::BrokerInputState;
using rimes::windows::tsf::BrokerKeyEvent;
using rimes::windows::tsf::BrokerKeyPhase;
using rimes::windows::tsf::BrokerKeyResult;
using rimes::windows::tsf::CreateBrokerClient;

void ExpectUnavailable(BrokerClient* client, BrokerKeyPhase phase,
                       WPARAM virtual_key, LPARAM key_data,
                       std::string_view label) {
  BrokerInputState state;
  state.has_snapshot = true;  // poison; Unavailable must clear/replace
  state.composing = true;
  state.composition = L"stale";
  state.commit_text = L"stale_commit";
  const BrokerKeyResult result =
      client->HandleKey({phase, virtual_key, key_data}, &state);
  Expect(result == BrokerKeyResult::kUnavailable, label);
  Expect(!state.has_snapshot,
         "unavailable must not leave a snapshot for later apply/replay");
  Expect(!state.composing,
         "unavailable must not retain a stale composing flag");
  Expect(state.composition.empty(),
         "unavailable must not retain composition text");
  Expect(state.commit_text.empty(),
         "unavailable must not retain commit text");
}

int RunContractTests() {
  using rimes::windows::core::KeyModifiers;
  using rimes::windows::tsf::ShouldPassIdleNumberKeyToHost;
  const auto shift = static_cast<std::uint32_t>(KeyModifiers::kShift);
  const auto caps = static_cast<std::uint32_t>(KeyModifiers::kCapsLock);
  for (WPARAM digit = '0'; digit <= '9'; ++digit) {
    Expect(ShouldPassIdleNumberKeyToHost(digit, 0, false, false),
           "idle number-row digits stay with the host before pre-claim");
    Expect(ShouldPassIdleNumberKeyToHost(digit, caps, false, false),
           "Caps Lock does not change ordinary digit routing");
    Expect(!ShouldPassIdleNumberKeyToHost(digit, shift, false, false),
           "shifted number-row punctuation remains eligible for Rime");
    Expect(!ShouldPassIdleNumberKeyToHost(digit, 0, true, false),
           "candidate selection digits remain eligible while composing");
    Expect(!ShouldPassIdleNumberKeyToHost(digit, 0, false, true),
           "Buffer still captures number-row digits without preedit");
  }
  for (WPARAM digit = VK_NUMPAD0; digit <= VK_NUMPAD9; ++digit) {
    Expect(ShouldPassIdleNumberKeyToHost(digit, 0, false, false),
           "idle numpad digits stay with the host before pre-claim");
    Expect(ShouldPassIdleNumberKeyToHost(digit, shift, false, false),
           "Shift does not turn numpad digits into punctuation");
    Expect(!ShouldPassIdleNumberKeyToHost(digit, 0, true, false),
           "numpad candidate selection remains eligible while composing");
    Expect(!ShouldPassIdleNumberKeyToHost(digit, 0, false, true),
           "Buffer still captures numpad digits without preedit");
  }
  for (const WPARAM key : {WPARAM{'A'}, WPARAM{VK_SPACE}, WPARAM{VK_OEM_PERIOD},
                           WPARAM{VK_BACK}, WPARAM{VK_SHIFT}}) {
    Expect(!ShouldPassIdleNumberKeyToHost(key, 0, false, false),
           "numeric bypass does not change letters, punctuation or controls");
  }

  auto client = CreateBrokerClient();
  Expect(client != nullptr, "CreateBrokerClient returns a client");
  if (!client) {
    return EXIT_FAILURE;
  }

  Expect(!client->IsConnected(), "fresh client is disconnected");

  // Exercise the production client, not just the stateless policy. Native
  // idle digits need neither a connection nor an authoritative IME snapshot.
  BYTE saved_keyboard[256]{};
  BYTE idle_keyboard[256]{};
  const bool saved = GetKeyboardState(saved_keyboard) != FALSE;
  Expect(saved && SetKeyboardState(idle_keyboard),
         "isolated logical keyboard state initialized");
  for (const WPARAM first : {WPARAM{'0'}, WPARAM{VK_NUMPAD0}}) {
    for (WPARAM digit = first; digit < first + 10; ++digit) {
      BrokerInputState state;
      state.has_snapshot = true;
      Expect(client->HandleKey({BrokerKeyPhase::kKeyDown, digit, 1}, &state) ==
                 BrokerKeyResult::kPassThrough,
             "production client leaves idle digits on the host path without IPC");
      Expect(!state.has_snapshot, "native digit does not invent an IME snapshot");
      Expect(client->HandleKey({BrokerKeyPhase::kKeyUp, digit, 0}, nullptr) ==
                 BrokerKeyResult::kPassThrough,
             "production client never owns an idle digit release");
    }
  }
  if (saved) SetKeyboardState(saved_keyboard);

  ExpectUnavailable(client.get(), BrokerKeyPhase::kTestKeyDown, 'N', 0,
                    "disconnected TestKeyDown is unavailable (fail-open)");
  ExpectUnavailable(client.get(), BrokerKeyPhase::kKeyDown, 'N', 1,
                    "disconnected KeyDown is unavailable (fail-open)");
  ExpectUnavailable(client.get(), BrokerKeyPhase::kTestKeyDown, 'I', 0,
                    "second disconnected letter is also unavailable");
  ExpectUnavailable(client.get(), BrokerKeyPhase::kKeyDown, 'I', 1,
                    "second disconnected KeyDown is unavailable");

  // Matching releases while never connected must not be consumed either:
  // nothing was processed, so the host keeps the key.
  BrokerInputState up_state;
  Expect(client->HandleKey({BrokerKeyPhase::kKeyUp, 'N', 0}, &up_state) ==
             BrokerKeyResult::kPassThrough,
         "KeyUp with no prior consumed KeyDown passes through");
  Expect(!up_state.has_snapshot, "unhandled KeyUp has no snapshot");

  Expect(client->HandleKey({BrokerKeyPhase::kTestKeyUp, 'N', 0}, nullptr) ==
             BrokerKeyResult::kUnavailable,
         "disconnected TestKeyUp is unavailable");

  // Never BeginConnect here: CTest must not launch Session Broker or wait.
  client->Disconnect();
  Expect(!client->IsConnected(), "Disconnect leaves the client disconnected");
  ExpectUnavailable(client.get(), BrokerKeyPhase::kKeyDown, 'A', 1,
                    "after Disconnect, keys remain unavailable");

  if (g_failures != 0) {
    std::cerr << g_failures << " broker-client contract failure(s)\n";
    return EXIT_FAILURE;
  }
  std::cerr << "broker-client numeric routing and unavailable/fail-open contract ok\n";
  return EXIT_SUCCESS;
}

}  // namespace

int wmain() { return RunContractTests(); }
