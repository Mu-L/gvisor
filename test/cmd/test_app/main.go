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

// Binary test_app is like a swiss knife for tests that need to run anything
// inside the sandbox. New functionality can be added with new commands.
package main

import (
	"context"
	"fmt"
	"io"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	sys "syscall"
	"time"

	"github.com/google/subcommands"
	"github.com/kr/pty"
	"gvisor.dev/gvisor/pkg/gvisordetect"
	gvisorrand "gvisor.dev/gvisor/pkg/rand"
	"gvisor.dev/gvisor/pkg/test/testutil"
	"gvisor.dev/gvisor/runsc/flag"
)

func main() {
	subcommands.Register(subcommands.HelpCommand(), "")
	subcommands.Register(subcommands.FlagsCommand(), "")
	subcommands.Register(new(capability), "")
	subcommands.Register(new(fdReceiver), "")
	subcommands.Register(new(fdSender), "")
	subcommands.Register(new(forkBomb), "")
	subcommands.Register(new(fsTreeCreator), "")
	subcommands.Register(new(gvisorDetect), "")
	subcommands.Register(new(ptyRunner), "")
	subcommands.Register(new(reaper), "")
	subcommands.Register(new(syscall), "")
	subcommands.Register(new(taskTree), "")
	subcommands.Register(new(uds), "")
	subcommands.Register(new(zombieTest), "")

	flag.Parse()

	exitCode := subcommands.Execute(context.Background())
	os.Exit(int(exitCode))
}

type fsTreeCreator struct {
	depth            uint
	numFilesPerLevel uint
	fileSize         uint
	targetDir        string
	createSymlink    bool
}

// Name implements subcommands.Command.Name.
func (*fsTreeCreator) Name() string {
	return "fsTreeCreate"
}

// Synopsis implements subcommands.Command.Synopsys.
func (*fsTreeCreator) Synopsis() string {
	return "creates a filesystem tree of a certain depth, with a certain number of files on each level and each file with a certain size and type, under a certain directory."
}

// Usage implements subcommands.Command.Usage.
func (*fsTreeCreator) Usage() string {
	return "fsTreeCreate <flags>"
}

// SetFlags implements subcommands.Command.SetFlags.
func (c *fsTreeCreator) SetFlags(f *flag.FlagSet) {
	f.UintVar(&c.depth, "depth", 10, "number of levels to create")
	f.UintVar(&c.numFilesPerLevel, "file-per-level", 10, "number of files to create per level")
	f.UintVar(&c.fileSize, "file-size", 4096, "size of each file")
	f.StringVar(&c.targetDir, "target-dir", "/", "directory under which to create the filesystem tree")
	f.BoolVar(&c.createSymlink, "create-symlink", false, "create symlinks other than the first file per level")
}

// Execute implements subcommands.Command.Execute.
func (c *fsTreeCreator) Execute(ctx context.Context, f *flag.FlagSet, args ...any) subcommands.ExitStatus {
	curDir := c.targetDir
	if _, err := os.Stat(curDir); os.IsNotExist(err) {
		if err := os.MkdirAll(curDir, 0777); err != nil {
			log.Fatalf("error creating directory %q: %v", curDir, err)
		}
	}

	data := make([]byte, c.fileSize)
	gvisorrand.Read(data)
	for i := uint(0); i < c.depth; i++ {
		for j := uint(0); j < c.numFilesPerLevel; j++ {
			filePath := filepath.Join(curDir, fmt.Sprintf("file%d", j))
			if c.createSymlink && j > 0 {
				if err := os.Symlink("file0", filePath); err != nil {
					log.Fatalf("error creating symlink %q: %v", filePath, err)
				}
			} else {
				if err := os.WriteFile(filePath, data, 0666); err != nil {
					log.Fatalf("error writing file %q: %v", filePath, err)
				}
			}
		}
		nextDir := filepath.Join(curDir, "dir")
		if err := os.Mkdir(nextDir, 0777); err != nil {
			log.Fatalf("error creating directory %q: %v", nextDir, err)
		}
		curDir = nextDir
	}
	return subcommands.ExitSuccess
}

type uds struct {
	fileName   string
	socketPath string
}

// Name implements subcommands.Command.Name.
func (*uds) Name() string {
	return "uds"
}

// Synopsis implements subcommands.Command.Synopsys.
func (*uds) Synopsis() string {
	return "creates unix domain socket client and server. Client sends a contant flow of sequential numbers. Server prints them to --file"
}

