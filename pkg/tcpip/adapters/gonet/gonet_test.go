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

package gonet

import (
	"context"
	"fmt"
	"io"
	"net"
	"reflect"
	"strings"
	"testing"
	"time"

	"golang.org/x/net/nettest"
	"gvisor.dev/gvisor/pkg/sync"
	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/link/loopback"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv4"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv6"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/udp"
	"gvisor.dev/gvisor/pkg/waiter"
)

const (
	NICID = 1
)

func TestTimeouts(t *testing.T) {
	nc := NewTCPConn(nil, nil)
	dlfs := []struct {
		name string
		f    func(time.Time) error
	}{
		{"SetDeadline", nc.SetDeadline},
		{"SetReadDeadline", nc.SetReadDeadline},
		{"SetWriteDeadline", nc.SetWriteDeadline},
	}

	for _, dlf := range dlfs {
		if err := dlf.f(time.Time{}); err != nil {
			t.Errorf("got %s(time.Time{}) = %v, want = %v", dlf.name, err, nil)
		}
	}
}

func newLoopbackStack() (*stack.Stack, tcpip.Error) {
	// Create the stack and add a NIC.
	s := stack.New(stack.Options{
		NetworkProtocols:   []stack.NetworkProtocolFactory{ipv4.NewProtocol, ipv6.NewProtocol},
		TransportProtocols: []stack.TransportProtocolFactory{tcp.NewProtocol, udp.NewProtocol},
	})

	if err := s.CreateNIC(NICID, loopback.New()); err != nil {
		return nil, err
	}

	// Add default route.
	s.SetRouteTable([]tcpip.Route{
		// IPv4
		{
			Destination: header.IPv4EmptySubnet,
			NIC:         NICID,
		},

		// IPv6
		{
			Destination: header.IPv6EmptySubnet,
			NIC:         NICID,
		},
	})

	return s, nil
}

type testConnection struct {
	wq *waiter.Queue
	e  *waiter.Entry
	ch chan struct{}
	ep tcpip.Endpoint
}

func connect(s *stack.Stack, addr tcpip.FullAddress) (*testConnection, tcpip.Error) {
	wq := &waiter.Queue{}
	ep, err := s.NewEndpoint(tcp.ProtocolNumber, ipv4.ProtocolNumber, wq)
	if err != nil {
		return nil, err
	}

	entry, ch := waiter.NewChannelEntry(waiter.WritableEvents)
	wq.EventRegister(&entry)

	err = ep.Connect(addr)
	if _, ok := err.(*tcpip.ErrConnectStarted); ok {
		<-ch
		err = ep.LastError()
	}
	if err != nil {
		return nil, err
	}

	wq.EventUnregister(&entry)
	entry, ch = waiter.NewChannelEntry(waiter.ReadableEvents)
	wq.EventRegister(&entry)

	return &testConnection{wq, &entry, ch, ep}, nil
}

func (c *testConnection) close() {
	c.wq.EventUnregister(c.e)
	c.ep.Close()
}

// TestCloseReader tests that Conn.Close() causes Conn.Read() to unblock.
func TestCloseReader(t *testing.T) {
	s, err := newLoopbackStack()
	if err != nil {
		t.Fatalf("newLoopbackStack() = %v", err)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	addr := tcpip.FullAddress{
		NIC:  NICID,
		Addr: tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4()),
		Port: 11211,
	}
	protocolAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: addr.Addr.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
	}

	l, e := ListenTCP(s, addr, ipv4.ProtocolNumber)
	if e != nil {
		t.Fatalf("NewListener() = %v", e)
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		c, err := l.Accept()
		if err != nil {
			t.Errorf("l.Accept() = %v", err)
			// Cannot call Fatalf in goroutine. Just return from the goroutine.
			return
		}

		// Give c.Read() a chance to block before closing the connection.
		time.AfterFunc(time.Millisecond*50, func() {
			c.Close()
		})

		buf := make([]byte, 256)
		n, err := c.Read(buf)
		if n != 0 || err != io.EOF {
			t.Errorf("c.Read() = (%d, %v), want (0, EOF)", n, err)
		}
	}()
	sender, err := connect(s, addr)
	if err != nil {
		t.Fatalf("connect() = %v", err)
	}

	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Errorf("c.Read() didn't unblock")
	}
	sender.close()
}

