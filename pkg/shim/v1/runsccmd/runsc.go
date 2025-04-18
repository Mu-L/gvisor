// Copyright 2018 The containerd Authors.
// Copyright 2018 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Package runsccmd provides an API to interact with runsc command line.
package runsccmd

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"time"

	runc "github.com/containerd/go-runc"
	"github.com/containerd/log"
	specs "github.com/opencontainers/runtime-spec/specs-go"
	"golang.org/x/sys/unix"
)

// DefaultCommand is the default command for Runsc.
const DefaultCommand = "runsc"

// ProcessMonitor is a subset of runc.ProcessMonitor. It does not include
// StartLocked(), which was added in containerd/runc v1.1.1. This is so that
// we can continue using containerd/containerd v1.4.13 with newer
// containerd/runc versions without breaking build.
type ProcessMonitor interface {
	Start(cmd *exec.Cmd) (chan runc.Exit, error)
	Wait(cmd *exec.Cmd, ch chan runc.Exit) (int, error)
}

// Monitor is the default process monitor to be used by runsc.
var Monitor ProcessMonitor = &LogMonitor{Next: runc.Monitor}

// LogMonitor implements the runc.ProcessMonitor interface, logging the command
// that is getting executed, and then forwarding the call to another
// implementation.
type LogMonitor struct {
	Next ProcessMonitor
}

// Start implements runc.ProcessMonitor.
func (l *LogMonitor) Start(cmd *exec.Cmd) (chan runc.Exit, error) {
	log.L.Debugf("Executing: %s", cmd.Args)
	return l.Next.Start(cmd)
}

// Wait implements runc.ProcessMonitor.
func (l *LogMonitor) Wait(cmd *exec.Cmd, ch chan runc.Exit) (int, error) {
	status, err := l.Next.Wait(cmd, ch)
	log.L.Debugf("Command exit code: %d, err: %v", status, err)
	return status, err
}

// Runsc is the client to the runsc cli.
type Runsc struct {
	Command      string
	PdeathSignal unix.Signal
	Setpgid      bool
	Root         string
	Log          string
	LogFormat    runc.Format
	PanicLog     string
	Config       map[string]string
}

// List returns all containers created inside the provided runsc root directory.
func (r *Runsc) List(context context.Context) ([]*runc.Container, error) {
	data, stderr, err := cmdOutput(r.command(context, "list", "--format=json"), false)
	if err != nil {
		return nil, fmt.Errorf("%w: %s", err, stderr)
	}
	var out []*runc.Container
	if err := json.Unmarshal(data, &out); err != nil {
		return nil, err
	}
	return out, nil
}

// State returns the state for the container provided by id.
func (r *Runsc) State(context context.Context, id string) (*runc.Container, error) {
	data, stderr, err := cmdOutput(r.command(context, "state", id), false)
	if err != nil {
		return nil, fmt.Errorf("%w: %s", err, stderr)
	}
	var c runc.Container
	if err := json.Unmarshal(data, &c); err != nil {
		return nil, err
	}
	return &c, nil
}

// CreateOpts is a set of options to Runsc.Create().
type CreateOpts struct {
	runc.IO
	ConsoleSocket runc.ConsoleSocket

	// PidFile is a path to where a pid file should be created.
	PidFile string

	// UserLog is a path to where runsc user log should be generated.
	UserLog string
}

func (o *CreateOpts) args() (out []string, err error) {
	if o.PidFile != "" {
		abs, err := filepath.Abs(o.PidFile)
		if err != nil {
			return nil, err
		}
		out = append(out, "--pid-file", abs)
	}
	if o.ConsoleSocket != nil {
		out = append(out, "--console-socket", o.ConsoleSocket.Path())
	}
	if o.UserLog != "" {
		out = append(out, "--user-log", o.UserLog)
	}
	return out, nil
}

// Create creates a new container and returns its pid if it was created successfully.
func (r *Runsc) Create(context context.Context, id, bundle string, opts *CreateOpts) error {
	args := []string{"create", "--bundle", bundle}
	if opts != nil {
		oargs, err := opts.args()
		if err != nil {
			return err
		}
		args = append(args, oargs...)
	}
	cmd := r.command(context, append(args, id)...)
	if opts != nil && opts.IO != nil {
		opts.Set(cmd)
	}

	if cmd.Stdout == nil && cmd.Stderr == nil {
		out, _, err := cmdOutput(cmd, true)
		if err != nil {
			return fmt.Errorf("%w: %s", err, out)
		}
		return nil
	}
	ec, err := Monitor.Start(cmd)
	if err != nil {
		return err
	}
	if opts != nil && opts.IO != nil {
		if c, ok := opts.IO.(runc.StartCloser); ok {
			if err := c.CloseAfterStart(); err != nil {
				return err
			}
		}
	}
	status, err := Monitor.Wait(cmd, ec)
	if err == nil && status != 0 {
		err = fmt.Errorf("%s did not terminate successfully", cmd.Args[0])
	}

	return err
}

