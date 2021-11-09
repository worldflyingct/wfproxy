package main

import (
    "log"
    "net"
    "io"
    "runtime"
    "strconv"
    "strings"
    "crypto/tls"
    "io/ioutil"
    "encoding/json"
)

const defconf = "{\n" +
                "  \"ssl\": false,\n" +
                "  \"bindport\": 1080,\n" +
                "  \"serveraddr\": \"proxyserver:443\",\n" +
                "  \"needauth\": false,\n" +
                "  \"path\": \"/\",\n" +
                "  \"key\": \"65f5bb36-8a0a-4be4-b0d0-18dee527b2d8\",\n" +
                "  \"connectmode\": false,\n" +
                "  \"targetaddr\": \"targetserver:443\"\n" +
                "}"
type Config struct {
    Ssl bool
    BindPort int
    ServerAddr string
    NeedAuth bool
    ConnectMode bool
}
var c Config
var auth []byte
var authlen int
var connproxy []byte
var connproxylen int

func initconfigdata () int {
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
    path := obj["path"].(string)
    key := obj["key"].(string)
    targetaddr := obj["targetaddr"].(string)
    colon := strings.Index(c.ServerAddr, ":")
    if c.NeedAuth {
        var serveraddr string
        if colon == -1 {
            serveraddr = c.ServerAddr
        } else {
            serveraddr = c.ServerAddr[:strings.Index(c.ServerAddr, ":")]
        }
        auth = []byte("GET " + path + " HTTP/1.1\r\nHost: " + serveraddr + "\r\nConnection: Upgrade\r\nPragma: no-cache\r\nCache-Control: no-cache\r\nUpgrade: websocket\r\nAuthorization: " + key + "\r\n\r\n")
        authlen = len(auth)
    }
    if colon == -1 {
        serveraddr = c.ServerAddr
        if c.Ssl {
            c.ServerAddr += ":443"
        } else {
            c.ServerAddr += ":80"
        }
    }
    if c.ConnectMode {
        connproxy = []byte("CONNECT " + targetaddr + " HTTP/1.1\r\nHost: " + targetaddr + "\r\nProxy-Connection: keep-alive\r\n\r\n")
        connproxylen = len(connproxy)
    }
    return 0
}

func main () {
    runtime.GOMAXPROCS(1)

    log.SetFlags(log.LstdFlags | log.Lshortfile)
    log.Println("version: " + runtime.Version())

    if initconfigdata() != 0 {
        return
    }

    ln, err := net.Listen("tcp", ":" + strconv.Itoa(c.BindPort))
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
        server, err := tls.Dial("tcp", c.ServerAddr, config)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }

        if c.NeedAuth {
            _, err = server.Write(auth)
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
            _, err = server.Write(connproxy)
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

        go IoCopy1(client, server)
        go IoCopy2(server, client)
    } else {
        server, err := net.Dial("tcp", c.ServerAddr)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }

        if c.NeedAuth {
            _, err = server.Write(auth)
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
            log.Println(string(b[:n]))
        }
        if c.ConnectMode {
            _, err = server.Write(connproxy)
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
