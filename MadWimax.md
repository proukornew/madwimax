= About the project =

[MadWimax](MadWimax.md) is a reverse-engineered linux driver for Mobile Wimax (802.16e) devices based on Samsung CMC-730 chip. These devices are currently supported:

  * [Samsung SWC-U200](SwcU200.md)

  * [Samsung SWC-E100](SwcE100.md)

  * [Samsung SWM-S10R](SwmS10R.md) (built in [Samsung NC-10](Nc10.md) netbook)

Project is named after [MadWifi](http://madwifi-project.org/wiki).

The driver is completely user-space. There are a few reasons for that:

  * portability

> It should run gracefully on every kernel from 2.4. It can be ported to other platforms with some effort.

  * ease of development

> Debugging is easy in user-space.

  * security

> You cannot crash kernel due to a bug in the user-space code (well, it's supposed to be this way :) ).

There is also a [project](http://git.altlinux.org/people/silicium/packages/kernel-source-u200.git) to port [MadWimax](MadWimax.md) to Linux kernel.

The driver uses [libusb-1.0 library](http://libusb.wiki.sourceforge.net/Libusb1.0) to communicate to the device. This library helps a lot to write a user-space USB driver and also supports the required asynchronous transfers. Unfortunately it works under Linux only at the moment. Mac OS X port is ongoing.

Linux TUN/TAP driver is used for user-space Ethernet interface.

![http://madwimax.googlecode.com/svn/wiki/block.png](http://madwimax.googlecode.com/svn/wiki/block.png)

# Users #

[Installation](Installation.md)

# Developers #

[USB protocol info](UsbProtocol.md)

[Reverse-engineering](ReverseEngineering.md)