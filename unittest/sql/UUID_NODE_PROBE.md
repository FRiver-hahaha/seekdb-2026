# Production UUID node initialization probe

This Linux component regression compiles the same `ObUUIDNode` constructor and
`init()` body included by `ob_expr_uuid.cpp`. Project error codes, utility macros
and memory copies remain real; only the logging sink is disabled. Linker
wrappers inject errors at OS boundaries, including fortified `__read_chk`.
Real modes bypass all fixtures and call the actual kernel interfaces.

Run from the repository root on Linux or WSL with Python 3, glibc and g++ (or
invoke the script by its absolute path from another directory):

```sh
python3 unittest/sql/run_uuid_node_probe.py
python3 unittest/sql/run_uuid_node_probe.py --sanitize
python3 unittest/sql/run_uuid_node_probe.py --real
python3 unittest/sql/run_uuid_node_probe.py --real-permission
python3 unittest/sql/run_uuid_node_probe.py --real-device
```

The runner pins itself, the compiler and the probe to one allowed CPU. It builds
only this component into `build_bugfix/uuid_node_probe`, starts no database and
uses no container. It is an explicit standalone probe, not an addition to the
normal full-build test target.

Default coverage includes valid Ethernet MAC preservation, socket/ioctl EPERM,
empty/loopback-only results, continuation after failed interfaces or TUN,
zero/multicast address rejection, synthetic node bytes/multicast bit,
getrandom interruptions and short results, ENOSYS/EPERM compatibility fallback,
interrupted open, interrupted/short device reads, EOF, persistent/partial entropy
failure, descriptor closure, failure without partial publication, constructor
failure, repeated failure and recovery. The MAC path also succeeds with broken
entropy. This probes sequential reinitialization, not concurrent initialization.

`--real-permission` installs a seccomp filter in the probe process that denies
discovery sockets with EPERM. `--real-device` also denies getrandom, exercising
the actual `/dev/urandom` compatibility path. Both assert a complete synthetic
node and require Linux seccomp support; the parent process is unaffected.

A real Linux TUN reproduction can run in a temporary child network namespace
(requires root, `unshare`, `ip` and TUN support):

```sh
# Compile first with the runner, then use the absolute binary path below.
taskset -c 0 unshare --net -- bash -c '
  set -e
  ip link set lo up
  ip tuntap add dev uuidtun mode tun
  ip addr add 192.0.2.1/32 dev uuidtun
  ip link set uuidtun up
  ip -d link show uuidtun
  /absolute/path/to/seekdb/build_bugfix/uuid_node_probe --real --expect-random
'
```

Choose an allowed CPU if CPU 0 is unavailable. The namespace and its interface
disappear when the child exits. The host namespace is not modified. Before the
fix TUN is accepted as an all-zero MAC; after the fix initialization returns a
synthetic node. Omit the four `ip tuntap`/address/link inspection commands to
test a loopback-only namespace: before the fix init returns `OB_ERR_UNEXPECTED`,
after the fix it uses a synthetic node. A normal host keeps its Ethernet MAC.

This fix scopes fallback changes to Linux. Android and the other platform
initialization branches are unchanged. UUID timestamp, clock sequence, version,
variant, formatting and generator synchronization are unchanged. No once-only
cache is introduced: startup owns the long-lived node; the temporary
`gen_server_uuid()` allocation keeps its existing independent lifecycle.

The probe does not validate SQL UUID formatting, global generator concurrency,
post-initialization fork behavior, other platform branches, full expression
builds or database readiness. Use the normal full build and SQL integration
checks when those resources are available.