// TestCloseReaderWithForwarder tests that TCPConn.Close wakes TCPConn.Read when
// using tcp.Forwarder.
func TestCloseReaderWithForwarder(t *testing.T) {
	s, err := newLoopbackStack()
	if err != nil {
		t.Fatalf("newLoopbackStack() = %v", err)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	addr := tcpip.FullAddress{
		NIC:  NICID,
		Addr: tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4()),
		Port: 11211,
	}
	protocolAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: addr.Addr.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
	}

	done := make(chan struct{})

	fwd := tcp.NewForwarder(s, 30000, 10, func(r *tcp.ForwarderRequest) {
		defer close(done)

		var wq waiter.Queue
		ep, err := r.CreateEndpoint(&wq)
		if err != nil {
			t.Fatalf("r.CreateEndpoint() = %v", err)
		}
		defer ep.Close()
		r.Complete(false)

		c := NewTCPConn(&wq, ep)

		// Give c.Read() a chance to block before closing the connection.
		time.AfterFunc(time.Millisecond*50, func() {
			c.Close()
		})

		buf := make([]byte, 256)
		n, e := c.Read(buf)
		if n != 0 || e != io.EOF {
			t.Errorf("c.Read() = (%d, %v), want (0, EOF)", n, e)
		}
	})
	s.SetTransportProtocolHandler(tcp.ProtocolNumber, fwd.HandlePacket)

	sender, err := connect(s, addr)
	if err != nil {
		t.Fatalf("connect() = %v", err)
	}

	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Errorf("c.Read() didn't unblock")
	}
	sender.close()
}

func TestCloseRead(t *testing.T) {
	s, terr := newLoopbackStack()
	if terr != nil {
		t.Fatalf("newLoopbackStack() = %v", terr)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	addr := tcpip.FullAddress{
		NIC:  NICID,
		Addr: tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4()),
		Port: 11211,
	}
	protocolAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: addr.Addr.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
	}

	done := make(chan struct{})
	fwd := tcp.NewForwarder(s, 30000, 10, func(r *tcp.ForwarderRequest) {
		var wq waiter.Queue
		_, err := r.CreateEndpoint(&wq)
		if err != nil {
			t.Fatalf("r.CreateEndpoint() = %v", err)
		}
		close(done)
		// Endpoint will be closed in deferred s.Close (above).
	})

	s.SetTransportProtocolHandler(tcp.ProtocolNumber, fwd.HandlePacket)

	tc, terr := connect(s, addr)
	if terr != nil {
		t.Fatalf("connect() = %v", terr)
	}
	c := NewTCPConn(tc.wq, tc.ep)

	if err := c.CloseRead(); err != nil {
		t.Errorf("c.CloseRead() = %v", err)
	}

	buf := make([]byte, 256)
	if n, err := c.Read(buf); err != io.EOF {
		t.Errorf("c.Read() = (%d, %v), want (0, io.EOF)", n, err)
	}

	if n, err := c.Write([]byte("abc123")); n != 6 || err != nil {
		t.Errorf("c.Write() = (%d, %v), want (6, nil)", n, err)
	}

	select {
	case <-done:
	case <-time.After(1 * time.Second):
		t.Fatalf("timed out waiting for r.CreateEndpoint(...) to complete")
	}
}

