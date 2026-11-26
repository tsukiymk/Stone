# Networking and local communication

Android networking combines image Java/native APIs with Stone's Windows socket bridges. Service samples host connectivity, wireless, and interface information in the background. Client publishes asynchronous snapshots without running Windows queries on the Android UI thread.

## Connectivity and network capabilities

Stone provides a host-managed virtual default network, reporting connectivity, Internet access, and metered state from snapshots. Android matches the image NetworkRequest's implicit constraints and filters. Default and filtered requests maintain separate callbacks; constraints are not ignored to report success.

Windows WLAN, IPv4 configuration, interface names/indexes, IPv4/IPv6 addresses, prefixes, scope, and MTU come from actual host information. Native getifaddrs, read-only ioctl, and image Java NetworkInterface use consistent snapshots. Unrepresentable values return explicit errors. Unavailable sampling is not presented as an empty list.

WifiManager tokens, multicast-lock reference counts, and broadcasts remain in ART. Hotspot control, Wi-Fi Direct, Bluetooth, and full Android network management are not connected.

## TCP, UDP, and local sockets

Native TCP uses Windows bridging. UDP supports the corresponding initialization and multicast options, group membership, and datagram send/receive. Full IPv6 multicast, receive-all-groups mode, ancillary control messages, and network switching remain limited.

On Windows x64, AF_UNIX/STREAM supports length-delimited abstract names, exclusive cross-process binding, listening, connections from unbound clients, and byte transfer. Session-local name reservations and actual Windows AF_UNIX nodes jointly manage lifetime. Final close removes reservations; crashes may leave temporary nodes that do not prevent rebinding.

Pathname sockets, connections from bound clients, descriptor/credential passing on named connections, and the corresponding sendmsg/recvmsg operations are not fully supported. The dedicated socketpair transport is managed separately and does not establish support for all ordinary local listening protocols.

## Errors and encryption

strerror, strerror_r, and the GNU variant use Bionic's Android numbers and descriptions rather than the Windows errno table. Unknown-error buffers are isolated per thread; bounds and errno follow each interface's semantics.

Image preparation includes system CAs. ART Java cryptography uses image Conscrypt. Certificate chains, handshakes, site compatibility, and application networking depend on the actual execution path. Extracted CAs or a working cryptographic API do not establish that arbitrary HTTPS requests work.

Network interface enumeration, multicast discovery, HTTP file transfers, and complete application exchanges are distinct capabilities. See [Windows integration](../windows/integration.md) for web Intents handled by the host browser.