// Pause will pause a running container.
func (r *Runsc) Pause(context context.Context, id string) error {
	if out, _, err := cmdOutput(r.command(context, "pause", id), true); err != nil {
		return fmt.Errorf("unable to pause: %w: %s", err, out)
	}
	return nil
}

// Resume will resume a paused container.
func (r *Runsc) Resume(context context.Context, id string) error {
	if out, _, err := cmdOutput(r.command(context, "resume", id), true); err != nil {
		return fmt.Errorf("unable to resume: %w: %s", err, out)
	}
	return nil
}

// Start will start an already created container.
func (r *Runsc) Start(context context.Context, id string, cio runc.IO) error {
	return r.start(context, cio, r.command(context, "start", id))
}

func (r *Runsc) start(context context.Context, cio runc.IO, cmd *exec.Cmd) error {
	if cio != nil {
		cio.Set(cmd)
	}

	if cmd.Stdout == nil && cmd.Stderr == nil {
		out, _, err := cmdOutput(cmd, true)
		if err != nil {
			return fmt.Errorf("%w: %s", err, out)
		}
		return nil
	}

	ec, err := Monitor.Start(cmd)
	if err != nil {
		return err
	}
	if cio != nil {
		if c, ok := cio.(runc.StartCloser); ok {
			if err := c.CloseAfterStart(); err != nil {
				return err
			}
		}
	}
	status, err := Monitor.Wait(cmd, ec)
	if err == nil && status != 0 {
		err = fmt.Errorf("%s did not terminate successfully", cmd.Args[0])
	}

	return err
}

// RestoreOpts is a set of options to runsc.Restore().
type RestoreOpts struct {
	ImagePath  string
	Detach     bool
	Direct     bool
	Background bool
}

func (o *RestoreOpts) args() []string {
	var out []string
	if o.ImagePath != "" {
		out = append(out, fmt.Sprintf("--image-path=%s", o.ImagePath))
	}
	if o.Detach {
		out = append(out, "--detach")
	}
	if o.Direct {
		out = append(out, "--direct")
	}
	if o.Background {
		out = append(out, "--background")
	}
	return out
}

// Restore will restore an already created container.
func (r *Runsc) Restore(context context.Context, id string, cio runc.IO, opts *RestoreOpts) error {
	args := []string{"restore"}
	if opts != nil {
		args = append(args, opts.args()...)
	}
	return r.start(context, cio, r.command(context, append(args, id)...))
}

type waitResult struct {
	ID         string `json:"id"`
	ExitStatus int    `json:"exitStatus"`
}

// Wait will wait for a running container, and return its exit status.
func (r *Runsc) Wait(context context.Context, id string) (int, error) {
	data, stderr, err := cmdOutput(r.command(context, "wait", id), false)
	if err != nil {
		return 0, fmt.Errorf("%w: %s", err, stderr)
	}
	var res waitResult
	if err := json.Unmarshal(data, &res); err != nil {
		return 0, err
	}
	return res.ExitStatus, nil
}

// ExecOpts is a set of options to runsc.Exec().
type ExecOpts struct {
	runc.IO
	PidFile         string
	InternalPidFile string
	ConsoleSocket   runc.ConsoleSocket
	Detach          bool
}

func (o *ExecOpts) args() (out []string, err error) {
	if o.ConsoleSocket != nil {
		out = append(out, "--console-socket", o.ConsoleSocket.Path())
	}
	if o.Detach {
		out = append(out, "--detach")
	}
	if o.PidFile != "" {
		abs, err := filepath.Abs(o.PidFile)
		if err != nil {
			return nil, err
		}
		out = append(out, "--pid-file", abs)
	}
	if o.InternalPidFile != "" {
		abs, err := filepath.Abs(o.InternalPidFile)
		if err != nil {
			return nil, err
		}
		out = append(out, "--internal-pid-file", abs)
	}
	return out, nil
}

