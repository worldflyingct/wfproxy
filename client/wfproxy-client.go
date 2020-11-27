package main

import (
    "log"
    "crypto/tls"
    "net"
    "io"
    "runtime"
    "strings"
    "io/ioutil"
    "encoding/json"
)

const defconf = "{\n" +
                "  \"ssl\": false,\n" +
                "  \"bindaddr\": \":1080\",\n" +
                "  \"serveraddr\": \"proxyserver\",\n" +
                "  \"httphead\": false,\n" +
                "  \"path\": \"/\",\n" +
                "  \"key\": \"D5lfC6LQ1W0BwzP9x3TsxvvdYBCFznqk\",\n" +
                "  \"connectmode\": false,\n" +
                "  \"targetaddr\": \"targetserver:443\"\n" +
                "}"
type Config struct {
    Ssl bool
    BindAddr string
    ServerAddr string
    HttpHead bool
    Path string
    Key string
    ConnectMode bool
    TargetAddr string
}
var c Config
var auth []byte
var authlen int
var connproxy []byte
var connproxylen int

func main () {
    runtime.GOMAXPROCS(1)

    log.SetFlags(log.LstdFlags | log.Lshortfile)
    log.Println("version: " + runtime.Version())

    f, err := ioutil.ReadFile("config.json")
    if err != nil {
        ioutil.WriteFile("config.json", []byte(defconf), 0777)
        f = []byte(defconf)
    }
    err = json.Unmarshal(f, &c)
    if err != nil {
        log.Println("config file read error!!!")
        return
    }
    if c.HttpHead {
        auth = []byte("GET " + c.Path + " HTTP/1.1\r\nHost: " + c.ServerAddr + "\r\nConnection: Upgrade\r\nAuthorization: " + c.Key + "\r\n\r\n")
        authlen = len(auth)
    }
    if c.ConnectMode {
        connproxy = []byte("CONNECT " + c.TargetAddr + " HTTP/1.1\r\nHost: " + c.TargetAddr + "\r\nProxy-Connection: keep-alive\r\n\r\n")
        connproxylen = len(connproxy)
    }

    ln, err := net.Listen("tcp", c.BindAddr)
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

func ProxyRequest (client net.Conn) {
    if c.Ssl {
        config := &tls.Config{
            InsecureSkipVerify: true,
        }
        if strings.Index(c.ServerAddr, ":") == -1 {
            c.ServerAddr += ":443"
        }
        log.Println(c.ServerAddr)
        server, err := tls.Dial("tcp", c.ServerAddr, config)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }

        if c.HttpHead {
            _, err = server.Write(auth)
            if err != nil {
                log.Println(err)
                client.Close()
                server.Close()
                return
            }
            b := make([]byte, 64)
            n, err := client.Read(b)
            if err != nil {
                log.Println(err)
                client.Close()
                server.Close()
                return
            }
            if string(b[:n]) != "HTTP/1.1 200 Authorization passed\r\n\r\n" {
                log.Println(string(b[:n]))
                client.Close()
                server.Close()
                return
            }
        }
        if c.ConnectMode {
            _, err = server.Write(connproxy)
            if err != nil {
                log.Println(err)
                client.Close()
                server.Close()
                return
            }
            b := make([]byte, 64)
            n, err := server.Read(b)
            if err != nil {
                log.Println(err)
                client.Close()
                server.Close()
                return
            }
            if string(b[:n]) != "HTTP/1.1 200 Connection established\r\n\r\n" {
                log.Println(string(b[:n]))
                client.Close()
                server.Close()
                return
            }
        }

        go IoCopy1(client, server)
        go IoCopy2(server, client)
    } else {
        if strings.Index(c.ServerAddr, ":") == -1 {
            c.ServerAddr += ":80"
        }
        server, err := net.Dial("tcp", c.ServerAddr)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        if c.HttpHead {
            _, err = server.Write(auth)
            if err != nil {
                log.Println(err)
                client.Close()
                server.Close()
                return
            }
            b := make([]byte, 64)
            n, err := client.Read(b)
            if err != nil {
                log.Println(err)
                client.Close()
                server.Close()
                return
            }
            if string(b[:n]) != "HTTP/1.1 200 Authorization passed\r\n\r\n" {
                log.Println(string(b[:n]))
                client.Close()
                server.Close()
                return
            }
        }
        if c.ConnectMode {
            _, err = server.Write(connproxy)
            if err != nil {
                log.Println(err)
                client.Close()
                server.Close()
                return
            }
            b := make([]byte, 64)
            n, err := server.Read(b)
            if err != nil {
                log.Println(err)
                client.Close()
                server.Close()
                return
            }
            if string(b[:n]) != "HTTP/1.1 200 Connection established\r\n\r\n" {
                log.Println(string(b[:n]))
                client.Close()
                server.Close()
                return
            }
        }

        go IoCopy(client, server)
        go IoCopy(server, client)
    }
}

func IoCopy(dst net.Conn, src net.Conn) {
    io.Copy(dst, src)
    dst.Close()
}

func IoCopy1(dst net.Conn, src *tls.Conn) {
    io.Copy(dst, src)
    dst.Close()
}

func IoCopy2(dst *tls.Conn, src net.Conn) {
    io.Copy(dst, src)
    dst.Close()
}