// Usage implements subcommands.Command.Usage.
func (*uds) Usage() string {
	return "uds <flags>"
}

// SetFlags implements subcommands.Command.SetFlags.
func (c *uds) SetFlags(f *flag.FlagSet) {
	f.StringVar(&c.fileName, "file", "", "name of output file")
	f.StringVar(&c.socketPath, "socket", "", "path to socket")
}

// Execute implements subcommands.Command.Execute.
func (c *uds) Execute(ctx context.Context, f *flag.FlagSet, args ...any) subcommands.ExitStatus {
	if c.fileName == "" || c.socketPath == "" {
		log.Fatalf("Flags cannot be empty, given: fileName: %q, socketPath: %q", c.fileName, c.socketPath)
		return subcommands.ExitFailure
	}
	outputFile, err := os.OpenFile(c.fileName, os.O_WRONLY|os.O_CREATE, 0666)
	if err != nil {
		log.Fatal("error opening output file:", err)
	}

	defer os.Remove(c.socketPath)

	listener, err := net.Listen("unix", c.socketPath)
	if err != nil {
		log.Fatalf("error listening on socket %q: %v", c.socketPath, err)
	}

	go server(listener, outputFile)
	for i := 0; ; i++ {
		conn, err := net.Dial("unix", c.socketPath)
		if err != nil {
			log.Fatal("error dialing:", err)
		}
		if _, err := conn.Write([]byte(strconv.Itoa(i))); err != nil {
			log.Fatal("error writing:", err)
		}
		conn.Close()
		time.Sleep(100 * time.Millisecond)
	}
}

func server(listener net.Listener, out *os.File) {
	buf := make([]byte, 16)

	for {
		c, err := listener.Accept()
		if err != nil {
			log.Fatal("error accepting connection:", err)
		}
		nr, err := c.Read(buf)
		if err != nil {
			log.Fatal("error reading from buf:", err)
		}
		data := buf[0:nr]
		fmt.Fprint(out, string(data)+"\n")
	}
}

type taskTree struct {
	depth int
	width int
	pause bool
}

// Name implements subcommands.Command.
func (*taskTree) Name() string {
	return "task-tree"
}

// Synopsis implements subcommands.Command.
func (*taskTree) Synopsis() string {
	return "creates a tree of tasks"
}

// Usage implements subcommands.Command.
func (*taskTree) Usage() string {
	return "task-tree <flags>"
}

// SetFlags implements subcommands.Command.
func (c *taskTree) SetFlags(f *flag.FlagSet) {
	f.IntVar(&c.depth, "depth", 1, "number of levels to create")
	f.IntVar(&c.width, "width", 1, "number of tasks at each level")
	f.BoolVar(&c.pause, "pause", false, "whether the tasks should pause perpetually")
}

// Execute implements subcommands.Command.
func (c *taskTree) Execute(ctx context.Context, f *flag.FlagSet, args ...any) subcommands.ExitStatus {
	if c.depth == 0 {
		log.Printf("Child sleeping, PID: %d\n", os.Getpid())
		for {
			time.Sleep(time.Hour)
		}
	}

	log.Printf("Parent %d creating %d children, PID: %d\n", c.depth, c.width, os.Getpid())

	stop := testutil.StartReaper()
	defer stop()

	var cmds []*exec.Cmd
	for i := 0; i < c.width; i++ {
		cmd := exec.Command(
			"/proc/self/exe", c.Name(),
			"--depth", strconv.Itoa(c.depth-1),
			"--width", strconv.Itoa(c.width),
			fmt.Sprintf("--pause=%t", c.pause))
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr

		if err := cmd.Start(); err != nil {
			log.Fatal("failed to call self:", err)
		}
		cmds = append(cmds, cmd)
	}

	for _, c := range cmds {
		c.Wait()
	}

	if c.pause {
		log.Printf("Parent %d sleeping, PID: %d\n", c.depth, os.Getpid())
		for {
			time.Sleep(time.Hour)
		}
	}

	return subcommands.ExitSuccess
}

type gvisorDetect struct {
	exitCodeOnGVisor    int
	exitCodeOnNotGVisor int
}

// Name implements subcommands.Command.Name.
func (*gvisorDetect) Name() string {
	return "gvisor-detect"
}

