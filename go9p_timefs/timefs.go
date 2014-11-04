// Copyright 2009 The Go9p Authors.  All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package main

import (
	"code.google.com/p/go9p/p"
	"code.google.com/p/go9p/p/srv"
	"flag"
	"fmt"
	"log"
	"net"
	"os/user"
	"time"
)

type Time struct {
	srv.File
}
type InfTime struct {
	srv.File
}

var addr = flag.String("s", "timefs", "/srv name")
var debug = flag.Bool("d", false, "print debug messages")
var debugall = flag.Bool("D", false, "print packets as well as debug messages")

func (*InfTime) Read(fid *srv.FFid, buf []byte, offset uint64) (int, error) {
	// push out time ignoring offset (infinite read)
	t := time.Now().String() + "\n"
	b := []byte(t)
	ml := len(b)
	if ml > len(buf) {
		ml = len(buf)
	}

	copy(buf, b[0:ml])
	return ml, nil
}

func (*Time) Read(fid *srv.FFid, buf []byte, offset uint64) (int, error) {
	b := []byte(time.Now().String())
	have := len(b)
	off := int(offset)

	if off >= have {
		return 0, nil
	}

	return copy(buf, b[off:]), nil
}

func main() {
	var err error
	var tm *Time
	var ntm *InfTime
	var s *srv.Fsrv
	var p9c net.Conn
	var ch chan bool
	var usr *user.User

	flag.Parse()
	root := new(srv.File)
	usr, err = user.Current()
	if err != nil {
		goto error
	}
	// log.Println("running as User: ", usr)
	err = root.Add(nil, "/", UserNone(usr.Uid), nil, p.DMDIR|0555, nil)
	if err != nil {
		goto error
	}

	tm = new(Time)
	err = tm.Add(root, "time", UserNone(usr.Uid), nil, 0444, tm)
	if err != nil {
		goto error
	}
	ntm = new(InfTime)
	err = ntm.Add(root, "inftime", UserNone(usr.Uid), nil, 0444, ntm)
	if err != nil {
		goto error
	}

	s = srv.NewFileSrv(root)
	s.Dotu = false
	s.Upool = NoneUsers{}

	if *debug {
		s.Debuglevel = 1
	}
	if *debugall {
		s.Debuglevel = 2
	}

	log.Println("starting tree")
	s.Start(s)
	p9c, err = NewSrvConn(*addr)
	if err != nil {
		goto error
	}
	log.Println("starting")
	s.NewConn(p9c)
	<- ch
	return

error:
	log.Println(fmt.Sprintf("Error: %s", err))
}

// implement p.User interface
type UserNone string
// implement p.Group interface
type GroupNone string
// implement p.Users interface
type NoneUsers []UserNone

func (u UserNone) Name() string {
	// log.Println("User.Name()")
	return string(u)
}

func (u UserNone) Id() int {
	// log.Println("User.Id()")
	return -1
}

func (u UserNone) Groups() []p.Group {
	// log.Println("User.Groups()")
	return []p.Group{GroupNone(string(u))}
}

func (u UserNone) IsMember(g p.Group) bool {
	// log.Println("User.IsMember: Group=", g)
	return true
}

func (g GroupNone) Name() string {
	// log.Println("Group.Name()")
	return string(g)
}

func (g GroupNone) Id() int {
	// log.Println("Group.Id()")
	return -1
}

func (g GroupNone) Members() []p.User {
	// log.Println("Group.Members()")
	return []p.User{UserNone(string(g))}
}

func (u NoneUsers) Uid2User(uid int) p.User {
	// log.Println("Uid2User: uid=", uid)
	return UserNone("none")
}

func (u NoneUsers) Uname2User(uname string) p.User {
	// log.Println("Uname2User: uname=", uname)
	return UserNone(uname)
}

func (u NoneUsers) Gid2Group(gid int) p.Group {
	// log.Println("Gid2Group: gid=", gid)
	return GroupNone("none")
}

func (u NoneUsers) Gname2Group(gname string) p.Group {
	// log.Println("Gname2Group: gname=", gname)
	return GroupNone(gname)
}