// Exec executes an additional process inside the container based on a full OCI
// Process specification.
func (r *Runsc) Exec(context context.Context, id string, spec specs.Process, opts *ExecOpts) error {
	f, err := os.CreateTemp(os.Getenv("XDG_RUNTIME_DIR"), "runsc-process")
	if err != nil {
		return err
	}
	defer os.Remove(f.Name())
	err = json.NewEncoder(f).Encode(spec)
	f.Close()
	if err != nil {
		return err
	}
	args := []string{"exec", "--process", f.Name()}
	if opts != nil {
		oargs, err := opts.args()
		if err != nil {
			return err
		}
		args = append(args, oargs...)
	}
	cmd := r.command(context, append(args, id)...)
	if opts != nil && opts.IO != nil {
		opts.Set(cmd)
	}
	if cmd.Stdout == nil && cmd.Stderr == nil {
		out, _, err := cmdOutput(cmd, true)
		if err != nil {
			return fmt.Errorf("%w: %s", err, out)
		}
		return nil
	}
	ec, err := Monitor.Start(cmd)
	if err != nil {
		return err
	}
	if opts != nil && opts.IO != nil {
		if c, ok := opts.IO.(runc.StartCloser); ok {
			if err := c.CloseAfterStart(); err != nil {
				return err
			}
		}
	}
	status, err := Monitor.Wait(cmd, ec)
	if err == nil && status != 0 {
		err = fmt.Errorf("%s did not terminate successfully", cmd.Args[0])
	}
	return err
}

// Run runs the create, start, delete lifecycle of the container and returns
// its exit status after it has exited.
func (r *Runsc) Run(context context.Context, id, bundle string, opts *CreateOpts) (int, error) {
	args := []string{"run", "--bundle", bundle}
	if opts != nil {
		oargs, err := opts.args()
		if err != nil {
			return -1, err
		}
		args = append(args, oargs...)
	}
	cmd := r.command(context, append(args, id)...)
	if opts != nil && opts.IO != nil {
		opts.Set(cmd)
	}
	ec, err := Monitor.Start(cmd)
	if err != nil {
		return -1, err
	}
	return Monitor.Wait(cmd, ec)
}

// DeleteOpts is a set of options to runsc.Delete().
type DeleteOpts struct {
	Force bool
}

func (o *DeleteOpts) args() (out []string) {
	if o.Force {
		out = append(out, "--force")
	}
	return out
}

// Delete deletes the container.
func (r *Runsc) Delete(context context.Context, id string, opts *DeleteOpts) error {
	args := []string{"delete"}
	if opts != nil {
		args = append(args, opts.args()...)
	}
	return r.runOrError(r.command(context, append(args, id)...))
}

// KillOpts specifies options for killing a container and its processes.
type KillOpts struct {
	All bool
	Pid int
}

func (o *KillOpts) args() (out []string) {
	if o.All {
		out = append(out, "--all")
	}
	if o.Pid != 0 {
		out = append(out, "--pid", strconv.Itoa(o.Pid))
	}
	return out
}

// Kill sends the specified signal to the container.
func (r *Runsc) Kill(context context.Context, id string, sig int, opts *KillOpts) error {
	args := []string{
		"kill",
	}
	if opts != nil {
		args = append(args, opts.args()...)
	}
	return r.runOrError(r.command(context, append(args, id, strconv.Itoa(sig))...))
}

// Stats return the stats for a container like cpu, memory, and I/O.
func (r *Runsc) Stats(context context.Context, id string) (*runc.Stats, error) {
	cmd := r.command(context, "events", "--stats", id)
	data, stderr, err := cmdOutput(cmd, false)
	if err != nil {
		return nil, fmt.Errorf("%w: %s", err, stderr)
	}
	var e runc.Event
	if err := json.Unmarshal(data, &e); err != nil {
		log.L.Debugf("Parsing events error: %v", err)
		return nil, err
	}
	log.L.Debugf("Stats returned, type: %s, stats: %+v", e.Type, e.Stats)
	if e.Type != "stats" {
		return nil, fmt.Errorf(`unexpected event type %q, wanted "stats"`, e.Type)
	}
	if e.Stats == nil {
		return nil, fmt.Errorf(`"runsc events -stat" succeeded but no stat was provided`)
	}
	return e.Stats, nil
}

