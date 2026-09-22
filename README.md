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

One file, shared with the broker, because the Enabler reads
`dds.ddsmodule` and the broker reads `dds.ngsild`:

```json
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

## Build

```sh
make            # needs the DDS Enabler and Fast DDS in /usr/local
make install    # drops dds.so into /opt/seamware/plugins/bridge/
```

Nothing of the broker's is linked: `ktrace` and the rest resolve from the
running process, which is linked `rdynamic`.

## State

Topics only. `channelAdd` answers `BRIDGE_UNSUPPORTED` for services and
actions, which is the contract's way of saying not yet.

---

Copyright 2026 Seamware · Apache-2.0
