// Copyright 2018 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package cmd

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math/rand"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/google/subcommands"
	specs "github.com/opencontainers/runtime-spec/specs-go"
	"golang.org/x/sys/unix"
	"gvisor.dev/gvisor/pkg/log"
	"gvisor.dev/gvisor/runsc/cmd/util"
	"gvisor.dev/gvisor/runsc/config"
	"gvisor.dev/gvisor/runsc/console"
	"gvisor.dev/gvisor/runsc/container"
	"gvisor.dev/gvisor/runsc/flag"
	"gvisor.dev/gvisor/runsc/specutils"
)

var errNoDefaultInterface = errors.New("no default interface found")

// Do implements subcommands.Command for the "do" command. It sets up a simple
// sandbox and executes the command inside it. See Usage() for more details.
type Do struct {
	root    string
	cwd     string
	erofs   string
	ip      string
	quiet   bool
	overlay bool
	uidMap  idMapSlice
	gidMap  idMapSlice
	volumes volumes
}

// Name implements subcommands.Command.Name.
func (*Do) Name() string {
	return "do"
}

// Synopsis implements subcommands.Command.Synopsis.
func (*Do) Synopsis() string {
	return "Simplistic way to execute a command inside the sandbox. It's to be used for testing only."
}

// Usage implements subcommands.Command.Usage.
func (*Do) Usage() string {
	return `do [flags] <cmd> - runs a command.

This command starts a sandbox with host filesystem mounted inside as readonly,
with a writable tmpfs overlay on top of it. The given command is executed inside
the sandbox. It's to be used to quickly test applications without having to
install or run docker. It doesn't give nearly as many options and it's to be
used for testing only.
`
}

type idMapSlice []specs.LinuxIDMapping

// String implements flag.Value.String.
func (is *idMapSlice) String() string {
	idMappings := make([]string, 0, len(*is))
	for _, m := range *is {
		idMappings = append(idMappings, fmt.Sprintf("%d %d %d", m.ContainerID, m.HostID, m.Size))
	}
	return strings.Join(idMappings, ",")
}

// Get implements flag.Value.Get.
func (is *idMapSlice) Get() any {
	return is
}

// Set implements flag.Value.Set. Set(String()) should be idempotent.
func (is *idMapSlice) Set(s string) error {
	for _, idMap := range strings.Split(s, ",") {
		fs := strings.Fields(idMap)
		if len(fs) != 3 {
			return fmt.Errorf("invalid mapping: %s", idMap)
		}
		var cid, hid, size int
		var err error
		if cid, err = strconv.Atoi(fs[0]); err != nil {
			return fmt.Errorf("invalid mapping: %s", idMap)
		}
		if hid, err = strconv.Atoi(fs[1]); err != nil {
			return fmt.Errorf("invalid mapping: %s", idMap)
		}
		if size, err = strconv.Atoi(fs[2]); err != nil {
			return fmt.Errorf("invalid mapping: %s", idMap)
		}
		m := specs.LinuxIDMapping{
			ContainerID: uint32(cid),
			HostID:      uint32(hid),
			Size:        uint32(size),
		}
		*is = append(*is, m)
	}
	return nil
}

type volumes []string

// Set implements flag.Value.Get.
func (v *volumes) Set(value string) error {
	*v = append(*v, value)
	return nil
}

// Get implements flag.Value.Get.
func (v *volumes) Get() any {
	return v
}

// String implements flag.Value.String.
func (v *volumes) String() string {
	return strings.Join(*v, ",")
}

// SetFlags implements subcommands.Command.SetFlags.
func (c *Do) SetFlags(f *flag.FlagSet) {
	f.StringVar(&c.root, "root", "/", `path to the root directory, defaults to "/"`)
	f.StringVar(&c.cwd, "cwd", ".", "path to the current directory, defaults to the current directory")
	f.StringVar(&c.erofs, "erofs", "", "path to the erofs root image")
	f.StringVar(&c.ip, "ip", "192.168.10.2", "IPv4 address for the sandbox")
	f.BoolVar(&c.quiet, "quiet", false, "suppress runsc messages to stdout. Application output is still sent to stdout and stderr")
	f.BoolVar(&c.overlay, "force-overlay", true, "use an overlay. WARNING: disabling gives the command write access to the host")
	f.Var(&c.uidMap, "uid-map", "Add a user id mapping [ContainerID, HostID, Size]")
	f.Var(&c.gidMap, "gid-map", "Add a group id mapping [ContainerID, HostID, Size]")
	f.Var(&c.volumes, "volume", "Add a volume path SRC[:DST]. This option can be used multiple times to add several volumes.")
}

