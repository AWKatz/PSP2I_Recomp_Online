# Network HLE audit (Phase 1, Task 1.1)

Every network-related firmware function that PSP2i (NPJH50332) imports, and how
the psprecomp runtime answers it today. The table at the end is generated from
`psprecomp/src/hle/*.c` and `port/gen/recomp_imports.c`; to verify it again,
run `port/tests/check_net_symbols.sh`.

## How an HLE call works

- The generated thunk `psp_import_XXXXXXXX()` calls `psp_hle_call(nid)` (`psprecomp/src/hle/hle.c:152`).
- Handlers are `static void hle_X(void)`. They read MIPS arguments with
  `psp_arg(n)` ($a0–$a3, $t0–$t3), return through `psp_ret(v)` ($v0), and
  touch guest memory with `psp_read*/psp_write*` or `psp_mem_write_block`.
  A pointer argument is a 32-bit PSP address, never a host pointer.
- Handlers are registered with `psp_hle_register(nid, lib, name, fn)`
  (`include/psprecomp/hle.h:36`), all from `psp_hle_init`
  (`hle.c:221-223`: `psp_net_register`, `psp_http_register`, `psp_offline_register`).
- An unregistered NID is logged and returns **0**. The game reads 0 as success.

## Network layer files

| File | Libraries | Behaviour |
|---|---|---|
| `src/hle/net.c` | sceWlanDrv, sceNet, sceNetInet (helpers), sceNetApctl, sceNetResolver, sceNetUpnp, sceUtilityNetconf | Real: answered from the host's own network (`getifaddrs`, `getaddrinfo`) |
| `src/hle/net_sock.c` | sceNetInet sockets (17) | Passed straight through to the host's BSD sockets; errno is mapped to PSP values |
| `src/hle/http.c` | sceHttp, sceSsl | Through the host-supplied `psp_http_transport` |
| `src/hle/offline.c` | sceNetAdhoc, sceNetAdhocctl, sceNetAdhocDiscover, sceNetAdhocMatching, sceNp*, sceNpMatching2, sceUtilityPsn | A table of stubs: start/stop calls return 0, every request returns `SCE_KERNEL_ERROR_ERROR` (0x80020001) |
| `src/net/transport.c` | none (the host seam) | **New**, built only with `ENABLE_MMO_NET`. `psp_net_transport` with a null default |

The client also calls `psp_net_set_forced_off(1)` (`port/host/sdl_main.c:896`),
so `sceWlanGetSwitchState` reads 0 and the game stops at its own "WLAN switch
is off" check before it reaches any ad hoc or Inet call.

## Imports that had no handler (fixed in this task)

These were imported by the game and answered only by the fallback (return 0).
They are now registered in `net.c` as `hle_NetUnmapped`, which logs the call
and returns 0, so behaviour is unchanged. The sceNet/sceNetInet names are confirmed:
`test_hle` checks SHA-1(name) against each NID. The NP ones are unidentified and
registered unnamed.

| Library | NID | Name |
|---|---|---|
| sceNet | 0xCC393E48 | sceNetGetMallocStat |
| sceNetInet | 0x162E6FD5 | sceNetInetGetsockname |
| sceNetInet | 0xE247B6D6 | sceNetInetGetpeername |
| sceNetInet | 0xB3888AD4 | sceNetInetGetTcpcbstat |
| sceNpService | 0x168B8DE5, 0x78802D5F, 0x1DA3E950 | unknown |
| sceNpMatching2 | 0x3892E9A6 | unknown |

(Task 1.1 first registered 0xCC393E48 under a guessed name, sceNetGetLocalEtherAddr.
`test_hle` caught the mismatch, and Task 1.3 corrected it.)

## Ad hoc prototypes (what Task 1.2 must implement)

These are the PSPSDK prototypes (`pspnet_adhoc.h`, `pspnet_adhocctl.h`,
`pspnet_adhocmatching.h`). Today every one returns 0x80020001 except Init/Term.
In the prototypes, pointer arguments are PSP addresses.
`unsigned char *mac` is a 6-byte `SceNetEtherAddr`.