func TestCloseWrite(t *testing.T) {
	s, terr := newLoopbackStack()
	if terr != nil {
		t.Fatalf("newLoopbackStack() = %v", terr)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	addr := tcpip.FullAddress{
		NIC:  NICID,
		Addr: tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4()),
		Port: 11211,
	}
	protocolAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: addr.Addr.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
	}

	fwd := tcp.NewForwarder(s, 30000, 10, func(r *tcp.ForwarderRequest) {
		var wq waiter.Queue
		ep, err := r.CreateEndpoint(&wq)
		if err != nil {
			t.Fatalf("r.CreateEndpoint() = %v", err)
		}
		defer ep.Close()
		r.Complete(false)

		c := NewTCPConn(&wq, ep)

		n, e := c.Read(make([]byte, 256))
		if n != 0 || e != io.EOF {
			t.Errorf("c.Read() = (%d, %v), want (0, io.EOF)", n, e)
		}

		if n, e = c.Write([]byte("abc123")); n != 6 || e != nil {
			t.Errorf("c.Write() = (%d, %v), want (6, nil)", n, e)
		}
	})

	s.SetTransportProtocolHandler(tcp.ProtocolNumber, fwd.HandlePacket)

	tc, terr := connect(s, addr)
	if terr != nil {
		t.Fatalf("connect() = %v", terr)
	}
	c := NewTCPConn(tc.wq, tc.ep)

	if err := c.CloseWrite(); err != nil {
		t.Errorf("c.CloseWrite() = %v", err)
	}

	buf := make([]byte, 256)
	n, err := c.Read(buf)
	if err != nil || string(buf[:n]) != "abc123" {
		t.Fatalf("c.Read() = (%d, %v), want (6, nil)", n, err)
	}

	n, err = c.Write([]byte("abc123"))
	got, ok := err.(*net.OpError)
	want := "endpoint is closed for send"
	if n != 0 || !ok || got.Op != "write" || got.Err == nil || !strings.HasSuffix(got.Err.Error(), want) {
		t.Errorf("c.Write() = (%d, %v), want (0, OpError(Op: write, Err: %s))", n, err, want)
	}
}

// TestCloseStack tests that stack.Close wakes TCPConn.Read when
// using tcp.Forwarder.
func TestCloseStack(t *testing.T) {
	s, err := newLoopbackStack()
	if err != nil {
		t.Fatalf("newLoopbackStack() = %v", err)
	}

	addr := tcpip.FullAddress{
		NIC:  NICID,
		Addr: tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4()),
		Port: 11211,
	}
	protocolAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: addr.Addr.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
	}

	done := make(chan struct{})

	fwd := tcp.NewForwarder(s, 30000, 10, func(r *tcp.ForwarderRequest) {
		defer close(done)

		var wq waiter.Queue
		ep, err := r.CreateEndpoint(&wq)
		if err != nil {
			t.Fatalf("r.CreateEndpoint() = %v", err)
		}
		r.Complete(false)

		c := NewTCPConn(&wq, ep)

		// Give c.Read() a chance to block before closing the stack.
		time.AfterFunc(50*time.Millisecond, func() {
			s.Close()
			s.Wait()
		})

		buf := make([]byte, 256)
		n, e := c.Read(buf)
		// Depending on the ordering of Close and Read, we should get
		// one of two errors.
		if n != 0 || (!strings.Contains(e.Error(), "operation aborted") && !strings.Contains(e.Error(), "connection reset by peer")) {
			t.Errorf("c.Read() = (%d, %v), want (0, operation aborted) or (0, connection reset by peer)", n, e)
		}
	})
	s.SetTransportProtocolHandler(tcp.ProtocolNumber, fwd.HandlePacket)

	sender, err := connect(s, addr)
	if err != nil {
		t.Fatalf("connect() = %v", err)
	}

	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Errorf("c.Read() didn't unblock")
	}
	sender.close()
}