// Execute implements subcommands.Command.Execute.
func (c *Do) Execute(_ context.Context, f *flag.FlagSet, args ...any) subcommands.ExitStatus {
	if len(f.Args()) == 0 {
		f.Usage()
		return subcommands.ExitUsageError
	}

	conf := args[0].(*config.Config)
	waitStatus := args[1].(*unix.WaitStatus)

	if conf.Rootless {
		if err := specutils.MaybeRunAsRoot(); err != nil {
			return util.Errorf("Error executing inside namespace: %v", err)
		}
		// Execution will continue here if no more capabilities are needed...
	}

	hostname, err := os.Hostname()
	if err != nil {
		return util.Errorf("Error to retrieve hostname: %v", err)
	}

	// If c.overlay is set, then enable overlay.
	conf.Overlay = false // conf.Overlay is deprecated.
	if c.overlay {
		conf.Overlay2.Set("all:memory")
	} else {
		conf.Overlay2.Set("none")
	}
	absRoot, err := resolvePath(c.root)
	if err != nil {
		return util.Errorf("Error resolving root: %v", err)
	}
	absCwd, err := resolvePath(c.cwd)
	if err != nil {
		return util.Errorf("Error resolving current directory: %v", err)
	}

	spec := &specs.Spec{
		Root: &specs.Root{
			Path: absRoot,
		},
		Process: &specs.Process{
			Cwd:          absCwd,
			Args:         f.Args(),
			Env:          os.Environ(),
			Capabilities: specutils.AllCapabilities(),
			Terminal:     console.StdioIsPty(),
		},
		Hostname: hostname,
	}
	if c.erofs != "" {
		spec.Annotations = map[string]string{
			"dev.gvisor.spec.rootfs.source":  c.erofs,
			"dev.gvisor.spec.rootfs.type":    "erofs",
			"dev.gvisor.spec.rootfs.overlay": "memory",
		}
	}
	for _, v := range c.volumes {
		parts := strings.SplitN(v, ":", 2)
		var src, dst string
		if len(parts) == 2 {
			src, dst = parts[0], parts[1]
		} else {
			src, dst = v, v
		}
		spec.Mounts = append(spec.Mounts, specs.Mount{
			Type:        "bind",
			Source:      src,
			Destination: dst,
		})
	}

	cid := fmt.Sprintf("runsc-%06d", rand.Int31n(1000000))

	if c.uidMap != nil || c.gidMap != nil {
		addNamespace(spec, specs.LinuxNamespace{Type: specs.UserNamespace})
		spec.Linux.UIDMappings = c.uidMap
		spec.Linux.GIDMappings = c.gidMap
	}

	if conf.Network == config.NetworkNone {
		addNamespace(spec, specs.LinuxNamespace{Type: specs.NetworkNamespace})
	} else if conf.Rootless {
		if conf.Network == config.NetworkSandbox {
			c.notifyUser("*** Warning: sandbox network isn't supported with --rootless, switching to host ***")
			conf.Network = config.NetworkHost
		}

	} else {
		switch clean, err := c.setupNet(cid, spec); err {
		case errNoDefaultInterface:
			log.Warningf("Network interface not found, using internal network")
			addNamespace(spec, specs.LinuxNamespace{Type: specs.NetworkNamespace})
			conf.Network = config.NetworkHost

		case nil:
			// Setup successful.
			defer clean()

		default:
			return util.Errorf("Error setting up network: %v", err)
		}
	}

	return startContainerAndWait(spec, conf, cid, waitStatus)
}

func addNamespace(spec *specs.Spec, ns specs.LinuxNamespace) {
	if spec.Linux == nil {
		spec.Linux = &specs.Linux{}
	}
	spec.Linux.Namespaces = append(spec.Linux.Namespaces, ns)
}

func (c *Do) notifyUser(format string, v ...any) {
	if !c.quiet {
		fmt.Printf(format+"\n", v...)
	}
	log.Warningf(format, v...)
}

func resolvePath(path string) (string, error) {
	var err error
	path, err = filepath.Abs(path)
	if err != nil {
		return "", fmt.Errorf("resolving %q: %v", path, err)
	}
	path = filepath.Clean(path)
	if err := unix.Access(path, 0); err != nil {
		return "", fmt.Errorf("unable to access %q: %v", path, err)
	}
	return path, nil
}