```c
/* sceNetAdhoc: PDP (datagram) and PTP (stream) over MAC addresses */
int sceNetAdhocInit(void);
int sceNetAdhocTerm(void);
int sceNetAdhocPdpCreate(unsigned char *mac, unsigned short port, unsigned int bufsize, int flag); /* -> pdp id */
int sceNetAdhocPdpSend(int id, unsigned char *dstmac, unsigned short dstport, void *data, unsigned int len, unsigned int timeout, int nonblock);
int sceNetAdhocPdpRecv(int id, unsigned char *srcmac, unsigned short *srcport, void *data, int *len /*in/out*/, unsigned int timeout, int nonblock);
int sceNetAdhocPdpDelete(int id, int unknown);
int sceNetAdhocGetPdpStat(int *size /*in/out*/, struct pdpStatStruct *stat);
int sceNetAdhocPtpOpen(unsigned char *srcmac, unsigned short srcport, unsigned char *dstmac, unsigned short dstport,
                       unsigned int bufsize, unsigned int delay, int count, int unk);
int sceNetAdhocPtpListen(unsigned char *srcmac, unsigned short srcport, unsigned int bufsize, unsigned int delay,
                         int count, int queue, int unk);
int sceNetAdhocPtpAccept(int id, unsigned char *mac, unsigned short *port, unsigned int timeout, int nonblock);
int sceNetAdhocPtpConnect(int id, unsigned int timeout, int nonblock);
int sceNetAdhocPtpSend(int id, void *data, int *len /*in/out*/, unsigned int timeout, int nonblock);
int sceNetAdhocPtpRecv(int id, void *data, int *len /*in/out*/, unsigned int timeout, int nonblock);
int sceNetAdhocPtpFlush(int id, unsigned int timeout, int nonblock);
int sceNetAdhocPtpClose(int id, int unk);
int sceNetAdhocGetPtpStat(int *size, struct ptpStatStruct *stat);
int sceNetAdhocSetSocketAlert(int id, int flag);

/* sceNetAdhocctl: the "network" (group) a PSP is in */
int sceNetAdhocctlInit(int stacksize, int priority, struct productStruct *product);
int sceNetAdhocctlTerm(void);
int sceNetAdhocctlAddHandler(sceNetAdhocctlHandler handler, void *arg);   /* -> handler id; handler(event, error, arg) */
int sceNetAdhocctlDelHandler(int id);
int sceNetAdhocctlConnect(const char *name);         /* 8-byte group name */
int sceNetAdhocctlCreate(const char *name);
int sceNetAdhocctlJoin(struct SceNetAdhocctlScanInfo *scaninfo);
int sceNetAdhocctlScan(void);
int sceNetAdhocctlDisconnect(void);
int sceNetAdhocctlGetState(int *event);
int sceNetAdhocctlGetParameter(struct SceNetAdhocctlParams *params);
int sceNetAdhocctlGetAdhocId(struct productStruct *product);
int sceNetAdhocctlGetScanInfo(int *length, void *buf);
int sceNetAdhocctlGetPeerList(int *length, void *buf);
int sceNetAdhocctlGetPeerInfo(unsigned char *mac, int size, struct SceNetAdhocctlPeerInfo *peerinfo);
int sceNetAdhocctlGetNameByAddr(unsigned char *mac, char *nickname);

/* sceNetAdhocMatching: host/client pairing on top of PDP */
int sceNetAdhocMatchingInit(int memsize);
int sceNetAdhocMatchingTerm(void);
int sceNetAdhocMatchingCreate(int mode, int maxpeers, unsigned short port, int bufsize, unsigned int hellodelay,
                              unsigned int pingdelay, int initcount, unsigned int msgdelay,
                              pspAdhocMatchingCallback callback);   /* -> matching id */
int sceNetAdhocMatchingStart(int matchingid, int evthpri, int evthstack, int inthpri, int inthstack, int optlen, void *optdata);
int sceNetAdhocMatchingStop(int matchingid);
int sceNetAdhocMatchingDelete(int matchingid);
int sceNetAdhocMatchingSelectTarget(int matchingid, unsigned char *mac, int optlen, void *optdata);
int sceNetAdhocMatchingCancelTarget(int matchingid, unsigned char *mac);
int sceNetAdhocMatchingCancelTargetWithOpt(int matchingid, unsigned char *mac, int optlen, void *optdata);
int sceNetAdhocMatchingSendData(int matchingid, unsigned char *mac, int datalen, void *data);
int sceNetAdhocMatchingAbortSendData(int matchingid, unsigned char *mac);
int sceNetAdhocMatchingSetHelloOpt(int matchingid, int optlen, void *optdata);
int sceNetAdhocMatchingGetHelloOpt(int matchingid, int *optlen, void *optdata);
```

