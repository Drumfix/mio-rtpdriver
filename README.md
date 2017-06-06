This userspace driver provide an alsa sequencer client for the iConnectivity (TM) MIO/Midi+ devices using the ethernet port.

Some notes about the MIO RTP Implementation:

- Mio fireware bug? It sets the M bit in the RTP Header to 0 instead of 1 and expects all RTP messages 
  to also have the M bit set to zero. This is the oposite of what RFC6295 and the Apple Midi Spec says.
- The Mio requires the host to bind its UDP ports to two consecutive ports, so default binding by an initial sendto is not possible
- After the first 3 sync cycles initiated by the host, the Mio itself initiates 2 sync cycles and accepts midi data only
  after these sync cycles have completed.
  IMO this is not compliant to the Apple Midi Spec, which says sync cycles are initiated by the session initiator.

Ok, now on with the driver.

Compilation requires gcc/make libc-dev and alsa-dev installed

make

sudo make install

The file "mio" will be installed under /usr/bin

TODO:
- periodically try to (re-)open RTP midi session if not connected
- endianess
- add/process timestamps

Usage:

Make sure your iConnectivity (tm) device is switched on and connected to the local LAN.

mio \<alsa client name\> \<mio host address\> \<mio port\> \<local port\></code>

\<alsa client name\> : Name to use for the alsa sequencer client

\<mio host address\> : IPV4 address of the MIO device

\<mio port\>         : Ethernet port used by the mio. This can be set with the iConfig tool (which can be run with wine)

\<local port\>       : Ethernet port used on the Linux computer

Example using all 4 possible ethernet connections (= RTP midi sessions):

mio  MIO-1 192.168.1.100 5004 6100

mio  MIO-2 192.168.1.100 5006 6102

mio  MIO-3 192.168.1.100 5008 6104

mio  MIO-4 192.168.1.100 5010 6108

Note 1: The devices require a few seconds (about 10-15) until they are ready to start transmission/reception of midi messages

Note 2: If you don't know the host address of your MIO device, execute the command 

arp -a

The host address of the MIO will be the one whose MAC address starts with the sequence ac:7a:42