// setupNet setups up the sandbox network, including the creation of a network
// namespace, and iptable rules to redirect the traffic. Returns a cleanup
// function to tear down the network. Returns errNoDefaultInterface when there
// is no network interface available to setup the network.
func (c *Do) setupNet(cid string, spec *specs.Spec) (func(), error) {
	dev, err := defaultDevice()
	if err != nil {
		return nil, errNoDefaultInterface
	}
	mtu, err := deviceMTU(dev)
	if err != nil {
		return nil, err
	}
	peerIP, err := calculatePeerIP(c.ip)
	if err != nil {
		return nil, err
	}
	veth, peer := deviceNames(cid)

	cmds := []string{
		fmt.Sprintf("ip link add %s mtu %v type veth peer name %s", veth, mtu, peer),

		// Setup device outside the namespace.
		fmt.Sprintf("ip addr add %s/24 dev %s", peerIP, peer),
		fmt.Sprintf("ip link set %s up", peer),

		// Setup device inside the namespace.
		fmt.Sprintf("ip netns add %s", cid),
		fmt.Sprintf("ip link set %s netns %s", veth, cid),
		fmt.Sprintf("ip netns exec %s ip addr add %s/24 dev %s", cid, c.ip, veth),
		fmt.Sprintf("ip netns exec %s ip link set %s up", cid, veth),
		fmt.Sprintf("ip netns exec %s ip link set lo up", cid),
		fmt.Sprintf("ip netns exec %s ip route add default via %s", cid, peerIP),

		// Enable network access.
		"sysctl -w net.ipv4.ip_forward=1",
		fmt.Sprintf("iptables -t nat -A POSTROUTING -s %s -o %s -m comment --comment runsc-%s -j MASQUERADE", c.ip, dev, peer),
		fmt.Sprintf("iptables -A FORWARD -i %s -o %s -j ACCEPT", dev, peer),
		fmt.Sprintf("iptables -A FORWARD -o %s -i %s -j ACCEPT", dev, peer),
	}

	for _, cmd := range cmds {
		log.Debugf("Run %q", cmd)
		args := strings.Split(cmd, " ")
		cmd := exec.Command(args[0], args[1:]...)
		if err := cmd.Run(); err != nil {
			c.cleanupNet(cid, dev, "", "", "")
			return nil, fmt.Errorf("failed to run %q: %v", cmd, err)
		}
	}

	resolvPath, err := makeFile("/etc/resolv.conf", "nameserver 8.8.8.8\n", spec)
	if err != nil {
		c.cleanupNet(cid, dev, "", "", "")
		return nil, err
	}
	hostnamePath, err := makeFile("/etc/hostname", cid+"\n", spec)
	if err != nil {
		c.cleanupNet(cid, dev, resolvPath, "", "")
		return nil, err
	}
	hosts := fmt.Sprintf("127.0.0.1\tlocalhost\n%s\t%s\n", c.ip, cid)
	hostsPath, err := makeFile("/etc/hosts", hosts, spec)
	if err != nil {
		c.cleanupNet(cid, dev, resolvPath, hostnamePath, "")
		return nil, err
	}

	netns := specs.LinuxNamespace{
		Type: specs.NetworkNamespace,
		Path: filepath.Join("/var/run/netns", cid),
	}
	addNamespace(spec, netns)

	return func() { c.cleanupNet(cid, dev, resolvPath, hostnamePath, hostsPath) }, nil
}

// cleanupNet tries to cleanup the network setup in setupNet.
//
// It may be called when setupNet is only partially complete, in which case it
// will cleanup as much as possible, logging warnings for the rest.
//
// Unfortunately none of this can be automatically cleaned up on process exit,
// we must do so explicitly.
func (c *Do) cleanupNet(cid, dev, resolvPath, hostnamePath, hostsPath string) {
	_, peer := deviceNames(cid)

	cmds := []string{
		fmt.Sprintf("ip link delete %s", peer),
		fmt.Sprintf("ip netns delete %s", cid),
		fmt.Sprintf("iptables -t nat -D POSTROUTING -s %s -o %s -m comment --comment runsc-%s -j MASQUERADE", c.ip, dev, peer),
		fmt.Sprintf("iptables -D FORWARD -i %s -o %s -j ACCEPT", dev, peer),
		fmt.Sprintf("iptables -D FORWARD -o %s -i %s -j ACCEPT", dev, peer),
	}

	for _, cmd := range cmds {
		log.Debugf("Run %q", cmd)
		args := strings.Split(cmd, " ")
		c := exec.Command(args[0], args[1:]...)
		if err := c.Run(); err != nil {
			log.Warningf("Failed to run %q: %v", cmd, err)
		}
	}

	tryRemove(resolvPath)
	tryRemove(hostnamePath)
	tryRemove(hostsPath)
}

