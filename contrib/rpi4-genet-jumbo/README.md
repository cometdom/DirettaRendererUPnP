# Raspberry Pi 4 (bcmgenet) jumbo frames beyond 3824 bytes — experimental kernel patches

Two patches for the Raspberry Pi kernel tree (`raspberrypi/linux`, branch
`rpi-6.18.y`, tested to apply and compile on commit `35fc4a654`) that let the
Pi 4 / Pi 400 / CM4 Ethernet driver receive frames longer than the current
3824 byte MTU ceiling, so a DRUP host on a Pi 4 can try MTU 9000 on the Diretta
link.

They are **compile-tested only** (arm64, `bcm2711_defconfig`, `W=1`, checkpatch
clean). Whether the GENET receive DMA really behaves as assumed above 3840 bytes
is exactly what a run on hardware has to tell.

## Background

The upstream history is in [raspberrypi/linux#5419](https://github.com/raspberrypi/linux/pull/5419)
(delgh1's original 9000 byte attempt), [raspberrypi/linux#5534](https://github.com/raspberrypi/linux/pull/5534)
(Dave Stevenson's cleaned-up WIP) and [raspberrypi/linux#5561](https://github.com/raspberrypi/linux/issues/5561)
(tracking issue). None of it is in any `rpi-6.x.y` branch today: `bcmgenet.c`
still has 2048 byte Rx buffers, the 1536 byte `ENET_MAX_MTU_SIZE`, and no
`max_mtu`, so a stock kernel refuses anything above MTU 1500.

What Dave Stevenson found on the hardware:

- The RBUF "packet ready" threshold register (`RBUF_PKT_RDY_THLD`, 8 bits in
  units of 16 bytes, hardware default 0x80 = 2048) decides when the Rx DMA
  starts on a frame. Below the threshold the whole frame is buffered first and
  the 64 byte receive status block (RSB) written at the head of the buffer
  describes the complete frame. Raising it to 0xf0 (3840 bytes, the largest
  value aligned to the 256 byte DMA burst) is where **3824** comes from.
- Above the threshold the DMA starts early: the RSB carries a truncated length
  and no `DMA_EOP` (he saw `0x08403f80`, i.e. 2112 = threshold + RSB, SOP set,
  EOP clear), the driver sees "no EOP" and drops the frame as fragmented. But
  re-reading the **descriptor's own length/status word** once the DMA had
  finished returned `0x0fe87f80`: full length, SOP and EOP. The information is
  there, the driver just never looks at it.
- Frames longer than the Rx buffer are additionally spread over consecutive
  descriptors (SOP on the first, EOP on the last), which the driver has never
  reassembled.

## The patches

1. `0001-net-bcmgenet-permit-MTU-up-to-9000-and-raise-RBUF_PK.patch` — port of
   the two commits of PR #5534: 9000 byte `ENET_MAX_MTU_SIZE`, `dev->max_mtu`,
   `RBUF_PKT_RDY_THLD = 0xf0`. Rx buffers stay at 2048 bytes (PR #5534 used
   10240). Two module parameters, `rx_buf_len` (2048..16384) and
   `pkt_rdy_thld` (16 byte units), allow comparing strategies without
   rebuilding.
2. `0002-net-bcmgenet-reassemble-Rx-frames-that-span-descript.patch` — the
   receive path is reworked around the descriptor status words:
   - `bcmgenet_rx_refill()` clears the descriptor word, so zero means "DMA not
     finished with this buffer";
   - before touching any buffer, the driver walks the descriptor words to find
     the SOP..EOP run of the next frame (up to 8 descriptors). If the run is
     incomplete, NAPI is asked to poll again immediately (no interrupt will
     announce the end of a frame whose descriptor was handed over early); a
     frame that never completes within 20 ms is dropped and counted;
   - the buffers of a complete frame are chained on the first one's
     `frag_list` and go up as a single skb; the hardware checksum from the RSB
     is only trusted when the RSB saw the whole frame (`DMA_EOP` set);
   - safety net: if the descriptor word of a frame is still zero, the RSB is
     peeked in place (64 byte `dma_sync`, no unmap). When it shows SOP+EOP the
     frame was fully buffered and the RSB is enough. So even if it turns out
     the hardware never writes descriptor words back in 64B-RSB mode, standard
     frames and everything up to 3840 bytes keep working exactly as today, and
     the Pi stays reachable.

Two new ethtool counters per ring show what the hardware actually did:
`rxqN_multi_bd` (frames reassembled from more than one descriptor) and
`rxqN_early_rdy` (frames whose RSB lacked EOP, i.e. handed over before their
end). `rxqN_fragmented_errors` now counts frames the driver gave up on.
The default ring shows up as `rxq4_*` in `ethtool -S`.

## Building

On the fork (`herisson-88/linux`, branch `rpi-6.18.y`):

```bash
git checkout -b rpi-6.18.y-genet-jumbo rpi-6.18.y
git am contrib/rpi4-genet-jumbo/000*.patch      # from this repository's checkout
```

Then build and install the kernel the usual Raspberry Pi way (64-bit,
`bcm2711_defconfig`, `Image`, `modules`, `dtbs`; see
https://www.raspberrypi.com/documentation/computers/linux_kernel.html).
`bcmgenet` is built in (`CONFIG_BCMGENET=y`), so the module parameters go on
the kernel command line (`/boot/firmware/cmdline.txt`), e.g.
`bcmgenet.rx_buf_len=10240` or `bcmgenet.pkt_rdy_thld=0x80`.

## Test protocol

Everything below is on the Pi unless stated. `PEER` is a jumbo-capable host
on the same switch, with the switch itself configured for jumbo frames.

1. **Sanity, MTU 1500.** Boot the new kernel, check `dmesg | grep -i genet`,
   confirm normal traffic (ssh, `ping`, `iperf3`) works and that
   `ethtool -S eth0 | grep -E 'rxq4_(multi_bd|early_rdy|fragmented_errors)'`
   stays at zero. This validates the reworked receive path on ordinary frames
   and the RSB safety net.

2. **The old ceiling.** `ip link set dev eth0 mtu 9000` (and the same on
   `PEER`). From `PEER`: `ping -M do -c 20 -s 3796 <pi>` must work as before.

3. **Beyond 3840.** From `PEER`, in order: `-s 3900`, `-s 4000`, `-s 6000`,
   `-s 8972` (the largest ICMP payload at MTU 9000). After each, read the
   three counters above and `dmesg`:
   - replies + `rxq4_early_rdy` climbing + `rxq4_multi_bd` climbing: the DMA
     splits over 2048 byte buffers and hands the first one over early; both
     mechanisms are needed and work;
   - replies + `rxq4_early_rdy` climbing, `rxq4_multi_bd` at zero: the DMA
     wrote the whole frame in one buffer (then `rx_buf_len=2048` was silently
     overrun: stop, and retest with `bcmgenet.rx_buf_len=10240`);
   - no replies + `rxq4_fragmented_errors` climbing + "frame never completed"
     in `dmesg`: the descriptor word is never written with EOP (or not at all).
     Retest with `bcmgenet.rx_buf_len=10240` to separate the two questions
     ("does the descriptor word get written?" vs "does the DMA chain buffers?").
   - "oversized packet" in `dmesg`: a descriptor reports more than the buffer
     size; report the exact `dmesg` lines.

4. **Reassembly with today's peers.** To exercise the multi-descriptor path
   without a 9000-capable peer, boot with `bcmgenet.pkt_rdy_thld=0x80` (the
   hardware default): every frame above about 2004 bytes then takes the early
   handover path, and at MTU 3824 with `-s 3000` pings `rxq4_multi_bd` and
   `rxq4_early_rdy` should both climb while pings keep answering.

5. **Diretta.** With the Diretta target and switch at MTU 9000, run DRUP at
   MTU 9000 (`--mtu 9000` or `MTU=9000`, or let `measSendMTU` find it), play
   DSD256/DSD512 and watch `[DirettaSync] Enabled, MTU=` in the log, the
   renderer's underrun counters, and the three ethtool counters. Note that DRUP
   mostly *transmits* on that link: the Tx side of the driver was not changed
   beyond the frame-length defines, and frames above 2048 bytes have never been
   sent with the Tx ring buffer size the driver uses (patch 1 sets it to the
   9216 byte maximum, mirroring PR #5534).

What to report back: the ping results per size, `ethtool -S eth0 | grep rxq4_`
before/after, `dmesg | grep -i genet`, and which `rx_buf_len` /
`pkt_rdy_thld` values were in effect.

## Known limits

- 8 descriptors per frame, i.e. up to 16 KB with 2048 byte buffers.
- A frame whose descriptor never completes keeps NAPI polling for up to 20 ms
  before it is dropped. This only happens above the threshold, and only if the
  hardware does not write descriptor words back.
- `DMA_BUFLENGTH_MASK` is 12 bits in the header but the driver has always used
  the full 16 bit field; with 2048 byte Rx buffers no Rx length ever exceeds
  4095. A 9000 byte *linear* Tx buffer in a single descriptor is untested.
