package main

import (
	"crypto/tls"
	"encoding/json"
	"io"
	"io/ioutil"
	"log"
	"net"
	"runtime"
	"strconv"
	"strings"
)

const defconf = "{\n" +
	"  \"ssl\": false,\n" +
	"  \"bindport\": 1080,\n" +
	"  \"serveraddr\": \"proxyserver:443\",\n" +
	"  \"needauth\": false,\n" +
	"  \"httphost\": \"localhost\",\n" +
	"  \"httppath\": \"/\",\n" +
	"  \"key\": \"65f5bb36-8a0a-4be4-b0d0-18dee527b2d8\",\n" +
	"  \"connectmode\": false,\n" +
	"  \"targetaddr\": \"targetserver:443\"\n" +
	"}"

type Config struct {
	Ssl         bool
	BindPort    int
	ServerAddr  string
	NeedAuth    bool
	ConnectMode bool
}

var c Config
var auth []byte
var authlen int
var connproxy []byte
var connproxylen int

func initconfigdata() int {
	f, err := ioutil.ReadFile("config.json")
	if err != nil {
		err = ioutil.WriteFile("config.json", []byte(defconf), 0777)
		if err != nil {
			log.Println(err)
			return -1
		}
		f = []byte(defconf)
	}
	var obj map[string]interface{}
	err = json.Unmarshal(f, &obj)
	if err != nil {
		log.Println(err)
		return -2
	}
	c.Ssl = obj["ssl"].(bool)
	c.BindPort = int(obj["bindport"].(float64))
	c.ServerAddr = obj["serveraddr"].(string)
	c.NeedAuth = obj["needauth"].(bool)
	c.ConnectMode = obj["connectmode"].(bool)
	httphost := obj["httphost"].(string)
	httppath := obj["httppath"].(string)
	key := obj["key"].(string)
	if c.NeedAuth {
		auth = []byte("GET " + httppath + " HTTP/1.1\r\nHost: " + httphost + "\r\nConnection: Upgrade\r\nPragma: no-cache\r\nCache-Control: no-cache\r\nUpgrade: websocket\r\nAuthorization: " + key + "\r\n\r\n")
		authlen = len(auth)
	}
	colon := strings.Index(c.ServerAddr, ":")
	if colon == -1 {
		if c.Ssl {
			c.ServerAddr += ":443"
		} else {
			c.ServerAddr += ":80"
		}
	}
	targetaddr := obj["targetaddr"].(string)
	if c.ConnectMode {
		connproxy = []byte("CONNECT " + targetaddr + " HTTP/1.1\r\nHost: " + targetaddr + "\r\nProxy-Connection: keep-alive\r\n\r\n")
		connproxylen = len(connproxy)
	}
	return 0
}

func main() {
	runtime.GOMAXPROCS(1)

	log.SetFlags(log.LstdFlags | log.Lshortfile)
	log.Println("version: " + runtime.Version())

	if initconfigdata() != 0 {
		return
	}

	ln, err := net.Listen("tcp", ":"+strconv.Itoa(c.BindPort))
	if err != nil {
		log.Println(err)
		return
	}
	for {
		client, err := ln.Accept()
		if err != nil {
			log.Println(err)
			ln.Close()
			return
		}
		go ProxyRequest(client)
	}
}

func ProxyRequest(client net.Conn) {
	var server net.Conn
	if c.Ssl {
		conf := &tls.Config{
			InsecureSkipVerify: true,
		}
		d := &tls.Dialer{
			Config: conf,
		}
		s, err := d.Dial("tcp", c.ServerAddr)
		if err != nil {
			log.Println(err)
			client.Close()
			return
		}
		server = s
	} else {
		s, err := net.Dial("tcp", c.ServerAddr)
		if err != nil {
			log.Println(err)
			client.Close()
			return
		}
		server = s
	}

	if c.NeedAuth {
		_, err := server.Write(auth)
		if err != nil {
			log.Println(err)
			client.Close()
			server.Close()
			return
		}
		b := make([]byte, 32*1024)
		n, err := server.Read(b)
		if err != nil {
			log.Println(err)
			client.Close()
			server.Close()
			return
		}
		if string(b[:34]) != "HTTP/1.1 101 Switching Protocols\r\n" {
			log.Println(string(b[:n]))
			client.Close()
			server.Close()
			return
		}
	}
	if c.ConnectMode {
		_, err := server.Write(connproxy)
		if err != nil {
			log.Println(err)
			client.Close()
			server.Close()
			return
		}
		b := make([]byte, 32*1024)
		n, err := server.Read(b)
		if err != nil {
			log.Println(err)
			client.Close()
			server.Close()
			return
		}
		if string(b[:37]) != "HTTP/1.1 200 Connection established\r\n" {
			log.Println(string(b[:n]))
			client.Close()
			server.Close()
			return
		}
	}

	go IoCopy(client, server)
	go IoCopy(server, client)
}

func IoCopy(dst net.Conn, src net.Conn) {
	io.Copy(dst, src)
	dst.Close()
}
