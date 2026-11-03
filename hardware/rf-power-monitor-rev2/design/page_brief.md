RF-Power-Monitor rev2 is a single board that watches whether 230 VAC mains is present and reports it over Ethernet with SNMP (v1/v2c traps, IF-MIB counters, CPU and ambient temperature). It is powered over Ethernet (IEEE 802.3af) or USB-C.

Rev1 was a carrier for the WIZnet W5500-EVB-Pico-PoE module. Rev2 puts everything from that module except the PoE power module on the board itself:

- Raspberry Pi RP2040 with 2 MB QSPI flash and a 12 MHz crystal (Raspberry Pi Pico core, so the same firmware runs unchanged)
- WIZnet W5500 hardwired TCP/IP Ethernet controller, 25 MHz crystal, HanRun HR601680 1CT:1CT magnetics and a shielded RJ45 without magnetics
- WIZnet WIZPoE-P1 isolated 802.3af PD module (plug-in or soldered), fed from the transformer centre taps (mode A) and the spare pairs (mode B)
- TI TLV62569 1.5 MHz buck from the 5 V rail to 3.3 V, USB-C for the serial configuration menu and flashing
- Isolated mains-presence detector (LTV-814S AC-input optocoupler, 74LVC1G14 Schmitt trigger) with 6.5 mm clearance and 6.4 mm creepage to the low-voltage side
- TMP1075 ambient temperature sensor, mains ON/OFF LEDs, status LED, BOOTSEL, reset and factory-reset buttons

Two copper layers, assembled by JLCPCB (parts with LCSC numbers). Designed with Adom AI Flow in KiCad 10.