func TestUDPForwarder(t *testing.T) {
	s, terr := newLoopbackStack()
	if terr != nil {
		t.Fatalf("newLoopbackStack() = %v", terr)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	ip1 := tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4())
	addr1 := tcpip.FullAddress{NIC: NICID, Addr: ip1, Port: 11211}
	protocolAddr1 := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: ip1.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr1, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr1, err)
	}
	ip2 := tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 2).To4())
	addr2 := tcpip.FullAddress{NIC: NICID, Addr: ip2, Port: 11311}
	protocolAddr2 := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: ip2.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr2, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr2, err)
	}

	done := make(chan struct{})
	fwd := udp.NewForwarder(s, func(r *udp.ForwarderRequest) bool {
		defer close(done)

		var wq waiter.Queue
		ep, err := r.CreateEndpoint(&wq)
		if err != nil {
			t.Fatalf("r.CreateEndpoint() = %v", err)
		}
		defer ep.Close()

		c := NewUDPConn(&wq, ep)

		buf := make([]byte, 256)
		n, e := c.Read(buf)
		if e != nil {
			t.Errorf("c.Read() = %v", e)
			return true
		}

		if _, e := c.Write(buf[:n]); e != nil {
			t.Errorf("c.Write() = %v", e)
			return true
		}

		return true
	})
	s.SetTransportProtocolHandler(udp.ProtocolNumber, fwd.HandlePacket)

	c2, err := DialUDP(s, &addr2, nil, ipv4.ProtocolNumber)
	if err != nil {
		t.Fatal("DialUDP(bind port 5):", err)
	}

	sent := "abc123"
	sendAddr := fullToUDPAddr(addr1)
	if n, err := c2.WriteTo([]byte(sent), sendAddr); err != nil || n != len(sent) {
		t.Errorf("c1.WriteTo(%q, %v) = %d, %v, want = %d, %v", sent, sendAddr, n, err, len(sent), nil)
	}

	buf := make([]byte, 256)
	n, recvAddr, err := c2.ReadFrom(buf)
	if err != nil || recvAddr.String() != sendAddr.String() {
		t.Errorf("c1.ReadFrom() = %d, %v, %v, want = %d, %v, %v", n, recvAddr, err, len(sent), sendAddr, nil)
	}
}

// TestDeadlineChange tests that changing the deadline affects currently blocked reads.
func TestDeadlineChange(t *testing.T) {
	s, err := newLoopbackStack()
	if err != nil {
		t.Fatalf("newLoopbackStack() = %v", err)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	addr := tcpip.FullAddress{
		NIC:  NICID,
		Addr: tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4()),
		Port: 11211,
	}

	protocolAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: addr.Addr.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
	}

	l, e := ListenTCP(s, addr, ipv4.ProtocolNumber)
	if e != nil {
		t.Fatalf("NewListener() = %v", e)
	}
	done := make(chan struct{})
	go func() {
		defer close(done)
		c, err := l.Accept()
		if err != nil {
			t.Errorf("l.Accept() = %v", err)
			// Cannot call Fatalf in goroutine. Just return from the goroutine.
			return
		}

		c.SetDeadline(time.Now().Add(time.Minute))
		// Give c.Read() a chance to block before closing the connection.
		time.AfterFunc(time.Millisecond*50, func() {
			c.SetDeadline(time.Now().Add(time.Millisecond * 10))
		})

		buf := make([]byte, 256)
		n, err := c.Read(buf)
		got, ok := err.(*net.OpError)
		want := "i/o timeout"
		if n != 0 || !ok || got.Err == nil || got.Err.Error() != want {
			t.Errorf("c.Read() = (%d, %v), want (0, OpError(%s))", n, err, want)
		}
	}()
	sender, err := connect(s, addr)
	if err != nil {
		t.Fatalf("connect() = %v", err)
	}

	select {
	case <-done:
	case <-time.After(time.Millisecond * 500):
		t.Errorf("c.Read() didn't unblock")
	}
	sender.close()
}