Callbacks (the Adhocctl handler and the Matching callback) are guest functions.
They must be invoked on a game thread through the scheduler, not from a host
network thread.

## The transport seam (`include/psprecomp/net_transport.h`)

The roadmap asks for an `INetworkTransport` C++ interface. The codebase is C11,
so this is the C equivalent, shaped like `psp_http_transport`: a struct of
function pointers that the host installs with `psp_net_set_transport()`.

| Roadmap method | C member |
|---|---|
| Initialize | `int initialize(void *ctx, const uint8_t self_mac[6])` |
| HostSession | `int host_session(void *ctx, const char *group, int max_players)` |
| JoinSession | `int join_session(void *ctx, const char *address, const char *group)` |
| SendPacket | `int send_packet(void *ctx, const psp_net_peer *to, int channel, uint16_t port, const void *data, uint32_t len)` |
| PollPackets | `int poll_packets(void *ctx, psp_net_packet_fn fn, void *user)` |
| Shutdown | `void shutdown(void *ctx)` |

Each function takes the transport's `ctx` (the instance), so several transports
can live in one process (a LAN host that also plays, and the tests).

### ENet implementation (Task 1.2): `include/psprecomp/net_enet.h`

`psp_net_enet_create(cfg)` returns an instance; `psp_net_enet_transport(e)` is its
`psp_net_transport`. The topology is a star: the host assigns ids (host = 1,
clients 2, 3, ...) after a HELLO/WELCOME exchange and relays client-to-client
packets. The host stamps every relayed packet's source from the connection, so
a client cannot impersonate another MAC or id. Channel 0 is ENet reliable
sequenced; channel 1 is ENet unreliable sequenced. ENet v1.3.18 is fetched by
CMake FetchContent and linked statically. Test: `psprecomp/tests/test_enet_transport.c`.

`psp_net_peer` = `{ uint8_t mac[6]; uint64_t id; }`, which maps the game's MAC to a
server-assigned id. Channel 0 is reliable and channel 1 is unreliable (sequenced).
Without an installed transport, the null transport returns `PSP_NET_ERR_UNSUPPORTED`.
The ad hoc handlers in `offline.c` are not wired to it yet; that is Task 1.2.

## Build flag

`-DENABLE_MMO_NET=ON` (psprecomp option, default OFF, propagated through the
CMake cache from `port/macos` or `port/`) adds `src/net/transport.c`, defines
`PSPRECOMP_MMO_NET=1` and builds `test_net_transport`. When it is OFF, the
runtime's source list is the same as before.

## Full table: network HLE registrations

"Imported" = present in PSP2i's import table.