// Events returns an event stream from runsc for a container with stats and OOM notifications.
func (r *Runsc) Events(context context.Context, id string, interval time.Duration) (chan *runc.Event, error) {
	cmd := r.command(context, "events", fmt.Sprintf("--interval=%ds", int(interval.Seconds())), id)
	rd, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	ec, err := Monitor.Start(cmd)
	if err != nil {
		rd.Close()
		return nil, err
	}
	var (
		dec = json.NewDecoder(rd)
		c   = make(chan *runc.Event, 128)
	)
	go func() {
		defer func() {
			close(c)
			rd.Close()
			Monitor.Wait(cmd, ec)
		}()
		for {
			var e runc.Event
			if err := dec.Decode(&e); err != nil {
				if err == io.EOF {
					return
				}
				e = runc.Event{
					Type: "error",
					Err:  err,
				}
			}
			c <- &e
		}
	}()
	return c, nil
}

// Ps lists all the processes inside the container returning their pids.
func (r *Runsc) Ps(context context.Context, id string) ([]int, error) {
	data, stderr, err := cmdOutput(r.command(context, "ps", "--format", "json", id), false)
	if err != nil {
		return nil, fmt.Errorf("%w: %s", err, stderr)
	}
	var pids []int
	if err := json.Unmarshal(data, &pids); err != nil {
		return nil, err
	}
	return pids, nil
}

// Top lists all the processes inside the container returning the full ps data.
func (r *Runsc) Top(context context.Context, id string) (*runc.TopResults, error) {
	data, stderr, err := cmdOutput(r.command(context, "ps", "--format", "table", id), false)
	if err != nil {
		return nil, fmt.Errorf("%w: %s", err, stderr)
	}

	topResults, err := runc.ParsePSOutput(data)
	if err != nil {
		return nil, fmt.Errorf("%s: ", err)
	}
	return topResults, nil
}

func (r *Runsc) args() []string {
	var args []string
	if r.Root != "" {
		args = append(args, fmt.Sprintf("--root=%s", r.Root))
	}
	if r.Log != "" {
		args = append(args, fmt.Sprintf("--log=%s", r.Log))
	}
	if r.LogFormat != "" {
		args = append(args, fmt.Sprintf("--log-format=%s", r.LogFormat))
	}
	if r.PanicLog != "" {
		args = append(args, fmt.Sprintf("--panic-log=%s", r.PanicLog))
	}
	for k, v := range r.Config {
		args = append(args, fmt.Sprintf("--%s=%s", k, v))
	}
	return args
}

// runOrError will run the provided command.
//
// If an error is encountered and neither Stdout or Stderr was set the error
// will be returned in the format of <error>: <stderr>.
func (r *Runsc) runOrError(cmd *exec.Cmd) error {
	if cmd.Stdout != nil || cmd.Stderr != nil {
		ec, err := Monitor.Start(cmd)
		if err != nil {
			return err
		}
		status, err := Monitor.Wait(cmd, ec)
		if err == nil && status != 0 {
			err = fmt.Errorf("%s did not terminate successfully", cmd.Args[0])
		}
		return err
	}
	out, _, err := cmdOutput(cmd, true)
	if err != nil {
		return fmt.Errorf("%w: %s", err, out)
	}
	return nil
}

func (r *Runsc) command(context context.Context, args ...string) *exec.Cmd {
	command := r.Command
	if command == "" {
		command = DefaultCommand
	}
	cmd := exec.CommandContext(context, command, append(r.args(), args...)...)
	cmd.SysProcAttr = &unix.SysProcAttr{
		Setpgid: r.Setpgid,
	}
	if r.PdeathSignal != 0 {
		cmd.SysProcAttr.Pdeathsig = r.PdeathSignal
	}

	return cmd
}

func cmdOutput(cmd *exec.Cmd, combined bool) ([]byte, []byte, error) {
	stdout := getBuf()
	defer putBuf(stdout)
	cmd.Stdout = stdout
	cmd.Stderr = stdout

	var stderr *bytes.Buffer
	if !combined {
		stderr = getBuf()
		defer putBuf(stderr)
		cmd.Stderr = stderr
	}
	ec, err := Monitor.Start(cmd)
	if err != nil {
		return nil, nil, err
	}

	status, err := Monitor.Wait(cmd, ec)
	if err == nil && status != 0 {
		err = fmt.Errorf("%q did not terminate successfully", cmd.Args[0])
	}
	if stderr == nil {
		return stdout.Bytes(), nil, err
	}
	return stdout.Bytes(), stderr.Bytes(), err
}