func TestPacketConnTransfer(t *testing.T) {
	s, e := newLoopbackStack()
	if e != nil {
		t.Fatalf("newLoopbackStack() = %v", e)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	ip1 := tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4())
	addr1 := tcpip.FullAddress{NIC: NICID, Addr: ip1, Port: 11211}
	protocolAddr1 := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: ip1.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr1, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr1, err)
	}
	ip2 := tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 2).To4())
	addr2 := tcpip.FullAddress{NIC: NICID, Addr: ip2, Port: 11311}
	protocolAddr2 := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: ip2.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr2, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr2, err)
	}

	c1, err := DialUDP(s, &addr1, nil, ipv4.ProtocolNumber)
	if err != nil {
		t.Fatal("DialUDP(bind port 4):", err)
	}
	c2, err := DialUDP(s, &addr2, nil, ipv4.ProtocolNumber)
	if err != nil {
		t.Fatal("DialUDP(bind port 5):", err)
	}

	c1.SetDeadline(time.Now().Add(time.Second))
	c2.SetDeadline(time.Now().Add(time.Second))

	sent := "abc123"
	sendAddr := fullToUDPAddr(addr2)
	if n, err := c1.WriteTo([]byte(sent), sendAddr); err != nil || n != len(sent) {
		t.Errorf("got c1.WriteTo(%q, %v) = %d, %v, want = %d, %v", sent, sendAddr, n, err, len(sent), nil)
	}
	recv := make([]byte, len(sent))
	n, recvAddr, err := c2.ReadFrom(recv)
	if err != nil || n != len(recv) {
		t.Errorf("got c2.ReadFrom() = %d, %v, want = %d, %v", n, err, len(recv), nil)
	}

	if recv := string(recv); recv != sent {
		t.Errorf("got recv = %q, want = %q", recv, sent)
	}

	if want := fullToUDPAddr(addr1); !reflect.DeepEqual(recvAddr, want) {
		t.Errorf("got recvAddr = %v, want = %v", recvAddr, want)
	}

	if err := c1.Close(); err != nil {
		t.Error("c1.Close():", err)
	}
	if err := c2.Close(); err != nil {
		t.Error("c2.Close():", err)
	}
}

func TestConnectedPacketConnTransfer(t *testing.T) {
	s, e := newLoopbackStack()
	if e != nil {
		t.Fatalf("newLoopbackStack() = %v", e)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	ip := tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4())
	addr := tcpip.FullAddress{NIC: NICID, Addr: ip, Port: 11211}
	protocolAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: ip.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
	}

	c1, err := DialUDP(s, &addr, nil, ipv4.ProtocolNumber)
	if err != nil {
		t.Fatal("DialUDP(bind port 4):", err)
	}
	c2, err := DialUDP(s, nil, &addr, ipv4.ProtocolNumber)
	if err != nil {
		t.Fatal("DialUDP(bind port 5):", err)
	}

	c1.SetDeadline(time.Now().Add(time.Second))
	c2.SetDeadline(time.Now().Add(time.Second))

	sent := "abc123"
	if n, err := c2.Write([]byte(sent)); err != nil || n != len(sent) {
		t.Errorf("got c2.Write(%q) = %d, %v, want = %d, %v", sent, n, err, len(sent), nil)
	}
	recv := make([]byte, len(sent))
	n, err := c1.Read(recv)
	if err != nil || n != len(recv) {
		t.Errorf("got c1.Read() = %d, %v, want = %d, %v", n, err, len(recv), nil)
	}

	if recv := string(recv); recv != sent {
		t.Errorf("got recv = %q, want = %q", recv, sent)
	}

	if err := c1.Close(); err != nil {
		t.Error("c1.Close():", err)
	}
	if err := c2.Close(); err != nil {
		t.Error("c2.Close():", err)
	}
}

func makePipe() (c1, c2 net.Conn, stop func(), err error) {
	s, e := newLoopbackStack()
	if e != nil {
		return nil, nil, nil, fmt.Errorf("newLoopbackStack() = %v", e)
	}

	ip := tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4())
	addr := tcpip.FullAddress{NIC: NICID, Addr: ip, Port: 11211}
	protocolAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: ip.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
		return nil, nil, nil, fmt.Errorf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
	}

	l, err := ListenTCP(s, addr, ipv4.ProtocolNumber)
	if err != nil {
		return nil, nil, nil, fmt.Errorf("NewListener: %w", err)
	}

	c1, err = DialTCP(s, addr, ipv4.ProtocolNumber)
	if err != nil {
		l.Close()
		return nil, nil, nil, fmt.Errorf("DialTCP: %w", err)
	}

	c2, err = l.Accept()
	if err != nil {
		l.Close()
		c1.Close()
		return nil, nil, nil, fmt.Errorf("l.Accept: %w", err)
	}

	stop = func() {
		c1.Close()
		c2.Close()
		s.Close()
		s.Wait()
	}

	if err := l.Close(); err != nil {
		stop()
		return nil, nil, nil, fmt.Errorf("l.Close(): %w", err)
	}

	return c1, c2, stop, nil
}

