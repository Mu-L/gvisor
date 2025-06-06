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
	"os"

	"github.com/google/subcommands"
	"golang.org/x/sys/unix"
	"gvisor.dev/gvisor/pkg/log"
	"gvisor.dev/gvisor/runsc/cmd/util"
	"gvisor.dev/gvisor/runsc/config"
	"gvisor.dev/gvisor/runsc/container"
	"gvisor.dev/gvisor/runsc/flag"
)

const (
	unsetPID = -1
)

// Wait implements subcommands.Command for the "wait" command.
type Wait struct {
	rootPID    int
	pid        int
	checkpoint bool
	restore    bool
}

// Name implements subcommands.Command.Name.
func (*Wait) Name() string {
	return "wait"
}

// Synopsis implements subcommands.Command.Synopsis.
func (*Wait) Synopsis() string {
	return "wait on a process inside a container"
}

// Usage implements subcommands.Command.Usage.
func (*Wait) Usage() string {
	return "wait [flags] <container id>\n"
}

// SetFlags implements subcommands.Command.SetFlags.
func (wt *Wait) SetFlags(f *flag.FlagSet) {
	f.IntVar(&wt.rootPID, "rootpid", unsetPID, "select a PID in the sandbox root PID namespace to wait on instead of the container's root process")
	f.IntVar(&wt.pid, "pid", unsetPID, "select a PID in the container's PID namespace to wait on instead of the container's root process")
	f.BoolVar(&wt.checkpoint, "checkpoint", false, "wait for the next checkpoint to complete")
	f.BoolVar(&wt.restore, "restore", false, "wait for the restore to complete")
}

// Execute implements subcommands.Command.Execute. It waits for a process in a
// container to exit before returning.
func (wt *Wait) Execute(_ context.Context, f *flag.FlagSet, args ...any) subcommands.ExitStatus {
	if f.NArg() != 1 {
		f.Usage()
		return subcommands.ExitUsageError
	}
	// You can't specify both -pid and -rootpid.
	if wt.rootPID != unsetPID && wt.pid != unsetPID {
		util.Fatalf("only one of -pid and -rootPid can be set")
	}

	id := f.Arg(0)
	conf := args[0].(*config.Config)

	c, err := container.Load(conf.RootDir, container.FullID{ContainerID: id}, container.LoadOpts{})
	if err != nil {
		util.Fatalf("loading container: %v", err)
	}

	if wt.checkpoint {
		if wt.rootPID != unsetPID || wt.pid != unsetPID {
			log.Warningf("waiting for checkpoint to complete, ignoring -pid and -rootpid")
		}
		if err := c.WaitCheckpoint(); err != nil {
			util.Fatalf("waiting for checkpoint to complete: %v", err)
		}
		return subcommands.ExitSuccess
	}

	if wt.restore {
		if wt.rootPID != unsetPID || wt.pid != unsetPID {
			log.Warningf("waiting for restore to complete, ignoring -pid and -rootpid")
		}
		if err := c.WaitRestore(); err != nil {
			util.Fatalf("waiting for restore to complete: %v", err)
		}
		return subcommands.ExitSuccess
	}

	var waitStatus unix.WaitStatus
	switch {
	// Wait on the whole container.
	case wt.rootPID == unsetPID && wt.pid == unsetPID:
		ws, err := c.Wait()
		if err != nil {
			util.Fatalf("waiting on container %q: %v", c.ID, err)
		}
		waitStatus = ws
	// Wait on a PID in the root PID namespace.
	case wt.rootPID != unsetPID:
		ws, err := c.WaitRootPID(int32(wt.rootPID))
		if err != nil {
			util.Fatalf("waiting on PID in root PID namespace %d in container %q: %v", wt.rootPID, c.ID, err)
		}
		waitStatus = ws
	// Wait on a PID in the container's PID namespace.
	case wt.pid != unsetPID:
		ws, err := c.WaitPID(int32(wt.pid))
		if err != nil {
			util.Fatalf("waiting on PID %d in container %q: %v", wt.pid, c.ID, err)
		}
		waitStatus = ws
	}
	result := waitResult{
		ID:         id,
		ExitStatus: exitStatus(waitStatus),
	}
	// Write json-encoded wait result directly to stdout.
	if err := json.NewEncoder(os.Stdout).Encode(result); err != nil {
		util.Fatalf("marshaling wait result: %v", err)
	}
	return subcommands.ExitSuccess
}

type waitResult struct {
	ID         string `json:"id"`
	ExitStatus int    `json:"exitStatus"`
}

// exitStatus returns the correct exit status for a process based on if it
// was signaled or exited cleanly.
func exitStatus(status unix.WaitStatus) int {
	if status.Signaled() {
		return 128 + int(status.Signal())
	}
	return status.ExitStatus()
}