| Library | Function | NID | Handler | File | Imported |
|---|---|---|---|---|---|
| sceHttp | `sceHttpCreateConnection` | `0x8EEFD953` | hle_CreateConnection | http.c | yes |
| sceHttp | `sceHttpCreateRequest` | `0x47347B50` | hle_CreateRequest | http.c | yes |
| sceHttp | `sceHttpCreateTemplate` | `0x9B1F1F36` | hle_CreateTemplate | http.c | yes |
| sceHttp | `sceHttpDeleteConnection` | `0x5152773B` | hle_DeleteConnection | http.c | yes |
| sceHttp | `sceHttpDeleteRequest` | `0xA5512E01` | hle_DeleteRequest | http.c | yes |
| sceHttp | `sceHttpDisableAuth` | `0xAE948FEE` | hle_DisableAuth | http.c | yes |
| sceHttp | `sceHttpDisableCookie` | `0x0B12ABFB` | hle_DisableCookie | http.c | yes |
| sceHttp | `sceHttpEnableRedirect` | `0x0809C831` | hle_EnableRedirect | http.c | yes |
| sceHttp | `sceHttpEnd` | `0xD1C8945E` | hle_HttpEnd | http.c | yes |
| sceHttp | `sceHttpGetContentLength` | `0x0282A3BD` | hle_GetContentLength | http.c | yes |
| sceHttp | `sceHttpGetStatusCode` | `0x4CC7D78F` | hle_GetStatusCode | http.c | yes |
| sceHttp | `sceHttpInit` | `0xAB1ABE07` | hle_HttpInit | http.c | yes |
| sceHttp | `sceHttpReadData` | `0xEDEEB999` | hle_ReadData | http.c | yes |
| sceHttp | `sceHttpSendRequest` | `0xBB70706F` | hle_SendRequest | http.c | yes |
| sceHttp | `sceHttpSetConnectTimeOut` | `0x8ACD1F73` | hle_SetConnectTimeOut | http.c | yes |
| sceHttp | `sceHttpSetRecvTimeOut` | `0x1F0FC3E3` | hle_SetRecvTimeOut | http.c | yes |
| sceHttp | `sceHttpSetResolveRetry` | `0x03D9526F` | hle_SetResolveRetry | http.c | yes |
| sceHttp | `sceHttpSetResolveTimeOut` | `0x47940436` | hle_SetResolveTimeOut | http.c | yes |
| sceHttp | `sceHttpSetSendTimeOut` | `0x9988172D` | hle_SetSendTimeOut | http.c | yes |
| sceHttp | `sceHttpsDisableOption` | `0xB3FAF831` | hle_HttpsDisableOption | http.c | yes |
| sceHttp | `sceHttpsEnableOption` | `0xBAC31BF1` | hle_HttpsEnableOption | http.c | yes |
| sceHttp | `sceHttpsEnd` | `0xF9D8EB63` | hle_HttpsEnd | http.c | yes |
| sceHttp | `sceHttpsInit` | `0xE4D21302` | hle_HttpsInit | http.c | yes |
| sceNet | `sceNetEtherNtostr` | `0x89360950` | hle_NetEtherNtostr | net.c | yes |
| sceNet | `sceNetEtherStrton` | `0xD27961C9` | hle_NetEtherStrton | net.c | yes |
| sceNet | `sceNetFreeThreadinfo` | `0x50647530` | hle_NetFreeThreadinfo | net.c | yes |
| sceNet | `sceNetGetMallocStat` | `0xCC393E48` | hle_NetUnmapped | net.c | yes |
| sceNet | `sceNetInit` | `0x39AF39A6` | hle_NetInit | net.c | yes |
| sceNet | `sceNetTerm` | `0x281928A9` | hle_NetTerm | net.c | yes |
| sceNetAdhoc | `sceNetAdhocGetPdpStat` | `0xC7C1FC57` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocGetPtpStat` | `0xB9685118` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocInit` | `0xE1D621D7` | ok (returns 0) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPdpCreate` | `0x6F92741B` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPdpDelete` | `0x7F27BB5E` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPdpRecv` | `0xDFE53E03` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPdpSend` | `0xABED3790` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPtpAccept` | `0x9DF81198` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPtpClose` | `0x157E6225` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPtpConnect` | `0xFC6FC07B` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPtpFlush` | `0x9AC2EEAC` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPtpListen` | `0xE08BDAC1` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPtpOpen` | `0x877F6D66` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPtpRecv` | `0x8BEA2B3E` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocPtpSend` | `0x4DA4C788` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocSetSocketAlert` | `0x73BFD52D` | fail (0x80020001) | offline.c | yes |
| sceNetAdhoc | `sceNetAdhocTerm` | `0xA62C6F57` | ok (returns 0) | offline.c | yes |
| sceNetAdhocDiscover | `sceNetAdhocDiscoverGetStatus` | `0x944DDBC6` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocDiscover | `sceNetAdhocDiscoverInitStart` | `0x941B3877` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocDiscover | `sceNetAdhocDiscoverRequestSuspend` | `0xA423A21B` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocDiscover | `sceNetAdhocDiscoverStop` | `0xF7D13214` | ok (returns 0) | offline.c | yes |
| sceNetAdhocDiscover | `sceNetAdhocDiscoverTerm` | `0xA2246614` | ok (returns 0) | offline.c | yes |
| sceNetAdhocDiscover | `sceNetAdhocDiscoverUpdate` | `0x52DE1B97` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingAbortSendData` | `0xEC19337D` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingCancelTargetWithOpt` | `0x8F58BEDF` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingCancelTarget` | `0xEA3C6108` | fail (0x80020001) | offline.c |  |
| sceNetAdhocMatching | `sceNetAdhocMatchingCreate` | `0xCA5EDA6F` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingDelete` | `0xF16EAF4F` | ok (returns 0) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingGetHelloOpt` | `0xB5D96C2A` | fail (0x80020001) | offline.c |  |
| sceNetAdhocMatching | `sceNetAdhocMatchingInit` | `0x2A2A1E07` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingSelectTarget` | `0x5E3D4B79` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingSendData` | `0xF79472D7` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingSetHelloOpt` | `0xB58E61B7` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingStart` | `0x93EF3843` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingStop` | `0x32B156B3` | ok (returns 0) | offline.c | yes |
| sceNetAdhocMatching | `sceNetAdhocMatchingTerm` | `0x7945ECDA` | ok (returns 0) | offline.c | yes |
| sceNetAdhocctl | `sceNetAdhocctlAddHandler` | `0x20B317A0` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocctl | `sceNetAdhocctlConnect` | `0x0AD043ED` | fail (0x80020001) | offline.c |  |
| sceNetAdhocctl | `sceNetAdhocctlCreate` | `0xEC0635C1` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocctl | `sceNetAdhocctlDelHandler` | `0x6402490B` | ok (returns 0) | offline.c | yes |
| sceNetAdhocctl | `sceNetAdhocctlDisconnect` | `0x34401D65` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocctl | `sceNetAdhocctlGetAdhocId` | `0x362CBE8F` | fail (0x80020001) | offline.c |  |
| sceNetAdhocctl | `sceNetAdhocctlGetNameByAddr` | `0x8916C003` | fail (0x80020001) | offline.c |  |
| sceNetAdhocctl | `sceNetAdhocctlGetParameter` | `0xDED9D28E` | fail (0x80020001) | offline.c |  |
| sceNetAdhocctl | `sceNetAdhocctlGetPeerInfo` | `0x8DB83FDC` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocctl | `sceNetAdhocctlGetPeerList` | `0xE162CB14` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocctl | `sceNetAdhocctlGetScanInfo` | `0x81AEE1BE` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocctl | `sceNetAdhocctlGetState` | `0x75ECD386` | fail (0x80020001) | offline.c |  |
| sceNetAdhocctl | `sceNetAdhocctlInit` | `0xE26F226E` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocctl | `sceNetAdhocctlJoin` | `0x5E7F79C9` | fail (0x80020001) | offline.c | yes |
| sceNetAdhocctl | `sceNetAdhocctlScan` | `0x08FFF7A0` | fail (0x80020001) | offline.c |  |
| sceNetAdhocctl | `sceNetAdhocctlTerm` | `0x9D689E13` | ok (returns 0) | offline.c | yes |
| sceNetApctl | `sceNetApctlAddHandler` | `0x8ABADD51` | hle_ApctlAddHandler | net.c | yes |
| sceNetApctl | `sceNetApctlDelHandler` | `0x5963991B` | hle_ApctlDelHandler | net.c | yes |
| sceNetApctl | `sceNetApctlDisconnect` | `0x24FE91A1` | hle_ApctlDisconnect | net.c | yes |
| sceNetApctl | `sceNetApctlGetInfo` | `0x2BEFDF23` | hle_ApctlGetInfo | net.c | yes |
| sceNetApctl | `sceNetApctlInit` | `0xE2F91F9B` | hle_ApctlInit | net.c | yes |
| sceNetApctl | `sceNetApctlTerm` | `0xB3EDD0EC` | hle_ApctlTerm | net.c | yes |
| sceNetInet | `sceNetInetAccept` | `0xDB094E1B` | hle_Accept | net_sock.c | yes |
| sceNetInet | `sceNetInetBind` | `0x1A33F9AE` | hle_Bind | net_sock.c | yes |
| sceNetInet | `sceNetInetClose` | `0x8D7284EA` | hle_Close | net_sock.c | yes |
| sceNetInet | `sceNetInetConnect` | `0x410B34AA` | hle_Connect | net_sock.c | yes |
| sceNetInet | `sceNetInetGetErrno` | `0xFBABE411` | hle_GetErrno | net_sock.c |  |
| sceNetInet | `sceNetInetGetPspError` | `0x8CA3A97E` | hle_GetPspError | net_sock.c | yes |
| sceNetInet | `sceNetInetGetTcpcbstat` | `0xB3888AD4` | hle_NetUnmapped | net.c | yes |
| sceNetInet | `sceNetInetGetpeername` | `0xE247B6D6` | hle_NetUnmapped | net.c | yes |
| sceNetInet | `sceNetInetGetsockname` | `0x162E6FD5` | hle_NetUnmapped | net.c | yes |
| sceNetInet | `sceNetInetGetsockopt` | `0x4A114C7C` | hle_Getsockopt | net_sock.c | yes |
| sceNetInet | `sceNetInetInetAddr` | `0xB75D5B0A` | hle_InetInetAddr | net.c | yes |
| sceNetInet | `sceNetInetInetNtop` | `0xD0792666` | hle_InetInetNtop | net.c | yes |
| sceNetInet | `sceNetInetInit` | `0x17943399` | hle_InetInit | net.c | yes |
| sceNetInet | `sceNetInetListen` | `0xD10A1A7A` | hle_Listen | net_sock.c | yes |
| sceNetInet | `sceNetInetRecv` | `0xCDA85C99` | hle_Recv | net_sock.c | yes |
| sceNetInet | `sceNetInetRecvfrom` | `0xC91142E4` | hle_Recvfrom | net_sock.c | yes |
| sceNetInet | `sceNetInetSelect` | `0x5BE8D595` | hle_Select | net_sock.c | yes |
| sceNetInet | `sceNetInetSend` | `0x7AA671BC` | hle_Send | net_sock.c | yes |
| sceNetInet | `sceNetInetSendto` | `0x05038FC7` | hle_Sendto | net_sock.c | yes |
| sceNetInet | `sceNetInetSetsockopt` | `0x2FE71FE7` | hle_Setsockopt | net_sock.c | yes |
| sceNetInet | `sceNetInetShutdown` | `0x4CFE4E56` | hle_Shutdown | net_sock.c | yes |
| sceNetInet | `sceNetInetSocketAbort` | `0x80A21ABD` | hle_SocketAbort | net_sock.c | yes |
| sceNetInet | `sceNetInetSocket` | `0x8B7B220F` | hle_Socket | net_sock.c | yes |
| sceNetInet | `sceNetInetTerm` | `0xA9ED66B9` | hle_InetTerm | net.c | yes |
| sceNetResolver | `sceNetResolverCreate` | `0x244172AF` | hle_ResolverCreate | net.c | yes |
| sceNetResolver | `sceNetResolverDelete` | `0x94523E09` | hle_ResolverDelete | net.c | yes |
| sceNetResolver | `sceNetResolverInit` | `0xF3370E61` | hle_ResolverInit | net.c | yes |
| sceNetResolver | `sceNetResolverStartNtoA` | `0x224C5F44` | hle_ResolverStartNtoA | net.c | yes |
| sceNetResolver | `sceNetResolverTerm` | `0x6138194A` | hle_ResolverTerm | net.c | yes |
| sceNetUpnp | `(unnamed)` | `0x27045362` | hle_UpnpGetNatInfo | net.c | yes |
| sceNetUpnp | `(unnamed)` | `0x3432B2E5` | hle_UpnpOk | net.c | yes |
| sceNetUpnp | `(unnamed)` | `0x3E32ED9E` | hle_UpnpOk | net.c | yes |
| sceNetUpnp | `(unnamed)` | `0x540491EF` | hle_UpnpOk | net.c | yes |
| sceNetUpnp | `(unnamed)` | `0xE24220B5` | hle_UpnpOk | net.c | yes |
| sceNp | `(unnamed)` | `0x1D60AE4B` | fail (0x80020001) | offline.c | yes |
| sceNp | `(unnamed)` | `0x37E1E274` | ok (returns 0) | offline.c | yes |
| sceNp | `(unnamed)` | `0x633B5F71` | fail (0x80020001) | offline.c | yes |
| sceNp | `(unnamed)` | `0x857B47D3` | ok (returns 0) | offline.c | yes |
| sceNp | `(unnamed)` | `0xBB069A87` | fail (0x80020001) | offline.c | yes |
| sceNpAuth | `(unnamed)` | `0x3F1C1F70` | fail (0x80020001) | offline.c | yes |
| sceNpAuth | `(unnamed)` | `0x4EC1F667` | ok (returns 0) | offline.c | yes |
| sceNpAuth | `(unnamed)` | `0x72BB0467` | fail (0x80020001) | offline.c | yes |
| sceNpAuth | `(unnamed)` | `0xA1DE86F8` | ok (returns 0) | offline.c | yes |
| sceNpAuth | `(unnamed)` | `0xCD86A656` | fail (0x80020001) | offline.c | yes |
| sceNpAuth | `(unnamed)` | `0xD99455DD` | fail (0x80020001) | offline.c | yes |
| sceNpCommerce2 | `(unnamed)` | `0x0E9956E3` | ok (returns 0) | offline.c | yes |
| sceNpCommerce2 | `(unnamed)` | `0xA5A34EA4` | ok (returns 0) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x12C5A111` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x190FF903` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x2B3892FC` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x2E61F6E1` | ok (returns 0) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x3892E9A6` | hle_NetUnmapped | net.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x3DE70241` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x4EE3A8EC` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x5030CC53` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x6D6D0C75` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x7BBFC427` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x81C13E6D` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x8BF37D8C` | ok (returns 0) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0x97529ECC` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0xA3C298D1` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0xA53E7C69` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0xAAD0946A` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0xC870535A` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0xC8FC5D41` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0xD7D4AEB2` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0xE6C93DBD` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0xF47342FC` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0xF940D9AD` | fail (0x80020001) | offline.c | yes |
| sceNpMatching2 | `(unnamed)` | `0xFADBA9DB` | fail (0x80020001) | offline.c | yes |
| sceNpService | `(unnamed)` | `0x00ACFAC3` | ok (returns 0) | offline.c | yes |
| sceNpService | `(unnamed)` | `0x0F8F5821` | ok (returns 0) | offline.c | yes |
| sceNpService | `(unnamed)` | `0x168B8DE5` | hle_NetUnmapped | net.c | yes |
| sceNpService | `(unnamed)` | `0x1DA3E950` | hle_NetUnmapped | net.c | yes |
| sceNpService | `(unnamed)` | `0x78802D5F` | hle_NetUnmapped | net.c | yes |
| scePower | `(unnamed)` | `0xA85880D0` | hle_PowerCheckWlanCoexistenceClock | net.c | yes |
| sceSsl | `sceSslEnd` | `0x191CDEFF` | hle_SslEnd | http.c | yes |
| sceSsl | `sceSslInit` | `0x957ECBE2` | hle_SslInit | http.c | yes |
| sceUtility | `sceUtilityNetconfGetStatus` | `0x6332AA39` | hle_NetconfGetStatus | net.c | yes |
| sceUtility | `sceUtilityNetconfInitStart` | `0x4DB1E739` | hle_NetconfInitStart | net.c | yes |
| sceUtility | `sceUtilityNetconfShutdownStart` | `0xF88155F6` | hle_NetconfShutdownStart | net.c | yes |
| sceUtility | `sceUtilityNetconfUpdate` | `0x91E70E35` | hle_NetconfUpdate | net.c | yes |
| sceUtility | `sceUtilityPsnGetStatus` | `0x094198B8` | fail (0x80020001) | offline.c | yes |
| sceUtility | `sceUtilityPsnInitStart` | `0xA7BB7C67` | fail (0x80020001) | offline.c | yes |
| sceUtility | `sceUtilityPsnShutdownStart` | `0xC130D441` | fail (0x80020001) | offline.c | yes |
| sceUtility | `sceUtilityPsnUpdate` | `0x0940A1B9` | fail (0x80020001) | offline.c | yes |
| sceWlanDrv | `sceWlanGetEtherAddr` | `0x0C622081` | hle_WlanGetEtherAddr | net.c | yes |
| sceWlanDrv | `sceWlanGetSwitchState` | `0xD7763699` | hle_WlanGetSwitchState | net.c | yes |


## MMO client entry (Task 1.3)

In the MMO build only:

- `port/host/mmo_client.c` reads `client_config.toml` from the data folder
  (`server_ip`, `server_port`, `lan_discovery`, `player_name`; see
  `docs/client_config.example.toml`), installs the ENet transport, clears the
  forced-offline flag and forces the WLAN switch on (`psp_net_set_wlan_forced_on`).
- `psprecomp/src/net/adhoc_mmo.c` replaces every stub in `offline.c` (ad hoc,
  NP, PSN dialog) with a traced version that logs the name and arguments and
  keeps the same result. The exception is `sceNetAdhocctlInit`: it joins the
  server and sends `MSG_C2S_HANDSHAKE` (`include/psprecomp/net_proto.h`). The
  server's `MSG_S2C_HANDSHAKE_ACK` is read by `psp_net_mmo_pump`, which runs every frame.
- DNAS is not involved: PSP2i's ad hoc mode does not import it.
- Checked by `psprecomp/tests/test_mmo_handshake.c`, which goes through the real
  dispatcher. `tests/mmo_listen` is a stand-in server for checks in the live game.

### Checked in the live game (2026-11-22)

The setup was the MMO build, a PPSSPP save loaded through `port/host/savecrypt.c`, and `mmo_listen 29001` running locally.

1. The mode select showed **WLAN/WIRELESS: ON**.
2. Choosing **Multi Mode** made the game call the following, in order:

   ```
   sceNetInit
   sceNetAdhocInit(0x60000, 0x28, 0)                          -> 0
   sceNetAdhocctlInit(stack 0x1000, prio 48, product 'NPJH50332')
     -> joined 127.0.0.1:29001, sent MSG_C2S_HANDSHAKE (78 bytes)
   sceNetAdhocctlAddHandler(0x08B87BF0, 0, 0x08EA16EC)        -> 0x80020001 (not implemented)
   MSG_S2C_HANDSHAKE_ACK status 0, session 2
   ```

3. `mmo_listen` logged `MSG_C2S_HANDSHAKE from id 2 ... name 'Tester'`.
4. The game reached the Matching Counter (Form Party / Join Party).

`sceNetAdhocctlAddHandler` is the first call that isn't implemented yet. The game registers its ad hoc event callback there. Implementing it, together with the connect, scan and matching calls behind Form Party and Join Party, is the next piece of client-side ad hoc work.

Each network log line appears twice in `logs/psp2i_log.txt`. That's a cosmetic issue in the host's log sink, not two separate calls.