func TestTCPConnTransfer(t *testing.T) {
	c1, c2, _, err := makePipe()
	if err != nil {
		t.Fatal(err)
	}
	defer func() {
		if err := c1.Close(); err != nil {
			t.Error("c1.Close():", err)
		}
		if err := c2.Close(); err != nil {
			t.Error("c2.Close():", err)
		}
	}()

	c1.SetDeadline(time.Now().Add(time.Second))
	c2.SetDeadline(time.Now().Add(time.Second))

	const sent = "abc123"

	tests := []struct {
		name string
		c1   net.Conn
		c2   net.Conn
	}{
		{"connected to accepted", c1, c2},
		{"accepted to connected", c2, c1},
	}

	for _, test := range tests {
		if n, err := test.c1.Write([]byte(sent)); err != nil || n != len(sent) {
			t.Errorf("%s: got test.c1.Write(%q) = %d, %v, want = %d, %v", test.name, sent, n, err, len(sent), nil)
			continue
		}

		recv := make([]byte, len(sent))
		n, err := test.c2.Read(recv)
		if err != nil || n != len(recv) {
			t.Errorf("%s: got test.c2.Read() = %d, %v, want = %d, %v", test.name, n, err, len(recv), nil)
			continue
		}

		if recv := string(recv); recv != sent {
			t.Errorf("%s: got recv = %q, want = %q", test.name, recv, sent)
		}
	}
}

func TestTCPDialError(t *testing.T) {
	s, e := newLoopbackStack()
	if e != nil {
		t.Fatalf("newLoopbackStack() = %v", e)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	ip := tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4())
	addr := tcpip.FullAddress{NIC: NICID, Addr: ip, Port: 11211}

	switch _, err := DialTCP(s, addr, ipv4.ProtocolNumber); err := err.(type) {
	case *net.OpError:
		if err.Err.Error() != (&tcpip.ErrHostUnreachable{}).String() {
			t.Errorf("got DialTCP() = %s, want = %s", err, &tcpip.ErrHostUnreachable{})
		}
	default:
		t.Errorf("got DialTCP(...) = %v, want %s", err, &tcpip.ErrHostUnreachable{})
	}
}

func TestDialContextTCPCanceled(t *testing.T) {
	s, err := newLoopbackStack()
	if err != nil {
		t.Fatalf("newLoopbackStack() = %v", err)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	addr := tcpip.FullAddress{
		NIC:  NICID,
		Addr: tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4()),
		Port: 11211,
	}
	protocolAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: addr.Addr.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
	}

	ctx := context.Background()
	ctx, cancel := context.WithCancel(ctx)
	cancel()

	if _, err := DialContextTCP(ctx, s, addr, ipv4.ProtocolNumber); err != context.Canceled {
		t.Errorf("got DialContextTCP(...) = %v, want = %v", err, context.Canceled)
	}
}

func TestDialContextTCPTimeout(t *testing.T) {
	s, err := newLoopbackStack()
	if err != nil {
		t.Fatalf("newLoopbackStack() = %v", err)
	}
	defer func() {
		s.Close()
		s.Wait()
	}()

	addr := tcpip.FullAddress{
		NIC:  NICID,
		Addr: tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4()),
		Port: 11211,
	}
	protocolAddr := tcpip.ProtocolAddress{
		Protocol:          ipv4.ProtocolNumber,
		AddressWithPrefix: addr.Addr.WithPrefix(),
	}
	if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
		t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
	}

	fwd := tcp.NewForwarder(s, 30000, 10, func(r *tcp.ForwarderRequest) {
		time.Sleep(time.Second)
		r.Complete(true)
	})
	s.SetTransportProtocolHandler(tcp.ProtocolNumber, fwd.HandlePacket)

	ctx := context.Background()
	ctx, cancel := context.WithDeadline(ctx, time.Now().Add(100*time.Millisecond))
	defer cancel()

	if _, err := DialContextTCP(ctx, s, addr, ipv4.ProtocolNumber); err != context.DeadlineExceeded {
		t.Errorf("got DialContextTCP(...) = %v, want = %v", err, context.DeadlineExceeded)
	}
}

