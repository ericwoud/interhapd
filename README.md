# interhapd - Inter-connecting hostapd daemon

Daemon to use together with 802.11r, 802.11k and 802.11v.

The deamon is still in Alpa fase, be sure to encounter bugs.

The program is split in two parts:

* A systemd daemon, which runs the script and comunicates to the network and hostapd interfaces.
* A python3 script. It talks to the daemon through stdin/stdout. This way any programming language could be used. Debug messages are printed on stderr.

It needs to run on every AP / Router (host) on your wifi network. AP's and Router are connected on one (layer 2) lan. This is used for hostapd's 802.11r Roaming.

What does it do?

* Communicates to other interhapd running on other hosts on the network.
* Send hostapd command to any interface on any host from script.
* Receive all events from hostapd from any interface from any host in script.
* A web-interface on port 11112.
* It builds the neighborhood (rrm_neighbor_report SET_NEIGHBOR) on every wifi interface.
* Keeps track of all stations on the entire network.


TODO:

* BSS_TM_REQ : Move station to another AP in the neighborhood
* REQ_BEACON : Get wifi signal strenght from stations

## Getting Started with interhapd

You need to build the program from source.

### Prerequisites

Hostapd needs to be installed. It needs to support the command `SHOW_NEIGHBOR`. This means for now you need to build hostapd from git. The standard version in Debian/Ubuntu/Archlinux is too old. Any 2.9 version is too old. In Archlinux you could install hostapd-git, but you need to have CONFIG_WNM enabled, which is not...

In the hostapd configuration there has to be the following statement, also after every bss= line:
```
ctrl_interface=/var/run/hostapd
```
Every host needs a unique hostname.


### Installing

Install necessary library. Libsystemd is only used for sending 2 messages to systemd, so the service can be of Type=notify. Remove '#define USE_SYSTEMD 1' from source and '-lsystemd' from Makefile if you do not want to use libsystemd.

On Debian/Ubuntu do: (Arch-linux not needed)
```
sudo apt install libsystemd-dev
```

Clone from Git

```
git clone https://github.com/ericwoud/interhapd.git
```

Change directory

```
cd interhapd
```

Now build the executable.

```
make
```

Test the program with:
```
./interhapd
```
End with CTRL-Z

Not recommended yet in Alpha version:
On Debian/Ubuntu/Arch-linux you can use the following to copy the files to the needed locations and start and enable the systemd service. You may need to use sudo if not logged on as root.

```
sudo make install
```

Edit the /etc/default/interhapd file. At least make sure that the name of the inter-connecting bridge is correct. Defaults to brlan.


Other make options:


Remove the installation:
```
sudo make remove
```


Clean:
```
make clean
```


## Features

The command line options are:

* -d number      : debug information 0 = none, 1 = some (default), 2 = all.
* -p number      : port number to use for internal network communication (UDP).
* -s script      : location of optional bash script that listens if some client connected on any bridge on your network (default = ./interhapd.sh). If not used at all it can be removed.
* -h path        ; hostapd ctrl_interface to use (default = /run/hostapd).
* -l             ; legacy option to clear fdb, see my repo [bridgefdbd](https://github.com/ericwoud/bridgefdbd). Not working anymore since kernel 5.15. 

After the options names of the interfaces to listen on are stated (default = brlan).

On the commandline you can use CTRL-Z to make a clean exit, CTRL-C for a quick and dirty exit.

