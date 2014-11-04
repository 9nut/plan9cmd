package main

import (
	"fmt"
	"errors"
	"net"
	"os"
	"syscall"
	"time"
)

type srvAddr string

type SrvConn struct {
	Name string
	SrvFile *os.File
	file *os.File
}

func NewSrvConn(srvname string) (net.Conn, error) {
	var pip [2]int

	err := syscall.Pipe(pip[:])
	if err != nil {
		return nil, err
	}
	srvfile, err := PostFD(srvname, pip[1])
	if err != nil {
		syscall.Close(pip[0])
		syscall.Close(pip[1])
		return nil, err
	}
	syscall.Close(pip[1])
	f := os.NewFile(uintptr(pip[0]), "|0")

	return &SrvConn{Name: srvname, SrvFile: srvfile, file : f}, nil
}

// Post a file's descriptor to /srv/name -- Plan 9 specific
func PostFD(name string, fd int) (srv *os.File, err error) {
	fname := "/srv/"+name
	// Plan 9 flag OWRITE|ORCLOSE|OCEXEC
	srvfd, err := syscall.Create(fname, 0x01|0x40|0x20, 0600)
	if err != nil {
		return
	}
	srv = os.NewFile(uintptr(srvfd), fname)

	_, err = fmt.Fprintf(srv, "%d", fd)
	return
}

func (a srvAddr) Network() string {
	return "/srv/"+string(a)
}

func (a srvAddr) String() string {
	return "/srv/"+string(a)
}

func (p *SrvConn) LocalAddr() net.Addr {
	return srvAddr(p.Name)
}

func (p *SrvConn) RemoteAddr() net.Addr {
	return srvAddr(p.Name)
}

func (p *SrvConn) SetDeadline(t time.Time) error {
	return errors.New("SrvConn does not support deadlines")
}

func (p *SrvConn) SetReadDeadline(t time.Time) error {
	return errors.New("SrvConn does not support deadlines")
}

func (p *SrvConn) SetWriteDeadline(t time.Time) error {
	return errors.New("SrvConn does not support deadlines")
}

func (p *SrvConn) Read(data []byte) (int, error) {
	return p.file.Read(data)
}

func (p *SrvConn) Write(data []byte) (int, error) {
	return p.file.Write(data)
}

func (p *SrvConn) Close() error {
	return p.file.Close()
}