// Synopsis implements subcommands.Command.Synopsys.
func (*gvisorDetect) Synopsis() string {
	return "checks if the process is running inside gVisor by checking for the marker file"
}

// Usage implements subcommands.Command.Usage.
func (*gvisorDetect) Usage() string {
	return "gvisor-detect [flags]"
}

// SetFlags implements subcommands.Command.SetFlags.
func (c *gvisorDetect) SetFlags(f *flag.FlagSet) {
	f.IntVar(&c.exitCodeOnGVisor, "exit-code-on-gvisor", 0, "exit code to return if running in gVisor")
	f.IntVar(&c.exitCodeOnNotGVisor, "exit-code-on-not-gvisor", 42, "exit code to return if not running in gVisor")
}

// Execute implements subcommands.Command.Execute.
func (c *gvisorDetect) Execute(ctx context.Context, f *flag.FlagSet, args ...any) subcommands.ExitStatus {
	isGVisor, err := gvisordetect.RunningInGVisor()
	if err != nil {
		fmt.Fprintf(os.Stderr, "Error checking whether we are running in gVisor: %v\n", err)
		return subcommands.ExitFailure
	}
	if isGVisor {
		fmt.Println("running with gVisor kernel")
		return subcommands.ExitStatus(c.exitCodeOnGVisor)
	}
	fmt.Println("running with non-gVisor kernel")
	return subcommands.ExitStatus(c.exitCodeOnNotGVisor)
}

type forkBomb struct {
	delay time.Duration
}

// Name implements subcommands.Command.
func (*forkBomb) Name() string {
	return "fork-bomb"
}

// Synopsis implements subcommands.Command.
func (*forkBomb) Synopsis() string {
	return "creates child process until the end of times"
}

// Usage implements subcommands.Command.
func (*forkBomb) Usage() string {
	return "fork-bomb <flags>"
}

// SetFlags implements subcommands.Command.
func (c *forkBomb) SetFlags(f *flag.FlagSet) {
	f.DurationVar(&c.delay, "delay", 100*time.Millisecond, "amount of time to delay creation of child")
}

// Execute implements subcommands.Command.
func (c *forkBomb) Execute(ctx context.Context, f *flag.FlagSet, args ...any) subcommands.ExitStatus {
	time.Sleep(c.delay)

	cmd := exec.Command("/proc/self/exe", c.Name())
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		log.Fatal("failed to call self:", err)
	}
	return subcommands.ExitSuccess
}

type reaper struct{}

// Name implements subcommands.Command.
func (*reaper) Name() string {
	return "reaper"
}

// Synopsis implements subcommands.Command.
func (*reaper) Synopsis() string {
	return "reaps all children in a loop"
}

// Usage implements subcommands.Command.
func (*reaper) Usage() string {
	return "reaper <flags>"
}

// SetFlags implements subcommands.Command.
func (*reaper) SetFlags(*flag.FlagSet) {}

// Execute implements subcommands.Command.
func (c *reaper) Execute(ctx context.Context, f *flag.FlagSet, args ...any) subcommands.ExitStatus {
	stop := testutil.StartReaper()
	defer stop()
	select {}
}

type syscall struct {
	sysno uint64
}

// Name implements subcommands.Command.
func (*syscall) Name() string {
	return "syscall"
}

// Synopsis implements subcommands.Command.
func (*syscall) Synopsis() string {
	return "syscall makes a syscall"
}

// Usage implements subcommands.Command.
func (*syscall) Usage() string {
	return "syscall <flags> [arg1 arg2...]"
}

// SetFlags implements subcommands.Command.
func (s *syscall) SetFlags(f *flag.FlagSet) {
	f.Uint64Var(&s.sysno, "syscall", 0, "syscall to call")
}

