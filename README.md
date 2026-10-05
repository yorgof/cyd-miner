# cyd-miner

Custom firmware for the Freenove FNK0103L "Cheap Yellow Display" (ESP32-32E with a 3.2"
240x320 ST7789 touch screen). It shows live Bitcoin stats and solo-mines against a
Stratum v1 pool.

Written in C on ESP-IDF v5.5 with no third-party components: the display driver, touch
driver, text rendering, SHA-256, Stratum client and web page are all in this repository.

> **This will not find a block.** At about 1 MH/s against a network of roughly
> 1 ZH/s, the odds are around one in twenty billion per year. Treat it as a stats
> display with a lottery ticket attached.

## What it does

Tap the screen to switch between two screens.

- **Stats**: price, block height, what a transaction costs right now, difficulty, network
  hashrate and blocks until the next halving, fetched from
  [mempool.space](https://mempool.space). The address of the device's web page is in the
  top right corner.
- **Miner**: hashrate, accepted and rejected shares, best share difficulty, pool
  difficulty, uptime, WiFi signal and pool status.

The transaction cost is for a typical 140 vB transaction at the fee rate that gets into
the next block, in your currency, with the rate itself next to it.

Mining runs on both cores. Core 1 drives the ESP32's hardware SHA engine (about
1030 kH/s) and core 0 hashes in software alongside WiFi and the UI (about 27 kH/s).
Once a minute the log shows both rates.

The hardware loop (`main/sha_hw_scan.S`) writes each SHA block into the engine's
registers while the block before it is still being computed, and waits a counted
number of CPU cycles instead of polling. It looks at 16 bits of each hash; a hash
that passes is computed again in software before anything is done with it, so a wrong
hash from the hardware costs a nonce and never a bad share. At boot the firmware checks
the loop against the software hash. If it gets hashes wrong, then or later, the miner
drops to a slower loop with wider margins (about 940 kH/s), then to a polled one (about
410 kH/s), then to software.

## Hardware

Built for and tested on the Freenove FNK0103L only. Other ESP32 "CYD" boards with an
SPI display should work after changing the pins, and possibly the panel init, in
`main/config.h` and `main/lcd.c`. The hardware SHA path needs an original ESP32 (not
S2, S3 or C3), and its timing was measured on an ESP32-D0WD-V3, revision 3.1.

## Install

Nothing needs to be compiled. Connect the board over USB, open the
[installer page](https://yorgof.github.io/cyd-miner/) in Chrome or Edge on a computer
and click Install.

Without such a browser, download `cyd-miner-<version>-factory.bin` from the
[releases page](https://github.com/yorgof/cyd-miner/releases) and write it with
[esptool](https://docs.espressif.com/projects/esptool/):

```sh
esptool.py -p /dev/ttyUSB0 write_flash 0x0 cyd-miner-<version>-factory.bin
```

Either way replaces everything on the board, including settings saved by an earlier
cyd-miner. To be able to go back to the firmware the board shipped with, save it first:

```sh
esptool.py -p /dev/ttyUSB0 -b 230400 read_flash 0 0x400000 stock-backup.bin
esptool.py -p /dev/ttyUSB0 write_flash 0x0 stock-backup.bin   # to restore it
```

## Set up

A board without settings starts a WiFi network of its own and shows its name, such as
`cyd-miner-1A2B`, on the screen.

1. Join that network with a phone. A setup page opens; if it does not, browse to the
   address on the screen, `http://192.168.4.1`.
2. Pick your 2.4 GHz WiFi network and enter its password, a pool, and the Bitcoin
   address a block reward should go to.
3. Save. The board restarts, joins your network and starts mining.

The pool connection is plain Stratum v1 over TCP. A pool with a high minimum
difficulty will never see a share from this device, so use one that honours
`mining.suggest_difficulty`, such as [Public Pool](https://web.public-pool.io), which
is what the page suggests.

To get back to the setup page later, for example after changing your WiFi password,
hold a finger on the screen for five seconds and let go when it says so. The board
goes back to mining when the screen is tapped, or after ten minutes with nobody on
the page. The setup network has no password, so while it is up anyone in range can
open the page.

## The web page

Once the board is on your network, open the address in the top right corner of the
stats screen in a browser. The page shows what the screens show, and has:

- **Settings**: WiFi, pool, address and worker name, the currency for the price, the
  picture turned upside down for a board mounted the other way up, and an optional
  password for the page. Saving restarts the board.
- **Firmware**: upload a `cyd-miner-<version>-ota.bin` from the releases page to
  update. Settings are kept. Mining pauses for the few seconds the upload takes.
  If the new firmware does not start, or does not get as far as joining the network,
  the board goes back to the one it was running at the next restart.

The page is plain HTTP and only answers when addressed by the board's IP address.
Without a password, anyone on your network can change the payout address. With one,
the page asks for it (any user name will do); it crosses your network unencrypted, so
choose one you use for nothing else. A forgotten password can be replaced from the
setup page, which is reached by holding the screen and asks for none.

## Build from source

Install [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/),
then:

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

`idf.py flash` leaves saved settings alone. `build/cyd-miner.bin` is the file the web
page takes as an update. Board pins and the share difficulty to ask the pool for are in
`main/config.h`.

`sdkconfig.defaults` only applies when `sdkconfig` is created. After a change to it,
delete `sdkconfig` and build again.

Pushing a tag such as `v0.2.0` makes `.github/workflows/release.yml` build that
version, attach both files to a GitHub release and publish the installer page.

A flash dump of a board that has been set up contains your WiFi password. The
firmware files do not.

## Test without the board

The hashing and Stratum job code builds on a PC:

```sh
gcc -O2 -Imain -o host_test test/host_test.c main/sha256.c main/work.c
./host_test                                                    # genesis block self-test
python3 test/pool_e2e.py <host> <port> <address> ./host_test   # mine and submit one share
```

## Tuning the hardware loop

The waits in `main/sha_hw_scan.S` come from a bench that runs on the board in place of
the miner:

```sh
idf.py -DSHA_BENCH=1 build && idf.py -p /dev/ttyUSB0 flash monitor
idf.py -DSHA_BENCH=0 build    # back to the miner
```

It runs each loop over the same nonces, checks every hash against software and prints
cycles per nonce and wrong hashes: with core 0 idle, with core 0 loading the buses the
SHA registers share, and next to the rest of the firmware. Loops to compare go in
`main/bench_variants.def`.

The SHA engine shares a bus with the ESP32's AES and big-number engines. TLS running on
those makes the fast loop get hashes wrong during every stats fetch, so
`sdkconfig.defaults` keeps mbedTLS in software.

## Limitations

- No TLS for the pool connection and no version rolling.
- The web page has no TLS either, and firmware uploads are checked for damage but not
  signed.

## License

MIT, see [LICENSE](LICENSE).

`main/font.c` is generated by `tools/psf2c.py` from
[Terminus Font](https://terminus-font.sourceforge.net), which is licensed under the
SIL Open Font License 1.1, see [licenses/Terminus-OFL.txt](licenses/Terminus-OFL.txt).
