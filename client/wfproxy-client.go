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
                "  \"path\": \"/\"," +
                "  \"key\": \"D5lfC6LQ1W0BwzP9x3TsxvvdYBCFznqk\",\n" +
                "  \"connectmode\": false,\n" +
                "  \"targetaddr\": \"targetserver:443\"\n" +
                "}"
type Config struct {
    ssl bool
    bindaddr string
    serveraddr string
    httphead bool
    path string
    key string
    connectmode bool
    targetaddr string
}
var c Config
var auth []byte
var authlen int

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
        log.Println("配置文件读取失败")
        return
    }
    if c.httphead {
        auth = []byte("GET " + c.path + " HTTP/1.1\r\nHost: " + c.serveraddr + "\r\nConnection: Upgrade\r\nAuthorization: " + c.key + "\r\n\r\n")
    } else {
        auth = make([]byte, 0)
    }
    if c.connectmode {
        auth = append(auth, []byte("CONNECT " + c.targetaddr + " HTTP/1.1\r\nHost: " + c.targetaddr + "\r\nProxy-Connection: keep-alive\r\n\r\n")...)
    }
    authlen = len(auth)

    ln, err := net.Listen("tcp", c.bindaddr)
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

func ProxyRequest (conn net.Conn) {
    client, _ := conn.(*net.TCPConn)
    client.SetReadBuffer(32*1024)
    client.SetWriteBuffer(32*1024)

    if c.ssl {
        config := &tls.Config{
            InsecureSkipVerify: true,
        }
        if strings.Index(c.serveraddr, ":") == -1 {
            c.serveraddr += ":443"
        }
        server, err := tls.Dial("tcp", c.serveraddr, config)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }

        b := make([]byte, 32*1024)
        if c.httphead || c.connectmode {
            copy(b, auth)
        }
        var n int
        if c.connectmode {
            n = 0
        } else {
            n, err = client.Read(b[authlen:])
            if err != nil {
                log.Println(err)
                client.Close()
                server.Close()
                return
            }
        }
        _, err = server.Write(b[:authlen+n])
        if err != nil {
            log.Println(err)
            client.Close()
            server.Close()
            return
        }
        if c.connectmode {
            n, err = client.Read(b)
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
        if strings.Index(c.serveraddr, ":") == -1 {
            c.serveraddr += ":80"
        }
        tcpaddr, err := net.ResolveTCPAddr("tcp", c.serveraddr)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        server, err := net.DialTCP("tcp", nil, tcpaddr)
        if err != nil {
            log.Println(err)
            client.Close()
            return
        }
        server.SetReadBuffer(32*1024)
        server.SetWriteBuffer(32*1024)

        b := make([]byte, 32*1024)
        if c.httphead || c.connectmode {
            copy(b, auth)
        }
        var n int
        if c.connectmode {
            n = 0
        } else {
            n, err = client.Read(b[authlen:])
            if err != nil {
                log.Println(err)
                client.Close()
                server.Close()
                return
            }
        }
        _, err = server.Write(b[:authlen+n])
        if err != nil {
            log.Println(err)
            client.Close()
            server.Close()
            return
        }
        if c.connectmode {
            n, err = client.Read(b)
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

func IoCopy(dst *net.TCPConn, src *net.TCPConn) {
    io.Copy(dst, src)
    dst.Close()
}

func IoCopy1(dst *net.TCPConn, src *tls.Conn) {
    io.Copy(dst, src)
    dst.Close()
}

func IoCopy2(dst *tls.Conn, src *net.TCPConn) {
    io.Copy(dst, src)
    dst.Close()
}