func deviceNames(cid string) (string, string) {
	// Device name is limited to 15 letters.
	return "ve-" + cid, "vp-" + cid

}

func defaultDevice() (string, error) {
	out, err := exec.Command("ip", "route", "list", "default").CombinedOutput()
	if err != nil {
		return "", err
	}
	parts := strings.Split(string(out), " ")
	if len(parts) < 5 {
		return "", fmt.Errorf("malformed %q output: %q", "ip route list default", string(out))
	}
	return parts[4], nil
}

func deviceMTU(dev string) (int, error) {
	intf, err := net.InterfaceByName(dev)
	if err != nil {
		return 0, err
	}
	return intf.MTU, nil
}

func makeFile(dest, content string, spec *specs.Spec) (string, error) {
	tmpFile, err := os.CreateTemp("", filepath.Base(dest))
	if err != nil {
		return "", err
	}
	if _, err := tmpFile.WriteString(content); err != nil {
		if err := os.Remove(tmpFile.Name()); err != nil {
			log.Warningf("Failed to remove %q: %v", tmpFile, err)
		}
		return "", err
	}
	spec.Mounts = append(spec.Mounts, specs.Mount{
		Source:      tmpFile.Name(),
		Destination: dest,
		Type:        "bind",
		Options:     []string{"ro"},
	})
	return tmpFile.Name(), nil
}

func tryRemove(path string) {
	if path == "" {
		return
	}

	if err := os.Remove(path); err != nil {
		log.Warningf("Failed to remove %q: %v", path, err)
	}
}

func calculatePeerIP(ip string) (string, error) {
	parts := strings.Split(ip, ".")
	if len(parts) != 4 {
		return "", fmt.Errorf("invalid IP format %q", ip)
	}
	n, err := strconv.Atoi(parts[3])
	if err != nil {
		return "", fmt.Errorf("invalid IP format %q: %v", ip, err)
	}
	n++
	if n > 255 {
		n = 1
	}
	return fmt.Sprintf("%s.%s.%s.%d", parts[0], parts[1], parts[2], n), nil
}

func startContainerAndWait(spec *specs.Spec, conf *config.Config, cid string, waitStatus *unix.WaitStatus) subcommands.ExitStatus {
	specutils.LogSpecDebug(spec, conf.OCISeccomp)

	out, err := json.Marshal(spec)
	if err != nil {
		return util.Errorf("Error to marshal spec: %v", err)
	}
	tmpDir, err := os.MkdirTemp("", "runsc-do")
	if err != nil {
		return util.Errorf("Error to create tmp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	log.Infof("Changing configuration RootDir to %q", tmpDir)
	conf.RootDir = tmpDir

	cfgPath := filepath.Join(tmpDir, "config.json")
	if err := os.WriteFile(cfgPath, out, 0755); err != nil {
		return util.Errorf("Error write spec: %v", err)
	}

	containerArgs := container.Args{
		ID:        cid,
		Spec:      spec,
		BundleDir: tmpDir,
		Attached:  true,
	}

	ct, err := container.New(conf, containerArgs)
	if err != nil {
		return util.Errorf("creating container: %v", err)
	}
	defer ct.Destroy()

	if err := ct.Start(conf); err != nil {
		return util.Errorf("starting container: %v", err)
	}

	// Forward signals to init in the container. Thus if we get SIGINT from
	// ^C, the container gracefully exit, and we can clean up.
	//
	// N.B. There is a still a window before this where a signal may kill
	// this process, skipping cleanup.
	stopForwarding := ct.ForwardSignals(0 /* pid */, spec.Process.Terminal /* fgProcess */)
	defer stopForwarding()

	ws, err := ct.Wait()
	if err != nil {
		return util.Errorf("waiting for container: %v", err)
	}

	*waitStatus = ws
	return subcommands.ExitSuccess
}
