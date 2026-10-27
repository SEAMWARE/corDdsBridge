# corDdsBridge

The **DDS bridge** for coraine: a `dds.so` that carries values between DDS
topics and NGSI-LD entity attributes.

It is a plugin, and deliberately not part of any umbrella build. The eProsima
stack it links is **21.4 MiB across nine libraries** — `libfastdds` alone is
16 MiB — which is roughly five times the size of the whole broker. Making that
a hard dependency of every coraine clone is exactly what the plugin
architecture exists to prevent, so this repo is cloned and built only by
deployments that want DDS.

## Why it is C++, and why that costs the broker nothing

The DDS Enabler's API takes `std::string`, `std::shared_ptr` and
`std::unique_ptr&` out-parameters. None of that is reachable from C: declaring
the mangled symbol gets you the name and never the argument ABI, and
`_GLIBCXX_USE_CXX11_ABI` is baked into every one of those names. There is no
`extern "C"` anywhere in the Enabler's public headers.

So this is a C++ translation unit that includes the C contract inside
`extern "C"`. Because a plugin is already a separate shared object, the broker
stays `PROJECT(coraine C)` and never sees a C++ token — it `dlopen`s a black
box exporting exactly one symbol, `bridgeRegister`.

`libstdc++` does enter the broker process at `dlopen` time. That is
unavoidable, contained, and real.

## The contract

`corBridge` holds it: `BridgeDriver.h` is what this fills in, `BridgeBroker.h`
is what the broker hands back. Everything crossing is plain data — `const
char*`, `int64_t` — so this plugin never touches the broker's allocator from a
DDS thread, and never learns what an entity is. Which topic corresponds to
which attribute is a Channel, and Channels live in the broker.

## Configuration

One file, shared with the broker: `dds.ddsmodule` is the DDS Enabler's
configuration (this plugin reads it and hands it to the Enabler) and the broker
reads `dds.ngsild`:

```jsonc
{
  "dds": {
    "ddsmodule": { "dds": { "domain": 0 } },
    "ngsild": {
      "topics": {
        "rt/pose": { "entityId": "urn:ngsi-ld:Robot:1", "entityType": "Robot", "attribute": "pose" }
      }
    }
  }
}
```

```sh
coraine --bridges dds --bridgeConfig /path/to/that.json
```

### Reaching the DDS system - `dds.ddsmodule.dds`

The Enabler joins the DDS system as one participant. These members of
`dds.ddsmodule.dds` decide which participants it finds:

| Member | Values | Default |
|---|---|---|
| `domain` | the DDS domain, 0-232 (ROS 2: `ROS_DOMAIN_ID`) | 0 |
| `transport` | `builtin` (shared memory and UDP), `udp`, `shm` | `builtin` |
| `whitelist-interfaces` | a list of IPv4 addresses of this host's interfaces - UDP uses only those | all interfaces |
| `ignore-participant-flags` | `no_filter`, `filter_different_host`, `filter_different_process`, `filter_same_process`, `filter_different_and_same_process` | `no_filter` |
| `allowlist`, `blocklist` | lists of `{"name": "<topic>", "type": "<type>"}`, `*` and `?` wildcards | everything |

```jsonc
"ddsmodule": {
  "dds": {
    "domain": 0,
    "transport": "udp",
    "whitelist-interfaces": ["192.168.1.138"]
  }
}
```

- Discovery is DDS's own: multicast (239.255.0.1) plus the participants on this
  host. It reaches the participants on the same host and on the same network
  segment; it does not cross a router, a VPN or a NAT.
- `shm` reaches only participants on the same host and sharing `/dev/shm` - a
  container without `--ipc=host` does not.
- `whitelist-interfaces` takes addresses, not interface names. With none of
  them on this host, the participant finds nothing; Fast DDS logs `All
  whitelist interfaces were filtered out`.
- A ROS 2 Discovery Server, unicast initial peers and Fast DDS XML profiles are
  not reachable through this configuration: the Enabler 1.2.2's participant
  reads none of them, and removes `ROS_DISCOVERY_SERVER` from the environment
  before it is created.
- A change of the file reloads `allowlist`, `blocklist` and `topics` while the
  broker runs. `domain`, `transport`, `whitelist-interfaces` and
  `ignore-participant-flags` are read once, at start.
- This plugin reads `dds.ddsmodule` itself and gives it to the Enabler as a
  YAML node, at start and on every change of the file. The Enabler's own
  reading of the file turns every plain value of a list into a map, and refuses
  `"whitelist-interfaces": ["127.0.0.1"]`.

## Build

