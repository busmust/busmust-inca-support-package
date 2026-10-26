# Third-party source attribution

New standalone BUSMUST adapter source in this directory is provided under
Apache-2.0 (LICENSE and NOTICE). Third-party sources below keep their own terms.
BMAPI, its SDK and linked native dependencies retain their separate licenses;
this directory's license does not relicense them or the enclosing application.

Only these components are compiled into the adapter; hardware transports and
examples from their repositories are excluded.

* `native/J2534/third_party/op2j2534/j2534.h`: Mackanized/op2j2534,
  commit `48aabcd558dc113da1fd35d0c9ab8ce3bc63c9fc`, Apache-2.0.
  The upstream header, LICENSE and NOTICE are unchanged. The enclosing private
  header selects standard one-byte packing and declares the supported extensions.
* `native/J2534/third_party/isotp-c`: SimonCahill/isotp-c,
  commit `1fc19e2f7e934c9fdf278ed1b87f32b9d983d513`, MIT.
  LICENSE is unchanged. BUSMUST changes in `isotp.h` and `isotp.c` add private
  per-link BS/STmin/WFT/timers/padding/frame flags, extended-address capacity,
  explicit transmit overrides, Flow Control retry and actual wire-ACK gating.
  BS=0 remains unlimited; invalid FC status and wrong CF sequence end a transfer.
  SF/FF encoding is checked before replacing a receive conversation; FD DLC
  and CF RX_DL checks ignore malformed frames without changing receive state.
  CF/FC frame-type checks ignore wrong N_AI types. Reserved received STmin uses 127 ms
  for the remainder of a segmented transmission unless an explicit TX override
  was selected before it started.
  Other upstream files retain their source content. All modified structs are
  private and have no BMAPI or J2534 ABI exposure.

The existing BMAPI SDK retains its own license. Standards and vendor SDKs used
for interface comparison are not included or redistributed by this component.

## Binary distribution

The customer ZIP supplies `BMAPI.dll` under its own SDK licence, retained at
`licenses/BMAPI/LICENSE`. This project's Apache-2.0 licence does not relicense it.

Official origins: https://github.com/Mackanized/op2j2534 ;
https://github.com/SimonCahill/isotp-c .
Neither ETAS binaries nor SAE/GM standards texts are included. This implementation
is not a claim of complete SAE/GMW conformance certification or an ETAS licence.