// Execute implements subcommands.Command.
func (s *syscall) Execute(ctx context.Context, f *flag.FlagSet, _ ...any) subcommands.ExitStatus {
	const maxSyscallArgs = 6
	numArgs := f.NArg()
	if numArgs > maxSyscallArgs {
		fmt.Printf("number of sycall arguments not supported: %d (max is %d)\n", numArgs, maxSyscallArgs)
		return subcommands.ExitUsageError
	}
	var syscallArgs [maxSyscallArgs]uintptr
	for i := 0; i < numArgs; i++ {
		uintArg, err := strconv.ParseUint(f.Arg(i), 10, 64)
		if err != nil {
			fmt.Printf("not an integer: %q\n", f.Arg(i))
			return subcommands.ExitUsageError
		}
		syscallArgs[i] = uintptr(uintArg)
	}
	var errno sys.Errno
	switch numArgs {
	case 0:
		_, _, errno = sys.Syscall(uintptr(s.sysno), 0, 0, 0)
	case 3:
		_, _, errno = sys.Syscall(uintptr(s.sysno), syscallArgs[0], syscallArgs[1], syscallArgs[2])
	case 6:
		_, _, errno = sys.Syscall6(uintptr(s.sysno), syscallArgs[0], syscallArgs[1], syscallArgs[2], syscallArgs[3], syscallArgs[4], syscallArgs[5])
	default:
		fmt.Printf("number of sycall arguments not supported: %d\n", numArgs)
		return subcommands.ExitUsageError
	}
	if errno != 0 {
		fmt.Printf("syscall(%d, %s) failed: %v\n", s.sysno, strings.Join(f.Args(), ", "), errno)
	} else {
		fmt.Printf("syscall(%d, %s) success\n", s.sysno, strings.Join(f.Args(), ", "))
	}
	return subcommands.ExitSuccess
}

type capability struct {
	enabled  uint64
	disabled uint64
}

// Name implements subcommands.Command.
func (*capability) Name() string {
	return "capability"
}

// Synopsis implements subcommands.Command.
func (*capability) Synopsis() string {
	return "checks if effective capabilities are set/unset"
}

// Usage implements subcommands.Command.
func (*capability) Usage() string {
	return "capability [--enabled=number] [--disabled=number]"
}

// SetFlags implements subcommands.Command.
func (c *capability) SetFlags(f *flag.FlagSet) {
	f.Uint64Var(&c.enabled, "enabled", 0, "")
	f.Uint64Var(&c.disabled, "disabled", 0, "")
}

// Execute implements subcommands.Command.
func (c *capability) Execute(ctx context.Context, f *flag.FlagSet, args ...any) subcommands.ExitStatus {
	if c.enabled == 0 && c.disabled == 0 {
		fmt.Println("One of the flags must be set")
		return subcommands.ExitUsageError
	}

	status, err := os.ReadFile("/proc/self/status")
	if err != nil {
		fmt.Printf("Error reading %q: %v\n", "proc/self/status", err)
		return subcommands.ExitFailure
	}
	re := regexp.MustCompile("CapEff:\t([0-9a-f]+)\n")
	matches := re.FindStringSubmatch(string(status))
	if matches == nil || len(matches) != 2 {
		fmt.Printf("Effective capabilities not found in\n%s\n", status)
		return subcommands.ExitFailure
	}
	caps, err := strconv.ParseUint(matches[1], 16, 64)
	if err != nil {
		fmt.Printf("failed to convert capabilities %q: %v\n", matches[1], err)
		return subcommands.ExitFailure
	}

	if c.enabled != 0 && (caps&c.enabled) != c.enabled {
		fmt.Printf("Missing capabilities, want: %#x: got: %#x\n", c.enabled, caps)
		return subcommands.ExitFailure
	}
	if c.disabled != 0 && (caps&c.disabled) != 0 {
		fmt.Printf("Extra capabilities found, dont_want: %#x: got: %#x\n", c.disabled, caps)
		return subcommands.ExitFailure
	}

	return subcommands.ExitSuccess
}

type ptyRunner struct{}

// Name implements subcommands.Command.
func (*ptyRunner) Name() string {
	return "pty-runner"
}

// Synopsis implements subcommands.Command.
func (*ptyRunner) Synopsis() string {
	return "runs the given command with an open pty terminal"
}

// Usage implements subcommands.Command.
func (*ptyRunner) Usage() string {
	return "pty-runner [command]"
}

// SetFlags implements subcommands.Command.SetFlags.
func (*ptyRunner) SetFlags(f *flag.FlagSet) {}

// Execute implements subcommands.Command.
func (*ptyRunner) Execute(_ context.Context, fs *flag.FlagSet, _ ...any) subcommands.ExitStatus {
	c := exec.Command(fs.Args()[0], fs.Args()[1:]...)
	f, err := pty.Start(c)
	if err != nil {
		fmt.Printf("pty.Start failed: %v", err)
		return subcommands.ExitFailure
	}
	defer f.Close()

	// Copy stdout from the command to keep this process alive until the
	// subprocess exits.
	io.Copy(os.Stdout, f)

	return subcommands.ExitSuccess
}