```sh
make            # build
make install    # drops dds.so into /opt/seamware/plugins/bridge/
make contract   # does this still satisfy BridgeDriver.h? needs no DDS at all
```

Needs the DDS Enabler and Fast DDS in `/usr/local`, and **`nlohmann-json3-dev`**
— that last one is transitive and surprising: the Enabler's own public header
`ddsenabler_participants/Writer.hpp` includes `<nlohmann/json.hpp>`, so anything
compiling against the Enabler needs it. Nothing here uses it directly, which is
why its absence reads as an error in a file nobody here wrote. `make` checks for
it by name.

Whether it builds at all is a configuration decision, not a matter of which
list this repo appears in:

| `COR_BRIDGE_DDS` | |
|---|---|
| `auto` (default) | build where the Enabler is installed, skip where it is not — and say which |
| `ON` | build; a missing dependency is an error |
| `OFF` | do not build |

Nothing of the broker's is linked: `corLog` and the rest resolve from the
running process, which is linked `rdynamic`.

## State

Topics, services and actions:

- **topics** — samples in (`sampleIn` and its qualified / meta forms) and `publish` out;
- **services** — a request sent with `serviceInvoke` / `serviceInvokeTracked`, its reply back as
  `replyIn`; `dds.ngsild.services` in the configuration names each one's `requestType` and
  `replyType` (default `<name>_Request` / `<name>_Response`);
- **actions** — goals sent with `actionGoalSend`, cancelled with `actionGoalCancel`, their
  feedback and result back as goal events; `dds.ngsild.actions` names each one's ROS 2 action
  `type`;
- services and actions the DDS network announces are reported to the broker as discovered
  endpoints (`endpointDiscoveredIn`).

- **inventory** (ABI 14) — what the DDS network has announced, for a mapping tool:
  `GET /admin/bridges/dds/inventory` on the broker. See [Inventory](#inventory).

Each slot is filled only when the broker's bridge ABI has it (`ddsRegister.cpp`). What the broker
makes of them: coraine's [DDS in coraine](https://github.com/SEAMWARE/coraine/blob/main/doc/dds.md).

## Inventory

`GET /admin/bridges/dds/inventory` answers with every topic, service and action the DDS network has
announced to the broker, configured or not, in the shape the DDS discovery tools write, plus
`settled` and `types`:

```jsonc
{
  "discoveredAt": "2026-10-27T08:37:25Z",
  "settled": true,
  "topics": [
    { "name": "rt/chatter", "type": "std_msgs::msg::dds_::String_", "qos": "reliability: true\ndurability: false\nownership: false\nkeyed: false" }
  ],
  "services": [
    { "name": "add_two_ints",
      "request": { "type": "example_interfaces::srv::dds_::AddTwoInts_Request_", "qos": "..." },
      "reply":   { "type": "example_interfaces::srv::dds_::AddTwoInts_Response_", "qos": "..." } }
  ],
  "actions": [
    { "name": "fibonacci",
      "goal":     { "request": { "type": "...", "qos": "..." }, "reply": { "type": "...", "qos": "..." } },
      "result":   { "request": { ... }, "reply": { ... } },
      "cancel":   { "request": { ... }, "reply": { ... } },
      "feedback": { "type": "...", "qos": "..." },
      "status":   { "type": "...", "qos": "..." } }
  ],
  "types": {
    "std_msgs::msg::dds_::String_": {
      "idl": "module std_msgs\n{\n    module msg\n ...",
      "sample": { "data": "" }
    }
  }
}
```

| Member | |
|---|---|
| `discoveredAt` | when the answer was made, UTC |
| `settled` | `true`: discovery had been quiet for 3 s before the answer. `false`: the call's time ran out first (10 s, the broker's), and more may still be announced |
| `topics`, `services`, `actions` | as announced; `qos` is the Enabler's serialized QoS, as it is. ROS 2 housekeeping is included: `ros_discovery_info`, `rt/parameter_events`, `rt/rosout` and every node's parameter services (`<node>/get_parameters`, ...) |
| `types` | per type name, its IDL text (`idl`) and a JSON sample of it with every member at its zero value (`sample`), as the Enabler hands them over. Kept in memory only - `typesDirectory` holds the Enabler's binary form, not these |

DDS has no "discovery is done", so the call waits for it to go quiet: no topic, type, service or
action announced for 3 s, measured from the broker's start too. One call made right after the
broker starts answers with what the network had to say - 3.4 s for three ROS 2 nodes; a broker that
has been up longer answers at once. A node that starts after the answer is in the next one.

The list is flat: `from` is answered 404 and `depth` is not used.

---

Copyright 2026 Seamware · Apache-2.0
