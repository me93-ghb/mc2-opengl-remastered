# Cross-platform multiplayer

The Windows x64 and macOS ARM64 builds use version 1 of the `MC2N` packet format.
Both peers need this change. Older builds use native C++ layouts and cannot join.
The existing Git revision check still applies after the protocol check.

`code/mpwire.cpp` defines the field order for all 21 active message types.
The eight-byte header contains `MC2N`, a little-endian 16-bit version, a message
type byte, and a zero reserved byte. Integers occupy 1, 2, or 4 bytes on the wire.
Native `long`, GUID Data1, and packed combat chunks use 4 bytes. Floats use their
32-bit IEEE representation. Strings are terminated and their unused bytes are
zero. No native padding or `MC2Player.player` pointer enters the packet.

I chose a codec at the transport boundary to preserve the existing gameplay
handlers. Changing every gameplay `long` and structure packing would also affect
save data and internal APIs. The receiver reconstructs its own native layout,
then the existing handlers check host and player authority. ENet channel choices
remain unchanged. Invalid or incompatible packets are rejected before dispatch;
the client reports a bad version and the peer is disconnected.

The ENet wrapper transfers ownership of broadcasts to ENet, including empty
lobbies. It frees unicast packets only when `enet_peer_send` fails.
`mp_transport_check` uses a checking allocator to catch a second free after an
empty-lobby broadcast, then checks reliable and unreliable delivery.

Build and run `mp_wire_check` for packet fixtures, explicit byte-order checks,
pointer omission, truncated and oversized messages, invalid versions, and a
bounded malformed-input sweep. The same fixture corpus must match on each OS:

```
mp_wire_check wire-v1.hex
```

For game checks, launch with the existing `MC2_MP_AUTOHOST`, `MC2_MP_AUTOJOIN`,
`MC2_MP_AUTOLAUNCH=2`, `MC2_MP_SCRIPT_ORDERS=1`, and `MC2_LOG=1` hooks. Collect
host and client logs, then run:

```
python tests/mp/check_crossplay_logs.py host.log client.log
```

The checker requires matching rosters and seeds, orders applied by the host,
movement, damage and world updates at the client, and the same match winner.
The automated quickstart match does not cover every multiplayer map, custom
lance configuration, or reinforcement option.

The TCP/IP panel passes its entered address through `beginSessionScan` before
requesting the direct session. The automated join hook uses this same method;
it must not bypass it with `setDirectAddress`, or a broken manual join can pass
the smoke test. The original numeric address field uses the default UDP port.