// TestInterruptListender tests that (*TCPListener).Accept can be interrupted.
func TestInterruptListender(t *testing.T) {
	for _, test := range []struct {
		name string
		stop func(l *TCPListener) error
	}{
		{
			"Close",
			(*TCPListener).Close,
		},
		{
			"Shutdown",
			func(l *TCPListener) error {
				l.Shutdown()
				return nil
			},
		},
		{
			"Double Shutdown",
			func(l *TCPListener) error {
				l.Shutdown()
				l.Shutdown()
				return nil
			},
		},
	} {
		t.Run(test.name, func(t *testing.T) {
			s, err := newLoopbackStack()
			if err != nil {
				t.Fatalf("newLoopbackStack() = %v", err)
			}
			defer func() {
				s.Close()
				s.Wait()
			}()

			addr := tcpip.FullAddress{
				NIC:  NICID,
				Addr: tcpip.AddrFromSlice(net.IPv4(169, 254, 10, 1).To4()),
				Port: 11211,
			}
			protocolAddr := tcpip.ProtocolAddress{
				Protocol:          ipv4.ProtocolNumber,
				AddressWithPrefix: addr.Addr.WithPrefix(),
			}
			if err := s.AddProtocolAddress(NICID, protocolAddr, stack.AddressProperties{}); err != nil {
				t.Fatalf("AddProtocolAddress(%d, %+v, {}): %s", NICID, protocolAddr, err)
			}

			l, e := ListenTCP(s, addr, ipv4.ProtocolNumber)
			if e != nil {
				t.Fatalf("NewListener() = %v", e)
			}
			defer l.Close()
			done := make(chan struct{})
			go func() {
				defer close(done)
				c, err := l.Accept()
				if err != nil {
					// Accept is expected to return an error.
					t.Log("Accept #1:", err)
					return
				}
				t.Errorf("Accept #1 returned a connection: %v -> %v", c.LocalAddr(), c.RemoteAddr())
				c.Close()
			}()

			// Give l.Accept a chance to block before stopping it.
			time.Sleep(time.Millisecond * 50)

			if err := test.stop(l); err != nil {
				t.Error("stop:", err)
			}

			select {
			case <-done:
			case <-time.After(5 * time.Second):
				t.Errorf("c.Accept didn't unblock")
			}

			done = make(chan struct{})
			go func() {
				defer close(done)
				c, err := l.Accept()
				if err != nil {
					// Accept is expected to return an error.
					t.Log("Accept #2:", err)
					return
				}
				t.Errorf("Accept #2 returned a connection: %v -> %v", c.LocalAddr(), c.RemoteAddr())
				c.Close()
			}()

			select {
			case <-done:
			case <-time.After(5 * time.Second):
				t.Errorf("c.Accept didn't unblock a second time")
			}
		})
	}
}

func TestNetTest(t *testing.T) {
	nettest.TestConn(t, makePipe)
}

// NOTE(gvisor.dev/issue/9885): Regression test.
func TestDeadlineTimerAfterZeroValue(t *testing.T) {
	timer := &deadlineTimer{}
	timer.init()

	wg := sync.WaitGroup{}
	ch := timer.readCancel()
	wg.Add(1)
	go func() {
		defer wg.Done()
		select {
		case <-ch:
		case <-time.After(1 * time.Second):
			t.Fail()
		}
	}()
	timer.SetReadDeadline(time.Now().Add(10 * time.Second))
	timer.SetReadDeadline(time.Time{})
	timer.SetReadDeadline(time.Unix(1, 0))
	wg.Wait()
}
